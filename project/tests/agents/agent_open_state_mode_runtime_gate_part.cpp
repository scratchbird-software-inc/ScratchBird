// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "engine_database_runtime.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

std::string Id(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1917019100000ull + salt);
  Require(generated.ok(), "PFAR-017A runtime UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

void RequireTypedUuid(platform::UuidKind kind,
                      std::string_view value,
                      std::string_view field_name) {
  Require(value.find(':') == std::string_view::npos,
          std::string(field_name) + " used label prefix");
  Require(value.rfind("agent.", 0) != 0 && value.rfind("policy.", 0) != 0 &&
              value.rfind("scope.", 0) != 0,
          std::string(field_name) + " used fake catalog label");
  Require(uuid::ParseDurableEngineIdentityUuid(kind, std::string(value)).ok(),
          std::string(field_name) + " is not a typed durable engine UUID");
}

db::DatabaseLifecycleState RuntimeDatabase(db::DatabaseLifecyclePhase phase,
                                           std::string_view suffix) {
  db::DatabaseLifecycleState database;
  database.path = (std::filesystem::temp_directory_path() /
                   ("pfar017a-runtime-" + std::string(suffix) + ".sbdb")).string();
  database.phase = phase;
  database.database_uuid = uuid::ParseDurableEngineIdentityUuid(
      platform::UuidKind::database,
      Id(platform::UuidKind::database, 1)).value;
  database.filespace_uuid = uuid::ParseDurableEngineIdentityUuid(
      platform::UuidKind::filespace,
      Id(platform::UuidKind::filespace, 2)).value;
  return database;
}

void RequireRuntimeMode(const api::EngineDatabaseRuntimeState& state,
                        std::string_view mode,
                        std::string_view diagnostic,
                        bool mutation_allowed,
                        bool drain_allowed,
                        bool safe_maintenance_allowed) {
  Require(state.open_state_mode == mode, "runtime open mode mismatch");
  Require(state.open_state_diagnostic_code == diagnostic,
          "runtime open diagnostic mismatch: " + state.open_state_diagnostic_code);
  Require(state.agent_inspect_allowed, "inspect was not allowed");
  Require(state.agent_mutation_allowed == mutation_allowed,
          "mutation allowance mismatch for " + std::string(mode));
  Require(state.agent_drain_allowed == drain_allowed,
          "drain allowance mismatch for " + std::string(mode));
  Require(state.safe_maintenance_allowed == safe_maintenance_allowed,
          "safe maintenance allowance mismatch for " + std::string(mode));
  Require(state.normal_background_action_loops_allowed == mutation_allowed,
          "background action loop gate mismatch for " + std::string(mode));
}

}  // namespace

void RunRuntimeOpenStateModeGatePart() {
  auto read_only = RuntimeDatabase(db::DatabaseLifecyclePhase::opened, "read-only");
  read_only.read_only_open = true;
  const auto read_only_result = api::MakeEngineDatabaseRuntimeState(read_only);
  Require(read_only_result.ok(), "read-only runtime admission failed");
  RequireRuntimeMode(read_only_result.state,
                     "read_only",
                     "SB_ENGINE_OPEN_STATE_READ_ONLY_INSPECT_ONLY",
                     false,
                     false,
                     false);

  const auto restricted = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::restricted_open, "restricted"));
  Require(restricted.ok(), "restricted runtime admission failed");
  RequireRuntimeMode(restricted.state,
                     "restricted_open",
                     "SB_ENGINE_OPEN_STATE_RESTRICTED_SAFE_ONLY",
                     false,
                     false,
                     true);

  const auto maintenance = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::maintenance, "maintenance"));
  Require(maintenance.ok(), "maintenance runtime admission failed");
  RequireRuntimeMode(maintenance.state,
                     "maintenance",
                     "SB_ENGINE_OPEN_STATE_MAINTENANCE_SAFE_ONLY",
                     false,
                     false,
                     true);

  api::EngineDatabaseRuntimeRouteAdmissionOptions route;
  route.repair_mode_active = true;
  const auto repair = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened, "repair"), {}, {}, {}, route);
  Require(repair.ok(), "repair runtime admission failed");
  RequireRuntimeMode(repair.state,
                     "repair",
                     "SB_ENGINE_OPEN_STATE_REPAIR_SAFE_ONLY",
                     false,
                     false,
                     true);

  route = {};
  route.backup_hold_active = true;
  const auto backup = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened, "backup"), {}, {}, {}, route);
  Require(backup.ok(), "backup-hold runtime admission failed");
  RequireRuntimeMode(backup.state,
                     "backup_hold",
                     "SB_ENGINE_OPEN_STATE_BACKUP_HOLD_INSPECT_ONLY",
                     false,
                     false,
                     false);

  route = {};
  route.archive_hold_active = true;
  const auto archive = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened, "archive"), {}, {}, {}, route);
  Require(archive.ok(), "archive-hold runtime admission failed");
  RequireRuntimeMode(archive.state,
                     "archive_hold",
                     "SB_ENGINE_OPEN_STATE_ARCHIVE_HOLD_INSPECT_ONLY",
                     false,
                     false,
                     false);

  route = {};
  route.shutdown_in_progress = true;
  const auto shutdown = api::MakeEngineDatabaseRuntimeState(
      RuntimeDatabase(db::DatabaseLifecyclePhase::opened, "shutdown"), {}, {}, {}, route);
  Require(shutdown.ok(), "shutdown runtime admission failed");
  RequireRuntimeMode(shutdown.state,
                     "shutdown_in_progress",
                     "SB_ENGINE_OPEN_STATE_SHUTDOWN_DRAIN_ONLY",
                     false,
                     true,
                     false);

  RequireTypedUuid(platform::UuidKind::database,
                   uuid::UuidToString(shutdown.state.database.database_uuid.value),
                   "database_uuid");
  RequireTypedUuid(platform::UuidKind::filespace,
                   uuid::UuidToString(shutdown.state.database.filespace_uuid.value),
                   "filespace_uuid");
}

int main() {
  RunRuntimeOpenStateModeGatePart();
  return EXIT_SUCCESS;
}
