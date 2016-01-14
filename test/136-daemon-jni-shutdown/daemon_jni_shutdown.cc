/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "base/casts.h"
#include "java_vm_ext.h"
#include "jni_env_ext.h"
#include "thread-inl.h"

namespace art {
namespace {

static volatile std::atomic<bool> vm_was_shutdown(false);

extern "C" JNIEXPORT void JNICALL Java_Main_waitAndCallIntoJniEnv(JNIEnv* env, jclass) {
  // Wait until the runtime is shutdown.
  while (!vm_was_shutdown.load()) {
    usleep(0);
  }
  std::cout << "About to call exception check\n";
  env->ExceptionCheck();
  LOG(ERROR) << "Should not be reached!";
}

NO_RETURN extern "C" JNIEXPORT void JNICALL Java_Main_destroyJavaVMAndExit(JNIEnv* env, jclass) {
  // Fake up the managed stack so we can detach.
  Thread* const self = Thread::Current();
  self->SetTopOfStack(nullptr);
  self->SetTopOfShadowStack(nullptr);
  JavaVM* vm = down_cast<JNIEnvExt*>(env)->vm;
  vm->DetachCurrentThread();
  vm->DestroyJavaVM();
  vm_was_shutdown.store(true);
  // Give threads some time to get stuck in ExceptionCheck.
  usleep(1000);
  exit(0);
}

}  // namespace
}  // namespace art
