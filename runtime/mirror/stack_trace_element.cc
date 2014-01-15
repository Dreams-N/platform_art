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

#include "stack_trace_element.h"

#include "class.h"
#include "class-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "object-inl.h"
#include "string.h"

namespace art {
namespace mirror {

Class* StackTraceElement::java_lang_StackTraceElement_ = NULL;

void StackTraceElement::SetClass(Class* java_lang_StackTraceElement) {
  CHECK(java_lang_StackTraceElement_ == NULL);
  CHECK(java_lang_StackTraceElement != NULL);
  java_lang_StackTraceElement_ = java_lang_StackTraceElement;
}

void StackTraceElement::ResetClass() {
  CHECK(java_lang_StackTraceElement_ != NULL);
  java_lang_StackTraceElement_ = NULL;
}

StackTraceElement* StackTraceElement::Alloc(Thread* self,
                                            SirtRef<String>& declaring_class,
                                            SirtRef<String>& method_name,
                                            SirtRef<String>& file_name,
                                            int32_t line_number) {
  StackTraceElement* trace =
      down_cast<StackTraceElement*>(GetStackTraceElement()->AllocObject(self));
  if (LIKELY(trace != NULL)) {
    if (Runtime::Current()->IsActiveTransaction()) {
      trace->Init<true>(declaring_class, method_name, file_name, line_number);
    } else {
      trace->Init<false>(declaring_class, method_name, file_name, line_number);
    }
  }
  return trace;
}

template<bool kTransactionActive>
void StackTraceElement::Init(SirtRef<String>& declaring_class, SirtRef<String>& method_name,
                             SirtRef<String>& file_name, int32_t line_number) {
  SetFieldObject<kTransactionActive>(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, declaring_class_),
                                     declaring_class.get(), false);
  SetFieldObject<kTransactionActive>(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, method_name_),
                                     method_name.get(), false);
  SetFieldObject<kTransactionActive>(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, file_name_),
                                     file_name.get(), false);
  SetField32<kTransactionActive>(OFFSET_OF_OBJECT_MEMBER(StackTraceElement, line_number_),
                                 line_number, false);
}

void StackTraceElement::VisitRoots(RootVisitor* visitor, void* arg) {
  if (java_lang_StackTraceElement_ != nullptr) {
    java_lang_StackTraceElement_ = down_cast<Class*>(visitor(java_lang_StackTraceElement_, arg));
  }
}


}  // namespace mirror
}  // namespace art
