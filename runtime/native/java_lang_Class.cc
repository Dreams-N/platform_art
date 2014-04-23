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

#include "class_linker.h"
#include "dex_file-inl.h"
#include "jni_internal.h"
#include "nth_caller_visitor.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/proxy.h"
#include "object_utils.h"
#include "reflection.h"
#include "scoped_thread_state_change.h"
#include "scoped_fast_native_object_access.h"
#include "ScopedLocalRef.h"
#include "ScopedUtfChars.h"
#include "well_known_classes.h"

namespace art {

static mirror::Class* DecodeClass(const ScopedFastNativeObjectAccess& soa, jobject java_class)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  mirror::Class* c = soa.Decode<mirror::Class*>(java_class);
  DCHECK(c != NULL);
  DCHECK(c->IsClass());
  // TODO: we could EnsureInitialized here, rather than on every reflective get/set or invoke .
  // For now, we conservatively preserve the old dalvik behavior. A quick "IsInitialized" check
  // every time probably doesn't make much difference to reflection performance anyway.
  return c;
}

// "name" is in "binary name" format, e.g. "dalvik.system.Debug$1".
static jclass Class_classForName(JNIEnv* env, jclass, jstring javaName, jboolean initialize,
                                 jobject javaLoader) {
  ScopedFastNativeObjectAccess soa(env);
  ScopedUtfChars name(env, javaName);
  if (name.c_str() == nullptr) {
    return nullptr;
  }

  // We need to validate and convert the name (from x.y.z to x/y/z).  This
  // is especially handy for array types, since we want to avoid
  // auto-generating bogus array classes.
  if (!IsValidBinaryClassName(name.c_str())) {
    ThrowLocation throw_location = soa.Self()->GetCurrentLocationForThrow();
    soa.Self()->ThrowNewExceptionF(throw_location, "Ljava/lang/ClassNotFoundException;",
                                   "Invalid name: %s", name.c_str());
    return nullptr;
  }

  std::string descriptor(DotToDescriptor(name.c_str()));
  SirtRef<mirror::ClassLoader> class_loader(soa.Self(),
                                            soa.Decode<mirror::ClassLoader*>(javaLoader));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  SirtRef<mirror::Class> c(soa.Self(), class_linker->FindClass(soa.Self(), descriptor.c_str(),
                                                               class_loader));
  if (c.get() == nullptr) {
    ScopedLocalRef<jthrowable> cause(env, env->ExceptionOccurred());
    env->ExceptionClear();
    jthrowable cnfe = reinterpret_cast<jthrowable>(env->NewObject(WellKnownClasses::java_lang_ClassNotFoundException,
                                                                  WellKnownClasses::java_lang_ClassNotFoundException_init,
                                                                  javaName, cause.get()));
    env->Throw(cnfe);
    return nullptr;
  }
  if (initialize) {
    class_linker->EnsureInitialized(c, true, true);
  }
  return soa.AddLocalReference<jclass>(c.get());
}

static jstring Class_getNameNative(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Class* c = DecodeClass(soa, javaThis);
  return soa.AddLocalReference<jstring>(c->ComputeName());
}

static jobjectArray Class_getProxyInterfaces(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::SynthesizedProxyClass* c =
      down_cast<mirror::SynthesizedProxyClass*>(DecodeClass(soa, javaThis));
  return soa.AddLocalReference<jobjectArray>(c->GetInterfaces()->Clone(soa.Self()));
}

static jobject Class_newInstance(JNIEnv* env, jobject javaThis, jobject javaConstructor) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::ArtMethod* m = mirror::ArtMethod::FromReflectedMethod(soa, javaConstructor);
  SirtRef<mirror::Class> c(soa.Self(), m->GetDeclaringClass());

  if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(c, true, true)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return nullptr;
  }

  bool movable = true;
  if (!kMovingMethods && c->IsArtMethodClass()) {
    movable = false;
  } else if (!kMovingFields && c->IsArtFieldClass()) {
    movable = false;
  } else if (!kMovingClasses && c->IsClassClass()) {
    movable = false;
  }
  mirror::Object* receiver =
      movable ? c->AllocObject(soa.Self()) : c->AllocNonMovableObject(soa.Self());
  if (receiver == nullptr) {
    return nullptr;
  }

  jobject javaReceiver = soa.AddLocalReference<jobject>(receiver);
  InvokeMethod(soa, javaConstructor, javaReceiver, nullptr, true, false);

  // Constructors are ()V methods, so we shouldn't touch the result of InvokeMethod.
  return javaReceiver;
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Class, classForName, "!(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getNameNative, "!()Ljava/lang/String;"),
  NATIVE_METHOD(Class, getProxyInterfaces, "!()[Ljava/lang/Class;"),
  NATIVE_METHOD(Class, newInstance, "!(Ljava/lang/reflect/Constructor;)Ljava/lang/Object;"),
};

void register_java_lang_Class(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Class");
}

}  // namespace art
