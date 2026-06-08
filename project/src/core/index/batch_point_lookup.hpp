// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-BATCH-POINT-LOOKUP-PRIMITIVE-ANCHOR
#include "candidate_set.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class BatchPointLookupPurpose : u32 {
  unknown = 0,
  key_value = 1,
  document_payload = 2,
  vector_rerank_payload = 3,
  graph_frontier = 4,
  search_payload = 5,
  foreign_key_check = 6,
  time_series_bucket = 7
};

struct BatchPointLookupKey {
  std::string encoded_key;
  u64 input_ordinal = 0;
};

struct BatchPointLookupPlan {
  BatchPointLookupPurpose purpose = BatchPointLookupPurpose::unknown;
  std::vector<BatchPointLookupKey> keys;
  std::string plan_id;
  bool stable_input_order_required = true;
  bool preserve_duplicate_keys = true;
  bool per_key_miss_diagnostics_required = true;
  bool exact_key_recheck_required = true;
  bool row_mga_visibility_recheck_required = true;
  bool row_security_authorization_recheck_required = true;
  bool cluster_route_requested = false;
  bool cluster_guard_checked = true;
  bool cluster_provider_authorized = false;
  std::vector<std::string> caller_evidence;
};

struct BatchPointLookupProviderRequest {
  BatchPointLookupPurpose purpose = BatchPointLookupPurpose::unknown;
  std::vector<BatchPointLookupKey> ordered_unique_keys;
  std::string plan_id;
  std::vector<std::string> caller_evidence;
};

struct BatchPointLookupProviderRow {
  std::string encoded_key;
  CandidateSetRow candidate;
  std::string payload;
  std::vector<std::pair<std::string, std::string>> attributes;
  bool exact_key_match = true;
  bool exact_row_uuid = true;
  bool ordered_point_lookup = true;
};

struct BatchPointLookupProviderResult {
  Status status;
  bool fail_closed = false;
  std::vector<BatchPointLookupProviderRow> rows;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

using BatchPointLookupProvider =
    std::function<BatchPointLookupProviderResult(
        const BatchPointLookupProviderRequest&)>;

struct BatchPointLookupRow {
  std::string encoded_key;
  u64 input_ordinal = 0;
  u64 duplicate_ordinal = 0;
  bool duplicate_key = false;
  TypedUuid row_uuid;
  double score = 0.0;
  std::string payload;
  std::vector<std::pair<std::string, std::string>> attributes;
};

struct BatchPointLookupMiss {
  std::string encoded_key;
  u64 input_ordinal = 0;
  std::string reason;
  DiagnosticRecord diagnostic;
};

struct BatchPointLookupResult {
  Status status;
  bool fail_closed = false;
  BatchPointLookupPurpose purpose = BatchPointLookupPurpose::unknown;
  std::vector<BatchPointLookupRow> rows;
  std::vector<BatchPointLookupMiss> misses;
  CandidateSetResult candidate_stream;
  CandidateSetResult exact_recheck;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;
  u64 input_key_count = 0;
  u64 unique_key_count = 0;
  u64 duplicate_key_occurrences = 0;
  bool provider_batch_executed = false;
  bool final_rows_authorized = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* BatchPointLookupPurposeName(BatchPointLookupPurpose purpose);

BatchPointLookupResult RunBatchPointLookup(
    const BatchPointLookupPlan& plan,
    const CandidateSetAuthorityContext& authority,
    const BatchPointLookupProvider& provider);

DiagnosticRecord MakeBatchPointLookupDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {});

}  // namespace scratchbird::core::index
