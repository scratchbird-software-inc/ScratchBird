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
#include "memory.hpp"
#include "page_header.hpp"
#include "page_manager.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
namespace memory = scratchbird::core::memory;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
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

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_policy_pack_catalog_import_gate";
  policy.hard_limit_bytes = 8 * 1024 * 1024;
  policy.soft_limit_bytes = 8 * 1024 * 1024;
  policy.per_context_limit_bytes = 8 * 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 8 * 1024 * 1024;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(MemoryPolicy(),
                                                      "public_policy_pack_catalog_import_gate");
  Require(configured.ok(), "policy catalog import memory fixture should configure");
  Require(configured.fixture_mode, "policy catalog import memory must be fixture mode");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path UniquePath(std::string_view stem) {
  return std::filesystem::temp_directory_path() /
         (std::string(stem) + "_" + std::to_string(CurrentUnixMillis()));
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
  Require(opened.ok(), "could not open database read-only for catalog parse");

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
    Require(header.ok(), "catalog page header did not validate");
    Require(header.classification.page_type == disk::PageType::catalog,
            "catalog chain page was not catalog");

    std::vector<scratchbird::core::platform::byte> body(
        page_size - disk::kPageHeaderSerializedBytes, 0);
    const auto body_offset =
        page::CheckedPageBodyOffset(page_size,
                                    page_number,
                                    disk::kPageHeaderSerializedBytes);
    Require(body_offset.ok(), "catalog page body offset check failed");
    const auto read = device.ReadAt(body_offset.offset,
                                    body.data(),
                                    body.size());
    Require(read.ok(), "catalog page body read failed");

    const auto parsed = page::ParseCatalogPageBody(body, page_number);
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
    Require(decoded.ok(), "typed catalog record decode failed");
    records.push_back({decoded.record, ParsePayloadFields(decoded.record.payload)});
  }
  Require(!records.empty(), "no typed catalog records were decoded");
  return records;
}

std::uint32_t CountPolicyClass(const std::vector<DecodedRecord>& records,
                               std::string_view policy_class) {
  std::uint32_t count = 0;
  for (const auto& record : records) {
    if (record.record.header.kind != catalog::CatalogRecordKind::policy) {
      continue;
    }
    const auto found = record.fields.find("policy_class");
    if (found != record.fields.end() && found->second == policy_class) {
      ++count;
    }
  }
  return count;
}

std::uint32_t CountKindFromPack(const std::vector<DecodedRecord>& records,
                                catalog::CatalogRecordKind kind,
                                std::string_view grant_class = {}) {
  std::uint32_t count = 0;
  for (const auto& record : records) {
    if (record.record.header.kind != kind ||
        record.fields.count("policy_pack_uuid") == 0) {
      continue;
    }
    if (!grant_class.empty()) {
      const auto found = record.fields.find("grant_class");
      if (found == record.fields.end() || found->second != grant_class) {
        continue;
      }
    }
    ++count;
  }
  return count;
}

void RequireImportedRecordsCreatedByTx1(const std::vector<DecodedRecord>& records) {
  for (const auto& record : records) {
    if (record.fields.count("policy_pack_uuid") == 0) {
      continue;
    }
    Require(record.fields.count("creator_tx") != 0 && record.fields.at("creator_tx") == "1",
            "imported policy catalog record was not created by tx1");
    Require(record.fields.count("created_txn") != 0 && record.fields.at("created_txn") == "1",
            "imported policy catalog record is missing created_txn=1");
    Require(record.fields.count("loaded_at_database_create") != 0 &&
                record.fields.at("loaded_at_database_create") == "1",
            "imported policy catalog record was not marked create-time loaded");
  }
}

void RequirePolicyPackSummary(const std::vector<page::CatalogPageRow>& rows,
                              const db::PolicySeedPackDescriptor& descriptor) {
  std::uint32_t summaries = 0;
  for (const auto& row : rows) {
    if (row.kind != page::CatalogPageRowKind::policy_seed_pack) {
      continue;
    }
    ++summaries;
    const auto fields = ParsePayloadFields(row.payload);
    Require(fields.at("policy_pack_id") == descriptor.policy_pack_id,
            "policy pack summary id mismatch");
    Require(fields.at("policy_pack_uuid") == descriptor.policy_pack_uuid,
            "policy pack summary UUID mismatch");
    Require(fields.at("content_sha256") == descriptor.content_sha256,
            "policy pack summary content hash mismatch");
    Require(fields.at("create_time_only") == "1",
            "policy pack summary was not create-time-only");
    Require(fields.at("post_create_filesystem_authority") == "0",
            "policy pack summary allowed post-create filesystem authority");
    Require(fields.at("materialized_inside_create_transaction") == "1",
            "policy pack summary missing create transaction materialization");
    Require(fields.at("requires_mga_catalog_commit") == "1",
            "policy pack summary missing MGA catalog commit requirement");
  }
  Require(summaries == 1, "expected exactly one policy seed pack summary row");
}

void RequirePolicyCatalogImport(const std::filesystem::path& database_path,
                                const db::DatabaseLifecycleState& state) {
  const auto descriptor = db::DefaultPolicyPackDescriptor();
  Require(state.policy_seed_catalog_present, "create/open state did not report policy seed catalog");
  Require(state.policy_seed_catalog.active, "policy seed catalog image was inactive");
  Require(state.policy_seed_catalog.policy_pack_id == descriptor.policy_pack_id,
          "state policy pack id mismatch");
  Require(state.policy_seed_catalog.policy_pack_uuid == descriptor.policy_pack_uuid,
          "state policy pack UUID mismatch");
  Require(state.policy_seed_catalog.content_sha256 == descriptor.content_sha256,
          "state policy pack content hash mismatch");
  Require(state.policy_seed_catalog.local_password_only,
          "state policy pack was not local-password-only");
  Require(!state.policy_seed_catalog.post_create_filesystem_authority,
          "state policy pack allowed post-create filesystem authority");
  Require(state.policy_seed_catalog.materialized_inside_create_transaction,
          "state policy pack did not report create transaction materialization");
  Require(state.policy_seed_catalog.requires_mga_catalog_commit,
          "state policy pack did not require MGA catalog commit");
  Require(state.policy_seed_catalog.enabled_local_password_provider_records == 1,
          "state did not report exactly one local password provider");

  const auto rows = ReadCatalogRows(database_path, state.header.page_size);
  const auto records = DecodeTypedRecords(rows);
  RequirePolicyPackSummary(rows, descriptor);
  Require(CountPolicyClass(records, "policy_pack") == 1,
          "missing typed policy_pack record");
  Require(CountPolicyClass(records, "security_provider") == 12,
          "security provider policy rows were not imported");
  Require(CountPolicyClass(records, "policy_profile") == 14,
          "policy profile rows were not imported");
  Require(CountKindFromPack(records, catalog::CatalogRecordKind::role_account) == 5,
          "standard policy roles were not imported");
  Require(CountKindFromPack(records, catalog::CatalogRecordKind::group_account) == 6,
          "standard policy groups were not imported");
  Require(CountKindFromPack(records, catalog::CatalogRecordKind::grant_record, "privilege") == 10,
          "standard policy grants were not imported");
  Require(CountKindFromPack(records, catalog::CatalogRecordKind::grant_record, "group_membership") == 5,
          "standard policy group memberships were not imported");
  RequireImportedRecordsCreatedByTx1(records);
}

db::DatabaseCreateConfig BaseCreateConfig(const std::filesystem::path& path,
                                          std::uint64_t now) {
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(), "test UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = now;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.policy_seed_pack_root = SB_DEFAULT_POLICY_PACK_ROOT;
  create.require_policy_seed_pack = true;
  create.allow_overwrite = true;
  return create;
}

void RunSuccessfulImportCase() {
  const auto database_path = UniquePath("sb_pcr129_policy_import").replace_extension(".sbdb");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      if (!path.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + ".sb.owner.lock", ignored);
      }
    }
  } cleanup{database_path};

  const auto now = CurrentUnixMillis();
  const auto created = db::CreateDatabaseFile(BaseCreateConfig(database_path, now));
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
  }
  Require(created.ok(), "CreateDatabaseFile failed for default policy pack");
  RequirePolicyCatalogImport(database_path, created.state);

  db::DatabaseOpenConfig open;
  open.path = database_path.string();
  open.read_only = true;
  const auto opened = db::OpenDatabaseFile(open);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "OpenDatabaseFile failed after policy import");
  RequirePolicyCatalogImport(database_path, opened.state);
}

void RunHashMismatchCase() {
  const auto root = UniquePath("sb_pcr129_policy_bad_pack");
  const auto database_path = root;
  struct Cleanup {
    std::filesystem::path root;
    std::filesystem::path db_path;
    ~Cleanup() {
      std::error_code ignored;
      if (!db_path.empty()) {
        std::filesystem::remove(db_path, ignored);
        std::filesystem::remove(db_path.string() + ".sb.owner.lock", ignored);
      }
      if (!root.empty()) {
        std::filesystem::remove_all(root, ignored);
      }
    }
  } cleanup{root, database_path};

  std::filesystem::copy(SB_DEFAULT_POLICY_PACK_ROOT,
                        root,
                        std::filesystem::copy_options::recursive);
  {
    std::ofstream roles(root / "policies/roles.json", std::ios::app);
    roles << "\n";
  }

  auto create = BaseCreateConfig(database_path, CurrentUnixMillis() + 1000);
  create.policy_seed_pack_root = root.string();
  const auto created = db::CreateDatabaseFile(create);
  Require(!created.ok(), "hash-mismatched policy pack unexpectedly created database");
  Require(created.diagnostic.diagnostic_code == "SB-POLICY-PACK-CONTENT-HASH-MISMATCH",
          "hash-mismatched policy pack did not fail with content hash diagnostic");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  RunSuccessfulImportCase();
  RunHashMismatchCase();
  return EXIT_SUCCESS;
}
