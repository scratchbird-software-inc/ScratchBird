// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-POLICY-CLOSURE-ANCHOR

#include "index_family_registry.hpp"

#include <string_view>

namespace scratchbird::core::index {

enum class IndexOperationRight : u32 {
  create,
  alter,
  drop,
  rebuild,
  verify,
  move,
  inspect,
  read_metrics,
  helper_use
};

struct IndexPolicyDecision {
  Status status;
  bool allowed = false;
  std::string required_right;
  DiagnosticRecord diagnostic;
};

const char* IndexOperationRightName(IndexOperationRight right);
IndexPolicyDecision EvaluateIndexPolicy(IndexOperationRight right,
                                        std::string_view principal_group,
                                        bool is_owner,
                                        bool security_admin_override = false);

}  // namespace scratchbird::core::index
