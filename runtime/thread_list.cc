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

#include "thread_list.h"

#include <dirent.h>
#include <ScopedLocalRef.h>
#include <ScopedUtfChars.h>
#include <sys/types.h>
#include <unistd.h>

#include "base/mutex.h"
#include "base/mutex-inl.h"
#include "base/timing_logger.h"
#include "debugger.h"
#include "jni_internal.h"
#include "lock_word.h"
#include "monitor.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "utils.h"
#include "well_known_classes.h"

namespace art {

ThreadList::ThreadList()
    : allocated_ids_lock_("allocated thread ids lock"),
      suspend_all_count_(0), debug_suspend_all_count_(0),
      thread_exit_cond_("thread exit condition variable", *Locks::thread_list_lock_) {
  CHECK(Monitor::IsValidLockWord(LockWord::FromThinLockId(kMaxThreadId, 1)));
}

ThreadList::~ThreadList() {
  // Detach the current thread if necessary. If we failed to start, there might not be any threads.
  // We need to detach the current thread here in case there's another thread waiting to join with
  // us.
  if (Contains(Thread::Current())) {
    Runtime::Current()->DetachCurrentThread();
  }

  WaitForOtherNonDaemonThreadsToExit();
  // TODO: there's an unaddressed race here where a thread may attach during shutdown, see
  //       Thread::Init.
  SuspendAllDaemonThreads();
}

bool ThreadList::Contains(Thread* thread) {
  return find(list_.begin(), list_.end(), thread) != list_.end();
}

bool ThreadList::Contains(pid_t tid) {
  for (const auto& thread : list_) {
    if (thread->tid_ == tid) {
      return true;
    }
  }
  return false;
}

pid_t ThreadList::GetLockOwner() {
  return Locks::thread_list_lock_->GetExclusiveOwnerTid();
}

void ThreadList::DumpNativeStacks(std::ostream& os) {
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  for (const auto& thread : list_) {
    os << "DUMPING THREAD " << thread->tid_ << "\n";
    DumpNativeStack(os, thread->tid_, "\t", true);
    os << "\n";
  }
}

void ThreadList::DumpForSigQuit(std::ostream& os) {
  {
    MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
    DumpLocked(os);
  }
  DumpUnattachedThreads(os);
}

static void DumpUnattachedThread(std::ostream& os, pid_t tid) NO_THREAD_SAFETY_ANALYSIS {
  // TODO: No thread safety analysis as DumpState with a NULL thread won't access fields, should
  // refactor DumpState to avoid skipping analysis.
  Thread::DumpState(os, NULL, tid);
  DumpKernelStack(os, tid, "  kernel: ", false);
  // TODO: Reenable this when the native code in system_server can handle it.
  // Currently "adb shell kill -3 `pid system_server`" will cause it to exit.
  if (false) {
    DumpNativeStack(os, tid, "  native: ", false);
  }
  os << "\n";
}

void ThreadList::DumpUnattachedThreads(std::ostream& os) {
  DIR* d = opendir("/proc/self/task");
  if (!d) {
    return;
  }

  Thread* self = Thread::Current();
  dirent* e;
  while ((e = readdir(d)) != NULL) {
    char* end;
    pid_t tid = strtol(e->d_name, &end, 10);
    if (!*end) {
      bool contains;
      {
        MutexLock mu(self, *Locks::thread_list_lock_);
        contains = Contains(tid);
      }
      if (!contains) {
        DumpUnattachedThread(os, tid);
      }
    }
  }
  closedir(d);
}

void ThreadList::DumpLocked(std::ostream& os) {
  os << "DALVIK THREADS (" << list_.size() << "):\n";
  for (const auto& thread : list_) {
    thread->Dump(os);
    os << "\n";
  }
}

void ThreadList::AssertThreadsAreSuspended(Thread* self, Thread* ignore1, Thread* ignore2) {
  MutexLock mu(self, *Locks::thread_list_lock_);
  MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
  for (const auto& thread : list_) {
    if (thread != ignore1 && thread != ignore2) {
      CHECK(thread->IsSuspended())
            << "\nUnsuspended thread: <<" << *thread << "\n"
            << "self: <<" << *Thread::Current();
    }
  }
}

#if HAVE_TIMED_RWLOCK
// Attempt to rectify locks so that we dump thread list with required locks before exiting.
static void UnsafeLogFatalForThreadSuspendAllTimeout(Thread* self) NO_THREAD_SAFETY_ANALYSIS __attribute__((noreturn));
static void UnsafeLogFatalForThreadSuspendAllTimeout(Thread* self) {
  Runtime* runtime = Runtime::Current();
  std::ostringstream ss;
  ss << "Thread suspend timeout\n";
  runtime->DumpLockHolders(ss);
  ss << "\n";
  runtime->GetThreadList()->DumpLocked(ss);
  LOG(FATAL) << ss.str();
  exit(0);
}
#endif

// Unlike suspending all threads where we can wait to acquire the mutator_lock_, suspending an
// individual thread requires polling. delay_us is the requested sleep and total_delay_us
// accumulates the total time spent sleeping for timeouts. The first sleep is just a yield,
// subsequently sleeps increase delay_us from 1ms to 500ms by doubling.
static void ThreadSuspendSleep(Thread* self, useconds_t* delay_us, useconds_t* total_delay_us,
                               bool holding_locks) {
  if (!holding_locks) {
    for (int i = kLockLevelCount - 1; i >= 0; --i) {
      BaseMutex* held_mutex = self->GetHeldMutex(static_cast<LockLevel>(i));
      if (held_mutex != NULL) {
        LOG(FATAL) << "Holding " << held_mutex->GetName() << " while sleeping for thread suspension";
      }
    }
  }
  useconds_t new_delay_us = (*delay_us) * 2;
  CHECK_GE(new_delay_us, *delay_us);
  if (new_delay_us < 500000) {  // Don't allow sleeping to be more than 0.5s.
    *delay_us = new_delay_us;
  }
  if (*delay_us == 0) {
    sched_yield();
    // Default to 1 milliseconds (note that this gets multiplied by 2 before the first sleep).
    *delay_us = 500;
  } else {
    usleep(*delay_us);
    *total_delay_us += *delay_us;
  }
}

size_t ThreadList::RunCheckpoint(Closure* checkpoint_function) {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotExclusiveHeld(self);
  Locks::thread_list_lock_->AssertNotHeld(self);
  Locks::thread_suspend_count_lock_->AssertNotHeld(self);
  if (kDebugLocking) {
    CHECK_NE(self->GetState(), kRunnable);
  }

  std::vector<Thread*> suspended_count_modified_threads;
  size_t count = 0;
  {
    // Call a checkpoint function for each thread, threads which are suspend get their checkpoint
    // manually called.
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    for (const auto& thread : list_) {
      if (thread != self) {
        while (true) {
          if (thread->RequestCheckpoint(checkpoint_function)) {
            // This thread will run its checkpoint some time in the near future.
            count++;
            break;
          } else {
            // We are probably suspended, try to make sure that we stay suspended.
            // The thread switched back to runnable.
            if (thread->GetState() == kRunnable) {
              // Spurious fail, try again.
              continue;
            }
            thread->ModifySuspendCount(self, +1, false);
            suspended_count_modified_threads.push_back(thread);
            break;
          }
        }
      }
    }
  }

  // Run the checkpoint on ourself while we wait for threads to suspend.
  checkpoint_function->Run(self);

  // Run the checkpoint on the suspended threads.
  for (const auto& thread : suspended_count_modified_threads) {
    if (!thread->IsSuspended()) {
      // Wait until the thread is suspended.
      useconds_t total_delay_us = 0;
      do {
        useconds_t delay_us = 100;
        ThreadSuspendSleep(self, &delay_us, &total_delay_us, true);
      } while (!thread->IsSuspended());
      // Shouldn't need to wait for longer than 1000 microseconds.
      constexpr useconds_t kLongWaitThresholdUS = 1000;
      if (UNLIKELY(total_delay_us > kLongWaitThresholdUS)) {
        LOG(WARNING) << "Waited " << total_delay_us << " us for thread suspend!";
      }
    }
    // We know for sure that the thread is suspended at this point.
    checkpoint_function->Run(thread);
    {
      MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
      thread->ModifySuspendCount(self, -1, false);
    }
  }

  {
    // Imitate ResumeAll, threads may be waiting on Thread::resume_cond_ since we raised their
    // suspend count. Now the suspend_count_ is lowered so we must do the broadcast.
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    Thread::resume_cond_->Broadcast(self);
  }

  // Add one for self.
  return count + suspended_count_modified_threads.size() + 1;
}

// Request that a checkpoint function be run on all active (non-suspended)
// threads.  Returns the number of successful requests.
size_t ThreadList::RunCheckpointOnRunnableThreads(Closure* checkpoint_function) {
  Thread* self = Thread::Current();
  if (kIsDebugBuild) {
    Locks::mutator_lock_->AssertNotExclusiveHeld(self);
    Locks::thread_list_lock_->AssertNotHeld(self);
    Locks::thread_suspend_count_lock_->AssertNotHeld(self);
    CHECK_NE(self->GetState(), kRunnable);
  }

  size_t count = 0;
  {
    // Call a checkpoint function for each non-suspended thread.
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    for (const auto& thread : list_) {
      if (thread != self) {
        if (thread->RequestCheckpoint(checkpoint_function)) {
          // This thread will run its checkpoint some time in the near future.
          count++;
        }
      }
    }
  }

  // Return the number of threads that will run the checkpoint function.
  return count;
}

void ThreadList::SuspendAll() {
  Thread* self = Thread::Current();
  DCHECK(self != nullptr);

  VLOG(threads) << *self << " SuspendAll starting...";

  Locks::mutator_lock_->AssertNotHeld(self);
  Locks::thread_list_lock_->AssertNotHeld(self);
  Locks::thread_suspend_count_lock_->AssertNotHeld(self);
  if (kDebugLocking) {
    CHECK_NE(self->GetState(), kRunnable);
  }
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    // Update global suspend all state for attaching threads.
    ++suspend_all_count_;
    // Increment everybody's suspend count (except our own).
    for (const auto& thread : list_) {
      if (thread == self) {
        continue;
      }
      VLOG(threads) << "requesting thread suspend: " << *thread;
      thread->ModifySuspendCount(self, +1, false);
    }
  }

  // Block on the mutator lock until all Runnable threads release their share of access.
#if HAVE_TIMED_RWLOCK
  // Timeout if we wait more than 30 seconds.
  if (!Locks::mutator_lock_->ExclusiveLockWithTimeout(self, 30 * 1000, 0)) {
    UnsafeLogFatalForThreadSuspendAllTimeout(self);
  }
#else
  Locks::mutator_lock_->ExclusiveLock(self);
#endif

  if (kDebugLocking) {
    // Debug check that all threads are suspended.
    AssertThreadsAreSuspended(self, self);
  }

  VLOG(threads) << *self << " SuspendAll complete";
}

void ThreadList::ResumeAll() {
  Thread* self = Thread::Current();

  VLOG(threads) << *self << " ResumeAll starting";

  if (kDebugLocking) {
    // Debug check that all threads are suspended.
    AssertThreadsAreSuspended(self, self);
  }

  Locks::mutator_lock_->ExclusiveUnlock(self);
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    // Update global suspend all state for attaching threads.
    --suspend_all_count_;
    // Decrement the suspend counts for all threads.
    for (const auto& thread : list_) {
      if (thread == self) {
        continue;
      }
      thread->ModifySuspendCount(self, -1, false);
    }

    // Broadcast a notification to all suspended threads, some or all of
    // which may choose to wake up.  No need to wait for them.
    VLOG(threads) << *self << " ResumeAll waking others";
    Thread::resume_cond_->Broadcast(self);
  }
  VLOG(threads) << *self << " ResumeAll complete";
}

void ThreadList::Resume(Thread* thread, bool for_debugger) {
  Thread* self = Thread::Current();
  DCHECK_NE(thread, self);
  VLOG(threads) << "Resume(" << *thread << ") starting..." << (for_debugger ? " (debugger)" : "");

  {
    // To check Contains.
    MutexLock mu(self, *Locks::thread_list_lock_);
    // To check IsSuspended.
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    DCHECK(thread->IsSuspended());
    if (!Contains(thread)) {
      return;
    }
    thread->ModifySuspendCount(self, -1, for_debugger);
  }

  {
    VLOG(threads) << "Resume(" << *thread << ") waking others";
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    Thread::resume_cond_->Broadcast(self);
  }

  VLOG(threads) << "Resume(" << *thread << ") complete";
}

static void ThreadSuspendByPeerWarning(Thread* self, int level, const char* message, jobject peer) {
  JNIEnvExt* env = self->GetJniEnv();
  ScopedLocalRef<jstring>
      scoped_name_string(env, (jstring)env->GetObjectField(peer,
                                                          WellKnownClasses::java_lang_Thread_name));
  ScopedUtfChars scoped_name_chars(env, scoped_name_string.get());
  if (scoped_name_chars.c_str() == NULL) {
      LOG(level) << message << ": " << peer;
      env->ExceptionClear();
  } else {
      LOG(level) << message << ": " << peer << ":" << scoped_name_chars.c_str();
  }
}

Thread* ThreadList::SuspendThreadByPeer(jobject peer, bool request_suspension,
                                        bool debug_suspension, bool* timed_out) {
  static const useconds_t kTimeoutUs = 30 * 1000000;  // 30s.
  useconds_t total_delay_us = 0;
  useconds_t delay_us = 0;
  bool did_suspend_request = false;
  *timed_out = false;
  Thread* self = Thread::Current();
  while (true) {
    Thread* thread;
    {
      ScopedObjectAccess soa(self);
      MutexLock mu(self, *Locks::thread_list_lock_);
      thread = Thread::FromManagedThread(soa, peer);
      if (thread == NULL) {
        ThreadSuspendByPeerWarning(self, WARNING, "No such thread for suspend", peer);
        return NULL;
      }
      {
        MutexLock mu(self, *Locks::thread_suspend_count_lock_);
        if (request_suspension) {
          thread->ModifySuspendCount(self, +1, debug_suspension);
          request_suspension = false;
          did_suspend_request = true;
        } else {
          // If the caller isn't requesting suspension, a suspension should have already occurred.
          CHECK_GT(thread->GetSuspendCount(), 0);
        }
        // IsSuspended on the current thread will fail as the current thread is changed into
        // Runnable above. As the suspend count is now raised if this is the current thread
        // it will self suspend on transition to Runnable, making it hard to work with. It's simpler
        // to just explicitly handle the current thread in the callers to this code.
        CHECK_NE(thread, self) << "Attempt to suspend the current thread for the debugger";
        // If thread is suspended (perhaps it was already not Runnable but didn't have a suspend
        // count, or else we've waited and it has self suspended) or is the current thread, we're
        // done.
        if (thread->IsSuspended()) {
          return thread;
        }
        if (total_delay_us >= kTimeoutUs) {
          ThreadSuspendByPeerWarning(self, FATAL, "Thread suspension timed out", peer);
          if (did_suspend_request) {
            thread->ModifySuspendCount(soa.Self(), -1, debug_suspension);
          }
          *timed_out = true;
          return NULL;
        }
      }
      // Release locks and come out of runnable state.
    }
    ThreadSuspendSleep(self, &delay_us, &total_delay_us, false);
  }
}

static void ThreadSuspendByThreadIdWarning(int level, const char* message, uint32_t thread_id) {
  LOG(level) << StringPrintf("%s: %d", message, thread_id);
}

Thread* ThreadList::SuspendThreadByThreadId(uint32_t thread_id, bool debug_suspension,
                                            bool* timed_out) {
  static const useconds_t kTimeoutUs = 30 * 1000000;  // 30s.
  useconds_t total_delay_us = 0;
  useconds_t delay_us = 0;
  *timed_out = false;
  Thread* suspended_thread = nullptr;
  Thread* self = Thread::Current();
  CHECK_NE(thread_id, kInvalidThreadId);
  while (true) {
    {
      Thread* thread = NULL;
      ScopedObjectAccess soa(self);
      MutexLock mu(self, *Locks::thread_list_lock_);
      for (const auto& it : list_) {
        if (it->GetThreadId() == thread_id) {
          thread = it;
          break;
        }
      }
      if (thread == nullptr) {
        CHECK(suspended_thread == nullptr) << "Suspended thread " << suspended_thread
            << " no longer in thread list";
        // There's a race in inflating a lock and the owner giving up ownership and then dying.
        ThreadSuspendByThreadIdWarning(WARNING, "No such thread id for suspend", thread_id);
        return NULL;
      }
      {
        MutexLock mu(self, *Locks::thread_suspend_count_lock_);
        if (suspended_thread == nullptr) {
          thread->ModifySuspendCount(self, +1, debug_suspension);
          suspended_thread = thread;
        } else {
          CHECK_EQ(suspended_thread, thread);
          // If the caller isn't requesting suspension, a suspension should have already occurred.
          CHECK_GT(thread->GetSuspendCount(), 0);
        }
        // IsSuspended on the current thread will fail as the current thread is changed into
        // Runnable above. As the suspend count is now raised if this is the current thread
        // it will self suspend on transition to Runnable, making it hard to work with. It's simpler
        // to just explicitly handle the current thread in the callers to this code.
        CHECK_NE(thread, self) << "Attempt to suspend the current thread for the debugger";
        // If thread is suspended (perhaps it was already not Runnable but didn't have a suspend
        // count, or else we've waited and it has self suspended) or is the current thread, we're
        // done.
        if (thread->IsSuspended()) {
          return thread;
        }
        if (total_delay_us >= kTimeoutUs) {
          ThreadSuspendByThreadIdWarning(WARNING, "Thread suspension timed out", thread_id);
          if (suspended_thread != nullptr) {
            thread->ModifySuspendCount(soa.Self(), -1, debug_suspension);
          }
          *timed_out = true;
          return NULL;
        }
      }
      // Release locks and come out of runnable state.
    }
    ThreadSuspendSleep(self, &delay_us, &total_delay_us, false);
  }
}

Thread* ThreadList::FindThreadByThreadId(uint32_t thin_lock_id) {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::thread_list_lock_);
  for (const auto& thread : list_) {
    if (thread->GetThreadId() == thin_lock_id) {
      CHECK(thread == self || thread->IsSuspended());
      return thread;
    }
  }
  return NULL;
}

void ThreadList::SuspendAllForDebugger() {
  Thread* self = Thread::Current();
  Thread* debug_thread = Dbg::GetDebugThread();

  VLOG(threads) << *self << " SuspendAllForDebugger starting...";

  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    {
      MutexLock mu(self, *Locks::thread_suspend_count_lock_);
      // Update global suspend all state for attaching threads.
      ++suspend_all_count_;
      ++debug_suspend_all_count_;
      // Increment everybody's suspend count (except our own).
      for (const auto& thread : list_) {
        if (thread == self || thread == debug_thread) {
          continue;
        }
        VLOG(threads) << "requesting thread suspend: " << *thread;
        thread->ModifySuspendCount(self, +1, true);
      }
    }
  }

  // Block on the mutator lock until all Runnable threads release their share of access then
  // immediately unlock again.
#if HAVE_TIMED_RWLOCK
  // Timeout if we wait more than 30 seconds.
  if (!Locks::mutator_lock_->ExclusiveLockWithTimeout(self, 30 * 1000, 0)) {
    UnsafeLogFatalForThreadSuspendAllTimeout(self);
  } else {
    Locks::mutator_lock_->ExclusiveUnlock(self);
  }
#else
  Locks::mutator_lock_->ExclusiveLock(self);
  Locks::mutator_lock_->ExclusiveUnlock(self);
#endif
  AssertThreadsAreSuspended(self, self, debug_thread);

  VLOG(threads) << *self << " SuspendAll complete";
}

void ThreadList::SuspendSelfForDebugger() {
  Thread* self = Thread::Current();

  // The debugger thread must not suspend itself due to debugger activity!
  Thread* debug_thread = Dbg::GetDebugThread();
  CHECK(debug_thread != NULL);
  CHECK(self != debug_thread);
  CHECK_NE(self->GetState(), kRunnable);
  Locks::mutator_lock_->AssertNotHeld(self);

  {
    // Collisions with other suspends aren't really interesting. We want
    // to ensure that we're the only one fiddling with the suspend count
    // though.
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    self->ModifySuspendCount(self, +1, true);
    CHECK_GT(self->suspend_count_, 0);
  }

  VLOG(threads) << *self << " self-suspending (debugger)";

  // Tell JDWP we've completed invocation and are ready to suspend.
  DebugInvokeReq* pReq = self->GetInvokeReq();
  DCHECK(pReq != NULL);
  if (pReq->invoke_needed) {
    // Clear this before signaling.
    pReq->invoke_needed = false;

    VLOG(jdwp) << "invoke complete, signaling";
    MutexLock mu(self, pReq->lock);
    pReq->cond.Signal(self);
  }

  // Tell JDWP that we've completed suspension. The JDWP thread can't
  // tell us to resume before we're fully asleep because we hold the
  // suspend count lock.
  Dbg::ClearWaitForEventThread();

  {
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    while (self->suspend_count_ != 0) {
      Thread::resume_cond_->Wait(self);
      if (self->suspend_count_ != 0) {
        // The condition was signaled but we're still suspended. This
        // can happen if the debugger lets go while a SIGQUIT thread
        // dump event is pending (assuming SignalCatcher was resumed for
        // just long enough to try to grab the thread-suspend lock).
        LOG(DEBUG) << *self << " still suspended after undo "
                   << "(suspend count=" << self->suspend_count_ << ")";
      }
    }
    CHECK_EQ(self->suspend_count_, 0);
  }

  VLOG(threads) << *self << " self-reviving (debugger)";
}

void ThreadList::UndoDebuggerSuspensions() {
  Thread* self = Thread::Current();

  VLOG(threads) << *self << " UndoDebuggerSuspensions starting";

  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    // Update global suspend all state for attaching threads.
    suspend_all_count_ -= debug_suspend_all_count_;
    debug_suspend_all_count_ = 0;
    // Update running threads.
    for (const auto& thread : list_) {
      if (thread == self || thread->debug_suspend_count_ == 0) {
        continue;
      }
      thread->ModifySuspendCount(self, -thread->debug_suspend_count_, true);
    }
  }

  {
    MutexLock mu(self, *Locks::thread_suspend_count_lock_);
    Thread::resume_cond_->Broadcast(self);
  }

  VLOG(threads) << "UndoDebuggerSuspensions(" << *self << ") complete";
}

void ThreadList::WaitForOtherNonDaemonThreadsToExit() {
  Thread* self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  bool all_threads_are_daemons;
  do {
    {
      // No more threads can be born after we start to shutdown.
      MutexLock mu(self, *Locks::runtime_shutdown_lock_);
      CHECK(Runtime::Current()->IsShuttingDownLocked());
      CHECK_EQ(Runtime::Current()->NumberOfThreadsBeingBorn(), 0U);
    }
    all_threads_are_daemons = true;
    MutexLock mu(self, *Locks::thread_list_lock_);
    for (const auto& thread : list_) {
      if (thread != self && !thread->IsDaemon()) {
        all_threads_are_daemons = false;
        break;
      }
    }
    if (!all_threads_are_daemons) {
      // Wait for another thread to exit before re-checking.
      thread_exit_cond_.Wait(self);
    }
  } while (!all_threads_are_daemons);
}

void ThreadList::SuspendAllDaemonThreads() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::thread_list_lock_);
  {  // Tell all the daemons it's time to suspend.
    MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
    for (const auto& thread : list_) {
      // This is only run after all non-daemon threads have exited, so the remainder should all be
      // daemons.
      CHECK(thread->IsDaemon()) << *thread;
      if (thread != self) {
        thread->ModifySuspendCount(self, +1, false);
      }
    }
  }
  // Give the threads a chance to suspend, complaining if they're slow.
  bool have_complained = false;
  for (int i = 0; i < 10; ++i) {
    usleep(200 * 1000);
    bool all_suspended = true;
    for (const auto& thread : list_) {
      if (thread != self && thread->GetState() == kRunnable) {
        if (!have_complained) {
          LOG(WARNING) << "daemon thread not yet suspended: " << *thread;
          have_complained = true;
        }
        all_suspended = false;
      }
    }
    if (all_suspended) {
      return;
    }
  }
  LOG(ERROR) << "suspend all daemons failed";
}
void ThreadList::Register(Thread* self) {
  DCHECK_EQ(self, Thread::Current());

  if (VLOG_IS_ON(threads)) {
    std::ostringstream oss;
    self->ShortDump(oss);  // We don't hold the mutator_lock_ yet and so cannot call Dump.
    LOG(INFO) << "ThreadList::Register() " << *self  << "\n" << oss;
  }

  // Atomically add self to the thread list and make its thread_suspend_count_ reflect ongoing
  // SuspendAll requests.
  MutexLock mu(self, *Locks::thread_list_lock_);
  MutexLock mu2(self, *Locks::thread_suspend_count_lock_);
  self->suspend_count_ = suspend_all_count_;
  self->debug_suspend_count_ = debug_suspend_all_count_;
  if (self->suspend_count_ > 0) {
    self->AtomicSetFlag(kSuspendRequest);
    self->TriggerSuspend();
  }
  CHECK(!Contains(self));
  list_.push_back(self);
}

void ThreadList::Unregister(Thread* self) {
  DCHECK_EQ(self, Thread::Current());

  VLOG(threads) << "ThreadList::Unregister() " << *self;

  // Any time-consuming destruction, plus anything that can call back into managed code or
  // suspend and so on, must happen at this point, and not in ~Thread.
  self->Destroy();

  uint32_t thin_lock_id = self->thin_lock_thread_id_;
  while (self != nullptr) {
    // Remove and delete the Thread* while holding the thread_list_lock_ and
    // thread_suspend_count_lock_ so that the unregistering thread cannot be suspended.
    // Note: deliberately not using MutexLock that could hold a stale self pointer.
    Locks::thread_list_lock_->ExclusiveLock(self);
    CHECK(Contains(self));
    // Note: we don't take the thread_suspend_count_lock_ here as to be suspending a thread other
    // than yourself you need to hold the thread_list_lock_ (see Thread::ModifySuspendCount).
    if (!self->IsSuspended()) {
      list_.remove(self);
      delete self;
      self = nullptr;
    }
    Locks::thread_list_lock_->ExclusiveUnlock(self);
  }
  // Release the thread ID after the thread is finished and deleted to avoid cases where we can
  // temporarily have multiple threads with the same thread id. When this occurs, it causes
  // problems in FindThreadByThreadId / SuspendThreadByThreadId.
  ReleaseThreadId(nullptr, thin_lock_id);

  // Clear the TLS data, so that the underlying native thread is recognizably detached.
  // (It may wish to reattach later.)
  CHECK_PTHREAD_CALL(pthread_setspecific, (Thread::pthread_key_self_, NULL), "detach self");

  // Signal that a thread just detached.
  MutexLock mu(NULL, *Locks::thread_list_lock_);
  thread_exit_cond_.Signal(NULL);
}

void ThreadList::ForEach(void (*callback)(Thread*, void*), void* context) {
  for (const auto& thread : list_) {
    callback(thread, context);
  }
}

void ThreadList::VisitRoots(RootCallback* callback, void* arg) const {
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  for (const auto& thread : list_) {
    thread->VisitRoots(callback, arg);
  }
}

class VerifyRootWrapperArg {
 public:
  VerifyRootWrapperArg(VerifyRootCallback* callback, void* arg) : callback_(callback), arg_(arg) {
  }
  VerifyRootCallback* const callback_;
  void* const arg_;
};

static void VerifyRootWrapperCallback(mirror::Object** root, void* arg, uint32_t /*thread_id*/,
                                      RootType /*root_type*/) {
  VerifyRootWrapperArg* wrapperArg = reinterpret_cast<VerifyRootWrapperArg*>(arg);
  wrapperArg->callback_(*root, wrapperArg->arg_, 0, NULL);
}

void ThreadList::VerifyRoots(VerifyRootCallback* callback, void* arg) const {
  VerifyRootWrapperArg wrapper(callback, arg);
  MutexLock mu(Thread::Current(), *Locks::thread_list_lock_);
  for (const auto& thread : list_) {
    thread->VisitRoots(VerifyRootWrapperCallback, &wrapper);
  }
}

uint32_t ThreadList::AllocThreadId(Thread* self) {
  MutexLock mu(self, allocated_ids_lock_);
  for (size_t i = 0; i < allocated_ids_.size(); ++i) {
    if (!allocated_ids_[i]) {
      allocated_ids_.set(i);
      return i + 1;  // Zero is reserved to mean "invalid".
    }
  }
  LOG(FATAL) << "Out of internal thread ids";
  return 0;
}

void ThreadList::ReleaseThreadId(Thread* self, uint32_t id) {
  MutexLock mu(self, allocated_ids_lock_);
  --id;  // Zero is reserved to mean "invalid".
  DCHECK(allocated_ids_[id]) << id;
  allocated_ids_.reset(id);
}

}  // namespace art
