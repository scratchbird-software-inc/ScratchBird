// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "batch_point_lookup.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefusalStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::engine};
}

struct RowKey {
  TypedUuid uuid;

  friend bool operator<(const RowKey& left, const RowKey& right) {
    if (left.uuid.kind != right.uuid.kind) {
      return static_cast<u32>(left.uuid.kind) <
             static_cast<u32>(right.uuid.kind);
    }
    return left.uuid.value.bytes < right.uuid.value.bytes;
  }
};

bool RowUuidLess(const BatchPointLookupProviderRow& left,
                 const BatchPointLookupProviderRow& right) {
  return RowKey{left.candidate.row_uuid} < RowKey{right.candidate.row_uuid};
}

bool ValidRowUuid(const TypedUuid& uuid) {
  return uuid.valid() && uuid.kind == UuidKind::row;
}

std::string CountEvidence(std::string key, u64 value) {
  return std::move(key) + "=" + std::to_string(value);
}

std::vector<std::string> BaseEvidence(BatchPointLookupPurpose purpose,
                                      u64 input_key_count,
                                      u64 unique_key_count) {
  return {"batch_point_lookup.purpose=" +
              std::string(BatchPointLookupPurposeName(purpose)),
          CountEvidence("batch_point_lookup.input_key_count", input_key_count),
          CountEvidence("batch_point_lookup.unique_key_count", unique_key_count),
          "batch_point_lookup.output_order=input_ordinal_then_row_uuid",
          "batch_point_lookup.duplicate_key_policy=preserve_occurrences",
          "batch_point_lookup.per_key_miss_diagnostics=true",
          "batch_point_lookup.exact_key_recheck.required=true",
          "batch_point_lookup.mga_visibility_recheck.required=true",
          "batch_point_lookup.security_authorization_recheck.required=true",
          "batch_point_lookup.transaction_finality_authority=false",
          "batch_point_lookup.visibility_authority=false"};
}

void AppendEvidence(std::vector<std::string>* out,
                    const std::vector<std::string>& values) {
  out->insert(out->end(), values.begin(), values.end());
}

BatchPointLookupResult Refuse(const BatchPointLookupPlan& plan,
                              const std::string& diagnostic_code,
                              const std::string& message_key,
                              const std::string& reason) {
  BatchPointLookupResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.purpose = plan.purpose;
  result.input_key_count = static_cast<u64>(plan.keys.size());
  result.diagnostic =
      MakeBatchPointLookupDiagnostic(result.status, diagnostic_code,
                                     message_key, reason);
  result.refusal_reasons.push_back(reason);
  result.evidence =
      BaseEvidence(plan.purpose, result.input_key_count, result.unique_key_count);
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  return result;
}

BatchPointLookupResult RefuseWithDiagnostic(
    const BatchPointLookupPlan& plan,
    const DiagnosticRecord& diagnostic,
    const std::string& reason,
    const std::vector<std::string>& nested_evidence = {}) {
  BatchPointLookupResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.purpose = plan.purpose;
  result.input_key_count = static_cast<u64>(plan.keys.size());
  result.diagnostic = MakeBatchPointLookupDiagnostic(
      result.status,
      diagnostic.diagnostic_code.empty()
          ? "SB_BATCH_POINT_LOOKUP.PROVIDER_REFUSED"
          : diagnostic.diagnostic_code,
      diagnostic.message_key.empty() ? "batch_point_lookup.provider_refused"
                                     : diagnostic.message_key,
      reason);
  result.refusal_reasons.push_back(reason);
  result.evidence =
      BaseEvidence(plan.purpose, result.input_key_count, result.unique_key_count);
  AppendEvidence(&result.evidence, nested_evidence);
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  return result;
}

BatchPointLookupResult ValidateAuthority(
    const BatchPointLookupPlan& plan,
    const CandidateSetAuthorityContext& authority) {
  if (authority.parser_or_donor_finality_or_visibility_authority ||
      authority.client_finality_or_visibility_authority ||
      authority.provider_finality_or_visibility_authority ||
      authority.wal_recovery_or_finality_authority) {
    return Refuse(plan, "SB_BATCH_POINT_LOOKUP.UNSAFE_AUTHORITY",
                  "batch_point_lookup.unsafe_authority",
                  "unsafe_visibility_or_finality_authority");
  }
  if (!authority.engine_mga_authoritative ||
      !authority.row_mga_recheck_required) {
    return Refuse(plan, "SB_BATCH_POINT_LOOKUP.MGA_RECHECK_REQUIRED",
                  "batch_point_lookup.mga_recheck_required",
                  "missing_mga_visibility_recheck");
  }
  if (!authority.security_context_bound ||
      !authority.row_security_recheck_required) {
    return Refuse(plan, "SB_BATCH_POINT_LOOKUP.SECURITY_RECHECK_REQUIRED",
                  "batch_point_lookup.security_recheck_required",
                  "missing_security_authorization_recheck");
  }
  if (!authority.exact_recheck_available) {
    return Refuse(plan, "SB_BATCH_POINT_LOOKUP.EXACT_RECHECK_REQUIRED",
                  "batch_point_lookup.exact_recheck_required",
                  "missing_exact_recheck");
  }
  BatchPointLookupResult ok;
  ok.status = OkStatus();
  ok.purpose = plan.purpose;
  return ok;
}

BatchPointLookupResult ValidatePlan(const BatchPointLookupPlan& plan) {
  if (plan.purpose == BatchPointLookupPurpose::unknown) {
    return Refuse(plan, "SB_BATCH_POINT_LOOKUP.PURPOSE_REQUIRED",
                  "batch_point_lookup.purpose_required",
                  "lookup_purpose_required");
  }
  if (!plan.stable_input_order_required) {
    return Refuse(plan, "SB_BATCH_POINT_LOOKUP.STABLE_ORDER_REQUIRED",
                  "batch_point_lookup.stable_order_required",
                  "stable_input_order_required");
  }
  if (!plan.preserve_duplicate_keys) {
    return Refuse(plan, "SB_BATCH_POINT_LOOKUP.DUPLICATE_POLICY_REQUIRED",
                  "batch_point_lookup.duplicate_policy_required",
                  "duplicate_key_occurrence_preservation_required");
  }
  if (!plan.per_key_miss_diagnostics_required) {
    return Refuse(plan, "SB_BATCH_POINT_LOOKUP.MISS_DIAGNOSTICS_REQUIRED",
                  "batch_point_lookup.miss_diagnostics_required",
                  "per_key_miss_diagnostics_required");
  }
  if (!plan.exact_key_recheck_required ||
      !plan.row_mga_visibility_recheck_required ||
      !plan.row_security_authorization_recheck_required) {
    return Refuse(plan, "SB_BATCH_POINT_LOOKUP.RECHECK_CONTRACT_REQUIRED",
                  "batch_point_lookup.recheck_contract_required",
                  "missing_batch_lookup_recheck_contract");
  }
  if (plan.cluster_route_requested &&
      (!plan.cluster_guard_checked || !plan.cluster_provider_authorized)) {
    return Refuse(plan, "SB_BATCH_POINT_LOOKUP.CLUSTER_GUARD_REQUIRED",
                  "batch_point_lookup.cluster_guard_required",
                  "cluster_route_requires_authorized_provider_guard");
  }
  for (u64 i = 0; i < static_cast<u64>(plan.keys.size()); ++i) {
    if (plan.keys[static_cast<std::size_t>(i)].encoded_key.empty()) {
      return Refuse(plan, "SB_BATCH_POINT_LOOKUP.KEY_REQUIRED",
                    "batch_point_lookup.key_required",
                    "empty_lookup_key");
    }
    if (plan.keys[static_cast<std::size_t>(i)].input_ordinal != i) {
      return Refuse(plan, "SB_BATCH_POINT_LOOKUP.UNSTABLE_INPUT_ORDER",
                    "batch_point_lookup.unstable_input_order",
                    "input_ordinal_must_match_batch_position");
    }
  }
  BatchPointLookupResult ok;
  ok.status = OkStatus();
  ok.purpose = plan.purpose;
  return ok;
}

std::map<std::string, std::vector<u64>> BuildOccurrences(
    const BatchPointLookupPlan& plan) {
  std::map<std::string, std::vector<u64>> occurrences;
  for (u64 i = 0; i < static_cast<u64>(plan.keys.size()); ++i) {
    occurrences[plan.keys[static_cast<std::size_t>(i)].encoded_key].push_back(i);
  }
  return occurrences;
}

std::vector<BatchPointLookupKey> OrderedUniqueKeys(
    const std::map<std::string, std::vector<u64>>& occurrences) {
  std::vector<BatchPointLookupKey> keys;
  keys.reserve(occurrences.size());
  u64 ordinal = 0;
  for (const auto& [key, ignored] : occurrences) {
    (void)ignored;
    keys.push_back({key, ordinal++});
  }
  return keys;
}

u64 DuplicateOccurrenceCount(
    const std::map<std::string, std::vector<u64>>& occurrences) {
  u64 count = 0;
  for (const auto& [key, positions] : occurrences) {
    (void)key;
    if (positions.size() > 1) {
      count += static_cast<u64>(positions.size() - 1);
    }
  }
  return count;
}

BatchPointLookupResult FinishEmpty(const BatchPointLookupPlan& plan) {
  BatchPointLookupResult result;
  result.status = OkStatus();
  result.purpose = plan.purpose;
  result.input_key_count = 0;
  result.unique_key_count = 0;
  result.final_rows_authorized = true;
  result.diagnostic =
      MakeBatchPointLookupDiagnostic(result.status, "SB_BATCH_POINT_LOOKUP.OK",
                                     "batch_point_lookup.ok",
                                     BatchPointLookupPurposeName(plan.purpose));
  result.evidence = BaseEvidence(plan.purpose, 0, 0);
  result.evidence.push_back("batch_point_lookup.provider_batch_executed=false");
  result.evidence.push_back("batch_point_lookup.final_rows_authorized=true");
  return result;
}

BatchPointLookupMiss MakeMiss(const BatchPointLookupPlan& plan,
                              const BatchPointLookupKey& key,
                              std::string reason) {
  BatchPointLookupMiss miss;
  miss.encoded_key = key.encoded_key;
  miss.input_ordinal = key.input_ordinal;
  miss.reason = std::move(reason);
  miss.diagnostic = MakeBatchPointLookupDiagnostic(
      RefusalStatus(), "SB_BATCH_POINT_LOOKUP.KEY_MISS",
      "batch_point_lookup.key_miss",
      "purpose=" + std::string(BatchPointLookupPurposeName(plan.purpose)) +
          ";input_ordinal=" + std::to_string(key.input_ordinal) +
          ";key=" + key.encoded_key + ";reason=" + miss.reason);
  return miss;
}

}  // namespace

const char* BatchPointLookupPurposeName(BatchPointLookupPurpose purpose) {
  switch (purpose) {
    case BatchPointLookupPurpose::key_value:
      return "key_value";
    case BatchPointLookupPurpose::document_payload:
      return "document_payload";
    case BatchPointLookupPurpose::vector_rerank_payload:
      return "vector_rerank_payload";
    case BatchPointLookupPurpose::graph_frontier:
      return "graph_frontier";
    case BatchPointLookupPurpose::search_payload:
      return "search_payload";
    case BatchPointLookupPurpose::foreign_key_check:
      return "foreign_key_check";
    case BatchPointLookupPurpose::time_series_bucket:
      return "time_series_bucket";
    case BatchPointLookupPurpose::unknown:
      break;
  }
  return "unknown";
}

BatchPointLookupResult RunBatchPointLookup(
    const BatchPointLookupPlan& plan,
    const CandidateSetAuthorityContext& authority,
    const BatchPointLookupProvider& provider) {
  const auto authority_check = ValidateAuthority(plan, authority);
  if (!authority_check.ok()) {
    return authority_check;
  }
  const auto plan_check = ValidatePlan(plan);
  if (!plan_check.ok()) {
    return plan_check;
  }
  if (plan.keys.empty()) {
    return FinishEmpty(plan);
  }
  if (!provider) {
    return Refuse(plan, "SB_BATCH_POINT_LOOKUP.PROVIDER_REQUIRED",
                  "batch_point_lookup.provider_required",
                  "batch_point_lookup_provider_required");
  }

  const auto occurrences = BuildOccurrences(plan);
  const auto ordered_unique_keys = OrderedUniqueKeys(occurrences);
  const auto duplicate_occurrences = DuplicateOccurrenceCount(occurrences);

  BatchPointLookupProviderRequest provider_request;
  provider_request.purpose = plan.purpose;
  provider_request.ordered_unique_keys = ordered_unique_keys;
  provider_request.plan_id = plan.plan_id;
  provider_request.caller_evidence = plan.caller_evidence;

  auto provider_result = provider(provider_request);
  if (!provider_result.ok()) {
    return RefuseWithDiagnostic(
        plan,
        provider_result.diagnostic.diagnostic_code.empty()
            ? MakeBatchPointLookupDiagnostic(
                  RefusalStatus(),
                  "SB_BATCH_POINT_LOOKUP.PROVIDER_REFUSED",
                  "batch_point_lookup.provider_refused",
                  "provider_refused_batch_lookup")
            : provider_result.diagnostic,
        "provider_refused_batch_lookup",
        provider_result.evidence);
  }

  std::set<std::string> requested_keys;
  for (const auto& key : ordered_unique_keys) {
    requested_keys.insert(key.encoded_key);
  }

  std::map<std::string, std::vector<BatchPointLookupProviderRow>> rows_by_key;
  std::set<std::string> key_row_pairs;
  std::vector<CandidateSetRow> candidates;
  for (auto& row : provider_result.rows) {
    if (requested_keys.find(row.encoded_key) == requested_keys.end()) {
      return Refuse(plan, "SB_BATCH_POINT_LOOKUP.PROVIDER_ROW_OUT_OF_PLAN",
                    "batch_point_lookup.provider_row_out_of_plan",
                    "provider_returned_unrequested_key");
    }
    if (!row.exact_key_match || !row.exact_row_uuid ||
        !row.ordered_point_lookup) {
      return Refuse(plan, "SB_BATCH_POINT_LOOKUP.PROVIDER_ROW_UNTRUSTED",
                    "batch_point_lookup.provider_row_untrusted",
                    "provider_row_missing_exact_point_lookup_proof");
    }
    if (!ValidRowUuid(row.candidate.row_uuid)) {
      return Refuse(plan, "SB_BATCH_POINT_LOOKUP.EXACT_ROW_UUID_REQUIRED",
                    "batch_point_lookup.exact_row_uuid_required",
                    "provider_row_missing_exact_row_uuid");
    }
    const auto pair_key =
        row.encoded_key + "|" + std::to_string(static_cast<u32>(
                                  row.candidate.row_uuid.kind)) +
        "|";
    std::string row_bytes;
    row_bytes.reserve(row.candidate.row_uuid.value.bytes.size() * 2);
    for (const auto byte : row.candidate.row_uuid.value.bytes) {
      row_bytes.push_back(static_cast<char>('A' + ((byte >> 4) & 0x0f)));
      row_bytes.push_back(static_cast<char>('A' + (byte & 0x0f)));
    }
    if (!key_row_pairs.insert(pair_key + row_bytes).second) {
      return Refuse(plan, "SB_BATCH_POINT_LOOKUP.PROVIDER_DUPLICATE_ROW_UUID",
                    "batch_point_lookup.provider_duplicate_row_uuid",
                    "provider_returned_duplicate_row_uuid_for_key");
    }
    candidates.push_back(row.candidate);
    rows_by_key[row.encoded_key].push_back(std::move(row));
  }

  for (auto& [key, rows] : rows_by_key) {
    (void)key;
    std::sort(rows.begin(), rows.end(), RowUuidLess);
  }

  auto candidate_stream =
      MakeExactRowUuidOrderedCandidateSet(std::move(candidates), authority,
                                          false);
  if (!candidate_stream.ok()) {
    return RefuseWithDiagnostic(plan, candidate_stream.diagnostic,
                                "candidate_stream_refused",
                                candidate_stream.evidence);
  }

  auto exact_recheck =
      ExactRecheckCandidateSet(candidate_stream.output, authority);
  if (!exact_recheck.ok()) {
    return RefuseWithDiagnostic(plan, exact_recheck.diagnostic,
                                "exact_recheck_refused",
                                exact_recheck.evidence);
  }

  std::set<RowKey> authorized_rows;
  for (const auto& row : exact_recheck.output.rows) {
    authorized_rows.insert(RowKey{row.row_uuid});
  }

  BatchPointLookupResult result;
  result.status = OkStatus();
  result.purpose = plan.purpose;
  result.input_key_count = static_cast<u64>(plan.keys.size());
  result.unique_key_count = static_cast<u64>(ordered_unique_keys.size());
  result.duplicate_key_occurrences = duplicate_occurrences;
  result.provider_batch_executed = true;
  result.final_rows_authorized = true;
  result.candidate_stream = std::move(candidate_stream);
  result.exact_recheck = std::move(exact_recheck);

  std::map<std::string, u64> occurrence_ordinals;
  for (const auto& key : plan.keys) {
    const auto duplicate_ordinal = occurrence_ordinals[key.encoded_key]++;
    const auto& positions = occurrences.at(key.encoded_key);
    const bool duplicate_key = positions.size() > 1;
    u64 rows_added = 0;
    const auto found = rows_by_key.find(key.encoded_key);
    if (found != rows_by_key.end()) {
      for (const auto& provider_row : found->second) {
        if (authorized_rows.find(RowKey{provider_row.candidate.row_uuid}) ==
            authorized_rows.end()) {
          continue;
        }
        BatchPointLookupRow row;
        row.encoded_key = key.encoded_key;
        row.input_ordinal = key.input_ordinal;
        row.duplicate_ordinal = duplicate_ordinal;
        row.duplicate_key = duplicate_key;
        row.row_uuid = provider_row.candidate.row_uuid;
        row.score = provider_row.candidate.score;
        row.payload = provider_row.payload;
        row.attributes = provider_row.attributes;
        result.rows.push_back(std::move(row));
        ++rows_added;
      }
    }
    if (rows_added == 0) {
      result.misses.push_back(MakeMiss(
          plan, key,
          found == rows_by_key.end()
              ? "key_not_found"
              : "filtered_by_exact_mga_security_recheck"));
    }
  }

  result.diagnostic =
      MakeBatchPointLookupDiagnostic(result.status, "SB_BATCH_POINT_LOOKUP.OK",
                                     "batch_point_lookup.ok",
                                     BatchPointLookupPurposeName(plan.purpose));
  result.evidence =
      BaseEvidence(plan.purpose, result.input_key_count, result.unique_key_count);
  result.evidence.push_back(
      CountEvidence("batch_point_lookup.duplicate_key_occurrences",
                    result.duplicate_key_occurrences));
  result.evidence.push_back(
      CountEvidence("batch_point_lookup.row_count",
                    static_cast<u64>(result.rows.size())));
  result.evidence.push_back(
      CountEvidence("batch_point_lookup.miss_count",
                    static_cast<u64>(result.misses.size())));
  result.evidence.push_back("batch_point_lookup.provider_batch_executed=true");
  result.evidence.push_back("batch_point_lookup.provider_key_order=encoded_key");
  result.evidence.push_back("batch_point_lookup.final_rows_authorized=true");
  result.evidence.push_back(
      "mga_finality_authority=engine_transaction_inventory");
  result.evidence.push_back("cluster_provider_dispatch=false");
  AppendEvidence(&result.evidence, provider_result.evidence);
  AppendEvidence(&result.evidence, result.candidate_stream.evidence);
  AppendEvidence(&result.evidence, result.exact_recheck.evidence);
  for (const auto& item : plan.caller_evidence) {
    result.evidence.push_back("batch_point_lookup.caller_evidence=" + item);
  }
  for (const auto& miss : result.misses) {
    result.evidence.push_back("batch_point_lookup.miss=" +
                              std::to_string(miss.input_ordinal) + ":" +
                              miss.encoded_key + ":" + miss.reason);
  }
  return result;
}

DiagnosticRecord MakeBatchPointLookupDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::move(diagnostic_code);
  record.message_key = std::move(message_key);
  if (!detail.empty()) {
    record.arguments.push_back(DiagnosticArgument{"detail", std::move(detail)});
  }
  record.source_component = "core.index.batch_point_lookup";
  return record;
}

}  // namespace scratchbird::core::index
