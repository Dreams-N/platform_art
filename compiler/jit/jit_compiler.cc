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

#include "jit_compiler.h"

#include "art_method-inl.h"
#include "arch/instruction_set.h"
#include "arch/instruction_set_features.h"
#include "base/time_utils.h"
#include "base/timing_logger.h"
#include "compiler_callbacks.h"
#include "dex/pass_manager.h"
#include "dex/quick_compiler_callbacks.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "oat_file-inl.h"
#include "object_lock.h"
#include "thread_list.h"
#include "verifier/method_verifier-inl.h"

namespace art {
namespace jit {

JitCompiler* JitCompiler::Create() {
  return new JitCompiler();
}

extern "C" void* jit_load(CompilerCallbacks** callbacks) {
  VLOG(jit) << "loading jit compiler";
  auto* const jit_compiler = JitCompiler::Create();
  CHECK(jit_compiler != nullptr);
  *callbacks = jit_compiler->GetCompilerCallbacks();
  VLOG(jit) << "Done loading jit compiler";
  return jit_compiler;
}

extern "C" void jit_unload(void* handle) {
  DCHECK(handle != nullptr);
  delete reinterpret_cast<JitCompiler*>(handle);
}

extern "C" bool jit_compile_method(void* handle, ArtMethod* method, Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  auto* jit_compiler = reinterpret_cast<JitCompiler*>(handle);
  DCHECK(jit_compiler != nullptr);
  return jit_compiler->CompileMethod(self, method);
}

JitCompiler::JitCompiler() : total_time_(0) {
  auto* pass_manager_options = new PassManagerOptions;
  pass_manager_options->SetDisablePassList("GVN,DCE,GVNCleanup");
  compiler_options_.reset(new CompilerOptions(
      CompilerOptions::kDefaultCompilerFilter,
      CompilerOptions::kDefaultHugeMethodThreshold,
      CompilerOptions::kDefaultLargeMethodThreshold,
      CompilerOptions::kDefaultSmallMethodThreshold,
      CompilerOptions::kDefaultTinyMethodThreshold,
      CompilerOptions::kDefaultNumDexMethodsThreshold,
      CompilerOptions::kDefaultInlineDepthLimit,
      CompilerOptions::kDefaultInlineMaxCodeUnits,
      /* include_patch_information */ false,
      CompilerOptions::kDefaultTopKProfileThreshold,
      Runtime::Current()->IsDebuggable(),
      CompilerOptions::kDefaultGenerateDebugInfo,
      /* implicit_null_checks */ true,
      /* implicit_so_checks */ true,
      /* implicit_suspend_checks */ false,
      /* pic */ true,  // TODO: Support non-PIC in optimizing.
      /* verbose_methods */ nullptr,
      pass_manager_options,
      /* init_failure_output */ nullptr,
      /* abort_on_hard_verifier_failure */ false));
  const InstructionSet instruction_set = kRuntimeISA;
  instruction_set_features_.reset(InstructionSetFeatures::FromCppDefines());
  cumulative_logger_.reset(new CumulativeLogger("jit times"));
  verification_results_.reset(new VerificationResults(compiler_options_.get()));
  method_inliner_map_.reset(new DexFileToMethodInlinerMap);
  callbacks_.reset(new QuickCompilerCallbacks(verification_results_.get(),
                                              method_inliner_map_.get(),
                                              CompilerCallbacks::CallbackMode::kCompileApp));
  compiler_driver_.reset(new CompilerDriver(
      compiler_options_.get(),
      verification_results_.get(),
      method_inliner_map_.get(),
      Compiler::kOptimizing,
      instruction_set,
      instruction_set_features_.get(),
      /* image */ false,
      /* image_classes */ nullptr,
      /* compiled_classes */ nullptr,
      /* compiled_methods */ nullptr,
      /* thread_count */ 1,
      /* dump_stats */ false,
      /* dump_passes */ false,
      /* dump_cfg_file_name */ "",
      /* dump_cfg_append */ false,
      cumulative_logger_.get(),
      /* swap_fd */ -1,
      /* profile_file */ ""));
  // Disable dedupe so we can remove compiled methods.
  compiler_driver_->SetDedupeEnabled(false);
  compiler_driver_->SetSupportBootImageFixup(false);
}

JitCompiler::~JitCompiler() {
}

bool JitCompiler::CompileMethod(Thread* self, ArtMethod* method) {
  TimingLogger logger("JIT compiler timing logger", true, VLOG_IS_ON(jit));
  const uint64_t start_time = NanoTime();
  StackHandleScope<2> hs(self);
  self->AssertNoPendingException();
  Runtime* runtime = Runtime::Current();

  // Check if the method is already compiled.
  if (runtime->GetJit()->GetCodeCache()->ContainsMethod(method)) {
    VLOG(jit) << "Already compiled " << PrettyMethod(method);
    return true;
  }

  // Don't compile the method if we are supposed to be deoptimized.
  if (runtime->GetInstrumentation()->AreAllMethodsDeoptimized()) {
    return false;
  }

  // Ensure the class is initialized.
  Handle<mirror::Class> h_class(hs.NewHandle(method->GetDeclaringClass()));
  if (!runtime->GetClassLinker()->EnsureInitialized(self, h_class, true, true)) {
    VLOG(jit) << "JIT failed to initialize " << PrettyMethod(method);
    return false;
  }

  // Do the compilation.
  CompiledMethod* compiled_method = nullptr;
  {
    TimingLogger::ScopedTiming t2("Compiling", &logger);
    compiled_method = compiler_driver_->CompileArtMethod(self, method);
  }

  // Trim maps to reduce memory usage.
  // TODO: measure how much this increases compile time.
  {
    TimingLogger::ScopedTiming t2("TrimMaps", &logger);
    runtime->GetArenaPool()->TrimMaps();
  }

  // Check if we failed compiling.
  if (compiled_method == nullptr) {
    return false;
  }

  total_time_ += NanoTime() - start_time;
  bool result = false;
  const void* code = runtime->GetClassLinker()->GetOatMethodQuickCodeFor(method);

  if (code != nullptr) {
    // Already have some compiled code, just use this instead of linking.
    // TODO: Fix recompilation.
    method->SetEntryPointFromQuickCompiledCode(code);
    result = true;
  } else {
    TimingLogger::ScopedTiming t2("LinkCode", &logger);
    OatFile::OatMethod oat_method(nullptr, 0);
    if (AddToCodeCache(method, compiled_method, &oat_method)) {
      oat_method.LinkMethod(method);
      CHECK(runtime->GetJit()->GetCodeCache()->ContainsMethod(method)) << PrettyMethod(method);
      result = true;
    }
  }

  // Remove the compiled method to save memory.
  compiler_driver_->RemoveCompiledMethod(
      MethodReference(h_class->GetDexCache()->GetDexFile(), method->GetDexMethodIndex()));
  runtime->GetJit()->AddTimingLogger(logger);
  return result;
}

CompilerCallbacks* JitCompiler::GetCompilerCallbacks() const {
  return callbacks_.get();
}

bool JitCompiler::AddToCodeCache(ArtMethod* method,
                                 const CompiledMethod* compiled_method,
                                 OatFile::OatMethod* out_method) {
  Runtime* runtime = Runtime::Current();
  JitCodeCache* const code_cache = runtime->GetJit()->GetCodeCache();
  const auto* quick_code = compiled_method->GetQuickCode();
  if (quick_code == nullptr) {
    return false;
  }
  const auto code_size = quick_code->size();
  Thread* const self = Thread::Current();
  auto* const mapping_table = compiled_method->GetMappingTable();
  auto* const vmap_table = compiled_method->GetVmapTable();
  auto* const gc_map = compiled_method->GetGcMap();
  uint8_t* mapping_table_ptr = nullptr;
  uint8_t* vmap_table_ptr = nullptr;
  uint8_t* gc_map_ptr = nullptr;

  if (mapping_table != nullptr) {
    // Write out pre-header stuff.
    mapping_table_ptr = code_cache->AddDataArray(
        self, mapping_table->data(), mapping_table->data() + mapping_table->size());
    if (mapping_table_ptr == nullptr) {
      return false;  // Out of data cache.
    }
  }

  if (vmap_table != nullptr) {
    vmap_table_ptr = code_cache->AddDataArray(
        self, vmap_table->data(), vmap_table->data() + vmap_table->size());
    if (vmap_table_ptr == nullptr) {
      return false;  // Out of data cache.
    }
  }

  if (gc_map != nullptr) {
    gc_map_ptr = code_cache->AddDataArray(
        self, gc_map->data(), gc_map->data() + gc_map->size());
    if (gc_map_ptr == nullptr) {
      return false;  // Out of data cache.
    }
  }

  uint8_t* const code = code_cache->CommitCode(self,
                                               mapping_table_ptr,
                                               vmap_table_ptr,
                                               gc_map_ptr,
                                               compiled_method->GetFrameSizeInBytes(),
                                               compiled_method->GetCoreSpillMask(),
                                               compiled_method->GetFpSpillMask(),
                                               compiled_method->GetQuickCode()->data(),
                                               compiled_method->GetQuickCode()->size());

  if (code == nullptr) {
    return false;
  }

  const size_t thumb_offset = compiled_method->CodeDelta();
  const uint32_t code_offset = sizeof(OatQuickMethodHeader) + thumb_offset;
  *out_method = OatFile::OatMethod(code, code_offset);
  DCHECK_EQ(out_method->GetGcMap(), gc_map_ptr);
  DCHECK_EQ(out_method->GetMappingTable(), mapping_table_ptr);
  DCHECK_EQ(out_method->GetVmapTable(), vmap_table_ptr);
  DCHECK_EQ(out_method->GetFrameSizeInBytes(), compiled_method->GetFrameSizeInBytes());
  DCHECK_EQ(out_method->GetCoreSpillMask(), compiled_method->GetCoreSpillMask());
  DCHECK_EQ(out_method->GetFpSpillMask(), compiled_method->GetFpSpillMask());
  VLOG(jit)
      << "JIT added "
      << PrettyMethod(method) << "@" << method
      << " ccache_size=" << PrettySize(code_cache->CodeCacheSize()) << ": "
      << reinterpret_cast<void*>(code + code_offset)
      << "," << reinterpret_cast<void*>(code + code_offset + code_size);
  return true;
}

}  // namespace jit
}  // namespace art
