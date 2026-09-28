// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"
#include "optimizer_explain.hpp"
#include "optimizer_feedback.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <type_traits>
#include "optimizer_request.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OPCH runtime payload/feedback gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool ContainsText(const std::string& value, const std::string& expected) {
  return value.find(expected) != std::string::npos;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

plan::LogicalPlan LogicalPlanWithPolicy() {
  auto logical = plan::BuildQueryShapePlan({plan::QueryShapeKind::kJoinQuery});
  logical.optimizer_policy.optimizer_policy_metadata_present = true;
  logical.optimizer_policy.policy_source_kind = "sblr_api";
  logical.optimizer_policy.policy_epoch = 7;
  logical.optimizer_policy.safe_control_ids = {
      "plan_profile:commercial_diagnostic",
      "join_search:bounded_dp_frontier",
      "memory_grant_policy:governed"};
  return logical;
}

opt::BoundOptimizerRequest Request() {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = "opch040.request";
  request.context.operation_id = "public_sql.select.join";
  request.context.sblr_digest = "sblr:opch040";
  request.context.descriptor_set_digest = "desc:opch040";
  request.context.statistics_snapshot_id = "stats:opch040";
  request.context.metric_snapshot_id = "metrics:opch040";
  request.context.executor_capability_set_id = "executor:opch040";
  request.context.catalog_epoch = 11;
  request.context.security_epoch = 13;
  request.context.policy_epoch = 7;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan = LogicalPlanWithPolicy();
  return request;
}

opt::BoundOptimizerResult Result() {
  opt::BoundOptimizerResult result;
  result.ok = true;
  result.diagnostic_code = "SB_OPT_OK";
  result.plan_id = "plan:opch040";
  result.optimizer_profile = "profile:opch040";

  opt::PlanCandidate refused;
  refused.candidate_id = "candidate.table_scan";
  refused.access_kind = plan::PhysicalAccessKind::kTableScan;
  refused.cost.selectable = false;
  refused.cost.rejection_reason = "benchmark_clean_requires_catalog_stats";
  refused.refusal_reasons.push_back("statistics_provenance_missing");
  result.candidates.push_back(refused);

  opt::PlanCandidate selected;
  selected.candidate_id = "candidate.join_hash";
  selected.access_kind = plan::PhysicalAccessKind::kJoinHash;
  selected.selected = true;
  selected.cost.selectable = true;
  selected.cost.total_cost = 10;
  selected.runtime_evidence = {
      "join_strategy=bounded_dp",
      "SB_OPT_JOIN_FRONTIER_PROPERTY_RETENTION",
      "adaptive_feedback.feedback_generation=22",
      "optimizer_feedback.spill_observed",
      "actual_rows=1000",
      "runtime_actual.rows_examined=1200",
      "runtime_actual.rows_filtered=200",
      "runtime_actual.loop_count=3"};
  result.candidates.push_back(selected);
  return result;
}

bool RuntimePlanPayloadCoversCommercialExplainFields() {
  // SEARCH_KEY: OPCH_RUNTIME_PLAN_PAYLOAD_EXPLAIN_PARITY
  const auto document = opt::BuildOptimizerExplainDocument(Request(), Result());
  const auto json = opt::RenderOptimizerExplainJson(document);
  return Require(!document.plan_hash.empty(), "plan hash missing") &&
         Require(Contains(document.invalidation_dependencies, "catalog_epoch=11"),
                 "catalog invalidation dependency missing") &&
         Require(Contains(document.invalidation_dependencies, "security_epoch=13"),
                 "security invalidation dependency missing") &&
         Require(Contains(document.invalidation_dependencies, "policy_epoch=7"),
                 "policy invalidation dependency missing") &&
         Require(Contains(document.statistics_provenance, "statistics_snapshot_id=stats:opch040"),
                 "statistics provenance missing") &&
         Require(Contains(document.optimizer_controls,
                          "safe_control_id=join_search:bounded_dp_frontier"),
                 "optimizer controls missing") &&
         Require(!document.candidate_refusals.empty(),
                 "candidate refusal evidence missing") &&
         Require(!document.executor_capability_evidence.empty(),
                 "executor capability evidence missing") &&
         Require(!document.join_search_telemetry.empty(),
                 "join telemetry missing") &&
         Require(!document.adaptive_feedback_evidence.empty(),
                 "adaptive feedback evidence missing") &&
         Require(!document.runtime_actuals.empty(),
                 "runtime actuals missing") &&
         Require(ContainsText(json, "\"plan_hash\""),
                 "rendered JSON omitted plan_hash") &&
         Require(ContainsText(json, "\"optimizer_controls\""),
                 "rendered JSON omitted optimizer_controls") &&
         Require(ContainsText(json, "\"adaptive_feedback_evidence\""),
                 "rendered JSON omitted adaptive feedback evidence");
}

opt::OptimizerRuntimeFeedback RuntimeFeedback() {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "hash_join";
  feedback.plan_shape = "join_hash";
  feedback.cost_profile_id = "profile:opch041";
  feedback.estimated_rows = 100;
  feedback.actual_rows = 5000;
  feedback.actual_rows_examined = 6000;
  feedback.actual_rows_filtered = 1000;
  feedback.loop_count = 3;
  feedback.estimated_pages = 10;
  feedback.actual_pages = 40;
  feedback.estimated_io_operations = 10;
  feedback.actual_io_operations = 40;
  feedback.estimated_visibility_recheck_rows = 100;
  feedback.actual_visibility_recheck_rows = 5000;
  feedback.estimated_spill_bytes = 0;
  feedback.actual_spill_bytes = 4096;
  feedback.memory_grant_bytes = 1024;
  feedback.peak_memory_bytes = 4096;
  feedback.estimated_latency_microseconds = 100;
  feedback.actual_latency_microseconds = 900;
  feedback.estimated_resource_units = 100;
  feedback.actual_resource_units = 1000;
  feedback.advisory_only = true;
  feedback.mga_visibility_recheck_preserved = true;
  feedback.transaction_finality_authority = "engine_transaction_inventory";
  return feedback;
}

bool RuntimeFeedbackPersistenceIsScopedAndInvalidatable() {
  // SEARCH_KEY: OPCH_ADAPTIVE_FEEDBACK_ACTUALS_PERSISTENCE
  opt::OptimizerRuntimeFeedbackStore store;
  opt::OptimizerRuntimeFeedbackRecord record;
  record.feedback_uuid = scratchbird::tests::FixtureUuid(0x041, 1);
  record.scope_uuid = scratchbird::tests::FixtureUuid(0x041, 2);
  record.route_label = "embedded/sql/hash_join";
  record.feedback_generation = 22;
  record.policy_generation = 7;
  record.catalog_epoch = 11;
  record.security_epoch = 13;
  record.feedback = RuntimeFeedback();

  const auto status = store.Record(record);
  const auto found = store.Find(record.feedback_uuid);
  auto snapshot = store.Snapshot();
  opt::OptimizerRuntimeFeedbackInvalidation event;
  event.scope_uuid = record.scope_uuid;
  event.catalog_epoch = 12;
  event.reason = "catalog_epoch_changed";
  const auto invalidated = store.Invalidate(event);
  const auto invalidated_record = store.Find(record.feedback_uuid);
  snapshot = store.Snapshot();

  return Require(status.ok && status.applied, "feedback was not accepted") &&
         Require(Contains(status.evidence, "runtime_feedback_persistence.recorded=true"),
                 "recorded evidence missing") &&
         Require(Contains(status.evidence,
                          "runtime_feedback_persistence.invalidatable=true"),
                 "invalidatable evidence missing") &&
         Require(found.has_value() && found->valid, "record not persisted as valid") &&
         Require(snapshot.total_records == 1, "snapshot record count changed") &&
         Require(invalidated == 1, "catalog invalidation did not invalidate record") &&
         Require(invalidated_record.has_value() && !invalidated_record->valid,
                 "invalidated record still valid") &&
         Require(snapshot.invalidated_records == 1, "invalidated snapshot count missing");
}

bool NativeFeedbackIdentityPreservesEveryByte() {
  using NativeUuid = scratchbird::core::platform::Uuid;
  using Record = opt::OptimizerRuntimeFeedbackRecord;
  using Invalidation = opt::OptimizerRuntimeFeedbackInvalidation;
  static_assert(std::is_same_v<decltype(Record::feedback_uuid), NativeUuid>);
  static_assert(std::is_same_v<decltype(Record::scope_uuid), NativeUuid>);
  static_assert(std::is_same_v<decltype(Invalidation::scope_uuid),
                               std::optional<NativeUuid>>);
  Record base;
  base.feedback_uuid = scratchbird::tests::FixtureUuid(0x041, 10);
  base.scope_uuid = scratchbird::tests::FixtureUuid(0x041, 11);
  base.route_label = "native_feedback";
  base.feedback_generation = base.policy_generation = 1;
  base.catalog_epoch = base.security_epoch = 1;
  base.feedback = RuntimeFeedback();
  for (const auto field : {&Record::feedback_uuid, &Record::scope_uuid}) {
    opt::OptimizerRuntimeFeedbackStore nil_store;
    auto nil = base;
    nil.*field = {};
    if (!Require(!nil_store.Record(nil).ok && nil_store.Snapshot().total_records == 0,
                 "nil identity reached feedback store")) return false;
    for (unsigned position = 0; position != 16; ++position) {
      for (unsigned octet = 0; octet != 256; ++octet) {
        auto changed = base;
        if ((changed.*field).bytes[position] == octet) continue;
        (changed.*field).bytes[position] = static_cast<std::uint8_t>(octet);
        const auto id = changed.*field;
        const bool valid = (id.bytes[6] & 0xf0) == 0x70 && (id.bytes[8] & 0xc0) == 0x80;
        opt::OptimizerRuntimeFeedbackStore store;
        if (!Require(store.Record(base).ok, "baseline feedback admission failed")) return false;
        const auto result = store.Record(changed);
        if (!Require(result.ok == valid && result.applied == valid,
                     "binary UUID admission disagrees with independent oracle")) return false;
        const auto snapshot = store.Snapshot();
        if (!Require(snapshot.total_records ==
                         (valid && field == &Record::feedback_uuid ? 2 : 1),
                     "distinct feedback UUIDs aliased or refusal mutated store")) return false;
        const auto found = store.Find(valid ? changed.feedback_uuid : base.feedback_uuid);
        if (!Require(found.has_value(), "exact native feedback lookup failed")) return false;
        if (valid) {
          if (!Require(found->feedback_uuid == changed.feedback_uuid &&
                       found->scope_uuid == changed.scope_uuid,
                       "feedback store normalized or truncated UUID bytes")) return false;
        } else {
          if (!Require(result.diagnostic_code ==
                           "SB_OPTIMIZER_FEEDBACK_PERSISTENCE.MISSING_SCOPE" &&
                       found->feedback_uuid == base.feedback_uuid &&
                       found->scope_uuid == base.scope_uuid && found->valid,
                       "malformed record changed existing feedback")) return false;
        }
      }
    }
  }
  opt::OptimizerRuntimeFeedbackStore store;
  if (!Require(store.Record(base).ok, "invalidation baseline refused")) return false;
  Invalidation event;
  event.catalog_epoch = 2;
  event.scope_uuid = NativeUuid{};
  if (!Require(store.Invalidate(event) == 0, "nil scope became wildcard")) return false;
  for (unsigned position = 0; position != 16; ++position) {
    for (unsigned octet = 0; octet != 256; ++octet) {
      auto id = base.scope_uuid;
      if (id.bytes[position] == octet) continue;
      id.bytes[position] = static_cast<std::uint8_t>(octet);
      event.scope_uuid = id;
      if (!Require(store.Invalidate(event) == 0,
                   "distinct or malformed scope invalidated another identity")) return false;
      auto lookup = base.feedback_uuid;
      lookup.bytes[position] = static_cast<std::uint8_t>(octet);
      if (lookup != base.feedback_uuid &&
          !Require(!store.Find(lookup), "distinct binary lookup aliased")) return false;
    }
  }
  if (!Require(store.Snapshot().valid_records == 1, "refusal changed valid records")) return false;
  event.scope_uuid = base.scope_uuid;
  if (!Require(store.Invalidate(event) == 1, "exact scope invalidation failed")) return false;
  if (!Require(store.Record(base).ok, "feedback replacement failed")) return false;
  auto second = base;
  second.feedback_uuid = scratchbird::tests::FixtureUuid(0x041, 12);
  second.scope_uuid = scratchbird::tests::FixtureUuid(0x041, 13);
  if (!Require(store.Record(second).ok, "second scope admission failed")) return false;
  event.scope_uuid.reset();
  return Require(store.Invalidate(event) == 2 && store.Snapshot().valid_records == 0,
                 "explicit all-scope invalidation lost its existing behavior");
}

bool UnsafeFeedbackAuthorityIsRejected() {
  opt::OptimizerRuntimeFeedbackStore store;
  opt::OptimizerRuntimeFeedbackRecord record;
  record.feedback_uuid = scratchbird::tests::FixtureUuid(0x041, 3);
  record.scope_uuid = scratchbird::tests::FixtureUuid(0x041, 4);
  record.route_label = "driver/sql/hash_join";
  record.feedback_generation = 1;
  record.policy_generation = 1;
  record.catalog_epoch = 1;
  record.security_epoch = 1;
  record.feedback = RuntimeFeedback();
  record.feedback.parser_or_reference_authority = true;

  const auto status = store.Record(record);
  return Require(!status.ok, "unsafe feedback was persisted") &&
         Require(status.diagnostic_code == "SB_OPTIMIZER_FEEDBACK.REJECTED_UNSAFE",
                 "unsafe feedback diagnostic changed");
}

}  // namespace

int main() {
  if (!RuntimePlanPayloadCoversCommercialExplainFields()) return EXIT_FAILURE;
  if (!RuntimeFeedbackPersistenceIsScopedAndInvalidatable()) return EXIT_FAILURE;
  if (!UnsafeFeedbackAuthorityIsRejected()) return EXIT_FAILURE;
  if (!NativeFeedbackIdentityPreservesEveryByte()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
