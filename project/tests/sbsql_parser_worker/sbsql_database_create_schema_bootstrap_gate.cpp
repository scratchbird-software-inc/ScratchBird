#include "../support/engine_evidence_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "api_types.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/schema_tree_api.hpp"
#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "security/identity_api.hpp"
#include "transaction/mutation_authority.hpp"
#include "transaction/savepoint_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <stdexcept>
#include <optional>
#include <utility>

#if defined(SB_IDENTITY_TEST_WRAP_IO)
#include "../common/single_tu_allocation_fault.hpp"
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
namespace {
enum class PublicationIoFault { none, api_sync, api_directory_sync, name_sync,
                                name_directory_sync, mark_directory_sync, release_directory_sync,
                                mark_ack, release_ack, allocation_after_mark,
                                allocation_after_release };
PublicationIoFault publication_io_fault = PublicationIoFault::none;
std::string publication_test_path;
struct stat publication_directory{};
unsigned inventory_journal_syncs = 0, name_syncs = 0, last_name_sync = 0;
bool inventory_directory_pending = false, api_directory_pending = false,
     name_directory_pending = false, publication_fault_hit = false;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  char fd_path[64]{}, path[4096]{};
  const auto length = std::snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
  const auto resolved = length > 0 ? ::readlink(fd_path, path, sizeof(path) - 1) : -1;
  struct stat file{};
  const bool directory = !publication_test_path.empty() && ::fstat(fd, &file) == 0 &&
      file.st_dev == publication_directory.st_dev && file.st_ino == publication_directory.st_ino;
  const bool owned_file = !publication_test_path.empty() && resolved >= 0 &&
      std::strncmp(path, publication_test_path.c_str(), publication_test_path.size()) == 0;
  const bool api = owned_file && std::strcmp(path + publication_test_path.size(), ".sb.api_events.v2") == 0;
  const bool names = owned_file && std::strcmp(path + publication_test_path.size(), ".sb.name_events.v2") == 0;
  const bool inventory = owned_file && std::strcmp(path + publication_test_path.size(), ".sb.txn_publish.tmp") == 0;
  if (names) ++name_syncs;
  const bool fail = (api && publication_io_fault == PublicationIoFault::api_sync) ||
      (names && name_syncs == last_name_sync && publication_io_fault == PublicationIoFault::name_sync) ||
      (directory && api_directory_pending && publication_io_fault == PublicationIoFault::api_directory_sync) ||
      (directory && name_directory_pending && publication_io_fault == PublicationIoFault::name_directory_sync) ||
      (directory && inventory_directory_pending &&
       ((inventory_journal_syncs == 2 && (publication_io_fault == PublicationIoFault::mark_ack ||
                                   publication_io_fault == PublicationIoFault::mark_directory_sync)) ||
        (inventory_journal_syncs == 4 && (publication_io_fault == PublicationIoFault::release_ack ||
                                   publication_io_fault == PublicationIoFault::release_directory_sync))));
  if (fail) {
    // Separate a barrier failure from a real successful barrier whose reply
    // is lost. Both can coexist with an actually selected publication.
    if ((publication_io_fault == PublicationIoFault::mark_ack ||
         publication_io_fault == PublicationIoFault::release_ack) && __real_fsync(fd) != 0)
      return -1;
    publication_fault_hit = true;
    publication_io_fault = PublicationIoFault::none;
    inventory_directory_pending = api_directory_pending = name_directory_pending = false;
    errno = EIO;
    return -1;
  }
  const auto result = __real_fsync(fd);
  if (result == 0 && directory) {
    if (inventory_directory_pending &&
        ((inventory_journal_syncs == 2 && publication_io_fault == PublicationIoFault::allocation_after_mark) ||
         (inventory_journal_syncs == 4 && publication_io_fault == PublicationIoFault::allocation_after_release))) {
      publication_fault_hit = true;
      publication_io_fault = PublicationIoFault::none;
      allocations_before_failure = 0;
    }
    inventory_directory_pending = api_directory_pending = name_directory_pending = false;
  }
  if (result == 0 && api) api_directory_pending = true;
  if (result == 0 && names && name_syncs == last_name_sync) name_directory_pending = true;
  if (result == 0 && inventory) {
    ++inventory_journal_syncs;
    // The native publisher renames this synchronized exact temp journal, then
    // synchronizes the bound parent. Do not pretend a linker rename wrapper
    // intercepts std::filesystem's call inside the shared C++ runtime.
    inventory_directory_pending = true;
  }
  return result;
}
#endif

namespace scratchbird::engine::internal_api {
template <typename... Args>
auto CheckedSchemaTreeRecords(Args&&... args) {
  EngineApiDiagnostic diagnostic;
  auto result = VisibleSchemaTreeRecords(std::forward<Args>(args)..., diagnostic);
  if (diagnostic.error) throw std::runtime_error(diagnostic.code + ":" + diagnostic.detail);
  return result;
}
template <typename... Args>
auto CheckedFindSchemaTreeRecord(Args&&... args) {
  EngineApiDiagnostic diagnostic;
  auto result = FindVisibleSchemaTreeRecord(std::forward<Args>(args)..., diagnostic);
  if (diagnostic.error) throw std::runtime_error(diagnostic.code + ":" + diagnostic.detail);
  return result;
}
}  // namespace scratchbird::engine::internal_api

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

memory::AllocationPolicy MemoryPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "sbsql_database_create_schema_bootstrap_gate";
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_database_create_schema_bootstrap_gate");
  Require(configured.ok(), "schema bootstrap memory fixture configuration failed");
  Require(configured.fixture_mode, "schema bootstrap memory fixture mode was not active");
}

std::uint64_t CurrentUnixMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

api::EngineUuid NewUuid(UuidKind kind) {
  if (kind == UuidKind::session) {
    // A session owns a live runtime identity, never a durable catalog identity.
    const auto issued = uuid::IssueRuntimeIdentityV7();
    Require(issued.has_value(), "test session UUID generation failed");
    const auto session = uuid::MakeTypedUuid(kind, *issued);
    Require(session.ok(), "test session UUID admission failed");
    return session.value.value;
  }
  static std::uint64_t sequence = 0;
  const auto seed = CurrentUnixMillis() + (++sequence);
  const auto generated = uuid::GenerateEngineIdentityV7(kind, seed);
  Require(generated.ok(), "test UUID generation failed");
  return generated.value.value;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                  const api::EngineUuid& database_uuid) {
  static const api::EngineUuid gate_session_uuid = NewUuid(UuidKind::session);
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(database_path.string());
  Require(bootstrap.ok() && bootstrap.state.present && bootstrap.state.committed_by_inventory,
          "fixture must resolve the actual committed bootstrap principal");
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "sbsql-database-create-schema-bootstrap-gate";
  context.database_path = database_path.string();
  context.database_uuid = database_uuid;
  context.principal_uuid = bootstrap.state.principal_uuid.value;
  context.session_uuid = gate_session_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  return context;
}

api::EngineUuid CreateDatabase(const std::filesystem::path& database_path) {
  const auto now = CurrentUnixMillis();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(), "test UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = now;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto templates = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                         "config/templates";
  create.metric_policy_defaults_path = (templates / "metric-policy-defaults.conf").string();
  create.metric_policy_families_path = (templates / "metric-policy-families.conf").string();
#ifdef SB_BOOTSTRAP_SEED_PACK_ROOT
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.allow_minimal_resource_bootstrap = false;
  create.require_resource_seed_pack = true;
#else
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
#endif
  create.allow_overwrite = false;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':' << created.diagnostic.message_key
              << ':' << created.diagnostic.remediation_hint << '\n';
    Require(false, "credentialed explicitly configured database creation failed");
  }
  return database_uuid.value.value;
}

std::uint32_t TypedKindCount(const std::vector<std::string>& counts, const std::string& kind) {
  const std::string prefix = kind + "=";
  for (const auto& count : counts) {
    if (count.rfind(prefix, 0) == 0) {
      return static_cast<std::uint32_t>(std::strtoul(count.substr(prefix.size()).c_str(), nullptr, 10));
    }
  }
  return 0;
}

bool HasCommittedTransaction(const db::DatabaseLifecycleState& state, std::uint64_t local_transaction_id) {
  for (const auto& entry : state.local_transaction_inventory.entries) {
    if (entry.identity.local_id.value == local_transaction_id &&
        entry.state == scratchbird::transaction::mga::TransactionState::committed &&
        entry.evidence_record_written) {
      return true;
    }
  }
  return false;
}

void RequireBootstrapCatalogRowsPersisted(const std::filesystem::path& database_path) {
  db::DatabaseOpenConfig open;
  open.path = database_path.string();
  const auto opened = db::OpenDatabaseFile(open);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "database with bootstrap catalog rows did not reopen");
  Require(opened.state.startup_state.first_open_activation_local_transaction_id == 2,
          "first writable open did not reserve transaction 2 for runtime activation");
  Require(opened.state.local_transaction_inventory.next_local_transaction_id == 3,
          "first writable open did not advance next transaction id after activation");
  Require(HasCommittedTransaction(opened.state, 1),
          "database create bootstrap transaction 1 was not committed in inventory");
  Require(HasCommittedTransaction(opened.state, 2),
          "first writable open activation transaction 2 was not committed in inventory");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "charset") > 50,
          "charset catalog rows were not persisted at database create");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "charset_alias") > 50,
          "charset alias catalog rows were not persisted at database create");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "collation") > 100,
          "collation catalog rows were not persisted at database create");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "timezone") > 100,
          "timezone catalog rows were not persisted at database create");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "timezone_transition") > 5,
          "timezone transition/source catalog rows were not persisted at database create");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "timezone_leap_second") > 1,
          "timezone leap-second catalog rows were not persisted at database create");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "policy") >= 10,
          "default policy rows were not persisted at database create");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "synonym_descriptor") >= 1,
          "sys.information_schema synonym descriptor was not persisted at database create");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "table_descriptor") >= 15,
          "metrics structure table descriptors were not persisted at database create");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "metric_descriptor") == 7,
          "exact explicitly selected native metric descriptor rows were not persisted");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "metric_current_value") == 0,
          "creation must not fabricate producer observations");
  const auto metrics = db::ReadDatabaseBootstrapMetricCatalog(database_path.string(),
                                                            opened.state.header.database_uuid);
  Require(metrics.ok() && metrics.definitions.size() == 7,
          "native owning read must validate all selected descriptor/dependency bundles");

  std::ifstream input(database_path, std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  const std::string bytes = buffer.str();
  Require(bytes.find("metrics_cluster_shared_history") == std::string::npos,
          "standalone database create persisted cluster metric retention policy");
  Require(bytes.find("cluster.sys.metrics") == std::string::npos,
          "standalone database create persisted cluster metric descriptors");
}

std::set<std::string> VisibleSchemaPaths(const api::EngineRequestContext& context) {
  std::set<std::string> paths;
  for (const auto& schema : api::CheckedSchemaTreeRecords(context, context.local_transaction_id)) {
    for (const auto& name : schema.localized_names) {
      if (!name.path.empty()) { paths.insert(name.path); }
    }
  }
  return paths;
}

void RequireBootstrapSchemas(const std::set<std::string>& paths) {
  const char* required[] = {
      "sys",
      "sys.catalog",
      "sys.metrics",
      "sys.security",
      "sys.configuration",
      "sys.management",
      "sys.fn",
      "sys.udr",
      "sys.parser",
      "sys.storage",
      "sys.mga",
      "sys.audit",
      "sys.compatibility",
      "sys.information",
      "sys.catalog_readable",
      "sys.diagnostics",
      "cluster",
      "users",
      "users.public",
      "emulated",
      "remote",
      "app",
  };
  for (const char* path : required) {
    Require(paths.count(path) == 1, std::string("missing bootstrap schema path ") + path);
  }
  Require(paths.count("sys.information_schema") == 0,
          "sys.information_schema was incorrectly materialized as a schema branch");
  for (const auto& path : paths) {
    Require(path == "cluster" || path.rfind("cluster.", 0) != 0,
            "standalone database create emitted cluster implementation schema path");
  }
}

void CreateUserThroughEnginePolicy(const std::filesystem::path& database_path,
                                   const api::EngineUuid& database_uuid) {
  api::EngineBeginTransactionRequest begin_request;
  begin_request.context = Context(database_path, database_uuid);
  const auto begin = api::EngineBeginTransaction(begin_request);
  if (!begin.ok && !begin.diagnostics.empty()) {
    std::cerr << begin.diagnostics.front().code << ": "
              << begin.diagnostics.front().detail << '\n';
  }
  Require(begin.ok, "first user transaction did not begin through engine MGA authority");
  Require(begin.local_transaction_id == 3,
          "first user transaction did not begin after bootstrap tx1 and activation tx2");
  auto context = Context(database_path, database_uuid);
  context.local_transaction_id = begin.local_transaction_id;
  context.transaction_uuid = begin.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = begin.local_transaction_id;

  api::EngineCreateIdentityRequest identity;
  identity.context = context;
  identity.target_object.uuid = NewUuid(UuidKind::principal);
  identity.target_object.object_kind = "security_identity";
  identity.localized_names.push_back(Name("benchmark_user"));
  identity.option_envelopes.push_back("identity_kind:user");
  identity.option_envelopes.push_back("principal_name:benchmark_user");
  const auto journal_bytes = [&](const std::string& suffix) -> std::optional<std::string> {
    const auto path = database_path.string() + suffix;
    if (!std::filesystem::exists(path)) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), "preflight journal read failed");
    std::ostringstream bytes;
    bytes << input.rdbuf();
    Require(!input.bad(), "preflight journal read incomplete");
    return bytes.str();
  };
  const auto unchanged_refusal = [&](const api::EngineCreateIdentityRequest& invalid) {
    const auto before = db::AcquireStrongLocalTransactionInventorySnapshot(database_path.string());
    Require(before.ok(), "principal preflight inventory read failed");
    const auto api_before = journal_bytes(".sb.api_events.v2");
    const auto names_before = journal_bytes(".sb.name_events.v2");
    const auto refused = api::EngineCreateIdentity(invalid);
    Require(!refused.ok, "invalid principal was admitted");
    const auto after = db::AcquireStrongLocalTransactionInventorySnapshot(database_path.string());
    Require(after.ok() && before.snapshot->inventory.publication_base == after.snapshot->inventory.publication_base,
            "effect-free principal refusal changed native inventory publication");
    Require(api_before == journal_bytes(".sb.api_events.v2") &&
                names_before == journal_bytes(".sb.name_events.v2"),
            "effect-free principal refusal changed object/name publication bytes");
    Require(!api::ValidateCurrentMutatingTransactionAuthority(context, "fixture.preflight").error,
            "effect-free principal refusal poisoned its transaction");
  };
  auto invalid = identity;
  invalid.target_object.uuid = {};
  unchanged_refusal(invalid);
  for (const std::uint8_t version : {1, 4, 5, 6, 8}) {
    invalid = identity;
    invalid.target_object.uuid.bytes[6] = static_cast<std::uint8_t>(
        (invalid.target_object.uuid.bytes[6] & 0x0f) | (version << 4));
    unchanged_refusal(invalid);
  }
  invalid = identity;
  invalid.localized_names.clear();
  invalid.option_envelopes = {"identity_kind:service"};
  unchanged_refusal(invalid);
  invalid = identity;
  invalid.option_envelopes.emplace_back("password:must-not-be-persisted");
  unchanged_refusal(invalid);
  api::EngineCreateSavepointRequest savepoint;
  savepoint.context = context;
  savepoint.option_envelopes = {"savepoint_name:identity_preflight"};
  Require(api::EngineCreateSavepoint(savepoint).ok, "native savepoint control did not create");
  unchanged_refusal(identity);
  api::EngineReleaseSavepointRequest release;
  static_cast<api::EngineApiRequest&>(release) = savepoint;
  Require(api::EngineReleaseSavepoint(release).ok, "native savepoint control did not release");
  auto created = api::EngineCreateIdentity(identity);
  if (!created.ok && !created.diagnostics.empty()) {
    std::cerr << created.diagnostics.front().code << ": "
              << created.diagnostics.front().detail << '\n';
  }
  Require(created.ok, "engine identity policy did not create the test user");
  unchanged_refusal(identity);
  invalid = identity;
  invalid.option_envelopes = {"identity_kind:service", "principal_name:duplicate_service"};
  unchanged_refusal(invalid);
  bool saw_home_schema = false;
  bool saw_home_schema_path = false;
  api::EngineUuid home_schema_uuid;
  for (const auto& evidence : created.evidence) {
    if (evidence.evidence_kind == "home_schema") {
      const auto* identity = std::get_if<api::EngineUuid>(&evidence.evidence_id);
      Require(identity && uuid::IsEngineIdentityUuid(*identity),
              "home schema evidence must contain a binary engine UUID");
      saw_home_schema = true;
      home_schema_uuid = *identity;
    }
    if (evidence.evidence_kind == "home_schema_path" && scratchbird::tests::EvidenceTextEquals(evidence.evidence_id, "users.benchmark_user")) {
      saw_home_schema_path = true;
    }
  }
  Require(saw_home_schema, "security.create_identity did not return generated home schema UUID evidence");
  Require(saw_home_schema_path, "security.create_identity did not return home schema path evidence");

  const auto check_principal = [&](const api::EngineRequestContext& reader, bool visible) {
    const auto loaded = api::LoadSecurityPrincipalLifecycleState(reader);
    Require(loaded.ok, "native principal owner read failed");
    const auto principal = std::find_if(loaded.state.principals.begin(), loaded.state.principals.end(),
        [&](const auto& row) { return row.principal_uuid == identity.target_object.uuid; });
    Require((principal != loaded.state.principals.end()) == visible,
            "identity publication visibility differs from the actual native principal owner");
    if (visible) {
      Require(principal->principal_name == "benchmark_user" && principal->principal_kind == "user" &&
              principal->creator_tx == context.local_transaction_id && principal->security_generation > 0 &&
              !principal->deleted && principal->lifecycle_state == "active" &&
              principal->credential_fingerprint.empty(),
              "native principal must preserve exact identity, kind, origin and absent credentials");
    }
  };
  check_principal(context, true);
  check_principal(Context(database_path, database_uuid), false);

  api::EngineCommitTransactionRequest commit_request;
  commit_request.context = context;
  const auto committed = api::EngineCommitTransaction(commit_request);
  if (!committed.ok && !committed.diagnostics.empty()) {
    std::cerr << committed.diagnostics.front().code << ": "
              << committed.diagnostics.front().detail << '\n';
  }
  Require(committed.ok, "engine MGA transaction did not commit the identity policy mutation");

  const auto observer = Context(database_path, database_uuid);
  check_principal(observer, true);
  const auto paths = VisibleSchemaPaths(observer);
  Require(paths.count("users.benchmark_user") == 1, "adduser policy did not create users.benchmark_user home schema");
  const auto home_schema = api::CheckedFindSchemaTreeRecord(observer, home_schema_uuid, 0);
  Require(home_schema.has_value(), "returned home schema UUID was not visible after commit");
  db::DatabaseOpenConfig reopen;
  reopen.path = database_path.string();
  reopen.read_only = true;
  Require(db::OpenDatabaseFile(reopen).ok(), "identity catalog did not reopen after commit");
  check_principal(Context(database_path, database_uuid), true);
}

void RequireCleanShutdownWritesFinalTransaction(const std::filesystem::path& database_path) {
  const auto clean = db::MarkDatabaseCleanShutdown(database_path.string());
  if (!clean.ok()) {
    std::cerr << clean.diagnostic.diagnostic_code << '\n';
  }
  Require(clean.ok(), "clean shutdown did not persist lifecycle state");

  db::DatabaseOpenConfig open;
  open.path = database_path.string();
  open.read_only = true;
  const auto opened = db::OpenDatabaseFile(open);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "database did not reopen after clean shutdown");
  Require(opened.state.startup_state.clean_shutdown,
          "clean shutdown marker was not persisted");
  Require(opened.state.startup_state.clean_shutdown_local_transaction_id == 4,
          "clean shutdown did not reserve the expected final lifecycle transaction");
  Require(opened.state.local_transaction_inventory.next_local_transaction_id == 5,
          "clean shutdown did not advance next transaction id after final lifecycle transaction");
  Require(HasCommittedTransaction(opened.state, 4),
          "clean shutdown final lifecycle transaction was not committed in inventory");
}

void RequireIdentityFailureExclusion(const std::filesystem::path& database_path) {
  const auto database_uuid = CreateDatabase(database_path);
  RequireBootstrapCatalogRowsPersisted(database_path);
  for (const std::string_view fault : {
           "engine.test.private_security_locator_fault.after_locator",
           "engine.test.private_security_locator_fault.after_anchor_copy_0",
           "engine.test.private_security_locator_fault.readback_mismatch"}) {
    api::EngineBeginTransactionRequest begin_request;
    begin_request.context = Context(database_path, database_uuid);
    const auto begin = api::EngineBeginTransaction(begin_request);
    Require(begin.ok, "failure qualification transaction did not begin");
    auto context = Context(database_path, database_uuid);
    context.local_transaction_id = begin.local_transaction_id;
    context.transaction_uuid = begin.transaction_uuid;
    context.snapshot_visible_through_local_transaction_id = begin.local_transaction_id;
    api::EngineCreateIdentityRequest identity;
    identity.context = context;
    identity.context.trace_tags.emplace_back(fault);
    identity.target_object.uuid = NewUuid(UuidKind::principal);
    identity.localized_names.push_back(Name("faulted_user"));
    identity.option_envelopes = {"identity_kind:user", "principal_name:faulted_user"};
    const auto failed = api::EngineCreateIdentity(identity);
    Require(!failed.ok, "actual locator publication failure returned success");
    const auto loaded = db::AcquireStrongLocalTransactionInventorySnapshot(database_path.string());
    Require(loaded.ok(), "failed identity exclusion could not be reopened");
    const auto entry = scratchbird::transaction::mga::LookupLocalTransaction(
        loaded.snapshot->inventory,
        scratchbird::transaction::mga::MakeLocalTransactionId(context.local_transaction_id));
    Require(entry.ok() && entry.entry.identity.transaction_uuid.value == context.transaction_uuid &&
                entry.entry.rollback_only,
            "partial identity publication did not retain durable COMMIT exclusion");
    api::EngineCommitTransactionRequest commit;
    commit.context = context;
    Require(!api::EngineCommitTransaction(commit).ok,
            "partial identity publication was allowed to COMMIT");
    identity.context = context;
    Require(!api::EngineCreateIdentity(identity).ok,
            "ordinary identity request bypassed retained exclusion");
    api::EngineSecurityCreatePrincipalRequest principal;
    principal.context = context;
    principal.principal_uuid = NewUuid(UuidKind::principal);
    principal.principal_name = "unfenced_service";
    principal.principal_kind = "service";
    Require(!api::EngineSecurityCreatePrincipal(principal).ok,
            "ordinary native writer bypassed retained exclusion");
    api::ApiBehaviorRecord ordinary;
    ordinary.creator_tx = context.local_transaction_id;
    ordinary.operation_id = "fixture.excluded_mutation";
    ordinary.object_uuid = NewUuid(UuidKind::object);
    ordinary.object_kind = "fixture_excluded_mutation";
    ordinary.state = "active";
    const auto encoded = api::MakeApiBehaviorRecordEvent(ordinary);
    Require(!encoded.empty(), "ordinary mutation refusal control was not a valid binary frame");
    const auto refused = api::AppendApiBehaviorEvent(context, encoded);
    Require(refused.error && refused.detail.find("transaction_rollback_only") != std::string::npos,
            "ordinary object journal writer did not enforce actual native exclusion");
    api::EngineRollbackTransactionRequest rollback;
    rollback.context = context;
    Require(api::EngineRollbackTransaction(rollback).ok,
            "failed identity publication could not be rolled back");
    auto observer = Context(database_path, database_uuid);
    const auto state = api::LoadSecurityPrincipalLifecycleState(observer);
    Require(state.ok && std::none_of(state.state.principals.begin(), state.state.principals.end(),
        [&](const auto& row) { return row.principal_uuid == identity.target_object.uuid; }),
        "rolled-back partial identity remained visible in the native owner");
    Require(VisibleSchemaPaths(observer).count("users.faulted_user") == 0,
            "rolled-back partial identity retained its home schema");

    begin_request.context = observer;
    const auto retry_begin = api::EngineBeginTransaction(begin_request);
    Require(retry_begin.ok, "fresh retry could not begin after durable rollback");
    identity.context = Context(database_path, database_uuid);
    identity.context.local_transaction_id = retry_begin.local_transaction_id;
    identity.context.transaction_uuid = retry_begin.transaction_uuid;
    identity.context.snapshot_visible_through_local_transaction_id = retry_begin.local_transaction_id;
    Require(api::EngineCreateIdentity(identity).ok,
            "fresh admitted retry did not publish actual principal and home schema");
    // Roll back the completed retry to exercise name reuse on the next cut.
    rollback.context = identity.context;
    Require(api::EngineRollbackTransaction(rollback).ok,
            "complete retry could not release names through actual rollback");
  }
#if defined(SB_IDENTITY_TEST_WRAP_IO)
  const auto begin_identity = [&](const std::string& name) {
    api::EngineBeginTransactionRequest begin_request;
    begin_request.context = Context(database_path, database_uuid);
    const auto begin = api::EngineBeginTransaction(begin_request);
    Require(begin.ok, "I/O qualification transaction did not begin");
    api::EngineCreateIdentityRequest identity;
    identity.context = Context(database_path, database_uuid);
    identity.context.local_transaction_id = begin.local_transaction_id;
    identity.context.transaction_uuid = begin.transaction_uuid;
    identity.context.snapshot_visible_through_local_transaction_id = begin.local_transaction_id;
    identity.target_object.uuid = NewUuid(UuidKind::principal);
    identity.localized_names.push_back(Name(name));
    identity.option_envelopes = {"identity_kind:user", "principal_name:" + name};
    return identity;
  };
  const auto arm = [&](PublicationIoFault fault) {
    Require(::stat(database_path.parent_path().c_str(), &publication_directory) == 0,
            "test publication directory identity unavailable");
    publication_test_path = database_path.string();
    inventory_journal_syncs = name_syncs = 0;
    inventory_directory_pending = api_directory_pending = name_directory_pending = publication_fault_hit = false;
    publication_io_fault = fault;
  };
  auto calibration = begin_identity("sync_calibration");
  arm(PublicationIoFault::none);
  const auto calibrated = api::EngineCreateIdentity(calibration);
  const auto calibrated_publications = inventory_journal_syncs;
  last_name_sync = name_syncs;
  publication_test_path.clear();
  Require(calibrated.ok && calibrated_publications == 4 && last_name_sync > 1,
          "actual publication synchronization calibration failed");
  api::EngineRollbackTransactionRequest calibration_rollback;
  calibration_rollback.context = calibration.context;
  Require(api::EngineRollbackTransaction(calibration_rollback).ok,
          "synchronization calibration rollback failed");
  unsigned sequence = 0;
  for (const auto fault : {PublicationIoFault::api_sync, PublicationIoFault::api_directory_sync,
           PublicationIoFault::name_sync, PublicationIoFault::name_directory_sync,
           PublicationIoFault::mark_directory_sync, PublicationIoFault::release_directory_sync,
           PublicationIoFault::mark_ack,
           PublicationIoFault::release_ack, PublicationIoFault::allocation_after_mark,
           PublicationIoFault::allocation_after_release}) {
    const auto name = "sync_fault_" + std::to_string(sequence++);
    const auto identity = begin_identity(name);
    arm(fault);
    api::EngineCreateIdentityResult failed;
    bool allocation_failed = false;
    try {
      failed = api::EngineCreateIdentity(identity);
    } catch (const std::bad_alloc&) {
      allocations_before_failure = -1;
      allocation_failed = true;
    } catch (...) {
      allocations_before_failure = -1;
      publication_io_fault = PublicationIoFault::none;
      publication_test_path.clear();
      throw;
    }
    allocations_before_failure = -1;
    publication_io_fault = PublicationIoFault::none;
    publication_test_path.clear();
    const bool completed = fault == PublicationIoFault::allocation_after_release;
    Require(publication_fault_hit && failed.ok == completed &&
                allocation_failed == (fault == PublicationIoFault::allocation_after_mark),
            "real publication I/O/allocation cut did not produce its exact failure");
    const auto inventory = db::AcquireStrongLocalTransactionInventorySnapshot(database_path.string());
    Require(inventory.ok(), "publication failure inventory readback failed");
    const auto entry = scratchbird::transaction::mga::LookupLocalTransaction(inventory.snapshot->inventory,
        scratchbird::transaction::mga::MakeLocalTransactionId(identity.context.local_transaction_id));
    const bool released = fault == PublicationIoFault::release_ack ||
        fault == PublicationIoFault::release_directory_sync || completed;
    Require(entry.ok() && entry.entry.rollback_only != released,
            "I/O failure reported an incorrect native COMMIT-exclusion outcome");
    if (!allocation_failed && !completed) {
      Require(!failed.diagnostics.empty(), "publication failure lost its typed diagnostic");
      const auto& fields = failed.diagnostics.front().fields;
      Require(std::any_of(fields.begin(), fields.end(), [&](const auto& field) {
          return field.key == "observed_native_commit_exclusion" &&
              field.value == (released ? "absent" : "retained");
        }),
          "publication diagnostic disagrees with reopened native inventory");
    }
    api::EngineCommitTransactionRequest commit;
    commit.context = identity.context;
    if (released) {
      const auto state = api::LoadSecurityPrincipalLifecycleState(identity.context);
      Require(state.ok && std::any_of(state.state.principals.begin(), state.state.principals.end(),
          [&](const auto& row) { return row.principal_uuid == identity.target_object.uuid; }) &&
              VisibleSchemaPaths(identity.context).count("users." + name) == 1,
          "lost release acknowledgement concealed an actually complete publication");
      Require(api::EngineCommitTransaction(commit).ok,
              "completed release acknowledgement loss was misclassified as retained exclusion");
    } else {
      Require(!api::EngineCommitTransaction(commit).ok,
              "publication I/O/allocation failure permitted COMMIT");
      api::EngineRollbackTransactionRequest rollback;
      rollback.context = identity.context;
      Require(api::EngineRollbackTransaction(rollback).ok,
              "publication I/O/allocation failure could not roll back");
      const auto observer = Context(database_path, database_uuid);
      const auto state = api::LoadSecurityPrincipalLifecycleState(observer);
      Require(state.ok && std::none_of(state.state.principals.begin(), state.state.principals.end(),
          [&](const auto& row) { return row.principal_uuid == identity.target_object.uuid; }) &&
              VisibleSchemaPaths(observer).count("users." + name) == 0,
          "rollback retained failed identity or home-schema visibility");
    }
  }
#endif
}

}  // namespace

int main() {
  try {
#if defined(SB_IDENTITY_TEST_WRAP_IO)
  for (const std::size_t alignment : {1, 2, 4, 8, 16, 64}) {
    const auto aligned = static_cast<std::align_val_t>(alignment);
    void* bytes = ::operator new(3, aligned);
    const bool valid = reinterpret_cast<std::uintptr_t>(bytes) % alignment == 0;
    ::operator delete(bytes, aligned);
    Require(valid, "disabled allocation injector rejected a valid alignment");
    allocations_before_failure = 0;
    bool refused = false;
    try { bytes = ::operator new(3, aligned); }
    catch (const std::bad_alloc&) { refused = true; }
    allocations_before_failure = -1;
    if (!refused) ::operator delete(bytes, aligned);
    Require(refused, "enabled allocation injector did not refuse the exact allocation");
  }
#endif
  ConfigureMemoryFixture();
  scratchbird::tests::OwnedTempDirectory temporary;
  const auto database_path = temporary.path() / "schema-bootstrap.sbdb";
  const auto database_uuid = CreateDatabase(database_path);
  auto context = Context(database_path, database_uuid);
  RequireBootstrapSchemas(VisibleSchemaPaths(context));
  RequireBootstrapCatalogRowsPersisted(database_path);
  CreateUserThroughEnginePolicy(database_path, database_uuid);
  RequireCleanShutdownWritesFinalTransaction(database_path);
  RequireIdentityFailureExclusion(temporary.path() / "identity-failure.sbdb");

  temporary.Cleanup();
  std::cout << "sbsql_database_create_schema_bootstrap_gate=passed\n";
  return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
