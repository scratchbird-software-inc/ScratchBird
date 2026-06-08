// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "strict_bulk_load_lifecycle.hpp"
#include "uuid.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

scratchbird::core::platform::TypedUuid Id(scratchbird::core::platform::UuidKind kind,
                                          scratchbird::core::platform::u64 seed) {
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : scratchbird::core::platform::TypedUuid{};
}

struct Fixture {
  scratchbird::core::platform::TypedUuid database_uuid = Id(scratchbird::core::platform::UuidKind::database, 11100);
  scratchbird::core::platform::TypedUuid object_uuid = Id(scratchbird::core::platform::UuidKind::object, 11101);
  scratchbird::core::platform::TypedUuid transaction_uuid = Id(scratchbird::core::platform::UuidKind::transaction, 11102);
  scratchbird::core::platform::TypedUuid policy_uuid = Id(scratchbird::core::platform::UuidKind::object, 11103);
};

scratchbird::core::bulk_load::StrictBulkLoadPolicySnapshot Policy(const Fixture& fixture) {
  scratchbird::core::bulk_load::StrictBulkLoadPolicySnapshot policy;
  policy.policy_uuid = fixture.policy_uuid;
  policy.enabled = true;
  policy.require_all_constraints_valid = true;
  policy.require_all_indexes_valid = true;
  policy.require_all_domains_valid = true;
  policy.require_all_policy_gates_valid = true;
  return policy;
}

scratchbird::core::bulk_load::StrictBulkLoadBeginRequest BeginRequest(const Fixture& fixture) {
  scratchbird::core::bulk_load::StrictBulkLoadBeginRequest request;
  request.database_uuid = fixture.database_uuid;
  request.object_uuid = fixture.object_uuid;
  request.transaction_uuid = fixture.transaction_uuid;
  request.local_transaction_id = 77;
  request.policy = Policy(fixture);
  request.staging_target = "staging:strict_bulk_load";
  return request;
}

scratchbird::core::bulk_load::StrictBulkLoadRow Row(scratchbird::core::platform::u64 seed,
                                                    std::string encoded,
                                                    bool valid = true) {
  scratchbird::core::bulk_load::StrictBulkLoadRow row;
  row.row_uuid = Id(scratchbird::core::platform::UuidKind::row, seed);
  row.encoded_row = std::move(encoded);
  row.constraints_valid = valid;
  row.indexes_valid = valid;
  row.domains_valid = valid;
  row.policy_gates_valid = valid;
  return row;
}

scratchbird::core::bulk_load::StrictBulkLoadAppendRequest AppendRequest(
    const Fixture& fixture,
    scratchbird::core::platform::TypedUuid bulk_load_id,
    std::vector<scratchbird::core::bulk_load::StrictBulkLoadRow> rows) {
  scratchbird::core::bulk_load::StrictBulkLoadAppendRequest request;
  request.bulk_load_id = bulk_load_id;
  request.transaction_uuid = fixture.transaction_uuid;
  request.local_transaction_id = 77;
  request.rows = std::move(rows);
  return request;
}

scratchbird::core::bulk_load::StrictBulkLoadFinalizeRequest FinalizeRequest(
    const Fixture& fixture,
    scratchbird::core::platform::TypedUuid bulk_load_id) {
  scratchbird::core::bulk_load::StrictBulkLoadFinalizeRequest request;
  request.bulk_load_id = bulk_load_id;
  request.transaction_uuid = fixture.transaction_uuid;
  request.local_transaction_id = 77;
  request.visibility_fence = "visibility:fence";
  return request;
}

}  // namespace

int main() {
  using namespace scratchbird::core::bulk_load;

  bool ok = true;
  const Fixture fixture;

  StrictBulkLoadLedger ledger;
  const auto begun = BeginStrictBulkLoad(&ledger, BeginRequest(fixture));
  ok &= Require(begun.ok(), "strict bulk-load begins");
  ok &= Require(begun.operation.state == StrictBulkLoadState::begun, "begin state tracked");
  ok &= Require(begun.evidence.durable_state_changed, "begin evidence durable");

  const auto appended = AppendStrictBulkLoadRows(
      &ledger,
      AppendRequest(fixture, begun.operation.bulk_load_id, {Row(11200, "a"), Row(11201, "b")}));
  ok &= Require(appended.ok(), "strict bulk-load append succeeds");
  ok &= Require(appended.staged_row_count == 2, "two rows staged");
  const auto* staged = FindStrictBulkLoadOperation(ledger, begun.operation.bulk_load_id);
  ok &= Require(staged != nullptr && staged->visible_rows.empty(), "append does not publish visibility");

  const auto finalized = FinalizeStrictBulkLoad(&ledger, FinalizeRequest(fixture, begun.operation.bulk_load_id));
  ok &= Require(finalized.ok(), "strict bulk-load finalize succeeds");
  ok &= Require(finalized.published_visible, "finalize publishes visible rows");
  ok &= Require(finalized.visible_row_count == 2, "visible row count tracked");
  ok &= Require(finalized.index_closeout_count == 2, "index closeout count tracked");
  const auto* visible = FindStrictBulkLoadOperation(ledger, begun.operation.bulk_load_id);
  ok &= Require(visible != nullptr && visible->staged_rows.empty(), "finalize clears staging");
  ok &= Require(visible != nullptr && visible->visible_rows.size() == 2, "finalize publishes all rows atomically");

  StrictBulkLoadLedger invalid_ledger;
  const auto invalid_begin = BeginStrictBulkLoad(&invalid_ledger, BeginRequest(fixture));
  ok &= Require(invalid_begin.ok(), "invalid fixture begins");
  const auto invalid_append = AppendStrictBulkLoadRows(
      &invalid_ledger,
      AppendRequest(fixture, invalid_begin.operation.bulk_load_id, {Row(11300, "bad", false)}));
  ok &= Require(!invalid_append.ok(), "constraint/domain/index/policy failure refused");
  const auto* invalid_operation = FindStrictBulkLoadOperation(invalid_ledger, invalid_begin.operation.bulk_load_id);
  ok &= Require(invalid_operation != nullptr && invalid_operation->state == StrictBulkLoadState::rolled_back,
                "invalid append rolls back operation");
  ok &= Require(invalid_operation != nullptr && invalid_operation->visible_rows.empty(),
                "invalid append has no partial visibility");

  StrictBulkLoadLedger failure_ledger;
  const auto failure_begin = BeginStrictBulkLoad(&failure_ledger, BeginRequest(fixture));
  ok &= Require(failure_begin.ok(), "failure fixture begins");
  const auto failure_append = AppendStrictBulkLoadRows(
      &failure_ledger,
      AppendRequest(fixture, failure_begin.operation.bulk_load_id, {Row(11400, "c")}));
  ok &= Require(failure_append.ok(), "failure fixture appends");
  auto failure_finalize_request = FinalizeRequest(fixture, failure_begin.operation.bulk_load_id);
  failure_finalize_request.simulate_finalize_failure_after_evidence = true;
  const auto failure_finalize = FinalizeStrictBulkLoad(&failure_ledger, failure_finalize_request);
  ok &= Require(!failure_finalize.ok(), "simulated finalize failure refused success");
  ok &= Require(failure_finalize.recovery_required, "simulated finalize failure requires recovery");
  const auto* failure_operation = FindStrictBulkLoadOperation(failure_ledger, failure_begin.operation.bulk_load_id);
  ok &= Require(failure_operation != nullptr && failure_operation->state == StrictBulkLoadState::recovery_required,
                "failure state requires recovery");
  ok &= Require(failure_operation != nullptr && failure_operation->visible_rows.empty(),
                "failure after evidence has no partial visibility");
  const auto recovery = ClassifyStrictBulkLoadLedgerForRecovery(failure_ledger);
  ok &= Require(recovery.ok(), "strict bulk-load recovery classification succeeds");
  ok &= Require(!recovery.classifications.empty(), "strict bulk-load recovery rows produced");
  ok &= Require(recovery.classifications.front().action == StrictBulkLoadRecoveryAction::complete_publication,
                "recovery-required finalize maps to complete publication");

  StrictBulkLoadLedger donor_ledger;
  auto donor_request = BeginRequest(fixture);
  donor_request.donor_relaxed_semantics_requested = true;
  const auto donor_refused = BeginStrictBulkLoad(&donor_ledger, donor_request);
  ok &= Require(!donor_refused.ok(), "donor relaxed semantics refused by default");
  ok &= Require(donor_refused.diagnostic.diagnostic_code == "strict_bulk_load_donor_relaxed_refused",
                "donor relaxed diagnostic");

  return ok ? 0 : 1;
}
