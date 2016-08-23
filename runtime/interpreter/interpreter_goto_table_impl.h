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

#ifndef ART_RUNTIME_INTERPRETER_INTERPRETER_GOTO_TABLE_IMPL_H_
#define ART_RUNTIME_INTERPRETER_INTERPRETER_GOTO_TABLE_IMPL_H_

#include "base/macros.h"
#include "base/mutex.h"
#include "dex_file.h"
#include "jvalue.h"

namespace art {

class ShadowFrame;
class Thread;

namespace interpreter {

template<bool do_access_check, bool transaction_active>
JValue ExecuteGotoImpl(Thread* self,
                       const DexFile::CodeItem* code_item,
                       ShadowFrame& shadow_frame,
                       JValue result_register) SHARED_REQUIRES(Locks::mutator_lock_);

}  // namespace interpreter
}  // namespace art

#endif  // ART_RUNTIME_INTERPRETER_INTERPRETER_GOTO_TABLE_IMPL_H_
