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

#ifndef ART_COMPILER_OPTIMIZING_NODES_H_
#define ART_COMPILER_OPTIMIZING_NODES_H_

#include "entrypoints/quick/quick_entrypoints_enum.h"
#include "handle.h"
#include "handle_scope.h"
#include "invoke_type.h"
#include "locations.h"
#include "mirror/class.h"
#include "offsets.h"
#include "primitive.h"
#include "utils/arena_object.h"
#include "utils/arena_bit_vector.h"
#include "utils/growable_array.h"

namespace art {

class HBasicBlock;
class HEnvironment;
class HInstruction;
class HIntConstant;
class HInvoke;
class HGraphVisitor;
class HPhi;
class HSuspendCheck;
class LiveInterval;
class LocationSummary;

static const int kDefaultNumberOfBlocks = 8;
static const int kDefaultNumberOfSuccessors = 2;
static const int kDefaultNumberOfPredecessors = 2;
static const int kDefaultNumberOfDominatedBlocks = 1;
static const int kDefaultNumberOfBackEdges = 1;

static constexpr uint32_t kMaxIntShiftValue = 0x1f;
static constexpr uint64_t kMaxLongShiftValue = 0x3f;

enum IfCondition {
  kCondEQ,
  kCondNE,
  kCondLT,
  kCondLE,
  kCondGT,
  kCondGE,
};

class HInstructionList {
 public:
  HInstructionList() : first_instruction_(nullptr), last_instruction_(nullptr) {}

  void AddInstruction(HInstruction* instruction);
  void RemoveInstruction(HInstruction* instruction);

  // Return true if this list contains `instruction`.
  bool Contains(HInstruction* instruction) const;

  // Return true if `instruction1` is found before `instruction2` in
  // this instruction list and false otherwise.  Abort if none
  // of these instructions is found.
  bool FoundBefore(const HInstruction* instruction1,
                   const HInstruction* instruction2) const;

  bool IsEmpty() const { return first_instruction_ == nullptr; }
  void Clear() { first_instruction_ = last_instruction_ = nullptr; }

  // Update the block of all instructions to be `block`.
  void SetBlockOfInstructions(HBasicBlock* block) const;

  void AddAfter(HInstruction* cursor, const HInstructionList& instruction_list);
  void Add(const HInstructionList& instruction_list);

 private:
  HInstruction* first_instruction_;
  HInstruction* last_instruction_;

  friend class HBasicBlock;
  friend class HGraph;
  friend class HInstruction;
  friend class HInstructionIterator;
  friend class HBackwardInstructionIterator;

  DISALLOW_COPY_AND_ASSIGN(HInstructionList);
};

// Control-flow graph of a method. Contains a list of basic blocks.
class HGraph : public ArenaObject<kArenaAllocMisc> {
 public:
  HGraph(ArenaAllocator* arena, int start_instruction_id = 0)
      : arena_(arena),
        blocks_(arena, kDefaultNumberOfBlocks),
        reverse_post_order_(arena, kDefaultNumberOfBlocks),
        entry_block_(nullptr),
        exit_block_(nullptr),
        maximum_number_of_out_vregs_(0),
        number_of_vregs_(0),
        number_of_in_vregs_(0),
        temporaries_vreg_slots_(0),
        current_instruction_id_(start_instruction_id) {}

  ArenaAllocator* GetArena() const { return arena_; }
  const GrowableArray<HBasicBlock*>& GetBlocks() const { return blocks_; }
  HBasicBlock* GetBlock(size_t id) const { return blocks_.Get(id); }

  HBasicBlock* GetEntryBlock() const { return entry_block_; }
  HBasicBlock* GetExitBlock() const { return exit_block_; }

  void SetEntryBlock(HBasicBlock* block) { entry_block_ = block; }
  void SetExitBlock(HBasicBlock* block) { exit_block_ = block; }

  void AddBlock(HBasicBlock* block);

  // Try building the SSA form of this graph, with dominance computation and loop
  // recognition. Returns whether it was successful in doing all these steps.
  bool TryBuildingSsa() {
    BuildDominatorTree();
    TransformToSsa();
    return AnalyzeNaturalLoops();
  }

  void BuildDominatorTree();
  void TransformToSsa();
  void SimplifyCFG();

  // Analyze all natural loops in this graph. Returns false if one
  // loop is not natural, that is the header does not dominate the
  // back edge.
  bool AnalyzeNaturalLoops() const;

  // Inline this graph in `outer_graph`, replacing the given `invoke` instruction.
  void InlineInto(HGraph* outer_graph, HInvoke* invoke);

  void SplitCriticalEdge(HBasicBlock* block, HBasicBlock* successor);
  void SimplifyLoop(HBasicBlock* header);

  int32_t GetNextInstructionId() {
    DCHECK_NE(current_instruction_id_, INT32_MAX);
    return current_instruction_id_++;
  }

  int32_t GetCurrentInstructionId() const {
    return current_instruction_id_;
  }

  void SetCurrentInstructionId(int32_t id) {
    current_instruction_id_ = id;
  }

  uint16_t GetMaximumNumberOfOutVRegs() const {
    return maximum_number_of_out_vregs_;
  }

  void SetMaximumNumberOfOutVRegs(uint16_t new_value) {
    maximum_number_of_out_vregs_ = new_value;
  }

  void UpdateTemporariesVRegSlots(size_t slots) {
    temporaries_vreg_slots_ = std::max(slots, temporaries_vreg_slots_);
  }

  size_t GetTemporariesVRegSlots() const {
    return temporaries_vreg_slots_;
  }

  void SetNumberOfVRegs(uint16_t number_of_vregs) {
    number_of_vregs_ = number_of_vregs;
  }

  uint16_t GetNumberOfVRegs() const {
    return number_of_vregs_;
  }

  void SetNumberOfInVRegs(uint16_t value) {
    number_of_in_vregs_ = value;
  }

  uint16_t GetNumberOfLocalVRegs() const {
    return number_of_vregs_ - number_of_in_vregs_;
  }

  const GrowableArray<HBasicBlock*>& GetReversePostOrder() const {
    return reverse_post_order_;
  }

 private:
  HBasicBlock* FindCommonDominator(HBasicBlock* first, HBasicBlock* second) const;
  void VisitBlockForDominatorTree(HBasicBlock* block,
                                  HBasicBlock* predecessor,
                                  GrowableArray<size_t>* visits);
  void FindBackEdges(ArenaBitVector* visited);
  void VisitBlockForBackEdges(HBasicBlock* block,
                              ArenaBitVector* visited,
                              ArenaBitVector* visiting);
  void RemoveInstructionsAsUsersFromDeadBlocks(const ArenaBitVector& visited) const;
  void RemoveDeadBlocks(const ArenaBitVector& visited) const;
  void RemoveBlock(HBasicBlock* block) const;

  ArenaAllocator* const arena_;

  // List of blocks in insertion order.
  GrowableArray<HBasicBlock*> blocks_;

  // List of blocks to perform a reverse post order tree traversal.
  GrowableArray<HBasicBlock*> reverse_post_order_;

  HBasicBlock* entry_block_;
  HBasicBlock* exit_block_;

  // The maximum number of virtual registers arguments passed to a HInvoke in this graph.
  uint16_t maximum_number_of_out_vregs_;

  // The number of virtual registers in this method. Contains the parameters.
  uint16_t number_of_vregs_;

  // The number of virtual registers used by parameters of this method.
  uint16_t number_of_in_vregs_;

  // Number of vreg size slots that the temporaries use (used in baseline compiler).
  size_t temporaries_vreg_slots_;

  // The current id to assign to a newly added instruction. See HInstruction.id_.
  int32_t current_instruction_id_;

  ART_FRIEND_TEST(GraphTest, IfSuccessorSimpleJoinBlock1);
  DISALLOW_COPY_AND_ASSIGN(HGraph);
};

class HLoopInformation : public ArenaObject<kArenaAllocMisc> {
 public:
  HLoopInformation(HBasicBlock* header, HGraph* graph)
      : header_(header),
        suspend_check_(nullptr),
        back_edges_(graph->GetArena(), kDefaultNumberOfBackEdges),
        // Make bit vector growable, as the number of blocks may change.
        blocks_(graph->GetArena(), graph->GetBlocks().Size(), true) {}

  HBasicBlock* GetHeader() const {
    return header_;
  }

  void SetHeader(HBasicBlock* block) {
    header_ = block;
  }

  HSuspendCheck* GetSuspendCheck() const { return suspend_check_; }
  void SetSuspendCheck(HSuspendCheck* check) { suspend_check_ = check; }
  bool HasSuspendCheck() const { return suspend_check_ != nullptr; }

  void AddBackEdge(HBasicBlock* back_edge) {
    back_edges_.Add(back_edge);
  }

  void RemoveBackEdge(HBasicBlock* back_edge) {
    back_edges_.Delete(back_edge);
  }

  bool IsBackEdge(HBasicBlock* block) {
    for (size_t i = 0, e = back_edges_.Size(); i < e; ++i) {
      if (back_edges_.Get(i) == block) return true;
    }
    return false;
  }

  size_t NumberOfBackEdges() const {
    return back_edges_.Size();
  }

  HBasicBlock* GetPreHeader() const;

  const GrowableArray<HBasicBlock*>& GetBackEdges() const {
    return back_edges_;
  }

  void ClearBackEdges() {
    back_edges_.Reset();
  }

  // Find blocks that are part of this loop. Returns whether the loop is a natural loop,
  // that is the header dominates the back edge.
  bool Populate();

  // Returns whether this loop information contains `block`.
  // Note that this loop information *must* be populated before entering this function.
  bool Contains(const HBasicBlock& block) const;

  // Returns whether this loop information is an inner loop of `other`.
  // Note that `other` *must* be populated before entering this function.
  bool IsIn(const HLoopInformation& other) const;

  const ArenaBitVector& GetBlocks() const { return blocks_; }

  void Add(HBasicBlock* block);

 private:
  // Internal recursive implementation of `Populate`.
  void PopulateRecursive(HBasicBlock* block);

  HBasicBlock* header_;
  HSuspendCheck* suspend_check_;
  GrowableArray<HBasicBlock*> back_edges_;
  ArenaBitVector blocks_;

  DISALLOW_COPY_AND_ASSIGN(HLoopInformation);
};

static constexpr size_t kNoLifetime = -1;
static constexpr uint32_t kNoDexPc = -1;

// A block in a method. Contains the list of instructions represented
// as a double linked list. Each block knows its predecessors and
// successors.

class HBasicBlock : public ArenaObject<kArenaAllocMisc> {
 public:
  explicit HBasicBlock(HGraph* graph, uint32_t dex_pc = kNoDexPc)
      : graph_(graph),
        predecessors_(graph->GetArena(), kDefaultNumberOfPredecessors),
        successors_(graph->GetArena(), kDefaultNumberOfSuccessors),
        loop_information_(nullptr),
        dominator_(nullptr),
        dominated_blocks_(graph->GetArena(), kDefaultNumberOfDominatedBlocks),
        block_id_(-1),
        dex_pc_(dex_pc),
        lifetime_start_(kNoLifetime),
        lifetime_end_(kNoLifetime),
        is_catch_block_(false) {}

  const GrowableArray<HBasicBlock*>& GetPredecessors() const {
    return predecessors_;
  }

  const GrowableArray<HBasicBlock*>& GetSuccessors() const {
    return successors_;
  }

  const GrowableArray<HBasicBlock*>& GetDominatedBlocks() const {
    return dominated_blocks_;
  }

  bool IsEntryBlock() const {
    return graph_->GetEntryBlock() == this;
  }

  bool IsExitBlock() const {
    return graph_->GetExitBlock() == this;
  }

  void AddBackEdge(HBasicBlock* back_edge) {
    if (loop_information_ == nullptr) {
      loop_information_ = new (graph_->GetArena()) HLoopInformation(this, graph_);
    }
    DCHECK_EQ(loop_information_->GetHeader(), this);
    loop_information_->AddBackEdge(back_edge);
  }

  HGraph* GetGraph() const { return graph_; }
  void SetGraph(HGraph* graph) { graph_ = graph; }

  int GetBlockId() const { return block_id_; }
  void SetBlockId(int id) { block_id_ = id; }

  HBasicBlock* GetDominator() const { return dominator_; }
  void SetDominator(HBasicBlock* dominator) { dominator_ = dominator; }
  void AddDominatedBlock(HBasicBlock* block) { dominated_blocks_.Add(block); }
  void ReplaceDominatedBlock(HBasicBlock* existing, HBasicBlock* new_block) {
    for (size_t i = 0, e = dominated_blocks_.Size(); i < e; ++i) {
      if (dominated_blocks_.Get(i) == existing) {
        dominated_blocks_.Put(i, new_block);
        return;
      }
    }
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  }

  int NumberOfBackEdges() const {
    return loop_information_ == nullptr
        ? 0
        : loop_information_->NumberOfBackEdges();
  }

  HInstruction* GetFirstInstruction() const { return instructions_.first_instruction_; }
  HInstruction* GetLastInstruction() const { return instructions_.last_instruction_; }
  const HInstructionList& GetInstructions() const { return instructions_; }
  const HInstructionList& GetPhis() const { return phis_; }
  HInstruction* GetFirstPhi() const { return phis_.first_instruction_; }

  void AddSuccessor(HBasicBlock* block) {
    successors_.Add(block);
    block->predecessors_.Add(this);
  }

  void ReplaceSuccessor(HBasicBlock* existing, HBasicBlock* new_block) {
    size_t successor_index = GetSuccessorIndexOf(existing);
    DCHECK_NE(successor_index, static_cast<size_t>(-1));
    existing->RemovePredecessor(this);
    new_block->predecessors_.Add(this);
    successors_.Put(successor_index, new_block);
  }

  void ReplacePredecessor(HBasicBlock* existing, HBasicBlock* new_block) {
    size_t predecessor_index = GetPredecessorIndexOf(existing);
    DCHECK_NE(predecessor_index, static_cast<size_t>(-1));
    existing->RemoveSuccessor(this);
    new_block->successors_.Add(this);
    predecessors_.Put(predecessor_index, new_block);
  }

  void RemovePredecessor(HBasicBlock* block) {
    predecessors_.Delete(block);
  }

  void RemoveSuccessor(HBasicBlock* block) {
    successors_.Delete(block);
  }

  void ClearAllPredecessors() {
    predecessors_.Reset();
  }

  void AddPredecessor(HBasicBlock* block) {
    predecessors_.Add(block);
    block->successors_.Add(this);
  }

  void SwapPredecessors() {
    DCHECK_EQ(predecessors_.Size(), 2u);
    HBasicBlock* temp = predecessors_.Get(0);
    predecessors_.Put(0, predecessors_.Get(1));
    predecessors_.Put(1, temp);
  }

  size_t GetPredecessorIndexOf(HBasicBlock* predecessor) {
    for (size_t i = 0, e = predecessors_.Size(); i < e; ++i) {
      if (predecessors_.Get(i) == predecessor) {
        return i;
      }
    }
    return -1;
  }

  size_t GetSuccessorIndexOf(HBasicBlock* successor) {
    for (size_t i = 0, e = successors_.Size(); i < e; ++i) {
      if (successors_.Get(i) == successor) {
        return i;
      }
    }
    return -1;
  }

  // Split the block into two blocks just after `cursor`. Returns the newly
  // created block. Note that this method just updates raw block information,
  // like predecessors, successors, dominators, and instruction list. It does not
  // update the graph, reverse post order, loop information, nor make sure the
  // blocks are consistent (for example ending with a control flow instruction).
  HBasicBlock* SplitAfter(HInstruction* cursor);

  // Merge `other` at the end of `this`. Successors and dominated blocks of
  // `other` are changed to be successors and dominated blocks of `this`. Note
  // that this method does not update the graph, reverse post order, loop
  // information, nor make sure the blocks are consistent (for example ending
  // with a control flow instruction).
  void MergeWith(HBasicBlock* other);

  // Replace `this` with `other`. Predecessors, successors, and dominated blocks
  // of `this` are moved to `other`.
  // Note that this method does not update the graph, reverse post order, loop
  // information, nor make sure the blocks are consistent (for example ending
  void ReplaceWith(HBasicBlock* other);

  void AddInstruction(HInstruction* instruction);
  void RemoveInstruction(HInstruction* instruction);
  void InsertInstructionBefore(HInstruction* instruction, HInstruction* cursor);
  // Replace instruction `initial` with `replacement` within this block.
  void ReplaceAndRemoveInstructionWith(HInstruction* initial,
                                       HInstruction* replacement);
  void AddPhi(HPhi* phi);
  void InsertPhiAfter(HPhi* instruction, HPhi* cursor);
  void RemovePhi(HPhi* phi);

  bool IsLoopHeader() const {
    return (loop_information_ != nullptr) && (loop_information_->GetHeader() == this);
  }

  bool IsLoopPreHeaderFirstPredecessor() const {
    DCHECK(IsLoopHeader());
    DCHECK(!GetPredecessors().IsEmpty());
    return GetPredecessors().Get(0) == GetLoopInformation()->GetPreHeader();
  }

  HLoopInformation* GetLoopInformation() const {
    return loop_information_;
  }

  // Set the loop_information_ on this block. Overrides the current
  // loop_information if it is an outer loop of the passed loop information.
  // Note that this method is called while creating the loop information.
  void SetInLoop(HLoopInformation* info) {
    if (IsLoopHeader()) {
      // Nothing to do. This just means `info` is an outer loop.
    } else if (loop_information_ == nullptr) {
      loop_information_ = info;
    } else if (loop_information_->Contains(*info->GetHeader())) {
      // Block is currently part of an outer loop. Make it part of this inner loop.
      // Note that a non loop header having a loop information means this loop information
      // has already been populated
      loop_information_ = info;
    } else {
      // Block is part of an inner loop. Do not update the loop information.
      // Note that we cannot do the check `info->Contains(loop_information_)->GetHeader()`
      // at this point, because this method is being called while populating `info`.
    }
  }

  // Raw update of the loop information.
  void SetLoopInformation(HLoopInformation* info) {
    loop_information_ = info;
  }

  bool IsInLoop() const { return loop_information_ != nullptr; }

  // Returns wheter this block dominates the blocked passed as parameter.
  bool Dominates(HBasicBlock* block) const;

  size_t GetLifetimeStart() const { return lifetime_start_; }
  size_t GetLifetimeEnd() const { return lifetime_end_; }

  void SetLifetimeStart(size_t start) { lifetime_start_ = start; }
  void SetLifetimeEnd(size_t end) { lifetime_end_ = end; }

  uint32_t GetDexPc() const { return dex_pc_; }

  bool IsCatchBlock() const { return is_catch_block_; }
  void SetIsCatchBlock() { is_catch_block_ = true; }

 private:
  HGraph* graph_;
  GrowableArray<HBasicBlock*> predecessors_;
  GrowableArray<HBasicBlock*> successors_;
  HInstructionList instructions_;
  HInstructionList phis_;
  HLoopInformation* loop_information_;
  HBasicBlock* dominator_;
  GrowableArray<HBasicBlock*> dominated_blocks_;
  int block_id_;
  // The dex program counter of the first instruction of this block.
  const uint32_t dex_pc_;
  size_t lifetime_start_;
  size_t lifetime_end_;
  bool is_catch_block_;

  friend class HGraph;
  friend class HInstruction;

  DISALLOW_COPY_AND_ASSIGN(HBasicBlock);
};

#define FOR_EACH_CONCRETE_INSTRUCTION(M)                                \
  M(Add, BinaryOperation)                                               \
  M(And, BinaryOperation)                                               \
  M(ArrayGet, Instruction)                                              \
  M(ArrayLength, Instruction)                                           \
  M(ArraySet, Instruction)                                              \
  M(BoundsCheck, Instruction)                                           \
  M(BoundType, Instruction)                                             \
  M(CheckCast, Instruction)                                             \
  M(ClinitCheck, Instruction)                                           \
  M(Compare, BinaryOperation)                                           \
  M(Condition, BinaryOperation)                                         \
  M(Div, BinaryOperation)                                               \
  M(DivZeroCheck, Instruction)                                          \
  M(DoubleConstant, Constant)                                           \
  M(Equal, Condition)                                                   \
  M(Exit, Instruction)                                                  \
  M(FloatConstant, Constant)                                            \
  M(Goto, Instruction)                                                  \
  M(GreaterThan, Condition)                                             \
  M(GreaterThanOrEqual, Condition)                                      \
  M(If, Instruction)                                                    \
  M(InstanceFieldGet, Instruction)                                      \
  M(InstanceFieldSet, Instruction)                                      \
  M(InstanceOf, Instruction)                                            \
  M(IntConstant, Constant)                                              \
  M(InvokeInterface, Invoke)                                            \
  M(InvokeStaticOrDirect, Invoke)                                       \
  M(InvokeVirtual, Invoke)                                              \
  M(LessThan, Condition)                                                \
  M(LessThanOrEqual, Condition)                                         \
  M(LoadClass, Instruction)                                             \
  M(LoadException, Instruction)                                         \
  M(LoadLocal, Instruction)                                             \
  M(LoadString, Instruction)                                            \
  M(Local, Instruction)                                                 \
  M(LongConstant, Constant)                                             \
  M(MonitorOperation, Instruction)                                      \
  M(Mul, BinaryOperation)                                               \
  M(Neg, UnaryOperation)                                                \
  M(NewArray, Instruction)                                              \
  M(NewInstance, Instruction)                                           \
  M(Not, UnaryOperation)                                                \
  M(NotEqual, Condition)                                                \
  M(NullCheck, Instruction)                                             \
  M(Or, BinaryOperation)                                                \
  M(ParallelMove, Instruction)                                          \
  M(ParameterValue, Instruction)                                        \
  M(Phi, Instruction)                                                   \
  M(Rem, BinaryOperation)                                               \
  M(Return, Instruction)                                                \
  M(ReturnVoid, Instruction)                                            \
  M(Shl, BinaryOperation)                                               \
  M(Shr, BinaryOperation)                                               \
  M(StaticFieldGet, Instruction)                                        \
  M(StaticFieldSet, Instruction)                                        \
  M(StoreLocal, Instruction)                                            \
  M(Sub, BinaryOperation)                                               \
  M(SuspendCheck, Instruction)                                          \
  M(Temporary, Instruction)                                             \
  M(Throw, Instruction)                                                 \
  M(TypeConversion, Instruction)                                        \
  M(UShr, BinaryOperation)                                              \
  M(Xor, BinaryOperation)                                               \

#define FOR_EACH_INSTRUCTION(M)                                         \
  FOR_EACH_CONCRETE_INSTRUCTION(M)                                      \
  M(Constant, Instruction)                                              \
  M(UnaryOperation, Instruction)                                        \
  M(BinaryOperation, Instruction)                                       \
  M(Invoke, Instruction)

#define FORWARD_DECLARATION(type, super) class H##type;
FOR_EACH_INSTRUCTION(FORWARD_DECLARATION)
#undef FORWARD_DECLARATION

#define DECLARE_INSTRUCTION(type)                                       \
  virtual InstructionKind GetKind() const { return k##type; }           \
  virtual const char* DebugName() const { return #type; }               \
  virtual const H##type* As##type() const OVERRIDE { return this; }     \
  virtual H##type* As##type() OVERRIDE { return this; }                 \
  virtual bool InstructionTypeEquals(HInstruction* other) const {       \
    return other->Is##type();                                           \
  }                                                                     \
  virtual void Accept(HGraphVisitor* visitor)

template <typename T> class HUseList;

template <typename T>
class HUseListNode : public ArenaObject<kArenaAllocMisc> {
 public:
  HUseListNode* GetPrevious() const { return prev_; }
  HUseListNode* GetNext() const { return next_; }
  T GetUser() const { return user_; }
  size_t GetIndex() const { return index_; }

 private:
  HUseListNode(T user, size_t index)
      : user_(user), index_(index), prev_(nullptr), next_(nullptr) {}

  T const user_;
  const size_t index_;
  HUseListNode<T>* prev_;
  HUseListNode<T>* next_;

  friend class HUseList<T>;

  DISALLOW_COPY_AND_ASSIGN(HUseListNode);
};

template <typename T>
class HUseList : public ValueObject {
 public:
  HUseList() : first_(nullptr) {}

  void Clear() {
    first_ = nullptr;
  }

  // Adds a new entry at the beginning of the use list and returns
  // the newly created node.
  HUseListNode<T>* AddUse(T user, size_t index, ArenaAllocator* arena) {
    HUseListNode<T>* new_node = new (arena) HUseListNode<T>(user, index);
    if (IsEmpty()) {
      first_ = new_node;
    } else {
      first_->prev_ = new_node;
      new_node->next_ = first_;
      first_ = new_node;
    }
    return new_node;
  }

  HUseListNode<T>* GetFirst() const {
    return first_;
  }

  void Remove(HUseListNode<T>* node) {
    if (node->prev_ != nullptr) {
      node->prev_->next_ = node->next_;
    }
    if (node->next_ != nullptr) {
      node->next_->prev_ = node->prev_;
    }
    if (node == first_) {
      first_ = node->next_;
    }
  }

  bool IsEmpty() const {
    return first_ == nullptr;
  }

  bool HasOnlyOneUse() const {
    return first_ != nullptr && first_->next_ == nullptr;
  }

 private:
  HUseListNode<T>* first_;
};

template<typename T>
class HUseIterator : public ValueObject {
 public:
  explicit HUseIterator(const HUseList<T>& uses) : current_(uses.GetFirst()) {}

  bool Done() const { return current_ == nullptr; }

  void Advance() {
    DCHECK(!Done());
    current_ = current_->GetNext();
  }

  HUseListNode<T>* Current() const {
    DCHECK(!Done());
    return current_;
  }

 private:
  HUseListNode<T>* current_;

  friend class HValue;
};

// Represents the side effects an instruction may have.
class SideEffects : public ValueObject {
 public:
  SideEffects() : flags_(0) {}

  static SideEffects None() {
    return SideEffects(0);
  }

  static SideEffects All() {
    return SideEffects(ChangesSomething().flags_ | DependsOnSomething().flags_);
  }

  static SideEffects ChangesSomething() {
    return SideEffects((1 << kFlagChangesCount) - 1);
  }

  static SideEffects DependsOnSomething() {
    int count = kFlagDependsOnCount - kFlagChangesCount;
    return SideEffects(((1 << count) - 1) << kFlagChangesCount);
  }

  SideEffects Union(SideEffects other) const {
    return SideEffects(flags_ | other.flags_);
  }

  bool HasSideEffects() const {
    size_t all_bits_set = (1 << kFlagChangesCount) - 1;
    return (flags_ & all_bits_set) != 0;
  }

  bool HasAllSideEffects() const {
    size_t all_bits_set = (1 << kFlagChangesCount) - 1;
    return all_bits_set == (flags_ & all_bits_set);
  }

  bool DependsOn(SideEffects other) const {
    size_t depends_flags = other.ComputeDependsFlags();
    return (flags_ & depends_flags) != 0;
  }

  bool HasDependencies() const {
    int count = kFlagDependsOnCount - kFlagChangesCount;
    size_t all_bits_set = (1 << count) - 1;
    return ((flags_ >> kFlagChangesCount) & all_bits_set) != 0;
  }

 private:
  static constexpr int kFlagChangesSomething = 0;
  static constexpr int kFlagChangesCount = kFlagChangesSomething + 1;

  static constexpr int kFlagDependsOnSomething = kFlagChangesCount;
  static constexpr int kFlagDependsOnCount = kFlagDependsOnSomething + 1;

  explicit SideEffects(size_t flags) : flags_(flags) {}

  size_t ComputeDependsFlags() const {
    return flags_ << kFlagChangesCount;
  }

  size_t flags_;
};

// A HEnvironment object contains the values of virtual registers at a given location.
class HEnvironment : public ArenaObject<kArenaAllocMisc> {
 public:
  HEnvironment(ArenaAllocator* arena, size_t number_of_vregs)
     : vregs_(arena, number_of_vregs) {
    vregs_.SetSize(number_of_vregs);
    for (size_t i = 0; i < number_of_vregs; i++) {
      vregs_.Put(i, VRegInfo(nullptr, nullptr));
    }
  }

  void CopyFrom(HEnvironment* env);

  void SetRawEnvAt(size_t index, HInstruction* instruction) {
    vregs_.Put(index, VRegInfo(instruction, nullptr));
  }

  // Record instructions' use entries of this environment for constant-time removal.
  void RecordEnvUse(HUseListNode<HEnvironment*>* env_use) {
    DCHECK(env_use->GetUser() == this);
    size_t index = env_use->GetIndex();
    VRegInfo info = vregs_.Get(index);
    DCHECK(info.vreg_ != nullptr);
    DCHECK(info.node_ == nullptr);
    vregs_.Put(index, VRegInfo(info.vreg_, env_use));
  }

  HInstruction* GetInstructionAt(size_t index) const {
    return vregs_.Get(index).vreg_;
  }

  HUseListNode<HEnvironment*>* GetInstructionEnvUseAt(size_t index) const {
    return vregs_.Get(index).node_;
  }

  size_t Size() const { return vregs_.Size(); }

 private:
  struct VRegInfo {
    HInstruction* vreg_;
    HUseListNode<HEnvironment*>* node_;

    VRegInfo(HInstruction* instruction, HUseListNode<HEnvironment*>* env_use)
        : vreg_(instruction), node_(env_use) {}
  };

  GrowableArray<VRegInfo> vregs_;

  DISALLOW_COPY_AND_ASSIGN(HEnvironment);
};

class ReferenceTypeInfo : ValueObject {
 public:
  typedef Handle<mirror::Class> TypeHandle;

  ReferenceTypeInfo() : is_exact_(false), is_top_(true) {}
  ReferenceTypeInfo(Handle<mirror::Class> type_handle, bool is_exact) {
    SetTypeHandle(type_handle, is_exact);
  }

  bool IsExact() const { return is_exact_; }
  bool IsTop() const { return is_top_; }

  Handle<mirror::Class> GetTypeHandle() const { return type_handle_; }

  void SetTop(bool is_exact) {
    is_exact_ = is_exact;
    is_top_ = true;
    type_handle_ = TypeHandle();
  }

  void SetInexact() { is_exact_ = false; }
  void SetTypeHandle(Handle<mirror::Class> type_handle, bool is_exact)
      SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    type_handle_ = type_handle;
    is_exact_ =  is_exact;
    if (type_handle_->IsObjectClass()) {
      is_top_ = true;
      // Override the type handle to be consistent with the case when we get to
      // Top but don't have the Object class available. It avoids having to guess
      // what value the type_handle has when it's Top.
      type_handle_ = Handle<mirror::Class>();
    } else {
      is_top_ = false;
    }
  }

  bool IsSupertypeOf(ReferenceTypeInfo rti) const SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (IsTop()) {
      // Top (equivalent for java.lang.Object) is supertype of anything.
      return true;
    }
    if (rti.IsTop()) {
      // If we get here `this` is not Top() so it can't be a supertype.
      return false;
    }
    return GetTypeHandle()->IsAssignableFrom(rti.GetTypeHandle().Get());
  }

  // Returns true if the type information provide the same amount of details.
  // Note that it does not mean that the instructions have the same actual type
  // (e.g. tops are equal but they can be the result of a merge).
  bool IsEqual(ReferenceTypeInfo rti) SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
    if (IsExact() != rti.IsExact()) {
      return false;
    }
    if (IsTop() && rti.IsTop()) {
      // `Top` means java.lang.Object, so the types are equivalent.
      return true;
    }
    if (IsTop() ^ rti.IsTop()) {
      // If only one is top or object than they are not equivalent.
      // NB: We need this extra check because the type_handle of `Top` is invalid
      // and we cannot inspect its reference.
      return false;
    }

    // Finally check the types.
    return GetTypeHandle().Get() == rti.GetTypeHandle().Get();
  }

 private:
  // The class of the object.
  TypeHandle type_handle_;
  // Whether or not the type is exact or a superclass of the actual type.
  // Whether or not we have any information about this type.
  bool is_exact_;
  // A true value here means that the object type should be java.lang.Object.
  // We don't have access to the corresponding mirror object every time so this
  // flag acts as a substitute. When true, the TypeHandle refers to a null
  // pointer and should not be used.
  bool is_top_;
};

std::ostream& operator<<(std::ostream& os, const ReferenceTypeInfo& rhs);

class HInstruction : public ArenaObject<kArenaAllocMisc> {
 public:
  explicit HInstruction(SideEffects side_effects)
      : previous_(nullptr),
        next_(nullptr),
        block_(nullptr),
        id_(-1),
        ssa_index_(-1),
        environment_(nullptr),
        locations_(nullptr),
        live_interval_(nullptr),
        lifetime_position_(kNoLifetime),
        side_effects_(side_effects) {}

  virtual ~HInstruction() {}

#define DECLARE_KIND(type, super) k##type,
  enum InstructionKind {
    FOR_EACH_INSTRUCTION(DECLARE_KIND)
  };
#undef DECLARE_KIND

  HInstruction* GetNext() const { return next_; }
  HInstruction* GetPrevious() const { return previous_; }

  HInstruction* GetNextDisregardingMoves() const;
  HInstruction* GetPreviousDisregardingMoves() const;

  HBasicBlock* GetBlock() const { return block_; }
  void SetBlock(HBasicBlock* block) { block_ = block; }
  bool IsInBlock() const { return block_ != nullptr; }
  bool IsInLoop() const { return block_->IsInLoop(); }
  bool IsLoopHeaderPhi() { return IsPhi() && block_->IsLoopHeader(); }

  virtual size_t InputCount() const = 0;
  virtual HInstruction* InputAt(size_t i) const = 0;

  virtual void Accept(HGraphVisitor* visitor) = 0;
  virtual const char* DebugName() const = 0;

  virtual Primitive::Type GetType() const { return Primitive::kPrimVoid; }
  virtual void SetRawInputAt(size_t index, HInstruction* input) = 0;

  virtual bool NeedsEnvironment() const { return false; }
  virtual bool IsControlFlow() const { return false; }
  virtual bool CanThrow() const { return false; }
  bool HasSideEffects() const { return side_effects_.HasSideEffects(); }

  // Does not apply for all instructions, but having this at top level greatly
  // simplifies the null check elimination.
  virtual bool CanBeNull() const { return true; }

  virtual bool CanDoImplicitNullCheck() const { return false; }

  void SetReferenceTypeInfo(ReferenceTypeInfo reference_type_info) {
    reference_type_info_ = reference_type_info;
  }

  ReferenceTypeInfo GetReferenceTypeInfo() const { return reference_type_info_; }

  void AddUseAt(HInstruction* user, size_t index) {
    uses_.AddUse(user, index, GetBlock()->GetGraph()->GetArena());
  }

  void AddEnvUseAt(HEnvironment* user, size_t index) {
    DCHECK(user != nullptr);
    HUseListNode<HEnvironment*>* env_use =
        env_uses_.AddUse(user, index, GetBlock()->GetGraph()->GetArena());
    user->RecordEnvUse(env_use);
  }

  void RemoveUser(HInstruction* user, size_t index);
  void RemoveEnvironmentUser(HUseListNode<HEnvironment*>* use);

  const HUseList<HInstruction*>& GetUses() { return uses_; }
  const HUseList<HEnvironment*>& GetEnvUses() { return env_uses_; }

  bool HasUses() const { return !uses_.IsEmpty() || !env_uses_.IsEmpty(); }
  bool HasEnvironmentUses() const { return !env_uses_.IsEmpty(); }

  // Does this instruction strictly dominate `other_instruction`?
  // Returns false if this instruction and `other_instruction` are the same.
  // Aborts if this instruction and `other_instruction` are both phis.
  bool StrictlyDominates(HInstruction* other_instruction) const;

  int GetId() const { return id_; }
  void SetId(int id) { id_ = id; }

  int GetSsaIndex() const { return ssa_index_; }
  void SetSsaIndex(int ssa_index) { ssa_index_ = ssa_index; }
  bool HasSsaIndex() const { return ssa_index_ != -1; }

  bool HasEnvironment() const { return environment_ != nullptr; }
  HEnvironment* GetEnvironment() const { return environment_; }
  void SetEnvironment(HEnvironment* environment) { environment_ = environment; }

  // Returns the number of entries in the environment. Typically, that is the
  // number of dex registers in a method. It could be more in case of inlining.
  size_t EnvironmentSize() const;

  LocationSummary* GetLocations() const { return locations_; }
  void SetLocations(LocationSummary* locations) { locations_ = locations; }

  void ReplaceWith(HInstruction* instruction);
  void ReplaceInput(HInstruction* replacement, size_t index);

  // Move `this` instruction before `cursor`.
  void MoveBefore(HInstruction* cursor);

#define INSTRUCTION_TYPE_CHECK(type, super)                                    \
  bool Is##type() const { return (As##type() != nullptr); }                    \
  virtual const H##type* As##type() const { return nullptr; }                  \
  virtual H##type* As##type() { return nullptr; }

  FOR_EACH_INSTRUCTION(INSTRUCTION_TYPE_CHECK)
#undef INSTRUCTION_TYPE_CHECK

  // Returns whether the instruction can be moved within the graph.
  virtual bool CanBeMoved() const { return false; }

  // Returns whether the two instructions are of the same kind.
  virtual bool InstructionTypeEquals(HInstruction* other) const {
    UNUSED(other);
    return false;
  }

  // Returns whether any data encoded in the two instructions is equal.
  // This method does not look at the inputs. Both instructions must be
  // of the same type, otherwise the method has undefined behavior.
  virtual bool InstructionDataEquals(HInstruction* other) const {
    UNUSED(other);
    return false;
  }

  // Returns whether two instructions are equal, that is:
  // 1) They have the same type and contain the same data (InstructionDataEquals).
  // 2) Their inputs are identical.
  bool Equals(HInstruction* other) const;

  virtual InstructionKind GetKind() const = 0;

  virtual size_t ComputeHashCode() const {
    size_t result = GetKind();
    for (size_t i = 0, e = InputCount(); i < e; ++i) {
      result = (result * 31) + InputAt(i)->GetId();
    }
    return result;
  }

  SideEffects GetSideEffects() const { return side_effects_; }

  size_t GetLifetimePosition() const { return lifetime_position_; }
  void SetLifetimePosition(size_t position) { lifetime_position_ = position; }
  LiveInterval* GetLiveInterval() const { return live_interval_; }
  void SetLiveInterval(LiveInterval* interval) { live_interval_ = interval; }
  bool HasLiveInterval() const { return live_interval_ != nullptr; }

  bool IsSuspendCheckEntry() const { return IsSuspendCheck() && GetBlock()->IsEntryBlock(); }

  // Returns whether the code generation of the instruction will require to have access
  // to the current method. Such instructions are:
  // (1): Instructions that require an environment, as calling the runtime requires
  //      to walk the stack and have the current method stored at a specific stack address.
  // (2): Object literals like classes and strings, that are loaded from the dex cache
  //      fields of the current method.
  bool NeedsCurrentMethod() const {
    return NeedsEnvironment() || IsLoadClass() || IsLoadString();
  }

 private:
  HInstruction* previous_;
  HInstruction* next_;
  HBasicBlock* block_;

  // An instruction gets an id when it is added to the graph.
  // It reflects creation order. A negative id means the instruction
  // has not been added to the graph.
  int id_;

  // When doing liveness analysis, instructions that have uses get an SSA index.
  int ssa_index_;

  // List of instructions that have this instruction as input.
  HUseList<HInstruction*> uses_;

  // List of environments that contain this instruction.
  HUseList<HEnvironment*> env_uses_;

  // The environment associated with this instruction. Not null if the instruction
  // might jump out of the method.
  HEnvironment* environment_;

  // Set by the code generator.
  LocationSummary* locations_;

  // Set by the liveness analysis.
  LiveInterval* live_interval_;

  // Set by the liveness analysis, this is the position in a linear
  // order of blocks where this instruction's live interval start.
  size_t lifetime_position_;

  const SideEffects side_effects_;

  // TODO: for primitive types this should be marked as invalid.
  ReferenceTypeInfo reference_type_info_;

  friend class HBasicBlock;
  friend class HGraph;
  friend class HInstructionList;

  DISALLOW_COPY_AND_ASSIGN(HInstruction);
};
std::ostream& operator<<(std::ostream& os, const HInstruction::InstructionKind& rhs);

class HInputIterator : public ValueObject {
 public:
  explicit HInputIterator(HInstruction* instruction) : instruction_(instruction), index_(0) {}

  bool Done() const { return index_ == instruction_->InputCount(); }
  HInstruction* Current() const { return instruction_->InputAt(index_); }
  void Advance() { index_++; }

 private:
  HInstruction* instruction_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HInputIterator);
};

class HInstructionIterator : public ValueObject {
 public:
  explicit HInstructionIterator(const HInstructionList& instructions)
      : instruction_(instructions.first_instruction_) {
    next_ = Done() ? nullptr : instruction_->GetNext();
  }

  bool Done() const { return instruction_ == nullptr; }
  HInstruction* Current() const { return instruction_; }
  void Advance() {
    instruction_ = next_;
    next_ = Done() ? nullptr : instruction_->GetNext();
  }

 private:
  HInstruction* instruction_;
  HInstruction* next_;

  DISALLOW_COPY_AND_ASSIGN(HInstructionIterator);
};

class HBackwardInstructionIterator : public ValueObject {
 public:
  explicit HBackwardInstructionIterator(const HInstructionList& instructions)
      : instruction_(instructions.last_instruction_) {
    next_ = Done() ? nullptr : instruction_->GetPrevious();
  }

  bool Done() const { return instruction_ == nullptr; }
  HInstruction* Current() const { return instruction_; }
  void Advance() {
    instruction_ = next_;
    next_ = Done() ? nullptr : instruction_->GetPrevious();
  }

 private:
  HInstruction* instruction_;
  HInstruction* next_;

  DISALLOW_COPY_AND_ASSIGN(HBackwardInstructionIterator);
};

// An embedded container with N elements of type T.  Used (with partial
// specialization for N=0) because embedded arrays cannot have size 0.
template<typename T, intptr_t N>
class EmbeddedArray {
 public:
  EmbeddedArray() : elements_() {}

  intptr_t GetLength() const { return N; }

  const T& operator[](intptr_t i) const {
    DCHECK_LT(i, GetLength());
    return elements_[i];
  }

  T& operator[](intptr_t i) {
    DCHECK_LT(i, GetLength());
    return elements_[i];
  }

  const T& At(intptr_t i) const {
    return (*this)[i];
  }

  void SetAt(intptr_t i, const T& val) {
    (*this)[i] = val;
  }

 private:
  T elements_[N];
};

template<typename T>
class EmbeddedArray<T, 0> {
 public:
  intptr_t length() const { return 0; }
  const T& operator[](intptr_t i) const {
    UNUSED(i);
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  }
  T& operator[](intptr_t i) {
    UNUSED(i);
    LOG(FATAL) << "Unreachable";
    UNREACHABLE();
  }
};

template<intptr_t N>
class HTemplateInstruction: public HInstruction {
 public:
  HTemplateInstruction<N>(SideEffects side_effects)
      : HInstruction(side_effects), inputs_() {}
  virtual ~HTemplateInstruction() {}

  virtual size_t InputCount() const { return N; }
  virtual HInstruction* InputAt(size_t i) const { return inputs_[i]; }

 protected:
  virtual void SetRawInputAt(size_t i, HInstruction* instruction) {
    inputs_[i] = instruction;
  }

 private:
  EmbeddedArray<HInstruction*, N> inputs_;

  friend class SsaBuilder;
};

template<intptr_t N>
class HExpression : public HTemplateInstruction<N> {
 public:
  HExpression<N>(Primitive::Type type, SideEffects side_effects)
      : HTemplateInstruction<N>(side_effects), type_(type) {}
  virtual ~HExpression() {}

  virtual Primitive::Type GetType() const { return type_; }

 protected:
  Primitive::Type type_;
};

// Represents dex's RETURN_VOID opcode. A HReturnVoid is a control flow
// instruction that branches to the exit block.
class HReturnVoid : public HTemplateInstruction<0> {
 public:
  HReturnVoid() : HTemplateInstruction(SideEffects::None()) {}

  virtual bool IsControlFlow() const { return true; }

  DECLARE_INSTRUCTION(ReturnVoid);

 private:
  DISALLOW_COPY_AND_ASSIGN(HReturnVoid);
};

// Represents dex's RETURN opcodes. A HReturn is a control flow
// instruction that branches to the exit block.
class HReturn : public HTemplateInstruction<1> {
 public:
  explicit HReturn(HInstruction* value) : HTemplateInstruction(SideEffects::None()) {
    SetRawInputAt(0, value);
  }

  virtual bool IsControlFlow() const { return true; }

  DECLARE_INSTRUCTION(Return);

 private:
  DISALLOW_COPY_AND_ASSIGN(HReturn);
};

// The exit instruction is the only instruction of the exit block.
// Instructions aborting the method (HThrow and HReturn) must branch to the
// exit block.
class HExit : public HTemplateInstruction<0> {
 public:
  HExit() : HTemplateInstruction(SideEffects::None()) {}

  virtual bool IsControlFlow() const { return true; }

  DECLARE_INSTRUCTION(Exit);

 private:
  DISALLOW_COPY_AND_ASSIGN(HExit);
};

// Jumps from one block to another.
class HGoto : public HTemplateInstruction<0> {
 public:
  HGoto() : HTemplateInstruction(SideEffects::None()) {}

  bool IsControlFlow() const OVERRIDE { return true; }

  HBasicBlock* GetSuccessor() const {
    return GetBlock()->GetSuccessors().Get(0);
  }

  DECLARE_INSTRUCTION(Goto);

 private:
  DISALLOW_COPY_AND_ASSIGN(HGoto);
};


// Conditional branch. A block ending with an HIf instruction must have
// two successors.
class HIf : public HTemplateInstruction<1> {
 public:
  explicit HIf(HInstruction* input) : HTemplateInstruction(SideEffects::None()) {
    SetRawInputAt(0, input);
  }

  bool IsControlFlow() const OVERRIDE { return true; }

  HBasicBlock* IfTrueSuccessor() const {
    return GetBlock()->GetSuccessors().Get(0);
  }

  HBasicBlock* IfFalseSuccessor() const {
    return GetBlock()->GetSuccessors().Get(1);
  }

  DECLARE_INSTRUCTION(If);

  virtual bool IsIfInstruction() const { return true; }

 private:
  DISALLOW_COPY_AND_ASSIGN(HIf);
};

class HUnaryOperation : public HExpression<1> {
 public:
  HUnaryOperation(Primitive::Type result_type, HInstruction* input)
      : HExpression(result_type, SideEffects::None()) {
    SetRawInputAt(0, input);
  }

  HInstruction* GetInput() const { return InputAt(0); }
  Primitive::Type GetResultType() const { return GetType(); }

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const {
    UNUSED(other);
    return true;
  }

  // Try to statically evaluate `operation` and return a HConstant
  // containing the result of this evaluation.  If `operation` cannot
  // be evaluated as a constant, return nullptr.
  HConstant* TryStaticEvaluation() const;

  // Apply this operation to `x`.
  virtual int32_t Evaluate(int32_t x) const = 0;
  virtual int64_t Evaluate(int64_t x) const = 0;

  DECLARE_INSTRUCTION(UnaryOperation);

 private:
  DISALLOW_COPY_AND_ASSIGN(HUnaryOperation);
};

class HBinaryOperation : public HExpression<2> {
 public:
  HBinaryOperation(Primitive::Type result_type,
                   HInstruction* left,
                   HInstruction* right) : HExpression(result_type, SideEffects::None()) {
    SetRawInputAt(0, left);
    SetRawInputAt(1, right);
  }

  HInstruction* GetLeft() const { return InputAt(0); }
  HInstruction* GetRight() const { return InputAt(1); }
  Primitive::Type GetResultType() const { return GetType(); }

  virtual bool IsCommutative() { return false; }

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const {
    UNUSED(other);
    return true;
  }

  // Try to statically evaluate `operation` and return a HConstant
  // containing the result of this evaluation.  If `operation` cannot
  // be evaluated as a constant, return nullptr.
  HConstant* TryStaticEvaluation() const;

  // Apply this operation to `x` and `y`.
  virtual int32_t Evaluate(int32_t x, int32_t y) const = 0;
  virtual int64_t Evaluate(int64_t x, int64_t y) const = 0;

  DECLARE_INSTRUCTION(BinaryOperation);

 private:
  DISALLOW_COPY_AND_ASSIGN(HBinaryOperation);
};

class HCondition : public HBinaryOperation {
 public:
  HCondition(HInstruction* first, HInstruction* second)
      : HBinaryOperation(Primitive::kPrimBoolean, first, second),
        needs_materialization_(true) {}

  virtual bool IsCommutative() { return true; }

  bool NeedsMaterialization() const { return needs_materialization_; }
  void ClearNeedsMaterialization() { needs_materialization_ = false; }

  // For code generation purposes, returns whether this instruction is just before
  // `if_`, and disregard moves in between.
  bool IsBeforeWhenDisregardMoves(HIf* if_) const;

  DECLARE_INSTRUCTION(Condition);

  virtual IfCondition GetCondition() const = 0;

 private:
  // For register allocation purposes, returns whether this instruction needs to be
  // materialized (that is, not just be in the processor flags).
  bool needs_materialization_;

  DISALLOW_COPY_AND_ASSIGN(HCondition);
};

// Instruction to check if two inputs are equal to each other.
class HEqual : public HCondition {
 public:
  HEqual(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x == y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x == y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(Equal);

  virtual IfCondition GetCondition() const {
    return kCondEQ;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HEqual);
};

class HNotEqual : public HCondition {
 public:
  HNotEqual(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x != y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x != y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(NotEqual);

  virtual IfCondition GetCondition() const {
    return kCondNE;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HNotEqual);
};

class HLessThan : public HCondition {
 public:
  HLessThan(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x < y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x < y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(LessThan);

  virtual IfCondition GetCondition() const {
    return kCondLT;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HLessThan);
};

class HLessThanOrEqual : public HCondition {
 public:
  HLessThanOrEqual(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x <= y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x <= y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(LessThanOrEqual);

  virtual IfCondition GetCondition() const {
    return kCondLE;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HLessThanOrEqual);
};

class HGreaterThan : public HCondition {
 public:
  HGreaterThan(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x > y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x > y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(GreaterThan);

  virtual IfCondition GetCondition() const {
    return kCondGT;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HGreaterThan);
};

class HGreaterThanOrEqual : public HCondition {
 public:
  HGreaterThanOrEqual(HInstruction* first, HInstruction* second)
      : HCondition(first, second) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x >= y ? 1 : 0;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x >= y ? 1 : 0;
  }

  DECLARE_INSTRUCTION(GreaterThanOrEqual);

  virtual IfCondition GetCondition() const {
    return kCondGE;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(HGreaterThanOrEqual);
};


// Instruction to check how two inputs compare to each other.
// Result is 0 if input0 == input1, 1 if input0 > input1, or -1 if input0 < input1.
class HCompare : public HBinaryOperation {
 public:
  // The bias applies for floating point operations and indicates how NaN
  // comparisons are treated:
  enum Bias {
    kNoBias,  // bias is not applicable (i.e. for long operation)
    kGtBias,  // return 1 for NaN comparisons
    kLtBias,  // return -1 for NaN comparisons
  };

  HCompare(Primitive::Type type, HInstruction* first, HInstruction* second, Bias bias)
      : HBinaryOperation(Primitive::kPrimInt, first, second), bias_(bias) {
    DCHECK_EQ(type, first->GetType());
    DCHECK_EQ(type, second->GetType());
  }

  int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return
      x == y ? 0 :
      x > y ? 1 :
      -1;
  }

  int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return
      x == y ? 0 :
      x > y ? 1 :
      -1;
  }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    return bias_ == other->AsCompare()->bias_;
  }

  bool IsGtBias() { return bias_ == kGtBias; }

  DECLARE_INSTRUCTION(Compare);

 private:
  const Bias bias_;

  DISALLOW_COPY_AND_ASSIGN(HCompare);
};

// A local in the graph. Corresponds to a Dex register.
class HLocal : public HTemplateInstruction<0> {
 public:
  explicit HLocal(uint16_t reg_number)
      : HTemplateInstruction(SideEffects::None()), reg_number_(reg_number) {}

  DECLARE_INSTRUCTION(Local);

  uint16_t GetRegNumber() const { return reg_number_; }

 private:
  // The Dex register number.
  const uint16_t reg_number_;

  DISALLOW_COPY_AND_ASSIGN(HLocal);
};

// Load a given local. The local is an input of this instruction.
class HLoadLocal : public HExpression<1> {
 public:
  HLoadLocal(HLocal* local, Primitive::Type type)
      : HExpression(type, SideEffects::None()) {
    SetRawInputAt(0, local);
  }

  HLocal* GetLocal() const { return reinterpret_cast<HLocal*>(InputAt(0)); }

  DECLARE_INSTRUCTION(LoadLocal);

 private:
  DISALLOW_COPY_AND_ASSIGN(HLoadLocal);
};

// Store a value in a given local. This instruction has two inputs: the value
// and the local.
class HStoreLocal : public HTemplateInstruction<2> {
 public:
  HStoreLocal(HLocal* local, HInstruction* value) : HTemplateInstruction(SideEffects::None()) {
    SetRawInputAt(0, local);
    SetRawInputAt(1, value);
  }

  HLocal* GetLocal() const { return reinterpret_cast<HLocal*>(InputAt(0)); }

  DECLARE_INSTRUCTION(StoreLocal);

 private:
  DISALLOW_COPY_AND_ASSIGN(HStoreLocal);
};

class HConstant : public HExpression<0> {
 public:
  explicit HConstant(Primitive::Type type) : HExpression(type, SideEffects::None()) {}

  virtual bool CanBeMoved() const { return true; }

  DECLARE_INSTRUCTION(Constant);

 private:
  DISALLOW_COPY_AND_ASSIGN(HConstant);
};

class HFloatConstant : public HConstant {
 public:
  explicit HFloatConstant(float value) : HConstant(Primitive::kPrimFloat), value_(value) {}

  float GetValue() const { return value_; }

  virtual bool InstructionDataEquals(HInstruction* other) const {
    return bit_cast<float, int32_t>(other->AsFloatConstant()->value_) ==
        bit_cast<float, int32_t>(value_);
  }

  virtual size_t ComputeHashCode() const { return static_cast<size_t>(GetValue()); }

  DECLARE_INSTRUCTION(FloatConstant);

 private:
  const float value_;

  DISALLOW_COPY_AND_ASSIGN(HFloatConstant);
};

class HDoubleConstant : public HConstant {
 public:
  explicit HDoubleConstant(double value) : HConstant(Primitive::kPrimDouble), value_(value) {}

  double GetValue() const { return value_; }

  virtual bool InstructionDataEquals(HInstruction* other) const {
    return bit_cast<double, int64_t>(other->AsDoubleConstant()->value_) ==
        bit_cast<double, int64_t>(value_);
  }

  virtual size_t ComputeHashCode() const { return static_cast<size_t>(GetValue()); }

  DECLARE_INSTRUCTION(DoubleConstant);

 private:
  const double value_;

  DISALLOW_COPY_AND_ASSIGN(HDoubleConstant);
};

// Constants of the type int. Those can be from Dex instructions, or
// synthesized (for example with the if-eqz instruction).
class HIntConstant : public HConstant {
 public:
  explicit HIntConstant(int32_t value) : HConstant(Primitive::kPrimInt), value_(value) {}

  int32_t GetValue() const { return value_; }

  virtual bool InstructionDataEquals(HInstruction* other) const {
    return other->AsIntConstant()->value_ == value_;
  }

  virtual size_t ComputeHashCode() const { return GetValue(); }

  DECLARE_INSTRUCTION(IntConstant);

 private:
  const int32_t value_;

  DISALLOW_COPY_AND_ASSIGN(HIntConstant);
};

class HLongConstant : public HConstant {
 public:
  explicit HLongConstant(int64_t value) : HConstant(Primitive::kPrimLong), value_(value) {}

  int64_t GetValue() const { return value_; }

  virtual bool InstructionDataEquals(HInstruction* other) const {
    return other->AsLongConstant()->value_ == value_;
  }

  virtual size_t ComputeHashCode() const { return static_cast<size_t>(GetValue()); }

  DECLARE_INSTRUCTION(LongConstant);

 private:
  const int64_t value_;

  DISALLOW_COPY_AND_ASSIGN(HLongConstant);
};

enum class Intrinsics {
#define OPTIMIZING_INTRINSICS(Name, IsStatic) k ## Name,
#include "intrinsics_list.h"
  kNone,
  INTRINSICS_LIST(OPTIMIZING_INTRINSICS)
#undef INTRINSICS_LIST
#undef OPTIMIZING_INTRINSICS
};
std::ostream& operator<<(std::ostream& os, const Intrinsics& intrinsic);

class HInvoke : public HInstruction {
 public:
  virtual size_t InputCount() const { return inputs_.Size(); }
  virtual HInstruction* InputAt(size_t i) const { return inputs_.Get(i); }

  // Runtime needs to walk the stack, so Dex -> Dex calls need to
  // know their environment.
  bool NeedsEnvironment() const OVERRIDE { return true; }

  void SetArgumentAt(size_t index, HInstruction* argument) {
    SetRawInputAt(index, argument);
  }

  virtual void SetRawInputAt(size_t index, HInstruction* input) {
    inputs_.Put(index, input);
  }

  virtual Primitive::Type GetType() const { return return_type_; }

  uint32_t GetDexPc() const { return dex_pc_; }

  uint32_t GetDexMethodIndex() const { return dex_method_index_; }

  Intrinsics GetIntrinsic() {
    return intrinsic_;
  }

  void SetIntrinsic(Intrinsics intrinsic) {
    intrinsic_ = intrinsic;
  }

  DECLARE_INSTRUCTION(Invoke);

 protected:
  HInvoke(ArenaAllocator* arena,
          uint32_t number_of_arguments,
          Primitive::Type return_type,
          uint32_t dex_pc,
          uint32_t dex_method_index)
    : HInstruction(SideEffects::All()),
      inputs_(arena, number_of_arguments),
      return_type_(return_type),
      dex_pc_(dex_pc),
      dex_method_index_(dex_method_index),
      intrinsic_(Intrinsics::kNone) {
    inputs_.SetSize(number_of_arguments);
  }

  GrowableArray<HInstruction*> inputs_;
  const Primitive::Type return_type_;
  const uint32_t dex_pc_;
  const uint32_t dex_method_index_;
  Intrinsics intrinsic_;

 private:
  DISALLOW_COPY_AND_ASSIGN(HInvoke);
};

class HInvokeStaticOrDirect : public HInvoke {
 public:
  HInvokeStaticOrDirect(ArenaAllocator* arena,
                        uint32_t number_of_arguments,
                        Primitive::Type return_type,
                        uint32_t dex_pc,
                        uint32_t dex_method_index,
                        bool is_recursive,
                        InvokeType invoke_type)
      : HInvoke(arena, number_of_arguments, return_type, dex_pc, dex_method_index),
        invoke_type_(invoke_type),
        is_recursive_(is_recursive) {}

  bool CanDoImplicitNullCheck() const OVERRIDE {
    // We access the method via the dex cache so we can't do an implicit null check.
    // TODO: for intrinsics we can generate implicit null checks.
    return false;
  }

  InvokeType GetInvokeType() const { return invoke_type_; }
  bool IsRecursive() const { return is_recursive_; }

  DECLARE_INSTRUCTION(InvokeStaticOrDirect);

 private:
  const InvokeType invoke_type_;
  const bool is_recursive_;

  DISALLOW_COPY_AND_ASSIGN(HInvokeStaticOrDirect);
};

class HInvokeVirtual : public HInvoke {
 public:
  HInvokeVirtual(ArenaAllocator* arena,
                 uint32_t number_of_arguments,
                 Primitive::Type return_type,
                 uint32_t dex_pc,
                 uint32_t dex_method_index,
                 uint32_t vtable_index)
      : HInvoke(arena, number_of_arguments, return_type, dex_pc, dex_method_index),
        vtable_index_(vtable_index) {}

  bool CanDoImplicitNullCheck() const OVERRIDE {
    // TODO: Add implicit null checks in intrinsics.
    return !GetLocations()->Intrinsified();
  }

  uint32_t GetVTableIndex() const { return vtable_index_; }

  DECLARE_INSTRUCTION(InvokeVirtual);

 private:
  const uint32_t vtable_index_;

  DISALLOW_COPY_AND_ASSIGN(HInvokeVirtual);
};

class HInvokeInterface : public HInvoke {
 public:
  HInvokeInterface(ArenaAllocator* arena,
                   uint32_t number_of_arguments,
                   Primitive::Type return_type,
                   uint32_t dex_pc,
                   uint32_t dex_method_index,
                   uint32_t imt_index)
      : HInvoke(arena, number_of_arguments, return_type, dex_pc, dex_method_index),
        imt_index_(imt_index) {}

  bool CanDoImplicitNullCheck() const OVERRIDE {
    // TODO: Add implicit null checks in intrinsics.
    return !GetLocations()->Intrinsified();
  }

  uint32_t GetImtIndex() const { return imt_index_; }
  uint32_t GetDexMethodIndex() const { return dex_method_index_; }

  DECLARE_INSTRUCTION(InvokeInterface);

 private:
  const uint32_t imt_index_;

  DISALLOW_COPY_AND_ASSIGN(HInvokeInterface);
};

class HNewInstance : public HExpression<0> {
 public:
  HNewInstance(uint32_t dex_pc, uint16_t type_index, QuickEntrypointEnum entrypoint)
      : HExpression(Primitive::kPrimNot, SideEffects::None()),
        dex_pc_(dex_pc),
        type_index_(type_index),
        entrypoint_(entrypoint) {}

  uint32_t GetDexPc() const { return dex_pc_; }
  uint16_t GetTypeIndex() const { return type_index_; }

  // Calls runtime so needs an environment.
  bool NeedsEnvironment() const OVERRIDE { return true; }
  // It may throw when called on:
  //   - interfaces
  //   - abstract/innaccessible/unknown classes
  // TODO: optimize when possible.
  bool CanThrow() const OVERRIDE { return true; }

  bool CanBeNull() const OVERRIDE { return false; }

  QuickEntrypointEnum GetEntrypoint() const { return entrypoint_; }

  DECLARE_INSTRUCTION(NewInstance);

 private:
  const uint32_t dex_pc_;
  const uint16_t type_index_;
  const QuickEntrypointEnum entrypoint_;

  DISALLOW_COPY_AND_ASSIGN(HNewInstance);
};

class HNeg : public HUnaryOperation {
 public:
  explicit HNeg(Primitive::Type result_type, HInstruction* input)
      : HUnaryOperation(result_type, input) {}

  virtual int32_t Evaluate(int32_t x) const OVERRIDE { return -x; }
  virtual int64_t Evaluate(int64_t x) const OVERRIDE { return -x; }

  DECLARE_INSTRUCTION(Neg);

 private:
  DISALLOW_COPY_AND_ASSIGN(HNeg);
};

class HNewArray : public HExpression<1> {
 public:
  HNewArray(HInstruction* length,
            uint32_t dex_pc,
            uint16_t type_index,
            QuickEntrypointEnum entrypoint)
      : HExpression(Primitive::kPrimNot, SideEffects::None()),
        dex_pc_(dex_pc),
        type_index_(type_index),
        entrypoint_(entrypoint) {
    SetRawInputAt(0, length);
  }

  uint32_t GetDexPc() const { return dex_pc_; }
  uint16_t GetTypeIndex() const { return type_index_; }

  // Calls runtime so needs an environment.
  bool NeedsEnvironment() const OVERRIDE { return true; }

  bool CanBeNull() const OVERRIDE { return false; }

  QuickEntrypointEnum GetEntrypoint() const { return entrypoint_; }

  DECLARE_INSTRUCTION(NewArray);

 private:
  const uint32_t dex_pc_;
  const uint16_t type_index_;
  const QuickEntrypointEnum entrypoint_;

  DISALLOW_COPY_AND_ASSIGN(HNewArray);
};

class HAdd : public HBinaryOperation {
 public:
  HAdd(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  virtual bool IsCommutative() { return true; }

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x + y;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x + y;
  }

  DECLARE_INSTRUCTION(Add);

 private:
  DISALLOW_COPY_AND_ASSIGN(HAdd);
};

class HSub : public HBinaryOperation {
 public:
  HSub(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    return x - y;
  }
  virtual int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    return x - y;
  }

  DECLARE_INSTRUCTION(Sub);

 private:
  DISALLOW_COPY_AND_ASSIGN(HSub);
};

class HMul : public HBinaryOperation {
 public:
  HMul(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  virtual bool IsCommutative() { return true; }

  virtual int32_t Evaluate(int32_t x, int32_t y) const { return x * y; }
  virtual int64_t Evaluate(int64_t x, int64_t y) const { return x * y; }

  DECLARE_INSTRUCTION(Mul);

 private:
  DISALLOW_COPY_AND_ASSIGN(HMul);
};

class HDiv : public HBinaryOperation {
 public:
  HDiv(Primitive::Type result_type, HInstruction* left, HInstruction* right, uint32_t dex_pc)
      : HBinaryOperation(result_type, left, right), dex_pc_(dex_pc) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const {
    // Our graph structure ensures we never have 0 for `y` during constant folding.
    DCHECK_NE(y, 0);
    // Special case -1 to avoid getting a SIGFPE on x86(_64).
    return (y == -1) ? -x : x / y;
  }

  virtual int64_t Evaluate(int64_t x, int64_t y) const {
    DCHECK_NE(y, 0);
    // Special case -1 to avoid getting a SIGFPE on x86(_64).
    return (y == -1) ? -x : x / y;
  }

  uint32_t GetDexPc() const { return dex_pc_; }

  DECLARE_INSTRUCTION(Div);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HDiv);
};

class HRem : public HBinaryOperation {
 public:
  HRem(Primitive::Type result_type, HInstruction* left, HInstruction* right, uint32_t dex_pc)
      : HBinaryOperation(result_type, left, right), dex_pc_(dex_pc) {}

  virtual int32_t Evaluate(int32_t x, int32_t y) const {
    DCHECK_NE(y, 0);
    // Special case -1 to avoid getting a SIGFPE on x86(_64).
    return (y == -1) ? 0 : x % y;
  }

  virtual int64_t Evaluate(int64_t x, int64_t y) const {
    DCHECK_NE(y, 0);
    // Special case -1 to avoid getting a SIGFPE on x86(_64).
    return (y == -1) ? 0 : x % y;
  }

  uint32_t GetDexPc() const { return dex_pc_; }

  DECLARE_INSTRUCTION(Rem);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HRem);
};

class HDivZeroCheck : public HExpression<1> {
 public:
  HDivZeroCheck(HInstruction* value, uint32_t dex_pc)
      : HExpression(value->GetType(), SideEffects::None()), dex_pc_(dex_pc) {
    SetRawInputAt(0, value);
  }

  bool CanBeMoved() const OVERRIDE { return true; }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    UNUSED(other);
    return true;
  }

  bool NeedsEnvironment() const OVERRIDE { return true; }
  bool CanThrow() const OVERRIDE { return true; }

  uint32_t GetDexPc() const { return dex_pc_; }

  DECLARE_INSTRUCTION(DivZeroCheck);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HDivZeroCheck);
};

class HShl : public HBinaryOperation {
 public:
  HShl(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE { return x << (y & kMaxIntShiftValue); }
  int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE { return x << (y & kMaxLongShiftValue); }

  DECLARE_INSTRUCTION(Shl);

 private:
  DISALLOW_COPY_AND_ASSIGN(HShl);
};

class HShr : public HBinaryOperation {
 public:
  HShr(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE { return x >> (y & kMaxIntShiftValue); }
  int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE { return x >> (y & kMaxLongShiftValue); }

  DECLARE_INSTRUCTION(Shr);

 private:
  DISALLOW_COPY_AND_ASSIGN(HShr);
};

class HUShr : public HBinaryOperation {
 public:
  HUShr(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE {
    uint32_t ux = static_cast<uint32_t>(x);
    uint32_t uy = static_cast<uint32_t>(y) & kMaxIntShiftValue;
    return static_cast<int32_t>(ux >> uy);
  }

  int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE {
    uint64_t ux = static_cast<uint64_t>(x);
    uint64_t uy = static_cast<uint64_t>(y) & kMaxLongShiftValue;
    return static_cast<int64_t>(ux >> uy);
  }

  DECLARE_INSTRUCTION(UShr);

 private:
  DISALLOW_COPY_AND_ASSIGN(HUShr);
};

class HAnd : public HBinaryOperation {
 public:
  HAnd(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  bool IsCommutative() OVERRIDE { return true; }

  int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE { return x & y; }
  int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE { return x & y; }

  DECLARE_INSTRUCTION(And);

 private:
  DISALLOW_COPY_AND_ASSIGN(HAnd);
};

class HOr : public HBinaryOperation {
 public:
  HOr(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  bool IsCommutative() OVERRIDE { return true; }

  int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE { return x | y; }
  int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE { return x | y; }

  DECLARE_INSTRUCTION(Or);

 private:
  DISALLOW_COPY_AND_ASSIGN(HOr);
};

class HXor : public HBinaryOperation {
 public:
  HXor(Primitive::Type result_type, HInstruction* left, HInstruction* right)
      : HBinaryOperation(result_type, left, right) {}

  bool IsCommutative() OVERRIDE { return true; }

  int32_t Evaluate(int32_t x, int32_t y) const OVERRIDE { return x ^ y; }
  int64_t Evaluate(int64_t x, int64_t y) const OVERRIDE { return x ^ y; }

  DECLARE_INSTRUCTION(Xor);

 private:
  DISALLOW_COPY_AND_ASSIGN(HXor);
};

// The value of a parameter in this method. Its location depends on
// the calling convention.
class HParameterValue : public HExpression<0> {
 public:
  HParameterValue(uint8_t index, Primitive::Type parameter_type, bool is_this = false)
      : HExpression(parameter_type, SideEffects::None()), index_(index), is_this_(is_this) {}

  uint8_t GetIndex() const { return index_; }

  bool CanBeNull() const OVERRIDE { return !is_this_; }

  DECLARE_INSTRUCTION(ParameterValue);

 private:
  // The index of this parameter in the parameters list. Must be less
  // than HGraph::number_of_in_vregs_.
  const uint8_t index_;

  // Whether or not the parameter value corresponds to 'this' argument.
  const bool is_this_;

  DISALLOW_COPY_AND_ASSIGN(HParameterValue);
};

class HNot : public HUnaryOperation {
 public:
  explicit HNot(Primitive::Type result_type, HInstruction* input)
      : HUnaryOperation(result_type, input) {}

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const {
    UNUSED(other);
    return true;
  }

  virtual int32_t Evaluate(int32_t x) const OVERRIDE { return ~x; }
  virtual int64_t Evaluate(int64_t x) const OVERRIDE { return ~x; }

  DECLARE_INSTRUCTION(Not);

 private:
  DISALLOW_COPY_AND_ASSIGN(HNot);
};

class HTypeConversion : public HExpression<1> {
 public:
  // Instantiate a type conversion of `input` to `result_type`.
  HTypeConversion(Primitive::Type result_type, HInstruction* input, uint32_t dex_pc)
      : HExpression(result_type, SideEffects::None()), dex_pc_(dex_pc) {
    SetRawInputAt(0, input);
    DCHECK_NE(input->GetType(), result_type);
  }

  HInstruction* GetInput() const { return InputAt(0); }
  Primitive::Type GetInputType() const { return GetInput()->GetType(); }
  Primitive::Type GetResultType() const { return GetType(); }

  // Required by the x86 and ARM code generators when producing calls
  // to the runtime.
  uint32_t GetDexPc() const { return dex_pc_; }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE { return true; }

  DECLARE_INSTRUCTION(TypeConversion);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HTypeConversion);
};

static constexpr uint32_t kNoRegNumber = -1;

class HPhi : public HInstruction {
 public:
  HPhi(ArenaAllocator* arena, uint32_t reg_number, size_t number_of_inputs, Primitive::Type type)
      : HInstruction(SideEffects::None()),
        inputs_(arena, number_of_inputs),
        reg_number_(reg_number),
        type_(type),
        is_live_(false),
        can_be_null_(true) {
    inputs_.SetSize(number_of_inputs);
  }

  size_t InputCount() const OVERRIDE { return inputs_.Size(); }
  HInstruction* InputAt(size_t i) const OVERRIDE { return inputs_.Get(i); }

  void SetRawInputAt(size_t index, HInstruction* input) OVERRIDE {
    inputs_.Put(index, input);
  }

  void AddInput(HInstruction* input);

  Primitive::Type GetType() const OVERRIDE { return type_; }
  void SetType(Primitive::Type type) { type_ = type; }

  bool CanBeNull() const OVERRIDE { return can_be_null_; }
  void SetCanBeNull(bool can_be_null) { can_be_null_ = can_be_null; }

  uint32_t GetRegNumber() const { return reg_number_; }

  void SetDead() { is_live_ = false; }
  void SetLive() { is_live_ = true; }
  bool IsDead() const { return !is_live_; }
  bool IsLive() const { return is_live_; }

  DECLARE_INSTRUCTION(Phi);

 private:
  GrowableArray<HInstruction*> inputs_;
  const uint32_t reg_number_;
  Primitive::Type type_;
  bool is_live_;
  bool can_be_null_;

  DISALLOW_COPY_AND_ASSIGN(HPhi);
};

class HNullCheck : public HExpression<1> {
 public:
  HNullCheck(HInstruction* value, uint32_t dex_pc)
      : HExpression(value->GetType(), SideEffects::None()), dex_pc_(dex_pc) {
    SetRawInputAt(0, value);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    UNUSED(other);
    return true;
  }

  bool NeedsEnvironment() const OVERRIDE { return true; }

  bool CanThrow() const OVERRIDE { return true; }

  bool CanBeNull() const OVERRIDE { return false; }

  uint32_t GetDexPc() const { return dex_pc_; }

  DECLARE_INSTRUCTION(NullCheck);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HNullCheck);
};

class FieldInfo : public ValueObject {
 public:
  FieldInfo(MemberOffset field_offset, Primitive::Type field_type, bool is_volatile)
      : field_offset_(field_offset), field_type_(field_type), is_volatile_(is_volatile) {}

  MemberOffset GetFieldOffset() const { return field_offset_; }
  Primitive::Type GetFieldType() const { return field_type_; }
  bool IsVolatile() const { return is_volatile_; }

 private:
  const MemberOffset field_offset_;
  const Primitive::Type field_type_;
  const bool is_volatile_;
};

class HInstanceFieldGet : public HExpression<1> {
 public:
  HInstanceFieldGet(HInstruction* value,
                    Primitive::Type field_type,
                    MemberOffset field_offset,
                    bool is_volatile)
      : HExpression(field_type, SideEffects::DependsOnSomething()),
        field_info_(field_offset, field_type, is_volatile) {
    SetRawInputAt(0, value);
  }

  bool CanBeMoved() const OVERRIDE { return !IsVolatile(); }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    HInstanceFieldGet* other_get = other->AsInstanceFieldGet();
    return GetFieldOffset().SizeValue() == other_get->GetFieldOffset().SizeValue();
  }

  bool CanDoImplicitNullCheck() const OVERRIDE {
    return GetFieldOffset().Uint32Value() < kPageSize;
  }

  size_t ComputeHashCode() const OVERRIDE {
    return (HInstruction::ComputeHashCode() << 7) | GetFieldOffset().SizeValue();
  }

  const FieldInfo& GetFieldInfo() const { return field_info_; }
  MemberOffset GetFieldOffset() const { return field_info_.GetFieldOffset(); }
  Primitive::Type GetFieldType() const { return field_info_.GetFieldType(); }
  bool IsVolatile() const { return field_info_.IsVolatile(); }

  DECLARE_INSTRUCTION(InstanceFieldGet);

 private:
  const FieldInfo field_info_;

  DISALLOW_COPY_AND_ASSIGN(HInstanceFieldGet);
};

class HInstanceFieldSet : public HTemplateInstruction<2> {
 public:
  HInstanceFieldSet(HInstruction* object,
                    HInstruction* value,
                    Primitive::Type field_type,
                    MemberOffset field_offset,
                    bool is_volatile)
      : HTemplateInstruction(SideEffects::ChangesSomething()),
        field_info_(field_offset, field_type, is_volatile) {
    SetRawInputAt(0, object);
    SetRawInputAt(1, value);
  }

  bool CanDoImplicitNullCheck() const OVERRIDE {
    return GetFieldOffset().Uint32Value() < kPageSize;
  }

  const FieldInfo& GetFieldInfo() const { return field_info_; }
  MemberOffset GetFieldOffset() const { return field_info_.GetFieldOffset(); }
  Primitive::Type GetFieldType() const { return field_info_.GetFieldType(); }
  bool IsVolatile() const { return field_info_.IsVolatile(); }
  HInstruction* GetValue() const { return InputAt(1); }

  DECLARE_INSTRUCTION(InstanceFieldSet);

 private:
  const FieldInfo field_info_;

  DISALLOW_COPY_AND_ASSIGN(HInstanceFieldSet);
};

class HArrayGet : public HExpression<2> {
 public:
  HArrayGet(HInstruction* array, HInstruction* index, Primitive::Type type)
      : HExpression(type, SideEffects::DependsOnSomething()) {
    SetRawInputAt(0, array);
    SetRawInputAt(1, index);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    UNUSED(other);
    return true;
  }
  bool CanDoImplicitNullCheck() const OVERRIDE {
    // TODO: We can be smarter here.
    // Currently, the array access is always preceded by an ArrayLength or a NullCheck
    // which generates the implicit null check. There are cases when these can be removed
    // to produce better code. If we ever add optimizations to do so we should allow an
    // implicit check here (as long as the address falls in the first page).
    return false;
  }

  void SetType(Primitive::Type type) { type_ = type; }

  HInstruction* GetArray() const { return InputAt(0); }
  HInstruction* GetIndex() const { return InputAt(1); }

  DECLARE_INSTRUCTION(ArrayGet);

 private:
  DISALLOW_COPY_AND_ASSIGN(HArrayGet);
};

class HArraySet : public HTemplateInstruction<3> {
 public:
  HArraySet(HInstruction* array,
            HInstruction* index,
            HInstruction* value,
            Primitive::Type expected_component_type,
            uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::ChangesSomething()),
        dex_pc_(dex_pc),
        expected_component_type_(expected_component_type),
        needs_type_check_(value->GetType() == Primitive::kPrimNot) {
    SetRawInputAt(0, array);
    SetRawInputAt(1, index);
    SetRawInputAt(2, value);
  }

  bool NeedsEnvironment() const OVERRIDE {
    // We currently always call a runtime method to catch array store
    // exceptions.
    return needs_type_check_;
  }

  bool CanDoImplicitNullCheck() const OVERRIDE {
    // TODO: Same as for ArrayGet.
    return false;
  }

  void ClearNeedsTypeCheck() {
    needs_type_check_ = false;
  }

  bool NeedsTypeCheck() const { return needs_type_check_; }

  uint32_t GetDexPc() const { return dex_pc_; }

  HInstruction* GetArray() const { return InputAt(0); }
  HInstruction* GetIndex() const { return InputAt(1); }
  HInstruction* GetValue() const { return InputAt(2); }

  Primitive::Type GetComponentType() const {
    // The Dex format does not type floating point index operations. Since the
    // `expected_component_type_` is set during building and can therefore not
    // be correct, we also check what is the value type. If it is a floating
    // point type, we must use that type.
    Primitive::Type value_type = GetValue()->GetType();
    return ((value_type == Primitive::kPrimFloat) || (value_type == Primitive::kPrimDouble))
        ? value_type
        : expected_component_type_;
  }

  DECLARE_INSTRUCTION(ArraySet);

 private:
  const uint32_t dex_pc_;
  const Primitive::Type expected_component_type_;
  bool needs_type_check_;

  DISALLOW_COPY_AND_ASSIGN(HArraySet);
};

class HArrayLength : public HExpression<1> {
 public:
  explicit HArrayLength(HInstruction* array)
      : HExpression(Primitive::kPrimInt, SideEffects::None()) {
    // Note that arrays do not change length, so the instruction does not
    // depend on any write.
    SetRawInputAt(0, array);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    UNUSED(other);
    return true;
  }
  bool CanDoImplicitNullCheck() const OVERRIDE { return true; }

  DECLARE_INSTRUCTION(ArrayLength);

 private:
  DISALLOW_COPY_AND_ASSIGN(HArrayLength);
};

class HBoundsCheck : public HExpression<2> {
 public:
  HBoundsCheck(HInstruction* index, HInstruction* length, uint32_t dex_pc)
      : HExpression(index->GetType(), SideEffects::None()), dex_pc_(dex_pc) {
    DCHECK(index->GetType() == Primitive::kPrimInt);
    SetRawInputAt(0, index);
    SetRawInputAt(1, length);
  }

  virtual bool CanBeMoved() const { return true; }
  virtual bool InstructionDataEquals(HInstruction* other) const {
    UNUSED(other);
    return true;
  }

  virtual bool NeedsEnvironment() const { return true; }

  virtual bool CanThrow() const { return true; }

  uint32_t GetDexPc() const { return dex_pc_; }

  DECLARE_INSTRUCTION(BoundsCheck);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HBoundsCheck);
};

/**
 * Some DEX instructions are folded into multiple HInstructions that need
 * to stay live until the last HInstruction. This class
 * is used as a marker for the baseline compiler to ensure its preceding
 * HInstruction stays live. `index` represents the stack location index of the
 * instruction (the actual offset is computed as index * vreg_size).
 */
class HTemporary : public HTemplateInstruction<0> {
 public:
  explicit HTemporary(size_t index) : HTemplateInstruction(SideEffects::None()), index_(index) {}

  size_t GetIndex() const { return index_; }

  Primitive::Type GetType() const OVERRIDE {
    // The previous instruction is the one that will be stored in the temporary location.
    DCHECK(GetPrevious() != nullptr);
    return GetPrevious()->GetType();
  }

  DECLARE_INSTRUCTION(Temporary);

 private:
  const size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HTemporary);
};

class HSuspendCheck : public HTemplateInstruction<0> {
 public:
  explicit HSuspendCheck(uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::None()), dex_pc_(dex_pc) {}

  virtual bool NeedsEnvironment() const {
    return true;
  }

  uint32_t GetDexPc() const { return dex_pc_; }

  DECLARE_INSTRUCTION(SuspendCheck);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HSuspendCheck);
};

/**
 * Instruction to load a Class object.
 */
class HLoadClass : public HExpression<0> {
 public:
  HLoadClass(uint16_t type_index,
             bool is_referrers_class,
             uint32_t dex_pc)
      : HExpression(Primitive::kPrimNot, SideEffects::None()),
        type_index_(type_index),
        is_referrers_class_(is_referrers_class),
        dex_pc_(dex_pc),
        generate_clinit_check_(false) {}

  bool CanBeMoved() const OVERRIDE { return true; }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    return other->AsLoadClass()->type_index_ == type_index_;
  }

  size_t ComputeHashCode() const OVERRIDE { return type_index_; }

  uint32_t GetDexPc() const { return dex_pc_; }
  uint16_t GetTypeIndex() const { return type_index_; }
  bool IsReferrersClass() const { return is_referrers_class_; }

  bool NeedsEnvironment() const OVERRIDE {
    // Will call runtime and load the class if the class is not loaded yet.
    // TODO: finer grain decision.
    return !is_referrers_class_;
  }

  bool MustGenerateClinitCheck() const {
    return generate_clinit_check_;
  }

  void SetMustGenerateClinitCheck() {
    generate_clinit_check_ = true;
  }

  bool CanCallRuntime() const {
    return MustGenerateClinitCheck() || !is_referrers_class_;
  }

  bool CanThrow() const OVERRIDE {
    // May call runtime and and therefore can throw.
    // TODO: finer grain decision.
    return !is_referrers_class_;
  }

  ReferenceTypeInfo GetLoadedClassRTI() {
    return loaded_class_rti_;
  }

  void SetLoadedClassRTI(ReferenceTypeInfo rti) {
    loaded_class_rti_ = rti;
  }

  DECLARE_INSTRUCTION(LoadClass);

 private:
  const uint16_t type_index_;
  const bool is_referrers_class_;
  const uint32_t dex_pc_;
  // Whether this instruction must generate the initialization check.
  // Used for code generation.
  bool generate_clinit_check_;

  ReferenceTypeInfo loaded_class_rti_;

  DISALLOW_COPY_AND_ASSIGN(HLoadClass);
};

class HLoadString : public HExpression<0> {
 public:
  HLoadString(uint32_t string_index, uint32_t dex_pc)
      : HExpression(Primitive::kPrimNot, SideEffects::None()),
        string_index_(string_index),
        dex_pc_(dex_pc) {}

  bool CanBeMoved() const OVERRIDE { return true; }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    return other->AsLoadString()->string_index_ == string_index_;
  }

  size_t ComputeHashCode() const OVERRIDE { return string_index_; }

  uint32_t GetDexPc() const { return dex_pc_; }
  uint32_t GetStringIndex() const { return string_index_; }

  // TODO: Can we deopt or debug when we resolve a string?
  bool NeedsEnvironment() const OVERRIDE { return false; }

  DECLARE_INSTRUCTION(LoadString);

 private:
  const uint32_t string_index_;
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HLoadString);
};

// TODO: Pass this check to HInvokeStaticOrDirect nodes.
/**
 * Performs an initialization check on its Class object input.
 */
class HClinitCheck : public HExpression<1> {
 public:
  explicit HClinitCheck(HLoadClass* constant, uint32_t dex_pc)
      : HExpression(Primitive::kPrimNot, SideEffects::All()),
        dex_pc_(dex_pc) {
    SetRawInputAt(0, constant);
  }

  bool CanBeMoved() const OVERRIDE { return true; }
  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    UNUSED(other);
    return true;
  }

  bool NeedsEnvironment() const OVERRIDE {
    // May call runtime to initialize the class.
    return true;
  }

  uint32_t GetDexPc() const { return dex_pc_; }

  HLoadClass* GetLoadClass() const { return InputAt(0)->AsLoadClass(); }

  DECLARE_INSTRUCTION(ClinitCheck);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HClinitCheck);
};

class HStaticFieldGet : public HExpression<1> {
 public:
  HStaticFieldGet(HInstruction* cls,
                  Primitive::Type field_type,
                  MemberOffset field_offset,
                  bool is_volatile)
      : HExpression(field_type, SideEffects::DependsOnSomething()),
        field_info_(field_offset, field_type, is_volatile) {
    SetRawInputAt(0, cls);
  }


  bool CanBeMoved() const OVERRIDE { return !IsVolatile(); }

  bool InstructionDataEquals(HInstruction* other) const OVERRIDE {
    HStaticFieldGet* other_get = other->AsStaticFieldGet();
    return GetFieldOffset().SizeValue() == other_get->GetFieldOffset().SizeValue();
  }

  size_t ComputeHashCode() const OVERRIDE {
    return (HInstruction::ComputeHashCode() << 7) | GetFieldOffset().SizeValue();
  }

  const FieldInfo& GetFieldInfo() const { return field_info_; }
  MemberOffset GetFieldOffset() const { return field_info_.GetFieldOffset(); }
  Primitive::Type GetFieldType() const { return field_info_.GetFieldType(); }
  bool IsVolatile() const { return field_info_.IsVolatile(); }

  DECLARE_INSTRUCTION(StaticFieldGet);

 private:
  const FieldInfo field_info_;

  DISALLOW_COPY_AND_ASSIGN(HStaticFieldGet);
};

class HStaticFieldSet : public HTemplateInstruction<2> {
 public:
  HStaticFieldSet(HInstruction* cls,
                  HInstruction* value,
                  Primitive::Type field_type,
                  MemberOffset field_offset,
                  bool is_volatile)
      : HTemplateInstruction(SideEffects::ChangesSomething()),
        field_info_(field_offset, field_type, is_volatile) {
    SetRawInputAt(0, cls);
    SetRawInputAt(1, value);
  }

  const FieldInfo& GetFieldInfo() const { return field_info_; }
  MemberOffset GetFieldOffset() const { return field_info_.GetFieldOffset(); }
  Primitive::Type GetFieldType() const { return field_info_.GetFieldType(); }
  bool IsVolatile() const { return field_info_.IsVolatile(); }

  HInstruction* GetValue() const { return InputAt(1); }

  DECLARE_INSTRUCTION(StaticFieldSet);

 private:
  const FieldInfo field_info_;

  DISALLOW_COPY_AND_ASSIGN(HStaticFieldSet);
};

// Implement the move-exception DEX instruction.
class HLoadException : public HExpression<0> {
 public:
  HLoadException() : HExpression(Primitive::kPrimNot, SideEffects::None()) {}

  DECLARE_INSTRUCTION(LoadException);

 private:
  DISALLOW_COPY_AND_ASSIGN(HLoadException);
};

class HThrow : public HTemplateInstruction<1> {
 public:
  HThrow(HInstruction* exception, uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::None()), dex_pc_(dex_pc) {
    SetRawInputAt(0, exception);
  }

  bool IsControlFlow() const OVERRIDE { return true; }

  bool NeedsEnvironment() const OVERRIDE { return true; }

  bool CanThrow() const OVERRIDE { return true; }

  uint32_t GetDexPc() const { return dex_pc_; }

  DECLARE_INSTRUCTION(Throw);

 private:
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HThrow);
};

class HInstanceOf : public HExpression<2> {
 public:
  HInstanceOf(HInstruction* object,
              HLoadClass* constant,
              bool class_is_final,
              uint32_t dex_pc)
      : HExpression(Primitive::kPrimBoolean, SideEffects::None()),
        class_is_final_(class_is_final),
        dex_pc_(dex_pc) {
    SetRawInputAt(0, object);
    SetRawInputAt(1, constant);
  }

  bool CanBeMoved() const OVERRIDE { return true; }

  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  bool NeedsEnvironment() const OVERRIDE {
    return false;
  }

  uint32_t GetDexPc() const { return dex_pc_; }

  bool IsClassFinal() const { return class_is_final_; }

  DECLARE_INSTRUCTION(InstanceOf);

 private:
  const bool class_is_final_;
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HInstanceOf);
};

class HBoundType : public HExpression<1> {
 public:
  HBoundType(HInstruction* input, ReferenceTypeInfo top_type)
      : HExpression(Primitive::kPrimNot, SideEffects::None()),
        top_type_(top_type) {
    SetRawInputAt(0, input);
  }

  const ReferenceTypeInfo& GetTopType() const { return top_type_; }

  DECLARE_INSTRUCTION(BoundType);

 private:
  const ReferenceTypeInfo top_type_;

  DISALLOW_COPY_AND_ASSIGN(HBoundType);
};

class HCheckCast : public HTemplateInstruction<2> {
 public:
  HCheckCast(HInstruction* object,
             HLoadClass* constant,
             bool class_is_final,
             uint32_t dex_pc)
      : HTemplateInstruction(SideEffects::None()),
        class_is_final_(class_is_final),
        dex_pc_(dex_pc) {
    SetRawInputAt(0, object);
    SetRawInputAt(1, constant);
  }

  bool CanBeMoved() const OVERRIDE { return true; }

  bool InstructionDataEquals(HInstruction* other ATTRIBUTE_UNUSED) const OVERRIDE {
    return true;
  }

  bool NeedsEnvironment() const OVERRIDE {
    // Instruction may throw a CheckCastError.
    return true;
  }

  bool CanThrow() const OVERRIDE { return true; }

  uint32_t GetDexPc() const { return dex_pc_; }

  bool IsClassFinal() const { return class_is_final_; }

  DECLARE_INSTRUCTION(CheckCast);

 private:
  const bool class_is_final_;
  const uint32_t dex_pc_;

  DISALLOW_COPY_AND_ASSIGN(HCheckCast);
};

class HMonitorOperation : public HTemplateInstruction<1> {
 public:
  enum OperationKind {
    kEnter,
    kExit,
  };

  HMonitorOperation(HInstruction* object, OperationKind kind, uint32_t dex_pc)
    : HTemplateInstruction(SideEffects::None()), kind_(kind), dex_pc_(dex_pc) {
    SetRawInputAt(0, object);
  }

  // Instruction may throw a Java exception, so we need an environment.
  bool NeedsEnvironment() const OVERRIDE { return true; }
  bool CanThrow() const OVERRIDE { return true; }

  uint32_t GetDexPc() const { return dex_pc_; }

  bool IsEnter() const { return kind_ == kEnter; }

  DECLARE_INSTRUCTION(MonitorOperation);

 private:
  const OperationKind kind_;
  const uint32_t dex_pc_;

 private:
  DISALLOW_COPY_AND_ASSIGN(HMonitorOperation);
};

class MoveOperands : public ArenaObject<kArenaAllocMisc> {
 public:
  MoveOperands(Location source, Location destination, HInstruction* instruction)
      : source_(source), destination_(destination), instruction_(instruction) {}

  Location GetSource() const { return source_; }
  Location GetDestination() const { return destination_; }

  void SetSource(Location value) { source_ = value; }
  void SetDestination(Location value) { destination_ = value; }

  // The parallel move resolver marks moves as "in-progress" by clearing the
  // destination (but not the source).
  Location MarkPending() {
    DCHECK(!IsPending());
    Location dest = destination_;
    destination_ = Location::NoLocation();
    return dest;
  }

  void ClearPending(Location dest) {
    DCHECK(IsPending());
    destination_ = dest;
  }

  bool IsPending() const {
    DCHECK(!source_.IsInvalid() || destination_.IsInvalid());
    return destination_.IsInvalid() && !source_.IsInvalid();
  }

  // True if this blocks a move from the given location.
  bool Blocks(Location loc) const {
    return !IsEliminated() && source_.Equals(loc);
  }

  // A move is redundant if it's been eliminated, if its source and
  // destination are the same, or if its destination is unneeded.
  bool IsRedundant() const {
    return IsEliminated() || destination_.IsInvalid() || source_.Equals(destination_);
  }

  // We clear both operands to indicate move that's been eliminated.
  void Eliminate() {
    source_ = destination_ = Location::NoLocation();
  }

  bool IsEliminated() const {
    DCHECK(!source_.IsInvalid() || destination_.IsInvalid());
    return source_.IsInvalid();
  }

  HInstruction* GetInstruction() const { return instruction_; }

 private:
  Location source_;
  Location destination_;
  // The instruction this move is assocatied with. Null when this move is
  // for moving an input in the expected locations of user (including a phi user).
  // This is only used in debug mode, to ensure we do not connect interval siblings
  // in the same parallel move.
  HInstruction* instruction_;
};

static constexpr size_t kDefaultNumberOfMoves = 4;

class HParallelMove : public HTemplateInstruction<0> {
 public:
  explicit HParallelMove(ArenaAllocator* arena)
      : HTemplateInstruction(SideEffects::None()), moves_(arena, kDefaultNumberOfMoves) {}

  void AddMove(Location source, Location destination, HInstruction* instruction) {
    DCHECK(source.IsValid());
    DCHECK(destination.IsValid());
    // The parallel move resolver does not handle pairs. So we decompose the
    // pair locations into two moves.
    if (source.IsPair() && destination.IsPair()) {
      AddMove(source.ToLow(), destination.ToLow(), instruction);
      AddMove(source.ToHigh(), destination.ToHigh(), nullptr);
    } else if (source.IsPair()) {
      DCHECK(destination.IsDoubleStackSlot()) << destination;
      AddMove(source.ToLow(), Location::StackSlot(destination.GetStackIndex()), instruction);
      AddMove(source.ToHigh(), Location::StackSlot(destination.GetHighStackIndex(4)), nullptr);
    } else if (destination.IsPair()) {
      if (source.IsConstant()) {
        // We put the same constant in the move. The code generator will handle which
        // low or high part to use.
        AddMove(source, destination.ToLow(), instruction);
        AddMove(source, destination.ToHigh(), nullptr);
      } else {
        DCHECK(source.IsDoubleStackSlot());
        AddMove(Location::StackSlot(source.GetStackIndex()), destination.ToLow(), instruction);
        // TODO: rewrite GetHighStackIndex to not require a word size. It's supposed to
        // always be 4.
        static constexpr int kHighOffset = 4;
        AddMove(Location::StackSlot(source.GetHighStackIndex(kHighOffset)),
                destination.ToHigh(),
                nullptr);
      }
    } else {
      if (kIsDebugBuild) {
        if (instruction != nullptr) {
          for (size_t i = 0, e = moves_.Size(); i < e; ++i) {
            DCHECK_NE(moves_.Get(i).GetInstruction(), instruction)
              << "Doing parallel moves for the same instruction.";
          }
        }
        for (size_t i = 0, e = moves_.Size(); i < e; ++i) {
          DCHECK(!destination.Equals(moves_.Get(i).GetDestination()))
              << "Same destination for two moves in a parallel move.";
        }
      }
      moves_.Add(MoveOperands(source, destination, instruction));
    }
  }

  MoveOperands* MoveOperandsAt(size_t index) const {
    return moves_.GetRawStorage() + index;
  }

  size_t NumMoves() const { return moves_.Size(); }

  DECLARE_INSTRUCTION(ParallelMove);

 private:
  GrowableArray<MoveOperands> moves_;

  DISALLOW_COPY_AND_ASSIGN(HParallelMove);
};

class HGraphVisitor : public ValueObject {
 public:
  explicit HGraphVisitor(HGraph* graph) : graph_(graph) {}
  virtual ~HGraphVisitor() {}

  virtual void VisitInstruction(HInstruction* instruction) { UNUSED(instruction); }
  virtual void VisitBasicBlock(HBasicBlock* block);

  // Visit the graph following basic block insertion order.
  void VisitInsertionOrder();

  // Visit the graph following dominator tree reverse post-order.
  void VisitReversePostOrder();

  HGraph* GetGraph() const { return graph_; }

  // Visit functions for instruction classes.
#define DECLARE_VISIT_INSTRUCTION(name, super)                                        \
  virtual void Visit##name(H##name* instr) { VisitInstruction(instr); }

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  HGraph* const graph_;

  DISALLOW_COPY_AND_ASSIGN(HGraphVisitor);
};

class HGraphDelegateVisitor : public HGraphVisitor {
 public:
  explicit HGraphDelegateVisitor(HGraph* graph) : HGraphVisitor(graph) {}
  virtual ~HGraphDelegateVisitor() {}

  // Visit functions that delegate to to super class.
#define DECLARE_VISIT_INSTRUCTION(name, super)                                        \
  virtual void Visit##name(H##name* instr) OVERRIDE { Visit##super(instr); }

  FOR_EACH_INSTRUCTION(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

 private:
  DISALLOW_COPY_AND_ASSIGN(HGraphDelegateVisitor);
};

class HInsertionOrderIterator : public ValueObject {
 public:
  explicit HInsertionOrderIterator(const HGraph& graph) : graph_(graph), index_(0) {}

  bool Done() const { return index_ == graph_.GetBlocks().Size(); }
  HBasicBlock* Current() const { return graph_.GetBlocks().Get(index_); }
  void Advance() { ++index_; }

 private:
  const HGraph& graph_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HInsertionOrderIterator);
};

class HReversePostOrderIterator : public ValueObject {
 public:
  explicit HReversePostOrderIterator(const HGraph& graph) : graph_(graph), index_(0) {}

  bool Done() const { return index_ == graph_.GetReversePostOrder().Size(); }
  HBasicBlock* Current() const { return graph_.GetReversePostOrder().Get(index_); }
  void Advance() { ++index_; }

 private:
  const HGraph& graph_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HReversePostOrderIterator);
};

class HPostOrderIterator : public ValueObject {
 public:
  explicit HPostOrderIterator(const HGraph& graph)
      : graph_(graph), index_(graph_.GetReversePostOrder().Size()) {}

  bool Done() const { return index_ == 0; }
  HBasicBlock* Current() const { return graph_.GetReversePostOrder().Get(index_ - 1); }
  void Advance() { --index_; }

 private:
  const HGraph& graph_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HPostOrderIterator);
};

// Iterator over the blocks that art part of the loop. Includes blocks part
// of an inner loop. The order in which the blocks are iterated is on their
// block id.
class HBlocksInLoopIterator : public ValueObject {
 public:
  explicit HBlocksInLoopIterator(const HLoopInformation& info)
      : blocks_in_loop_(info.GetBlocks()),
        blocks_(info.GetHeader()->GetGraph()->GetBlocks()),
        index_(0) {
    if (!blocks_in_loop_.IsBitSet(index_)) {
      Advance();
    }
  }

  bool Done() const { return index_ == blocks_.Size(); }
  HBasicBlock* Current() const { return blocks_.Get(index_); }
  void Advance() {
    ++index_;
    for (size_t e = blocks_.Size(); index_ < e; ++index_) {
      if (blocks_in_loop_.IsBitSet(index_)) {
        break;
      }
    }
  }

 private:
  const BitVector& blocks_in_loop_;
  const GrowableArray<HBasicBlock*>& blocks_;
  size_t index_;

  DISALLOW_COPY_AND_ASSIGN(HBlocksInLoopIterator);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_NODES_H_
