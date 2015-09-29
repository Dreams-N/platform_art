/*
 * Copyright (C) 2012 The Android Open Source Project
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

#ifndef ART_RUNTIME_ENTRYPOINTS_ENTRYPOINT_UTILS_INL_H_
#define ART_RUNTIME_ENTRYPOINTS_ENTRYPOINT_UTILS_INL_H_

#include "entrypoint_utils.h"

#include "art_method.h"
#include "class_linker-inl.h"
#include "common_throws.h"
#include "dex_file.h"
#include "entrypoints/quick/callee_save_frame.h"
#include "handle_scope-inl.h"
#include "indirect_reference_table.h"
#include "invoke_type.h"
#include "jni_internal.h"
#include "mirror/array.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/throwable.h"
#include "nth_caller_visitor.h"
#include "runtime.h"
#include "thread.h"

namespace art {

inline ArtMethod* GetResolvedMethod(ArtMethod* outer_method,
                                    const InlineInfo& inline_info,
                                    uint8_t inlining_depth)
  SHARED_REQUIRES(Locks::mutator_lock_) {
  uint32_t method_index = inline_info.GetMethodIndexAtDepth(inlining_depth);
  InvokeType invoke_type = static_cast<InvokeType>(
        inline_info.GetInvokeTypeAtDepth(inlining_depth));
  ArtMethod* caller = outer_method->GetDexCacheResolvedMethod(method_index, sizeof(void*));
  if (!caller->IsRuntimeMethod()) {
    return caller;
  }

  // The method in the dex cache can be the runtime method responsible for invoking
  // the stub that will then update the dex cache. Therefore, we need to do the
  // resolution ourselves.

  // We first find the class loader of our caller. If it is the outer method, we can directly
  // use its class loader. Otherwise, we also need to resolve our caller.
  StackHandleScope<2> hs(Thread::Current());
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  MutableHandle<mirror::ClassLoader> class_loader(hs.NewHandle<mirror::Class>(nullptr));
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(outer_method->GetDexCache()));
  if (inlining_depth == 0) {
    class_loader.Assign(outer_method->GetClassLoader());
  } else {
    caller = GetResolvedMethod(outer_method, inline_info, inlining_depth - 1);
    class_loader.Assign(caller->GetClassLoader());
  }

  return class_linker->ResolveMethod(
      *outer_method->GetDexFile(), method_index, dex_cache, class_loader, nullptr, invoke_type);
}

extern "C" void art_quick_instrumentation_exit();

inline ArtMethod* GetCalleeSaveMethodCaller(ArtMethod** sp,
                                            Runtime::CalleeSaveType type,
                                            bool do_caller_check = false)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  DCHECK_EQ(*sp, Runtime::Current()->GetCalleeSaveMethod(type));

  const size_t callee_frame_size = GetCalleeSaveFrameSize(kRuntimeISA, type);
  auto** caller_sp = reinterpret_cast<ArtMethod**>(
      reinterpret_cast<uintptr_t>(sp) + callee_frame_size);
  ArtMethod* outer_method = *caller_sp;
  ArtMethod* caller = outer_method;

  if ((outer_method != nullptr) && outer_method->IsOptimized(sizeof(void*))) {
    const size_t callee_return_pc_offset = GetCalleeSaveReturnPcOffset(kRuntimeISA, type);
    uintptr_t caller_pc = *reinterpret_cast<uintptr_t*>(
        (reinterpret_cast<uint8_t*>(sp) + callee_return_pc_offset));
    if (LIKELY(caller_pc != reinterpret_cast<uintptr_t>(art_quick_instrumentation_exit))) {
      uintptr_t native_pc_offset = outer_method->NativeQuickPcOffset(caller_pc);
      CodeInfo code_info = outer_method->GetOptimizedCodeInfo();
      StackMapEncoding encoding = code_info.ExtractEncoding();
      StackMap stack_map = code_info.GetStackMapForNativePcOffset(native_pc_offset, encoding);
      DCHECK(stack_map.IsValid());
      if (stack_map.HasInlineInfo(encoding)) {
        InlineInfo inline_info = code_info.GetInlineInfoOf(stack_map, encoding);
        caller = GetResolvedMethod(outer_method, inline_info, inline_info.GetDepth() - 1);
      }
    } else {
      // We're instrumenting, just use the StackVisitor which knows how to
      // handle instrumented frames.
      NthCallerVisitor visitor(Thread::Current(), 1, true);
      visitor.WalkStack();
      caller = visitor.caller;
      do_caller_check = false;
    }
  }

  if (kIsDebugBuild && do_caller_check) {
    // Note that do_caller_check is optional, as this method can be called by
    // stubs, and tests without a proper call stack.
    NthCallerVisitor visitor(Thread::Current(), 1, true);
    visitor.WalkStack();
    CHECK_EQ(caller, visitor.caller);
  }

  return caller;
}

inline ArtMethod* GetCalleeSaveMethodCaller(Thread* self, Runtime::CalleeSaveType type)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return GetCalleeSaveMethodCaller(
      self->GetManagedStack()->GetTopQuickFrame(), type, true /* do_caller_check */);
}

template <const bool kAccessCheck>
ALWAYS_INLINE
inline mirror::Class* CheckObjectAlloc(uint32_t type_idx,
                                       ArtMethod* method,
                                       Thread* self, bool* slow_path) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  size_t pointer_size = class_linker->GetImagePointerSize();
  mirror::Class* klass = method->GetDexCacheResolvedType<false>(type_idx, pointer_size);
  if (UNLIKELY(klass == nullptr)) {
    klass = class_linker->ResolveType(type_idx, method);
    *slow_path = true;
    if (klass == nullptr) {
      DCHECK(self->IsExceptionPending());
      return nullptr;  // Failure
    } else {
      DCHECK(!self->IsExceptionPending());
    }
  }
  if (kAccessCheck) {
    if (UNLIKELY(!klass->IsInstantiable())) {
      self->ThrowNewException("Ljava/lang/InstantiationError;", PrettyDescriptor(klass).c_str());
      *slow_path = true;
      return nullptr;  // Failure
    }
    mirror::Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      ThrowIllegalAccessErrorClass(referrer, klass);
      *slow_path = true;
      return nullptr;  // Failure
    }
  }
  if (UNLIKELY(!klass->IsInitialized())) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_klass(hs.NewHandle(klass));
    // EnsureInitialized (the class initializer) might cause a GC.
    // may cause us to suspend meaning that another thread may try to
    // change the allocator while we are stuck in the entrypoints of
    // an old allocator. Also, the class initialization may fail. To
    // handle these cases we mark the slow path boolean as true so
    // that the caller knows to check the allocator type to see if it
    // has changed and to null-check the return value in case the
    // initialization fails.
    *slow_path = true;
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(self, h_klass, true, true)) {
      DCHECK(self->IsExceptionPending());
      return nullptr;  // Failure
    } else {
      DCHECK(!self->IsExceptionPending());
    }
    return h_klass.Get();
  }
  return klass;
}

ALWAYS_INLINE
inline mirror::Class* CheckClassInitializedForObjectAlloc(mirror::Class* klass,
                                                          Thread* self,
                                                          bool* slow_path) {
  if (UNLIKELY(!klass->IsInitialized())) {
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_class(hs.NewHandle(klass));
    // EnsureInitialized (the class initializer) might cause a GC.
    // may cause us to suspend meaning that another thread may try to
    // change the allocator while we are stuck in the entrypoints of
    // an old allocator. Also, the class initialization may fail. To
    // handle these cases we mark the slow path boolean as true so
    // that the caller knows to check the allocator type to see if it
    // has changed and to null-check the return value in case the
    // initialization fails.
    *slow_path = true;
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(self, h_class, true, true)) {
      DCHECK(self->IsExceptionPending());
      return nullptr;  // Failure
    }
    return h_class.Get();
  }
  return klass;
}

// Given the context of a calling Method, use its DexCache to resolve a type to a Class. If it
// cannot be resolved, throw an error. If it can, use it to create an instance.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
template <bool kAccessCheck, bool kInstrumented>
ALWAYS_INLINE
inline mirror::Object* AllocObjectFromCode(uint32_t type_idx,
                                           ArtMethod* method,
                                           Thread* self,
                                           gc::AllocatorType allocator_type) {
  bool slow_path = false;
  mirror::Class* klass = CheckObjectAlloc<kAccessCheck>(type_idx, method, self, &slow_path);
  if (UNLIKELY(slow_path)) {
    if (klass == nullptr) {
      return nullptr;
    }
    return klass->Alloc<kInstrumented>(self, Runtime::Current()->GetHeap()->GetCurrentAllocator());
  }
  DCHECK(klass != nullptr);
  return klass->Alloc<kInstrumented>(self, allocator_type);
}

// Given the context of a calling Method and a resolved class, create an instance.
template <bool kInstrumented>
ALWAYS_INLINE
inline mirror::Object* AllocObjectFromCodeResolved(mirror::Class* klass,
                                                   Thread* self,
                                                   gc::AllocatorType allocator_type) {
  DCHECK(klass != nullptr);
  bool slow_path = false;
  klass = CheckClassInitializedForObjectAlloc(klass, self, &slow_path);
  if (UNLIKELY(slow_path)) {
    if (klass == nullptr) {
      return nullptr;
    }
    gc::Heap* heap = Runtime::Current()->GetHeap();
    // Pass in false since the object can not be finalizable.
    return klass->Alloc<kInstrumented, false>(self, heap->GetCurrentAllocator());
  }
  // Pass in false since the object can not be finalizable.
  return klass->Alloc<kInstrumented, false>(self, allocator_type);
}

// Given the context of a calling Method and an initialized class, create an instance.
template <bool kInstrumented>
ALWAYS_INLINE
inline mirror::Object* AllocObjectFromCodeInitialized(mirror::Class* klass,
                                                      Thread* self,
                                                      gc::AllocatorType allocator_type) {
  DCHECK(klass != nullptr);
  // Pass in false since the object can not be finalizable.
  return klass->Alloc<kInstrumented, false>(self, allocator_type);
}


template <bool kAccessCheck>
ALWAYS_INLINE
inline mirror::Class* CheckArrayAlloc(uint32_t type_idx,
                                      int32_t component_count,
                                      ArtMethod* method,
                                      bool* slow_path) {
  if (UNLIKELY(component_count < 0)) {
    ThrowNegativeArraySizeException(component_count);
    *slow_path = true;
    return nullptr;  // Failure
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  size_t pointer_size = class_linker->GetImagePointerSize();
  mirror::Class* klass = method->GetDexCacheResolvedType<false>(type_idx, pointer_size);
  if (UNLIKELY(klass == nullptr)) {  // Not in dex cache so try to resolve
    klass = class_linker->ResolveType(type_idx, method);
    *slow_path = true;
    if (klass == nullptr) {  // Error
      DCHECK(Thread::Current()->IsExceptionPending());
      return nullptr;  // Failure
    }
    CHECK(klass->IsArrayClass()) << PrettyClass(klass);
  }
  if (kAccessCheck) {
    mirror::Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      ThrowIllegalAccessErrorClass(referrer, klass);
      *slow_path = true;
      return nullptr;  // Failure
    }
  }
  return klass;
}

// Given the context of a calling Method, use its DexCache to resolve a type to an array Class. If
// it cannot be resolved, throw an error. If it can, use it to create an array.
// When verification/compiler hasn't been able to verify access, optionally perform an access
// check.
template <bool kAccessCheck, bool kInstrumented>
ALWAYS_INLINE
inline mirror::Array* AllocArrayFromCode(uint32_t type_idx,
                                         int32_t component_count,
                                         ArtMethod* method,
                                         Thread* self,
                                         gc::AllocatorType allocator_type) {
  bool slow_path = false;
  mirror::Class* klass = CheckArrayAlloc<kAccessCheck>(type_idx, component_count, method,
                                                       &slow_path);
  if (UNLIKELY(slow_path)) {
    if (klass == nullptr) {
      return nullptr;
    }
    gc::Heap* heap = Runtime::Current()->GetHeap();
    return mirror::Array::Alloc<kInstrumented>(self, klass, component_count,
                                               klass->GetComponentSizeShift(),
                                               heap->GetCurrentAllocator());
  }
  return mirror::Array::Alloc<kInstrumented>(self, klass, component_count,
                                             klass->GetComponentSizeShift(), allocator_type);
}

template <bool kAccessCheck, bool kInstrumented>
ALWAYS_INLINE
inline mirror::Array* AllocArrayFromCodeResolved(mirror::Class* klass,
                                                 int32_t component_count,
                                                 ArtMethod* method,
                                                 Thread* self,
                                                 gc::AllocatorType allocator_type) {
  DCHECK(klass != nullptr);
  if (UNLIKELY(component_count < 0)) {
    ThrowNegativeArraySizeException(component_count);
    return nullptr;  // Failure
  }
  if (kAccessCheck) {
    mirror::Class* referrer = method->GetDeclaringClass();
    if (UNLIKELY(!referrer->CanAccess(klass))) {
      ThrowIllegalAccessErrorClass(referrer, klass);
      return nullptr;  // Failure
    }
  }
  // No need to retry a slow-path allocation as the above code won't cause a GC or thread
  // suspension.
  return mirror::Array::Alloc<kInstrumented>(self, klass, component_count,
                                             klass->GetComponentSizeShift(), allocator_type);
}

template<FindFieldType type, bool access_check>
inline ArtField* FindFieldFromCode(uint32_t field_idx, ArtMethod* referrer,
                                           Thread* self, size_t expected_size) {
  bool is_primitive;
  bool is_set;
  bool is_static;
  switch (type) {
    case InstanceObjectRead:     is_primitive = false; is_set = false; is_static = false; break;
    case InstanceObjectWrite:    is_primitive = false; is_set = true;  is_static = false; break;
    case InstancePrimitiveRead:  is_primitive = true;  is_set = false; is_static = false; break;
    case InstancePrimitiveWrite: is_primitive = true;  is_set = true;  is_static = false; break;
    case StaticObjectRead:       is_primitive = false; is_set = false; is_static = true;  break;
    case StaticObjectWrite:      is_primitive = false; is_set = true;  is_static = true;  break;
    case StaticPrimitiveRead:    is_primitive = true;  is_set = false; is_static = true;  break;
    case StaticPrimitiveWrite:   // Keep GCC happy by having a default handler, fall-through.
    default:                     is_primitive = true;  is_set = true;  is_static = true;  break;
  }
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  ArtField* resolved_field = class_linker->ResolveField(field_idx, referrer, is_static);
  if (UNLIKELY(resolved_field == nullptr)) {
    DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
    return nullptr;  // Failure.
  }
  mirror::Class* fields_class = resolved_field->GetDeclaringClass();
  if (access_check) {
    if (UNLIKELY(resolved_field->IsStatic() != is_static)) {
      ThrowIncompatibleClassChangeErrorField(resolved_field, is_static, referrer);
      return nullptr;
    }
    mirror::Class* referring_class = referrer->GetDeclaringClass();
    if (UNLIKELY(!referring_class->CheckResolvedFieldAccess(fields_class, resolved_field,
                                                            field_idx))) {
      DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
      return nullptr;  // Failure.
    }
    if (UNLIKELY(is_set && resolved_field->IsFinal() && (fields_class != referring_class))) {
      ThrowIllegalAccessErrorFinalField(referrer, resolved_field);
      return nullptr;  // Failure.
    } else {
      if (UNLIKELY(resolved_field->IsPrimitiveType() != is_primitive ||
                   resolved_field->FieldSize() != expected_size)) {
        self->ThrowNewExceptionF("Ljava/lang/NoSuchFieldError;",
                                 "Attempted read of %zd-bit %s on field '%s'",
                                 expected_size * (32 / sizeof(int32_t)),
                                 is_primitive ? "primitive" : "non-primitive",
                                 PrettyField(resolved_field, true).c_str());
        return nullptr;  // Failure.
      }
    }
  }
  if (!is_static) {
    // instance fields must be being accessed on an initialized class
    return resolved_field;
  } else {
    // If the class is initialized we're done.
    if (LIKELY(fields_class->IsInitialized())) {
      return resolved_field;
    } else {
      StackHandleScope<1> hs(self);
      Handle<mirror::Class> h_class(hs.NewHandle(fields_class));
      if (LIKELY(class_linker->EnsureInitialized(self, h_class, true, true))) {
        // Otherwise let's ensure the class is initialized before resolving the field.
        return resolved_field;
      }
      DCHECK(self->IsExceptionPending());  // Throw exception and unwind
      return nullptr;  // Failure.
    }
  }
}

// Explicit template declarations of FindFieldFromCode for all field access types.
#define EXPLICIT_FIND_FIELD_FROM_CODE_TEMPLATE_DECL(_type, _access_check) \
template SHARED_REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE \
ArtField* FindFieldFromCode<_type, _access_check>(uint32_t field_idx, \
                                                  ArtMethod* referrer, \
                                                  Thread* self, size_t expected_size) \

#define EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(_type) \
    EXPLICIT_FIND_FIELD_FROM_CODE_TEMPLATE_DECL(_type, false); \
    EXPLICIT_FIND_FIELD_FROM_CODE_TEMPLATE_DECL(_type, true)

EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(InstanceObjectRead);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(InstanceObjectWrite);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(InstancePrimitiveRead);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(InstancePrimitiveWrite);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(StaticObjectRead);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(StaticObjectWrite);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(StaticPrimitiveRead);
EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL(StaticPrimitiveWrite);

#undef EXPLICIT_FIND_FIELD_FROM_CODE_TYPED_TEMPLATE_DECL
#undef EXPLICIT_FIND_FIELD_FROM_CODE_TEMPLATE_DECL

template<InvokeType type, bool access_check>
inline ArtMethod* FindMethodFromCode(uint32_t method_idx, mirror::Object** this_object,
                                     ArtMethod* referrer, Thread* self) {
  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  ArtMethod* resolved_method = class_linker->GetResolvedMethod(method_idx, referrer);
  if (resolved_method == nullptr) {
    StackHandleScope<1> hs(self);
    mirror::Object* null_this = nullptr;
    HandleWrapper<mirror::Object> h_this(
        hs.NewHandleWrapper(type == kStatic ? &null_this : this_object));
    resolved_method = class_linker->ResolveMethod(self, method_idx, referrer, type);
  }
  if (UNLIKELY(resolved_method == nullptr)) {
    DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
    return nullptr;  // Failure.
  } else if (UNLIKELY(*this_object == nullptr && type != kStatic)) {
    // Maintain interpreter-like semantics where NullPointerException is thrown
    // after potential NoSuchMethodError from class linker.
    ThrowNullPointerExceptionForMethodAccess(method_idx, type);
    return nullptr;  // Failure.
  } else if (access_check) {
    // Incompatible class change should have been handled in resolve method.
    if (UNLIKELY(resolved_method->CheckIncompatibleClassChange(type))) {
      ThrowIncompatibleClassChangeError(type, resolved_method->GetInvokeType(), resolved_method,
                                        referrer);
      return nullptr;  // Failure.
    }
    mirror::Class* methods_class = resolved_method->GetDeclaringClass();
    mirror::Class* referring_class = referrer->GetDeclaringClass();
    bool can_access_resolved_method =
        referring_class->CheckResolvedMethodAccess<type>(methods_class, resolved_method,
                                                         method_idx);
    if (UNLIKELY(!can_access_resolved_method)) {
      DCHECK(self->IsExceptionPending());  // Throw exception and unwind.
      return nullptr;  // Failure.
    }
  }
  switch (type) {
    case kStatic:
    case kDirect:
      return resolved_method;
    case kVirtual: {
      mirror::Class* klass = (*this_object)->GetClass();
      uint16_t vtable_index = resolved_method->GetMethodIndex();
      if (access_check &&
          (!klass->HasVTable() ||
           vtable_index >= static_cast<uint32_t>(klass->GetVTableLength()))) {
        // Behavior to agree with that of the verifier.
        ThrowNoSuchMethodError(type, resolved_method->GetDeclaringClass(),
                               resolved_method->GetName(), resolved_method->GetSignature());
        return nullptr;  // Failure.
      }
      DCHECK(klass->HasVTable()) << PrettyClass(klass);
      return klass->GetVTableEntry(vtable_index, class_linker->GetImagePointerSize());
    }
    case kSuper: {
      mirror::Class* super_class = referrer->GetDeclaringClass()->GetSuperClass();
      uint16_t vtable_index = resolved_method->GetMethodIndex();
      if (access_check) {
        // Check existence of super class.
        if (super_class == nullptr || !super_class->HasVTable() ||
            vtable_index >= static_cast<uint32_t>(super_class->GetVTableLength())) {
          // Behavior to agree with that of the verifier.
          ThrowNoSuchMethodError(type, resolved_method->GetDeclaringClass(),
                                 resolved_method->GetName(), resolved_method->GetSignature());
          return nullptr;  // Failure.
        }
      } else {
        // Super class must exist.
        DCHECK(super_class != nullptr);
      }
      DCHECK(super_class->HasVTable());
      return super_class->GetVTableEntry(vtable_index, class_linker->GetImagePointerSize());
    }
    case kInterface: {
      uint32_t imt_index = resolved_method->GetDexMethodIndex() % mirror::Class::kImtSize;
      ArtMethod* imt_method = (*this_object)->GetClass()->GetEmbeddedImTableEntry(
          imt_index, class_linker->GetImagePointerSize());
      if (!imt_method->IsImtConflictMethod() && !imt_method->IsImtUnimplementedMethod()) {
        if (kIsDebugBuild) {
          mirror::Class* klass = (*this_object)->GetClass();
          ArtMethod* method = klass->FindVirtualMethodForInterface(
              resolved_method, class_linker->GetImagePointerSize());
          CHECK_EQ(imt_method, method) << PrettyMethod(resolved_method) << " / " <<
              PrettyMethod(imt_method) << " / " << PrettyMethod(method) << " / " <<
              PrettyClass(klass);
        }
        return imt_method;
      } else {
        ArtMethod* interface_method = (*this_object)->GetClass()->FindVirtualMethodForInterface(
            resolved_method, class_linker->GetImagePointerSize());
        if (UNLIKELY(interface_method == nullptr)) {
          ThrowIncompatibleClassChangeErrorClassForInterfaceDispatch(resolved_method,
                                                                     *this_object, referrer);
          return nullptr;  // Failure.
        }
        return interface_method;
      }
    }
    default:
      LOG(FATAL) << "Unknown invoke type " << type;
      return nullptr;  // Failure.
  }
}

// Explicit template declarations of FindMethodFromCode for all invoke types.
#define EXPLICIT_FIND_METHOD_FROM_CODE_TEMPLATE_DECL(_type, _access_check)                 \
  template SHARED_REQUIRES(Locks::mutator_lock_) ALWAYS_INLINE                       \
  ArtMethod* FindMethodFromCode<_type, _access_check>(uint32_t method_idx,         \
                                                      mirror::Object** this_object, \
                                                      ArtMethod* referrer, \
                                                      Thread* self)
#define EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(_type) \
    EXPLICIT_FIND_METHOD_FROM_CODE_TEMPLATE_DECL(_type, false);   \
    EXPLICIT_FIND_METHOD_FROM_CODE_TEMPLATE_DECL(_type, true)

EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(kStatic);
EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(kDirect);
EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(kVirtual);
EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(kSuper);
EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL(kInterface);

#undef EXPLICIT_FIND_METHOD_FROM_CODE_TYPED_TEMPLATE_DECL
#undef EXPLICIT_FIND_METHOD_FROM_CODE_TEMPLATE_DECL

// Fast path field resolution that can't initialize classes or throw exceptions.
inline ArtField* FindFieldFast(uint32_t field_idx, ArtMethod* referrer, FindFieldType type,
                               size_t expected_size) {
  ArtField* resolved_field =
      referrer->GetDeclaringClass()->GetDexCache()->GetResolvedField(field_idx, sizeof(void*));
  if (UNLIKELY(resolved_field == nullptr)) {
    return nullptr;
  }
  // Check for incompatible class change.
  bool is_primitive;
  bool is_set;
  bool is_static;
  switch (type) {
    case InstanceObjectRead:     is_primitive = false; is_set = false; is_static = false; break;
    case InstanceObjectWrite:    is_primitive = false; is_set = true;  is_static = false; break;
    case InstancePrimitiveRead:  is_primitive = true;  is_set = false; is_static = false; break;
    case InstancePrimitiveWrite: is_primitive = true;  is_set = true;  is_static = false; break;
    case StaticObjectRead:       is_primitive = false; is_set = false; is_static = true;  break;
    case StaticObjectWrite:      is_primitive = false; is_set = true;  is_static = true;  break;
    case StaticPrimitiveRead:    is_primitive = true;  is_set = false; is_static = true;  break;
    case StaticPrimitiveWrite:   is_primitive = true;  is_set = true;  is_static = true;  break;
    default:
      LOG(FATAL) << "UNREACHABLE";
      UNREACHABLE();
  }
  if (UNLIKELY(resolved_field->IsStatic() != is_static)) {
    // Incompatible class change.
    return nullptr;
  }
  mirror::Class* fields_class = resolved_field->GetDeclaringClass();
  if (is_static) {
    // Check class is initialized else fail so that we can contend to initialize the class with
    // other threads that may be racing to do this.
    if (UNLIKELY(!fields_class->IsInitialized())) {
      return nullptr;
    }
  }
  mirror::Class* referring_class = referrer->GetDeclaringClass();
  if (UNLIKELY(!referring_class->CanAccess(fields_class) ||
               !referring_class->CanAccessMember(fields_class, resolved_field->GetAccessFlags()) ||
               (is_set && resolved_field->IsFinal() && (fields_class != referring_class)))) {
    // Illegal access.
    return nullptr;
  }
  if (UNLIKELY(resolved_field->IsPrimitiveType() != is_primitive ||
               resolved_field->FieldSize() != expected_size)) {
    return nullptr;
  }
  return resolved_field;
}

// Fast path method resolution that can't throw exceptions.
inline ArtMethod* FindMethodFast(uint32_t method_idx, mirror::Object* this_object,
                                 ArtMethod* referrer, bool access_check, InvokeType type) {
  if (UNLIKELY(this_object == nullptr && type != kStatic)) {
    return nullptr;
  }
  ArtMethod* resolved_method =
      referrer->GetDeclaringClass()->GetDexCache()->GetResolvedMethod(method_idx, sizeof(void*));
  if (UNLIKELY(resolved_method == nullptr)) {
    return nullptr;
  }
  if (access_check) {
    // Check for incompatible class change errors and access.
    bool icce = resolved_method->CheckIncompatibleClassChange(type);
    if (UNLIKELY(icce)) {
      return nullptr;
    }
    mirror::Class* methods_class = resolved_method->GetDeclaringClass();
    mirror::Class* referring_class = referrer->GetDeclaringClass();
    if (UNLIKELY(!referring_class->CanAccess(methods_class) ||
                 !referring_class->CanAccessMember(methods_class,
                                                   resolved_method->GetAccessFlags()))) {
      // Potential illegal access, may need to refine the method's class.
      return nullptr;
    }
  }
  if (type == kInterface) {  // Most common form of slow path dispatch.
    return this_object->GetClass()->FindVirtualMethodForInterface(resolved_method, sizeof(void*));
  } else if (type == kStatic || type == kDirect) {
    return resolved_method;
  } else if (type == kSuper) {
    return referrer->GetDeclaringClass()->GetSuperClass()->GetVTableEntry(
        resolved_method->GetMethodIndex(), sizeof(void*));
  } else {
    DCHECK(type == kVirtual);
    return this_object->GetClass()->GetVTableEntry(
        resolved_method->GetMethodIndex(), sizeof(void*));
  }
}

inline mirror::Class* ResolveVerifyAndClinit(uint32_t type_idx, ArtMethod* referrer, Thread* self,
                                             bool can_run_clinit, bool verify_access) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  mirror::Class* klass = class_linker->ResolveType(type_idx, referrer);
  if (UNLIKELY(klass == nullptr)) {
    CHECK(self->IsExceptionPending());
    return nullptr;  // Failure - Indicate to caller to deliver exception
  }
  // Perform access check if necessary.
  mirror::Class* referring_class = referrer->GetDeclaringClass();
  if (verify_access && UNLIKELY(!referring_class->CanAccess(klass))) {
    ThrowIllegalAccessErrorClass(referring_class, klass);
    return nullptr;  // Failure - Indicate to caller to deliver exception
  }
  // If we're just implementing const-class, we shouldn't call <clinit>.
  if (!can_run_clinit) {
    return klass;
  }
  // If we are the <clinit> of this class, just return our storage.
  //
  // Do not set the DexCache InitializedStaticStorage, since that implies <clinit> has finished
  // running.
  if (klass == referring_class && referrer->IsConstructor() && referrer->IsStatic()) {
    return klass;
  }
  StackHandleScope<1> hs(self);
  Handle<mirror::Class> h_class(hs.NewHandle(klass));
  if (!class_linker->EnsureInitialized(self, h_class, true, true)) {
    CHECK(self->IsExceptionPending());
    return nullptr;  // Failure - Indicate to caller to deliver exception
  }
  return h_class.Get();
}

inline mirror::String* ResolveStringFromCode(ArtMethod* referrer, uint32_t string_idx) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return class_linker->ResolveString(string_idx, referrer);
}

inline void UnlockJniSynchronizedMethod(jobject locked, Thread* self) {
  // Save any pending exception over monitor exit call.
  mirror::Throwable* saved_exception = nullptr;
  if (UNLIKELY(self->IsExceptionPending())) {
    saved_exception = self->GetException();
    self->ClearException();
  }
  // Decode locked object and unlock, before popping local references.
  self->DecodeJObject(locked)->MonitorExit(self);
  if (UNLIKELY(self->IsExceptionPending())) {
    LOG(FATAL) << "Synchronized JNI code returning with an exception:\n"
        << saved_exception->Dump()
        << "\nEncountered second exception during implicit MonitorExit:\n"
        << self->GetException()->Dump();
  }
  // Restore pending exception.
  if (saved_exception != nullptr) {
    self->SetException(saved_exception);
  }
}

template <typename INT_TYPE, typename FLOAT_TYPE>
inline INT_TYPE art_float_to_integral(FLOAT_TYPE f) {
  const INT_TYPE kMaxInt = static_cast<INT_TYPE>(std::numeric_limits<INT_TYPE>::max());
  const INT_TYPE kMinInt = static_cast<INT_TYPE>(std::numeric_limits<INT_TYPE>::min());
  const FLOAT_TYPE kMaxIntAsFloat = static_cast<FLOAT_TYPE>(kMaxInt);
  const FLOAT_TYPE kMinIntAsFloat = static_cast<FLOAT_TYPE>(kMinInt);
  if (LIKELY(f > kMinIntAsFloat)) {
     if (LIKELY(f < kMaxIntAsFloat)) {
       return static_cast<INT_TYPE>(f);
     } else {
       return kMaxInt;
     }
  } else {
    return (f != f) ? 0 : kMinInt;  // f != f implies NaN
  }
}

}  // namespace art

#endif  // ART_RUNTIME_ENTRYPOINTS_ENTRYPOINT_UTILS_INL_H_
