/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include <regex>

#include "base/arena_allocator.h"
#include "builder.h"
#include "gtest/gtest.h"
#include "induction_var_analysis.h"
#include "nodes.h"
#include "optimizing_unit_test.h"

namespace art {

/**
 * Fixture class for the InductionVarAnalysis tests.
 */
class InductionVarAnalysisTest : public testing::Test {
 public:
  InductionVarAnalysisTest() : pool_(), allocator_(&pool_) {
    graph_ = CreateGraph(&allocator_);
  }

  ~InductionVarAnalysisTest() { }

  // Builds single for-loop at depth d.
  void BuildForLoop(int d, int n) {
    ASSERT_LT(d, n);
    loop_preheader_[d] = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(loop_preheader_[d]);
    loop_header_[d] = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(loop_header_[d]);
    loop_preheader_[d]->AddSuccessor(loop_header_[d]);
    if (d < (n - 1)) {
      BuildForLoop(d + 1, n);
    }
    loop_body_[d] = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(loop_body_[d]);
    loop_body_[d]->AddSuccessor(loop_header_[d]);
    if (d < (n - 1)) {
      loop_header_[d]->AddSuccessor(loop_preheader_[d + 1]);
      loop_header_[d + 1]->AddSuccessor(loop_body_[d]);
    } else {
      loop_header_[d]->AddSuccessor(loop_body_[d]);
    }
  }

  // Builds a n-nested loop in CFG where each loop at depth 0 <= d < n
  // is defined as "for (int i_d = 0; i_d < 100; i_d++)". Tests can further
  // populate the loop with instructions to set up interesting scenarios.
  void BuildLoopNest(int n) {
    ASSERT_LE(n, 10);
    graph_->SetNumberOfVRegs(n + 1);

    // Build basic blocks with entry, nested loop, exit.
    entry_ = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(entry_);
    BuildForLoop(0, n);
    exit_ = new (&allocator_) HBasicBlock(graph_);
    graph_->AddBlock(exit_);
    entry_->AddSuccessor(loop_preheader_[0]);
    loop_header_[0]->AddSuccessor(exit_);
    graph_->SetEntryBlock(entry_);
    graph_->SetExitBlock(exit_);

    // Provide entry and exit instructions.
    // 0 : parameter
    // 1 : constant 0
    // 2 : constant 1
    // 3 : constant 100
    parameter_ = new (&allocator_)
        HParameterValue(0, Primitive::kPrimNot, true);
    entry_->AddInstruction(parameter_);
    constant0_ = new (&allocator_) HConstant(Primitive::kPrimInt);
    entry_->AddInstruction(constant0_);
    constant1_ = new (&allocator_) HConstant(Primitive::kPrimInt);
    entry_->AddInstruction(constant1_);
    constant100_ = new (&allocator_) HConstant(Primitive::kPrimInt);
    entry_->AddInstruction(constant100_);
    exit_->AddInstruction(new (&allocator_) HExit());
    induc_ = new (&allocator_) HLocal(n);
    entry_->AddInstruction(induc_);
    entry_->AddInstruction(new (&allocator_) HStoreLocal(induc_, constant0_));

    // Provide loop instructions.
    for (int d = 0; d < n; d++) {
      basic_[d] = new (&allocator_) HLocal(d);
      entry_->AddInstruction(basic_[d]);
      loop_preheader_[d]->AddInstruction(
           new (&allocator_) HStoreLocal(basic_[d], constant0_));
      HInstruction* load = new (&allocator_)
          HLoadLocal(basic_[d], Primitive::kPrimInt);
      loop_header_[d]->AddInstruction(load);
      HInstruction* compare = new (&allocator_)
          HGreaterThanOrEqual(load, constant100_);
      loop_header_[d]->AddInstruction(compare);
      loop_header_[d]->AddInstruction(new (&allocator_) HIf(compare));
      load = new (&allocator_) HLoadLocal(basic_[d], Primitive::kPrimInt);
      loop_body_[d]->AddInstruction(load);
      increment_[d] = new (&allocator_)
          HAdd(Primitive::kPrimInt, load, constant1_);
      loop_body_[d]->AddInstruction(increment_[d]);
      loop_body_[d]->AddInstruction(
               new (&allocator_) HStoreLocal(basic_[d], increment_[d]));
      loop_body_[d]->AddInstruction(new (&allocator_) HGoto());
    }
  }

  // Inserts instruction right before increment at depth d.
  HInstruction* InsertInstruction(HInstruction* instruction, int d) {
    loop_body_[d]->InsertInstructionBefore(instruction, increment_[d]);
    return instruction;
  }

  // Inserts local load at depth d.
  HInstruction* InsertLocalLoad(HLocal* local, int d) {
    return InsertInstruction(
        new (&allocator_) HLoadLocal(local, Primitive::kPrimInt), d);
  }

  // Inserts local store at depth d.
  HInstruction* InsertLocalStore(HLocal* local, HInstruction* rhs, int d) {
    return InsertInstruction(new (&allocator_) HStoreLocal(local, rhs), d);
  }

  // Inserts an array store with given local as subscript at depth d to
  // enable tests to inspect the computed induction at that point easily.
  HInstruction* InsertArrayStore(HLocal* subscript, int d) {
    HInstruction* load = InsertInstruction(
        new (&allocator_) HLoadLocal(subscript, Primitive::kPrimInt), d);
    return InsertInstruction(new (&allocator_) HArraySet(
        parameter_, load, constant0_, Primitive::kPrimInt, 0), d);
  }

  // Returns loop information of loop at depth d.
  HLoopInformation* GetLoopInfo(int d) {
    return loop_body_[d]->GetLoopInformation();
  }

  // Performs InductionVarAnalysis (after proper set up).
  void PerformInductionVarAnalysis() {
    ASSERT_TRUE(graph_->TryBuildingSsa());
    iva_ = new (&allocator_) HInductionVarAnalysis(graph_);
    iva_->Run();
  }

  // General building fields.
  ArenaPool pool_;
  ArenaAllocator allocator_;
  HGraph* graph_;
  HInductionVarAnalysis* iva_;

  // Fixed basic blocks and instructions.
  HBasicBlock* entry_;
  HBasicBlock* exit_;
  HInstruction* parameter_;  // "this"
  HInstruction* constant0_;
  HInstruction* constant1_;
  HInstruction* constant100_;
  HLocal* induc_;  // "vreg_n", the "k"

  // Loop specifics.
  HBasicBlock* loop_preheader_[10];
  HBasicBlock* loop_header_[10];
  HBasicBlock* loop_body_[10];
  HInstruction* increment_[10];
  HLocal* basic_[10];  // "vreg_d", the "i_d"
};

//
// The actual InductionVarAnalysis tests.
//

TEST_F(InductionVarAnalysisTest, ProperLoopSetup) {
  // Setup:
  // for (int i_0 = 0; i_0 < 100; i_0++) {
  //   ..
  //     for (int i_9 = 0; i_9 < 100; i_9++) {
  //     }
  //   ..
  // }
  BuildLoopNest(10);
  ASSERT_TRUE(graph_->TryBuildingSsa());
  ASSERT_EQ(entry_->GetLoopInformation(), nullptr);
  for (int d = 0; d < 1; d++) {
    ASSERT_EQ(loop_preheader_[d]->GetLoopInformation(),
              (d == 0) ? nullptr
                       : loop_header_[d - 1]->GetLoopInformation());
    ASSERT_NE(loop_header_[d]->GetLoopInformation(), nullptr);
    ASSERT_NE(loop_body_[d]->GetLoopInformation(), nullptr);
    ASSERT_EQ(loop_header_[d]->GetLoopInformation(),
              loop_body_[d]->GetLoopInformation());
  }
  ASSERT_EQ(exit_->GetLoopInformation(), nullptr);
}

TEST_F(InductionVarAnalysisTest, FindBasicInductionVar) {
  // Setup:
  // for (int i = 0; i < 100; i++) {
  //    a[i] = 0;
  // }
  BuildLoopNest(1);
  HInstruction* store = InsertArrayStore(basic_[0], 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ(
      "((2:Constant) * i + (1:Constant))",
      iva_->InductionToString(GetLoopInfo(0), store->InputAt(1)).c_str());
  EXPECT_STREQ(
      "((2:Constant) * i + ((1:Constant) + (2:Constant)))",
      iva_->InductionToString(GetLoopInfo(0), increment_[0]).c_str());
}

TEST_F(InductionVarAnalysisTest, FindDerivedInductionVarAdd) {
  // Setup:
  // for (int i = 0; i < 100; i++) {
  //    k = 100 + i;
  //    a[k] = 0;
  // }
  BuildLoopNest(1);
  HInstruction *add = InsertInstruction(
      new (&allocator_) HAdd(
          Primitive::kPrimInt, constant100_, InsertLocalLoad(basic_[0], 0)), 0);
  InsertLocalStore(induc_, add, 0);
  HInstruction* store = InsertArrayStore(induc_, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ(
      "((2:Constant) * i + ((3:Constant) + (1:Constant)))",
      iva_->InductionToString(GetLoopInfo(0), store->InputAt(1)).c_str());
}

TEST_F(InductionVarAnalysisTest, FindDerivedInductionVarSub) {
  // Setup:
  // for (int i = 0; i < 100; i++) {
  //    k = 100 - i;
  //    a[k] = 0;
  // }
  BuildLoopNest(1);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(
          Primitive::kPrimInt, constant100_, InsertLocalLoad(basic_[0], 0)), 0);
  InsertLocalStore(induc_, sub, 0);
  HInstruction* store = InsertArrayStore(induc_, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ(
      "(( - (2:Constant)) * i + ((3:Constant) - (1:Constant)))",
      iva_->InductionToString(GetLoopInfo(0), store->InputAt(1)).c_str());
}

TEST_F(InductionVarAnalysisTest, FindDerivedInductionVarMul) {
  // Setup:
  // for (int i = 0; i < 100; i++) {
  //    k = 100 * i;
  //    a[k] = 0;
  // }
  BuildLoopNest(1);
  HInstruction *mul = InsertInstruction(
      new (&allocator_) HMul(
          Primitive::kPrimInt, constant100_, InsertLocalLoad(basic_[0], 0)), 0);
  InsertLocalStore(induc_, mul, 0);
  HInstruction* store = InsertArrayStore(induc_, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ(
      "(((3:Constant) * (2:Constant)) * i + ((3:Constant) * (1:Constant)))",
      iva_->InductionToString(GetLoopInfo(0), store->InputAt(1)).c_str());
}

TEST_F(InductionVarAnalysisTest, FindDerivedInductionVarNeg) {
  // Setup:
  // for (int i = 0; i < 100; i++) {
  //    k = - i;
  //    a[k] = 0;
  // }
  BuildLoopNest(1);
  HInstruction *neg = InsertInstruction(
      new (&allocator_) HNeg(
          Primitive::kPrimInt, InsertLocalLoad(basic_[0], 0)), 0);
  InsertLocalStore(induc_, neg, 0);
  HInstruction* store = InsertArrayStore(induc_, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ(
      "(( - (2:Constant)) * i + ( - (1:Constant)))",
      iva_->InductionToString(GetLoopInfo(0), store->InputAt(1)).c_str());
}

TEST_F(InductionVarAnalysisTest, FindChainInduction) {
  // Setup:
  // k = 0;
  // for (int i = 0; i < 100; i++) {
  //    k = k + 100;
  //    a[k] = 0;
  //    k = k - 1;
  //    a[k] = 0;
  // }
  BuildLoopNest(1);
  HInstruction *add = InsertInstruction(
      new (&allocator_) HAdd(
          Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant100_), 0);
  InsertLocalStore(induc_, add, 0);
  HInstruction* store1 = InsertArrayStore(induc_, 0);
  HInstruction *sub = InsertInstruction(
      new (&allocator_) HSub(
          Primitive::kPrimInt, InsertLocalLoad(induc_, 0), constant1_), 0);
  InsertLocalStore(induc_, sub, 0);
  HInstruction* store2 = InsertArrayStore(induc_, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ(
      "(((3:Constant) - (2:Constant)) * i + ((1:Constant) + (3:Constant)))",
      iva_->InductionToString(GetLoopInfo(0), store1->InputAt(1)).c_str());
  EXPECT_STREQ(
      "(((3:Constant) - (2:Constant)) * i + "
      "(((1:Constant) + (3:Constant)) - (2:Constant)))",
      iva_->InductionToString(GetLoopInfo(0), store2->InputAt(1)).c_str());
}

TEST_F(InductionVarAnalysisTest, FindTwoWayDerivedInduction) {
  // Setup:
  // for (int i = 0; i < 100; i++) {
  //    if () k = i + 1;
  //    else  k = i + 1;
  //    a[k] = 0;
  // }
  BuildLoopNest(1);
  HBasicBlock* cond = new (&allocator_) HBasicBlock(graph_);
  HBasicBlock* ifTrue = new (&allocator_) HBasicBlock(graph_);
  HBasicBlock* ifFalse = new (&allocator_) HBasicBlock(graph_);
  graph_->AddBlock(cond);
  graph_->AddBlock(ifTrue);
  graph_->AddBlock(ifFalse);
  // Conditional split.
  loop_header_[0]->ReplaceSuccessor(loop_body_[0], cond);
  cond->AddSuccessor(ifTrue);
  cond->AddSuccessor(ifFalse);
  ifTrue->AddSuccessor(loop_body_[0]);
  ifFalse->AddSuccessor(loop_body_[0]);
  cond->AddInstruction(new (&allocator_) HIf(parameter_));
  // True-branch.
  HInstruction* load1 = new (&allocator_)
      HLoadLocal(basic_[0], Primitive::kPrimInt);
  ifTrue->AddInstruction(load1);
  HInstruction* inc1 = new (&allocator_)
      HAdd(Primitive::kPrimInt, load1, constant1_);
  ifTrue->AddInstruction(inc1);
  ifTrue->AddInstruction(new (&allocator_) HStoreLocal(induc_, inc1));
  ifTrue->AddInstruction(new (&allocator_) HGoto());
  // False-branch.
  HInstruction* load2 = new (&allocator_)
      HLoadLocal(basic_[0], Primitive::kPrimInt);
  ifFalse->AddInstruction(load2);
  HInstruction* inc2 = new (&allocator_)
        HAdd(Primitive::kPrimInt, load2, constant1_);
  ifFalse->AddInstruction(inc2);
  ifFalse->AddInstruction(new (&allocator_) HStoreLocal(induc_, inc2));
  ifFalse->AddInstruction(new (&allocator_) HGoto());
  // Merge over a phi.
  HInstruction* store = InsertArrayStore(induc_, 0);
  PerformInductionVarAnalysis();

  EXPECT_STREQ(
      "((2:Constant) * i + ((1:Constant) + (2:Constant)))",
      iva_->InductionToString(GetLoopInfo(0), store->InputAt(1)).c_str());
}

TEST_F(InductionVarAnalysisTest, FindDeepLoopInduction) {
  // Setup:
  // k = 0;
  // for (int i_0 = 0; i_0 < 100; i_0++) {
  //   ..
  //     for (int i_9 = 0; i_9 < 100; i_9++) {
  //       k++;
  //       a[k] = 0;
  //     }
  //   ..
  // }
  BuildLoopNest(10);
  HInstruction *inc = InsertInstruction(
      new (&allocator_) HAdd(
          Primitive::kPrimInt, constant1_, InsertLocalLoad(induc_, 9)), 9);
  InsertLocalStore(induc_, inc, 9);
  HInstruction* store = InsertArrayStore(induc_, 9);
  PerformInductionVarAnalysis();

  // Match exact number of constants, but be less picky on phi number,
  // since that depends on the ssa building phase.
  std::regex r("\\(\\(2\\:Constant\\) \\* i \\+ "
               "\\(\\(2\\:Constant\\) \\+ \\(\\d+\\:Phi\\)\\)\\)");

  for (int d = 0; d < 10; d++) {
    if (d == 9) {
      EXPECT_TRUE(std::regex_match(
          iva_->InductionToString(GetLoopInfo(d), store->InputAt(1)), r));
    } else {
      EXPECT_STREQ(
          "",
          iva_->InductionToString(GetLoopInfo(d), store->InputAt(1)).c_str());
    }
    EXPECT_STREQ(
        "((2:Constant) * i + ((1:Constant) + (2:Constant)))",
        iva_->InductionToString(GetLoopInfo(d), increment_[d]).c_str());
  }
}

}  // namespace art
