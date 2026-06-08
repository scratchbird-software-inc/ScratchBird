// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_llvm_seam.hpp"

#include "sblr_refusal.hpp"

#include <utility>

namespace scratchbird::engine::sblr {

SblrLlvmDecision SelectSblrLlvmExecutionMode(const SblrOpcodeEntry& opcode,
                                             const SblrLlvmPolicy& policy) {
  SblrLlvmDecision decision;
  if (!policy.llvm_library_available && policy.opcode_requires_llvm) {
    decision.mode = SblrLlvmExecutionMode::refuse;
    decision.diagnostic_id = "SB_DIAG_LLVM_REQUIRED_UNAVAILABLE";
    decision.detail = opcode.operation_id;
    return decision;
  }
  if (policy.jit_enabled_by_policy && policy.llvm_library_available && opcode.support == SblrOpcodeSupport::implemented) {
    decision.mode = SblrLlvmExecutionMode::llvm_jit;
    return decision;
  }
  if (policy.aot_enabled_by_policy && policy.llvm_library_available && opcode.support == SblrOpcodeSupport::implemented) {
    decision.mode = SblrLlvmExecutionMode::llvm_aot;
    return decision;
  }
  decision.mode = SblrLlvmExecutionMode::interpreter;
  decision.diagnostic_id = "SB_DIAG_LLVM_LOWERING_FALLBACK";
  decision.detail = opcode.operation_id;
  return decision;
}

SblrResult RefuseLlvmExecution(const SblrExecutionContext& context,
                               std::string operation_id,
                               const SblrLlvmDecision& decision) {
  return RefuseSblrOperation(context,
                             std::move(operation_id),
                             decision.diagnostic_id.empty() ? "SB_DIAG_LLVM_REQUIRED_UNAVAILABLE" : decision.diagnostic_id,
                             decision.detail);
}

std::string ToString(SblrLlvmExecutionMode mode) {
  switch (mode) {
    case SblrLlvmExecutionMode::interpreter: return "interpreter";
    case SblrLlvmExecutionMode::llvm_jit: return "llvm_jit";
    case SblrLlvmExecutionMode::llvm_aot: return "llvm_aot";
    case SblrLlvmExecutionMode::refuse: return "refuse";
  }
  return "refuse";
}

}  // namespace scratchbird::engine::sblr
