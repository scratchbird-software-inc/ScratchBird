// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "temp_spill_executor.hpp"

#include "temporary_work_index_runtime.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

namespace idx = scratchbird::core::index;

void AddEvidence(TempSpillResult* result, std::string evidence) {
  result->evidence.push_back(std::move(evidence));
}

std::string Bool(bool value) { return value ? "true" : "false"; }

std::string StableHash(const std::vector<std::string>& rows) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& row : rows) {
    for (const unsigned char ch : row) {
      hash ^= ch;
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << "fnv1a64:" << std::hex << hash;
  return out.str();
}

TempSpillResult Refuse(const TempSpillRequest& request,
                       std::string diagnostic,
                       std::string fallback_reason) {
  TempSpillResult result;
  result.ok = false;
  result.benchmark_clean = false;
  result.fallback_used = request.exact_fallback_available;
  result.fail_closed = !request.exact_fallback_available;
  result.diagnostic_code = std::move(diagnostic);
  result.fallback_reason = std::move(fallback_reason);
  AddEvidence(&result, "orh283.route_label=" + request.route_label);
  AddEvidence(&result, "orh283.route_kind=" +
                           std::string(TempSpillRouteKindName(request.route_kind)));
  AddEvidence(&result, "orh283.refused=" + result.diagnostic_code);
  AddEvidence(&result, "orh283.exact_fallback_used=" + Bool(result.fallback_used));
  AddEvidence(&result, "orh283.fallback_reason=" + result.fallback_reason);
  AddEvidence(&result, "orh283.benchmark_clean=false");
  AddEvidence(&result, "orh283.temp_metadata.row_identity_authority=false");
  AddEvidence(&result, "orh283.temp_metadata.visibility_authority=false");
  AddEvidence(&result, "orh283.temp_metadata.security_authority=false");
  AddEvidence(&result, "orh283.temp_metadata.finality_authority=false");
  AddEvidence(&result, "orh283.temp_metadata.recovery_authority=false");
  AddEvidence(&result,
              "orh283.mga_finality_authority=engine_transaction_inventory");
  return result;
}

idx::TemporaryWorkAuthorityProof Proof(const TempSpillRequest& request,
                                       bool recovery_proof) {
  idx::TemporaryWorkAuthorityProof proof;
  proof.proof_supplied = recovery_proof;
  proof.exact_recheck_required = true;
  proof.exact_recheck_available =
      recovery_proof && request.authority.exact_recheck_required;
  proof.mga_visibility_recheck_required = true;
  proof.mga_visibility_recheck_available =
      recovery_proof && request.authority.engine_mga_snapshot_bound &&
      request.authority.transaction_inventory_authoritative;
  proof.security_recheck_required = true;
  proof.security_context_bound =
      recovery_proof && request.authority.security_recheck_required &&
      request.authority.security_context_bound;
  proof.parser_finality_authority_claimed =
      request.authority.parser_client_or_reference_spill_authority;
  proof.reference_finality_authority_claimed =
      request.authority.parser_client_or_reference_spill_authority;
  proof.transaction_finality_authority_claimed =
      request.authority.temp_metadata_visibility_or_finality_authority;
  proof.visibility_authority_claimed =
      request.authority.temp_metadata_visibility_or_finality_authority;
  proof.recovery_finality_authority_claimed =
      request.authority.temp_metadata_recovery_authority;
  proof.security_authority_claimed =
      request.authority.temp_metadata_visibility_or_finality_authority;
  proof.evidence_ref = "orh283.executor_mga_security_exact_recheck";
  return proof;
}

idx::TemporaryWorkRuntimeState Runtime(const TempSpillRequest& request,
                                      std::uint64_t generation) {
  idx::TemporaryWorkRuntimeOptions options;
  options.spill_directory = request.spill_directory;
  options.runtime_generation = generation;
  options.memory_quota_bytes = request.memory_quota_bytes;
  options.artifact_prefix = "orh283_temp_spill";
  return idx::CreateTemporaryWorkRuntime(std::move(options));
}

std::vector<idx::TemporaryWorkRecord> SortRecords(
    const std::vector<TempSpillInputRow>& rows) {
  std::vector<idx::TemporaryWorkRecord> records;
  records.reserve(rows.size());
  for (const auto& row : rows) {
    records.push_back({row.key, std::to_string(row.value), row.row_ordinal});
  }
  return records;
}

std::vector<idx::TemporaryHashBuildRow> HashRows(
    const std::vector<TempSpillInputRow>& rows) {
  std::vector<idx::TemporaryHashBuildRow> records;
  records.reserve(rows.size());
  for (const auto& row : rows) {
    records.push_back({row.key, std::to_string(row.value), row.row_ordinal});
  }
  return records;
}

std::vector<std::string> SortedOutput(
    const std::vector<idx::TemporaryWorkRecord>& rows) {
  std::vector<std::string> output;
  output.reserve(rows.size());
  for (const auto& row : rows) {
    output.push_back(row.key + ":" + row.payload + ":" +
                     std::to_string(row.row_ordinal));
  }
  return output;
}

std::vector<std::string> TopNOutput(
    const std::vector<idx::TemporaryWorkRecord>& rows,
    std::size_t top_n) {
  auto output = SortedOutput(rows);
  if (top_n < output.size()) {
    output.resize(top_n);
  }
  return output;
}

std::vector<std::string> DistinctOutput(
    const std::vector<idx::TemporaryWorkRecord>& rows) {
  std::vector<std::string> output;
  std::string last_key;
  bool have_last = false;
  for (const auto& row : rows) {
    if (!have_last || row.key != last_key) {
      output.push_back(row.key);
      last_key = row.key;
      have_last = true;
    }
  }
  return output;
}

std::vector<std::string> GroupByOutput(
    const std::vector<idx::TemporaryWorkRecord>& rows) {
  std::vector<std::string> output;
  std::string current_key;
  std::int64_t sum = 0;
  bool have_group = false;
  const auto flush = [&]() {
    if (have_group) {
      output.push_back(current_key + "=" + std::to_string(sum));
    }
  };
  for (const auto& row : rows) {
    if (!have_group || row.key != current_key) {
      flush();
      current_key = row.key;
      sum = 0;
      have_group = true;
    }
    sum += std::stoll(row.payload);
  }
  flush();
  return output;
}

std::vector<std::string> HashAggregateOutput(
    std::vector<idx::TemporaryHashBuildRow> rows) {
  std::stable_sort(rows.begin(), rows.end(), [](const auto& left,
                                                const auto& right) {
    return std::tie(left.key, left.row_ordinal, left.payload) <
           std::tie(right.key, right.row_ordinal, right.payload);
  });
  std::vector<idx::TemporaryWorkRecord> sort_rows;
  sort_rows.reserve(rows.size());
  for (const auto& row : rows) {
    sort_rows.push_back({row.key, row.payload, row.row_ordinal});
  }
  return GroupByOutput(sort_rows);
}

std::vector<std::string> BaselineOutput(const TempSpillRequest& request) {
  auto records = SortRecords(request.rows);
  std::stable_sort(records.begin(), records.end(), [](const auto& left,
                                                      const auto& right) {
    return std::tie(left.key, left.row_ordinal, left.payload) <
           std::tie(right.key, right.row_ordinal, right.payload);
  });
  switch (request.route_kind) {
    case TempSpillRouteKind::kSort:
      return SortedOutput(records);
    case TempSpillRouteKind::kTopN:
      return TopNOutput(records, request.top_n);
    case TempSpillRouteKind::kDistinct:
      return DistinctOutput(records);
    case TempSpillRouteKind::kGroupBy:
      return GroupByOutput(records);
    case TempSpillRouteKind::kHashAggregate:
      return HashAggregateOutput(HashRows(request.rows));
  }
  return {};
}

void AppendTemporaryEvidence(TempSpillResult* result,
                             const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    result->evidence.push_back(item);
  }
}

}  // namespace

const char* TempSpillRouteKindName(TempSpillRouteKind kind) {
  switch (kind) {
    case TempSpillRouteKind::kSort:
      return "sort";
    case TempSpillRouteKind::kTopN:
      return "top_n";
    case TempSpillRouteKind::kDistinct:
      return "distinct";
    case TempSpillRouteKind::kGroupBy:
      return "group_by";
    case TempSpillRouteKind::kHashAggregate:
      return "hash_aggregate";
  }
  return "unknown";
}

TempSpillResult ExecuteBoundedTempSpillRoute(const TempSpillRequest& request) {
  if (request.route_label.empty()) {
    return Refuse(request, "ORH_SORT_SPILL_MISSING_ROUTE_LABEL",
                  "route_label_required");
  }
  if (!request.runtime_enabled) {
    return Refuse(request, "ORH_SORT_SPILL_NO_RUNTIME",
                  "runtime_consumption_missing");
  }
  if (request.rows.empty()) {
    return Refuse(request, "ORH_SORT_SPILL_EMPTY_INPUT",
                  "empty_input_not_benchmark_clean");
  }
  if (!request.exact_fallback_available) {
    return Refuse(request, "ORH_SORT_SPILL_EXACT_FALLBACK_UNAVAILABLE",
                  "exact_fallback_required");
  }
  if (request.benchmark_or_reference_dominance_claim) {
    return Refuse(request, "ORH_SORT_SPILL_DOMINANCE_OVERCLAIM",
                  "temp_spill_gate_is_not_reference_dominance");
  }
  if (request.authority.parser_client_or_reference_spill_authority ||
      request.authority.temp_metadata_visibility_or_finality_authority ||
      request.authority.temp_metadata_recovery_authority) {
    return Refuse(request, "ORH_SORT_SPILL_UNSAFE_AUTHORITY",
                  "temp_spill_metadata_is_advisory_only");
  }
  if (!request.authority.engine_mga_snapshot_bound ||
      !request.authority.transaction_inventory_authoritative) {
    return Refuse(request, "ORH_SORT_SPILL_MGA_UNPROVEN",
                  "engine_mga_transaction_inventory_required");
  }
  if (!request.authority.security_recheck_required ||
      !request.authority.security_context_bound ||
      !request.authority.exact_recheck_required) {
    return Refuse(request, "ORH_SORT_SPILL_SECURITY_UNPROVEN",
                  "security_and_exact_recheck_required");
  }
  if (!request.temp_accounting_available) {
    return Refuse(request, "ORH_SORT_SPILL_TEMP_ACCOUNTING_MISSING",
                  "temp_accounting_required");
  }

  auto runtime = Runtime(request, request.runtime_generation);
  const auto proof = Proof(request, true);
  idx::TemporaryWorkResult built;
  if (request.route_kind == TempSpillRouteKind::kHashAggregate) {
    built = idx::BuildTemporaryHashJoinTable(&runtime, HashRows(request.rows),
                                             proof, request.spill_allowed);
  } else {
    built = idx::BuildTemporarySortRun(&runtime, SortRecords(request.rows),
                                       proof, request.spill_allowed);
  }

  if (!built.ok()) {
    if (built.open_class == idx::TemporaryWorkOpenClass::memory_grant_denied) {
      return Refuse(request, "ORH_SORT_SPILL_MEMORY_PRESSURE_FALLBACK",
                    "temporary_memory_grant_denied");
    }
    return Refuse(request, "ORH_SORT_SPILL_RUNTIME_REFUSED",
                  built.diagnostic.diagnostic_code);
  }

  if (request.cancellation_requested) {
    if (!request.cleanup_after_cancellation) {
      auto cleanup =
          idx::CleanupTemporaryWorkArtifact(&runtime, built.descriptor.artifact_id);
      TempSpillResult refused =
          Refuse(request, "ORH_SORT_SPILL_CANCEL_CLEANUP_MISSING",
                 "cancellation_cleanup_proof_missing");
      refused.cleanup_proven = cleanup.ok();
      AddEvidence(&refused, "orh283.cancel_requested=true");
      AddEvidence(&refused, "orh283.cancel_cleanup_proven=false");
      return refused;
    }
    auto cancelled = idx::CancelTemporaryWorkRuntime(&runtime);
    TempSpillResult refused =
        Refuse(request, "ORH_SORT_SPILL_CANCELLED",
               "cancellation_cleanup_completed");
    refused.cleanup_proven = cancelled.ok() && cancelled.cleaned;
    AppendTemporaryEvidence(&refused, cancelled.evidence);
    AddEvidence(&refused, "orh283.cancel_requested=true");
    AddEvidence(&refused,
                "orh283.cancel_cleanup_proven=" + Bool(refused.cleanup_proven));
    return refused;
  }

  if (!built.descriptor.spilled) {
    auto cleanup =
        idx::CleanupTemporaryWorkArtifact(&runtime, built.descriptor.artifact_id);
    TempSpillResult refused =
        Refuse(request, "ORH_SORT_SPILL_NOT_SPILLED",
               "bounded_external_spill_required");
    refused.cleanup_proven = cleanup.ok();
    return refused;
  }

  const auto reopen_generation = request.reopen_runtime_generation == 0
                                     ? request.runtime_generation
                                     : request.reopen_runtime_generation;
  auto reopened_runtime = Runtime(request, reopen_generation);
  auto reopened = idx::OpenTemporaryWorkArtifact(
      &reopened_runtime, built.descriptor, built.descriptor.family,
      Proof(request, request.restart_recovery_proof_available));
  if (!reopened.ok()) {
    auto cleanup =
        idx::CleanupTemporaryWorkArtifact(&runtime, built.descriptor.artifact_id);
    TempSpillResult refused =
        Refuse(request,
               reopened.open_class ==
                       idx::TemporaryWorkOpenClass::stale_runtime_generation
                   ? "ORH_SORT_SPILL_STALE_GENERATION"
                   : "ORH_SORT_SPILL_RECOVERY_PROOF_MISSING",
               reopened.diagnostic.diagnostic_code);
    refused.cleanup_proven = cleanup.ok();
    AppendTemporaryEvidence(&refused, reopened.evidence);
    return refused;
  }

  std::vector<std::string> output;
  if (request.route_kind == TempSpillRouteKind::kHashAggregate) {
    output = HashAggregateOutput(reopened.hash_build_rows);
  } else if (request.route_kind == TempSpillRouteKind::kTopN) {
    output = TopNOutput(reopened.sorted_rows, request.top_n);
  } else if (request.route_kind == TempSpillRouteKind::kDistinct) {
    output = DistinctOutput(reopened.sorted_rows);
  } else if (request.route_kind == TempSpillRouteKind::kGroupBy) {
    output = GroupByOutput(reopened.sorted_rows);
  } else {
    output = SortedOutput(reopened.sorted_rows);
  }
  const auto baseline = BaselineOutput(request);
  const auto result_hash = StableHash(output);
  const auto baseline_hash = StableHash(baseline);
  if (result_hash != baseline_hash ||
      (!request.expected_result_hash.empty() &&
       result_hash != request.expected_result_hash)) {
    auto cleanup =
        idx::CleanupTemporaryWorkArtifact(&runtime, built.descriptor.artifact_id);
    TempSpillResult refused =
        Refuse(request, "ORH_SORT_SPILL_RESULT_MISMATCH",
               "spilled_result_hash_mismatch");
    refused.cleanup_proven = cleanup.ok();
    refused.result_hash = result_hash;
    refused.output_rows = std::move(output);
    return refused;
  }

  auto cleanup =
      request.cleanup_required
          ? idx::CleanupTemporaryWorkArtifact(&runtime,
                                              built.descriptor.artifact_id)
          : idx::TemporaryWorkCleanupResult{};
  if (request.cleanup_required && !cleanup.ok()) {
    return Refuse(request, "ORH_SORT_SPILL_CLEANUP_MISSING",
                  "temporary_cleanup_failed");
  }

  TempSpillResult result;
  result.ok = true;
  result.benchmark_clean = true;
  result.runtime_consumed = true;
  result.spilled = built.descriptor.spilled;
  result.cleanup_proven = !request.cleanup_required || cleanup.ok();
  result.reopen_recovery_proven = reopened.ok();
  result.diagnostic_code = "ORH_SORT_TOPN_DISTINCT_TEMP_SPILL.OK";
  result.fallback_reason = "none";
  result.result_hash = result_hash;
  result.output_rows = std::move(output);
  AddEvidence(&result, "orh283.route_label=" + request.route_label);
  AddEvidence(&result, "orh283.route_kind=" +
                           std::string(TempSpillRouteKindName(request.route_kind)));
  AddEvidence(&result, "orh283.runtime_consumed=true");
  AddEvidence(&result, "orh283.spilled=true");
  AddEvidence(&result, "orh283.result_hash=" + result.result_hash);
  AddEvidence(&result, "orh283.result_equivalence=true");
  AddEvidence(&result,
              "orh283.memory_grant_bytes=" +
                  std::to_string(built.descriptor.memory_grant_bytes));
  AddEvidence(&result,
              "orh283.temp_live_granted_bytes_after_cleanup=" +
                  std::to_string(runtime.live_granted_bytes));
  AddEvidence(&result,
              "orh283.spill_generation=" +
                  std::to_string(built.descriptor.runtime_generation));
  AddEvidence(&result, "orh283.restart_reopen_recovery_proven=true");
  AddEvidence(&result, "orh283.cleanup_proven=true");
  AddEvidence(&result, "orh283.exact_fallback_available=true");
  AddEvidence(&result, "orh283.temp_metadata.row_identity_authority=false");
  AddEvidence(&result, "orh283.temp_metadata.visibility_authority=false");
  AddEvidence(&result, "orh283.temp_metadata.security_authority=false");
  AddEvidence(&result, "orh283.temp_metadata.finality_authority=false");
  AddEvidence(&result, "orh283.temp_metadata.recovery_authority=false");
  AddEvidence(&result,
              "orh283.mga_finality_authority=engine_transaction_inventory");
  AddEvidence(&result, "orh283.security_recheck_required=true");
  AddEvidence(&result, "orh283.exact_recheck_required=true");
  AddEvidence(&result, "orh283.benchmark_clean=true");
  AppendTemporaryEvidence(&result, built.evidence);
  AppendTemporaryEvidence(&result, reopened.evidence);
  if (request.cleanup_required) {
    AppendTemporaryEvidence(&result, cleanup.evidence);
  }
  return result;
}

}  // namespace scratchbird::engine::executor
