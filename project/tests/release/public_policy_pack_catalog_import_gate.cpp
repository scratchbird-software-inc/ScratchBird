// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_security_record_codec.hpp"
#include "catalog_bootstrap_policy.hpp"
#include "local_transaction_store.hpp"
#include "../support/owned_temp_directory.hpp"
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "memory.hpp"
#include "hash_digest.hpp"
#include "page_header.hpp"
#include "page_manager.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace memory = scratchbird::core::memory;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
static_assert(std::is_same_v<decltype(db::PolicySeedPackCatalogImage{}.policy_pack_uuid),
                             scratchbird::core::platform::Uuid>);

struct DecodedRecord {
  catalog::CatalogTypedRecord record;
  std::map<std::string, std::string> fields;
};

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
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

scratchbird::tests::OwnedTempDirectory& TestRoot() {
  static scratchbird::tests::OwnedTempDirectory root;
  return root;
}
std::filesystem::path UniquePath(std::string_view stem) {
  return TestRoot().path() /
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
    if (catalog::IsCatalogSecurityRecordKind(decoded.record.header.kind)) {
      const auto security = catalog::DecodeCatalogSecurityRecord(decoded.record.header.kind, decoded.record.payload);
      Require(security.ok(), "imported native security payload did not decode");
      auto fields = security.record->attributes;
      // Readable assertions only; no textual identities persist in the catalog.
      for (const auto& [name, identity] : security.record->identities)
        fields.emplace(name, uuid::UuidToString(identity));
      records.push_back({decoded.record, std::move(fields)});
      continue;
    }
    if(catalog::IsCatalogBootstrapPolicyPayload(decoded.record.payload)) {
      const auto provider=catalog::DecodeCatalogBootstrapSecurityProvider(decoded.record.payload);
      const auto profile=catalog::DecodeCatalogBootstrapPolicyProfile(decoded.record.payload);
      const auto pack=catalog::DecodeCatalogBootstrapPolicyPack(decoded.record.payload);
      Require(provider.ok()||profile.ok()||pack.ok(),"native imported policy declaration did not decode");
      std::map<std::string,std::string> fields;
      fields.emplace("policy_class",provider.ok()?"security_provider":profile.ok()?"policy_profile":"policy_pack");
      // Test-side presentation only; the source body contains raw16 identity
      // and native unsigned origin fields, not these readable strings.
      fields.emplace("policy_pack_uuid",uuid::UuidToString(provider.ok()?provider.record->policy_pack_uuid:profile.ok()?profile.record->policy_pack_uuid:pack.record->policy_pack_uuid));
      records.push_back({decoded.record,std::move(fields)});
      continue;
    }
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

std::uint32_t CountDefaultPoliciesFromPack(const std::vector<DecodedRecord>& records) {
  std::uint32_t count = 0;
  for (const auto& record : records) {
    if (record.record.header.kind != catalog::CatalogRecordKind::policy ||
        record.fields.count("policy_pack_uuid") == 0 ||
        record.fields.count("default_profile") == 0) {
      continue;
    }
    const auto tx1_seed = record.fields.find("tx1_seed_required");
    const auto loaded = record.fields.find("loaded_at_database_create");
    const auto authority = record.fields.find("catalog_authority");
    if (tx1_seed != record.fields.end() && tx1_seed->second == "1" &&
        loaded != record.fields.end() && loaded->second == "1" &&
        authority != record.fields.end() &&
        authority->second == "durable_catalog_after_create") {
      ++count;
    }
  }
  return count;
}

void RequireImportedRecordsCreatedByTx1(const std::vector<DecodedRecord>& records,
                                      const scratchbird::core::platform::TypedUuid& expected_origin) {
  for (const auto& record : records) {
    if (record.fields.count("policy_pack_uuid") == 0) {
      continue;
    }
    if(catalog::IsCatalogBootstrapPolicyPayload(record.record.payload)) {
      const auto provider=catalog::DecodeCatalogBootstrapSecurityProvider(record.record.payload);
      const auto profile=catalog::DecodeCatalogBootstrapPolicyProfile(record.record.payload);
      const auto pack=catalog::DecodeCatalogBootstrapPolicyPack(record.record.payload);
      Require(provider.ok()||profile.ok()||pack.ok(),"native imported origin body invalid");
      const auto& origin=provider.ok()?provider.record->origin_transaction_uuid:profile.ok()?profile.record->origin_transaction_uuid:pack.record->origin_transaction_uuid;
      const auto local=provider.ok()?provider.record->origin_local_transaction_id:profile.ok()?profile.record->origin_local_transaction_id:pack.record->origin_local_transaction_id;
      Require(local==1&&origin.kind==UuidKind::transaction&&origin.kind==expected_origin.kind&&origin.value==expected_origin.value,
          "native imported declaration does not bind actual committed construction tx1 identity");
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
    const auto decoded=catalog::DecodeCatalogBootstrapPolicyPack(row.payload);
    Require(decoded.ok(),"policy pack summary is not a native declaration");
    const auto& pack=*decoded.record;
    const auto expected=uuid::ParseTypedUuid(UuidKind::object,descriptor.policy_pack_uuid);
    Require(pack.pack_id == descriptor.policy_pack_id,
            "policy pack summary id mismatch");
    Require(expected.ok()&&pack.policy_pack_uuid == expected.value.value,
            "policy pack summary UUID mismatch");
    Require(scratchbird::core::hash::HexLower(pack.content_sha256) == descriptor.content_sha256,
            "policy pack summary content hash mismatch");
    Require(pack.create_time_only,
            "policy pack summary was not create-time-only");
    Require(!pack.post_create_filesystem_authority,
            "policy pack summary allowed post-create filesystem authority");
    Require(pack.origin_transaction_uuid.kind==UuidKind::transaction&&pack.origin_local_transaction_id==1,
            "policy pack summary missing create transaction materialization");
    Require(pack.generation==1&&pack.imported_schema_version==1,
            "policy pack summary missing MGA catalog commit requirement");
    Require(pack.declared_counts[8] == 58,
            "policy pack summary did not record all default policies");
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
  const auto expected_pack=uuid::ParseTypedUuid(UuidKind::object,descriptor.policy_pack_uuid);
  Require(expected_pack.ok()&&state.policy_seed_catalog.policy_pack_uuid == expected_pack.value.value,
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
  Require(state.policy_seed_catalog.default_policy_records == 58,
          "state did not report all default policy records");

  const auto rows = ReadCatalogRows(database_path, state.header.page_size);
  const auto records = DecodeTypedRecords(rows);
  RequirePolicyPackSummary(rows, descriptor);
  Require(CountPolicyClass(records, "policy_pack") == 1,
          "missing typed policy_pack record");
  Require(CountPolicyClass(records, "security_provider") == 12,
          "security provider policy rows were not imported");
  Require(CountPolicyClass(records, "policy_profile") == 14,
          "policy profile rows were not imported");
  Require(CountDefaultPoliciesFromPack(records) == 58,
          "default policy rows were not imported from the policy pack");
  Require(CountKindFromPack(records, catalog::CatalogRecordKind::role_account) == 5,
          "standard policy roles were not imported");
  Require(CountKindFromPack(records, catalog::CatalogRecordKind::group_account) == 6,
          "standard policy groups were not imported");
  Require(CountKindFromPack(records, catalog::CatalogRecordKind::grant_record, "privilege") == 10,
          "standard policy grants were not imported");
  Require(CountKindFromPack(records, catalog::CatalogRecordKind::grant_record, "group_membership") == 5,
          "standard policy group memberships were not imported");
  const auto inventory=db::AcquireStrongLocalTransactionInventorySnapshot(database_path.string());
  Require(inventory.ok(),"actual policy construction inventory unavailable");
  const auto creator=scratchbird::transaction::mga::LookupLocalTransaction(inventory.snapshot->inventory,
      scratchbird::transaction::mga::MakeLocalTransactionId(1));
  Require(creator.ok()&&scratchbird::transaction::mga::HasCommittedInventoryOutcome(creator.entry),
          "actual policy construction transaction is not committed");
  RequireImportedRecordsCreatedByTx1(records,creator.entry.identity.transaction_uuid);
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

void RunNativeReservedRoleIdentityCases() {
  const std::string original = "018f7a10-1280-7000-8000-000000000100";
  for (const std::string replacement : {
           "018f7a10-1280-7000-8000-000000000105",
           "018F7A10-1280-7000-8000-000000000105",
           "018F7A10-1280-7000-8000-000000000106"}) {
    const auto root = UniquePath("sb_policy_native_reserved_role");
    const auto database_path = root / "test.sbdb";
    struct Cleanup {
      std::filesystem::path root;
      ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } cleanup{root};
    std::filesystem::copy(SB_DEFAULT_POLICY_PACK_ROOT, root,
                          std::filesystem::copy_options::recursive);
    const auto read = [](const auto& path) {
      std::ifstream input(path, std::ios::binary);
      Require(static_cast<bool>(input), "reserved role fixture read failed");
      return std::string(std::istreambuf_iterator<char>(input), {});
    };
    const auto write = [](const auto& path, const std::string& content) {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output.write(content.data(), content.size());
      Require(static_cast<bool>(output), "reserved role fixture write failed");
    };
    const auto replace_all = [](std::string& text, const std::string& from, const std::string& to) {
      for (std::size_t at = 0; (at = text.find(from, at)) != text.npos; at += to.size())
        text.replace(at, from.size(), to);
    };
    const auto sha = [](const std::string& text) {
      // Policy-pack digest normalization is CRLF to LF; retain every other byte.
      std::string canonical;
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        canonical.push_back(text[i]);
      }
      const auto hash = scratchbird::core::hash::ComputeSha256Digest(
          reinterpret_cast<const scratchbird::core::platform::byte*>(canonical.data()), canonical.size());
      Require(hash.ok(), "reserved role fixture digest failed");
      return scratchbird::core::hash::HexLower(hash.digest);
    };
    auto manifest = read(root / "POLICY_PACK_MANIFEST.json");
    const std::regex entry(R"re("path"\s*:\s*"([^"]+)"\s*,\s*"sha256"\s*:\s*"([0-9a-f]{64})")re");
    std::string aggregate;
    unsigned files = 0;
    // Iterate a stable original manifest; replacements below only update hashes.
    const auto original_manifest = manifest;
    for (auto it = std::sregex_iterator(original_manifest.begin(), original_manifest.end(), entry);
         it != std::sregex_iterator(); ++it) {
      const auto relative = (*it)[1].str();
      auto content = read(root / relative);
      replace_all(content, original, replacement);
      write(root / relative, content);
      const auto digest = sha(content);
      replace_all(manifest, (*it)[2].str(), digest);
      aggregate += relative; aggregate.push_back('\0'); aggregate += digest; aggregate.push_back('\n');
      ++files;
    }
    Require(files == 9, "reserved role fixture content manifest coverage changed");
    const std::regex aggregate_field(R"re("content_sha256"\s*:\s*"([0-9a-f]{64})")re");
    std::smatch aggregate_match;
    Require(std::regex_search(manifest, aggregate_match, aggregate_field), "aggregate digest field missing");
    manifest.replace(aggregate_match.position(1), aggregate_match.length(1), sha(aggregate));
    write(root / "POLICY_PACK_MANIFEST.json", manifest);
    auto create = BaseCreateConfig(database_path, CurrentUnixMillis());
    create.policy_seed_pack_root = root.string();
    const auto result = db::CreateDatabaseFile(create);
    if (replacement.ends_with("106")) {
      Require(result.ok() && result.create_finality == db::DatabaseCreateFinalityClass::committed,
              "valid nonreserved uppercase UUID was refused");
    } else {
      Require(!result.ok() && result.diagnostic.diagnostic_code == "SB-DB-BOOTSTRAP-SECURITY-SYSARCH-DUPLICATE" &&
              result.create_finality == db::DatabaseCreateFinalityClass::not_published &&
              !std::filesystem::exists(database_path),
              "reserved binary role identity bypassed prepublication duplicate protection");
    }
  }
}

template<class Edit> void RewriteAndResealPack(const std::filesystem::path& root,Edit edit) {
  const auto read=[](const auto& path){std::ifstream input(path,std::ios::binary);
    Require(bool(input),"identity fixture read failed");return std::string(std::istreambuf_iterator<char>(input),{});};
  const auto write=[](const auto& path,const std::string& value){std::ofstream output(path,std::ios::binary|std::ios::trunc);
    output.write(value.data(),value.size());Require(bool(output),"identity fixture write failed");};
  const auto replace=[](std::string& value,const std::string& from,const std::string& to) {
    for(std::size_t at=0;(at=value.find(from,at))!=value.npos;at+=to.size())value.replace(at,from.size(),to);
  };
  const auto sha=[](const std::string& value){std::string normalized;
    for(std::size_t i=0;i<value.size();++i){if(value[i]=='\r'&&i+1<value.size()&&value[i+1]=='\n')continue;normalized+=value[i];}
    const auto result=scratchbird::core::hash::ComputeSha256Digest(
        reinterpret_cast<const scratchbird::core::platform::byte*>(normalized.data()),normalized.size());
    Require(result.ok(),"identity fixture hash failed");return scratchbird::core::hash::HexLower(result.digest);
  };
  auto manifest=read(root/"POLICY_PACK_MANIFEST.json");
  edit(std::string("POLICY_PACK_MANIFEST.json"),manifest,replace);
  const auto original=manifest;
  const std::regex entry(R"re("path"\s*:\s*"([^"]+)"\s*,\s*"sha256"\s*:\s*"([0-9a-f]{64})")re");
  std::string aggregate;unsigned files=0;
  for(auto it=std::sregex_iterator(original.begin(),original.end(),entry);it!=std::sregex_iterator();++it) {
    const auto relative=(*it)[1].str();auto value=read(root/relative);edit(relative,value,replace);write(root/relative,value);
    const auto digest=sha(value);replace(manifest,(*it)[2].str(),digest);
    aggregate+=relative;aggregate.push_back('\0');aggregate+=digest;aggregate.push_back('\n');++files;
  }
  Require(files==9,"identity fixture retains complete manifest");
  const std::regex field(R"re("content_sha256"\s*:\s*"([0-9a-f]{64})")re");std::smatch match;
  Require(std::regex_search(manifest,match,field),"identity fixture aggregate field missing");
  manifest.replace(match.position(1),match.length(1),sha(aggregate));write(root/"POLICY_PACK_MANIFEST.json",manifest);
}

void RunBinaryIdentityBindingCases() {
  for(unsigned scenario=0;scenario<11;++scenario) {
    const auto root=UniquePath("policy_binary_identity_"+std::to_string(scenario));
    std::filesystem::copy(SB_DEFAULT_POLICY_PACK_ROOT,root,std::filesystem::copy_options::recursive);
    RewriteAndResealPack(root,[&](const auto& path,std::string& value,const auto& replace) {
      if(scenario==0&&(path=="policies/groups.json"||path=="policies/grants.json"))
        replace(value,"\"parent_uuid\": \"018f7a10-1280-7000-8000-000000000100\"",
                      "\"parent_uuid\": \"018F7A10-1280-7000-8000-000000000100\"");
      if(scenario==0&&path=="policies/grants.json")
        replace(value,"\"subject_uuid\": \"018f7a10-1280-7000-8000-000000000100\"",
                      "\"subject_uuid\": \"018F7A10-1280-7000-8000-000000000100\"");
      if(scenario==1&&path=="policies/security_providers.json")
        replace(value,"018f7a10-1280-7000-8000-000000000011","018F7A10-1280-7000-8000-000000000010");
      if(scenario==2)replace(value,"018f7a10-1280-7000-8000-000000000200","018F7A10-1280-7000-8000-000000000010");
      if(scenario==3&&path=="policies/roles.json")
        replace(value,"018f7a10-1280-7000-8000-000000000101","018F7A10-1280-7000-8000-000000000100");
      if(scenario>=4&&scenario<=6&&path=="policies/grants.json") {
        const std::string malformed=scenario==4?"42":scenario==5?"{}":"\"\", \"target_uuid\": \"\"";
        replace(value,"\"target_uuid\": \"\"","\"target_uuid\": "+malformed);
      }
      if(scenario>=7&&path=="policies/security_providers.json") {
        if(scenario==10)return;
        const std::string malformed=scenario==7?"42":scenario==8?"{}":"\"\", \"credential_verifier_policy\": \"\"";
        replace(value,"\"external_provider\": true","\"credential_verifier_policy\": "+malformed+", \"external_provider\": true");
      }
      if(scenario==10&&path=="POLICY_PACK_MANIFEST.json")
        replace(value,"018f7a10-1280-7000-8000-000000000001","018F7A10-1280-7000-8000-000000000001");
    });
    const auto path=root/"node.sbdb";auto config=BaseCreateConfig(path,CurrentUnixMillis());config.policy_seed_pack_root=root.string();
    const auto result=db::CreateDatabaseFile(config);
    if(scenario==0||scenario==10) {
      Require(result.ok()&&result.create_finality==db::DatabaseCreateFinalityClass::committed,
              "independent reference spelling changed binary relationship identity");
      db::DatabaseOpenConfig open;open.path=path.string();open.read_only=true;
      const auto reopened=db::OpenDatabaseFile(open);Require(reopened.ok(),"binary alias relationship did not reopen");
      const auto records=DecodeTypedRecords(ReadCatalogRows(path,reopened.state.header.page_size));
      Require(CountKindFromPack(records,catalog::CatalogRecordKind::grant_record,"group_membership")==5&&
          CountKindFromPack(records,catalog::CatalogRecordKind::grant_record,"privilege")==10,
          "binary alias binding lost actual membership/grant rows");
      const auto expected=uuid::ParseTypedUuid(UuidKind::object,"018f7a10-1280-7000-8000-000000000100");
      Require(expected.ok(),"binary oracle identity invalid");unsigned memberships=0,grants=0;
      for(const auto& record:records)if(record.record.header.kind==catalog::CatalogRecordKind::grant_record) {
        const auto security=catalog::DecodeCatalogSecurityRecord(record.record.header.kind,record.record.payload);
        Require(security.ok(),"actual binary grant payload invalid");
        memberships+=security.record->Identity("parent_uuid")==expected.value.value;
        grants+=security.record->Identity("subject_uuid")==expected.value.value;
      }
      Require(memberships==1&&grants==1,"reference-only casing did not preserve exact raw16 parent/subject identity");
    }else {
      const char* expected[]{"","SB-POLICY-PACK-PROVIDERS-INVALID","SB-POLICY-PACK-GROUPS-INVALID","SB-POLICY-PACK-ROLES-INVALID",
          "SB-POLICY-PACK-GRANTS-INVALID","SB-POLICY-PACK-GRANTS-INVALID","SB-POLICY-PACK-GRANTS-INVALID",
          "SB-POLICY-PACK-PROVIDERS-INVALID","SB-POLICY-PACK-PROVIDERS-INVALID","SB-POLICY-PACK-PROVIDERS-INVALID"};
      Require(!result.ok()&&result.diagnostic.diagnostic_code==expected[scenario]&&
          result.create_finality==db::DatabaseCreateFinalityClass::not_published&&!std::filesystem::exists(path),
          "same raw16 identity reused within/across imported classes reached physical creation");
    }
  }
}

void RunNativePolicyReopenCorruptionCases() {
  for (unsigned scenario=0; scenario<10; ++scenario) {
    const auto path=UniquePath("policy_reopen_corruption_"+std::to_string(scenario)+".sbdb");
    auto config=BaseCreateConfig(path,CurrentUnixMillis());
    const auto created=db::CreateDatabaseFile(config);
    Require(created.ok(),"reopen corruption fixture creation failed");
    auto rows=ReadCatalogRows(path,config.page_size);
    std::optional<page::CatalogPageRow> previous_provider;
    bool changed=false;
    for (auto& row:rows) {
      if(scenario>=3) {
        catalog::CatalogTypedRecord typed;
        std::string_view payload=row.payload;
        if(row.kind==page::CatalogPageRowKind::typed_catalog_record) {
          const auto decoded=catalog::DecodeCatalogTypedRecord(row);
          Require(decoded.ok(),"pack corruption source framing invalid");
          typed=decoded.record;payload=typed.payload;
        }else if(row.kind!=page::CatalogPageRowKind::policy_seed_pack)continue;
        auto pack=catalog::DecodeCatalogBootstrapPolicyPack(payload);
        if(!pack.ok())continue;
        if(scenario==3&&row.kind!=page::CatalogPageRowKind::policy_seed_pack)continue;
        if(scenario==3)pack.record->pack_version="1.0.9";
        if(scenario==4) {
          const auto foreign=uuid::ParseTypedUuid(UuidKind::transaction,"018f7a10-1280-7000-8000-000000000999");
          Require(foreign.ok(),"foreign pack origin fixture invalid");pack.record->origin_transaction_uuid=foreign.value;
        }
        if(scenario==5)pack.record->declared_counts[8]=0;
        if(scenario>=6)++pack.record->declared_counts[scenario-3];
        std::array<scratchbird::core::platform::byte,2048> backing{};
        const auto encoded=catalog::EncodeCatalogBootstrapPolicyPackInto(*pack.record,backing);
        Require(encoded.ok(),"pack corruption is not structurally valid");
        if(row.kind==page::CatalogPageRowKind::policy_seed_pack)row.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
        else {
          typed.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
          const auto wrapped=catalog::EncodeCatalogTypedRecord(typed,row.ordinal);
          Require(wrapped.ok(),"corrupted pack common framing failed");row=wrapped.row;
        }
        changed=true;continue;
      }
      if(row.kind!=page::CatalogPageRowKind::typed_catalog_record)continue;
      auto decoded=catalog::DecodeCatalogTypedRecord(row);
      Require(decoded.ok(),"reopen corruption source framing invalid");
      auto provider=catalog::DecodeCatalogBootstrapSecurityProvider(decoded.record.payload);
      if(!provider.ok())continue;
      if(scenario==0) {
        const auto foreign=uuid::ParseTypedUuid(UuidKind::transaction,"018f7a10-1280-7000-8000-000000000999");
        Require(foreign.ok(),"foreign origin fixture invalid");
        provider.record->origin_transaction_uuid=foreign.value;
        std::array<scratchbird::core::platform::byte,2048> backing{};
        const auto encoded=catalog::EncodeCatalogBootstrapSecurityProviderInto(*provider.record,backing);
        Require(encoded.ok(),"foreign origin is not structurally valid");
        decoded.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
        const auto wrapped=catalog::EncodeCatalogTypedRecord(decoded.record,row.ordinal);
        Require(wrapped.ok(),"foreign origin common framing failed");row=wrapped.row;
        changed=true;break;
      }
      if(!provider.record->external_provider)continue;
      if(!previous_provider){previous_provider=row;continue;}
      if(scenario==1) {
        const auto ordinal=row.ordinal;row=*previous_provider;row.ordinal=ordinal;
      } else {
        const auto prior=catalog::DecodeCatalogTypedRecord(*previous_provider);
        Require(prior.ok(),"duplicate row source invalid");
        decoded.record.header.row_uuid=prior.record.header.row_uuid;
        const auto wrapped=catalog::EncodeCatalogTypedRecord(decoded.record,row.ordinal);
        Require(wrapped.ok(),"duplicate row common framing failed");row=wrapped.row;
      }
      changed=true;break;
    }
    Require(changed,"native corruption did not change intended definition");
    const auto pages=page::BuildCatalogPageSet(rows,config.page_size,db::kCatalogPageNumber,db::kCatalogOverflowFirstPageNumber);
    Require(pages.ok(),"corrupted definitions could not be validly checksummed");
    {
      disk::FileDevice device;Require(device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"corruption writer open failed");
      for(const auto& image:pages.pages) {
        const auto offset=page::CheckedPageBodyOffset(config.page_size,image.page_number,disk::kPageHeaderSerializedBytes);
        Require(offset.ok()&&device.WriteAt(offset.offset,image.body.data(),image.body.size()).ok(),"corruption body write failed");
      }
      Require(device.Sync().ok(),"corruption fixture sync failed");
    }
    // First verify transport/framing integrity independently of admission.
    Require(ReadCatalogRows(path,config.page_size).size()==rows.size(),"corruption failed page integrity before admission");
    db::DatabaseOpenConfig open;open.path=path.string();open.read_only=true;
    const auto result=db::OpenDatabaseFile(open);
    Require(!result.ok()&&result.diagnostic.diagnostic_code=="SB-POLICY-PACK-CATALOG-INVALID",
            "foreign native origin or duplicate raw16 identity survived reopen");
  }
}

}  // namespace

int main() try {
  ConfigureMemoryFixture();
  RunSuccessfulImportCase();
  RunHashMismatchCase();
  RunNativeReservedRoleIdentityCases();
  RunBinaryIdentityBindingCases();
  RunNativePolicyReopenCorruptionCases();
  TestRoot().Cleanup();
  return EXIT_SUCCESS;
}catch(const std::exception& error) {
  std::cerr<<error.what()<<'\n';
  try{TestRoot().Cleanup();}catch(const std::exception& cleanup){std::cerr<<cleanup.what()<<'\n';}
  return EXIT_FAILURE;
}
