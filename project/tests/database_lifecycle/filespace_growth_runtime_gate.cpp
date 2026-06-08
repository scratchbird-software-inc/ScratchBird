// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_growth.hpp"
#include "filespace_header.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
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
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1910000000000ull + seed);
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

struct PhysicalMemberFixture {
  std::filesystem::path root;
  std::filesystem::path path;

  PhysicalMemberFixture(std::string_view name, platform::u64 seed) {
    root = std::filesystem::temp_directory_path() /
           ("scratchbird_filespace_growth_runtime_gate_" + std::string(name) +
            "_" + std::to_string(1910000000000ull + seed));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    path = root / "member.sbfs";
  }

  ~PhysicalMemberFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
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

filespace::FilespaceRegistry Registry(const FixtureIds& ids,
                                      const std::filesystem::path& path) {
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = ids.database_uuid;
  descriptor.filespace_uuid = ids.filespace_uuid;
  descriptor.path = path.string();
  descriptor.role = filespace::FilespaceRole::secondary_data;
  descriptor.state = filespace::FilespaceState::online;
  descriptor.page_size = 16384;
  descriptor.generation = 19;
  descriptor.active = true;
  descriptor.read_only = false;

  filespace::FilespaceRegistry registry;
  registry.filespaces.push_back(descriptor);
  return registry;
}

filespace::FilespacePhysicalGrowthRequest Request(const FixtureIds& ids,
                                                  const std::filesystem::path& path) {
  filespace::FilespacePhysicalGrowthRequest request;
  request.request_uuid = ids.request_uuid;
  request.database_uuid = ids.database_uuid;
  request.filespace_uuid = ids.filespace_uuid;
  request.policy_uuid = ids.policy_uuid;
  request.storage_profile_uuid = ids.storage_profile_uuid;
  request.requested_growth_pages = 12;
  request.page_size_bytes = 16384;
  request.policy_generation = 9;
  request.observed_policy_generation = 9;
  request.catalog_generation = 11;
  request.observed_catalog_generation = 11;
  request.caller_mode = filespace::FilespacePhysicalGrowthCallerMode::filespace_capacity_manager;
  request.authorization.obs_agent_control_right = true;
  request.authorization.filespace_lifecycle_right = true;
  request.authorization.action_approval = true;
  request.evidence_store_present = true;
  request.evidence_before_success = true;
  request.policy_expand_allowed = true;
  request.engine_owned_authority = true;
  request.reserve_growth_as_preallocated = true;
  request.reason = "filespace-growth-runtime-gate";
  request.member_capacity.present = true;
  request.member_capacity.explicit_capacity_context = true;
  request.member_capacity.file_member_uuid = ids.file_member_uuid;
  request.member_capacity.start_page_number = 1000;
  request.member_capacity.current_page_count = 64;
  request.member_capacity.preallocated_page_count = 4;
  request.member_capacity.maximum_page_count = 128;
  request.member_capacity.physical_path = path.string();
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

void PreparePhysicalMember(const filespace::FilespacePhysicalGrowthRequest& request) {
  filespace::PhysicalFilespaceHeader header;
  header.database_uuid = request.database_uuid;
  header.filespace_uuid = request.filespace_uuid;
  header.role = filespace::FilespaceRole::secondary_data;
  header.state = filespace::FilespaceState::online;
  header.page_size = request.page_size_bytes;
  header.physical_filespace_id = 1;
  header.total_pages =
      request.member_capacity.current_page_count +
      request.member_capacity.preallocated_page_count;
  header.free_pages = 0;
  header.preallocated_pages = request.member_capacity.preallocated_page_count;
  header.allocation_root_page = 1;
  header.header_generation = 1;
  header.writer_identity_uuid = request.storage_profile_uuid;
  header.creation_operation_uuid = "filespace-growth-runtime-gate";
  const auto written =
      filespace::WritePhysicalFilespaceHeader(request.member_capacity.physical_path, header, false);
  if (!written.ok()) {
    Fail("physical member prepare failed: " + written.diagnostic.diagnostic_code);
  }
}

struct LedgerSnapshot {
  std::size_t operations = 0;
  std::size_t evidence = 0;
  std::size_t extents = 0;
  std::size_t windows = 0;
  platform::u64 next_sequence = 0;
};

LedgerSnapshot Capture(const filespace::FilespaceGrowthLedger& ledger) {
  return {ledger.physical_growth_operations.size(),
          ledger.physical_growth_evidence.size(),
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

void ExpectRefused(const filespace::FilespacePhysicalGrowthResult& result,
                   const filespace::FilespaceGrowthLedger& ledger,
                   const LedgerSnapshot& before,
                   std::string_view diagnostic_code,
                   std::string_view label) {
  Require(!result.ok(), std::string(label) + " unexpectedly succeeded");
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          std::string(label) + " diagnostic mismatch: " + result.diagnostic.diagnostic_code);
  Require(!result.durable_state_changed, std::string(label) + " reported durable mutation");
  Require(!result.cache_invalidation_required, std::string(label) + " reported cache invalidation");
  Require(!result.grown, std::string(label) + " reported growth");
  RequireUnchanged(ledger, before, label);
}

void TestFilespaceCapacityManagerSuccessMetadataEvidenceMetricsAndRecovery() {
  const auto ids = MakeIds(1);
  PhysicalMemberFixture member("capacity_manager", 1);
  const auto registry = Registry(ids, member.path);
  filespace::FilespaceGrowthLedger ledger;
  const auto request = Request(ids, member.path);
  PreparePhysicalMember(request);

  const auto result = filespace::ExecuteFilespacePhysicalGrowth(&ledger, registry, request);
  Require(result.ok(), "filespace growth failed: " + result.diagnostic.diagnostic_code);
  Require(result.durable_state_changed, "success did not report durable state change");
  Require(result.cache_invalidation_required, "success did not request cache invalidation");
  Require(result.metrics_emitted, "success did not emit metrics");
  Require(!result.allocated_logical_pages, "growth allocated logical pages");
  Require(!result.page_allocation_authority_bypassed, "growth bypassed page allocation authority");
  Require(result.operation.metrics_emitted, "operation metrics flag was not set");
  Require(result.operation.state == filespace::FilespacePhysicalGrowthState::completed,
          "operation state was not completed");
  Require(result.operation.caller_mode ==
              filespace::FilespacePhysicalGrowthCallerMode::filespace_capacity_manager,
          "caller mode was not recorded");
  Require(result.operation.grown_page_count == request.requested_growth_pages,
          "operation grown page count mismatch");
  Require(result.operation.bytes_grown == request.requested_growth_pages * request.page_size_bytes,
          "operation byte count mismatch");
  Require(result.operation.growth_start_page_number == 1068, "growth start page mismatch");
  Require(result.operation.member_physical_page_count_before == 68,
          "member physical page count before mismatch");
  Require(result.operation.member_physical_page_count_after == 80,
          "member physical page count after mismatch");
  Require(result.operation.member_preallocated_pages_after == 16,
          "member preallocated pages after mismatch");
  Require(result.operation.physical_extension_completed,
          "physical extension completion flag missing");
  Require(result.operation.physical_extension_synced,
          "physical extension sync flag missing");
  Require(result.operation.physical_header_updated,
          "physical header update flag missing");
  Require(result.operation.metadata_commit_after_physical_extension,
          "metadata commit was not ordered after physical extension");
  Require(result.operation.physical_file_size_before_bytes == 68 * request.page_size_bytes,
          "physical file size before mismatch");
  Require(result.operation.physical_file_size_after_bytes == 80 * request.page_size_bytes,
          "physical file size after mismatch");
  Require(result.operation.physical_file_expected_size_after_bytes ==
              result.operation.physical_file_size_after_bytes,
          "physical expected size mismatch");

  Require(ledger.physical_growth_evidence.size() == 1, "success evidence count mismatch");
  Require(ledger.physical_growth_operations.size() == 1, "success operation count mismatch");
  Require(ledger.preallocated_extents.size() == 1, "success reserved-free extent count mismatch");
  Require(ledger.member_capacity_windows.size() == 1, "success window count mismatch");
  Require(ledger.physical_growth_evidence.front().evidence_before_success,
          "evidence-before-success flag missing");
  Require(ledger.physical_growth_evidence.front().durable_state_changed,
          "evidence durable state flag missing");
  Require(ledger.physical_growth_evidence.front().new_state ==
              filespace::FilespacePhysicalGrowthState::completed,
          "evidence completed state missing");
  Require(ledger.physical_growth_evidence.front().metadata_commit_after_physical_extension,
          "evidence did not record physical-before-metadata ordering");
  Require(ledger.preallocated_extents.front().page_count == request.requested_growth_pages,
          "reserved-free extent page count mismatch");
  Require(ledger.preallocated_extents.front().start_page_number == 1068,
          "reserved-free extent start mismatch");
  Require(ledger.member_capacity_windows.front().logical_page_count == 64,
          "window logical page count mismatch");
  Require(ledger.member_capacity_windows.front().physical_page_count == 80,
          "window physical page count mismatch");
  Require(ledger.member_capacity_windows.front().preallocated_page_count == 16,
          "window preallocated page count mismatch");

  const auto found = filespace::FindFilespacePhysicalGrowthOperation(
      ledger, result.operation.growth_operation_id);
  Require(found != nullptr, "completed growth operation was not findable");

  const auto recovery = filespace::ClassifyFilespacePhysicalGrowthLedgerForRecovery(ledger);
  Require(recovery.ok(), "growth recovery classification failed");
  Require(recovery.classifications.size() == 1, "growth recovery count mismatch");
  Require(recovery.classifications.front().action ==
              filespace::FilespacePhysicalGrowthRecoveryAction::retain_physical_growth,
          "growth recovery did not retain physical metadata");
  Require(!recovery.classifications.front().fail_closed,
          "completed growth recovery failed closed");
}

void TestDirectSysArchSuccessDoesNotReserveFreeExtent() {
  const auto ids = MakeIds(2);
  PhysicalMemberFixture member("direct_sysarch", 2);
  const auto registry = Registry(ids, member.path);
  filespace::FilespaceGrowthLedger ledger;
  auto request = Request(ids, member.path);
  request.caller_mode = filespace::FilespacePhysicalGrowthCallerMode::direct_sysarch;
  request.authorization.obs_agent_control_right = false;
  request.authorization.filespace_lifecycle_right = false;
  request.authorization.storage_filespace_control_right = true;
  request.reserve_growth_as_preallocated = false;
  PreparePhysicalMember(request);

  const auto result = filespace::ExecuteFilespacePhysicalGrowth(&ledger, registry, request);
  Require(result.ok(), "direct SysArch growth failed: " + result.diagnostic.diagnostic_code);
  Require(result.operation.caller_mode ==
              filespace::FilespacePhysicalGrowthCallerMode::direct_sysarch,
          "direct SysArch caller mode was not recorded");
  Require(result.operation.member_physical_page_count_before == 68,
          "direct SysArch physical page count before mismatch");
  Require(result.operation.member_physical_page_count_after == 80,
          "direct SysArch physical page count mismatch");
  Require(result.operation.member_preallocated_pages_after == 4,
          "direct SysArch unexpectedly changed preallocated pages");
  Require(ledger.preallocated_extents.empty(), "direct SysArch created reserved-free extent");
  Require(!result.allocated_logical_pages, "direct SysArch growth allocated logical pages");
  Require(!result.page_allocation_authority_bypassed,
          "direct SysArch growth bypassed page allocation authority");
}

void TestPreallocatedPagesCanExceedLogicalPages() {
  const auto ids = MakeIds(22);
  PhysicalMemberFixture member("preallocated_heavy", 22);
  const auto registry = Registry(ids, member.path);
  filespace::FilespaceGrowthLedger ledger;
  auto request = Request(ids, member.path);
  request.requested_growth_pages = 8;
  request.member_capacity.current_page_count = 0;
  request.member_capacity.preallocated_page_count = 32;
  request.member_capacity.maximum_page_count = 64;
  PreparePhysicalMember(request);

  const auto result = filespace::ExecuteFilespacePhysicalGrowth(&ledger, registry, request);
  Require(result.ok(), "growth rejected preallocated pages beyond logical pages: " +
                           result.diagnostic.diagnostic_code);
  Require(result.operation.growth_start_page_number == 1032,
          "growth start did not include existing preallocated pages");
  Require(result.operation.member_physical_page_count_before == 32,
          "physical page count before did not include preallocated pages");
  Require(result.operation.member_physical_page_count_after == 40,
          "physical page count after mismatch for preallocated-heavy member");
  Require(result.member_capacity_window.logical_page_count == 0,
          "logical page count drifted during physical growth");
  Require(result.member_capacity_window.preallocated_page_count == 40,
          "reserved-free count did not include existing and newly grown pages");
}

void TestIdempotentDuplicateDoesNotGrowTwice() {
  const auto ids = MakeIds(3);
  PhysicalMemberFixture member("idempotent", 3);
  const auto registry = Registry(ids, member.path);
  filespace::FilespaceGrowthLedger ledger;
  const auto request = Request(ids, member.path);
  PreparePhysicalMember(request);
  const auto first = filespace::ExecuteFilespacePhysicalGrowth(&ledger, registry, request);
  Require(first.ok(), "first growth failed");
  const auto before = Capture(ledger);

  const auto duplicate = filespace::ExecuteFilespacePhysicalGrowth(&ledger, registry, request);
  Require(duplicate.ok(), "duplicate growth replay failed");
  Require(duplicate.duplicate_request, "duplicate request flag missing");
  Require(!duplicate.durable_state_changed, "duplicate reported durable mutation");
  Require(duplicate.operation.growth_operation_id.value == first.operation.growth_operation_id.value,
          "duplicate returned different operation id");
  Require(duplicate.member_capacity_window.physical_page_count ==
              first.member_capacity_window.physical_page_count,
          "duplicate did not return existing member capacity window");
  Require(ledger.physical_growth_operations.size() == before.operations,
          "duplicate appended another operation");
  Require(ledger.preallocated_extents.size() == before.extents,
          "duplicate appended another extent");
  Require(ledger.member_capacity_windows.size() == before.windows,
          "duplicate appended another window");
  Require(ledger.physical_growth_evidence.size() == before.evidence + 1,
          "duplicate did not write replay evidence");
}

void RunRefusalCase(
    std::string_view label,
    std::string_view diagnostic_code,
    const std::function<void(filespace::FilespacePhysicalGrowthRequest&,
                             filespace::FilespaceRegistry&)>& mutate) {
  const auto ids = MakeIds(100 + label.size());
  PhysicalMemberFixture member(label, 100 + label.size());
  auto registry = Registry(ids, member.path);
  auto request = Request(ids, member.path);
  filespace::FilespaceGrowthLedger ledger;
  PreparePhysicalMember(request);
  mutate(request, registry);
  const auto before = Capture(ledger);
  const auto result = filespace::ExecuteFilespacePhysicalGrowth(&ledger, registry, request);
  ExpectRefused(result, ledger, before, diagnostic_code, label);
}

void TestRefusalsDoNotPartiallyMutate() {
  RunRefusalCase("missing evidence store",
                 "filespace_growth_missing_evidence_store",
                 [](auto& request, auto&) { request.evidence_store_present = false; });
  RunRefusalCase("no evidence before success",
                 "filespace_growth_evidence_before_success_required",
                 [](auto& request, auto&) { request.evidence_before_success = false; });
  RunRefusalCase("non-engine authority",
                 "filespace_growth_non_engine_authority",
                 [](auto& request, auto&) { request.engine_owned_authority = false; });
  RunRefusalCase("invalid caller mode",
                 "filespace_growth_invalid_caller_mode",
                 [](auto& request, auto&) {
                   request.caller_mode = filespace::FilespacePhysicalGrowthCallerMode::unknown;
                 });
  RunRefusalCase("policy denied",
                 "filespace_growth_policy_denied",
                 [](auto& request, auto&) { request.policy_expand_allowed = false; });
  RunRefusalCase("missing approval",
                 "filespace_growth_permission_denied",
                 [](auto& request, auto&) { request.authorization.action_approval = false; });
  RunRefusalCase("missing filespace manager right",
                 "filespace_growth_permission_denied",
                 [](auto& request, auto&) { request.authorization.obs_agent_control_right = false; });
  RunRefusalCase("missing lifecycle right",
                 "filespace_growth_permission_denied",
                 [](auto& request, auto&) { request.authorization.filespace_lifecycle_right = false; });
  RunRefusalCase("missing sysarch right",
                 "filespace_growth_permission_denied",
                 [](auto& request, auto&) {
                   request.caller_mode = filespace::FilespacePhysicalGrowthCallerMode::direct_sysarch;
                   request.authorization.obs_agent_control_right = false;
                   request.authorization.filespace_lifecycle_right = false;
                   request.authorization.storage_filespace_control_right = false;
                 });
  RunRefusalCase("invalid request uuid",
                 "filespace_growth_invalid_request_uuid",
                 [](auto& request, auto&) { request.request_uuid = platform::TypedUuid{}; });
  RunRefusalCase("invalid database uuid",
                 "filespace_growth_invalid_database_uuid",
                 [](auto& request, auto&) { request.database_uuid = platform::TypedUuid{}; });
  RunRefusalCase("invalid filespace uuid",
                 "filespace_growth_invalid_filespace_uuid",
                 [](auto& request, auto&) { request.filespace_uuid = platform::TypedUuid{}; });
  RunRefusalCase("invalid policy uuid",
                 "filespace_growth_invalid_policy_uuid",
                 [](auto& request, auto&) { request.policy_uuid = platform::TypedUuid{}; });
  RunRefusalCase("invalid storage profile uuid",
                 "filespace_growth_invalid_storage_profile_uuid",
                 [](auto& request, auto&) { request.storage_profile_uuid = platform::TypedUuid{}; });
  RunRefusalCase("zero pages",
                 "filespace_growth_zero_pages",
                 [](auto& request, auto&) { request.requested_growth_pages = 0; });
  RunRefusalCase("invalid page size",
                 "filespace_growth_invalid_page_size",
                 [](auto& request, auto&) { request.page_size_bytes = 12345; });
  RunRefusalCase("stale policy generation",
                 "filespace_growth_stale_policy_generation",
                 [](auto& request, auto&) {
                   request.observed_policy_generation = request.policy_generation + 1;
                 });
  RunRefusalCase("stale catalog generation",
                 "filespace_growth_stale_catalog_generation",
                 [](auto& request, auto&) {
                   request.observed_catalog_generation = request.catalog_generation + 1;
                 });
  RunRefusalCase("invalid transaction context",
                 "filespace_growth_invalid_transaction_context",
                 [](auto& request, auto&) {
                   request.transaction_context.durability_fence_satisfied = false;
                 });
  RunRefusalCase("no filespace",
                 "filespace_growth_no_filespace",
                 [](auto& request, auto& registry) {
                   request.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 90000);
                   registry.filespaces.clear();
                 });
  RunRefusalCase("page size mismatch",
                 "filespace_growth_page_size_mismatch",
                 [](auto& request, auto& registry) {
                   registry.filespaces.front().page_size = request.page_size_bytes * 2;
                 });
  RunRefusalCase("missing physical member context",
                 "filespace_growth_missing_physical_member_context",
                 [](auto& request, auto&) { request.member_capacity.present = false; });
  RunRefusalCase("invalid member uuid",
                 "filespace_growth_missing_physical_member_context",
                 [](auto& request, auto&) { request.member_capacity.file_member_uuid = platform::TypedUuid{}; });
  RunRefusalCase("member unavailable",
                 "filespace_growth_member_unavailable",
                 [](auto& request, auto&) { request.member_capacity.writable = false; });
  RunRefusalCase("missing physical member path",
                 "filespace_growth_missing_physical_member_path",
                 [](auto& request, auto&) { request.member_capacity.physical_path.clear(); });
  RunRefusalCase("insufficient capacity",
                 "filespace_growth_insufficient_capacity",
                 [](auto& request, auto&) { request.member_capacity.maximum_page_count = 70; });
  RunRefusalCase("byte overflow",
                 "filespace_growth_capacity_overflow",
                 [](auto& request, auto&) {
                   request.requested_growth_pages =
                       std::numeric_limits<platform::u64>::max() / request.page_size_bytes + 1;
                   request.member_capacity.current_page_count = 0;
                   request.member_capacity.preallocated_page_count = 0;
                   request.member_capacity.maximum_page_count = std::numeric_limits<platform::u64>::max();
                 });
  RunRefusalCase("read only",
                 "filespace_growth_read_only",
                 [](auto&, auto& registry) { registry.filespaces.front().read_only = true; });
  RunRefusalCase("quarantine",
                 "filespace_growth_quarantine",
                 [](auto&, auto& registry) {
                   registry.filespaces.front().state = filespace::FilespaceState::quarantine;
                 });
  RunRefusalCase("forbidden",
                 "filespace_growth_forbidden",
                 [](auto&, auto& registry) {
                   registry.filespaces.front().role = filespace::FilespaceRole::forbidden;
                 });
  RunRefusalCase("drop pending",
                 "filespace_growth_drop_pending",
                 [](auto&, auto& registry) {
                   registry.filespaces.front().state = filespace::FilespaceState::drop_pending;
                 });
  RunRefusalCase("unavailable filespace",
                 "filespace_growth_filespace_unavailable",
                 [](auto&, auto& registry) { registry.filespaces.front().active = false; });
}

void TestMissingLedgerFailsClosed() {
  const auto ids = MakeIds(400);
  PhysicalMemberFixture member("missing_ledger", 400);
  const auto registry = Registry(ids, member.path);
  const auto request = Request(ids, member.path);
  PreparePhysicalMember(request);
  const auto result = filespace::ExecuteFilespacePhysicalGrowth(nullptr, registry, request);
  Require(!result.ok(), "missing ledger unexpectedly succeeded");
  Require(result.diagnostic.diagnostic_code == "filespace_growth_missing_ledger",
          "missing ledger diagnostic mismatch");
  Require(!result.durable_state_changed, "missing ledger reported durable mutation");
}

}  // namespace

int main() {
  TestFilespaceCapacityManagerSuccessMetadataEvidenceMetricsAndRecovery();
  TestDirectSysArchSuccessDoesNotReserveFreeExtent();
  TestPreallocatedPagesCanExceedLogicalPages();
  TestIdempotentDuplicateDoesNotGrowTwice();
  TestRefusalsDoNotPartiallyMutate();
  TestMissingLedgerFailsClosed();
  return EXIT_SUCCESS;
}
