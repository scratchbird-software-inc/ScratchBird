// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "server_agent_runtime.hpp"
#include "time.hpp"
#include "uuid.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <pthread.h>
#include <semaphore.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

// SEARCH_KEY: SERVER_AGENT_SHUTDOWN_PREDICATE_PARK_RACE
// Link against the static C++ runtime so GNU --wrap also observes the native
// calls made by std::condition_variable. No production state is fabricated:
// Start creates its actual service/leases and Stop joins its actual workers.
namespace {
namespace server = scratchbird::server;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

sem_t waiter_at_park;
sem_t allow_park;
sem_t stop_boundary;
sem_t stop_finished;
std::atomic<bool> armed{false};
std::atomic<bool> observed_boundary{false};
std::atomic<bool> publication_serialized{false};
pthread_mutex_t* schedule_mutex = nullptr;
pthread_cond_t* schedule_condition = nullptr;
thread_local bool stop_thread = false;

[[noreturn]] void Fail(const char* message) {
  std::cerr << message << '\n';
  std::_Exit(EXIT_FAILURE);
}

void Require(bool condition, const char* message) {
  if (!condition) { Fail(message); }
}

void Wait(sem_t& event, const char* failure) {
  timespec limit{};
  Require(clock_gettime(CLOCK_REALTIME, &limit) == 0, "clock_gettime failed");
  limit.tv_sec += 30;
  int result;
  do {
    result = sem_timedwait(&event, &limit);
  } while (result == -1 && errno == EINTR);
  Require(result == 0, failure);
}

void Signal(sem_t& event) {
  Require(sem_post(&event) == 0, "sem_post failed");
}

platform::TypedUuid NewIdentity(platform::UuidKind kind, platform::u64 millis) {
  const auto result = uuid::GenerateEngineIdentityV7(kind, millis);
  Require(result.ok(), "fixture identity generation failed");
  return result.value;
}
}  // namespace

extern "C" int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern "C" int __real_pthread_cond_broadcast(pthread_cond_t*);
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);

extern "C" int __wrap_pthread_cond_wait(pthread_cond_t* condition,
                                         pthread_mutex_t* mutex) {
  if (armed.load(std::memory_order_acquire)) {
    char name[16]{};
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) == 0 &&
        std::string_view(name) == "sb-agent-w00" && armed.exchange(false)) {
      // The predicate was false and the worker still owns schedule_mutex.
      // Freeze exactly before the atomic unlock-and-park operation.
      schedule_mutex = mutex;
      schedule_condition = condition;
      Signal(waiter_at_park);
      Wait(allow_park, "controller did not release the worker to park");
    }
  }
  return __real_pthread_cond_wait(condition, mutex);
}

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (stop_thread && mutex == schedule_mutex &&
      !observed_boundary.exchange(true)) {
    publication_serialized.store(true);
    Signal(stop_boundary);
  }
  return __real_pthread_mutex_lock(mutex);
}

extern "C" int __wrap_pthread_cond_broadcast(pthread_cond_t* condition) {
  const int result = __real_pthread_cond_broadcast(condition);
  if (stop_thread && condition == schedule_condition &&
      !observed_boundary.exchange(true)) {
    // Old implementation: Stop has already published and notified, although
    // the worker is still between its false predicate and parking.
    Signal(stop_boundary);
  }
  return result;
}

int main() {
  for (auto* event : {&waiter_at_park, &allow_park, &stop_boundary, &stop_finished}) {
    Require(sem_init(event, 0, 0) == 0, "sem_init failed");
  }

  std::string pattern = (std::filesystem::temp_directory_path() /
                         "sb-runtime-shutdown-XXXXXX").string();
  Require(mkdtemp(pattern.data()) != nullptr, "fixture directory create failed");
  const std::filesystem::path directory(pattern);
  const auto clock = scratchbird::core::time::ReadLocalNodeClockSnapshot();
  Require(clock.ok(), "node clock unavailable");
  const auto millis = scratchbird::core::time::WallClockToUuidV7Millis(clock.value.wall_clock);
  Require(millis.ok(), "UUID clock conversion failed");
  db::DatabaseCreateConfig create;
  create.path = (directory / "runtime.sbdb").string();
  create.database_uuid = NewIdentity(platform::UuidKind::database, millis.unix_epoch_millis);
  create.filespace_uuid = NewIdentity(platform::UuidKind::filespace, millis.unix_epoch_millis);
  create.creation_unix_epoch_millis = millis.unix_epoch_millis;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
    Fail("real database fixture creation failed");
  }

  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_path = create.path;
  database.database_uuid = create.database_uuid.value;
  database.filespace_uuid = create.filespace_uuid.value;
  database.database_created = true;
  database.database_open = true;
  database.write_admission_fenced = false;
  database.config_policy_security_lifecycle_present = true;
  database.selected_agent_type_ids = {"page_allocation_manager", "filespace_capacity_manager"};
  server::HostedEngineState engine;
  engine.engine_context_active = true;
  engine.databases.push_back(std::move(database));
  server::ServerBootstrapConfig config;
  config.control_dir = directory / "control";
  server::ServerAgentRuntime runtime;
  std::vector<server::ServerDiagnostic> diagnostics;

  armed.store(true, std::memory_order_release);
  if (!runtime.Start(config, engine, &diagnostics)) {
    for (const auto& diagnostic : diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
    Fail("actual runtime Start failed");
  }
  const auto active = runtime.Snapshot();
  Require(active.started && active.worker_thread_count == 2,
          "real runtime did not start both workers");
  Require(active.durable_lease_count >= 2, "real worker leases were not created");
  Wait(waiter_at_park, "worker did not reach its native predicate/park boundary");

  std::thread stopper([&] {
    stop_thread = true;
    runtime.Stop();
    stop_thread = false;
    Signal(stop_finished);
  });
  Wait(stop_boundary, "Stop reached neither the predicate mutex nor notification");
  Signal(allow_park);

  if (!publication_serialized.load()) {
    // Rescue only a failing baseline so its real cleanup can finish. Acquiring
    // the native mutex proves the paused worker has now entered its wait.
    Require(__real_pthread_mutex_lock(schedule_mutex) == 0, "rescue lock failed");
    Require(pthread_mutex_unlock(schedule_mutex) == 0, "rescue unlock failed");
    Require(__real_pthread_cond_broadcast(schedule_condition) == 0, "rescue wake failed");
  }
  Wait(stop_finished, "Stop did not join workers and finish its lifecycle path");
  stopper.join();
  Require(!runtime.Snapshot().started, "Stop returned with a running runtime");
  runtime.Stop();  // Repeated stop after joins must remain harmless.

  std::error_code error;
  std::filesystem::remove_all(directory, error);
  Require(!error, "fixture cleanup failed");
  for (auto* event : {&waiter_at_park, &allow_park, &stop_boundary, &stop_finished}) {
    Require(sem_destroy(event) == 0, "sem_destroy failed");
  }
  Require(publication_serialized.load(),
          "lost wakeup: Stop notified before synchronizing with the wait predicate mutex");
  std::cout << "server_agent_shutdown_wakeup_gate=passed\n";
}
