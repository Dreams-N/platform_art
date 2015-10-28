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

#include "profiling_info.h"

#include "art_method-inl.h"
#include "dex_instruction.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {

bool ProfilingInfo::Create(Thread* self, ArtMethod* method, bool is_compiler_task) {
  // Walk over the dex instructions of the method and keep track of
  // instructions we are interested in profiling.
  DCHECK(!method->IsNative());
  const DexFile::CodeItem& code_item = *method->GetCodeItem();
  const uint16_t* code_ptr = code_item.insns_;
  const uint16_t* code_end = code_item.insns_ + code_item.insns_size_in_code_units_;

  uint32_t dex_pc = 0;
  std::vector<uint32_t> entries;
  while (code_ptr < code_end) {
    const Instruction& instruction = *Instruction::At(code_ptr);
    switch (instruction.Opcode()) {
      case Instruction::INVOKE_VIRTUAL:
      case Instruction::INVOKE_VIRTUAL_RANGE:
      case Instruction::INVOKE_VIRTUAL_QUICK:
      case Instruction::INVOKE_VIRTUAL_RANGE_QUICK:
      case Instruction::INVOKE_INTERFACE:
      case Instruction::INVOKE_INTERFACE_RANGE:
        entries.push_back(dex_pc);
        break;

      default:
        break;
    }
    dex_pc += instruction.SizeInCodeUnits();
    code_ptr += instruction.SizeInCodeUnits();
  }

  // If there is no instruction we are interested in, no need to create a `ProfilingInfo`
  // object, it will never be filled.
  if (entries.empty()) {
    return true;
  }

  // Allocate the `ProfilingInfo` object int the JIT's data space.
  jit::JitCodeCache* code_cache = Runtime::Current()->GetJit()->GetCodeCache();
  return code_cache->AddProfilingInfo(self, method, entries, is_compiler_task) != nullptr;
}

void ProfilingInfo::AddInvokeInfo(uint32_t dex_pc, mirror::Class* cls) {
  InlineCache* cache = nullptr;
  // TODO: binary search if array is too long.
  for (size_t i = 0; i < number_of_inline_caches_; ++i) {
    if (cache_[i].dex_pc == dex_pc) {
      cache = &cache_[i];
      break;
    }
  }
  DCHECK(cache != nullptr);

  for (size_t i = 0; i < InlineCache::kIndividualCacheSize; ++i) {
    mirror::Class* existing = cache->classes_[i].Read();
    if (existing == cls) {
      // Receiver type is already in the cache, nothing else to do.
      return;
    } else if (existing == nullptr) {
      // Cache entry is empty, try to put `cls` in it.
      GcRoot<mirror::Class> expected_root(nullptr);
      GcRoot<mirror::Class> desired_root(cls);
      if (!reinterpret_cast<Atomic<GcRoot<mirror::Class>>*>(&cache->classes_[i])->
              CompareExchangeStrongSequentiallyConsistent(expected_root, desired_root)) {
        // Some other thread put a class in the cache, continue iteration starting at this
        // entry in case the entry contains `cls`.
        --i;
      } else {
        // We successfully set `cls`, just return.
        return;
      }
    }
  }
  // Unsuccessfull - cache is full, making it megamorphic.
  DCHECK(cache->IsMegamorphic());
}

}  // namespace art
