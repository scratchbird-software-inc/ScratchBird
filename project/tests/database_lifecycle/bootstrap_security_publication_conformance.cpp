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
#include "page_manager.hpp"
#include "uuid.hpp"

#include <array>
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
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
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
                                 ".sb.local_password_auth"}) {
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

std::map<std::string, std::string> ParseFields(const std::string& payload) {
  std::map<std::string, std::string> fields;
  std::stringstream lines(payload);
  std::string line;
  while (std::getline(lines, line)) {
    std::stringstream tokens(line);
    std::string token;
    while (tokens >> token) {
      const auto split = token.find('=');
      if (split != std::string::npos) {
        fields[token.substr(0, split)] = token.substr(split + 1);
      }
    }
  }
  return fields;
}

std::string SerializeFields(
    const std::map<std::string, std::string>& fields) {
  std::string payload;
  for (const auto& field : fields) {
    payload += field.first + "=" + field.second + "\n";
  }
  return payload;
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
    auto fields = ParseFields(decoded.record.payload);
    if (!mutated && predicate(decoded.record, fields)) {
      auto record = decoded.record;
      mutator(&record, &fields);
      record.payload = SerializeFields(fields);
      const auto encoded = catalog::EncodeCatalogTypedRecord(record,
                                                              row.ordinal);
      Require(encoded.ok(), "tamper typed record encode failed");
      row = encoded.row;
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
    Require(argument.value.find("0123456789abcdef") == std::string::npos,
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

}  // namespace

int main() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name =
      "database_lifecycle_bootstrap_security_publication_conformance";
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      policy,
      "database_lifecycle_bootstrap_security_publication_conformance");
  Require(configured.ok(), "bootstrap security memory fixture failed");

  Cleanup cleanup;
  TestPrepublicationFaults(&cleanup);
  TestPostpublicationFaults(&cleanup);
  TestReplayRefused(&cleanup);
  TestCredentialEnvelopeRefusedAndRedacted(&cleanup);
  TestSemanticTamperRejection(&cleanup);
  TestForbiddenSecuritySidecars(&cleanup);
  return EXIT_SUCCESS;
}
