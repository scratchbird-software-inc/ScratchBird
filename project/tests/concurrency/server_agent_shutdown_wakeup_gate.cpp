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

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <pthread.h>
#include <semaphore.h>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

// SEARCH_KEY: SERVER_AGENT_SHUTDOWN_PREDICATE_PARK_RACE
// Link against the static C++ runtime so GNU --wrap also observes the native
// calls made by std::condition_variable and std::thread. No production state is fabricated:
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
sem_t final_join_reached;
sem_t allow_cleanup;
sem_t second_stop_boundary;
sem_t second_stop_finished;
std::atomic<bool> armed{false};
std::atomic<bool> observed_boundary{false};
std::atomic<bool> publication_serialized{false};
pthread_mutex_t* schedule_mutex = nullptr;
pthread_cond_t* schedule_condition = nullptr;
thread_local bool stop_thread = false;
thread_local int stop_caller = 0;
thread_local unsigned completed_joins = 0;
unsigned expected_joins = 0;
pthread_mutex_t* stop_entry_mutex = nullptr;
std::atomic<bool> second_waiting{false};
std::atomic<bool> second_returned_early{false};
// Startup probe data is accessed only by the test's Start caller; other threads
// bypass it through the thread-local flags. Native joins verify actual handles.
thread_local bool startup_probe = false;
thread_local bool capture_state_unlock = false;
thread_local bool reading_startup_snapshot = false;
pthread_mutex_t* runtime_state_mutex = nullptr;
server::ServerAgentRuntime* startup_runtime = nullptr;
bool runtime_threads_ready = false;
bool startup_failure_injected = false;
unsigned fail_launch = 0;
unsigned launch_attempts = 0;
unsigned launched_threads = 0;
std::array<pthread_t, 3> startup_threads{};
std::array<bool, 3> startup_joined{};

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
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
extern "C" int __real_pthread_join(pthread_t, void**);
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*,
                                      void* (*)(void*), void*);

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                                      void* (*entry)(void*), void* argument) {
  const bool track = startup_probe && runtime_threads_ready && !startup_failure_injected;
  if (track && ++launch_attempts == fail_launch) {
    startup_failure_injected = true;
    return EAGAIN;
  }
  const int created = __real_pthread_create(thread, attributes, entry, argument);
  if (track && created == 0) {
    Require(launched_threads < startup_threads.size(), "unexpected startup thread count");
    startup_threads[launched_threads++] = *thread;
  }
  return created;
}

extern "C" int __wrap_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  if (capture_state_unlock) {
    // Snapshot's final guard release is its state mutex. Discover it from the
    // real public operation, without exposing private production test hooks.
    runtime_state_mutex = mutex;
  }
  const int unlocked = __real_pthread_mutex_unlock(mutex);
  if (startup_probe && !reading_startup_snapshot && mutex == runtime_state_mutex &&
      unlocked == 0) {
    // Inspect only AFTER releasing the state mutex; never recurse on our own
    // Snapshot's unlock. Service-initialization helper threads are not targets.
    reading_startup_snapshot = true;
    const auto snapshot = startup_runtime->Snapshot();
    runtime_threads_ready = snapshot.started && snapshot.worker_thread_count == 2 &&
                            snapshot.durable_lease_count >= 2;
    reading_startup_snapshot = false;
  }
  return unlocked;
}

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
  if (stop_caller == 1 && stop_entry_mutex == nullptr) {
    stop_entry_mutex = mutex;
  }
  if (stop_caller == 2 && mutex == stop_entry_mutex && !second_waiting.load()) {
    // One observation, not a retry loop. The first caller is paused after all
    // native joins, so no worker can transiently hold the old state mutex.
    const int observed = pthread_mutex_trylock(mutex);
    if (observed == EBUSY) {
      second_waiting.store(true);
      Signal(second_stop_boundary);
    } else {
      Require(observed == 0, "stop entry mutex probe failed");
      Require(pthread_mutex_unlock(mutex) == 0, "stop entry probe unlock failed");
    }
  }
  if (stop_thread && mutex == schedule_mutex &&
      !observed_boundary.exchange(true)) {
    publication_serialized.store(true);
    Signal(stop_boundary);
  }
  return __real_pthread_mutex_lock(mutex);
}

extern "C" int __wrap_pthread_join(pthread_t thread, void** result) {
  unsigned tracked = startup_threads.size();
  if (startup_probe) {
    for (unsigned i = 0; i < launched_threads; ++i) {
      if (!startup_joined[i] && pthread_equal(thread, startup_threads[i])) {
        tracked = i;
        break;
      }
    }
  }
  const int joined = __real_pthread_join(thread, result);
  if (startup_probe && tracked < launched_threads && joined == 0) {
    startup_joined[tracked] = true;
  }
  if (stop_caller == 1 && joined == 0 && ++completed_joins == expected_joins) {
    Signal(final_join_reached);
    Wait(allow_cleanup, "controller did not release Stop cleanup");
  }
  return joined;
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

// SEARCH_KEY: SERVER_AGENT_CONCURRENT_STOP_COMPLETION
bool CheckConcurrentStop(server::ServerAgentRuntime& runtime, unsigned worker_count) {
  expected_joins = worker_count + 1;  // Workers plus scheduler.
  std::thread first([&] {
    stop_caller = 1;
    runtime.Stop();
    stop_caller = 0;
    Signal(stop_finished);
  });
  Wait(final_join_reached, "first Stop did not complete its native joins");
  const auto stopping = runtime.Snapshot();
  Require(stopping.started && stopping.stopping,
          "first Stop was not paused before completion publication");
  std::thread second([&] {
    stop_caller = 2;
    runtime.Stop();
    stop_caller = 0;
    second_returned_early.store(runtime.Snapshot().started);
    if (!second_waiting.load()) {
      Signal(second_stop_boundary);
    }
    Signal(second_stop_finished);
  });
  Wait(second_stop_boundary, "second Stop neither waited nor returned");
  Signal(allow_cleanup);
  Wait(stop_finished, "first Stop did not finish cleanup");
  Wait(second_stop_finished, "second Stop did not finish");
  first.join();
  second.join();
  return second_waiting.load() && !second_returned_early.load();
}

// SEARCH_KEY: SERVER_AGENT_PARTIAL_STARTUP_UNWIND
bool CheckStartupFailure(server::ServerAgentRuntime& runtime,
                         const server::ServerBootstrapConfig& config,
                         const server::HostedEngineState& engine,
                         std::vector<server::ServerDiagnostic>& diagnostics) {
  bool expected_exception = false;
  startup_runtime = &runtime;
  capture_state_unlock = true;
  (void)runtime.Snapshot();
  capture_state_unlock = false;
  Require(runtime_state_mutex != nullptr, "could not observe the runtime state mutex");
  startup_probe = true;
  try {
    (void)runtime.Start(config, engine, &diagnostics);
  } catch (const std::system_error& error) {
    expected_exception = error.code() == std::errc::resource_unavailable_try_again;
  } catch (...) {
    // Wrong exception is a failure, but allow the real cleanup below first.
  }
  startup_probe = false;
  const auto after_failure = runtime.Snapshot();
  bool all_joined = true;
  for (unsigned i = 0; i < launched_threads; ++i) {
    all_joined = all_joined && startup_joined[i];
  }
  const bool unwound = expected_exception && launch_attempts == fail_launch &&
                       launched_threads == fail_launch - 1 && all_joined &&
                       !after_failure.started && !after_failure.stopping;
  std::cout << "startup_failure=" << fail_launch
            << " actual_launches=" << launched_threads
            << " all_joined_before_catch=" << all_joined
            << " started_after_catch=" << after_failure.started << '\n';
  // Rescue only after recording the oracle. These joins cannot satisfy it.
  runtime.Stop();
  return unwound;
}

int main(int argc, char** argv) {
  const bool concurrent_stop = argc == 2 && std::string_view(argv[1]) == "--concurrent-stop";
  const bool startup_failure = argc == 3 && std::string_view(argv[1]) == "--startup-failure";
  if (startup_failure) {
    const std::string_view index(argv[2]);
    Require(index == "1" || index == "2" || index == "3", "invalid failed launch index");
    fail_launch = static_cast<unsigned>(index[0] - '0');
  }
  Require(argc == 1 || concurrent_stop || startup_failure, "unknown shutdown test mode");
  const auto events = {&waiter_at_park, &allow_park, &stop_boundary, &stop_finished,
                       &final_join_reached, &allow_cleanup, &second_stop_boundary,
                       &second_stop_finished};
  for (auto* event : events) {
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

  bool startup_unwound = false;
  armed.store(!concurrent_stop && !startup_failure, std::memory_order_release);
  if (startup_failure) {
    startup_unwound = CheckStartupFailure(runtime, config, engine, diagnostics);
  } else if (!runtime.Start(config, engine, &diagnostics)) {
    for (const auto& diagnostic : diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
    Fail("actual runtime Start failed");
  }
  const auto active = runtime.Snapshot();
  Require(startup_failure || (active.started && active.worker_thread_count == 2),
          "real runtime did not start both workers");
  Require(startup_failure || active.durable_lease_count >= 2,
          "real worker leases were not created");
  bool completion_serialized = false;
  if (concurrent_stop) {
    completion_serialized = CheckConcurrentStop(runtime, active.worker_thread_count);
  } else if (!startup_failure) {
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
  }
  Require(!runtime.Snapshot().started, "Stop returned with a running runtime");
  runtime.Stop();  // Repeated stop after joins must remain harmless.

  std::error_code error;
  std::filesystem::remove_all(directory, error);
  Require(!error, "fixture cleanup failed");
  for (auto* event : events) {
    Require(sem_destroy(event) == 0, "sem_destroy failed");
  }
  if (startup_failure) {
    Require(startup_unwound,
            "native startup failure escaped before partial runtime cleanup completed");
    std::cout << "server_agent_startup_failure_gate=passed\n";
  } else if (concurrent_stop) {
    Require(completion_serialized,
            "concurrent Stop returned before the ongoing stop operation completed");
    std::cout << "server_agent_concurrent_stop_completion_gate=passed\n";
  } else {
    Require(publication_serialized.load(),
            "lost wakeup: Stop notified before synchronizing with the wait predicate mutex");
    std::cout << "server_agent_shutdown_wakeup_gate=passed\n";
  }
}
