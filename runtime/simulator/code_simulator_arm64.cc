/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "simulator/code_simulator_arm64.h"

namespace art {
namespace arm64 {

// VIXL has not been tested on 32bit arches, so vixl::Simulator is not always
// available. To avoid linker error on these arches, the following methods are
// wrapped with CanSimulateArm64.
// TODO: remove `if (CanSimuateArm64())` when vixl::Simulator is always available.

CodeSimulatorArm64::CodeSimulatorArm64()
    : decoder_(nullptr), simulator_(nullptr) {
  DCHECK(kCanSimulate);
  if (kCanSimulate) {
    decoder_ = new vixl::Decoder;
    simulator_ = new vixl::Simulator(decoder_);
  }
}

CodeSimulatorArm64::~CodeSimulatorArm64() {
  if (kCanSimulate) {
    delete simulator_;
    delete decoder_;
  }
}

void CodeSimulatorArm64::RunFrom(intptr_t code_buffer) {
  if (kCanSimulate) {
    simulator_->RunFrom(reinterpret_cast<const vixl::Instruction*>(code_buffer));
  }
}

bool CodeSimulatorArm64::GetCReturnBool() {
  if (kCanSimulate) {
    return simulator_->wreg(0);
  } else {
    UNREACHABLE();
  }
}

int32_t CodeSimulatorArm64::GetCReturnInt32() {
  if (kCanSimulate) {
    return simulator_->wreg(0);
  } else {
    UNREACHABLE();
  }
}

int64_t CodeSimulatorArm64::GetCReturnInt64() {
  if (kCanSimulate) {
    return simulator_->xreg(0);
  } else {
    UNREACHABLE();
  }
}

}  // namespace arm64
}  // namespace art
