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

#include "base/macros.h"
#include "builder.h"
#include "code_generator_arm.h"
#include "code_generator_x86.h"
#include "code_generator_x86_64.h"
#include "common_compiler_test.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "instruction_set.h"
#include "nodes.h"
#include "optimizing_unit_test.h"

#include "gtest/gtest.h"

namespace art {

class InternalCodeAllocator : public CodeAllocator {
 public:
  InternalCodeAllocator() { }

  virtual uint8_t* Allocate(size_t size) {
    size_ = size;
    memory_.reset(new uint8_t[size]);
    return memory_.get();
  }

  size_t GetSize() const { return size_; }
  uint8_t* GetMemory() const { return memory_.get(); }

 private:
  size_t size_;
  std::unique_ptr<uint8_t[]> memory_;

  DISALLOW_COPY_AND_ASSIGN(InternalCodeAllocator);
};

static void Run(const InternalCodeAllocator& allocator,
                const CodeGenerator& codegen,
                bool has_result,
                int32_t expected) {
  typedef int32_t (*fptr)();
  CommonCompilerTest::MakeExecutable(allocator.GetMemory(), allocator.GetSize());
  fptr f = reinterpret_cast<fptr>(allocator.GetMemory());
  if (codegen.GetInstructionSet() == kThumb2) {
    // For thumb we need the bottom bit set.
    f = reinterpret_cast<fptr>(reinterpret_cast<uintptr_t>(f) + 1);
  }
  int32_t result = f();
  if (has_result) {
    CHECK_EQ(result, expected);
  }
}

static void TestGraph(HGraph* graph, bool has_result = false, int32_t expected = 0) {
  // Remove suspend checks, they cannot be executed in this context.
  RemoveSuspendChecks(graph);
  ASSERT_NE(graph, nullptr);
  InternalCodeAllocator allocator;

  x86::CodeGeneratorX86 codegenX86(graph);
  // We avoid doing a stack overflow check that requires the runtime being setup,
  // by making sure the compiler knows the methods we are running are leaf methods.
  codegenX86.CompileBaseline(&allocator, true);
  if (kRuntimeISA == kX86) {
    Run(allocator, codegenX86, has_result, expected);
  }

  arm::CodeGeneratorARM codegenARM(graph);
  codegenARM.CompileBaseline(&allocator, true);
  if (kRuntimeISA == kArm || kRuntimeISA == kThumb2) {
    Run(allocator, codegenARM, has_result, expected);
  }

  x86_64::CodeGeneratorX86_64 codegenX86_64(graph);
  codegenX86_64.CompileBaseline(&allocator, true);
  if (kRuntimeISA == kX86_64) {
    Run(allocator, codegenX86_64, has_result, expected);
  }
}

static void TestCode(const uint16_t* data, bool has_result = false, int32_t expected = 0) {
  ArenaPool pool;
  ArenaAllocator arena(&pool);
  HGraphBuilder builder(&arena);
  const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
  HGraph* graph = builder.BuildGraph(*item);

  TestGraph(graph, has_result, expected);
}

TEST(CodegenTest, ReturnVoid) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(Instruction::RETURN_VOID);
  TestCode(data);
}

TEST(CodegenTest, CFG1) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, CFG2) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, CFG3) {
  const uint16_t data1[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO | 0x200,
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0xFF00);

  TestCode(data1);

  const uint16_t data2[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO_16, 3,
    Instruction::RETURN_VOID,
    Instruction::GOTO_16, 0xFFFF);

  TestCode(data2);

  const uint16_t data3[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::GOTO_32, 4, 0,
    Instruction::RETURN_VOID,
    Instruction::GOTO_32, 0xFFFF, 0xFFFF);

  TestCode(data3);
}

TEST(CodegenTest, CFG4) {
  const uint16_t data[] = ZERO_REGISTER_CODE_ITEM(
    Instruction::RETURN_VOID,
    Instruction::GOTO | 0x100,
    Instruction::GOTO | 0xFE00);

  TestCode(data);
}

TEST(CodegenTest, CFG5) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 3,
    Instruction::GOTO | 0x100,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, IntConstant) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN_VOID);

  TestCode(data);
}

TEST(CodegenTest, Return1) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN | 0);

  TestCode(data, true, 0);
}

TEST(CodegenTest, Return2) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 0 | 1 << 8,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 0);
}

TEST(CodegenTest, Return3) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 1);
}

TEST(CodegenTest, ReturnIf1) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::IF_EQ, 3,
    Instruction::RETURN | 0 << 8,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 1);
}

TEST(CodegenTest, ReturnIf2) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::IF_EQ | 0 << 4 | 1 << 8, 3,
    Instruction::RETURN | 0 << 8,
    Instruction::RETURN | 1 << 8);

  TestCode(data, true, 0);
}

TEST(CodegenTest, ReturnAdd1) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 3 << 12 | 0,
    Instruction::CONST_4 | 4 << 12 | 1 << 8,
    Instruction::ADD_INT, 1 << 8 | 0,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

TEST(CodegenTest, ReturnAdd2) {
  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 3 << 12 | 0,
    Instruction::CONST_4 | 4 << 12 | 1 << 8,
    Instruction::ADD_INT_2ADDR | 1 << 12,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

TEST(CodegenTest, ReturnAdd3) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 4 << 12 | 0 << 8,
    Instruction::ADD_INT_LIT8, 3 << 8 | 0,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

TEST(CodegenTest, ReturnAdd4) {
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 4 << 12 | 0 << 8,
    Instruction::ADD_INT_LIT16, 3,
    Instruction::RETURN);

  TestCode(data, true, 7);
}

static HBasicBlock* CreateEntryBlock(HGraph* graph, ArenaAllocator* allocator) {
  HBasicBlock* block = new (allocator) HBasicBlock(graph);
  graph->AddBlock(block);
  block->AddInstruction(new (allocator) HGoto());
  return block;
}

static HBasicBlock* CreateBlock(HGraph* graph, ArenaAllocator* arena) {
  HBasicBlock* block = new (arena) HBasicBlock(graph);
  graph->AddBlock(block);
  return block;
}

static HBasicBlock* CreateExitBlock(HGraph* graph, ArenaAllocator* allocator) {
  HBasicBlock* block = new (allocator) HBasicBlock(graph);
  graph->AddBlock(block);
  block->AddInstruction(new (allocator) HExit());
  return block;
}


TEST(CodegenTest, MaterializedCond1) {
  // Check that condition are materialized correctly. A materialized condition
  // should yield `1` if it evaluated to true, and `0` otherwise.
  // We force the materialization of comparisons for different combinations of
  // inputs and check the results.

  int less = 1;
  int greater_equal = 0;

  int lhs[] = {-1, 2, 0xabc};
  int rhs[] = {2, 1, 0xabc};
  int expected_res[] = {less, greater_equal, greater_equal};

  for (size_t i = 0; i < arraysize(expected_res); i++) {
    ArenaPool pool;
    ArenaAllocator arena(&pool);
    HGraph* graph = new (&arena) HGraph(&arena);

    HBasicBlock* entry_block = CreateEntryBlock(graph, &arena);
    HBasicBlock* cond_block = CreateBlock(graph, &arena);
    HBasicBlock* exit_block = CreateExitBlock(graph, &arena);

    graph->SetEntryBlock(entry_block);
    entry_block->AddSuccessor(cond_block);
    cond_block->AddSuccessor(exit_block);
    graph->SetExitBlock(exit_block);

    HIntConstant cst_lhs(lhs[i]);
    HIntConstant cst_rhs(rhs[i]);
    HLessThan cmp_lt(&cst_lhs, &cst_rhs);
    cmp_lt.SetForceMaterialization(true);
    HReturn ret(&cmp_lt);

    cond_block->AddInstruction(&cst_lhs);
    cond_block->AddInstruction(&cst_rhs);
    cond_block->AddInstruction(&cmp_lt);
    cond_block->AddInstruction(&ret);

    TestGraph(graph, true, expected_res[i]);
  }
}

TEST(CodegenTest, MaterializedCond2) {
  // Check that HIf correctly interprets a materialized condition.
  // We force the materialization of comparisons for different combinations of
  // inputs. An HIf takes the materialized combination as input and returns a
  // value that we verify.

  int less = 314;
  int greater_equal = 2718;

  int lhs[] = {-1, 2, 0xabc};
  int rhs[] = {2, 1, 0xabc};
  int expected_res[] = {less, greater_equal, greater_equal};

  for (size_t i = 0; i < arraysize(expected_res); i++) {
    ArenaPool pool;
    ArenaAllocator arena(&pool);
    HGraph* graph = new (&arena) HGraph(&arena);

    HBasicBlock* entry_block = CreateEntryBlock(graph, &arena);
    HBasicBlock* if_block = CreateBlock(graph, &arena);
    HBasicBlock* if_true_block = CreateBlock(graph, &arena);
    HBasicBlock* if_false_block = CreateBlock(graph, &arena);
    HBasicBlock* exit_block = CreateExitBlock(graph, &arena);

    graph->SetEntryBlock(entry_block);
    entry_block->AddSuccessor(if_block);
    if_block->AddSuccessor(if_true_block);
    if_block->AddSuccessor(if_false_block);
    if_true_block->AddSuccessor(exit_block);
    if_false_block->AddSuccessor(exit_block);
    graph->SetExitBlock(exit_block);

    HIntConstant cst_lhs(lhs[i]);
    if_block->AddInstruction(&cst_lhs);
    HIntConstant cst_rhs(rhs[i]);
    if_block->AddInstruction(&cst_rhs);
    HLessThan cmp_lt(&cst_lhs, &cst_rhs);
    cmp_lt.SetForceMaterialization(true);
    if_block->AddInstruction(&cmp_lt);
    HIf if_lt(&cmp_lt);
    if_block->AddInstruction(&if_lt);

    HIntConstant cst_ls(less);
    if_true_block->AddInstruction(&cst_ls);
    HReturn ret_ls(&cst_ls);
    if_true_block->AddInstruction(&ret_ls);
    HIntConstant cst_ge(greater_equal);
    if_false_block->AddInstruction(&cst_ge);
    HReturn ret_ge(&cst_ge);
    if_false_block->AddInstruction(&ret_ge);

    TestGraph(graph, true, expected_res[i]);
  }
}

}  // namespace art
