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

#ifndef ART_COMPILER_DEX_QUICK_X86_BACKEND_X86_H_
#define ART_COMPILER_DEX_QUICK_X86_BACKEND_X86_H_

namespace art {

struct CompilationUnit;
class Mir2Lir;
class MIRGraph;
class ArenaAllocator;
class Thread;

Mir2Lir* X86CodeGenerator(CompilationUnit* const cu, MIRGraph* const mir_graph,
                          ArenaAllocator* const arena, Thread* self);

}  // namespace art

#endif  // ART_COMPILER_DEX_QUICK_X86_BACKEND_X86_H_
