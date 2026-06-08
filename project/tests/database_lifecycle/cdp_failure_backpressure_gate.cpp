// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_workload_resource_quota.hpp"
#include "page_filespace_handoff.hpp"
#include "strict_bulk_load_lifecycle.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace bulk = scratchbird::core::bulk_load;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1910000000000ull + seed);
  Require(generated.ok(), "CDP failure/backpressure UUID generation failed");
  return generated.value;
}

struct FixtureIds {
  platform::TypedUuid database_uuid;
  platform::TypedUuid object_uuid;
  platform::TypedUuid filespace_uuid;
  platform::TypedUuid transaction_uuid;
  platform::TypedUuid policy_uuid;
};

FixtureIds MakeIds(platform::u64 seed) {
  return {MakeUuid(platform::UuidKind::database, seed + 100),
          MakeUuid(platform::UuidKind::object, seed + 200),
          MakeUuid(platform::UuidKind::filespace, seed + 300),
          MakeUuid(platform::UuidKind::transaction, seed + 400),
          MakeUuid(platform::UuidKind::object, seed + 500)};
}

bulk::StrictBulkLoadPolicySnapshot Policy(const FixtureIds& ids) {
  bulk::StrictBulkLoadPolicySnapshot policy;
  policy.policy_uuid = ids.policy_uuid;
  policy.enabled = true;
  return policy;
}

bulk::StrictBulkLoadBeginRequest BeginRequest(const FixtureIds& ids) {
  bulk::StrictBulkLoadBeginRequest request;
  request.database_uuid = ids.database_uuid;
  request.object_uuid = ids.object_uuid;
  request.transaction_uuid = ids.transaction_uuid;
  request.local_transaction_id = 314;
  request.policy = Policy(ids);
  request.staging_target = "cdp-failure-backpressure-strict-copy";
  return request;
}

bulk::StrictBulkLoadRow Row(platform::u64 seed, std::string encoded_row) {
  bulk::StrictBulkLoadRow row;
  row.row_uuid = MakeUuid(platform::UuidKind::object, seed);
  row.encoded_row = std::move(encoded_row);
  return row;
}

bulk::StrictBulkLoadAppendRequest AppendRequest(const bulk::StrictBulkLoadBeginResult& begin,
                                                const FixtureIds& ids,
                                                std::vector<bulk::StrictBulkLoadRow> rows) {
  bulk::StrictBulkLoadAppendRequest request;
  request.bulk_load_id = begin.operation.bulk_load_id;
  request.transaction_uuid = ids.transaction_uuid;
  request.local_transaction_id = 314;
  request.rows = std::move(rows);
  return request;
}

bulk::DmlPageFilespaceDemandHintRequest DemandHint(const FixtureIds& ids) {
  bulk::DmlPageFilespaceDemandHintRequest request;
  request.database_uuid = ids.database_uuid;
  request.object_uuid = ids.object_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.transaction_uuid = ids.transaction_uuid;
  request.local_transaction_id = 314;
  request.batch_sequence = 1;
  request.batch_row_count = 100;
  request.estimated_row_bytes = 120;
  request.page_size_bytes = 4096;
  request.requested_page_count = 8;
  request.max_preallocation_pages = 4;
  request.source = "COPY";
  return request;
}

bulk::CopyBatchMetricRequest CopyMetric(platform::u64 sequence) {
  bulk::CopyBatchMetricRequest request;
  request.batch_sequence = sequence;
  request.batch_row_count = 100;
  request.append_timing.elapsed_nanos = 1000;
  request.finality_timing.elapsed_nanos = 2000;
  request.message_status = bulk::CopyBatchMetricStatus::accepted;
  request.result_status = bulk::CopyBatchMetricStatus::completed;
  request.message_status_detail = "engine accepted COPY batch";
  request.result_status_detail = "engine completed COPY batch";
  return request;
}

agents::WorkloadResourceVector AllResources(std::uint64_t value) {
  agents::WorkloadResourceVector resources;
  resources.memory_bytes = value;
  resources.worker_slots = value;
  resources.temp_bytes = value;
  resources.filespace_bytes = value;
  resources.active_requests = value;
  resources.open_cursors = value;
  resources.transaction_slots = value;
  resources.buffer_bytes = value;
  resources.udr_bytes = value;
  return resources;
}

agents::WorkloadResourcePoolConfig Pool(std::string pool_id,
                                        agents::WorkloadQuotaLimits limits) {
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = std::move(pool_id);
  pool.workload_class = agents::WorkloadClass::foreground;
  pool.limits = limits;
  return pool;
}

agents::WorkloadAdmissionRequest Admission(std::string request_uuid,
                                           std::string pool_id,
                                           agents::WorkloadResourceVector requested) {
  agents::WorkloadAdmissionRequest request;
  request.request_uuid = std::move(request_uuid);
  request.pool_id = std::move(pool_id);
  request.workload_class = agents::WorkloadClass::foreground;
  request.source = agents::WorkloadAdmissionSource::engine;
  request.requested = requested;
  request.principal_tag = "principal-secret-must-not-leak";
  return request;
}

void RequireZeroUsage(const agents::WorkloadResourceQuotaController& controller,
                      std::string_view pool_id,
                      std::string_view message) {
  const auto usage = controller.UsageForPool(std::string(pool_id));
  Require(usage.memory_bytes == 0 && usage.worker_slots == 0 && usage.temp_bytes == 0 &&
              usage.filespace_bytes == 0 && usage.active_requests == 0 &&
              usage.open_cursors == 0 && usage.transaction_slots == 0 &&
              usage.buffer_bytes == 0 && usage.udr_bytes == 0,
          message);
}

page::PageFilespaceAgentRequest PageQueueRequest(const FixtureIds& ids) {
  page::PageFilespaceAgentRequest request;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.policy_uuid = ids.policy_uuid;
  request.kind = page::PageFilespaceAgentRequestKind::reserve_pages;
  request.requesting_agent = "filespace_capacity_manager";
  request.responding_agent = "page_allocation_manager";
  request.page_family = "data";
  request.requested_pages = 4;
  request.reason = "foreground DML pressure handoff";
  return request;
}

page::PageFilespaceAgentRequestPolicy PageQueuePolicy(const FixtureIds& ids) {
  page::PageFilespaceAgentRequestPolicy policy;
  policy.policy_uuid = ids.policy_uuid;
  policy.min_free_pages = 4;
  policy.target_free_pages = 8;
  policy.page_agent_may_allocate_pages = true;
  return policy;
}

void TestInvalidCopyMetricRefusalDoesNotMutateLedger() {
  bulk::CopyBatchMetricLedger ledger;
  const auto accepted = bulk::RecordCopyBatchMetric(&ledger, CopyMetric(1));
  Require(accepted.ok(), "baseline COPY metric was not accepted");
  Require(ledger.records.size() == 1, "baseline COPY metric record count mismatch");
  Require(ledger.totals.metric_record_count == 1, "baseline COPY metric totals mismatch");

  auto invalid = CopyMetric(2);
  invalid.result_status = bulk::CopyBatchMetricStatus::unknown;
  const auto refused = bulk::RecordCopyBatchMetric(&ledger, invalid);
  Require(!refused.ok(), "invalid COPY metric was accepted");
  Require(refused.diagnostic.diagnostic_code == "copy_batch_metric_invalid_result_status",
          "invalid COPY metric diagnostic mismatch");
  Require(ledger.records.size() == 1, "invalid COPY metric mutated records");
  Require(ledger.totals.metric_record_count == 1, "invalid COPY metric mutated record total");
  Require(ledger.totals.completed_result_count == 1,
          "invalid COPY metric mutated completed result total");
  Require(ledger.totals.refused_result_count == 0,
          "invalid COPY metric mutated refused result total");
}

void TestStrictBulkLoadFinalizeFailureClassifiesWithoutVisibility() {
  const auto ids = MakeIds(10);
  bulk::StrictBulkLoadLedger ledger;
  const auto begin = bulk::BeginStrictBulkLoad(&ledger, BeginRequest(ids));
  Require(begin.ok(), "strict bulk-load begin failed");
  const auto append = bulk::AppendStrictBulkLoadRows(
      &ledger,
      AppendRequest(begin, ids, {Row(1001, "copy-row-before-finalize-failure")}));
  Require(append.ok(), "strict bulk-load append failed");

  bulk::StrictBulkLoadFinalizeRequest finalize;
  finalize.bulk_load_id = begin.operation.bulk_load_id;
  finalize.transaction_uuid = ids.transaction_uuid;
  finalize.local_transaction_id = 314;
  finalize.visibility_fence = "cdp-failure-backpressure-mga-fence";
  finalize.simulate_finalize_failure_after_evidence = true;
  const auto failed = bulk::FinalizeStrictBulkLoad(&ledger, finalize);
  Require(!failed.ok() && failed.recovery_required && !failed.published_visible,
          "finalize failure did not require recovery without publication");

  const auto* operation = bulk::FindStrictBulkLoadOperation(ledger, begin.operation.bulk_load_id);
  Require(operation != nullptr, "strict bulk-load operation disappeared");
  Require(operation->state == bulk::StrictBulkLoadState::recovery_required,
          "finalize failure state mismatch");
  Require(operation->visible_rows.empty(), "finalize failure published visible rows");
  Require(operation->staged_rows.size() == 1, "finalize failure lost staged recovery evidence");

  const auto recovery = bulk::ClassifyStrictBulkLoadLedgerForRecovery(ledger);
  Require(recovery.ok(), "strict bulk-load recovery classification failed");
  Require(recovery.classifications.size() == 1, "strict bulk-load recovery count mismatch");
  Require(recovery.classifications.front().observed_state ==
              bulk::StrictBulkLoadState::recovery_required,
          "strict bulk-load recovery observed state mismatch");
  Require(recovery.classifications.front().action ==
              bulk::StrictBulkLoadRecoveryAction::complete_publication,
          "strict bulk-load recovery action mismatch");
  Require(!recovery.classifications.front().fail_closed,
          "recoverable finalize failure was incorrectly fail-closed");
}

void TestDmlDemandHintRefusalsAndPageQueueCancelEvidence() {
  const auto ids = MakeIds(20);

  auto invalid_filespace = DemandHint(ids);
  invalid_filespace.filespace_uuid = ids.object_uuid;
  const auto invalid_result = bulk::MakeDmlPageFilespaceDemandHint(invalid_filespace);
  Require(!invalid_result.ok(), "invalid filespace identity demand hint was accepted");
  Require(invalid_result.record.decision == bulk::DmlPageFilespaceDemandHintDecision::refused,
          "invalid filespace identity demand hint did not retain refused decision");
  Require(invalid_result.diagnostic.diagnostic_code ==
              "dml_demand_hint_invalid_filespace_identity",
          "invalid filespace identity demand hint diagnostic mismatch");

  auto unbounded = DemandHint(ids);
  unbounded.max_preallocation_pages = 0;
  const auto unbounded_result = bulk::MakeDmlPageFilespaceDemandHint(unbounded);
  Require(!unbounded_result.ok(), "unbounded demand hint was accepted");
  Require(unbounded_result.record.decision == bulk::DmlPageFilespaceDemandHintDecision::refused,
          "unbounded demand hint did not retain refused decision");
  Require(unbounded_result.diagnostic.diagnostic_code == "dml_demand_hint_missing_bound",
          "unbounded demand hint diagnostic mismatch");

  page::PageFilespaceAgentRequestQueue queue;
  const auto enqueued =
      page::EnqueuePageFilespaceAgentRequest(&queue, PageQueueRequest(ids), PageQueuePolicy(ids));
  Require(enqueued.ok() && enqueued.enqueued, "page/filespace agent request was not enqueued");
  Require(queue.records.size() == 1, "page/filespace queue record count mismatch");
  Require(queue.records.front().allowed, "page/filespace queued request was not allowed");
  Require(queue.records.front().page_agent_action_required,
          "page/filespace queued request did not target page agent");
  Require(queue.records.front().explicit_evidence,
          "page/filespace queued request did not carry explicit evidence");

  const auto cancelled = page::CancelPageFilespaceAgentRequest(
      &queue,
      enqueued.request_uuid,
      "foreground DML cancellation releases queued capacity request");
  Require(cancelled.ok() && cancelled.cancelled,
          "page/filespace queued request cancellation failed");
  Require(queue.records.front().request.state == page::PageFilespaceAgentRequestState::cancelled,
          "page/filespace queued request state was not cancelled");
  Require(queue.records.front().transitions.size() == 2,
          "page/filespace queued request did not record enqueue and cancel transitions");
  Require(queue.records.front().transitions.back().new_state ==
              page::PageFilespaceAgentRequestState::cancelled,
          "page/filespace queued request cancel transition mismatch");
}

void TestQuotaSoftThrottleHardDenyAndCancelReleaseEvidence() {
  agents::WorkloadQuotaLimits limits;
  limits.hard = AllResources(10);
  limits.soft = AllResources(5);
  agents::WorkloadResourceQuotaController controller;
  Require(controller.RegisterPool(Pool("foreground", limits)).ok,
          "foreground quota pool registration failed");

  const auto base = controller.Admit(Admission("base", "foreground", AllResources(4)));
  Require(base.status.ok && base.reservation_created(), "base foreground reservation failed");
  const auto throttled = controller.Admit(Admission("throttled", "foreground", AllResources(2)));
  Require(throttled.status.ok &&
              throttled.decision == agents::WorkloadAdmissionDecisionClass::throttled,
          "soft quota did not throttle");
  Require(throttled.reservation_created(), "soft quota throttle did not create reservation");
  Require(throttled.diagnostic.diagnostic_code == "WORKLOAD_RESOURCE.SOFT_THROTTLE",
          "soft quota throttle diagnostic mismatch");
  Require(throttled.evidence.reservation_created,
          "soft quota throttle evidence did not record reservation");

  const auto hard_denied = controller.Admit(Admission("hard-denied", "foreground", AllResources(5)));
  Require(!hard_denied.status.ok &&
              hard_denied.decision == agents::WorkloadAdmissionDecisionClass::rejected,
          "hard quota did not deny");
  Require(hard_denied.diagnostic.diagnostic_code == "WORKLOAD_RESOURCE.HARD_DENIED",
          "hard quota diagnostic mismatch");
  Require(!hard_denied.reservation_created() && !hard_denied.evidence.reservation_created,
          "hard quota denial created reservation evidence");

  Require(controller.Release(base.reservation.token_id,
                             agents::WorkloadReleaseReason::success)
              .ok,
          "base reservation release failed");
  Require(controller.Cancel(throttled.reservation.token_id).ok,
          "throttled reservation cancellation release failed");
  RequireZeroUsage(controller, "foreground", "quota release/cancel leaked usage");
  Require(controller.ActiveReservationCount() == 0,
          "quota release/cancel left active reservations");

  const auto& evidence = controller.evidence_log();
  Require(evidence.size() == 5, "quota evidence row count mismatch");
  Require(evidence[1].decision == "throttled" &&
              evidence[1].diagnostic_code == "WORKLOAD_RESOURCE.SOFT_THROTTLE",
          "soft throttle evidence mismatch");
  Require(evidence[2].decision == "rejected" &&
              evidence[2].diagnostic_code == "WORKLOAD_RESOURCE.HARD_DENIED",
          "hard denial evidence mismatch");
  Require(evidence[4].decision == "released" &&
              evidence[4].diagnostic_code == "WORKLOAD_RESOURCE.RELEASED.cancellation",
          "cancellation release evidence mismatch");
}

}  // namespace

int main() {
  TestInvalidCopyMetricRefusalDoesNotMutateLedger();
  TestStrictBulkLoadFinalizeFailureClassifiesWithoutVisibility();
  TestDmlDemandHintRefusalsAndPageQueueCancelEvidence();
  TestQuotaSoftThrottleHardDenyAndCancelReleaseEvidence();
  return EXIT_SUCCESS;
}
