// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/insert_physical_integration.hpp"
#include "filespace_lifecycle.hpp"
#include "overflow_persistence.hpp"
#include "page_selection.hpp"
#include "strict_bulk_load_lifecycle.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace dml = scratchbird::engine::internal_api::dml;
namespace filespace = scratchbird::storage::filespace;
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
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1916000000000ull + seed);
  Require(generated.ok(), "IPAR-P3-06 UUID generation failed");
  return generated.value;
}

struct FixtureIds {
  platform::TypedUuid database_uuid = MakeUuid(platform::UuidKind::database, 1);
  platform::TypedUuid transaction_uuid = MakeUuid(platform::UuidKind::transaction, 2);
  platform::TypedUuid policy_uuid = MakeUuid(platform::UuidKind::object, 3);
  platform::TypedUuid table_object_uuid = MakeUuid(platform::UuidKind::object, 4);
  platform::TypedUuid index_object_uuid = MakeUuid(platform::UuidKind::object, 5);
  platform::TypedUuid blob_object_uuid = MakeUuid(platform::UuidKind::object, 6);
  platform::TypedUuid temp_object_uuid = MakeUuid(platform::UuidKind::object, 7);
  platform::TypedUuid data_filespace_uuid = MakeUuid(platform::UuidKind::filespace, 10);
  platform::TypedUuid index_filespace_uuid = MakeUuid(platform::UuidKind::filespace, 11);
  platform::TypedUuid overflow_filespace_uuid = MakeUuid(platform::UuidKind::filespace, 12);
  platform::TypedUuid temp_filespace_uuid = MakeUuid(platform::UuidKind::filespace, 13);
};

filespace::FilespaceDescriptor Descriptor(const FixtureIds& ids,
                                          platform::TypedUuid filespace_uuid,
                                          filespace::FilespaceRole role,
                                          platform::u64 seed) {
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = ids.database_uuid;
  descriptor.filespace_uuid = filespace_uuid;
  descriptor.path = "ipar-p3-06-" + std::to_string(seed) + ".sbfs";
  descriptor.role = role;
  descriptor.state = filespace::FilespaceState::online;
  descriptor.page_size = 16 * 1024;
  descriptor.generation = 50 + seed;
  descriptor.read_only = false;
  descriptor.active = true;
  descriptor.physical_filespace_id = static_cast<platform::u16>(seed);
  descriptor.total_pages = 128;
  descriptor.free_pages = 64;
  descriptor.preallocated_pages = 4;
  descriptor.allocation_root_page = 1;
  descriptor.header_generation = 9;
  descriptor.writer_identity_uuid = MakeUuid(platform::UuidKind::object, 100 + seed);
  return descriptor;
}

filespace::FilespaceRegistry Registry(const FixtureIds& ids) {
  filespace::FilespaceRegistry registry;
  registry.filespaces.push_back(
      Descriptor(ids, ids.data_filespace_uuid, filespace::FilespaceRole::secondary_data, 1));
  registry.filespaces.push_back(
      Descriptor(ids, ids.index_filespace_uuid, filespace::FilespaceRole::secondary_index, 2));
  registry.filespaces.push_back(
      Descriptor(ids, ids.overflow_filespace_uuid, filespace::FilespaceRole::secondary_overflow, 3));
  registry.filespaces.push_back(
      Descriptor(ids, ids.temp_filespace_uuid, filespace::FilespaceRole::temporary, 4));
  return registry;
}

filespace::FilespacePlacementPolicy PlacementPolicy(const FixtureIds& ids) {
  filespace::FilespacePlacementPolicy policy;
  policy.present = true;
  policy.require_explicit_binding = true;
  policy.default_preallocate_page_count = 2;
  policy.bindings.push_back({filespace::FilespaceObjectClass::hot_row,
                             "row_data",
                             ids.data_filespace_uuid,
                             true,
                             2});
  policy.bindings.push_back({filespace::FilespaceObjectClass::exact_index,
                             "index",
                             ids.index_filespace_uuid,
                             true,
                             3});
  policy.bindings.push_back({filespace::FilespaceObjectClass::large_blob,
                             "overflow",
                             ids.overflow_filespace_uuid,
                             true,
                             4});
  policy.bindings.push_back({filespace::FilespaceObjectClass::temp_spill,
                             "overflow",
                             ids.temp_filespace_uuid,
                             true,
                             5});
  return policy;
}

void RegisterCandidate(page::PageSelectionLedger* ledger,
                       const FixtureIds& ids,
                       platform::TypedUuid filespace_uuid,
                       platform::TypedUuid object_uuid,
                       std::string page_family,
                       platform::u64 seed,
                       platform::u64 free_bytes = 8192) {
  page::InsertPageCandidate candidate;
  candidate.database_uuid = ids.database_uuid;
  candidate.filespace_uuid = filespace_uuid;
  candidate.object_uuid = object_uuid;
  candidate.page_uuid = MakeUuid(platform::UuidKind::page, 1000 + seed);
  candidate.page_family = std::move(page_family);
  candidate.page_number = 100 + seed;
  candidate.page_generation = 1;
  candidate.free_bytes = free_bytes;
  const auto registered = page::RegisterInsertPageCandidate(ledger, candidate);
  Require(registered.ok(), "IPAR-P3-06 page candidate registration failed");
}

struct RunResult {
  dml::InsertPhysicalIntegrationResult result;
  page::PageReservationLedger reservation_ledger;
  page::PageSelectionLedger selection_ledger;
  filespace::FilespaceGrowthLedger growth_ledger;
  scratchbird::storage::page::OverflowLedger overflow_ledger;
  scratchbird::core::bulk_load::StrictBulkLoadLedger strict_bulk_ledger;
};

RunResult RunPlacement(const FixtureIds& ids,
                       const filespace::FilespaceRegistry& registry,
                       const filespace::FilespacePlacementPolicy& policy,
                       platform::TypedUuid object_uuid,
                       platform::TypedUuid expected_filespace_uuid,
                       filespace::FilespaceObjectClass object_class,
                       std::string page_family,
                       platform::u64 seed,
                       bool persist_overflow = false) {
  RunResult run;
  dml::InsertPhysicalIntegrationContext context;
  context.page_reservation_ledger = &run.reservation_ledger;
  context.page_selection_ledger = &run.selection_ledger;
  context.filespace_growth_ledger = &run.growth_ledger;
  context.filespace_registry = &registry;
  context.overflow_ledger = &run.overflow_ledger;
  context.strict_bulk_load_ledger = &run.strict_bulk_ledger;

  RegisterCandidate(&run.selection_ledger,
                    ids,
                    ids.data_filespace_uuid,
                    object_uuid,
                    page_family,
                    4000 + seed);
  RegisterCandidate(&run.selection_ledger,
                    ids,
                    expected_filespace_uuid,
                    object_uuid,
                    page_family,
                    5000 + seed);

  dml::InsertPhysicalIntegrationRequest request;
  request.database_uuid = ids.database_uuid;
  request.object_uuid = object_uuid;
  request.transaction_uuid = ids.transaction_uuid;
  request.local_transaction_id = 77 + seed;
  request.policy_uuid = ids.policy_uuid;
  request.request_id = MakeUuid(platform::UuidKind::object, 2000 + seed);
  request.page_family = std::move(page_family);
  request.estimated_row_count = 8;
  request.estimated_payload_bytes = 4096;
  request.encoded_row_bytes = 128;
  request.page_size = 16 * 1024;
  request.placement_object_class = object_class;
  request.placement_policy = policy;
  request.require_placement_policy = true;
  request.require_placement_preallocation = true;
  request.placement_preallocation_pages = 2 + seed;
  if (persist_overflow) {
    request.persist_overflow_payload = true;
    request.row_uuid = MakeUuid(platform::UuidKind::object, 3000 + seed);
    request.overflow_value_descriptor = "canonical=character";
    request.overflow_payload.assign(9000, static_cast<platform::byte>('L'));
    request.overflow_chunk_size = 1024;
  }

  run.result = dml::ExecuteInsertPhysicalIntegration(&context, request);
  return run;
}

void RequirePlaced(const RunResult& run,
                   platform::TypedUuid expected_filespace_uuid,
                   std::string_view expected_class,
                   std::string_view label) {
  Require(run.result.ok(), std::string(label) + " integration failed");
  Require(run.result.filespace_placement_resolved,
          std::string(label) + " placement was not resolved");
  Require(run.result.filespace_preallocation_admitted,
          std::string(label) + " preallocation was not admitted");
  Require(run.result.resolved_filespace_uuid.value == expected_filespace_uuid.value,
          std::string(label) + " resolved wrong filespace");
  Require(run.result.resolved_filespace_class == expected_class,
          std::string(label) + " resolved wrong filespace class");
  Require(run.reservation_ledger.reservations.size() == 1,
          std::string(label) + " reservation count mismatch");
  Require(run.reservation_ledger.reservations.front().filespace_uuid.value ==
              expected_filespace_uuid.value,
          std::string(label) + " reservation filespace mismatch");
  Require(run.selection_ledger.selections.size() == 1,
          std::string(label) + " selection count mismatch");
  Require(run.selection_ledger.selections.front().filespace_uuid.value ==
              expected_filespace_uuid.value,
          std::string(label) + " selected wrong filespace");
  Require(run.growth_ledger.preallocation_operations.size() == 1,
          std::string(label) + " preallocation operation missing");
  Require(run.growth_ledger.preallocation_operations.front().filespace_uuid.value ==
              expected_filespace_uuid.value,
          std::string(label) + " preallocated wrong filespace");
}

void TestPolicyDrivenPlacementAcrossObjectClasses() {
  const FixtureIds ids;
  const auto registry = Registry(ids);
  const auto policy = PlacementPolicy(ids);

  const auto data = RunPlacement(ids,
                                 registry,
                                 policy,
                                 ids.table_object_uuid,
                                 ids.data_filespace_uuid,
                                 filespace::FilespaceObjectClass::hot_row,
                                 "row_data",
                                 1);
  RequirePlaced(data, ids.data_filespace_uuid, "hot_row", "table data");

  const auto index = RunPlacement(ids,
                                  registry,
                                  policy,
                                  ids.index_object_uuid,
                                  ids.index_filespace_uuid,
                                  filespace::FilespaceObjectClass::exact_index,
                                  "index",
                                  2);
  RequirePlaced(index, ids.index_filespace_uuid, "exact_index", "index data");

  const auto large = RunPlacement(ids,
                                  registry,
                                  policy,
                                  ids.blob_object_uuid,
                                  ids.overflow_filespace_uuid,
                                  filespace::FilespaceObjectClass::large_blob,
                                  "overflow",
                                  3,
                                  true);
  RequirePlaced(large, ids.overflow_filespace_uuid, "large_blob", "large value");
  Require(large.result.overflow_persisted,
          "large value placement did not persist overflow payload");

  const auto temp = RunPlacement(ids,
                                 registry,
                                 policy,
                                 ids.temp_object_uuid,
                                 ids.temp_filespace_uuid,
                                 filespace::FilespaceObjectClass::temp_spill,
                                 "overflow",
                                 4);
  RequirePlaced(temp, ids.temp_filespace_uuid, "temp_spill", "temp work");
}

void TestRoleMismatchRefusesBeforeReservation() {
  const FixtureIds ids;
  const auto registry = Registry(ids);
  auto policy = PlacementPolicy(ids);
  for (auto& binding : policy.bindings) {
    if (binding.object_class == filespace::FilespaceObjectClass::exact_index) {
      binding.filespace_uuid = ids.data_filespace_uuid;
    }
  }

  page::PageReservationLedger reservation_ledger;
  page::PageSelectionLedger selection_ledger;
  filespace::FilespaceGrowthLedger growth_ledger;
  dml::InsertPhysicalIntegrationContext context;
  context.page_reservation_ledger = &reservation_ledger;
  context.page_selection_ledger = &selection_ledger;
  context.filespace_growth_ledger = &growth_ledger;
  context.filespace_registry = &registry;

  dml::InsertPhysicalIntegrationRequest request;
  request.database_uuid = ids.database_uuid;
  request.object_uuid = ids.index_object_uuid;
  request.transaction_uuid = ids.transaction_uuid;
  request.local_transaction_id = 200;
  request.policy_uuid = ids.policy_uuid;
  request.request_id = MakeUuid(platform::UuidKind::object, 9000);
  request.page_family = "index";
  request.estimated_row_count = 1;
  request.estimated_payload_bytes = 256;
  request.encoded_row_bytes = 128;
  request.page_size = 16 * 1024;
  request.placement_object_class = filespace::FilespaceObjectClass::exact_index;
  request.placement_policy = policy;
  request.require_placement_policy = true;
  request.require_placement_preallocation = true;

  const auto result = dml::ExecuteInsertPhysicalIntegration(&context, request);
  Require(!result.ok(), "role-mismatched placement unexpectedly succeeded");
  Require(result.diagnostic.diagnostic_code == "SB-FILESPACE-PLACEMENT-ROLE-MISMATCH",
          "role-mismatched placement returned wrong diagnostic");
  Require(reservation_ledger.reservations.empty(),
          "role-mismatched placement reserved pages");
  Require(growth_ledger.preallocation_operations.empty(),
          "role-mismatched placement preallocated pages");
}

}  // namespace

int main() {
  TestPolicyDrivenPlacementAcrossObjectClasses();
  TestRoleMismatchRefusesBeforeReservation();
  std::cout << "ipar_multi_filespace_placement_gate=passed\n";
  return EXIT_SUCCESS;
}
