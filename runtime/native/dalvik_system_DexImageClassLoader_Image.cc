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

#include "dalvik_system_DexImageClassLoader_Image.h"

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

static MemMap* allocateAnonymousMemory(JNIEnv* env, jint start, jint end) {
  // Make a private copy of the data to mitigate risks of tampering with the data and
  // ensure appropriate alignment.

  if (end <= start) {
    ScopedObjectAccess soa(env);
    ThrowWrappedIOException("Bad range");
    return nullptr;
  }

  std::string error_message;
  size_t length = static_cast<size_t>(end - start);
  MemMap* allocated_map = MemMap::MapAnonymous("",
                                               nullptr,
                                               length,
                                               PROT_READ | PROT_WRITE,
                                               false,
                                               false,
                                               &error_message);
  if (allocated_map == nullptr) {
    ScopedObjectAccess soa(env);
    ThrowWrappedIOException("%s", error_message.c_str());
  }
  return allocated_map;
}

static jlong DexFileToCookie(const DexFile* dex_file) {
  return reinterpret_cast<jlong>(dex_file);
}

static const DexFile* CookieToDexFile(jlong cookie) {
  return reinterpret_cast<const DexFile*>(cookie);
}

static const DexFile* CreateDexFile(JNIEnv* env, MemMap* map) {
  std::ostringstream os;
  os << "DexImageClassLoader_Image@"
     << static_cast<void*>(map->Begin()) << '-' << static_cast<void*>(map->End());

  std::string location(os.str());
  std::string error_message;
  std::unique_ptr<const DexFile> dex_file(DexFile::Open(location, 0, map, &error_message));
  if (dex_file.get() == nullptr) {
    ScopedObjectAccess soa(env);
    ThrowWrappedIOException("%s", error_message.c_str());
    return nullptr;
  }

  if (!dex_file->DisableWrite()) {
    ScopedObjectAccess soa(env);
    ThrowWrappedIOException("Failed to make image read-only");
    return nullptr;
  }

  return dex_file.release();
}

static jlong DexImageClassLoader_Image_initializeWithDirectBuffer(
    JNIEnv* env, jclass, jobject buffer, jint start, jint end) {
  ScopedObjectAccess soa(env);
  uint8_t* base_address = reinterpret_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
  if (base_address == nullptr) {
    ThrowWrappedIOException("dexFileBuffer not direct");
    return 0;
  }

  MemMap* allocated_map = allocateAnonymousMemory(env, start, end);
  if (allocated_map == nullptr) {
    return 0;
  }

  size_t length = static_cast<size_t>(end - start);
  memcpy(allocated_map->Begin(), base_address, length);
  return DexFileToCookie(CreateDexFile(env, allocated_map));
}

static jlong DexImageClassLoader_Image_initializeWithArray(
    JNIEnv* env, jclass, jbyteArray buffer, jint start, jint end) {
  ScopedObjectAccess soa(env);

  MemMap* allocated_map = allocateAnonymousMemory(env, start, end);
  if (allocated_map == nullptr) {
    return 0;
  }

  auto destination = reinterpret_cast<jbyte*>(allocated_map->Begin());
  env->GetByteArrayRegion(buffer, start, end - start, destination);

  return DexFileToCookie(CreateDexFile(env, allocated_map));
}

static jlong DexImageClassLoader_Image_uninitialize(JNIEnv* env, jclass, jlong cookie) {
  CHECK_NE(cookie, 0);
  ScopedObjectAccess soa(env);
  const DexFile* dex_file = CookieToDexFile(cookie);
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  if (class_linker->FindDexCache(soa.Self(), *dex_file, true) == nullptr) {
    // Only delete dex_file if not found in the dex_cache to prevent
    // runtime crashes if there are calls to DexImageClassLoader_Image.close()
    // while image is in use.
    delete dex_file;
    cookie = 0;
  }
  return cookie;
}

static jclass DexImageClassLoader_Image_findClass(
    JNIEnv* env, jobject dexMemoryImage, jstring name, jobject loader, jlong cookie) {
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
        // Protect against the finalizer cleaning up dexMemoryImage if
        // a class has been loaded from this source.
        class_linker->InsertDexFileInToClassLoader(
            soa.Decode<mirror::Object*>(dexMemoryImage), class_loader.Get());
        return soa.AddLocalReference<jclass>(result);
      }
  }

  VLOG(class_linker) << "Failed to find dex_class_def " << class_name;
  return nullptr;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(DexImageClassLoader_Image, initializeWithDirectBuffer,
                "(Ljava/nio/ByteBuffer;II)J"),
  NATIVE_METHOD(DexImageClassLoader_Image, initializeWithArray, "([BII)J"),
  NATIVE_METHOD(DexImageClassLoader_Image, uninitialize, "(J)J"),
  NATIVE_METHOD(DexImageClassLoader_Image, findClass,
                "(Ljava/lang/String;Ljava/lang/ClassLoader;J)Ljava/lang/Class;"),
};

void register_dalvik_system_DexImageClassLoader_Image(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/DexImageClassLoader$Image");
}

}  // namespace art
