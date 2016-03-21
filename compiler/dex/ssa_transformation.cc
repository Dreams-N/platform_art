/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "base/bit_vector-inl.h"
#include "base/logging.h"
#include "base/scoped_arena_containers.h"
#include "compiler_ir.h"
#include "dataflow_iterator-inl.h"

#define NOTVISITED (-1)

namespace art {

void MIRGraph::ClearAllVisitedFlags() {
  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    bb->visited = false;
  }
}

BasicBlock* MIRGraph::NeedsVisit(BasicBlock* bb) {
  if (bb != nullptr) {
    if (bb->visited || bb->hidden) {
      bb = nullptr;
    }
  }
  return bb;
}

BasicBlock* MIRGraph::NextUnvisitedSuccessor(BasicBlock* bb) {
  BasicBlock* res = NeedsVisit(GetBasicBlock(bb->fall_through));
  if (res == nullptr) {
    res = NeedsVisit(GetBasicBlock(bb->taken));
    if (res == nullptr) {
      if (bb->successor_block_list_type != kNotUsed) {
        for (SuccessorBlockInfo* sbi : bb->successor_blocks) {
          res = NeedsVisit(GetBasicBlock(sbi->block));
          if (res != nullptr) {
            break;
          }
        }
      }
    }
  }
  return res;
}

void MIRGraph::MarkPreOrder(BasicBlock* block) {
  block->visited = true;
  /* Enqueue the pre_order block id */
  if (block->id != NullBasicBlockId) {
    dfs_order_.push_back(block->id);
  }
}

void MIRGraph::RecordDFSOrders(BasicBlock* block) {
  ScopedArenaAllocator allocator(&cu_->arena_stack);
  ScopedArenaVector<BasicBlock*> succ(allocator.Adapter());
  succ.reserve(GetNumBlocks());
  MarkPreOrder(block);
  succ.push_back(block);
  while (!succ.empty()) {
    BasicBlock* curr = succ.back();
    BasicBlock* next_successor = NextUnvisitedSuccessor(curr);
    if (next_successor != nullptr) {
      MarkPreOrder(next_successor);
      succ.push_back(next_successor);
      continue;
    }
    curr->dfs_id = dfs_post_order_.size();
    if (curr->id != NullBasicBlockId) {
      dfs_post_order_.push_back(curr->id);
    }
    succ.pop_back();
  }
}

/* Sort the blocks by the Depth-First-Search */
void MIRGraph::ComputeDFSOrders() {
  /* Clear the DFS pre-order and post-order lists. */
  dfs_order_.clear();
  dfs_order_.reserve(GetNumBlocks());
  dfs_post_order_.clear();
  dfs_post_order_.reserve(GetNumBlocks());

  // Reset visited flags from all nodes
  ClearAllVisitedFlags();

  // Record dfs orders
  RecordDFSOrders(GetEntryBlock());

  num_reachable_blocks_ = dfs_order_.size();

  if (num_reachable_blocks_ != GetNumBlocks()) {
    // Kill all unreachable blocks.
    AllNodesIterator iter(this);
    for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
      if (!bb->visited) {
        bb->Kill(this);
      }
    }
  }
  dfs_orders_up_to_date_ = true;
}

/*
 * Mark block bit on the per-Dalvik register vector to denote that Dalvik
 * register idx is defined in BasicBlock bb.
 */
bool MIRGraph::FillDefBlockMatrix(BasicBlock* bb) {
  if (bb->data_flow_info == nullptr) {
    return false;
  }

  for (uint32_t idx : bb->data_flow_info->def_v->Indexes()) {
    /* Block bb defines register idx */
    temp_.ssa.def_block_matrix[idx]->SetBit(bb->id);
  }
  return true;
}

void MIRGraph::ComputeDefBlockMatrix() {
  int num_registers = GetNumOfCodeAndTempVRs();
  /* Allocate num_registers bit vector pointers */
  DCHECK(temp_scoped_alloc_ != nullptr);
  DCHECK(temp_.ssa.def_block_matrix == nullptr);
  temp_.ssa.def_block_matrix =
      temp_scoped_alloc_->AllocArray<ArenaBitVector*>(num_registers, kArenaAllocDFInfo);
  int i;

  /* Initialize num_register vectors with num_blocks bits each */
  for (i = 0; i < num_registers; i++) {
    temp_.ssa.def_block_matrix[i] = new (temp_scoped_alloc_.get()) ArenaBitVector(
        arena_, GetNumBlocks(), false);
    temp_.ssa.def_block_matrix[i]->ClearAllBits();
  }

  AllNodesIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    FindLocalLiveIn(bb);
  }
  AllNodesIterator iter2(this);
  for (BasicBlock* bb = iter2.Next(); bb != nullptr; bb = iter2.Next()) {
    FillDefBlockMatrix(bb);
  }

  /*
   * Also set the incoming parameters as defs in the entry block.
   * Only need to handle the parameters for the outer method.
   */
  int num_regs = GetNumOfCodeVRs();
  int in_reg = GetFirstInVR();
  for (; in_reg < num_regs; in_reg++) {
    temp_.ssa.def_block_matrix[in_reg]->SetBit(GetEntryBlock()->id);
  }
}

void MIRGraph::ComputeDomPostOrderTraversal(BasicBlock* bb) {
  // Clear the dominator post-order list.
  dom_post_order_traversal_.clear();
  dom_post_order_traversal_.reserve(num_reachable_blocks_);

  ClearAllVisitedFlags();
  ScopedArenaAllocator allocator(&cu_->arena_stack);
  ScopedArenaVector<std::pair<BasicBlock*, ArenaBitVector::IndexIterator>> work_stack(
      allocator.Adapter());
  bb->visited = true;
  work_stack.push_back(std::make_pair(bb, bb->i_dominated->Indexes().begin()));
  while (!work_stack.empty()) {
    std::pair<BasicBlock*, ArenaBitVector::IndexIterator>* curr = &work_stack.back();
    BasicBlock* curr_bb = curr->first;
    ArenaBitVector::IndexIterator* curr_idom_iter = &curr->second;
    while (!curr_idom_iter->Done() && (NeedsVisit(GetBasicBlock(**curr_idom_iter)) == nullptr)) {
      ++*curr_idom_iter;
    }
    // NOTE: work_stack.push_back()/pop_back() invalidate curr and curr_idom_iter.
    if (!curr_idom_iter->Done()) {
      BasicBlock* new_bb = GetBasicBlock(**curr_idom_iter);
      ++*curr_idom_iter;
      new_bb->visited = true;
      work_stack.push_back(std::make_pair(new_bb, new_bb->i_dominated->Indexes().begin()));
    } else {
      // no successor/next
      if (curr_bb->id != NullBasicBlockId) {
        dom_post_order_traversal_.push_back(curr_bb->id);
      }
      work_stack.pop_back();
    }
  }
}

void MIRGraph::CheckForDominanceFrontier(BasicBlock* dom_bb,
                                         const BasicBlock* succ_bb) {
  /*
   * TODO - evaluate whether phi will ever need to be inserted into exit
   * blocks.
   */
  if (succ_bb->i_dom != dom_bb->id &&
    succ_bb->block_type == kDalvikByteCode &&
    succ_bb->hidden == false) {
    dom_bb->dom_frontier->SetBit(succ_bb->id);
  }
}

/* Worker function to compute the dominance frontier */
bool MIRGraph::ComputeDominanceFrontier(BasicBlock* bb) {
  /* Calculate DF_local */
  if (bb->taken != NullBasicBlockId) {
    CheckForDominanceFrontier(bb, GetBasicBlock(bb->taken));
  }
  if (bb->fall_through != NullBasicBlockId) {
    CheckForDominanceFrontier(bb, GetBasicBlock(bb->fall_through));
  }
  if (bb->successor_block_list_type != kNotUsed) {
    for (SuccessorBlockInfo* successor_block_info : bb->successor_blocks) {
      BasicBlock* succ_bb = GetBasicBlock(successor_block_info->block);
      CheckForDominanceFrontier(bb, succ_bb);
    }
  }

  /* Calculate DF_up */
  for (uint32_t dominated_idx : bb->i_dominated->Indexes()) {
    BasicBlock* dominated_bb = GetBasicBlock(dominated_idx);
    for (uint32_t df_up_block_idx : dominated_bb->dom_frontier->Indexes()) {
      BasicBlock* df_up_block = GetBasicBlock(df_up_block_idx);
      CheckForDominanceFrontier(bb, df_up_block);
    }
  }

  return true;
}

/* Worker function for initializing domination-related data structures */
void MIRGraph::InitializeDominationInfo(BasicBlock* bb) {
  int num_total_blocks = GetBasicBlockListCount();

  if (bb->dominators == nullptr) {
    bb->dominators = new (arena_) ArenaBitVector(arena_, num_total_blocks, true /* expandable */);
    bb->i_dominated = new (arena_) ArenaBitVector(arena_, num_total_blocks, true /* expandable */);
    bb->dom_frontier = new (arena_) ArenaBitVector(arena_, num_total_blocks, true /* expandable */);
  } else {
    bb->dominators->ClearAllBits();
    bb->i_dominated->ClearAllBits();
    bb->dom_frontier->ClearAllBits();
  }
  /* Set all bits in the dominator vector */
  bb->dominators->SetInitialBits(num_total_blocks);

  return;
}

/*
 * Walk through the ordered i_dom list until we reach common parent.
 * Given the ordering of i_dom_list, this common parent represents the
 * last element of the intersection of block1 and block2 dominators.
  */
int MIRGraph::FindCommonParent(int block1, int block2) {
  while (block1 != block2) {
    while (block1 < block2) {
      block1 = i_dom_list_[block1];
      DCHECK_NE(block1, NOTVISITED);
    }
    while (block2 < block1) {
      block2 = i_dom_list_[block2];
      DCHECK_NE(block2, NOTVISITED);
    }
  }
  return block1;
}

/* Worker function to compute each block's immediate dominator */
bool MIRGraph::ComputeblockIDom(BasicBlock* bb) {
  /* Special-case entry block */
  if ((bb->id == NullBasicBlockId) || (bb == GetEntryBlock())) {
    return false;
  }

  /* Iterate through the predecessors */
  auto it = bb->predecessors.begin(), end = bb->predecessors.end();

  /* Find the first processed predecessor */
  int idom = -1;
  for ( ; ; ++it) {
    CHECK(it != end);
    BasicBlock* pred_bb = GetBasicBlock(*it);
    DCHECK(pred_bb != nullptr);
    if (i_dom_list_[pred_bb->dfs_id] != NOTVISITED) {
      idom = pred_bb->dfs_id;
      break;
    }
  }

  /* Scan the rest of the predecessors */
  for ( ; it != end; ++it) {
      BasicBlock* pred_bb = GetBasicBlock(*it);
      DCHECK(pred_bb != nullptr);
      if (i_dom_list_[pred_bb->dfs_id] == NOTVISITED) {
        continue;
      } else {
        idom = FindCommonParent(pred_bb->dfs_id, idom);
      }
  }

  DCHECK_NE(idom, NOTVISITED);

  /* Did something change? */
  if (i_dom_list_[bb->dfs_id] != idom) {
    i_dom_list_[bb->dfs_id] = idom;
    return true;
  }
  return false;
}

/* Worker function to compute each block's domintors */
bool MIRGraph::ComputeBlockDominators(BasicBlock* bb) {
  if (bb == GetEntryBlock()) {
    bb->dominators->ClearAllBits();
  } else {
    bb->dominators->Copy(GetBasicBlock(bb->i_dom)->dominators);
  }
  bb->dominators->SetBit(bb->id);
  return false;
}

bool MIRGraph::SetDominators(BasicBlock* bb) {
  if (bb != GetEntryBlock()) {
    int idom_dfs_idx = i_dom_list_[bb->dfs_id];
    DCHECK_NE(idom_dfs_idx, NOTVISITED);
    int i_dom_idx = dfs_post_order_[idom_dfs_idx];
    BasicBlock* i_dom = GetBasicBlock(i_dom_idx);
    bb->i_dom = i_dom->id;
    /* Add bb to the i_dominated set of the immediate dominator block */
    i_dom->i_dominated->SetBit(bb->id);
  }
  return false;
}

/* Compute dominators, immediate dominator, and dominance fronter */
void MIRGraph::ComputeDominators() {
  int num_reachable_blocks = num_reachable_blocks_;

  /* Initialize domination-related data structures */
  PreOrderDfsIterator iter(this);
  for (BasicBlock* bb = iter.Next(); bb != nullptr; bb = iter.Next()) {
    InitializeDominationInfo(bb);
  }

  /* Initialize & Clear i_dom_list */
  if (max_num_reachable_blocks_ < num_reachable_blocks_) {
    i_dom_list_ = arena_->AllocArray<int>(num_reachable_blocks, kArenaAllocDFInfo);
  }
  for (int i = 0; i < num_reachable_blocks; i++) {
    i_dom_list_[i] = NOTVISITED;
  }

  /* For post-order, last block is entry block.  Set its i_dom to istelf */
  DCHECK_EQ(GetEntryBlock()->dfs_id, num_reachable_blocks-1);
  i_dom_list_[GetEntryBlock()->dfs_id] = GetEntryBlock()->dfs_id;

  /* Compute the immediate dominators */
  RepeatingReversePostOrderDfsIterator iter2(this);
  bool change = false;
  for (BasicBlock* bb = iter2.Next(false); bb != nullptr; bb = iter2.Next(change)) {
    change = ComputeblockIDom(bb);
  }

  /* Set the dominator for the root node */
  GetEntryBlock()->dominators->ClearAllBits();
  GetEntryBlock()->dominators->SetBit(GetEntryBlock()->id);

  GetEntryBlock()->i_dom = 0;

  PreOrderDfsIterator iter3(this);
  for (BasicBlock* bb = iter3.Next(); bb != nullptr; bb = iter3.Next()) {
    SetDominators(bb);
  }

  ReversePostOrderDfsIterator iter4(this);
  for (BasicBlock* bb = iter4.Next(); bb != nullptr; bb = iter4.Next()) {
    ComputeBlockDominators(bb);
  }

  // Compute the dominance frontier for each block.
  ComputeDomPostOrderTraversal(GetEntryBlock());
  PostOrderDOMIterator iter5(this);
  for (BasicBlock* bb = iter5.Next(); bb != nullptr; bb = iter5.Next()) {
    ComputeDominanceFrontier(bb);
  }

  domination_up_to_date_ = true;
}

/*
 * Perform dest U= src1 ^ ~src2
 * This is probably not general enough to be placed in BitVector.[ch].
 */
void MIRGraph::ComputeSuccLineIn(ArenaBitVector* dest, const ArenaBitVector* src1,
                                 const ArenaBitVector* src2) {
  if (dest->GetStorageSize() != src1->GetStorageSize() ||
      dest->GetStorageSize() != src2->GetStorageSize() ||
      dest->IsExpandable() != src1->IsExpandable() ||
      dest->IsExpandable() != src2->IsExpandable()) {
    LOG(FATAL) << "Incompatible set properties";
  }

  unsigned int idx;
  for (idx = 0; idx < dest->GetStorageSize(); idx++) {
    dest->GetRawStorage()[idx] |= src1->GetRawStorageWord(idx) & ~(src2->GetRawStorageWord(idx));
  }
}

/*
 * Iterate through all successor blocks and propagate up the live-in sets.
 * The calculated result is used for phi-node pruning - where we only need to
 * insert a phi node if the variable is live-in to the block.
 */
bool MIRGraph::ComputeBlockLiveIns(BasicBlock* bb) {
  DCHECK_EQ(temp_.ssa.num_vregs, cu_->mir_graph.get()->GetNumOfCodeAndTempVRs());
  ArenaBitVector* temp_live_vregs = temp_.ssa.work_live_vregs;

  if (bb->data_flow_info == nullptr) {
    return false;
  }
  temp_live_vregs->Copy(bb->data_flow_info->live_in_v);
  BasicBlock* bb_taken = GetBasicBlock(bb->taken);
  BasicBlock* bb_fall_through = GetBasicBlock(bb->fall_through);
  if (bb_taken && bb_taken->data_flow_info)
    ComputeSuccLineIn(temp_live_vregs, bb_taken->data_flow_info->live_in_v,
                      bb->data_flow_info->def_v);
  if (bb_fall_through && bb_fall_through->data_flow_info)
    ComputeSuccLineIn(temp_live_vregs, bb_fall_through->data_flow_info->live_in_v,
                      bb->data_flow_info->def_v);
  if (bb->successor_block_list_type != kNotUsed) {
    for (SuccessorBlockInfo* successor_block_info : bb->successor_blocks) {
      BasicBlock* succ_bb = GetBasicBlock(successor_block_info->block);
      if (succ_bb->data_flow_info) {
        ComputeSuccLineIn(temp_live_vregs, succ_bb->data_flow_info->live_in_v,
                          bb->data_flow_info->def_v);
      }
    }
  }
  if (!temp_live_vregs->Equal(bb->data_flow_info->live_in_v)) {
    bb->data_flow_info->live_in_v->Copy(temp_live_vregs);
    return true;
  }
  return false;
}

/* For each dalvik reg, find blocks that need phi nodes according to the dominance frontiers. */
void MIRGraph::FindPhiNodeBlocks() {
  RepeatingPostOrderDfsIterator iter(this);
  bool change = false;
  for (BasicBlock* bb = iter.Next(false); bb != nullptr; bb = iter.Next(change)) {
    change = ComputeBlockLiveIns(bb);
  }

  ArenaBitVector* phi_blocks = new (temp_scoped_alloc_.get()) ArenaBitVector(
      temp_scoped_alloc_.get(), GetNumBlocks(), false);

  // Reuse the def_block_matrix storage for phi_node_blocks.
  ArenaBitVector** def_block_matrix = temp_.ssa.def_block_matrix;
  ArenaBitVector** phi_node_blocks = def_block_matrix;
  DCHECK(temp_.ssa.phi_node_blocks == nullptr);
  temp_.ssa.phi_node_blocks = phi_node_blocks;
  temp_.ssa.def_block_matrix = nullptr;

  /* Iterate through each Dalvik register */
  for (int dalvik_reg = GetNumOfCodeAndTempVRs() - 1; dalvik_reg >= 0; dalvik_reg--) {
    phi_blocks->ClearAllBits();
    ArenaBitVector* input_blocks = def_block_matrix[dalvik_reg];
    do {
      // TUNING: When we repeat this, we could skip indexes from the previous pass.
      for (uint32_t idx : input_blocks->Indexes()) {
        BasicBlock* def_bb = GetBasicBlock(idx);
        if (def_bb->dom_frontier != nullptr) {
          phi_blocks->Union(def_bb->dom_frontier);
        }
      }
    } while (input_blocks->Union(phi_blocks));

    def_block_matrix[dalvik_reg] = phi_blocks;
    phi_blocks = input_blocks;  // Reuse the bit vector in next iteration.
  }
}

/*
 * Worker function to insert phi-operands with latest SSA names from
 * predecessor blocks
 */
bool MIRGraph::InsertPhiNodeOperands(BasicBlock* bb) {
  /* Phi nodes are at the beginning of each block */
  for (MIR* mir = bb->first_mir_insn; mir != nullptr; mir = mir->next) {
    if (mir->dalvikInsn.opcode != static_cast<Instruction::Code>(kMirOpPhi))
      return true;
    int ssa_reg = mir->ssa_rep->defs[0];
    DCHECK_GE(ssa_reg, 0);   // Shouldn't see compiler temps here
    int v_reg = SRegToVReg(ssa_reg);

    /* Iterate through the predecessors */
    size_t num_uses = bb->predecessors.size();
    AllocateSSAUseData(mir, num_uses);
    int* uses = mir->ssa_rep->uses;
    BasicBlockId* incoming = arena_->AllocArray<BasicBlockId>(num_uses, kArenaAllocDFInfo);
    mir->meta.phi_incoming = incoming;
    int idx = 0;
    for (BasicBlockId pred_id : bb->predecessors) {
      BasicBlock* pred_bb = GetBasicBlock(pred_id);
      DCHECK(pred_bb != nullptr);
      uses[idx] = pred_bb->data_flow_info->vreg_to_ssa_map_exit[v_reg];
      incoming[idx] = pred_id;
      idx++;
    }
  }

  return true;
}

void MIRGraph::DoDFSPreOrderSSARename(BasicBlock* block) {
  if (block->visited || block->hidden) {
    return;
  }

  typedef struct {
    BasicBlock* bb;
    int32_t* ssa_map;
  } BasicBlockInfo;
  BasicBlockInfo temp;

  ScopedArenaAllocator allocator(&cu_->arena_stack);
  ScopedArenaVector<BasicBlockInfo> bi_stack(allocator.Adapter());
  ScopedArenaVector<BasicBlock*> succ_stack(allocator.Adapter());

  uint32_t num_vregs = GetNumOfCodeAndTempVRs();
  size_t map_size = sizeof(int32_t) * num_vregs;
  temp.bb = block;
  temp.ssa_map = vreg_to_ssa_map_;
  bi_stack.push_back(temp);

  while (!bi_stack.empty()) {
    temp = bi_stack.back();
    bi_stack.pop_back();
    BasicBlock* b = temp.bb;

    if (b->visited || b->hidden) {
      continue;
    }
    b->visited = true;

    /* Restore SSA map snapshot, except for the first block */
    if (b != block) {
      memcpy(vreg_to_ssa_map_, temp.ssa_map, map_size);
    }

    /* Process this block */
    DoSSAConversion(b);

    /* If there are no successor, taken, and fall through blocks, continue */
    if (b->successor_block_list_type == kNotUsed &&
        b->taken == NullBasicBlockId &&
        b->fall_through == NullBasicBlockId) {
      continue;
    }

    /* Save SSA map snapshot */
    int32_t* saved_ssa_map =
      allocator.AllocArray<int32_t>(num_vregs, kArenaAllocDalvikToSSAMap);
    memcpy(saved_ssa_map, vreg_to_ssa_map_, map_size);

    if (b->successor_block_list_type != kNotUsed) {
      for (SuccessorBlockInfo* successor_block_info : b->successor_blocks) {
        BasicBlock* succ_bb = GetBasicBlock(successor_block_info->block);
        succ_stack.push_back(succ_bb);
      }
      while (!succ_stack.empty()) {
        temp.bb = succ_stack.back();
        succ_stack.pop_back();
        temp.ssa_map = saved_ssa_map;
        bi_stack.push_back(temp);
      }
    }
    if (b->taken != NullBasicBlockId) {
      temp.bb = GetBasicBlock(b->taken);
      temp.ssa_map = saved_ssa_map;
      bi_stack.push_back(temp);
    }
    if (b->fall_through != NullBasicBlockId) {
      temp.bb = GetBasicBlock(b->fall_through);
      temp.ssa_map = saved_ssa_map;
      bi_stack.push_back(temp);
    }
  }
}

}  // namespace art
