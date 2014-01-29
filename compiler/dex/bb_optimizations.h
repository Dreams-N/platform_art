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

#ifndef ART_COMPILER_DEX_BB_OPTIMIZATIONS_H_
#define ART_COMPILER_DEX_BB_OPTIMIZATIONS_H_

#include "compiler_internals.h"
#include "pass.h"

namespace art {

/**
 * @class AnnotateUsedFields
 * @brief Perform the annotation of fields for IGET/IPUT/SGET/SPUT insns.
 */
class AnnotateUsedFields : public Pass {
 public:
  AnnotateUsedFields() : Pass("AnnotateInstanceFields", kNoNodes) {
  }

  void Start(CompilationUnit* cUnit) const {
    cUnit->mir_graph->DoAnnotateUsedFields();
  }
};

/**
 * @class CodeLayout
 * @brief Perform the code layout pass.
 */
class CodeLayout : public Pass {
 public:
  CodeLayout() : Pass("CodeLayout", "2_post_layout_cfg") {
  }

  void Start(CompilationUnit* cUnit) const {
    cUnit->mir_graph->VerifyDataflow();
  }

  bool WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const;
};

/**
 * @class SSATransformation
 * @brief Perform an SSA representation pass on the CompilationUnit.
 */
class SSATransformation : public Pass {
 public:
  SSATransformation() : Pass("SSATransformation", kPreOrderDFSTraversal, "3_post_ssa_cfg") {
  }

  bool WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const;

  void Start(CompilationUnit* cUnit) const {
    cUnit->mir_graph->InitializeSSATransformation();
  }

  void End(CompilationUnit* cUnit) const;
};

/**
 * @class ConstantPropagation
 * @brief Perform a constant propagation pass.
 */
class ConstantPropagation : public Pass {
 public:
  ConstantPropagation() : Pass("ConstantPropagation") {
  }

  bool WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const;

  void Start(CompilationUnit* cUnit) const {
    cUnit->mir_graph->InitializeConstantPropagation();
  }
};

/**
 * @class InitRegLocations
 * @brief Initialize Register Locations.
 */
class InitRegLocations : public Pass {
 public:
  InitRegLocations() : Pass("InitRegLocation", kNoNodes) {
  }

  void Start(CompilationUnit* cUnit) const {
    cUnit->mir_graph->InitRegLocations();
  }
};

/**
 * @class MethodUseCount
 * @brief Count the register uses of the method
 */
class MethodUseCount : public Pass {
 public:
  MethodUseCount() : Pass("UseCount") {
  }

  bool WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const;

  bool Gate(const CompilationUnit* cUnit) const;
};

/**
 * @class NullCheckEliminationAndTypeInferenceInit
 * @brief Null check elimination and type inference initialization step.
 */
class NullCheckEliminationAndTypeInferenceInit : public Pass {
 public:
  NullCheckEliminationAndTypeInferenceInit() : Pass("NCE_TypeInferenceInit") {
  }

  bool WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const;

  bool Gate(const CompilationUnit* cUnit) const;
};

/**
 * @class NullCheckEliminationAndTypeInference
 * @brief Null check elimination and type inference.
 */
class NullCheckEliminationAndTypeInference : public Pass {
 public:
  NullCheckEliminationAndTypeInference()
    : Pass("NCE_TypeInference", kRepeatingPreOrderDFSTraversal, "4_post_nce_cfg") {
  }

  bool WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const {
    return cUnit->mir_graph->EliminateNullChecksAndInferTypes(bb);
  }
};

/**
 * @class NullCheckEliminationAndTypeInference
 * @brief Null check elimination and type inference.
 */
class BBCombine : public Pass {
 public:
  BBCombine() : Pass("BBCombine", kPreOrderDFSTraversal, "5_post_bbcombine_cfg") {
  }

  bool Gate(const CompilationUnit* cUnit) const {
    return ((cUnit->disable_opt & (1 << kSuppressExceptionEdges)) != 0);
  }

  bool WalkBasicBlocks(CompilationUnit* cUnit, BasicBlock* bb) const;
};

/**
 * @class BasicBlock Optimizations
 * @brief Any simple BasicBlock optimization can be put here.
 */
class BBOptimizations : public Pass {
 public:
  BBOptimizations() : Pass("BBOptimizations", kNoNodes, "5_post_bbo_cfg") {
  }

  bool Gate(const CompilationUnit* cUnit) const {
    return ((cUnit->disable_opt & (1 << kBBOpt)) == 0);
  }

  void Start(CompilationUnit* cUnit) const;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_BB_OPTIMIZATIONS_H_
