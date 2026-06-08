// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_row_locator_stream.hpp"

#include "api_diagnostics.hpp"
#include "uuid.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic Refusal(std::string_view reason) {
  return MakeEngineApiDiagnostic("SB-DML-ROW-LOCATOR-STREAM-REFUSED",
                                 "dml.row_locator_stream.refused",
                                 std::string(reason),
                                 true);
}

EngineApiDiagnostic PhysicalScanDiagnostic(
    const platform::DiagnosticRecord& diagnostic,
    std::string_view fallback_detail) {
  return MakeEngineApiDiagnostic(
      diagnostic.diagnostic_code.empty()
          ? "SB-DML-ROW-LOCATOR-PHYSICAL-SCAN-REFUSED"
          : diagnostic.diagnostic_code,
      diagnostic.message_key.empty()
          ? "dml.row_locator_stream.physical_scan_refused"
          : diagnostic.message_key,
      diagnostic.remediation_hint.empty() ? std::string(fallback_detail)
                                          : diagnostic.remediation_hint,
      true);
}

void AddEvidence(DmlRowLocatorStreamResult* result,
                 std::string kind,
                 std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

DmlRowLocatorStreamResult Fail(std::string_view reason,
                               DmlRowLocatorStreamResult result = {}) {
  result.ok = false;
  result.source = DmlRowLocatorStreamSource::refused;
  result.diagnostic = Refusal(reason);
  AddEvidence(&result, "dml_row_locator_stream_refusal", std::string(reason));
  AddEvidence(&result, "runtime_route_capability", "false");
  AddEvidence(&result, "index_benchmark_clean", "false");
  return result;
}

std::string TypedUuidText(const platform::TypedUuid& typed) {
  return typed.valid() ? uuid::UuidToString(typed.value) : std::string{};
}

bool AccessPlanIsRowUuid(const DmlTargetAccessPlan& plan) {
  return plan.access_kind == DmlTargetAccessKind::row_uuid_singleton ||
         plan.access_kind == DmlTargetAccessKind::row_uuid_list;
}

bool AccessPlanIsIndexBacked(const DmlTargetAccessPlan& plan) {
  return plan.access_kind == DmlTargetAccessKind::unique_index_lookup ||
         plan.access_kind == DmlTargetAccessKind::nonunique_index_lookup ||
         plan.access_kind == DmlTargetAccessKind::range_index_lookup;
}

bool ConsumerRequiresUniquePoint(DmlRowLocatorStreamConsumer consumer) {
  return consumer == DmlRowLocatorStreamConsumer::on_conflict;
}

DmlRowLocatorStreamSource SourceForPlan(const DmlTargetAccessPlan& plan,
                                        bool index_unique) {
  switch (plan.access_kind) {
    case DmlTargetAccessKind::row_uuid_singleton:
      return DmlRowLocatorStreamSource::row_uuid_singleton;
    case DmlTargetAccessKind::row_uuid_list:
      return DmlRowLocatorStreamSource::row_uuid_list;
    case DmlTargetAccessKind::unique_index_lookup:
      return DmlRowLocatorStreamSource::physical_unique_btree_point;
    case DmlTargetAccessKind::nonunique_index_lookup:
      return index_unique ? DmlRowLocatorStreamSource::physical_unique_btree_point
                          : DmlRowLocatorStreamSource::physical_btree_point;
    case DmlTargetAccessKind::range_index_lookup:
      return DmlRowLocatorStreamSource::physical_btree_range;
    case DmlTargetAccessKind::table_scan:
      return DmlRowLocatorStreamSource::table_scan_fallback;
    case DmlTargetAccessKind::refused:
    case DmlTargetAccessKind::summary_pruned:
      return DmlRowLocatorStreamSource::refused;
  }
  return DmlRowLocatorStreamSource::refused;
}

void AddCommonAcceptedEvidence(const DmlRowLocatorStreamRequest& request,
                               DmlRowLocatorStreamResult* result) {
  AddEvidence(result, "dml_row_locator_stream_consumer",
              DmlRowLocatorStreamConsumerName(request.consumer));
  AddEvidence(result, "dml_row_locator_stream_source",
              DmlRowLocatorStreamSourceName(result->source));
  AddEvidence(result, "dml_target_access_kind",
              DmlTargetAccessKindName(request.access_plan.access_kind));
  AddEvidence(result, "mga_visibility_recheck", "required");
  AddEvidence(result, "security_recheck", "required");
  AddEvidence(result, "mga_finality_authority",
              "engine_transaction_inventory");
  AddEvidence(result, "parser_or_donor_authority", "false");
  AddEvidence(result, "index_or_cache_finality_authority", "false");
  AddEvidence(result, "runtime_route_capability", "false");
  AddEvidence(result, "index_benchmark_clean", "false");
  for (const auto& evidence : request.access_plan.evidence) {
    AddEvidence(result, "dml_target_access_plan_evidence", evidence);
  }
}

void AddMergeOrdinalEvidence(const DmlRowLocatorStreamRequest& request,
                             DmlRowLocatorStreamResult* result) {
  if (request.consumer != DmlRowLocatorStreamConsumer::merge) {
    return;
  }
  for (const auto& ordinal : request.merge_ordinals) {
    AddEvidence(result,
                "merge_locator_stream_source_action_order",
                std::to_string(ordinal.source_ordinal) + ":" +
                    std::to_string(ordinal.action_ordinal) + ":" +
                    (ordinal.matched ? "matched" : "unmatched"));
  }
}

void AddPhysicalScanEvidence(const page::IndexBtreePhysicalScanResult& scan,
                             DmlRowLocatorStreamResult* result) {
  AddEvidence(result,
              "dml_row_locator_stream_physical_scan",
              "index_btree_physical_tree");
  AddEvidence(result,
              "dml_row_locator_stream_no_table_scan",
              "physical_index_locator_stream_consumed");
  AddEvidence(result,
              "dml_row_locator_stream_locator_count",
              std::to_string(scan.locators.size()));
  AddEvidence(result,
              "dml_row_locator_stream_visited_leaf_pages",
              std::to_string(scan.visited_leaf_pages));
  AddEvidence(result,
              "dml_row_locator_stream_pruned_leaf_pages",
              std::to_string(scan.pruned_leaf_pages));
  for (const auto& evidence : scan.evidence) {
    AddEvidence(result, "index_btree_locator_stream_evidence", evidence);
  }
}

void CopyPhysicalLocators(const DmlRowLocatorStreamRequest& request,
                          const page::IndexBtreePhysicalScanResult& scan,
                          DmlRowLocatorStreamResult* result) {
  for (const auto& locator : scan.locators) {
    DmlRowLocator row;
    row.row_uuid = TypedUuidText(locator.row_uuid);
    row.version_uuid = TypedUuidText(locator.version_uuid);
    row.index_uuid = request.access_plan.index_uuid;
    row.leaf_page_number = locator.leaf_page_number;
    row.cell_ordinal = locator.cell_ordinal;
    row.from_physical_index = true;
    row.mga_recheck_required = locator.mga_recheck_required;
    row.security_recheck_required = locator.security_recheck_required;
    result->locators.push_back(std::move(row));
  }
}

DmlRowLocatorStreamResult BuildPhysicalStream(
    const DmlRowLocatorStreamRequest& request,
    DmlRowLocatorStreamSource source,
    DmlRowLocatorStreamResult result) {
  if (request.physical_tree == nullptr) {
    return Fail("physical_index_tree_required", std::move(result));
  }
  if (ConsumerRequiresUniquePoint(request.consumer) &&
      source != DmlRowLocatorStreamSource::physical_unique_btree_point) {
    return Fail("on_conflict_requires_unique_index_locator_stream",
                std::move(result));
  }

  page::IndexBtreePhysicalScanResult scan;
  if (source == DmlRowLocatorStreamSource::physical_btree_range) {
    scan = page::RangeScanIndexBtreePhysicalTree(*request.physical_tree,
                                                 request.lower_bound,
                                                 request.upper_bound);
  } else {
    if (request.encoded_point_key.empty()) {
      return Fail("encoded_point_key_required", std::move(result));
    }
    scan = page::PointLookupIndexBtreePhysicalTree(*request.physical_tree,
                                                  request.encoded_point_key);
  }
  if (!scan.ok()) {
    result.ok = false;
    result.source = DmlRowLocatorStreamSource::refused;
    result.diagnostic = PhysicalScanDiagnostic(scan.diagnostic,
                                               "physical_locator_scan_refused");
    AddEvidence(&result,
                "dml_row_locator_stream_refusal",
                "physical_locator_scan_refused");
    return result;
  }

  result.ok = true;
  result.source = source;
  result.diagnostic = OkDiagnostic();
  CopyPhysicalLocators(request, scan, &result);
  AddCommonAcceptedEvidence(request, &result);
  AddPhysicalScanEvidence(scan, &result);
  if (source == DmlRowLocatorStreamSource::physical_unique_btree_point) {
    AddEvidence(&result,
                "on_conflict_unique_locator_stream",
                request.consumer == DmlRowLocatorStreamConsumer::on_conflict
                    ? "consumed_no_table_scan"
                    : "available");
  }
  AddMergeOrdinalEvidence(request, &result);
  return result;
}

DmlRowLocatorStreamResult BuildRowUuidStream(
    const DmlRowLocatorStreamRequest& request,
    DmlRowLocatorStreamSource source,
    DmlRowLocatorStreamResult result) {
  result.ok = true;
  result.source = source;
  result.diagnostic = OkDiagnostic();
  if (source == DmlRowLocatorStreamSource::row_uuid_singleton) {
    DmlRowLocator locator;
    locator.row_uuid = request.access_plan.row_uuid;
    result.locators.push_back(std::move(locator));
  } else {
    for (const auto& row_uuid : request.access_plan.row_uuids) {
      DmlRowLocator locator;
      locator.row_uuid = row_uuid;
      result.locators.push_back(std::move(locator));
    }
  }
  AddCommonAcceptedEvidence(request, &result);
  AddEvidence(&result,
              "dml_row_locator_stream_no_table_scan",
              "explicit_row_uuid_locator_stream_consumed");
  AddEvidence(&result,
              "dml_row_locator_stream_locator_count",
              std::to_string(result.locators.size()));
  AddMergeOrdinalEvidence(request, &result);
  return result;
}

}  // namespace

const char* DmlRowLocatorStreamConsumerName(DmlRowLocatorStreamConsumer consumer) {
  switch (consumer) {
    case DmlRowLocatorStreamConsumer::on_conflict: return "on_conflict";
    case DmlRowLocatorStreamConsumer::merge: return "merge";
    case DmlRowLocatorStreamConsumer::update: return "update";
    case DmlRowLocatorStreamConsumer::delete_row: return "delete";
  }
  return "unknown";
}

const char* DmlRowLocatorStreamSourceName(DmlRowLocatorStreamSource source) {
  switch (source) {
    case DmlRowLocatorStreamSource::refused: return "refused";
    case DmlRowLocatorStreamSource::row_uuid_singleton: return "row_uuid_singleton";
    case DmlRowLocatorStreamSource::row_uuid_list: return "row_uuid_list";
    case DmlRowLocatorStreamSource::physical_unique_btree_point:
      return "physical_unique_btree_point";
    case DmlRowLocatorStreamSource::physical_btree_point:
      return "physical_btree_point";
    case DmlRowLocatorStreamSource::physical_btree_range:
      return "physical_btree_range";
    case DmlRowLocatorStreamSource::table_scan_fallback:
      return "table_scan_fallback";
  }
  return "refused";
}

DmlRowLocatorStreamResult BuildDmlRowLocatorStream(
    const DmlRowLocatorStreamRequest& request) {
  DmlRowLocatorStreamResult result;
  result.runtime_route_capability = false;
  result.benchmark_clean = false;

  if (!request.access_plan.ok) {
    for (const auto& diagnostic : request.access_plan.diagnostics) {
      AddEvidence(&result, "dml_target_access_plan_refusal", diagnostic);
    }
    return Fail("access_plan_not_safe", std::move(result));
  }
  if (!request.access_plan_engine_authority_proof) {
    return Fail("access_plan_engine_authority_proof_required",
                std::move(result));
  }
  if (!request.durable_mga_inventory_proof) {
    return Fail("durable_mga_inventory_proof_required", std::move(result));
  }
  if (!request.mga_visibility_recheck_planned) {
    return Fail("mga_visibility_recheck_required", std::move(result));
  }
  if (!request.security_recheck_planned) {
    return Fail("security_recheck_required", std::move(result));
  }
  if (request.parser_or_donor_authority) {
    return Fail("parser_or_donor_authority_forbidden", std::move(result));
  }
  if (request.index_or_cache_finality_authority) {
    return Fail("index_or_cache_finality_authority_forbidden",
                std::move(result));
  }

  const auto source = SourceForPlan(request.access_plan, request.index_unique);
  if (AccessPlanIsRowUuid(request.access_plan)) {
    if (ConsumerRequiresUniquePoint(request.consumer)) {
      return Fail("on_conflict_requires_unique_index_locator_stream",
                  std::move(result));
    }
    return BuildRowUuidStream(request, source, std::move(result));
  }
  if (AccessPlanIsIndexBacked(request.access_plan)) {
    return BuildPhysicalStream(request, source, std::move(result));
  }

  if (request.access_plan.access_kind == DmlTargetAccessKind::table_scan) {
    if (request.applicable_physical_index_exists) {
      return Fail("table_scan_fallback_refused_applicable_index_exists",
                  std::move(result));
    }
    if (!request.table_scan_fallback_allowed) {
      return Fail("table_scan_fallback_not_allowed", std::move(result));
    }
    result.ok = true;
    result.source = DmlRowLocatorStreamSource::table_scan_fallback;
    result.table_scan_fallback = true;
    result.diagnostic = OkDiagnostic();
    AddCommonAcceptedEvidence(request, &result);
    AddEvidence(&result,
                "dml_row_locator_stream_table_scan_fallback",
                "allowed_no_applicable_row_uuid_or_physical_index_locator");
    AddMergeOrdinalEvidence(request, &result);
    return result;
  }

  return Fail("locator_stream_source_unsupported", std::move(result));
}

}  // namespace scratchbird::engine::internal_api
