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
#include "resource_seed_pack.hpp"
#include "uuid.hpp"

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
namespace memory = scratchbird::core::memory;
namespace page = scratchbird::storage::page;
namespace resources = scratchbird::core::resources;
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

void RequireOk(const db::DatabaseLifecycleResult& result, std::string_view message) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << '\n';
  }
  Require(result.ok(), message);
}

void RequireFailureCode(const db::DatabaseLifecycleResult& result,
                        std::string_view expected_code,
                        std::string_view message) {
  Require(!result.ok(), message);
  if (result.diagnostic.diagnostic_code != expected_code) {
    std::cerr << "expected=" << expected_code
              << " actual=" << result.diagnostic.diagnostic_code << '\n';
  }
  Require(result.diagnostic.diagnostic_code == expected_code, message);
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::uint64_t UniqueMillis() {
  static std::uint64_t counter = 0;
  return CurrentUnixMillis() + (++counter * 1000);
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc_013aa_resource_seed_" + std::to_string(UniqueMillis()) + ".sbdb");
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
  Require(opened.ok(), "could not open database read-only for catalog verification");

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
            "catalog chain page was not a catalog page");

    std::vector<scratchbird::core::platform::byte> body(
        page_size - disk::kPageHeaderSerializedBytes, 0);
    const auto read = device.ReadAt(page::PageOffset(page_size, page_number) +
                                        disk::kPageHeaderSerializedBytes,
                                    body.data(),
                                    body.size());
    Require(read.ok(), "catalog page body read failed");
    const auto parsed = page::ParseCatalogPageBody(body, page_number);
    Require(parsed.ok(), "catalog page body parse failed");
    rows.insert(rows.end(), parsed.body.rows.begin(), parsed.body.rows.end());
    page_number = parsed.body.next_page_number;
  }
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
  return records;
}

bool HasResourceBundleField(const std::vector<DecodedRecord>& records,
                            std::string_view key,
                            std::string_view expected) {
  for (const auto& record : records) {
    if (record.record.header.kind != catalog::CatalogRecordKind::resource_bundle) {
      continue;
    }
    const auto found = record.fields.find(std::string(key));
    if (found != record.fields.end() && found->second == expected) {
      return true;
    }
  }
  return false;
}

bool HasIndexDependencyEvidence(const std::vector<DecodedRecord>& records,
                                std::string_view evidence) {
  for (const auto& record : records) {
    if (record.record.header.kind != catalog::CatalogRecordKind::index_descriptor) {
      continue;
    }
    const auto found = record.fields.find("resource_dependency_evidence");
    if (found != record.fields.end() && found->second == evidence) {
      const auto rebuild = record.fields.find("index_rebuild_required_on_epoch_change");
      const auto proof = record.fields.find("compatibility_proof_required");
      return rebuild != record.fields.end() && rebuild->second == "1" &&
             proof != record.fields.end() && proof->second == "1";
    }
  }
  return false;
}

resources::ResourceSeedCatalogImage LoadSeedPack() {
  resources::ResourceSeedLoadConfig config;
  config.seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  config.allow_minimal_bootstrap = false;
  const auto loaded = resources::LoadResourceSeedPack(config);
  if (!loaded.ok()) {
    std::cerr << loaded.diagnostic.diagnostic_code << '\n';
  }
  Require(loaded.ok(), "resource seed pack did not load");
  return loaded.image;
}

void RequireSeedLifecycleReady(const resources::ResourceSeedCatalogImage& image) {
  Require(image.active, "resource seed image is not active");
  Require(image.database_create_ready, "resource seed image is not create-ready");
  Require(image.database_open_ready, "resource seed image is not open-ready");
  Require(!image.charset_version.empty(), "charset seed version is missing");
  Require(!image.collation_version.empty(), "collation seed version is missing");
  Require(!image.locale_version.empty(), "locale seed version is missing");
  Require(!image.timezone_version.empty(), "timezone seed version is missing");
  Require(image.charset_epoch == 1, "charset activation epoch is not initialized");
  Require(image.collation_epoch == 1, "collation activation epoch is not initialized");
  Require(image.locale_epoch == 1, "locale activation epoch is not initialized");
  Require(image.timezone_epoch == 1, "timezone activation epoch is not initialized");
  Require(image.runtime_cache_epoch == 1, "runtime cache epoch is not initialized");
  Require(image.resource_activation_records >= 4, "resource activation evidence is missing");
  Require(image.runtime_cache_invalidation_records >= 9,
          "runtime cache invalidation coverage is missing");
  Require(image.index_dependency_records >= 2, "index dependency evidence is missing");
}

void RequireRuntimeCacheInvalidation(const resources::ResourceSeedCatalogImage& image) {
  const auto cache_epoch = resources::MakeResourceSeedRuntimeCacheEpoch(image);
  const auto current = resources::EvaluateResourceSeedRuntimeCache(image, cache_epoch);
  Require(current.ok() && current.cache_epoch_current,
          "current runtime cache epoch was not accepted");

  auto replacement = image;
  ++replacement.resource_epoch;
  ++replacement.collation_epoch;
  ++replacement.runtime_cache_epoch;
  replacement.collation_version += ":replacement";
  for (auto& family : replacement.family_versions) {
    if (family.family == resources::ResourceSeedFamily::collation) {
      family.version = replacement.collation_version;
      family.activation_epoch = replacement.collation_epoch;
    }
  }

  const auto stale = resources::EvaluateResourceSeedRuntimeCache(replacement, cache_epoch);
  Require(!stale.ok(), "stale runtime cache epoch was accepted");
  Require(stale.runtime_cache_invalidation_required,
          "stale runtime cache did not require invalidation");
  Require(stale.diagnostic.diagnostic_code == "RESOURCE.CACHE.INVALIDATION_REQUIRED",
          "stale runtime cache used the wrong diagnostic");
}

void RequireIndexDependencyEvidence(const resources::ResourceSeedCatalogImage& image) {
  resources::ResourceSeedIndexDependencyEvidence dependency;
  dependency.dependent_artifact_name = "sys.catalog.resource_seed_text_order_dependency_idx";
  dependency.dependent_artifact_class = "index";
  dependency.family = resources::ResourceSeedFamily::collation;
  dependency.required_version = image.collation_version;
  dependency.required_content_hash = image.collation_content_hash;
  dependency.dependency_epoch = image.collation_epoch;

  const auto current = resources::EvaluateResourceSeedIndexDependency(image, dependency);
  Require(current.ok() && current.index_dependency_current,
          "current index resource dependency was not accepted");

  auto replacement = image;
  ++replacement.collation_epoch;
  replacement.collation_version += ":incompatible";
  for (auto& family : replacement.family_versions) {
    if (family.family == resources::ResourceSeedFamily::collation) {
      family.version = replacement.collation_version;
      family.activation_epoch = replacement.collation_epoch;
    }
  }

  const auto stale = resources::EvaluateResourceSeedIndexDependency(replacement, dependency);
  Require(!stale.ok(), "stale index resource dependency was accepted");
  Require(stale.index_rebuild_required, "stale index did not require rebuild");
  Require(stale.diagnostic.diagnostic_code == "RESOURCE.INDEX.REBUILD_REQUIRED",
          "stale index dependency used the wrong diagnostic");

  dependency.compatibility_proven = true;
  dependency.compatibility_evidence = "semantic_equivalence_suite:resource_seed_text_order_v1";
  const auto proven = resources::EvaluateResourceSeedIndexDependency(replacement, dependency);
  Require(proven.ok(), "compatible index dependency proof was not accepted");
}

}  // namespace

int main() {
  auto memory_policy = memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "database_lifecycle_resource_seed_conformance";
  const auto memory_configured = memory::ConfigureDefaultMemoryManagerForFixture(
      memory_policy, "database_lifecycle_resource_seed_conformance");
  if (!memory_configured.ok()) {
    std::cerr << memory_configured.diagnostic.diagnostic_code << '\n';
  }
  Require(memory_configured.ok(), "default memory fixture configuration failed");

  const auto loaded_image = LoadSeedPack();
  RequireSeedLifecycleReady(loaded_image);
  RequireRuntimeCacheInvalidation(loaded_image);
  RequireIndexDependencyEvidence(loaded_image);

  resources::ResourceSeedLoadConfig missing_seed;
  missing_seed.seed_pack_root =
      (std::filesystem::temp_directory_path() / "sb_missing_resource_seed_pack").string();
  const auto missing = resources::LoadResourceSeedPack(missing_seed);
  Require(!missing.ok(), "missing required seed pack was accepted");
  Require(missing.diagnostic.diagnostic_code == "SB_RESOURCE_SEED_MISSING",
          "missing required seed pack used the wrong diagnostic");

  const auto database_path = TestDatabasePath();
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      std::filesystem::remove(path.string() + ".dirty.manifest", ignored);
      std::filesystem::remove(path.string() + ".recovery.evidence", ignored);
    }
  } cleanup{database_path};

  const auto now = UniqueMillis();
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
  RequireOk(created, "CreateDatabaseFile failed");
  RequireSeedLifecycleReady(created.state.resource_seed_catalog);

  db::DatabaseOpenConfig read_only_open;
  read_only_open.path = database_path.string();
  read_only_open.read_only = true;
  read_only_open.expected_resource_seed_pack_name = created.state.resource_seed_catalog.seed_pack_name;
  read_only_open.expected_resource_seed_pack_version = created.state.resource_seed_catalog.seed_pack_version;
  read_only_open.expected_resource_seed_pack_content_hash = created.state.resource_seed_catalog.content_hash;
  const auto opened = db::OpenDatabaseFile(read_only_open);
  RequireOk(opened, "OpenDatabaseFile read-only with matching seed failed");
  RequireSeedLifecycleReady(opened.state.resource_seed_catalog);

  db::DatabaseOpenConfig incompatible_open;
  incompatible_open.path = database_path.string();
  incompatible_open.read_only = false;
  incompatible_open.expected_resource_seed_pack_content_hash = "fnv1a64:not-current";
  const auto incompatible = db::OpenDatabaseFile(incompatible_open);
  RequireFailureCode(incompatible,
                     "FORMAT.UPGRADE_REQUIRED",
                     "incompatible resource seed did not refuse writable open");

  const auto rows = ReadCatalogRows(database_path, created.state.header.page_size);
  const auto records = DecodeTypedRecords(rows);
  Require(HasResourceBundleField(records, "database_create_ready", "1"),
          "resource bundle did not persist create readiness");
  Require(HasResourceBundleField(records, "database_open_ready", "1"),
          "resource bundle did not persist open readiness");
  Require(HasResourceBundleField(records, "runtime_cache_epoch", "1"),
          "resource bundle did not persist runtime cache epoch");
  Require(HasIndexDependencyEvidence(records,
                                     "index_dependency_charset_collation_locale_epoch_v1"),
          "text index dependency evidence is missing");
  Require(HasIndexDependencyEvidence(records, "index_dependency_timezone_epoch_v1"),
          "timezone index dependency evidence is missing");

  return EXIT_SUCCESS;
}
