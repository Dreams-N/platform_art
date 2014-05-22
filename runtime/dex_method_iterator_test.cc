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

#include "dex_method_iterator.h"

#include "common_runtime_test.h"

namespace art {

class DexMethodIteratorTest : public CommonRuntimeTest {
 public:
  bool OpenDexFile(const std::string& partial_filename, std::vector<const DexFile*>* dex_files) {
    std::string dfn = GetDexFileName(partial_filename);
    std::string error_msg;
    if (!DexFile::Open(dfn.c_str(), dfn.c_str(), &error_msg, dex_files)) {
      LG << "Failed to open '" << dfn << "': " << error_msg;
      return false;
    } else {
      return true;
    }
  }
};

TEST_F(DexMethodIteratorTest, Basic) {
  ScopedObjectAccess soa(Thread::Current());
  std::vector<const DexFile*> dex_files;
  OpenDexFile("core-libart", &dex_files);
  OpenDexFile("conscrypt", &dex_files);
  OpenDexFile("okhttp", &dex_files);
  OpenDexFile("core-junit", &dex_files);
  OpenDexFile("bouncycastle", &dex_files);
  DexMethodIterator it(dex_files);
  while (it.HasNext()) {
    const DexFile& dex_file = it.GetDexFile();
    InvokeType invoke_type = it.GetInvokeType();
    uint32_t method_idx = it.GetMemberIndex();
    if (false) {
      LG << invoke_type << " " << PrettyMethod(method_idx, dex_file);
    }
    it.Next();
  }
  STLDeleteElements(&dex_files);
}

}  // namespace art
