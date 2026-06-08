// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "page_header.hpp"
#include "page_manager.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

struct DecodedRecord {
  catalog::CatalogTypedRecord record;
  std::map<std::string, std::string> fields;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc_004a_filespace_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

std::string UuidString(const TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

std::map<std::string, std::string> ParsePayloadFields(const std::string& payload) {
  std::map<std::string, std::string> fields;
  std::stringstream lines(payload);
  std::string line;
  while (std::getline(lines, line)) {
    std::stringstream tokens(line);
    std::string token;
    while (tokens >> token) {
      const std::size_t split = token.find('=');
      if (split == std::string::npos) {
        continue;
      }
      fields[token.substr(0, split)] = token.substr(split + 1);
    }
  }
  return fields;
}

std::vector<page::CatalogPageRow> ReadCatalogRows(const std::filesystem::path& database_path,
                                                  std::uint32_t page_size) {
  disk::FileDevice device;
  const auto opened = device.Open(database_path.string(), disk::FileOpenMode::open_existing_read_only);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "could not open created database read-only for catalog parse");

  disk::DiskDevicePolicy policy;
  policy.page_size = page_size;
  policy.access_mode = disk::DiskAccessMode::read_only;
  policy.checksum_policy = disk::DiskChecksumPolicy::require_valid;
  policy.unknown_page_policy = disk::UnknownPagePolicy::reject_all;

  std::vector<page::CatalogPageRow> rows;
  std::uint64_t page_number = db::kCatalogPageNumber;
  std::set<std::uint64_t> visited;
  while (page_number != 0) {
    Require(visited.insert(page_number).second, "catalog page chain contains a cycle");
    const auto header = disk::ReadDevicePageHeader(&device, page_size, page_number, policy);
    if (!header.ok()) {
      std::cerr << header.diagnostic.diagnostic_code << '\n';
    }
    Require(header.ok(), "catalog page header did not validate");
    Require(header.classification.page_type == disk::PageType::catalog,
            "catalog chain page was not classified as a catalog page");

    std::vector<scratchbird::core::platform::byte> body(
        page_size - disk::kPageHeaderSerializedBytes, 0);
    const auto read = device.ReadAt(page::PageOffset(page_size, page_number) +
                                        disk::kPageHeaderSerializedBytes,
                                    body.data(),
                                    body.size());
    if (!read.ok()) {
      std::cerr << read.diagnostic.diagnostic_code << '\n';
    }
    Require(read.ok(), "catalog page body read failed");

    const auto parsed = page::ParseCatalogPageBody(body, page_number);
    if (!parsed.ok()) {
      std::cerr << parsed.diagnostic.diagnostic_code << '\n';
    }
    Require(parsed.ok(), "catalog page body parse failed");
    rows.insert(rows.end(), parsed.body.rows.begin(), parsed.body.rows.end());
    page_number = parsed.body.next_page_number;
  }
  Require(!rows.empty(), "catalog page chain contained no rows");
  return rows;
}

std::vector<DecodedRecord> DecodeTypedRecords(const std::vector<page::CatalogPageRow>& rows) {
  std::vector<DecodedRecord> records;
  for (const auto& row : rows) {
    if (row.kind != page::CatalogPageRowKind::typed_catalog_record) {
      continue;
    }
    const auto decoded = catalog::DecodeCatalogTypedRecord(row);
    if (!decoded.ok()) {
      std::cerr << decoded.diagnostic.diagnostic_code << '\n';
    }
    Require(decoded.ok(), "typed catalog record decode failed");
    records.push_back({decoded.record, ParsePayloadFields(decoded.record.payload)});
  }
  Require(!records.empty(), "no typed catalog records were decoded");
  return records;
}

std::vector<DecodedRecord> FilespaceRecords(const std::vector<DecodedRecord>& records) {
  std::vector<DecodedRecord> filespaces;
  for (const auto& record : records) {
    if (record.record.header.kind == catalog::CatalogRecordKind::filespace) {
      filespaces.push_back(record);
    }
  }
  return filespaces;
}

void RequireField(const DecodedRecord& record, std::string_view key, std::string_view expected) {
  const auto found = record.fields.find(std::string(key));
  Require(found != record.fields.end(), std::string("filespace record missing field ") + std::string(key));
  Require(found->second == expected,
          std::string("filespace record field ") + std::string(key) + " has unexpected value");
}

void RequireFilespaceCatalogRecord(const std::vector<DecodedRecord>& records,
                                   const TypedUuid& database_uuid,
                                   const TypedUuid& filespace_uuid) {
  const auto filespaces = FilespaceRecords(records);
  Require(filespaces.size() == 1, "catalog did not contain exactly one typed filespace record");
  const auto& record = filespaces.front();

  RequireField(record, "database_uuid", UuidString(database_uuid));
  RequireField(record, "filespace_uuid", UuidString(filespace_uuid));
  RequireField(record, "filespace_role", "active_primary");
  RequireField(record, "first_filespace", "1");
  RequireField(record, "startup_authority", "1");
  RequireField(record, "catalog_persistence_owner", "1");
  RequireField(record, "filespace_manifest_owner", "1");
  RequireField(record, "recovery_evidence_owner", "1");
  RequireField(record, "state", "online");
  RequireField(record, "read_only", "0");

  RequireField(record, "physical_filespace_id", "0");
  RequireField(record, "lifecycle_generation", "1");
  RequireField(record, "filespace_manifest_generation", "1");
  RequireField(record, "registered_txn", "1");
  RequireField(record, "last_lifecycle_transaction", "1");
  RequireField(record, "uuid_source", "fresh_uuidv7");
  RequireField(record, "header_database_uuid_match_required", "1");
  RequireField(record, "header_filespace_uuid_match_required", "1");
  RequireField(record, "startup_state_coupled", "1");
  RequireField(record, "page_header_coupled", "1");
  RequireField(record, "open_validate_header", "1");
  RequireField(record, "attach_admission_validate_header", "1");
  RequireField(record, "transaction_admission_validate_filespace", "1");
  RequireField(record, "maintenance_validate_header", "1");
  RequireField(record, "verify_repair_validate_header", "1");
  RequireField(record, "shutdown_validate_header", "1");
  RequireField(record, "recovery_validate_header", "1");
  RequireField(record, "drop_requires_database_lifecycle", "1");
  RequireField(record, "quarantine_on_ambiguous", "1");
  RequireField(record, "state_change_evidence_before_success", "1");
  RequireField(record, "mga_visibility_required", "1");
  RequireField(record, "path_is_locator_not_identity", "1");
  RequireField(record, "duplicate_identity_refusal", "1");
  RequireField(record, "stale_identity_refusal", "1");
}

void RequireStartupIdentity(const db::DatabaseLifecycleState& state,
                            const TypedUuid& database_uuid,
                            const TypedUuid& filespace_uuid,
                            std::string_view context) {
  Require(SameTypedUuid(state.database_uuid, database_uuid),
          std::string(context) + " database_uuid did not match create config");
  Require(SameTypedUuid(state.filespace_uuid, filespace_uuid),
          std::string(context) + " filespace_uuid did not match create config");
  Require(state.startup_state_present, std::string(context) + " startup state was missing");
  Require(SameTypedUuid(state.startup_state.database_uuid, database_uuid),
          std::string(context) + " startup database_uuid did not match create config");
  Require(SameTypedUuid(state.startup_state.first_filespace_uuid, filespace_uuid),
          std::string(context) + " startup first_filespace_uuid did not match create config");
}

void RequireFixedPageHeaders(const std::filesystem::path& database_path,
                             std::uint32_t page_size,
                             const TypedUuid& database_uuid,
                             const TypedUuid& filespace_uuid) {
  disk::FileDevice device;
  const auto opened = device.Open(database_path.string(), disk::FileOpenMode::open_existing_read_only);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "could not open database read-only for fixed page header verification");

  disk::DiskDevicePolicy policy;
  policy.page_size = page_size;
  policy.access_mode = disk::DiskAccessMode::read_only;
  policy.checksum_policy = disk::DiskChecksumPolicy::require_valid;
  policy.unknown_page_policy = disk::UnknownPagePolicy::reject_all;

  const std::array<std::uint64_t, 5> fixed_pages = {{
      db::kSystemStatePageNumber,
      db::kCatalogPageNumber,
      db::kAllocationMapPageNumber,
      db::kTransactionInventoryPageNumber,
      db::kBootstrapReservedPageNumber,
  }};

  for (const auto page_number : fixed_pages) {
    const auto page_header = disk::ReadDevicePageHeader(&device, page_size, page_number, policy);
    if (!page_header.ok()) {
      std::cerr << page_header.diagnostic.diagnostic_code << '\n';
    }
    Require(page_header.ok(), "fixed page header did not validate");
    const auto parsed = disk::ParsePageHeader(page_header.serialized);
    if (!parsed.ok()) {
      std::cerr << parsed.diagnostic.diagnostic_code << '\n';
    }
    Require(parsed.ok(), "fixed page header parse failed");
    Require(parsed.header.database_uuid == database_uuid.value,
            "fixed page header database_uuid did not match create config");
    Require(parsed.header.filespace_uuid == filespace_uuid.value,
            "fixed page header filespace_uuid did not match first filespace");
  }
}

db::DatabaseLifecycleResult OpenDatabase(const std::filesystem::path& database_path, bool read_only) {
  db::DatabaseOpenConfig open;
  open.path = database_path.string();
  open.read_only = read_only;
  return db::OpenDatabaseFile(open);
}

}  // namespace

int main() {
  const auto database_path = TestDatabasePath();
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      if (!path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }
    }
  } cleanup{database_path};

  const auto now = CurrentUnixMillis();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(), "test UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = now;
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  create.allow_overwrite = true;

  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
  }
  Require(created.ok(), "CreateDatabaseFile failed");
  Require(created.state.resource_seed_catalog_present, "create did not return resource seed catalog");
  Require(created.state.resource_seed_catalog.active, "resource seed catalog was not active");
  Require(created.state.resource_seed_catalog.seed_pack_name == "initial-resource-pack",
          "real initial-resource-pack was not used");
  RequireStartupIdentity(created.state, database_uuid.value, filespace_uuid.value, "create");
  RequireFixedPageHeaders(database_path,
                          created.state.header.page_size,
                          database_uuid.value,
                          filespace_uuid.value);

  const auto rows = ReadCatalogRows(database_path, created.state.header.page_size);
  const auto records = DecodeTypedRecords(rows);
  RequireFilespaceCatalogRecord(records, database_uuid.value, filespace_uuid.value);

  const auto opened_ro = OpenDatabase(database_path, true);
  if (!opened_ro.ok()) {
    std::cerr << opened_ro.diagnostic.diagnostic_code << '\n';
  }
  Require(opened_ro.ok(), "OpenDatabaseFile read-only failed");
  RequireStartupIdentity(opened_ro.state, database_uuid.value, filespace_uuid.value, "read-only open");

  const auto opened_rw = OpenDatabase(database_path, false);
  if (!opened_rw.ok()) {
    std::cerr << opened_rw.diagnostic.diagnostic_code << '\n';
  }
  Require(opened_rw.ok(), "OpenDatabaseFile read-write failed");
  RequireStartupIdentity(opened_rw.state, database_uuid.value, filespace_uuid.value, "read-write open");

  const auto clean = db::MarkDatabaseCleanShutdown(database_path.string());
  if (!clean.ok()) {
    std::cerr << clean.diagnostic.diagnostic_code << '\n';
  }
  Require(clean.ok(), "MarkDatabaseCleanShutdown failed");

  const auto reopened_ro = OpenDatabase(database_path, true);
  if (!reopened_ro.ok()) {
    std::cerr << reopened_ro.diagnostic.diagnostic_code << '\n';
  }
  Require(reopened_ro.ok(), "OpenDatabaseFile read-only after clean shutdown failed");
  RequireStartupIdentity(reopened_ro.state,
                         database_uuid.value,
                         filespace_uuid.value,
                         "read-only reopen after clean shutdown");
  RequireFixedPageHeaders(database_path,
                          reopened_ro.state.header.page_size,
                          database_uuid.value,
                          filespace_uuid.value);

  return EXIT_SUCCESS;
}
