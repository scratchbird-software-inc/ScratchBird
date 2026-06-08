// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "indexed_physical_operator.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
using scratchbird::engine::internal_api::EngineEvidenceReference;

void AddEvidence(IndexedPhysicalOperatorResult* result,
                 std::string kind,
                 std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

IndexedPhysicalOperatorResult Fail(std::string code, std::string detail) {
  IndexedPhysicalOperatorResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  AddEvidence(&result, "indexed_physical_operator_refusal", result.diagnostic_detail);
  AddEvidence(&result, "runtime_route_capability", "false");
  AddEvidence(&result, "index_benchmark_clean", "false");
  AddEvidence(&result, "mga_finality_authority", "engine_transaction_inventory");
  AddEvidence(&result, "parser_or_donor_authority", "false");
  AddEvidence(&result, "index_or_cache_finality_authority", "false");
  return result;
}

std::string TypedUuidText(const platform::TypedUuid& typed) {
  return typed.valid() ? uuid::UuidToString(typed.value) : std::string{};
}

bool HasBoundKey(const page::IndexBtreePhysicalScanBound& bound) {
  return bound.unbounded || !bound.encoded_key.empty();
}

bool LocatorPreservesRechecks(const page::IndexBtreePhysicalRowLocator& locator) {
  return locator.mga_recheck_required &&
         locator.security_recheck_required &&
         !locator.visibility_authority &&
         !locator.authorization_authority &&
         !locator.transaction_finality_authority &&
         !locator.recovery_authority;
}

IndexedPhysicalOperatorResult CheckBaseRequest(
    const IndexedPhysicalOperatorRequest& request) {
  if (!request.plan_safe) {
    return Fail("SB-IRC060-STALE-OR-UNSAFE-PLAN", "stale_or_unsafe_plan");
  }
  if (!request.physical_tree_available) {
    return Fail("SB-IRC060-PHYSICAL-TREE-UNAVAILABLE",
                "physical_index_tree_available_required");
  }
  if (request.physical_tree == nullptr) {
    return Fail("SB-IRC060-PHYSICAL-TREE-REQUIRED",
                "physical_index_tree_required");
  }
  if (!request.durable_mga_inventory_proof) {
    return Fail("SB-IRC060-MGA-PROOF-REQUIRED",
                "durable_mga_inventory_proof_required");
  }
  if (!request.mga_visibility_recheck_planned) {
    return Fail("SB-IRC060-MGA-RECHECK-REQUIRED",
                "mga_visibility_recheck_required");
  }
  if (!request.security_recheck_planned) {
    return Fail("SB-IRC060-SECURITY-RECHECK-REQUIRED",
                "security_recheck_required");
  }
  if (request.parser_or_donor_authority) {
    return Fail("SB-IRC060-PARSER-DONOR-AUTHORITY-FORBIDDEN",
                "parser_or_donor_authority_forbidden");
  }
  if (request.index_or_cache_finality_authority) {
    return Fail("SB-IRC060-INDEX-FINALITY-AUTHORITY-FORBIDDEN",
                "index_or_cache_finality_authority_forbidden");
  }

  const auto validation = page::ValidateIndexBtreePhysicalTree(*request.physical_tree);
  if (!validation.ok()) {
    return Fail(validation.diagnostic.diagnostic_code.empty()
                    ? "SB-IRC060-UNSAFE-PHYSICAL-TREE"
                    : validation.diagnostic.diagnostic_code,
                "unsafe_physical_index_tree");
  }

  IndexedPhysicalOperatorResult ok;
  ok.ok = true;
  return ok;
}

void AddAcceptedCommonEvidence(const IndexedPhysicalOperatorRequest& request,
                               IndexedPhysicalOperatorResult* result) {
  result->runtime_route_capability = true;
  result->benchmark_clean = false;
  result->table_scan_consumed = false;
  AddEvidence(result, "indexed_physical_operator",
              IndexedPhysicalOperatorKindName(request.kind));
  AddEvidence(result, "indexed_physical_operator_physical_scan_consumed",
              "index_btree_physical_tree");
  AddEvidence(result, "indexed_physical_operator_no_table_scan",
              "physical_index_scan_output_consumed");
  AddEvidence(result, "runtime_route_capability", "true");
  AddEvidence(result, "index_benchmark_clean", "false");
  AddEvidence(result, "mga_visibility_recheck", "required");
  AddEvidence(result, "security_recheck", "required");
  AddEvidence(result, "mga_finality_authority", "engine_transaction_inventory");
  AddEvidence(result, "parser_or_donor_authority", "false");
  AddEvidence(result, "index_or_cache_finality_authority", "false");
}

IndexedPhysicalOperatorLocator CopyLocator(
    const page::IndexBtreePhysicalRowLocator& locator,
    std::uint64_t outer_ordinal = 0) {
  IndexedPhysicalOperatorLocator copied;
  copied.row_uuid = TypedUuidText(locator.row_uuid);
  copied.version_uuid = TypedUuidText(locator.version_uuid);
  copied.encoded_key = locator.encoded_key;
  copied.outer_ordinal = outer_ordinal;
  copied.leaf_page_number = locator.leaf_page_number;
  copied.cell_ordinal = locator.cell_ordinal;
  copied.from_physical_index = true;
  copied.mga_recheck_required = locator.mga_recheck_required;
  copied.security_recheck_required = locator.security_recheck_required;
  return copied;
}

IndexedPhysicalOperatorResult ConsumeScan(
    const IndexedPhysicalOperatorRequest& request,
    page::IndexBtreePhysicalScanResult scan,
    std::string_view scan_kind,
    std::uint64_t outer_ordinal = 0) {
  if (!scan.ok()) {
    return Fail(scan.diagnostic.diagnostic_code.empty()
                    ? "SB-IRC060-PHYSICAL-SCAN-REFUSED"
                    : scan.diagnostic.diagnostic_code,
                "physical_index_scan_refused");
  }

  IndexedPhysicalOperatorResult result;
  result.ok = true;
  result.diagnostic_code = "SB-IRC060-OK";
  result.diagnostic_detail = "indexed_physical_operator.ok";
  AddAcceptedCommonEvidence(request, &result);
  AddEvidence(&result, "indexed_physical_operator_scan_kind", std::string(scan_kind));
  AddEvidence(&result, "indexed_physical_operator_locator_count",
              std::to_string(scan.locators.size()));
  AddEvidence(&result, "indexed_physical_operator_visited_leaf_pages",
              std::to_string(scan.visited_leaf_pages));
  AddEvidence(&result, "indexed_physical_operator_pruned_leaf_pages",
              std::to_string(scan.pruned_leaf_pages));
  for (const auto& evidence : scan.evidence) {
    AddEvidence(&result, "index_btree_physical_scan_evidence", evidence);
  }

  for (const auto& locator : scan.locators) {
    if (!LocatorPreservesRechecks(locator)) {
      return Fail("SB-IRC060-LOCATOR-RECHECK-EVIDENCE-MISSING",
                  "physical_locator_missing_mga_or_security_recheck");
    }
    result.locators.push_back(CopyLocator(locator, outer_ordinal));
  }
  return result;
}

IndexedPhysicalOperatorResult ExecutePoint(
    const IndexedPhysicalOperatorRequest& request) {
  if (!request.encoded_key_proof || request.encoded_point_key.empty()) {
    return Fail("SB-IRC060-ENCODED-POINT-KEY-REQUIRED",
                "encoded_point_key_required");
  }
  return ConsumeScan(request,
                     page::PointLookupIndexBtreePhysicalTree(
                         *request.physical_tree, request.encoded_point_key),
                     "point_lookup");
}

IndexedPhysicalOperatorResult ExecuteRange(
    const IndexedPhysicalOperatorRequest& request) {
  if (!request.encoded_bounds_proof ||
      !HasBoundKey(request.lower_bound) ||
      !HasBoundKey(request.upper_bound)) {
    return Fail("SB-IRC060-ENCODED-RANGE-BOUNDS-REQUIRED",
                "encoded_range_bounds_required");
  }
  return ConsumeScan(request,
                     page::RangeScanIndexBtreePhysicalTree(
                         *request.physical_tree,
                         request.lower_bound,
                         request.upper_bound),
                     "range_scan");
}

IndexedPhysicalOperatorResult ExecuteOrderedLimit(
    const IndexedPhysicalOperatorRequest& request) {
  if (request.limit == 0) {
    return Fail("SB-IRC060-ORDERED-LIMIT-BOUND-REQUIRED",
                "ordered_limit_bound_required");
  }
  auto result = ConsumeScan(request,
                            page::OrderedScanIndexBtreePhysicalTree(
                                *request.physical_tree,
                                page::IndexBtreePhysicalScanOrdering::forward,
                                request.limit),
                            "ordered_limit");
  if (result.ok) {
    AddEvidence(&result, "ordered_limit_bound", std::to_string(request.limit));
    AddEvidence(&result,
                "ordered_limit_stopped_at_bound",
                result.locators.size() <= request.limit ? "true" : "false");
    if (result.locators.size() > request.limit) {
      return Fail("SB-IRC060-ORDERED-LIMIT-OVERFLOW",
                  "ordered_limit_exceeded_bound");
    }
  }
  return result;
}

IndexedPhysicalOperatorResult ExecuteNestedLoop(
    const IndexedPhysicalOperatorRequest& request) {
  if (request.outer_probes.empty()) {
    return Fail("SB-IRC060-OUTER-PROBES-REQUIRED", "outer_probe_keys_required");
  }

  IndexedPhysicalOperatorResult result;
  result.ok = true;
  result.diagnostic_code = "SB-IRC060-OK";
  result.diagnostic_detail = "indexed_physical_operator.ok";
  AddAcceptedCommonEvidence(request, &result);
  AddEvidence(&result, "indexed_nested_loop_physical_probe_count",
              std::to_string(request.outer_probes.size()));

  for (const auto& probe : request.outer_probes) {
    page::IndexBtreePhysicalScanResult scan;
    if (probe.range_probe) {
      if (!request.encoded_bounds_proof ||
          !HasBoundKey(probe.lower_bound) ||
          !HasBoundKey(probe.upper_bound)) {
        return Fail("SB-IRC060-NESTED-RANGE-BOUNDS-REQUIRED",
                    "nested_loop_encoded_range_bounds_required");
      }
      scan = page::RangeScanIndexBtreePhysicalTree(*request.physical_tree,
                                                   probe.lower_bound,
                                                   probe.upper_bound);
    } else {
      if (!request.encoded_key_proof || probe.encoded_key.empty()) {
        return Fail("SB-IRC060-NESTED-POINT-KEY-REQUIRED",
                    "nested_loop_encoded_point_key_required");
      }
      scan = page::PointLookupIndexBtreePhysicalTree(*request.physical_tree,
                                                     probe.encoded_key);
    }
    if (!scan.ok()) {
      return Fail(scan.diagnostic.diagnostic_code.empty()
                      ? "SB-IRC060-NESTED-PROBE-REFUSED"
                      : scan.diagnostic.diagnostic_code,
                  "indexed_nested_loop_physical_probe_refused");
    }
    AddEvidence(&result, "indexed_nested_loop_outer_ordinal",
                std::to_string(probe.outer_ordinal));
    AddEvidence(&result, "indexed_nested_loop_probe_locator_count",
                std::to_string(probe.outer_ordinal) + ":" +
                    std::to_string(scan.locators.size()));
    for (const auto& locator : scan.locators) {
      if (!LocatorPreservesRechecks(locator)) {
        return Fail("SB-IRC060-LOCATOR-RECHECK-EVIDENCE-MISSING",
                    "physical_locator_missing_mga_or_security_recheck");
      }
      result.locators.push_back(CopyLocator(locator, probe.outer_ordinal));
    }
  }
  AddEvidence(&result, "indexed_physical_operator_locator_count",
              std::to_string(result.locators.size()));
  return result;
}

bool EncodedLess(const IndexedPhysicalOperatorLocator& left,
                 const IndexedPhysicalOperatorLocator& right) {
  return std::lexicographical_compare(left.encoded_key.begin(),
                                      left.encoded_key.end(),
                                      right.encoded_key.begin(),
                                      right.encoded_key.end());
}

bool EncodedEqual(const IndexedPhysicalOperatorLocator& left,
                  const IndexedPhysicalOperatorLocator& right) {
  return left.encoded_key == right.encoded_key;
}

IndexedPhysicalOperatorResult ExecuteMergeOrdered(
    const IndexedPhysicalOperatorRequest& request) {
  if (request.right_physical_tree == nullptr) {
    return Fail("SB-IRC060-RIGHT-PHYSICAL-TREE-REQUIRED",
                "right_physical_index_tree_required");
  }
  const auto right_validation =
      page::ValidateIndexBtreePhysicalTree(*request.right_physical_tree);
  if (!right_validation.ok()) {
    return Fail(right_validation.diagnostic.diagnostic_code.empty()
                    ? "SB-IRC060-UNSAFE-RIGHT-PHYSICAL-TREE"
                    : right_validation.diagnostic.diagnostic_code,
                "unsafe_right_physical_index_tree");
  }

  auto left = ConsumeScan(request,
                          page::OrderedScanIndexBtreePhysicalTree(
                              *request.physical_tree),
                          "merge_ordered_left");
  if (!left.ok) {
    return left;
  }
  auto right = ConsumeScan(request,
                           page::OrderedScanIndexBtreePhysicalTree(
                               *request.right_physical_tree),
                           "merge_ordered_right");
  if (!right.ok) {
    return right;
  }

  IndexedPhysicalOperatorResult result;
  result.ok = true;
  result.diagnostic_code = "SB-IRC060-OK";
  result.diagnostic_detail = "indexed_physical_operator.ok";
  AddAcceptedCommonEvidence(request, &result);
  AddEvidence(&result, "merge_ordered_input_left_count",
              std::to_string(left.locators.size()));
  AddEvidence(&result, "merge_ordered_input_right_count",
              std::to_string(right.locators.size()));
  for (const auto& evidence : left.evidence) {
    AddEvidence(&result,
                "merge_ordered_left_stream_evidence",
                evidence.evidence_kind + "=" + evidence.evidence_id);
  }
  for (const auto& evidence : right.evidence) {
    AddEvidence(&result,
                "merge_ordered_right_stream_evidence",
                evidence.evidence_kind + "=" + evidence.evidence_id);
  }

  std::size_t i = 0;
  std::size_t j = 0;
  while (i < left.locators.size() && j < right.locators.size()) {
    if (EncodedEqual(left.locators[i], right.locators[j])) {
      IndexedPhysicalMergePair pair;
      pair.left_ordinal = i;
      pair.right_ordinal = j;
      pair.left = left.locators[i];
      pair.right = right.locators[j];
      result.merge_pairs.push_back(std::move(pair));
      AddEvidence(&result, "merge_ordered_input_pair",
                  std::to_string(i) + ":" + std::to_string(j));
      ++i;
      ++j;
    } else if (EncodedLess(left.locators[i], right.locators[j])) {
      AddEvidence(&result, "merge_ordered_input_advance",
                  "left:" + std::to_string(i));
      ++i;
    } else {
      AddEvidence(&result, "merge_ordered_input_advance",
                  "right:" + std::to_string(j));
      ++j;
    }
  }
  while (i < left.locators.size()) {
    AddEvidence(&result, "merge_ordered_input_advance",
                "left:" + std::to_string(i++));
  }
  while (j < right.locators.size()) {
    AddEvidence(&result, "merge_ordered_input_advance",
                "right:" + std::to_string(j++));
  }
  AddEvidence(&result, "merge_ordered_input_pair_count",
              std::to_string(result.merge_pairs.size()));
  return result;
}

IndexedPhysicalOperatorResult ExecuteRuntimeFilter(
    const IndexedPhysicalOperatorRequest& request) {
  if (request.runtime_filter_keys.empty()) {
    return Fail("SB-IRC060-RUNTIME-FILTER-KEYS-REQUIRED",
                "runtime_filter_encoded_keys_required");
  }
  if (!request.encoded_key_proof) {
    return Fail("SB-IRC060-RUNTIME-FILTER-ENCODED-KEY-PROOF-REQUIRED",
                "runtime_filter_encoded_key_proof_required");
  }
  auto scanned = ConsumeScan(request,
                             page::OrderedScanIndexBtreePhysicalTree(
                                 *request.physical_tree),
                             "runtime_filter_candidate_stream");
  if (!scanned.ok) {
    return scanned;
  }
  IndexedPhysicalOperatorResult result;
  result.ok = true;
  result.diagnostic_code = "SB-IRC060-OK";
  result.diagnostic_detail = "indexed_physical_operator.ok";
  AddAcceptedCommonEvidence(request, &result);
  AddEvidence(&result, "runtime_filter_candidate_count",
              std::to_string(scanned.locators.size()));
  AddEvidence(&result, "runtime_filter_exact_recheck",
              "mga_visibility_and_security_required_per_candidate");
  for (const auto& locator : scanned.locators) {
    const bool accepted =
        std::any_of(request.runtime_filter_keys.begin(),
                    request.runtime_filter_keys.end(),
                    [&](const auto& key) { return key == locator.encoded_key; });
    if (!accepted) {
      AddEvidence(&result, "runtime_filter_candidate_rejected", "encoded_key");
      continue;
    }
    result.locators.push_back(locator);
    AddEvidence(&result, "runtime_filter_candidate_accepted", "encoded_key");
  }
  AddEvidence(&result, "indexed_physical_operator_locator_count",
              std::to_string(result.locators.size()));
  return result;
}

}  // namespace

const char* IndexedPhysicalOperatorKindName(IndexedPhysicalOperatorKind kind) {
  switch (kind) {
    case IndexedPhysicalOperatorKind::point_lookup:
      return "point_lookup";
    case IndexedPhysicalOperatorKind::range_scan:
      return "range_scan";
    case IndexedPhysicalOperatorKind::ordered_limit:
      return "ordered_limit";
    case IndexedPhysicalOperatorKind::indexed_nested_loop:
      return "indexed_nested_loop";
    case IndexedPhysicalOperatorKind::merge_ordered_input:
      return "merge_ordered_input";
    case IndexedPhysicalOperatorKind::runtime_filter:
      return "runtime_filter";
  }
  return "unknown";
}

IndexedPhysicalOperatorResult ExecuteIndexedPhysicalOperator(
    const IndexedPhysicalOperatorRequest& request) {
  auto base = CheckBaseRequest(request);
  if (!base.ok) {
    return base;
  }

  switch (request.kind) {
    case IndexedPhysicalOperatorKind::point_lookup:
      return ExecutePoint(request);
    case IndexedPhysicalOperatorKind::range_scan:
      return ExecuteRange(request);
    case IndexedPhysicalOperatorKind::ordered_limit:
      return ExecuteOrderedLimit(request);
    case IndexedPhysicalOperatorKind::indexed_nested_loop:
      return ExecuteNestedLoop(request);
    case IndexedPhysicalOperatorKind::merge_ordered_input:
      return ExecuteMergeOrdered(request);
    case IndexedPhysicalOperatorKind::runtime_filter:
      return ExecuteRuntimeFilter(request);
  }
  return Fail("SB-IRC060-OPERATOR-UNSUPPORTED", "indexed_operator_unsupported");
}

}  // namespace scratchbird::engine::executor
