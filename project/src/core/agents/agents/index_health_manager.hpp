// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class IndexHealthManagerDecisionKind : u32 {
  no_action,
  recommend_index_rebuild,
  recommend_index_drop,
  request_fast_filespace_for_index_rebuild,
  refused
};

struct IndexHealthManagerPolicy {
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool rebuild_recommendation_allowed = true;
  bool drop_recommendation_allowed = true;
  bool filespace_request_allowed = true;
  u64 read_amplification_threshold = 4;
  u64 unused_window_microseconds = 604800000000ull;
  u64 fsync_p99_fast_filespace_threshold_microseconds = 2000;
};

struct IndexHealthManagerSnapshot {
  std::string index_uuid;
  u64 read_amplification_ratio = 0;
  u64 lookup_latency_p99_microseconds = 0;
  u64 unused_for_microseconds = 0;
  u64 filespace_fsync_p99_microseconds = 0;
  bool index_metrics_authoritative = false;
  bool filespace_metrics_authoritative = false;
  bool index_visible = true;
  bool index_unique_or_constraint_backed = false;
  bool parser_authority = false;
  bool donor_authority = false;
};

struct IndexHealthManagerEvidenceField {
  std::string key;
  std::string value;
};

struct IndexHealthManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexHealthManagerDecisionKind decision =
      IndexHealthManagerDecisionKind::refused;
  std::vector<IndexHealthManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool advisory_only = true;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* IndexHealthManagerDecisionKindName(
    IndexHealthManagerDecisionKind decision);
IndexHealthManagerResult EvaluateIndexHealthManager(
    const IndexHealthManagerSnapshot& snapshot,
    const IndexHealthManagerPolicy& policy = {});
DiagnosticRecord MakeIndexHealthManagerDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {});

const char* index_health_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
