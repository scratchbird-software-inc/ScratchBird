// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/diagnostic_value_fixture.hpp"

#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_security_record_codec.hpp"
#include "database_lifecycle.hpp"
#include "bootstrap_security_validation.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include "memory.hpp"
#include "page_manager.hpp"
#include "uuid.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
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
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr const char* kCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=89abcdef0123456789abcdef01234567:"
    "verifier=abcdef0123456789abcdef0123456789"
    "abcdef0123456789abcdef0123456789";

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

u64 UniqueMillis() {
  static u64 sequence = 0;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return static_cast<u64>(now) + (++sequence * 1000);
}

std::filesystem::path TestPath(std::string_view label) {
  return std::filesystem::temp_directory_path() /
         ("sb_bootstrap_security_" + std::string(label) + "_" +
          std::to_string(UniqueMillis()) + ".sbdb");
}

struct Cleanup {
  std::vector<std::filesystem::path> paths;

  ~Cleanup() {
    for (const auto& path : paths) {
      for (const char* suffix : {"",
                                 ".sb.owner.lock",
                                 ".sb.txn_publish",
                                 ".sb.txn_publish.tmp",
                                 ".sb.security_principal_events",
                                 ".sb.local_password_auth",
                                 ".sb.security_context_generation_v1.migration_tmp",
                                 ".sb.security_context_generation_v1.migration_tmp.sb.owner.lock"}) {
        std::error_code ignored;
        std::filesystem::remove(path.string() + suffix, ignored);
      }
    }
  }
};

db::DatabaseCreateConfig MakeConfig(const std::filesystem::path& path,
                                    std::string fault = {}) {
  const u64 now = UniqueMillis();
  const auto database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "bootstrap security test UUID generation failed");
  db::DatabaseCreateConfig config;
  config.path = path.string();
  config.database_uuid = database_uuid.value;
  config.filespace_uuid = filespace_uuid.value;
  config.creation_unix_epoch_millis = now;
  config.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  config.require_resource_seed_pack = true;
  config.bootstrap_principal_name = "ROOT";
  config.bootstrap_credential_fingerprint = kCredentialFingerprint;
  config.require_bootstrap_principal = true;
  config.allow_uncredentialed_bootstrap = false;
  config.create_fault_injection_point = std::move(fault);
  config.allow_overwrite = false;
  return config;
}

void RequireNoSecuritySidecars(const std::filesystem::path& path) {
  Require(!std::filesystem::exists(path.string() +
                                   ".sb.security_principal_events"),
          "security principal sidecar exists");
  Require(!std::filesystem::exists(path.string() +
                                   ".sb.local_password_auth"),
          "local password auth sidecar exists");
}

db::DatabaseLifecycleResult CreateCommitted(Cleanup* cleanup,
                                            std::string_view label,
                                            std::filesystem::path* path_out = nullptr) {
  const auto path = TestPath(label);
  cleanup->paths.push_back(path);
  const auto created = db::CreateDatabaseFile(MakeConfig(path));
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
  }
  Require(created.ok(), "committed bootstrap database create failed");
  Require(created.create_finality ==
              db::DatabaseCreateFinalityClass::committed,
          "normal bootstrap create was not classified committed");
  if (path_out != nullptr) {
    *path_out = path;
  }
  return created;
}

// Human-readable fixture inputs only: production records remain native binary.
std::map<std::string, std::string> FixtureFields(const catalog::CatalogTypedRecord& record) {
  const auto decoded = catalog::DecodeCatalogSecurityRecord(record.header.kind, record.payload);
  Require(decoded.ok(), "security fixture payload decode failed");
  auto fields = decoded.record->attributes;
  for (const auto& [name, identity] : decoded.record->identities)
    fields.emplace(name, uuid::UuidToString(identity));
  return fields;
}

std::string SerializeFields(
    catalog::CatalogRecordKind kind,
    const std::map<std::string, std::string>& fields) {
  catalog::CatalogSecurityRecord record;
  record.kind = kind;
  for (const auto& [name, value] : fields) {
    if (name.ends_with("_uuid")) {
      const auto parsed = uuid::ParseTypedUuid(UuidKind::object, value);
      Require(parsed.ok(), "security fixture identity parse failed");
      record.identities.emplace(name, parsed.value.value);
    } else {
      record.attributes.emplace(name, value);
    }
  }
  const auto encoded = catalog::EncodeCatalogSecurityRecord(record);
  Require(encoded.ok(), "security fixture payload encode failed");
  return std::string(encoded.bytes.begin(), encoded.bytes.end());
}

std::vector<page::CatalogPageRow> ReadCatalogRows(
    const std::filesystem::path& path,
    std::uint32_t page_size) {
  disk::FileDevice device;
  const auto opened =
      device.Open(path.string(), disk::FileOpenMode::open_existing_read_only);
  Require(opened.ok(), "tamper catalog open failed");
  std::vector<page::CatalogPageRow> rows;
  std::set<u64> visited;
  u64 page_number = db::kCatalogPageNumber;
  while (page_number != 0) {
    Require(visited.insert(page_number).second,
            "tamper catalog chain cycle");
    const auto offset = page::CheckedPageBodyOffset(
        page_size, page_number, disk::kPageHeaderSerializedBytes);
    Require(offset.ok(), "tamper catalog body offset failed");
    std::vector<scratchbird::core::platform::byte> body(
        page_size - disk::kPageHeaderSerializedBytes, 0);
    const auto read = device.ReadAt(offset.offset, body.data(), body.size());
    Require(read.ok(), "tamper catalog read failed");
    const auto parsed = page::ParseCatalogPageBody(body, page_number);
    Require(parsed.ok(), "tamper catalog parse failed");
    rows.insert(rows.end(), parsed.body.rows.begin(), parsed.body.rows.end());
    page_number = parsed.body.next_page_number;
  }
  return rows;
}

void WriteCatalogRows(const std::filesystem::path& path,
                      std::uint32_t page_size,
                      const std::vector<page::CatalogPageRow>& rows) {
  const auto page_set = page::BuildCatalogPageSet(rows,
                                                  page_size,
                                                  db::kCatalogPageNumber,
                                                  db::kCatalogOverflowFirstPageNumber);
  Require(page_set.ok(), "tamper catalog rebuild failed");
  disk::FileDevice device;
  const auto opened =
      device.Open(path.string(), disk::FileOpenMode::open_existing);
  Require(opened.ok(), "tamper catalog write open failed");
  for (const auto& catalog_page : page_set.pages) {
    const auto offset = page::CheckedPageBodyOffset(
        page_size,
        catalog_page.page_number,
        disk::kPageHeaderSerializedBytes);
    Require(offset.ok(), "tamper catalog write offset failed");
    const auto written = device.WriteAt(offset.offset,
                                        catalog_page.body.data(),
                                        catalog_page.body.size());
    Require(written.ok(), "tamper catalog write failed");
  }
  Require(device.Sync().ok(), "tamper catalog sync failed");
  Require(device.Close().ok(), "tamper catalog close failed");
}

using RecordPredicate = std::function<bool(
    const catalog::CatalogTypedRecord&,
    const std::map<std::string, std::string>&)>;
using RecordMutator = std::function<void(
    catalog::CatalogTypedRecord*,
    std::map<std::string, std::string>*)>;

void MutateOneTypedRecord(const std::filesystem::path& path,
                          std::uint32_t page_size,
                          const RecordPredicate& predicate,
                          const RecordMutator& mutator) {
  auto rows = ReadCatalogRows(path, page_size);
  bool mutated = false;
  for (auto& row : rows) {
    if (row.kind != page::CatalogPageRowKind::typed_catalog_record) {
      continue;
    }
    const auto decoded = catalog::DecodeCatalogTypedRecord(row);
    Require(decoded.ok(), "tamper typed record decode failed");
    if (!catalog::IsCatalogSecurityRecordKind(decoded.record.header.kind)) continue;
    auto fields = FixtureFields(decoded.record);
    if (!mutated && predicate(decoded.record, fields)) {
      auto record = decoded.record;
      mutator(&record, &fields);
      record.payload = SerializeFields(record.header.kind, fields);
      // Construct valid framing first, then inject the requested header/payload
      // mismatch below. Production encoding must never admit that mismatch.
      const auto requested_object = record.header.object_uuid;
      const auto security = catalog::DecodeCatalogSecurityRecord(record.header.kind, record.payload);
      Require(security.ok(), "mutated security fixture decode failed");
      record.header.object_uuid.value = security.record->Identity(
          catalog::CatalogSecurityPrimaryIdentityName(record.header.kind));
      const auto encoded = catalog::EncodeCatalogTypedRecord(record,
                                                              row.ordinal);
      Require(encoded.ok(), "tamper typed record encode failed");
      row = encoded.row;
      Require(row.payload.size() >= 96, "typed fixture header missing");
      std::copy(requested_object.value.bytes.begin(), requested_object.value.bytes.end(),
                row.payload.begin() + 56);
      mutated = true;
    }
  }
  Require(mutated, "tamper target record not found");
  WriteCatalogRows(path, page_size, rows);
}

void RequireReaderRejects(const std::filesystem::path& path,
                          std::string_view message) {
  const auto read = db::ReadDatabaseBootstrapSecurityCatalog(path.string());
  if (read.ok()) {
    std::cerr << "unexpected reader success for " << path << '\n';
  }
  Require(!read.ok(), message);
}

void TestPrepublicationFaults(Cleanup* cleanup) {
  static constexpr std::array<const char*, 5> faults = {{
      "before_catalog",
      "after_catalog_before_prepublish_sync",
      "after_prepublish_sync_before_inventory_publish",
      "inventory_publish_write",
      "inventory_publish_sync",
  }};
  for (const char* fault : faults) {
    const auto path = TestPath(fault);
    cleanup->paths.push_back(path);
    const auto created = db::CreateDatabaseFile(MakeConfig(path, fault));
    Require(!created.ok(), "prepublication fault reported success");
    Require(created.create_finality ==
                db::DatabaseCreateFinalityClass::not_published,
            "prepublication fault returned published finality");
    Require(!std::filesystem::exists(path),
            "prepublication fault left an admitted database file");
  }
}

void TestPostpublicationFaults(Cleanup* cleanup) {
  static constexpr std::array<const char*, 2> faults = {{
      "subordinate_publish_journal",
      "after_inventory_durable_before_readback",
  }};
  for (const char* fault : faults) {
    const auto path = TestPath(fault);
    cleanup->paths.push_back(path);
    const auto created = db::CreateDatabaseFile(MakeConfig(path, fault));
    Require(created.ok(), "postpublication fault reported retryable failure");
    Require(created.create_finality ==
                db::DatabaseCreateFinalityClass::committed_with_warning,
            "postpublication fault was not committed-with-warning");
    const auto read =
        db::ReadDatabaseBootstrapSecurityCatalog(path.string());
    Require(read.ok() && read.state.present &&
                read.state.committed_by_inventory,
            "postpublication fault lost committed bootstrap catalog");
    db::DatabaseOpenConfig open;
    open.path = path.string();
    open.read_only = true;
    Require(db::OpenDatabaseFile(open).ok(),
            "postpublication fault did not restart cleanly");
    RequireNoSecuritySidecars(path);
  }
}

void TestReplayRefused(Cleanup* cleanup) {
  std::filesystem::path path;
  const auto created = CreateCommitted(cleanup, "replay", &path);
  auto replay_config = MakeConfig(path);
  replay_config.allow_overwrite = true;
  const auto replay = db::CreateDatabaseFile(replay_config);
  Require(!replay.ok(), "existing database create replay was admitted");
  Require(replay.create_finality ==
              db::DatabaseCreateFinalityClass::not_published,
          "existing database replay returned published finality");
  const auto read = db::ReadDatabaseBootstrapSecurityCatalog(path.string());
  Require(read.ok() && read.state.present,
          "replay attempt damaged original database");
  (void)created;
}

void TestCredentialEnvelopeRefusedAndRedacted(Cleanup* cleanup) {
  const auto path = TestPath("credential_refusal");
  cleanup->paths.push_back(path);
  auto config = MakeConfig(path);
  const std::string secret =
      "local-password-pbkdf2-sha256:v1:iterations=210000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef";
  config.bootstrap_credential_fingerprint = secret;
  const auto refused = db::CreateDatabaseFile(config);
  Require(!refused.ok(), "weak PBKDF2 envelope was admitted");
  Require(refused.diagnostic.diagnostic_code.find("0123456789abcdef") ==
              std::string::npos &&
              refused.diagnostic.message_key.find("0123456789abcdef") ==
                  std::string::npos,
          "credential material leaked through public diagnostic fields");
  for (const auto& argument : refused.diagnostic.arguments) {
    Require(scratchbird::tests::DiagnosticValueBytes(argument.value).find("0123456789abcdef") == std::string::npos,
            "credential material leaked through diagnostic arguments");
  }
  Require(!std::filesystem::exists(path),
          "invalid credential envelope created a database file");
}

void TestSemanticTamperRejection(Cleanup* cleanup) {
  {
    std::filesystem::path path;
    const auto created = CreateCommitted(cleanup, "fingerprint_tamper", &path);
    MutateOneTypedRecord(
        path,
        created.state.header.page_size,
        [](const auto& record, const auto& fields) {
          return record.header.kind == catalog::CatalogRecordKind::user_account &&
                 fields.count("bootstrap_principal") != 0;
        },
        [](auto*, auto* fields) {
          (*fields)["credential_fingerprint"] =
              "local-password-verifier:v1:sha256:"
              "0123456789abcdef0123456789abcdef"
              "0123456789abcdef0123456789abcdef";
        });
    RequireReaderRejects(path, "legacy credential tamper was admitted");
  }
  {
    std::filesystem::path path;
    const auto created = CreateCommitted(cleanup, "role_uuid_tamper", &path);
    MutateOneTypedRecord(
        path,
        created.state.header.page_size,
        [](const auto& record, const auto& fields) {
          return record.header.kind == catalog::CatalogRecordKind::role_account &&
                 fields.count("role_code") != 0 &&
                 fields.at("role_code") == "ROLE_SYSARCH";
        },
        [](auto*, auto* fields) {
          (*fields)["role_uuid"] =
              "018f7a10-1280-7000-8000-000000000106";
        });
    RequireReaderRejects(path, "wrong SYSARCH role UUID was admitted");
  }
  {
    std::filesystem::path path;
    const auto created = CreateCommitted(cleanup, "provenance_tamper", &path);
    MutateOneTypedRecord(
        path,
        created.state.header.page_size,
        [](const auto& record, const auto& fields) {
          return record.header.kind == catalog::CatalogRecordKind::role_account &&
                 fields.count("role_code") != 0 &&
                 fields.at("role_code") == "ROLE_SYSARCH";
        },
        [](auto*, auto* fields) {
          (*fields)["authority_class"] = "mutable_named_role";
        });
    RequireReaderRejects(path, "SYSARCH provenance tamper was admitted");
  }
  {
    std::filesystem::path path;
    const auto created = CreateCommitted(cleanup, "membership_principal_tamper", &path);
    MutateOneTypedRecord(
        path,
        created.state.header.page_size,
        [](const auto& record, const auto& fields) {
          return record.header.kind == catalog::CatalogRecordKind::grant_record &&
                 fields.count("grant_class") != 0 &&
                 fields.at("grant_class") == "role_membership" &&
                 fields.count("role_uuid") != 0 &&
                 fields.at("role_uuid") == db::kCanonicalSysarchRoleObjectUuid;
        },
        [](auto*, auto* fields) {
          (*fields)["member_uuid"] =
              "018f7a10-1280-7000-8000-000000000199";
          (*fields)["principal_uuid"] =
              "018f7a10-1280-7000-8000-000000000199";
        });
    RequireReaderRejects(path,
                         "membership principal UUID tamper was admitted");
  }
  {
    std::filesystem::path path;
    const auto created = CreateCommitted(cleanup, "membership_role_tamper", &path);
    MutateOneTypedRecord(
        path,
        created.state.header.page_size,
        [](const auto& record, const auto& fields) {
          return record.header.kind == catalog::CatalogRecordKind::grant_record &&
                 fields.count("grant_class") != 0 &&
                 fields.at("grant_class") == "role_membership" &&
                 fields.count("role_uuid") != 0 &&
                 fields.at("role_uuid") == db::kCanonicalSysarchRoleObjectUuid;
        },
        [](auto*, auto* fields) {
          (*fields)["role_uuid"] =
              "018f7a10-1280-7000-8000-000000000106";
          (*fields)["parent_uuid"] =
              "018f7a10-1280-7000-8000-000000000106";
        });
    RequireReaderRejects(path,
                         "membership role UUID tamper was admitted");
  }
  {
    std::filesystem::path path;
    const auto created = CreateCommitted(cleanup, "duplicate_sysarch", &path);
    const auto canonical = uuid::ParseTypedUuid(
        UuidKind::object, db::kCanonicalSysarchRoleObjectUuid);
    Require(canonical.ok(), "canonical SYSARCH UUID did not parse");
    MutateOneTypedRecord(
        path,
        created.state.header.page_size,
        [](const auto& record, const auto& fields) {
          return record.header.kind == catalog::CatalogRecordKind::role_account &&
                 fields.count("role_code") != 0 &&
                 fields.at("role_code") != "ROLE_SYSARCH";
        },
        [&](auto* record, auto* fields) {
          record->header.object_uuid = canonical.value;
          fields->clear();
          (*fields)["role_uuid"] = db::kCanonicalSysarchRoleObjectUuid;
          (*fields)["role_code"] = "ROLE_SYSARCH";
          (*fields)["authority_class"] = "engine_owned_sysarch";
          (*fields)["engine_owned"] = "1";
          (*fields)["identity_authority"] = "uuid";
          (*fields)["immutable"] = "1";
          (*fields)["create_time_only"] = "1";
          (*fields)["creator_tx"] = "1";
          (*fields)["policy_generation"] = "1";
          (*fields)["security_context_authority_version"] = "1";
          (*fields)["security_context_generation"] = "1";
          (*fields)["active"] = "1";
        });
    RequireReaderRejects(path, "duplicate canonical SYSARCH role was admitted");
  }
}

void TestForbiddenSecuritySidecars(Cleanup* cleanup) {
  std::filesystem::path path;
  (void)CreateCommitted(cleanup, "forbidden_sidecars", &path);
  {
    std::ofstream sidecar(path.string() + ".sb.security_principal_events",
                          std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(sidecar),
            "could not create forbidden principal sidecar fixture");
    sidecar << "forbidden\n";
  }
  RequireReaderRejects(path, "principal-event sidecar was admitted");
  {
    std::error_code ignored;
    std::filesystem::remove(path.string() +
                            ".sb.security_principal_events",
                            ignored);
  }
  {
    std::ofstream sidecar(path.string() + ".sb.local_password_auth",
                          std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(sidecar),
            "could not create forbidden local-auth sidecar fixture");
    sidecar << "forbidden\n";
  }
  RequireReaderRejects(path, "local-password sidecar was admitted");
}

// Catalog identity and generations, not row placement, determine bootstrap
// authority. Exercise every order of the three actual published owners.
void CheckSemanticValidation(
    const std::vector<catalog::CatalogTypedRecord>& records,
    const db::DatabaseBootstrapSecurityCatalogState& expected) {
  const auto valid = db::ValidateBootstrapSecurityRecords(records, "semantic-fixture");
  Require(valid.ok() && valid.state.present && !valid.state.committed_by_inventory &&
              valid.state.principal_uuid.value == expected.principal_uuid.value &&
              valid.state.sysarch_role_uuid.value == db::kCanonicalSysarchRoleIdentity &&
              valid.state.membership_uuid.value == expected.membership_uuid.value &&
              valid.state.credential_fingerprint == expected.credential_fingerprint &&
              valid.state.creator_tx == 1 &&
              valid.state.policy_generation == expected.policy_generation &&
              valid.state.security_context_generation == expected.security_context_generation,
          "semantic validation changed fields or asserted storage commitment");
  auto refused = [](const auto& candidate, std::string_view code) {
    const auto result = db::ValidateBootstrapSecurityRecords(candidate, "semantic-fixture");
    Require(!result.ok() && result.diagnostic.diagnostic_code == code &&
                !result.state.present && !result.state.committed_by_inventory &&
                result.state.principal_uuid.value.is_nil() &&
                result.state.sysarch_role_uuid.value.is_nil(),
            "semantic refusal lost its diagnostic or exposed partial authority");
    const std::map<std::string_view, std::string_view> keys{
        {"SB-DB-BOOTSTRAP-SECURITY-SYSARCH-PROVENANCE-INVALID", "storage.database_lifecycle.bootstrap_security_sysarch_provenance_invalid"},
        {"SB-DB-BOOTSTRAP-SECURITY-PRINCIPAL-INVALID", "storage.database_lifecycle.bootstrap_security_principal_invalid"},
        {"SB-DB-BOOTSTRAP-SECURITY-MEMBERSHIP-INVALID", "storage.database_lifecycle.bootstrap_security_membership_invalid"},
        {"SB-DB-BOOTSTRAP-SECURITY-GENERATION-MISMATCH", "storage.database_lifecycle.bootstrap_security_generation_mismatch"},
        {"SB-DB-BOOTSTRAP-SECURITY-SYSARCH-CARDINALITY-INVALID", "storage.database_lifecycle.bootstrap_security_sysarch_cardinality_invalid"},
        {"SB-DB-BOOTSTRAP-SECURITY-CARDINALITY-INVALID", "storage.database_lifecycle.bootstrap_security_cardinality_invalid"},
        {"SB-DB-BOOTSTRAP-SECURITY-PRINCIPAL-NAME-DUPLICATE", "storage.database_lifecycle.bootstrap_security_principal_name_duplicate"},
        {"CATALOG.INVALID_INPUT", "catalog.security_record.invalid"}};
    Require(result.diagnostic.message_key == keys.at(code), "semantic diagnostic message key drift");
    bool has_path = false;
    for (const auto& argument : result.diagnostic.arguments) {
      const auto bytes = scratchbird::tests::DiagnosticValueBytes(argument.value);
      has_path = has_path || bytes == "semantic-fixture";
      Require(bytes.find("local-password-") == std::string::npos &&
                  bytes.find("89abcdef0123456789abcdef01234567") == std::string::npos &&
                  bytes.find("not-a-credential") == std::string::npos,
              "semantic diagnostic exposed credential data");
    }
    Require(code == "CATALOG.INVALID_INPUT" || has_path,
            "semantic diagnostic lost its source label");
  };
  auto change = [](auto& record, const char* field, const std::string& value) {
    auto security = catalog::DecodeCatalogSecurityRecord(record.header.kind, record.payload);
    Require(security.ok(), "semantic mutation decode failed");
    security.record->attributes[field] = value;
    const auto encoded = catalog::EncodeCatalogSecurityRecord(*security.record);
    Require(encoded.ok(), "semantic mutation encode failed");
    record.payload.assign(encoded.bytes.begin(), encoded.bytes.end());
  };
  for (std::size_t i = 0; i < records.size(); ++i) {
    for (const char* field : {"policy_generation", "security_context_generation"}) {
      auto candidate = records;
      change(candidate[i], field, "2");
      refused(candidate, "SB-DB-BOOTSTRAP-SECURITY-GENERATION-MISMATCH");
    }
    const bool role = records[i].header.kind == catalog::CatalogRecordKind::role_account;
    const bool principal = records[i].header.kind == catalog::CatalogRecordKind::user_account;
    const std::string_view invalid = role ?
        "SB-DB-BOOTSTRAP-SECURITY-SYSARCH-PROVENANCE-INVALID" : principal ?
        "SB-DB-BOOTSTRAP-SECURITY-PRINCIPAL-INVALID" :
        "SB-DB-BOOTSTRAP-SECURITY-MEMBERSHIP-INVALID";
    for (const char* field : {"policy_generation", "security_context_generation"}) {
      for (const std::string bad : {"", "0", "-1", "+1", " 1", "1junk",
                                    "18446744073709551616", "999999999999999999999999999"}) {
        auto candidate = records;
        change(candidate[i], field, bad);
        refused(candidate, invalid);
      }
      auto candidate = records;
      change(candidate[i], field, std::string("1\0suffix", 8));
      refused(candidate, invalid);
    }
    for (const char* overflow : {"4294967296", "4294967297"}) {
      auto candidate = records;
      change(candidate[i], "policy_generation", overflow);
      refused(candidate, invalid);
    }
    const std::string_view cardinality = role ?
        "SB-DB-BOOTSTRAP-SECURITY-SYSARCH-CARDINALITY-INVALID" :
        "SB-DB-BOOTSTRAP-SECURITY-CARDINALITY-INVALID";
    auto candidate = records;
    candidate.erase(candidate.begin() + i);
    refused(candidate, cardinality);
    candidate = records;
    candidate.push_back(records[i]);
    refused(candidate, role ? cardinality : principal ?
        "SB-DB-BOOTSTRAP-SECURITY-PRINCIPAL-NAME-DUPLICATE" :
        "SB-DB-BOOTSTRAP-SECURITY-MEMBERSHIP-INVALID");
    candidate = records;
    candidate[i].header.deleted = true;
    refused(candidate, cardinality);
    candidate = records;
    candidate[i].header.object_uuid.value.bytes[15] ^= 0x40;
    refused(candidate, "CATALOG.INVALID_INPUT");
    candidate = records;
    change(candidate[i], "creator_tx", "2");
    refused(candidate, invalid);
    if (principal) {
      candidate = records;
      change(candidate[i], "credential_fingerprint", "not-a-credential");
      refused(candidate, "SB-DB-BOOTSTRAP-SECURITY-PRINCIPAL-INVALID");
    }
  }
  auto maxima = records;
  for (auto& record : maxima) {
    change(record, "policy_generation", "4294967295");
    change(record, "security_context_generation", "18446744073709551615");
  }
  const auto maximum = db::ValidateBootstrapSecurityRecords(maxima, "semantic-fixture");
  Require(maximum.ok() && !maximum.state.committed_by_inventory &&
              maximum.state.policy_generation == UINT32_MAX &&
              maximum.state.security_context_generation == UINT64_MAX,
          "exact maximum generation was lost or refused");
  std::vector<catalog::CatalogTypedRecord> role_only;
  for (const auto& record : records)
    if (record.header.kind == catalog::CatalogRecordKind::role_account) role_only.push_back(record);
  const auto uncredentialed = db::ValidateBootstrapSecurityRecords(role_only, "semantic-fixture");
  Require(uncredentialed.ok() && !uncredentialed.state.present &&
              !uncredentialed.state.committed_by_inventory &&
              uncredentialed.state.principal_uuid.value.is_nil() &&
              uncredentialed.state.sysarch_role_uuid.value == expected.sysarch_role_uuid.value,
          "role-only validation invented a principal or durable grant");
}

void TestBootstrapRecordOrder() {
  const auto file_image = [](const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    std::ifstream input(path, std::ios::binary);
    const auto digest = scratchbird::core::hash::ComputeSha256Stream(input, size);
    Require(digest.ok(), "record-order whole-file digest failed");
    return std::make_pair(size, digest.digest);
  };
  for (const std::uint32_t page_size : {8192u, 16384u, 32768u, 65536u, 131072u}) {
    Cleanup cleanup;
    const auto path = TestPath("record_order");
    cleanup.paths.push_back(path);
    auto config = MakeConfig(path);
    config.page_size = page_size;
    const auto created = db::CreateDatabaseFile(config);
    Require(created.ok(), "record-order database creation failed");
    const auto expected = db::ReadDatabaseBootstrapSecurityCatalog(path.string());
    Require(expected.ok() && expected.state.present && expected.state.committed_by_inventory,
            "record-order baseline missing");
    const auto original = ReadCatalogRows(path, page_size);
    std::vector<std::size_t> positions;
    for (std::size_t i = 0; i < original.size(); ++i) {
      if (original[i].kind != page::CatalogPageRowKind::typed_catalog_record) continue;
      const auto decoded = catalog::DecodeCatalogTypedRecord(original[i]);
      Require(decoded.ok(), "record-order fixture decode failed");
      const auto id = decoded.record.header.object_uuid.value;
      if (id == expected.state.sysarch_role_uuid.value ||
          id == expected.state.principal_uuid.value ||
          id == expected.state.membership_uuid.value) positions.push_back(i);
    }
    Require(positions.size() == 3, "record-order fixture did not select three owners");
    std::array<unsigned, 3> order{0, 1, 2};
    unsigned permutations = 0;
    do {
      auto rows = original;
      for (std::size_t i = 0; i < order.size(); ++i) {
        rows[positions[i]] = original[positions[order[i]]];
        rows[positions[i]].ordinal = original[positions[i]].ordinal;
      }
      std::vector<catalog::CatalogTypedRecord> records;
      for (const auto position : positions) {
        auto decoded = catalog::DecodeCatalogTypedRecord(rows[position]);
        Require(decoded.ok(), "permuted fixture decode failed");
        records.push_back(std::move(decoded.record));
      }
      CheckSemanticValidation(records, expected.state);
      WriteCatalogRows(path, page_size, rows);
      const auto actual = db::ReadDatabaseBootstrapSecurityCatalog(path.string());
      if (!actual.ok()) std::cerr << actual.diagnostic.diagnostic_code << '\n';
      Require(actual.ok() && actual.state.present && actual.state.committed_by_inventory,
              "valid bootstrap record permutation was refused");
      Require(actual.state.sysarch_role_uuid.value == expected.state.sysarch_role_uuid.value &&
                  actual.state.principal_uuid.value == expected.state.principal_uuid.value &&
                  actual.state.membership_uuid.value == expected.state.membership_uuid.value &&
                  actual.state.principal_name == expected.state.principal_name &&
                  actual.state.credential_fingerprint == expected.state.credential_fingerprint &&
                  actual.state.creator_tx == expected.state.creator_tx &&
                  actual.state.policy_generation == expected.state.policy_generation &&
                  actual.state.security_context_generation == expected.state.security_context_generation,
              "record permutation changed bootstrap identity or generation");
      db::DatabaseOpenConfig open;
      open.path = path.string();
      open.read_only = true;
      Require(db::OpenDatabaseFile(open).ok(), "permuted bootstrap did not reopen");
      // An actual persisted generation disagreement must still refuse in
      // every order; the next iteration restores the original complete rows.
      MutateOneTypedRecord(path, page_size,
          [](const auto& record, const auto& fields) {
            return record.header.kind == catalog::CatalogRecordKind::user_account &&
                fields.contains("bootstrap_principal");
          }, [](auto*, auto* fields) { (*fields)["policy_generation"] = "2"; });
      const auto before_refusal = file_image(path);
      const auto mismatch = db::ReadDatabaseBootstrapSecurityCatalog(path.string());
      Require(!mismatch.ok() && !mismatch.state.committed_by_inventory &&
                  mismatch.diagnostic.diagnostic_code == "SB-DB-BOOTSTRAP-SECURITY-GENERATION-MISMATCH",
              "persisted generation mismatch was not refused exactly");
      for (const bool read_only : {true, false}) {
        open.read_only = read_only;
        const auto refused_open = db::OpenDatabaseFile(open);
        Require(!refused_open.ok() && refused_open.diagnostic.diagnostic_code ==
                    "SB-DB-BOOTSTRAP-SECURITY-GENERATION-MISMATCH",
                "open admitted mismatched bootstrap generations");
        Require(file_image(path) == before_refusal,
                "refused bootstrap read/open changed persisted bytes");
      }
      ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));
    Require(permutations == 6, "bootstrap permutation coverage incomplete");
  }
  std::cout << "bootstrap ordering: 5 profiles, 30 real-file permutations/reopens, "
               "30 persisted mismatch refusals, 60 no-effect open refusals, "
               "30 semantic matrices\n";
}

void TestNativeSecurityGenerationRepair(Cleanup* cleanup) {
  for (const char* fault : {"", "after_copy_before_rewrite",
                            "after_rewrite_sync_before_publish", "after_publish_before_ack"}) {
    std::filesystem::path path;
    const auto created = CreateCommitted(cleanup, "native_generation_repair", &path);
    const auto before = db::ReadDatabaseBootstrapSecurityCatalog(path.string());
    Require(before.ok() && before.state.present, "generation repair baseline missing");
    auto rows = ReadCatalogRows(path, created.state.header.page_size);
    unsigned stripped = 0;
    for (auto& row : rows) {
      if (row.kind != page::CatalogPageRowKind::typed_catalog_record) continue;
      auto outer = catalog::DecodeCatalogTypedRecord(row);
      Require(outer.ok(), "generation fixture outer decode failed");
      if (!catalog::IsCatalogSecurityRecordKind(outer.record.header.kind)) continue;
      auto security = catalog::DecodeCatalogSecurityRecord(outer.record.header.kind, outer.record.payload);
      Require(security.ok(), "generation fixture native decode failed");
      if (!security.record->attributes.contains("security_context_authority_version")) continue;
      security.record->attributes.erase("security_context_authority_version");
      security.record->attributes.erase("security_context_generation");
      const auto encoded = catalog::EncodeCatalogSecurityRecord(*security.record);
      Require(encoded.ok(), "generation fixture native encode failed");
      outer.record.payload.assign(encoded.bytes.begin(), encoded.bytes.end());
      const auto framed = catalog::EncodeCatalogTypedRecord(outer.record, row.ordinal);
      Require(framed.ok(), "generation fixture outer encode failed");
      row = framed.row;
      ++stripped;
    }
    Require(stripped == 3, "generation fixture did not select all three bootstrap owners");
    WriteCatalogRows(path, created.state.header.page_size, rows);
    db::DatabaseOpenConfig open;
    open.path = path.string(); open.read_only = true;
    const auto readonly = db::OpenDatabaseFile(open);
    Require(!readonly.ok() && readonly.diagnostic.diagnostic_code == "FORMAT.UPGRADE_REQUIRED",
            "read-only missing generation did not require explicit writable repair");
    open.read_only = false;
    open.security_context_migration_fault_injection_point = fault;
    const auto repaired = db::OpenDatabaseFile(open);
    Require(repaired.ok() == std::string_view(fault).empty(), "generation repair fault finality incorrect");
    open.security_context_migration_fault_injection_point.clear();
    Require(db::OpenDatabaseFile(open).ok(), "generation repair did not recover on reopen");
    const auto after = db::ReadDatabaseBootstrapSecurityCatalog(path.string());
    Require(after.ok() && after.state.present && after.state.committed_by_inventory &&
            after.state.security_context_generation == 1 &&
            after.state.principal_uuid.value == before.state.principal_uuid.value &&
            after.state.sysarch_role_uuid.value == before.state.sysarch_role_uuid.value &&
            after.state.membership_uuid.value == before.state.membership_uuid.value &&
            after.state.credential_fingerprint == before.state.credential_fingerprint,
            "native generation repair changed security ownership or credential authority");
    for (const auto& row : ReadCatalogRows(path, created.state.header.page_size)) {
      if (row.kind != page::CatalogPageRowKind::typed_catalog_record) continue;
      const auto outer = catalog::DecodeCatalogTypedRecord(row);
      Require(outer.ok(), "repaired catalog framing invalid");
      if (catalog::IsCatalogSecurityRecordKind(outer.record.header.kind))
        Require(catalog::DecodeCatalogSecurityRecord(outer.record.header.kind, outer.record.payload).ok(),
                "generation repair appended text to a binary security record");
    }
  }
}

}  // namespace

int main() try {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name =
      "database_lifecycle_bootstrap_security_publication_conformance";
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      policy,
      "database_lifecycle_bootstrap_security_publication_conformance");
  Require(configured.ok(), "bootstrap security memory fixture failed");

  Cleanup cleanup;
  TestBootstrapRecordOrder();
  TestPrepublicationFaults(&cleanup);
  TestPostpublicationFaults(&cleanup);
  TestReplayRefused(&cleanup);
  TestCredentialEnvelopeRefusedAndRedacted(&cleanup);
  TestSemanticTamperRejection(&cleanup);
  TestNativeSecurityGenerationRepair(&cleanup);
  TestForbiddenSecuritySidecars(&cleanup);
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
