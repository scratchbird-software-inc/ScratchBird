// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_growth.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace filespace = scratchbird::storage::filespace;
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
  platform::TypedUuid request_uuid;
  platform::TypedUuid database_uuid;
  platform::TypedUuid filespace_uuid;
  platform::TypedUuid policy_uuid;
  platform::TypedUuid storage_profile_uuid;
  platform::TypedUuid file_member_uuid;
  platform::TypedUuid transaction_uuid;
};

FixtureIds MakeIds(platform::u64 seed) {
  return {MakeUuid(platform::UuidKind::object, 100 + seed),
          MakeUuid(platform::UuidKind::database, 200 + seed),
          MakeUuid(platform::UuidKind::filespace, 300 + seed),
          MakeUuid(platform::UuidKind::object, 400 + seed),
          MakeUuid(platform::UuidKind::object, 500 + seed),
          MakeUuid(platform::UuidKind::object, 600 + seed),
          MakeUuid(platform::UuidKind::transaction, 700 + seed)};
}

filespace::FilespaceRegistry Registry(const FixtureIds& ids) {
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = ids.database_uuid;
  descriptor.filespace_uuid = ids.filespace_uuid;
  descriptor.role = filespace::FilespaceRole::secondary_data;
  descriptor.state = filespace::FilespaceState::online;
  descriptor.page_size = 16384;
  descriptor.generation = 17;
  descriptor.active = true;
  descriptor.read_only = false;

  filespace::FilespaceRegistry registry;
  registry.filespaces.push_back(descriptor);
  return registry;
}

filespace::FilespacePreallocationRequest Request(const FixtureIds& ids) {
  filespace::FilespacePreallocationRequest request;
  request.request_uuid = ids.request_uuid;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.policy_uuid = ids.policy_uuid;
  request.storage_profile_uuid = ids.storage_profile_uuid;
  request.requested_page_count = 12;
  request.page_size_bytes = 16384;
  request.policy_generation = 9;
  request.observed_policy_generation = 9;
  request.catalog_generation = 11;
  request.observed_catalog_generation = 11;
  request.evidence_store_present = true;
  request.evidence_before_success = true;
  request.reason = "filespace-preallocation-runtime-gate";
  request.member_capacity.present = true;
  request.member_capacity.explicit_capacity_context = true;
  request.member_capacity.file_member_uuid = ids.file_member_uuid;
  request.member_capacity.start_page_number = 1000;
  request.member_capacity.current_page_count = 64;
  request.member_capacity.preallocated_page_count = 4;
  request.member_capacity.maximum_page_count = 128;
  request.member_capacity.online = true;
  request.member_capacity.writable = true;
  request.transaction_context.present = true;
  request.transaction_context.transaction_uuid = ids.transaction_uuid;
  request.transaction_context.transaction_number = 77;
  request.transaction_context.durable_inventory_admitted = true;
  request.transaction_context.write_intent = true;
  request.transaction_context.durability_fence_satisfied = true;
  return request;
}

struct LedgerSnapshot {
  std::size_t operations = 0;
  std::size_t evidence = 0;
  std::size_t extents = 0;
  std::size_t windows = 0;
  platform::u64 next_sequence = 0;
};

LedgerSnapshot Capture(const filespace::FilespaceGrowthLedger& ledger) {
  return {ledger.preallocation_operations.size(),
          ledger.preallocation_evidence.size(),
          ledger.preallocated_extents.size(),
          ledger.member_capacity_windows.size(),
          ledger.next_evidence_sequence};
}

void RequireUnchanged(const filespace::FilespaceGrowthLedger& ledger,
                      const LedgerSnapshot& before,
                      std::string_view label) {
  const auto after = Capture(ledger);
  Require(after.operations == before.operations, std::string(label) + " changed operations");
  Require(after.evidence == before.evidence, std::string(label) + " changed evidence");
  Require(after.extents == before.extents, std::string(label) + " changed extents");
  Require(after.windows == before.windows, std::string(label) + " changed member windows");
  Require(after.next_sequence == before.next_sequence, std::string(label) + " changed evidence sequence");
}

void ExpectRefused(const filespace::FilespacePreallocationResult& result,
                   const filespace::FilespaceGrowthLedger& ledger,
                   const LedgerSnapshot& before,
                   std::string_view diagnostic_code,
                   std::string_view label) {
  Require(!result.ok(), std::string(label) + " unexpectedly succeeded");
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          std::string(label) + " diagnostic mismatch: " + result.diagnostic.diagnostic_code);
  Require(!result.durable_state_changed, std::string(label) + " reported durable mutation");
  Require(!result.cache_invalidation_required, std::string(label) + " reported cache invalidation");
  Require(!result.preallocated, std::string(label) + " reported preallocation");
  RequireUnchanged(ledger, before, label);
}

void TestSuccessMetadataEvidenceMetricsAndRecovery() {
  const auto ids = MakeIds(1);
  const auto registry = Registry(ids);
  filespace::FilespaceGrowthLedger ledger;
  const auto request = Request(ids);

  const auto result = filespace::PreallocateFilespace(&ledger, registry, request);
  Require(result.ok(), "preallocation failed: " + result.diagnostic.diagnostic_code);
  Require(result.durable_state_changed, "success did not report durable state change");
  Require(result.cache_invalidation_required, "success did not request cache invalidation");
  Require(result.metrics_emitted, "success did not emit metrics");
  Require(result.operation.metrics_emitted, "operation metrics flag was not set");
  Require(result.operation.state == filespace::FilespacePreallocationState::completed,
          "operation state was not completed");
  Require(result.operation.preallocated_page_count == request.requested_page_count,
          "operation preallocated page count mismatch");
  Require(result.operation.bytes_preallocated == request.requested_page_count * request.page_size_bytes,
          "operation byte count mismatch");
  Require(result.operation.start_page_number == 1068, "operation start page mismatch");
  Require(result.operation.member_page_count_after == 80, "member page count after mismatch");
  Require(result.operation.member_preallocated_pages_after == 16,
          "member preallocated pages after mismatch");

  Require(ledger.preallocation_evidence.size() == 1, "success evidence count mismatch");
  Require(ledger.preallocation_operations.size() == 1, "success operation count mismatch");
  Require(ledger.preallocated_extents.size() == 1, "success extent count mismatch");
  Require(ledger.member_capacity_windows.size() == 1, "success window count mismatch");
  Require(ledger.preallocation_evidence.front().evidence_before_success,
          "evidence-before-success flag missing");
  Require(ledger.preallocation_evidence.front().durable_state_changed,
          "evidence durable state flag missing");
  Require(ledger.preallocation_evidence.front().new_state ==
              filespace::FilespacePreallocationState::completed,
          "evidence completed state missing");
  Require(ledger.preallocated_extents.front().page_count == request.requested_page_count,
          "extent page count mismatch");
  Require(ledger.preallocated_extents.front().bytes_preallocated == result.operation.bytes_preallocated,
          "extent bytes mismatch");
  Require(ledger.member_capacity_windows.front().physical_page_count == 80,
          "window physical page count mismatch");
  Require(ledger.member_capacity_windows.front().preallocated_page_count == 16,
          "window preallocated page count mismatch");

  const auto found = filespace::FindFilespacePreallocationOperation(
      ledger, result.operation.preallocation_operation_id);
  Require(found != nullptr, "completed preallocation operation was not findable");

  const auto recovery = filespace::ClassifyFilespacePreallocationLedgerForRecovery(ledger);
  Require(recovery.ok(), "preallocation recovery classification failed");
  Require(recovery.classifications.size() == 1, "preallocation recovery count mismatch");
  Require(recovery.classifications.front().action ==
              filespace::FilespacePreallocationRecoveryAction::retain_reserved_free,
          "preallocation recovery did not retain reserved-free metadata");
  Require(!recovery.classifications.front().fail_closed,
          "completed preallocation recovery failed closed");
}

void TestIdempotentDuplicateDoesNotReserveTwice() {
  const auto ids = MakeIds(2);
  const auto registry = Registry(ids);
  filespace::FilespaceGrowthLedger ledger;
  const auto request = Request(ids);
  const auto first = filespace::PreallocateFilespace(&ledger, registry, request);
  Require(first.ok(), "first preallocation failed");
  const auto before = Capture(ledger);

  const auto duplicate = filespace::PreallocateFilespace(&ledger, registry, request);
  Require(duplicate.ok(), "duplicate preallocation replay failed");
  Require(duplicate.duplicate_request, "duplicate request flag missing");
  Require(!duplicate.durable_state_changed, "duplicate reported durable mutation");
  Require(duplicate.operation.preallocation_operation_id.value ==
              first.operation.preallocation_operation_id.value,
          "duplicate returned different operation id");
  Require(duplicate.extent.page_count == first.extent.page_count &&
              duplicate.extent.start_page_number == first.extent.start_page_number,
          "duplicate did not return existing extent metadata");
  Require(duplicate.member_capacity_window.physical_page_count ==
              first.member_capacity_window.physical_page_count,
          "duplicate did not return existing member capacity window");
  Require(ledger.preallocation_operations.size() == before.operations,
          "duplicate appended another operation");
  Require(ledger.preallocated_extents.size() == before.extents,
          "duplicate appended another extent");
  Require(ledger.member_capacity_windows.size() == before.windows,
          "duplicate appended another window");
  Require(ledger.preallocation_evidence.size() == before.evidence + 1,
          "duplicate did not write replay evidence");
}

void RunRefusalCase(
    std::string_view label,
    std::string_view diagnostic_code,
    const std::function<void(filespace::FilespacePreallocationRequest&,
                             filespace::FilespaceRegistry&)>& mutate) {
  const auto ids = MakeIds(100 + label.size());
  auto registry = Registry(ids);
  auto request = Request(ids);
  filespace::FilespaceGrowthLedger ledger;
  mutate(request, registry);
  const auto before = Capture(ledger);
  const auto result = filespace::PreallocateFilespace(&ledger, registry, request);
  ExpectRefused(result, ledger, before, diagnostic_code, label);
}

void TestRefusalsDoNotPartiallyMutate() {
  RunRefusalCase("missing evidence store",
                 "filespace_preallocate_missing_evidence_store",
                 [](auto& request, auto&) {
                   request.evidence_store_present = false;
                 });
  RunRefusalCase("no evidence before success",
                 "filespace_preallocate_evidence_before_success_required",
                 [](auto& request, auto&) {
                   request.evidence_before_success = false;
                 });
  RunRefusalCase("invalid database uuid",
                 "filespace_preallocate_invalid_database_uuid",
                 [](auto& request, auto&) {
                   request.database_uuid = platform::TypedUuid{};
                 });
  RunRefusalCase("invalid filespace uuid",
                 "filespace_preallocate_invalid_filespace_uuid",
                 [](auto& request, auto&) {
                   request.filespace_uuid = platform::TypedUuid{};
                 });
  RunRefusalCase("invalid policy uuid",
                 "filespace_preallocate_invalid_policy_uuid",
                 [](auto& request, auto&) {
                   request.policy_uuid = platform::TypedUuid{};
                 });
  RunRefusalCase("invalid storage profile uuid",
                 "filespace_preallocate_invalid_storage_profile_uuid",
                 [](auto& request, auto&) {
                   request.storage_profile_uuid = platform::TypedUuid{};
                 });
  RunRefusalCase("zero pages",
                 "filespace_preallocate_zero_pages",
                 [](auto& request, auto&) {
                   request.requested_page_count = 0;
                 });
  RunRefusalCase("invalid page size",
                 "filespace_preallocate_invalid_page_size",
                 [](auto& request, auto&) {
                   request.page_size_bytes = 12345;
                 });
  RunRefusalCase("stale policy generation",
                 "filespace_preallocate_stale_policy_generation",
                 [](auto& request, auto&) {
                   request.observed_policy_generation = request.policy_generation + 1;
                 });
  RunRefusalCase("stale catalog generation",
                 "filespace_preallocate_stale_catalog_generation",
                 [](auto& request, auto&) {
                   request.observed_catalog_generation = request.catalog_generation + 1;
                 });
  RunRefusalCase("no filespace",
                 "filespace_preallocate_no_filespace",
                 [](auto& request, auto& registry) {
                   request.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 90000);
                   registry.filespaces.clear();
                 });
  RunRefusalCase("page size mismatch",
                 "filespace_preallocate_page_size_mismatch",
                 [](auto& request, auto& registry) {
                   registry.filespaces.front().page_size = request.page_size_bytes * 2;
                 });
  RunRefusalCase("invalid transaction context",
                 "filespace_preallocate_invalid_transaction_context",
                 [](auto& request, auto&) {
                   request.transaction_context.durability_fence_satisfied = false;
                 });
  RunRefusalCase("missing physical member context",
                 "filespace_preallocate_missing_physical_member_context",
                 [](auto& request, auto&) {
                   request.member_capacity.present = false;
                 });
  RunRefusalCase("invalid member uuid",
                 "filespace_preallocate_missing_physical_member_context",
                 [](auto& request, auto&) {
                   request.member_capacity.file_member_uuid = platform::TypedUuid{};
                 });
  RunRefusalCase("member unavailable",
                 "filespace_preallocate_member_unavailable",
                 [](auto& request, auto&) {
                   request.member_capacity.writable = false;
                 });
  RunRefusalCase("insufficient capacity",
                 "filespace_preallocate_insufficient_capacity",
                 [](auto& request, auto&) {
                   request.member_capacity.maximum_page_count = 70;
                 });
  RunRefusalCase("read only",
                 "filespace_preallocate_read_only",
                 [](auto&, auto& registry) {
                   registry.filespaces.front().read_only = true;
                 });
  RunRefusalCase("quarantine",
                 "filespace_preallocate_quarantine",
                 [](auto&, auto& registry) {
                   registry.filespaces.front().state = filespace::FilespaceState::quarantine;
                 });
  RunRefusalCase("forbidden",
                 "filespace_preallocate_forbidden",
                 [](auto&, auto& registry) {
                   registry.filespaces.front().role = filespace::FilespaceRole::forbidden;
                 });
  RunRefusalCase("drop pending",
                 "filespace_preallocate_drop_pending",
                 [](auto&, auto& registry) {
                   registry.filespaces.front().state = filespace::FilespaceState::drop_pending;
                 });
  RunRefusalCase("unavailable filespace",
                 "filespace_preallocate_filespace_unavailable",
                 [](auto&, auto& registry) {
                   registry.filespaces.front().active = false;
                 });
}

void TestMissingLedgerFailsClosed() {
  const auto ids = MakeIds(400);
  const auto registry = Registry(ids);
  const auto request = Request(ids);
  const auto result = filespace::PreallocateFilespace(nullptr, registry, request);
  Require(!result.ok(), "missing ledger unexpectedly succeeded");
  Require(result.diagnostic.diagnostic_code == "filespace_preallocate_missing_ledger",
          "missing ledger diagnostic mismatch");
  Require(!result.durable_state_changed, "missing ledger reported durable mutation");
}

}  // namespace

int main() {
  TestSuccessMetadataEvidenceMetricsAndRecovery();
  TestIdempotentDuplicateDoesNotReserveTwice();
  TestRefusalsDoNotPartiallyMutate();
  TestMissingLedgerFailsClosed();
  return EXIT_SUCCESS;
}
