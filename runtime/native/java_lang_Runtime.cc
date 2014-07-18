/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

#include "gc/heap.h"
#include "handle_scope-inl.h"
#include "jni_internal.h"
#include "mirror/class_loader.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "ScopedUtfChars.h"
#include "ScopedFd.h"
#include "ScopeGuard.h"
#include "verify_object-inl.h"

#include <ziparchive/zip_archive.h>
#ifdef HAVE_ANDROID_OS
#include "cutils/properties.h"
#ifdef __LP64__
#define CPU_ABI_LIST_PROPERTY "ro.product.cpu.abilist64"
#else
#define CPU_ABI_LIST_PROPERTY "ro.product.cpu.abilist32"
#endif
#endif

namespace art {

static void Runtime_gc(JNIEnv*, jclass) {
  if (Runtime::Current()->IsExplicitGcDisabled()) {
      LOG(INFO) << "Explicit GC skipped.";
      return;
  }
  Runtime::Current()->GetHeap()->CollectGarbage(false);
}

static void Runtime_nativeExit(JNIEnv*, jclass, jint status) {
  Runtime::Current()->CallExitHook(status);
  exit(status);
}

#ifdef HAVE_ANDROID_OS
static std::unique_ptr<const char> g_cpu_abilist_str;
static std::vector<const char*> g_cpu_abilist;
static std::vector<const char*> g_dex_path_vector;
static std::unique_ptr<const char> g_dex_path_c_str;

static void init_cpu_abilist() {
  if (g_cpu_abilist_str.get() != nullptr) {
    return;
  }

  std::unique_ptr<char> cpu_abilist_str(new char[PROPERTY_VALUE_MAX]);
  property_get(CPU_ABI_LIST_PROPERTY, cpu_abilist_str.get(), "");

  char* separator;
  char* current_str = cpu_abilist_str.get();
  while ((separator = strchr(current_str, ',')) != nullptr) {
    *separator = '\0';
    if (strlen(current_str) > 0) {
      g_cpu_abilist.push_back(current_str);
    }
    current_str = separator + 1;
  }

  if (strlen(current_str) > 0) {
    g_cpu_abilist.push_back(current_str);
  }

  if (g_cpu_abilist.empty()) {
    LOG(ERROR) << "Invalid " << CPU_ABI_LIST_PROPERTY << " property: " << cpu_abilist_str.get() << ". Won't be able to load libraries from apk";
    return;
  }

  g_cpu_abilist_str = std::move(cpu_abilist_str);
}

static void update_dex_path(const char* path) {
  g_dex_path_vector.clear();

  if (path == nullptr) {
    g_dex_path_c_str.reset(nullptr);
    return;
  }

  init_cpu_abilist();

  char* dex_path = strdup(path);
  g_dex_path_c_str.reset(dex_path);

  char* separator;
  while ((separator = strchr(dex_path, ':')) != nullptr) {
    *separator = '\0';
    if (strlen(dex_path) > 0) {
      g_dex_path_vector.push_back(dex_path);
    }
    dex_path = separator + 1;
  }

  if (strlen(dex_path) > 0) {
    g_dex_path_vector.push_back(dex_path);
  }
}

static int apk_lookup_fn(const char* filename, int* fd, off_t* offset, int* close_file) {
  if (strchr(filename, '/') != nullptr) {
    return -1;
  }

  for (auto abi : g_cpu_abilist) {
    for (auto path : g_dex_path_vector) {
      ScopedFd zip_fd(TEMP_FAILURE_RETRY(open(path, O_RDONLY | O_CLOEXEC)));
      if (zip_fd.get() == -1) {
        continue;
      }

      ZipArchiveHandle zip_handle = nullptr;
      auto zip_guard = create_scope_guard([&]() {
        if (zip_handle != nullptr) {
          CloseArchive(zip_handle);
        }
      });

      if (OpenArchiveFd(zip_fd.get(), nullptr, &zip_handle, false) != 0) {
        continue;
      }

      std::stringstream ss;
      ss << "lib/" << abi << "/" << filename;
      std::string entry_name = ss.str();

      ZipEntry entry;
      if (FindEntry(zip_handle, entry_name.c_str(), &entry) != 0 || entry.method != kCompressStored) {
        continue;
      }

      *offset = static_cast<off_t>(entry.offset);
      *fd = zip_fd.release();
      *close_file = true;

      return 0;
    }
  }

  return -1;
}
#else
static void update_dex_path(const char* path) { }
static int apk_lookup_fn(const char* filename, int* fd, off_t* offset, int* close_file) {
  return -1;
}
#endif

static jstring Runtime_nativeLoad(JNIEnv* env, jclass, jstring javaFilename, jobject javaLoader, jstring javaLdLibraryPath, jstring javaDexPath) {
  ScopedUtfChars filename(env, javaFilename);
  if (filename.c_str() == nullptr) {
    return nullptr;
  }

  if (javaLdLibraryPath != nullptr) {
    ScopedUtfChars ldLibraryPath(env, javaLdLibraryPath);
    if (ldLibraryPath.c_str() == nullptr) {
      return nullptr;
    }
    void* sym = dlsym(RTLD_DEFAULT, "android_update_LD_LIBRARY_PATH");
    if (sym != nullptr) {
      typedef void (*Fn)(const char*);
      Fn android_update_LD_LIBRARY_PATH = reinterpret_cast<Fn>(sym);
      android_update_LD_LIBRARY_PATH(ldLibraryPath.c_str());
    } else {
      LOG(ERROR) << "android_update_LD_LIBRARY_PATH not found; .so dependencies will not work!";
    }
  }

  typedef int (*lookup_fn_t)(const char* filename, int* fd, off_t* offset, int* close_flag);
  typedef void (*android_update_lookup_fn_t)(lookup_fn_t);

  android_update_lookup_fn_t android_update_lookup_fn = nullptr;
  auto guard = create_scope_guard([&]() {
    if (android_update_lookup_fn != nullptr) {
      android_update_lookup_fn(nullptr);
    }
  });

  if (javaDexPath != nullptr) {
    ScopedUtfChars dexPath(env, javaDexPath);
    void* sym = dlsym(RTLD_DEFAULT, "android_update_lookup_fn");
    if (sym != nullptr) {
      android_update_lookup_fn = reinterpret_cast<android_update_lookup_fn_t>(sym);
      update_dex_path(dexPath.c_str());
      android_update_lookup_fn(apk_lookup_fn);
    } else {
      LOG(WARNING) << "android_update_lookup_fn not found; .so dependencies may not work!";
    }
  }

  std::string detail;
  {
    ScopedObjectAccess soa(env);
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::ClassLoader> classLoader(
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(javaLoader)));
    JavaVMExt* vm = Runtime::Current()->GetJavaVM();
    bool success = vm->LoadNativeLibrary(filename.c_str(), classLoader, &detail);
    if (success) {
      return nullptr;
    }
  }

  // Don't let a pending exception from JNI_OnLoad cause a CheckJNI issue with NewStringUTF.
  env->ExceptionClear();
  return env->NewStringUTF(detail.c_str());
}

static jlong Runtime_maxMemory(JNIEnv*, jclass) {
  return Runtime::Current()->GetHeap()->GetMaxMemory();
}

static jlong Runtime_totalMemory(JNIEnv*, jclass) {
  return Runtime::Current()->GetHeap()->GetTotalMemory();
}

static jlong Runtime_freeMemory(JNIEnv*, jclass) {
  return Runtime::Current()->GetHeap()->GetFreeMemory();
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Runtime, freeMemory, "!()J"),
  NATIVE_METHOD(Runtime, gc, "()V"),
  NATIVE_METHOD(Runtime, maxMemory, "!()J"),
  NATIVE_METHOD(Runtime, nativeExit, "(I)V"),
  NATIVE_METHOD(Runtime, nativeLoad, "(Ljava/lang/String;Ljava/lang/ClassLoader;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
  NATIVE_METHOD(Runtime, totalMemory, "!()J"),
};

void register_java_lang_Runtime(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Runtime");
}

}  // namespace art
