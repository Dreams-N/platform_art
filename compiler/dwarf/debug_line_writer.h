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

#ifndef ART_COMPILER_DWARF_DEBUG_LINE_WRITER_H_
#define ART_COMPILER_DWARF_DEBUG_LINE_WRITER_H_

#include "debug_line_opcode_writer.h"
#include "dwarf.h"
#include "writer.h"

namespace art {
namespace dwarf {

// Writer for the .debug_line section (DWARF-3).
template<typename Allocator = std::allocator<uint8_t>>
class DebugLineWriter FINAL : private Writer<Allocator> {
 public:
  typedef struct {
    const char* file_name;
    int directory_index;
    int modification_time;
    int file_size;
  } FileEntry;

  void WriteTable(std::vector<const char*>& include_directories,
                  std::vector<FileEntry>& files,
                  DebugLineOpCodeWriter<Allocator>& opcodes) {
    size_t header_start = this->data()->size();
    this->PushUint32(0);  // Section-length placeholder.
    // Claim DWARF-2 version even though we use some DWARF-3 features.
    // DWARF-2 consumers will ignore the unknown opcodes.
    // This is what clang currently does.
    this->PushUint16(2);  // .debug_line version.
    size_t header_length_pos = this->data()->size();
    this->PushUint32(0);  // Header-length placeholder.
    this->PushUint8(1 << opcodes.code_factor_bits());
    this->PushUint8(DebugLineOpCodeWriter<Allocator>::kDefaultIsStmt ? 1 : 0);
    this->PushInt8(DebugLineOpCodeWriter<Allocator>::kLineBase);
    this->PushUint8(DebugLineOpCodeWriter<Allocator>::kLineRange);
    this->PushUint8(DebugLineOpCodeWriter<Allocator>::kOpcodeBase);
    static const int opcode_lengths[DebugLineOpCodeWriter<Allocator>::kOpcodeBase] = { 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 };
    for (int i = 1; i < DebugLineOpCodeWriter<Allocator>::kOpcodeBase; i++) {
      this->PushUint8(opcode_lengths[i]);
    }
    for (auto include_directory : include_directories) {
      this->PushString(include_directory);
    }
    this->PushUint8(0);  // Terminate include_directories list.
    for (auto file : files) {
      this->PushString(file.file_name);
      this->PushUleb128(file.directory_index);
      this->PushUleb128(file.modification_time);
      this->PushUleb128(file.file_size);
    }
    this->PushUint8(0);  // Terminate file list.
    this->UpdateUint32(header_length_pos, this->data()->size() - header_length_pos - 4);
    this->PushData(opcodes.data()->data(), opcodes.data()->size());
    this->UpdateUint32(header_start, this->data()->size() - header_start - 4);
  }

  explicit DebugLineWriter(std::vector<uint8_t, Allocator>* buffer)
    : Writer<Allocator>(buffer) {
  }
};

}  // namespace dwarf
}  // namespace art

#endif  // ART_COMPILER_DWARF_DEBUG_LINE_WRITER_H_
