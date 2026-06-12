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
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "page_header.hpp"
#include "page_manager.hpp"
#include "startup_state.hpp"
#include "transaction_state.hpp"
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
namespace mga = scratchbird::transaction::mga;
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

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc_004_create_bootstrap_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
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

std::set<std::string> LoadExpectedPolicyKeys() {
  static constexpr std::array<std::string_view, 58> kKeys = {{
      "policy.catalog.bootstrap",
      "database.identity",
      "database.create.failure_cleanup",
      "database.bootstrap.tx1",
      "database.first_open.tx2_activation",
      "schema.bootstrap.roots",
      "security.authority_selection",
      "security.authentication_provider",
      "security.bootstrap_password",
      "security.authorization_default",
      "security.principal_role_group_seed",
      "security.user_home_schema",
      "security.audit",
      "security.redaction",
      "security.protected_material",
      "security.encryption_key_admission",
      "configuration.source_precedence",
      "configuration.override_reload",
      "resource.seed_i18n",
      "resource.signature_provenance",
      "storage.filespace_profile",
      "storage.filespace_lifecycle",
      "storage.allocation_freespace_pagemap",
      "lifecycle.ownership_stale_owner",
      "lifecycle.recovery_dirty_open",
      "lifecycle.maintenance_restricted",
      "lifecycle.shutdown_graceful_drain",
      "lifecycle.shutdown_force",
      "transaction.admission",
      "transaction.default_isolation_snapshot",
      "transaction.commit_durability",
      "transaction.rollback_savepoint_limbo",
      "transaction.mga_gc_retention",
      "concurrency.lock_wait_deadlock",
      "cache.checkpoint_preload_flush",
      "backup.archive_restore_snapshot_shadow",
      "workload.resource_quota",
      "temp.spill_workspace",
      "session.disconnect_timeout",
      "server.route_listener_startup",
      "listener.bind_tls_pool",
      "parser.package_admission",
      "ipc.frame_auth_backpressure",
      "udr.extension_trust_resource",
      "executable.side_effect",
      "sequence.generator_cache",
      "event.queue_notification",
      "diagnostics.message_vector",
      "observability.metrics_log",
      "support.bundle",
      "evidence.retention",
      "job.scheduler",
      "capability.feature_gate",
      "upgrade.migration_refusal",
      "admin.management_command_authorization",
      "donor.emulation_profile",
      "replication.cdc_changefeed_boundary",
      "cluster.boundary_fail_closed",
  }};
  std::set<std::string> keys;
  for (const auto key : kKeys) {
    keys.insert(std::string(key));
  }
  Require(keys.size() == 58, "default policy catalog did not expose exactly 58 policy keys");
  return keys;
}

bool HasCommittedEvidenceTransaction(const mga::LocalTransactionInventory& inventory,
                                     std::uint64_t local_transaction_id) {
  for (const auto& entry : inventory.entries) {
    if (entry.identity.local_id.value == local_transaction_id &&
        entry.state == mga::TransactionState::committed &&
        entry.evidence_record_written) {
      return true;
    }
  }
  return false;
}

bool HasEvidenceFlag(const db::StartupStateRecord& state, std::uint64_t flag) {
  return (state.durable_evidence_flags & flag) != 0;
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

bool HasKind(const std::vector<DecodedRecord>& records, catalog::CatalogRecordKind kind) {
  for (const auto& record : records) {
    if (record.record.header.kind == kind) {
      return true;
    }
  }
  return false;
}

std::uint32_t CountKind(const std::vector<DecodedRecord>& records, catalog::CatalogRecordKind kind) {
  std::uint32_t count = 0;
  for (const auto& record : records) {
    if (record.record.header.kind == kind) {
      ++count;
    }
  }
  return count;
}

void RequireAllTypedRecordsCreatedByTx1(const std::vector<DecodedRecord>& records) {
  for (const auto& record : records) {
    const auto found = record.fields.find("creator_tx");
    Require(found != record.fields.end(), "typed catalog record payload is missing creator_tx");
    Require(found->second == "1", "typed catalog record payload was not created by tx1");
  }
}

void RequireBootstrapEvidence(const std::filesystem::path& database_path,
                              const db::DatabaseLifecycleState& created) {
  Require(created.startup_state_present, "create did not return startup state");
  Require(created.local_transaction_inventory_present,
          "create did not return transaction inventory");
  Require(created.startup_state.bootstrap_local_transaction_id == 1,
          "startup state did not persist bootstrap tx1");
  Require(created.startup_state.last_lifecycle_local_transaction_id == 1,
          "startup state last lifecycle transaction is not tx1");
  Require(created.startup_state.durable_lifecycle_phase ==
              db::StartupLifecycleDurablePhase::create_tx1_committed,
          "startup state did not persist create_tx1_committed phase");
  Require(HasEvidenceFlag(created.startup_state,
                          db::StartupLifecycleEvidenceFlag::bootstrap_tx1_committed),
          "startup state did not persist bootstrap tx1 evidence flag");
  Require(HasCommittedEvidenceTransaction(created.local_transaction_inventory, 1),
          "returned transaction inventory does not show tx1 committed with evidence");

  disk::FileDevice device;
  const auto opened = device.Open(database_path.string(), disk::FileOpenMode::open_existing_read_only);
  Require(opened.ok(), "could not reopen database to verify durable tx1 state");
  const auto durable_startup = db::ReadStartupStatePageBody(&device, created.header.page_size);
  Require(durable_startup.ok(), "durable startup state page did not parse");
  Require(durable_startup.state.bootstrap_local_transaction_id == 1,
          "durable startup state did not preserve bootstrap tx1");
  Require(HasEvidenceFlag(durable_startup.state,
                          db::StartupLifecycleEvidenceFlag::bootstrap_tx1_committed),
          "durable startup state did not preserve bootstrap tx1 evidence");

  const auto durable_inventory =
      db::LoadLocalTransactionInventoryFromOpenDevice(&device, created.header.page_size);
  Require(durable_inventory.ok(), "durable transaction inventory did not parse");
  Require(HasCommittedEvidenceTransaction(durable_inventory.inventory, 1),
          "durable transaction inventory does not show tx1 committed with evidence");
}

void RequireSchemas(const std::vector<DecodedRecord>& records) {
  std::set<std::string> paths;
  for (const auto& record : records) {
    if (record.record.header.kind != catalog::CatalogRecordKind::schema) {
      continue;
    }
    const auto found = record.fields.find("path");
    if (found != record.fields.end()) {
      paths.insert(found->second);
    }
  }

  const std::vector<std::string> required = {
      "sys",
      "sys.catalog",
      "sys.information",
      "sys.metrics",
      "sys.security",
      "users",
      "users.public",
      "remote",
      "emulated",
  };
  for (const auto& path : required) {
    Require(paths.count(path) == 1, std::string("missing bootstrap schema path ") + path);
  }
  for (const auto& path : paths) {
    Require(path != "cluster" && path.rfind("cluster.", 0) != 0,
            "standalone database create emitted a cluster schema");
    Require(path != "sys.information_schema",
            "legacy information_schema synonym was created as a second schema branch");
  }
}

void RequireInformationSchemaSynonym(const std::vector<DecodedRecord>& records) {
  bool found = false;
  for (const auto& record : records) {
    if (record.record.header.kind != catalog::CatalogRecordKind::synonym_descriptor) {
      continue;
    }
    const auto path = record.fields.find("path");
    const auto target = record.fields.find("canonical_target_path");
    const auto remap = record.fields.find("child_parent_remap_required");
    const auto branch = record.fields.find("second_schema_branch");
    if (path != record.fields.end() && path->second == "sys.information_schema") {
      found = true;
      Require(target != record.fields.end() && target->second == "sys.information",
              "information_schema synonym did not target sys.information");
      Require(remap != record.fields.end() && remap->second == "1",
              "information_schema synonym did not require child parent remap");
      Require(branch != record.fields.end() && branch->second == "0",
              "information_schema synonym allowed a second schema branch");
    }
  }
  Require(found, "missing sys.information_schema synonym descriptor");
}

void RequireDefaultPolicies(const std::vector<DecodedRecord>& records,
                            const std::set<std::string>& expected_keys) {
  std::set<std::string> seen;
  for (const auto& record : records) {
    if (record.record.header.kind != catalog::CatalogRecordKind::policy) {
      continue;
    }
    const auto key = record.fields.find("policy_key");
    if (key == record.fields.end() || expected_keys.count(key->second) == 0) {
      continue;
    }
    Require(record.fields.find("policy_generation") != record.fields.end() &&
                record.fields.at("policy_generation") == "1",
            "default policy record does not have policy_generation=1");
    Require(record.fields.find("created_txn") != record.fields.end() &&
                record.fields.at("created_txn") == "1",
            "default policy record does not have created_txn=1");
    Require(record.fields.find("tx1_seed_required") != record.fields.end() &&
                record.fields.at("tx1_seed_required") == "1",
            "default policy record does not have tx1_seed_required=1");
    Require(record.fields.find("uuid_source") != record.fields.end() &&
                record.fields.at("uuid_source") == "fresh_uuidv7",
            "default policy record does not have uuid_source=fresh_uuidv7");
    seen.insert(key->second);
  }
  for (const auto& key : expected_keys) {
    Require(seen.count(key) == 1, std::string("missing default policy catalog key ") + key);
  }
}

void RequireResourceSeedData(const std::vector<page::CatalogPageRow>& rows,
                             const std::vector<DecodedRecord>& records) {
  bool saw_seed_pack = false;
  bool saw_seed_artifact = false;
  bool saw_charset_row = false;
  bool saw_collation_row = false;
  bool saw_timezone_row = false;
  for (const auto& row : rows) {
    saw_seed_pack = saw_seed_pack || row.kind == page::CatalogPageRowKind::resource_seed_pack;
    saw_seed_artifact = saw_seed_artifact || row.kind == page::CatalogPageRowKind::resource_seed_artifact;
    saw_charset_row = saw_charset_row || row.kind == page::CatalogPageRowKind::charset_alias_record ||
                                      row.kind == page::CatalogPageRowKind::charset_record;
    saw_collation_row = saw_collation_row || row.kind == page::CatalogPageRowKind::collation_record;
    saw_timezone_row = saw_timezone_row || row.kind == page::CatalogPageRowKind::timezone_record;
  }

  Require(saw_seed_pack, "resource seed pack catalog row is missing");
  Require(saw_seed_artifact, "resource seed artifact catalog rows are missing");
  Require(saw_charset_row, "charset resource seed catalog rows are missing");
  Require(saw_collation_row, "collation resource seed catalog rows are missing");
  Require(saw_timezone_row, "timezone resource seed catalog rows are missing");
  Require(HasKind(records, catalog::CatalogRecordKind::resource_bundle),
          "typed resource_bundle record is missing");
  Require(HasKind(records, catalog::CatalogRecordKind::resource_artifact),
          "typed resource_artifact record is missing");
  Require(HasKind(records, catalog::CatalogRecordKind::charset),
          "typed charset record is missing");
  Require(HasKind(records, catalog::CatalogRecordKind::collation),
          "typed collation record is missing");
  Require(HasKind(records, catalog::CatalogRecordKind::timezone),
          "typed timezone record is missing");
}

void RequireMetricBootstrapData(const std::vector<DecodedRecord>& records) {
  Require(CountKind(records, catalog::CatalogRecordKind::metric_descriptor) > 0,
          "typed metric_descriptor records are missing");
  Require(CountKind(records, catalog::CatalogRecordKind::metric_current_value) > 0,
          "typed metric_current_value records are missing");
}

void RequireSecurityBootstrapData(const std::vector<DecodedRecord>& records) {
  Require(HasKind(records, catalog::CatalogRecordKind::user_account),
          "typed user_account record is missing");
  Require(HasKind(records, catalog::CatalogRecordKind::group_account),
          "typed group_account record is missing");
  Require(HasKind(records, catalog::CatalogRecordKind::role_account),
          "typed role_account record is missing");
  Require(HasKind(records, catalog::CatalogRecordKind::grant_record),
          "typed grant_record record is missing");
}

}  // namespace

int main() {
  auto memory_policy = memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "database_lifecycle_create_bootstrap_conformance";
  const auto memory_configured = memory::ConfigureDefaultMemoryManagerForFixture(
      memory_policy, "database_lifecycle_create_bootstrap_conformance");
  if (!memory_configured.ok()) {
    std::cerr << memory_configured.diagnostic.diagnostic_code << '\n';
  }
  Require(memory_configured.ok(), "default memory fixture configuration failed");

  const auto expected_policy_keys = LoadExpectedPolicyKeys();
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
  RequireBootstrapEvidence(database_path, created.state);

  const auto rows = ReadCatalogRows(database_path, created.state.header.page_size);
  const auto records = DecodeTypedRecords(rows);
  RequireAllTypedRecordsCreatedByTx1(records);
  RequireSchemas(records);
  RequireInformationSchemaSynonym(records);
  RequireDefaultPolicies(records, expected_policy_keys);
  RequireResourceSeedData(rows, records);
  RequireMetricBootstrapData(records);
  RequireSecurityBootstrapData(records);

  return EXIT_SUCCESS;
}
