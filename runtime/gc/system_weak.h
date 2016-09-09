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

#ifndef ART_RUNTIME_GC_SYSTEM_WEAK_H_
#define ART_RUNTIME_GC_SYSTEM_WEAK_H_

#include "base/mutex.h"
#include "object_callbacks.h"
#include "thread-inl.h"

namespace art {
namespace gc {

class AbstractSystemWeakHolder {
 public:
  virtual ~AbstractSystemWeakHolder() {}

  virtual void AllowNewSystemWeaks() REQUIRES_SHARED(Locks::mutator_lock_) = 0;
  virtual void DisallowNewSystemWeaks() REQUIRES_SHARED(Locks::mutator_lock_) = 0;
  virtual void BroadcastForNewSystemWeaks() REQUIRES_SHARED(Locks::mutator_lock_) = 0;

  virtual void SweepWeaks(IsMarkedVisitor* visitor) REQUIRES_SHARED(Locks::mutator_lock_) = 0;
};

class SystemWeakHolder : public AbstractSystemWeakHolder {
 public:
  explicit SystemWeakHolder(LockLevel level)
      : allow_disallow_lock_("SystemWeakHolder", level),
        new_weak_condition_("SystemWeakHolder new condition", allow_disallow_lock_),
        allow_new_system_weak_(true) {
  }
  virtual ~SystemWeakHolder() {}

  void AllowNewSystemWeaks() OVERRIDE
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!allow_disallow_lock_) {
    CHECK(!kUseReadBarrier);
    MutexLock mu(Thread::Current(), allow_disallow_lock_);
    allow_new_system_weak_ = true;
    new_weak_condition_.Broadcast(Thread::Current());
  }

  void DisallowNewSystemWeaks() OVERRIDE
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!allow_disallow_lock_) {
    CHECK(!kUseReadBarrier);
    MutexLock mu(Thread::Current(), allow_disallow_lock_);
    allow_new_system_weak_ = false;
  }

  void BroadcastForNewSystemWeaks() OVERRIDE
      REQUIRES_SHARED(Locks::mutator_lock_)
      REQUIRES(!allow_disallow_lock_) {
    CHECK(kUseReadBarrier);
    MutexLock mu(Thread::Current(), allow_disallow_lock_);
    new_weak_condition_.Broadcast(Thread::Current());
  }

  Mutex allow_disallow_lock_;

 protected:
  void WaitForAllowance(Thread* self) REQUIRES_SHARED(allow_disallow_lock_) {
    // Wait for GC's sweeping to complete and allow new records
    while (UNLIKELY((!kUseReadBarrier && !allow_new_system_weak_) ||
                    (kUseReadBarrier && !self->GetWeakRefAccessEnabled()))) {
      new_weak_condition_.WaitHoldingLocks(self);
    }
  }

  ConditionVariable new_weak_condition_ GUARDED_BY(allow_disallow_lock_);
  bool allow_new_system_weak_ GUARDED_BY(allow_disallow_lock_);
};

}  // namespace gc
}  // namespace art

#endif  // ART_RUNTIME_GC_SYSTEM_WEAK_H_
