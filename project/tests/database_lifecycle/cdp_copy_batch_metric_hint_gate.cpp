// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "strict_bulk_load_lifecycle.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace bulk = scratchbird::core::bulk_load;
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
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1900000000000ull + seed);
  Require(generated.ok(), "uuid generation failed");
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
  return {MakeUuid(platform::UuidKind::database, 100 + seed),
          MakeUuid(platform::UuidKind::object, 200 + seed),
          MakeUuid(platform::UuidKind::filespace, 300 + seed),
          MakeUuid(platform::UuidKind::transaction, 400 + seed),
          MakeUuid(platform::UuidKind::object, 500 + seed)};
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
  request.local_transaction_id = 77;
  request.policy = Policy(ids);
  request.staging_target = "cdp-copy-batch-metric-hint-gate";
  return request;
}

bulk::CopyBatchMetricRequest MetricRequest(platform::u64 sequence) {
  bulk::CopyBatchMetricRequest request;
  request.batch_sequence = sequence;
  request.batch_row_count = 512;
  request.parse_timing.elapsed_nanos = 10;
  request.bind_timing.elapsed_nanos = 20;
  request.append_timing.elapsed_nanos = 30;
  request.page_timing.elapsed_nanos = 40;
  request.index_timing.elapsed_nanos = 50;
  request.finality_timing.elapsed_nanos = 60;
  request.message_status = bulk::CopyBatchMetricStatus::accepted;
  request.result_status = bulk::CopyBatchMetricStatus::completed;
  request.message_status_detail = "COPY batch accepted by engine path";
  request.result_status_detail = "COPY batch completed by engine path";
  return request;
}

bulk::DmlPageFilespaceDemandHintRequest HintRequest(const FixtureIds& ids) {
  bulk::DmlPageFilespaceDemandHintRequest request;
  request.database_uuid = ids.database_uuid;
  request.object_uuid = ids.object_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.transaction_uuid = ids.transaction_uuid;
  request.local_transaction_id = 77;
  request.batch_sequence = 3;
  request.batch_row_count = 1000;
  request.estimated_row_bytes = 100;
  request.page_size_bytes = 4096;
  request.requested_page_count = 48;
  request.max_preallocation_pages = 16;
  request.source = "COPY";
  return request;
}

void TestCopyBatchMetricLedgerIsObservabilityOnly() {
  const auto ids = MakeIds(1);
  bulk::StrictBulkLoadLedger strict_ledger;
  const auto begin = bulk::BeginStrictBulkLoad(&strict_ledger, BeginRequest(ids));
  Require(begin.ok(), "strict bulk-load begin failed");
  Require(strict_ledger.operations.size() == 1, "strict ledger operation setup mismatch");
  Require(strict_ledger.evidence.size() == 1, "strict ledger evidence setup mismatch");

  bulk::CopyBatchMetricLedger metric_ledger;
  const auto result = bulk::RecordCopyBatchMetric(&metric_ledger, MetricRequest(1));
  Require(result.ok(), "COPY batch metric was not accepted");
  Require(metric_ledger.records.size() == 1, "metric ledger did not store record");
  Require(metric_ledger.totals.metric_record_count == 1, "metric record count mismatch");
  Require(metric_ledger.totals.total_batch_rows == 512, "metric row count mismatch");
  Require(metric_ledger.totals.accepted_message_count == 1, "accepted message count mismatch");
  Require(metric_ledger.totals.completed_result_count == 1, "completed result count mismatch");
  Require(metric_ledger.totals.parse_nanos == 10 && metric_ledger.totals.finality_nanos == 60,
          "metric timing totals mismatch");
  Require(metric_ledger.records.front().page_timing.recorded, "page timing was not normalized");

  Require(strict_ledger.operations.size() == 1, "metric recording mutated strict operations");
  Require(strict_ledger.evidence.size() == 1, "metric recording emitted bulk-load evidence");
  Require(strict_ledger.operations.front().state == bulk::StrictBulkLoadState::begun,
          "metric recording changed bulk-load state");
}

void TestCopyBatchMetricRefusalDoesNotMutateLedger() {
  bulk::CopyBatchMetricLedger metric_ledger;
  auto request = MetricRequest(2);
  request.message_status = bulk::CopyBatchMetricStatus::unknown;

  const auto result = bulk::RecordCopyBatchMetric(&metric_ledger, request);
  Require(!result.ok(), "invalid COPY batch metric was accepted");
  Require(result.diagnostic.diagnostic_code == "copy_batch_metric_invalid_message_status",
          "invalid metric diagnostic mismatch");
  Require(metric_ledger.records.empty(), "invalid metric mutated records");
  Require(metric_ledger.totals.metric_record_count == 0, "invalid metric changed totals");
}

void TestDmlDemandHintIsBoundedAndTyped() {
  const auto ids = MakeIds(2);
  const auto result = bulk::MakeDmlPageFilespaceDemandHint(HintRequest(ids));
  Require(result.ok(), "DML demand hint was not accepted");
  Require(result.record.decision == bulk::DmlPageFilespaceDemandHintDecision::capped,
          "DML demand hint was not capped");
  Require(result.record.requested_page_count == 48, "DML demand requested page count mismatch");
  Require(result.record.granted_page_count == 16, "DML demand grant was not bounded");
  Require(result.record.filespace_uuid.kind == platform::UuidKind::filespace,
          "DML demand did not retain filespace UUID kind");
  Require(result.record.page_agent_hint && result.record.filespace_agent_hint,
          "DML demand did not target page and filespace agents");
}

void TestDmlDemandHintDisabledAndRefusedCases() {
  const auto ids = MakeIds(3);
  auto disabled = HintRequest(ids);
  disabled.enabled = false;
  const auto disabled_result = bulk::MakeDmlPageFilespaceDemandHint(disabled);
  Require(disabled_result.status.ok(), "disabled DML hint should be a successful no-op");
  Require(!disabled_result.accepted, "disabled DML hint reported accepted work");
  Require(disabled_result.record.decision == bulk::DmlPageFilespaceDemandHintDecision::disabled,
          "disabled DML hint decision mismatch");

  auto wrong_filespace_kind = HintRequest(ids);
  wrong_filespace_kind.filespace_uuid = ids.object_uuid;
  const auto wrong_kind_result = bulk::MakeDmlPageFilespaceDemandHint(wrong_filespace_kind);
  Require(!wrong_kind_result.ok(), "wrong filespace UUID kind was accepted");
  Require(wrong_kind_result.diagnostic.diagnostic_code == "dml_demand_hint_invalid_filespace_identity",
          "wrong filespace UUID kind diagnostic mismatch");

  auto missing_bound = HintRequest(ids);
  missing_bound.max_preallocation_pages = 0;
  const auto missing_bound_result = bulk::MakeDmlPageFilespaceDemandHint(missing_bound);
  Require(!missing_bound_result.ok(), "unbounded DML demand hint was accepted");
  Require(missing_bound_result.diagnostic.diagnostic_code == "dml_demand_hint_missing_bound",
          "unbounded DML demand diagnostic mismatch");
}

}  // namespace

int main() {
  TestCopyBatchMetricLedgerIsObservabilityOnly();
  TestCopyBatchMetricRefusalDoesNotMutateLedger();
  TestDmlDemandHintIsBoundedAndTyped();
  TestDmlDemandHintDisabledAndRefusedCases();
  return EXIT_SUCCESS;
}
