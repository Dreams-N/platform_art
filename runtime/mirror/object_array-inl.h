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

#ifndef ART_RUNTIME_MIRROR_OBJECT_ARRAY_INL_H_
#define ART_RUNTIME_MIRROR_OBJECT_ARRAY_INL_H_

#include "object_array.h"

#include "gc/heap.h"
#include "mirror/art_field.h"
#include "mirror/class.h"
#include "runtime.h"
#include "sirt_ref.h"
#include "thread.h"
#include <string>

namespace art {
namespace mirror {

template<class T>
inline ObjectArray<T>* ObjectArray<T>::Alloc(Thread* self, Class* object_array_class,
                                             int32_t length, gc::AllocatorType allocator_type) {
  Array* array = Array::Alloc<true>(self, object_array_class, length,
                                    sizeof(HeapReference<Object>), allocator_type);
  if (UNLIKELY(array == nullptr)) {
    return nullptr;
  } else {
    return array->AsObjectArray<T>();
  }
}

template<class T>
inline ObjectArray<T>* ObjectArray<T>::Alloc(Thread* self, Class* object_array_class,
                                             int32_t length) {
  return Alloc(self, object_array_class, length,
               Runtime::Current()->GetHeap()->GetCurrentAllocator());
}

template<class T>
inline T* ObjectArray<T>::Get(int32_t i) {
  if (UNLIKELY(!CheckIsValidIndex(i))) {
    DCHECK(Thread::Current()->IsExceptionPending());
    return NULL;
  }
  return GetFieldObject<T>(OffsetOfElement(i), false);
}

template<class T> template<size_t kVerifyFlags>
inline bool ObjectArray<T>::CheckAssignable(T* object) {
  if (object != NULL) {
    Class* element_class = GetClass<kVerifyFlags>()->GetComponentType();
    if (UNLIKELY(!object->InstanceOf(element_class))) {
      ThrowArrayStoreException(object);
      return false;
    }
  }
  return true;
}

template<class T>
inline void ObjectArray<T>::Set(int32_t i, T* object) {
  if (Runtime::Current()->IsActiveTransaction()) {
    Set<true>(i, object);
  } else {
    Set<false>(i, object);
  }
}

template<class T>
template<bool kTransactionActive, bool kCheckTransaction, size_t kVerifyFlags>
inline void ObjectArray<T>::Set(int32_t i, T* object) {
  if (LIKELY(CheckIsValidIndex(i) && CheckAssignable<kVerifyFlags>(object))) {
    SetFieldObject<kTransactionActive, kCheckTransaction, kVerifyFlags>(OffsetOfElement(i), object,
                                                                        false);
  } else {
    DCHECK(Thread::Current()->IsExceptionPending());
  }
}

template<class T>
template<bool kTransactionActive, bool kCheckTransaction, size_t kVerifyFlags>
inline void ObjectArray<T>::SetWithoutChecks(int32_t i, T* object) {
  DCHECK(CheckIsValidIndex<kVerifyFlags>(i));
  DCHECK(CheckAssignable<0>(object));
  SetFieldObject<kTransactionActive, kCheckTransaction>(OffsetOfElement(i), object, false);
}

template<class T>
template<bool kTransactionActive, bool kCheckTransaction, size_t kVerifyFlags>
inline void ObjectArray<T>::SetWithoutChecksAndWriteBarrier(int32_t i, T* object) {
  DCHECK(CheckIsValidIndex<kVerifyFlags>(i));
  // TODO:  enable this check. It fails when writing the image in ImageWriter::FixupObjectArray.
  // DCHECK(CheckAssignable(object));
  SetFieldObjectWithoutWriteBarrier<kTransactionActive, kCheckTransaction, kVerifyFlags>(
      OffsetOfElement(i), object, false);
}

template<class T>
inline T* ObjectArray<T>::GetWithoutChecks(int32_t i) {
  DCHECK(CheckIsValidIndex(i));
  return GetFieldObject<T>(OffsetOfElement(i), false);
}

template<class T>
inline void ObjectArray<T>::AssignableMemmove(int32_t dst_pos, ObjectArray<T>* src,
                                              int32_t src_pos, int32_t count) {
  if (kIsDebugBuild) {
    for (int i = 0; i < count; ++i) {
      // The Get will perform the VerifyObject.
      src->GetWithoutChecks(src_pos + i);
    }
  }
  // Perform the memmove using int memmove then perform the write barrier.
  CHECK_EQ(sizeof(HeapReference<T>), sizeof(uint32_t));
  IntArray* dstAsIntArray = reinterpret_cast<IntArray*>(this);
  IntArray* srcAsIntArray = reinterpret_cast<IntArray*>(src);
  dstAsIntArray->Memmove(dst_pos, srcAsIntArray, src_pos, count);
  Runtime::Current()->GetHeap()->WriteBarrierArray(this, dst_pos, count);
  if (kIsDebugBuild) {
    for (int i = 0; i < count; ++i) {
      // The Get will perform the VerifyObject.
      GetWithoutChecks(dst_pos + i);
    }
  }
}

template<class T>
inline void ObjectArray<T>::AssignableMemcpy(int32_t dst_pos, ObjectArray<T>* src,
                                             int32_t src_pos, int32_t count) {
  if (kIsDebugBuild) {
    for (int i = 0; i < count; ++i) {
      // The Get will perform the VerifyObject.
      src->GetWithoutChecks(src_pos + i);
    }
  }
  // Perform the memmove using int memcpy then perform the write barrier.
  CHECK_EQ(sizeof(HeapReference<T>), sizeof(uint32_t));
  IntArray* dstAsIntArray = reinterpret_cast<IntArray*>(this);
  IntArray* srcAsIntArray = reinterpret_cast<IntArray*>(src);
  dstAsIntArray->Memcpy(dst_pos, srcAsIntArray, src_pos, count);
  Runtime::Current()->GetHeap()->WriteBarrierArray(this, dst_pos, count);
  if (kIsDebugBuild) {
    for (int i = 0; i < count; ++i) {
      // The Get will perform the VerifyObject.
      GetWithoutChecks(dst_pos + i);
    }
  }
}

template<class T>
inline void ObjectArray<T>::AssignableCheckingMemcpy(int32_t dst_pos, ObjectArray<T>* src,
                                                     int32_t src_pos, int32_t count,
                                                     bool throw_exception) {
  DCHECK_NE(this, src)
      << "This case should be handled with memmove that handles overlaps correctly";
  // We want to avoid redundant IsAssignableFrom checks where possible, so we cache a class that
  // we know is assignable to the destination array's component type.
  Class* dst_class = GetClass()->GetComponentType();
  Class* lastAssignableElementClass = dst_class;

  Object* o = nullptr;
  int i = 0;
  for (; i < count; ++i) {
    // The follow get operations force the objects to be verified.
    o = src->GetWithoutChecks(src_pos + i);
    if (o == nullptr) {
      // Null is always assignable.
      SetWithoutChecks<false>(dst_pos + i, nullptr);
    } else {
      // TODO: use the underlying class reference to avoid uncompression when not necessary.
      Class* o_class = o->GetClass();
      if (LIKELY(lastAssignableElementClass == o_class)) {
        SetWithoutChecks<false>(dst_pos + i, o);
      } else if (LIKELY(dst_class->IsAssignableFrom(o_class))) {
        lastAssignableElementClass = o_class;
        SetWithoutChecks<false>(dst_pos + i, o);
      } else {
        // Can't put this element into the array, break to perform write-barrier and throw
        // exception.
        break;
      }
    }
  }
  Runtime::Current()->GetHeap()->WriteBarrierArray(this, dst_pos, count);
  if (UNLIKELY(i != count)) {
    std::string actualSrcType(PrettyTypeOf(o));
    std::string dstType(PrettyTypeOf(this));
    Thread* self = Thread::Current();
    ThrowLocation throw_location = self->GetCurrentLocationForThrow();
    if (throw_exception) {
      self->ThrowNewExceptionF(throw_location, "Ljava/lang/ArrayStoreException;",
                               "source[%d] of type %s cannot be stored in destination array of type %s",
                               src_pos + i, actualSrcType.c_str(), dstType.c_str());
    } else {
      LOG(FATAL) << StringPrintf("source[%d] of type %s cannot be stored in destination array of type %s",
                                 src_pos + i, actualSrcType.c_str(), dstType.c_str());
    }
  }
}

template<class T>
inline ObjectArray<T>* ObjectArray<T>::CopyOf(Thread* self, int32_t new_length) {
  DCHECK_GE(new_length, 0);
  // We may get copied by a compacting GC.
  SirtRef<ObjectArray<T> > sirt_this(self, this);
  gc::Heap* heap = Runtime::Current()->GetHeap();
  gc::AllocatorType allocator_type = heap->IsMovableObject(this) ? heap->GetCurrentAllocator() :
      heap->GetCurrentNonMovingAllocator();
  ObjectArray<T>* new_array = Alloc(self, GetClass(), new_length, allocator_type);
  if (LIKELY(new_array != nullptr)) {
    new_array->AssignableMemcpy(0, sirt_this.get(), 0, std::min(sirt_this->GetLength(), new_length));
  }
  return new_array;
}

template<class T>
inline MemberOffset ObjectArray<T>::OffsetOfElement(int32_t i) {
  return MemberOffset(DataOffset(sizeof(HeapReference<Object>)).Int32Value() +
                      (i * sizeof(HeapReference<Object>)));
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_OBJECT_ARRAY_INL_H_
