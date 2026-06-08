// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_opcode_registry.hpp"
#include "sblr_runtime.hpp"

#include <string>

namespace scratchbird::engine::sblr {

enum class SblrLlvmExecutionMode {
  interpreter,
  llvm_jit,
  llvm_aot,
  refuse,
};

struct SblrLlvmPolicy {
  bool llvm_library_available = true;
  bool jit_enabled_by_policy = false;
  bool aot_enabled_by_policy = false;
  bool opcode_requires_llvm = false;
};

struct SblrLlvmDecision {
  SblrLlvmExecutionMode mode = SblrLlvmExecutionMode::interpreter;
  std::string diagnostic_id;
  std::string detail;
};

SblrLlvmDecision SelectSblrLlvmExecutionMode(const SblrOpcodeEntry& opcode,
                                             const SblrLlvmPolicy& policy);
SblrResult RefuseLlvmExecution(const SblrExecutionContext& context,
                               std::string operation_id,
                               const SblrLlvmDecision& decision);
std::string ToString(SblrLlvmExecutionMode mode);

}  // namespace scratchbird::engine::sblr
