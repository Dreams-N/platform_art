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

#include "dalvik_system_InMemoryDexClassLoader_DexData.h"

#include <sstream>

#include "class_linker.h"
#include "common_throws.h"
#include "dex_file.h"
#include "jni_internal.h"
#include "mem_map.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "scoped_thread_state_change.h"
#include "ScopedUtfChars.h"

namespace art {

static std::unique_ptr<MemMap> AllocateDexMemoryMap(jint start, jint end)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (end <= start) {
    ThrowWrappedIOException("Bad range");
    return nullptr;
  }

  std::string error_message;
  size_t length = static_cast<size_t>(end - start);
  std::unique_ptr<MemMap> dex_mem_map(MemMap::MapAnonymous("DEX data",
                                                           nullptr,
                                                           length,
                                                           PROT_READ | PROT_WRITE,
                                                           /* low_4gb */ false,
                                                           /* reuse */ false,
                                                           &error_message));
  if (dex_mem_map == nullptr) {
    ThrowWrappedIOException("%s", error_message.c_str());
  }
  return dex_mem_map;
}

static jlong DexFileToCookie(const DexFile* dex_file) {
  return reinterpret_cast<jlong>(dex_file);
}

static const DexFile* CookieToDexFile(jlong cookie) {
  return reinterpret_cast<const DexFile*>(cookie);
}

static const DexFile* CreateDexFile(std::unique_ptr<MemMap> dex_mem_map)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  std::string location = StringPrintf("InMemoryDexClassLoader_DexData@%p-%p",
                                      dex_mem_map->Begin(),
                                      dex_mem_map->End());
  std::string error_message;
  std::unique_ptr<const DexFile> dex_file(DexFile::Open(location,
                                                        0,
                                                        std::move(dex_mem_map),
                                                        /* verify */ true,
                                                        /* verify_location */ true,
                                                        &error_message));
  if (dex_file == nullptr) {
    ThrowWrappedIOException("%s", error_message.c_str());
    return nullptr;
  }

  if (!dex_file->DisableWrite()) {
    ThrowWrappedIOException("Failed to make image read-only");
    return nullptr;
  }

  return dex_file.release();
}

static jlong InMemoryDexClassLoader_DexData_initializeWithDirectBuffer(
    JNIEnv* env, jclass, jobject buffer, jint start, jint end) {
  ScopedObjectAccess soa(env);
  uint8_t* base_address = reinterpret_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
  if (base_address == nullptr) {
    ThrowWrappedIOException("dexFileBuffer not direct");
    return 0;
  }

  std::unique_ptr<MemMap> dex_mem_map(AllocateDexMemoryMap(start, end));
  if (dex_mem_map == nullptr) {
    DCHECK(soa.Self()->IsExceptionPending());
    return 0;
  }

  size_t length = static_cast<size_t>(end - start);
  memcpy(dex_mem_map->Begin(), base_address, length);
  return DexFileToCookie(CreateDexFile(std::move(dex_mem_map)));
}

static jlong InMemoryDexClassLoader_DexData_initializeWithArray(
    JNIEnv* env, jclass, jbyteArray buffer, jint start, jint end) {
  ScopedObjectAccess soa(env);

  std::unique_ptr<MemMap> dex_mem_map(AllocateDexMemoryMap(start, end));
  if (dex_mem_map == nullptr) {
    DCHECK(soa.Self()->IsExceptionPending());
    return 0;
  }

  auto destination = reinterpret_cast<jbyte*>(dex_mem_map.get()->Begin());
  env->GetByteArrayRegion(buffer, start, end - start, destination);
  return DexFileToCookie(CreateDexFile(std::move(dex_mem_map)));
}

static void InMemoryDexClassLoader_DexData_uninitialize(JNIEnv* env, jclass, jlong cookie) {
  CHECK_NE(cookie, 0);

  ScopedObjectAccess soa(env);
  const DexFile* dex_file = CookieToDexFile(cookie);
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();

  // Check file data is not in use (no longer referenced by cache).
  CHECK(class_linker->FindDexCache(soa.Self(), *dex_file, true) == nullptr);
  delete dex_file;
}

static jclass InMemoryDexClassLoader_DexData_findClass(
    JNIEnv* env, jobject dexData, jstring name, jobject loader, jlong cookie) {
  if (cookie == 0) {
    ScopedObjectAccess soa(env);
    ThrowWrappedIOException("closed");
    return nullptr;
  }

  ScopedUtfChars scoped_class_name(env, name);
  const char* class_name = scoped_class_name.c_str();
  if (env->ExceptionCheck()) {
    return nullptr;
  }

  const std::string descriptor(DotToDescriptor(class_name));
  const char* class_descriptor = descriptor.c_str();
  const size_t hash = ComputeModifiedUtf8Hash(class_descriptor);
  const DexFile* dex_file = CookieToDexFile(cookie);
  const DexFile::ClassDef* dex_class_def = dex_file->FindClassDef(class_descriptor, hash);
  if (dex_class_def != nullptr) {
      ScopedObjectAccess soa(env);
      ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
      StackHandleScope<1> handle_scope(soa.Self());
      Handle<mirror::ClassLoader> class_loader(
          handle_scope.NewHandle(soa.Decode<mirror::ClassLoader*>(loader)));
      // Add dex_file to DexCache.
      class_linker->RegisterDexFile(*dex_file, class_loader.Get());
      mirror::Class* result = class_linker->DefineClass(
          soa.Self(), class_descriptor, hash, class_loader, *dex_file, *dex_class_def);
      if (result != nullptr) {
        // Protect against the finalizer cleaning up dexData if
        // a class has been loaded from this source.
        class_linker->InsertDexFileInToClassLoader(
            soa.Decode<mirror::Object*>(dexData), class_loader.Get());
        return soa.AddLocalReference<jclass>(result);
      }
  }

  VLOG(class_linker) << "Failed to find dex_class_def " << class_name;
  return nullptr;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(InMemoryDexClassLoader_DexData,
                initializeWithDirectBuffer,
                "(Ljava/nio/ByteBuffer;II)J"),
  NATIVE_METHOD(InMemoryDexClassLoader_DexData, initializeWithArray, "([BII)J"),
  NATIVE_METHOD(InMemoryDexClassLoader_DexData, uninitialize, "(J)V"),
  NATIVE_METHOD(InMemoryDexClassLoader_DexData,
                findClass,
                "(Ljava/lang/String;Ljava/lang/ClassLoader;J)Ljava/lang/Class;"),
};

void register_dalvik_system_InMemoryDexClassLoader_DexData(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/InMemoryDexClassLoader$DexData");
}

}  // namespace art
