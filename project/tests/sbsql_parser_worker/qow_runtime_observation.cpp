// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/canonical_query_runtime_observation_support.hpp"
#include "engine/sblr/canonical_query_runtime_memory_support.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace sblr = scratchbird::engine::sblr;
namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

struct Results {
  unsigned cases = 0, failures = 0;
  void Check(bool ok, std::string_view label) {
    ++cases;
    if (!ok) { ++failures; std::cerr << "observation failed: " << label << '\n'; }
  }
};

// Component-only supplied callback carriers. No transaction/snapshot fixture
// is manufactured, and no storage or selected-DAG executor is called here.
struct Fixture {
  exec::TypedPhysicalNodeDag dag;
  exec::PhysicalNodeRecord node;
  exec::CanonicalPhysicalDispatchStepResult supplied;
  std::vector<exec::CanonicalPhysicalDispatchInput> inputs;
  sblr::OrdinaryRuntimeMemoryReceipts receipts;
  std::vector<exec::CanonicalPhysicalExecutorRegistration> registrations;

  Fixture() {
    node.physical_node_id = 7;
    node.implementation_id = "fixture.observation.v1";
    supplied.materialized_output_batch.emplace();
    supplied.rows_examined = 3;
    auto& receipt = receipts[7];
    receipt.physical_node_id = 7;
    receipt.implementation_id = node.implementation_id;
    receipt.producer_peak_memory_bytes = 100;
    receipt.producer_peak_memory_exact = true;
    receipt.residual_recheck_applicable = true;
    receipt.non_accessing_proven = true;
    registrations.emplace_back();
    auto& registration = registrations.front();
    registration.engine_owned = true;
    registration.implementation_id = node.implementation_id;
    registration.retained_live_memory_bytes_v1 = 64;
    registration.execute = [this](const auto&, const auto&, const auto&) {
      return supplied;
    };
  }
  void Wrap() { sblr::PublishOrdinaryRuntimeObservations(&registrations, receipts); }
  auto Execute() { return registrations.front().execute(dag, node, inputs); }
};

void Eligibility(Results& results) {
  for (unsigned mask = 0; mask < 128; ++mask) {
    Fixture f;
    auto& r = f.registrations.front();
    r.engine_owned = mask & 1;
    if (!(mask & 2)) r.execute = {};
    f.receipts[7].producer_peak_memory_exact = mask & 4;
    if (mask & 8) r.implementation_id = "scan.heap.fixture.v1";
    r.publishes_runtime_observation_v1 = mask & 16;
    r.honors_dispatcher_memory_limit_v1 = mask & 32;
    if (mask & 64) f.receipts[7].implementation_id = "different.v1";
    const bool eligible = mask == 7;
    results.Check(sblr::HasOrdinaryRuntimeObservationWrapperTarget(
        f.registrations, f.receipts) == eligible, "eligibility " + std::to_string(mask));
    f.Wrap();
    results.Check(r.publishes_runtime_observation_v1 == (eligible || (mask & 16)) &&
        (r.retained_live_memory_bytes_v1 > 64) == eligible,
        "wrapping " + std::to_string(mask));
  }
}

void StaticReceipts(Results& results) {
  Fixture f;
  sblr::PublishOrdinaryRuntimeObservations(nullptr, f.receipts);
  f.Wrap();
  const auto retained = f.registrations.front().retained_live_memory_bytes_v1;
  f.Wrap();
  results.Check(retained > 64 && retained == f.registrations.front().retained_live_memory_bytes_v1 &&
      !sblr::HasOrdinaryRuntimeObservationWrapperTarget(f.registrations, f.receipts), "idempotent wrapper");
  auto step = f.Execute();
  const auto& observation = step.runtime_observation;
  using State = exec::CanonicalRuntimeMetricState;
  results.Check(step.diagnostic.ok && observation.producer_receipt_complete &&
      observation.current_memory_bytes.state == State::kObserved &&
      observation.current_memory_bytes.value == 1 &&
      observation.peak_memory_bytes.state == State::kObserved &&
      observation.peak_memory_bytes.value == 100 &&
      observation.residual_recheck_count.value == 3 &&
      observation.pages_read.state == State::kNotApplicable &&
      observation.elapsed_ns.state == State::kUnavailable, "exact producer metrics, not optimizer grant");
  results.Check(step.data_access_observation_known && !step.data_access_observed &&
      observation.authority.engine_execution_observation &&
      !observation.authority.owns_transaction_finality && !observation.authority.owns_visibility &&
      !observation.authority.owns_feedback, "observation-only authority");
  f.supplied.pages_read = 4;
  f.supplied.spill_bytes = 8;
  f.supplied.data_access_observation_known = true;
  f.supplied.data_access_observed = true;
  f.receipts[7].residual_recheck_applicable = false;
  step = f.Execute();
  results.Check(step.runtime_observation.pages_read.state == State::kObserved &&
      step.runtime_observation.pages_read.value == 4 &&
      step.runtime_observation.spill_bytes_written.value == 8 &&
      step.runtime_observation.residual_recheck_count.state == State::kNotApplicable &&
      step.data_access_observed, "existing access observations preserved");

  const std::function<void(Fixture&)> mutations[] = {
      [](auto& x) { x.receipts.clear(); },
      [](auto& x) { x.receipts[7].implementation_id = "wrong.v1"; },
      [](auto& x) { x.receipts[7].physical_node_id = 8; },
      [](auto& x) { x.receipts[7].producer_peak_memory_exact = false; },
      [](auto& x) { x.supplied.materialized_output_batch.reset(); },
      [](auto& x) { x.receipts[7].producer_peak_memory_bytes = 0; },
      [](auto& x) { x.receipts[7].accounted_auxiliary_memory_bytes = 100; }};
  for (const auto& mutate : mutations) {
    Fixture x;
    x.Wrap();
    mutate(x);
    auto refused = x.Execute();
    results.Check(!refused.diagnostic.ok &&
        refused.diagnostic.diagnostic_code == "QOW-DIAG-OPT-017-REFUSAL-V1" &&
        !refused.runtime_observation.producer_receipt_complete, "receipt drift refused");
  }
  Fixture failed;
  failed.supplied.diagnostic.ok = false;
  failed.supplied.diagnostic.detail = "original callback failure";
  failed.Wrap();
  auto refused = failed.Execute();
  results.Check(!refused.diagnostic.ok && refused.diagnostic.detail == "original callback failure" &&
      !refused.runtime_observation.producer_receipt_complete, "callback failure preserved");
  for (const auto bytes : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {
    Fixture x;
    x.registrations.front().retained_live_memory_bytes_v1 = bytes;
    x.Wrap();
    results.Check(x.registrations.front().retained_live_memory_bytes_v1 == 0,
                  "unknown or overflowing retained memory remains unavailable");
  }
}

void DynamicReceipts(Results& results) {
  using Kind = exec::PhysicalNodeKind;
  struct Case { Kind kind; const char* implementation; unsigned inputs; std::uint64_t work; bool copied; };
  std::uint64_t join_work = 0;
  results.Check(exec::BoundCanonicalJoinRetainedStateBytes(3, 2, &join_work), "join bound fixture");
  const Case cases[] = {
      {Kind::kFilter, "filter.3vl.row.v1", 1, 3 * sizeof(api::EngineSqlTruthValue), false},
      {Kind::kSort, "sort.fixture.v1", 1, 9 + 3 * sizeof(std::size_t), false},
      {Kind::kJoin, "join.cross.3vl.nested.v1", 2, join_work, false},
      {Kind::kJoin, "join.inner.3vl.nested.v1", 2, join_work, false},
      {Kind::kJoin, "join.left-outer.3vl.nested.v1", 2, join_work, false},
      {Kind::kJoin, "join.right-outer.3vl.nested.v1", 2, join_work, false},
      {Kind::kJoin, "join.full-outer.3vl.nested.v1", 2, join_work, false},
      {Kind::kJoin, "join.left-semi.3vl.nested.v1", 2, join_work, false},
      {Kind::kJoin, "join.left-anti.3vl.nested.v1", 2, join_work, false},
      {Kind::kJoin, "join.lateral-inner.correlated.typed.v1", 2, 0, false},
      {Kind::kJoin, "join.lateral-left.correlated.typed.v1", 2, 0, false},
      {Kind::kJoin, "join.cross-apply.correlated.typed.v1", 2, 0, false},
      {Kind::kJoin, "join.outer-apply.correlated.typed.v1", 2, 0, false},
      {Kind::kJoin, "join.custom.fixture.v1", 2, 6 * sizeof(api::EngineSqlTruthValue), false},
      {Kind::kAggregate, "aggregate.query-distinct.typed.v1", 1, 3 * sizeof(std::size_t), false},
      {Kind::kAggregate, "aggregate.count-star.v1", 1, sizeof(std::int64_t), false},
      {Kind::kSubquery, "subquery.fixture.v1", 1, 0, false},
      {Kind::kSubquery, "subquery.fixture.v1", 1, 1, true},
      {Kind::kCte, "cte.fixture.v1", 1, 0, false},
      {Kind::kCte, "cte.fixture.v1", 1, 1, true}};
  for (const auto& c : cases) {
    Fixture f;
    f.node.node_kind = c.kind;
    f.node.implementation_id = c.implementation;
    f.registrations.front().implementation_id = c.implementation;
    f.receipts[7].implementation_id = c.implementation;
    f.receipts[7].producer_peak_from_runtime_batches = true;
    f.receipts[7].accounted_auxiliary_from_first_input_batch = c.copied;
    f.inputs.resize(c.inputs);
    for (unsigned i = 0; i < c.inputs; ++i) {
      f.inputs[i].materialized_output_batch.emplace();
      f.inputs[i].materialized_output_batch->rows.resize(i == 0 ? 3 : 2);
    }
    f.Wrap();
    auto step = f.Execute();
    results.Check(step.diagnostic.ok && step.runtime_observation.producer_receipt_complete &&
        step.runtime_observation.peak_memory_bytes.value == 1 + c.inputs + c.work, c.implementation);
    if (std::string_view(c.implementation).find("correlated") != std::string_view::npos) {
      // A category marker is sufficient for this memory-only component check;
      // comparison execution and catalog authority are not exercised here.
      for (auto& input : f.inputs) {
        input.materialized_output_batch->columns.resize(1);
        input.materialized_output_batch->columns.front().descriptor.encoded_descriptor =
            "timezone_profile_id=fixture";
      }
      step = f.Execute();
      results.Check(step.diagnostic.ok && step.runtime_observation.peak_memory_bytes.value ==
          3 + 6 * sizeof(std::optional<int>), "descriptor-bound correlation retains comparison carriers");
    }
    if (!f.inputs.empty()) {
      f.inputs.front().materialized_output_batch.reset();
      step = f.Execute();
      results.Check(!step.diagnostic.ok && step.diagnostic.detail ==
          "runtime_observation_dynamic_input_peak", "missing dynamic batch refused");
    }
  }
  Fixture overflow;
  overflow.node.node_kind = Kind::kAggregate;
  overflow.receipts[7].producer_peak_from_runtime_batches = true;
  overflow.receipts[7].accounted_auxiliary_memory_bytes = std::numeric_limits<std::uint64_t>::max();
  overflow.Wrap();
  const auto step = overflow.Execute();
  results.Check(!step.diagnostic.ok && step.diagnostic.detail ==
      "runtime_observation_dynamic_work_peak", "dynamic work overflow refused");
  for (const auto kind : {Kind::kSubquery, Kind::kAggregate}) {
    Fixture f;
    f.node.node_kind = kind;
    f.node.implementation_id = kind == Kind::kAggregate
        ? "aggregate.query-distinct.typed.v1" : "subquery.fixture.v1";
    f.registrations.front().implementation_id = f.node.implementation_id;
    f.receipts[7].implementation_id = f.node.implementation_id;
    f.receipts[7].producer_peak_from_runtime_batches = true;
    f.receipts[7].accounted_auxiliary_from_first_input_batch = true;
    f.inputs.resize(2);
    for (auto& input : f.inputs) input.materialized_output_batch.emplace();
    f.Wrap();
    const auto refused = f.Execute();
    results.Check(!refused.diagnostic.ok && refused.diagnostic.detail ==
        "runtime_observation_dynamic_work_peak", "wrong dynamic input arity refused");
  }
}

}  // namespace

int main() {
  Results results;
  Eligibility(results);
  StaticReceipts(results);
  DynamicReceipts(results);
  std::cout << "runtime_observation_cases=" << results.cases << " failures=" << results.failures << '\n';
  return results.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
