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
#include <array>
#include <charconv>
#include <limits>
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

std::string FormatStableHash(const std::uint64_t hash) {
  std::array<char, 32> rendered{};
  constexpr std::string_view prefix = "fnv1a64:";
  std::copy(prefix.begin(), prefix.end(), rendered.begin());
  const auto converted = std::to_chars(
      rendered.data() + static_cast<std::ptrdiff_t>(prefix.size()),
      rendered.data() + rendered.size(), hash, 16);
  if (converted.ec != std::errc{}) return {};
  return std::string(rendered.data(), converted.ptr);
}

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
  return FormatStableHash(hash);
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
  if (request.maximum_live_memory_bytes != 0 &&
      request.retained_memory_bytes <= request.maximum_live_memory_bytes) {
    options.maximum_executor_buffer_bytes =
        request.maximum_live_memory_bytes - request.retained_memory_bytes;
  }
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

bool AddLiveBytes(std::size_t* total, const std::size_t amount) {
  if (total == nullptr ||
      *total > std::numeric_limits<std::size_t>::max() - amount) {
    return false;
  }
  *total += amount;
  return true;
}

std::size_t SignedDecimalBytes(const std::int64_t value) {
  std::array<char, 32> storage{};
  const auto rendered =
      std::to_chars(storage.data(), storage.data() + storage.size(), value);
  return rendered.ec == std::errc{}
             ? static_cast<std::size_t>(rendered.ptr - storage.data())
             : std::numeric_limits<std::size_t>::max();
}

std::size_t UnsignedDecimalBytes(const std::uint64_t value) {
  std::array<char, 32> storage{};
  const auto rendered =
      std::to_chars(storage.data(), storage.data() + storage.size(), value);
  return rendered.ec == std::errc{}
             ? static_cast<std::size_t>(rendered.ptr - storage.data())
             : std::numeric_limits<std::size_t>::max();
}

bool TempInputLogicalBytes(const std::vector<TempSpillInputRow>& rows,
                           std::size_t* bytes) {
  if (bytes == nullptr ||
      rows.size() > std::numeric_limits<std::size_t>::max() /
                        sizeof(TempSpillInputRow)) {
    return false;
  }
  *bytes = rows.size() * sizeof(TempSpillInputRow);
  for (const auto& row : rows) {
    if (!AddLiveBytes(bytes, row.key.size())) return false;
  }
  return true;
}

bool ConvertedRowLogicalBytes(const std::vector<TempSpillInputRow>& rows,
                              const bool hash_rows,
                              std::size_t* bytes) {
  const auto row_bytes = hash_rows ? sizeof(idx::TemporaryHashBuildRow)
                                   : sizeof(idx::TemporaryWorkRecord);
  if (bytes == nullptr ||
      rows.size() > std::numeric_limits<std::size_t>::max() / row_bytes) {
    return false;
  }
  *bytes = rows.size() * row_bytes;
  for (const auto& row : rows) {
    const auto payload_bytes = SignedDecimalBytes(row.value);
    if (payload_bytes == std::numeric_limits<std::size_t>::max() ||
        !AddLiveBytes(bytes, row.key.size()) ||
        !AddLiveBytes(bytes, payload_bytes)) {
      return false;
    }
  }
  return true;
}

bool SortedOutputLogicalBytes(
    const std::vector<idx::TemporaryWorkRecord>& rows,
    const std::size_t limit,
    std::size_t* bytes) {
  const auto count = std::min(limit, rows.size());
  if (bytes == nullptr ||
      count > std::numeric_limits<std::size_t>::max() /
                  sizeof(std::string)) {
    return false;
  }
  *bytes = count * sizeof(std::string);
  for (std::size_t index = 0; index < count; ++index) {
    const auto ordinal_bytes = UnsignedDecimalBytes(rows[index].row_ordinal);
    if (ordinal_bytes == std::numeric_limits<std::size_t>::max() ||
        !AddLiveBytes(bytes, rows[index].key.size()) ||
        !AddLiveBytes(bytes, 1) ||
        !AddLiveBytes(bytes, rows[index].payload.size()) ||
        !AddLiveBytes(bytes, 1) ||
        !AddLiveBytes(bytes, ordinal_bytes)) {
      return false;
    }
  }
  return true;
}


template <typename Row>
bool RuntimeRowsLogicalBytes(const std::vector<Row>& rows,
                             std::size_t* bytes) {
  if (bytes == nullptr ||
      rows.size() > std::numeric_limits<std::size_t>::max() / sizeof(Row)) {
    return false;
  }
  *bytes = rows.size() * sizeof(Row);
  for (const auto& row : rows) {
    if (!AddLiveBytes(bytes, row.key.size()) ||
        !AddLiveBytes(bytes, row.payload.size())) {
      return false;
    }
  }
  return true;
}

bool AdmitRoutePeak(const TempSpillRequest& request,
                    const std::size_t phase_bytes,
                    std::size_t* peak_live_bytes) {
  if (peak_live_bytes == nullptr ||
      request.retained_memory_bytes >
          std::numeric_limits<std::size_t>::max() - phase_bytes) {
    return false;
  }
  const auto live = request.retained_memory_bytes + phase_bytes;
  if (request.maximum_live_memory_bytes != 0 &&
      live > request.maximum_live_memory_bytes) {
    return false;
  }
  *peak_live_bytes = std::max(*peak_live_bytes, live);
  return true;
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
    const std::vector<idx::TemporaryHashBuildRow>& rows) {
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
    case TempSpillRouteKind::kHashAggregate: {
      auto hash_rows = HashRows(request.rows);
      std::sort(hash_rows.begin(), hash_rows.end(), [](const auto& left,
                                                       const auto& right) {
        return std::tie(left.key, left.row_ordinal, left.payload) <
               std::tie(right.key, right.row_ordinal, right.payload);
      });
      return HashAggregateOutput(hash_rows);
    }
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

std::string ComputeOrderedTempSpillSortResultHash(
    const std::vector<TempSpillInputRow>& rows) {
  std::uint64_t hash = 1469598103934665603ull;
  const auto add = [&](const std::string_view bytes) {
    for (const unsigned char byte : bytes) {
      hash ^= byte;
      hash *= 1099511628211ull;
    }
  };
  for (std::size_t index = 0; index < rows.size(); ++index) {
    if (index != 0 && rows[index].key <= rows[index - 1].key) return {};
    std::array<char, 32> value_text{};
    std::array<char, 32> ordinal_text{};
    const auto value = std::to_chars(
        value_text.data(), value_text.data() + value_text.size(),
        rows[index].value);
    const auto ordinal = std::to_chars(
        ordinal_text.data(), ordinal_text.data() + ordinal_text.size(),
        rows[index].row_ordinal);
    if (value.ec != std::errc{} || ordinal.ec != std::errc{}) return {};
    add(rows[index].key);
    add(":");
    add(std::string_view(
        value_text.data(),
        static_cast<std::size_t>(value.ptr - value_text.data())));
    add(":");
    add(std::string_view(
        ordinal_text.data(),
        static_cast<std::size_t>(ordinal.ptr - ordinal_text.data())));
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  return FormatStableHash(hash);
}

TempSpillResult ExecuteBoundedTempSpillRouteOwned(TempSpillRequest request) {
  std::size_t peak_live_memory_bytes = request.retained_memory_bytes;
  const auto refuse = [&](std::string diagnostic,
                          std::string reason) {
    auto refused =
        Refuse(request, std::move(diagnostic), std::move(reason));
    refused.peak_live_memory_bytes = peak_live_memory_bytes;
    return refused;
  };
  if (request.maximum_live_memory_bytes != 0 &&
      (request.retained_memory_bytes > request.maximum_live_memory_bytes ||
       request.expected_result_hash.empty())) {
    return refuse("ORH_SORT_SPILL_LIVE_MEMORY_CONTRACT_INVALID",
                  "bounded_route_requires_retained_ceiling_and_expected_hash");
  }
  if (request.route_label.empty()) {
    return refuse("ORH_SORT_SPILL_MISSING_ROUTE_LABEL",
                  "route_label_required");
  }
  if (!request.runtime_enabled) {
    return refuse("ORH_SORT_SPILL_NO_RUNTIME",
                  "runtime_consumption_missing");
  }
  if (request.rows.empty()) {
    return refuse("ORH_SORT_SPILL_EMPTY_INPUT",
                  "empty_input_not_benchmark_clean");
  }
  if (!request.exact_fallback_available) {
    return refuse("ORH_SORT_SPILL_EXACT_FALLBACK_UNAVAILABLE",
                  "exact_fallback_required");
  }
  if (request.benchmark_or_reference_dominance_claim) {
    return refuse("ORH_SORT_SPILL_DOMINANCE_OVERCLAIM",
                  "temp_spill_gate_is_not_reference_dominance");
  }
  if (request.authority.parser_client_or_reference_spill_authority ||
      request.authority.temp_metadata_visibility_or_finality_authority ||
      request.authority.temp_metadata_recovery_authority) {
    return refuse("ORH_SORT_SPILL_UNSAFE_AUTHORITY",
                  "temp_spill_metadata_is_advisory_only");
  }
  if (!request.authority.engine_mga_snapshot_bound ||
      !request.authority.transaction_inventory_authoritative) {
    return refuse("ORH_SORT_SPILL_MGA_UNPROVEN",
                  "engine_mga_transaction_inventory_required");
  }
  if (!request.authority.security_recheck_required ||
      !request.authority.security_context_bound ||
      !request.authority.exact_recheck_required) {
    return refuse("ORH_SORT_SPILL_SECURITY_UNPROVEN",
                  "security_and_exact_recheck_required");
  }
  if (!request.temp_accounting_available) {
    return refuse("ORH_SORT_SPILL_TEMP_ACCOUNTING_MISSING",
                  "temp_accounting_required");
  }

  std::size_t input_row_bytes = 0;
  std::size_t converted_row_bytes = 0;
  if (!TempInputLogicalBytes(request.rows, &input_row_bytes) ||
      !ConvertedRowLogicalBytes(
          request.rows,
          request.route_kind == TempSpillRouteKind::kHashAggregate,
          &converted_row_bytes) ||
      input_row_bytes >
          std::numeric_limits<std::size_t>::max() - converted_row_bytes ||
      !AdmitRoutePeak(request, input_row_bytes + converted_row_bytes,
                      &peak_live_memory_bytes)) {
    return refuse("ORH_SORT_SPILL_LIVE_MEMORY_EXHAUSTED",
                  "input_conversion_peak_exceeds_bound");
  }

  auto runtime = Runtime(request, request.runtime_generation);
  const auto proof = Proof(request, true);
  idx::TemporaryWorkResult built;
  if (request.route_kind == TempSpillRouteKind::kHashAggregate) {
    auto converted = HashRows(request.rows);
    if (request.maximum_live_memory_bytes != 0) {
      decltype(request.rows)().swap(request.rows);
    }
    built = idx::BuildTemporaryHashJoinTable(
        &runtime, std::move(converted), proof, request.spill_allowed);
  } else {
    auto converted = SortRecords(request.rows);
    if (request.maximum_live_memory_bytes != 0) {
      decltype(request.rows)().swap(request.rows);
    }
    built = idx::BuildTemporarySortRun(
        &runtime, std::move(converted), proof, request.spill_allowed);
  }
  if (built.peak_executor_buffer_bytes >
          std::numeric_limits<std::size_t>::max() ||
      !AdmitRoutePeak(
          request, static_cast<std::size_t>(
                       built.peak_executor_buffer_bytes),
          &peak_live_memory_bytes)) {
    return refuse("ORH_SORT_SPILL_LIVE_MEMORY_EXHAUSTED",
                  "build_peak_exceeds_bound");
  }

  if (!built.ok()) {
    if (built.open_class == idx::TemporaryWorkOpenClass::memory_grant_denied) {
      return refuse("ORH_SORT_SPILL_MEMORY_PRESSURE_FALLBACK",
                    "temporary_memory_grant_denied");
    }
    return refuse("ORH_SORT_SPILL_RUNTIME_REFUSED",
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

  decltype(built.sorted_rows)().swap(built.sorted_rows);
  decltype(built.hash_build_rows)().swap(built.hash_build_rows);

  const auto reopen_generation = request.reopen_runtime_generation == 0
                                     ? request.runtime_generation
                                     : request.reopen_runtime_generation;
  auto reopened_runtime = Runtime(request, reopen_generation);
  auto reopened = idx::OpenTemporaryWorkArtifact(
      &reopened_runtime, built.descriptor, built.descriptor.family,
      Proof(request, request.restart_recovery_proof_available));
  if (reopened.peak_executor_buffer_bytes >
          std::numeric_limits<std::size_t>::max() ||
      !AdmitRoutePeak(
          request, static_cast<std::size_t>(
                       reopened.peak_executor_buffer_bytes),
          &peak_live_memory_bytes)) {
    auto cleanup =
        idx::CleanupTemporaryWorkArtifact(&runtime,
                                          built.descriptor.artifact_id);
    (void)cleanup;
    return refuse("ORH_SORT_SPILL_LIVE_MEMORY_EXHAUSTED",
                  "reopen_peak_exceeds_bound");
  }
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
  if (request.maximum_live_memory_bytes != 0) {
    if (request.route_kind != TempSpillRouteKind::kSort) {
      auto cleanup = idx::CleanupTemporaryWorkArtifact(
          &runtime, built.descriptor.artifact_id);
      (void)cleanup;
      return refuse("ORH_SORT_SPILL_LIVE_MEMORY_CONTRACT_INVALID",
                    "bounded_output_planner_requires_sort_route");
    }
    std::size_t reopened_row_bytes = 0;
    std::size_t output_row_bytes = 0;
    if (!RuntimeRowsLogicalBytes(reopened.sorted_rows,
                                 &reopened_row_bytes) ||
        !SortedOutputLogicalBytes(reopened.sorted_rows,
                                  reopened.sorted_rows.size(),
                                  &output_row_bytes) ||
        reopened_row_bytes >
            std::numeric_limits<std::size_t>::max() - output_row_bytes ||
        !AdmitRoutePeak(request,
                        reopened_row_bytes + output_row_bytes,
                        &peak_live_memory_bytes)) {
      auto cleanup = idx::CleanupTemporaryWorkArtifact(
          &runtime, built.descriptor.artifact_id);
      (void)cleanup;
      return refuse("ORH_SORT_SPILL_LIVE_MEMORY_EXHAUSTED",
                    "output_peak_exceeds_bound");
    }
  }
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
  const auto result_hash = StableHash(output);
  const auto baseline_hash = request.expected_result_hash.empty()
                                 ? StableHash(BaselineOutput(request))
                                 : request.expected_result_hash;
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
  result.peak_live_memory_bytes = peak_live_memory_bytes;
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
  AddEvidence(&result,
              "orh283.peak_live_memory_bytes=" +
                  std::to_string(result.peak_live_memory_bytes));
  AppendTemporaryEvidence(&result, built.evidence);
  AppendTemporaryEvidence(&result, reopened.evidence);
  if (request.cleanup_required) {
    AppendTemporaryEvidence(&result, cleanup.evidence);
  }
  return result;
}

TempSpillResult ExecuteBoundedTempSpillRoute(
    const TempSpillRequest& request) {
  if (request.maximum_live_memory_bytes != 0) {
    return Refuse(request, "ORH_SORT_SPILL_OWNERSHIP_REQUIRED",
                  "bounded_route_requires_owned_input");
  }
  return ExecuteBoundedTempSpillRouteOwned(request);
}

TempSpillResult ExecuteBoundedTempSpillRoute(TempSpillRequest&& request) {
  return ExecuteBoundedTempSpillRouteOwned(std::move(request));
}

}  // namespace scratchbird::engine::executor
