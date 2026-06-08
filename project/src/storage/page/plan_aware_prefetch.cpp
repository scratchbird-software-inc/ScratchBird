// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "plan_aware_prefetch.hpp"

#include <limits>
#include <string>

namespace scratchbird::storage::page {
namespace platform = scratchbird::core::platform;

namespace {

Status PrefetchOkStatus() {
  return {platform::StatusCode::ok, platform::Severity::info,
          platform::Subsystem::storage_page};
}

Status PrefetchErrorStatus() {
  return {platform::StatusCode::platform_required_feature_missing,
          platform::Severity::error, platform::Subsystem::storage_page};
}

bool AddWouldOverflow(u64 lhs, u64 rhs) {
  return lhs > std::numeric_limits<u64>::max() - rhs;
}

void AddEvidenceCounters(PlanAwarePrefetchResult* result) {
  result->evidence.push_back("plan_aware_prefetch.considered_items=" +
                             std::to_string(result->counters.considered_items));
  result->evidence.push_back("plan_aware_prefetch.scheduled_items=" +
                             std::to_string(result->counters.scheduled_items));
  result->evidence.push_back("plan_aware_prefetch.scheduled_heap_pages=" +
                             std::to_string(result->counters.scheduled_heap_pages));
  result->evidence.push_back("plan_aware_prefetch.scheduled_large_payloads=" +
                             std::to_string(result->counters.scheduled_large_payloads));
  result->evidence.push_back(
      "plan_aware_prefetch.scheduled_vector_payload_pages=" +
      std::to_string(result->counters.scheduled_vector_payload_pages));
  result->evidence.push_back(
      "plan_aware_prefetch.scheduled_graph_adjacency_pages=" +
      std::to_string(result->counters.scheduled_graph_adjacency_pages));
  result->evidence.push_back(
      "plan_aware_prefetch.scheduled_time_series_buckets=" +
      std::to_string(result->counters.scheduled_time_series_buckets));
  result->evidence.push_back(
      "plan_aware_prefetch.scheduled_extent_metadata=" +
      std::to_string(result->counters.scheduled_extent_metadata));
  result->evidence.push_back("plan_aware_prefetch.skipped_items=" +
                             std::to_string(result->counters.skipped_items));
  result->evidence.push_back("plan_aware_prefetch.refused_items=" +
                             std::to_string(result->counters.refused_items));
  result->evidence.push_back("plan_aware_prefetch.used_bytes=" +
                             std::to_string(result->counters.used_bytes));
  result->evidence.push_back("plan_aware_prefetch.used_pages=" +
                             std::to_string(result->counters.used_pages));
  result->evidence.push_back(
      std::string("plan_aware_prefetch.budget_exhausted=") +
      (result->counters.budget_exhausted ? "true" : "false"));
  result->evidence.push_back(std::string("plan_aware_prefetch.cancelled=") +
                             (result->counters.cancelled ? "true" : "false"));
  result->evidence.push_back(
      "plan_aware_prefetch.max_outstanding_observed=" +
      std::to_string(result->counters.max_outstanding_observed));
  result->evidence.push_back(
      "plan_aware_prefetch.diagnostic_only_authority=true");
  result->evidence.push_back("plan_aware_prefetch.finality_authority=false");
  result->evidence.push_back("plan_aware_prefetch.visibility_authority=false");
  result->evidence.push_back("plan_aware_prefetch.security_authority=false");
  result->evidence.push_back(
      "plan_aware_prefetch.mga_visibility_authority=false");
}

PlanAwarePrefetchResult Refuse(const PlanAwarePrefetchRequest& request,
                               std::string diagnostic_code,
                               std::string message_key,
                               std::string detail,
                               std::string item_id = {}) {
  PlanAwarePrefetchResult result;
  result.status = PrefetchErrorStatus();
  result.fail_closed = true;
  result.counters.refused_items =
      item_id.empty() ? request.descriptors.size() : 1;
  if (!item_id.empty()) {
    result.refused_item_ids.push_back(item_id);
  }
  result.evidence.push_back("plan_aware_prefetch.fail_closed=true");
  result.evidence.push_back("plan_aware_prefetch.refused=" + diagnostic_code);
  if (!request.physical_plan_id.empty()) {
    result.evidence.push_back("plan_aware_prefetch.physical_plan_source=" +
                              request.physical_plan_id);
  }
  AddEvidenceCounters(&result);
  result.diagnostic = MakePlanAwarePrefetchDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  return result;
}

bool IsSupportedFamily(PlanAwarePrefetchFamily family) {
  switch (family) {
    case PlanAwarePrefetchFamily::kHeapPage:
    case PlanAwarePrefetchFamily::kLargePayload:
    case PlanAwarePrefetchFamily::kVectorPayloadPage:
    case PlanAwarePrefetchFamily::kGraphAdjacencyPage:
    case PlanAwarePrefetchFamily::kTimeSeriesBucket:
    case PlanAwarePrefetchFamily::kExtentMetadata:
      return true;
    case PlanAwarePrefetchFamily::kUnsupported:
      return false;
  }
  return false;
}

bool RequiresLateMaterializationProof(
    const PlanAwarePrefetchDescriptor& descriptor) {
  return descriptor.full_payload_prefetch;
}

void CountScheduledFamily(PlanAwarePrefetchCounters* counters,
                          PlanAwarePrefetchFamily family) {
  switch (family) {
    case PlanAwarePrefetchFamily::kHeapPage:
      ++counters->scheduled_heap_pages;
      break;
    case PlanAwarePrefetchFamily::kLargePayload:
      ++counters->scheduled_large_payloads;
      break;
    case PlanAwarePrefetchFamily::kVectorPayloadPage:
      ++counters->scheduled_vector_payload_pages;
      break;
    case PlanAwarePrefetchFamily::kGraphAdjacencyPage:
      ++counters->scheduled_graph_adjacency_pages;
      break;
    case PlanAwarePrefetchFamily::kTimeSeriesBucket:
      ++counters->scheduled_time_series_buckets;
      break;
    case PlanAwarePrefetchFamily::kExtentMetadata:
      ++counters->scheduled_extent_metadata;
      break;
    case PlanAwarePrefetchFamily::kUnsupported:
      break;
  }
}

}  // namespace

const char* PlanAwarePrefetchFamilyName(PlanAwarePrefetchFamily family) {
  switch (family) {
    case PlanAwarePrefetchFamily::kHeapPage:
      return "heap_page";
    case PlanAwarePrefetchFamily::kLargePayload:
      return "large_payload";
    case PlanAwarePrefetchFamily::kVectorPayloadPage:
      return "vector_payload_page";
    case PlanAwarePrefetchFamily::kGraphAdjacencyPage:
      return "graph_adjacency_page";
    case PlanAwarePrefetchFamily::kTimeSeriesBucket:
      return "time_series_bucket";
    case PlanAwarePrefetchFamily::kExtentMetadata:
      return "extent_metadata";
    case PlanAwarePrefetchFamily::kUnsupported:
      return "unsupported";
  }
  return "unsupported";
}

PlanAwarePrefetchResult ExecutePlanAwarePrefetch(
    const PlanAwarePrefetchRequest& request) {
  if (request.physical_plan_id.empty() || request.physical_plan_generation == 0) {
    return Refuse(request, "plan_prefetch_physical_plan_identity_required",
                  "storage.page.plan_aware_prefetch.physical_plan_identity_required",
                  "physical plan identity and generation are required");
  }

  u64 total_byte_cost = 0;
  u64 total_page_cost = 0;
  for (const auto& descriptor : request.descriptors) {
    if (descriptor.item_id.empty() || descriptor.physical_plan_node_id.empty() ||
        descriptor.physical_plan_descriptor_digest.empty()) {
      return Refuse(request, "plan_prefetch_descriptor_identity_required",
                    "storage.page.plan_aware_prefetch.descriptor_identity_required",
                    "prefetch descriptor must be bound to a physical plan node",
                    descriptor.item_id);
    }
    if (!IsSupportedFamily(descriptor.family)) {
      return Refuse(request, "plan_prefetch_unsupported_family",
                    "storage.page.plan_aware_prefetch.unsupported_family",
                    "unsupported prefetch family", descriptor.item_id);
    }
    if (descriptor.physical_plan_generation !=
            request.physical_plan_generation ||
        descriptor.descriptor_generation != request.physical_plan_generation) {
      return Refuse(request, "plan_prefetch_stale_descriptor_generation",
                    "storage.page.plan_aware_prefetch.stale_descriptor_generation",
                    "prefetch descriptor generation does not match physical plan",
                    descriptor.item_id);
    }
    if (RequiresLateMaterializationProof(descriptor) &&
        (!descriptor.late_materialization_proof_present ||
         descriptor.late_materialization_source.empty())) {
      return Refuse(request, "plan_prefetch_late_materialization_proof_required",
                    "storage.page.plan_aware_prefetch.late_materialization_proof_required",
                    "full payload prefetch requires engine late-materialization proof",
                    descriptor.item_id);
    }
    if (descriptor.parser_or_donor_finality_or_visibility_authority ||
        descriptor.client_finality_or_visibility_authority ||
        descriptor.provider_finality_or_visibility_authority ||
        descriptor.write_ahead_log_recovery_or_finality_authority) {
      return Refuse(request, "plan_prefetch_unsafe_external_authority",
                    "storage.page.plan_aware_prefetch.unsafe_external_authority",
                    "prefetch descriptor claimed non-engine visibility or finality authority",
                    descriptor.item_id);
    }
    if (descriptor.prefetch_evidence_used_for_mga_or_security_authority ||
        !descriptor.diagnostic_only_authority) {
      return Refuse(request, "plan_prefetch_diagnostic_only_authority_required",
                    "storage.page.plan_aware_prefetch.diagnostic_only_authority_required",
                    "prefetch evidence cannot authorize MGA visibility, finality, or security",
                    descriptor.item_id);
    }
    if (AddWouldOverflow(total_byte_cost, descriptor.byte_cost) ||
        AddWouldOverflow(total_page_cost, descriptor.page_cost)) {
      return Refuse(request, "plan_prefetch_budget_overflow",
                    "storage.page.plan_aware_prefetch.budget_overflow",
                    "prefetch descriptor cost overflow", descriptor.item_id);
    }
    total_byte_cost += descriptor.byte_cost;
    total_page_cost += descriptor.page_cost;
  }

  PlanAwarePrefetchResult result;
  result.status = PrefetchOkStatus();
  result.counters.physical_plan_source_present = true;
  result.evidence.push_back("plan_aware_prefetch.physical_plan_source=" +
                            request.physical_plan_id);
  result.evidence.push_back("plan_aware_prefetch.physical_plan_generation=" +
                            std::to_string(request.physical_plan_generation));
  result.evidence.push_back(
      "plan_aware_prefetch.physical_plan_descriptor_driven=true");
  result.evidence.push_back(
      "plan_aware_prefetch.parser_client_provider_text_driven=false");

  if (request.cancellation.cancel_before_scheduling) {
    result.counters.cancelled = true;
    result.counters.cancellation_before_scheduling = true;
    result.counters.skipped_items = request.descriptors.size();
    for (const auto& descriptor : request.descriptors) {
      result.skipped_item_ids.push_back(descriptor.item_id);
    }
    result.evidence.push_back(
        "plan_aware_prefetch.cancellation_before_scheduling=true");
    AddEvidenceCounters(&result);
    result.diagnostic = MakePlanAwarePrefetchDiagnostic(
        result.status, "ok",
        "storage.page.plan_aware_prefetch.cancelled_before_scheduling",
        "prefetch cancelled before scheduling");
    return result;
  }

  for (const auto& descriptor : request.descriptors) {
    ++result.counters.considered_items;
    result.evidence.push_back("plan_aware_prefetch.considered=" +
                              descriptor.item_id + ";family=" +
                              PlanAwarePrefetchFamilyName(descriptor.family));
    if (descriptor.late_materialization_proof_present) {
      result.counters.late_materialization_source_present = true;
      result.evidence.push_back("plan_aware_prefetch.late_materialization_source=" +
                                descriptor.late_materialization_source);
    }

    if (request.cancellation.cancel_after_considered_items != 0 &&
        result.counters.considered_items >
            request.cancellation.cancel_after_considered_items) {
      result.counters.cancelled = true;
      result.counters.cancellation_while_scheduling = true;
      ++result.counters.skipped_items;
      result.skipped_item_ids.push_back(descriptor.item_id);
      result.evidence.push_back(
          "plan_aware_prefetch.cancellation_while_scheduling=true");
      break;
    }

    if (result.counters.scheduled_items == request.budget.max_items ||
        result.counters.scheduled_items == request.budget.max_outstanding ||
        descriptor.byte_cost >
            request.budget.max_bytes - result.counters.used_bytes ||
        descriptor.page_cost >
            request.budget.max_pages - result.counters.used_pages) {
      result.counters.budget_exhausted = true;
      ++result.counters.skipped_items;
      result.skipped_item_ids.push_back(descriptor.item_id);
      result.evidence.push_back("plan_aware_prefetch.skipped_budget=" +
                                descriptor.item_id);
      continue;
    }

    if (AddWouldOverflow(result.counters.used_bytes, descriptor.byte_cost) ||
        AddWouldOverflow(result.counters.used_pages, descriptor.page_cost) ||
        AddWouldOverflow(result.counters.scheduled_items, 1)) {
      return Refuse(request, "plan_prefetch_budget_overflow",
                    "storage.page.plan_aware_prefetch.budget_overflow",
                    "prefetch budget accounting overflow", descriptor.item_id);
    }

    result.counters.used_bytes += descriptor.byte_cost;
    result.counters.used_pages += descriptor.page_cost;
    ++result.counters.scheduled_items;
    result.counters.max_outstanding_observed =
        result.counters.scheduled_items >
                result.counters.max_outstanding_observed
            ? result.counters.scheduled_items
            : result.counters.max_outstanding_observed;
    CountScheduledFamily(&result.counters, descriptor.family);
    result.scheduled_item_ids.push_back(descriptor.item_id);
    result.evidence.push_back("plan_aware_prefetch.scheduled=" +
                              descriptor.item_id + ";family=" +
                              PlanAwarePrefetchFamilyName(descriptor.family));
  }

  if (result.counters.cancelled) {
    const auto remaining = request.descriptors.size() >
                                   result.counters.considered_items
                               ? request.descriptors.size() -
                                     result.counters.considered_items
                               : 0;
    result.counters.skipped_items += remaining;
  }

  AddEvidenceCounters(&result);
  result.diagnostic = MakePlanAwarePrefetchDiagnostic(
      result.status, "ok", "storage.page.plan_aware_prefetch.executed",
      "plan-aware prefetch scheduling evaluated");
  return result;
}

DiagnosticRecord MakePlanAwarePrefetchDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(diagnostic_code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.source_component = "storage.page.plan_aware_prefetch";
  diagnostic.remediation_hint = std::move(detail);
  return diagnostic;
}

}  // namespace scratchbird::storage::page
