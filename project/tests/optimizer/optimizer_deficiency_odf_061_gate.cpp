// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_lifecycle.hpp"
#include "page_allocation_lifecycle.hpp"
#include "page_reservation.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

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

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1779610000000ull + salt);
  Require(generated.ok(), "ODF-061 UUID generation failed");
  return generated.value;
}

struct Ids {
  platform::TypedUuid database_uuid = NewUuid(platform::UuidKind::database, 1);
  platform::TypedUuid filespace_uuid = NewUuid(platform::UuidKind::filespace, 2);
  platform::TypedUuid object_uuid = NewUuid(platform::UuidKind::object, 3);
  platform::TypedUuid transaction_uuid = NewUuid(platform::UuidKind::transaction, 4);
  platform::TypedUuid policy_uuid = NewUuid(platform::UuidKind::object, 5);
};

void RequireNoRuntimeDocTokens(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs", "execution-plans", "contracts", "findings", "reference"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-061 runtime evidence leaked documentation token");
    }
  }
}

std::vector<std::string> AllocationEvidence(const page::PageAllocationEvidenceRecord& evidence) {
  return {evidence.action,
          evidence.filespace_class,
          evidence.filespace_class_reason,
          evidence.reason,
          evidence.diagnostic_code};
}

std::vector<std::string> ReservationEvidence(const page::PageReservationEvidenceRecord& evidence) {
  return {evidence.action,
          evidence.filespace_class,
          evidence.filespace_class_reason,
          evidence.reason,
          evidence.diagnostic_code};
}

filespace::FilespaceClassDecision Resolve(const Ids& ids,
                                          filespace::FilespaceObjectClass object_class,
                                          std::string page_family,
                                          platform::u64 owner_salt) {
  filespace::FilespaceClassRequest request;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = NewUuid(platform::UuidKind::filespace, 100 + owner_salt);
  request.owner_object_uuid = NewUuid(platform::UuidKind::object, 200 + owner_salt);
  request.object_class = object_class;
  request.page_family = std::move(page_family);
  request.reason = "odf061_runtime_gate";
  request.explicit_object_class = true;
  return filespace::ResolveFilespaceClass(request);
}

void AllRequiredClassesRouteSeparately() {
  const Ids ids;
  const std::vector<std::pair<filespace::FilespaceObjectClass, std::string>> cases = {
      {filespace::FilespaceObjectClass::hot_row, "data"},
      {filespace::FilespaceObjectClass::cold_row, "data"},
      {filespace::FilespaceObjectClass::secondary_delta_ledger, "index"},
      {filespace::FilespaceObjectClass::exact_index, "index"},
      {filespace::FilespaceObjectClass::immutable_generation, "index"},
      {filespace::FilespaceObjectClass::large_blob, "blob"},
      {filespace::FilespaceObjectClass::temp_spill, "overflow"},
      {filespace::FilespaceObjectClass::backup_stream, "archive"},
  };

  std::set<std::string> selected_classes;
  platform::u64 salt = 1;
  for (const auto& item : cases) {
    const auto decision = Resolve(ids, item.first, item.second, salt++);
    Require(decision.ok(), "ODF-061 filespace class resolver refused required class");
    Require(decision.object_class == item.first,
            "ODF-061 filespace class resolver changed object class");
    Require(decision.filespace_class != filespace::FilespaceClass::unknown &&
                decision.filespace_class != filespace::FilespaceClass::forbidden,
            "ODF-061 filespace class resolver selected non-runtime class");
    Require(selected_classes.insert(filespace::FilespaceClassName(decision.filespace_class)).second,
            "ODF-061 filespace class resolver did not keep classes separate");
    Require(decision.recommended_role != filespace::FilespaceRole::unknown,
            "ODF-061 filespace class resolver missed recommended role");
    RequireNoRuntimeDocTokens(decision.evidence);
  }
  Require(selected_classes.size() == cases.size(),
          "ODF-061 filespace class resolver collapsed required classes");
}

void InvalidAndMissingIdentityFailsClosed() {
  const Ids ids;
  filespace::FilespaceClassRequest missing_filespace;
  missing_filespace.database_uuid = ids.database_uuid;
  missing_filespace.owner_object_uuid = ids.object_uuid;
  missing_filespace.object_class = filespace::FilespaceObjectClass::exact_index;
  missing_filespace.page_family = "index";
  missing_filespace.explicit_object_class = true;
  const auto no_filespace = filespace::ResolveFilespaceClass(missing_filespace);
  Require(!no_filespace.ok() &&
              no_filespace.filespace_class == filespace::FilespaceClass::forbidden,
          "ODF-061 missing filespace identity did not fail closed");
  RequireNoRuntimeDocTokens(no_filespace.evidence);

  filespace::FilespaceClassRequest missing_owner = missing_filespace;
  missing_owner.filespace_uuid = ids.filespace_uuid;
  missing_owner.owner_object_uuid = platform::TypedUuid{};
  const auto no_owner = filespace::ResolveFilespaceClass(missing_owner);
  Require(!no_owner.ok() &&
              no_owner.diagnostic.diagnostic_code ==
                  "SB-FILESPACE-CLASS-OWNER-OBJECT-UUID-INVALID",
          "ODF-061 explicit object class accepted missing owner identity");

  page::PageAllocationLedger ledger;
  ledger.database_uuid = ids.database_uuid;
  ledger.filespace_uuid = ids.filespace_uuid;
  ledger.free_extents.push_back({100, 8});
  page::PageAllocationRequest request;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.creator_transaction_uuid = ids.transaction_uuid;
  request.creator_local_transaction_id = 61;
  request.page_family = "index";
  request.object_class = filespace::FilespaceObjectClass::exact_index;
  request.page_count = 1;
  request.engine_authoritative = true;
  request.explicit_filespace_class = true;
  const auto refused = page::ReservePageAllocation(&ledger, request);
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "SB-FILESPACE-CLASS-OWNER-OBJECT-UUID-INVALID",
          "ODF-061 page allocation accepted explicit class with missing owner");
  RequireNoRuntimeDocTokens(AllocationEvidence(refused.evidence));
}

void AllocationAndReservationCarryClassEvidence() {
  const Ids ids;
  page::PageAllocationLedger ledger;
  ledger.database_uuid = ids.database_uuid;
  ledger.filespace_uuid = ids.filespace_uuid;
  ledger.free_extents.push_back({200, 16});

  page::PageAllocationRequest allocation;
  allocation.database_uuid = ids.database_uuid;
  allocation.filespace_uuid = ids.filespace_uuid;
  allocation.owner_object_uuid = ids.object_uuid;
  allocation.creator_transaction_uuid = ids.transaction_uuid;
  allocation.creator_local_transaction_id = 62;
  allocation.page_family = "index";
  allocation.object_class = filespace::FilespaceObjectClass::secondary_delta_ledger;
  allocation.page_count = 2;
  allocation.engine_authoritative = true;
  allocation.explicit_filespace_class = true;
  const auto allocated = page::ReservePageAllocation(&ledger, allocation);
  Require(allocated.ok(), "ODF-061 explicit filespace class allocation refused");
  Require(allocated.allocation.filespace_class == "secondary_delta_ledger",
          "ODF-061 allocation did not store selected filespace class");
  Require(allocated.evidence.reason.find("mga_visibility_authority=durable_transaction_inventory") !=
              std::string::npos,
          "ODF-061 allocation evidence lost MGA authority marker");
  Require(allocated.evidence.reason.find("finality_authority=false") != std::string::npos,
          "ODF-061 allocation evidence lost non-finality marker");
  RequireNoRuntimeDocTokens(AllocationEvidence(allocated.evidence));

  page::PageReservationLedger reservations;
  page::InsertPageReservationRequest reservation;
  reservation.database_uuid = ids.database_uuid;
  reservation.transaction_uuid = ids.transaction_uuid;
  reservation.local_transaction_id = 63;
  reservation.object_uuid = ids.object_uuid;
  reservation.preferred_filespace_uuid = ids.filespace_uuid;
  reservation.policy_uuid = ids.policy_uuid;
  reservation.page_family = "blob";
  reservation.object_class = filespace::FilespaceObjectClass::large_blob;
  reservation.estimated_payload_bytes = 32768;
  reservation.current_time_authority_tick = 1000;
  const auto reserved = page::ReserveInsertPagesDurable(&reservations, reservation);
  Require(reserved.ok(), "ODF-061 explicit filespace class reservation refused");
  Require(reserved.reservation.filespace_class == "large_blob",
          "ODF-061 reservation did not store selected filespace class");
  Require(reserved.evidence.reason.find("visibility_authority=false") != std::string::npos,
          "ODF-061 reservation evidence lost non-visibility marker");
  RequireNoRuntimeDocTokens(ReservationEvidence(reserved.evidence));
}

void ExistingPageAllocationStillWorks() {
  const Ids ids;
  page::PageAllocationLedger ledger;
  ledger.database_uuid = ids.database_uuid;
  ledger.filespace_uuid = ids.filespace_uuid;
  ledger.free_extents.push_back({300, 4});

  page::PageAllocationRequest request;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.owner_object_uuid = ids.object_uuid;
  request.creator_transaction_uuid = ids.transaction_uuid;
  request.creator_local_transaction_id = 64;
  request.page_family = "data";
  request.page_count = 1;
  request.engine_authoritative = true;
  const auto allocated = page::ReservePageAllocation(&ledger, request);
  Require(allocated.ok(), "ODF-061 default page allocation was broken");
  Require(allocated.allocation.start_page == 300,
          "ODF-061 default page allocation start changed");
  Require(allocated.allocation.filespace_class == "hot_row",
          "ODF-061 default data allocation did not derive hot_row class");
  RequireNoRuntimeDocTokens(AllocationEvidence(allocated.evidence));
}

void PreallocatedPoolsRemainClassSeparated() {
  const Ids ids;
  page::PageAllocationLedger ledger;
  ledger.database_uuid = ids.database_uuid;
  ledger.filespace_uuid = ids.filespace_uuid;
  ledger.free_extents.push_back({400, 8});

  page::PagePreallocationRequest preallocate;
  preallocate.database_uuid = ids.database_uuid;
  preallocate.filespace_uuid = ids.filespace_uuid;
  preallocate.policy_uuid = ids.policy_uuid;
  preallocate.creator_transaction_uuid = ids.transaction_uuid;
  preallocate.creator_local_transaction_id = 65;
  preallocate.page_family = "index";
  preallocate.object_class = filespace::FilespaceObjectClass::exact_index;
  preallocate.page_count = 4;
  preallocate.engine_authoritative = true;
  preallocate.durability_fence_satisfied = true;
  preallocate.explicit_filespace_class = true;
  const auto exact_pool = page::PreallocatePageFamilyPool(&ledger, preallocate);
  Require(exact_pool.ok() &&
              exact_pool.allocation.filespace_class == "exact_index" &&
              exact_pool.allocation.start_page == 400,
          "ODF-061 exact-index preallocation setup failed");

  page::PageAllocationRequest delta_request;
  delta_request.database_uuid = ids.database_uuid;
  delta_request.filespace_uuid = ids.filespace_uuid;
  delta_request.owner_object_uuid = ids.object_uuid;
  delta_request.creator_transaction_uuid = ids.transaction_uuid;
  delta_request.creator_local_transaction_id = 66;
  delta_request.page_family = "index";
  delta_request.object_class = filespace::FilespaceObjectClass::secondary_delta_ledger;
  delta_request.page_count = 2;
  delta_request.engine_authoritative = true;
  delta_request.explicit_filespace_class = true;
  const auto delta_allocation = page::ReservePageAllocation(&ledger, delta_request);
  Require(delta_allocation.ok() &&
              delta_allocation.allocation.filespace_class == "secondary_delta_ledger",
          "ODF-061 secondary delta allocation refused");
  Require(delta_allocation.allocation.start_page == 404,
          "ODF-061 allocation reused a preallocated pool from another filespace class");

  page::PageAllocationRequest exact_request = delta_request;
  exact_request.creator_local_transaction_id = 67;
  exact_request.object_class = filespace::FilespaceObjectClass::exact_index;
  const auto exact_allocation = page::ReservePageAllocation(&ledger, exact_request);
  Require(exact_allocation.ok() &&
              exact_allocation.allocation.filespace_class == "exact_index" &&
              exact_allocation.allocation.start_page == 400,
          "ODF-061 matching filespace class did not use preallocated pool");
}

void BackupStreamAllocationUsesArchiveFamily() {
  const Ids ids;
  page::PageAllocationLedger ledger;
  ledger.database_uuid = ids.database_uuid;
  ledger.filespace_uuid = ids.filespace_uuid;
  ledger.free_extents.push_back({500, 4});

  page::PageAllocationRequest request;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.owner_object_uuid = ids.object_uuid;
  request.creator_transaction_uuid = ids.transaction_uuid;
  request.creator_local_transaction_id = 68;
  request.page_family = "archive";
  request.object_class = filespace::FilespaceObjectClass::backup_stream;
  request.page_count = 1;
  request.engine_authoritative = true;
  request.explicit_filespace_class = true;
  const auto allocated = page::ReservePageAllocation(&ledger, request);
  Require(allocated.ok() &&
              allocated.allocation.filespace_class == "backup_stream" &&
              allocated.allocation.page_family == "archive",
          "ODF-061 backup stream archive allocation refused");
  RequireNoRuntimeDocTokens(AllocationEvidence(allocated.evidence));
}

void ExistingPageFamiliesRemainClassified() {
  const Ids ids;
  const std::vector<std::string> families = {
      "row_data",
      "catalog",
      "allocation_map",
      "transaction_inventory",
      "metrics",
      "columnar",
      "vector",
      "graph",
      "archive",
  };
  for (const auto& family : families) {
    filespace::FilespaceClassRequest request;
    request.database_uuid = ids.database_uuid;
    request.filespace_uuid = ids.filespace_uuid;
    request.owner_object_uuid = platform::TypedUuid{};
    request.object_class = filespace::FilespaceObjectClass::unspecified;
    request.page_family = family;
    request.reason = "odf061_existing_page_family_compatibility";
    request.explicit_object_class = false;
    const auto decision = filespace::ResolveFilespaceClass(request);
    Require(decision.ok(),
            "ODF-061 default filespace class refused an existing page family");
    Require(decision.filespace_class != filespace::FilespaceClass::unknown &&
                decision.filespace_class != filespace::FilespaceClass::forbidden,
            "ODF-061 existing page family resolved to non-runtime class");
    RequireNoRuntimeDocTokens(decision.evidence);
  }
}

}  // namespace

int main() {
  AllRequiredClassesRouteSeparately();
  InvalidAndMissingIdentityFailsClosed();
  AllocationAndReservationCarryClassEvidence();
  ExistingPageAllocationStillWorks();
  PreallocatedPoolsRemainClassSeparated();
  BackupStreamAllocationUsesArchiveFamily();
  ExistingPageFamiliesRemainClassified();
  return EXIT_SUCCESS;
}
