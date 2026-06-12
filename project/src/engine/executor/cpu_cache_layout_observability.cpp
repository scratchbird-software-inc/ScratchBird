// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cpu_cache_layout_observability.hpp"

#include <algorithm>
#include <sstream>

namespace scratchbird::engine::executor {
namespace {

namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;

void Add(CpuCacheLayoutObservationResult* result,
         std::string key,
         std::string value) {
  result->evidence.push_back("orh289." + std::move(key) + "=" +
                             std::move(value));
}

std::string Bool(bool value) {
  return value ? "true" : "false";
}

std::string Hash(std::vector<std::string> fields) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& field : fields) {
    for (const unsigned char ch : field) {
      hash ^= ch;
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << "fnv64:" << std::hex << hash;
  return out.str();
}

CpuCacheLayoutObservationResult Refuse(
    const CpuCacheLayoutObservationRequest& request,
    std::string code,
    std::string detail) {
  CpuCacheLayoutObservationResult result;
  result.ok = false;
  result.benchmark_clean = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  result.metrics_authority =
      request.authority.parser_client_or_reference_layout_authority ||
      request.authority.layout_metrics_visibility_authority ||
      request.authority.layout_metrics_finality_authority ||
      request.authority.layout_metrics_security_authority ||
      request.authority.layout_metrics_recovery_authority;
  Add(&result, "route_label", request.route_label);
  Add(&result, "fail_closed", "true");
  Add(&result, "diagnostic", result.diagnostic_code);
  Add(&result, "benchmark_clean", "false");
  Add(&result,
      "exact_fallback_available",
      Bool(request.exact_fallback_available));
  Add(&result,
      "parser_client_reference_layout_authority",
      Bool(request.authority.parser_client_or_reference_layout_authority));
  Add(&result,
      "layout_metrics_visibility_authority",
      Bool(request.authority.layout_metrics_visibility_authority));
  Add(&result,
      "layout_metrics_finality_authority",
      Bool(request.authority.layout_metrics_finality_authority));
  Add(&result,
      "layout_metrics_security_authority",
      Bool(request.authority.layout_metrics_security_authority));
  Add(&result,
      "layout_metrics_recovery_authority",
      Bool(request.authority.layout_metrics_recovery_authority));
  return result;
}

std::uint64_t CellPayloadBytes(const page::RowDataCell& cell) {
  return static_cast<std::uint64_t>(cell.value.payload.size());
}

std::uint64_t RowWidth(const page::RowDataRecord& row) {
  std::uint64_t bytes = 16 + 16 + sizeof(row.local_transaction_id) +
                        sizeof(row.internal_row_ordinal) +
                        sizeof(row.row_version) + 1;
  for (const auto& cell : row.cells) {
    bytes += sizeof(cell.column_ordinal) + CellPayloadBytes(cell);
  }
  return bytes;
}

std::uint64_t BatchMaterializedCells(const Batch& batch) {
  std::uint64_t cells = 0;
  for (const auto& row : batch.rows) {
    cells += static_cast<std::uint64_t>(row.values.size());
  }
  return cells;
}

std::string BtreeIdentity(const page::IndexBtreePhysicalTree& tree) {
  return Hash({"btree",
               std::to_string(tree.root_page_number),
               std::to_string(tree.next_page_number),
               std::to_string(tree.page_size),
               std::to_string(tree.pages.size())});
}

std::string HashIdentity(const page::IndexHashPhysicalIndex& index) {
  return Hash({"hash",
               std::to_string(index.directory_page_number),
               std::to_string(index.next_page_number),
               std::to_string(index.page_size),
               std::to_string(index.pages.size()),
               std::to_string(index.hash_algorithm_version)});
}

bool PhysicalResultHasRechecks(const IndexedPhysicalOperatorResult& result) {
  if (!result.ok || !result.runtime_route_capability ||
      result.table_scan_consumed) {
    return false;
  }
  return std::all_of(result.locators.begin(),
                     result.locators.end(),
                     [](const auto& locator) {
                       return locator.from_physical_index &&
                              locator.mga_recheck_required &&
                              locator.security_recheck_required;
                     });
}

}  // namespace

CpuCacheLayoutObservationResult ObserveCpuCacheLayoutHotPath(
    const CpuCacheLayoutObservationRequest& request) {
  if (!request.exact_fallback_available) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.NO_EXACT_FALLBACK",
                  "exact fallback is required before observing layout metrics");
  }
  if (!request.runtime_consumed || request.contract_only_evidence) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.NO_RUNTIME_CONSUMPTION",
                  "layout evidence must be emitted by a consumed runtime path");
  }
  if (request.authority.parser_client_or_reference_layout_authority) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.UNSAFE_LAYOUT_AUTHORITY",
                  "parser/client/reference layout authority is forbidden");
  }
  if (request.authority.layout_metrics_visibility_authority ||
      request.authority.layout_metrics_finality_authority ||
      request.authority.layout_metrics_security_authority ||
      request.authority.layout_metrics_recovery_authority) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.METRICS_AUTHORITY_UNSAFE",
                  "layout metrics are advisory observability only");
  }
  if (!request.authority.engine_mga_visibility_authority ||
      !request.authority.security_recheck_authority) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.MISSING_MGA_SECURITY_PROOF",
                  "MGA/security authority evidence is required");
  }
  if (request.layout_generation == 0 ||
      request.layout_generation != request.expected_layout_generation) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.STALE_LAYOUT_GENERATION",
                  "layout generation mismatch");
  }
  if (request.route_capability_generation == 0 ||
      request.route_capability_generation !=
          request.expected_route_capability_generation) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.STALE_ROUTE_CAPABILITY",
                  "route capability generation mismatch");
  }
  if (request.benchmark_clean_claim || request.reference_dominance_claim) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.BENCHMARK_OVERCLAIM",
                  "layout observability is not benchmark or reference dominance proof");
  }
  if (request.expected_family != request.observed_family) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.ROUTE_FAMILY_IDENTITY_MISMATCH",
                  "observed physical family does not match expected route family");
  }
  const auto* capability = idx::FindBuiltinIndexRouteCapabilityState(
      request.route, request.expected_family);
  if (capability == nullptr || !capability->route_complete()) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.UNSUPPORTED_PHYSICAL_FAMILY_IDENTITY",
                  capability == nullptr ? "route capability missing"
                                        : capability->route_diagnostic_code);
  }
  if (request.row_page == nullptr || request.row_page->rows.empty()) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.STORAGE_LAYOUT_MISSING",
                  "row page layout evidence required");
  }
  if (request.btree_tree == nullptr ||
      request.physical_result == nullptr ||
      !PhysicalResultHasRechecks(*request.physical_result)) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.INDEX_RUNTIME_IDENTITY_MISSING",
                  "physical index route identity and recheck evidence required");
  }
  const auto btree_report =
      page::BuildIndexBtreePhysicalTreeReport(*request.btree_tree);
  if (!btree_report.ok() || !btree_report.report.valid ||
      btree_report.report.visibility_authority ||
      btree_report.report.transaction_finality_authority ||
      btree_report.report.authorization_authority ||
      btree_report.report.recovery_authority) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.BTREE_REPORT_UNSAFE",
                  "physical B-tree report must be valid and non-authoritative");
  }
  if (request.hash_index == nullptr || request.hash_probe == nullptr ||
      !request.hash_probe->ok()) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.HASH_RUNTIME_IDENTITY_MISSING",
                  "physical hash probe identity required");
  }
  const auto hash_report = page::BuildIndexHashPhysicalReport(*request.hash_index);
  if (!hash_report.ok() || !hash_report.report.valid ||
      hash_report.report.visibility_authority ||
      hash_report.report.transaction_finality_authority ||
      hash_report.report.authorization_authority ||
      hash_report.report.recovery_authority) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.HASH_REPORT_UNSAFE",
                  "physical hash report must be valid and non-authoritative");
  }
  if (request.executor_batch == nullptr ||
      request.executor_batch_result == nullptr ||
      request.executor_batch_result->evidence.error ||
      request.executor_batch_result->evidence.authority.owns_visibility ||
      request.executor_batch_result->evidence.authority.owns_transaction_finality ||
      request.executor_batch_result->evidence.authority.owns_recovery) {
    return Refuse(request,
                  "ORH_CPU_CACHE_LAYOUT.EXECUTOR_LAYOUT_MISSING",
                  "executor tuple decode and batch width evidence required");
  }

  CpuCacheLayoutObservationResult result;
  result.ok = true;
  result.benchmark_clean = true;
  result.fail_closed = false;
  result.diagnostic_code = "ORH_CPU_CACHE_LAYOUT.OK";
  result.diagnostic_detail = "runtime layout observability accepted";
  result.row_count = static_cast<std::uint64_t>(request.row_page->rows.size());
  for (const auto& row : request.row_page->rows) {
    result.row_width_bytes += RowWidth(row);
    result.tuple_decode_cost_units += static_cast<std::uint64_t>(row.cells.size());
    for (const auto& cell : row.cells) {
      result.tuple_decode_cost_units += CellPayloadBytes(cell) / 8U;
    }
  }
  result.row_width_bytes /= std::max<std::uint64_t>(1, result.row_count);
  result.batch_width =
      request.executor_batch_result->evidence.resource_counters
          .max_observed_batch_rows;
  if (result.batch_width == 0) {
    result.batch_width =
        static_cast<std::uint64_t>(request.executor_batch->rows.size());
  }
  result.tuple_decode_cost_units += BatchMaterializedCells(*request.executor_batch);
  result.visibility_branch_proxy =
      result.row_count +
      static_cast<std::uint64_t>(request.physical_result->locators.size()) +
      static_cast<std::uint64_t>(request.hash_probe->locators.size());
  result.cache_miss_proxy =
      btree_report.report.reachable_leaf_count +
      request.hash_probe->pages_traversed +
      request.hash_probe->collision_entries_traversed +
      request.hash_probe->fingerprint_mismatch_count;
  result.route_identity = Hash({
      idx::IndexRouteKindName(request.route),
      idx::IndexFamilyName(request.expected_family),
      capability->route_diagnostic_code,
  });
  result.physical_btree_identity = BtreeIdentity(*request.btree_tree);
  result.physical_hash_identity = HashIdentity(*request.hash_index);
  result.storage_layout_observed = true;
  result.btree_layout_observed = true;
  result.hash_layout_observed = true;
  result.executor_layout_observed = true;
  result.mga_security_evidence = true;
  result.metrics_authority = false;

  Add(&result, "route_label", request.route_label);
  Add(&result, "runtime_consumed", "true");
  Add(&result, "route_kind", idx::IndexRouteKindName(request.route));
  Add(&result, "physical_index_family", idx::IndexFamilyName(request.expected_family));
  Add(&result, "route_identity", result.route_identity);
  Add(&result, "storage_page_layout_observed", "true");
  Add(&result, "row_count", std::to_string(result.row_count));
  Add(&result, "row_width_bytes", std::to_string(result.row_width_bytes));
  Add(&result,
      "tuple_decode_cost_units",
      std::to_string(result.tuple_decode_cost_units));
  Add(&result,
      "visibility_branch_proxy",
      std::to_string(result.visibility_branch_proxy));
  Add(&result, "cache_miss_proxy", std::to_string(result.cache_miss_proxy));
  Add(&result, "batch_width", std::to_string(result.batch_width));
  Add(&result, "physical_btree_identity", result.physical_btree_identity);
  Add(&result, "physical_hash_identity", result.physical_hash_identity);
  Add(&result, "btree_report_valid", "true");
  Add(&result, "hash_report_valid", "true");
  Add(&result, "executor_batch_layout_observed", "true");
  Add(&result, "mga_visibility_authority", "engine_transaction_inventory");
  Add(&result, "security_authority", "engine_security_recheck");
  Add(&result, "layout_metrics_visibility_authority", "false");
  Add(&result, "layout_metrics_finality_authority", "false");
  Add(&result, "layout_metrics_security_authority", "false");
  Add(&result, "layout_metrics_recovery_authority", "false");
  Add(&result, "optimization_authority", "false");
  Add(&result, "benchmark_clean", "true");
  return result;
}

}  // namespace scratchbird::engine::executor
