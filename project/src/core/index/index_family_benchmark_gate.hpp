// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-FAMILY-BENCHMARK-GATE

#include "index_family_registry.hpp"
#include "index_route_capability.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u64;

struct IndexFamilyBenchmarkGateRequest {
  u32 sample_iterations = 5;
  bool include_fallback_disabled_rows = true;
};

struct IndexFamilyBenchmarkEvidenceRow {
  std::string family_id;
  std::string workload;
  std::string route_operation;
  IndexRouteKind route_kind = IndexRouteKind::unknown;
  std::string route_capability_kind;
  bool runtime_available = false;
  bool benchmark_clean_admissible = false;
  bool fallback_disabled = false;
  std::string cache_classification;
  u64 p50_microseconds = 0;
  u64 p95_microseconds = 0;
  u64 p99_microseconds = 0;
  u64 operation_count = 0;
  u64 rows_examined = 0;
  u64 rows_returned = 0;
  u64 rows_materialized = 0;
  u64 pages_or_containers_touched = 0;
  std::string diagnostic_code;
  std::string message_key;
  std::string blocker;
  bool concrete_runtime_consumed = false;
  bool standalone_provider_gate = false;
  bool optimizer_selected_gate = false;
  bool route_consumed_gate = false;
  bool driver_visible_gate = false;
  bool dml_route_consumed = false;
  bool sql_query_route_consumed = false;
  bool nosql_route_consumed = false;
  bool maintenance_route_consumed = false;
  bool fail_closed = true;
  bool observational_only = true;
  bool catalog_authority = false;
  bool execution_authority = false;
  bool visibility_authority = false;
  bool security_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool provider_authority = false;
  bool runtime_dependency_free = true;
  std::vector<std::string> evidence;
};

struct IndexFamilyBenchmarkGateResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<IndexFamilyBenchmarkEvidenceRow> rows;

  bool ok() const { return status.ok(); }
};

IndexFamilyBenchmarkGateResult BuildIndexFamilyBenchmarkEvidence(
    const IndexFamilyBenchmarkGateRequest& request = {});

}  // namespace scratchbird::core::index
