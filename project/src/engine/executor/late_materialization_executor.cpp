// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-EXECUTOR-LATE-MATERIALIZATION-ANCHOR
#include "late_materialization_executor.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::core::index::CandidateSetRow;
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
namespace page = scratchbird::storage::page;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::engine};
}

struct RowKey {
  TypedUuid uuid;

  friend bool operator<(const RowKey& left, const RowKey& right) {
    if (left.uuid.kind != right.uuid.kind) {
      return static_cast<scratchbird::core::platform::u32>(left.uuid.kind) <
             static_cast<scratchbird::core::platform::u32>(right.uuid.kind);
    }
    return left.uuid.value.bytes < right.uuid.value.bytes;
  }
};

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.valid() && right.valid() && left.kind == right.kind &&
         left.value == right.value;
}

bool SameDescriptor(const page::LargePayloadDescriptor& left,
                    const page::LargePayloadDescriptor& right) {
  return SameUuid(left.payload_uuid, right.payload_uuid) &&
         SameUuid(left.owner_object_uuid, right.owner_object_uuid) &&
         SameUuid(left.generation_scope_uuid, right.generation_scope_uuid) &&
         SameUuid(left.filespace_uuid, right.filespace_uuid) &&
         SameUuid(left.overflow_value_uuid, right.overflow_value_uuid) &&
         left.family == right.family &&
         left.generation == right.generation &&
         left.creator_local_transaction_id ==
             right.creator_local_transaction_id &&
         left.retired_by_local_transaction_id ==
             right.retired_by_local_transaction_id &&
         left.byte_count == right.byte_count &&
         left.content_hash == right.content_hash &&
         left.filespace_class == right.filespace_class &&
         left.page_family == right.page_family &&
         left.inline_payload == right.inline_payload &&
         left.inline_text == right.inline_text;
}

bool RowUuidLess(const CandidateSetRow& left, const CandidateSetRow& right) {
  return RowKey{left.row_uuid} < RowKey{right.row_uuid};
}

bool ValidRowUuid(const TypedUuid& value) {
  return value.valid() && value.kind == UuidKind::row;
}

std::string UuidText(const TypedUuid& value) {
  if (!value.valid()) {
    return "invalid";
  }
  return scratchbird::core::uuid::UuidToString(value.value);
}

std::string JoinUuidOrder(const std::vector<TypedUuid>& values) {
  std::ostringstream out;
  bool first = true;
  for (const auto& value : values) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << UuidText(value);
  }
  return out.str();
}

void AppendEvidence(std::vector<std::string>* target,
                    const std::vector<std::string>& source) {
  target->insert(target->end(), source.begin(), source.end());
}

void AppendCounterEvidence(LateMaterializationResult* result) {
  const auto& counters = result->counters;
  result->evidence.push_back("late_materialization.candidate_input_count=" +
                             std::to_string(counters.candidate_input_count));
  result->evidence.push_back(
      "late_materialization.rows_after_exact_mga_security_recheck=" +
      std::to_string(counters.rows_after_exact_mga_security_recheck));
  result->evidence.push_back("late_materialization.top_k_limit=" +
                             std::to_string(counters.top_k_limit));
  result->evidence.push_back("late_materialization.rows_after_top_k_pruning=" +
                             std::to_string(counters.rows_after_top_k_pruning));
  result->evidence.push_back(
      "late_materialization.payload_fetcher_invocation_count=" +
      std::to_string(counters.payload_fetcher_invocation_count));
  result->evidence.push_back("late_materialization.payload_fetch_count=" +
                             std::to_string(counters.payload_fetch_count));
  result->evidence.push_back("late_materialization.redacted_payload_count=" +
                             std::to_string(counters.redacted_payload_count));
  result->evidence.push_back("late_materialization.skipped_row_count=" +
                             std::to_string(counters.skipped_row_count));
  result->evidence.push_back(
      "late_materialization.skipped_by_exact_mga_security_count=" +
      std::to_string(counters.skipped_by_exact_mga_security_count));
  result->evidence.push_back("late_materialization.skipped_by_top_k_count=" +
                             std::to_string(counters.skipped_by_top_k_count));
  result->evidence.push_back("late_materialization.materialization_order=" +
                             JoinUuidOrder(counters.materialization_order));
}

LateMaterializationResult Fail(LateMaterializationResult result,
                               std::string diagnostic_code,
                               std::string message_key,
                               std::string detail) {
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.rows.clear();
  result.evidence.push_back("late_materialization.fail_closed=true");
  result.evidence.push_back("late_materialization.refused=" + diagnostic_code);
  result.diagnostic = MakeLateMaterializationDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  return result;
}

bool HasUnsafeAuthorityDrift(const LateMaterializationPlan& plan) {
  return plan.parser_or_reference_finality_or_visibility_authority ||
         plan.client_finality_or_visibility_authority ||
         plan.provider_finality_or_visibility_authority ||
         plan.wal_recovery_or_finality_authority;
}

LateMaterializationResult ValidatePlan(
    const LateMaterializationPlan& plan,
    const LateMaterializationPayloadFetcher& payload_fetcher) {
  LateMaterializationResult result;
  result.status = OkStatus();
  result.counters.candidate_input_count =
      static_cast<u64>(plan.candidate_intersection.rows.size());
  result.counters.top_k_limit = plan.top_k_limit;

  if (!payload_fetcher) {
    return Fail(std::move(result),
                "SB_LATE_MATERIALIZATION.PAYLOAD_FETCHER_REQUIRED",
                "engine.executor.late_materialization.payload_fetcher_required",
                "payload fetcher is required");
  }
  if (HasUnsafeAuthorityDrift(plan)) {
    return Fail(std::move(result),
                "SB_LATE_MATERIALIZATION.UNSAFE_AUTHORITY",
                "engine.executor.late_materialization.unsafe_authority",
                "late materialization attempted visibility or finality authority");
  }
  if (!plan.candidate_intersection_proven) {
    return Fail(std::move(result),
                "SB_LATE_MATERIALIZATION.CANDIDATE_INTERSECTION_REQUIRED",
                "engine.executor.late_materialization.intersection_required",
                "candidate intersection proof is required");
  }
  if (!plan.exact_predicate_recheck_required ||
      !plan.mga_visibility_recheck_required ||
      !plan.security_authorization_recheck_required) {
    return Fail(std::move(result),
                "SB_LATE_MATERIALIZATION.RECHECK_CONTRACT_REQUIRED",
                "engine.executor.late_materialization.recheck_required",
                "exact, MGA, and security rechecks are required");
  }
  if (!plan.redaction_gate_required) {
    return Fail(std::move(result),
                "SB_LATE_MATERIALIZATION.REDACTION_GATE_REQUIRED",
                "engine.executor.late_materialization.redaction_gate_required",
                "redaction gate is required");
  }
  if (!plan.top_k_pruning_required || plan.top_k_limit == 0) {
    return Fail(std::move(result),
                "SB_LATE_MATERIALIZATION.TOP_K_REQUIRED",
                "engine.executor.late_materialization.top_k_required",
                "top-K pruning limit is required");
  }
  return result;
}

std::vector<CandidateSetRow> TopKAfterRecheck(std::vector<CandidateSetRow> rows,
                                              u64 limit) {
  std::stable_sort(rows.begin(), rows.end(),
                   [](const CandidateSetRow& left,
                      const CandidateSetRow& right) {
                     if (left.score != right.score) {
                       return left.score > right.score;
                     }
                     return RowUuidLess(left, right);
                   });
  if (rows.size() > limit) {
    rows.resize(static_cast<std::size_t>(limit));
  }
  std::sort(rows.begin(), rows.end(), RowUuidLess);
  return rows;
}

using ReferenceMap = std::map<RowKey, page::LatePayloadReference>;

LateMaterializationResult BuildReferenceMap(
    LateMaterializationResult result,
    const LateMaterializationPlan& plan,
    ReferenceMap* references) {
  for (const auto& reference : plan.payload_references) {
    if (!ValidRowUuid(reference.row_uuid)) {
      return Fail(std::move(result),
                  "SB_LATE_MATERIALIZATION.PAYLOAD_REFERENCE_INVALID",
                  "engine.executor.late_materialization.payload_reference_invalid",
                  "payload reference row UUID is invalid");
    }
    const auto inserted = references->emplace(RowKey{reference.row_uuid},
                                             reference);
    if (!inserted.second) {
      return Fail(std::move(result),
                  "SB_LATE_MATERIALIZATION.PAYLOAD_REFERENCE_DUPLICATE",
                  "engine.executor.late_materialization.payload_reference_duplicate",
                  "payload reference row UUID is duplicated");
    }
  }
  return result;
}

}  // namespace

LateMaterializationResult ExecuteLateMaterialization(
    const LateMaterializationPlan& plan,
    const LateMaterializationPayloadFetcher& payload_fetcher) {
  auto result = ValidatePlan(plan, payload_fetcher);
  if (!result.ok()) {
    return result;
  }

  result.evidence.push_back("late_materialization.candidate_intersection.proven=true");
  result.evidence.push_back("late_materialization.fetch_after_candidate_intersection=true");
  result.evidence.push_back("late_materialization.fetch_after_exact_recheck=true");
  result.evidence.push_back("late_materialization.fetch_after_mga_recheck=true");
  result.evidence.push_back("late_materialization.fetch_after_security_redaction_gate=true");
  result.evidence.push_back("late_materialization.fetch_after_top_k_pruning=true");
  result.evidence.push_back("late_materialization.transaction_finality_authority=false");
  result.evidence.push_back("late_materialization.visibility_authority=false");

  result.exact_recheck =
      scratchbird::core::index::ExactRecheckCandidateSet(
          plan.candidate_intersection, plan.authority);
  AppendEvidence(&result.evidence, result.exact_recheck.evidence);
  if (!result.exact_recheck.ok()) {
    result.status = result.exact_recheck.status;
    result.fail_closed = true;
    result.diagnostic = result.exact_recheck.diagnostic;
    result.evidence.push_back("late_materialization.fail_closed=true");
    result.evidence.push_back(
        "late_materialization.refused=exact_recheck_refused");
    return result;
  }

  result.counters.rows_after_exact_mga_security_recheck =
      static_cast<u64>(result.exact_recheck.output.rows.size());
  result.counters.skipped_by_exact_mga_security_count =
      result.counters.candidate_input_count -
      result.counters.rows_after_exact_mga_security_recheck;

  auto pruned_rows = TopKAfterRecheck(result.exact_recheck.output.rows,
                                      plan.top_k_limit);
  result.counters.rows_after_top_k_pruning =
      static_cast<u64>(pruned_rows.size());
  result.counters.skipped_by_top_k_count =
      result.counters.rows_after_exact_mga_security_recheck -
      result.counters.rows_after_top_k_pruning;
  result.counters.skipped_row_count =
      result.counters.candidate_input_count -
      result.counters.rows_after_top_k_pruning;

  ReferenceMap references;
  result = BuildReferenceMap(std::move(result), plan, &references);
  if (!result.ok()) {
    return result;
  }

  for (const auto& row : pruned_rows) {
    const auto found = references.find(RowKey{row.row_uuid});
    if (found == references.end()) {
      return Fail(std::move(result),
                  "SB_LATE_MATERIALIZATION.PAYLOAD_REFERENCE_REQUIRED",
                  "engine.executor.late_materialization.payload_reference_required",
                  "final row is missing payload reference");
    }

    page::LatePayloadFetchRequest fetch_request;
    fetch_request.reference = found->second;
    fetch_request.requester_final_authorized_and_pruned = true;
    fetch_request.allow_full_payload_bytes =
        !found->second.redaction_required;
    fetch_request.reason =
        "late_materialization;diagnostic_only=true;finality_authority=false;visibility_authority=false";

    ++result.counters.payload_fetcher_invocation_count;
    auto fetched = payload_fetcher(fetch_request);
    AppendEvidence(&result.evidence, fetched.evidence);
    if (!fetched.ok()) {
      result.status = fetched.status;
      result.fail_closed = true;
      result.rows.clear();
      result.diagnostic = fetched.diagnostic;
      result.evidence.push_back("late_materialization.fail_closed=true");
      result.evidence.push_back("late_materialization.refused=payload_fetch_refused");
      return result;
    }
    if (!SameUuid(fetched.row_uuid, row.row_uuid)) {
      return Fail(std::move(result),
                  "SB_LATE_MATERIALIZATION.FETCHER_ROW_MISMATCH",
                  "engine.executor.late_materialization.fetcher_row_mismatch",
                  "payload fetcher returned a different row UUID");
    }
    if (!SameDescriptor(fetched.descriptor, found->second.descriptor)) {
      return Fail(std::move(result),
                  "SB_LATE_MATERIALIZATION.FETCHER_DESCRIPTOR_MISMATCH",
                  "engine.executor.late_materialization.fetcher_descriptor_mismatch",
                  "payload fetcher returned a different payload descriptor");
    }
    if (!found->second.redaction_required && fetched.redacted) {
      return Fail(std::move(result),
                  "SB_LATE_MATERIALIZATION.UNEXPECTED_REDACTION",
                  "engine.executor.late_materialization.unexpected_redaction",
                  "unredacted final row was returned as redacted");
    }
    if (found->second.redaction_required &&
        (fetched.fetched || !fetched.payload_bytes.empty())) {
      return Fail(std::move(result),
                  "SB_LATE_MATERIALIZATION.UNREDACTED_PROTECTED_PAYLOAD",
                  "engine.executor.late_materialization.unredacted_protected",
                  "redacted row exposed payload bytes");
    }
    if (!fetched.redacted && !fetched.fetched) {
      return Fail(std::move(result),
                  "SB_LATE_MATERIALIZATION.PAYLOAD_FETCH_REQUIRED",
                  "engine.executor.late_materialization.payload_fetch_required",
                  "unredacted final row did not fetch payload bytes");
    }
    if (fetched.fetched &&
        (fetched.payload_bytes.empty() ||
         static_cast<u64>(fetched.payload_bytes.size()) !=
             fetched.descriptor.byte_count)) {
      return Fail(std::move(result),
                  "SB_LATE_MATERIALIZATION.FETCHER_PAYLOAD_SIZE_MISMATCH",
                  "engine.executor.late_materialization.fetcher_payload_size_mismatch",
                  "payload fetcher returned bytes that do not match descriptor size");
    }

    LateMaterializedRow output;
    output.row_uuid = row.row_uuid;
    output.descriptor = fetched.descriptor;
    output.redacted = fetched.redacted;
    output.payload_bytes_present = fetched.fetched;
    output.payload_bytes = std::move(fetched.payload_bytes);
    output.redaction_reason = found->second.redaction_reason;
    if (output.redacted) {
      ++result.counters.redacted_payload_count;
    }
    if (output.payload_bytes_present) {
      ++result.counters.payload_fetch_count;
    }
    result.counters.materialization_order.push_back(row.row_uuid);
    result.rows.push_back(std::move(output));
  }

  result.status = OkStatus();
  result.fail_closed = false;
  result.evidence.push_back(
      "late_materialization.fetcher.rows_final_authorized_pruned_only=true");
  result.evidence.push_back("late_materialization.redaction_gate.passed=true");
  result.evidence.push_back("mga_finality_authority=engine_transaction_inventory");
  AppendCounterEvidence(&result);
  result.diagnostic = MakeLateMaterializationDiagnostic(
      result.status, "ok", "engine.executor.late_materialization.materialized",
      "payloads materialized after candidate, recheck, redaction, and top-K gates");
  return result;
}

DiagnosticRecord MakeLateMaterializationDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "engine.executor.late_materialization",
                        status.ok() ? "" : "fail closed before exposing payload bytes");
}

}  // namespace scratchbird::engine::executor
