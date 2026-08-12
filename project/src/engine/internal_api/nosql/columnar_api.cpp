// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "columnar_api.hpp"

#include <algorithm>
#include <numeric>
#include <set>

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

bool ExactOrderedOperationChain(const ColumnarExecutionRequestV1& request) {
  const bool exact_chain =
      request.operation_ids ==
          std::vector<std::string>{"COLUMNAR_SOURCE"} ||
      request.operation_ids == std::vector<std::string>{
                                   "COLUMNAR_SOURCE", "COLUMNAR_FILTER"} ||
      request.operation_ids == std::vector<std::string>{
                                   "COLUMNAR_SOURCE", "COLUMNAR_PROJECT"} ||
      request.operation_ids == std::vector<std::string>{
                                   "COLUMNAR_SOURCE", "COLUMNAR_FILTER",
                                   "COLUMNAR_PROJECT"};
  if (!exact_chain) return false;
  if (request.operation_ids.size() == 1) {
    return request.operation_id == "COLUMNAR_SOURCE";
  }
  if (request.operation_ids.size() == 2) {
    return request.operation_id == request.operation_ids.back();
  }
  return request.operation_id.empty();
}

}  // namespace

ColumnarExecutionResultV1 ExecuteColumnarLogicalV1(
    const ColumnarExecutionRequestV1& request) {
  ColumnarExecutionResultV1 result;
  const auto refuse = [&](std::string diagnostic, std::string detail) {
    result.batch = {};
    result.row_uuids.clear();
    result.diagnostic_id = std::move(diagnostic);
    result.detail = std::move(detail);
    return result;
  };
  const bool exact_operations = ExactOrderedOperationChain(request);
  const bool source = request.operation_ids.size() == 1;
  const bool project = std::ranges::find(request.operation_ids,
                                         "COLUMNAR_PROJECT") !=
                       request.operation_ids.end();
  const bool filter = std::ranges::find(request.operation_ids,
                                        "COLUMNAR_FILTER") !=
                      request.operation_ids.end();
  if (request.abi_version != 1 ||
      request.profile_id != kColumnarLogicalReconstructionV1 ||
      !exact_operations || (!source && !project && !filter) ||
      !CanonicalUuid(request.relation_uuid) ||
      request.logical_rows.columns.empty() ||
      request.logical_rows.columns.size() > 256 ||
      request.row_uuids.size() != request.logical_rows.rows.size() ||
      request.source_generation == 0 || request.catalog_generation == 0 ||
      request.summary_generation == 0 || request.maximum_rows == 0 ||
      request.maximum_cells == 0 || !request.security_admitted ||
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
          request.maximum_cells / request.logical_rows.columns.size()) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "columnar reconstruction exceeds bounded grant");
  }
  std::set<std::string> unique_rows;
  for (const auto& row_uuid : request.row_uuids) {
    if (!CanonicalUuid(row_uuid) || !unique_rows.insert(row_uuid).second) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "columnar row identities are invalid or duplicated");
    }
  }

  const std::set<std::string> physical_only_names = {
      "dictionary_id", "segment_id", "segment_offset", "compression_code",
      "zone_id", "zone_min", "zone_max"};
  for (const auto& column : request.logical_rows.columns) {
    if (physical_only_names.contains(column.stable_name)) {
      return refuse("SB_MODEL_COLUMNAR_ENCODING_LEAK_REFUSED_V1",
                    "columnar physical encoding field reached the logical row");
    }
  }

  executor::DescriptorBatch reconstructed = request.logical_rows;
  for (std::size_t row = 0; row < reconstructed.rows.size(); ++row) {
    if (reconstructed.rows[row].values.size() != reconstructed.columns.size()) {
      return refuse("SB_EXECUTOR_ROW_WIDTH_MISMATCH",
                    "columnar physical row width is invalid");
    }
    for (std::size_t column = 0; column < reconstructed.columns.size();
         ++column) {
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
  const auto valid = executor::ValidateDescriptorBatch(reconstructed);
  if (!valid.ok) return refuse(valid.diagnostic_code, valid.detail);

  std::vector<std::size_t> candidates;
  if (ZoneProofUsable(request.zone_proof)) {
    if (!request.zone_proof.safe_negative_prune) {
      std::set<std::size_t> unique;
      for (const auto ordinal : request.zone_proof.candidate_row_ordinals) {
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
  if (project) {
    if (request.projected_columns.empty() ||
        request.projected_columns.size() > 256) {
      return refuse("SB_MODEL_COLUMNAR_PROJECTION_INVALID_V1",
                    "columnar projection width is outside 1..256");
    }
    std::set<std::size_t> unique;
    for (const auto column : request.projected_columns) {
      if (column >= reconstructed.columns.size() ||
          !unique.insert(column).second) {
        return refuse("SB_MODEL_COLUMNAR_PROJECT_DUPLICATE_REFUSED_V1",
                      "columnar projection is absent, ambiguous, or duplicate");
      }
      projections.push_back(column);
    }
  } else {
    projections.resize(reconstructed.columns.size());
    std::iota(projections.begin(), projections.end(), 0);
  }

  if (filter && request.filter_truth_values.size() != reconstructed.rows.size()) {
    return refuse("SB_MODEL_COLUMNAR_FILTER_INVALID_V1",
                  "columnar filter is not row-bound canonical truth");
  }
  for (const auto ordinal : candidates) {
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
    executor::DescriptorTuple row;
    for (const auto column : projections) {
      row.values.push_back(reconstructed.rows[ordinal].values[column]);
    }
    result.batch.rows.push_back(std::move(row));
    result.row_uuids.push_back(request.row_uuids[ordinal]);
  }
  for (const auto column : projections) {
    result.batch.columns.push_back(reconstructed.columns[column]);
  }
  result.accepted = true;
  result.root_publishable = true;
  result.exact_reconstruction_complete = true;
  result.predicate_recheck_complete = true;
  result.mga_recheck_complete = true;
  result.diagnostic_id = "SB_EXECUTOR_OK";
  return result;
}

}  // namespace scratchbird::engine::internal_api::nosql
