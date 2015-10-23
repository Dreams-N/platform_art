/*
 * Copyright 2014 The Android Open Source Project
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

#include "jit_code_cache.h"

#include <sstream>

#include "art_method-inl.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc/accounting/bitmap-inl.h"
#include "mem_map.h"
#include "oat_file-inl.h"
#include "thread_list.h"

namespace art {
namespace jit {

static constexpr int kProtAll = PROT_READ | PROT_WRITE | PROT_EXEC;
static constexpr int kProtData = PROT_READ | PROT_WRITE;
static constexpr int kProtCode = PROT_READ | PROT_EXEC;

#define CHECKED_MPROTECT(memory, size, prot)                \
  do {                                                      \
    int rc = mprotect(memory, size, prot);                  \
    if (UNLIKELY(rc != 0)) {                                \
      errno = rc;                                           \
      PLOG(FATAL) << "Failed to mprotect jit code cache";   \
    }                                                       \
  } while (false)                                           \

JitCodeCache* JitCodeCache::Create(size_t capacity, std::string* error_msg) {
  CHECK_GT(capacity, 0U);
  CHECK_LT(capacity, kMaxCapacity);
  std::string error_str;
  // Map name specific for android_os_Debug.cpp accounting.
  MemMap* data_map = MemMap::MapAnonymous(
    "data-code-cache", nullptr, capacity, kProtAll, false, false, &error_str);
  if (data_map == nullptr) {
    std::ostringstream oss;
    oss << "Failed to create read write execute cache: " << error_str << " size=" << capacity;
    *error_msg = oss.str();
    return nullptr;
  }

  // Data cache is 1 / 4 of the map.
  // TODO: Make this variable?
  size_t data_size = RoundUp(data_map->Size() / 4, kPageSize);
  size_t code_size = data_map->Size() - data_size;
  uint8_t* divider = data_map->Begin() + data_size;

  // We need to have 32 bit offsets from method headers in code cache which point to things
  // in the data cache. If the maps are more than 4G apart, having multiple maps wouldn't work.
  MemMap* code_map = data_map->RemapAtEnd(divider, "jit-code-cache", kProtAll, &error_str);
  if (code_map == nullptr) {
    std::ostringstream oss;
    oss << "Failed to create read write execute cache: " << error_str << " size=" << capacity;
    *error_msg = oss.str();
    return nullptr;
  }
  DCHECK_EQ(code_map->Size(), code_size);
  DCHECK_EQ(code_map->Begin(), divider);
  return new JitCodeCache(code_map, data_map);
}

JitCodeCache::JitCodeCache(MemMap* code_map, MemMap* data_map)
    : lock_("Jit code cache", kJitCodeCacheLock),
      lock_cond_("Jit code cache variable", lock_),
      collection_in_progress_(false),
      code_map_(code_map),
      data_map_(data_map) {

  code_mspace_ = create_mspace_with_base(code_map_->Begin(), code_map_->Size(), false /*locked*/);
  data_mspace_ = create_mspace_with_base(data_map_->Begin(), data_map_->Size(), false /*locked*/);

  if (code_mspace_ == nullptr || data_mspace_ == nullptr) {
    PLOG(FATAL) << "create_mspace_with_base failed";
  }

  // Prevent morecore requests from the mspace.
  mspace_set_footprint_limit(code_mspace_, code_map_->Size());
  mspace_set_footprint_limit(data_mspace_, data_map_->Size());

  CHECKED_MPROTECT(code_map_->Begin(), code_map_->Size(), kProtCode);
  CHECKED_MPROTECT(data_map_->Begin(), data_map_->Size(), kProtData);

  live_bitmap_.reset(CodeCacheBitmap::Create("code-cache-bitmap",
                                             reinterpret_cast<uintptr_t>(code_map_->Begin()),
                                             reinterpret_cast<uintptr_t>(code_map_->End())));

  if (live_bitmap_.get() == nullptr) {
    PLOG(FATAL) << "creating bitmaps for the JIT code cache failed";
  }

  VLOG(jit) << "Created jit code cache: data size="
            << PrettySize(data_map_->Size())
            << ", code size="
            << PrettySize(code_map_->Size());
}

bool JitCodeCache::ContainsPc(const void* ptr) const {
  return code_map_->Begin() <= ptr && ptr < code_map_->End();
}

class ScopedCodeCacheWrite {
 public:
  explicit ScopedCodeCacheWrite(MemMap* code_map) : code_map_(code_map) {
    CHECKED_MPROTECT(code_map_->Begin(), code_map_->Size(), kProtAll);
  }
  ~ScopedCodeCacheWrite() {
    CHECKED_MPROTECT(code_map_->Begin(), code_map_->Size(), kProtCode);
  }
 private:
  MemMap* const code_map_;

  DISALLOW_COPY_AND_ASSIGN(ScopedCodeCacheWrite);
};

uint8_t* JitCodeCache::CommitCode(Thread* self,
                                  ArtMethod* method,
                                  const uint8_t* mapping_table,
                                  const uint8_t* vmap_table,
                                  const uint8_t* gc_map,
                                  size_t frame_size_in_bytes,
                                  size_t core_spill_mask,
                                  size_t fp_spill_mask,
                                  const uint8_t* code,
                                  size_t code_size) {
  uint8_t* result = CommitCodeInternal(self,
                                       method,
                                       mapping_table,
                                       vmap_table,
                                       gc_map,
                                       frame_size_in_bytes,
                                       core_spill_mask,
                                       fp_spill_mask,
                                       code,
                                       code_size);
  if (result == nullptr) {
    // Retry.
    GarbageCollectCache(self);
    result = CommitCodeInternal(self,
                                method,
                                mapping_table,
                                vmap_table,
                                gc_map,
                                frame_size_in_bytes,
                                core_spill_mask,
                                fp_spill_mask,
                                code,
                                code_size);
  }
  return result;
}

bool JitCodeCache::WaitForPotentialCollectionToComplete(Thread* self) {
  bool in_collection = false;
  while (collection_in_progress_) {
    in_collection = true;
    lock_cond_.Wait(self);
  }
  return in_collection;
}

uint8_t* JitCodeCache::CommitCodeInternal(Thread* self,
                                          ArtMethod* method,
                                          const uint8_t* mapping_table,
                                          const uint8_t* vmap_table,
                                          const uint8_t* gc_map,
                                          size_t frame_size_in_bytes,
                                          size_t core_spill_mask,
                                          size_t fp_spill_mask,
                                          const uint8_t* code,
                                          size_t code_size) {
  size_t alignment = GetInstructionSetAlignment(kRuntimeISA);
  // Ensure the header ends up at expected instruction alignment.
  size_t header_size = RoundUp(sizeof(OatQuickMethodHeader), alignment);
  size_t total_size = header_size + code_size;

  OatQuickMethodHeader* method_header = nullptr;
  uint8_t* code_ptr = nullptr;

  ScopedThreadSuspension sts(self, kSuspended);
  MutexLock mu(self, lock_);
  WaitForPotentialCollectionToComplete(self);
  {
    ScopedCodeCacheWrite scc(code_map_.get());
    uint8_t* result = reinterpret_cast<uint8_t*>(
        mspace_memalign(code_mspace_, alignment, total_size));
    if (result == nullptr) {
      return nullptr;
    }
    code_ptr = result + header_size;
    DCHECK_ALIGNED_PARAM(reinterpret_cast<uintptr_t>(code_ptr), alignment);

    std::copy(code, code + code_size, code_ptr);
    method_header = OatQuickMethodHeader::FromCodePointer(code_ptr);
    new (method_header) OatQuickMethodHeader(
        (mapping_table == nullptr) ? 0 : code_ptr - mapping_table,
        (vmap_table == nullptr) ? 0 : code_ptr - vmap_table,
        (gc_map == nullptr) ? 0 : code_ptr - gc_map,
        frame_size_in_bytes,
        core_spill_mask,
        fp_spill_mask,
        code_size);
  }

  __builtin___clear_cache(reinterpret_cast<char*>(code_ptr),
                          reinterpret_cast<char*>(code_ptr + code_size));
  method_code_map_.Put(code_ptr, method);
  method->SetEntryPointFromQuickCompiledCode(method_header->GetEntryPoint());
  return reinterpret_cast<uint8_t*>(method_header);
}

size_t JitCodeCache::CodeCacheSize() {
  MutexLock mu(Thread::Current(), lock_);
  size_t bytes_allocated = 0;
  mspace_inspect_all(code_mspace_, DlmallocBytesAllocatedCallback, &bytes_allocated);
  return bytes_allocated;
}

size_t JitCodeCache::DataCacheSize() {
  MutexLock mu(Thread::Current(), lock_);
  size_t bytes_allocated = 0;
  mspace_inspect_all(data_mspace_, DlmallocBytesAllocatedCallback, &bytes_allocated);
  return bytes_allocated;
}

size_t JitCodeCache::NumberOfCompiledCode() {
  MutexLock mu(Thread::Current(), lock_);
  return method_code_map_.size();
}

uint8_t* JitCodeCache::ReserveData(Thread* self, size_t size) {
  size = RoundUp(size, sizeof(void*));
  uint8_t* result = nullptr;

  {
    ScopedThreadSuspension sts(self, kSuspended);
    MutexLock mu(self, lock_);
    WaitForPotentialCollectionToComplete(self);
    result = reinterpret_cast<uint8_t*>(mspace_malloc(data_mspace_, size));
  }

  if (result == nullptr) {
    // Retry.
    GarbageCollectCache(self);
    ScopedThreadSuspension sts(self, kSuspended);
    MutexLock mu(self, lock_);
    WaitForPotentialCollectionToComplete(self);
    result = reinterpret_cast<uint8_t*>(mspace_malloc(data_mspace_, size));
  }

  return result;
}

uint8_t* JitCodeCache::AddDataArray(Thread* self, const uint8_t* begin, const uint8_t* end) {
  uint8_t* result = ReserveData(self, end - begin);
  if (result == nullptr) {
    return nullptr;  // Out of space in the data cache.
  }
  std::copy(begin, end, result);
  return result;
}

static uintptr_t FromCodeToAllocation(const void* code) {
  size_t alignment = GetInstructionSetAlignment(kRuntimeISA);
  return reinterpret_cast<uintptr_t>(code) - RoundUp(sizeof(OatQuickMethodHeader), alignment);
}

class MarkCodeVisitor FINAL : public StackVisitor {
 public:
  MarkCodeVisitor(Thread* thread_in, JitCodeCache* code_cache_in)
      : StackVisitor(thread_in, nullptr, StackVisitor::StackWalkKind::kSkipInlinedFrames),
        code_cache_(code_cache_in),
        bitmap_(code_cache_->GetLiveBitmap()) {}

  bool VisitFrame() OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    const OatQuickMethodHeader* method_header = GetCurrentOatQuickMethodHeader();
    if (method_header == nullptr) {
      return true;
    }
    const void* code = method_header->GetCode();
    if (code_cache_->ContainsPc(code)) {
      bitmap_->Set(FromCodeToAllocation(code));
    }
    return true;
  }

 private:
  JitCodeCache* const code_cache_;
  CodeCacheBitmap* const bitmap_;
};

class MarkCodeClosure FINAL : public Closure {
 public:
  MarkCodeClosure(JitCodeCache* code_cache, Barrier* barrier)
      : code_cache_(code_cache), barrier_(barrier) {}

  void Run(Thread* thread) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    MarkCodeVisitor visitor(thread, code_cache_);
    visitor.WalkStack();
    if (thread->GetState() == kRunnable) {
      barrier_->Pass(Thread::Current());
    }
  }

 private:
  JitCodeCache* const code_cache_;
  Barrier* const barrier_;
};

void JitCodeCache::GarbageCollectCache(Thread* self) {
  VLOG(jit) << "Clearing code cache, code="
            << PrettySize(CodeCacheSize())
            << ", data=" << PrettySize(DataCacheSize());

  size_t map_size = 0;
  ScopedThreadSuspension sts(self, kSuspended);
  {
    MutexLock mu(self, lock_);
    if (WaitForPotentialCollectionToComplete(self)) {
      return;
    }
    collection_in_progress_ = true;
    map_size = method_code_map_.size();
    for (auto& it : method_code_map_) {
      it.second->SetEntryPointFromQuickCompiledCode(GetQuickToInterpreterBridge());
    }
  }

  {
    Barrier barrier(0);
    MarkCodeClosure closure(this, &barrier);
    size_t threads_running_checkpoint =
        Runtime::Current()->GetThreadList()->RunCheckpoint(&closure);
    if (threads_running_checkpoint != 0) {
      barrier.Increment(self, threads_running_checkpoint);
    }
  }

  {
    MutexLock mu(self, lock_);
    DCHECK_EQ(map_size, method_code_map_.size());
    ScopedCodeCacheWrite scc(code_map_.get());
    for (auto it = method_code_map_.begin(); it != method_code_map_.end();) {
      const void* code_ptr = it->first;
      uintptr_t allocation = FromCodeToAllocation(code_ptr);
      ArtMethod* method = it->second;
      const OatQuickMethodHeader* method_header = OatQuickMethodHeader::FromCodePointer(code_ptr);
      if (GetLiveBitmap()->Test(allocation)) {
        method->SetEntryPointFromQuickCompiledCode(method_header->GetEntryPoint());
        ++it;
      } else {
        method->ClearCounter();
        DCHECK_NE(method->GetEntryPointFromQuickCompiledCode(), method_header->GetCode());
        const uint8_t* data = method_header->GetNativeGcMap();
        if (data != nullptr) {
          mspace_free(data_mspace_, const_cast<uint8_t*>(data));
        }
        data = method_header->GetMappingTable();
        if (data != nullptr) {
          mspace_free(data_mspace_, const_cast<uint8_t*>(data));
        }
        // Use the offset directly to prevent sanity check that the method is
        // compiled with optimizing.
        // TODO(ngeoffray): Clean up.
        if (method_header->vmap_table_offset_ != 0) {
          data = method_header->code_ - method_header->vmap_table_offset_;
          mspace_free(data_mspace_, const_cast<uint8_t*>(data));
        }
        mspace_free(code_mspace_, reinterpret_cast<uint8_t*>(allocation));
        it = method_code_map_.erase(it);
      }
    }
    collection_in_progress_ = false;
    lock_cond_.Broadcast(self);
  }

  VLOG(jit) << "After clearing code cache, code="
            << PrettySize(CodeCacheSize())
            << ", data=" << PrettySize(DataCacheSize());
}


OatQuickMethodHeader* JitCodeCache::LookupMethodHeader(uintptr_t pc, ArtMethod* method) {
  static_assert(kRuntimeISA != kThumb2, "kThumb2 cannot be a runtime ISA");
  if (kRuntimeISA == kArm) {
    // On Thumb-2, the pc is offset by one.
    --pc;
  }
  if (!ContainsPc(reinterpret_cast<const void*>(pc))) {
    return nullptr;
  }

  MutexLock mu(Thread::Current(), lock_);
  if (method_code_map_.empty()) {
    return nullptr;
  }
  auto it = method_code_map_.lower_bound(reinterpret_cast<const void*>(pc));
  --it;

  const void* code_ptr = it->first;
  OatQuickMethodHeader* method_header = OatQuickMethodHeader::FromCodePointer(code_ptr);
  if (!method_header->Contains(pc)) {
    return nullptr;
  }
  DCHECK_EQ(it->second, method)
      << PrettyMethod(method) << " " << PrettyMethod(it->second) << " " << std::hex << pc;
  return method_header;
}

}  // namespace jit
}  // namespace art
