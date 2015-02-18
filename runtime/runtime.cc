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

#include "runtime.h"

// sys/mount.h has to come before linux/fs.h due to redefinition of MS_RDONLY, MS_BIND, etc
#include <sys/mount.h>
#ifdef __linux__
#include <linux/fs.h>
#endif

#include <signal.h>
#include <sys/syscall.h>
#include <valgrind.h>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory_representation.h>
#include <vector>
#include <fcntl.h>

#include "JniConstants.h"
#include "ScopedLocalRef.h"
#include "arch/arm/quick_method_frame_info_arm.h"
#include "arch/arm/registers_arm.h"
#include "arch/arm64/quick_method_frame_info_arm64.h"
#include "arch/arm64/registers_arm64.h"
#include "arch/instruction_set_features.h"
#include "arch/mips/quick_method_frame_info_mips.h"
#include "arch/mips/registers_mips.h"
#include "arch/mips64/quick_method_frame_info_mips64.h"
#include "arch/mips64/registers_mips64.h"
#include "arch/x86/quick_method_frame_info_x86.h"
#include "arch/x86/registers_x86.h"
#include "arch/x86_64/quick_method_frame_info_x86_64.h"
#include "arch/x86_64/registers_x86_64.h"
#include "asm_support.h"
#include "atomic.h"
#include "base/arena_allocator.h"
#include "base/dumpable.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "debugger.h"
#include "elf_file.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "fault_handler.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/heap.h"
#include "gc/space/image_space.h"
#include "gc/space/space.h"
#include "handle_scope-inl.h"
#include "image.h"
#include "instrumentation.h"
#include "intern_table.h"
#include "jni_internal.h"
#include "mirror/array.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/stack_trace_element.h"
#include "mirror/throwable.h"
#include "monitor.h"
#include "native/dalvik_system_DexFile.h"
#include "native/dalvik_system_VMDebug.h"
#include "native/dalvik_system_VMRuntime.h"
#include "native/dalvik_system_VMStack.h"
#include "native/dalvik_system_ZygoteHooks.h"
#include "native/java_lang_Class.h"
#include "native/java_lang_DexCache.h"
#include "native/java_lang_Object.h"
#include "native/java_lang_Runtime.h"
#include "native/java_lang_String.h"
#include "native/java_lang_System.h"
#include "native/java_lang_Thread.h"
#include "native/java_lang_Throwable.h"
#include "native/java_lang_VMClassLoader.h"
#include "native/java_lang_ref_FinalizerReference.h"
#include "native/java_lang_ref_Reference.h"
#include "native/java_lang_reflect_Array.h"
#include "native/java_lang_reflect_Constructor.h"
#include "native/java_lang_reflect_Field.h"
#include "native/java_lang_reflect_Method.h"
#include "native/java_lang_reflect_Proxy.h"
#include "native/java_util_concurrent_atomic_AtomicLong.h"
#include "native/org_apache_harmony_dalvik_ddmc_DdmServer.h"
#include "native/org_apache_harmony_dalvik_ddmc_DdmVmInternal.h"
#include "native/sun_misc_Unsafe.h"
#include "native_bridge_art_interface.h"
#include "oat_file.h"
#include "os.h"
#include "parsed_options.h"
#include "profiler.h"
#include "quick/quick_method_frame_info.h"
#include "reflection.h"
#include "runtime_options.h"
#include "ScopedLocalRef.h"
#include "scoped_thread_state_change.h"
#include "sigchain.h"
#include "signal_catcher.h"
#include "signal_set.h"
#include "thread.h"
#include "thread_list.h"
#include "trace.h"
#include "transaction.h"
#include "verifier/method_verifier.h"
#include "well_known_classes.h"

namespace art {

// If a signal isn't handled properly, enable a handler that attempts to dump the Java stack.
static constexpr bool kEnableJavaStackTraceHandler = false;
Runtime* Runtime::instance_ = nullptr;

Runtime::Runtime()
    : instruction_set_(kNone),
      compiler_callbacks_(nullptr),
      is_zygote_(false),
      must_relocate_(false),
      is_concurrent_gc_enabled_(true),
      is_explicit_gc_disabled_(false),
      dex2oat_enabled_(true),
      image_dex2oat_enabled_(true),
      default_stack_size_(0),
      heap_(nullptr),
      max_spins_before_thin_lock_inflation_(Monitor::kDefaultMaxSpinsBeforeThinLockInflation),
      monitor_list_(nullptr),
      monitor_pool_(nullptr),
      thread_list_(nullptr),
      intern_table_(nullptr),
      class_linker_(nullptr),
      signal_catcher_(nullptr),
      java_vm_(nullptr),
      fault_message_lock_("Fault message lock"),
      fault_message_(""),
      method_verifier_lock_("Method verifiers lock"),
      threads_being_born_(0),
      shutdown_cond_(new ConditionVariable("Runtime shutdown", *Locks::runtime_shutdown_lock_)),
      shutting_down_(false),
      shutting_down_started_(false),
      started_(false),
      finished_starting_(false),
      vfprintf_(nullptr),
      exit_(nullptr),
      abort_(nullptr),
      stats_enabled_(false),
      running_on_valgrind_(RUNNING_ON_VALGRIND > 0),
      profiler_started_(false),
      method_trace_(false),
      method_trace_file_size_(0),
      instrumentation_(),
      use_compile_time_class_path_(false),
      main_thread_group_(nullptr),
      system_thread_group_(nullptr),
      system_class_loader_(nullptr),
      dump_gc_performance_on_shutdown_(false),
      preinitialization_transaction_(nullptr),
      verify_(false),
      target_sdk_version_(0),
      implicit_null_checks_(false),
      implicit_so_checks_(false),
      implicit_suspend_checks_(false),
      is_native_bridge_loaded_(false) {
  CheckAsmSupportOffsetsAndSizes();
}

Runtime::~Runtime() {
  if (is_native_bridge_loaded_) {
    UnloadNativeBridge();
  }
  if (dump_gc_performance_on_shutdown_) {
    // This can't be called from the Heap destructor below because it
    // could call RosAlloc::InspectAll() which needs the thread_list
    // to be still alive.
    heap_->DumpGcPerformanceInfo(LOG(INFO));
  }

  Thread* self = Thread::Current();
  if (self == nullptr) {
    CHECK(AttachCurrentThread("Shutdown thread", false, nullptr, false));
    self = Thread::Current();
  } else {
    LOG(WARNING) << "Current thread not detached in Runtime shutdown";
  }

  {
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    shutting_down_started_ = true;
    while (threads_being_born_ > 0) {
      shutdown_cond_->Wait(self);
    }
    shutting_down_ = true;
  }
  // Shutdown and wait for the daemons.
  CHECK(self != nullptr);
  if (IsFinishedStarting()) {
    self->ClearException();
    self->GetJniEnv()->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                                            WellKnownClasses::java_lang_Daemons_stop);
  }
  DetachCurrentThread();
  self = nullptr;

  // Shut down background profiler before the runtime exits.
  if (profiler_started_) {
    BackgroundMethodSamplingProfiler::Shutdown();
  }

  Trace::Shutdown();

  // Make sure to let the GC complete if it is running.
  heap_->WaitForGcToComplete(gc::kGcCauseBackground, self);
  heap_->DeleteThreadPool();

  // Make sure our internal threads are dead before we start tearing down things they're using.
  Dbg::StopJdwp();
  delete signal_catcher_;

  // Make sure all other non-daemon threads have terminated, and all daemon threads are suspended.
  delete thread_list_;

  // Shutdown the fault manager if it was initialized.
  fault_manager.Shutdown();

  delete monitor_list_;
  delete monitor_pool_;
  delete class_linker_;
  delete heap_;
  delete intern_table_;
  delete java_vm_;
  Thread::Shutdown();
  QuasiAtomic::Shutdown();
  verifier::MethodVerifier::Shutdown();
  MemMap::Shutdown();
  // TODO: acquire a static mutex on Runtime to avoid racing.
  CHECK(instance_ == nullptr || instance_ == this);
  instance_ = nullptr;
}

struct AbortState {
  void Dump(std::ostream& os) const {
    if (gAborting > 1) {
      os << "Runtime aborting --- recursively, so no thread-specific detail!\n";
      return;
    }
    gAborting++;
    os << "Runtime aborting...\n";
    if (Runtime::Current() == NULL) {
      os << "(Runtime does not yet exist!)\n";
      return;
    }
    Thread* self = Thread::Current();
    if (self == nullptr) {
      os << "(Aborting thread was not attached to runtime!)\n";
      DumpKernelStack(os, GetTid(), "  kernel: ", false);
      DumpNativeStack(os, GetTid(), "  native: ", nullptr);
    } else {
      os << "Aborting thread:\n";
      if (Locks::mutator_lock_->IsExclusiveHeld(self) || Locks::mutator_lock_->IsSharedHeld(self)) {
        DumpThread(os, self);
      } else {
        if (Locks::mutator_lock_->SharedTryLock(self)) {
          DumpThread(os, self);
          Locks::mutator_lock_->SharedUnlock(self);
        }
      }
    }
    DumpAllThreads(os, self);
  }

  // No thread-safety analysis as we do explicitly test for holding the mutator lock.
  void DumpThread(std::ostream& os, Thread* self) const NO_THREAD_SAFETY_ANALYSIS {
    DCHECK(Locks::mutator_lock_->IsExclusiveHeld(self) || Locks::mutator_lock_->IsSharedHeld(self));
    self->Dump(os);
    if (self->IsExceptionPending()) {
      ThrowLocation throw_location;
      mirror::Throwable* exception = self->GetException(&throw_location);
      os << "Pending exception " << PrettyTypeOf(exception)
          << " thrown by '" << throw_location.Dump() << "'\n"
          << exception->Dump();
    }
  }

  void DumpAllThreads(std::ostream& os, Thread* self) const {
    Runtime* runtime = Runtime::Current();
    if (runtime != nullptr) {
      ThreadList* thread_list = runtime->GetThreadList();
      if (thread_list != nullptr) {
        bool tll_already_held = Locks::thread_list_lock_->IsExclusiveHeld(self);
        bool ml_already_held = Locks::mutator_lock_->IsSharedHeld(self);
        if (!tll_already_held || !ml_already_held) {
          os << "Dumping all threads without appropriate locks held:"
              << (!tll_already_held ? " thread list lock" : "")
              << (!ml_already_held ? " mutator lock" : "")
              << "\n";
        }
        os << "All threads:\n";
        thread_list->Dump(os);
      }
    }
  }
};

void Runtime::Abort() {
  gAborting++;  // set before taking any locks

  // Ensure that we don't have multiple threads trying to abort at once,
  // which would result in significantly worse diagnostics.
  MutexLock mu(Thread::Current(), *Locks::abort_lock_);

  // Get any pending output out of the way.
  fflush(NULL);

  // Many people have difficulty distinguish aborts from crashes,
  // so be explicit.
  AbortState state;
  LOG(INTERNAL_FATAL) << Dumpable<AbortState>(state);

  // Call the abort hook if we have one.
  if (Runtime::Current() != NULL && Runtime::Current()->abort_ != NULL) {
    LOG(INTERNAL_FATAL) << "Calling abort hook...";
    Runtime::Current()->abort_();
    // notreached
    LOG(INTERNAL_FATAL) << "Unexpectedly returned from abort hook!";
  }

#if defined(__GLIBC__)
  // TODO: we ought to be able to use pthread_kill(3) here (or abort(3),
  // which POSIX defines in terms of raise(3), which POSIX defines in terms
  // of pthread_kill(3)). On Linux, though, libcorkscrew can't unwind through
  // libpthread, which means the stacks we dump would be useless. Calling
  // tgkill(2) directly avoids that.
  syscall(__NR_tgkill, getpid(), GetTid(), SIGABRT);
  // TODO: LLVM installs it's own SIGABRT handler so exit to be safe... Can we disable that in LLVM?
  // If not, we could use sigaction(3) before calling tgkill(2) and lose this call to exit(3).
  exit(1);
#else
  abort();
#endif
  // notreached
}

void Runtime::PreZygoteFork() {
  heap_->PreZygoteFork();
}

void Runtime::CallExitHook(jint status) {
  if (exit_ != NULL) {
    ScopedThreadStateChange tsc(Thread::Current(), kNative);
    exit_(status);
    LOG(WARNING) << "Exit hook returned instead of exiting!";
  }
}

void Runtime::SweepSystemWeaks(IsMarkedCallback* visitor, void* arg) {
  GetInternTable()->SweepInternTableWeaks(visitor, arg);
  GetMonitorList()->SweepMonitorList(visitor, arg);
  GetJavaVM()->SweepJniWeakGlobals(visitor, arg);
}

bool Runtime::Create(const RuntimeOptions& options, bool ignore_unrecognized) {
  // TODO: acquire a static mutex on Runtime to avoid racing.
  if (Runtime::instance_ != NULL) {
    return false;
  }
  InitLogging(NULL);  // Calls Locks::Init() as a side effect.
  instance_ = new Runtime;
  if (!instance_->Init(options, ignore_unrecognized)) {
    delete instance_;
    instance_ = NULL;
    return false;
  }
  return true;
}

static jobject CreateSystemClassLoader() {
  if (Runtime::Current()->UseCompileTimeClassPath()) {
    return NULL;
  }

  ScopedObjectAccess soa(Thread::Current());
  ClassLinker* cl = Runtime::Current()->GetClassLinker();

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> class_loader_class(
      hs.NewHandle(soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_ClassLoader)));
  CHECK(cl->EnsureInitialized(soa.Self(), class_loader_class, true, true));

  mirror::ArtMethod* getSystemClassLoader =
      class_loader_class->FindDirectMethod("getSystemClassLoader", "()Ljava/lang/ClassLoader;");
  CHECK(getSystemClassLoader != NULL);

  JValue result = InvokeWithJValues(soa, nullptr, soa.EncodeMethod(getSystemClassLoader), nullptr);
  JNIEnv* env = soa.Self()->GetJniEnv();
  ScopedLocalRef<jobject> system_class_loader(env,
                                              soa.AddLocalReference<jobject>(result.GetL()));
  CHECK(system_class_loader.get() != nullptr);

  soa.Self()->SetClassLoaderOverride(system_class_loader.get());

  Handle<mirror::Class> thread_class(
      hs.NewHandle(soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_Thread)));
  CHECK(cl->EnsureInitialized(soa.Self(), thread_class, true, true));

  mirror::ArtField* contextClassLoader =
      thread_class->FindDeclaredInstanceField("contextClassLoader", "Ljava/lang/ClassLoader;");
  CHECK(contextClassLoader != NULL);

  // We can't run in a transaction yet.
  contextClassLoader->SetObject<false>(soa.Self()->GetPeer(),
                                       soa.Decode<mirror::ClassLoader*>(system_class_loader.get()));

  return env->NewGlobalRef(system_class_loader.get());
}

std::string Runtime::GetPatchoatExecutable() const {
  if (!patchoat_executable_.empty()) {
    return patchoat_executable_;
  }
  std::string patchoat_executable(GetAndroidRoot());
  patchoat_executable += (kIsDebugBuild ? "/bin/patchoatd" : "/bin/patchoat");
  return patchoat_executable;
}

std::string Runtime::GetCompilerExecutable() const {
  if (!compiler_executable_.empty()) {
    return compiler_executable_;
  }
  std::string compiler_executable(GetAndroidRoot());
  compiler_executable += (kIsDebugBuild ? "/bin/dex2oatd" : "/bin/dex2oat");
  return compiler_executable;
}

bool Runtime::Start() {
  VLOG(startup) << "Runtime::Start entering";

  // Restore main thread state to kNative as expected by native code.
  Thread* self = Thread::Current();

  self->TransitionFromRunnableToSuspended(kNative);

  started_ = true;

  // Use !IsCompiler so that we get test coverage, tests are never the zygote.
  if (!IsCompiler()) {
    ScopedObjectAccess soa(self);
    gc::space::ImageSpace* image_space = heap_->GetImageSpace();
    if (image_space != nullptr) {
      Runtime::Current()->GetInternTable()->AddImageStringsToTable(image_space);
      Runtime::Current()->GetClassLinker()->MoveImageClassesToClassTable();
    }
  }

  if (!IsImageDex2OatEnabled() || !Runtime::Current()->GetHeap()->HasImageSpace()) {
    ScopedObjectAccess soa(self);
    StackHandleScope<1> hs(soa.Self());
    auto klass(hs.NewHandle<mirror::Class>(mirror::Class::GetJavaLangClass()));
    class_linker_->EnsureInitialized(soa.Self(), klass, true, true);
  }

  // InitNativeMethods needs to be after started_ so that the classes
  // it touches will have methods linked to the oat file if necessary.
  InitNativeMethods();

  // Initialize well known thread group values that may be accessed threads while attaching.
  InitThreadGroups(self);

  Thread::FinishStartup();

  system_class_loader_ = CreateSystemClassLoader();

  if (is_zygote_) {
    if (!InitZygote()) {
      return false;
    }
  } else {
    if (is_native_bridge_loaded_) {
      PreInitializeNativeBridge(".");
    }
    DidForkFromZygote(self->GetJniEnv(), NativeBridgeAction::kInitialize,
                      GetInstructionSetString(kRuntimeISA));
  }

  StartDaemonThreads();

  {
    ScopedObjectAccess soa(self);
    self->GetJniEnv()->locals.AssertEmpty();
  }

  VLOG(startup) << "Runtime::Start exiting";
  finished_starting_ = true;

  if (profiler_options_.IsEnabled() && !profile_output_filename_.empty()) {
    // User has asked for a profile using -Xenable-profiler.
    // Create the profile file if it doesn't exist.
    int fd = open(profile_output_filename_.c_str(), O_RDWR|O_CREAT|O_EXCL, 0660);
    if (fd >= 0) {
      close(fd);
    } else if (errno != EEXIST) {
      LOG(INFO) << "Failed to access the profile file. Profiler disabled.";
      return true;
    }
    StartProfiler(profile_output_filename_.c_str());
  }

  return true;
}

void Runtime::EndThreadBirth() EXCLUSIVE_LOCKS_REQUIRED(Locks::runtime_shutdown_lock_) {
  DCHECK_GT(threads_being_born_, 0U);
  threads_being_born_--;
  if (shutting_down_started_ && threads_being_born_ == 0) {
    shutdown_cond_->Broadcast(Thread::Current());
  }
}

// Do zygote-mode-only initialization.
bool Runtime::InitZygote() {
#ifdef __linux__
  // zygote goes into its own process group
  setpgid(0, 0);

  // See storage config details at http://source.android.com/tech/storage/
  // Create private mount namespace shared by all children
  if (unshare(CLONE_NEWNS) == -1) {
    PLOG(WARNING) << "Failed to unshare()";
    return false;
  }

  // Mark rootfs as being a slave so that changes from default
  // namespace only flow into our children.
  if (mount("rootfs", "/", NULL, (MS_SLAVE | MS_REC), NULL) == -1) {
    PLOG(WARNING) << "Failed to mount() rootfs as MS_SLAVE";
    return false;
  }

  // Create a staging tmpfs that is shared by our children; they will
  // bind mount storage into their respective private namespaces, which
  // are isolated from each other.
  const char* target_base = getenv("EMULATED_STORAGE_TARGET");
  if (target_base != NULL) {
    if (mount("tmpfs", target_base, "tmpfs", MS_NOSUID | MS_NODEV,
              "uid=0,gid=1028,mode=0751") == -1) {
      LOG(WARNING) << "Failed to mount tmpfs to " << target_base;
      return false;
    }
  }

  return true;
#else
  UNIMPLEMENTED(FATAL);
  return false;
#endif
}

void Runtime::DidForkFromZygote(JNIEnv* env, NativeBridgeAction action, const char* isa) {
  is_zygote_ = false;

  if (is_native_bridge_loaded_) {
    switch (action) {
      case NativeBridgeAction::kUnload:
        UnloadNativeBridge();
        is_native_bridge_loaded_ = false;
        break;

      case NativeBridgeAction::kInitialize:
        InitializeNativeBridge(env, isa);
        break;
    }
  }

  // Create the thread pool.
  heap_->CreateThreadPool();

  StartSignalCatcher();

  // Start the JDWP thread. If the command-line debugger flags specified "suspend=y",
  // this will pause the runtime, so we probably want this to come last.
  Dbg::StartJdwp();
}

void Runtime::StartSignalCatcher() {
  if (!is_zygote_) {
    signal_catcher_ = new SignalCatcher(stack_trace_file_);
  }
}

bool Runtime::IsShuttingDown(Thread* self) {
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  return IsShuttingDownLocked();
}

void Runtime::StartDaemonThreads() {
  VLOG(startup) << "Runtime::StartDaemonThreads entering";

  Thread* self = Thread::Current();

  // Must be in the kNative state for calling native methods.
  CHECK_EQ(self->GetState(), kNative);

  JNIEnv* env = self->GetJniEnv();
  env->CallStaticVoidMethod(WellKnownClasses::java_lang_Daemons,
                            WellKnownClasses::java_lang_Daemons_start);
  if (env->ExceptionCheck()) {
    env->ExceptionDescribe();
    LOG(FATAL) << "Error starting java.lang.Daemons";
  }

  VLOG(startup) << "Runtime::StartDaemonThreads exiting";
}

static bool OpenDexFilesFromImage(const std::string& image_location,
                                  std::vector<std::unique_ptr<const DexFile>>* dex_files,
                                  size_t* failures) {
  DCHECK(dex_files != nullptr) << "OpenDexFilesFromImage: out-param is NULL";
  std::string system_filename;
  bool has_system = false;
  std::string cache_filename_unused;
  bool dalvik_cache_exists_unused;
  bool has_cache_unused;
  bool is_global_cache_unused;
  bool found_image = gc::space::ImageSpace::FindImageFilename(image_location.c_str(),
                                                              kRuntimeISA,
                                                              &system_filename,
                                                              &has_system,
                                                              &cache_filename_unused,
                                                              &dalvik_cache_exists_unused,
                                                              &has_cache_unused,
                                                              &is_global_cache_unused);
  *failures = 0;
  if (!found_image || !has_system) {
    return false;
  }
  std::string error_msg;
  // We are falling back to non-executable use of the oat file because patching failed, presumably
  // due to lack of space.
  std::string oat_filename = ImageHeader::GetOatLocationFromImageLocation(system_filename.c_str());
  std::string oat_location = ImageHeader::GetOatLocationFromImageLocation(image_location.c_str());
  std::unique_ptr<File> file(OS::OpenFileForReading(oat_filename.c_str()));
  if (file.get() == nullptr) {
    return false;
  }
  std::unique_ptr<ElfFile> elf_file(ElfFile::Open(file.release(), false, false, &error_msg));
  if (elf_file.get() == nullptr) {
    return false;
  }
  std::unique_ptr<OatFile> oat_file(OatFile::OpenWithElfFile(elf_file.release(), oat_location,
                                                             &error_msg));
  if (oat_file.get() == nullptr) {
    LOG(INFO) << "Unable to use '" << oat_filename << "' because " << error_msg;
    return false;
  }

  for (const OatFile::OatDexFile* oat_dex_file : oat_file->GetOatDexFiles()) {
    if (oat_dex_file == nullptr) {
      *failures += 1;
      continue;
    }
    std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error_msg);
    if (dex_file.get() == nullptr) {
      *failures += 1;
    } else {
      dex_files->push_back(std::move(dex_file));
    }
  }
  Runtime::Current()->GetClassLinker()->RegisterOatFile(oat_file.release());
  return true;
}


static size_t OpenDexFiles(const std::vector<std::string>& dex_filenames,
                           const std::vector<std::string>& dex_locations,
                           const std::string& image_location,
                           std::vector<std::unique_ptr<const DexFile>>* dex_files) {
  DCHECK(dex_files != nullptr) << "OpenDexFiles: out-param is NULL";
  size_t failure_count = 0;
  if (!image_location.empty() && OpenDexFilesFromImage(image_location, dex_files, &failure_count)) {
    return failure_count;
  }
  failure_count = 0;
  for (size_t i = 0; i < dex_filenames.size(); i++) {
    const char* dex_filename = dex_filenames[i].c_str();
    const char* dex_location = dex_locations[i].c_str();
    std::string error_msg;
    if (!OS::FileExists(dex_filename)) {
      LOG(WARNING) << "Skipping non-existent dex file '" << dex_filename << "'";
      continue;
    }
    if (!DexFile::Open(dex_filename, dex_location, &error_msg, dex_files)) {
      LOG(WARNING) << "Failed to open .dex from file '" << dex_filename << "': " << error_msg;
      ++failure_count;
    }
  }
  return failure_count;
}

bool Runtime::Init(const RuntimeOptions& raw_options, bool ignore_unrecognized) {
  CHECK_EQ(sysconf(_SC_PAGE_SIZE), kPageSize);

  MemMap::Init();

  using Opt = RuntimeArgumentMap;
  RuntimeArgumentMap runtime_options;
  std::unique_ptr<ParsedOptions> parsed_options(
      ParsedOptions::Create(raw_options, ignore_unrecognized, &runtime_options));
  if (parsed_options.get() == nullptr) {
    LOG(ERROR) << "Failed to parse options";
    return false;
  }
  VLOG(startup) << "Runtime::Init -verbose:startup enabled";

  QuasiAtomic::Startup();

  Monitor::Init(runtime_options.GetOrDefault(Opt::LockProfThreshold),
                runtime_options.GetOrDefault(Opt::HookIsSensitiveThread));

  boot_class_path_string_ = runtime_options.ReleaseOrDefault(Opt::BootClassPath);
  class_path_string_ = runtime_options.ReleaseOrDefault(Opt::ClassPath);
  properties_ = runtime_options.ReleaseOrDefault(Opt::PropertiesList);

  compiler_callbacks_ = runtime_options.GetOrDefault(Opt::CompilerCallbacksPtr);
  patchoat_executable_ = runtime_options.ReleaseOrDefault(Opt::PatchOat);
  must_relocate_ = runtime_options.GetOrDefault(Opt::Relocate);
  is_zygote_ = runtime_options.Exists(Opt::Zygote);
  is_explicit_gc_disabled_ = runtime_options.Exists(Opt::DisableExplicitGC);
  dex2oat_enabled_ = runtime_options.GetOrDefault(Opt::Dex2Oat);
  image_dex2oat_enabled_ = runtime_options.GetOrDefault(Opt::ImageDex2Oat);

  vfprintf_ = runtime_options.GetOrDefault(Opt::HookVfprintf);
  exit_ = runtime_options.GetOrDefault(Opt::HookExit);
  abort_ = runtime_options.GetOrDefault(Opt::HookAbort);

  default_stack_size_ = runtime_options.GetOrDefault(Opt::StackSize);
  stack_trace_file_ = runtime_options.ReleaseOrDefault(Opt::StackTraceFile);

  compiler_executable_ = runtime_options.ReleaseOrDefault(Opt::Compiler);
  compiler_options_ = runtime_options.ReleaseOrDefault(Opt::CompilerOptions);
  image_compiler_options_ = runtime_options.ReleaseOrDefault(Opt::ImageCompilerOptions);
  image_location_ = runtime_options.GetOrDefault(Opt::Image);

  max_spins_before_thin_lock_inflation_ =
      runtime_options.GetOrDefault(Opt::MaxSpinsBeforeThinLockInflation);

  monitor_list_ = new MonitorList;
  monitor_pool_ = MonitorPool::Create();
  thread_list_ = new ThreadList;
  intern_table_ = new InternTable;

  verify_ = runtime_options.GetOrDefault(Opt::Verify);

  if (runtime_options.GetOrDefault(Opt::Interpret)) {
    GetInstrumentation()->ForceInterpretOnly();
  }

  XGcOption xgc_option = runtime_options.GetOrDefault(Opt::GcOption);
  heap_ = new gc::Heap(runtime_options.GetOrDefault(Opt::MemoryInitialSize),
                       runtime_options.GetOrDefault(Opt::HeapGrowthLimit),
                       runtime_options.GetOrDefault(Opt::HeapMinFree),
                       runtime_options.GetOrDefault(Opt::HeapMaxFree),
                       runtime_options.GetOrDefault(Opt::HeapTargetUtilization),
                       runtime_options.GetOrDefault(Opt::ForegroundHeapGrowthMultiplier),
                       runtime_options.GetOrDefault(Opt::MemoryMaximumSize),
                       runtime_options.GetOrDefault(Opt::NonMovingSpaceCapacity),
                       runtime_options.GetOrDefault(Opt::Image),
                       runtime_options.GetOrDefault(Opt::ImageInstructionSet),
                       xgc_option.collector_type_,
                       runtime_options.GetOrDefault(Opt::BackgroundGc),
                       runtime_options.GetOrDefault(Opt::LargeObjectSpace),
                       runtime_options.GetOrDefault(Opt::LargeObjectThreshold),
                       runtime_options.GetOrDefault(Opt::ParallelGCThreads),
                       runtime_options.GetOrDefault(Opt::ConcGCThreads),
                       runtime_options.Exists(Opt::LowMemoryMode),
                       runtime_options.GetOrDefault(Opt::LongPauseLogThreshold),
                       runtime_options.GetOrDefault(Opt::LongGCLogThreshold),
                       runtime_options.Exists(Opt::IgnoreMaxFootprint),
                       runtime_options.Exists(Opt::UseTLAB),
                       xgc_option.verify_pre_gc_heap_,
                       xgc_option.verify_pre_sweeping_heap_,
                       xgc_option.verify_post_gc_heap_,
                       xgc_option.verify_pre_gc_rosalloc_,
                       xgc_option.verify_pre_sweeping_rosalloc_,
                       xgc_option.verify_post_gc_rosalloc_,
                       runtime_options.GetOrDefault(Opt::EnableHSpaceCompactForOOM),
                       runtime_options.GetOrDefault(Opt::HSpaceCompactForOOMMinIntervalsMs));

  dump_gc_performance_on_shutdown_ = runtime_options.Exists(Opt::DumpGCPerformanceOnShutdown);

  if (runtime_options.Exists(Opt::JdwpOptions)) {
    Dbg::ConfigureJdwp(runtime_options.GetOrDefault(Opt::JdwpOptions));
  }

  BlockSignals();
  InitPlatformSignalHandlers();

  // Change the implicit checks flags based on runtime architecture.
  switch (kRuntimeISA) {
    case kArm:
    case kThumb2:
    case kX86:
    case kArm64:
    case kX86_64:
      implicit_null_checks_ = true;
      // Installing stack protection does not play well with valgrind.
      implicit_so_checks_ = (RUNNING_ON_VALGRIND == 0);
      break;
    default:
      // Keep the defaults.
      break;
  }

  // Always initialize the signal chain so that any calls to sigaction get
  // correctly routed to the next in the chain regardless of whether we
  // have claimed the signal or not.
  InitializeSignalChain();

  if (implicit_null_checks_ || implicit_so_checks_ || implicit_suspend_checks_) {
    fault_manager.Init();

    // These need to be in a specific order.  The null point check handler must be
    // after the suspend check and stack overflow check handlers.
    //
    // Note: the instances attach themselves to the fault manager and are handled by it. The manager
    //       will delete the instance on Shutdown().
    if (implicit_suspend_checks_) {
      new SuspensionHandler(&fault_manager);
    }

    if (implicit_so_checks_) {
      new StackOverflowHandler(&fault_manager);
    }

    if (implicit_null_checks_) {
      new NullPointerHandler(&fault_manager);
    }

    if (kEnableJavaStackTraceHandler) {
      new JavaStackTraceHandler(&fault_manager);
    }
  }

  java_vm_ = new JavaVMExt(this, runtime_options);

  Thread::Startup();

  // ClassLinker needs an attached thread, but we can't fully attach a thread without creating
  // objects. We can't supply a thread group yet; it will be fixed later. Since we are the main
  // thread, we do not get a java peer.
  Thread* self = Thread::Attach("main", false, nullptr, false);
  CHECK_EQ(self->GetThreadId(), ThreadList::kMainThreadId);
  CHECK(self != nullptr);

  // Set us to runnable so tools using a runtime can allocate and GC by default
  self->TransitionFromSuspendedToRunnable();

  // Now we're attached, we can take the heap locks and validate the heap.
  GetHeap()->EnableObjectValidation();

  CHECK_GE(GetHeap()->GetContinuousSpaces().size(), 1U);
  class_linker_ = new ClassLinker(intern_table_);
  if (GetHeap()->HasImageSpace()) {
    class_linker_->InitFromImage();
    if (kIsDebugBuild) {
      GetHeap()->GetImageSpace()->VerifyImageAllocations();
    }
    if (boot_class_path_string_.empty()) {
      // The bootclasspath is not explicitly specified: construct it from the loaded dex files.
      const std::vector<const DexFile*>& boot_class_path = GetClassLinker()->GetBootClassPath();
      std::vector<std::string> dex_locations;
      dex_locations.reserve(boot_class_path.size());
      for (const DexFile* dex_file : boot_class_path) {
        dex_locations.push_back(dex_file->GetLocation());
      }
      boot_class_path_string_ = Join(dex_locations, ':');
    }
  } else {
    std::vector<std::string> dex_filenames;
    Split(boot_class_path_string_, ':', &dex_filenames);

    std::vector<std::string> dex_locations;
    if (!runtime_options.Exists(Opt::BootClassPathLocations)) {
      dex_locations = dex_filenames;
    } else {
      dex_locations = runtime_options.GetOrDefault(Opt::BootClassPathLocations);
      CHECK_EQ(dex_filenames.size(), dex_locations.size());
    }

    std::vector<std::unique_ptr<const DexFile>> boot_class_path;
    OpenDexFiles(dex_filenames,
                 dex_locations,
                 runtime_options.GetOrDefault(Opt::Image),
                 &boot_class_path);
    instruction_set_ = runtime_options.GetOrDefault(Opt::ImageInstructionSet);
    class_linker_->InitWithoutImage(std::move(boot_class_path));

    // TODO: Should we move the following to InitWithoutImage?
    SetInstructionSet(instruction_set_);
    for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
      Runtime::CalleeSaveType type = Runtime::CalleeSaveType(i);
      if (!HasCalleeSaveMethod(type)) {
        SetCalleeSaveMethod(CreateCalleeSaveMethod(), type);
      }
    }
  }

  CHECK(class_linker_ != nullptr);

  // Initialize the special sentinel_ value early.
  sentinel_ = GcRoot<mirror::Object>(class_linker_->AllocObject(self));
  CHECK(sentinel_.Read() != nullptr);

  verifier::MethodVerifier::Init();

  method_trace_ = runtime_options.Exists(Opt::MethodTrace);
  method_trace_file_ = runtime_options.ReleaseOrDefault(Opt::MethodTraceFile);
  method_trace_file_size_ = runtime_options.ReleaseOrDefault(Opt::MethodTraceFileSize);

  {
    auto&& profiler_options = runtime_options.ReleaseOrDefault(Opt::ProfilerOpts);
    profile_output_filename_ = profiler_options.output_file_name_;

    // TODO: Don't do this, just change ProfilerOptions to include the output file name?
    ProfilerOptions other_options(
        profiler_options.enabled_,
        profiler_options.period_s_,
        profiler_options.duration_s_,
        profiler_options.interval_us_,
        profiler_options.backoff_coefficient_,
        profiler_options.start_immediately_,
        profiler_options.top_k_threshold_,
        profiler_options.top_k_change_threshold_,
        profiler_options.profile_type_,
        profiler_options.max_stack_depth_);

    profiler_options_ = other_options;
  }

  // TODO: move this to just be an Trace::Start argument
  Trace::SetDefaultClockSource(runtime_options.GetOrDefault(Opt::ProfileClock));

  if (method_trace_) {
    ScopedThreadStateChange tsc(self, kWaitingForMethodTracingStart);
    Trace::Start(method_trace_file_.c_str(),
                 -1,
                 static_cast<int>(method_trace_file_size_),
                 0,
                 false,
                 false,
                 0);
  }

  // Pre-allocate an OutOfMemoryError for the double-OOME case.
  self->ThrowNewException(ThrowLocation(), "Ljava/lang/OutOfMemoryError;",
                          "OutOfMemoryError thrown while trying to throw OutOfMemoryError; "
                          "no stack trace available");
  pre_allocated_OutOfMemoryError_ = GcRoot<mirror::Throwable>(self->GetException(NULL));
  self->ClearException();

  // Pre-allocate a NoClassDefFoundError for the common case of failing to find a system class
  // ahead of checking the application's class loader.
  self->ThrowNewException(ThrowLocation(), "Ljava/lang/NoClassDefFoundError;",
                          "Class not found using the boot class loader; no stack trace available");
  pre_allocated_NoClassDefFoundError_ = GcRoot<mirror::Throwable>(self->GetException(NULL));
  self->ClearException();

  // Look for a native bridge.
  //
  // The intended flow here is, in the case of a running system:
  //
  // Runtime::Init() (zygote):
  //   LoadNativeBridge -> dlopen from cmd line parameter.
  //  |
  //  V
  // Runtime::Start() (zygote):
  //   No-op wrt native bridge.
  //  |
  //  | start app
  //  V
  // DidForkFromZygote(action)
  //   action = kUnload -> dlclose native bridge.
  //   action = kInitialize -> initialize library
  //
  //
  // The intended flow here is, in the case of a simple dalvikvm call:
  //
  // Runtime::Init():
  //   LoadNativeBridge -> dlopen from cmd line parameter.
  //  |
  //  V
  // Runtime::Start():
  //   DidForkFromZygote(kInitialize) -> try to initialize any native bridge given.
  //   No-op wrt native bridge.
  {
    std::string native_bridge_file_name = runtime_options.ReleaseOrDefault(Opt::NativeBridge);
    is_native_bridge_loaded_ = LoadNativeBridge(native_bridge_file_name);
  }

  VLOG(startup) << "Runtime::Init exiting";
  return true;
}

void Runtime::InitNativeMethods() {
  VLOG(startup) << "Runtime::InitNativeMethods entering";
  Thread* self = Thread::Current();
  JNIEnv* env = self->GetJniEnv();

  // Must be in the kNative state for calling native methods (JNI_OnLoad code).
  CHECK_EQ(self->GetState(), kNative);

  // First set up JniConstants, which is used by both the runtime's built-in native
  // methods and libcore.
  JniConstants::init(env);
  WellKnownClasses::Init(env);

  // Then set up the native methods provided by the runtime itself.
  RegisterRuntimeNativeMethods(env);

  // Then set up libcore, which is just a regular JNI library with a regular JNI_OnLoad.
  // Most JNI libraries can just use System.loadLibrary, but libcore can't because it's
  // the library that implements System.loadLibrary!
  {
    std::string reason;
    if (!java_vm_->LoadNativeLibrary(env, "libjavacore.so", nullptr, &reason)) {
      LOG(FATAL) << "LoadNativeLibrary failed for \"libjavacore.so\": " << reason;
    }
  }

  // Initialize well known classes that may invoke runtime native methods.
  WellKnownClasses::LateInit(env);

  VLOG(startup) << "Runtime::InitNativeMethods exiting";
}

void Runtime::InitThreadGroups(Thread* self) {
  JNIEnvExt* env = self->GetJniEnv();
  ScopedJniEnvLocalRefState env_state(env);
  main_thread_group_ =
      env->NewGlobalRef(env->GetStaticObjectField(
          WellKnownClasses::java_lang_ThreadGroup,
          WellKnownClasses::java_lang_ThreadGroup_mainThreadGroup));
  CHECK(main_thread_group_ != NULL || IsCompiler());
  system_thread_group_ =
      env->NewGlobalRef(env->GetStaticObjectField(
          WellKnownClasses::java_lang_ThreadGroup,
          WellKnownClasses::java_lang_ThreadGroup_systemThreadGroup));
  CHECK(system_thread_group_ != NULL || IsCompiler());
}

jobject Runtime::GetMainThreadGroup() const {
  CHECK(main_thread_group_ != NULL || IsCompiler());
  return main_thread_group_;
}

jobject Runtime::GetSystemThreadGroup() const {
  CHECK(system_thread_group_ != NULL || IsCompiler());
  return system_thread_group_;
}

jobject Runtime::GetSystemClassLoader() const {
  CHECK(system_class_loader_ != NULL || IsCompiler());
  return system_class_loader_;
}

void Runtime::RegisterRuntimeNativeMethods(JNIEnv* env) {
  register_dalvik_system_DexFile(env);
  register_dalvik_system_VMDebug(env);
  register_dalvik_system_VMRuntime(env);
  register_dalvik_system_VMStack(env);
  register_dalvik_system_ZygoteHooks(env);
  register_java_lang_Class(env);
  register_java_lang_DexCache(env);
  register_java_lang_Object(env);
  register_java_lang_ref_FinalizerReference(env);
  register_java_lang_reflect_Array(env);
  register_java_lang_reflect_Constructor(env);
  register_java_lang_reflect_Field(env);
  register_java_lang_reflect_Method(env);
  register_java_lang_reflect_Proxy(env);
  register_java_lang_ref_Reference(env);
  register_java_lang_Runtime(env);
  register_java_lang_String(env);
  register_java_lang_System(env);
  register_java_lang_Thread(env);
  register_java_lang_Throwable(env);
  register_java_lang_VMClassLoader(env);
  register_java_util_concurrent_atomic_AtomicLong(env);
  register_org_apache_harmony_dalvik_ddmc_DdmServer(env);
  register_org_apache_harmony_dalvik_ddmc_DdmVmInternal(env);
  register_sun_misc_Unsafe(env);
}

void Runtime::DumpForSigQuit(std::ostream& os) {
  GetClassLinker()->DumpForSigQuit(os);
  GetInternTable()->DumpForSigQuit(os);
  GetJavaVM()->DumpForSigQuit(os);
  GetHeap()->DumpForSigQuit(os);
  TrackedAllocators::Dump(os);
  os << "\n";

  thread_list_->DumpForSigQuit(os);
  BaseMutex::DumpAll(os);
}

void Runtime::DumpLockHolders(std::ostream& os) {
  uint64_t mutator_lock_owner = Locks::mutator_lock_->GetExclusiveOwnerTid();
  pid_t thread_list_lock_owner = GetThreadList()->GetLockOwner();
  pid_t classes_lock_owner = GetClassLinker()->GetClassesLockOwner();
  pid_t dex_lock_owner = GetClassLinker()->GetDexLockOwner();
  if ((thread_list_lock_owner | classes_lock_owner | dex_lock_owner) != 0) {
    os << "Mutator lock exclusive owner tid: " << mutator_lock_owner << "\n"
       << "ThreadList lock owner tid: " << thread_list_lock_owner << "\n"
       << "ClassLinker classes lock owner tid: " << classes_lock_owner << "\n"
       << "ClassLinker dex lock owner tid: " << dex_lock_owner << "\n";
  }
}

void Runtime::SetStatsEnabled(bool new_state) {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::instrument_entrypoints_lock_);
  if (new_state == true) {
    GetStats()->Clear(~0);
    // TODO: wouldn't it make more sense to clear _all_ threads' stats?
    self->GetStats()->Clear(~0);
    if (stats_enabled_ != new_state) {
      GetInstrumentation()->InstrumentQuickAllocEntryPointsLocked();
    }
  } else if (stats_enabled_ != new_state) {
    GetInstrumentation()->UninstrumentQuickAllocEntryPointsLocked();
  }
  stats_enabled_ = new_state;
}

void Runtime::ResetStats(int kinds) {
  GetStats()->Clear(kinds & 0xffff);
  // TODO: wouldn't it make more sense to clear _all_ threads' stats?
  Thread::Current()->GetStats()->Clear(kinds >> 16);
}

int32_t Runtime::GetStat(int kind) {
  RuntimeStats* stats;
  if (kind < (1<<16)) {
    stats = GetStats();
  } else {
    stats = Thread::Current()->GetStats();
    kind >>= 16;
  }
  switch (kind) {
  case KIND_ALLOCATED_OBJECTS:
    return stats->allocated_objects;
  case KIND_ALLOCATED_BYTES:
    return stats->allocated_bytes;
  case KIND_FREED_OBJECTS:
    return stats->freed_objects;
  case KIND_FREED_BYTES:
    return stats->freed_bytes;
  case KIND_GC_INVOCATIONS:
    return stats->gc_for_alloc_count;
  case KIND_CLASS_INIT_COUNT:
    return stats->class_init_count;
  case KIND_CLASS_INIT_TIME:
    // Convert ns to us, reduce to 32 bits.
    return static_cast<int>(stats->class_init_time_ns / 1000);
  case KIND_EXT_ALLOCATED_OBJECTS:
  case KIND_EXT_ALLOCATED_BYTES:
  case KIND_EXT_FREED_OBJECTS:
  case KIND_EXT_FREED_BYTES:
    return 0;  // backward compatibility
  default:
    LOG(FATAL) << "Unknown statistic " << kind;
    return -1;  // unreachable
  }
}

void Runtime::BlockSignals() {
  SignalSet signals;
  signals.Add(SIGPIPE);
  // SIGQUIT is used to dump the runtime's state (including stack traces).
  signals.Add(SIGQUIT);
  // SIGUSR1 is used to initiate a GC.
  signals.Add(SIGUSR1);
  signals.Block();
}

bool Runtime::AttachCurrentThread(const char* thread_name, bool as_daemon, jobject thread_group,
                                  bool create_peer) {
  return Thread::Attach(thread_name, as_daemon, thread_group, create_peer) != NULL;
}

void Runtime::DetachCurrentThread() {
  Thread* self = Thread::Current();
  if (self == NULL) {
    LOG(FATAL) << "attempting to detach thread that is not attached";
  }
  if (self->HasManagedStack()) {
    LOG(FATAL) << *Thread::Current() << " attempting to detach while still running code";
  }
  thread_list_->Unregister(self);
}

mirror::Throwable* Runtime::GetPreAllocatedOutOfMemoryError() {
  mirror::Throwable* oome = pre_allocated_OutOfMemoryError_.Read();
  if (oome == nullptr) {
    LOG(ERROR) << "Failed to return pre-allocated OOME";
  }
  return oome;
}

mirror::Throwable* Runtime::GetPreAllocatedNoClassDefFoundError() {
  mirror::Throwable* ncdfe = pre_allocated_NoClassDefFoundError_.Read();
  if (ncdfe == nullptr) {
    LOG(ERROR) << "Failed to return pre-allocated NoClassDefFoundError";
  }
  return ncdfe;
}

void Runtime::VisitConstantRoots(RootCallback* callback, void* arg) {
  // Visit the classes held as static in mirror classes, these can be visited concurrently and only
  // need to be visited once per GC since they never change.
  mirror::ArtField::VisitRoots(callback, arg);
  mirror::ArtMethod::VisitRoots(callback, arg);
  mirror::Class::VisitRoots(callback, arg);
  mirror::Reference::VisitRoots(callback, arg);
  mirror::StackTraceElement::VisitRoots(callback, arg);
  mirror::String::VisitRoots(callback, arg);
  mirror::Throwable::VisitRoots(callback, arg);
  // Visit all the primitive array types classes.
  mirror::PrimitiveArray<uint8_t>::VisitRoots(callback, arg);   // BooleanArray
  mirror::PrimitiveArray<int8_t>::VisitRoots(callback, arg);    // ByteArray
  mirror::PrimitiveArray<uint16_t>::VisitRoots(callback, arg);  // CharArray
  mirror::PrimitiveArray<double>::VisitRoots(callback, arg);    // DoubleArray
  mirror::PrimitiveArray<float>::VisitRoots(callback, arg);     // FloatArray
  mirror::PrimitiveArray<int32_t>::VisitRoots(callback, arg);   // IntArray
  mirror::PrimitiveArray<int64_t>::VisitRoots(callback, arg);   // LongArray
  mirror::PrimitiveArray<int16_t>::VisitRoots(callback, arg);   // ShortArray
}

void Runtime::VisitConcurrentRoots(RootCallback* callback, void* arg, VisitRootFlags flags) {
  intern_table_->VisitRoots(callback, arg, flags);
  class_linker_->VisitRoots(callback, arg, flags);
  if ((flags & kVisitRootFlagNewRoots) == 0) {
    // Guaranteed to have no new roots in the constant roots.
    VisitConstantRoots(callback, arg);
  }
}

void Runtime::VisitTransactionRoots(RootCallback* callback, void* arg) {
  if (preinitialization_transaction_ != nullptr) {
    preinitialization_transaction_->VisitRoots(callback, arg);
  }
}

void Runtime::VisitNonThreadRoots(RootCallback* callback, void* arg) {
  java_vm_->VisitRoots(callback, arg);
  sentinel_.VisitRootIfNonNull(callback, arg, RootInfo(kRootVMInternal));
  pre_allocated_OutOfMemoryError_.VisitRootIfNonNull(callback, arg, RootInfo(kRootVMInternal));
  resolution_method_.VisitRoot(callback, arg, RootInfo(kRootVMInternal));
  pre_allocated_NoClassDefFoundError_.VisitRootIfNonNull(callback, arg, RootInfo(kRootVMInternal));
  imt_conflict_method_.VisitRootIfNonNull(callback, arg, RootInfo(kRootVMInternal));
  imt_unimplemented_method_.VisitRootIfNonNull(callback, arg, RootInfo(kRootVMInternal));
  default_imt_.VisitRootIfNonNull(callback, arg, RootInfo(kRootVMInternal));
  for (int i = 0; i < Runtime::kLastCalleeSaveType; i++) {
    callee_save_methods_[i].VisitRootIfNonNull(callback, arg, RootInfo(kRootVMInternal));
  }
  verifier::MethodVerifier::VisitStaticRoots(callback, arg);
  {
    MutexLock mu(Thread::Current(), method_verifier_lock_);
    for (verifier::MethodVerifier* verifier : method_verifiers_) {
      verifier->VisitRoots(callback, arg);
    }
  }
  VisitTransactionRoots(callback, arg);
  instrumentation_.VisitRoots(callback, arg);
}

void Runtime::VisitNonConcurrentRoots(RootCallback* callback, void* arg) {
  thread_list_->VisitRoots(callback, arg);
  VisitNonThreadRoots(callback, arg);
}

void Runtime::VisitThreadRoots(RootCallback* callback, void* arg) {
  thread_list_->VisitRoots(callback, arg);
}

size_t Runtime::FlipThreadRoots(Closure* thread_flip_visitor, Closure* flip_callback,
                                gc::collector::GarbageCollector* collector) {
  return thread_list_->FlipThreadRoots(thread_flip_visitor, flip_callback, collector);
}

void Runtime::VisitRoots(RootCallback* callback, void* arg, VisitRootFlags flags) {
  VisitNonConcurrentRoots(callback, arg);
  VisitConcurrentRoots(callback, arg, flags);
}

mirror::ObjectArray<mirror::ArtMethod>* Runtime::CreateDefaultImt(ClassLinker* cl) {
  Thread* self = Thread::Current();
  StackHandleScope<1> hs(self);
  Handle<mirror::ObjectArray<mirror::ArtMethod>> imtable(
      hs.NewHandle(cl->AllocArtMethodArray(self, 64)));
  mirror::ArtMethod* imt_conflict_method = Runtime::Current()->GetImtConflictMethod();
  for (size_t i = 0; i < static_cast<size_t>(imtable->GetLength()); i++) {
    imtable->Set<false>(i, imt_conflict_method);
  }
  return imtable.Get();
}

mirror::ArtMethod* Runtime::CreateImtConflictMethod() {
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(self);
  Handle<mirror::ArtMethod> method(hs.NewHandle(class_linker->AllocArtMethod(self)));
  method->SetDeclaringClass(mirror::ArtMethod::GetJavaLangReflectArtMethod());
  // TODO: use a special method for imt conflict method saves.
  method->SetDexMethodIndex(DexFile::kDexNoIndex);
  // When compiling, the code pointer will get set later when the image is loaded.
  if (runtime->IsCompiler()) {
    size_t pointer_size = GetInstructionSetPointerSize(instruction_set_);
    method->SetEntryPointFromQuickCompiledCodePtrSize(nullptr, pointer_size);
  } else {
    method->SetEntryPointFromQuickCompiledCode(GetQuickImtConflictStub());
  }
  return method.Get();
}

mirror::ArtMethod* Runtime::CreateResolutionMethod() {
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(self);
  Handle<mirror::ArtMethod> method(hs.NewHandle(class_linker->AllocArtMethod(self)));
  method->SetDeclaringClass(mirror::ArtMethod::GetJavaLangReflectArtMethod());
  // TODO: use a special method for resolution method saves
  method->SetDexMethodIndex(DexFile::kDexNoIndex);
  // When compiling, the code pointer will get set later when the image is loaded.
  if (runtime->IsCompiler()) {
    size_t pointer_size = GetInstructionSetPointerSize(instruction_set_);
    method->SetEntryPointFromQuickCompiledCodePtrSize(nullptr, pointer_size);
  } else {
    method->SetEntryPointFromQuickCompiledCode(GetQuickResolutionStub());
  }
  return method.Get();
}

mirror::ArtMethod* Runtime::CreateCalleeSaveMethod() {
  Thread* self = Thread::Current();
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(self);
  Handle<mirror::ArtMethod> method(hs.NewHandle(class_linker->AllocArtMethod(self)));
  method->SetDeclaringClass(mirror::ArtMethod::GetJavaLangReflectArtMethod());
  // TODO: use a special method for callee saves
  method->SetDexMethodIndex(DexFile::kDexNoIndex);
  size_t pointer_size = GetInstructionSetPointerSize(instruction_set_);
  method->SetEntryPointFromQuickCompiledCodePtrSize(nullptr, pointer_size);
  DCHECK_NE(instruction_set_, kNone);
  return method.Get();
}

void Runtime::DisallowNewSystemWeaks() {
  monitor_list_->DisallowNewMonitors();
  intern_table_->DisallowNewInterns();
  java_vm_->DisallowNewWeakGlobals();
}

void Runtime::AllowNewSystemWeaks() {
  monitor_list_->AllowNewMonitors();
  intern_table_->AllowNewInterns();
  java_vm_->AllowNewWeakGlobals();
}

void Runtime::EnsureNewSystemWeaksDisallowed() {
  // Lock and unlock the system weak locks once to ensure that no
  // threads are still in the middle of adding new system weaks.
  monitor_list_->EnsureNewMonitorsDisallowed();
  intern_table_->EnsureNewInternsDisallowed();
  java_vm_->EnsureNewWeakGlobalsDisallowed();
}

void Runtime::SetInstructionSet(InstructionSet instruction_set) {
  instruction_set_ = instruction_set;
  if ((instruction_set_ == kThumb2) || (instruction_set_ == kArm)) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = arm::ArmCalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kMips) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = mips::MipsCalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kMips64) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = mips64::Mips64CalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kX86) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = x86::X86CalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kX86_64) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = x86_64::X86_64CalleeSaveMethodFrameInfo(type);
    }
  } else if (instruction_set_ == kArm64) {
    for (int i = 0; i != kLastCalleeSaveType; ++i) {
      CalleeSaveType type = static_cast<CalleeSaveType>(i);
      callee_save_method_frame_infos_[i] = arm64::Arm64CalleeSaveMethodFrameInfo(type);
    }
  } else {
    UNIMPLEMENTED(FATAL) << instruction_set_;
  }
}

void Runtime::SetCalleeSaveMethod(mirror::ArtMethod* method, CalleeSaveType type) {
  DCHECK_LT(static_cast<int>(type), static_cast<int>(kLastCalleeSaveType));
  callee_save_methods_[type] = GcRoot<mirror::ArtMethod>(method);
}

const std::vector<const DexFile*>& Runtime::GetCompileTimeClassPath(jobject class_loader) {
  if (class_loader == NULL) {
    return GetClassLinker()->GetBootClassPath();
  }
  CHECK(UseCompileTimeClassPath());
  CompileTimeClassPaths::const_iterator it = compile_time_class_paths_.find(class_loader);
  CHECK(it != compile_time_class_paths_.end());
  return it->second;
}

void Runtime::SetCompileTimeClassPath(jobject class_loader,
                                      std::vector<const DexFile*>& class_path) {
  CHECK(!IsStarted());
  use_compile_time_class_path_ = true;
  compile_time_class_paths_.Put(class_loader, class_path);
}

void Runtime::AddMethodVerifier(verifier::MethodVerifier* verifier) {
  DCHECK(verifier != nullptr);
  if (gAborting) {
    return;
  }
  MutexLock mu(Thread::Current(), method_verifier_lock_);
  method_verifiers_.insert(verifier);
}

void Runtime::RemoveMethodVerifier(verifier::MethodVerifier* verifier) {
  DCHECK(verifier != nullptr);
  if (gAborting) {
    return;
  }
  MutexLock mu(Thread::Current(), method_verifier_lock_);
  auto it = method_verifiers_.find(verifier);
  CHECK(it != method_verifiers_.end());
  method_verifiers_.erase(it);
}

void Runtime::StartProfiler(const char* profile_output_filename) {
  profile_output_filename_ = profile_output_filename;
  profiler_started_ =
    BackgroundMethodSamplingProfiler::Start(profile_output_filename_, profiler_options_);
}

// Transaction support.
void Runtime::EnterTransactionMode(Transaction* transaction) {
  DCHECK(IsCompiler());
  DCHECK(transaction != nullptr);
  DCHECK(!IsActiveTransaction());
  preinitialization_transaction_ = transaction;
}

void Runtime::ExitTransactionMode() {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_ = nullptr;
}


bool Runtime::IsTransactionAborted() const {
  if (!IsActiveTransaction()) {
    return false;
  } else {
    DCHECK(IsCompiler());
    return preinitialization_transaction_->IsAborted();
  }
}

void Runtime::AbortTransactionAndThrowInternalError(Thread* self,
                                                    const std::string& abort_message) {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->Abort(abort_message);
  ThrowInternalErrorForAbortedTransaction(self);
}

void Runtime::ThrowInternalErrorForAbortedTransaction(Thread* self) {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  DCHECK(IsTransactionAborted());
  preinitialization_transaction_->ThrowInternalError(self);
}

void Runtime::RecordWriteFieldBoolean(mirror::Object* obj, MemberOffset field_offset,
                                      uint8_t value, bool is_volatile) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteFieldBoolean(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteFieldByte(mirror::Object* obj, MemberOffset field_offset,
                                   int8_t value, bool is_volatile) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteFieldByte(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteFieldChar(mirror::Object* obj, MemberOffset field_offset,
                                   uint16_t value, bool is_volatile) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteFieldChar(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteFieldShort(mirror::Object* obj, MemberOffset field_offset,
                                    int16_t value, bool is_volatile) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteFieldShort(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteField32(mirror::Object* obj, MemberOffset field_offset,
                                 uint32_t value, bool is_volatile) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteField32(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteField64(mirror::Object* obj, MemberOffset field_offset,
                                 uint64_t value, bool is_volatile) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteField64(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteFieldReference(mirror::Object* obj, MemberOffset field_offset,
                                        mirror::Object* value, bool is_volatile) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteFieldReference(obj, field_offset, value, is_volatile);
}

void Runtime::RecordWriteArray(mirror::Array* array, size_t index, uint64_t value) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWriteArray(array, index, value);
}

void Runtime::RecordStrongStringInsertion(mirror::String* s) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordStrongStringInsertion(s);
}

void Runtime::RecordWeakStringInsertion(mirror::String* s) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWeakStringInsertion(s);
}

void Runtime::RecordStrongStringRemoval(mirror::String* s) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordStrongStringRemoval(s);
}

void Runtime::RecordWeakStringRemoval(mirror::String* s) const {
  DCHECK(IsCompiler());
  DCHECK(IsActiveTransaction());
  preinitialization_transaction_->RecordWeakStringRemoval(s);
}

void Runtime::SetFaultMessage(const std::string& message) {
  MutexLock mu(Thread::Current(), fault_message_lock_);
  fault_message_ = message;
}

void Runtime::AddCurrentRuntimeFeaturesAsDex2OatArguments(std::vector<std::string>* argv)
    const {
  if (GetInstrumentation()->InterpretOnly()) {
    argv->push_back("--compiler-filter=interpret-only");
  }

  // Make the dex2oat instruction set match that of the launching runtime. If we have multiple
  // architecture support, dex2oat may be compiled as a different instruction-set than that
  // currently being executed.
  std::string instruction_set("--instruction-set=");
  instruction_set += GetInstructionSetString(kRuntimeISA);
  argv->push_back(instruction_set);

  std::unique_ptr<const InstructionSetFeatures> features(InstructionSetFeatures::FromCppDefines());
  std::string feature_string("--instruction-set-features=");
  feature_string += features->GetFeatureString();
  argv->push_back(feature_string);
}

void Runtime::UpdateProfilerState(int state) {
  VLOG(profiler) << "Profiler state updated to " << state;
}
}  // namespace art
