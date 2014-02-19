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

#ifndef ART_RUNTIME_MIRROR_OBJECT_INL_H_
#define ART_RUNTIME_MIRROR_OBJECT_INL_H_

#include "object.h"

#include "art_field.h"
#include "art_method.h"
#include "atomic.h"
#include "array-inl.h"
#include "class.h"
#include "lock_word-inl.h"
#include "monitor.h"
#include "runtime.h"
#include "throwable.h"

namespace art {
namespace mirror {

template<size_t kVerifyFlags>
inline Class* Object::GetClass() {
  return GetFieldObject<Class, kVerifyFlags>(OFFSET_OF_OBJECT_MEMBER(Object, klass_), false);
}

template<size_t kVerifyFlags>
inline void Object::SetClass(Class* new_klass) {
  // new_klass may be NULL prior to class linker initialization.
  // We don't mark the card as this occurs as part of object allocation. Not all objects have
  // backing cards, such as large objects.
  // We use non transactional version since we can't undo this write. We also disable checking as
  // we may run in transaction mode here.
  SetFieldObjectWithoutWriteBarrier<false, false, kVerifyFlags & ~kVerifyThis>(
      OFFSET_OF_OBJECT_MEMBER(Object, klass_), new_klass, false);
}

inline LockWord Object::GetLockWord() {
  return LockWord(GetField32(OFFSET_OF_OBJECT_MEMBER(Object, monitor_), true));
}

inline void Object::SetLockWord(LockWord new_val) {
  // Force use of non-transactional mode and do not check.
  SetField32<false, false>(OFFSET_OF_OBJECT_MEMBER(Object, monitor_), new_val.GetValue(), true);
}

inline bool Object::CasLockWord(LockWord old_val, LockWord new_val) {
  // Force use of non-transactional mode and do not check.
  return CasField32<false, false>(OFFSET_OF_OBJECT_MEMBER(Object, monitor_), old_val.GetValue(),
                                  new_val.GetValue());
}

inline uint32_t Object::GetLockOwnerThreadId() {
  return Monitor::GetLockOwnerThreadId(this);
}

inline mirror::Object* Object::MonitorEnter(Thread* self) {
  return Monitor::MonitorEnter(self, this);
}

inline bool Object::MonitorExit(Thread* self) {
  return Monitor::MonitorExit(self, this);
}

inline void Object::Notify(Thread* self) {
  Monitor::Notify(self, this);
}

inline void Object::NotifyAll(Thread* self) {
  Monitor::NotifyAll(self, this);
}

inline void Object::Wait(Thread* self) {
  Monitor::Wait(self, this, 0, 0, true, kWaiting);
}

inline void Object::Wait(Thread* self, int64_t ms, int32_t ns) {
  Monitor::Wait(self, this, ms, ns, true, kTimedWaiting);
}

template<size_t kVerifyFlags>
inline bool Object::VerifierInstanceOf(Class* klass) {
  DCHECK(klass != NULL);
  DCHECK(GetClass<kVerifyFlags>() != NULL);
  return klass->IsInterface() || InstanceOf(klass);
}

template<size_t kVerifyFlags>
inline bool Object::InstanceOf(Class* klass) {
  DCHECK(klass != NULL);
  DCHECK(GetClass<kVerifyFlags>() != NULL);
  return klass->IsAssignableFrom(GetClass<false>());
}

template<size_t kVerifyFlags>
inline bool Object::IsClass() {
  Class* java_lang_Class = GetClass<kVerifyFlags>()->GetClass();
  return GetClass<kVerifyFlags & ~kVerifyThis>() == java_lang_Class;
}

template<size_t kVerifyFlags>
inline Class* Object::AsClass() {
  DCHECK(IsClass<kVerifyFlags>());
  return down_cast<Class*>(this);
}

template<size_t kVerifyFlags>
inline bool Object::IsObjectArray() {
  return IsArrayInstance<kVerifyFlags>() &&
      !GetClass<kVerifyFlags & ~kVerifyThis>()->GetComponentType()->IsPrimitive();
}

template<class T, size_t kVerifyFlags>
inline ObjectArray<T>* Object::AsObjectArray() {
  DCHECK(IsObjectArray<kVerifyFlags>());
  return down_cast<ObjectArray<T>*>(this);
}

template<size_t kVerifyFlags>
inline bool Object::IsArrayInstance() {
  return GetClass<kVerifyFlags>()->IsArrayClass();
}

template<size_t kVerifyFlags>
inline bool Object::IsArtField() {
  return GetClass<kVerifyFlags>()->IsArtFieldClass();
}

template<size_t kVerifyFlags>
inline ArtField* Object::AsArtField() {
  DCHECK(IsArtField<kVerifyFlags>());
  return down_cast<ArtField*>(this);
}

template<size_t kVerifyFlags>
inline bool Object::IsArtMethod() {
  return GetClass<kVerifyFlags>()->IsArtMethodClass();
}

template<size_t kVerifyFlags>
inline ArtMethod* Object::AsArtMethod() {
  DCHECK(IsArtMethod<kVerifyFlags>());
  return down_cast<ArtMethod*>(this);
}

template<size_t kVerifyFlags>
inline bool Object::IsReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsReferenceClass();
}

template<size_t kVerifyFlags>
inline Array* Object::AsArray() {
  DCHECK(IsArrayInstance<kVerifyFlags>());
  return down_cast<Array*>(this);
}

template<size_t kVerifyFlags>
inline BooleanArray* Object::AsBooleanArray() {
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<false>()->GetComponentType()->IsPrimitiveBoolean());
  return down_cast<BooleanArray*>(this);
}

template<size_t kVerifyFlags>
inline ByteArray* Object::AsByteArray() {
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<0>()->GetComponentType()->IsPrimitiveByte());
  return down_cast<ByteArray*>(this);
}

template<size_t kVerifyFlags>
inline ByteArray* Object::AsByteSizedArray() {
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<0>()->GetComponentType()->IsPrimitiveByte() ||
         GetClass<0>()->GetComponentType()->IsPrimitiveBoolean());
  return down_cast<ByteArray*>(this);
}

template<size_t kVerifyFlags>
inline CharArray* Object::AsCharArray() {
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<0>()->GetComponentType()->IsPrimitiveChar());
  return down_cast<CharArray*>(this);
}

template<size_t kVerifyFlags>
inline ShortArray* Object::AsShortArray() {
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<0>()->GetComponentType()->IsPrimitiveShort());
  return down_cast<ShortArray*>(this);
}

template<size_t kVerifyFlags>
inline ShortArray* Object::AsShortSizedArray() {
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<0>()->GetComponentType()->IsPrimitiveShort() ||
         GetClass<0>()->GetComponentType()->IsPrimitiveChar());
  return down_cast<ShortArray*>(this);
}

template<size_t kVerifyFlags>
inline IntArray* Object::AsIntArray() {
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<0>()->GetComponentType()->IsPrimitiveInt() ||
         GetClass<0>()->GetComponentType()->IsPrimitiveFloat());
  return down_cast<IntArray*>(this);
}

template<size_t kVerifyFlags>
inline LongArray* Object::AsLongArray() {
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<0>()->GetComponentType()->IsPrimitiveLong() ||
         GetClass<0>()->GetComponentType()->IsPrimitiveDouble());
  return down_cast<LongArray*>(this);
}

template<size_t kVerifyFlags>
inline FloatArray* Object::AsFloatArray() {
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<0>()->GetComponentType()->IsPrimitiveFloat());
  return down_cast<FloatArray*>(this);
}

template<size_t kVerifyFlags>
inline DoubleArray* Object::AsDoubleArray() {
  DCHECK(GetClass<kVerifyFlags>()->IsArrayClass());
  DCHECK(GetClass<0>()->GetComponentType()->IsPrimitiveDouble());
  return down_cast<DoubleArray*>(this);
}

template<size_t kVerifyFlags>
inline String* Object::AsString() {
  DCHECK(GetClass<kVerifyFlags>()->IsStringClass());
  return down_cast<String*>(this);
}

template<size_t kVerifyFlags>
inline Throwable* Object::AsThrowable() {
  DCHECK(GetClass<kVerifyFlags>()->IsThrowableClass());
  return down_cast<Throwable*>(this);
}

template<size_t kVerifyFlags>
inline bool Object::IsWeakReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsWeakReferenceClass();
}

template<size_t kVerifyFlags>
inline bool Object::IsSoftReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsSoftReferenceClass();
}

template<size_t kVerifyFlags>
inline bool Object::IsFinalizerReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsFinalizerReferenceClass();
}

template<size_t kVerifyFlags>
inline bool Object::IsPhantomReferenceInstance() {
  return GetClass<kVerifyFlags>()->IsPhantomReferenceClass();
}

template<size_t kVerifyFlags>
inline size_t Object::SizeOf() {
  size_t result;
  if (IsArrayInstance<kVerifyFlags>()) {
    result = AsArray<kVerifyFlags & ~kVerifyThis>()->SizeOf<kVerifyFlags & ~kVerifyThis>();
  } else if (IsClass<false>()) {
    result = AsClass<kVerifyFlags & ~kVerifyThis>()->SizeOf<kVerifyFlags & ~kVerifyThis>();
  } else {
    result = GetClass<kVerifyFlags & ~kVerifyFlags>()->GetObjectSize();
  }
  DCHECK_GE(result, sizeof(Object)) << " class=" << PrettyTypeOf(GetClass<false>());
  DCHECK(!IsArtField<kVerifyFlags & ~kVerifyThis>()  || result == sizeof(ArtField));
  DCHECK(!IsArtMethod<kVerifyFlags & ~kVerifyThis>() || result == sizeof(ArtMethod));
  return result;
}

template<size_t kVerifyFlags>
inline int32_t Object::GetField32(MemberOffset field_offset, bool is_volatile) {
  if (kVerifyFlags & kVerifyFlags) {
    VerifyObject(this);
  }
  const byte* raw_addr = reinterpret_cast<const byte*>(this) + field_offset.Int32Value();
  const int32_t* word_addr = reinterpret_cast<const int32_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    int32_t result = *(reinterpret_cast<volatile int32_t*>(const_cast<int32_t*>(word_addr)));
    QuasiAtomic::MembarLoadLoad();  // Ensure volatile loads don't re-order.
    return result;
  } else {
    return *word_addr;
  }
}

template<bool kTransactionActive, bool kCheckTransaction, size_t kVerifyFlags>
inline void Object::SetField32(MemberOffset field_offset, int32_t new_value, bool is_volatile) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField32(this, field_offset, GetField32(field_offset, is_volatile),
                                           is_volatile);
  }
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  int32_t* word_addr = reinterpret_cast<int32_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    QuasiAtomic::MembarStoreStore();  // Ensure this store occurs after others in the queue.
    *word_addr = new_value;
    QuasiAtomic::MembarStoreLoad();  // Ensure this store occurs before any volatile loads.
  } else {
    *word_addr = new_value;
  }
}

template<bool kTransactionActive, bool kCheckTransaction, size_t kVerifyFlags>
inline bool Object::CasField32(MemberOffset field_offset, int32_t old_value, int32_t new_value) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField32(this, field_offset, old_value, true);
  }
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  volatile int32_t* addr = reinterpret_cast<volatile int32_t*>(raw_addr);
  return __sync_bool_compare_and_swap(addr, old_value, new_value);
}

template<size_t kVerifyFlags>
inline int64_t Object::GetField64(MemberOffset field_offset, bool is_volatile) {
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  const byte* raw_addr = reinterpret_cast<const byte*>(this) + field_offset.Int32Value();
  const int64_t* addr = reinterpret_cast<const int64_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    int64_t result = QuasiAtomic::Read64(addr);
    QuasiAtomic::MembarLoadLoad();  // Ensure volatile loads don't re-order.
    return result;
  } else {
    return *addr;
  }
}

template<bool kTransactionActive, bool kCheckTransaction, size_t kVerifyFlags>
inline void Object::SetField64(MemberOffset field_offset, int64_t new_value, bool is_volatile) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField64(this, field_offset, GetField64(field_offset, is_volatile),
                                           is_volatile);
  }
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  int64_t* addr = reinterpret_cast<int64_t*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    QuasiAtomic::MembarStoreStore();  // Ensure this store occurs after others in the queue.
    QuasiAtomic::Write64(addr, new_value);
    if (!QuasiAtomic::LongAtomicsUseMutexes()) {
      QuasiAtomic::MembarStoreLoad();  // Ensure this store occurs before any volatile loads.
    } else {
      // Fence from from mutex is enough.
    }
  } else {
    *addr = new_value;
  }
}

template<bool kTransactionActive, bool kCheckTransaction, size_t kVerifyFlags>
inline bool Object::CasField64(MemberOffset field_offset, int64_t old_value, int64_t new_value) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteField64(this, field_offset, old_value, true);
  }
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  volatile int64_t* addr = reinterpret_cast<volatile int64_t*>(raw_addr);
  return QuasiAtomic::Cas64(old_value, new_value, addr);
}

template<class T, size_t kVerifyFlags>
inline T* Object::GetFieldObject(MemberOffset field_offset, bool is_volatile) {
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  HeapReference<T>* objref_addr = reinterpret_cast<HeapReference<T>*>(raw_addr);
  HeapReference<T> objref = *objref_addr;

  if (UNLIKELY(is_volatile)) {
    QuasiAtomic::MembarLoadLoad();  // Ensure loads don't re-order.
  }
  T* result = objref.AsMirrorPtr();
  if (kVerifyFlags & kVerifyReads) {
    VerifyObject(result);
  }
  return result;
}

template<bool kTransactionActive, bool kCheckTransaction, size_t kVerifyFlags>
inline void Object::SetFieldObjectWithoutWriteBarrier(MemberOffset field_offset, Object* new_value,
                                                      bool is_volatile) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteFieldReference(this, field_offset,
                                                  GetFieldObject<Object>(field_offset, is_volatile),
                                                  true);
  }
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  if (kVerifyFlags & kVerifyWrites) {
    VerifyObject(new_value);
  }
  HeapReference<Object> objref(HeapReference<Object>::FromMirrorPtr(new_value));
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  HeapReference<Object>* objref_addr = reinterpret_cast<HeapReference<Object>*>(raw_addr);
  if (UNLIKELY(is_volatile)) {
    QuasiAtomic::MembarStoreStore();  // Ensure this store occurs after others in the queue.
    objref_addr->Assign(new_value);
    QuasiAtomic::MembarStoreLoad();  // Ensure this store occurs before any loads.
  } else {
    objref_addr->Assign(new_value);
  }
}

template<bool kTransactionActive, bool kCheckTransaction, size_t kVerifyFlags>
inline void Object::SetFieldObject(MemberOffset field_offset, Object* new_value, bool is_volatile) {
  SetFieldObjectWithoutWriteBarrier<kTransactionActive, kCheckTransaction, kVerifyFlags>(
      field_offset, new_value, is_volatile);
  if (new_value != nullptr) {
    CheckFieldAssignment(field_offset, new_value);
    Runtime::Current()->GetHeap()->WriteBarrierField(this, field_offset, new_value);
  }
}

template <size_t kVerifyFlags>
inline HeapReference<Object>* Object::GetFieldObjectReferenceAddr(MemberOffset field_offset) {
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  return reinterpret_cast<HeapReference<Object>*>(reinterpret_cast<byte*>(this) +
      field_offset.Int32Value());
}

template<bool kTransactionActive, bool kCheckTransaction, size_t kVerifyFlags>
inline bool Object::CasFieldObject(MemberOffset field_offset, Object* old_value,
                                   Object* new_value) {
  if (kCheckTransaction) {
    DCHECK_EQ(kTransactionActive, Runtime::Current()->IsActiveTransaction());
  }
  if (kVerifyFlags & kVerifyThis) {
    VerifyObject(this);
  }
  if (kVerifyFlags & kVerifyWrites) {
    VerifyObject(new_value);
  }
  if (kTransactionActive) {
    Runtime::Current()->RecordWriteFieldReference(this, field_offset, old_value, true);
  }
  byte* raw_addr = reinterpret_cast<byte*>(this) + field_offset.Int32Value();
  volatile int32_t* addr = reinterpret_cast<volatile int32_t*>(raw_addr);
  HeapReference<Object> old_ref(HeapReference<Object>::FromMirrorPtr(old_value));
  HeapReference<Object> new_ref(HeapReference<Object>::FromMirrorPtr(new_value));
  bool success =  __sync_bool_compare_and_swap(addr, old_ref.reference_, new_ref.reference_);
  if (success) {
    Runtime::Current()->GetHeap()->WriteBarrierField(this, field_offset, new_value);
  }
  return success;
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_OBJECT_INL_H_
