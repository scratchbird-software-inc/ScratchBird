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
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace filespace = scratchbird::storage::filespace;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1960000000000ull + seed);
  if (!generated.ok()) {
    std::cerr << "uuid generation failed\n";
    std::exit(EXIT_FAILURE);
  }
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
  return {MakeUuid(platform::UuidKind::object, seed + 1),
          MakeUuid(platform::UuidKind::database, seed + 2),
          MakeUuid(platform::UuidKind::filespace, seed + 3),
          MakeUuid(platform::UuidKind::object, seed + 4),
          MakeUuid(platform::UuidKind::object, seed + 5),
          MakeUuid(platform::UuidKind::object, seed + 6),
          MakeUuid(platform::UuidKind::transaction, seed + 7)};
}

std::filesystem::path TempRoot() {
  std::string scope = std::filesystem::current_path().filename().string();
  if (scope.empty()) {
    scope = "default";
  }
#ifdef _WIN32
  const auto pid = static_cast<unsigned long long>(::GetCurrentProcessId());
#else
  const auto pid = static_cast<unsigned long long>(::getpid());
#endif
  auto root = std::filesystem::temp_directory_path() /
              ("scratchbird_engine_listener_filespace_growth_" + scope +
               "_" + std::to_string(pid));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  return root;
}

void RemoveRoot(const std::filesystem::path& root) {
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

filespace::FilespaceRegistry Registry(const FixtureIds& ids,
                                      const std::filesystem::path& path,
                                      platform::u32 page_size) {
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = ids.database_uuid;
  descriptor.filespace_uuid = ids.filespace_uuid;
  descriptor.path = path.string();
  descriptor.role = filespace::FilespaceRole::secondary_data;
  descriptor.state = filespace::FilespaceState::online;
  descriptor.page_size = page_size;
  descriptor.generation = 1;
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
  request.requested_growth_pages = 3;
  request.page_size_bytes = 8192;
  request.policy_generation = 7;
  request.observed_policy_generation = 7;
  request.catalog_generation = 9;
  request.observed_catalog_generation = 9;
  request.caller_mode = filespace::FilespacePhysicalGrowthCallerMode::filespace_capacity_manager;
  request.authorization.obs_agent_control_right = true;
  request.authorization.filespace_lifecycle_right = true;
  request.authorization.action_approval = true;
  request.evidence_store_present = true;
  request.evidence_before_success = true;
  request.policy_expand_allowed = true;
  request.engine_owned_authority = true;
  request.reserve_growth_as_preallocated = true;
  request.reason = "engine-listener-filespace-growth-conformance";
  request.member_capacity.present = true;
  request.member_capacity.explicit_capacity_context = true;
  request.member_capacity.file_member_uuid = ids.file_member_uuid;
  request.member_capacity.start_page_number = 100;
  request.member_capacity.current_page_count = 4;
  request.member_capacity.preallocated_page_count = 1;
  request.member_capacity.maximum_page_count = 16;
  request.member_capacity.physical_path = path.string();
  request.member_capacity.online = true;
  request.member_capacity.writable = true;
  request.transaction_context.present = true;
  request.transaction_context.transaction_uuid = ids.transaction_uuid;
  request.transaction_context.transaction_number = 11;
  request.transaction_context.durable_inventory_admitted = true;
  request.transaction_context.write_intent = true;
  request.transaction_context.durability_fence_satisfied = true;
  return request;
}

void WriteHeaderForRequest(const filespace::FilespacePhysicalGrowthRequest& request,
                           platform::u64 total_pages_override = 0) {
  filespace::PhysicalFilespaceHeader header;
  header.database_uuid = request.database_uuid;
  header.filespace_uuid = request.filespace_uuid;
  header.role = filespace::FilespaceRole::secondary_data;
  header.state = filespace::FilespaceState::online;
  header.page_size = request.page_size_bytes;
  header.physical_filespace_id = 1;
  header.total_pages =
      total_pages_override == 0
          ? request.member_capacity.current_page_count +
                request.member_capacity.preallocated_page_count
          : total_pages_override;
  header.free_pages = 0;
  header.preallocated_pages = request.member_capacity.preallocated_page_count;
  header.allocation_root_page = 1;
  header.header_generation = 1;
  header.writer_identity_uuid = request.storage_profile_uuid;
  header.creation_operation_uuid = "engine-listener-filespace-growth-conformance";
  const auto written =
      filespace::WritePhysicalFilespaceHeader(request.member_capacity.physical_path, header, false);
  if (!written.ok()) {
    std::cerr << written.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool LedgerEmpty(const filespace::FilespaceGrowthLedger& ledger) {
  return ledger.physical_growth_operations.empty() &&
         ledger.physical_growth_evidence.empty() &&
         ledger.preallocated_extents.empty() &&
         ledger.member_capacity_windows.empty();
}

bool PhysicalGrowthExtendsHeaderBeforeLedgerCommit() {
  const auto root = TempRoot() / "success";
  std::filesystem::create_directories(root);
  const auto path = root / "member.sbfs";
  const auto ids = MakeIds(100);
  const auto request = Request(ids, path);
  const auto registry = Registry(ids, path, request.page_size_bytes);
  WriteHeaderForRequest(request);

  filespace::FilespaceGrowthLedger ledger;
  const auto result = filespace::ExecuteFilespacePhysicalGrowth(&ledger, registry, request);
  if (!Require(result.ok(), "physical growth failed: " + result.diagnostic.diagnostic_code) ||
      !Require(ledger.physical_growth_operations.size() == 1, "ledger operation missing") ||
      !Require(ledger.physical_growth_evidence.size() == 1, "ledger evidence missing") ||
      !Require(result.operation.physical_extension_required, "physical extension not required") ||
      !Require(result.operation.physical_extension_completed, "physical extension not completed") ||
      !Require(result.operation.physical_extension_synced, "physical extension not synced") ||
      !Require(result.operation.physical_header_updated, "physical header not updated") ||
      !Require(result.operation.metadata_commit_after_physical_extension,
               "metadata commit ordering flag missing") ||
      !Require(result.operation.physical_file_size_before_bytes == 5 * request.page_size_bytes,
               "file size before growth mismatch") ||
      !Require(result.operation.physical_file_size_after_bytes == 8 * request.page_size_bytes,
               "file size after growth mismatch") ||
      !Require(result.operation.physical_file_expected_size_after_bytes ==
                   result.operation.physical_file_size_after_bytes,
               "expected file size after growth mismatch") ||
      !Require(result.evidence.metadata_commit_after_physical_extension,
               "evidence ordering flag missing")) {
    RemoveRoot(root);
    return false;
  }

  const auto read = filespace::ReadPhysicalFilespaceHeader(path.string());
  const bool ok =
      Require(read.ok(), "grown header did not read back: " + read.diagnostic.diagnostic_code) &&
      Require(read.header.total_pages == 8, "grown header total pages mismatch") &&
      Require(read.header.preallocated_pages == 4, "grown header preallocated pages mismatch") &&
      Require(read.header.header_generation == 2, "grown header generation did not advance") &&
      Require(read.file_size_bytes == 8 * request.page_size_bytes,
              "grown file size mismatch");
  RemoveRoot(root);
  return ok;
}

bool MetadataOnlyGrowthIsRejected() {
  const auto root = TempRoot() / "metadata_only";
  std::filesystem::create_directories(root);
  const auto path = root / "member.sbfs";
  const auto ids = MakeIds(200);
  auto request = Request(ids, path);
  const auto registry = Registry(ids, path, request.page_size_bytes);
  WriteHeaderForRequest(request);
  request.member_capacity.physical_path.clear();

  filespace::FilespaceGrowthLedger ledger;
  const auto result = filespace::ExecuteFilespacePhysicalGrowth(&ledger, registry, request);
  const bool ok =
      Require(!result.ok(), "metadata-only growth unexpectedly succeeded") &&
      Require(result.diagnostic.diagnostic_code ==
                  "filespace_growth_missing_physical_member_path",
              "metadata-only diagnostic mismatch: " + result.diagnostic.diagnostic_code) &&
      Require(LedgerEmpty(ledger), "metadata-only refusal mutated the ledger");
  RemoveRoot(root);
  return ok;
}

bool HeaderMismatchRejectsBeforeLedgerMutation() {
  const auto root = TempRoot() / "header_mismatch";
  std::filesystem::create_directories(root);
  const auto path = root / "member.sbfs";
  const auto ids = MakeIds(300);
  const auto request = Request(ids, path);
  const auto registry = Registry(ids, path, request.page_size_bytes);
  WriteHeaderForRequest(request, 6);

  filespace::FilespaceGrowthLedger ledger;
  const auto result = filespace::ExecuteFilespacePhysicalGrowth(&ledger, registry, request);
  const bool ok =
      Require(!result.ok(), "header mismatch growth unexpectedly succeeded") &&
      Require(result.diagnostic.diagnostic_code ==
                  "SB-FILESPACE-HEADER-GROWTH-BASE-MISMATCH",
              "header mismatch diagnostic mismatch: " + result.diagnostic.diagnostic_code) &&
      Require(LedgerEmpty(ledger), "header mismatch refusal mutated the ledger");
  RemoveRoot(root);
  return ok;
}

bool CrashWindowAndLegacyMetadataOnlyRowsFailClosed() {
  const auto root = TempRoot() / "crash_window";
  std::filesystem::create_directories(root);
  const auto path = root / "member.sbfs";
  const auto ids = MakeIds(400);
  const auto request = Request(ids, path);
  WriteHeaderForRequest(request);
  std::filesystem::resize_file(path, 8 * request.page_size_bytes);

  const auto read = filespace::ReadPhysicalFilespaceHeader(path.string());
  if (!Require(!read.ok(), "interrupted growth header/file mismatch was accepted") ||
      !Require(read.diagnostic.diagnostic_code ==
                   "SB-FILESPACE-HEADER-FILE-SIZE-CAPACITY-MISMATCH",
               "interrupted growth diagnostic mismatch")) {
    RemoveRoot(root);
    return false;
  }

  filespace::FilespaceGrowthLedger ledger;
  filespace::FilespacePhysicalGrowthEntry legacy;
  legacy.growth_operation_id = MakeUuid(platform::UuidKind::object, 401);
  legacy.state = filespace::FilespacePhysicalGrowthState::completed;
  legacy.member_physical_page_count_before = 5;
  legacy.member_physical_page_count_after = 8;
  legacy.physical_file_size_after_bytes = 8 * request.page_size_bytes;
  legacy.physical_file_expected_size_after_bytes = 8 * request.page_size_bytes;
  ledger.physical_growth_operations.push_back(legacy);
  const auto recovery = filespace::ClassifyFilespacePhysicalGrowthLedgerForRecovery(ledger);
  const bool ok =
      Require(recovery.ok(), "legacy recovery classification failed") &&
      Require(recovery.classifications.size() == 1, "legacy recovery count mismatch") &&
      Require(recovery.classifications.front().fail_closed,
              "legacy metadata-only growth did not fail closed") &&
      Require(recovery.classifications.front().action ==
                  filespace::FilespacePhysicalGrowthRecoveryAction::fail_closed,
              "legacy metadata-only recovery action mismatch");
  RemoveRoot(root);
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = PhysicalGrowthExtendsHeaderBeforeLedgerCommit() && ok;
  ok = MetadataOnlyGrowthIsRejected() && ok;
  ok = HeaderMismatchRejectsBeforeLedgerMutation() && ok;
  ok = CrashWindowAndLegacyMetadataOnlyRowsFailClosed() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
