/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "code_generator_arm.h"

#include "entrypoints/quick/quick_entrypoints.h"
#include "gc/accounting/card_table.h"
#include "mirror/array-inl.h"
#include "mirror/art_method.h"
#include "mirror/class.h"
#include "thread.h"
#include "utils/arm/assembler_arm.h"
#include "utils/arm/managed_register_arm.h"
#include "utils/assembler.h"
#include "utils/stack_checks.h"

namespace art {

namespace arm {

static DRegister FromLowSToD(SRegister reg) {
  DCHECK_EQ(reg % 2, 0);
  return static_cast<DRegister>(reg / 2);
}

static constexpr bool kExplicitStackOverflowCheck = false;

static constexpr int kNumberOfPushedRegistersAtEntry = 1 + 2;  // LR, R6, R7
static constexpr int kCurrentMethodStackOffset = 0;

static constexpr Register kRuntimeParameterCoreRegisters[] = { R0, R1, R2, R3 };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static constexpr SRegister kRuntimeParameterFpuRegisters[] = { };
static constexpr size_t kRuntimeParameterFpuRegistersLength = 0;

class InvokeRuntimeCallingConvention : public CallingConvention<Register, SRegister> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

#define __ reinterpret_cast<ArmAssembler*>(codegen->GetAssembler())->
#define QUICK_ENTRY_POINT(x) QUICK_ENTRYPOINT_OFFSET(kArmWordSize, x).Int32Value()

class SlowPathCodeARM : public SlowPathCode {
 public:
  SlowPathCodeARM() : entry_label_(), exit_label_() {}

  Label* GetEntryLabel() { return &entry_label_; }
  Label* GetExitLabel() { return &exit_label_; }

 private:
  Label entry_label_;
  Label exit_label_;

  DISALLOW_COPY_AND_ASSIGN(SlowPathCodeARM);
};

class NullCheckSlowPathARM : public SlowPathCodeARM {
 public:
  explicit NullCheckSlowPathARM(HNullCheck* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    arm_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowNullPointer), instruction_, instruction_->GetDexPc());
  }

 private:
  HNullCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(NullCheckSlowPathARM);
};

class DivZeroCheckSlowPathARM : public SlowPathCodeARM {
 public:
  explicit DivZeroCheckSlowPathARM(HDivZeroCheck* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    arm_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowDivZero), instruction_, instruction_->GetDexPc());
  }

 private:
  HDivZeroCheck* const instruction_;
  DISALLOW_COPY_AND_ASSIGN(DivZeroCheckSlowPathARM);
};

class StackOverflowCheckSlowPathARM : public SlowPathCodeARM {
 public:
  StackOverflowCheckSlowPathARM() {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    __ Bind(GetEntryLabel());
    __ LoadFromOffset(kLoadWord, PC, TR,
        QUICK_ENTRYPOINT_OFFSET(kArmWordSize, pThrowStackOverflow).Int32Value());
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(StackOverflowCheckSlowPathARM);
};

class SuspendCheckSlowPathARM : public SlowPathCodeARM {
 public:
  SuspendCheckSlowPathARM(HSuspendCheck* instruction, HBasicBlock* successor)
      : instruction_(instruction), successor_(successor) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(instruction_->GetLocations());
    arm_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pTestSuspend), instruction_, instruction_->GetDexPc());
    codegen->RestoreLiveRegisters(instruction_->GetLocations());
    if (successor_ == nullptr) {
      __ b(GetReturnLabel());
    } else {
      __ b(arm_codegen->GetLabelOf(successor_));
    }
  }

  Label* GetReturnLabel() {
    DCHECK(successor_ == nullptr);
    return &return_label_;
  }

 private:
  HSuspendCheck* const instruction_;
  // If not null, the block to branch to after the suspend check.
  HBasicBlock* const successor_;

  // If `successor_` is null, the label to branch to after the suspend check.
  Label return_label_;

  DISALLOW_COPY_AND_ASSIGN(SuspendCheckSlowPathARM);
};

class BoundsCheckSlowPathARM : public SlowPathCodeARM {
 public:
  BoundsCheckSlowPathARM(HBoundsCheck* instruction,
                         Location index_location,
                         Location length_location)
      : instruction_(instruction),
        index_location_(index_location),
        length_location_(length_location) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        index_location_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        length_location_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
    arm_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pThrowArrayBounds), instruction_, instruction_->GetDexPc());
  }

 private:
  HBoundsCheck* const instruction_;
  const Location index_location_;
  const Location length_location_;

  DISALLOW_COPY_AND_ASSIGN(BoundsCheckSlowPathARM);
};

class LoadClassSlowPathARM : public SlowPathCodeARM {
 public:
  LoadClassSlowPathARM(HLoadClass* cls,
                       HInstruction* at,
                       uint32_t dex_pc,
                       bool do_clinit)
      : cls_(cls), at_(at), dex_pc_(dex_pc), do_clinit_(do_clinit) {
    DCHECK(at->IsLoadClass() || at->IsClinitCheck());
  }

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = at_->GetLocations();

    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(locations);

    InvokeRuntimeCallingConvention calling_convention;
    __ LoadImmediate(calling_convention.GetRegisterAt(0), cls_->GetTypeIndex());
    arm_codegen->LoadCurrentMethod(calling_convention.GetRegisterAt(1));
    int32_t entry_point_offset = do_clinit_
        ? QUICK_ENTRY_POINT(pInitializeStaticStorage)
        : QUICK_ENTRY_POINT(pInitializeType);
    arm_codegen->InvokeRuntime(entry_point_offset, at_, dex_pc_);

    // Move the class to the desired location.
    Location out = locations->Out();
    if (out.IsValid()) {
      DCHECK(out.IsRegister() && !locations->GetLiveRegisters()->ContainsCoreRegister(out.reg()));
      arm_codegen->Move32(locations->Out(), Location::RegisterLocation(R0));
    }
    codegen->RestoreLiveRegisters(locations);
    __ b(GetExitLabel());
  }

 private:
  // The class this slow path will load.
  HLoadClass* const cls_;

  // The instruction where this slow path is happening.
  // (Might be the load class or an initialization check).
  HInstruction* const at_;

  // The dex PC of `at_`.
  const uint32_t dex_pc_;

  // Whether to initialize the class.
  const bool do_clinit_;

  DISALLOW_COPY_AND_ASSIGN(LoadClassSlowPathARM);
};

class LoadStringSlowPathARM : public SlowPathCodeARM {
 public:
  explicit LoadStringSlowPathARM(HLoadString* instruction) : instruction_(instruction) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(!locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(locations);

    InvokeRuntimeCallingConvention calling_convention;
    arm_codegen->LoadCurrentMethod(calling_convention.GetRegisterAt(0));
    __ LoadImmediate(calling_convention.GetRegisterAt(1), instruction_->GetStringIndex());
    arm_codegen->InvokeRuntime(
        QUICK_ENTRY_POINT(pResolveString), instruction_, instruction_->GetDexPc());
    arm_codegen->Move32(locations->Out(), Location::RegisterLocation(R0));

    codegen->RestoreLiveRegisters(locations);
    __ b(GetExitLabel());
  }

 private:
  HLoadString* const instruction_;

  DISALLOW_COPY_AND_ASSIGN(LoadStringSlowPathARM);
};

class TypeCheckSlowPathARM : public SlowPathCodeARM {
 public:
  TypeCheckSlowPathARM(HInstruction* instruction,
                       Location class_to_check,
                       Location object_class,
                       uint32_t dex_pc)
      : instruction_(instruction),
        class_to_check_(class_to_check),
        object_class_(object_class),
        dex_pc_(dex_pc) {}

  void EmitNativeCode(CodeGenerator* codegen) OVERRIDE {
    LocationSummary* locations = instruction_->GetLocations();
    DCHECK(instruction_->IsCheckCast()
           || !locations->GetLiveRegisters()->ContainsCoreRegister(locations->Out().reg()));

    CodeGeneratorARM* arm_codegen = down_cast<CodeGeneratorARM*>(codegen);
    __ Bind(GetEntryLabel());
    codegen->SaveLiveRegisters(locations);

    // We're moving two locations to locations that could overlap, so we need a parallel
    // move resolver.
    InvokeRuntimeCallingConvention calling_convention;
    codegen->EmitParallelMoves(
        class_to_check_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(0)),
        object_class_,
        Location::RegisterLocation(calling_convention.GetRegisterAt(1)));

    if (instruction_->IsInstanceOf()) {
      arm_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pInstanceofNonTrivial), instruction_, dex_pc_);
      arm_codegen->Move32(locations->Out(), Location::RegisterLocation(R0));
    } else {
      DCHECK(instruction_->IsCheckCast());
      arm_codegen->InvokeRuntime(QUICK_ENTRY_POINT(pCheckCast), instruction_, dex_pc_);
    }

    codegen->RestoreLiveRegisters(locations);
    __ b(GetExitLabel());
  }

 private:
  HInstruction* const instruction_;
  const Location class_to_check_;
  const Location object_class_;
  uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(TypeCheckSlowPathARM);
};

#undef __

#undef __
#define __ reinterpret_cast<ArmAssembler*>(GetAssembler())->

inline Condition ARMCondition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return EQ;
    case kCondNE: return NE;
    case kCondLT: return LT;
    case kCondLE: return LE;
    case kCondGT: return GT;
    case kCondGE: return GE;
    default:
      LOG(FATAL) << "Unknown if condition";
  }
  return EQ;        // Unreachable.
}

inline Condition ARMOppositeCondition(IfCondition cond) {
  switch (cond) {
    case kCondEQ: return NE;
    case kCondNE: return EQ;
    case kCondLT: return GE;
    case kCondLE: return GT;
    case kCondGT: return LE;
    case kCondGE: return LT;
    default:
      LOG(FATAL) << "Unknown if condition";
  }
  return EQ;        // Unreachable.
}

void CodeGeneratorARM::DumpCoreRegister(std::ostream& stream, int reg) const {
  stream << ArmManagedRegister::FromCoreRegister(Register(reg));
}

void CodeGeneratorARM::DumpFloatingPointRegister(std::ostream& stream, int reg) const {
  stream << ArmManagedRegister::FromSRegister(SRegister(reg));
}

size_t CodeGeneratorARM::SaveCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ StoreToOffset(kStoreWord, static_cast<Register>(reg_id), SP, stack_index);
  return kArmWordSize;
}

size_t CodeGeneratorARM::RestoreCoreRegister(size_t stack_index, uint32_t reg_id) {
  __ LoadFromOffset(kLoadWord, static_cast<Register>(reg_id), SP, stack_index);
  return kArmWordSize;
}

CodeGeneratorARM::CodeGeneratorARM(HGraph* graph)
    : CodeGenerator(graph, kNumberOfCoreRegisters, kNumberOfSRegisters, kNumberOfRegisterPairs),
      block_labels_(graph->GetArena(), 0),
      location_builder_(graph, this),
      instruction_visitor_(graph, this),
      move_resolver_(graph->GetArena(), this),
      assembler_(true) {}

size_t CodeGeneratorARM::FrameEntrySpillSize() const {
  return kNumberOfPushedRegistersAtEntry * kArmWordSize;
}

Location CodeGeneratorARM::AllocateFreeRegister(Primitive::Type type) const {
  switch (type) {
    case Primitive::kPrimLong: {
      size_t reg = FindFreeEntry(blocked_register_pairs_, kNumberOfRegisterPairs);
      ArmManagedRegister pair =
          ArmManagedRegister::FromRegisterPair(static_cast<RegisterPair>(reg));
      DCHECK(!blocked_core_registers_[pair.AsRegisterPairLow()]);
      DCHECK(!blocked_core_registers_[pair.AsRegisterPairHigh()]);

      blocked_core_registers_[pair.AsRegisterPairLow()] = true;
      blocked_core_registers_[pair.AsRegisterPairHigh()] = true;
      UpdateBlockedPairRegisters();
      return Location::RegisterPairLocation(pair.AsRegisterPairLow(), pair.AsRegisterPairHigh());
    }

    case Primitive::kPrimByte:
    case Primitive::kPrimBoolean:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      int reg = FindFreeEntry(blocked_core_registers_, kNumberOfCoreRegisters);
      // Block all register pairs that contain `reg`.
      for (int i = 0; i < kNumberOfRegisterPairs; i++) {
        ArmManagedRegister current =
            ArmManagedRegister::FromRegisterPair(static_cast<RegisterPair>(i));
        if (current.AsRegisterPairLow() == reg || current.AsRegisterPairHigh() == reg) {
          blocked_register_pairs_[i] = true;
        }
      }
      return Location::RegisterLocation(reg);
    }

    case Primitive::kPrimFloat: {
      int reg = FindFreeEntry(blocked_fpu_registers_, kNumberOfSRegisters);
      return Location::FpuRegisterLocation(reg);
    }

    case Primitive::kPrimDouble: {
      int reg = FindTwoFreeConsecutiveAlignedEntries(blocked_fpu_registers_, kNumberOfSRegisters);
      DCHECK_EQ(reg % 2, 0);
      return Location::FpuRegisterPairLocation(reg, reg + 1);
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << type;
  }

  return Location();
}

void CodeGeneratorARM::SetupBlockedRegisters() const {
  // Don't allocate the dalvik style register pair passing.
  blocked_register_pairs_[R1_R2] = true;

  // Stack register, LR and PC are always reserved.
  blocked_core_registers_[SP] = true;
  blocked_core_registers_[LR] = true;
  blocked_core_registers_[PC] = true;

  // Reserve thread register.
  blocked_core_registers_[TR] = true;

  // Reserve temp register.
  blocked_core_registers_[IP] = true;

  // TODO: We currently don't use Quick's callee saved registers.
  // We always save and restore R6 and R7 to make sure we can use three
  // register pairs for long operations.
  blocked_core_registers_[R4] = true;
  blocked_core_registers_[R5] = true;
  blocked_core_registers_[R8] = true;
  blocked_core_registers_[R10] = true;
  blocked_core_registers_[R11] = true;

  blocked_fpu_registers_[S16] = true;
  blocked_fpu_registers_[S17] = true;
  blocked_fpu_registers_[S18] = true;
  blocked_fpu_registers_[S19] = true;
  blocked_fpu_registers_[S20] = true;
  blocked_fpu_registers_[S21] = true;
  blocked_fpu_registers_[S22] = true;
  blocked_fpu_registers_[S23] = true;
  blocked_fpu_registers_[S24] = true;
  blocked_fpu_registers_[S25] = true;
  blocked_fpu_registers_[S26] = true;
  blocked_fpu_registers_[S27] = true;
  blocked_fpu_registers_[S28] = true;
  blocked_fpu_registers_[S29] = true;
  blocked_fpu_registers_[S30] = true;
  blocked_fpu_registers_[S31] = true;

  UpdateBlockedPairRegisters();
}

void CodeGeneratorARM::UpdateBlockedPairRegisters() const {
  for (int i = 0; i < kNumberOfRegisterPairs; i++) {
    ArmManagedRegister current =
        ArmManagedRegister::FromRegisterPair(static_cast<RegisterPair>(i));
    if (blocked_core_registers_[current.AsRegisterPairLow()]
        || blocked_core_registers_[current.AsRegisterPairHigh()]) {
      blocked_register_pairs_[i] = true;
    }
  }
}

InstructionCodeGeneratorARM::InstructionCodeGeneratorARM(HGraph* graph, CodeGeneratorARM* codegen)
      : HGraphVisitor(graph),
        assembler_(codegen->GetAssembler()),
        codegen_(codegen) {}

void CodeGeneratorARM::GenerateFrameEntry() {
  bool skip_overflow_check = IsLeafMethod() && !FrameNeedsStackCheck(GetFrameSize(), InstructionSet::kArm);
  if (!skip_overflow_check) {
    if (kExplicitStackOverflowCheck) {
      SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) StackOverflowCheckSlowPathARM();
      AddSlowPath(slow_path);

      __ LoadFromOffset(kLoadWord, IP, TR, Thread::StackEndOffset<kArmWordSize>().Int32Value());
      __ cmp(SP, ShifterOperand(IP));
      __ b(slow_path->GetEntryLabel(), CC);
    } else {
      __ AddConstant(IP, SP, -static_cast<int32_t>(GetStackOverflowReservedBytes(kArm)));
      __ LoadFromOffset(kLoadWord, IP, IP, 0);
      RecordPcInfo(nullptr, 0);
    }
  }

  core_spill_mask_ |= (1 << LR | 1 << R6 | 1 << R7);
  __ PushList(1 << LR | 1 << R6 | 1 << R7);

  // The return PC has already been pushed on the stack.
  __ AddConstant(SP, -(GetFrameSize() - kNumberOfPushedRegistersAtEntry * kArmWordSize));
  __ StoreToOffset(kStoreWord, R0, SP, 0);
}

void CodeGeneratorARM::GenerateFrameExit() {
  __ AddConstant(SP, GetFrameSize() - kNumberOfPushedRegistersAtEntry * kArmWordSize);
  __ PopList(1 << PC | 1 << R6 | 1 << R7);
}

void CodeGeneratorARM::Bind(HBasicBlock* block) {
  __ Bind(GetLabelOf(block));
}

Location CodeGeneratorARM::GetStackLocation(HLoadLocal* load) const {
  switch (load->GetType()) {
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      return Location::DoubleStackSlot(GetStackSlot(load->GetLocal()));
      break;

    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimFloat:
      return Location::StackSlot(GetStackSlot(load->GetLocal()));

    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected type " << load->GetType();
  }

  LOG(FATAL) << "Unreachable";
  return Location();
}

Location InvokeDexCallingConventionVisitor::GetNextLocation(Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      uint32_t index = gp_index_++;
      uint32_t stack_index = stack_index_++;
      if (index < calling_convention.GetNumberOfRegisters()) {
        return Location::RegisterLocation(calling_convention.GetRegisterAt(index));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index));
      }
    }

    case Primitive::kPrimLong: {
      uint32_t index = gp_index_;
      uint32_t stack_index = stack_index_;
      gp_index_ += 2;
      stack_index_ += 2;
      if (index + 1 < calling_convention.GetNumberOfRegisters()) {
        ArmManagedRegister pair = ArmManagedRegister::FromRegisterPair(
            calling_convention.GetRegisterPairAt(index));
        return Location::RegisterPairLocation(pair.AsRegisterPairLow(), pair.AsRegisterPairHigh());
      } else if (index + 1 == calling_convention.GetNumberOfRegisters()) {
        return Location::QuickParameter(index, stack_index);
      } else {
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index));
      }
    }

    case Primitive::kPrimFloat: {
      uint32_t stack_index = stack_index_++;
      if (float_index_ % 2 == 0) {
        float_index_ = std::max(double_index_, float_index_);
      }
      if (float_index_ < calling_convention.GetNumberOfFpuRegisters()) {
        return Location::FpuRegisterLocation(calling_convention.GetFpuRegisterAt(float_index_++));
      } else {
        return Location::StackSlot(calling_convention.GetStackOffsetOf(stack_index));
      }
    }

    case Primitive::kPrimDouble: {
      double_index_ = std::max(double_index_, RoundUp(float_index_, 2));
      uint32_t stack_index = stack_index_;
      stack_index_ += 2;
      if (double_index_ + 1 < calling_convention.GetNumberOfFpuRegisters()) {
        uint32_t index = double_index_;
        double_index_ += 2;
        return Location::FpuRegisterPairLocation(
          calling_convention.GetFpuRegisterAt(index),
          calling_convention.GetFpuRegisterAt(index + 1));
      } else {
        return Location::DoubleStackSlot(calling_convention.GetStackOffsetOf(stack_index));
      }
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unexpected parameter type " << type;
      break;
  }
  return Location();
}

Location InvokeDexCallingConventionVisitor::GetReturnLocation(Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      return Location::RegisterLocation(R0);
    }

    case Primitive::kPrimFloat: {
      return Location::FpuRegisterLocation(S0);
    }

    case Primitive::kPrimLong: {
      return Location::RegisterPairLocation(R0, R1);
    }

    case Primitive::kPrimDouble: {
      return Location::FpuRegisterPairLocation(S0, S1);
    }

    case Primitive::kPrimVoid:
      return Location();
  }
  UNREACHABLE();
  return Location();
}

void CodeGeneratorARM::Move32(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegister()) {
    if (source.IsRegister()) {
      __ Mov(destination.As<Register>(), source.As<Register>());
    } else if (source.IsFpuRegister()) {
      __ vmovrs(destination.As<Register>(), source.As<SRegister>());
    } else {
      __ LoadFromOffset(kLoadWord, destination.As<Register>(), SP, source.GetStackIndex());
    }
  } else if (destination.IsFpuRegister()) {
    if (source.IsRegister()) {
      __ vmovsr(destination.As<SRegister>(), source.As<Register>());
    } else if (source.IsFpuRegister()) {
      __ vmovs(destination.As<SRegister>(), source.As<SRegister>());
    } else {
      __ LoadSFromOffset(destination.As<SRegister>(), SP, source.GetStackIndex());
    }
  } else {
    DCHECK(destination.IsStackSlot()) << destination;
    if (source.IsRegister()) {
      __ StoreToOffset(kStoreWord, source.As<Register>(), SP, destination.GetStackIndex());
    } else if (source.IsFpuRegister()) {
      __ StoreSToOffset(source.As<SRegister>(), SP, destination.GetStackIndex());
    } else {
      DCHECK(source.IsStackSlot()) << source;
      __ LoadFromOffset(kLoadWord, IP, SP, source.GetStackIndex());
      __ StoreToOffset(kStoreWord, IP, SP, destination.GetStackIndex());
    }
  }
}

void CodeGeneratorARM::Move64(Location destination, Location source) {
  if (source.Equals(destination)) {
    return;
  }
  if (destination.IsRegisterPair()) {
    if (source.IsRegisterPair()) {
      __ Mov(destination.AsRegisterPairLow<Register>(), source.AsRegisterPairLow<Register>());
      __ Mov(destination.AsRegisterPairHigh<Register>(), source.AsRegisterPairHigh<Register>());
    } else if (source.IsFpuRegister()) {
      UNIMPLEMENTED(FATAL);
    } else if (source.IsQuickParameter()) {
      uint16_t register_index = source.GetQuickParameterRegisterIndex();
      uint16_t stack_index = source.GetQuickParameterStackIndex();
      InvokeDexCallingConvention calling_convention;
      __ Mov(destination.AsRegisterPairLow<Register>(),
             calling_convention.GetRegisterAt(register_index));
      __ LoadFromOffset(kLoadWord, destination.AsRegisterPairHigh<Register>(),
             SP, calling_convention.GetStackOffsetOf(stack_index + 1) + GetFrameSize());
    } else {
      DCHECK(source.IsDoubleStackSlot());
      if (destination.AsRegisterPairLow<Register>() == R1) {
        DCHECK_EQ(destination.AsRegisterPairHigh<Register>(), R2);
        __ LoadFromOffset(kLoadWord, R1, SP, source.GetStackIndex());
        __ LoadFromOffset(kLoadWord, R2, SP, source.GetHighStackIndex(kArmWordSize));
      } else {
        __ LoadFromOffset(kLoadWordPair, destination.AsRegisterPairLow<Register>(),
                          SP, source.GetStackIndex());
      }
    }
  } else if (destination.IsFpuRegisterPair()) {
    if (source.IsDoubleStackSlot()) {
      __ LoadDFromOffset(FromLowSToD(destination.AsFpuRegisterPairLow<SRegister>()),
                         SP,
                         source.GetStackIndex());
    } else {
      UNIMPLEMENTED(FATAL);
    }
  } else if (destination.IsQuickParameter()) {
    InvokeDexCallingConvention calling_convention;
    uint16_t register_index = destination.GetQuickParameterRegisterIndex();
    uint16_t stack_index = destination.GetQuickParameterStackIndex();
    if (source.IsRegisterPair()) {
      __ Mov(calling_convention.GetRegisterAt(register_index),
             source.AsRegisterPairLow<Register>());
      __ StoreToOffset(kStoreWord, source.AsRegisterPairHigh<Register>(),
             SP, calling_convention.GetStackOffsetOf(stack_index + 1));
    } else if (source.IsFpuRegister()) {
      UNIMPLEMENTED(FATAL);
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ LoadFromOffset(
          kLoadWord, calling_convention.GetRegisterAt(register_index), SP, source.GetStackIndex());
      __ LoadFromOffset(kLoadWord, R0, SP, source.GetHighStackIndex(kArmWordSize));
      __ StoreToOffset(kStoreWord, R0, SP, calling_convention.GetStackOffsetOf(stack_index + 1));
    }
  } else {
    DCHECK(destination.IsDoubleStackSlot());
    if (source.IsRegisterPair()) {
      if (source.AsRegisterPairLow<Register>() == R1) {
        DCHECK_EQ(source.AsRegisterPairHigh<Register>(), R2);
        __ StoreToOffset(kStoreWord, R1, SP, destination.GetStackIndex());
        __ StoreToOffset(kStoreWord, R2, SP, destination.GetHighStackIndex(kArmWordSize));
      } else {
        __ StoreToOffset(kStoreWordPair, source.AsRegisterPairLow<Register>(),
                         SP, destination.GetStackIndex());
      }
    } else if (source.IsQuickParameter()) {
      InvokeDexCallingConvention calling_convention;
      uint16_t register_index = source.GetQuickParameterRegisterIndex();
      uint16_t stack_index = source.GetQuickParameterStackIndex();
      __ StoreToOffset(kStoreWord, calling_convention.GetRegisterAt(register_index),
             SP, destination.GetStackIndex());
      __ LoadFromOffset(kLoadWord, R0,
             SP, calling_convention.GetStackOffsetOf(stack_index + 1) + GetFrameSize());
      __ StoreToOffset(kStoreWord, R0, SP, destination.GetHighStackIndex(kArmWordSize));
    } else if (source.IsFpuRegisterPair()) {
      __ StoreDToOffset(FromLowSToD(source.AsFpuRegisterPairLow<SRegister>()),
                        SP,
                        destination.GetStackIndex());
    } else {
      DCHECK(source.IsDoubleStackSlot());
      __ LoadFromOffset(kLoadWord, IP, SP, source.GetStackIndex());
      __ StoreToOffset(kStoreWord, IP, SP, destination.GetStackIndex());
      __ LoadFromOffset(kLoadWord, IP, SP, source.GetHighStackIndex(kArmWordSize));
      __ StoreToOffset(kStoreWord, IP, SP, destination.GetHighStackIndex(kArmWordSize));
    }
  }
}

void CodeGeneratorARM::Move(HInstruction* instruction, Location location, HInstruction* move_for) {
  LocationSummary* locations = instruction->GetLocations();
  if (locations != nullptr && locations->Out().Equals(location)) {
    return;
  }

  if (locations != nullptr && locations->Out().IsConstant()) {
    HConstant* const_to_move = locations->Out().GetConstant();
    if (const_to_move->IsIntConstant()) {
      int32_t value = const_to_move->AsIntConstant()->GetValue();
      if (location.IsRegister()) {
        __ LoadImmediate(location.As<Register>(), value);
      } else {
        DCHECK(location.IsStackSlot());
        __ LoadImmediate(IP, value);
        __ StoreToOffset(kStoreWord, IP, SP, location.GetStackIndex());
      }
    } else if (const_to_move->IsLongConstant()) {
      int64_t value = const_to_move->AsLongConstant()->GetValue();
      if (location.IsRegisterPair()) {
        __ LoadImmediate(location.AsRegisterPairLow<Register>(), Low32Bits(value));
        __ LoadImmediate(location.AsRegisterPairHigh<Register>(), High32Bits(value));
      } else {
        DCHECK(location.IsDoubleStackSlot());
        __ LoadImmediate(IP, Low32Bits(value));
        __ StoreToOffset(kStoreWord, IP, SP, location.GetStackIndex());
        __ LoadImmediate(IP, High32Bits(value));
        __ StoreToOffset(kStoreWord, IP, SP, location.GetHighStackIndex(kArmWordSize));
      }
    }
  } else if (instruction->IsLoadLocal()) {
    uint32_t stack_slot = GetStackSlot(instruction->AsLoadLocal()->GetLocal());
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimInt:
      case Primitive::kPrimNot:
      case Primitive::kPrimFloat:
        Move32(location, Location::StackSlot(stack_slot));
        break;

      case Primitive::kPrimLong:
      case Primitive::kPrimDouble:
        Move64(location, Location::DoubleStackSlot(stack_slot));
        break;

      default:
        LOG(FATAL) << "Unexpected type " << instruction->GetType();
    }
  } else if (instruction->IsTemporary()) {
    Location temp_location = GetTemporaryLocation(instruction->AsTemporary());
    if (temp_location.IsStackSlot()) {
      Move32(location, temp_location);
    } else {
      DCHECK(temp_location.IsDoubleStackSlot());
      Move64(location, temp_location);
    }
  } else {
    DCHECK((instruction->GetNext() == move_for) || instruction->GetNext()->IsTemporary());
    switch (instruction->GetType()) {
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
      case Primitive::kPrimNot:
      case Primitive::kPrimInt:
      case Primitive::kPrimFloat:
        Move32(location, locations->Out());
        break;

      case Primitive::kPrimLong:
      case Primitive::kPrimDouble:
        Move64(location, locations->Out());
        break;

      default:
        LOG(FATAL) << "Unexpected type " << instruction->GetType();
    }
  }
}

void CodeGeneratorARM::InvokeRuntime(int32_t entry_point_offset,
                                     HInstruction* instruction,
                                     uint32_t dex_pc) {
  __ LoadFromOffset(kLoadWord, LR, TR, entry_point_offset);
  __ blx(LR);
  RecordPcInfo(instruction, dex_pc);
  DCHECK(instruction->IsSuspendCheck()
      || instruction->IsBoundsCheck()
      || instruction->IsNullCheck()
      || instruction->IsDivZeroCheck()
      || !IsLeafMethod());
}

void LocationsBuilderARM::VisitGoto(HGoto* got) {
  got->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitGoto(HGoto* got) {
  HBasicBlock* successor = got->GetSuccessor();
  DCHECK(!successor->IsExitBlock());

  HBasicBlock* block = got->GetBlock();
  HInstruction* previous = got->GetPrevious();

  HLoopInformation* info = block->GetLoopInformation();
  if (info != nullptr && info->IsBackEdge(block) && info->HasSuspendCheck()) {
    codegen_->ClearSpillSlotsFromLoopPhisInStackMap(info->GetSuspendCheck());
    GenerateSuspendCheck(info->GetSuspendCheck(), successor);
    return;
  }

  if (block->IsEntryBlock() && (previous != nullptr) && previous->IsSuspendCheck()) {
    GenerateSuspendCheck(previous->AsSuspendCheck(), nullptr);
  }
  if (!codegen_->GoesToNextBlock(got->GetBlock(), successor)) {
    __ b(codegen_->GetLabelOf(successor));
  }
}

void LocationsBuilderARM::VisitExit(HExit* exit) {
  exit->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitExit(HExit* exit) {
  UNUSED(exit);
  if (kIsDebugBuild) {
    __ Comment("Unreachable");
    __ bkpt(0);
  }
}

void LocationsBuilderARM::VisitIf(HIf* if_instr) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(if_instr, LocationSummary::kNoCall);
  HInstruction* cond = if_instr->InputAt(0);
  if (!cond->IsCondition() || cond->AsCondition()->NeedsMaterialization()) {
    locations->SetInAt(0, Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM::VisitIf(HIf* if_instr) {
  HInstruction* cond = if_instr->InputAt(0);
  if (cond->IsIntConstant()) {
    // Constant condition, statically compared against 1.
    int32_t cond_value = cond->AsIntConstant()->GetValue();
    if (cond_value == 1) {
      if (!codegen_->GoesToNextBlock(if_instr->GetBlock(),
                                     if_instr->IfTrueSuccessor())) {
        __ b(codegen_->GetLabelOf(if_instr->IfTrueSuccessor()));
      }
      return;
    } else {
      DCHECK_EQ(cond_value, 0);
    }
  } else {
    if (!cond->IsCondition() || cond->AsCondition()->NeedsMaterialization()) {
      // Condition has been materialized, compare the output to 0
      DCHECK(if_instr->GetLocations()->InAt(0).IsRegister());
      __ cmp(if_instr->GetLocations()->InAt(0).As<Register>(),
             ShifterOperand(0));
      __ b(codegen_->GetLabelOf(if_instr->IfTrueSuccessor()), NE);
    } else {
      // Condition has not been materialized, use its inputs as the
      // comparison and its condition as the branch condition.
      LocationSummary* locations = cond->GetLocations();
      if (locations->InAt(1).IsRegister()) {
        __ cmp(locations->InAt(0).As<Register>(),
               ShifterOperand(locations->InAt(1).As<Register>()));
      } else {
        DCHECK(locations->InAt(1).IsConstant());
        int32_t value =
            locations->InAt(1).GetConstant()->AsIntConstant()->GetValue();
        ShifterOperand operand;
        if (ShifterOperand::CanHoldArm(value, &operand)) {
          __ cmp(locations->InAt(0).As<Register>(), ShifterOperand(value));
        } else {
          Register temp = IP;
          __ LoadImmediate(temp, value);
          __ cmp(locations->InAt(0).As<Register>(), ShifterOperand(temp));
        }
      }
      __ b(codegen_->GetLabelOf(if_instr->IfTrueSuccessor()),
           ARMCondition(cond->AsCondition()->GetCondition()));
    }
  }
  if (!codegen_->GoesToNextBlock(if_instr->GetBlock(),
                                 if_instr->IfFalseSuccessor())) {
    __ b(codegen_->GetLabelOf(if_instr->IfFalseSuccessor()));
  }
}


void LocationsBuilderARM::VisitCondition(HCondition* comp) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(comp, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(comp->InputAt(1)));
  if (comp->NeedsMaterialization()) {
    locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
  }
}

void InstructionCodeGeneratorARM::VisitCondition(HCondition* comp) {
  if (!comp->NeedsMaterialization()) return;

  LocationSummary* locations = comp->GetLocations();
  if (locations->InAt(1).IsRegister()) {
    __ cmp(locations->InAt(0).As<Register>(),
           ShifterOperand(locations->InAt(1).As<Register>()));
  } else {
    DCHECK(locations->InAt(1).IsConstant());
    int32_t value = locations->InAt(1).GetConstant()->AsIntConstant()->GetValue();
    ShifterOperand operand;
    if (ShifterOperand::CanHoldArm(value, &operand)) {
      __ cmp(locations->InAt(0).As<Register>(), ShifterOperand(value));
    } else {
      Register temp = IP;
      __ LoadImmediate(temp, value);
      __ cmp(locations->InAt(0).As<Register>(), ShifterOperand(temp));
    }
  }
  __ it(ARMCondition(comp->GetCondition()), kItElse);
  __ mov(locations->Out().As<Register>(), ShifterOperand(1),
         ARMCondition(comp->GetCondition()));
  __ mov(locations->Out().As<Register>(), ShifterOperand(0),
         ARMOppositeCondition(comp->GetCondition()));
}

void LocationsBuilderARM::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitEqual(HEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitNotEqual(HNotEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitLessThan(HLessThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitLessThanOrEqual(HLessThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitGreaterThan(HGreaterThan* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void InstructionCodeGeneratorARM::VisitGreaterThanOrEqual(HGreaterThanOrEqual* comp) {
  VisitCondition(comp);
}

void LocationsBuilderARM::VisitLocal(HLocal* local) {
  local->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitLocal(HLocal* local) {
  DCHECK_EQ(local->GetBlock(), GetGraph()->GetEntryBlock());
}

void LocationsBuilderARM::VisitLoadLocal(HLoadLocal* load) {
  load->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitLoadLocal(HLoadLocal* load) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(load);
}

void LocationsBuilderARM::VisitStoreLocal(HStoreLocal* store) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(store, LocationSummary::kNoCall);
  switch (store->InputAt(1)->GetType()) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte:
    case Primitive::kPrimChar:
    case Primitive::kPrimShort:
    case Primitive::kPrimInt:
    case Primitive::kPrimNot:
    case Primitive::kPrimFloat:
      locations->SetInAt(1, Location::StackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      locations->SetInAt(1, Location::DoubleStackSlot(codegen_->GetStackSlot(store->GetLocal())));
      break;

    default:
      LOG(FATAL) << "Unexpected local type " << store->InputAt(1)->GetType();
  }
}

void InstructionCodeGeneratorARM::VisitStoreLocal(HStoreLocal* store) {
  UNUSED(store);
}

void LocationsBuilderARM::VisitIntConstant(HIntConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM::VisitIntConstant(HIntConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM::VisitLongConstant(HLongConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM::VisitLongConstant(HLongConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM::VisitFloatConstant(HFloatConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM::VisitFloatConstant(HFloatConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM::VisitDoubleConstant(HDoubleConstant* constant) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(constant, LocationSummary::kNoCall);
  locations->SetOut(Location::ConstantLocation(constant));
}

void InstructionCodeGeneratorARM::VisitDoubleConstant(HDoubleConstant* constant) {
  // Will be generated at use site.
  UNUSED(constant);
}

void LocationsBuilderARM::VisitReturnVoid(HReturnVoid* ret) {
  ret->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitReturnVoid(HReturnVoid* ret) {
  UNUSED(ret);
  codegen_->GenerateFrameExit();
}

void LocationsBuilderARM::VisitReturn(HReturn* ret) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(ret, LocationSummary::kNoCall);
  locations->SetInAt(0, parameter_visitor_.GetReturnLocation(ret->InputAt(0)->GetType()));
}

void InstructionCodeGeneratorARM::VisitReturn(HReturn* ret) {
  UNUSED(ret);
  codegen_->GenerateFrameExit();
}

void LocationsBuilderARM::VisitInvokeStatic(HInvokeStatic* invoke) {
  HandleInvoke(invoke);
}

void CodeGeneratorARM::LoadCurrentMethod(Register reg) {
  __ LoadFromOffset(kLoadWord, reg, SP, kCurrentMethodStackOffset);
}

void InstructionCodeGeneratorARM::VisitInvokeStatic(HInvokeStatic* invoke) {
  Register temp = invoke->GetLocations()->GetTemp(0).As<Register>();

  // TODO: Implement all kinds of calls:
  // 1) boot -> boot
  // 2) app -> boot
  // 3) app -> app
  //
  // Currently we implement the app -> app logic, which looks up in the resolve cache.

  // temp = method;
  codegen_->LoadCurrentMethod(temp);
  // temp = temp->dex_cache_resolved_methods_;
  __ LoadFromOffset(
      kLoadWord, temp, temp, mirror::ArtMethod::DexCacheResolvedMethodsOffset().Int32Value());
  // temp = temp[index_in_cache]
  __ LoadFromOffset(
      kLoadWord, temp, temp, CodeGenerator::GetCacheOffset(invoke->GetIndexInDexCache()));
  // LR = temp[offset_of_quick_compiled_code]
  __ LoadFromOffset(kLoadWord, LR, temp,
                     mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(
                         kArmWordSize).Int32Value());
  // LR()
  __ blx(LR);

  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
  DCHECK(!codegen_->IsLeafMethod());
}

void LocationsBuilderARM::HandleInvoke(HInvoke* invoke) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(invoke, LocationSummary::kCall);
  locations->AddTemp(Location::RegisterLocation(R0));

  InvokeDexCallingConventionVisitor calling_convention_visitor;
  for (size_t i = 0; i < invoke->InputCount(); i++) {
    HInstruction* input = invoke->InputAt(i);
    locations->SetInAt(i, calling_convention_visitor.GetNextLocation(input->GetType()));
  }

  locations->SetOut(calling_convention_visitor.GetReturnLocation(invoke->GetType()));
}

void LocationsBuilderARM::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  HandleInvoke(invoke);
}

void InstructionCodeGeneratorARM::VisitInvokeVirtual(HInvokeVirtual* invoke) {
  Register temp = invoke->GetLocations()->GetTemp(0).As<Register>();
  uint32_t method_offset = mirror::Class::EmbeddedVTableOffset().Uint32Value() +
          invoke->GetVTableIndex() * sizeof(mirror::Class::VTableEntry);
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ LoadFromOffset(kLoadWord, temp, SP, receiver.GetStackIndex());
    __ LoadFromOffset(kLoadWord, temp, temp, class_offset);
  } else {
    __ LoadFromOffset(kLoadWord, temp, receiver.As<Register>(), class_offset);
  }
  // temp = temp->GetMethodAt(method_offset);
  uint32_t entry_point = mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(
      kArmWordSize).Int32Value();
  __ LoadFromOffset(kLoadWord, temp, temp, method_offset);
  // LR = temp->GetEntryPoint();
  __ LoadFromOffset(kLoadWord, LR, temp, entry_point);
  // LR();
  __ blx(LR);
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderARM::VisitInvokeInterface(HInvokeInterface* invoke) {
  HandleInvoke(invoke);
  // Add the hidden argument.
  invoke->GetLocations()->AddTemp(Location::RegisterLocation(R12));
}

void InstructionCodeGeneratorARM::VisitInvokeInterface(HInvokeInterface* invoke) {
  // TODO: b/18116999, our IMTs can miss an IncompatibleClassChangeError.
  Register temp = invoke->GetLocations()->GetTemp(0).As<Register>();
  uint32_t method_offset = mirror::Class::EmbeddedImTableOffset().Uint32Value() +
          (invoke->GetImtIndex() % mirror::Class::kImtSize) * sizeof(mirror::Class::ImTableEntry);
  LocationSummary* locations = invoke->GetLocations();
  Location receiver = locations->InAt(0);
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();

  // Set the hidden argument.
  __ LoadImmediate(invoke->GetLocations()->GetTemp(1).As<Register>(), invoke->GetDexMethodIndex());

  // temp = object->GetClass();
  if (receiver.IsStackSlot()) {
    __ LoadFromOffset(kLoadWord, temp, SP, receiver.GetStackIndex());
    __ LoadFromOffset(kLoadWord, temp, temp, class_offset);
  } else {
    __ LoadFromOffset(kLoadWord, temp, receiver.As<Register>(), class_offset);
  }
  // temp = temp->GetImtEntryAt(method_offset);
  uint32_t entry_point = mirror::ArtMethod::EntryPointFromQuickCompiledCodeOffset(
      kArmWordSize).Int32Value();
  __ LoadFromOffset(kLoadWord, temp, temp, method_offset);
  // LR = temp->GetEntryPoint();
  __ LoadFromOffset(kLoadWord, LR, temp, entry_point);
  // LR();
  __ blx(LR);
  DCHECK(!codegen_->IsLeafMethod());
  codegen_->RecordPcInfo(invoke, invoke->GetDexPc());
}

void LocationsBuilderARM::VisitNeg(HNeg* neg) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(neg, LocationSummary::kNoCall);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      bool output_overlaps = (neg->GetResultType() == Primitive::kPrimLong);
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), output_overlaps);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void InstructionCodeGeneratorARM::VisitNeg(HNeg* neg) {
  LocationSummary* locations = neg->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  switch (neg->GetResultType()) {
    case Primitive::kPrimInt:
      DCHECK(in.IsRegister());
      __ rsb(out.As<Register>(), in.As<Register>(), ShifterOperand(0));
      break;

    case Primitive::kPrimLong:
      DCHECK(in.IsRegisterPair());
      // out.lo = 0 - in.lo (and update the carry/borrow (C) flag)
      __ rsbs(out.AsRegisterPairLow<Register>(),
              in.AsRegisterPairLow<Register>(),
              ShifterOperand(0));
      // We cannot emit an RSC (Reverse Subtract with Carry)
      // instruction here, as it does not exist in the Thumb-2
      // instruction set.  We use the following approach
      // using SBC and SUB instead.
      //
      // out.hi = -C
      __ sbc(out.AsRegisterPairHigh<Register>(),
             out.AsRegisterPairHigh<Register>(),
             ShifterOperand(out.AsRegisterPairHigh<Register>()));
      // out.hi = out.hi - in.hi
      __ sub(out.AsRegisterPairHigh<Register>(),
             out.AsRegisterPairHigh<Register>(),
             ShifterOperand(in.AsRegisterPairHigh<Register>()));
      break;

    case Primitive::kPrimFloat:
      DCHECK(in.IsFpuRegister());
      __ vnegs(out.As<SRegister>(), in.As<SRegister>());
      break;

    case Primitive::kPrimDouble:
      DCHECK(in.IsFpuRegisterPair());
      __ vnegd(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(in.AsFpuRegisterPairLow<SRegister>()));
      break;

    default:
      LOG(FATAL) << "Unexpected neg type " << neg->GetResultType();
  }
}

void LocationsBuilderARM::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(conversion, LocationSummary::kNoCall);
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();
  switch (result_type) {
    case Primitive::kPrimByte:
      switch (input_type) {
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-byte' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimShort:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-short' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimInt:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Processing a Dex `long-to-int' instruction.
          locations->SetInAt(0, Location::Any());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        case Primitive::kPrimFloat:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimLong:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-long' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        case Primitive::kPrimFloat:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type << " to "
                     << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimChar:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-char' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimFloat:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-float' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimLong:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    case Primitive::kPrimDouble:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-double' instruction.
          locations->SetInAt(0, Location::RequiresRegister());
          locations->SetOut(Location::RequiresFpuRegister());
          break;

        case Primitive::kPrimLong:
        case Primitive::kPrimFloat:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    default:
      LOG(FATAL) << "Unexpected type conversion from " << input_type
                 << " to " << result_type;
  }
}

void InstructionCodeGeneratorARM::VisitTypeConversion(HTypeConversion* conversion) {
  LocationSummary* locations = conversion->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  Primitive::Type result_type = conversion->GetResultType();
  Primitive::Type input_type = conversion->GetInputType();
  switch (result_type) {
    case Primitive::kPrimByte:
      switch (input_type) {
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-byte' instruction.
          __ sbfx(out.As<Register>(), in.As<Register>(), 0, 8);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimShort:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-short' instruction.
          __ sbfx(out.As<Register>(), in.As<Register>(), 0, 16);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimInt:
      switch (input_type) {
        case Primitive::kPrimLong:
          // Processing a Dex `long-to-int' instruction.
          DCHECK(out.IsRegister());
          if (in.IsRegisterPair()) {
            __ Mov(out.As<Register>(), in.AsRegisterPairLow<Register>());
          } else if (in.IsDoubleStackSlot()) {
            __ LoadFromOffset(kLoadWord, out.As<Register>(), SP, in.GetStackIndex());
          } else {
            DCHECK(in.IsConstant());
            DCHECK(in.GetConstant()->IsLongConstant());
            int64_t value = in.GetConstant()->AsLongConstant()->GetValue();
            __ LoadImmediate(out.As<Register>(), static_cast<int32_t>(value));
          }
          break;

        case Primitive::kPrimFloat:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimLong:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-long' instruction.
          DCHECK(out.IsRegisterPair());
          DCHECK(in.IsRegister());
          __ Mov(out.AsRegisterPairLow<Register>(), in.As<Register>());
          // Sign extension.
          __ Asr(out.AsRegisterPairHigh<Register>(),
                 out.AsRegisterPairLow<Register>(),
                 31);
          break;

        case Primitive::kPrimFloat:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type << " to "
                     << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimChar:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar:
          // Processing a Dex `int-to-char' instruction.
          __ ubfx(out.As<Register>(), in.As<Register>(), 0, 16);
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      }
      break;

    case Primitive::kPrimFloat:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar: {
          // Processing a Dex `int-to-float' instruction.
          __ vmovsr(out.As<SRegister>(), in.As<Register>());
          __ vcvtsi(out.As<SRegister>(), out.As<SRegister>());
          break;
        }

        case Primitive::kPrimLong:
        case Primitive::kPrimDouble:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    case Primitive::kPrimDouble:
      switch (input_type) {
        case Primitive::kPrimByte:
        case Primitive::kPrimShort:
        case Primitive::kPrimInt:
        case Primitive::kPrimChar: {
          // Processing a Dex `int-to-double' instruction.
          __ vmovsr(out.AsFpuRegisterPairLow<SRegister>(), in.As<Register>());
          __ vcvtdi(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
                    out.AsFpuRegisterPairLow<SRegister>());
          break;
        }

        case Primitive::kPrimLong:
        case Primitive::kPrimFloat:
          LOG(FATAL) << "Type conversion from " << input_type
                     << " to " << result_type << " not yet implemented";
          break;

        default:
          LOG(FATAL) << "Unexpected type conversion from " << input_type
                     << " to " << result_type;
      };
      break;

    default:
      LOG(FATAL) << "Unexpected type conversion from " << input_type
                 << " to " << result_type;
  }
}

void LocationsBuilderARM::VisitAdd(HAdd* add) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(add, LocationSummary::kNoCall);
  switch (add->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      bool output_overlaps = (add->GetResultType() == Primitive::kPrimLong);
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(add->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), output_overlaps);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
  }
}

void InstructionCodeGeneratorARM::VisitAdd(HAdd* add) {
  LocationSummary* locations = add->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  switch (add->GetResultType()) {
    case Primitive::kPrimInt:
      if (second.IsRegister()) {
        __ add(out.As<Register>(), first.As<Register>(), ShifterOperand(second.As<Register>()));
      } else {
        __ AddConstant(out.As<Register>(),
                       first.As<Register>(),
                       second.GetConstant()->AsIntConstant()->GetValue());
      }
      break;

    case Primitive::kPrimLong:
      __ adds(out.AsRegisterPairLow<Register>(),
              first.AsRegisterPairLow<Register>(),
              ShifterOperand(second.AsRegisterPairLow<Register>()));
      __ adc(out.AsRegisterPairHigh<Register>(),
             first.AsRegisterPairHigh<Register>(),
             ShifterOperand(second.AsRegisterPairHigh<Register>()));
      break;

    case Primitive::kPrimFloat:
      __ vadds(out.As<SRegister>(), first.As<SRegister>(), second.As<SRegister>());
      break;

    case Primitive::kPrimDouble:
      __ vaddd(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(first.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(second.AsFpuRegisterPairLow<SRegister>()));
      break;

    default:
      LOG(FATAL) << "Unexpected add type " << add->GetResultType();
  }
}

void LocationsBuilderARM::VisitSub(HSub* sub) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(sub, LocationSummary::kNoCall);
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong: {
      bool output_overlaps = (sub->GetResultType() == Primitive::kPrimLong);
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(sub->InputAt(1)));
      locations->SetOut(Location::RequiresRegister(), output_overlaps);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void InstructionCodeGeneratorARM::VisitSub(HSub* sub) {
  LocationSummary* locations = sub->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  switch (sub->GetResultType()) {
    case Primitive::kPrimInt: {
      if (second.IsRegister()) {
        __ sub(out.As<Register>(), first.As<Register>(), ShifterOperand(second.As<Register>()));
      } else {
        __ AddConstant(out.As<Register>(),
                       first.As<Register>(),
                       -second.GetConstant()->AsIntConstant()->GetValue());
      }
      break;
    }

    case Primitive::kPrimLong: {
      __ subs(out.AsRegisterPairLow<Register>(),
              first.AsRegisterPairLow<Register>(),
              ShifterOperand(second.AsRegisterPairLow<Register>()));
      __ sbc(out.AsRegisterPairHigh<Register>(),
             first.AsRegisterPairHigh<Register>(),
             ShifterOperand(second.AsRegisterPairHigh<Register>()));
      break;
    }

    case Primitive::kPrimFloat: {
      __ vsubs(out.As<SRegister>(), first.As<SRegister>(), second.As<SRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ vsubd(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(first.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(second.AsFpuRegisterPairLow<SRegister>()));
      break;
    }


    default:
      LOG(FATAL) << "Unexpected sub type " << sub->GetResultType();
  }
}

void LocationsBuilderARM::VisitMul(HMul* mul) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(mul, LocationSummary::kNoCall);
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt:
    case Primitive::kPrimLong:  {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void InstructionCodeGeneratorARM::VisitMul(HMul* mul) {
  LocationSummary* locations = mul->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);
  switch (mul->GetResultType()) {
    case Primitive::kPrimInt: {
      __ mul(out.As<Register>(), first.As<Register>(), second.As<Register>());
      break;
    }
    case Primitive::kPrimLong: {
      Register out_hi = out.AsRegisterPairHigh<Register>();
      Register out_lo = out.AsRegisterPairLow<Register>();
      Register in1_hi = first.AsRegisterPairHigh<Register>();
      Register in1_lo = first.AsRegisterPairLow<Register>();
      Register in2_hi = second.AsRegisterPairHigh<Register>();
      Register in2_lo = second.AsRegisterPairLow<Register>();

      // Extra checks to protect caused by the existence of R1_R2.
      // The algorithm is wrong if out.hi is either in1.lo or in2.lo:
      // (e.g. in1=r0_r1, in2=r2_r3 and out=r1_r2);
      DCHECK_NE(out_hi, in1_lo);
      DCHECK_NE(out_hi, in2_lo);

      // input: in1 - 64 bits, in2 - 64 bits
      // output: out
      // formula: out.hi : out.lo = (in1.lo * in2.hi + in1.hi * in2.lo)* 2^32 + in1.lo * in2.lo
      // parts: out.hi = in1.lo * in2.hi + in1.hi * in2.lo + (in1.lo * in2.lo)[63:32]
      // parts: out.lo = (in1.lo * in2.lo)[31:0]

      // IP <- in1.lo * in2.hi
      __ mul(IP, in1_lo, in2_hi);
      // out.hi <- in1.lo * in2.hi + in1.hi * in2.lo
      __ mla(out_hi, in1_hi, in2_lo, IP);
      // out.lo <- (in1.lo * in2.lo)[31:0];
      __ umull(out_lo, IP, in1_lo, in2_lo);
      // out.hi <- in2.hi * in1.lo +  in2.lo * in1.hi + (in1.lo * in2.lo)[63:32]
      __ add(out_hi, out_hi, ShifterOperand(IP));
      break;
    }

    case Primitive::kPrimFloat: {
      __ vmuls(out.As<SRegister>(), first.As<SRegister>(), second.As<SRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ vmuld(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(first.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(second.AsFpuRegisterPairLow<SRegister>()));
      break;
    }

    default:
      LOG(FATAL) << "Unexpected mul type " << mul->GetResultType();
  }
}

void LocationsBuilderARM::VisitDiv(HDiv* div) {
  LocationSummary::CallKind call_kind = div->GetResultType() == Primitive::kPrimLong
      ? LocationSummary::kCall
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(div, call_kind);

  switch (div->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(0), calling_convention.GetRegisterAt(1)));
      locations->SetInAt(1, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(2), calling_convention.GetRegisterAt(3)));
      // The runtime helper puts the output in R0,R2.
      locations->SetOut(Location::RegisterPairLocation(R0, R2));
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresFpuRegister(), Location::kNoOutputOverlap);
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void InstructionCodeGeneratorARM::VisitDiv(HDiv* div) {
  LocationSummary* locations = div->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);

  switch (div->GetResultType()) {
    case Primitive::kPrimInt: {
      __ sdiv(out.As<Register>(), first.As<Register>(), second.As<Register>());
      break;
    }

    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      DCHECK_EQ(calling_convention.GetRegisterAt(0), first.AsRegisterPairLow<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(1), first.AsRegisterPairHigh<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(2), second.AsRegisterPairLow<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(3), second.AsRegisterPairHigh<Register>());
      DCHECK_EQ(R0, out.AsRegisterPairLow<Register>());
      DCHECK_EQ(R2, out.AsRegisterPairHigh<Register>());

      codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pLdiv), div, div->GetDexPc());
      break;
    }

    case Primitive::kPrimFloat: {
      __ vdivs(out.As<SRegister>(), first.As<SRegister>(), second.As<SRegister>());
      break;
    }

    case Primitive::kPrimDouble: {
      __ vdivd(FromLowSToD(out.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(first.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(second.AsFpuRegisterPairLow<SRegister>()));
      break;
    }

    default:
      LOG(FATAL) << "Unexpected div type " << div->GetResultType();
  }
}

void LocationsBuilderARM::VisitRem(HRem* rem) {
  LocationSummary::CallKind call_kind = rem->GetResultType() == Primitive::kPrimLong
      ? LocationSummary::kCall
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(rem, call_kind);

  switch (rem->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      locations->AddTemp(Location::RequiresRegister());
      break;
    }
    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(0), calling_convention.GetRegisterAt(1)));
      locations->SetInAt(1, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(2), calling_convention.GetRegisterAt(3)));
      // The runtime helper puts the output in R2,R3.
      locations->SetOut(Location::RegisterPairLocation(R2, R3));
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      LOG(FATAL) << "Unimplemented rem type " << rem->GetResultType();
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << rem->GetResultType();
  }
}

void InstructionCodeGeneratorARM::VisitRem(HRem* rem) {
  LocationSummary* locations = rem->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);

  switch (rem->GetResultType()) {
    case Primitive::kPrimInt: {
      Register reg1 = first.As<Register>();
      Register reg2 = second.As<Register>();
      Register temp = locations->GetTemp(0).As<Register>();

      // temp = reg1 / reg2  (integer division)
      // temp = temp * reg2
      // dest = reg1 - temp
      __ sdiv(temp, reg1, reg2);
      __ mul(temp, temp, reg2);
      __ sub(out.As<Register>(), reg1, ShifterOperand(temp));
      break;
    }

    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      DCHECK_EQ(calling_convention.GetRegisterAt(0), first.AsRegisterPairLow<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(1), first.AsRegisterPairHigh<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(2), second.AsRegisterPairLow<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(3), second.AsRegisterPairHigh<Register>());
      DCHECK_EQ(R2, out.AsRegisterPairLow<Register>());
      DCHECK_EQ(R3, out.AsRegisterPairHigh<Register>());

      codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pLmod), rem, rem->GetDexPc());
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      LOG(FATAL) << "Unimplemented rem type " << rem->GetResultType();
      break;
    }

    default:
      LOG(FATAL) << "Unexpected rem type " << rem->GetResultType();
  }
}

void LocationsBuilderARM::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RegisterOrConstant(instruction->InputAt(0)));
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM::VisitDivZeroCheck(HDivZeroCheck* instruction) {
  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) DivZeroCheckSlowPathARM(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location value = locations->InAt(0);

  switch (instruction->GetType()) {
    case Primitive::kPrimInt: {
      if (value.IsRegister()) {
        __ cmp(value.As<Register>(), ShifterOperand(0));
        __ b(slow_path->GetEntryLabel(), EQ);
      } else {
        DCHECK(value.IsConstant()) << value;
        if (value.GetConstant()->AsIntConstant()->GetValue() == 0) {
          __ b(slow_path->GetEntryLabel());
        }
      }
      break;
    }
    case Primitive::kPrimLong: {
      if (value.IsRegisterPair()) {
        __ orrs(IP,
                value.AsRegisterPairLow<Register>(),
                ShifterOperand(value.AsRegisterPairHigh<Register>()));
        __ b(slow_path->GetEntryLabel(), EQ);
      } else {
        DCHECK(value.IsConstant()) << value;
        if (value.GetConstant()->AsLongConstant()->GetValue() == 0) {
          __ b(slow_path->GetEntryLabel());
        }
      }
      break;
    default:
      LOG(FATAL) << "Unexpected type for HDivZeroCheck " << instruction->GetType();
    }
  }
}

void LocationsBuilderARM::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary::CallKind call_kind = op->GetResultType() == Primitive::kPrimLong
      ? LocationSummary::kCall
      : LocationSummary::kNoCall;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(op, call_kind);

  switch (op->GetResultType()) {
    case Primitive::kPrimInt: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RegisterOrConstant(op->InputAt(1)));
      locations->SetOut(Location::RequiresRegister());
      break;
    }
    case Primitive::kPrimLong: {
      InvokeRuntimeCallingConvention calling_convention;
      locations->SetInAt(0, Location::RegisterPairLocation(
          calling_convention.GetRegisterAt(0), calling_convention.GetRegisterAt(1)));
      locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
      // The runtime helper puts the output in R0,R2.
      locations->SetOut(Location::RegisterPairLocation(R0, R2));
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << op->GetResultType();
  }
}

void InstructionCodeGeneratorARM::HandleShift(HBinaryOperation* op) {
  DCHECK(op->IsShl() || op->IsShr() || op->IsUShr());

  LocationSummary* locations = op->GetLocations();
  Location out = locations->Out();
  Location first = locations->InAt(0);
  Location second = locations->InAt(1);

  Primitive::Type type = op->GetResultType();
  switch (type) {
    case Primitive::kPrimInt: {
      Register out_reg = out.As<Register>();
      Register first_reg = first.As<Register>();
      // Arm doesn't mask the shift count so we need to do it ourselves.
      if (second.IsRegister()) {
        Register second_reg = second.As<Register>();
        __ and_(second_reg, second_reg, ShifterOperand(kMaxIntShiftValue));
        if (op->IsShl()) {
          __ Lsl(out_reg, first_reg, second_reg);
        } else if (op->IsShr()) {
          __ Asr(out_reg, first_reg, second_reg);
        } else {
          __ Lsr(out_reg, first_reg, second_reg);
        }
      } else {
        int32_t cst = second.GetConstant()->AsIntConstant()->GetValue();
        uint32_t shift_value = static_cast<uint32_t>(cst & kMaxIntShiftValue);
        if (shift_value == 0) {  // arm does not support shifting with 0 immediate.
          __ Mov(out_reg, first_reg);
        } else if (op->IsShl()) {
          __ Lsl(out_reg, first_reg, shift_value);
        } else if (op->IsShr()) {
          __ Asr(out_reg, first_reg, shift_value);
        } else {
          __ Lsr(out_reg, first_reg, shift_value);
        }
      }
      break;
    }
    case Primitive::kPrimLong: {
      // TODO: Inline the assembly instead of calling the runtime.
      InvokeRuntimeCallingConvention calling_convention;
      DCHECK_EQ(calling_convention.GetRegisterAt(0), first.AsRegisterPairLow<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(1), first.AsRegisterPairHigh<Register>());
      DCHECK_EQ(calling_convention.GetRegisterAt(2), second.As<Register>());
      DCHECK_EQ(R0, out.AsRegisterPairLow<Register>());
      DCHECK_EQ(R2, out.AsRegisterPairHigh<Register>());

      int32_t entry_point_offset;
      if (op->IsShl()) {
        entry_point_offset = QUICK_ENTRY_POINT(pShlLong);
      } else if (op->IsShr()) {
        entry_point_offset = QUICK_ENTRY_POINT(pShrLong);
      } else {
        entry_point_offset = QUICK_ENTRY_POINT(pUshrLong);
      }
      __ LoadFromOffset(kLoadWord, LR, TR, entry_point_offset);
      __ blx(LR);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected operation type " << type;
  }
}

void LocationsBuilderARM::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void InstructionCodeGeneratorARM::VisitShl(HShl* shl) {
  HandleShift(shl);
}

void LocationsBuilderARM::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void InstructionCodeGeneratorARM::VisitShr(HShr* shr) {
  HandleShift(shr);
}

void LocationsBuilderARM::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void InstructionCodeGeneratorARM::VisitUShr(HUShr* ushr) {
  HandleShift(ushr);
}

void LocationsBuilderARM::VisitNewInstance(HNewInstance* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(Location::RegisterLocation(R0));
}

void InstructionCodeGeneratorARM::VisitNewInstance(HNewInstance* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->LoadCurrentMethod(calling_convention.GetRegisterAt(1));
  __ LoadImmediate(calling_convention.GetRegisterAt(0), instruction->GetTypeIndex());
  codegen_->InvokeRuntime(
      QUICK_ENTRY_POINT(pAllocObjectWithAccessCheck), instruction, instruction->GetDexPc());
}

void LocationsBuilderARM::VisitNewArray(HNewArray* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
  locations->AddTemp(Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
  locations->SetOut(Location::RegisterLocation(R0));
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
}

void InstructionCodeGeneratorARM::VisitNewArray(HNewArray* instruction) {
  InvokeRuntimeCallingConvention calling_convention;
  codegen_->LoadCurrentMethod(calling_convention.GetRegisterAt(1));
  __ LoadImmediate(calling_convention.GetRegisterAt(0), instruction->GetTypeIndex());
  codegen_->InvokeRuntime(
      QUICK_ENTRY_POINT(pAllocArrayWithAccessCheck), instruction, instruction->GetDexPc());
}

void LocationsBuilderARM::VisitParameterValue(HParameterValue* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  Location location = parameter_visitor_.GetNextLocation(instruction->GetType());
  if (location.IsStackSlot()) {
    location = Location::StackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  } else if (location.IsDoubleStackSlot()) {
    location = Location::DoubleStackSlot(location.GetStackIndex() + codegen_->GetFrameSize());
  }
  locations->SetOut(location);
}

void InstructionCodeGeneratorARM::VisitParameterValue(HParameterValue* instruction) {
  // Nothing to do, the parameter is already at its location.
  UNUSED(instruction);
}

void LocationsBuilderARM::VisitNot(HNot* not_) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(not_, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM::VisitNot(HNot* not_) {
  LocationSummary* locations = not_->GetLocations();
  Location out = locations->Out();
  Location in = locations->InAt(0);
  switch (not_->InputAt(0)->GetType()) {
    case Primitive::kPrimBoolean:
      __ eor(out.As<Register>(), in.As<Register>(), ShifterOperand(1));
      break;

    case Primitive::kPrimInt:
      __ mvn(out.As<Register>(), ShifterOperand(in.As<Register>()));
      break;

    case Primitive::kPrimLong:
      __ mvn(out.AsRegisterPairLow<Register>(),
             ShifterOperand(in.AsRegisterPairLow<Register>()));
      __ mvn(out.AsRegisterPairHigh<Register>(),
             ShifterOperand(in.AsRegisterPairHigh<Register>()));
      break;

    default:
      LOG(FATAL) << "Unimplemented type for not operation " << not_->GetResultType();
  }
}

void LocationsBuilderARM::VisitCompare(HCompare* compare) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(compare, LocationSummary::kNoCall);
  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimLong: {
      locations->SetInAt(0, Location::RequiresRegister());
      locations->SetInAt(1, Location::RequiresRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble: {
      locations->SetInAt(0, Location::RequiresFpuRegister());
      locations->SetInAt(1, Location::RequiresFpuRegister());
      locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected type for compare operation " << compare->InputAt(0)->GetType();
  }
}

void InstructionCodeGeneratorARM::VisitCompare(HCompare* compare) {
  LocationSummary* locations = compare->GetLocations();
  Register out = locations->Out().As<Register>();
  Location left = locations->InAt(0);
  Location right = locations->InAt(1);

  Label less, greater, done;
  switch (compare->InputAt(0)->GetType()) {
    case Primitive::kPrimLong: {
      __ cmp(left.AsRegisterPairHigh<Register>(),
             ShifterOperand(right.AsRegisterPairHigh<Register>()));  // Signed compare.
      __ b(&less, LT);
      __ b(&greater, GT);
      // Do LoadImmediate before any `cmp`, as LoadImmediate might affect the status flags.
      __ LoadImmediate(out, 0);
      __ cmp(left.AsRegisterPairLow<Register>(),
             ShifterOperand(right.AsRegisterPairLow<Register>()));  // Unsigned compare.
      break;
    }
    case Primitive::kPrimFloat: {
      __ LoadImmediate(out, 0);
      __ vcmps(left.As<SRegister>(), right.As<SRegister>());
      __ b(compare->IsGtBias() ? &greater : &less, VS);  // VS for unordered
      break;
    }
    case Primitive::kPrimDouble: {
      __ LoadImmediate(out, 0);
      __ vcmpd(FromLowSToD(left.AsFpuRegisterPairLow<SRegister>()),
               FromLowSToD(right.AsFpuRegisterPairLow<SRegister>()));
      __ b(compare->IsGtBias() ? &greater : &less, VS);
      break;
    }
    default:
      LOG(FATAL) << "Unexpected compare type " << compare->InputAt(0)->GetType();
  }
  __ b(&done, EQ);
  __ b(&less, CC);  // CC is for both: unsigned compare for longs and 'less than' for floats.

  __ Bind(&greater);
  __ LoadImmediate(out, 1);
  __ b(&done);

  __ Bind(&less);
  __ LoadImmediate(out, -1);

  __ Bind(&done);
}

void LocationsBuilderARM::VisitPhi(HPhi* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  for (size_t i = 0, e = instruction->InputCount(); i < e; ++i) {
    locations->SetInAt(i, Location::Any());
  }
  locations->SetOut(Location::Any());
}

void InstructionCodeGeneratorARM::VisitPhi(HPhi* instruction) {
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

void LocationsBuilderARM::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(instruction->GetFieldType(), instruction->GetValue());
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // Temporary registers for the write barrier.
  if (needs_write_barrier) {
    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM::VisitInstanceFieldSet(HInstanceFieldSet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).As<Register>();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();
  Primitive::Type field_type = instruction->GetFieldType();

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      Register value = locations->InAt(1).As<Register>();
      __ StoreToOffset(kStoreByte, value, obj, offset);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      Register value = locations->InAt(1).As<Register>();
      __ StoreToOffset(kStoreHalfword, value, obj, offset);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register value = locations->InAt(1).As<Register>();
      __ StoreToOffset(kStoreWord, value, obj, offset);
      if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->GetValue())) {
        Register temp = locations->GetTemp(0).As<Register>();
        Register card = locations->GetTemp(1).As<Register>();
        codegen_->MarkGCCard(temp, card, obj, value);
      }
      break;
    }

    case Primitive::kPrimLong: {
      Location value = locations->InAt(1);
      __ StoreToOffset(kStoreWordPair, value.AsRegisterPairLow<Register>(), obj, offset);
      break;
    }

    case Primitive::kPrimFloat: {
      SRegister value = locations->InAt(1).As<SRegister>();
      __ StoreSToOffset(value, obj, offset);
      break;
    }

    case Primitive::kPrimDouble: {
      DRegister value = FromLowSToD(locations->InAt(1).AsFpuRegisterPairLow<SRegister>());
      __ StoreDToOffset(value, obj, offset);
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }
}

void LocationsBuilderARM::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM::VisitInstanceFieldGet(HInstanceFieldGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).As<Register>();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      Register out = locations->Out().As<Register>();
      __ LoadFromOffset(kLoadUnsignedByte, out, obj, offset);
      break;
    }

    case Primitive::kPrimByte: {
      Register out = locations->Out().As<Register>();
      __ LoadFromOffset(kLoadSignedByte, out, obj, offset);
      break;
    }

    case Primitive::kPrimShort: {
      Register out = locations->Out().As<Register>();
      __ LoadFromOffset(kLoadSignedHalfword, out, obj, offset);
      break;
    }

    case Primitive::kPrimChar: {
      Register out = locations->Out().As<Register>();
      __ LoadFromOffset(kLoadUnsignedHalfword, out, obj, offset);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register out = locations->Out().As<Register>();
      __ LoadFromOffset(kLoadWord, out, obj, offset);
      break;
    }

    case Primitive::kPrimLong: {
      // TODO: support volatile.
      Location out = locations->Out();
      __ LoadFromOffset(kLoadWordPair, out.AsRegisterPairLow<Register>(), obj, offset);
      break;
    }

    case Primitive::kPrimFloat: {
      SRegister out = locations->Out().As<SRegister>();
      __ LoadSFromOffset(out, obj, offset);
      break;
    }

    case Primitive::kPrimDouble: {
      DRegister out = FromLowSToD(locations->Out().AsFpuRegisterPairLow<SRegister>());
      __ LoadDFromOffset(out, obj, offset);
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderARM::VisitNullCheck(HNullCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM::VisitNullCheck(HNullCheck* instruction) {
  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) NullCheckSlowPathARM(instruction);
  codegen_->AddSlowPath(slow_path);

  LocationSummary* locations = instruction->GetLocations();
  Location obj = locations->InAt(0);

  if (obj.IsRegister()) {
    __ cmp(obj.As<Register>(), ShifterOperand(0));
    __ b(slow_path->GetEntryLabel(), EQ);
  } else {
    DCHECK(obj.IsConstant()) << obj;
    DCHECK_EQ(obj.GetConstant()->AsIntConstant()->GetValue(), 0);
    __ b(slow_path->GetEntryLabel());
  }
}

void LocationsBuilderARM::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM::VisitArrayGet(HArrayGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).As<Register>();
  Location index = locations->InAt(1);

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Register out = locations->Out().As<Register>();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ LoadFromOffset(kLoadUnsignedByte, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.As<Register>()));
        __ LoadFromOffset(kLoadUnsignedByte, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int8_t)).Uint32Value();
      Register out = locations->Out().As<Register>();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ LoadFromOffset(kLoadSignedByte, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.As<Register>()));
        __ LoadFromOffset(kLoadSignedByte, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimShort: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int16_t)).Uint32Value();
      Register out = locations->Out().As<Register>();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ LoadFromOffset(kLoadSignedHalfword, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.As<Register>(), LSL, TIMES_2));
        __ LoadFromOffset(kLoadSignedHalfword, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      Register out = locations->Out().As<Register>();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ LoadFromOffset(kLoadUnsignedHalfword, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.As<Register>(), LSL, TIMES_2));
        __ LoadFromOffset(kLoadUnsignedHalfword, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      DCHECK_EQ(sizeof(mirror::HeapReference<mirror::Object>), sizeof(int32_t));
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
      Register out = locations->Out().As<Register>();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
        __ LoadFromOffset(kLoadWord, out, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.As<Register>(), LSL, TIMES_4));
        __ LoadFromOffset(kLoadWord, out, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      Location out = locations->Out();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ LoadFromOffset(kLoadWordPair, out.AsRegisterPairLow<Register>(), obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.As<Register>(), LSL, TIMES_8));
        __ LoadFromOffset(kLoadWordPair, out.AsRegisterPairLow<Register>(), IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();
      UNREACHABLE();
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderARM::VisitArraySet(HArraySet* instruction) {
  Primitive::Type value_type = instruction->GetComponentType();

  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());
  bool needs_runtime_call = instruction->NeedsTypeCheck();

  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, needs_runtime_call ? LocationSummary::kCall : LocationSummary::kNoCall);
  if (needs_runtime_call) {
    InvokeRuntimeCallingConvention calling_convention;
    locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
    locations->SetInAt(1, Location::RegisterLocation(calling_convention.GetRegisterAt(1)));
    locations->SetInAt(2, Location::RegisterLocation(calling_convention.GetRegisterAt(2)));
  } else {
    locations->SetInAt(0, Location::RequiresRegister());
    locations->SetInAt(1, Location::RegisterOrConstant(instruction->InputAt(1)));
    locations->SetInAt(2, Location::RequiresRegister());

    if (needs_write_barrier) {
      // Temporary registers for the write barrier.
      locations->AddTemp(Location::RequiresRegister());
      locations->AddTemp(Location::RequiresRegister());
    }
  }
}

void InstructionCodeGeneratorARM::VisitArraySet(HArraySet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).As<Register>();
  Location index = locations->InAt(1);
  Primitive::Type value_type = instruction->GetComponentType();
  bool needs_runtime_call = locations->WillCall();
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(value_type, instruction->GetValue());

  switch (value_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint8_t)).Uint32Value();
      Register value = locations->InAt(2).As<Register>();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_1) + data_offset;
        __ StoreToOffset(kStoreByte, value, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.As<Register>()));
        __ StoreToOffset(kStoreByte, value, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(uint16_t)).Uint32Value();
      Register value = locations->InAt(2).As<Register>();
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_2) + data_offset;
        __ StoreToOffset(kStoreHalfword, value, obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.As<Register>(), LSL, TIMES_2));
        __ StoreToOffset(kStoreHalfword, value, IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      if (!needs_runtime_call) {
        uint32_t data_offset = mirror::Array::DataOffset(sizeof(int32_t)).Uint32Value();
        Register value = locations->InAt(2).As<Register>();
        if (index.IsConstant()) {
          size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_4) + data_offset;
          __ StoreToOffset(kStoreWord, value, obj, offset);
        } else {
          DCHECK(index.IsRegister()) << index;
          __ add(IP, obj, ShifterOperand(index.As<Register>(), LSL, TIMES_4));
          __ StoreToOffset(kStoreWord, value, IP, data_offset);
        }
        if (needs_write_barrier) {
          DCHECK_EQ(value_type, Primitive::kPrimNot);
          Register temp = locations->GetTemp(0).As<Register>();
          Register card = locations->GetTemp(1).As<Register>();
          codegen_->MarkGCCard(temp, card, obj, value);
        }
      } else {
        DCHECK_EQ(value_type, Primitive::kPrimNot);
        codegen_->InvokeRuntime(QUICK_ENTRY_POINT(pAputObject), instruction, instruction->GetDexPc());
      }
      break;
    }

    case Primitive::kPrimLong: {
      uint32_t data_offset = mirror::Array::DataOffset(sizeof(int64_t)).Uint32Value();
      Location value = locations->InAt(2);
      if (index.IsConstant()) {
        size_t offset = (index.GetConstant()->AsIntConstant()->GetValue() << TIMES_8) + data_offset;
        __ StoreToOffset(kStoreWordPair, value.AsRegisterPairLow<Register>(), obj, offset);
      } else {
        __ add(IP, obj, ShifterOperand(index.As<Register>(), LSL, TIMES_8));
        __ StoreToOffset(kStoreWordPair, value.AsRegisterPairLow<Register>(), IP, data_offset);
      }
      break;
    }

    case Primitive::kPrimFloat:
    case Primitive::kPrimDouble:
      LOG(FATAL) << "Unimplemented register type " << instruction->GetType();
      UNREACHABLE();
    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderARM::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM::VisitArrayLength(HArrayLength* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  uint32_t offset = mirror::Array::LengthOffset().Uint32Value();
  Register obj = locations->InAt(0).As<Register>();
  Register out = locations->Out().As<Register>();
  __ LoadFromOffset(kLoadWord, out, obj, offset);
}

void LocationsBuilderARM::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  if (instruction->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM::VisitBoundsCheck(HBoundsCheck* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) BoundsCheckSlowPathARM(
      instruction, locations->InAt(0), locations->InAt(1));
  codegen_->AddSlowPath(slow_path);

  Register index = locations->InAt(0).As<Register>();
  Register length = locations->InAt(1).As<Register>();

  __ cmp(index, ShifterOperand(length));
  __ b(slow_path->GetEntryLabel(), CS);
}

void CodeGeneratorARM::MarkGCCard(Register temp, Register card, Register object, Register value) {
  Label is_null;
  __ CompareAndBranchIfZero(value, &is_null);
  __ LoadFromOffset(kLoadWord, card, TR, Thread::CardTableOffset<kArmWordSize>().Int32Value());
  __ Lsr(temp, object, gc::accounting::CardTable::kCardShift);
  __ strb(card, Address(card, temp));
  __ Bind(&is_null);
}

void LocationsBuilderARM::VisitTemporary(HTemporary* temp) {
  temp->SetLocations(nullptr);
}

void InstructionCodeGeneratorARM::VisitTemporary(HTemporary* temp) {
  // Nothing to do, this is driven by the code generator.
  UNUSED(temp);
}

void LocationsBuilderARM::VisitParallelMove(HParallelMove* instruction) {
  UNUSED(instruction);
  LOG(FATAL) << "Unreachable";
}

void InstructionCodeGeneratorARM::VisitParallelMove(HParallelMove* instruction) {
  codegen_->GetMoveResolver()->EmitNativeCode(instruction);
}

void LocationsBuilderARM::VisitSuspendCheck(HSuspendCheck* instruction) {
  new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCallOnSlowPath);
}

void InstructionCodeGeneratorARM::VisitSuspendCheck(HSuspendCheck* instruction) {
  HBasicBlock* block = instruction->GetBlock();
  if (block->GetLoopInformation() != nullptr) {
    DCHECK(block->GetLoopInformation()->GetSuspendCheck() == instruction);
    // The back edge will generate the suspend check.
    return;
  }
  if (block->IsEntryBlock() && instruction->GetNext()->IsGoto()) {
    // The goto will generate the suspend check.
    return;
  }
  GenerateSuspendCheck(instruction, nullptr);
}

void InstructionCodeGeneratorARM::GenerateSuspendCheck(HSuspendCheck* instruction,
                                                       HBasicBlock* successor) {
  SuspendCheckSlowPathARM* slow_path =
      new (GetGraph()->GetArena()) SuspendCheckSlowPathARM(instruction, successor);
  codegen_->AddSlowPath(slow_path);

  __ LoadFromOffset(
      kLoadUnsignedHalfword, IP, TR, Thread::ThreadFlagsOffset<kArmWordSize>().Int32Value());
  __ cmp(IP, ShifterOperand(0));
  // TODO: Figure out the branch offsets and use cbz/cbnz.
  if (successor == nullptr) {
    __ b(slow_path->GetEntryLabel(), NE);
    __ Bind(slow_path->GetReturnLabel());
  } else {
    __ b(codegen_->GetLabelOf(successor), EQ);
    __ b(slow_path->GetEntryLabel());
  }
}

ArmAssembler* ParallelMoveResolverARM::GetAssembler() const {
  return codegen_->GetAssembler();
}

void ParallelMoveResolverARM::EmitMove(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister()) {
    if (destination.IsRegister()) {
      __ Mov(destination.As<Register>(), source.As<Register>());
    } else {
      DCHECK(destination.IsStackSlot());
      __ StoreToOffset(kStoreWord, source.As<Register>(),
                       SP, destination.GetStackIndex());
    }
  } else if (source.IsStackSlot()) {
    if (destination.IsRegister()) {
      __ LoadFromOffset(kLoadWord, destination.As<Register>(),
                        SP, source.GetStackIndex());
    } else {
      DCHECK(destination.IsStackSlot());
      __ LoadFromOffset(kLoadWord, IP, SP, source.GetStackIndex());
      __ StoreToOffset(kStoreWord, IP, SP, destination.GetStackIndex());
    }
  } else {
    DCHECK(source.IsConstant());
    DCHECK(source.GetConstant()->IsIntConstant());
    int32_t value = source.GetConstant()->AsIntConstant()->GetValue();
    if (destination.IsRegister()) {
      __ LoadImmediate(destination.As<Register>(), value);
    } else {
      DCHECK(destination.IsStackSlot());
      __ LoadImmediate(IP, value);
      __ StoreToOffset(kStoreWord, IP, SP, destination.GetStackIndex());
    }
  }
}

void ParallelMoveResolverARM::Exchange(Register reg, int mem) {
  __ Mov(IP, reg);
  __ LoadFromOffset(kLoadWord, reg, SP, mem);
  __ StoreToOffset(kStoreWord, IP, SP, mem);
}

void ParallelMoveResolverARM::Exchange(int mem1, int mem2) {
  ScratchRegisterScope ensure_scratch(this, IP, R0, codegen_->GetNumberOfCoreRegisters());
  int stack_offset = ensure_scratch.IsSpilled() ? kArmWordSize : 0;
  __ LoadFromOffset(kLoadWord, static_cast<Register>(ensure_scratch.GetRegister()),
                    SP, mem1 + stack_offset);
  __ LoadFromOffset(kLoadWord, IP, SP, mem2 + stack_offset);
  __ StoreToOffset(kStoreWord, static_cast<Register>(ensure_scratch.GetRegister()),
                   SP, mem2 + stack_offset);
  __ StoreToOffset(kStoreWord, IP, SP, mem1 + stack_offset);
}

void ParallelMoveResolverARM::EmitSwap(size_t index) {
  MoveOperands* move = moves_.Get(index);
  Location source = move->GetSource();
  Location destination = move->GetDestination();

  if (source.IsRegister() && destination.IsRegister()) {
    DCHECK_NE(source.As<Register>(), IP);
    DCHECK_NE(destination.As<Register>(), IP);
    __ Mov(IP, source.As<Register>());
    __ Mov(source.As<Register>(), destination.As<Register>());
    __ Mov(destination.As<Register>(), IP);
  } else if (source.IsRegister() && destination.IsStackSlot()) {
    Exchange(source.As<Register>(), destination.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsRegister()) {
    Exchange(destination.As<Register>(), source.GetStackIndex());
  } else if (source.IsStackSlot() && destination.IsStackSlot()) {
    Exchange(source.GetStackIndex(), destination.GetStackIndex());
  } else {
    LOG(FATAL) << "Unimplemented";
  }
}

void ParallelMoveResolverARM::SpillScratch(int reg) {
  __ Push(static_cast<Register>(reg));
}

void ParallelMoveResolverARM::RestoreScratch(int reg) {
  __ Pop(static_cast<Register>(reg));
}

void LocationsBuilderARM::VisitLoadClass(HLoadClass* cls) {
  LocationSummary::CallKind call_kind = cls->CanCallRuntime()
      ? LocationSummary::kCallOnSlowPath
      : LocationSummary::kNoCall;
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(cls, call_kind);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM::VisitLoadClass(HLoadClass* cls) {
  Register out = cls->GetLocations()->Out().As<Register>();
  if (cls->IsReferrersClass()) {
    DCHECK(!cls->CanCallRuntime());
    DCHECK(!cls->MustGenerateClinitCheck());
    codegen_->LoadCurrentMethod(out);
    __ LoadFromOffset(kLoadWord, out, out, mirror::ArtMethod::DeclaringClassOffset().Int32Value());
  } else {
    DCHECK(cls->CanCallRuntime());
    codegen_->LoadCurrentMethod(out);
    __ LoadFromOffset(
        kLoadWord, out, out, mirror::ArtMethod::DexCacheResolvedTypesOffset().Int32Value());
    __ LoadFromOffset(kLoadWord, out, out, CodeGenerator::GetCacheOffset(cls->GetTypeIndex()));

    SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathARM(
        cls, cls, cls->GetDexPc(), cls->MustGenerateClinitCheck());
    codegen_->AddSlowPath(slow_path);
    __ cmp(out, ShifterOperand(0));
    __ b(slow_path->GetEntryLabel(), EQ);
    if (cls->MustGenerateClinitCheck()) {
      GenerateClassInitializationCheck(slow_path, out);
    } else {
      __ Bind(slow_path->GetExitLabel());
    }
  }
}

void LocationsBuilderARM::VisitClinitCheck(HClinitCheck* check) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(check, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  if (check->HasUses()) {
    locations->SetOut(Location::SameAsFirstInput());
  }
}

void InstructionCodeGeneratorARM::VisitClinitCheck(HClinitCheck* check) {
  // We assume the class is not null.
  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) LoadClassSlowPathARM(
      check->GetLoadClass(), check, check->GetDexPc(), true);
  codegen_->AddSlowPath(slow_path);
  GenerateClassInitializationCheck(slow_path, check->GetLocations()->InAt(0).As<Register>());
}

void InstructionCodeGeneratorARM::GenerateClassInitializationCheck(
    SlowPathCodeARM* slow_path, Register class_reg) {
  __ LoadFromOffset(kLoadWord, IP, class_reg, mirror::Class::StatusOffset().Int32Value());
  __ cmp(IP, ShifterOperand(mirror::Class::kStatusInitialized));
  __ b(slow_path->GetEntryLabel(), LT);
  // Even if the initialized flag is set, we may be in a situation where caches are not synced
  // properly. Therefore, we do a memory fence.
  __ dmb(ISH);
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister(), Location::kNoOutputOverlap);
}

void InstructionCodeGeneratorARM::VisitStaticFieldGet(HStaticFieldGet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register cls = locations->InAt(0).As<Register>();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();

  switch (instruction->GetType()) {
    case Primitive::kPrimBoolean: {
      Register out = locations->Out().As<Register>();
      __ LoadFromOffset(kLoadUnsignedByte, out, cls, offset);
      break;
    }

    case Primitive::kPrimByte: {
      Register out = locations->Out().As<Register>();
      __ LoadFromOffset(kLoadSignedByte, out, cls, offset);
      break;
    }

    case Primitive::kPrimShort: {
      Register out = locations->Out().As<Register>();
      __ LoadFromOffset(kLoadSignedHalfword, out, cls, offset);
      break;
    }

    case Primitive::kPrimChar: {
      Register out = locations->Out().As<Register>();
      __ LoadFromOffset(kLoadUnsignedHalfword, out, cls, offset);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register out = locations->Out().As<Register>();
      __ LoadFromOffset(kLoadWord, out, cls, offset);
      break;
    }

    case Primitive::kPrimLong: {
      // TODO: support volatile.
      Location out = locations->Out();
      __ LoadFromOffset(kLoadWordPair, out.AsRegisterPairLow<Register>(), cls, offset);
      break;
    }

    case Primitive::kPrimFloat: {
      SRegister out = locations->Out().As<SRegister>();
      __ LoadSFromOffset(out, cls, offset);
      break;
    }

    case Primitive::kPrimDouble: {
      DRegister out = FromLowSToD(locations->Out().AsFpuRegisterPairLow<SRegister>());
      __ LoadDFromOffset(out, cls, offset);
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << instruction->GetType();
      UNREACHABLE();
  }
}

void LocationsBuilderARM::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  bool needs_write_barrier =
      CodeGenerator::StoreNeedsWriteBarrier(instruction->GetFieldType(), instruction->GetValue());
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  // Temporary registers for the write barrier.
  if (needs_write_barrier) {
    locations->AddTemp(Location::RequiresRegister());
    locations->AddTemp(Location::RequiresRegister());
  }
}

void InstructionCodeGeneratorARM::VisitStaticFieldSet(HStaticFieldSet* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register cls = locations->InAt(0).As<Register>();
  uint32_t offset = instruction->GetFieldOffset().Uint32Value();
  Primitive::Type field_type = instruction->GetFieldType();

  switch (field_type) {
    case Primitive::kPrimBoolean:
    case Primitive::kPrimByte: {
      Register value = locations->InAt(1).As<Register>();
      __ StoreToOffset(kStoreByte, value, cls, offset);
      break;
    }

    case Primitive::kPrimShort:
    case Primitive::kPrimChar: {
      Register value = locations->InAt(1).As<Register>();
      __ StoreToOffset(kStoreHalfword, value, cls, offset);
      break;
    }

    case Primitive::kPrimInt:
    case Primitive::kPrimNot: {
      Register value = locations->InAt(1).As<Register>();
      __ StoreToOffset(kStoreWord, value, cls, offset);
      if (CodeGenerator::StoreNeedsWriteBarrier(field_type, instruction->GetValue())) {
        Register temp = locations->GetTemp(0).As<Register>();
        Register card = locations->GetTemp(1).As<Register>();
        codegen_->MarkGCCard(temp, card, cls, value);
      }
      break;
    }

    case Primitive::kPrimLong: {
      Location value = locations->InAt(1);
      __ StoreToOffset(kStoreWordPair, value.AsRegisterPairLow<Register>(), cls, offset);
      break;
    }

    case Primitive::kPrimFloat: {
      SRegister value = locations->InAt(1).As<SRegister>();
      __ StoreSToOffset(value, cls, offset);
      break;
    }

    case Primitive::kPrimDouble: {
      DRegister value = FromLowSToD(locations->InAt(1).AsFpuRegisterPairLow<SRegister>());
      __ StoreDToOffset(value, cls, offset);
      break;
    }

    case Primitive::kPrimVoid:
      LOG(FATAL) << "Unreachable type " << field_type;
      UNREACHABLE();
  }
}

void LocationsBuilderARM::VisitLoadString(HLoadString* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kCallOnSlowPath);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM::VisitLoadString(HLoadString* load) {
  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) LoadStringSlowPathARM(load);
  codegen_->AddSlowPath(slow_path);

  Register out = load->GetLocations()->Out().As<Register>();
  codegen_->LoadCurrentMethod(out);
  __ LoadFromOffset(
      kLoadWord, out, out, mirror::ArtMethod::DexCacheStringsOffset().Int32Value());
  __ LoadFromOffset(kLoadWord, out, out, CodeGenerator::GetCacheOffset(load->GetStringIndex()));
  __ cmp(out, ShifterOperand(0));
  __ b(slow_path->GetEntryLabel(), EQ);
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM::VisitLoadException(HLoadException* load) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(load, LocationSummary::kNoCall);
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM::VisitLoadException(HLoadException* load) {
  Register out = load->GetLocations()->Out().As<Register>();
  int32_t offset = Thread::ExceptionOffset<kArmWordSize>().Int32Value();
  __ LoadFromOffset(kLoadWord, out, TR, offset);
  __ LoadImmediate(IP, 0);
  __ StoreToOffset(kStoreWord, IP, TR, offset);
}

void LocationsBuilderARM::VisitThrow(HThrow* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorARM::VisitThrow(HThrow* instruction) {
  codegen_->InvokeRuntime(
      QUICK_ENTRY_POINT(pDeliverException), instruction, instruction->GetDexPc());
}

void LocationsBuilderARM::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary::CallKind call_kind = instruction->IsClassFinal()
      ? LocationSummary::kNoCall
      : LocationSummary::kCallOnSlowPath;
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(instruction, call_kind);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->SetOut(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM::VisitInstanceOf(HInstanceOf* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).As<Register>();
  Register cls = locations->InAt(1).As<Register>();
  Register out = locations->Out().As<Register>();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();
  Label done, zero;
  SlowPathCodeARM* slow_path = nullptr;

  // Return 0 if `obj` is null.
  // TODO: avoid this check if we know obj is not null.
  __ cmp(obj, ShifterOperand(0));
  __ b(&zero, EQ);
  // Compare the class of `obj` with `cls`.
  __ LoadFromOffset(kLoadWord, out, obj, class_offset);
  __ cmp(out, ShifterOperand(cls));
  if (instruction->IsClassFinal()) {
    // Classes must be equal for the instanceof to succeed.
    __ b(&zero, NE);
    __ LoadImmediate(out, 1);
    __ b(&done);
  } else {
    // If the classes are not equal, we go into a slow path.
    DCHECK(locations->OnlyCallsOnSlowPath());
    slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathARM(
        instruction, locations->InAt(1), locations->Out(), instruction->GetDexPc());
    codegen_->AddSlowPath(slow_path);
    __ b(slow_path->GetEntryLabel(), NE);
    __ LoadImmediate(out, 1);
    __ b(&done);
  }
  __ Bind(&zero);
  __ LoadImmediate(out, 0);
  if (slow_path != nullptr) {
    __ Bind(slow_path->GetExitLabel());
  }
  __ Bind(&done);
}

void LocationsBuilderARM::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = new (GetGraph()->GetArena()) LocationSummary(
      instruction, LocationSummary::kCallOnSlowPath);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  locations->AddTemp(Location::RequiresRegister());
}

void InstructionCodeGeneratorARM::VisitCheckCast(HCheckCast* instruction) {
  LocationSummary* locations = instruction->GetLocations();
  Register obj = locations->InAt(0).As<Register>();
  Register cls = locations->InAt(1).As<Register>();
  Register temp = locations->GetTemp(0).As<Register>();
  uint32_t class_offset = mirror::Object::ClassOffset().Int32Value();

  SlowPathCodeARM* slow_path = new (GetGraph()->GetArena()) TypeCheckSlowPathARM(
      instruction, locations->InAt(1), locations->GetTemp(0), instruction->GetDexPc());
  codegen_->AddSlowPath(slow_path);

  // TODO: avoid this check if we know obj is not null.
  __ cmp(obj, ShifterOperand(0));
  __ b(slow_path->GetExitLabel(), EQ);
  // Compare the class of `obj` with `cls`.
  __ LoadFromOffset(kLoadWord, temp, obj, class_offset);
  __ cmp(temp, ShifterOperand(cls));
  __ b(slow_path->GetEntryLabel(), NE);
  __ Bind(slow_path->GetExitLabel());
}

void LocationsBuilderARM::VisitMonitorOperation(HMonitorOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kCall);
  InvokeRuntimeCallingConvention calling_convention;
  locations->SetInAt(0, Location::RegisterLocation(calling_convention.GetRegisterAt(0)));
}

void InstructionCodeGeneratorARM::VisitMonitorOperation(HMonitorOperation* instruction) {
  codegen_->InvokeRuntime(instruction->IsEnter()
        ? QUICK_ENTRY_POINT(pLockObject) : QUICK_ENTRY_POINT(pUnlockObject),
      instruction,
      instruction->GetDexPc());
}

void LocationsBuilderARM::VisitAnd(HAnd* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderARM::VisitOr(HOr* instruction) { HandleBitwiseOperation(instruction); }
void LocationsBuilderARM::VisitXor(HXor* instruction) { HandleBitwiseOperation(instruction); }

void LocationsBuilderARM::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations =
      new (GetGraph()->GetArena()) LocationSummary(instruction, LocationSummary::kNoCall);
  DCHECK(instruction->GetResultType() == Primitive::kPrimInt
         || instruction->GetResultType() == Primitive::kPrimLong);
  locations->SetInAt(0, Location::RequiresRegister());
  locations->SetInAt(1, Location::RequiresRegister());
  bool output_overlaps = (instruction->GetResultType() == Primitive::kPrimLong);
  locations->SetOut(Location::RequiresRegister(), output_overlaps);
}

void InstructionCodeGeneratorARM::VisitAnd(HAnd* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorARM::VisitOr(HOr* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorARM::VisitXor(HXor* instruction) {
  HandleBitwiseOperation(instruction);
}

void InstructionCodeGeneratorARM::HandleBitwiseOperation(HBinaryOperation* instruction) {
  LocationSummary* locations = instruction->GetLocations();

  if (instruction->GetResultType() == Primitive::kPrimInt) {
    Register first = locations->InAt(0).As<Register>();
    Register second = locations->InAt(1).As<Register>();
    Register out = locations->Out().As<Register>();
    if (instruction->IsAnd()) {
      __ and_(out, first, ShifterOperand(second));
    } else if (instruction->IsOr()) {
      __ orr(out, first, ShifterOperand(second));
    } else {
      DCHECK(instruction->IsXor());
      __ eor(out, first, ShifterOperand(second));
    }
  } else {
    DCHECK_EQ(instruction->GetResultType(), Primitive::kPrimLong);
    Location first = locations->InAt(0);
    Location second = locations->InAt(1);
    Location out = locations->Out();
    if (instruction->IsAnd()) {
      __ and_(out.AsRegisterPairLow<Register>(),
              first.AsRegisterPairLow<Register>(),
              ShifterOperand(second.AsRegisterPairLow<Register>()));
      __ and_(out.AsRegisterPairHigh<Register>(),
              first.AsRegisterPairHigh<Register>(),
              ShifterOperand(second.AsRegisterPairHigh<Register>()));
    } else if (instruction->IsOr()) {
      __ orr(out.AsRegisterPairLow<Register>(),
             first.AsRegisterPairLow<Register>(),
             ShifterOperand(second.AsRegisterPairLow<Register>()));
      __ orr(out.AsRegisterPairHigh<Register>(),
             first.AsRegisterPairHigh<Register>(),
             ShifterOperand(second.AsRegisterPairHigh<Register>()));
    } else {
      DCHECK(instruction->IsXor());
      __ eor(out.AsRegisterPairLow<Register>(),
             first.AsRegisterPairLow<Register>(),
             ShifterOperand(second.AsRegisterPairLow<Register>()));
      __ eor(out.AsRegisterPairHigh<Register>(),
             first.AsRegisterPairHigh<Register>(),
             ShifterOperand(second.AsRegisterPairHigh<Register>()));
    }
  }
}

}  // namespace arm
}  // namespace art
