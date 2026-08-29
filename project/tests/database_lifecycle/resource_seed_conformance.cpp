// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "catalog/name_resolution_api.hpp"
#include "database_lifecycle.hpp"
#include "ddl/create_api.hpp"
#include "disk_device.hpp"
#include "memory.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "page_header.hpp"
#include "page_manager.hpp"
#include "resource_seed_pack.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
namespace engine = scratchbird::engine::internal_api;
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
  const auto directory = std::filesystem::temp_directory_path() /
                         ("sb_dblc_013aa_resource_seed_" +
                          std::to_string(UniqueMillis()));
  std::filesystem::create_directories(directory);
  return directory / "resource_seed.sbdb";
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
    std::cerr << loaded.diagnostic.diagnostic_code << ':'
              << loaded.diagnostic.message_key;
    for (const auto& argument : loaded.diagnostic.arguments) {
      std::cerr << ':' << argument.key << '=' << argument.value;
    }
    std::cerr << '\n';
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

void RequireGbkDescriptorModel(const resources::ResourceSeedCatalogImage& image,
                               bool durable_identity_required) {
  const auto* gbk = resources::FindResourceSeedCharset(image, "CP936");
  Require(gbk != nullptr, "CP936 did not resolve to the GBK charset descriptor");
  Require(gbk->canonical_name == "GBK", "CP936 resolved to the wrong charset");
  Require(gbk->min_bytes == 1 && gbk->max_bytes == 2,
          "GBK width metadata is incorrect");
  Require(gbk->family_epoch == image.charset_epoch &&
              gbk->family_version == image.charset_version,
          "GBK charset family authority is incomplete");
  Require(gbk->default_collation_name == "GBK",
          "GBK default collation was not derived by the neutral resource loader");

  const auto* gbk_collation = resources::FindResourceSeedCollation(image, "gbk");
  const auto* unicode_collation =
      resources::FindResourceSeedCollation(image, "GBK_UNICODE");
  Require(gbk_collation != nullptr && unicode_collation != nullptr,
          "GBK collation descriptors are missing");
  Require(gbk_collation->charset_name == "GBK" &&
              unicode_collation->charset_name == "GBK",
          "GBK collation parent names are incorrect");
  Require(gbk_collation->default_for_charset &&
              !unicode_collation->default_for_charset,
          "GBK default collation authority is ambiguous");
  Require(gbk_collation->default_authority ==
              "seed_pack.default_collations.v1",
          "GBK default collation lacks explicit seed-pack authority");
  Require(gbk_collation->family_epoch == image.collation_epoch &&
              gbk_collation->family_version == image.collation_version,
          "GBK collation family authority is incomplete");

  if (durable_identity_required) {
    Require(!gbk->resource_uuid.empty() && !gbk_collation->resource_uuid.empty() &&
                !unicode_collation->resource_uuid.empty(),
            "database-scoped GBK resource UUIDs are missing");
    Require(gbk_collation->charset_uuid == gbk->resource_uuid &&
                unicode_collation->charset_uuid == gbk->resource_uuid,
            "GBK collation parent UUIDs do not match the charset UUID");
    Require(gbk->default_collation_uuid == gbk_collation->resource_uuid,
            "GBK default collation UUID was not retained");
  }
}

void RequirePersistedGbkRecords(const std::vector<DecodedRecord>& records,
                                const resources::ResourceSeedCatalogImage& image) {
  const auto* gbk = resources::FindResourceSeedCharset(image, "GBK");
  const auto* collation = resources::FindResourceSeedCollation(image, "GBK");
  Require(gbk != nullptr && collation != nullptr,
          "GBK descriptors are unavailable for typed-row verification");
  bool saw_charset = false;
  bool saw_collation = false;
  for (const auto& record : records) {
    const auto canonical = record.fields.find("canonical_name");
    if (canonical == record.fields.end() || canonical->second != "GBK") continue;
    if (record.record.header.kind == catalog::CatalogRecordKind::charset) {
      saw_charset = uuid::UuidToString(record.record.header.object_uuid.value) ==
                        gbk->resource_uuid &&
                    record.fields.at("min_bytes") == "1" &&
                    record.fields.at("max_bytes") == "2" &&
                    record.fields.at("default_collation_uuid") ==
                        gbk->default_collation_uuid;
    } else if (record.record.header.kind == catalog::CatalogRecordKind::collation) {
      saw_collation =
          uuid::UuidToString(record.record.header.object_uuid.value) ==
              collation->resource_uuid &&
          uuid::UuidToString(record.record.header.parent_uuid.value) ==
              gbk->resource_uuid &&
          record.fields.at("charset_uuid") == gbk->resource_uuid &&
          record.fields.at("default_for_charset") == "1" &&
          record.fields.at("default_authority") ==
              "seed_pack.default_collations.v1";
    }
  }
  Require(saw_charset, "typed GBK charset descriptor record is incomplete");
  Require(saw_collation, "typed GBK collation relationship record is incomplete");
}

engine::EngineRequestContext BeginEngineTransaction(
    const std::filesystem::path& database_path,
    const db::DatabaseLifecycleResult& created,
    std::uint64_t now,
    bool read_only = false) {
  engine::EngineRequestContext context;
  context.trust_mode = engine::EngineTrustMode::server_isolated;
  context.request_id = read_only
                           ? "resource-seed-descriptor-conformance-read-only"
                           : "resource-seed-descriptor-conformance";
  context.database_path = database_path.string();
  context.database_uuid.canonical =
      uuid::UuidToString(created.state.database_uuid.value);
  const auto principal = uuid::GenerateEngineIdentityV7(UuidKind::principal, now + 300);
  const auto session = uuid::GenerateEngineIdentityV7(UuidKind::object, now + 301);
  Require(principal.ok() && session.ok(), "engine context UUID generation failed");
  context.principal_uuid.canonical = uuid::UuidToString(principal.value.value);
  context.session_uuid.canonical = uuid::UuidToString(session.value.value);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = created.state.resource_seed_catalog.resource_epoch;
  context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.name_resolution_epoch = 1;

  engine::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  if (read_only) {
    begin.transaction_policy_profile.encoded_profiles.push_back(
        "transaction_read_mode:read_only");
  }
  const auto begun = engine::EngineBeginTransaction(begin);
  Require(begun.ok, "engine transaction begin failed for resource resolution");
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  context.read_only_mode = begun.read_only;
  return context;
}

void RequireEngineResourceResolution(const engine::EngineRequestContext& context) {
  engine::EngineResolveNameRequest charset_request;
  charset_request.context = context;
  charset_request.sql_object_reference.expected_object_type = "charset";
  charset_request.sql_object_reference.object_name.raw_text = "CP936";
  const auto charset = engine::EngineResolveName(charset_request);
  Require(charset.ok && charset.resource_descriptor.present,
          "engine did not resolve CP936 through durable charset authority");
  Require(charset.resource_descriptor.canonical_name == "GBK" &&
              charset.resource_descriptor.max_bytes == 2 &&
              !charset.resource_descriptor.default_collation_uuid.canonical.empty(),
          "engine returned incomplete GBK charset metadata");

  engine::EngineResolveNameRequest collation_request;
  collation_request.context = context;
  collation_request.sql_object_reference.expected_object_type = "collation";
  collation_request.sql_object_reference.object_name.raw_text = "gbk";
  const auto collation = engine::EngineResolveName(collation_request);
  Require(collation.ok && collation.resource_descriptor.present,
          "engine did not resolve GBK through durable collation authority");
  Require(collation.resource_descriptor.default_for_parent &&
              collation.resource_descriptor.parent_resource_uuid.canonical ==
                  charset.primary_object.uuid.canonical,
          "engine returned an invalid GBK collation relationship");

  auto stale_context = context;
  ++stale_context.resource_epoch;
  charset_request.context = stale_context;
  const auto stale = engine::EngineResolveName(charset_request);
  Require(!stale.ok && !stale.diagnostics.empty() &&
              stale.diagnostics.front().code == "CATALOG.RESOURCE.EPOCH_STALE",
          "stale resource epoch was not refused by the engine");

  engine::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(engine::EngineRollbackTransaction(rollback).ok,
          "engine transaction rollback failed after resource resolution");
}

template <typename TResult>
void RequireEngineOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, message);
}

bool HasDiagnosticDetail(const engine::EngineApiResult& result,
                         std::string_view expected) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.find(expected) != std::string::npos) return true;
  }
  return false;
}

std::string NewEngineUuidText(UuidKind kind, std::uint64_t now) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, now);
  Require(generated.ok(), "engine DDL UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

engine::EngineLocalizedName EngineName(std::string value) {
  engine::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = value;
  name.raw_name_text = value;
  name.display_name = value;
  name.default_name = true;
  return name;
}

engine::EngineColumnDefinition EngineColumn(std::string name,
                                            std::string type,
                                            std::string descriptor) {
  engine::EngineColumnDefinition column;
  column.names.push_back(EngineName(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  column.descriptor.encoded_descriptor = std::move(descriptor);
  column.nullable = true;
  return column;
}

engine::EngineCreateTableResult CreateEngineTable(
    const engine::EngineRequestContext& context,
    const std::string& schema_uuid,
    std::string table_name,
    engine::EngineColumnDefinition column) {
  engine::EngineCreateTableRequest table;
  table.context = context;
  table.target_schema.uuid.canonical = schema_uuid;
  table.target_schema.object_kind = "schema";
  table.table_names.push_back(EngineName(std::move(table_name)));
  table.table_columns.push_back(std::move(column));
  return engine::EngineCreateTable(table);
}

std::string ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

engine::MgaRelationStorageDescriptor LoadPersistedDescriptorWithoutWrite(
    const engine::EngineRequestContext& context,
    const std::string& relation_uuid,
    const std::filesystem::path& descriptor_path) {
  const std::string before = ReadBinaryFile(descriptor_path);
  Require(!before.empty(),
          "CREATE TABLE did not persist an MGA relation descriptor");
  const auto loaded =
      engine::LoadMgaRelationStorageDescriptor(context, relation_uuid);
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.detail
              << '\n';
  }
  Require(loaded.ok, "persisted MGA relation descriptor load failed");
  Require(ReadBinaryFile(descriptor_path) == before,
          "load-only relation descriptor API changed durable bytes");
  return loaded.descriptor;
}

void RequireResourceColumn(
    const engine::MgaRelationStorageDescriptor& descriptor,
    const std::string& expected_relation_uuid,
    const std::string& expected_charset_uuid,
    const std::string& expected_collation_uuid) {
  Require(descriptor.relation_uuid.canonical == expected_relation_uuid,
          "relation descriptor identifies the wrong table");
  Require(descriptor.columns.size() == 1,
          "relation descriptor column count is incorrect");
  const auto& column = descriptor.columns.front();
  Require(column.charset_uuid == expected_charset_uuid,
          "relation descriptor lost the charset UUID");
  Require(column.collation_uuid == expected_collation_uuid,
          "relation descriptor lost the collation UUID");
  Require(column.character_length == 20,
          "relation descriptor lost canonical character length");
  Require(column.value_descriptor.encoded_descriptor.find(
              "character_length=20") != std::string::npos,
          "encoded descriptor lost canonical character length");
}

void RequireLargeTextResourceColumn(
    const engine::MgaRelationStorageDescriptor& descriptor,
    const std::string& expected_relation_uuid,
    const std::string& expected_charset_uuid,
    const std::string& expected_collation_uuid) {
  Require(descriptor.relation_uuid.canonical == expected_relation_uuid,
          "large-text relation descriptor identifies the wrong table");
  Require(descriptor.columns.size() == 1,
          "large-text relation descriptor column count is incorrect");
  const auto& column = descriptor.columns.front();
  Require(column.charset_uuid == expected_charset_uuid,
          "large-text relation descriptor lost the charset UUID");
  Require(column.collation_uuid == expected_collation_uuid,
          "large-text relation descriptor lost the collation UUID");
  Require(column.character_length == 0,
          "large-text relation descriptor fabricated a character length");
  Require(column.value_descriptor.canonical_type_name == "BLOB" &&
              column.value_descriptor.encoded_descriptor.find(
                  "text_resource_storage=large_object") !=
                  std::string::npos &&
              column.value_descriptor.encoded_descriptor.find(
                  "character_length=") == std::string::npos,
          "large-text relation descriptor lost its canonical storage semantics");
}

void RequireGbkRelationDescriptorPersistence(
    const std::filesystem::path& database_path,
    const db::DatabaseLifecycleResult& created,
    std::uint64_t now) {
  const auto& image = created.state.resource_seed_catalog;
  const auto* gbk = resources::FindResourceSeedCharset(image, "GBK");
  const auto* gbk_default = resources::FindResourceSeedCollation(image, "GBK");
  const auto* gbk_unicode =
      resources::FindResourceSeedCollation(image, "GBK_UNICODE");
  Require(gbk != nullptr && gbk_default != nullptr && gbk_unicode != nullptr,
          "durable GBK descriptors are missing for relation DDL");
  const resources::ResourceSeedCollationDescriptor* foreign_collation = nullptr;
  for (const auto& candidate : image.collations) {
    if (!candidate.resource_uuid.empty() &&
        !candidate.charset_uuid.empty() &&
        candidate.charset_uuid != gbk->resource_uuid) {
      foreign_collation = &candidate;
      break;
    }
  }
  Require(foreign_collation != nullptr,
          "foreign charset collation is missing for mismatch refusal");

  auto context = BeginEngineTransaction(database_path, created, now + 1000);
  engine::EngineUuid gbk_uuid;
  gbk_uuid.canonical = gbk->resource_uuid;
  auto missing_epoch_context = context;
  missing_epoch_context.resource_epoch = 0;
  const auto missing_epoch = engine::LookupEngineResourceDescriptorByUuid(
      missing_epoch_context, gbk_uuid, "charset");
  Require(!missing_epoch.ok &&
              missing_epoch.diagnostic.code ==
                  "CATALOG.RESOURCE.EPOCH_REQUIRED",
          "UUID resource lookup accepted a zero resource epoch");
  auto stale_epoch_context = context;
  ++stale_epoch_context.resource_epoch;
  const auto stale_epoch = engine::LookupEngineResourceDescriptorByUuid(
      stale_epoch_context, gbk_uuid, "charset");
  Require(!stale_epoch.ok &&
              stale_epoch.diagnostic.code == "CATALOG.RESOURCE.EPOCH_STALE",
          "UUID resource lookup accepted a stale resource epoch");

  const std::string schema_uuid =
      NewEngineUuidText(UuidKind::schema, now + 1100);
  context.current_schema_uuid.canonical = schema_uuid;
  engine::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid.canonical = schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(EngineName("resource_relation"));
  RequireEngineOk(engine::EngineCreateSchema(schema),
                  "resource relation schema creation failed");

  const auto explicit_table = CreateEngineTable(
      context,
      schema_uuid,
      "explicit_gbk",
      EngineColumn("f1",
                   "text",
                   "type=text;charset_uuid=" + gbk->resource_uuid +
                       ";collation_uuid=" + gbk_unicode->resource_uuid +
                       ";character_length=20"));
  RequireEngineOk(explicit_table,
                  "explicit GBK/GBK_UNICODE table creation failed");
  Require(!explicit_table.effective_table_descriptor.descriptor_uuid.canonical.empty(),
          "CREATE TABLE did not publish its MGA descriptor identity");

  const auto default_table = CreateEngineTable(
      context,
      schema_uuid,
      "default_gbk",
      EngineColumn("f1",
                   "text",
                   "type=text;charset_uuid=" + gbk->resource_uuid +
                       ";character_length=20"));
  RequireEngineOk(default_table,
                  "GBK default-collation table creation failed");

  const auto large_text_table = CreateEngineTable(
      context,
      schema_uuid,
      "large_text_gbk",
      EngineColumn("f1",
                   "BLOB",
                   "type=BLOB;charset_uuid=" + gbk->resource_uuid +
                       ";collation_uuid=" + gbk_unicode->resource_uuid +
                       ";text_resource_storage=large_object"));
  RequireEngineOk(large_text_table,
                  "GBK large-text object table creation failed");

  const auto missing_character_length = CreateEngineTable(
      context,
      schema_uuid,
      "missing_character_length",
      EngineColumn("f1",
                   "VARCHAR",
                   "type=VARCHAR;charset_uuid=" + gbk->resource_uuid));
  Require(!missing_character_length.ok &&
              HasDiagnosticDetail(
                  missing_character_length,
                  "text_resource_descriptor_requires_character_length"),
          "bounded character string accepted resource UUIDs without a length");

  const auto unmarked_large_text = CreateEngineTable(
      context,
      schema_uuid,
      "unmarked_large_text",
      EngineColumn("f1",
                   "BLOB",
                   "type=BLOB;charset_uuid=" + gbk->resource_uuid));
  Require(!unmarked_large_text.ok &&
              HasDiagnosticDetail(
                  unmarked_large_text,
                  "text_resource_modifier_on_incompatible_type"),
          "BLOB resource UUIDs were accepted without large-text semantics");

  const auto invalid_large_text_storage = CreateEngineTable(
      context,
      schema_uuid,
      "invalid_large_text_storage",
      EngineColumn("f1",
                   "BLOB",
                   "type=BLOB;charset_uuid=" + gbk->resource_uuid +
                       ";text_resource_storage=inline"));
  Require(!invalid_large_text_storage.ok &&
              HasDiagnosticDetail(invalid_large_text_storage,
                                  "text_resource_storage_invalid"),
          "unknown text resource storage semantics were accepted");

  const auto bounded_large_text = CreateEngineTable(
      context,
      schema_uuid,
      "bounded_large_text",
      EngineColumn("f1",
                   "BLOB",
                   "type=BLOB;charset_uuid=" + gbk->resource_uuid +
                       ";character_length=20;"
                       "text_resource_storage=large_object"));
  Require(!bounded_large_text.ok &&
              HasDiagnosticDetail(
                  bounded_large_text,
                  "large_object_text_resource_forbids_character_length"),
          "large-text object accepted a fabricated character length");

  const auto incompatible_large_text = CreateEngineTable(
      context,
      schema_uuid,
      "incompatible_large_text",
      EngineColumn("f1",
                   "INTEGER",
                   "type=INTEGER;charset_uuid=" + gbk->resource_uuid +
                       ";text_resource_storage=large_object"));
  Require(!incompatible_large_text.ok &&
              HasDiagnosticDetail(
                  incompatible_large_text,
                  "text_resource_modifier_on_incompatible_type"),
          "non-LOB column accepted large-text storage semantics");

  const auto mismatch = CreateEngineTable(
      context,
      schema_uuid,
      "mismatched_gbk",
      EngineColumn("f1",
                   "VARCHAR(20)",
                   "type=VARCHAR(20);charset_uuid=" + gbk->resource_uuid +
                       ";collation_uuid=" +
                       foreign_collation->resource_uuid +
                       ";character_length=20"));
  Require(!mismatch.ok &&
              HasDiagnosticDetail(
                  mismatch, "collation_charset_relationship_mismatch"),
          "mismatched charset/collation relationship was accepted");

  const auto incompatible = CreateEngineTable(
      context,
      schema_uuid,
      "incompatible_integer",
      EngineColumn("f1",
                   "INTEGER",
                   "type=INTEGER;charset_uuid=" + gbk->resource_uuid +
                       ";character_length=20"));
  Require(!incompatible.ok &&
              HasDiagnosticDetail(
                  incompatible, "text_resource_modifier_on_incompatible_type"),
          "integer column accepted text resource modifiers");

  const auto descriptor_path = std::filesystem::path(
      database_path.string() + ".sb.mga_relation_metadata");
  const auto explicit_descriptor = LoadPersistedDescriptorWithoutWrite(
      context, explicit_table.primary_object.uuid.canonical, descriptor_path);
  const auto default_descriptor = LoadPersistedDescriptorWithoutWrite(
      context, default_table.primary_object.uuid.canonical, descriptor_path);
  const auto large_text_descriptor = LoadPersistedDescriptorWithoutWrite(
      context,
      large_text_table.primary_object.uuid.canonical,
      descriptor_path);
  RequireResourceColumn(explicit_descriptor,
                        explicit_table.primary_object.uuid.canonical,
                        gbk->resource_uuid,
                        gbk_unicode->resource_uuid);
  RequireResourceColumn(default_descriptor,
                        default_table.primary_object.uuid.canonical,
                        gbk->resource_uuid,
                        gbk_default->resource_uuid);
  RequireLargeTextResourceColumn(
      large_text_descriptor,
      large_text_table.primary_object.uuid.canonical,
      gbk->resource_uuid,
      gbk_unicode->resource_uuid);
  const auto explicit_fields =
      engine::SerializeMgaRelationStorageDescriptor(explicit_descriptor);
  const auto default_fields =
      engine::SerializeMgaRelationStorageDescriptor(default_descriptor);
  const auto large_text_fields =
      engine::SerializeMgaRelationStorageDescriptor(large_text_descriptor);

  auto wrong_transaction = context;
  wrong_transaction.transaction_uuid.canonical =
      NewEngineUuidText(UuidKind::transaction, now + 1200);
  const std::string before_refusal = ReadBinaryFile(descriptor_path);
  const auto refused = engine::LoadMgaRelationStorageDescriptor(
      wrong_transaction, explicit_table.primary_object.uuid.canonical);
  Require(!refused.ok &&
              refused.diagnostic.detail.find(
                  "exact_active_transaction_identity_required") !=
                  std::string::npos,
          "descriptor load accepted a mismatched transaction UUID");
  Require(ReadBinaryFile(descriptor_path) == before_refusal,
          "refused descriptor load changed durable bytes");

  engine::EngineCommitTransactionRequest commit;
  commit.context = context;
  RequireEngineOk(engine::EngineCommitTransaction(commit),
                  "resource relation transaction commit failed");

  db::DatabaseOpenConfig reopen;
  reopen.path = database_path.string();
  reopen.read_only = true;
  reopen.suppress_background_agents = true;
  RequireOk(db::OpenDatabaseFile(reopen),
            "resource relation database read-only reopen failed");

  auto reopened_context =
      BeginEngineTransaction(database_path, created, now + 2000, true);
  reopened_context.current_schema_uuid.canonical = schema_uuid;
  const auto reopened_explicit = LoadPersistedDescriptorWithoutWrite(
      reopened_context,
      explicit_table.primary_object.uuid.canonical,
      descriptor_path);
  const auto reopened_default = LoadPersistedDescriptorWithoutWrite(
      reopened_context,
      default_table.primary_object.uuid.canonical,
      descriptor_path);
  const auto reopened_large_text = LoadPersistedDescriptorWithoutWrite(
      reopened_context,
      large_text_table.primary_object.uuid.canonical,
      descriptor_path);
  Require(engine::SerializeMgaRelationStorageDescriptor(reopened_explicit) ==
              explicit_fields,
          "explicit resource descriptor changed after commit/reopen");
  Require(engine::SerializeMgaRelationStorageDescriptor(reopened_default) ==
              default_fields,
          "default resource descriptor changed after commit/reopen");
  Require(engine::SerializeMgaRelationStorageDescriptor(reopened_large_text) ==
              large_text_fields,
          "large-text resource descriptor changed after commit/reopen");

  engine::EngineRollbackTransactionRequest rollback;
  rollback.context = reopened_context;
  RequireEngineOk(engine::EngineRollbackTransaction(rollback),
                  "read-only resource relation transaction rollback failed");
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
  RequireGbkDescriptorModel(loaded_image, false);
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
      std::filesystem::remove_all(path.parent_path(), ignored);
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
  RequireGbkDescriptorModel(created.state.resource_seed_catalog, true);

  db::DatabaseOpenConfig read_only_open;
  read_only_open.path = database_path.string();
  read_only_open.read_only = true;
  read_only_open.expected_resource_seed_pack_name = created.state.resource_seed_catalog.seed_pack_name;
  read_only_open.expected_resource_seed_pack_version = created.state.resource_seed_catalog.seed_pack_version;
  read_only_open.expected_resource_seed_pack_content_hash = created.state.resource_seed_catalog.content_hash;
  const auto opened = db::OpenDatabaseFile(read_only_open);
  RequireOk(opened, "OpenDatabaseFile read-only with matching seed failed");
  RequireSeedLifecycleReady(opened.state.resource_seed_catalog);
  RequireGbkDescriptorModel(opened.state.resource_seed_catalog, true);
  const auto* created_gbk =
      resources::FindResourceSeedCharset(created.state.resource_seed_catalog, "GBK");
  const auto* opened_gbk =
      resources::FindResourceSeedCharset(opened.state.resource_seed_catalog, "GBK");
  Require(created_gbk != nullptr && opened_gbk != nullptr &&
              created_gbk->resource_uuid == opened_gbk->resource_uuid &&
              created_gbk->default_collation_uuid ==
                  opened_gbk->default_collation_uuid,
          "GBK durable identities changed across database reopen");

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
  RequirePersistedGbkRecords(records, opened.state.resource_seed_catalog);
  RequireEngineResourceResolution(
      BeginEngineTransaction(database_path, created, now));
  RequireGbkRelationDescriptorPersistence(database_path, created, now);

  return EXIT_SUCCESS;
}
