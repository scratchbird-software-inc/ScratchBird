// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "catalog/schema_tree_api.hpp"
#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "uuid.hpp"

#include <chrono>
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

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
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

std::string NewUuid(UuidKind kind) {
  static std::uint64_t sequence = 0;
  const auto seed = CurrentUnixMillis() + (++sequence);
  if (kind == UuidKind::session) {
    const auto generated = uuid::GenerateCompatibilityUnixTimeV7(seed);
    Require(generated.ok(), "test UUID generation failed");
    return uuid::UuidToString(generated.value);
  }
  const auto generated = uuid::GenerateEngineIdentityV7(kind, seed);
  Require(generated.ok(), "test UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

void AddAdminGrant(api::EngineRequestContext* context,
                   std::string right,
                   std::string grant_uuid) {
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = std::move(grant_uuid);
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.right = std::move(right);
  grant.security_epoch = context->security_epoch;
  context->authorization_context.grants.push_back(std::move(grant));
}

void ConfigureAuthorizationContext(api::EngineRequestContext* context) {
  context->authorization_context.present = true;
  context->authorization_context.authority_uuid.canonical =
      "019e0b21-5eed-7000-8000-00000000a001";
  context->authorization_context.principal_uuid = context->principal_uuid;
  context->authorization_context.security_epoch = context->security_epoch;
  context->authorization_context.policy_epoch = context->resource_epoch;
  context->authorization_context.catalog_generation_id =
      context->catalog_generation_id;
  context->authorization_context.effective_subjects.push_back(
      {context->principal_uuid, "principal"});
  AddAdminGrant(context,
                "SEC_IDENTITY_ADMIN",
                "019e0b21-5eed-7000-8000-00000000a101");
}

api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                  const std::string& database_uuid) {
  static const std::string gate_principal_uuid = NewUuid(UuidKind::principal);
  static const std::string gate_session_uuid = NewUuid(UuidKind::session);
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "sbsql-database-create-schema-bootstrap-gate";
  context.database_path = database_path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = gate_principal_uuid;
  context.session_uuid.canonical = gate_session_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("group:SEC");
  ConfigureAuthorizationContext(&context);
  return context;
}

sblr::SblrOperationEnvelope Envelope(std::string operation_id, std::string opcode) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id), std::move(opcode), "sbsql.schema_bootstrap_gate");
  envelope.parser_package_uuid = "019e0b21-5eed-7000-8000-000000000010";
  envelope.registry_snapshot_uuid = "019e0b21-5eed-7000-8000-000000000011";
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  return envelope;
}

sblr::SblrDispatchResult Dispatch(std::string operation_id,
                                  std::string opcode,
                                  api::EngineRequestContext context,
                                  api::EngineApiRequest request = {},
                                  bool requires_transaction = false) {
  auto envelope = Envelope(operation_id, std::move(opcode));
  envelope.requires_transaction_context = requires_transaction;
  request.context = context;
  request.operation_id = operation_id;
  sblr::SblrDispatchRequest dispatch;
  dispatch.context = std::move(context);
  dispatch.envelope = std::move(envelope);
  dispatch.api_request = std::move(request);
  auto result = sblr::DispatchSblrOperation(dispatch);
  if (!result.accepted || !result.envelope_validated || !result.dispatched_to_api || !result.api_result.ok) {
    std::cerr << "dispatch failed for " << operation_id << '\n'
              << sblr::SerializeSblrDispatchResultToJson(result);
    std::exit(EXIT_FAILURE);
  }
  return result;
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_schema_bootstrap_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

std::string CreateDatabase(const std::filesystem::path& database_path) {
  const auto now = CurrentUnixMillis();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(), "test UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = now;
#ifdef SB_BOOTSTRAP_SEED_PACK_ROOT
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.allow_minimal_resource_bootstrap = false;
  create.require_resource_seed_pack = true;
#else
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
#endif
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  return uuid::UuidToString(database_uuid.value.value);
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
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "metric_descriptor") > 100,
          "metric descriptor seed rows were not persisted at database create");
  Require(TypedKindCount(opened.state.typed_catalog_record_kinds, "metric_current_value") >= 4,
          "initial metric current-value seed rows were not persisted at database create");

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
  for (const auto& schema : api::VisibleSchemaTreeRecords(context, context.local_transaction_id)) {
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
                                   const std::string& database_uuid) {
  const auto begin = Dispatch("transaction.begin", "SBLR_TRANSACTION_BEGIN", Context(database_path, database_uuid));
  Require(begin.api_result.local_transaction_id == 3,
          "first user transaction did not begin after bootstrap tx1 and activation tx2");
  auto context = Context(database_path, database_uuid);
  context.local_transaction_id = begin.api_result.local_transaction_id;
  context.transaction_uuid = begin.api_result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = begin.api_result.local_transaction_id;

  api::EngineApiRequest identity;
  identity.target_object.uuid.canonical = NewUuid(UuidKind::principal);
  identity.target_object.object_kind = "security_identity";
  identity.localized_names.push_back(Name("benchmark_user"));
  identity.option_envelopes.push_back("identity_kind:user");
  identity.option_envelopes.push_back("principal_name:benchmark_user");
  auto created = Dispatch("security.create_identity",
                          "SBLR_SECURITY_CREATE_IDENTITY",
                          context,
                          std::move(identity),
                          true);
  bool saw_home_schema = false;
  bool saw_home_schema_path = false;
  std::string home_schema_uuid;
  for (const auto& evidence : created.api_result.evidence) {
    if (evidence.evidence_kind == "home_schema" && !evidence.evidence_id.empty()) {
      saw_home_schema = true;
      home_schema_uuid = evidence.evidence_id;
    }
    if (evidence.evidence_kind == "home_schema_path" && evidence.evidence_id == "users.benchmark_user") {
      saw_home_schema_path = true;
    }
  }
  Require(saw_home_schema, "security.create_identity did not return generated home schema UUID evidence");
  Require(saw_home_schema_path, "security.create_identity did not return home schema path evidence");

  (void)Dispatch("transaction.commit", "SBLR_TRANSACTION_COMMIT", context, {}, true);

  const auto paths = VisibleSchemaPaths(context);
  Require(paths.count("users.benchmark_user") == 1, "adduser policy did not create users.benchmark_user home schema");
  const auto home_schema = api::FindVisibleSchemaTreeRecord(context, home_schema_uuid, context.local_transaction_id);
  Require(home_schema.has_value(), "returned home schema UUID was not visible after commit");
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

}  // namespace

int main() {
  ConfigureMemoryFixture();
  const auto database_path = TestDatabasePath();
  const std::string database_uuid = CreateDatabase(database_path);
  auto context = Context(database_path, database_uuid);
  RequireBootstrapSchemas(VisibleSchemaPaths(context));
  RequireBootstrapCatalogRowsPersisted(database_path);
  CreateUserThroughEnginePolicy(database_path, database_uuid);
  RequireCleanShutdownWritesFinalTransaction(database_path);

  std::error_code ignored;
  std::filesystem::remove(database_path, ignored);
  std::filesystem::remove(database_path.string() + ".sb.api_events", ignored);
  std::filesystem::remove(database_path.string() + ".sb.crud_events", ignored);
  std::filesystem::remove(database_path.string() + ".sb.name_events", ignored);
  std::cout << "sbsql_database_create_schema_bootstrap_gate=passed\n";
  return EXIT_SUCCESS;
}
