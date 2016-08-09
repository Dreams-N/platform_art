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

#include "ti/env.h"

namespace openjdkjvmti {
extern "C" void CreateArtJvmTiEnv(void** new_jvmtiEnv, art::ti::Env* new_art_ti);
}  // namespace openjdkjvmti

namespace art {
namespace ti {

Env* Env::Create(JavaVMExt* vm) {
  Env* ret = new Env(vm);
  // openjdkjvmti::CreateArtJvmTiEnv(&ret->jvmti_env_, ret);
  return ret;
}

}  // namespace ti
}  // namespace art
