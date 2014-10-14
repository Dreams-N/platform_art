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

#ifndef ART_DISASSEMBLER_DISASSEMBLER_ARM64_H_
#define ART_DISASSEMBLER_DISASSEMBLER_ARM64_H_

#include "disassembler.h"

#include "a64/decoder-a64.h"
#include "a64/disasm-a64.h"
#include "arch/arm64/registers_arm64.h"

namespace art {
namespace arm64 {

class CustomDisassembler : public vixl::Disassembler {
 public:
  explicit CustomDisassembler(bool read_literals = false) :
      vixl::Disassembler(), read_literals_(read_literals) {}

  // Use register aliases in the disassembly.
  virtual void AppendRegisterNameToOutput(const vixl::Instruction* instr,
                                          const vixl::CPURegister& reg);

  // Improve the disassembly of literal load instructions.
  virtual void VisitLoadLiteral(const vixl::Instruction* instr);

 private:
  // Indicate if the disassembler should read data loaded from literal pools.
  // This should only be enabled if reading the target of literal loads is safe.
  // Here are possible outputs when the option is on or off:
  // read_literals_ | disassembly
  //           true | 0x72681558: 1c000acb  ldr s11, pc+344 (addr 0x726816b0)
  //          false | 0x72681558: 1c000acb  ldr s11, pc+344 (addr 0x726816b0) (3.40282e+38)
  bool read_literals_;
};

class DisassemblerArm64 FINAL : public Disassembler {
 public:
  // TODO: Update this code once VIXL provides the ability to map code addresses
  // to disassemble as a different address (the way FormatInstructionPointer()
  // does).
  explicit DisassemblerArm64(DisassemblerOptions* options) :
      Disassembler(options), disasm(options->can_read_literals_) {
    decoder.AppendVisitor(&disasm);
  }

  size_t Dump(std::ostream& os, const uint8_t* begin) OVERRIDE;
  void Dump(std::ostream& os, const uint8_t* begin, const uint8_t* end) OVERRIDE;

 private:
  vixl::Decoder decoder;
  CustomDisassembler disasm;

  DISALLOW_COPY_AND_ASSIGN(DisassemblerArm64);
};

}  // namespace arm64
}  // namespace art

#endif  // ART_DISASSEMBLER_DISASSEMBLER_ARM64_H_
