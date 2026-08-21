// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "columnar_api.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <numeric>
#include <set>
#include <stdexcept>

namespace scratchbird::engine::internal_api::nosql {
namespace {

bool CanonicalUuid(std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    if (!((value[i] >= '0' && value[i] <= '9') ||
          (value[i] >= 'a' && value[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool ZoneProofUsable(const ColumnarZoneProofV1& proof) {
  return proof.present && proof.profile_active && proof.comparison_exact &&
         proof.source_generation_matches &&
         proof.catalog_generation_matches &&
         proof.summary_generation_matches && proof.fresh && proof.clean &&
         proof.snapshot_safe;
}

bool ExactOrderedOperationChain(const ColumnarExecutionRequestV2& request) {
  static constexpr std::array<std::string_view, 1> kSource{
      "COLUMNAR_SOURCE"};
  static constexpr std::array<std::string_view, 2> kFilter{
      "COLUMNAR_SOURCE", "COLUMNAR_FILTER"};
  static constexpr std::array<std::string_view, 2> kProject{
      "COLUMNAR_SOURCE", "COLUMNAR_PROJECT"};
  static constexpr std::array<std::string_view, 3> kFilterProject{
      "COLUMNAR_SOURCE", "COLUMNAR_FILTER", "COLUMNAR_PROJECT"};
  const auto exact = [&](const auto& expected) {
    return request.operation_ids.size() == expected.size() &&
           std::ranges::equal(request.operation_ids, expected);
  };
  const bool exact_chain = exact(kSource) || exact(kFilter) ||
                           exact(kProject) || exact(kFilterProject);
  if (!exact_chain) return false;
  if (request.operation_ids.size() == 1) {
    return request.operation_id == "COLUMNAR_SOURCE";
  }
  if (request.operation_ids.size() == 2) {
    return request.operation_id == request.operation_ids.back();
  }
  return request.operation_id.empty();
}

bool CheckedAdd(const std::uint64_t left, const std::uint64_t right,
                std::uint64_t* out) {
  if (out == nullptr ||
      left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  *out = left + right;
  return true;
}

bool CheckedMultiply(const std::uint64_t left, const std::uint64_t right,
                     std::uint64_t* out) {
  if (out == nullptr ||
      (left != 0 &&
       right > std::numeric_limits<std::uint64_t>::max() / left)) {
    return false;
  }
  *out = left * right;
  return true;
}

bool AccountArray(const std::size_t count, const std::size_t element_bytes,
                  std::uint64_t* bytes) {
  std::uint64_t allocation = 0;
  return CheckedMultiply(count, element_bytes, &allocation) &&
         CheckedAdd(*bytes, allocation, bytes);
}

bool AccountString(const std::string& value, std::uint64_t* bytes) {
  return CheckedAdd(*bytes, value.capacity(), bytes) &&
         CheckedAdd(*bytes, 1, bytes);
}

bool AccountDescriptor(const EngineDescriptor& descriptor,
                       std::uint64_t* bytes) {
  return AccountString(descriptor.descriptor_uuid.canonical, bytes) &&
         AccountString(descriptor.descriptor_kind, bytes) &&
         AccountString(descriptor.canonical_type_name, bytes) &&
         AccountString(descriptor.encoded_descriptor, bytes);
}

bool AccountValue(const EngineTypedValue& value, std::uint64_t* bytes) {
  return AccountDescriptor(value.descriptor, bytes) &&
         AccountString(value.encoded_value, bytes) &&
         CheckedAdd(*bytes, value.binary_value.capacity(), bytes);
}

std::optional<std::uint64_t> DescriptorBatchMemoryBytes(
    const executor::DescriptorBatch& batch) {
  std::uint64_t bytes = sizeof(batch);
  if (!AccountArray(batch.columns.capacity(),
                    sizeof(executor::ExecutorColumnDescriptor), &bytes) ||
      !AccountArray(batch.rows.capacity(),
                    sizeof(executor::DescriptorTuple), &bytes)) {
    return std::nullopt;
  }
  for (const auto& column : batch.columns) {
    if (!AccountString(column.stable_name, &bytes) ||
        !AccountDescriptor(column.descriptor, &bytes)) {
      return std::nullopt;
    }
  }
  for (const auto& row : batch.rows) {
    if (!AccountArray(row.values.capacity(), sizeof(EngineTypedValue),
                      &bytes)) {
      return std::nullopt;
    }
    for (const auto& value : row.values) {
      if (!AccountValue(value, &bytes)) return std::nullopt;
    }
  }
  return bytes;
}

bool AccountMgaContext(const executor::PhysicalMgaStatementContext& context,
                       std::uint64_t* bytes) {
  return AccountString(context.statement_uuid, bytes) &&
         AccountString(context.owning_transaction_uuid, bytes) &&
         AccountString(context.statement_snapshot_uuid, bytes) &&
         AccountString(context.statement_metadata_snapshot_uuid, bytes) &&
         AccountArray(context.active_excluded_local_transaction_ids.capacity(),
                      sizeof(std::uint64_t), bytes) &&
         AccountArray(context.in_doubt_excluded_local_transaction_ids.capacity(),
                      sizeof(std::uint64_t), bytes) &&
         AccountString(context.snapshot_kind, bytes) &&
         AccountString(context.statement_timestamp, bytes);
}

std::optional<std::uint64_t> RequestCarrierMemoryBytes(
    const ColumnarExecutionRequestV2& request) {
  std::uint64_t bytes = sizeof(request);
  const auto batch = DescriptorBatchMemoryBytes(request.logical_rows);
  if (!batch.has_value() || !CheckedAdd(bytes, *batch, &bytes) ||
      !AccountString(request.profile_id, &bytes) ||
      !AccountString(request.operation_id, &bytes) ||
      !AccountString(request.relation_uuid, &bytes) ||
      !AccountMgaContext(request.statement_context, &bytes) ||
      !AccountMgaContext(request.current_statement_context, &bytes) ||
      !AccountArray(request.operation_ids.capacity(), sizeof(std::string),
                    &bytes) ||
      !AccountArray(request.row_uuids.capacity(), sizeof(std::string),
                    &bytes) ||
      !AccountArray(request.projected_columns.capacity(), sizeof(std::size_t),
                    &bytes) ||
      !AccountArray(request.filter_truth_values.capacity(),
                    sizeof(EngineSqlTruthValue), &bytes) ||
      !AccountArray(request.zone_proof.candidate_row_ordinals.capacity(),
                    sizeof(std::size_t), &bytes)) {
    return std::nullopt;
  }
  for (const auto& operation : request.operation_ids) {
    if (!AccountString(operation, &bytes)) return std::nullopt;
  }
  for (const auto& row_uuid : request.row_uuids) {
    if (!AccountString(row_uuid, &bytes)) return std::nullopt;
  }
  return bytes;
}

}  // namespace

static ColumnarExecutionResultV2 ExecuteColumnarLogicalV2Impl(
    ColumnarExecutionRequestV2 request) {
  ColumnarExecutionResultV2 result;
  result.memory_grant_bytes = request.maximum_memory_bytes;
  const auto refuse = [&](std::string diagnostic, std::string detail) {
    result.batch = {};
    result.row_uuids.clear();
    result.diagnostic_id = std::move(diagnostic);
    result.detail = std::move(detail);
    return std::move(result);
  };
  const auto cancelled = [&]() noexcept {
    if (!request.cancellation_requested) return false;
    try {
      if (!request.cancellation_requested(request.cancellation_context)) {
        return false;
      }
      result.cancellation_observed = true;
      return true;
    } catch (...) {
      result.cancellation_probe_failed = true;
      return true;
    }
  };
  const auto cancellation_refusal = [&] {
    return refuse(result.cancellation_probe_failed
                      ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                      : "SB_MODEL_EXECUTION_CANCELLED_V1",
                  result.cancellation_probe_failed
                      ? "columnar cancellation probe failed"
                      : "columnar execution was cancelled");
  };
  try {
  if (cancelled()) return cancellation_refusal();
  const bool exact_operations = ExactOrderedOperationChain(request);
  const bool source = request.operation_ids.size() == 1;
  const bool project = std::ranges::find(request.operation_ids,
                                         "COLUMNAR_PROJECT") !=
                       request.operation_ids.end();
  const bool filter = std::ranges::find(request.operation_ids,
                                        "COLUMNAR_FILTER") !=
                      request.operation_ids.end();
  if (request.abi_version != 2 ||
      request.profile_id != kColumnarLogicalReconstructionV2 ||
      !exact_operations || (!source && !project && !filter) ||
      !CanonicalUuid(request.relation_uuid) ||
      request.logical_rows.columns.empty() ||
      request.logical_rows.columns.size() > 256 ||
      request.row_uuids.size() != request.logical_rows.rows.size() ||
      request.source_generation == 0 || request.catalog_generation == 0 ||
      request.summary_generation == 0 || request.maximum_rows == 0 ||
      request.maximum_input_cells == 0 || request.maximum_cells == 0 ||
      request.maximum_memory_bytes == 0 ||
      !request.cancellation_requested || !request.security_admitted ||
      request.parser_execution_authority_claimed ||
      request.zone_visibility_authority_claimed ||
      request.zone_finality_authority_claimed) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "columnar request is incomplete or claims forbidden authority");
  }
  if (!executor::PhysicalMgaStatementContextValid(request.statement_context) ||
      !executor::PhysicalMgaStatementContextValid(
          request.current_statement_context) ||
      !executor::PhysicalMgaStatementContextEqual(
          request.statement_context, request.current_statement_context)) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "columnar execution statement context changed");
  }
  if (filter &&
      (!request.canonical_predicate_bound || request.lossy_coercion_requested)) {
    return refuse(request.lossy_coercion_requested
                      ? "SB_MODEL_JOIN_LOSSY_COERCION_REFUSED_V1"
                      : "SB_MODEL_COLUMNAR_FILTER_INVALID_V1",
                  "columnar filter is not an exact canonical typed predicate");
  }
  if (request.logical_rows.rows.size() > request.maximum_rows ||
      request.logical_rows.rows.size() >
          request.maximum_input_cells / request.logical_rows.columns.size()) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "columnar reconstruction exceeds bounded grant");
  }
  const auto request_memory = RequestCarrierMemoryBytes(request);
  const auto input_batch_memory =
      DescriptorBatchMemoryBytes(request.logical_rows);
  std::uint64_t projected_peak_memory = 0;
  std::uint64_t auxiliary_memory = 0;
  std::uint64_t output_row_identity_memory = 0;
  std::uint64_t row_set_memory = 0;
  std::uint64_t ordinal_set_memory = 0;
  std::uint64_t vector_memory = 0;
  constexpr std::uint64_t kResultStringBudget = 512;
  const auto maximum_set_ordinals = std::max(
      request.logical_rows.rows.size(), request.logical_rows.columns.size());
  const auto row_set_node_bytes =
      sizeof(std::string_view) + 6 * sizeof(void*);
  const auto ordinal_set_node_bytes =
      sizeof(std::size_t) + 6 * sizeof(void*);
  if (!AccountArray(request.row_uuids.size(), sizeof(std::string),
                    &output_row_identity_memory) ||
      !CheckedMultiply(request.row_uuids.size(), row_set_node_bytes,
                       &row_set_memory) ||
      !CheckedMultiply(maximum_set_ordinals, ordinal_set_node_bytes,
                       &ordinal_set_memory) ||
      !AccountArray(request.logical_rows.rows.size(), sizeof(std::size_t),
                    &vector_memory) ||
      !AccountArray(request.logical_rows.columns.size(), sizeof(std::size_t),
                    &vector_memory)) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "columnar auxiliary memory bound overflowed");
  }
  for (const auto& row_uuid : request.row_uuids) {
    if (!AccountString(row_uuid, &output_row_identity_memory)) {
      output_row_identity_memory =
          std::numeric_limits<std::uint64_t>::max();
      break;
    }
  }
  if (!request_memory.has_value() || !input_batch_memory.has_value() ||
      !CheckedAdd(row_set_memory, ordinal_set_memory,
                  &auxiliary_memory) ||
      !CheckedAdd(auxiliary_memory, vector_memory, &auxiliary_memory) ||
      !CheckedAdd(auxiliary_memory, sizeof(result), &auxiliary_memory) ||
      !CheckedAdd(auxiliary_memory, kResultStringBudget,
                  &auxiliary_memory) ||
      !CheckedAdd(*request_memory, *input_batch_memory,
                  &projected_peak_memory) ||
      !CheckedAdd(projected_peak_memory, auxiliary_memory,
                  &projected_peak_memory) ||
      !CheckedAdd(projected_peak_memory, output_row_identity_memory,
                  &projected_peak_memory) ||
      projected_peak_memory > request.maximum_memory_bytes) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "columnar input and reconstruction memory exceed the grant");
  }
  result.current_live_memory_bytes = *request_memory;
  result.peak_live_memory_bytes = projected_peak_memory;
  {
    std::set<std::string_view> unique_rows;
    for (const auto& row_uuid : request.row_uuids) {
      if (cancelled()) return cancellation_refusal();
      if (!CanonicalUuid(row_uuid) || !unique_rows.insert(row_uuid).second) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "columnar row identities are invalid or duplicated");
      }
    }
  }

  static constexpr std::array<std::string_view, 7> kPhysicalOnlyNames{
      "dictionary_id", "segment_id", "segment_offset", "compression_code",
      "zone_id", "zone_min", "zone_max"};
  for (const auto& column : request.logical_rows.columns) {
    if (cancelled()) return cancellation_refusal();
    if (std::ranges::find(kPhysicalOnlyNames, column.stable_name) !=
        kPhysicalOnlyNames.end()) {
      return refuse("SB_MODEL_COLUMNAR_ENCODING_LEAK_REFUSED_V1",
                    "columnar physical encoding field reached the logical row");
    }
  }

  executor::DescriptorBatch reconstructed = std::move(request.logical_rows);
  for (std::size_t row = 0; row < reconstructed.rows.size(); ++row) {
    if (cancelled()) return cancellation_refusal();
    if (reconstructed.rows[row].values.size() != reconstructed.columns.size()) {
      return refuse("SB_EXECUTOR_ROW_WIDTH_MISMATCH",
                    "columnar physical row width is invalid");
    }
    for (std::size_t column = 0; column < reconstructed.columns.size();
         ++column) {
      if (cancelled()) return cancellation_refusal();
      auto& value = reconstructed.rows[row].values[column];
      if (value.state == EngineValueState::missing) {
        if (!reconstructed.columns[column].nullable) {
          return refuse("SB_EXECUTOR_NULL_NOT_ALLOWED",
                        "missing columnar value targets a non-null descriptor");
        }
        value.encoded_value.clear();
        value.binary_value.clear();
        value.setState(EngineValueState::sql_null);
      } else if (value.state != EngineValueState::value &&
                 value.state != EngineValueState::sql_null) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "columnar logical reconstruction received a non-value sentinel");
      }
    }
  }
  bool validation_cancelled = false;
  const auto valid = executor::ValidateDescriptorBatch(
      reconstructed, request.cancellation_requested,
      request.cancellation_context, &validation_cancelled);
  if (!valid.ok) {
    if (validation_cancelled) result.cancellation_observed = true;
    if (valid.diagnostic_code == "SB_MODEL_COORDINATOR_LEG_FAILED_V1") {
      result.cancellation_probe_failed = true;
    }
    return refuse(valid.diagnostic_code, valid.detail);
  }

  std::vector<std::size_t> candidates;
  candidates.reserve(reconstructed.rows.size());
  if (ZoneProofUsable(request.zone_proof)) {
    if (!request.zone_proof.safe_negative_prune) {
      std::set<std::size_t> unique;
      for (const auto ordinal : request.zone_proof.candidate_row_ordinals) {
        if (cancelled()) return cancellation_refusal();
        if (ordinal >= reconstructed.rows.size() ||
            !unique.insert(ordinal).second) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        "columnar zone candidate receipt is malformed");
        }
        candidates.push_back(ordinal);
      }
    }
    result.physical_operator_id = "PHYSICAL_COLUMNAR_ZONE_SCAN_V1";
  } else {
    if (!request.exact_reconstruction_fallback_available) {
      return refuse("SB_MODEL_COLUMNAR_EXACT_FALLBACK_UNAVAILABLE_V1",
                    "exact row reconstruction fallback is unavailable");
    }
    candidates.resize(reconstructed.rows.size());
    if (cancelled()) return cancellation_refusal();
    std::iota(candidates.begin(), candidates.end(), 0);
    result.exact_fallback_selected = true;
    result.physical_operator_id = "COLUMNAR_ROW_RECONSTRUCTION_SCAN_V1";
    if (request.zone_proof.present) {
      result.fallback_reason_id =
          request.zone_proof.snapshot_safe
              ? "ZONE.COMPARISON_PROFILE_MISMATCH"
              : "ZONE.MGA_SNAPSHOT_UNSAFE";
    }
  }

  std::vector<std::size_t> projections;
  projections.reserve(project ? request.projected_columns.size()
                              : reconstructed.columns.size());
  if (project) {
    if (request.projected_columns.empty() ||
        request.projected_columns.size() > 256) {
      return refuse("SB_MODEL_COLUMNAR_PROJECTION_INVALID_V1",
                    "columnar projection width is outside 1..256");
    }
    std::set<std::size_t> unique;
    for (const auto column : request.projected_columns) {
      if (cancelled()) return cancellation_refusal();
      if (column >= reconstructed.columns.size() ||
          !unique.insert(column).second) {
        return refuse("SB_MODEL_COLUMNAR_PROJECT_DUPLICATE_REFUSED_V1",
                      "columnar projection is absent, ambiguous, or duplicate");
      }
      projections.push_back(column);
    }
  } else {
    projections.resize(reconstructed.columns.size());
    if (cancelled()) return cancellation_refusal();
    std::iota(projections.begin(), projections.end(), 0);
  }

  if (filter && request.filter_truth_values.size() != reconstructed.rows.size()) {
    return refuse("SB_MODEL_COLUMNAR_FILTER_INVALID_V1",
                  "columnar filter is not row-bound canonical truth");
  }
  std::size_t selected_row_count = 0;
  for (const auto ordinal : candidates) {
    if (cancelled()) return cancellation_refusal();
    if (filter) {
      const auto truth = request.filter_truth_values[ordinal];
      if (truth == EngineSqlTruthValue::false_value ||
          truth == EngineSqlTruthValue::unknown) {
        continue;
      }
      if (truth != EngineSqlTruthValue::true_value) {
        return refuse("SB_MODEL_COLUMNAR_FILTER_INVALID_V1",
                      "columnar filter received an unspecified truth value");
      }
    }
    ++selected_row_count;
  }
  if (selected_row_count > request.maximum_rows ||
      (selected_row_count != 0 &&
       projections.size() > request.maximum_cells / selected_row_count)) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "columnar output exceeds its row or cell ceiling");
  }
  result.batch.columns.reserve(projections.size());
  result.batch.rows.reserve(selected_row_count);
  result.row_uuids.reserve(selected_row_count);
  for (const auto ordinal : candidates) {
    if (cancelled()) return cancellation_refusal();
    if (filter &&
        request.filter_truth_values[ordinal] !=
            EngineSqlTruthValue::true_value) {
      continue;
    }
    executor::DescriptorTuple row;
    row.values.reserve(projections.size());
    for (const auto column : projections) {
      if (cancelled()) return cancellation_refusal();
      row.values.push_back(reconstructed.rows[ordinal].values[column]);
    }
    result.batch.rows.push_back(std::move(row));
    result.row_uuids.push_back(request.row_uuids[ordinal]);
  }
  for (const auto column : projections) {
    if (cancelled()) return cancellation_refusal();
    result.batch.columns.push_back(reconstructed.columns[column]);
  }
  if (cancelled()) return cancellation_refusal();
  result.diagnostic_id = "SB_EXECUTOR_OK";
  const auto retained_batch_memory = DescriptorBatchMemoryBytes(result.batch);
  std::uint64_t retained_memory = sizeof(result);
  if (!retained_batch_memory.has_value() ||
      !CheckedAdd(retained_memory, *retained_batch_memory,
                  &retained_memory) ||
      !AccountArray(result.row_uuids.capacity(), sizeof(std::string),
                    &retained_memory) ||
      !AccountString(result.physical_operator_id, &retained_memory) ||
      !AccountString(result.fallback_reason_id, &retained_memory) ||
      !AccountString(result.diagnostic_id, &retained_memory) ||
      !AccountString(result.detail, &retained_memory)) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "columnar result memory receipt overflowed");
  }
  for (const auto& row_uuid : result.row_uuids) {
    if (cancelled()) return cancellation_refusal();
    if (!AccountString(row_uuid, &retained_memory)) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "columnar result identity memory receipt overflowed");
    }
  }
  if (retained_memory > request.maximum_memory_bytes) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "columnar retained result exceeds its byte grant");
  }
  result.current_live_memory_bytes = retained_memory;
  result.peak_live_memory_bytes =
      std::max(result.peak_live_memory_bytes, retained_memory);
  result.memory_receipt_complete =
      result.current_live_memory_bytes <= result.peak_live_memory_bytes &&
      result.peak_live_memory_bytes <= result.memory_grant_bytes;
  if (!result.memory_receipt_complete) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "columnar memory receipt is incomplete");
  }
  if (cancelled()) return cancellation_refusal();
  result.accepted = true;
  result.root_publishable = true;
  result.exact_reconstruction_complete = true;
  result.predicate_recheck_complete = true;
  result.mga_recheck_complete = true;
  return std::move(result);
  } catch (const std::bad_alloc&) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "columnar execution allocation was refused");
  } catch (const std::length_error&) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "columnar execution allocation length was refused");
  } catch (const std::exception& exception) {
    return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                  std::string("columnar execution threw: ") +
                      exception.what());
  } catch (...) {
    return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                  "columnar execution threw a non-standard exception");
  }
}

ColumnarExecutionResultV1 ExecuteColumnarLogicalV1(
    const ColumnarExecutionRequestV1& request) {
  if (request.abi_version != 1 ||
      request.profile_id != kColumnarLogicalReconstructionV1) {
    ColumnarExecutionResultV1 result;
    result.diagnostic_id = "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1";
    result.detail =
        "columnar request is incomplete or claims forbidden authority";
    return result;
  }
  ColumnarExecutionRequestV2 bounded;
  bounded.operation_ids = request.operation_ids;
  bounded.operation_id = request.operation_id;
  bounded.relation_uuid = request.relation_uuid;
  bounded.row_uuids = request.row_uuids;
  bounded.logical_rows = request.logical_rows;
  bounded.projected_columns = request.projected_columns;
  bounded.filter_truth_values = request.filter_truth_values;
  bounded.representation = request.representation;
  bounded.zone_proof = request.zone_proof;
  bounded.statement_context = request.statement_context;
  bounded.current_statement_context = request.current_statement_context;
  bounded.source_generation = request.source_generation;
  bounded.catalog_generation = request.catalog_generation;
  bounded.summary_generation = request.summary_generation;
  bounded.maximum_rows = request.maximum_rows;
  bounded.maximum_input_cells = request.maximum_cells;
  bounded.maximum_cells = request.maximum_cells;
  bounded.maximum_memory_bytes =
      std::numeric_limits<std::uint64_t>::max();
  bounded.cancellation_requested = [](const void*) { return false; };
  bounded.security_admitted = request.security_admitted;
  bounded.canonical_predicate_bound = request.canonical_predicate_bound;
  bounded.lossy_coercion_requested = request.lossy_coercion_requested;
  bounded.exact_reconstruction_fallback_available =
      request.exact_reconstruction_fallback_available;
  bounded.parser_execution_authority_claimed =
      request.parser_execution_authority_claimed;
  bounded.zone_visibility_authority_claimed =
      request.zone_visibility_authority_claimed;
  bounded.zone_finality_authority_claimed =
      request.zone_finality_authority_claimed;
  auto upgraded = ExecuteColumnarLogicalV2(std::move(bounded));
  ColumnarExecutionResultV1 result;
  result.accepted = upgraded.accepted;
  result.root_publishable = upgraded.root_publishable;
  result.exact_fallback_selected = upgraded.exact_fallback_selected;
  result.exact_reconstruction_complete =
      upgraded.exact_reconstruction_complete;
  result.predicate_recheck_complete = upgraded.predicate_recheck_complete;
  result.mga_recheck_complete = upgraded.mga_recheck_complete;
  result.row_uuids = std::move(upgraded.row_uuids);
  result.batch = std::move(upgraded.batch);
  result.physical_operator_id = std::move(upgraded.physical_operator_id);
  result.fallback_reason_id = std::move(upgraded.fallback_reason_id);
  result.diagnostic_id = std::move(upgraded.diagnostic_id);
  result.detail = std::move(upgraded.detail);
  return result;
}

ColumnarExecutionResultV2 ExecuteColumnarLogicalV2(
    const ColumnarExecutionRequestV2& request) {
  try {
    return ExecuteColumnarLogicalV2Impl(request);
  } catch (const std::bad_alloc&) {
    ColumnarExecutionResultV2 result;
    result.memory_grant_bytes = request.maximum_memory_bytes;
    result.diagnostic_id = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
    result.detail = "columnar request copy allocation was refused";
    return result;
  } catch (const std::length_error&) {
    ColumnarExecutionResultV2 result;
    result.memory_grant_bytes = request.maximum_memory_bytes;
    result.diagnostic_id = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
    result.detail = "columnar request copy length was refused";
    return result;
  } catch (...) {
    ColumnarExecutionResultV2 result;
    result.memory_grant_bytes = request.maximum_memory_bytes;
    result.diagnostic_id = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
    result.detail = "columnar request copy failed";
    return result;
  }
}

ColumnarExecutionResultV2 ExecuteColumnarLogicalV2(
    ColumnarExecutionRequestV2&& request) {
  const auto memory_grant_bytes = request.maximum_memory_bytes;
  try {
    return ExecuteColumnarLogicalV2Impl(std::move(request));
  } catch (const std::bad_alloc&) {
    ColumnarExecutionResultV2 result;
    result.memory_grant_bytes = memory_grant_bytes;
    result.diagnostic_id = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
    result.detail = "columnar request move allocation was refused";
    return result;
  } catch (...) {
    ColumnarExecutionResultV2 result;
    result.memory_grant_bytes = memory_grant_bytes;
    result.diagnostic_id = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
    result.detail = "columnar request move failed";
    return result;
  }
}

}  // namespace scratchbird::engine::internal_api::nosql
