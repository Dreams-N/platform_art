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

#ifndef ART_COMPILER_OPTIMIZING_DEAD_BLOCK_ELIMINATION_H_
#define ART_COMPILER_OPTIMIZING_DEAD_BLOCK_ELIMINATION_H_

#include "nodes.h"
#include "optimization.h"

namespace art {

/**
 * Optimization pass performing dead code elimination (removal of
 * unused variables/instructions) on the SSA form.
 */
class HDeadBlockElimination : public HOptimization {
 public:
  explicit HDeadBlockElimination(HGraph* graph,
                                 OptimizingCompilerStats* stats = nullptr)
      : HOptimization(graph, true, kDeadBlockEliminationPassName, stats) {}

  void Run() OVERRIDE;

  static constexpr const char* kDeadBlockEliminationPassName =
    "dead_block_elimination";

 private:
  DISALLOW_COPY_AND_ASSIGN(HDeadBlockElimination);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_DEAD_BLOCK_ELIMINATION_H_
