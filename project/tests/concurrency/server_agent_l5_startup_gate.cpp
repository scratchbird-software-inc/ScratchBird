// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// L5 startup on a database created the way the production bootstrap creates it
// (owner, no caller-supplied service principal), opened by the server path:
// StartHostedEngine, then StartCoordinator.
//
// SEARCH_KEY: SB_L5_PRINCIPAL_ALWAYS_PROVISIONED, SB_COORDINATOR_STARTS_COMMITTED_AGENTS,
// SB_AGENT_PRINCIPAL_BINDING_BY_UUID, SB_AGENT_SELF_ACTION_RIGHTS
//
// Modes, one per process (a completed open binds the process):
//   --committed-agents  L5 starts the committed agents, each as its own principal
//   --no-l5             L5's rows removed: the coordinator refuses and starts nothing
//   --unbound-agent     one agent's link removed: only that agent is refused
//   --rights-missing    an agent lacking its own-action rights is refused
// Every mode removes its whole directory at exit, including on failure.

#include "catalog_page.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_security_record_codec.hpp"
#include "catalog_runtime_authority_binding.hpp"
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "engine_host.hpp"
#include "memory.hpp"
#include "page_manager.hpp"
#include "server_agent_runtime.hpp"
#include "time.hpp"
#include "uuid.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "../support/engine_statement_fixture.hpp"

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;

std::filesystem::path g_directory;

void RemoveDirectory() {
  if (g_directory.empty()) return;
  std::error_code ignored;
  std::filesystem::remove_all(g_directory, ignored);
}

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "FAIL server_agent_l5_startup: " << message << '\n';
  std::exit(EXIT_FAILURE);  // RemoveDirectory runs through atexit.
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

std::vector<page::CatalogPageRow> ReadCatalogRows(const std::filesystem::path& path,
                                                  std::uint32_t page_size) {
  disk::FileDevice device;
  Require(device.Open(path.string(), disk::FileOpenMode::open_existing_read_only).ok(),
          "catalog open failed");
  std::vector<page::CatalogPageRow> rows;
  std::set<platform::u64> visited;
  platform::u64 page_number = db::kCatalogPageNumber;
  while (page_number != 0) {
    Require(visited.insert(page_number).second, "catalog chain cycle");
    const auto offset = page::CheckedPageBodyOffset(page_size, page_number,
                                                    disk::kPageHeaderSerializedBytes);
    Require(offset.ok(), "catalog body offset failed");
    std::vector<platform::byte> body(page_size - disk::kPageHeaderSerializedBytes, 0);
    Require(device.ReadAt(offset.offset, body.data(), body.size()).ok(), "catalog read failed");
    const auto parsed = page::ParseCatalogPageBody(body, page_number);
    Require(parsed.ok(), "catalog parse failed");
    rows.insert(rows.end(), parsed.body.rows.begin(), parsed.body.rows.end());
    page_number = parsed.body.next_page_number;
  }
  Require(device.Close().ok(), "catalog close failed");
  return rows;
}

void WriteCatalogRows(const std::filesystem::path& path, std::uint32_t page_size,
                      const std::vector<page::CatalogPageRow>& rows) {
  const auto page_set = page::BuildCatalogPageSet(rows, page_size, db::kCatalogPageNumber,
                                                  db::kCatalogOverflowFirstPageNumber);
  Require(page_set.ok(), "catalog rebuild failed");
  disk::FileDevice device;
  Require(device.Open(path.string(), disk::FileOpenMode::open_existing).ok(),
          "catalog write open failed");
  for (const auto& catalog_page : page_set.pages) {
    const auto offset = page::CheckedPageBodyOffset(page_size, catalog_page.page_number,
                                                    disk::kPageHeaderSerializedBytes);
    Require(offset.ok(), "catalog write offset failed");
    Require(device.WriteAt(offset.offset, catalog_page.body.data(), catalog_page.body.size()).ok(),
            "catalog write failed");
  }
  Require(device.Sync().ok(), "catalog sync failed");
  Require(device.Close().ok(), "catalog close failed");
}

// Removes every row the predicate selects; returns how many.
std::size_t StripRows(const std::filesystem::path& path, std::uint32_t page_size,
                      const std::function<bool(const catalog::CatalogTypedRecord&)>& strip) {
  std::vector<page::CatalogPageRow> kept;
  std::size_t removed = 0;
  for (const auto& row : ReadCatalogRows(path, page_size)) {
    if (row.kind == page::CatalogPageRowKind::typed_catalog_record) {
      const auto outer = catalog::DecodeCatalogTypedRecord(row);
      Require(outer.ok(), "catalog record decode failed");
      if (strip(outer.record)) { ++removed; continue; }
    }
    kept.push_back(row);
  }
  WriteCatalogRows(path, page_size, kept);
  return removed;
}

std::optional<std::string> SecurityText(const catalog::CatalogTypedRecord& record,
                                        std::string_view key) {
  if (!catalog::IsCatalogSecurityRecordKind(record.header.kind)) return std::nullopt;
  const auto security = catalog::DecodeCatalogSecurityRecord(record.header.kind, record.payload);
  if (!security.ok()) return std::nullopt;
  const auto value = security.record->Attribute(key);
  return value ? std::optional<std::string>(std::string(*value)) : std::nullopt;
}

platform::Uuid SecurityIdentity(const catalog::CatalogTypedRecord& record, std::string_view key) {
  const auto security = catalog::DecodeCatalogSecurityRecord(record.header.kind, record.payload);
  return security.ok() ? security.record->Identity(key) : platform::Uuid{};
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2, "usage: server_agent_l5_startup_gate <mode>");
  const std::string mode = argv[1];
  Require(mode == "--committed-agents" || mode == "--no-l5" || mode == "--unbound-agent" ||
              mode == "--rights-missing",
          "unknown mode");

  std::string pattern = (std::filesystem::temp_directory_path() / "sb-l5-startup-XXXXXX").string();
  Require(mkdtemp(pattern.data()) != nullptr, "fixture directory create failed");
  g_directory = pattern;
  std::atexit(RemoveDirectory);

  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
              scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(),
              "server_agent_l5_startup_gate").ok(),
          "fixture memory manager configuration failed");
  const auto clock = scratchbird::core::time::ReadLocalNodeClockSnapshot();
  Require(clock.ok(), "node clock unavailable");
  const auto millis = scratchbird::core::time::WallClockToUuidV7Millis(clock.value.wall_clock);
  Require(millis.ok(), "UUID clock conversion failed");

  // Created as the production bootstrap tool creates: an owner, and no
  // caller-supplied internal service principal. L5 is written regardless.
  db::DatabaseCreateConfig create;
  create.path = (g_directory / "l5.sbdb").string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(platform::UuidKind::database, millis.unix_epoch_millis).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(platform::UuidKind::filespace, millis.unix_epoch_millis + 1).value;
  create.creation_unix_epoch_millis = millis.unix_epoch_millis;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) std::cerr << created.diagnostic.diagnostic_code << '\n';
  Require(created.ok() && created.creation_ready_for_handoff(), "production-style create failed");
  const auto page_size = created.state.header.page_size;

  const auto committed = db::ReadDatabaseBootstrapSecurityCatalog(create.path);
  Require(committed.ok() && committed.state.runtime_authority_binding_present,
          "create did not commit L5's binding");
  const auto l5 = committed.state.service_principal_uuid;
  Require(!l5.is_nil() && committed.state.l5_principal_uuid.value == l5,
          "the committed L5 principal is not the binding's service principal");
  std::map<platform::Uuid, platform::Uuid> principal_by_policy;
  for (const auto& link : committed.state.committed_agent_principal_bindings)
    principal_by_policy[link.policy_uuid] = link.principal_uuid;
  std::map<std::string, std::vector<platform::Uuid>> policies_by_type;
  for (const auto& policy : committed.state.committed_agent_policies)
    if (policy.primary) policies_by_type[policy.agent_type].push_back(policy.policy_uuid);
  Require(!principal_by_policy.empty(), "create wrote no agent principal links");

  if (mode == "--no-l5") {
    // Remove L5 entirely: its principal, its SYSARCH membership, its grants
    // and its runtime authority binding.
    const auto removed = StripRows(create.path, page_size, [&](const catalog::CatalogTypedRecord& r) {
      if (catalog::CatalogRuntimeAuthorityBindingMatchesHeader(r)) return true;
      if (r.header.kind == catalog::CatalogRecordKind::user_account)
        return r.header.object_uuid.value == l5;
      if (r.header.kind == catalog::CatalogRecordKind::grant_record)
        return SecurityIdentity(r, "member_uuid") == l5 || SecurityIdentity(r, "subject_uuid") == l5;
      return false;
    });
    Require(removed >= 3, "fixture did not remove L5's rows");
  }
  if (mode == "--rights-missing") {
    // The owner explicitly denies alert_manager its operating right, through
    // the engine's own grant API, committed before the server opens the file.
    // (Removing a policy-pack grant from the file is refused at open as pack
    // tampering, so the right is denied rather than deleted.) This also
    // proves a runtime deny overrides a create-time grant.
    namespace api = scratchbird::engine::internal_api;
    const auto& alert_policies = policies_by_type["alert_manager"];
    Require(!alert_policies.empty() && principal_by_policy.contains(alert_policies.front()),
            "fixture found no committed alert_manager principal");
    auto owner = scratchbird::tests::BootstrapFixtureOwnerContext(create);
    api::EngineBeginTransactionRequest begin;
    begin.context = owner;
    begin.isolation_level = "read_committed";
    const auto begun = api::EngineBeginTransaction(begin);
    Require(begun.ok, "owner transaction did not begin");
    owner.transaction_uuid = begun.transaction_uuid;
    owner.local_transaction_id = begun.local_transaction_id;
    owner.snapshot_visible_through_local_transaction_id =
        begun.snapshot_visible_through_local_transaction_id;
    owner.transaction_isolation_level = begun.isolation_level;
    api::EngineSecurityGrantPrivilegeRequest deny;
    deny.context = owner;
    deny.grant_uuid = api::EngineUuid{
        uuid::GenerateEngineIdentityV7(platform::UuidKind::object, millis.unix_epoch_millis + 7).value.value};
    deny.grantee_uuid = api::EngineUuid{principal_by_policy[alert_policies.front()]};
    deny.grantee_kind = "principal";
    deny.privilege = "OBS_ALERT_INTERNAL";
    deny.grant_effect = "deny";
    const auto denied = api::EngineSecurityGrantPrivilege(deny);
    if (!denied.ok && !denied.diagnostics.empty())
      std::cerr << denied.diagnostics.front().code << ':' << denied.diagnostics.front().detail << '\n';
    Require(denied.ok, "owner could not deny the alert right");
    api::EngineCommitTransactionRequest commit;
    commit.context = owner;
    Require(api::EngineCommitTransaction(commit).ok, "owner deny did not commit");
  }
  constexpr std::string_view kUnbound = "page_allocation_manager";
  if (mode == "--unbound-agent") {
    const auto& policies = policies_by_type[std::string(kUnbound)];
    Require(!policies.empty(), "fixture agent has no committed policy");
    const std::set<platform::Uuid> targets(policies.begin(), policies.end());
    const auto removed = StripRows(create.path, page_size, [&](const catalog::CatalogTypedRecord& r) {
      return r.header.kind == catalog::CatalogRecordKind::grant_record &&
             SecurityText(r, "grant_class") == std::optional<std::string>("agent_principal_binding") &&
             targets.contains(SecurityIdentity(r, "target_uuid"));
    });
    Require(removed == policies.size(), "fixture did not remove the agent's links");
  }

  server::ServerBootstrapConfig config;
  config.control_dir = g_directory / "control";
  config.database_default_path = create.path;
  config.sbps_endpoint = config.control_dir / "ipc.sock";
  auto opened = server::StartHostedEngine(config);
  for (const auto& diagnostic : opened.diagnostics)
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
  Require(opened.state.engine_context_active && !opened.state.databases.empty(),
          "server-path open failed");

  {
    server::ServerAgentRuntime runtime;
    std::vector<server::ServerDiagnostic> diagnostics;
    const bool started = runtime.StartCoordinator(config, opened.state, &diagnostics, nullptr);
    const auto snapshot = runtime.Snapshot();

    if (mode == "--no-l5") {
      Require(!started, "the coordinator started without L5's identity");
      Require(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& d) {
                return d.code == "SERVER.AGENT_RUNTIME.L5_IDENTITY_ABSENT";
              }),
              "the refusal did not name the missing L5 identity");
      Require(snapshot.workers.empty(), "agents started without L5");
    } else {
      for (const auto& d : diagnostics) std::cerr << d.code << ':' << d.safe_message << '\n';
      Require(started, "the coordinator did not start on a production-style database");
      for (const auto& r : snapshot.refused_agents)
        std::cerr << "refused " << r.agent_type_id << ' ' << r.reason << '\n';
      Require(!snapshot.workers.empty(), "L5 started no committed agent");
      std::set<std::string> refused;
      for (const auto& r : snapshot.refused_agents) {
        Require(!r.reason.empty(), "a refused agent carries no reason");
        refused.insert(r.agent_type_id);
      }
      for (const auto& worker : snapshot.workers) {
        Require(!refused.contains(worker.agent_type_id), "a refused agent was started anyway");
        Require(!worker.policy_uuid.is_nil() && !worker.principal_uuid.is_nil(),
                "a started agent is not bound by UUID");
        const auto& type_policies = policies_by_type[worker.agent_type_id];
        Require(std::find(type_policies.begin(), type_policies.end(), worker.policy_uuid) !=
                    type_policies.end(),
                "a worker's policy is not its type's committed primary policy");
        const auto linked = principal_by_policy.find(worker.policy_uuid);
        Require(linked != principal_by_policy.end() && linked->second == worker.principal_uuid,
                "a worker does not run as the principal create linked to its policy");
        Require(worker.principal_uuid != l5 &&
                    worker.principal_uuid != committed.state.principal_uuid.value,
                "a worker runs as L5 or the owner");
      }
      if (mode == "--unbound-agent") {
        const auto it = std::find_if(snapshot.refused_agents.begin(), snapshot.refused_agents.end(),
            [&](const auto& r) { return r.agent_type_id == kUnbound; });
        Require(it != snapshot.refused_agents.end() &&
                    it->reason == "agent_principal_binding_absent",
                "an agent with no principal link was not refused for it");
      }
      if (mode == "--rights-missing") {
        // alert_manager's operating right was removed from the file above.
        const auto it = std::find_if(snapshot.refused_agents.begin(), snapshot.refused_agents.end(),
            [](const auto& r) { return r.agent_type_id == "alert_manager"; });
        Require(it != snapshot.refused_agents.end() &&
                    it->reason == "agent_rights_missing:OBS_ALERT_INTERNAL",
                "an agent lacking its own-action right was not refused for it");
      }
      if (mode == "--committed-agents") {
        // Owner, 2026-10-30: every committed agent holds its own-action right
        // from create. Only L5's own duties, which have no principal, are not
        // started as agents.
        for (const auto& r : snapshot.refused_agents)
          Require((r.agent_type_id == "startup_agent" || r.agent_type_id == "shutdown_agent" ||
                   r.agent_type_id == "agent_coordinator") &&
                      r.reason == "agent_principal_binding_absent",
                  "a committed agent was refused: " + r.agent_type_id + " " + r.reason);
      }
    }
    runtime.Stop();
  }
  opened.state = server::HostedEngineState{};

  // Nothing may stay locked or behind once the server path has released it.
  for (const auto& entry : std::filesystem::directory_iterator(g_directory)) {
    const auto name = entry.path().filename().string();
    Require(name.find(".sb.owner.lock") == std::string::npos &&
                name.find(".sb.route.owner.lock") == std::string::npos,
            "a lock file remained after the server path released the database");
  }
  std::cout << "server_agent_l5_startup" << mode << "=passed\n";
  return EXIT_SUCCESS;
}
