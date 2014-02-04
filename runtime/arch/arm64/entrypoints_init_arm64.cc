/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "entrypoints/interpreter/interpreter_entrypoints.h"
#include "entrypoints/portable/portable_entrypoints.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "entrypoints/entrypoint_utils.h"
#include "entrypoints/math_entrypoints.h"

namespace art {

// Interpreter entrypoints.
extern "C" void artInterpreterToInterpreterBridge(Thread* self, MethodHelper& mh,
                                                 const DexFile::CodeItem* code_item,
                                                 ShadowFrame* shadow_frame, JValue* result);
extern "C" void artInterpreterToCompiledCodeBridge(Thread* self, MethodHelper& mh,
                                           const DexFile::CodeItem* code_item,
                                           ShadowFrame* shadow_frame, JValue* result);

// Portable entrypoints.
extern "C" void art_portable_resolution_trampoline(mirror::ArtMethod*);
extern "C" void art_portable_to_interpreter_bridge(mirror::ArtMethod*);

// Cast entrypoints.
extern "C" uint32_t artIsAssignableFromCode(const mirror::Class* klass,
                                            const mirror::Class* ref_class);
extern "C" void art_quick_check_cast(void*, void*);

// DexCache entrypoints.
extern "C" void* art_quick_initialize_static_storage(uint32_t, void*);
extern "C" void* art_quick_initialize_type(uint32_t, void*);
extern "C" void* art_quick_initialize_type_and_verify_access(uint32_t, void*);
extern "C" void* art_quick_resolve_string(void*, uint32_t);

// Exception entrypoints.
extern "C" void* GetAndClearException(Thread*);

// Field entrypoints.
extern "C" int art_quick_set32_instance(uint32_t, void*, int32_t);
extern "C" int art_quick_set32_static(uint32_t, int32_t);
extern "C" int art_quick_set64_instance(uint32_t, void*, int64_t);
extern "C" int art_quick_set64_static(uint32_t, int64_t);
extern "C" int art_quick_set_obj_instance(uint32_t, void*, void*);
extern "C" int art_quick_set_obj_static(uint32_t, void*);
extern "C" int32_t art_quick_get32_instance(uint32_t, void*);
extern "C" int32_t art_quick_get32_static(uint32_t);
extern "C" int64_t art_quick_get64_instance(uint32_t, void*);
extern "C" int64_t art_quick_get64_static(uint32_t);
extern "C" void* art_quick_get_obj_instance(uint32_t, void*);
extern "C" void* art_quick_get_obj_static(uint32_t);

// Array entrypoints.
extern "C" void art_quick_aput_obj_with_null_and_bound_check(void*, uint32_t, void*);
extern "C" void art_quick_aput_obj_with_bound_check(void*, uint32_t, void*);
extern "C" void art_quick_aput_obj(void*, uint32_t, void*);
extern "C" void art_quick_handle_fill_data(void*, void*);

// Lock entrypoints.
extern "C" void art_quick_lock_object(void*);
extern "C" void art_quick_unlock_object(void*);

// Math entrypoints.
extern int32_t CmpgDouble(double a, double b);
extern int32_t CmplDouble(double a, double b);
extern int32_t CmpgFloat(float a, float b);
extern int32_t CmplFloat(float a, float b);

// Single-precision FP arithmetics.
extern "C" float fmodf(float a, float b);          // REM_FLOAT[_2ADDR]

// Double-precision FP arithmetics.
extern "C" double fmod(double a, double b);         // REM_DOUBLE[_2ADDR]

// Integer arithmetics.
extern "C" int __aeabi_idivmod(int32_t, int32_t);  // [DIV|REM]_INT[_2ADDR|_LIT8|_LIT16]

// Long long arithmetics - REM_LONG[_2ADDR] and DIV_LONG[_2ADDR]
extern "C" int64_t art_quick_mul_long(int64_t, int64_t);
extern "C" uint64_t art_quick_shl_long(uint64_t, uint32_t);
extern "C" uint64_t art_quick_shr_long(uint64_t, uint32_t);
extern "C" uint64_t art_quick_ushr_long(uint64_t, uint32_t);

// Intrinsic entrypoints.
extern "C" int32_t __memcmp16(void*, void*, int32_t);
extern "C" int32_t art_quick_indexof(void*, uint32_t, uint32_t, uint32_t);
extern "C" int32_t art_quick_string_compareto(void*, void*);

// Invoke entrypoints.
extern "C" void art_quick_imt_conflict_trampoline(mirror::ArtMethod*);
extern "C" void art_quick_resolution_trampoline(mirror::ArtMethod*);
extern "C" void art_quick_to_interpreter_bridge(mirror::ArtMethod*);
extern "C" void art_quick_invoke_direct_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_quick_invoke_interface_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_quick_invoke_static_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_quick_invoke_super_trampoline_with_access_check(uint32_t, void*);
extern "C" void art_quick_invoke_virtual_trampoline_with_access_check(uint32_t, void*);

// Thread entrypoints.
extern void CheckSuspendFromCode(Thread* thread);
extern "C" void art_quick_test_suspend();

// Throw entrypoints.
extern "C" void art_quick_deliver_exception(void*);
extern "C" void art_quick_throw_array_bounds(int32_t index, int32_t limit);
extern "C" void art_quick_throw_div_zero();
extern "C" void art_quick_throw_no_such_method(int32_t method_idx);
extern "C" void art_quick_throw_null_pointer_exception();
extern "C" void art_quick_throw_stack_overflow(void*);

extern void ResetQuickAllocEntryPoints(QuickEntryPoints* qpoints);

void InitEntryPoints(InterpreterEntryPoints* ipoints, JniEntryPoints* jpoints,
                     PortableEntryPoints* ppoints, QuickEntryPoints* qpoints) {
  // Interpreter
  ipoints->pInterpreterToInterpreterBridge = artInterpreterToInterpreterBridge;
  ipoints->pInterpreterToCompiledCodeBridge = artInterpreterToCompiledCodeBridge;

  // JNI
  jpoints->pDlsymLookup = art_jni_dlsym_lookup_stub;

  // Portable
  ppoints->pPortableResolutionTrampoline = art_portable_resolution_trampoline;
  ppoints->pPortableToInterpreterBridge = art_portable_to_interpreter_bridge;

  // Alloc
  ResetQuickAllocEntryPoints(qpoints);

  // Cast
  qpoints->pInstanceofNonTrivial = artIsAssignableFromCode;
  qpoints->pCheckCast = art_quick_check_cast;

  // DexCache
  qpoints->pInitializeStaticStorage = art_quick_initialize_static_storage;
  qpoints->pInitializeTypeAndVerifyAccess = art_quick_initialize_type_and_verify_access;
  qpoints->pInitializeType = art_quick_initialize_type;
  qpoints->pResolveString = art_quick_resolve_string;

  // Field
  qpoints->pSet32Instance = art_quick_set32_instance;
  qpoints->pSet32Static = art_quick_set32_static;
  qpoints->pSet64Instance = art_quick_set64_instance;
  qpoints->pSet64Static = art_quick_set64_static;
  qpoints->pSetObjInstance = art_quick_set_obj_instance;
  qpoints->pSetObjStatic = art_quick_set_obj_static;
  qpoints->pGet32Instance = art_quick_get32_instance;
  qpoints->pGet64Instance = art_quick_get64_instance;
  qpoints->pGetObjInstance = art_quick_get_obj_instance;
  qpoints->pGet32Static = art_quick_get32_static;
  qpoints->pGet64Static = art_quick_get64_static;
  qpoints->pGetObjStatic = art_quick_get_obj_static;

  // Array
  qpoints->pAputObjectWithNullAndBoundCheck = art_quick_aput_obj_with_null_and_bound_check;
  qpoints->pAputObjectWithBoundCheck = art_quick_aput_obj_with_bound_check;
  qpoints->pAputObject = art_quick_aput_obj;
  qpoints->pHandleFillArrayData = art_quick_handle_fill_data;

  // JNI
  qpoints->pJniMethodStart = JniMethodStart;
  qpoints->pJniMethodStartSynchronized = JniMethodStartSynchronized;
  qpoints->pJniMethodEnd = JniMethodEnd;
  qpoints->pJniMethodEndSynchronized = JniMethodEndSynchronized;
  qpoints->pJniMethodEndWithReference = JniMethodEndWithReference;
  qpoints->pJniMethodEndWithReferenceSynchronized = JniMethodEndWithReferenceSynchronized;

  // Locks
  qpoints->pLockObject = art_quick_lock_object;
  qpoints->pUnlockObject = art_quick_unlock_object;

  // Math
  // TODO NULL entrypoints not needed for ARM64 - generate inline.
  qpoints->pCmpgDouble = CmpgDouble;
  qpoints->pCmpgFloat = CmpgFloat;
  qpoints->pCmplDouble = CmplDouble;
  qpoints->pCmplFloat = CmplFloat;
  qpoints->pFmod = fmod;
  qpoints->pSqrt = sqrt;
  qpoints->pL2d = NULL;
  qpoints->pFmodf = fmodf;
  qpoints->pL2f = NULL;
  qpoints->pD2iz = NULL;
  qpoints->pF2iz = NULL;
  qpoints->pIdivmod = NULL;
  qpoints->pD2l = NULL;
  qpoints->pF2l = NULL;
  qpoints->pLdiv = NULL;
  qpoints->pLmod = NULL;
  qpoints->pLmul = art_quick_mul_long;
  qpoints->pShlLong = art_quick_shl_long;
  qpoints->pShrLong = art_quick_shr_long;
  qpoints->pUshrLong = art_quick_ushr_long;

  // Intrinsics
  qpoints->pIndexOf = art_quick_indexof;
  qpoints->pMemcmp16 = __memcmp16;
  qpoints->pStringCompareTo = art_quick_string_compareto;
  qpoints->pMemcpy = memcpy;

  // Invocation
  qpoints->pQuickImtConflictTrampoline = art_quick_imt_conflict_trampoline;
  qpoints->pQuickResolutionTrampoline = art_quick_resolution_trampoline;
  qpoints->pQuickToInterpreterBridge = art_quick_to_interpreter_bridge;
  qpoints->pInvokeDirectTrampolineWithAccessCheck = art_quick_invoke_direct_trampoline_with_access_check;
  qpoints->pInvokeInterfaceTrampolineWithAccessCheck = art_quick_invoke_interface_trampoline_with_access_check;
  qpoints->pInvokeStaticTrampolineWithAccessCheck = art_quick_invoke_static_trampoline_with_access_check;
  qpoints->pInvokeSuperTrampolineWithAccessCheck = art_quick_invoke_super_trampoline_with_access_check;
  qpoints->pInvokeVirtualTrampolineWithAccessCheck = art_quick_invoke_virtual_trampoline_with_access_check;

  // Thread
  qpoints->pCheckSuspend = CheckSuspendFromCode;
  qpoints->pTestSuspend = art_quick_test_suspend;

  // Throws
  qpoints->pDeliverException = art_quick_deliver_exception;
  qpoints->pThrowArrayBounds = art_quick_throw_array_bounds;
  qpoints->pThrowDivZero = art_quick_throw_div_zero;
  qpoints->pThrowNoSuchMethod = art_quick_throw_no_such_method;
  qpoints->pThrowNullPointer = art_quick_throw_null_pointer_exception;
  qpoints->pThrowStackOverflow = art_quick_throw_stack_overflow;
};

}  // namespace art
