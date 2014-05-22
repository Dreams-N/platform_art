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

#include "builder.h"
#include "code_generator.h"
#include "dex_file.h"
#include "dex_instruction.h"
#include "nodes.h"
#include "optimizing_unit_test.h"
#include "register_allocator.h"
#include "ssa_liveness_analysis.h"
#include "utils/arena_allocator.h"

#include "gtest/gtest.h"

namespace art {

// Note: the register allocator tests rely on the fact that constants have live
// intervals and registers get allocated to them.

static bool Check(const uint16_t* data) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraphBuilder builder(&allocator);
  const DexFile::CodeItem* item = reinterpret_cast<const DexFile::CodeItem*>(data);
  HGraph* graph = builder.BuildGraph(*item);
  graph->BuildDominatorTree();
  graph->TransformToSSA();
  graph->FindNaturalLoops();
  SsaLivenessAnalysis liveness(*graph);
  liveness.Analyze();
  CodeGenerator* codegen = CodeGenerator::Create(&allocator, graph, kX86);
  RegisterAllocator register_allocator(&allocator, *codegen);
  register_allocator.AllocateRegisters(liveness);
  return register_allocator.Validate(liveness, false);
}

/**
 * Unit testing of RegisterAllocator::ValidateIntervals. Register allocator
 * tests are based on this validation method.
 */
TEST(RegisterAllocatorTest, ValidateIntervals) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = new (&allocator) HGraph(&allocator);
  CodeGenerator* codegen = CodeGenerator::Create(&allocator, graph, kX86);
  GrowableArray<LiveInterval*> intervals(&allocator, 0);

  // Test with two intervals of the same range.
  {
    static constexpr size_t ranges[][2] = {{0, 42}};
    intervals.Add(BuildInterval(ranges, arraysize(ranges), &allocator, 0));
    intervals.Add(BuildInterval(ranges, arraysize(ranges), &allocator, 1));
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(intervals, *codegen, &allocator, true, false));

    intervals.Get(1)->SetRegister(0);
    ASSERT_FALSE(RegisterAllocator::ValidateIntervals(intervals, *codegen, &allocator, true, false));
    intervals.Reset();
  }

  // Test with two non-intersecting intervals.
  {
    static constexpr size_t ranges1[][2] = {{0, 42}};
    intervals.Add(BuildInterval(ranges1, arraysize(ranges1), &allocator, 0));
    static constexpr size_t ranges2[][2] = {{42, 43}};
    intervals.Add(BuildInterval(ranges2, arraysize(ranges2), &allocator, 1));
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(intervals, *codegen, &allocator, true, false));

    intervals.Get(1)->SetRegister(0);
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(intervals, *codegen, &allocator, true, false));
    intervals.Reset();
  }

  // Test with two non-intersecting intervals, with one with a lifetime hole.
  {
    static constexpr size_t ranges1[][2] = {{0, 42}, {45, 48}};
    intervals.Add(BuildInterval(ranges1, arraysize(ranges1), &allocator, 0));
    static constexpr size_t ranges2[][2] = {{42, 43}};
    intervals.Add(BuildInterval(ranges2, arraysize(ranges2), &allocator, 1));
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(intervals, *codegen, &allocator, true, false));

    intervals.Get(1)->SetRegister(0);
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(intervals, *codegen, &allocator, true, false));
    intervals.Reset();
  }

  // Test with siblings.
  {
    static constexpr size_t ranges1[][2] = {{0, 42}, {44, 48}};
    intervals.Add(BuildInterval(ranges1, arraysize(ranges1), &allocator, 0));
    intervals.Get(0)->SplitAt(43);
    static constexpr size_t ranges2[][2] = {{42, 47}};
    intervals.Add(BuildInterval(ranges2, arraysize(ranges2), &allocator, 1));
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(intervals, *codegen, &allocator, true, false));

    intervals.Get(1)->SetRegister(0);
    // Sibling of the first interval has no register allocated to it.
    ASSERT_TRUE(RegisterAllocator::ValidateIntervals(intervals, *codegen, &allocator, true, false));

    intervals.Get(0)->GetNextSibling()->SetRegister(0);
    ASSERT_FALSE(RegisterAllocator::ValidateIntervals(intervals, *codegen, &allocator, true, false));
  }
}

TEST(RegisterAllocatorTest, ValidateIntervals4) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = new (&allocator) HGraph(&allocator);
  CodeGenerator* codegen = CodeGenerator::Create(&allocator, graph, kX86);
  GrowableArray<LiveInterval*> intervals(&allocator, 0);

  static constexpr size_t ranges1[][2] = {{0, 42}, {44, 48}};
  intervals.Add(BuildInterval(ranges1, arraysize(ranges1), &allocator, 0));
  static constexpr size_t ranges2[][2] = {{42, 47}};
  intervals.Add(BuildInterval(ranges2, arraysize(ranges2), &allocator, 1));
  ASSERT_TRUE(RegisterAllocator::ValidateIntervals(intervals, *codegen, &allocator, true, false));

  intervals.Get(1)->SetRegister(0);
  ASSERT_FALSE(RegisterAllocator::ValidateIntervals(intervals, *codegen, &allocator, true, false));
}

TEST(RegisterAllocatorTest, CFG1) {
  /*
   * Test the following snippet:
   *  return 0;
   *
   * Which becomes the following graph:
   *       constant0
   *       goto
   *        |
   *       return
   *        |
   *       exit
   */
  const uint16_t data[] = ONE_REGISTER_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::RETURN);

  ASSERT_TRUE(Check(data));
}

TEST(RegisterAllocatorTest, Loop1) {
  /*
   * Test the following snippet:
   *  var a = 0;
   *  while (a == a) {
   *    a = 4;
   *  }
   *  return 5;
   *
   * Which becomes the following graph:
   *       constant0
   *       constant4
   *       constant5
   *       goto
   *        |
   *       goto
   *        |
   *       phi
   *       equal
   *       if +++++
   *        |       \ +
   *        |     goto
   *        |
   *       return
   *        |
   *       exit
   */

  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::IF_EQ, 4,
    Instruction::CONST_4 | 4 << 12 | 0,
    Instruction::GOTO | 0xFD00,
    Instruction::CONST_4 | 5 << 12 | 1 << 8,
    Instruction::RETURN | 1 << 8);

  ASSERT_TRUE(Check(data));
}

TEST(RegisterAllocatorTest, Loop2) {
  /*
   * Test the following snippet:
   *  var a = 0;
   *  while (a == 8) {
   *    a = 4 + 5;
   *  }
   *  return 6 + 7;
   *
   * Which becomes the following graph:
   *       constant0
   *       constant4
   *       constant5
   *       constant6
   *       constant7
   *       constant8
   *       goto
   *        |
   *       goto
   *        |
   *       phi
   *       equal
   *       if +++++
   *        |       \ +
   *        |      4 + 5   
   *        |      goto
   *        |
   *       6 + 7
   *       return
   *        |
   *       exit
   */

  const uint16_t data[] = TWO_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 | 0,
    Instruction::CONST_4 | 8 << 12 | 1 << 8,
    Instruction::IF_EQ | 1 << 8, 7,
    Instruction::CONST_4 | 4 << 12 | 0 << 8,
    Instruction::CONST_4 | 5 << 12 | 1 << 8,
    Instruction::ADD_INT, 1 << 8 | 0,
    Instruction::GOTO | 0xFA00,
    Instruction::CONST_4 | 6 << 12 | 1 << 8,
    Instruction::CONST_4 | 7 << 12 | 1 << 8,
    Instruction::ADD_INT, 1 << 8 | 0,
    Instruction::RETURN | 1 << 8);

  ASSERT_TRUE(Check(data));
}

}  // namespace art
