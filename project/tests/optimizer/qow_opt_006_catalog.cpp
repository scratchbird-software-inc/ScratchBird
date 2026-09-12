// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_catalog_backed_planning.hpp"
#include "optimizer_profile_factory.hpp"
#include "model_family_profile_factory.hpp"
#include "../sbsql_sblr_alignment/binary_uuid_fixture.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <set>
#include <chrono>
#include <thread>

#ifndef QOW_OPT_006_FIXTURE_ONLY
namespace profile_fault {
thread_local long remaining = -1;
thread_local bool hit = false;
unsigned failures = 0;
}
void* operator new(std::size_t size) {
  if (profile_fault::remaining >= 0 && profile_fault::remaining-- == 0) {
    profile_fault::remaining = 0; profile_fault::hit = true;
    throw std::bad_alloc();
  }
  if (void* result = std::malloc(size ? size : 1)) return result;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
#endif

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

using scratchbird::tests::BinaryUuid;
unsigned checks = 0;

constexpr std::string_view kTree =
    "019f0000-0000-7300-8000-000000006101";
constexpr std::string_view kCatalog =
    "019f0000-0000-7300-8000-000000006102";
constexpr std::string_view kMetadata =
    "019f0000-0000-7300-8000-000000006109";
constexpr std::string_view kSecurity =
    "019f0000-0000-7300-8000-000000006103";
constexpr std::string_view kRelation =
    "019f0000-0000-7300-8000-000000006104";
constexpr std::string_view kStatistics =
    "019f0000-0000-7300-8000-000000006105";
constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActive = 0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kHorizon = 0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubt = 0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kInventoryNext = 0xffff'ffff'ffff'fff0ULL;

bool Require(const bool condition, const std::string_view detail) {
  ++checks;
  if (!condition) {
    std::cerr << "QOW-TEST-OPT-006-CATALOG-V1: " << detail << '\n';
  }
  return condition;
}

plan::CanonicalMgaStatementContext MgaContext() {
  plan::CanonicalMgaStatementContext context;
  context.statement_uuid =
      BinaryUuid("019f0000-0000-7300-8000-000000006110");
  context.owning_transaction_uuid =
      BinaryUuid("019f0000-0000-7300-8000-000000006111");
  context.statement_snapshot_uuid =
      BinaryUuid("019f0000-0000-7300-8000-000000006112");
  context.statement_metadata_snapshot_uuid = BinaryUuid(kMetadata);
  context.owning_local_transaction_id = kOwner;
  context.visible_committed_high_watermark = 0;
  context.oldest_active_transaction_id = kOldestActive;
  context.oldest_interesting_transaction_id = kHorizon;
  context.oldest_snapshot_transaction_id = kHorizon;
  context.retention_horizon_transaction_id = kHorizon;
  context.active_excluded_local_transaction_ids = {kOldestActive, kOwner};
  context.in_doubt_excluded_local_transaction_ids = {kInDoubt};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = kInventoryNext;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

opt::CanonicalOptimizerAdmissionRequest Request() {
  opt::CanonicalOptimizerAdmissionRequest request;
  auto& graph = request.logical_graph;
  graph.bound_sblr_tree_uuid = BinaryUuid(kTree);
  graph.catalog_epoch_uuid = BinaryUuid(kCatalog);
  graph.security_context_uuid = BinaryUuid(kSecurity);
  graph.local_transaction_id = kOwner;
  graph.statement_snapshot_id = 0;
  graph.mga_statement_context = MgaContext();
  graph.root_logical_node_id = 1;
  graph.result_descriptor_ids = {1};
  plan::CanonicalLogicalRelationalNode source;
  source.logical_node_id = 1;
  source.node_kind = plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  source.output_descriptor_ids = {1};
  source.origin_relational_node_ids = {1};
  source.required_object_uuids = {BinaryUuid(kRelation)};
  source.semantic_variant_id = "relation.source.v1";
  graph.nodes = {source};

  request.logical_properties.bound_sblr_tree_uuid = BinaryUuid(kTree);
  request.logical_properties.catalog_epoch_uuid = BinaryUuid(kCatalog);
  request.logical_properties.security_context_uuid = BinaryUuid(kSecurity);
  request.logical_properties.local_transaction_id = kOwner;
  request.logical_properties.statement_snapshot_id = 0;
  request.logical_properties.mga_statement_context = MgaContext();

  request.catalog.snapshot_uuid = BinaryUuid(kMetadata);
  request.catalog.catalog_epoch_uuid = BinaryUuid(kCatalog);
  request.catalog.catalog_generation = 17;
  request.catalog.object_uuids = {BinaryUuid(kRelation)};
  request.catalog.descriptor_ids = {1};
  request.catalog.engine_owned = true;

  request.security.security_context_uuid = BinaryUuid(kSecurity);
  request.security.security_epoch = 18;
  request.security.policy_epoch = 19;
  request.security.catalog_generation = 17;
  request.security.authorized_object_uuids = {BinaryUuid(kRelation)};
  request.security.engine_owned = true;

  request.mga.local_transaction_id = kOwner;
  request.mga.statement_snapshot_id = 0;
  request.mga.statement_context = MgaContext();
  request.mga.metadata_snapshot_uuid = BinaryUuid(kMetadata);
  request.mga.transaction_active = true;
  request.mga.statement_snapshot_fixed = true;
  request.mga.engine_owned = true;

  request.policy_capability.policy_snapshot_uuid = BinaryUuid(kSecurity);
  request.policy_capability.policy_epoch = 19;
  request.policy_capability.capability_snapshot_uuid =
      BinaryUuid("019f0000-0000-7300-8000-000000006106");
  request.policy_capability.capability_abi_version = 1;
  request.policy_capability.supported_node_kinds = {
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource};
  request.policy_capability.engine_owned = true;

  request.resource.resource_snapshot_uuid =
      BinaryUuid("019f0000-0000-7300-8000-000000006107");
  request.resource.resource_epoch = 20;
  request.resource.memory_budget_bytes = 4 * 1024 * 1024;
  request.resource.maximum_candidate_count = 8;
  request.resource.maximum_memo_groups = 8;
  request.resource.maximum_search_steps = 64;
  request.resource.maximum_planning_time_ns = 1'000'000;
  request.resource.spill_allowed = true;
  request.resource.engine_owned = true;

  request.statistics.statistics_snapshot_uuid = BinaryUuid(kStatistics);
  request.statistics.catalog_epoch_uuid = BinaryUuid(kCatalog);
  request.statistics.statistics_generation = 21;
  request.statistics.admitted_at_monotonic_ns = 1'000'000;
  request.statistics.captured_before_data_access = true;
  opt::CanonicalOptimizerNodeEstimate estimate;
  estimate.logical_node_id = 1;
  estimate.object_uuid = BinaryUuid(kRelation);
  estimate.state = opt::CanonicalOptimizerStatisticState::kKnown;
  estimate.source = opt::CanonicalOptimizerStatisticSource::kCatalogExact;
  estimate.catalog_epoch_uuid = BinaryUuid(kCatalog);
  estimate.statistics_snapshot_uuid = BinaryUuid(kStatistics);
  estimate.statistics_generation = 21;
  estimate.collected_at_monotonic_ns = 900'000;
  estimate.admitted_at_monotonic_ns = 1'000'000;
  estimate.maximum_age_ns = 200'000;
  estimate.confidence = opt::CostConfidence::kExact;
  estimate.row_count_present = true;
  estimate.row_count = 0;
  estimate.page_count_present = true;
  estimate.page_count = 1;
  request.statistics.node_estimates = {estimate};

  request.route.route_snapshot_uuid =
      BinaryUuid("019f0000-0000-7300-8000-000000006108");
  request.route.route_epoch = 22;
  request.route.route_generation = 23;
  request.route.operation_id = "query.execute";
  request.route.route_id = "native.sblr.query.execute.v2";
  request.route.native_local_route = true;
  request.route.engine_owned = true;
  request.populated_from_admitted_typed_sblr = true;
  return request;
}

bool ValidateCatalogAdmission() {
  const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(Request());
  bool passed = true;
  passed &= Require(result.admitted && result.planning_allowed &&
                        result.benchmark_clean_ready &&
                        !result.degraded_for_unknown_statistics &&
                        !result.data_access_allowed && result.issues.empty() &&
                        result.evidence.size() == 8 &&
                        result.local_transaction_id == kOwner &&
                        result.statement_snapshot_id == 0 &&
                        plan::CanonicalMgaStatementContextEqual(
                            result.mga_statement_context, MgaContext()),
                    "complete catalog-backed request was not admitted");
  for (std::size_t index = 0; index < result.evidence.size(); ++index) {
    passed &= Require(
        result.evidence[index].stage ==
            static_cast<opt::CanonicalOptimizerAdmissionStage>(index + 1),
        "admission evidence order changed");
  }
  return passed;
}

bool ValidateCatalogRefusals() {
  bool passed = true;
  const auto expect_catalog_refusal = [&](auto mutation,
                                           const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(!result.admitted && !result.planning_allowed &&
                       !result.data_access_allowed &&
                       result.evidence.size() == 1 &&
                       result.issues.size() == 1 &&
                       result.issues.front().stage ==
                           opt::CanonicalOptimizerAdmissionStage::kCatalogEpoch &&
                       result.issues.front().diagnostic_id ==
                           "QOW-DIAG-OPTIMIZER-ADMISSION-CATALOG-V1",
                   detail);
  };
  passed &= expect_catalog_refusal(
      [](auto& request) { request.catalog.engine_owned = false; },
      "non-engine catalog snapshot was admitted");
  passed &= expect_catalog_refusal(
      [](auto& request) { request.catalog.object_uuids.clear(); },
      "missing catalog object was admitted");
  passed &= expect_catalog_refusal(
      [](auto& request) { request.catalog.descriptor_ids.clear(); },
      "missing descriptor snapshot was admitted");
  passed &= expect_catalog_refusal(
      [](auto& request) {
        request.catalog.catalog_epoch_uuid =
            BinaryUuid("019f0000-0000-7300-8000-000000006999");
      },
      "stale catalog epoch was admitted");
  passed &= expect_catalog_refusal(
      [](auto& request) {
        request.catalog.catalog_epoch_uuid = {};
      },
      "nil catalog epoch was admitted");
  passed &= expect_catalog_refusal(
      [](auto& request) {
        request.catalog.snapshot_uuid = request.catalog.catalog_epoch_uuid;
      },
      "catalog epoch was accepted as the metadata snapshot");
  passed &= expect_catalog_refusal(
      [](auto& request) {
        std::swap(request.catalog.snapshot_uuid,
                  request.catalog.catalog_epoch_uuid);
      },
      "swapped catalog epoch and metadata snapshot were admitted");

  const auto expect_mga_refusal = [&](auto mutation,
                                      const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(!result.admitted && !result.planning_allowed &&
                       !result.issues.empty() &&
                       result.issues.front().stage ==
                           opt::CanonicalOptimizerAdmissionStage::
                               kMgaStatementBoundary,
                   detail);
  };
  passed &= expect_mga_refusal(
      [](auto& request) {
        request.mga.statement_context.current = false;
      },
      "non-current inventory snapshot was admitted");
  passed &= expect_mga_refusal(
      [](auto& request) {
        request.mga.statement_context.oldest_active_transaction_id =
            kInventoryNext;
      },
      "future snapshot horizon was admitted");
  passed &= expect_mga_refusal(
      [](auto& request) {
        request.mga.statement_context
            .active_excluded_local_transaction_ids =
                {kOldestActive, kOwner, kInventoryNext};
      },
      "out-of-ceiling snapshot exclusion was admitted");
  return passed;
}

bool ValidateCompleteMgaCarrierRefusals() {
  const auto expect_refusal = [](auto mutation,
                                 const opt::CanonicalOptimizerAdmissionStage stage,
                                 const std::size_t evidence_count,
                                 const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(!result.admitted && !result.planning_allowed &&
                       !result.data_access_allowed &&
                       result.evidence.size() == evidence_count &&
                       result.issues.size() == 1 &&
                       result.issues.front().stage == stage,
                   detail);
  };
  using Stage = opt::CanonicalOptimizerAdmissionStage;
  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) {
        request.logical_graph.mga_statement_context.complete = false;
      }, Stage::kBoundRequest, 0,
      "incomplete logical-graph statement context reached admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.logical_properties.mga_statement_context.current = false;
      }, Stage::kBoundRequest, 0,
      "stale logical-property statement context reached admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.logical_graph.local_transaction_id =
            static_cast<std::uint32_t>(kOwner);
      }, Stage::kBoundRequest, 0,
      "narrowed graph transaction alias reached admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.statement_uuid = {};
      }, Stage::kMgaStatementBoundary, 3,
      "missing statement UUID reached MGA admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.owning_transaction_uuid =
            BinaryUuid("019f0000-0000-4300-8000-000000006111");
      }, Stage::kMgaStatementBoundary, 3,
      "malformed owner UUID reached MGA admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.in_doubt_excluded_local_transaction_ids =
            {kInDoubt, kInDoubt};
      }, Stage::kMgaStatementBoundary, 3,
      "duplicate in-doubt exclusion reached MGA admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.in_doubt_excluded_local_transaction_ids =
            {kOwner};
      }, Stage::kMgaStatementBoundary, 3,
      "overlapping exclusion vectors reached MGA admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.publication_inventory_next_local_transaction_id =
            static_cast<std::uint32_t>(kInventoryNext);
      }, Stage::kMgaStatementBoundary, 3,
      "truncated inventory ceiling reached MGA admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.local_transaction_id =
            static_cast<std::uint32_t>(kOwner);
      }, Stage::kMgaStatementBoundary, 3,
      "narrowed MGA transaction alias reached admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.statement_snapshot_uuid =
            BinaryUuid("019f0000-0000-7300-8000-000000006998");
      }, Stage::kMgaStatementBoundary, 3,
      "swapped MGA statement context reached admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.metadata_snapshot_uuid =
            request.catalog.catalog_epoch_uuid;
      }, Stage::kMgaStatementBoundary, 3,
      "catalog epoch was accepted as MGA metadata snapshot");
  return passed;
}

#ifndef QOW_OPT_006_FIXTURE_ONLY
opt::CanonicalOptimizerExecutorAvailability Availability(const opt::CanonicalOptimizerAdmissionRequest& request) {
  opt::CanonicalOptimizerExecutorAvailability result;
  result.engine_owned = true;
  auto& catalog = result.capability_catalog;
  catalog.engine_owned = true;
  catalog.capability_snapshot_uuid = request.policy_capability.capability_snapshot_uuid;
  catalog.policy_epoch = request.policy_capability.policy_epoch;
  opt::CanonicalExecutorCapabilityRecord capability;
  capability.capability_uuid = BinaryUuid("019f0000-0000-7300-8000-000000006201");
  capability.capability_abi_version = 1;
  capability.implementation_id = "scan.local";
  capability.logical_node_kind = plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  capability.physical_node_kind = scratchbird::engine::executor::PhysicalNodeKind::kScan;
  capability.maximum_memory_bytes = 4096;
  capability.storage_read_capable = true;
  capability.mga_visibility_capable = true;
  capability.available = true;
  capability.engine_owned = true;
  catalog.capabilities = {capability};
  result.node_bindings = {{1, capability.capability_uuid, 128, true, {}}};
  return result;
}

bool ValidateProfileIdentityOwnership() {
  auto request = Request();
  const auto admission = opt::AdmitCanonicalOptimizerPlanningRequest(request);
  auto availability = Availability(request);
  const auto calibration = BinaryUuid("019f0000-0000-7300-8000-000000006202");
  const auto before = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  const auto first = opt::BuildCanonicalOptimizerAlternativeProfiles(
      request, admission, availability, calibration);
  const auto after = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  if (!Require(first.accepted && first.identity_owner && first.candidates.size() == 1,
               "actual profile factory did not retain an issued identity owner")) {
    for (const auto& issue : first.issues) std::cerr << issue.diagnostic_id << ":" << issue.field_id << '\n';
    return false;
  }
  const auto& candidate = first.candidates.front();
  std::set<plan::CanonicalPlannerUuid> identities{
      first.identity_owner->ScopeUuid(), candidate.alternative_uuid,
      candidate.transformation_uuid, candidate.cost_terms.cost_vector_uuid};
  bool passed = Require(identities.size() == 4, "runtime objects share an issued identity");
  for (const auto& id : identities) {
    passed &= Require((id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80,
                      "profile identity is not native binary UUIDv7");
    std::uint64_t millis = 0;
    for (unsigned i = 0; i != 6; ++i) millis = (millis << 8) | id.bytes[i];
    passed &= Require(millis >= static_cast<std::uint64_t>(before) &&
                      millis <= static_cast<std::uint64_t>(after),
                      "profile UUID is not issued at the observed runtime time");
  }
  const auto repeated = opt::BuildCanonicalOptimizerAlternativeProfiles(
      request, admission, availability, calibration, first.identity_owner);
  passed &= Require(repeated.accepted && repeated.identity_owner == first.identity_owner &&
      repeated.candidates.front().alternative_uuid == candidate.alternative_uuid &&
      repeated.candidates.front().transformation_uuid == candidate.transformation_uuid &&
      repeated.candidates.front().cost_terms.cost_vector_uuid == candidate.cost_terms.cost_vector_uuid,
      "same retained scope did not reuse all issued identities");
  passed &= Require(candidate.cost_terms.cpu_units == 0,
                    "factory raised a known zero row count to one");

  const std::vector<std::function<void(opt::CanonicalOptimizerAdmissionRequest&)>> mutations{
    [](auto& value) { ++value.catalog.catalog_generation; },
    [](auto& value) { ++value.security.security_epoch; },
    [](auto& value) { value.mga.metadata_snapshot_uuid.bytes[15] ^= 1; },
    [](auto& value) { ++value.policy_capability.policy_epoch; },
    [](auto& value) { --value.resource.memory_budget_bytes; },
    [](auto& value) { ++value.resource.maximum_search_steps; },
    [](auto& value) { ++value.statistics.node_estimates[0].row_count; },
    [](auto& value) { ++value.statistics.node_estimates[0].page_count; },
    [](auto& value) { ++value.statistics.admitted_at_monotonic_ns; },
    [](auto& value) { ++value.route.route_generation; },
    [](auto& value) { value.logical_graph.nodes[0].shareable = true; },
    [](auto& value) { value.logical_graph.nodes[0].bound_expression_ids = {1}; },
  };
  for (const auto& mutate : mutations) {
    auto changed = request; mutate(changed);
    const auto result = opt::BuildCanonicalOptimizerAlternativeProfiles(
        changed, admission, availability, calibration, first.identity_owner);
    passed &= Require(!result.accepted && !result.identity_owner && result.candidates.empty(),
                      "changed scope reused issued identities or published a partial owner");
  }
  for (unsigned bit = 0; bit != 128; ++bit) {
    auto changed = calibration;
    changed.bytes[bit / 8] ^= 1u << (bit % 8);
    const auto refused = opt::BuildCanonicalOptimizerAlternativeProfiles(
        request, admission, availability, changed, first.identity_owner);
    passed &= Require(!refused.accepted && !refused.identity_owner,
                      "calibration bit change reused another scope");
  }
  auto stale = request;
  ++stale.security.security_epoch;
  const auto stale_result = opt::BuildCanonicalOptimizerAlternativeProfiles(
      stale, admission, availability, calibration);
  passed &= Require(!stale_result.accepted && !stale_result.identity_owner,
                    "fresh owner accepted stale admission metadata");
  auto duplicated = availability;
  duplicated.capability_catalog.capabilities.push_back(duplicated.capability_catalog.capabilities[0]);
  const auto duplicate_result = opt::BuildCanonicalOptimizerAlternativeProfiles(
      request, admission, duplicated, calibration);
  passed &= Require(!duplicate_result.accepted && !duplicate_result.identity_owner,
                    "duplicate capability records gained factory inventory authority");
  auto parser_capability = availability;
  parser_capability.capability_catalog.capabilities[0].parser_execution_authority_claimed = true;
  const auto parser_result = opt::BuildCanonicalOptimizerAlternativeProfiles(
      request, admission, parser_capability, calibration);
  passed &= Require(!parser_result.accepted && !parser_result.identity_owner,
                    "parser execution authority gained factory inventory authority");
  availability.capability_catalog.capabilities[0].implementation_id = "index.scan.local";
  request.statistics.node_estimates[0].page_count = 0;
  auto zero = opt::BuildCanonicalOptimizerAlternativeProfiles(
      request, admission, availability, calibration);
  passed &= Require(zero.accepted && zero.candidates[0].cost_terms.page_read_random_units == 0 &&
      zero.candidates[0].cost_terms.cache_units == 0, "known zero pages became synthetic IO");
  request.statistics.node_estimates[0].page_count = std::numeric_limits<std::uint64_t>::max();
  auto maximum = opt::BuildCanonicalOptimizerAlternativeProfiles(
      request, admission, availability, calibration);
  passed &= Require(maximum.accepted &&
      maximum.candidates[0].cost_terms.page_read_random_units == (std::uint64_t{1} << 61),
      "page ceiling division overflowed UINT64_MAX");

  auto unknown_request = Request();
  auto& estimate = unknown_request.statistics.node_estimates[0];
  estimate.state = opt::CanonicalOptimizerStatisticState::kUnknown;
  estimate.source = opt::CanonicalOptimizerStatisticSource::kUnavailable;
  estimate.confidence = opt::CostConfidence::kUnknown;
  estimate.row_count_present = estimate.page_count_present = false;
  estimate.row_count = estimate.page_count = 0;
  estimate.collected_at_monotonic_ns = estimate.maximum_age_ns = 0;
  const auto unknown_admission = opt::AdmitCanonicalOptimizerPlanningRequest(unknown_request);
  const auto unknown = opt::BuildCanonicalOptimizerAlternativeProfiles(
      unknown_request, unknown_admission, Availability(unknown_request), calibration);
  passed &= Require(unknown.accepted && unknown.candidates[0].cost_terms.confidence == opt::CostConfidence::kLow,
                    "unknown statistics gained medium/exact confidence");
  return passed;
}

opt::ModelFamilyProfileFactoryRequestV1 ModelRequest() {
  opt::ModelFamilyProfileFactoryRequestV1 request;
  auto& logical = request.logical_request;
  logical.family_id = "document";
  logical.operation_id = "DOCUMENT_FIND";
  logical.logical_operator_id = "LOGICAL_DOCUMENT_SOURCE_V1";
  logical.logical_node_id = 1;
  logical.object_uuid = BinaryUuid(kRelation);
  logical.output_descriptor_ids = {1};
  logical.bound_sblr_tree_uuid = BinaryUuid(kTree);
  logical.catalog_epoch_uuid = BinaryUuid(kCatalog);
  logical.security_context_uuid = BinaryUuid(kSecurity);
  logical.capability_snapshot_uuid = BinaryUuid("019f0000-0000-7300-8000-000000006106");
  logical.resource_snapshot_uuid = BinaryUuid("019f0000-0000-7300-8000-000000006107");
  logical.statistics_snapshot_uuid = BinaryUuid(kStatistics);
  logical.route_snapshot_uuid = BinaryUuid("019f0000-0000-7300-8000-000000006108");
  logical.catalog_generation = logical.current_catalog_generation = 17;
  logical.security_epoch = 18; logical.policy_epoch = 19;
  logical.resource_epoch = 20; logical.statistics_generation = 21;
  logical.route_epoch = 22; logical.route_generation = 23;
  logical.memory_budget_bytes = 4 * 1024 * 1024;
  const auto source = MgaContext();
  auto& mga = logical.mga_statement_context;
  mga.statement_uuid = source.statement_uuid;
  mga.owning_transaction_uuid = source.owning_transaction_uuid;
  mga.statement_snapshot_uuid = source.statement_snapshot_uuid;
  mga.statement_metadata_snapshot_uuid = source.statement_metadata_snapshot_uuid;
  mga.owning_local_transaction_id = source.owning_local_transaction_id;
  mga.visible_committed_high_watermark = source.visible_committed_high_watermark;
  mga.oldest_active_transaction_id = source.oldest_active_transaction_id;
  mga.oldest_interesting_transaction_id = source.oldest_interesting_transaction_id;
  mga.oldest_snapshot_transaction_id = source.oldest_snapshot_transaction_id;
  mga.retention_horizon_transaction_id = source.retention_horizon_transaction_id;
  mga.active_excluded_local_transaction_ids = source.active_excluded_local_transaction_ids;
  mga.in_doubt_excluded_local_transaction_ids = source.in_doubt_excluded_local_transaction_ids;
  mga.snapshot_kind = source.snapshot_kind;
  mga.publication_inventory_next_local_transaction_id = source.publication_inventory_next_local_transaction_id;
  mga.inventory_authoritative = mga.complete = mga.current = true;
  opt::ModelFamilyCapabilitySnapshotV1 cap;
  cap.provider_uuid = BinaryUuid("019f0000-0000-7300-8000-000000006210");
  cap.capability_uuid = BinaryUuid("019f0000-0000-7300-8000-000000006211");
  cap.provider_generation = 1;
  cap.available = true;
  cap.metrics.statistics_snapshot_uuid = logical.statistics_snapshot_uuid;
  cap.metrics.property_snapshot_uuid = BinaryUuid("019f0000-0000-7300-8000-000000006212");
  cap.metrics.calibration_profile_uuid = BinaryUuid("019f0000-0000-7300-8000-000000006213");
  cap.metrics.statistics_generation = logical.statistics_generation;
  cap.metrics.confidence_basis_points = 10'000;
  // Exact empty input with actual resident implementation state; zero is not absence.
  cap.metrics.working_set_bytes = 128;
  request.capability_snapshots = {cap};
  return request;
}

bool ValidateModelProfileOwnership() {
  auto request = ModelRequest();
  auto native = request.capability_snapshots.front();
  auto fallback = native;
  fallback.route_class = opt::ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback;
  // Same capability/provider on different routes must have separate runtime IDs.
  request.capability_snapshots = {fallback, native};
  auto first = opt::BuildModelFamilyAlternativeProfilesV1(request);
  if (!Require(first.accepted && first.identity_owner && first.candidates.size() == 2,
               "model factory failed to publish empty-source profiles")) return false;
  bool passed = Require(first.native_alternative_count == 1 && first.exact_fallback_alternative_count == 1,
                        "model route inventory is incomplete");
  std::set<plan::CanonicalPlannerUuid> ids{first.candidate_inventory_receipt_uuid};
  for (const auto& candidate : first.candidates) {
    ids.insert(candidate.alternative_uuid);
    ids.insert(candidate.cost.cost_vector_uuid);
    passed &= Require(candidate.cost.cpu_units == 0 && candidate.cost.sequential_read_units == 0 &&
        candidate.cost.scalar_score == 128, "empty model source invented rows or lost actual resident cost");
  }
  passed &= Require(ids.size() == 5, "model runtime identities alias");
  for (const auto& id : ids)
    passed &= Require((id.bytes[6] >> 4) == 7 && (id.bytes[8] & 0xc0) == 0x80,
                      "model identity is not binary UUIDv7");
  const auto stable_owner = first.identity_owner;
  std::vector<unsigned> reads(8);
  std::vector<std::thread> readers;
  for (unsigned i = 0; i != reads.size(); ++i)
    readers.emplace_back([stable_owner, native, &reads, i] {
      for (unsigned iteration = 0; iteration != 128; ++iteration) {
        const auto* value = stable_owner->Find({native.route_class, native.provider_uuid, native.capability_uuid});
        if (!value || value->alternative_uuid.is_nil() || value->cost_vector_uuid.is_nil()) return;
      }
      reads[i] = 1;
    });
  for (auto& reader : readers) reader.join();
  passed &= Require(std::ranges::all_of(reads, [](auto value) { return value == 1; }),
                    "concurrent model owner reads lost identities");
  request.identity_owner = first.identity_owner;
  std::reverse(request.capability_snapshots.begin(), request.capability_snapshots.end());
  const auto repeated = opt::BuildModelFamilyAlternativeProfilesV1(request);
  passed &= Require(repeated.accepted && repeated.identity_owner == first.identity_owner &&
      repeated.candidates[0].alternative_uuid == first.candidates[0].alternative_uuid &&
      repeated.candidates[1].cost.cost_vector_uuid == first.candidates[1].cost.cost_vector_uuid,
      "reordered model inventory did not reuse retained identities");
  const auto refuse_changed = [&](const auto& changed) {
    const auto result = opt::BuildModelFamilyAlternativeProfilesV1(changed);
    return Require(!result.accepted && !result.identity_owner && result.candidates.empty() &&
        result.candidate_inventory_receipt_uuid.is_nil(), "changed model scope reused owner or published a prefix");
  };
  using Request = opt::ModelFamilyProfileFactoryRequestV1;
  const std::vector<std::function<void(Request&)>> mutations{
    [](auto& r) { r.logical_request.operation_ids = {"DOCUMENT_PATH"}; },
    [](auto& r) { r.logical_request.operation_id = "DOCUMENT_PATH"; },
    [](auto& r) { r.logical_request.output_descriptor_ids = {1, 2}; },
    [](auto& r) { r.logical_request.object_uuid.bytes[15] ^= 1; },
    [](auto& r) { r.logical_request.bound_sblr_tree_uuid.bytes[15] ^= 1; },
    [](auto& r) { ++r.logical_request.catalog_generation; },
    [](auto& r) { ++r.logical_request.current_catalog_generation; },
    [](auto& r) { ++r.logical_request.security_epoch; },
    [](auto& r) { ++r.logical_request.policy_epoch; },
    [](auto& r) { ++r.logical_request.resource_epoch; },
    [](auto& r) { --r.logical_request.memory_budget_bytes; },
    [](auto& r) { ++r.logical_request.route_epoch; },
    [](auto& r) { ++r.logical_request.route_generation; },
    [](auto& r) { r.logical_request.security_admitted = false; },
    [](auto& r) { r.logical_request.mga_statement_context.statement_timestamp = "2026-09-12T00:00:00Z"; },
    [](auto& r) { ++r.logical_request.mga_statement_context.retention_horizon_transaction_id; },
    [](auto& r) { r.logical_request.mga_statement_context.active_excluded_local_transaction_ids.push_back(1); },
    [](auto& r) { r.logical_request.mga_statement_context.in_doubt_excluded_local_transaction_ids.clear(); },
    [](auto& r) { ++r.capability_snapshots[0].provider_generation; },
    [](auto& r) { r.capability_snapshots[0].available = false; },
    [](auto& r) { r.capability_snapshots[0].local_scope = false; },
    [](auto& r) { r.capability_snapshots[0].engine_owned = false; },
    [](auto& r) { r.capability_snapshots[0].parser_planning_authority_claimed = true; },
    [](auto& r) { r.capability_snapshots[0].transaction_finality_authority_claimed = true; },
    [](auto& r) { r.capability_snapshots[0].metrics.property_snapshot_uuid.bytes[15] ^= 1; },
    [](auto& r) { --r.capability_snapshots[0].metrics.confidence_basis_points; },
  };
  for (const auto& mutate : mutations) { auto changed = request; mutate(changed); passed &= refuse_changed(changed); }
  using Metric = opt::ModelFamilyMetricSnapshotV1;
  const std::vector<std::uint64_t Metric::*> counts{
    &Metric::statistics_generation, &Metric::startup_events, &Metric::estimated_rows,
    &Metric::sequential_pages, &Metric::random_page_lookups, &Metric::page_writes,
    &Metric::cache_operations, &Metric::working_set_bytes, &Metric::memory_grant_units,
    &Metric::spill_bytes, &Metric::network_bytes, &Metric::compressed_bytes, &Metric::encrypted_bytes,
    &Metric::predicate_evaluations, &Metric::vector_distance_evaluations, &Metric::text_score_evaluations,
    &Metric::spatial_evaluations, &Metric::udr_invocations, &Metric::mga_rechecks,
    &Metric::index_maintenance_operations, &Metric::uncertainty_events, &Metric::risk_events};
  for (const auto field : counts) {
    auto changed = request; ++(changed.capability_snapshots[0].metrics.*field);
    passed &= refuse_changed(changed);
  }
  for (unsigned bit = 0; bit != 128; ++bit) {
    auto changed = request;
    changed.capability_snapshots[0].metrics.calibration_profile_uuid.bytes[bit / 8] ^= 1u << (bit % 8);
    passed &= refuse_changed(changed);
  }
  const auto selected = opt::PlanOptimizerOwnedModelFamilySourceV1(request);
  passed &= Require(selected.accepted && selected.selected && selected.physical_dag.model_profile_identity_owner == first.identity_owner &&
      selected.selected_candidate.route_class == opt::ModelFamilyAlternativeRouteClassV1::kNative,
      "actual model coordinator did not retain owner or selected a random equal-cost route");
  auto new_scope = request; new_scope.identity_owner.reset();
  const auto fresh = opt::PlanOptimizerOwnedModelFamilySourceV1(new_scope);
  passed &= Require(fresh.accepted && fresh.selected_candidate.route_class == selected.selected_candidate.route_class &&
      fresh.candidate_inventory_receipt_uuid != selected.candidate_inventory_receipt_uuid,
      "new model scope reused IDs or randomized semantic tie-breaking");
  auto duplicate = new_scope;
  duplicate.capability_snapshots.push_back(duplicate.capability_snapshots[0]);
  passed &= refuse_changed(duplicate);
  auto invalid = new_scope;
  invalid.capability_snapshots[0].route_class = static_cast<opt::ModelFamilyAlternativeRouteClassV1>(255);
  passed &= refuse_changed(invalid);
  invalid = new_scope; invalid.logical_request.memory_budget_bytes = 1;
  passed &= refuse_changed(invalid);
  auto overflow = new_scope;
  overflow.capability_snapshots[0].metrics.estimated_rows = std::numeric_limits<std::uint64_t>::max();
  overflow.capability_snapshots[0].metrics.startup_events = 1;
  passed &= refuse_changed(overflow);
  const std::vector<std::pair<std::string, std::string>> families{
      {"document", "DOCUMENT_FIND"}, {"graph", "GRAPH_MATCH"},
      {"key_value", "KEY_VALUE_GET"}, {"time_series", "TIME_SERIES_RANGE_READ"},
      {"vector", "VECTOR_EXACT_SEARCH"}, {"search", "SEARCH_RANKED_QUERY"},
      {"spatial", "SPATIAL_SOURCE"}, {"columnar", "COLUMNAR_SOURCE"}};
  for (const auto& [family, operation] : families) {
    auto family_request = ModelRequest();
    auto& logical = family_request.logical_request;
    logical.family_id = family;
    logical.operation_id = operation;
    std::string upper = family;
    for (auto& ch : upper) if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    logical.logical_operator_id = "LOGICAL_" + upper + "_SOURCE_V1";
    if (family == "spatial" || family == "columnar") logical.operation_ids = {operation};
    if (family != "document" && family != "graph")
      logical.mga_statement_context.statement_timestamp = "2026-09-12T00:00:00.000000Z";
    const auto plan = opt::PlanOptimizerOwnedModelFamilySourceV1(family_request);
    passed &= Require(plan.accepted && plan.selected &&
        plan.physical_dag.model_profile_identity_owner && plan.selected_candidate.cost.cpu_units == 0,
        "empty model family failed actual profile-to-coordinator publication");
    if (!plan.accepted) std::cerr << family << ": " << plan.diagnostic_id << " " << plan.detail << '\n';
    family_request.capability_snapshots[0].metrics.working_set_bytes = 0;
    const auto zero_state = opt::BuildModelFamilyAlternativeProfilesV1(family_request);
    passed &= Require(zero_state.accepted && zero_state.candidates[0].cost.memory_bytes_required == 0 &&
        zero_state.candidates[0].cost.scalar_score == 0,
        "zero model working-set observation was replaced by a synthetic allocation");
  }
  overflow = new_scope;
  overflow.capability_snapshots[0].metrics.uncertainty_events = std::numeric_limits<std::uint64_t>::max();
  overflow.capability_snapshots[0].metrics.confidence_basis_points = 9999;
  passed &= refuse_changed(overflow);
  // Actual owner survives factory/request release through the published DAG.
  std::weak_ptr<const opt::ModelFamilyProfileIdentityOwnerV1> weak;
  scratchbird::engine::executor::TypedPhysicalNodeDag retained;
  {
    auto publication = opt::PlanOptimizerOwnedModelFamilySourceV1(new_scope);
    weak = publication.physical_dag.model_profile_identity_owner;
    retained = publication.physical_dag;
  }
  passed &= Require(!weak.expired(), "model DAG lost the retained runtime owner");
  retained = {};
  passed &= Require(weak.expired(), "model DAG leaked its final owner");
  // Both fresh issuance and reuse paths must publish atomically under failure.
  for (const bool reuse : {false, true}) {
    auto fault_request = reuse ? request : new_scope;
    bool finished = false;
    for (long failure = 0; failure != 4096 && !finished; ++failure) {
      std::optional<opt::ModelFamilyCoordinatorResultV1> published;
      profile_fault::remaining = failure; profile_fault::hit = false;
      try { published = opt::PlanOptimizerOwnedModelFamilySourceV1(fault_request); }
      catch (const std::bad_alloc&) {}
      profile_fault::remaining = -1;
      if (!profile_fault::hit) {
        passed &= Require(published && published->accepted &&
            published->physical_dag.model_profile_identity_owner, "model allocation sweep failed to reach publication");
        finished = true;
      } else {
        ++profile_fault::failures;
        passed &= Require(!published || (!published->accepted && !published->selected &&
            !published->physical_dag.model_profile_identity_owner && published->physical_dag.nodes.empty()),
            "model allocation failure published a partial DAG");
      }
    }
    passed &= Require(finished, "model allocation sweep incomplete");
  }
  return passed;
}

bool ValidateOwnerLifetimeAndFailure() {
  using Owner = opt::CanonicalOptimizerProfileIdentityOwner;
  const auto cap = BinaryUuid("019f0000-0000-7300-8000-000000006201");
  const std::vector<Owner::Key> keys{{2, cap}, {1, cap}};
  const std::string binding("scope\0binary", 12);
  auto owner = Owner::Create(binding, keys, 2, binding.size());
  if (!Require(owner && owner->Size() == 2, "complete owner creation failed")) return false;
  bool passed = Require(!Owner::Create(binding, {{1, cap}, {1, cap}}, 2, 128),
                        "duplicate identity keys admitted");
  passed &= Require(!Owner::Create(binding, keys, 1, 128) &&
                    !Owner::Create(binding, keys, 2, binding.size() - 1),
                    "owner count/content bounds were ignored");
  const auto stable = *owner->Find(1, cap);
  for (std::size_t i = 0; i < binding.size(); ++i) {
    auto changed = binding; changed[i] ^= 1;
    passed &= Require(!owner->Matches(changed), "embedded NUL truncated the scope binding");
  }
  std::vector<unsigned> results(8);
  std::vector<std::thread> readers;
  for (unsigned i = 0; i != 8; ++i)
    readers.emplace_back([owner, &results, i, cap, binding, stable] {
      for (unsigned round = 0; round != 128; ++round) {
        const auto* found = owner->Find(1, cap);
        if (!owner->Matches(binding) || !found ||
            found->alternative_uuid != stable.alternative_uuid ||
            found->cost_vector_uuid != stable.cost_vector_uuid) return;
      }
      results[i] = 1;
    });
  for (auto& reader : readers) reader.join();
  passed &= Require(std::ranges::all_of(results, [](auto value) { return value == 1; }),
                    "immutable owner changed during concurrent reads");
  std::weak_ptr<const Owner> weak = owner;
  scratchbird::engine::executor::TypedPhysicalNodeDag dag;
  dag.profile_identity_owner = owner;
  owner.reset();
  passed &= Require(!weak.expired(), "physical DAG did not retain the profile owner");
  auto copied = dag;
  dag.profile_identity_owner.reset();
  passed &= Require(!weak.expired(), "copied DAG lost profile ownership");
  copied.profile_identity_owner.reset();
  passed &= Require(weak.expired(), "last DAG release leaked the profile owner");

  const auto request = Request();
  const auto admission = opt::AdmitCanonicalOptimizerPlanningRequest(request);
  const auto availability = Availability(request);
  const auto calibration = BinaryUuid("019f0000-0000-7300-8000-000000006202");
  bool finished = false;
  for (long failure = 0; failure != 4096 && !finished; ++failure) {
    std::optional<opt::CanonicalOptimizerProfileFactoryResult> published;
    profile_fault::remaining = failure; profile_fault::hit = false;
    try {
      published = opt::BuildCanonicalOptimizerAlternativeProfiles(
          request, admission, availability, calibration);
    } catch (const std::bad_alloc&) {}
    profile_fault::remaining = -1;
    if (!profile_fault::hit) {
      passed &= Require(published && published->accepted && published->identity_owner,
                        "factory allocation sweep did not reach success");
      finished = true;
    } else {
      ++profile_fault::failures;
      passed &= Require(!published || (!published->accepted && !published->identity_owner &&
                         published->candidates.empty()),
                        "allocation failure published a partial owner/profile");
    }
  }
  return passed && Require(finished, "factory allocation sweep incomplete");
}
#endif

}  // namespace

#ifndef QOW_OPT_006_FIXTURE_ONLY
// QOW-TEST-OPT-006-CATALOG-V1
int main() {
  bool passed = true;
  passed &= ValidateCatalogAdmission();
  passed &= ValidateCatalogRefusals();
  passed &= ValidateCompleteMgaCarrierRefusals();
  passed &= ValidateProfileIdentityOwnership();
  passed &= ValidateOwnerLifetimeAndFailure();
  passed &= ValidateModelProfileOwnership();
  std::cout << checks << " profile/catalog checks, " << profile_fault::failures << " allocation faults\n";
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
