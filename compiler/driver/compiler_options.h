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

#ifndef ART_COMPILER_DRIVER_COMPILER_OPTIONS_H_
#define ART_COMPILER_DRIVER_COMPILER_OPTIONS_H_

#include <ostream>
#include <string>
#include <vector>

#include "base/macros.h"
#include "dex/pass_manager.h"
#include "globals.h"
#include "utils.h"

namespace art {

class CompilerOptions FINAL {
 public:
  enum CompilerFilter {
    kVerifyNone,          // Skip verification and compile nothing except JNI stubs.
    kInterpretOnly,       // Verify, and compile only JNI stubs.
    kVerifyAtRuntime,     // Only compile JNI stubs and verify at runtime.
    kSpace,               // Maximize space savings.
    kBalanced,            // Try to get the best performance return on compilation investment.
    kSpeed,               // Maximize runtime performance.
    kEverything,          // Force compilation of everything capable of being compiled.
    kTime,                // Compile methods, but minimize compilation time.
  };

  // Guide heuristics to determine whether to compile method if profile data not available.
  static const CompilerFilter kDefaultCompilerFilter = kSpeed;
  static const size_t kDefaultHugeMethodThreshold = 10000;
  static const size_t kDefaultLargeMethodThreshold = 600;
  static const size_t kDefaultSmallMethodThreshold = 60;
  static const size_t kDefaultTinyMethodThreshold = 20;
  static const size_t kDefaultNumDexMethodsThreshold = 900;
  static constexpr double kDefaultTopKProfileThreshold = 90.0;
  static const bool kDefaultNativeDebuggable = false;
  static const bool kDefaultGenerateDebugInfo = false;
  static const bool kDefaultIncludePatchInformation = false;
  static const size_t kDefaultInlineDepthLimit = 3;
  static const size_t kDefaultInlineMaxCodeUnits = 32;
  static constexpr size_t kUnsetInlineDepthLimit = -1;
  static constexpr size_t kUnsetInlineMaxCodeUnits = -1;

  // Default inlining settings when the space filter is used.
  static constexpr size_t kSpaceFilterInlineDepthLimit = 3;
  static constexpr size_t kSpaceFilterInlineMaxCodeUnits = 10;

  CompilerOptions();
  ~CompilerOptions();

  CompilerOptions(CompilerFilter compiler_filter,
                  size_t huge_method_threshold,
                  size_t large_method_threshold,
                  size_t small_method_threshold,
                  size_t tiny_method_threshold,
                  size_t num_dex_methods_threshold,
                  size_t inline_depth_limit,
                  size_t inline_max_code_units,
                  const std::vector<const DexFile*>* no_inline_from,
                  bool include_patch_information,
                  double top_k_profile_threshold,
                  bool debuggable,
                  bool generate_debug_info,
                  bool implicit_null_checks,
                  bool implicit_so_checks,
                  bool implicit_suspend_checks,
                  bool compile_pic,
                  const std::vector<std::string>* verbose_methods,
                  std::ostream* init_failure_output,
                  bool abort_on_hard_verifier_failure,
                  const std::string& dump_cfg_file_name,
                  bool dump_cfg_append);

  CompilerFilter GetCompilerFilter() const {
    return compiler_filter_;
  }

  void SetCompilerFilter(CompilerFilter compiler_filter) {
    compiler_filter_ = compiler_filter;
  }

  bool VerifyAtRuntime() const {
    return compiler_filter_ == CompilerOptions::kVerifyAtRuntime;
  }

  bool IsCompilationEnabled() const {
    return compiler_filter_ != CompilerOptions::kVerifyNone &&
        compiler_filter_ != CompilerOptions::kInterpretOnly &&
        compiler_filter_ != CompilerOptions::kVerifyAtRuntime;
  }

  bool IsVerificationEnabled() const {
    return compiler_filter_ != CompilerOptions::kVerifyNone &&
        compiler_filter_ != CompilerOptions::kVerifyAtRuntime;
  }

  bool NeverVerify() const {
    return compiler_filter_ == CompilerOptions::kVerifyNone;
  }

  size_t GetHugeMethodThreshold() const {
    return huge_method_threshold_;
  }

  size_t GetLargeMethodThreshold() const {
    return large_method_threshold_;
  }

  size_t GetSmallMethodThreshold() const {
    return small_method_threshold_;
  }

  size_t GetTinyMethodThreshold() const {
    return tiny_method_threshold_;
  }

  bool IsHugeMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > huge_method_threshold_;
  }

  bool IsLargeMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > large_method_threshold_;
  }

  bool IsSmallMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > small_method_threshold_;
  }

  bool IsTinyMethod(size_t num_dalvik_instructions) const {
    return num_dalvik_instructions > tiny_method_threshold_;
  }

  size_t GetNumDexMethodsThreshold() const {
    return num_dex_methods_threshold_;
  }

  size_t GetInlineDepthLimit() const {
    return inline_depth_limit_;
  }

  size_t GetInlineMaxCodeUnits() const {
    return inline_max_code_units_;
  }

  double GetTopKProfileThreshold() const {
    return top_k_profile_threshold_;
  }

  bool GetDebuggable() const {
    return debuggable_;
  }

  bool GetNativeDebuggable() const {
    return native_debuggable_;
  }

  bool GetGenerateDebugInfo() const {
    return generate_debug_info_;
  }

  bool GetImplicitNullChecks() const {
    return implicit_null_checks_;
  }

  bool GetImplicitStackOverflowChecks() const {
    return implicit_so_checks_;
  }

  bool GetImplicitSuspendChecks() const {
    return implicit_suspend_checks_;
  }

  bool GetIncludePatchInformation() const {
    return include_patch_information_;
  }

  // Should the code be compiled as position independent?
  bool GetCompilePic() const {
    return compile_pic_;
  }

  bool HasVerboseMethods() const {
    return verbose_methods_ != nullptr && !verbose_methods_->empty();
  }

  bool IsVerboseMethod(const std::string& pretty_method) const {
    for (const std::string& cur_method : *verbose_methods_) {
      if (pretty_method.find(cur_method) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  std::ostream* GetInitFailureOutput() const {
    return init_failure_output_.get();
  }

  const PassManagerOptions* GetPassManagerOptions() const {
    return &pass_manager_options_;
  }

  bool AbortOnHardVerifierFailure() const {
    return abort_on_hard_verifier_failure_;
  }

  const std::vector<const DexFile*>* GetNoInlineFromDexFile() const {
    return no_inline_from_;
  }

  bool ParseCompilerOption(const StringPiece& option, UsageFn Usage);

  const std::string& GetDumpCfgFileName() const {
    return dump_cfg_file_name_;
  }

  bool GetDumpCfgAppend() const {
    return dump_cfg_append_;
  }

 private:
  void ParseDumpInitFailures(const StringPiece& option, UsageFn Usage);
  void ParsePassOptions(const StringPiece& option, UsageFn Usage);
  void ParseDumpCfgPasses(const StringPiece& option, UsageFn Usage);
  void ParsePrintPasses(const StringPiece& option, UsageFn Usage);
  void ParseDisablePasses(const StringPiece& option, UsageFn Usage);
  void ParseInlineMaxCodeUnits(const StringPiece& option, UsageFn Usage);
  void ParseInlineDepthLimit(const StringPiece& option, UsageFn Usage);
  void ParseNumDexMethods(const StringPiece& option, UsageFn Usage);
  void ParseTinyMethodMax(const StringPiece& option, UsageFn Usage);
  void ParseSmallMethodMax(const StringPiece& option, UsageFn Usage);
  void ParseLargeMethodMax(const StringPiece& option, UsageFn Usage);
  void ParseHugeMethodMax(const StringPiece& option, UsageFn Usage);

  CompilerFilter compiler_filter_;
  size_t huge_method_threshold_;
  size_t large_method_threshold_;
  size_t small_method_threshold_;
  size_t tiny_method_threshold_;
  size_t num_dex_methods_threshold_;
  size_t inline_depth_limit_;
  size_t inline_max_code_units_;

  // A dex file from which we should not inline code.
  const std::vector<const DexFile*>* no_inline_from_;

  bool include_patch_information_;
  // When using a profile file only the top K% of the profiled samples will be compiled.
  double top_k_profile_threshold_;
  bool debuggable_;
  bool native_debuggable_;
  bool generate_debug_info_;
  bool implicit_null_checks_;
  bool implicit_so_checks_;
  bool implicit_suspend_checks_;
  bool compile_pic_;

  // Vector of methods to have verbose output enabled for.
  const std::vector<std::string>* verbose_methods_;

  PassManagerOptions pass_manager_options_;

  // Abort compilation with an error if we find a class that fails verification with a hard
  // failure.
  bool abort_on_hard_verifier_failure_;

  // Log initialization of initialization failures to this stream if not null.
  std::unique_ptr<std::ostream> init_failure_output_;

  std::string dump_cfg_file_name_;
  bool dump_cfg_append_;

  friend class Dex2Oat;

  DISALLOW_COPY_AND_ASSIGN(CompilerOptions);
};
std::ostream& operator<<(std::ostream& os, const CompilerOptions::CompilerFilter& rhs);

}  // namespace art

#endif  // ART_COMPILER_DRIVER_COMPILER_OPTIONS_H_
