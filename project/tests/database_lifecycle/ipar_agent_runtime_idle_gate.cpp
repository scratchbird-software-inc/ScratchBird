// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "engine_host.hpp"
#include "server_agent_runtime.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

platform::u64 TimeSeed() {
  return static_cast<platform::u64>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1900000000000ull + salt);
  Require(generated.ok(), "IPAR agent runtime UUID generation failed");
  return generated.value;
}

std::string UuidText(const platform::TypedUuid& typed) {
  return uuid::UuidToString(typed.value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  platform::TypedUuid database_uuid;
  platform::TypedUuid filespace_uuid;

  ~Fixture() {
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

Fixture MakeFixture() {
  const auto seed = TimeSeed();
  Fixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_agent_runtime_idle_" +
                 std::to_string(seed));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_agent_runtime_idle.sbdb";
  fixture.database_uuid = NewUuid(platform::UuidKind::database, seed + 1);
  fixture.filespace_uuid = NewUuid(platform::UuidKind::filespace, seed + 2);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.creation_unix_epoch_millis = 1900000000000ull + seed + 3;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR agent runtime database create failed");
  return fixture;
}

server::HostedEngineState HostedState(const Fixture& fixture) {
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_path = fixture.database_path.string();
  database.database_uuid = UuidText(fixture.database_uuid);
  database.filespace_uuid = UuidText(fixture.filespace_uuid);
  database.database_created = true;
  database.database_open = true;
  database.write_admission_fenced = false;
  database.config_policy_security_lifecycle_present = true;
  database.selected_agent_type_ids = {
      "page_allocation_manager",
      "filespace_capacity_manager",
  };

  server::HostedEngineState state;
  state.engine_context_active = true;
  state.databases.push_back(std::move(database));
  return state;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

void DumpDiagnostics(const std::vector<server::ServerDiagnostic>& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << "  " << field.key << '=' << field.value << '\n';
    }
  }
}

bool Contains(std::string_view text, std::string_view needle) {
  return text.find(needle) != std::string_view::npos;
}

server::ServerAgentRuntimeSnapshot WaitForSnapshot(
    server::ServerAgentRuntime* runtime,
    std::chrono::milliseconds timeout,
    const std::function<bool(const server::ServerAgentRuntimeSnapshot&)>& predicate,
    std::string_view message) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  server::ServerAgentRuntimeSnapshot snapshot;
  while (std::chrono::steady_clock::now() < deadline) {
    snapshot = runtime->Snapshot();
    if (predicate(snapshot)) {
      return snapshot;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  snapshot = runtime->Snapshot();
  if (!predicate(snapshot)) {
    Fail(message);
  }
  return snapshot;
}

std::uint64_t SumWorkerTicks(
    const std::vector<server::ServerAgentRuntimeWorkerSnapshot>& workers) {
  std::uint64_t total = 0;
  for (const auto& worker : workers) {
    total += worker.ticks;
  }
  return total;
}

void RequireFairWorkerProgress(
    const server::ServerAgentRuntimeSnapshot& snapshot,
    std::uint64_t expected_workers) {
  Require(snapshot.workers.size() == expected_workers,
          "IPAR agent runtime snapshot lost worker evidence");
  Require(snapshot.scheduled_worker_count == expected_workers,
          "IPAR agent runtime did not schedule every selected worker");
  Require(snapshot.total_worker_ticks == SumWorkerTicks(snapshot.workers),
          "IPAR agent runtime aggregate worker ticks drifted");
  for (const auto& worker : snapshot.workers) {
    Require(worker.ticks > 0,
            "IPAR agent runtime left a selected worker unscheduled");
  }
  Require(snapshot.starvation_events == 0,
          "IPAR agent runtime recorded starvation events");
  Require(snapshot.max_worker_ticks >= snapshot.min_worker_ticks,
          "IPAR agent runtime fairness min/max tick evidence inverted");
  Require(snapshot.max_worker_ticks - snapshot.min_worker_ticks <= 1,
          "IPAR agent runtime allowed one worker to monopolize scheduler ticks");
}

void TestLiveRuntimeIdleAndStop() {
  auto fixture = MakeFixture();

  server::ServerBootstrapConfig config;
  config.control_dir = fixture.dir / "control";
  config.embedded_direct_mode = false;

  server::ServerAgentRuntime runtime;
  std::vector<server::ServerDiagnostic> diagnostics;
  const bool started = runtime.Start(config, HostedState(fixture), &diagnostics);
  if (!started) { DumpDiagnostics(diagnostics); }
  Require(started, "IPAR agent runtime failed to start");

  const auto initial = runtime.Snapshot();
  Require(initial.started, "IPAR agent runtime initial snapshot not started");
  Require(!initial.stopping, "IPAR agent runtime initial snapshot marked stopping");
  Require(initial.worker_thread_count == 2,
          "IPAR agent runtime did not honor bounded selected worker count");
  Require(initial.background_worker_slots == 2,
          "IPAR agent runtime background worker slot count drifted");
  Require(initial.foreground_reserved_capacity >= 1,
          "IPAR agent runtime did not reserve foreground capacity");
  Require(initial.worker_wake_policy == "staggered_worker_per_scheduler_tick",
          "IPAR agent runtime wake policy drifted");

  std::this_thread::sleep_for(std::chrono::milliseconds(4500));

  const auto idle = runtime.Snapshot();
  Require(idle.started, "IPAR agent runtime stopped unexpectedly");
  Require(idle.scheduler_ticks > 0,
          "IPAR agent runtime scheduler never ticked");
  Require(idle.scheduler_ticks <= 3,
          "IPAR agent runtime scheduler exceeded bounded idle tick budget");
  Require(idle.total_worker_ticks <= idle.scheduler_ticks,
          "IPAR agent runtime woke more than one worker per scheduler tick");
  Require(idle.durable_lease_count >= 1,
          "IPAR agent runtime did not acquire durable worker leases");
  Require(idle.total_actions_accepted >= 1,
          "IPAR agent runtime did not perform a live bounded agent action");
  RequireFairWorkerProgress(idle, 2);

  const auto idle_source = server::BuildIparAgentLifecycleProjectionSource(idle);
  Require(idle_source.lifecycle_state == "running",
          "IPAR agent lifecycle source did not report running");
  Require(idle_source.idle_state == "idle_resident",
          "IPAR agent lifecycle source did not report idle resident");
  Require(idle_source.worker_thread_count == 2,
          "IPAR agent lifecycle source lost worker count");
  Require(idle_source.scheduler_ticks == idle.scheduler_ticks,
          "IPAR agent lifecycle source lost scheduler tick evidence");
  Require(idle_source.scheduled_worker_count == 2 &&
              idle_source.starvation_events == 0,
          "IPAR agent lifecycle source lost fairness evidence");

  runtime.Stop();
  const auto stopped = runtime.Snapshot();
  Require(!stopped.started, "IPAR agent runtime remained started after Stop");
  Require(!stopped.stopping, "IPAR agent runtime retained stopping flag after Stop");
  const auto stopped_source =
      server::BuildIparAgentLifecycleProjectionSource(stopped);
  Require(stopped_source.lifecycle_state == "stopped",
          "IPAR agent lifecycle source did not report stopped after Stop");
  Require(stopped_source.idle_state == "idle",
          "IPAR agent lifecycle source did not report idle after Stop");

  const auto status_json = ReadFile(config.control_dir / "sb_server.agent_runtime.json");
  Require(status_json.find("\"started\":false") != std::string::npos,
          "IPAR agent runtime status file did not record stopped state");
  Require(status_json.find("\"stopping\":false") != std::string::npos,
          "IPAR agent runtime status file did not clear stopping state");
}

void TestFailureDiagnosticsAndFairness() {
  auto fixture = MakeFixture();

  server::ServerBootstrapConfig config;
  config.control_dir = fixture.dir / "control";
  config.embedded_direct_mode = false;
  config.server_agent_runtime_test_options = {
      "server_agent_runtime_test.fail_once:page_allocation_manager:"
      "page_preallocation_request:IPAR.TEST.AGENT_ACTION_FAILED:"
      "bounded_page_preallocation_failure",
      "server_agent_runtime_test.refuse_once:filespace_capacity_manager:"
      "filespace_growth_request:IPAR.TEST.AGENT_ACTION_REFUSED:"
      "bounded_filespace_growth_refusal",
  };

  server::ServerAgentRuntime runtime;
  std::vector<server::ServerDiagnostic> diagnostics;
  const bool started = runtime.Start(config, HostedState(fixture), &diagnostics);
  if (!started) { DumpDiagnostics(diagnostics); }
  Require(started, "IPAR failure/fairness runtime failed to start");

  const auto observed = WaitForSnapshot(
      &runtime,
      std::chrono::milliseconds(7000),
      [](const server::ServerAgentRuntimeSnapshot& snapshot) {
        return snapshot.scheduler_ticks >= 2 &&
               snapshot.total_actions_failed == 1 &&
               snapshot.total_actions_refused == 1;
      },
      "IPAR agent runtime did not record bounded failure/refusal evidence");

  Require(observed.started,
          "IPAR agent runtime stopped during failure diagnostic proof");
  Require(observed.foreground_reserved_capacity >= 1,
          "IPAR agent runtime lost foreground reserved capacity");
  Require(observed.total_actions_accepted == 0,
          "IPAR test hooks did not consume the first bounded agent actions");
  Require(observed.total_actions_failed == 1,
          "IPAR agent runtime failed-action counter missing");
  Require(observed.total_actions_refused == 1,
          "IPAR agent runtime refused-action counter missing");
  Require(observed.last_diagnostic_code == "IPAR.TEST.AGENT_ACTION_REFUSED",
          "IPAR agent runtime did not expose latest fail-closed diagnostic");
  Require(observed.last_diagnostic_outcome == "refused",
          "IPAR agent runtime did not expose fail-closed diagnostic outcome");
  Require(observed.last_diagnostic_action == "filespace_growth_request",
          "IPAR agent runtime did not expose diagnostic action evidence");
  RequireFairWorkerProgress(observed, 2);

  bool saw_failed_worker = false;
  bool saw_refused_worker = false;
  for (const auto& worker : observed.workers) {
    if (worker.agent_type_id == "page_allocation_manager") {
      saw_failed_worker =
          worker.actions_failed == 1 &&
          worker.last_action == "page_preallocation_request" &&
          worker.last_action_outcome == "failed_closed" &&
          worker.last_diagnostic_code == "IPAR.TEST.AGENT_ACTION_FAILED";
    }
    if (worker.agent_type_id == "filespace_capacity_manager") {
      saw_refused_worker =
          worker.actions_refused == 1 &&
          worker.last_action == "filespace_growth_request" &&
          worker.last_action_outcome == "refused" &&
          worker.last_diagnostic_code == "IPAR.TEST.AGENT_ACTION_REFUSED";
    }
  }
  Require(saw_failed_worker,
          "IPAR agent runtime did not preserve per-worker failure evidence");
  Require(saw_refused_worker,
          "IPAR agent runtime did not preserve per-worker refusal evidence");

  const auto source =
      server::BuildIparAgentLifecycleProjectionSource(observed);
  Require(source.total_actions_failed == 1 &&
              source.total_actions_refused == 1,
          "IPAR lifecycle source lost failure/refusal counters");
  Require(source.last_diagnostic_code == "IPAR.TEST.AGENT_ACTION_REFUSED",
          "IPAR lifecycle source lost last diagnostic code");
  Require(source.last_diagnostic_detail == "bounded_filespace_growth_refusal",
          "IPAR lifecycle source lost last diagnostic detail");
  Require(source.scheduled_worker_count == 2 &&
              source.starvation_events == 0,
          "IPAR lifecycle source lost fairness/starvation proof");

  runtime.Stop();
  const auto stopped = runtime.Snapshot();
  Require(!stopped.started,
          "IPAR failure/fairness runtime remained started after Stop");

  const auto status_json = ReadFile(config.control_dir / "sb_server.agent_runtime.json");
  Require(Contains(status_json, "\"total_actions_failed\":1"),
          "IPAR status JSON did not record failed action count");
  Require(Contains(status_json, "\"total_actions_refused\":1"),
          "IPAR status JSON did not record refused action count");
  Require(Contains(status_json,
                   "\"last_diagnostic_code\":\"IPAR.TEST.AGENT_ACTION_REFUSED\""),
          "IPAR status JSON did not record latest diagnostic code");
  Require(Contains(status_json, "\"starvation_events\":0"),
          "IPAR status JSON did not record starvation proof");
}

}  // namespace

int main() {
  TestLiveRuntimeIdleAndStop();
  TestFailureDiagnosticsAndFairness();
  return EXIT_SUCCESS;
}
