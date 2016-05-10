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

#ifndef ART_COMPILER_LINKER_MIPS_RELATIVE_PATCHER_MIPS_BASE_H_
#define ART_COMPILER_LINKER_MIPS_RELATIVE_PATCHER_MIPS_BASE_H_

#include "linker/relative_patcher.h"

namespace art {
namespace linker {

class MipsBaseRelativePatcher : public RelativePatcher {
 public:
  uint32_t ReserveSpace(uint32_t offset,
                        const CompiledMethod* compiled_method,
                        MethodReference method_ref) OVERRIDE;
  uint32_t ReserveSpaceEnd(uint32_t offset) OVERRIDE;
  uint32_t WriteThunks(OutputStream* out, uint32_t offset) OVERRIDE;
  void PatchCall(std::vector<uint8_t>* code,
                 uint32_t literal_offset,
                 uint32_t patch_offset,
                 uint32_t target_offset) OVERRIDE;

 protected:
  // We'll maximize the range of a single load instruction for dex cache array accesses
  // by aligning offset -32768 with the offset of the first used element.
  static constexpr uint32_t kDexCacheArrayLwOffset = 0x8000;

  MipsBaseRelativePatcher() { }

 private:
  DISALLOW_COPY_AND_ASSIGN(MipsBaseRelativePatcher);
};

}  // namespace linker
}  // namespace art

#endif  // ART_COMPILER_LINKER_MIPS_RELATIVE_PATCHER_MIPS_BASE_H_
