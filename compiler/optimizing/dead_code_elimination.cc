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

#include "dead_code_elimination.h"

#include "base/bit_vector-inl.h"

namespace art {

void HDeadCodeElimination::Run() {
  // Process basic blocks in post-order in the dominator tree, so that
  // a dead instruction depending on another dead instruction is
  // removed.
  HGraphVisitor::VisitPostOrder();
}

void HDeadCodeElimination::VisitBasicBlock(HBasicBlock* block) {
  // Traverse this block's instructions in backward order and remove
  // the unused ones.
  HBackwardInstructionIterator i(block->GetInstructions());
  // Skip the first iteration, as the last instruction of a block is
  // a branching instruction.
  DCHECK(i.Current()->IsControlFlow());
  for (i.Advance(); !i.Done(); i.Advance()) {
    i.Current()->Accept(this);
  }
}

void HDeadCodeElimination::VisitInstruction(HInstruction* instruction) {
  DCHECK(!instruction->IsControlFlow());
  if (!instruction->HasSideEffects()
      && !instruction->CanThrow()
      && !instruction->IsSuspendCheck()
      && !instruction->HasUses()) {
    instruction->GetBlock()->RemoveInstruction(instruction);
  }
}

}  // namespace art
