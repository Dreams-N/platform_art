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

#include "task_processor.h"

#include "scoped_thread_state_change.h"

namespace art {
namespace gc {

TaskProcessor::TaskProcessor() : lock_(new Mutex("Task processor lock")), is_running_(true) {
  cond_.reset(new ConditionVariable("Task processor condition", *lock_));
}

TaskProcessor::~TaskProcessor() {
  delete lock_;
}

void TaskProcessor::AddTask(Thread* self, HeapTask* task) {
  ScopedThreadStateChange tsc(self, kBlocked);
  MutexLock mu(self, *lock_);
  tasks_.push(task);
  cond_->Signal(self);
}

HeapTask* TaskProcessor::GetTask(Thread* self) {
  ScopedThreadStateChange tsc(self, kBlocked);
  MutexLock mu(self, *lock_);
  while (true) {
    if (tasks_.empty()) {
      if (!is_running_) {
        return nullptr;
      }
      cond_->Wait(self);  // Empty queue, wait until we are signalled.
    } else {
      // Non empty queue, look at the top element and see if we are ready to run it.
      const uint64_t current_time = NanoTime();
      HeapTask* task = tasks_.top();
      // If we are shutting down, return the task right away without waiting. Otherwise return the
      // task if it is late enough.
      uint64_t target_time = task->GetTargetRunTime();
      if (!is_running_ || target_time <= current_time) {
        tasks_.pop();
        return task;
      }
      // Check if the target time was updated, if so re-insert then wait.
      if (target_time != task->GetUpdatedTargetTime()) {
        tasks_.pop();
        task->UpdateTargetTime();
        tasks_.push(task);
        target_time = task->GetTargetRunTime();
      }
      DCHECK_GT(target_time, current_time);
      // Wait untl we hit the target run time.
      const uint64_t delta_time = target_time - current_time;
      const uint64_t ms_delta = NsToMs(delta_time);
      const uint64_t ns_delta = delta_time - MsToNs(ms_delta);
      cond_->TimedWait(self, static_cast<int64_t>(ms_delta), static_cast<int32_t>(ns_delta));
    }
  }
  return nullptr;
}

bool TaskProcessor::IsRunning() const {
  MutexLock mu(Thread::Current(), *lock_);
  return is_running_;
}

void TaskProcessor::Interrupt(Thread* self) {
  MutexLock mu(self, *lock_);
  is_running_ = false;
  cond_->Broadcast(self);
}

void TaskProcessor::RunTasksUntilInterrupted(Thread* self) {
  {
    MutexLock mu(self, *lock_);
    is_running_ = true;
  }
  while (true) {
    // Wait and get a task, may be interrupted.
    HeapTask* task = GetTask(self);
    if (task != nullptr) {
      task->Run(self);
    } else if (!IsRunning()) {
      break;
    }
  }
}

}  // namespace gc
}  // namespace art
