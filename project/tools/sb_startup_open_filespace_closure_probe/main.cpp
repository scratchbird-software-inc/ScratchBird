// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-STARTUP-OPEN-FILESPACE-CLOSURE-PROBE-ANCHOR
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "filespace_header.hpp"
#include "filespace_lifecycle.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::storage::database::CreateDatabaseFile;
using scratchbird::storage::database::DatabaseCreateConfig;
using scratchbird::storage::database::DatabaseOpenConfig;
using scratchbird::storage::database::kCatalogPageNumber;
using scratchbird::storage::database::MarkDatabaseCleanShutdown;
using scratchbird::storage::database::OpenDatabaseFile;
using scratchbird::storage::database::ReadStartupStatePageBody;
using scratchbird::storage::database::StartupRecoveryClassification;
using scratchbird::storage::database::WriteStartupStatePageBody;
using scratchbird::storage::disk::FileDevice;
using scratchbird::storage::disk::FileOpenMode;
using scratchbird::storage::filespace::CreatePhysicalFilespaceFile;
using scratchbird::storage::filespace::FilespaceOperation;
using scratchbird::storage::filespace::FilespaceOperationRequest;
using scratchbird::storage::filespace::FilespaceRegistry;
using scratchbird::storage::filespace::FilespaceRole;
using scratchbird::storage::filespace::FilespaceState;
using scratchbird::storage::filespace::PhysicalFilespaceHeader;
using scratchbird::storage::filespace::ReadPhysicalFilespaceHeader;
using scratchbird::storage::filespace::ValidatePhysicalFilespaceHeader;
using scratchbird::storage::filespace::ApplyFilespaceOperation;

TypedUuid Generate(UuidKind kind, u64 millis) {
  const auto generated = GenerateEngineIdentityV7(kind, millis);
  return generated.ok() ? generated.value : TypedUuid{};
}

void PrintCheck(const char* name, bool passed) {
  std::cout << name << "=" << (passed ? "true" : "false") << "\n";
}

FilespaceOperationRequest Request(FilespaceOperation op,
                                  TypedUuid database_uuid,
                                  TypedUuid filespace_uuid,
                                  std::string path,
                                  FilespaceRole role) {
  FilespaceOperationRequest request;
  request.operation = op;
  request.database_uuid = database_uuid;
  request.filespace_uuid = filespace_uuid;
  request.path = std::move(path);
  request.role = role;
  request.page_size = 16384;
  request.reason = "sof_probe";
  request.policy.allow_primary_replacement = true;
  return request;
}

bool CreateProbeDatabase(const std::string& path,
                         TypedUuid database_uuid,
                         TypedUuid filespace_uuid,
                         u64 creation_millis) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path + ".sb.owner.lock", ignored);

  DatabaseCreateConfig create;
  create.path = path;
  create.database_uuid = database_uuid;
  create.filespace_uuid = filespace_uuid;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = creation_millis;
  const char* seed_root = std::getenv("SB_RESOURCE_SEED_PACK_ROOT");
  create.resource_seed_pack_root = seed_root == nullptr
      ? "project/resources/seed-packs/initial-resource-pack"
      : seed_root;
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  return CreateDatabaseFile(create).ok();
}

bool TamperByte(const std::string& path, u64 offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file.is_open()) {
    return false;
  }
  file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  char zero = '\0';
  file.write(&zero, 1);
  file.flush();
  return static_cast<bool>(file);
}

bool RewriteStartupFlags(const std::string& path,
                         bool clean_shutdown,
                         bool startup_dirty,
                         bool write_admission_fenced) {
  FileDevice device;
  const auto opened = device.Open(path, FileOpenMode::open_existing);
  if (!opened.ok()) {
    return false;
  }
  auto state = ReadStartupStatePageBody(&device, 16384);
  if (!state.ok()) {
    (void)device.Close();
    return false;
  }
  state.state.clean_shutdown = clean_shutdown;
  state.state.startup_dirty = startup_dirty;
  state.state.write_admission_fenced = write_admission_fenced;
  const bool written = WriteStartupStatePageBody(&device, state.state).ok();
  (void)device.Close();
  return written;
}

}  // namespace

int main() {
  const std::string db_path = "/tmp/sb_sof_probe.sbdb";
  const std::string corrupt_path = "/tmp/sb_sof_probe_corrupt.sbdb";
  const std::string mismatch_path = "/tmp/sb_sof_probe_mismatch.sbdb";
  const std::string fenced_path = "/tmp/sb_sof_probe_fenced.sbdb";
  const std::string checkpoint_path = "/tmp/sb_sof_probe_checkpoint.sbdb";
  const std::string secondary_path = "/tmp/sb_sof_probe_secondary.sbf";
  const std::string tampered_secondary_path = "/tmp/sb_sof_probe_secondary_tampered.sbf";
  std::error_code ignored;
  std::filesystem::remove(db_path, ignored);
  std::filesystem::remove(db_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(corrupt_path, ignored);
  std::filesystem::remove(corrupt_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(mismatch_path, ignored);
  std::filesystem::remove(mismatch_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(fenced_path, ignored);
  std::filesystem::remove(fenced_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(checkpoint_path, ignored);
  std::filesystem::remove(checkpoint_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(secondary_path, ignored);
  std::filesystem::remove(secondary_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(tampered_secondary_path, ignored);
  std::filesystem::remove(tampered_secondary_path + ".sb.owner.lock", ignored);

  const u64 base = 1772000000000ull;
  const TypedUuid database_uuid = Generate(UuidKind::database, base);
  const TypedUuid first_filespace_uuid = Generate(UuidKind::filespace, base + 1);
  const TypedUuid secondary_filespace_uuid = Generate(UuidKind::filespace, base + 2);
  const TypedUuid mismatch_filespace_uuid = Generate(UuidKind::filespace, base + 3);

  const bool created = CreateProbeDatabase(db_path, database_uuid, first_filespace_uuid, base);

  DatabaseOpenConfig open_rw;
  open_rw.path = db_path;
  const auto opened_rw = OpenDatabaseFile(open_rw);
  const bool fixed_map_and_first_filespace_ok = created && opened_rw.ok() &&
      opened_rw.state.filespace_uuid.value == first_filespace_uuid.value &&
      opened_rw.state.startup_state_present &&
      !opened_rw.state.write_admission_fenced &&
      opened_rw.state.startup_state.startup_dirty &&
      opened_rw.state.startup_recovery_classification == "clean_checkpoint_path";

  DatabaseOpenConfig dirty_ro;
  dirty_ro.path = db_path;
  dirty_ro.read_only = true;
  const auto opened_dirty_ro = OpenDatabaseFile(dirty_ro);
  const bool dirty_open_classified_ok = opened_dirty_ro.ok() &&
      opened_dirty_ro.state.startup_state.startup_dirty &&
      opened_dirty_ro.state.startup_recovery_classification == "repaired_recovery";

  FileDevice held_owner;
  const auto held_owner_open = held_owner.Open(db_path, FileOpenMode::open_existing);
  const auto double_open = OpenDatabaseFile(open_rw);
  const bool double_read_write_open_rejected = held_owner_open.ok() &&
      !double_open.ok() &&
      double_open.diagnostic.diagnostic_code == "SB-STORAGE-DISK-OWNER-LOCK-HELD";
  if (held_owner_open.ok()) {
    (void)held_owner.Close();
  }

  const auto clean = MarkDatabaseCleanShutdown(db_path);
  DatabaseOpenConfig open_ro;
  open_ro.path = db_path;
  open_ro.read_only = true;
  const auto opened_ro = OpenDatabaseFile(open_ro);
  const bool clean_read_only_open_ok = clean.ok() && opened_ro.ok() &&
      opened_ro.state.startup_state.clean_shutdown &&
      !opened_ro.state.startup_state.startup_dirty &&
      opened_ro.state.filespace_uuid.value == first_filespace_uuid.value;

  bool invalid_clean_marker_rejected = false;
  {
    FileDevice invalid_device;
    const auto invalid_open = invalid_device.Open(db_path, FileOpenMode::open_existing);
    if (invalid_open.ok()) {
      auto invalid_state = ReadStartupStatePageBody(&invalid_device, 16384);
      if (invalid_state.ok()) {
        invalid_state.state.clean_shutdown = true;
        invalid_state.state.startup_dirty = true;
        const auto invalid_write = WriteStartupStatePageBody(&invalid_device, invalid_state.state);
        (void)invalid_device.Close();
        const auto invalid_reopen = OpenDatabaseFile(open_rw);
        invalid_clean_marker_rejected = invalid_write.ok() &&
            !invalid_reopen.ok() &&
            invalid_reopen.diagnostic.diagnostic_code == "SB-DB-LIFECYCLE-RESTRICTED-OPEN-REQUIRED";
      } else {
        (void)invalid_device.Close();
      }
    }
  }

  const bool corrupt_create = CreateProbeDatabase(corrupt_path,
                                                 Generate(UuidKind::database, base + 10),
                                                 Generate(UuidKind::filespace, base + 11),
                                                 base + 10);
  const bool corrupt_tamper = TamperByte(corrupt_path, 16384ull * kCatalogPageNumber);
  DatabaseOpenConfig corrupt_open_config;
  corrupt_open_config.path = corrupt_path;
  const auto corrupt_open = OpenDatabaseFile(corrupt_open_config);
  const bool corrupt_fixed_page_rejected = corrupt_create && corrupt_tamper && !corrupt_open.ok();

  const bool mismatch_create = CreateProbeDatabase(mismatch_path,
                                                  Generate(UuidKind::database, base + 20),
                                                  Generate(UuidKind::filespace, base + 21),
                                                  base + 20);
  bool mismatch_written = false;
  if (mismatch_create) {
    FileDevice mismatch_device;
    const auto mismatch_open = mismatch_device.Open(mismatch_path, FileOpenMode::open_existing);
    if (mismatch_open.ok()) {
      auto mismatch_state = ReadStartupStatePageBody(&mismatch_device, 16384);
      if (mismatch_state.ok()) {
        mismatch_state.state.first_filespace_uuid = mismatch_filespace_uuid;
        mismatch_written = WriteStartupStatePageBody(&mismatch_device, mismatch_state.state).ok();
      }
      (void)mismatch_device.Close();
    }
  }
  DatabaseOpenConfig mismatch_open_config;
  mismatch_open_config.path = mismatch_path;
  const auto mismatch_open = OpenDatabaseFile(mismatch_open_config);
  const bool first_filespace_uuid_mismatch_rejected = mismatch_create &&
      mismatch_written &&
      !mismatch_open.ok() &&
      mismatch_open.diagnostic.diagnostic_code == "SB-DB-LIFECYCLE-STARTUP-PAGE-FILESPACE-UUID-MISMATCH";

  const bool fenced_create = CreateProbeDatabase(fenced_path,
                                                Generate(UuidKind::database, base + 30),
                                                Generate(UuidKind::filespace, base + 31),
                                                base + 30);
  const bool fenced_written = fenced_create && RewriteStartupFlags(fenced_path, false, false, true);
  DatabaseOpenConfig fenced_ro_config;
  fenced_ro_config.path = fenced_path;
  fenced_ro_config.read_only = true;
  const auto fenced_ro_open = OpenDatabaseFile(fenced_ro_config);
  DatabaseOpenConfig fenced_rw_config;
  fenced_rw_config.path = fenced_path;
  const auto fenced_rw_open = OpenDatabaseFile(fenced_rw_config);
  const bool fenced_open_classified_and_write_refused = fenced_written &&
      fenced_ro_open.ok() &&
      fenced_ro_open.state.startup_recovery_classification == "fence_writes_until_safe" &&
      fenced_ro_open.state.write_admission_fenced &&
      !fenced_rw_open.ok() &&
      fenced_rw_open.diagnostic.diagnostic_code == "SB-DB-LIFECYCLE-RESTRICTED-OPEN-REQUIRED";

  const bool checkpoint_create = CreateProbeDatabase(checkpoint_path,
                                                    Generate(UuidKind::database, base + 40),
                                                    Generate(UuidKind::filespace, base + 41),
                                                    base + 40);
  const bool checkpoint_written = checkpoint_create && RewriteStartupFlags(checkpoint_path, false, false, false);
  DatabaseOpenConfig checkpoint_ro_config;
  checkpoint_ro_config.path = checkpoint_path;
  checkpoint_ro_config.read_only = true;
  const auto checkpoint_ro_open = OpenDatabaseFile(checkpoint_ro_config);
  DatabaseOpenConfig checkpoint_rw_config;
  checkpoint_rw_config.path = checkpoint_path;
  const auto checkpoint_rw_open = OpenDatabaseFile(checkpoint_rw_config);
  const bool checkpoint_open_classified_and_write_refused = checkpoint_written &&
      checkpoint_ro_open.ok() &&
      checkpoint_ro_open.state.startup_recovery_classification == "checkpoint_rebuild_required" &&
      !checkpoint_rw_open.ok() &&
      checkpoint_rw_open.diagnostic.diagnostic_code == "SB-DB-LIFECYCLE-RESTRICTED-OPEN-REQUIRED";

  PhysicalFilespaceHeader header;
  header.database_uuid = database_uuid;
  header.filespace_uuid = secondary_filespace_uuid;
  header.role = FilespaceRole::primary_candidate;
  header.state = FilespaceState::online;
  header.page_size = 16384;
  header.creation_operation_uuid = "sof_probe_create_secondary";
  const auto physical_create = CreatePhysicalFilespaceFile(secondary_path, header, false);
  const auto physical_read = ReadPhysicalFilespaceHeader(secondary_path);
  const auto physical_validate = physical_read.ok()
      ? ValidatePhysicalFilespaceHeader(header, physical_read.header)
      : decltype(ValidatePhysicalFilespaceHeader(header, header)){};
  const bool physical_filespace_header_ok = physical_create.ok() && physical_read.ok() && physical_validate.ok();
  PhysicalFilespaceHeader mismatched_state = header;
  mismatched_state.state = FilespaceState::read_only;
  const auto physical_state_mismatch = physical_read.ok()
      ? ValidatePhysicalFilespaceHeader(mismatched_state, physical_read.header)
      : decltype(ValidatePhysicalFilespaceHeader(header, header)){};
  const bool physical_filespace_header_mismatch_rejected =
      !physical_state_mismatch.ok() &&
      physical_state_mismatch.diagnostic.diagnostic_code == "SB-FILESPACE-HEADER-STATE-MISMATCH";

  PhysicalFilespaceHeader tampered_header = header;
  tampered_header.filespace_uuid = Generate(UuidKind::filespace, base + 4);
  const auto tampered_create = CreatePhysicalFilespaceFile(tampered_secondary_path, tampered_header, false);
  const bool tampered_write = TamperByte(tampered_secondary_path, 53);
  const auto tampered_read = ReadPhysicalFilespaceHeader(tampered_secondary_path);
  const bool physical_filespace_header_checksum_rejected =
      tampered_create.ok() &&
      tampered_write &&
      !tampered_read.ok() &&
      tampered_read.diagnostic.diagnostic_code == "SB-FILESPACE-HEADER-CHECKSUM-MISMATCH";

  FilespaceRegistry registry;
  const auto primary = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::create_filespace, database_uuid, first_filespace_uuid, db_path, FilespaceRole::active_primary));
  const auto candidate = ApplyFilespaceOperation(&registry,
      Request(FilespaceOperation::attach_filespace, database_uuid, secondary_filespace_uuid, secondary_path, FilespaceRole::primary_candidate));
  auto promote_request = Request(FilespaceOperation::promote_filespace,
                                 database_uuid,
                                 secondary_filespace_uuid,
                                 secondary_path,
                                 FilespaceRole::primary_candidate);
  promote_request.policy.allow_primary_replacement = true;
  promote_request.policy.require_physical_header_for_promote = true;
  const auto promoted = ApplyFilespaceOperation(&registry, promote_request);
  PhysicalFilespaceHeader promoted_expected = header;
  promoted_expected.role = FilespaceRole::active_primary;
  promoted_expected.state = FilespaceState::online;
  const auto promoted_physical_read = ReadPhysicalFilespaceHeader(secondary_path);
  const auto promoted_physical_validate = promoted_physical_read.ok()
      ? ValidatePhysicalFilespaceHeader(promoted_expected, promoted_physical_read.header)
      : decltype(ValidatePhysicalFilespaceHeader(header, header)){};
  const bool promotion_header_updated = promoted_physical_read.ok() && promoted_physical_validate.ok();
  const bool lifecycle_state_matrix_ok = primary.ok() && candidate.ok() && promoted.ok() &&
      promoted.descriptor.role == FilespaceRole::active_primary &&
      promoted.descriptor.state == FilespaceState::online &&
      promotion_header_updated;

  PrintCheck("fixed_map_and_first_filespace_ok", fixed_map_and_first_filespace_ok);
  PrintCheck("corrupt_fixed_page_rejected", corrupt_fixed_page_rejected);
  PrintCheck("first_filespace_uuid_mismatch_rejected", first_filespace_uuid_mismatch_rejected);
  PrintCheck("clean_read_only_open_ok", clean_read_only_open_ok);
  PrintCheck("dirty_open_classified_ok", dirty_open_classified_ok);
  PrintCheck("fenced_open_classified_and_write_refused", fenced_open_classified_and_write_refused);
  PrintCheck("checkpoint_open_classified_and_write_refused", checkpoint_open_classified_and_write_refused);
  PrintCheck("double_read_write_open_rejected", double_read_write_open_rejected);
  PrintCheck("invalid_clean_marker_rejected", invalid_clean_marker_rejected);
  PrintCheck("physical_filespace_header_ok", physical_filespace_header_ok);
  PrintCheck("physical_filespace_header_mismatch_rejected", physical_filespace_header_mismatch_rejected);
  PrintCheck("physical_filespace_header_checksum_rejected", physical_filespace_header_checksum_rejected);
  PrintCheck("promotion_header_updated", promotion_header_updated);
  PrintCheck("lifecycle_state_matrix_ok", lifecycle_state_matrix_ok);

  std::filesystem::remove(db_path, ignored);
  std::filesystem::remove(db_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(corrupt_path, ignored);
  std::filesystem::remove(corrupt_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(mismatch_path, ignored);
  std::filesystem::remove(mismatch_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(fenced_path, ignored);
  std::filesystem::remove(fenced_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(checkpoint_path, ignored);
  std::filesystem::remove(checkpoint_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(secondary_path, ignored);
  std::filesystem::remove(secondary_path + ".sb.owner.lock", ignored);
  std::filesystem::remove(tampered_secondary_path, ignored);
  std::filesystem::remove(tampered_secondary_path + ".sb.owner.lock", ignored);

  return fixed_map_and_first_filespace_ok && corrupt_fixed_page_rejected &&
                 first_filespace_uuid_mismatch_rejected && clean_read_only_open_ok &&
                 dirty_open_classified_ok && double_read_write_open_rejected &&
                 fenced_open_classified_and_write_refused &&
                 checkpoint_open_classified_and_write_refused &&
                 invalid_clean_marker_rejected && physical_filespace_header_ok &&
                 physical_filespace_header_mismatch_rejected &&
                 physical_filespace_header_checksum_rejected &&
                 promotion_header_updated && lifecycle_state_matrix_ok
             ? 0
             : 1;
}
