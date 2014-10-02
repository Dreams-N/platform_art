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

#include "mir_field_info.h"

#include <string.h>

#include "base/logging.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_driver-inl.h"
#include "mirror/class_loader.h"  // Only to allow casts in Handle<ClassLoader>.
#include "mirror/dex_cache.h"     // Only to allow casts in Handle<DexCache>.
#include "scoped_thread_state_change.h"
#include "handle_scope-inl.h"

namespace art {

void MirIFieldLoweringInfo::Resolve(CompilerDriver* compiler_driver,
                                    const DexCompilationUnit* mUnit,
                                    MirIFieldLoweringInfo* field_infos, size_t count) {
  if (kIsDebugBuild) {
    DCHECK(field_infos != nullptr);
    DCHECK_NE(count, 0u);
    for (auto it = field_infos, end = field_infos + count; it != end; ++it) {
      MirIFieldLoweringInfo unresolved(it->field_idx_, it->MemAccessType());
      unresolved.declaring_dex_file_ = it->declaring_dex_file_;
      unresolved.CheckEquals(*it);
    }
  }

  // We're going to resolve fields and check access in a tight loop. It's better to hold
  // the lock and needed references once than re-acquiring them again and again.
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(compiler_driver->GetDexCache(mUnit)));
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(compiler_driver->GetClassLoader(soa, mUnit)));
  Handle<mirror::Class> referrer_class(hs.NewHandle(
      compiler_driver->ResolveCompilingMethodsClass(soa, dex_cache, class_loader, mUnit)));
  // Even if the referrer class is unresolved (i.e. we're compiling a method without class
  // definition) we still want to resolve fields and record all available info.

  for (auto it = field_infos, end = field_infos + count; it != end; ++it) {
    const uint32_t field_idx = it->field_idx_;
    mirror::ArtField* resolved_field;
    if (it->declaring_dex_file_ == nullptr ||
        it->declaring_dex_file_ == mUnit->GetDexFile()) {
      resolved_field =
          compiler_driver->ResolveField(soa, dex_cache, class_loader, mUnit, field_idx, false);
    } else {
      StackHandleScope<1> hs2(soa.Self());
      auto* const cl = mUnit->GetClassLinker();
      auto h_dex_cache = hs2.NewHandle(cl->FindDexCache(*it->declaring_dex_file_));
      resolved_field =
          cl->ResolveField(*it->declaring_dex_file_, field_idx, h_dex_cache, class_loader, false);
      if (resolved_field == nullptr) {
        soa.Self()->ClearException();
      } else if (UNLIKELY(resolved_field->IsStatic() != false)) {
        resolved_field = nullptr;
      }
    }
    if (UNLIKELY(resolved_field == nullptr)) {
      it->declaring_dex_file_ = nullptr;
      continue;
    }
    compiler_driver->GetResolvedFieldDexFileLocation(resolved_field,
        &it->declaring_dex_file_, &it->declaring_class_idx_, &it->declaring_field_idx_);
    bool is_volatile = compiler_driver->IsFieldVolatile(resolved_field);
    it->field_offset_ = compiler_driver->GetFieldOffset(resolved_field);
    std::pair<bool, bool> fast_path = compiler_driver->IsFastInstanceField(
        dex_cache.Get(), referrer_class.Get(), resolved_field, field_idx);
    it->flags_ = 0u |  // Without kFlagIsStatic.
        (it->flags_ & (kMemAccessTypeMask << kBitMemAccessTypeBegin)) |
        (is_volatile ? kFlagIsVolatile : 0u) |
        (fast_path.first ? kFlagFastGet : 0u) |
        (fast_path.second ? kFlagFastPut : 0u);
  }
}

void MirSFieldLoweringInfo::Resolve(CompilerDriver* compiler_driver,
                                    const DexCompilationUnit* mUnit,
                                    MirSFieldLoweringInfo* field_infos, size_t count) {
  if (kIsDebugBuild) {
    DCHECK(field_infos != nullptr);
    DCHECK_NE(count, 0u);
    for (auto it = field_infos, end = field_infos + count; it != end; ++it) {
      MirSFieldLoweringInfo unresolved(it->field_idx_, it->MemAccessType());
      // In 64-bit builds, there's padding after storage_index_, don't include it in memcmp.
      size_t size = OFFSETOF_MEMBER(MirSFieldLoweringInfo, storage_index_) +
          sizeof(it->storage_index_);
      DCHECK_EQ(memcmp(&unresolved, &*it, size), 0);
    }
  }

  // We're going to resolve fields and check access in a tight loop. It's better to hold
  // the lock and needed references once than re-acquiring them again and again.
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<3> hs(soa.Self());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(compiler_driver->GetDexCache(mUnit)));
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(compiler_driver->GetClassLoader(soa, mUnit)));
  Handle<mirror::Class> referrer_class_handle(hs.NewHandle(
      compiler_driver->ResolveCompilingMethodsClass(soa, dex_cache, class_loader, mUnit)));
  // Even if the referrer class is unresolved (i.e. we're compiling a method without class
  // definition) we still want to resolve fields and record all available info.

  for (auto it = field_infos, end = field_infos + count; it != end; ++it) {
    uint32_t field_idx = it->field_idx_;
    mirror::ArtField* resolved_field =
        compiler_driver->ResolveField(soa, dex_cache, class_loader, mUnit, field_idx, true);
    if (UNLIKELY(resolved_field == nullptr)) {
      continue;
    }
    compiler_driver->GetResolvedFieldDexFileLocation(resolved_field,
        &it->declaring_dex_file_, &it->declaring_class_idx_, &it->declaring_field_idx_);
    bool is_volatile = compiler_driver->IsFieldVolatile(resolved_field) ? 1u : 0u;

    mirror::Class* referrer_class = referrer_class_handle.Get();
    std::pair<bool, bool> fast_path = compiler_driver->IsFastStaticField(
        dex_cache.Get(), referrer_class, resolved_field, field_idx, &it->storage_index_);
    uint16_t flags = kFlagIsStatic |
        (it->flags_ & (kMemAccessTypeMask << kBitMemAccessTypeBegin)) |
        (is_volatile ? kFlagIsVolatile : 0u) |
        (fast_path.first ? kFlagFastGet : 0u) |
        (fast_path.second ? kFlagFastPut : 0u);
    if (fast_path.first) {
      it->field_offset_ = compiler_driver->GetFieldOffset(resolved_field);
      bool is_referrers_class =
          compiler_driver->IsStaticFieldInReferrerClass(referrer_class, resolved_field);
      bool is_class_initialized =
          compiler_driver->IsStaticFieldsClassInitialized(referrer_class, resolved_field);
      bool is_class_in_dex_cache = !is_referrers_class &&  // If referrer's class, we don't care.
          compiler_driver->CanAssumeTypeIsPresentInDexCache(*dex_cache->GetDexFile(),
                                                            it->storage_index_);
      flags |= (is_referrers_class ? kFlagIsReferrersClass : 0u) |
          (is_class_initialized ? kFlagClassIsInitialized : 0u) |
          (is_class_in_dex_cache ? kFlagClassIsInDexCache : 0u);
    }
    it->flags_ = flags;
  }
}

}  // namespace art
