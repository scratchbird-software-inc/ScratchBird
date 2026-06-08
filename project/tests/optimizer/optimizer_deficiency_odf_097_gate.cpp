// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "physical_plan_prefetch.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace opt = scratchbird::engine::optimizer;
namespace page = scratchbird::storage::page;
namespace plan = scratchbird::engine::planner;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "provider_finality_or_visibility_authority=true",
          "client_finality_or_visibility_authority=true",
          "parser_or_donor_finality_or_visibility_authority=true",
          "write_ahead_log_recovery_or_finality_authority=true",
          "finality_authority=true", "visibility_authority=true",
          "security_authority=true", "mga_visibility_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-097 evidence leaked forbidden authority or document token");
    }
  }
}

opt::PhysicalPlanNode Node(std::string node_id,
                           plan::PhysicalAccessKind access_kind,
                           std::string descriptor_digest) {
  opt::PhysicalPlanNode node;
  node.node_id = std::move(node_id);
  node.access_kind = access_kind;
  node.executor_capability_id =
      opt::RequiredExecutorCapabilityForAccessKind(access_kind);
  node.descriptor_digest = std::move(descriptor_digest);
  node.storage_backed = access_kind != plan::PhysicalAccessKind::kNone;
  node.preserves_visibility = true;
  return node;
}

opt::PhysicalPlanNode PhysicalPlan() {
  auto root = Node("odf097.plan.root", plan::PhysicalAccessKind::kNone,
                   "digest.root");
  root.children = {
      Node("odf097.heap", plan::PhysicalAccessKind::kTableScan, "digest.heap"),
      Node("odf097.blob", plan::PhysicalAccessKind::kDocumentPathProbe,
           "digest.blob"),
      Node("odf097.vector",
           plan::PhysicalAccessKind::kVectorApproximateWithFallback,
           "digest.vector"),
      Node("odf097.graph", plan::PhysicalAccessKind::kGraphTraversalSeed,
           "digest.graph"),
      Node("odf097.bucket", plan::PhysicalAccessKind::kTimeSeriesAppendPath,
           "digest.bucket"),
      Node("odf097.extent", plan::PhysicalAccessKind::kBitmapSummaryScan,
           "digest.extent")};
  return root;
}

page::PlanAwarePrefetchDescriptor Descriptor(
    page::PlanAwarePrefetchFamily family,
    std::string item_id,
    std::string node_id,
    std::string digest,
    bool full_payload_prefetch,
    platform::u64 byte_cost = 128,
    platform::u64 page_cost = 1) {
  page::PlanAwarePrefetchDescriptor descriptor;
  descriptor.family = family;
  descriptor.item_id = std::move(item_id);
  descriptor.physical_plan_node_id = std::move(node_id);
  descriptor.physical_plan_descriptor_digest = std::move(digest);
  descriptor.physical_plan_generation = 97;
  descriptor.descriptor_generation = 97;
  descriptor.full_payload_prefetch = full_payload_prefetch;
  descriptor.byte_cost = byte_cost;
  descriptor.page_cost = page_cost;
  if (full_payload_prefetch) {
    descriptor.late_materialization_proof_present = true;
    descriptor.late_materialization_source =
        "late_materialization.final_authorized_pruned";
  } else {
    descriptor.late_materialization_source =
        "physical_plan.scan_descriptor";
  }
  return descriptor;
}

std::vector<page::PlanAwarePrefetchDescriptor> AllFamilyDescriptors() {
  return {
      Descriptor(page::PlanAwarePrefetchFamily::kHeapPage, "heap.page.1",
                 "odf097.heap", "digest.heap", false),
      Descriptor(page::PlanAwarePrefetchFamily::kLargePayload, "blob.payload.1",
                 "odf097.blob", "digest.blob", true),
      Descriptor(page::PlanAwarePrefetchFamily::kVectorPayloadPage,
                 "vector.payload.page.1", "odf097.vector", "digest.vector",
                 true),
      Descriptor(page::PlanAwarePrefetchFamily::kGraphAdjacencyPage,
                 "graph.adjacency.page.1", "odf097.graph", "digest.graph",
                 false),
      Descriptor(page::PlanAwarePrefetchFamily::kTimeSeriesBucket,
                 "time.bucket.1", "odf097.bucket", "digest.bucket", false),
      Descriptor(page::PlanAwarePrefetchFamily::kExtentMetadata,
                 "extent.metadata.1", "odf097.extent", "digest.extent",
                 false)};
}

opt::PhysicalPlanPrefetchInput Input(
    std::vector<page::PlanAwarePrefetchDescriptor> descriptors) {
  opt::PhysicalPlanPrefetchInput input;
  input.physical_plan_generation = 97;
  input.descriptors = std::move(descriptors);
  input.budget.max_bytes = 4096;
  input.budget.max_pages = 32;
  input.budget.max_items = 32;
  input.budget.max_outstanding = 32;
  return input;
}

void AllFamiliesArePlanDrivenAndDiagnosticOnly() {
  const auto result =
      opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(),
                                             Input(AllFamilyDescriptors()));
  Require(result.ok(), "ODF-097 all-family prefetch scheduling failed");
  Require(result.counters.considered_items == 6,
          "ODF-097 considered-item counter changed");
  Require(result.counters.scheduled_items == 6,
          "ODF-097 did not schedule all requested families");
  Require(result.counters.scheduled_heap_pages == 1,
          "ODF-097 heap page prefetch counter missing");
  Require(result.counters.scheduled_large_payloads == 1,
          "ODF-097 blob/large-payload prefetch counter missing");
  Require(result.counters.scheduled_vector_payload_pages == 1,
          "ODF-097 vector payload/page prefetch counter missing");
  Require(result.counters.scheduled_graph_adjacency_pages == 1,
          "ODF-097 graph adjacency prefetch counter missing");
  Require(result.counters.scheduled_time_series_buckets == 1,
          "ODF-097 time-series bucket prefetch counter missing");
  Require(result.counters.scheduled_extent_metadata == 1,
          "ODF-097 extent metadata prefetch counter missing");
  Require(result.counters.late_materialization_source_present,
          "ODF-097 late-materialization source evidence missing");
  Require(result.counters.physical_plan_source_present,
          "ODF-097 physical-plan source evidence missing");
  Require(EvidenceHas(result.evidence,
                      "plan_aware_prefetch.physical_plan_source=odf097.plan.root"),
          "ODF-097 physical-plan source token missing");
  Require(EvidenceHas(result.evidence,
                      "plan_aware_prefetch.physical_plan_descriptor_driven=true"),
          "ODF-097 descriptor-driven evidence missing");
  Require(EvidenceHas(result.evidence,
                      "plan_aware_prefetch.optimizer_physical_plan_driven=true"),
          "ODF-097 optimizer physical-plan evidence missing");
  Require(EvidenceHas(result.evidence,
                      "plan_aware_prefetch.diagnostic_only_authority=true"),
          "ODF-097 diagnostic-only evidence missing");
  Require(EvidenceHas(result.evidence,
                      "plan_aware_prefetch.finality_authority=false"),
          "ODF-097 finality non-authority evidence missing");
  Require(EvidenceHas(result.evidence,
                      "plan_aware_prefetch.visibility_authority=false"),
          "ODF-097 visibility non-authority evidence missing");
  Require(EvidenceHas(result.evidence,
                      "plan_aware_prefetch.security_authority=false"),
          "ODF-097 security non-authority evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

void BudgetAndCancellationExposeDeterministicPartialScheduling() {
  auto budgeted = Input(AllFamilyDescriptors());
  budgeted.budget.max_items = 3;
  budgeted.budget.max_outstanding = 3;
  const auto budgeted_result =
      opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(), budgeted);
  Require(budgeted_result.ok(), "ODF-097 budgeted prefetch refused");
  Require(budgeted_result.counters.scheduled_items == 3,
          "ODF-097 item/outstanding budget did not cap scheduled work");
  Require(budgeted_result.counters.skipped_items == 3,
          "ODF-097 budget skip counter changed");
  Require(budgeted_result.counters.budget_exhausted,
          "ODF-097 budget-exhausted evidence missing");
  Require(budgeted_result.scheduled_item_ids.size() == 3 &&
              budgeted_result.scheduled_item_ids[0] == "heap.page.1" &&
              budgeted_result.scheduled_item_ids[1] == "blob.payload.1" &&
              budgeted_result.scheduled_item_ids[2] == "vector.payload.page.1",
          "ODF-097 budgeted scheduling order was not deterministic");

  auto cancelled = Input(AllFamilyDescriptors());
  cancelled.cancellation.cancel_after_considered_items = 2;
  const auto cancelled_result =
      opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(), cancelled);
  Require(cancelled_result.ok(), "ODF-097 cancellable prefetch refused");
  Require(cancelled_result.counters.cancelled,
          "ODF-097 cancellation counter missing");
  Require(cancelled_result.counters.cancellation_while_scheduling,
          "ODF-097 while-scheduling cancellation evidence missing");
  Require(cancelled_result.counters.scheduled_items == 2,
          "ODF-097 cancellation did not stop after deterministic partial work");
  Require(cancelled_result.scheduled_item_ids.size() == 2 &&
              cancelled_result.scheduled_item_ids[0] == "heap.page.1" &&
              cancelled_result.scheduled_item_ids[1] == "blob.payload.1",
          "ODF-097 cancelled scheduling order was not deterministic");

  auto pre_cancelled = Input(AllFamilyDescriptors());
  pre_cancelled.cancellation.cancel_before_scheduling = true;
  const auto pre_cancelled_result =
      opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(), pre_cancelled);
  Require(pre_cancelled_result.ok(),
          "ODF-097 pre-scheduling cancellation refused");
  Require(pre_cancelled_result.counters.cancelled &&
              pre_cancelled_result.counters.cancellation_before_scheduling,
          "ODF-097 pre-scheduling cancellation evidence missing");
  Require(pre_cancelled_result.counters.scheduled_items == 0,
          "ODF-097 scheduled work after pre-scheduling cancellation");
}

void RefusalsFailClosed() {
  auto missing_identity_plan = PhysicalPlan();
  missing_identity_plan.node_id.clear();
  auto result = opt::ExecutePhysicalPlanDrivenPrefetch(
      missing_identity_plan, Input(AllFamilyDescriptors()));
  Require(!result.ok() && result.fail_closed,
          "ODF-097 missing physical plan identity was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "plan_prefetch_physical_plan_identity_required",
          "ODF-097 missing physical plan identity diagnostic changed");

  auto stale = AllFamilyDescriptors();
  stale.front().descriptor_generation = 96;
  result = opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(), Input(stale));
  Require(!result.ok() && result.fail_closed,
          "ODF-097 stale descriptor generation was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "plan_prefetch_stale_descriptor_generation",
          "ODF-097 stale descriptor diagnostic changed");

  auto unsupported = AllFamilyDescriptors();
  unsupported.front().family = page::PlanAwarePrefetchFamily::kUnsupported;
  result = opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(),
                                                 Input(unsupported));
  Require(!result.ok() && result.fail_closed,
          "ODF-097 unsupported family was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "plan_prefetch_unsupported_family",
          "ODF-097 unsupported family diagnostic changed");

  auto corrupt_family = AllFamilyDescriptors();
  corrupt_family.front().family =
      static_cast<page::PlanAwarePrefetchFamily>(999);
  result = opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(),
                                                 Input(corrupt_family));
  Require(!result.ok() && result.fail_closed,
          "ODF-097 corrupt family enum was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "plan_prefetch_unsupported_family",
          "ODF-097 corrupt family diagnostic changed");

  auto missing_late = AllFamilyDescriptors();
  missing_late[1].late_materialization_proof_present = false;
  result = opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(),
                                                 Input(missing_late));
  Require(!result.ok() && result.fail_closed,
          "ODF-097 missing late-materialization proof was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "plan_prefetch_late_materialization_proof_required",
          "ODF-097 missing late-materialization diagnostic changed");

  for (const auto authority_case :
       {"parser", "client", "provider", "write_after_log"}) {
    auto unsafe = AllFamilyDescriptors();
    if (std::string_view(authority_case) == "parser") {
      unsafe.front().parser_or_donor_finality_or_visibility_authority = true;
    } else if (std::string_view(authority_case) == "client") {
      unsafe.front().client_finality_or_visibility_authority = true;
    } else if (std::string_view(authority_case) == "provider") {
      unsafe.front().provider_finality_or_visibility_authority = true;
    } else {
      unsafe.front().write_ahead_log_recovery_or_finality_authority = true;
    }
    result =
        opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(), Input(unsafe));
    Require(!result.ok() && result.fail_closed,
            "ODF-097 unsafe external authority was accepted");
    Require(result.diagnostic.diagnostic_code ==
                "plan_prefetch_unsafe_external_authority",
            "ODF-097 unsafe authority diagnostic changed");
  }

  auto unsafe = AllFamilyDescriptors();
  unsafe.front().prefetch_evidence_used_for_mga_or_security_authority = true;
  result = opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(), Input(unsafe));
  Require(!result.ok() && result.fail_closed,
          "ODF-097 prefetch evidence authority claim was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "plan_prefetch_diagnostic_only_authority_required",
          "ODF-097 diagnostic-only authority diagnostic changed");

  auto mismatch = AllFamilyDescriptors();
  mismatch.front().physical_plan_descriptor_digest = "digest.other";
  result = opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(),
                                                 Input(mismatch));
  Require(!result.ok() && result.fail_closed,
          "ODF-097 physical descriptor mismatch was accepted");
  Require(result.diagnostic.diagnostic_code ==
              "plan_prefetch_physical_descriptor_mismatch",
          "ODF-097 physical descriptor mismatch diagnostic changed");

  auto overflow = AllFamilyDescriptors();
  overflow.front().byte_cost = std::numeric_limits<platform::u64>::max();
  overflow[1].byte_cost = 1;
  auto overflow_input = Input(overflow);
  overflow_input.budget.max_bytes = std::numeric_limits<platform::u64>::max();
  result = opt::ExecutePhysicalPlanDrivenPrefetch(PhysicalPlan(),
                                                 overflow_input);
  Require(!result.ok() && result.fail_closed,
          "ODF-097 budget overflow was accepted");
  Require(result.diagnostic.diagnostic_code == "plan_prefetch_budget_overflow",
          "ODF-097 budget overflow diagnostic changed");
}

}  // namespace

int main() {
  AllFamiliesArePlanDrivenAndDiagnosticOnly();
  BudgetAndCancellationExposeDeterministicPartialScheduling();
  RefusalsFailClosed();
  return EXIT_SUCCESS;
}
