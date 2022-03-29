#include <glog/logging.h>
#include <remill/Arch/Sleigh/SleighArch.h>
#include <remill/BC/ABI.h>
#include <remill/BC/IntrinsicTable.h>
#include <remill/BC/SleighLifter.h>
#include <remill/BC/Util.h>

#include <unordered_map>
namespace remill {


class SleighLifter::PcodeToLLVMEmitIntoBlock : public PcodeEmit {
 private:
  class Parameter {
   public:
    virtual std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                                       llvm::Type *ty) = 0;


    virtual LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                                      llvm::Value *inner_lifted) = 0;
  };


  using ParamPtr = std::shared_ptr<Parameter>;

  class RegisterValue : public Parameter {
   private:
    llvm::Value *register_pointer;

   public:
    // TODO(Ian): allow this to be fallible and have better error handling
    std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                               llvm::Type *ty) override {
      return bldr.CreateLoad(ty, register_pointer);
    }

    LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                              llvm::Value *inner_lifted) override {
      bldr.CreateStore(inner_lifted, register_pointer);
      return LiftStatus::kLiftedInstruction;
    }

   public:
    RegisterValue(llvm::Value *register_pointer)
        : register_pointer(register_pointer) {}

    static ParamPtr CreatRegister(llvm::Value *register_pointer) {
      return std::make_shared<RegisterValue>(register_pointer);
    }

    virtual ~RegisterValue() {}
  };


  class Memory : public Parameter {
   public:
    virtual ~Memory() {}
    Memory(llvm::Value *memory_ref_ptr, llvm::Value *index,
           const IntrinsicTable *intrinsics, llvm::Type *memory_ptr_type)
        : memory_ref_ptr(memory_ref_ptr),
          index(index),
          intrinsics(intrinsics),
          memory_ptr_type(memory_ptr_type) {}

    static ParamPtr CreateMemory(llvm::Value *memory_ref_ptr,
                                 llvm::Value *index,
                                 const IntrinsicTable *intrinsics,
                                 llvm::Type *memory_ptr_type) {
      return std::make_shared<Memory>(memory_ref_ptr, index, intrinsics,
                                      memory_ptr_type);
    }

   private:
    llvm::Value *memory_ref_ptr;
    llvm::Value *index;
    const IntrinsicTable *intrinsics;
    llvm::Type *memory_ptr_type;

    std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                               llvm::Type *ty) override {
      if (auto *inty = llvm::dyn_cast<llvm::IntegerType>(ty)) {
        llvm::Function *intrinsic = nullptr;
        switch (inty->getBitWidth()) {
          case 8: intrinsic = this->intrinsics->read_memory_8; break;
          case 16: intrinsic = this->intrinsics->read_memory_16; break;
          case 32: intrinsic = this->intrinsics->read_memory_32; break;
          case 64: intrinsic = this->intrinsics->read_memory_64; break;
        }

        if (intrinsic) {
          llvm::Value *temp_args[] = {
              bldr.CreateLoad(this->memory_ptr_type, this->memory_ref_ptr),
              this->index};
          return bldr.CreateCall(intrinsic, temp_args);
        }
      }

      return std::nullopt;
    }

    LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                              llvm::Value *inner_lifted) override {


      if (auto *inty =
              llvm::dyn_cast<llvm::IntegerType>(inner_lifted->getType())) {
        llvm::Function *intrinsic = nullptr;
        switch (inty->getBitWidth()) {
          case 8: intrinsic = this->intrinsics->write_memory_8; break;
          case 16: intrinsic = this->intrinsics->write_memory_16; break;
          case 32: intrinsic = this->intrinsics->write_memory_32; break;
          case 64: intrinsic = this->intrinsics->write_memory_64; break;
        }

        if (intrinsic) {
          llvm::Value *temp_args[] = {
              bldr.CreateLoad(this->memory_ptr_type, this->memory_ref_ptr),
              this->index, inner_lifted};
          llvm::Value *new_memory_ptr = bldr.CreateCall(intrinsic, temp_args);
          bldr.CreateStore(new_memory_ptr, this->memory_ref_ptr);
          return LiftStatus::kLiftedInstruction;
        }
      }

      return LiftStatus::kLiftedUnsupportedInstruction;
    }
  };

  class ConstantValue : public Parameter {
   private:
    llvm::Constant *cst;

   public:
    std::optional<llvm::Value *> LiftAsInParam(llvm::IRBuilder<> &bldr,
                                               llvm::Type *ty) override {
      if (ty != cst->getType()) {
        return std::nullopt;
      }
      return this->cst;
    }

    LiftStatus StoreIntoParam(llvm::IRBuilder<> &bldr,
                              llvm::Value *inner_lifted) override {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }

    ConstantValue(llvm::Constant *cst) : cst(cst) {}

    static ParamPtr CreatConstant(llvm::Constant *cst) {
      return std::make_shared<ConstantValue>(cst);
    }
    virtual ~ConstantValue() {}
  };


  llvm::BasicBlock *target_block;
  llvm::Value *state_pointer;
  llvm::LLVMContext &context;
  const Instruction &insn;
  LiftStatus status;
  SleighLifter &insn_lifter_parent;
  std::unordered_map<uint64_t, llvm::Value *> cached_unique_ptrs;


  void UpdateStatus(LiftStatus new_status, OpCode opc) {
    if (new_status != LiftStatus::kLiftedInstruction) {
      LOG(ERROR) << "Failed to lift insn with opcode: " << get_opname(opc);
      this->status = new_status;
    } else if (status == LiftStatus::kLiftedInvalidInstruction) {
      this->status = new_status;
    }
  }

 public:
  PcodeToLLVMEmitIntoBlock(llvm::BasicBlock *target_block,
                           llvm::Value *state_pointer, const Instruction &insn,
                           SleighLifter &insn_lifter_parent)
      : target_block(target_block),
        state_pointer(state_pointer),
        context(target_block->getContext()),
        insn(insn),
        status(remill::LiftStatus::kLiftedInvalidInstruction),
        insn_lifter_parent(insn_lifter_parent){};


  llvm::Value *GetUniquePtr(uint64_t offset, uint64_t size,
                            llvm::IRBuilder<> &bldr) {
    if (this->cached_unique_ptrs.find(offset) !=
        this->cached_unique_ptrs.end()) {
      return this->cached_unique_ptrs.find(offset)->second;
    }
    auto ptr = bldr.CreateAlloca(
        llvm::IntegerType::get(this->context, size * 8), 0, nullptr);
    this->cached_unique_ptrs.insert({offset, ptr});
    return ptr;
  }


  ParamPtr CreateMemoryAddress(llvm::Value *offset) {

    const auto mem_ptr_ref = this->insn_lifter_parent.LoadRegAddress(
        this->target_block, this->state_pointer, kMemoryVariableName);
    // compute pointer into memory at offset


    return Memory::CreateMemory(mem_ptr_ref, offset,
                                this->insn_lifter_parent.GetIntrinsicTable(),
                                this->insn_lifter_parent.GetMemoryType());
  }

  //TODO(Ian): Maybe this should be a failable function that returns an unsupported insn in certain failures
  ParamPtr LiftParamPtr(llvm::IRBuilder<> &bldr, VarnodeData vnode) {
    auto space_name = vnode.getAddr().getSpace()->getName();
    if (space_name == "ram") {
      // compute pointer into memory at offset

      auto constant_offset = llvm::ConstantInt::get(
          this->insn_lifter_parent.GetWordType(), vnode.offset);

      return this->CreateMemoryAddress(constant_offset);
    } else if (space_name == "register") {
      auto reg_name = this->insn_lifter_parent.GetEngine().getRegisterName(
          vnode.space, vnode.offset, vnode.size);
      for (auto &c : reg_name)
        c = toupper(c);
      LOG(INFO) << "Looking for reg name " << reg_name << " from offset "
                << vnode.offset;
      // TODO(Ian): will probably need to adjust the pointer here in certain circumstances
      auto reg_ptr = this->insn_lifter_parent.LoadRegAddress(
          bldr.GetInsertBlock(), this->state_pointer, reg_name);
      return RegisterValue::CreatRegister(reg_ptr);
    } else if (space_name == "const") {
      auto cst_v = llvm::ConstantInt::get(
          this->insn_lifter_parent.GetWordType(), vnode.offset);
      return ConstantValue::CreatConstant(cst_v);
    } else if (space_name == "unique") {
      auto reg_ptr = this->GetUniquePtr(vnode.offset, vnode.size, bldr);
      return RegisterValue::CreatRegister(reg_ptr);
    } else {
      LOG(FATAL) << "Unhandled memory space: " << space_name;
    }
  }

  std::optional<llvm::Value *> LiftInParam(llvm::IRBuilder<> &bldr,
                                           VarnodeData vnode, llvm::Type *ty) {
    ParamPtr ptr = this->LiftParamPtr(bldr, vnode);

    return ptr->LiftAsInParam(bldr, ty);
  }

  std::optional<llvm::Value *> LiftIntegerInParam(llvm::IRBuilder<> &bldr,
                                                  VarnodeData vnode) {
    return this->LiftInParam(
        bldr, vnode, llvm::IntegerType::get(this->context, vnode.size * 8));
  }

  LiftStatus
  LiftRequireOutParam(std::function<LiftStatus(VarnodeData)> inner_lift,
                      VarnodeData *outvar) {
    if (outvar) {
      return inner_lift(*outvar);
    } else {
      return LiftStatus::kLiftedUnsupportedInstruction;
    }
  }


  LiftStatus LiftStoreIntoOutParam(llvm::IRBuilder<> &bldr,
                                   llvm::Value *inner_lifted,
                                   VarnodeData *outvar) {
    return this->LiftRequireOutParam(
        [&bldr, this, inner_lifted](VarnodeData out_param_data) {
          auto ptr = this->LiftParamPtr(bldr, out_param_data);
          return ptr->StoreIntoParam(bldr, inner_lifted);
        },
        outvar);
  }


  LiftStatus LiftUnOp(llvm::IRBuilder<> &bldr, OpCode opc, VarnodeData *outvar,
                      VarnodeData input_var) {
    // TODO(Ian): when we lift a param we need to specify the type we want


    switch (opc) {
      case OpCode::CPUI_BOOL_NEGATE: {
        auto bneg_inval = this->LiftInParam(
            bldr, input_var, llvm::IntegerType::get(this->context, 8));
        if (bneg_inval.has_value()) {
          return this->LiftStoreIntoOutParam(bldr, bldr.CreateNot(*bneg_inval),
                                             outvar);
        }
      }
      case OpCode::CPUI_COPY: {
        auto copy_inval = this->LiftInParam(
            bldr, input_var,
            llvm::IntegerType::get(this->context, input_var.size * 8));
        if (copy_inval.has_value()) {
          return this->LiftStoreIntoOutParam(bldr, *copy_inval, outvar);
        }
      }
    }
    return LiftStatus::kLiftedUnsupportedInstruction;
  }
  using BinaryOperator = std::function<llvm::Value *(
      llvm::Value *, llvm::Value *, llvm::IRBuilder<> &)>;
  static std::map<OpCode, BinaryOperator> INTEGER_BINARY_OPS;


  LiftStatus LiftIntegerBinop(llvm::IRBuilder<> &bldr, OpCode opc,
                              VarnodeData *outvar, VarnodeData lhs,
                              VarnodeData rhs) {
    if (INTEGER_BINARY_OPS.find(opc) != INTEGER_BINARY_OPS.end()) {
      auto &op_func = INTEGER_BINARY_OPS.find(opc)->second;
      auto lifted_lhs = this->LiftIntegerInParam(bldr, lhs);
      auto lifted_rhs = this->LiftIntegerInParam(bldr, rhs);
      if (lifted_lhs.has_value() && lifted_rhs.has_value()) {
        LOG(INFO) << "Binop with lhs: "
                  << remill::LLVMThingToString(*lifted_lhs);
        LOG(INFO) << "Binop with rhs" << remill::LLVMThingToString(*lifted_rhs);
        return this->LiftStoreIntoOutParam(
            bldr, op_func(*lifted_lhs, *lifted_rhs, bldr), outvar);
      }
    }
    return LiftStatus::kLiftedUnsupportedInstruction;
  }

  LiftStatus LiftBinOp(llvm::IRBuilder<> &bldr, OpCode opc, VarnodeData *outvar,
                       VarnodeData lhs, VarnodeData rhs) {
    auto res = this->LiftIntegerBinop(bldr, opc, outvar, lhs, rhs);
    if (res == LiftStatus::kLiftedInstruction) {
      return res;
    }

    if (opc == OpCode::CPUI_LOAD && outvar) {
      auto out_op = *outvar;
      auto addr_operand = rhs;
      auto lifted_addr_offset = this->LiftInParam(
          bldr, addr_operand, this->insn_lifter_parent.GetWordType());

      if (lifted_addr_offset) {

        auto out_type = llvm::IntegerType::get(this->context, out_op.size * 8);
        auto lifted_addr = this->CreateMemoryAddress(*lifted_addr_offset);

        auto loaded_value = lifted_addr->LiftAsInParam(bldr, out_type);
        if (loaded_value.has_value()) {
          auto lifted_out = this->LiftParamPtr(bldr, out_op);
          return lifted_out->StoreIntoParam(bldr, *loaded_value);
        }
      }
    }


    return LiftStatus::kLiftedUnsupportedInstruction;
  }


  void dump(const Address &addr, OpCode opc, VarnodeData *outvar,
            VarnodeData *vars, int4 isize) override {
    llvm::IRBuilder bldr(this->target_block);
    switch (isize) {
      case 1:
        this->UpdateStatus(this->LiftUnOp(bldr, opc, outvar, vars[0]), opc);
        break;
      case 2:
        this->UpdateStatus(this->LiftBinOp(bldr, opc, outvar, vars[0], vars[1]),
                           opc);
        break;
      default:
        this->UpdateStatus(LiftStatus::kLiftedUnsupportedInstruction, opc);
        return;
    }
  }

  LiftStatus GetStatus() {
    return this->status;
  }
};

std::map<OpCode, SleighLifter::PcodeToLLVMEmitIntoBlock::BinaryOperator>
    SleighLifter::PcodeToLLVMEmitIntoBlock::INTEGER_BINARY_OPS = {
        {OpCode::CPUI_INT_AND,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateAnd(lhs, rhs);
         }},
        {OpCode::CPUI_INT_ADD,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateAdd(lhs, rhs);
         }},
        {OpCode::CPUI_INT_MULT,
         [](llvm::Value *lhs, llvm::Value *rhs, llvm::IRBuilder<> &bldr) {
           return bldr.CreateMul(lhs, rhs);
         }

        }};

LiftStatus
SleighLifter::LiftIntoBlock(Instruction &inst, llvm::BasicBlock *block,
                            llvm::Value *state_ptr, bool is_delayed) {

  if (!inst.IsValid()) {
    LOG(ERROR) << "Invalid function" << inst.Serialize();
    inst.operands.clear();
    return kLiftedInvalidInstruction;
  }

  SleighLifter::PcodeToLLVMEmitIntoBlock lifter(block, state_ptr, inst, *this);
  auto res = this->sleigh_context.oneInstruction(lifter, inst.bytes);

  //NOTE(Ian): If we made it past decoding we should be able to decode the bytes again
  assert(res.has_value());

  return lifter.GetStatus();
}

Sleigh &SleighLifter::GetEngine() {
  return this->sleigh_context.GetEngine();
}
}  // namespace remill