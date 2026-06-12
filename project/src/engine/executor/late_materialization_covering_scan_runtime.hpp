// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "covering_index_payload.hpp"
#include "indexed_physical_operator.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

struct IndexRuntimeEngineRecheckProof {
  bool physical_tree_present = true;
  bool plan_safe = true;
  bool durable_mga_inventory_proof = true;
  bool mga_visibility_rechecked_by_engine = true;
  bool security_authorized_by_engine = true;
  bool redaction_checked_by_engine = true;
  bool payload_freshness_safe = true;
  bool descriptor_or_map_scan_fallback = false;
  bool full_table_scan_or_materialization = false;
  bool parser_or_reference_authority = false;
  bool index_payload_authority = false;
};

struct LateMaterializedIndexedRow {
  std::string row_uuid;
  std::string version_uuid;
  std::uint64_t stream_ordinal = 0;
  std::vector<std::string> projected_values;
  bool redacted = false;
  bool provider_full_table_scan_used = false;
  bool provider_descriptor_or_map_scan_used = false;
  std::vector<std::string> evidence;
};

struct LateMaterializationIndexedProviderResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  LateMaterializedIndexedRow row;
  std::vector<std::string> evidence;
};

struct LateMaterializationIndexedRuntimeResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<LateMaterializedIndexedRow> rows;
  std::vector<std::string> evidence;
  bool runtime_route_capability = false;
  bool benchmark_clean = false;
  bool full_table_scan_or_materialization = false;
};

using LateMaterializationIndexedRowProvider =
    std::function<LateMaterializationIndexedProviderResult(
        const IndexedPhysicalOperatorLocator&)>;

LateMaterializationIndexedRuntimeResult
ConsumeIndexedRowIdStreamForLateMaterialization(
    const IndexedPhysicalOperatorResult& physical_stream,
    const IndexRuntimeEngineRecheckProof& proof,
    const LateMaterializationIndexedRowProvider& row_provider);

struct CoveringProjectionCell {
  std::uint32_t projection_ordinal = 0;
  scratchbird::core::index::CoveringIndexPayloadValueKind kind =
      scratchbird::core::index::CoveringIndexPayloadValueKind::null_value;
  std::vector<scratchbird::core::platform::byte> encoded_value;
  bool redacted = false;
};

struct CoveringProjectionRow {
  std::string row_uuid;
  std::string version_uuid;
  std::uint64_t stream_ordinal = 0;
  std::vector<CoveringProjectionCell> cells;
};

struct CoveringProjectionOnlyScanRequest {
  const IndexedPhysicalOperatorResult* physical_stream = nullptr;
  std::vector<const scratchbird::core::index::CoveringIndexPayloadAdmission*>
      admissions;
  IndexRuntimeEngineRecheckProof proof;
};

struct CoveringProjectionOnlyScanResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<CoveringProjectionRow> rows;
  std::vector<std::string> evidence;
  bool runtime_route_capability = false;
  bool benchmark_clean = false;
  bool projection_only = false;
  bool full_table_scan_or_materialization = false;
};

CoveringProjectionOnlyScanResult ExecuteCoveringProjectionOnlyScan(
    const CoveringProjectionOnlyScanRequest& request);

enum class IndexPlanShapeRequiredPath {
  indexed_point_lookup,
  indexed_range_scan,
  indexed_ordered_limit,
  late_materialization_row_id_stream,
  covering_projection_only
};

struct IndexPlanShapeRegressionGuardRequest {
  std::string route_name;
  IndexPlanShapeRequiredPath required_path =
      IndexPlanShapeRequiredPath::indexed_point_lookup;
  const IndexedPhysicalOperatorResult* physical_result = nullptr;
  const LateMaterializationIndexedRuntimeResult* late_materialization_result =
      nullptr;
  const CoveringProjectionOnlyScanResult* covering_projection_result = nullptr;
  bool table_scan_fallback = false;
  bool descriptor_scan_fallback = false;
  bool map_scan_fallback = false;
  bool per_row_wrapper_execution = false;
  bool text_result_materialization = false;
  bool statistics_only_optimizer_route = false;
  bool local_candidate_planning = false;
  bool contract_only_evidence = false;
  bool stale_route_capability = false;
  std::uint64_t expected_route_capability_generation = 0;
  std::uint64_t observed_route_capability_generation = 0;
  bool invalid_route_family_use = false;
  std::string invalid_route_family_detail;
  bool missing_physical_tree_blocker = false;
  bool stale_plan_blocker = false;
  bool missing_mga_security_redaction_proof_blocker = false;
  bool missing_covering_payload_blocker = false;
  bool missing_encoded_key_or_bounds_blocker = false;
  bool unsupported_physical_family_blocker = false;
  std::string unsupported_physical_family;
  bool benchmark_clean_claim = false;
  bool reference_dominance_claim = false;
};

struct IndexPlanShapeRegressionGuardResult {
  bool ok = false;
  bool exact_blocker = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<std::string> evidence;
  bool physical_route_consumed = false;
  bool scan_only_regression = false;
  bool per_row_wrapper_regression = false;
  bool text_result_regression = false;
  bool statistics_only_regression = false;
  bool benchmark_clean = false;
};

const char* IndexPlanShapeRequiredPathName(IndexPlanShapeRequiredPath path);

IndexPlanShapeRegressionGuardResult EvaluateIndexPlanShapeRegressionGuard(
    const IndexPlanShapeRegressionGuardRequest& request);

}  // namespace scratchbird::engine::executor
