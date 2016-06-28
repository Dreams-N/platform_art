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

#include "verifier_metadata.h"

#include "bytecode_utils.h"
#include "class_linker.h"
#include "common_runtime_test.h"
#include "dex_file.h"
#include "handle_scope-inl.h"
#include "method_verifier.h"
#include "mirror/class_loader.h"
#include "runtime.h"
#include "thread.h"
#include "scoped_thread_state_change.h"

namespace art {
namespace verifier {

class VerifierMetadataTest : public CommonRuntimeTest {
 protected:
  bool VerifyMethod(const std::string& method_name,
                    ScopedObjectAccess* soa,
                    /* out */ const DexFile::CodeItem** code,
                    /* out */ VerifierMetadata* metadata) SHARED_REQUIRES(Locks::mutator_lock_) {
    Thread* self = Thread::Current();

    jobject class_loader_object = LoadDex("VerifierMetadata");
    std::vector<const DexFile*> dex_files = GetDexFiles(class_loader_object);
    if (dex_files.size() != 1u) {
      return false;
    }
    const DexFile& dex_file = *dex_files.front();

    mirror::ClassLoader* class_loader = soa->Decode<mirror::ClassLoader*>(class_loader_object);
    ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
    class_linker->RegisterDexFile(dex_file, class_loader);

    StackHandleScope<2> hs(self);
    Handle<mirror::ClassLoader> class_loader_handle(hs.NewHandle(class_loader));

    mirror::Class* klass = class_linker->FindClass(self, "LMain;", class_loader_handle);
    if (klass == nullptr) {
      return false;
    }
    Handle<mirror::DexCache> dex_cache_handle(hs.NewHandle(klass->GetDexCache()));

    const DexFile::ClassDef* class_def = klass->GetClassDef();
    const uint8_t* class_data = dex_file.GetClassData(*class_def);
    if (class_data == nullptr) {
      return false;
    }
    ClassDataItemIterator it(dex_file, class_data);
    while (it.HasNextStaticField() || it.HasNextInstanceField()) {
      it.Next();
    }

    ArtMethod* method = nullptr;
    while (it.HasNextDirectMethod()) {
      ArtMethod* resolved_method = class_linker->ResolveMethod<ClassLinker::kNoICCECheckForCache>(
          dex_file,
          it.GetMemberIndex(),
          dex_cache_handle,
          class_loader_handle,
          nullptr,
          it.GetMethodInvokeType(*class_def));
      if (resolved_method == nullptr) {
        return false;
      }
      if (method_name == resolved_method->GetName()) {
        method = resolved_method;
        break;
      }
      it.Next();
    }
    if (method == nullptr) {
      return false;
    }

    MethodVerifier verifier(self,
                            &dex_file,
                            dex_cache_handle,
                            class_loader_handle,
                            class_def,
                            it.GetMethodCodeItem(),
                            it.GetMemberIndex(),
                            method,
                            it.GetMethodAccessFlags(),
                            true /* can_load_classes */,
                            true /* allow_soft_failures */,
                            true /* need_precise_constants */,
                            false /* verify to dump */,
                            true /* allow_thread_suspension */);
    if (!verifier.Verify()) {
      return false;
    }

    std::cout << verifier.metadata_.Dump() << std::endl;

    *code = it.GetMethodCodeItem();
    *metadata = verifier.metadata_;
    return true;
  }

  template<typename T>
  bool IsSubsetOf(const std::vector<T>& subset, std::vector<T>& superset) {
    EXPECT_TRUE(std::is_sorted(subset.begin(), subset.end()));
    std::sort(superset.begin(), superset.end());

    return (superset.size() == superset.size()) &&
           (std::includes(superset.begin(), superset.end(), subset.begin(), subset.end()));
  }

  bool HasResolvedClasses(const std::vector<std::string>& expected_classes)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    std::string tmp;
    std::vector<std::string> resolved_classes;
    for (mirror::Class* entry : metadata_.GetResolvedClasses()) {
      resolved_classes.push_back(entry->GetDescriptor(&tmp));
    }
    return IsSubsetOf(expected_classes, resolved_classes);
  }

  bool HasResolvedMethods(const std::vector<std::string>& expected_methods)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    std::string tmp;
    std::vector<std::string> resolved_methods;
    for (ArtMethod* entry : metadata_.GetResolvedMethods()) {
      resolved_methods.push_back(std::string(entry->GetDeclaringClass()->GetDescriptor(&tmp)) +
                                "->" +
                                entry->GetName() +
                                entry->GetSignature().ToString());
    }
    return IsSubsetOf(expected_methods, resolved_methods);
  }

  bool HasResolvedFields(const std::vector<std::string>& expected_fields)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    std::string tmp;
    std::vector<std::string> resolved_fields;
    for (ArtField* entry : metadata_.GetResolvedFields()) {
      resolved_fields.push_back(std::string(entry->GetDeclaringClass()->GetDescriptor(&tmp)) +
                                "->" +
                                entry->GetName() +
                                ":" +
                                entry->GetTypeDescriptor());
    }
    return IsSubsetOf(expected_fields, resolved_fields);
  }

  bool HasSubtypeRelations(const std::vector<std::string>& expected_deps)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    std::string tmp;
    std::vector<std::string> recorded_deps;
    for (const VerifierMetadata::SubtypeRelation& entry : metadata_.GetExtendsRelations()) {
      recorded_deps.push_back(std::string(entry.GetChild()->GetDescriptor(&tmp)) +
                              " extends " +
                              entry.GetParent()->GetDescriptor(&tmp));
    }
    for (const VerifierMetadata::SubtypeRelation& entry : metadata_.GetImplementsRelations()) {
      recorded_deps.push_back(std::string(entry.GetChild()->GetDescriptor(&tmp)) +
                              " implements " +
                              entry.GetParent()->GetDescriptor(&tmp));
    }
    return IsSubsetOf(expected_deps, recorded_deps);
  }

  bool HasUnresolvedClasses(const std::vector<std::string>& expected_classes) {
    auto& tmp = metadata_.GetUnresolvedClasses();
    std::vector<std::string> unresolved_classes(tmp.begin(), tmp.end());
    return IsSubsetOf(expected_classes, unresolved_classes);
  }

  bool HasUnresolvedMethods(const std::vector<std::string>& expected_methods)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    std::string tmp;
    std::vector<std::string> unresolved_methods;
    for (auto&& entry : metadata_.GetUnresolvedMethods()) {
      unresolved_methods.push_back(std::string(entry.GetDeclaringClass()->GetDescriptor(&tmp)) +
                                   "->" +
                                   entry.GetName() +
                                   entry.GetSignature());
    }
    return IsSubsetOf(expected_methods, unresolved_methods);
  }

  bool HasUnresolvedFields(const std::vector<std::string>& expected_fields)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    std::string tmp;
    std::vector<std::string> unresolved_fields;
    for (auto&& entry : metadata_.GetUnresolvedFields()) {
      unresolved_fields.push_back(std::string(entry.GetDeclaringClass()->GetDescriptor(&tmp)) +
                                  "->" +
                                  entry.GetName() +
                                  ":" +
                                  entry.GetType());
    }
    return IsSubsetOf(expected_fields, unresolved_fields);
  }

  const DexFile::CodeItem* code_;
  VerifierMetadata metadata_;
};

TEST_F(VerifierMetadataTest, ArgumentType_Resolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("ArgumentType_Resolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/IllegalStateException;" }));
}

TEST_F(VerifierMetadataTest, ArgumentType_Unresolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("ArgumentType_Unresolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasUnresolvedClasses({ "LUnresolvedClass;" }));
}

TEST_F(VerifierMetadataTest, ReturnType_Resolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("ReturnType_Resolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/IllegalStateException;" }));
}

TEST_F(VerifierMetadataTest, ReturnType_Unresolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("ReturnType_Unresolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasUnresolvedClasses({ "LUnresolvedClass;" }));
}

TEST_F(VerifierMetadataTest, ConstClass_Resolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("ConstClass_Resolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/Class;", "Ljava/lang/IllegalStateException;" }));
}

TEST_F(VerifierMetadataTest, ConstClass_Unresolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("ConstClass_Unresolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/Class;" }));
  ASSERT_TRUE(HasUnresolvedClasses({ "LUnresolvedClass;" }));
}

TEST_F(VerifierMetadataTest, CheckCast_Resolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("CheckCast_Resolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/IllegalStateException;" }));
}

TEST_F(VerifierMetadataTest, CheckCast_Unresolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("CheckCast_Unresolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasUnresolvedClasses({ "LUnresolvedClass;" }));
}

TEST_F(VerifierMetadataTest, InstanceOf_Resolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InstanceOf_Resolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/IllegalStateException;" }));
}

TEST_F(VerifierMetadataTest, InstanceOf_Unresolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InstanceOf_Unresolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasUnresolvedClasses({ "LUnresolvedClass;" }));
}

TEST_F(VerifierMetadataTest, NewInstance_Resolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("NewInstance_Resolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/IllegalStateException;" }));
}

TEST_F(VerifierMetadataTest, NewInstance_Unresolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("NewInstance_Unresolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasUnresolvedClasses({ "LUnresolvedClass;" }));
}

TEST_F(VerifierMetadataTest, NewArray_Resolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("NewArray_Resolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/Object;" }));
}

TEST_F(VerifierMetadataTest, NewArray_Unresolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("NewArray_Unresolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasUnresolvedClasses({ "[LUnresolvedClass;" }));
}

TEST_F(VerifierMetadataTest, StaticField_Resolved_DeclaredInReferenced) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("StaticField_Resolved_DeclaredInReferenced", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/io/PrintStream;", "Ljava/lang/System;" }));
  ASSERT_TRUE(HasResolvedFields({ "Ljava/lang/System;->out:Ljava/io/PrintStream;" }));
}

TEST_F(VerifierMetadataTest, StaticField_Resolved_DeclaredInSuperclass1) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("StaticField_Resolved_DeclaredInSuperclass1", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/util/SimpleTimeZone;", "Ljava/util/TimeZone;" }));
  ASSERT_TRUE(HasSubtypeRelations({ "Ljava/util/SimpleTimeZone; extends Ljava/util/TimeZone;" }));
  ASSERT_TRUE(HasResolvedFields({ "Ljava/util/TimeZone;->LONG:I" }));
}

TEST_F(VerifierMetadataTest, StaticField_Resolved_DeclaredInSuperclass2) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("StaticField_Resolved_DeclaredInSuperclass2", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/util/SimpleTimeZone;", "Ljava/util/TimeZone;" }));
  ASSERT_TRUE(HasSubtypeRelations({ "Ljava/util/SimpleTimeZone; extends Ljava/util/TimeZone;" }));
  ASSERT_TRUE(HasResolvedFields({ "Ljava/util/TimeZone;->SHORT:I" }));
}

TEST_F(VerifierMetadataTest, StaticField_Resolved_DeclaredInInterface1) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("StaticField_Resolved_DeclaredInInterface1", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({
      "Ljava/lang/String;",
      "Ljavax/xml/transform/Result;",
      "Ljavax/xml/transform/dom/DOMResult;" }));
  ASSERT_TRUE(HasResolvedFields({
      "Ljavax/xml/transform/Result;->PI_ENABLE_OUTPUT_ESCAPING:Ljava/lang/String;" }));
  ASSERT_TRUE(HasSubtypeRelations({
      "Ljavax/xml/transform/dom/DOMResult; implements Ljavax/xml/transform/Result;" }));
}

TEST_F(VerifierMetadataTest, StaticField_Resolved_DeclaredInInterface2) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("StaticField_Resolved_DeclaredInInterface2", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({
      "Ljava/lang/String;",
      "Ljavax/xml/transform/Result;",
      "Ljavax/xml/transform/dom/DOMResult;" }));
  ASSERT_TRUE(HasResolvedFields({
      "Ljavax/xml/transform/Result;->PI_ENABLE_OUTPUT_ESCAPING:Ljava/lang/String;" }));
  ASSERT_TRUE(HasSubtypeRelations({
      "Ljavax/xml/transform/dom/DOMResult; implements Ljavax/xml/transform/Result;" }));
}

TEST_F(VerifierMetadataTest, StaticField_Resolved_DeclaredInInterface3) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("StaticField_Resolved_DeclaredInInterface3", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/String;", "Ljavax/xml/transform/Result;" }));
  ASSERT_TRUE(HasResolvedFields({
      "Ljavax/xml/transform/Result;->PI_ENABLE_OUTPUT_ESCAPING:Ljava/lang/String;" }));
}

TEST_F(VerifierMetadataTest, StaticField_Resolved_DeclaredInInterface4) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("StaticField_Resolved_DeclaredInInterface4", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Lorg/w3c/dom/Document;", "Lorg/w3c/dom/Node;" }));
  ASSERT_TRUE(HasResolvedFields({ "Lorg/w3c/dom/Node;->ELEMENT_NODE:S" }));
  ASSERT_TRUE(HasSubtypeRelations({ "Lorg/w3c/dom/Document; implements Lorg/w3c/dom/Node;" }));
}

TEST_F(VerifierMetadataTest, StaticField_UnresolvedClass) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("StaticField_UnresolvedClass", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasUnresolvedClasses({ "LUnresolvedClass;" }));
}

TEST_F(VerifierMetadataTest, StaticField_UnresolvedField1) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("StaticField_UnresolvedField1", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/util/TimeZone;" }));
  ASSERT_TRUE(HasUnresolvedFields({ "Ljava/util/TimeZone;->x:I" }));
}

TEST_F(VerifierMetadataTest, StaticField_UnresolvedField2) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("StaticField_UnresolvedField2", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/util/SimpleTimeZone;" }));
  ASSERT_TRUE(HasUnresolvedFields({ "Ljava/util/SimpleTimeZone;->x:I" }));
}

TEST_F(VerifierMetadataTest, InstanceField_Resolved_DeclaredInReferenced) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod(
      "InstanceField_Resolved_DeclaredInReferenced", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/io/InterruptedIOException;" }));
  ASSERT_TRUE(HasResolvedFields({ "Ljava/io/InterruptedIOException;->bytesTransferred:I" }));
}

TEST_F(VerifierMetadataTest, InstanceField_Resolved_DeclaredInSuperclass1) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod(
      "InstanceField_Resolved_DeclaredInSuperclass1", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({
      "Ljava/io/InterruptedIOException;",
      "Ljava/net/SocketTimeoutException;" }));
  ASSERT_TRUE(HasSubtypeRelations({
      "Ljava/net/SocketTimeoutException; extends Ljava/io/InterruptedIOException;" }));
  ASSERT_TRUE(HasResolvedFields({ "Ljava/io/InterruptedIOException;->bytesTransferred:I" }));
}

TEST_F(VerifierMetadataTest, InstanceField_Resolved_DeclaredInSuperclass2) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod(
      "InstanceField_Resolved_DeclaredInSuperclass2", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({
      "Ljava/io/InterruptedIOException;",
      "Ljava/net/SocketTimeoutException;" }));
  ASSERT_TRUE(HasSubtypeRelations({
      "Ljava/net/SocketTimeoutException; extends Ljava/io/InterruptedIOException;" }));
  ASSERT_TRUE(HasResolvedFields({ "Ljava/io/InterruptedIOException;->bytesTransferred:I" }));
}

TEST_F(VerifierMetadataTest, InstanceField_UnresolvedClass) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InstanceField_UnresolvedClass", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasUnresolvedClasses({ "LUnresolvedClass;" }));
}

TEST_F(VerifierMetadataTest, InstanceField_UnresolvedField1) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InstanceField_UnresolvedField1", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/io/InterruptedIOException;" }));
  ASSERT_TRUE(HasUnresolvedFields({ "Ljava/io/InterruptedIOException;->x:I" }));
}

TEST_F(VerifierMetadataTest, InstanceField_UnresolvedField2) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InstanceField_UnresolvedField2", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/net/SocketTimeoutException;" }));
  ASSERT_TRUE(HasUnresolvedFields({ "Ljava/net/SocketTimeoutException;->x:I" }));
}

TEST_F(VerifierMetadataTest, InvokeVirtual_Resolved_DeclaredInReferenced) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod(
      "InvokeVirtual_Resolved_DeclaredInReferenced", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/String;", "Ljava/lang/Throwable;" }));
  ASSERT_TRUE(HasResolvedMethods({ "Ljava/lang/Throwable;->getMessage()Ljava/lang/String;" }));
}

TEST_F(VerifierMetadataTest, InvokeVirtual_Resolved_DeclaredInSuperclass1) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod(
      "InvokeVirtual_Resolved_DeclaredInSuperclass1", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({
      "Ljava/io/InterruptedIOException;",
      "Ljava/lang/String;",
      "Ljava/lang/Throwable;" }));
  ASSERT_TRUE(HasSubtypeRelations({
      "Ljava/io/InterruptedIOException; extends Ljava/lang/Throwable;" }));
  ASSERT_TRUE(HasResolvedMethods({ "Ljava/lang/Throwable;->getMessage()Ljava/lang/String;" }));
}

TEST_F(VerifierMetadataTest, InvokeVirtual_Resolved_DeclaredInSuperclass2) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod(
      "InvokeVirtual_Resolved_DeclaredInSuperclass2", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({
      "Ljava/lang/String;",
      "Ljava/lang/Throwable;",
      "Ljava/net/SocketTimeoutException;" }));
  ASSERT_TRUE(HasSubtypeRelations({
      "Ljava/net/SocketTimeoutException; extends Ljava/lang/Throwable;" }));
  ASSERT_TRUE(HasResolvedMethods({ "Ljava/lang/Throwable;->getMessage()Ljava/lang/String;" }));
}

TEST_F(VerifierMetadataTest, InvokeVirtual_UnresolvedClass) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InvokeVirtual_UnresolvedClass", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasUnresolvedClasses({ "LUnresolvedClass;" }));
}

TEST_F(VerifierMetadataTest, InvokeVirtual_UnresolvedMethod1) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InvokeVirtual_UnresolvedMethod1", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/io/InterruptedIOException;" }));
  ASSERT_TRUE(HasUnresolvedMethods({ "Ljava/io/InterruptedIOException;->x()V" }));
}

TEST_F(VerifierMetadataTest, InvokeVirtual_UnresolvedMethod2) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InvokeVirtual_UnresolvedMethod2", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/net/SocketTimeoutException;" }));
  ASSERT_TRUE(HasUnresolvedMethods({ "Ljava/net/SocketTimeoutException;->x()V" }));
}

TEST_F(VerifierMetadataTest, InvokeStatic_Resolved_DeclaredInReferenced) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InvokeStatic_Resolved_DeclaredInReferenced", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/net/Socket;", "Ljava/net/SocketImplFactory;" }));
  ASSERT_TRUE(HasResolvedMethods({
      "Ljava/net/Socket;->setSocketImplFactory(Ljava/net/SocketImplFactory;)V" }));
}

TEST_F(VerifierMetadataTest, InvokeStatic_Resolved_DeclaredInSuperclass1) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod(
      "InvokeStatic_Resolved_DeclaredInSuperclass1", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({
      "Ljava/net/Socket;",
      "Ljava/net/SocketImplFactory;",
      "Ljavax/net/ssl/SSLSocket;" }));
  ASSERT_TRUE(HasSubtypeRelations({ "Ljavax/net/ssl/SSLSocket; extends Ljava/net/Socket;" }));
  ASSERT_TRUE(HasResolvedMethods({
      "Ljava/net/Socket;->setSocketImplFactory(Ljava/net/SocketImplFactory;)V" }));
}

TEST_F(VerifierMetadataTest, InvokeStatic_Resolved_DeclaredInSuperclass2) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod(
      "InvokeStatic_Resolved_DeclaredInSuperclass2", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({
      "Ljava/net/Socket;",
      "Ljava/net/SocketImplFactory;",
      "Ljavax/net/ssl/SSLSocket;" }));
  ASSERT_TRUE(HasSubtypeRelations({ "Ljavax/net/ssl/SSLSocket; extends Ljava/net/Socket;" }));
  ASSERT_TRUE(HasResolvedMethods({
      "Ljava/net/Socket;->setSocketImplFactory(Ljava/net/SocketImplFactory;)V" }));
}

TEST_F(VerifierMetadataTest, InvokeStatic_DeclaredInInterface1) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InvokeStatic_DeclaredInInterface1", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/util/Map$Entry;" }));
  ASSERT_TRUE(HasResolvedMethods({
      "Ljava/util/Map$Entry;->comparingByKey()Ljava/util/Comparator;" }));
}

TEST_F(VerifierMetadataTest, InvokeStatic_DeclaredInInterface2) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InvokeStatic_DeclaredInInterface2", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/util/AbstractMap$SimpleEntry;" }));
  ASSERT_TRUE(HasUnresolvedMethods({
      "Ljava/util/AbstractMap$SimpleEntry;->comparingByKey()Ljava/util/Comparator;" }));
}

TEST_F(VerifierMetadataTest, InvokeStatic_UnresolvedClass) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InvokeStatic_UnresolvedClass", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasUnresolvedClasses({ "LUnresolvedClass;" }));
}

TEST_F(VerifierMetadataTest, InvokeStatic_UnresolvedMethod1) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InvokeStatic_UnresolvedMethod1", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljavax/net/ssl/SSLSocket;" }));
  ASSERT_TRUE(HasUnresolvedMethods({ "Ljavax/net/ssl/SSLSocket;->x()V" }));
}

TEST_F(VerifierMetadataTest, InvokeStatic_UnresolvedMethod2) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InvokeStatic_UnresolvedMethod2", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljavax/net/ssl/SSLSocket;" }));
  ASSERT_TRUE(HasUnresolvedMethods({ "Ljavax/net/ssl/SSLSocket;->x()V" }));
}

TEST_F(VerifierMetadataTest, InvokeInterface_Resolved_DeclaredInReferenced) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("InvokeInterface_Resolved_DeclaredInReferenced", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/Runnable;" }));
  ASSERT_TRUE(HasResolvedMethods({ "Ljava/lang/Runnable;->run()V" }));
}

TEST_F(VerifierMetadataTest, InvokeInterface_Resolved_DeclaredInSuperinterface1) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod(
      "InvokeInterface_Resolved_DeclaredInSuperinterface1", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/Thread;" }));
  ASSERT_TRUE(HasResolvedMethods({ "Ljava/lang/Thread;->run()V" }));
}

TEST_F(VerifierMetadataTest, InvokeInterface_Resolved_DeclaredInSuperinterface2) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod(
      "InvokeInterface_Resolved_DeclaredInSuperinterface2", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/Runnable;", "Ljava/lang/Thread;" }));
  ASSERT_TRUE(HasSubtypeRelations({ "Ljava/lang/Thread; implements Ljava/lang/Runnable;" }));
  ASSERT_TRUE(HasResolvedMethods({ "Ljava/lang/Runnable;->run()V" }));
}

TEST_F(VerifierMetadataTest, InvokeInterface) {
  ScopedObjectAccess soa(Thread::Current());

  ASSERT_TRUE(VerifyMethod("Opcode_INVOKE_INTERFACE_Resolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/Runnable;" }));

  ASSERT_TRUE(VerifyMethod("Opcode_INVOKE_INTERFACE_Unresolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasUnresolvedClasses({
      "LUnresolvedClass;",
      "LUnresolvedInterface;" }));
}

TEST_F(VerifierMetadataTest, InvokeDirect) {
  ScopedObjectAccess soa(Thread::Current());

  ASSERT_TRUE(VerifyMethod("<init>", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/io/InvalidClassException;" }));

  // TODO: INVOKE_DIRECT into an unresolved super class!!!
}

TEST_F(VerifierMetadataTest, InvokeSuper) {
  ScopedObjectAccess soa(Thread::Current());

  ASSERT_TRUE(VerifyMethod("Opcode_INVOKE_SUPER_Resolved", &soa, &code_, &metadata_));
}

TEST_F(VerifierMetadataTest, MoveException_Resolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("MoveException_Resolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/io/IOException;", "Ljava/lang/Throwable;" }));
}

TEST_F(VerifierMetadataTest, MoveException_Unresolved) {
  ScopedObjectAccess soa(Thread::Current());
  ASSERT_TRUE(VerifyMethod("MoveException_Unresolved", &soa, &code_, &metadata_));
  ASSERT_TRUE(HasResolvedClasses({ "Ljava/lang/Throwable;" }));
  ASSERT_TRUE(HasUnresolvedClasses({ "LUnresolvedException;" }));
}

}  // namespace verifier
}  // namespace art
