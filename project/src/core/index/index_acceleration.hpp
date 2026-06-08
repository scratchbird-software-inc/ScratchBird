// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-ACCELERATION-CLOSURE-ANCHOR

#include "index_family_registry.hpp"

namespace scratchbird::core::index {

enum class IndexAccelerationKind : u32 { none, llvm_jit, llvm_aot, gpu };

enum class IndexAccelerationDecisionKind : u32 { allowed, disabled_fallback_cpu, policy_blocked };

struct IndexAccelerationDecision {
  Status status;
  IndexAccelerationDecisionKind decision = IndexAccelerationDecisionKind::disabled_fallback_cpu;
  IndexAccelerationKind requested = IndexAccelerationKind::none;
  bool semantic_authority = false;
  std::string fallback_path;
  DiagnosticRecord diagnostic;
};

const char* IndexAccelerationKindName(IndexAccelerationKind kind);
const char* IndexAccelerationDecisionKindName(IndexAccelerationDecisionKind decision);
IndexAccelerationDecision EvaluateIndexAcceleration(IndexFamily family,
                                                    IndexAccelerationKind requested,
                                                    bool profile_enabled,
                                                    bool resource_epoch_current);

}  // namespace scratchbird::core::index
