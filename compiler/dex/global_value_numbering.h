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

#ifndef ART_COMPILER_DEX_GLOBAL_VALUE_NUMBERING_H_
#define ART_COMPILER_DEX_GLOBAL_VALUE_NUMBERING_H_

#include "base/macros.h"
#include "compiler_internals.h"
#include "utils/scoped_arena_containers.h"

namespace art {

class LocalValueNumbering;
class MirFieldInfo;

class GlobalValueNumbering {
 public:
  GlobalValueNumbering(CompilationUnit* cu, ScopedArenaAllocator* allocator);
  ~GlobalValueNumbering();

  LocalValueNumbering* PrepareBasicBlock(BasicBlock* bb);
  bool FinishBasicBlock(BasicBlock* bb);

  // Checks that the value names didn't overflow.
  bool Good() const {
    return last_value_ < kNoValue;
  }

  // Allow modifications.
  void AllowModifications() {
    // TODO: This should be used only if Good().
    // DCHECK(Good());
    modifications_allowed_ = true;
  }

  bool CanModify() const {
    // TODO: DCHECK(Good()), see AllowModifications() and NewValueName().
    return modifications_allowed_ && Good();
  }

  // GlobalValueNumbering should be allocated on the ArenaStack (or the native stack).
  static void* operator new(size_t size, ScopedArenaAllocator* allocator) {
    return allocator->Alloc(sizeof(GlobalValueNumbering), kArenaAllocMIR);
  }

  // Allow delete-expression to destroy a GlobalValueNumbering object without deallocation.
  static void operator delete(void* ptr) { UNUSED(ptr); }

 private:
  static constexpr uint16_t kNoValue = 0xffffu;

  // Allocate a new value name.
  uint16_t NewValueName() {
    // TODO: No new values should be needed once we allow modifications.
    // DCHECK(!modifications_allowed_);
    ++last_value_;
    return last_value_;
  }

  // Key is concatenation of opcode, operand1, operand2 and modifier, value is value name.
  typedef ScopedArenaSafeMap<uint64_t, uint16_t> ValueMap;

  static uint64_t BuildKey(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier) {
    return (static_cast<uint64_t>(op) << 48 | static_cast<uint64_t>(operand1) << 32 |
            static_cast<uint64_t>(operand2) << 16 | static_cast<uint64_t>(modifier));
  };

  // Look up a value in the global value map, adding a new entry if there was none before.
  uint16_t LookupValue(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier) {
    uint16_t res;
    uint64_t key = BuildKey(op, operand1, operand2, modifier);
    ValueMap::iterator lb = global_value_map_.lower_bound(key);
    if (lb != global_value_map_.end() && lb->first == key) {
      res = lb->second;
    } else {
      res = NewValueName();
      global_value_map_.PutHint(lb, key, res);
    }
    return res;
  };

  // Store a value in the global value map. This should be used only for insns that define
  // a new memory version or a new non-aliasing reference to store initial values for that
  // memory version or reference. For example, an IPUT via an aliasing reference creates a
  // new memory version for all potentially aliased accesses to the same field but we know
  // that when we read using the same reference that was used in the IPUT, as long as the
  // memory version is the same, we get the value we stored. Similarly, NEW_FILLED_ARRAY
  // fills the new unique array with
  void StoreValue(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier,
                  uint16_t value) {
    uint64_t key = BuildKey(op, operand1, operand2, modifier);
    auto lb = global_value_map_.lower_bound(key);
    if (lb != global_value_map_.end() && lb->first == key) {
      if (lb->second != value) {
        // The value name has changed, we need to rerun all dependent LVNs.
        change_ = true;
        lb->second = value;
      }
    } else {
      global_value_map_.PutHint(lb, key, value);
    }
  }

  // Check if the exact value is stored in the global value map. This should be used only for
  // PUT insns to check if we're trying to store the same value as the initial value of the
  // memory location or for a given memory version. See StoreValue().
  bool HasValue(uint16_t op, uint16_t operand1, uint16_t operand2, uint16_t modifier,
                uint16_t value) const {
    DCHECK(value != 0u || !Good());
    DCHECK_LE(value, last_value_);
    // This is equivalent to value == LookupValue(op, operand1, operand2, modifier)
    // except that it doesn't add an entry to the global value map if it's not there.
    uint64_t key = BuildKey(op, operand1, operand2, modifier);
    ValueMap::const_iterator it = global_value_map_.find(key);
    return (it != global_value_map_.end() && it->second == value);
  };

  // FieldReference represents a unique resolved field.
  struct FieldReference {
    const DexFile* dex_file;
    uint16_t field_idx;
    uint16_t type;
  };

  struct FieldReferenceComparator {
    bool operator()(const FieldReference& lhs, const FieldReference& rhs) const {
      if (lhs.field_idx != rhs.field_idx) {
        return lhs.field_idx < rhs.field_idx;
      }
      // If the field_idx and dex_file match, the type must also match.
      DCHECK(lhs.dex_file != rhs.dex_file || lhs.type == rhs.type);
      return lhs.dex_file < rhs.dex_file;
    }
  };

  // Maps field key to field id for resolved fields.
  typedef ScopedArenaSafeMap<FieldReference, uint32_t, FieldReferenceComparator> FieldIndexMap;

  // Get a field id.
  uint16_t GetFieldId(const MirFieldInfo& field_info, uint16_t type);

  // Get a field type based on field id.
  uint16_t GetFieldType(uint16_t field_id) {
    DCHECK_LT(field_id, field_index_reverse_map_.size());
    return field_index_reverse_map_[field_id]->first.type;
  }

  struct ArrayLocation {
    uint16_t base;
    uint16_t index;
  };

  struct ArrayLocationComparator {
    bool operator()(const ArrayLocation& lhs, const ArrayLocation& rhs) const {
      if (lhs.base != rhs.base) {
        return lhs.base < rhs.base;
      }
      return lhs.index < rhs.index;
    }
  };

  typedef ScopedArenaSafeMap<ArrayLocation, uint16_t, ArrayLocationComparator> ArrayLocationMap;

  // Get an array location.
  uint16_t GetArrayLocation(uint16_t base, uint16_t index);

  // Get the array base from an array location.
  uint16_t GetArrayLocationBase(uint16_t location) const {
    return array_location_reverse_map_[location]->first.base;
  }

  // Get the array index from an array location.
  uint16_t GetArrayLocationIndex(uint16_t location) const {
    return array_location_reverse_map_[location]->first.index;
  }

  // Key is s_reg, value is value name.
  typedef ScopedArenaSafeMap<uint16_t, uint16_t> SregValueMap;

  void SetOperandValueImpl(uint16_t s_reg, uint16_t value, SregValueMap* map) {
    auto lb = map->lower_bound(s_reg);
    if (lb != map->end() && lb->first == s_reg) {
      if (lb->second != value) {
        // The value name has changed, we need to rerun all dependent LVNs.
        change_ = true;
        lb->second = value;
      }
    } else {
      map->PutHint(lb, s_reg, value);
    }
  }

  uint16_t GetOperandValueImpl(int s_reg, SregValueMap* map) {
    uint16_t res = kNoValue;
    auto lb = map->lower_bound(s_reg);
    if (lb != map->end() && lb->first == s_reg) {
      res = lb->second;
    } else {
      // First use
      res = LookupValue(kNoValue, s_reg, kNoValue, kNoValue);
      map->PutHint(lb, s_reg, res);
    }
    return res;
  }

  void SetOperandValue(uint16_t s_reg, uint16_t value) {
    SetOperandValueImpl(s_reg, value, &sreg_value_map_);
  };

  uint16_t GetOperandValue(int s_reg) {
    return GetOperandValueImpl(s_reg, &sreg_value_map_);
  };

  void SetOperandValueWide(uint16_t s_reg, uint16_t value) {
    SetOperandValueImpl(s_reg, value, &sreg_wide_value_map_);
  };

  uint16_t GetOperandValueWide(int s_reg) {
    return GetOperandValueImpl(s_reg, &sreg_wide_value_map_);
  };

  // A set of value names.
  typedef ScopedArenaSet<uint16_t> ValueNameSet;

  // A map from a set of references to the set id.
  typedef ScopedArenaSafeMap<ValueNameSet, uint16_t> RefSetIdMap;

  uint16_t GetRefSetId(const ValueNameSet& ref_set) {
    uint16_t res = kNoValue;
    auto it = ref_set_map_.lower_bound(ref_set);
    if (it != ref_set_map_.end() && !ref_set_map_.key_comp()(ref_set, it->first)) {
      res = it->second;
    } else {
      res = NewValueName();
      ref_set_map_.PutHint(it, ref_set, res);
    }
    return res;
  }

  bool HasNullCheckLastInsn(const BasicBlock* pred_bb, BasicBlockId succ_id) const;

  bool NullCheckedInAllPredecessors(const ScopedArenaVector<uint16_t>& merge_names) const;

  CompilationUnit* GetCompilationUnit() {
    return cu_;
  }

  MIRGraph* GetMirGraph() {
    return cu_->mir_graph.get();
  }

  ScopedArenaAllocator* Allocator() {
    return allocator_;
  }

  CompilationUnit* const cu_;
  ScopedArenaAllocator* allocator_;

  static constexpr uint32_t kMaxRepeatCount = 10u;

  // Track the repeat count to make sure the GVN converges quickly and abort the GVN otherwise.
  uint32_t repeat_count_;

  // We have 32-bit last_value_ so that we can detect when we run out of value names, see Good().
  // We usually don't check Good() until the end of LVN unless we're about to modify code.
  uint32_t last_value_;

  // Marks whether code modifications are allowed. The initial GVN is done without code
  // modifications to settle the value names. Afterwards, we allow modifications and rerun
  // LVN once for each BasicBlock.
  bool modifications_allowed_;

  ValueMap global_value_map_;
  FieldIndexMap field_index_map_;
  ScopedArenaVector<const FieldIndexMap::value_type*> field_index_reverse_map_;
  ArrayLocationMap array_location_map_;
  ScopedArenaVector<const ArrayLocationMap::value_type*> array_location_reverse_map_;
  SregValueMap sreg_value_map_;
  SregValueMap sreg_wide_value_map_;
  RefSetIdMap ref_set_map_;

  ScopedArenaVector<const LocalValueNumbering*> lvns_;        // Owning.
  std::unique_ptr<LocalValueNumbering> work_lvn_;
  ScopedArenaVector<const LocalValueNumbering*> merge_lvns_;  // Not owning.
  bool change_;

  friend class LocalValueNumbering;

  DISALLOW_COPY_AND_ASSIGN(GlobalValueNumbering);
};

}  // namespace art

#endif  // ART_COMPILER_DEX_GLOBAL_VALUE_NUMBERING_H_
