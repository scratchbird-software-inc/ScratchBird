// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/direct_bulk_ordered_ingest.hpp"

#include "api_diagnostics.hpp"
#include "bulk_placement_order.hpp"
#include "ordered_ingest.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace scratchbird::engine::internal_api::dml::detail {

// SEARCH_KEY: SB_ENGINE_DIRECT_BULK_ORDERED_INGEST_AUTHORITY
// Placement planning only; the caller remains responsible for publication.

namespace {

std::string DirectOptionValue(const DirectPhysicalBulkAppendRequest& request,
                              const std::string& key) {
  const std::string equals_prefix = key + "=";
  const std::string colon_prefix = key + ":";
  for (const auto& candidate : request.option_envelopes) {
    if (candidate.rfind(equals_prefix, 0) == 0) {
      return candidate.substr(equals_prefix.size());
    }
    if (candidate.rfind(colon_prefix, 0) == 0) {
      return candidate.substr(colon_prefix.size());
    }
  }
  return {};
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

bool IsDirectTruthyValue(const std::string& value) {
  const std::string lowered = LowerAscii(value);
  return lowered == "1" || lowered == "true" || lowered == "enabled" ||
         lowered == "on" || lowered == "required";
}

bool DirectOptionEnabled(const DirectPhysicalBulkAppendRequest& request,
                         const std::string& option) {
  for (const auto& candidate : request.option_envelopes) {
    if (candidate == option) {
      return true;
    }
    const auto equals = option.find('=');
    if (equals != std::string::npos &&
        candidate == option.substr(0, equals) + ":" +
                         option.substr(equals + 1)) {
      return true;
    }
  }
  return false;
}

std::uint64_t DirectOptionU64(const DirectPhysicalBulkAppendRequest& request,
                              const std::string& key,
                              std::uint64_t fallback) {
  const std::string value = DirectOptionValue(request, key);
  if (value.empty()) {
    return fallback;
  }
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') {
      return fallback;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return fallback;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed == 0 ? fallback : parsed;
}

}  // namespace

bool DirectOrderedIngestRequested(const DirectPhysicalBulkAppendRequest& request) {
  const std::string primary = DirectOptionValue(request, "ordered_ingest");
  if (!primary.empty()) {
    return IsDirectTruthyValue(primary);
  }
  const std::string odf = DirectOptionValue(request, "odf047.ordered_ingest");
  if (!odf.empty()) {
    return IsDirectTruthyValue(odf);
  }
  return false;
}

bool DirectOrderedIngestDeriveForLargeLoad(
    const DirectPhysicalBulkAppendRequest& request) {
  const std::string primary =
      DirectOptionValue(request, "ordered_ingest.derive_for_large_load");
  if (!primary.empty()) {
    return IsDirectTruthyValue(primary);
  }
  const std::string odf =
      DirectOptionValue(request, "odf047.derive_order_for_large_load");
  if (!odf.empty()) {
    return IsDirectTruthyValue(odf);
  }
  return false;
}

std::string DirectOrderedPlacementKeyColumn(
    const DirectPhysicalBulkAppendRequest& request) {
  std::string key = DirectOptionValue(request, "ordered_ingest.placement_key");
  if (!key.empty()) {
    return key;
  }
  key = DirectOptionValue(request, "placement_key");
  if (!key.empty()) {
    return key;
  }
  return DirectOptionValue(request, "odf047.placement_key");
}

bool DirectPhysicalClusteringRequested(
    const DirectPhysicalBulkAppendRequest& request) {
  const std::string primary = DirectOptionValue(request, "physical_clustering");
  if (!primary.empty()) {
    return IsDirectTruthyValue(primary);
  }
  const std::string enabled =
      DirectOptionValue(request, "physical_clustering.enabled");
  if (!enabled.empty()) {
    return IsDirectTruthyValue(enabled);
  }
  return false;
}

std::string DirectPhysicalClusteringKeyColumn(
    const DirectPhysicalBulkAppendRequest& request,
    const std::string& placement_key_column) {
  std::string key = DirectOptionValue(request, "physical_clustering.key");
  if (!key.empty()) {
    return key;
  }
  key = DirectOptionValue(request, "physical_clustering.placement_key");
  if (!key.empty()) {
    return key;
  }
  return placement_key_column;
}

std::string DirectValueForColumn(
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& column_name) {
  for (const auto& [name, value] : values) {
    if (name == column_name) {
      return value;
    }
  }
  return {};
}

template <typename T>
std::vector<T> ApplySourceOrdinalPermutation(
    const std::vector<T>& source,
    const std::vector<std::uint64_t>& ordinals) {
  std::vector<T> reordered;
  reordered.reserve(source.size());
  for (const auto ordinal : ordinals) {
    if (ordinal < source.size()) {
      reordered.push_back(source[static_cast<std::size_t>(ordinal)]);
    }
  }
  return reordered;
}

DirectOrderedIngestSelection ApplyDirectOrderedIngestPlan(
    const DirectPhysicalBulkAppendRequest& request,
    std::vector<CrudRowVersionRecord>* staged_rows,
    std::vector<std::vector<std::pair<std::string, std::string>>>* logical_value_batch) {
  DirectOrderedIngestSelection selection;
  if (staged_rows == nullptr || logical_value_batch == nullptr ||
      staged_rows->size() != logical_value_batch->size()) {
    selection.ok = false;
    selection.failure_reason = "ordered_ingest_batch_shape_invalid";
    selection.diagnostic = MakeInvalidRequestDiagnostic(
        "dml.direct_physical_bulk_append",
        selection.failure_reason);
    return selection;
  }

  const bool ordered_ingest_requested = DirectOrderedIngestRequested(request);
  const bool derive_for_large_load =
      DirectOrderedIngestDeriveForLargeLoad(request);
  const std::uint64_t large_load_threshold =
      DirectOptionU64(request, "ordered_ingest.large_load_threshold", 1024);
  const bool physical_clustering_requested =
      DirectPhysicalClusteringRequested(request);
  const bool large_load =
      large_load_threshold != 0 &&
      staged_rows->size() >= large_load_threshold;
  if (!ordered_ingest_requested &&
      !(derive_for_large_load && large_load) &&
      !physical_clustering_requested) {
    selection.evidence.push_back({"bulk_placement_order_planner",
                                  "engine_optimizer"});
    selection.evidence.push_back({"bulk_placement_order_requested", "false"});
    selection.evidence.push_back(
        {"bulk_placement_order_large_load_threshold",
         std::to_string(large_load_threshold)});
    selection.evidence.push_back({"bulk_placement_order_input_rows",
                                  std::to_string(staged_rows->size())});
    selection.evidence.push_back({"bulk_placement_order_selected", "false"});
    selection.evidence.push_back({"ordered_ingest_storage_policy",
                                  "storage_page"});
    selection.evidence.push_back({"ordered_ingest_selected", "false"});
    selection.evidence.push_back(
        {"ordered_ingest_physical_clustering_requested", "false"});
    selection.evidence.push_back(
        {"ordered_ingest_physical_clustering",
         "not_requested_descriptor_unchanged"});
    return selection;
  }

  const std::string placement_key_column =
      DirectOrderedPlacementKeyColumn(request);
  scratchbird::engine::optimizer::BulkPlacementOrderRequest plan_request;
  plan_request.ordered_ingest_requested = ordered_ingest_requested;
  plan_request.derive_for_large_load = derive_for_large_load;
  plan_request.large_load_row_threshold = large_load_threshold;
  plan_request.placement_key_column = placement_key_column;
  plan_request.rows.reserve(staged_rows->size());
  for (std::size_t index = 0; index < staged_rows->size(); ++index) {
    scratchbird::engine::optimizer::BulkPlacementOrderRow row;
    row.source_ordinal = static_cast<std::uint64_t>(index);
    row.row_uuid = (*staged_rows)[index].row_uuid;
    row.placement_key =
        DirectValueForColumn((*logical_value_batch)[index], placement_key_column);
    plan_request.rows.push_back(std::move(row));
  }

  const auto plan =
      scratchbird::engine::optimizer::PlanBulkPlacementOrder(plan_request);
  for (const auto& item : plan.evidence) {
    selection.evidence.push_back({item.first, item.second});
  }
  if (!plan.ok) {
    selection.ok = false;
    selection.failure_reason = plan.diagnostic_code.empty()
                                   ? "ordered_ingest_refused"
                                   : plan.diagnostic_code;
    selection.diagnostic = MakeInvalidRequestDiagnostic(
        "dml.direct_physical_bulk_append",
        selection.failure_reason);
    return selection;
  }

  scratchbird::storage::page::OrderedIngestPhysicalClusteringRequest clustering;
  clustering.current_descriptor.relation_uuid =
      request.target_table.uuid.canonical;
  clustering.current_descriptor.placement_key_column =
      DirectOptionValue(request, "physical_clustering.current_key");
  clustering.current_descriptor.policy_uuid =
      DirectOptionValue(request, "physical_clustering.current_policy_uuid");
  clustering.current_descriptor.descriptor_generation =
      DirectOptionU64(request, "physical_clustering.current_generation", 0);
  clustering.current_descriptor.physical_clustering_enabled =
      !clustering.current_descriptor.placement_key_column.empty();
  clustering.requested_placement_key_column =
      DirectPhysicalClusteringKeyColumn(request, placement_key_column);
  clustering.requested_policy_uuid =
      DirectOptionValue(request, "physical_clustering.policy_uuid");
  clustering.ordered_ingest_selected = plan.ordered_ingest_selected;
  clustering.physical_clustering_requested = physical_clustering_requested;
  clustering.explicit_policy_present =
      DirectOptionEnabled(request, "physical_clustering.policy=explicit") ||
      !clustering.requested_policy_uuid.empty();
  clustering.allow_clustering_key_change =
      IsDirectTruthyValue(DirectOptionValue(request,
                                            "physical_clustering.allow_key_change"));
  const auto clustering_result =
      scratchbird::storage::page::ResolveOrderedIngestPhysicalClustering(
          clustering);
  for (const auto& item : clustering_result.evidence) {
    selection.evidence.push_back({item.first, item.second});
  }
  if (!clustering_result.ok) {
    selection.ok = false;
    selection.failure_reason = clustering_result.diagnostic_detail.empty()
                                   ? "physical_clustering_policy_refused"
                                   : clustering_result.diagnostic_detail;
    selection.diagnostic = MakeEngineApiDiagnostic(
        clustering_result.diagnostic_code.empty()
            ? "SB_ENGINE_API_INVALID_REQUEST"
            : clustering_result.diagnostic_code,
        "storage.ordered_ingest.physical_clustering_refused",
        selection.failure_reason,
        true);
    return selection;
  }

  selection.selected = plan.ordered_ingest_selected;
  if (plan.ordered_ingest_selected &&
      plan.source_ordinals_in_apply_order.size() == staged_rows->size()) {
    *staged_rows = ApplySourceOrdinalPermutation(
        *staged_rows,
        plan.source_ordinals_in_apply_order);
    *logical_value_batch = ApplySourceOrdinalPermutation(
        *logical_value_batch,
        plan.source_ordinals_in_apply_order);
    selection.evidence.push_back({"ordered_ingest_apply_order",
                                  "placement_key"});
    selection.evidence.push_back({"ordered_ingest_applied_rows",
                                  std::to_string(staged_rows->size())});
  }
  return selection;
}

}  // namespace scratchbird::engine::internal_api::dml::detail
