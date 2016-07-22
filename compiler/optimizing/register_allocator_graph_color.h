/*
 * Copyright (C) 2016 The Android Open Source Project
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

#ifndef ART_COMPILER_OPTIMIZING_REGISTER_ALLOCATOR_GRAPH_COLOR_H_
#define ART_COMPILER_OPTIMIZING_REGISTER_ALLOCATOR_GRAPH_COLOR_H_

#include "arch/instruction_set.h"
#include "base/arena_containers.h"
#include "base/arena_object.h"
#include "base/macros.h"
#include "primitive.h"
#include "register_allocator.h"

namespace art {

class CodeGenerator;
class HBasicBlock;
class HGraph;
class HInstruction;
class HParallelMove;
class Location;
class SsaLivenessAnalysis;
class InterferenceNode;
struct CoalesceOpportunity;
enum class CoalesceKind;

/**
 * A graph coloring register allocator.
 *
 * The algorithm proceeds as follows:
 * (1) Build an interference graph, where nodes represent live intervals, and edges represent
 *     interferences between two intervals. Coloring this graph with k colors is isomorphic to
 *     finding a valid register assignment with k registers.
 * (2) To color the graph, first prune all nodes with degree less than k, since these nodes are
 *     guaranteed a color. (No matter how we color their adjacent nodes, we can give them a
 *     different color.) As we prune nodes from the graph, more nodes may drop below degree k,
 *     enabling further pruning. The key is to maintain the pruning order in a stack, so that we
 *     can color the nodes in the reverse order.
 *     When there are no more nodes with degree less than k, we start pruning alternate nodes based
 *     on heuristics. Since these nodes are not guaranteed a color, we are careful to
 *     prioritize nodes that require a register. We also prioritize short intervals, because
 *     short intervals cannot be split very much if coloring fails (see below). "Prioritizing"
 *     a node amounts to pruning it later, since it will have fewer interferences if we prune other
 *     nodes first.
 * (3) We color nodes in the reverse order in which we pruned them. If we cannot assign
 *     a node a color, we do one of two things:
 *     - If the node requires a register, we consider the current coloring attempt a failure.
 *       However, we split the node's live interval in order to make the interference graph
 *       sparser, so that future coloring attempts may succeed.
 *     - If the node does not require a register, we simply assign it a location on the stack.
 *
 * A good reference for graph coloring register allocation is
 * "Modern Compiler Implementation in Java" (Andrew W. Appel, 2nd Edition).
 */
class RegisterAllocatorGraphColor : public RegisterAllocator {
 public:
  RegisterAllocatorGraphColor(ArenaAllocator* allocator,
                              CodeGenerator* codegen,
                              const SsaLivenessAnalysis& analysis,
                              bool iterative_move_coalescing = true);
  ~RegisterAllocatorGraphColor() OVERRIDE {}

  void AllocateRegisters() OVERRIDE;

  bool Validate(bool log_fatal_on_failure);

 private:
  // Initialize physical register nodes, and add ranges for globally blocked registers,
  // such as the stack pointer.
  void SetupPhysicalNodes();

  // Collect all intervals and prepare for register allocation.
  void ProcessInstructions();
  void ProcessInstruction(HInstruction* instruction);

  // Collect all live intervals associated with the temporary locations
  // needed by an instruction.
  void CheckForTempLiveIntervals(HInstruction* instruction);

  // If any inputs require specific registers, block those registers
  // at the position of this instruction.
  void CheckForFixedInputs(HInstruction* instruction);

  // If the output of an instruction requires a specific register, split
  // the interval and assign the register to the first part.
  void CheckForFixedOutput(HInstruction* instruction);

  // If a safe point is needed, add a synthesized interval to later record
  // the number of live registers at this point.
  void CheckForSafepoint(HInstruction* instruction);

  // Add all applicable safepoints to a live interval.
  // Currently depends on instruction processing order.
  void AddSafepointsFor(HInstruction* instruction);

  // Split an interval, but only if `position` is inside of `interval`.
  // Return either the new interval, or the original interval if not split.
  static LiveInterval* TrySplit(LiveInterval* interval, size_t position);

  // To ensure every graph can be colored, split live intervals
  // at their register defs and uses. This creates short intervals with low
  // degree in the interference graph, which are prioritized during graph
  // coloring.
  void SplitAtRegisterUses(LiveInterval* interval);

  // If the given instruction is a catch phi, give it a spill slot.
  void AllocateSpillSlotForCatchPhi(HInstruction* instruction);

  // Ensure that the given register cannot be allocated for a given range.
  void BlockRegister(Location location, size_t start, size_t end);
  void BlockRegisters(size_t start, size_t end, bool caller_save_only = false);

  static bool GreaterNodePriority(const InterferenceNode* lhs, const InterferenceNode* rhs);

  // Compare two coalesce opportunities based on their priority.
  static bool CmpCoalesceOpportunity(const CoalesceOpportunity* lhs,
                                     const CoalesceOpportunity* rhs);

  // Use the intervals collected from instructions to construct an
  // interference graph mapping intervals to adjacency lists.
  // Also, collect synthesized safepoint nodes, used to keep
  // track of live intervals across safepoints.
  // TODO: Should build safepoints elsewhere.
  void BuildInterferenceGraph(const ArenaVector<LiveInterval*>& intervals,
                              const ArenaVector<InterferenceNode*>& physical_nodes,
                              ArenaVector<InterferenceNode*>* safepoints);

  // Prune nodes from the interference graph to be colored later. Build
  // a stack (pruned_nodes) containing these intervals in an order determined
  // by various heuristics.
  void PruneInterferenceGraph(const ArenaVector<InterferenceNode*>& prunable_nodes,
                              size_t num_registers,
                              ArenaStdStack<InterferenceNode*>* pruned_nodes);

  void AddPotentialInterference(InterferenceNode* from,
                                InterferenceNode* to,
                                bool both_directions = true);

  // Create a coalesce opportunity between two nodes.
  void CreateCoalesceOpportunity(InterferenceNode* a, InterferenceNode* b, CoalesceKind kind);

  // Add coalesce opportunities to interference nodes.
  void FindCoalesceOpportunities();

  // Prune nodes from the interference graph to be colored later. Returns
  // a stack containing these intervals in an order determined by various
  // heuristics.
  // Also performs iterative conservative coalescing, based on Modern Compiler Implementation
  // in Java, 2nd ed. (Andrew Appel, Cambridge University Press.)
  void PruneInterferenceGraph(size_t num_registers);

  // Invalidate all coalesce opportunities this node has, so that it (and possibly its neighbors)
  // may be pruned from the interference graph.
  void FreezeMoves(InterferenceNode* node);

  // Prune a node from the interference graph, updating worklists if necessary.
  void PruneNode(InterferenceNode* node, size_t num_regs);

  // Add coalesce opportunities associated with this node to the coalesce worklist.
  void EnableCoalesceOpportunities(InterferenceNode* node);

  // If needed, from `node` from the freeze worklist to the simplify worklist.
  void CheckTransitionFromFreezeWorklist(InterferenceNode* node, size_t num_regs);

  // Returns true if `from` can be conservatively coalesced into another node `into`.
  bool PrecoloredHeuristic(InterferenceNode* from, InterferenceNode* into, size_t num_regs);
  bool UncoloredHeuristic(InterferenceNode* from, InterferenceNode* into, size_t num_regs);

  void Coalesce(CoalesceOpportunity* opportunity, size_t num_regs);

  // Merge `from` into `into` in the interference graph.
  void Combine(InterferenceNode* from, InterferenceNode* into, size_t num_regs);

  bool IsCallerSave(size_t reg, bool processing_core_regs);

  // Process pruned_intervals_ to color the interference graph, spilling when
  // necessary. Returns true if successful. Else, some intervals have been
  // split, and the interference graph should be rebuilt for another attempt.
  bool ColorInterferenceGraph(size_t num_registers,
                              bool processing_core_regs);

  // Return the maximum number of registers live at safepoints,
  // based on the outgoing interference edges of safepoint nodes.
  size_t ComputeMaxSafepointLiveRegisters(const ArenaVector<InterferenceNode*>& safepoints);

  // If necessary, add the given interval to the list of spilled intervals,
  // and make sure it's ready to be spilled to the stack.
  void AllocateSpillSlotFor(LiveInterval* interval);

  // Whether iterative move coalescing should be performed. Iterative move coalescing
  // improves code quality, but increases compile time.
  const bool iterative_move_coalescing_;

  // Live intervals, split by kind (core and floating point).
  // These should not contain high intervals, as those are represented by
  // the corresponding low interval throughout register allocation.
  ArenaVector<LiveInterval*> core_intervals_, fp_intervals_;

  // Intervals for temporaries, saved for special handling in the resolution phase.
  ArenaVector<LiveInterval*> temp_intervals_;

  // Safepoints, saved for special handling while processing instructions.
  ArenaVector<HInstruction*> safepoints_;

  // Interference nodes representing specific registers. These are "pre-colored" nodes
  // in the interference graph.
  ArenaVector<InterferenceNode*> physical_core_nodes_, physical_fp_nodes_;

  // Allocated stack slot counters.
  size_t int_spill_slot_counter_;
  size_t double_spill_slot_counter_;
  size_t float_spill_slot_counter_;
  size_t long_spill_slot_counter_;
  size_t catch_phi_spill_slot_counter_;

  // Number of stack slots needed for the pointer to the current method.
  // This is 1 for 32-bit architectures, and 2 for 64-bit architectures.
  const size_t reserved_art_method_slots_;

  // Number of stack slots needed for outgoing arguments.
  const size_t reserved_out_slots_;

  // The maximum number of registers live at safe points. Needed by the code generator.
  size_t max_safepoint_live_core_regs_;
  size_t max_safepoint_live_fp_regs_;

  // An arena allocator used for each round of graph coloring, since there could be several.
  ArenaAllocator* coloring_attempt_allocator_;

  // It is important that interference nodes receive unique IDs.
  size_t node_id_counter_;

  // A map from live intervals to interference nodes.
  ArenaHashMap<LiveInterval*, InterferenceNode*> interval_node_map_;

  // Uncolored nodes that should be pruned from the interference graph.
  ArenaVector<InterferenceNode*> prunable_nodes_;

  // A stack of nodes pruned from the interference graph, waiting to be pruned.
  ArenaStdStack<InterferenceNode*> pruned_nodes_;

  // Contains low degree, non-move-related nodes that can pruned immediately.
  ArenaDeque<InterferenceNode*> simplify_worklist_;

  // Contains low degree, move-related nodes.
  ArenaDeque<InterferenceNode*> freeze_worklist_;

  // Contains high degree nodes.
  // If we have to prune from the spill worklist, we cannot guarantee
  // the pruned node a color, so we order the worklist by priority.
  ArenaPriorityQueue<InterferenceNode*, decltype(&GreaterNodePriority)> spill_worklist_;

  // Contains coalesce opportunities.
  // We order the coalesce worklist by priority, since some coalesce opportunities (e.g., those
  // inside of loops) are more important than others.
  ArenaPriorityQueue<CoalesceOpportunity*, decltype(&CmpCoalesceOpportunity)> coalesce_worklist_;

  DISALLOW_COPY_AND_ASSIGN(RegisterAllocatorGraphColor);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_REGISTER_ALLOCATOR_GRAPH_COLOR_H_
