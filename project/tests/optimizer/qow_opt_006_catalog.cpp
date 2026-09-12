// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_catalog_backed_planning.hpp"
#include "optimizer_profile_factory.hpp"
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
  std::cout << checks << " profile/catalog checks, " << profile_fault::failures << " allocation faults\n";
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
