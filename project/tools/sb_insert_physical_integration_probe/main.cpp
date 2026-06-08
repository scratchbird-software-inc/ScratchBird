// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_growth.hpp"
#include "insert_physical_integration.hpp"
#include "page_selection.hpp"
#include "secondary_index_delta_overlay.hpp"
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

std::vector<scratchbird::core::platform::byte> Payload(std::size_t size) {
  std::vector<scratchbird::core::platform::byte> payload;
  payload.reserve(size);
  for (std::size_t i = 0; i < size; ++i) {
    payload.push_back(static_cast<scratchbird::core::platform::byte>((i * 19) % 251));
  }
  return payload;
}

struct Fixture {
  scratchbird::core::platform::TypedUuid database_uuid = Id(scratchbird::core::platform::UuidKind::database, 12100);
  scratchbird::core::platform::TypedUuid object_uuid = Id(scratchbird::core::platform::UuidKind::object, 12101);
  scratchbird::core::platform::TypedUuid transaction_uuid = Id(scratchbird::core::platform::UuidKind::transaction, 12102);
  scratchbird::core::platform::TypedUuid filespace_uuid = Id(scratchbird::core::platform::UuidKind::filespace, 12103);
  scratchbird::core::platform::TypedUuid policy_uuid = Id(scratchbird::core::platform::UuidKind::object, 12104);
  scratchbird::core::platform::TypedUuid page_uuid = Id(scratchbird::core::platform::UuidKind::page, 12105);
  scratchbird::core::platform::TypedUuid row_uuid = Id(scratchbird::core::platform::UuidKind::row, 12106);
  scratchbird::core::platform::TypedUuid secondary_index_uuid = Id(scratchbird::core::platform::UuidKind::object, 12107);
};

scratchbird::storage::filespace::FilespaceRegistry Registry(const Fixture& fixture) {
  scratchbird::storage::filespace::FilespaceRegistry registry;
  scratchbird::storage::filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = fixture.database_uuid;
  descriptor.filespace_uuid = fixture.filespace_uuid;
  descriptor.role = scratchbird::storage::filespace::FilespaceRole::secondary_data;
  descriptor.state = scratchbird::storage::filespace::FilespaceState::online;
  descriptor.active = true;
  registry.filespaces.push_back(descriptor);
  return registry;
}

scratchbird::engine::internal_api::dml::InsertPhysicalIntegrationRequest Request(const Fixture& fixture) {
  scratchbird::engine::internal_api::dml::InsertPhysicalIntegrationRequest request;
  request.database_uuid = fixture.database_uuid;
  request.object_uuid = fixture.object_uuid;
  request.transaction_uuid = fixture.transaction_uuid;
  request.local_transaction_id = 88;
  request.filespace_uuid = fixture.filespace_uuid;
  request.policy_uuid = fixture.policy_uuid;
  request.request_id = Id(scratchbird::core::platform::UuidKind::object, 12200);
  request.page_family = "row_data";
  request.estimated_row_count = 1;
  request.estimated_payload_bytes = 1024;
  request.encoded_row_bytes = 512;
  return request;
}

scratchbird::core::bulk_load::StrictBulkLoadRow BulkRow(const Fixture& fixture) {
  scratchbird::core::bulk_load::StrictBulkLoadRow row;
  row.row_uuid = fixture.row_uuid;
  row.encoded_row = "bulk-row";
  return row;
}

}  // namespace

int main() {
  using namespace scratchbird::engine::internal_api::dml;

  bool ok = true;
  const Fixture fixture;

  scratchbird::storage::page::PageReservationLedger reservation_ledger;
  scratchbird::storage::page::PageSelectionLedger selection_ledger;
  scratchbird::storage::filespace::FilespaceGrowthLedger growth_ledger;
  auto registry = Registry(fixture);
  scratchbird::core::index::SecondaryIndexOverlayLedger overlay_ledger;
  scratchbird::core::index::SecondaryIndexDeltaMergeLedger merge_ledger;
  std::vector<scratchbird::core::index::SecondaryIndexBaseEntry> base_entries;
  scratchbird::core::index::SecondaryIndexDeltaLedger delta_ledger;
  scratchbird::storage::page::OverflowLedger overflow_ledger;
  scratchbird::core::bulk_load::StrictBulkLoadLedger bulk_load_ledger;

  scratchbird::storage::page::InsertPageCandidate candidate;
  candidate.database_uuid = fixture.database_uuid;
  candidate.filespace_uuid = fixture.filespace_uuid;
  candidate.object_uuid = fixture.object_uuid;
  candidate.page_uuid = fixture.page_uuid;
  candidate.page_family = "row_data";
  candidate.page_number = 12;
  candidate.page_generation = 1;
  candidate.free_bytes = 4096;
  ok &= Require(scratchbird::storage::page::RegisterInsertPageCandidate(&selection_ledger, candidate).ok(),
                "candidate registered");

  scratchbird::core::index::SecondaryIndexDeltaEntry delta;
  delta.delta_id = Id(scratchbird::core::platform::UuidKind::object, 12300);
  delta.index_uuid = fixture.secondary_index_uuid;
  delta.table_uuid = fixture.object_uuid;
  delta.row_uuid = fixture.row_uuid;
  delta.version_uuid = Id(scratchbird::core::platform::UuidKind::row, 12301);
  delta.transaction_uuid = fixture.transaction_uuid;
  delta.local_transaction_id = 88;
  delta.delta_kind = scratchbird::core::index::SecondaryIndexDeltaKind::insert;
  delta.key_payload = "idx-key";
  delta.cleanup_horizon_token = "horizon-token";
  delta.committed = true;
  delta_ledger.deltas.push_back(delta);

  InsertPhysicalIntegrationContext context;
  context.page_reservation_ledger = &reservation_ledger;
  context.page_selection_ledger = &selection_ledger;
  context.filespace_growth_ledger = &growth_ledger;
  context.filespace_registry = &registry;
  context.secondary_index_overlay_ledger = &overlay_ledger;
  context.secondary_index_merge_ledger = &merge_ledger;
  context.secondary_index_base_entries = &base_entries;
  context.secondary_index_delta_ledger = &delta_ledger;
  context.overflow_ledger = &overflow_ledger;
  context.strict_bulk_load_ledger = &bulk_load_ledger;

  auto request = Request(fixture);
  request.enable_deferred_secondary_index = true;
  request.deferred_index_overlay_gate = true;
  request.deferred_index_merge_gate = true;
  request.deferred_index_cleanup_gate = true;
  request.deferred_index_recovery_gate = true;
  request.secondary_index_uuid = fixture.secondary_index_uuid;
  request.secondary_index_snapshot_high_water_local_transaction_id = 100;
  request.secondary_index_cleanup_horizon_local_transaction_id = 100;
  request.persist_overflow_payload = true;
  request.row_uuid = fixture.row_uuid;
  request.overflow_value_descriptor = "descriptor:blob.binary";
  request.overflow_payload = Payload(3000);
  request.overflow_chunk_size = 1024;
  request.run_strict_bulk_load = true;
  request.strict_bulk_load_policy.policy_uuid = fixture.policy_uuid;
  request.strict_bulk_load_policy.enabled = true;
  request.strict_bulk_load_staging_target = "staging:physical-integration";
  request.strict_bulk_load_rows.push_back(BulkRow(fixture));

  const auto integrated = ExecuteInsertPhysicalIntegration(&context, request);
  ok &= Require(integrated.ok(), "physical integration succeeds");
  ok &= Require(integrated.page_reserved, "page reservation called");
  ok &= Require(integrated.page_selected, "page selection called");
  ok &= Require(integrated.deferred_secondary_index_verified, "deferred secondary index gates verified");
  ok &= Require(integrated.overflow_persisted, "overflow persistence called");
  ok &= Require(integrated.strict_bulk_load_finalized, "strict bulk-load finalized");
  ok &= Require(!integrated.evidence_refs.empty(), "integration evidence refs populated");
  ok &= Require(!base_entries.empty(), "secondary index merge populated base index");
  ok &= Require(!overflow_ledger.values.empty() &&
                    overflow_ledger.values.front().state == scratchbird::storage::page::OverflowValueState::committed_visible,
                "overflow value committed visible");
  ok &= Require(!bulk_load_ledger.operations.empty() &&
                    bulk_load_ledger.operations.front().state ==
                        scratchbird::core::bulk_load::StrictBulkLoadState::published_visible,
                "strict bulk-load published visible");

  auto gated_refusal_request = Request(fixture);
  gated_refusal_request.enable_deferred_secondary_index = true;
  gated_refusal_request.secondary_index_uuid = fixture.secondary_index_uuid;
  const auto gated_refusal = ExecuteInsertPhysicalIntegration(&context, gated_refusal_request);
  ok &= Require(!gated_refusal.ok(), "deferred index without proven gates refused");
  ok &= Require(gated_refusal.diagnostic.diagnostic_code ==
                    "insert_physical_integration_deferred_index_gates_unproven",
                "deferred index gate diagnostic");

  scratchbird::storage::page::PageReservationLedger growth_reservation_ledger;
  scratchbird::storage::page::PageSelectionLedger growth_selection_ledger;
  InsertPhysicalIntegrationContext growth_context;
  growth_context.page_reservation_ledger = &growth_reservation_ledger;
  growth_context.page_selection_ledger = &growth_selection_ledger;
  growth_context.filespace_growth_ledger = &growth_ledger;
  growth_context.filespace_registry = &registry;
  auto growth_request = Request(fixture);
  growth_request.request_id = Id(scratchbird::core::platform::UuidKind::object, 12400);
  growth_request.request_filespace_growth_on_missing_page = true;
  growth_request.growth_page_count = 4;
  growth_request.predicted_insert_pressure_pages = 2;
  const auto growth_integrated = ExecuteInsertPhysicalIntegration(&growth_context, growth_request);
  ok &= Require(growth_integrated.ok(), "missing page can route through filespace growth admission");
  ok &= Require(growth_integrated.filespace_growth_admitted, "filespace growth admission called");
  ok &= Require(!growth_integrated.integrated, "growth admission does not report row integration");
  ok &= Require(!growth_ledger.operations.empty(), "growth operation recorded");

  return ok ? 0 : 1;
}
