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
#include "dex_file.h"
#include "dex_instruction.h"
#include "constant_propagation.h"
#include "graph_checker.h"
#include "optimizing_unit_test.h"

#include "gtest/gtest.h"

namespace art {

// Create a control-flow graph from Dex bytes.
HGraph* CreateCFG(ArenaAllocator* allocator, const uint16_t* data) {
  HGraphBuilder builder(allocator);
  const DexFile::CodeItem* item =
    reinterpret_cast<const DexFile::CodeItem*>(data);
  HGraph* graph = builder.BuildGraph(*item);
  return graph;
}


static void TestCode(const uint16_t* data) {
  ArenaPool pool;
  ArenaAllocator allocator(&pool);
  HGraph* graph = CreateCFG(&allocator, data);
  ASSERT_NE(graph, nullptr);

  graph->BuildDominatorTree();
  graph->TransformToSSA();

  ConstantPropagation(graph).Run();

  SSAChecker ssa_checker(&allocator, graph);
  ssa_checker.VisitInsertionOrder();
  ASSERT_TRUE(ssa_checker.IsValid());
}


/* Tiny three-register program exercising constant folding on addition.

                                16-bit
                                offset
                                ------
       v0 <- 1                  0.      const/4 v0, #+1
       v1 <- 2                  1.      const/4 v1, #+2
       v2 <- v0 + v1            2.      add-int v2, v0, v1
       return v2                4.      return v2
*/
TEST(ConstantPropagation, ConstantFoldingOnAddition1) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 << 8 | 1 << 12,
    Instruction::CONST_4 | 1 << 8 | 2 << 12,
    Instruction::ADD_INT | 2 << 8, 0 | 1 << 8,
    Instruction::RETURN | 2 << 8);

  TestCode(data);
}

/* Small three-register program exercising constant folding on addition.

                                16-bit
                                offset
                                ------
       v0 <- 1                  0.      const/4 v0, #+1
       v1 <- 2                  1.      const/4 v1, #+2
       v0 <- v0 + v1            2.      add-int/2addr v0, v1
       v1 <- 3                  3.      const/4 v1, #+3
       v2 <- 4                  4.      const/4 v2, #+4
       v1 <- v1 + v2            5.      add-int/2addr v1, v2
       v2 <- v0 + v1            6.      add-int v2, v0, v1
       return v2                8.      return v2
*/
TEST(ConstantPropagation, ConstantFoldingOnAddition2) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 << 8 | 1 << 12,
    Instruction::CONST_4 | 1 << 8 | 2 << 12,
    Instruction::ADD_INT_2ADDR | 0 << 8 | 1 << 12,
    Instruction::CONST_4 | 1 << 8 | 3 << 12,
    Instruction::CONST_4 | 2 << 8 | 4 << 12,
    Instruction::ADD_INT_2ADDR | 1 << 8 | 2 << 12,
    Instruction::ADD_INT | 2 << 8, 0 | 1 << 8,
    Instruction::RETURN | 2 << 8);

  TestCode(data);
}

/* Tiny three-register program exercising constant folding on subtraction.

                                16-bit
                                offset
                                ------
       v0 <- 3                  0.      const/4 v0, #+3
       v1 <- 2                  1.      const/4 v1, #+2
       v2 <- v0 - v1            2.      sub-int v2, v0, v1
       return v2                4.      return v2
*/
TEST(ConstantPropagation, ConstantFoldingOnSubtraction) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 0 << 8 | 3 << 12,
    Instruction::CONST_4 | 1 << 8 | 2 << 12,
    Instruction::SUB_INT | 2 << 8, 0 | 1 << 8,
    Instruction::RETURN | 2 << 8);

  TestCode(data);
}

/* Three-register program with a constant (static) condition.

                                16-bit
                                offset
                                ------
       v1 <- 1                  0.      const/4 v1, #+1
       v0 <- 0                  1.      const/4 v0, #+0
       if v1 >= 0 goto L1       2.      if-gez v1, +3
       v0 <- v1                 4.      move v0, v1
   L1: v2 <- v0 + v1            5.      add-int v2, v0, v1
       return-void              7.      return
*/
TEST(ConstantPropagation, ConstantCondition) {
  const uint16_t data[] = THREE_REGISTERS_CODE_ITEM(
    Instruction::CONST_4 | 1 << 8 | 1 << 12,
    Instruction::CONST_4 | 0 << 8 | 0 << 12,
    Instruction::IF_GEZ | 1 << 8, 3,
    Instruction::MOVE | 0 << 8 | 1 << 12,
    Instruction::ADD_INT | 2 << 8, 0 | 1 << 8,
    Instruction::RETURN_VOID);

  TestCode(data);
}

}  // namespace art
