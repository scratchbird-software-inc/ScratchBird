// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// Actual collector/planner/store component tests. The typed metadata and metric
// callbacks below are fixtures, not storage scans or SQL/IPC acceptance.
#define QOW_OPT_006_FIXTURE_ONLY
#include "../optimizer/qow_opt_006_catalog.cpp"
#include "optimizer_prepare_metric_collector.hpp"
#include "relational_planner.hpp"
#include <atomic>
#include <stdexcept>

namespace metric_fault {
std::atomic<long> remaining{-1};
std::atomic<bool> hit{false};
}
void* operator new(std::size_t size) {
  auto count = metric_fault::remaining.load();
  while (count >= 0) {
    if (count == 0) {
      metric_fault::hit = true;
      throw std::bad_alloc();
    }
    if (metric_fault::remaining.compare_exchange_weak(count, count - 1)) break;
  }
  if (void* value = std::malloc(size ? size : 1)) return value;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
using Id = scratchbird::engine::internal_api::EngineUuid;
Id Identity(std::uint64_t n) {
  Id id{};
  id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80;
  for (unsigned i = 0; i != 6; ++i) { id.bytes[15-i] = n & 255; n >>= 8; }
  return id;
}

opt::CanonicalPreparePhysicalPlanRequest PreparedFixture(bool mga_capable = true) {
  opt::RelationalDagPlanningInput input;
  input.admission_request = Request();
  input.admission = opt::AdmitCanonicalOptimizerPlanningRequest(input.admission_request);
  auto& availability = input.executor_availability;
  availability.engine_owned = true;
  auto& catalog = availability.capability_catalog;
  catalog.engine_owned = true;
  catalog.capability_snapshot_uuid =
      input.admission_request.policy_capability.capability_snapshot_uuid;
  catalog.policy_epoch = input.admission_request.policy_capability.policy_epoch;
  opt::CanonicalExecutorCapabilityRecord capability;
  capability.capability_uuid = Identity(10);
  capability.capability_abi_version = 1;
  capability.implementation_id = "scan.local";
  capability.logical_node_kind = plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  capability.physical_node_kind = scratchbird::engine::executor::PhysicalNodeKind::kScan;
  capability.maximum_memory_bytes = 4096;
  capability.storage_read_capable = true;
  capability.mga_visibility_capable = mga_capable;
  capability.available = capability.engine_owned = true;
  catalog.capabilities = {capability};
  availability.node_bindings = {{1, capability.capability_uuid, 128, true, {}}};
  input.search_policy = {1, 16, 4, 1, true};
  input.publication_identity.selected_plan_uuid = Identity(11);
  input.publication_identity.first_causal_counter_id = 1;
  input.publication_identity.engine_owned = true;
  input.calibration_profile_uuid = Identity(12);
  const auto planned = opt::PlanCanonicalRelationalDag(input);
  if (!mga_capable) {
    if (planned.accepted || planned.diagnostics.empty())
      throw std::runtime_error("zero cost bypassed MGA capability validation");
    return {};
  }
  if (!planned.accepted) {
    for (const auto& issue : planned.diagnostics) std::cerr << issue << '\n';
    for (const auto& issue : planned.search.issues) std::cerr << "search: " << issue.field_id << '\n';
    for (const auto& issue : planned.publication.issues) std::cerr << "publish: " << issue.field_id << '\n';
    throw std::runtime_error("actual planner fixture failed");
  }
  opt::CanonicalPreparePhysicalPlanRequest result;
  result.prepared_plan_uuid = Identity(20);
  result.prepare_generation = 1;
  result.parameter_shape_uuid = Identity(21);
  result.result_schema_uuid = Identity(22);
  result.selected_physical_dag = planned.publication.physical_dag;
  opt::CanonicalPreparedPlanResultDescriptor descriptor;
  descriptor.ordinal = descriptor.descriptor_id = 1;
  descriptor.descriptor_uuid = Identity(23);
  descriptor.type_uuid = Identity(24);
  descriptor.type_modifier_digest = std::string(64, 'a');
  result.result_descriptors = {descriptor};
  result.dependencies = {
    {opt::CanonicalPreparedPlanDependencyKind::kObject, BinaryUuid(kRelation), 17, std::string(64, 'b')},
    {opt::CanonicalPreparedPlanDependencyKind::kDatatype, Identity(24), 1, std::string(64, 'c')}};
  result.engine_prepare_authorized = true;
  return result;
}

struct Calls {
  std::atomic<unsigned> collected{0}, planned{0}, cleaned{0}, assembled{0};
};
opt::CanonicalPrepareWithMetricCollectionRequest MetricFixture(
    const opt::CanonicalPreparePhysicalPlanRequest& prepared, Calls& calls,
    unsigned count = 12) {
  opt::CanonicalPrepareWithMetricCollectionRequest request;
  request.coordinator_policy_uuid = Identity(30);
  request.coordinator_policy_generation = 1;
  request.bound_sblr_tree_uuid = prepared.selected_physical_dag.bound_sblr_tree_uuid;
  request.route_snapshot_uuid = prepared.selected_physical_dag.route_snapshot_uuid;
  request.route_epoch = prepared.selected_physical_dag.route_epoch;
  request.route_generation = prepared.selected_physical_dag.route_generation;
  request.cluster_scope_id = "local_only";
  request.metric_thread_budget = 3;
  request.timeout_ns = 60'000'000'000;
  request.engine_prepare_authorized = request.global_security_admitted =
      request.global_mga_admitted = true;
  for (unsigned i = 0; i != count; ++i) {
    opt::CanonicalPrepareMetricLegRequest leg;
    leg.leg_uuid = Identity(100 + i);
    leg.family_id = i % 2 ? "graph" : "relational";
    if (i >= 3) leg.dependency_leg_uuids = {Identity(100 + i - 3)};
    leg.required_metric_ids = opt::CanonicalRequiredPrepareMetricIdsForFamily(leg.family_id);
    leg.collect_metrics = [i, &calls](const auto& context) {
      ++calls.collected;
      if (context.completed_dependency_plans.size() != (i >= 3 ? 1 : 0)) {
        throw std::runtime_error("missing dependency plan");
      }
      opt::CanonicalPrepareMetricCollectionOutput output;
      output.collected = true;
      output.metric_snapshot_uuid = Identity(200 + i);
      output.metric_snapshot_generation = 3;
      for (const auto& metric : context.required_metric_ids) {
        output.metrics.push_back({metric, "fixture_units", 0, Identity(200+i), 3});
      }
      return output;
    };
    leg.plan_leg = [i, &calls](const auto&) {
      ++calls.planned;
      opt::CanonicalPrepareLegPlanningOutput output;
      output.planned = true;
      output.selected_leg_plan_uuid = Identity(300+i);
      output.selected_alternative_uuid = Identity(400+i);
      output.family_local_cost_vector_uuid = Identity(500+i);
      output.retained_alternative_uuids = {Identity(400+i), Identity(600+i)};
      return output;
    };
    leg.cleanup_transient_state = [&calls] { ++calls.cleaned; return true; };
    request.legs.push_back(std::move(leg));
  }
  request.assemble_selected_plan = [prepared, &calls](const auto&, const auto&, const auto&) {
    ++calls.assembled;
    return prepared;
  };
  return request;
}

bool RefusedClean(const opt::CanonicalPrepareWithMetricCollectionResult& r,
                  const opt::CanonicalPreparedPlanStore& store, const Calls& calls) {
  return Require(!r.accepted && !r.prepared && !r.persisted && store.Size() == 0 &&
                 !r.prepare_result.prepared_plan && r.metric_receipts.empty() &&
                 r.leg_plan_receipts.empty() && r.metric_receipts.capacity() == 0 &&
                 r.leg_plan_receipts.capacity() == 0 && r.cleanup_invocation_count == calls.cleaned,
                 "failure published partial state or lost cleanup accounting");
}
}

int RunMetricTests() {
  const auto prepared = PreparedFixture();
  bool passed = true;
  passed &= Require(PreparedFixture(false).selected_physical_dag.nodes.empty(),
                    "missing MGA capability was admitted");
  passed &= Require(prepared.selected_physical_dag.nodes.front().retained_cost.mga_visibility_checks_expected == 0,
                    "known-zero scan was coerced to positive MGA cost");
  Calls calls;
  auto request = MetricFixture(prepared, calls);
  opt::CanonicalPreparedPlanStore first_store, replay_store;
  const auto first = opt::PrepareCanonicalPhysicalPlanWithMetricCollection(request, &first_store);
  if (!first.accepted) for (const auto& issue : first.issues) std::cerr << issue.field_id << '\n';
  passed &= Require(first.accepted && first.metric_receipts.size() == 12 &&
                    first.all_workers_joined && first.transient_state_cleaned &&
                    first.maximum_observed_concurrency <= 3 &&
                    calls.collected == 12 && calls.planned == 12 && calls.cleaned == 12 &&
                    first_store.Find(prepared.prepared_plan_uuid) == first.prepare_result.prepared_plan,
                    "repeated-family twelve-leg plan was not actually stored");
  if (!first.accepted) return EXIT_FAILURE;
  passed &= Require(first.prepare_result.prepared_plan->profile_identity_owner ==
      prepared.selected_physical_dag.profile_identity_owner &&
      first.prepare_result.prepared_plan->profile_identity_owner != nullptr,
      "prepared plan dropped its selected profile identity owner");
  const auto replay = opt::PrepareCanonicalPhysicalPlanWithMetricCollection(request, &replay_store);
  passed &= Require(replay.accepted, "repeat collection refused");
  if (!replay.accepted) return EXIT_FAILURE;
  passed &= Require(first.metric_receipts.front().dependency_definition_digest ==
      "2709217dcb5776b43ccf36297140eb61d63a1123a91c8cad0063c7d50799c6bc",
      "metric binding disagrees with independently encoded SHA-256 fixture");
  std::set<Id> identities;
  for (unsigned i = 0; i != 12; ++i) {
    const auto& a = first.metric_receipts[i];
    const auto& b = replay.metric_receipts[i];
    passed &= Require(a.collection_receipt_uuid != b.collection_receipt_uuid &&
        a.dependency_definition_digest == b.dependency_definition_digest &&
        a.stable_leg_ordinal == i + 1 && a.dependency_wave == i / 3 &&
        a.metrics.front().unsigned_value == 0,
        "receipt identity is a hash, binding is unstable, or known zero was lost");
    identities.insert(a.collection_receipt_uuid);
    identities.insert(first.leg_plan_receipts[i].planning_receipt_uuid);
  }
  passed &= Require(identities.size() == 24 &&
      std::ranges::all_of(identities, scratchbird::core::uuid::IsEngineIdentityUuid),
      "receipt identities are not distinct binary UUIDv7");

  for (unsigned scenario = 0; scenario != 12; ++scenario) {
    Calls c; opt::CanonicalPreparedPlanStore store;
    auto r = MetricFixture(prepared, c, 1);
    if (scenario == 0) r.legs[0].leg_uuid.bytes[6] = 0x40;
    if (scenario == 1) r.legs[0].dependency_leg_uuids = {r.legs[0].leg_uuid};
    if (scenario == 2) r.legs[0].collect_metrics = [](const auto&) ->
        opt::CanonicalPrepareMetricCollectionOutput { throw std::bad_alloc(); };
    if (scenario == 3) r.legs[0].plan_leg = [](const auto&) ->
        opt::CanonicalPrepareLegPlanningOutput { throw std::runtime_error("fixture"); };
    if (scenario == 4) r.legs[0].cleanup_transient_state = [&c] { ++c.cleaned; return false; };
    if (scenario == 5) r.timeout_ns = 1;
    if (scenario == 6) r.timeout_ns = std::numeric_limits<std::uint64_t>::max();
    if (scenario == 7) {
      r.cancellation_requested = [&c] { return c.assembled != 0; };
    }
    if (scenario == 8) r.cancellation_requested = []() -> bool { throw std::runtime_error("cancel"); };
    if (scenario == 9) r.legs[0].cleanup_transient_state = [&c]() -> bool {
      ++c.cleaned; throw std::bad_alloc();
    };
    unsigned cancellation_probes = 0;
    if (scenario == 11) r.cancellation_requested = [&] {
      return ++cancellation_probes == 1;
    };
    if (scenario == 10) r.assemble_selected_plan = [](const auto&, const auto&, const auto&) ->
        opt::CanonicalPreparePhysicalPlanRequest { throw std::bad_alloc(); };
    const auto out = opt::PrepareCanonicalPhysicalPlanWithMetricCollection(r, &store);
    passed &= RefusedClean(out, store, c);
  }


  // Changes to separate counted/framed inputs must not alias, including NULs.
  for (unsigned field = 0; field != 6; ++field) {
    Calls c; opt::CanonicalPreparedPlanStore store;
    auto r = MetricFixture(prepared, c, 1);
    r.metric_thread_budget = 65;
    r.timeout_ns = 90'000'000'000;
    if (field == 0) ++r.coordinator_policy_generation;
    if (field == 1) r.cluster_scope_id = std::string("local_only\0|leg:", 16);
    if (field == 2) r.coordinator_policy_uuid.bytes[1] ^= 1;
    if (field >= 3) {
      auto collect = r.legs[0].collect_metrics;
      r.legs[0].collect_metrics = [collect, field](const auto& context) {
        auto output = collect(context);
        if (field == 3) ++output.metrics[0].unsigned_value;
        if (field == 4) ++output.metrics[0].source_generation;
        if (field == 5) output.metrics[0].unit_id = std::string("fixture_units\0|m:", 17);
        return output;
      };
    }
    const auto out = opt::PrepareCanonicalPhysicalPlanWithMetricCollection(r, &store);
    passed &= Require(out.accepted && out.metric_receipts.front().dependency_definition_digest !=
        first.metric_receipts.front().dependency_definition_digest,
        "changed metric binding aliased its original or policy budget was capped arbitrarily");
  }

  // The actual prepared store checks cancellation after all store allocation.
  for (unsigned mode = 0; mode != 2; ++mode) {
    auto direct = prepared;
    unsigned probes = 0;
    direct.publication_cancelled = [&]() -> bool {
      ++probes;
      if (mode) throw std::bad_alloc();
      return true;
    };
    opt::CanonicalPreparedPlanStore store;
    try {
      const auto out = opt::PrepareCanonicalPhysicalPlan(direct, &store);
      passed &= Require(!out.accepted && !out.prepared_plan, "cancelled direct PREPARE published");
    } catch (const std::bad_alloc&) {
      passed &= Require(mode == 1, "unexpected store allocation refusal");
    }
    passed &= Require(probes == 1 && store.Size() == 0,
                      "final publication probe was skipped or left a plan");
  }

  unsigned faults = 0;
  for (long point = 0; point != 4000; ++point) {
    Calls c; opt::CanonicalPreparedPlanStore store;
    auto r = MetricFixture(prepared, c, 1);
    metric_fault::hit = false; metric_fault::remaining = point;
    const auto out = opt::PrepareCanonicalPhysicalPlanWithMetricCollection(r, &store);
    metric_fault::remaining = -1;
    if (!metric_fault::hit) {
      passed &= Require(out.accepted, "allocation sweep never reached actual publication");
      break;
    }
    ++faults;
    passed &= RefusedClean(out, store, c);
    passed &= Require(!out.accepted && (c.cleaned == 0 || out.all_workers_joined),
                      "allocation failure escaped worker cleanup");
    if (point == 3999) passed &= Require(false, "allocation sweep did not terminate");
  }
  std::cout << "prepare metric component checks=" << checks << " allocation_faults=" << faults << '\n';
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

int main() {
  try { return RunMetricTests(); }
  catch (const std::exception& e) { metric_fault::remaining = -1; std::cerr << e.what() << '\n'; return EXIT_FAILURE; }
}
