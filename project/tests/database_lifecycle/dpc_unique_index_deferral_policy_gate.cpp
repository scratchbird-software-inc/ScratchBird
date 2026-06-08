// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "secondary_index_delta_overlay.hpp"
#include "unique_index_deferral_policy.hpp"
#include "uuid.hpp"
#include "dml/delete_batch.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace idx = scratchbird::core::index;
namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kPolicySearchKey = "DPC_UNIQUE_INDEX_DEFERRAL_POLICY";
constexpr std::string_view kGateSearchKey = "DPC_UNIQUE_INDEX_DEFERRAL_POLICY_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

struct UuidFactory {
  std::uint64_t base_millis = NowMillis();

  TypedUuid Typed(UuidKind kind, std::uint64_t salt) const {
    const auto generated = uuid::GenerateEngineIdentityV7(kind, base_millis + salt);
    Require(generated.ok(), "DPC-019 UUID generation failed");
    return generated.value;
  }
};

idx::UniqueIndexDeferralPolicyDecision Evaluate(
    idx::IndexUniquenessClass uniqueness,
    idx::IndexDeferralRequestKind request_kind,
    bool non_unique_policy_enabled = false,
    bool reservation_present = false,
    bool reservation_proven = false,
    bool reservation_enabled = false) {
  idx::UniqueIndexDeferralPolicyRequest request;
  request.uniqueness = uniqueness;
  request.request_kind = request_kind;
  request.non_unique_deferral_policy_enabled = non_unique_policy_enabled;
  request.reservation_protocol_present = reservation_present;
  request.reservation_protocol_proven = reservation_proven;
  request.reservation_protocol_enabled = reservation_enabled;
  return idx::EvaluateUniqueIndexDeferralPolicy(request);
}

api::CrudTableRecord Table() {
  api::CrudTableRecord table;
  table.creator_tx = 42;
  table.table_uuid = "table-dpc-019";
  table.default_name = "dpc019_table";
  table.columns.push_back({"id", "canonical=int64"});
  table.columns.push_back({"name", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(std::string uuid,
                           std::string column,
                           std::string family,
                           bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = 42;
  index.index_uuid = std::move(uuid);
  index.table_uuid = "table-dpc-019";
  index.column_name = std::move(column);
  index.family = std::move(family);
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = unique;
  if (unique) {
    index.key_envelopes.push_back("unique");
  }
  return index;
}

api::CrudState State() {
  api::CrudState state;
  state.transactions[42] = "active";
  state.tables.push_back(Table());
  api::CrudRowVersionRecord row;
  row.creator_tx = 42;
  row.table_uuid = "table-dpc-019";
  row.row_uuid = "row-dpc-019";
  row.version_uuid = "version-dpc-019";
  row.values.push_back({"id", "1"});
  row.values.push_back({"name", "alpha"});
  state.row_versions.push_back(std::move(row));
  return state;
}

api::EngineDeleteRowsRequest DeleteRequest() {
  api::EngineDeleteRowsRequest request;
  request.context.request_id = "dpc-019-delete";
  request.context.database_uuid.canonical = "database-dpc-019";
  request.context.principal_uuid.canonical = "principal-dpc-019";
  request.context.transaction_uuid.canonical = "transaction-dpc-019";
  request.context.local_transaction_id = 42;
  request.context.snapshot_visible_through_local_transaction_id = 42;
  request.context.security_context_present = true;
  request.target_table.uuid.canonical = "table-dpc-019";
  request.delete_predicate.predicate_kind = "row_uuid_match";
  request.tombstone_only = true;
  return request;
}

bool HasDeleteActionForIndex(const api::DeleteIndexMaintenancePlan& plan,
                             const std::string& index_uuid,
                             api::DeleteIndexMaintenanceAction action) {
  for (const auto& entry : plan.entries) {
    if (entry.index.index_uuid == index_uuid && entry.action == action) {
      return true;
    }
  }
  return false;
}

void RequireDiagnostic(const idx::UniqueIndexDeferralPolicyDecision& decision,
                       std::string_view diagnostic_code,
                       std::string_view message_key,
                       std::string_view message) {
  if (decision.diagnostic.diagnostic_code != diagnostic_code ||
      decision.diagnostic.message_key != message_key) {
    std::cerr << "expected_code=" << diagnostic_code
              << " actual_code=" << decision.diagnostic.diagnostic_code
              << " expected_key=" << message_key
              << " actual_key=" << decision.diagnostic.message_key << '\n';
  }
  Require(decision.diagnostic.diagnostic_code == diagnostic_code, message);
  Require(decision.diagnostic.message_key == message_key, message);
}

void RequireRefused(const idx::UniqueIndexDeferralPolicyDecision& decision,
                    std::string_view diagnostic_code,
                    std::string_view message_key,
                    std::string_view message) {
  Require(!decision.ok(), message);
  Require(!decision.accepted, message);
  Require(!decision.deferred_eligible, message);
  Require(decision.synchronous_required, message);
  Require(decision.status.code == StatusCode::platform_required_feature_missing, message);
  RequireDiagnostic(decision, diagnostic_code, message_key, message);
}

void ValidateUniqueIndexesDefaultSynchronous() {
  const auto primary = Evaluate(idx::IndexUniquenessClass::unique_primary,
                                idx::IndexDeferralRequestKind::synchronous_immediate);
  Require(primary.ok(), "DPC-019 unique primary synchronous policy refused");
  Require(primary.accepted, "DPC-019 unique primary synchronous policy was not accepted");
  Require(!primary.deferred_eligible, "DPC-019 unique primary defaulted to deferred");
  Require(primary.synchronous_required, "DPC-019 unique primary did not require synchronous maintenance");
  RequireDiagnostic(primary,
                    "INDEX.UNIQUE_DEFERRAL.SYNCHRONOUS_REQUIRED",
                    "core.index.deferral.unique_synchronous_required",
                    "DPC-019 unique primary synchronous diagnostic changed");

  const auto secondary = Evaluate(idx::IndexUniquenessClass::unique_secondary,
                                  idx::IndexDeferralRequestKind::synchronous_immediate);
  Require(secondary.ok(), "DPC-019 unique secondary synchronous policy refused");
  Require(!secondary.deferred_eligible, "DPC-019 unique secondary defaulted to deferred");
  Require(secondary.synchronous_required, "DPC-019 unique secondary did not require synchronous maintenance");
  RequireDiagnostic(secondary,
                    "INDEX.UNIQUE_DEFERRAL.SYNCHRONOUS_REQUIRED",
                    "core.index.deferral.unique_synchronous_required",
                    "DPC-019 unique secondary synchronous diagnostic changed");
}

void ValidateGenericDeferralRefusesUniqueIndexes() {
  const auto primary = Evaluate(idx::IndexUniquenessClass::unique_primary,
                                idx::IndexDeferralRequestKind::generic_deferred);
  RequireRefused(primary,
                 "INDEX.UNIQUE_DEFERRAL.FORBIDDEN",
                 "core.index.deferral.unique_forbidden",
                 "DPC-019 generic deferral did not refuse unique primary index");
  Require(primary.reservation_protocol_required,
          "DPC-019 generic deferral did not require reservation evidence for unique primary");

  const auto secondary = Evaluate(idx::IndexUniquenessClass::unique_secondary,
                                  idx::IndexDeferralRequestKind::generic_deferred);
  RequireRefused(secondary,
                 "INDEX.UNIQUE_DEFERRAL.FORBIDDEN",
                 "core.index.deferral.unique_forbidden",
                 "DPC-019 generic deferral did not refuse unique secondary index");
  Require(secondary.reservation_protocol_required,
          "DPC-019 generic deferral did not require reservation evidence for unique secondary");
}

void ValidateReservationProtocolGate() {
  const auto missing = Evaluate(
      idx::IndexUniquenessClass::unique_secondary,
      idx::IndexDeferralRequestKind::unique_deferred_with_reservation);
  RequireRefused(missing,
                 "INDEX.UNIQUE_DEFERRAL.RESERVATION_REQUIRED",
                 "core.index.deferral.unique_reservation_required",
                 "DPC-019 missing reservation protocol diagnostic changed");
  Require(missing.reservation_protocol_required,
          "DPC-019 missing reservation protocol did not set reservation requirement");

  const auto disabled = Evaluate(
      idx::IndexUniquenessClass::unique_secondary,
      idx::IndexDeferralRequestKind::unique_deferred_with_reservation,
      false,
      true,
      true,
      false);
  RequireRefused(disabled,
                 "INDEX.UNIQUE_DEFERRAL.RESERVATION_REQUIRED",
                 "core.index.deferral.unique_reservation_required",
                 "DPC-019 disabled reservation protocol diagnostic changed");

  const auto unproven = Evaluate(
      idx::IndexUniquenessClass::unique_secondary,
      idx::IndexDeferralRequestKind::unique_deferred_with_reservation,
      false,
      true,
      false,
      true);
  RequireRefused(unproven,
                 "INDEX.UNIQUE_DEFERRAL.RESERVATION_UNPROVEN",
                 "core.index.deferral.unique_reservation_unproven",
                 "DPC-019 unproven reservation protocol diagnostic changed");

  const auto accepted = Evaluate(
      idx::IndexUniquenessClass::unique_secondary,
      idx::IndexDeferralRequestKind::unique_deferred_with_reservation,
      false,
      true,
      true,
      true);
  RequireRefused(accepted,
                 "INDEX.UNIQUE_DEFERRAL.RESERVATION_UNPROVEN",
                 "core.index.deferral.unique_reservation_unproven",
                 "DPC-019 accepted reservation flags without a real separate protocol gate");
  Require(accepted.reservation_protocol_required,
          "DPC-019 proven reservation protocol did not record reservation dependency");
}

void ValidateNonUniqueDeferralRemainsSeparate() {
  const auto deferred = Evaluate(idx::IndexUniquenessClass::non_unique_secondary,
                                 idx::IndexDeferralRequestKind::generic_deferred,
                                 true);
  Require(deferred.ok(), "DPC-019 non-unique deferred eligibility was refused");
  Require(deferred.deferred_eligible,
          "DPC-019 non-unique secondary was not deferred-eligible when policy allowed");
  Require(!deferred.synchronous_required,
          "DPC-019 non-unique deferred eligibility still required synchronous maintenance");
  Require(!deferred.reservation_protocol_required,
          "DPC-019 non-unique secondary incorrectly required unique reservation protocol");
  RequireDiagnostic(deferred,
                    "INDEX.DEFERRAL.NON_UNIQUE_ELIGIBLE",
                    "core.index.deferral.non_unique_eligible",
                    "DPC-019 non-unique deferred diagnostic changed");

  const auto disabled = Evaluate(idx::IndexUniquenessClass::non_unique_secondary,
                                 idx::IndexDeferralRequestKind::generic_deferred,
                                 false);
  Require(disabled.ok(), "DPC-019 disabled non-unique deferral should fall back synchronously");
  Require(!disabled.deferred_eligible,
          "DPC-019 disabled non-unique deferral was marked eligible");
  Require(disabled.synchronous_required,
          "DPC-019 disabled non-unique deferral did not require synchronous maintenance");
}

void ValidateOverlayCannotBypassUniquePolicy() {
  const UuidFactory uuids;
  idx::SecondaryIndexOverlayLedger overlay_ledger;
  const idx::SecondaryIndexDeltaLedger delta_ledger;
  const std::vector<idx::SecondaryIndexBaseEntry> base_entries;

  idx::SecondaryIndexOverlayRequest generic_request;
  generic_request.index_uuid = uuids.Typed(UuidKind::object, 1);
  generic_request.table_uuid = uuids.Typed(UuidKind::object, 2);
  generic_request.transaction_uuid = uuids.Typed(UuidKind::transaction, 3);
  generic_request.index_kind = idx::SecondaryIndexKind::unique;
  const auto generic = idx::BuildSecondaryIndexDeltaOverlay(
      &overlay_ledger, base_entries, delta_ledger, generic_request);
  Require(!generic.ok(), "DPC-019 unique overlay accepted generic deferred request");
  Require(generic.diagnostic.diagnostic_code == "INDEX.UNIQUE_DEFERRAL.FORBIDDEN",
          "DPC-019 unique overlay generic refusal diagnostic changed");
  Require(generic.diagnostic.message_key == "core.index.deferral.unique_forbidden",
          "DPC-019 unique overlay generic refusal message key changed");

  idx::SecondaryIndexOverlayRequest legacy_flag_request = generic_request;
  legacy_flag_request.allow_deferred_unique_overlay = true;
  const auto legacy_flag = idx::BuildSecondaryIndexDeltaOverlay(
      &overlay_ledger, base_entries, delta_ledger, legacy_flag_request);
  Require(!legacy_flag.ok(),
          "DPC-019 unique overlay accepted legacy unique deferral flag without reservation protocol");
  Require(legacy_flag.diagnostic.diagnostic_code ==
              "INDEX.UNIQUE_DEFERRAL.RESERVATION_REQUIRED",
          "DPC-019 unique overlay reservation-required diagnostic changed");
  Require(legacy_flag.diagnostic.message_key ==
              "core.index.deferral.unique_reservation_required",
          "DPC-019 unique overlay reservation-required message key changed");

  idx::SecondaryIndexOverlayRequest asserted_protocol_request = generic_request;
  asserted_protocol_request.allow_deferred_unique_overlay = true;
  asserted_protocol_request.unique_reservation_protocol_present = true;
  asserted_protocol_request.unique_reservation_protocol_proven = true;
  asserted_protocol_request.unique_reservation_protocol_enabled = true;
  const auto asserted_protocol = idx::BuildSecondaryIndexDeltaOverlay(
      &overlay_ledger, base_entries, delta_ledger, asserted_protocol_request);
  Require(!asserted_protocol.ok(),
          "DPC-019 unique overlay accepted asserted reservation protocol flags without a real protocol gate");
  Require(asserted_protocol.diagnostic.diagnostic_code ==
              "INDEX.UNIQUE_DEFERRAL.RESERVATION_UNPROVEN",
          "DPC-019 unique overlay asserted-protocol diagnostic changed");
  Require(asserted_protocol.diagnostic.message_key ==
              "core.index.deferral.unique_reservation_unproven",
          "DPC-019 unique overlay asserted-protocol message key changed");
}

void ValidateDeletePlannerCannotDeferUniqueIndexes() {
  const auto table = Table();
  const auto state = State();
  const std::vector<api::CrudIndexRecord> indexes = {
      Index("index-dpc-019-name", "name", api::kCrudIndexFamilyBtree, false),
      Index("index-dpc-019-id-unique", "id", api::kCrudIndexFamilyBtree, true)};

  api::DeleteFeatureGates gates;
  gates.secondary_index_delta_ledger = api::DeleteFeatureState::enabled;
  const auto plan = api::BuildDeleteIndexMaintenancePlan(
      DeleteRequest(), state, table, indexes, gates);

  Require(HasDeleteActionForIndex(plan,
                                  "index-dpc-019-name",
                                  api::DeleteIndexMaintenanceAction::tombstone_delta_ledger),
          "DPC-019 delete planner did not keep non-unique delta eligibility separate");
  Require(!HasDeleteActionForIndex(plan,
                                   "index-dpc-019-id-unique",
                                   api::DeleteIndexMaintenanceAction::tombstone_delta_ledger),
          "DPC-019 delete planner selected delta ledger for unique index");
  Require(HasDeleteActionForIndex(plan,
                                  "index-dpc-019-id-unique",
                                  api::DeleteIndexMaintenanceAction::visibility_recheck_only),
          "DPC-019 delete planner did not keep unique index on synchronous-safe recheck path");
}

}  // namespace

int main() {
  Require(!kPolicySearchKey.empty(), "DPC-019 policy search key missing");
  Require(!kGateSearchKey.empty(), "DPC-019 gate search key missing");

  ValidateUniqueIndexesDefaultSynchronous();
  ValidateGenericDeferralRefusesUniqueIndexes();
  ValidateReservationProtocolGate();
  ValidateNonUniqueDeferralRemainsSeparate();
  ValidateOverlayCannotBypassUniquePolicy();
  ValidateDeletePlannerCannotDeferUniqueIndexes();

  return EXIT_SUCCESS;
}
