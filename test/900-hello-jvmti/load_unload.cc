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

#include <iostream>
#include <pthread.h>
#include <stdio.h>
#include <vector>

#include "art_method-inl.h"
#include "base/logging.h"
#include "base/macros.h"
#include <jni.h>
#include "../../libcore/ojluni/src/main/native/jvmti.h"

namespace art {

extern "C" JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void* reserved) {
  UNUSED(vm);
  UNUSED(reserved);
  printf("Agent_OnLoad called with options \"%s\"\n", options);
  return 0;
}

extern "C" JNIEXPORT void JNICALL Agent_OnUnload(JavaVM* vm) {
  UNUSED(vm);
  printf("Agent_OnUnload called\n");
}

}  // namespace art
