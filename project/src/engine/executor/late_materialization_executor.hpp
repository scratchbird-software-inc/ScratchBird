// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-EXECUTOR-LATE-MATERIALIZATION-ANCHOR
#include "candidate_set.hpp"
#include "late_payload_fetch.hpp"
#include "runtime_platform.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;

struct LateMaterializationPlan {
  std::string plan_id;
  scratchbird::core::index::CandidateSet candidate_intersection;
  scratchbird::core::index::CandidateSetAuthorityContext authority;
  std::vector<scratchbird::storage::page::LatePayloadReference> payload_references;
  u64 top_k_limit = 0;
  bool candidate_intersection_proven = false;
  bool exact_predicate_recheck_required = true;
  bool mga_visibility_recheck_required = true;
  bool security_authorization_recheck_required = true;
  bool redaction_gate_required = true;
  bool top_k_pruning_required = true;
  bool parser_or_donor_finality_or_visibility_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool wal_recovery_or_finality_authority = false;
};

struct LateMaterializedRow {
  TypedUuid row_uuid;
  scratchbird::storage::page::LargePayloadDescriptor descriptor;
  bool redacted = false;
  bool payload_bytes_present = false;
  std::vector<byte> payload_bytes;
  std::string redaction_reason;
};

struct LateMaterializationCounters {
  u64 candidate_input_count = 0;
  u64 rows_after_exact_mga_security_recheck = 0;
  u64 top_k_limit = 0;
  u64 rows_after_top_k_pruning = 0;
  u64 payload_fetcher_invocation_count = 0;
  u64 payload_fetch_count = 0;
  u64 redacted_payload_count = 0;
  u64 skipped_row_count = 0;
  u64 skipped_by_exact_mga_security_count = 0;
  u64 skipped_by_top_k_count = 0;
  std::vector<TypedUuid> materialization_order;
};

struct LateMaterializationResult {
  Status status;
  bool fail_closed = false;
  LateMaterializationCounters counters;
  std::vector<LateMaterializedRow> rows;
  scratchbird::core::index::CandidateSetResult exact_recheck;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && !fail_closed; }
};

using LateMaterializationPayloadFetcher = std::function<
    scratchbird::storage::page::LatePayloadFetchResult(
        const scratchbird::storage::page::LatePayloadFetchRequest&)>;

LateMaterializationResult ExecuteLateMaterialization(
    const LateMaterializationPlan& plan,
    const LateMaterializationPayloadFetcher& payload_fetcher);

DiagnosticRecord MakeLateMaterializationDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::engine::executor
