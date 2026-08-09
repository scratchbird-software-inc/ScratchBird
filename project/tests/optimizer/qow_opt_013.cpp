// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// QOW-TEST-OPT-013-V1

#include "optimizer_feedback.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

int failures = 0;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "         \
                << #expression << '\n';                                      \
      ++failures;                                                             \
    }                                                                         \
  } while (false)

std::string Uuid(const std::uint64_t value) {
  std::ostringstream out;
  out << "10000000-0000-0000-0000-" << std::hex << std::setfill('0')
      << std::setw(12) << value;
  return out.str();
}

opt::OptimizerGovernedFeedbackMetric Observed(const std::uint64_t value) {
  return {opt::OptimizerGovernedFeedbackMetricState::kObserved, value};
}

opt::OptimizerGovernedFeedbackMetric NotApplicable() {
  return {opt::OptimizerGovernedFeedbackMetricState::kNotApplicable, 0};
}

opt::OptimizerGovernedFeedbackPolicy Policy() {
  opt::OptimizerGovernedFeedbackPolicy policy;
  policy.deviation_threshold_basis_points = 1000;
  policy.required_compatible_samples = 2;
  policy.maximum_samples_per_identity = 3;
  policy.maximum_retained_identities = 16;
  policy.maximum_quarantine_records = 32;
  policy.minimum_multiplier_basis_points = 2500;
  policy.maximum_multiplier_basis_points = 40000;
  policy.maximum_advisory_rows = 1000000;
  policy.maximum_advisory_memory_bytes = 1000000;
  return policy;
}

opt::OptimizerGovernedFeedbackObservation Observation(
    const std::uint64_t sequence, const std::uint64_t identity_seed = 1,
    const std::uint64_t observation_seed = 0) {
  opt::OptimizerGovernedFeedbackObservation observation;
  observation.identity.observation_uuid =
      Uuid(observation_seed == 0 ? 100000 + sequence : observation_seed);
  observation.identity.selected_plan_uuid = Uuid(200000 + identity_seed);
  observation.identity.selected_plan_signature =
      "plan-signature-" + std::to_string(identity_seed);
  observation.identity.physical_node_id = 1000 + identity_seed;
  observation.identity.logical_node_id =
      static_cast<std::uint32_t>(100 + identity_seed);
  observation.identity.causal_counter_id = 2000 + identity_seed;
  observation.identity.implementation_id =
      "implementation-" + std::to_string(identity_seed);
  observation.identity.selected_alternative_uuid = Uuid(300000 + identity_seed);
  observation.identity.output_descriptor_digest =
      "descriptor-digest-" + std::to_string(identity_seed);
  observation.identity.result_identity_digest =
      "result-digest-" + std::to_string(identity_seed);
  observation.identity.dependency_signature =
      "dependency-signature-" + std::to_string(identity_seed);

  observation.generations = {1, 1, 1, 1, 1, 1, 1, 1};
  observation.authority.engine_execution_observation = true;
  observation.authority.producer_receipt_complete = true;
  observation.authority.counters_frozen_after_finish = true;
  observation.authority.estimates_frozen_before_access = true;
  observation.authority.plan_identity_verified = true;
  observation.authority.node_causal_identity_verified = true;
  observation.authority.implementation_alternative_identity_verified = true;
  observation.authority.descriptor_identity_verified = true;
  observation.authority.result_identity_verified = true;
  observation.authority.dependency_identity_verified = true;
  observation.authority.mga_statement_context_verified = true;
  observation.authority.security_recheck_preserved = true;
  observation.authority.donor_semantics_preserved = true;

  auto& metrics = observation.metrics;
  metrics.estimated_input_rows = Observed(100);
  metrics.estimated_output_rows = Observed(100);
  metrics.actual_input_rows = Observed(300);
  metrics.actual_output_rows = Observed(300);
  metrics.actual_rows_examined = Observed(300);
  metrics.elapsed_ns = Observed(1000);
  metrics.operator_wait_ns = Observed(100);
  metrics.current_memory_bytes = Observed(500);
  metrics.peak_memory_bytes = Observed(1000);
  metrics.estimated_pages = Observed(1);
  metrics.decoded_bytes = Observed(3000);
  metrics.bytes_read = Observed(2000);
  metrics.bytes_written = Observed(0);
  metrics.pages_read = Observed(3);
  metrics.pages_written = NotApplicable();
  metrics.spill_bytes_read = NotApplicable();
  metrics.spill_bytes_written = NotApplicable();
  metrics.visibility_rechecks = Observed(0);
  metrics.security_rechecks = Observed(0);
  metrics.storage_rechecks = Observed(0);
  metrics.index_rechecks = Observed(0);
  metrics.residual_rechecks = Observed(0);
  metrics.compatibility_rechecks = Observed(0);
  metrics.archive_bytes_read = NotApplicable();
  metrics.cluster_bytes_sent = NotApplicable();
  metrics.cluster_bytes_received = NotApplicable();
  metrics.estimated_resource_units = Observed(100);
  metrics.actual_resource_units = Observed(300);

  observation.outcome = opt::OptimizerGovernedFeedbackOutcome::kSucceeded;
  observation.observation_sequence = sequence;
  observation.policy_allowed = true;
  observation.quality_improvement_observed = true;
  return observation;
}

opt::OptimizerGovernedFeedbackObservation Admit(
    opt::OptimizerGovernedFeedbackStore* store,
    const opt::OptimizerGovernedFeedbackPolicy& policy,
    const std::uint64_t identity_seed = 1,
    const std::uint64_t first_sequence = 1) {
  auto first = Observation(first_sequence, identity_seed,
                           1000000 + identity_seed * 100 + first_sequence);
  const auto pending = store->Publish(first, policy);
  CHECK(pending.accepted);
  CHECK(pending.disposition ==
        opt::OptimizerGovernedFeedbackDisposition::kPending);
  auto second = Observation(first_sequence + 1, identity_seed,
                            1000000 + identity_seed * 100 + first_sequence + 1);
  second.metrics.actual_output_rows = Observed(320);
  second.metrics.peak_memory_bytes = Observed(1200);
  const auto admitted = store->Publish(second, policy);
  CHECK(admitted.accepted);
  CHECK(admitted.disposition ==
        opt::OptimizerGovernedFeedbackDisposition::kAdmitted);
  return second;
}

opt::OptimizerGovernedFeedbackConsumptionRequest Consumption(
    const opt::OptimizerGovernedFeedbackObservation& source,
    const std::uint64_t planning_sequence = 10000) {
  opt::OptimizerGovernedFeedbackConsumptionRequest request;
  request.source_identity = source.identity;
  request.current_generations = source.generations;
  request.planning_attempt_sequence = planning_sequence;
  request.later_planning_attempt = true;
  request.policy_allowed = true;
  request.alternative.candidate_alternative_uuid = Uuid(900001);
  request.alternative.exact_fallback_alternative_uuid =
      source.identity.selected_alternative_uuid;
  request.alternative.identical_logical_semantics = true;
  request.alternative.identical_output_descriptors = true;
  request.alternative.identical_required_properties = true;
  request.alternative.identical_delivered_properties = true;
  request.alternative.identical_security_behavior = true;
  request.alternative.identical_mga_behavior = true;
  request.alternative.identical_donor_compatibility = true;
  request.alternative.compatible_capability = true;
  request.alternative.exact_fallback_available = true;
  return request;
}

void TestAcceptedPublicationAndConsumption() {
  const auto policy = Policy();
  opt::OptimizerGovernedFeedbackStore store;
  const auto source = Admit(&store, policy);
  const auto snapshot = store.Snapshot();
  CHECK(snapshot.retained_identity_count == 1);
  CHECK(snapshot.admitted_identity_count == 1);
  CHECK(snapshot.records.front().compatible_sample_count == 2);
  CHECK(snapshot.records.front().advisory_output_rows == 310);
  CHECK(snapshot.records.front().cardinality_multiplier_basis_points == 31000);
  CHECK(snapshot.records.front().advisory_memory_bytes == 1375);

  const auto before_actual = source.metrics.actual_output_rows;
  const auto before_plan = source.identity.selected_plan_signature;
  const auto before_generations = source.generations;
  const auto consumed = store.Consume(Consumption(source));
  CHECK(consumed.consumed);
  CHECK(!consumed.conservative_fallback_required);
  CHECK(consumed.feedback_generation == 1);
  CHECK(consumed.advisory_output_rows == 310);
  CHECK(source.metrics.actual_output_rows == before_actual);
  CHECK(source.identity.selected_plan_signature == before_plan);
  CHECK(source.generations == before_generations);
}

void TestThresholdMetricStatesAndMath() {
  const auto policy = Policy();
  {
    opt::OptimizerGovernedFeedbackStore store;
    auto below = Observation(1);
    below.metrics.actual_output_rows = Observed(105);
    const auto result = store.Publish(below, policy);
    CHECK(result.accepted);
    CHECK(result.disposition ==
          opt::OptimizerGovernedFeedbackDisposition::kIgnored);
    CHECK(store.Snapshot().retained_identity_count == 0);
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    auto zero = Observation(1);
    zero.metrics.actual_input_rows = Observed(0);
    zero.metrics.actual_output_rows = Observed(0);
    zero.metrics.actual_rows_examined = Observed(0);
    const auto result = store.Publish(zero, policy);
    CHECK(result.accepted);
    CHECK(result.disposition ==
          opt::OptimizerGovernedFeedbackDisposition::kPending);
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    auto unavailable = Observation(1);
    unavailable.metrics.actual_output_rows = {};
    const auto result = store.Publish(unavailable, policy);
    CHECK(!result.accepted);
    CHECK(result.diagnostic_code == "QOW-DIAG-OPT-013-METRIC-STATE-V1");
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    auto optional_unavailable = Observation(1);
    optional_unavailable.metrics.pages_written = {};
    CHECK(store.Publish(optional_unavailable, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-METRIC-STATE-V1");
    opt::OptimizerGovernedFeedbackStore not_applicable_store;
    auto optional_not_applicable = Observation(1);
    optional_not_applicable.metrics.pages_written = NotApplicable();
    CHECK(not_applicable_store.Publish(optional_not_applicable, policy).accepted);
  }
  {
    const std::vector<std::function<void(
        opt::OptimizerGovernedFeedbackMetrics*)>> required_core_not_applicable = {
        [](auto* m) { m->estimated_output_rows = NotApplicable(); },
        [](auto* m) { m->actual_output_rows = NotApplicable(); },
        [](auto* m) { m->elapsed_ns = NotApplicable(); },
        [](auto* m) { m->current_memory_bytes = NotApplicable(); },
        [](auto* m) { m->peak_memory_bytes = NotApplicable(); },
    };
    for (const auto& mutate : required_core_not_applicable) {
      opt::OptimizerGovernedFeedbackStore store;
      auto not_applicable_required = Observation(1);
      mutate(&not_applicable_required.metrics);
      const auto result = store.Publish(not_applicable_required, policy);
      CHECK(!result.accepted);
      CHECK(result.diagnostic_code == "QOW-DIAG-OPT-013-METRIC-STATE-V1");
    }
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    auto contradiction = Observation(1);
    contradiction.metrics.operator_wait_ns = Observed(1001);
    CHECK(store.Publish(contradiction, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-METRIC-CONTRADICTION-V1");
    contradiction = Observation(2);
    contradiction.metrics.current_memory_bytes = Observed(1001);
    CHECK(store.Publish(contradiction, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-METRIC-CONTRADICTION-V1");
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    auto overflow = Observation(1);
    overflow.metrics.decoded_bytes =
        Observed(std::numeric_limits<std::uint64_t>::max());
    overflow.metrics.bytes_read = Observed(1);
    CHECK(store.Publish(overflow, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-METRIC-OVERFLOW-V1");
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    auto huge1 = Observation(1, 1, 700001);
    auto huge2 = Observation(2, 1, 700002);
    huge1.metrics.estimated_output_rows = Observed(1);
    huge2.metrics.estimated_output_rows = Observed(1);
    huge1.metrics.actual_output_rows =
        Observed(std::numeric_limits<std::uint64_t>::max());
    huge2.metrics.actual_output_rows =
        Observed(std::numeric_limits<std::uint64_t>::max());
    CHECK(store.Publish(huge1, policy).accepted);
    const auto result = store.Publish(huge2, policy);
    CHECK(result.accepted);
    CHECK(result.cardinality_multiplier_basis_points == 40000);
    CHECK(result.advisory_output_rows <= policy.maximum_advisory_rows);
  }
}

void TestPolicyBoundsAndBoundedMalformedInput() {
  const auto base = Policy();
  std::vector<std::function<void(opt::OptimizerGovernedFeedbackPolicy*)>> invalid = {
      [](auto* p) { p->deviation_threshold_basis_points = 0; },
      [](auto* p) { p->deviation_threshold_basis_points = 1000001; },
      [](auto* p) { p->required_compatible_samples = 1; },
      [](auto* p) { p->maximum_samples_per_identity = 65; },
      [](auto* p) { p->required_compatible_samples = 4; },
      [](auto* p) { p->maximum_retained_identities = 0; },
      [](auto* p) { p->maximum_retained_identities = 1025; },
      [](auto* p) { p->maximum_quarantine_records = 0; },
      [](auto* p) { p->maximum_quarantine_records = 4097; },
      [](auto* p) { p->minimum_multiplier_basis_points = 0; },
      [](auto* p) { p->minimum_multiplier_basis_points = 10001; },
      [](auto* p) { p->maximum_multiplier_basis_points = 9999; },
      [](auto* p) { p->maximum_multiplier_basis_points = 1000001; },
      [](auto* p) {
        p->minimum_multiplier_basis_points = 9000;
        p->maximum_multiplier_basis_points = 8000;
      },
      [](auto* p) { p->maximum_advisory_rows = 0; },
      [](auto* p) { p->maximum_advisory_memory_bytes = 0; },
  };
  for (const auto& mutate : invalid) {
    opt::OptimizerGovernedFeedbackStore store;
    auto policy = base;
    mutate(&policy);
    const auto result = store.Publish(Observation(1), policy);
    CHECK(!result.accepted);
    CHECK(result.diagnostic_code == "QOW-DIAG-OPT-013-POLICY-BOUNDS-V1");
  }

  {
    opt::OptimizerGovernedFeedbackStore store;
    auto oversized = Observation(1);
    oversized.identity.dependency_signature = std::string(10000, 'x');
    const auto result = store.Publish(oversized, base);
    CHECK(!result.accepted);
    CHECK(result.identity_key == "invalid_identity");
    CHECK(store.Snapshot().quarantine_record_count == 1);
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    auto invalid_policy = base;
    invalid_policy.maximum_quarantine_records = 0;
    for (std::uint64_t sequence = 1; sequence <= 4100; ++sequence) {
      auto observation = Observation(sequence, 1, 800000 + sequence);
      store.Publish(observation, invalid_policy);
    }
    CHECK(store.Snapshot().quarantine_record_count == 4096);
  }
}

void TestOutcomeAndAuthorityRefusals() {
  const auto policy = Policy();
  const std::vector<opt::OptimizerGovernedFeedbackOutcome> outcomes = {
      opt::OptimizerGovernedFeedbackOutcome::kFailed,
      opt::OptimizerGovernedFeedbackOutcome::kCrashed,
      opt::OptimizerGovernedFeedbackOutcome::kCancelled,
      opt::OptimizerGovernedFeedbackOutcome::kRolledBack,
      opt::OptimizerGovernedFeedbackOutcome::kUnauthorized,
      opt::OptimizerGovernedFeedbackOutcome::kRedacted,
      opt::OptimizerGovernedFeedbackOutcome::kCorrupt,
      opt::OptimizerGovernedFeedbackOutcome::kPoisoned,
      opt::OptimizerGovernedFeedbackOutcome::kPolicyQuarantined,
  };
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    opt::OptimizerGovernedFeedbackStore store;
    auto observation = Observation(index + 1);
    observation.outcome = outcomes[index];
    const auto result = store.Publish(observation, policy);
    CHECK(!result.accepted);
    if (outcomes[index] == opt::OptimizerGovernedFeedbackOutcome::kUnauthorized ||
        outcomes[index] == opt::OptimizerGovernedFeedbackOutcome::kRedacted) {
      CHECK(result.disposition ==
            opt::OptimizerGovernedFeedbackDisposition::kDiagnosticOnly);
    } else {
      CHECK(result.disposition ==
            opt::OptimizerGovernedFeedbackDisposition::kQuarantined);
    }
  }

  using Authority = opt::OptimizerGovernedFeedbackAuthorityReceipt;
  const std::vector<bool Authority::*> forbidden = {
      &Authority::owns_execution,
      &Authority::owns_mga_visibility,
      &Authority::owns_transaction_finality,
      &Authority::owns_security,
      &Authority::owns_parser,
      &Authority::owns_reference,
      &Authority::owns_donor,
      &Authority::owns_recovery,
      &Authority::owns_wal,
      &Authority::owns_benchmark,
      &Authority::owns_release,
      &Authority::owns_cluster,
  };
  for (std::size_t index = 0; index < forbidden.size(); ++index) {
    opt::OptimizerGovernedFeedbackStore store;
    auto observation = Observation(index + 1);
    observation.authority.*forbidden[index] = true;
    CHECK(store.Publish(observation, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-FORBIDDEN-AUTHORITY-V1");
  }

  const std::vector<bool Authority::*> required = {
      &Authority::engine_execution_observation,
      &Authority::producer_receipt_complete,
      &Authority::counters_frozen_after_finish,
      &Authority::estimates_frozen_before_access,
      &Authority::plan_identity_verified,
      &Authority::node_causal_identity_verified,
      &Authority::implementation_alternative_identity_verified,
      &Authority::descriptor_identity_verified,
      &Authority::result_identity_verified,
      &Authority::dependency_identity_verified,
      &Authority::mga_statement_context_verified,
      &Authority::security_recheck_preserved,
      &Authority::donor_semantics_preserved,
  };
  for (std::size_t index = 0; index < required.size(); ++index) {
    opt::OptimizerGovernedFeedbackStore store;
    auto observation = Observation(index + 1);
    observation.authority.*required[index] = false;
    CHECK(store.Publish(observation, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-UNFROZEN-OBSERVATION-V1");
  }
}

void TestExactIdentityGenerationAndAlternativeMatching() {
  const auto policy = Policy();
  opt::OptimizerGovernedFeedbackStore store;
  const auto source = Admit(&store, policy);

  std::vector<std::function<void(opt::OptimizerGovernedFeedbackIdentity*)>>
      identity_mismatches = {
          [](auto* id) { id->selected_plan_uuid = Uuid(999001); },
          [](auto* id) { id->selected_plan_signature += "-stale"; },
          [](auto* id) { ++id->physical_node_id; },
          [](auto* id) { ++id->logical_node_id; },
          [](auto* id) { ++id->causal_counter_id; },
          [](auto* id) { id->implementation_id += "-other"; },
          [](auto* id) { id->selected_alternative_uuid = Uuid(999002); },
          [](auto* id) { id->output_descriptor_digest += "-other"; },
          [](auto* id) { id->result_identity_digest += "-other"; },
          [](auto* id) { id->dependency_signature += "-stale"; },
      };
  for (const auto& mutate : identity_mismatches) {
    auto request = Consumption(source);
    mutate(&request.source_identity);
    CHECK(!store.Consume(request).consumed);
  }
  {
    opt::OptimizerGovernedFeedbackStore malformed_store;
    auto malformed = Observation(1);
    malformed.identity.selected_alternative_uuid = "not-a-canonical-uuid";
    CHECK(malformed_store.Publish(malformed, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-IDENTITY-V1");
  }

  using Generations = opt::OptimizerGovernedFeedbackGenerations;
  const std::vector<std::uint64_t Generations::*> generation_fields = {
      &Generations::catalog,    &Generations::security,
      &Generations::policy,     &Generations::statistics,
      &Generations::capability, &Generations::route,
      &Generations::resource,   &Generations::feedback,
  };
  for (const auto field : generation_fields) {
    auto request = Consumption(source);
    ++(request.current_generations.*field);
    CHECK(store.Consume(request).diagnostic_code ==
          "QOW-DIAG-OPT-013-CONSUME-STALE-GENERATION-V1");
  }

  using Proof = opt::OptimizerGovernedFeedbackAlternativeProof;
  const std::vector<bool Proof::*> equivalence_fields = {
      &Proof::identical_logical_semantics,
      &Proof::identical_output_descriptors,
      &Proof::identical_required_properties,
      &Proof::identical_delivered_properties,
      &Proof::identical_security_behavior,
      &Proof::identical_mga_behavior,
      &Proof::identical_donor_compatibility,
      &Proof::compatible_capability,
      &Proof::exact_fallback_available,
  };
  for (const auto field : equivalence_fields) {
    auto request = Consumption(source);
    request.alternative.*field = false;
    CHECK(store.Consume(request).diagnostic_code ==
          "QOW-DIAG-OPT-013-ALTERNATIVE-INCOMPATIBLE-V1");
  }
  {
    auto request = Consumption(source);
    request.alternative.exact_fallback_alternative_uuid = Uuid(999003);
    CHECK(store.Consume(request).diagnostic_code ==
          "QOW-DIAG-OPT-013-ALTERNATIVE-INCOMPATIBLE-V1");
  }
  {
    auto request = Consumption(source);
    request.alternative.candidate_alternative_uuid = "NOT-A-UUID";
    CHECK(store.Consume(request).diagnostic_code ==
          "QOW-DIAG-OPT-013-ALTERNATIVE-INCOMPATIBLE-V1");
    request = Consumption(source);
    request.alternative.exact_fallback_alternative_uuid = "not-a-uuid";
    CHECK(store.Consume(request).diagnostic_code ==
          "QOW-DIAG-OPT-013-ALTERNATIVE-INCOMPATIBLE-V1");
  }
  {
    auto request = Consumption(source, source.observation_sequence);
    CHECK(store.Consume(request).diagnostic_code ==
          "QOW-DIAG-OPT-013-CONSUME-CURRENT-EXECUTION-V1");
  }
}

void TestConsumeSideEffectRefusals() {
  const auto policy = Policy();
  opt::OptimizerGovernedFeedbackStore store;
  const auto source = Admit(&store, policy);
  using Request = opt::OptimizerGovernedFeedbackConsumptionRequest;
  const std::vector<bool Request::*> forbidden = {
      &Request::mutate_executed_plan_requested,
      &Request::mutate_runtime_actual_requested,
      &Request::mutate_result_requested,
      &Request::mutate_transaction_requested,
      &Request::force_execution_requested,
      &Request::retry_requested,
      &Request::reprepare_requested,
      &Request::statistics_refresh_requested,
      &Request::operator_replacement_requested,
      &Request::feedback_authority_claimed,
  };
  for (const auto field : forbidden) {
    auto request = Consumption(source);
    request.*field = true;
    CHECK(store.Consume(request).diagnostic_code ==
          "QOW-DIAG-OPT-013-CONSUME-FORBIDDEN-SIDE-EFFECT-V1");
  }
  auto request = Consumption(source);
  request.later_planning_attempt = false;
  CHECK(!store.Consume(request).consumed);
  request = Consumption(source);
  request.policy_allowed = false;
  CHECK(!store.Consume(request).consumed);
}

void TestQuarantineInvalidationAndPolicyTransition() {
  const auto policy = Policy();
  {
    opt::OptimizerGovernedFeedbackStore store;
    const auto source = Admit(&store, policy);
    auto regression = Observation(3, 1, 600003);
    regression.regression_observed = true;
    CHECK(store.Publish(regression, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-REGRESSION-QUARANTINED-V1");
    CHECK(!store.Consume(Consumption(source)).consumed);
    const auto snapshot = store.Snapshot();
    CHECK(snapshot.quarantined_identity_count == 1);
    CHECK(snapshot.invalidated_identity_count == 0);
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    const auto source = Admit(&store, policy);
    auto incompatible = Observation(3, 1, 600004);
    incompatible.metrics.estimated_output_rows = Observed(101);
    CHECK(store.Publish(incompatible, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-INCOMPATIBLE-SAMPLE-V1");
    CHECK(!store.Consume(Consumption(source)).consumed);
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    const auto source = Admit(&store, policy);
    auto stale = Observation(3, 1, 600005);
    ++stale.generations.catalog;
    CHECK(store.Publish(stale, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-STALE-GENERATION-V1");
    CHECK(store.Consume(Consumption(source)).consumed);
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    const auto source = Admit(&store, policy);
    opt::OptimizerGovernedFeedbackInvalidation wildcard;
    wildcard.reason = "invalid_wildcard";
    CHECK(store.Invalidate(wildcard) == 0);
    CHECK(store.Consume(Consumption(source)).consumed);

    auto oversized_reason_generations = source.generations;
    ++oversized_reason_generations.catalog;
    CHECK(store.Invalidate({source.identity.selected_plan_uuid,
                            oversized_reason_generations,
                            std::string(10000, 'x')}) == 0);
    CHECK(store.Consume(Consumption(source)).consumed);

    auto current = source.generations;
    ++current.statistics;
    CHECK(store.Invalidate({source.identity.selected_plan_uuid, current,
                            "statistics_generation_changed"}) == 1);
    CHECK(!store.Consume(Consumption(source)).consumed);
    const auto snapshot = store.Snapshot();
    CHECK(snapshot.invalidated_identity_count == 1);
    CHECK(snapshot.quarantined_identity_count == 0);
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    const auto old_source = Admit(&store, policy);
    auto newer = old_source.generations;
    ++newer.catalog;
    CHECK(store.Invalidate({old_source.identity.selected_plan_uuid, newer,
                            "catalog_generation_changed"}) == 1);
    auto replacement1 = Observation(3, 1, 610003);
    auto replacement2 = Observation(4, 1, 610004);
    replacement1.generations = newer;
    replacement2.generations = newer;
    replacement2.metrics.actual_output_rows = Observed(320);
    CHECK(store.Publish(replacement1, policy).disposition ==
          opt::OptimizerGovernedFeedbackDisposition::kPending);
    CHECK(store.Publish(replacement2, policy).disposition ==
          opt::OptimizerGovernedFeedbackDisposition::kAdmitted);
    CHECK(!store.Consume(Consumption(old_source)).consumed);
    CHECK(store.Consume(Consumption(replacement2)).consumed);
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    auto source = Admit(&store, policy);
    auto current = source.generations;
    ++current.policy;
    CHECK(store.Invalidate({source.identity.selected_plan_uuid, current,
                            "policy_generation_changed"}) == 1);
    auto next_policy = policy;
    next_policy.maximum_retained_identities = 8;
    auto next1 = Observation(3, 1, 600006);
    next1.generations.policy = 2;
    next1.generations.feedback = 2;
    auto next2 = Observation(4, 1, 600007);
    next2.generations.policy = 2;
    next2.generations.feedback = 2;
    next2.metrics.actual_output_rows = Observed(320);
    CHECK(store.Publish(next1, next_policy).disposition ==
          opt::OptimizerGovernedFeedbackDisposition::kPending);
    CHECK(store.Publish(next2, next_policy).disposition ==
          opt::OptimizerGovernedFeedbackDisposition::kAdmitted);
    CHECK(store.Consume(Consumption(next2)).consumed);
  }
}

void TestSequenceAndRetentionBounds() {
  auto policy = Policy();
  policy.maximum_samples_per_identity = 2;
  {
    opt::OptimizerGovernedFeedbackStore store;
    auto first = Observation(10, 1, 500010);
    auto second = Observation(20, 1, 500020);
    auto third = Observation(30, 1, 500030);
    CHECK(store.Publish(first, policy).accepted);
    CHECK(store.Publish(second, policy).accepted);
    CHECK(store.Publish(third, policy).accepted);
    auto old = Observation(15, 1, 500015);
    CHECK(store.Publish(old, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-NON-MONOTONIC-OBSERVATION-V1");
    const auto snapshot = store.Snapshot();
    CHECK(snapshot.records.front().retained_samples.size() == 2);
    CHECK(snapshot.records.front().retained_samples[0].observation_sequence == 20);
    CHECK(snapshot.records.front().retained_samples[1].observation_sequence == 30);
  }
  {
    opt::OptimizerGovernedFeedbackStore store;
    auto first = Observation(10, 1, 500010);
    CHECK(store.Publish(first, policy).accepted);
    CHECK(store.Publish(first, policy).diagnostic_code ==
          "QOW-DIAG-OPT-013-NON-MONOTONIC-OBSERVATION-V1");
  }
  {
    auto bounded = Policy();
    bounded.maximum_retained_identities = 3;
    bounded.maximum_quarantine_records = 4;
    opt::OptimizerGovernedFeedbackStore store;
    for (std::uint64_t identity = 1; identity <= 4; ++identity) {
      Admit(&store, bounded, identity, identity * 10);
    }
    auto snapshot = store.Snapshot();
    CHECK(snapshot.retained_identity_count == 3);
    CHECK(snapshot.records.front().identity.physical_node_id == 1002);
    for (std::uint64_t sequence = 100; sequence < 110; ++sequence) {
      auto refused = Observation(sequence, 9, 900000 + sequence);
      refused.outcome = opt::OptimizerGovernedFeedbackOutcome::kFailed;
      store.Publish(refused, bounded);
    }
    snapshot = store.Snapshot();
    CHECK(snapshot.quarantine_record_count == 4);
    CHECK(snapshot.quarantine.front().observation_sequence == 106);
    CHECK(snapshot.quarantine.back().observation_sequence == 109);
  }
}

void TestConcurrentIsolationAndDeterministicReplay() {
  auto policy = Policy();
  policy.maximum_retained_identities = 64;
  policy.maximum_quarantine_records = 64;
  opt::OptimizerGovernedFeedbackStore store;
  std::atomic<int> thread_failures{0};
  std::vector<std::thread> workers;
  for (std::uint64_t identity = 1; identity <= 8; ++identity) {
    workers.emplace_back([&, identity]() {
      auto first = Observation(identity * 100 + 1, identity,
                               950000 + identity * 2);
      auto second = Observation(identity * 100 + 2, identity,
                                950001 + identity * 2);
      second.metrics.actual_output_rows = Observed(320);
      if (store.Publish(first, policy).disposition !=
              opt::OptimizerGovernedFeedbackDisposition::kPending ||
          store.Publish(second, policy).disposition !=
              opt::OptimizerGovernedFeedbackDisposition::kAdmitted) {
        ++thread_failures;
      }
      if (!store.Consume(Consumption(second, identity * 100 + 10000)).consumed) {
        ++thread_failures;
      }
    });
  }
  for (auto& worker : workers) worker.join();
  CHECK(thread_failures.load() == 0);
  const auto concurrent = store.Snapshot();
  CHECK(concurrent.retained_identity_count == 8);
  CHECK(concurrent.admitted_identity_count == 8);
  for (const auto& record : concurrent.records) {
    CHECK(record.compatible_sample_count == 2);
    CHECK(record.retained_samples.size() == 2);
  }

  opt::OptimizerGovernedFeedbackStore forward;
  opt::OptimizerGovernedFeedbackStore reverse;
  for (std::uint64_t identity = 1; identity <= 8; ++identity) {
    Admit(&forward, policy, identity, identity * 10);
  }
  for (std::uint64_t identity = 8; identity >= 1; --identity) {
    Admit(&reverse, policy, identity, identity * 10);
    if (identity == 1) break;
  }
  const auto left = forward.Snapshot();
  const auto right = reverse.Snapshot();
  CHECK(left.records.size() == right.records.size());
  for (std::size_t index = 0;
       index < std::min(left.records.size(), right.records.size()); ++index) {
    CHECK(left.records[index].identity.selected_plan_uuid ==
          right.records[index].identity.selected_plan_uuid);
    CHECK(left.records[index].last_observation_sequence ==
          right.records[index].last_observation_sequence);
    CHECK(left.records[index].advisory_output_rows ==
          right.records[index].advisory_output_rows);
  }
}

void TestExistingApiCompatibility() {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "scan";
  feedback.plan_shape = "single-node";
  feedback.estimated_rows = 10;
  feedback.actual_rows = 20;
  const auto status = opt::EvaluateOptimizerRuntimeFeedback(feedback);
  CHECK(status.ok);
  CHECK(status.applied);
  CHECK(status.cost_profile.apply);
}

}  // namespace

int main() {
  TestAcceptedPublicationAndConsumption();
  TestThresholdMetricStatesAndMath();
  TestPolicyBoundsAndBoundedMalformedInput();
  TestOutcomeAndAuthorityRefusals();
  TestExactIdentityGenerationAndAlternativeMatching();
  TestConsumeSideEffectRefusals();
  TestQuarantineInvalidationAndPolicyTransition();
  TestSequenceAndRetentionBounds();
  TestConcurrentIsolationAndDeterministicReplay();
  TestExistingApiCompatibility();
  if (failures != 0) {
    std::cerr << failures << " qow_opt_013 checks failed\n";
    return 1;
  }
  std::cout << "qow_opt_013 governed feedback checks passed\n";
  return 0;
}
