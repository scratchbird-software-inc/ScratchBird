// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "executor_batching.hpp"
#include "indexed_physical_operator.hpp"
#include "index_hash_page.hpp"
#include "index_route_capability.hpp"
#include "row_data_page.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

struct CpuCacheLayoutAuthorityContext {
  bool engine_mga_visibility_authority = false;
  bool security_recheck_authority = false;
  bool parser_client_or_donor_layout_authority = false;
  bool layout_metrics_visibility_authority = false;
  bool layout_metrics_finality_authority = false;
  bool layout_metrics_security_authority = false;
  bool layout_metrics_recovery_authority = false;
};

struct CpuCacheLayoutObservationRequest {
  std::string route_label;
  scratchbird::core::index::IndexRouteKind route =
      scratchbird::core::index::IndexRouteKind::unknown;
  scratchbird::core::index::IndexFamily expected_family =
      scratchbird::core::index::IndexFamily::unknown;
  scratchbird::core::index::IndexFamily observed_family =
      scratchbird::core::index::IndexFamily::unknown;
  const scratchbird::storage::page::RowDataPageBody* row_page = nullptr;
  const scratchbird::storage::page::IndexBtreePhysicalTree* btree_tree =
      nullptr;
  const scratchbird::storage::page::IndexHashPhysicalIndex* hash_index =
      nullptr;
  const scratchbird::storage::page::IndexHashPhysicalProbeResult* hash_probe =
      nullptr;
  const IndexedPhysicalOperatorResult* physical_result = nullptr;
  const Batch* executor_batch = nullptr;
  const ExecutorBatchResult* executor_batch_result = nullptr;
  CpuCacheLayoutAuthorityContext authority;
  std::uint64_t layout_generation = 0;
  std::uint64_t expected_layout_generation = 0;
  std::uint64_t route_capability_generation = 0;
  std::uint64_t expected_route_capability_generation = 0;
  bool runtime_consumed = false;
  bool contract_only_evidence = false;
  bool exact_fallback_available = true;
  bool benchmark_clean_claim = false;
  bool donor_dominance_claim = false;
};

struct CpuCacheLayoutObservationResult {
  bool ok = false;
  bool benchmark_clean = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<std::string> evidence;
  std::uint64_t row_count = 0;
  std::uint64_t row_width_bytes = 0;
  std::uint64_t tuple_decode_cost_units = 0;
  std::uint64_t visibility_branch_proxy = 0;
  std::uint64_t cache_miss_proxy = 0;
  std::uint64_t batch_width = 0;
  std::string route_identity;
  std::string physical_btree_identity;
  std::string physical_hash_identity;
  bool storage_layout_observed = false;
  bool btree_layout_observed = false;
  bool hash_layout_observed = false;
  bool executor_layout_observed = false;
  bool mga_security_evidence = false;
  bool metrics_authority = false;
};

CpuCacheLayoutObservationResult ObserveCpuCacheLayoutHotPath(
    const CpuCacheLayoutObservationRequest& request);

}  // namespace scratchbird::engine::executor
