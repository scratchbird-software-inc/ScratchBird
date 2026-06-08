// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_growth.hpp"
#include "uuid.hpp"

#include <iostream>
#include <string>

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
  scratchbird::core::platform::TypedUuid database_uuid = Id(scratchbird::core::platform::UuidKind::database, 4100);
  scratchbird::core::platform::TypedUuid filespace_uuid = Id(scratchbird::core::platform::UuidKind::filespace, 4101);
  scratchbird::core::platform::TypedUuid index_filespace_uuid = Id(scratchbird::core::platform::UuidKind::filespace, 4102);
  scratchbird::core::platform::TypedUuid policy_uuid = Id(scratchbird::core::platform::UuidKind::object, 4103);
};

scratchbird::storage::filespace::FilespaceDescriptor Descriptor(
    const Fixture& fixture,
    scratchbird::core::platform::TypedUuid filespace_uuid,
    scratchbird::storage::filespace::FilespaceRole role) {
  scratchbird::storage::filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = fixture.database_uuid;
  descriptor.filespace_uuid = filespace_uuid;
  descriptor.role = role;
  descriptor.state = scratchbird::storage::filespace::FilespaceState::online;
  descriptor.active = true;
  descriptor.read_only = false;
  return descriptor;
}

scratchbird::storage::filespace::InsertFilespaceGrowthRequest BaseRequest(const Fixture& fixture) {
  scratchbird::storage::filespace::InsertFilespaceGrowthRequest request;
  request.database_uuid = fixture.database_uuid;
  request.filespace_uuid = fixture.filespace_uuid;
  request.filespace_role = scratchbird::storage::filespace::FilespaceRole::secondary_data;
  request.page_family = "row_data";
  request.requested_page_count = 8;
  request.urgency_class = scratchbird::storage::filespace::InsertFilespaceGrowthUrgency::normal;
  request.predicted_insert_pressure_pages = 4;
  request.policy_uuid = fixture.policy_uuid;
  return request;
}

}  // namespace

int main() {
  using namespace scratchbird::storage::filespace;

  bool ok = true;
  const Fixture fixture;

  FilespaceRegistry registry;
  registry.filespaces.push_back(Descriptor(fixture, fixture.filespace_uuid, FilespaceRole::secondary_data));
  registry.filespaces.push_back(Descriptor(fixture, fixture.index_filespace_uuid, FilespaceRole::secondary_index));

  FilespaceGrowthLedger ledger;
  const auto admitted = RequestInsertFilespaceGrowth(&ledger, registry, BaseRequest(fixture));
  ok &= Require(admitted.ok(), "normal filespace growth admitted");
  ok &= Require(admitted.expected_usable_pages == 12, "expected usable pages includes predicted pressure");
  ok &= Require(admitted.wait_policy == InsertFilespaceGrowthWaitPolicy::bounded_wait, "normal urgency uses bounded wait");
  ok &= Require(admitted.operation.state == InsertFilespaceGrowthState::admitted_pending_allocation, "admitted state tracked");
  ok &= Require(admitted.evidence.durable_state_changed, "admission evidence durable state changed");
  ok &= Require(ledger.operations.size() == 1, "growth ledger has operation");
  ok &= Require(ledger.evidence.size() == 1, "growth ledger has evidence");
  ok &= Require(FindFilespaceGrowthOperation(ledger, admitted.growth_operation_id) != nullptr, "operation is findable");
  const auto completed = CompleteInsertFilespaceGrowth(
      &ledger,
      CompleteFilespaceGrowthRequest{admitted.growth_operation_id, 16, "probe_allocation_complete"});
  ok &= Require(completed.ok() && completed.changed, "growth allocation completion succeeds");
  ok &= Require(completed.operation.state == InsertFilespaceGrowthState::allocation_complete,
                "growth allocation completion state tracked");
  ok &= Require(completed.operation.expected_usable_pages == 16, "actual usable page count recorded");
  ok &= Require(ledger.evidence.size() == 2, "completion evidence appended");

  InsertFilespaceGrowthRequest role_request = BaseRequest(fixture);
  role_request.filespace_uuid = scratchbird::core::platform::TypedUuid{};
  role_request.filespace_role = FilespaceRole::secondary_index;
  role_request.page_family = "index";
  role_request.urgency_class = InsertFilespaceGrowthUrgency::critical;
  const auto role_selected = RequestInsertFilespaceGrowth(&ledger, registry, role_request);
  ok &= Require(role_selected.ok(), "role-selected filespace growth admitted");
  ok &= Require(role_selected.operation.filespace_uuid.kind == fixture.index_filespace_uuid.kind &&
                    role_selected.operation.filespace_uuid.value == fixture.index_filespace_uuid.value,
                "role selection chose index filespace");
  ok &= Require(role_selected.wait_policy == InsertFilespaceGrowthWaitPolicy::no_wait, "critical urgency uses no_wait");
  const auto undersized_completion = CompleteInsertFilespaceGrowth(
      &ledger,
      CompleteFilespaceGrowthRequest{role_selected.growth_operation_id, 4, "probe_undersized_allocation"});
  ok &= Require(!undersized_completion.ok(), "undersized growth completion refused");
  ok &= Require(undersized_completion.diagnostic.diagnostic_code ==
                    "insert_filespace_growth_actual_pages_below_expected",
                "undersized completion diagnostic");

  FilespaceGrowthLedger unknown_family_ledger;
  InsertFilespaceGrowthRequest unknown_family = BaseRequest(fixture);
  unknown_family.page_family = "not_a_family";
  const auto unknown = RequestInsertFilespaceGrowth(&unknown_family_ledger, registry, unknown_family);
  ok &= Require(!unknown.ok(), "unknown page family refused");
  ok &= Require(unknown.diagnostic.diagnostic_code == "insert_filespace_growth_unknown_page_family",
                "unknown family diagnostic");

  FilespaceGrowthLedger unavailable_ledger;
  FilespaceRegistry read_only_registry = registry;
  read_only_registry.filespaces.front().read_only = true;
  const auto unavailable = RequestInsertFilespaceGrowth(&unavailable_ledger, read_only_registry, BaseRequest(fixture));
  ok &= Require(!unavailable.ok(), "read-only filespace refused");
  ok &= Require(unavailable.diagnostic.diagnostic_code == "insert_filespace_growth_filespace_unavailable",
                "unavailable diagnostic");

  FilespaceGrowthLedger no_filespace_ledger;
  InsertFilespaceGrowthRequest no_filespace = BaseRequest(fixture);
  no_filespace.filespace_uuid = Id(scratchbird::core::platform::UuidKind::filespace, 4999);
  const auto missing = RequestInsertFilespaceGrowth(&no_filespace_ledger, registry, no_filespace);
  ok &= Require(!missing.ok(), "missing filespace refused");
  ok &= Require(missing.diagnostic.diagnostic_code == "insert_filespace_growth_no_filespace",
                "missing filespace diagnostic");

  InsertFilespaceGrowthRequest zero_pages = BaseRequest(fixture);
  zero_pages.requested_page_count = 0;
  const auto zero = RequestInsertFilespaceGrowth(&no_filespace_ledger, registry, zero_pages);
  ok &= Require(!zero.ok(), "zero page growth refused");
  ok &= Require(zero.diagnostic.diagnostic_code == "insert_filespace_growth_zero_pages",
                "zero page diagnostic");

  const auto recovery = ClassifyFilespaceGrowthLedgerForRecovery(ledger);
  ok &= Require(recovery.ok(), "growth recovery classification succeeds");
  ok &= Require(!recovery.classifications.empty(), "growth recovery classifications produced");
  ok &= Require(recovery.classifications.front().action == FilespaceGrowthRecoveryAction::no_action,
                "completed growth has no restart action");
  ok &= Require(recovery.classifications.size() > 1 &&
                    recovery.classifications[1].action == FilespaceGrowthRecoveryAction::complete,
                "admitted growth still completes on recovery");

  return ok ? 0 : 1;
}
