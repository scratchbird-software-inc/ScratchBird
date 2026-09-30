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
sem_t scheduler_at_entry;
sem_t allow_scheduler;
sem_t scheduler_after_tick;
sem_t worker_repark;
sem_t scheduler_timeout;
sem_t scheduler_next_wait;
bool spurious_wake = false;
bool scheduler_timeout_mode = false;
thread_local bool observed_waiter = false;
thread_local bool timed_scheduler = false;
unsigned scheduler_timed_waits = 0;
bool scheduler_deadline_observed = false;
bool scheduler_stop_woke_wait = false;
std::atomic<unsigned> native_wait_returns{0};
std::atomic<unsigned> notifications_sent{0};
thread_local unsigned observed_notifications = 0;
thread_local bool scheduler_probe = false;
thread_local unsigned scheduler_schedule_locks = 0;
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
bool cleanup_failure_mode = false;
unsigned cleanup_sync_failures = 0;
bool cleanup_after_joins = false;
bool track_native_creates = true;
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

// SEARCH_KEY: SERVER_AGENT_SNAPSHOT_BINARY_IDENTITIES
bool HasBinarySnapshotIdentities(const server::ServerAgentRuntimeSnapshot& snapshot,
                                 const server::HostedEngineState& engine) {
  const auto equal_bytes = [](const std::string& bytes, const platform::Uuid& id) {
    if (bytes.size() != id.bytes.size()) return false;
    for (std::size_t i = 0; i < id.bytes.size(); ++i) {
      if (static_cast<unsigned char>(bytes[i]) != id.bytes[i]) return false;
    }
    return true;
  };
  if (engine.databases.empty() ||
      !equal_bytes(snapshot.database_uuid, engine.databases.front().database_uuid) ||
      !equal_bytes(snapshot.filespace_uuid, engine.databases.front().filespace_uuid)) return false;
  for (const auto& worker : snapshot.workers) {
    platform::Uuid id;
    if (worker.instance_uuid.size() != id.bytes.size()) return false;
    for (std::size_t i = 0; i < id.bytes.size(); ++i) {
      id.bytes[i] = static_cast<unsigned char>(worker.instance_uuid[i]);
    }
    if (!uuid::IsEngineIdentityUuid(id)) return false;
  }
  return snapshot.workers.size() == 2;
}
}  // namespace

extern "C" int __real_pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
extern "C" int __real_pthread_cond_broadcast(pthread_cond_t*);
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __real_pthread_mutex_unlock(pthread_mutex_t*);
extern "C" int __real_pthread_join(pthread_t, void**);
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*,
                                      void* (*)(void*), void*);
extern "C" int __real_pthread_setname_np(pthread_t, const char*);
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if (startup_probe && cleanup_failure_mode && startup_failure_injected &&
      cleanup_sync_failures == 0) {
    cleanup_after_joins = true;
    for (unsigned i = 0; i < launched_threads; ++i) {
      cleanup_after_joins = cleanup_after_joins && startup_joined[i];
    }
    ++cleanup_sync_failures;
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}
extern "C" int __real_pthread_cond_clockwait(pthread_cond_t*, pthread_mutex_t*,
                                             clockid_t, const timespec*);

// SEARCH_KEY: SERVER_AGENT_SCHEDULER_NATIVE_TIMEOUT_AND_STOP
extern "C" int __wrap_pthread_cond_clockwait(pthread_cond_t* condition,
                                              pthread_mutex_t* mutex,
                                              clockid_t clock,
                                              const timespec* deadline) {
  if (!timed_scheduler) {
    return __real_pthread_cond_clockwait(condition, mutex, clock, deadline);
  }
  Require(clock == CLOCK_MONOTONIC, "scheduler wait did not use monotonic time");
  if (scheduler_timed_waits == 0) {
    schedule_mutex = mutex;
    schedule_condition = condition;
  } else {
    Require(mutex == schedule_mutex && condition == schedule_condition,
            "scheduler changed its predicate/wait binding");
  }
  // Count actual expired wait phases, not incidental spurious native returns.
  if (scheduler_timed_waits == 1) {
    Signal(scheduler_next_wait);
    Wait(allow_scheduler, "controller did not release scheduler timed park");
  }
  const int result = __real_pthread_cond_clockwait(condition, mutex, clock, deadline);
  Require(result == 0 || result == ETIMEDOUT, "native scheduler wait failed");
  if (scheduler_timed_waits == 0 && result == ETIMEDOUT) {
    timespec now{};
    Require(clock_gettime(clock, &now) == 0, "monotonic observation failed");
    scheduler_deadline_observed = now.tv_sec > deadline->tv_sec ||
        (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
    ++scheduler_timed_waits;
    Signal(scheduler_timeout);
    Wait(allow_scheduler, "controller did not release actual timeout");
  } else if (scheduler_timed_waits == 1) {
    scheduler_stop_woke_wait = result == 0;
    ++scheduler_timed_waits;
  }
  return result;
}

extern "C" int __wrap_pthread_setname_np(pthread_t thread, const char* name) {
  const int result = __real_pthread_setname_np(thread, name);
  if (scheduler_timeout_mode && std::string_view(name) == "sb-agent-sch") {
    timed_scheduler = true;
  }
  if (spurious_wake && std::string_view(name) == "sb-agent-sch") {
    // SchedulerLoop names itself before taking runtime locks or publishing a
    // generation. Hold that actual thread; do not fake time or work results.
    Signal(scheduler_at_entry);
    Wait(allow_scheduler, "controller did not release scheduler entry");
    scheduler_probe = true;
  }
  return result;
}

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                                      void* (*entry)(void*), void* argument) {
  const bool track = startup_probe && track_native_creates && runtime_threads_ready &&
                     !startup_failure_injected;
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
  if (spurious_wake && observed_waiter &&
      notifications_sent.load() > observed_notifications) {
    // Reaching this call again means the production predicate rejected the
    // preceding wake. The worker still owns the same native predicate mutex.
    Require(condition == schedule_condition && mutex == schedule_mutex,
            "worker changed its predicate/wait binding");
    observed_notifications = notifications_sent.load();
    Signal(worker_repark);
    Wait(allow_park, "controller did not release repark boundary");
  }
  if (armed.load(std::memory_order_acquire)) {
    char name[16]{};
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) == 0 &&
        std::string_view(name) == "sb-agent-w00" && armed.exchange(false)) {
      // The predicate was false and the worker still owns schedule_mutex.
      // Freeze exactly before the atomic unlock-and-park operation.
      schedule_mutex = mutex;
      schedule_condition = condition;
      observed_waiter = true;
      Signal(waiter_at_park);
      Wait(allow_park, "controller did not release the worker to park");
    }
  }
  const int result = __real_pthread_cond_wait(condition, mutex);
  if (spurious_wake && observed_waiter) {
    Require(result == 0, "native worker wait failed");
    native_wait_returns.fetch_add(1);
  }
  return result;
}

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (scheduler_probe && mutex == schedule_mutex && ++scheduler_schedule_locks == 4) {
    // Initial timed wait, first dispatch, completion barrier, then the next
    // dispatch. Hold BEFORE the fourth lock; the real first tick has completed.
    Signal(scheduler_after_tick);
    Wait(allow_scheduler, "controller did not release scheduler after first tick");
  }
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

// SEARCH_KEY: SERVER_AGENT_SPURIOUS_WAKE_RECHECK
bool CheckSpuriousWake(server::ServerAgentRuntime& runtime) {
  Wait(scheduler_at_entry, "scheduler did not reach controlled entry");
  Wait(waiter_at_park, "worker did not reach initial predicate/park boundary");
  Signal(allow_park);
  Signal(allow_scheduler);
  Wait(scheduler_after_tick, "scheduler did not complete its first real tick");
  const auto baseline = runtime.Snapshot();
  constexpr unsigned wake_count = 8;
  bool no_work = baseline.scheduler_ticks == 1 && baseline.total_worker_ticks == 1;
  for (unsigned wake = 0; wake < wake_count; ++wake) {
    if (wake != 0) Signal(allow_park);
    // The worker owns this mutex until the REAL cond_wait atomically releases
    // it. Acquiring it proves the notification cannot precede registration.
    Require(__real_pthread_mutex_lock(schedule_mutex) == 0, "wake probe lock failed");
    notifications_sent.fetch_add(1);
    Require(__real_pthread_cond_broadcast(schedule_condition) == 0, "wake probe broadcast failed");
    Require(pthread_mutex_unlock(schedule_mutex) == 0, "wake probe unlock failed");
    Wait(worker_repark, "notification escaped the false work predicate");
    const auto snapshot = runtime.Snapshot();
    no_work = no_work && snapshot.started && !snapshot.stopping &&
              snapshot.scheduler_ticks == baseline.scheduler_ticks &&
              snapshot.total_worker_ticks == baseline.total_worker_ticks &&
              snapshot.total_actions_accepted == baseline.total_actions_accepted &&
              snapshot.total_actions_refused == baseline.total_actions_refused &&
              snapshot.total_actions_failed == baseline.total_actions_failed &&
              native_wait_returns.load() >= wake + 2;
  }
  // Finish with a real Stop at the last false-predicate/park boundary. Release
  // the scheduler only after stop publication has reached the predicate mutex.
  std::thread stopper([&] {
    stop_thread = true;
    runtime.Stop();
    stop_thread = false;
    Signal(stop_finished);
  });
  Wait(stop_boundary, "Stop did not reach final predicate mutex");
  Signal(allow_park);
  Signal(allow_scheduler);
  Wait(stop_finished, "Stop did not drain the woken worker cohort");
  stopper.join();
  return no_work && publication_serialized.load() && !runtime.Snapshot().started;
}

bool CheckSchedulerTimeout(server::ServerAgentRuntime& runtime) {
  Wait(scheduler_timeout, "scheduler initial delay did not actually expire");
  const auto expired = runtime.Snapshot();
  const bool no_early_dispatch = scheduler_deadline_observed &&
      expired.scheduler_ticks == 0 && expired.total_worker_ticks == 0;
  Signal(allow_scheduler);
  Wait(scheduler_next_wait, "scheduler did not begin its next timed wait");
  std::thread stopper([&] {
    stop_thread = true;
    runtime.Stop();
    stop_thread = false;
    Signal(stop_finished);
  });
  Wait(stop_boundary, "Stop did not serialize with scheduler timed park");
  Signal(allow_scheduler);
  Wait(stop_finished, "Stop did not join scheduler after timed notification");
  stopper.join();
  const auto stopped = runtime.Snapshot();
  return no_early_dispatch && scheduler_stop_woke_wait &&
      scheduler_timed_waits == 2 && publication_serialized.load() &&
      !stopped.started && stopped.scheduler_ticks == 0 && stopped.total_worker_ticks == 0;
}

bool CheckSequentialRestart(server::ServerAgentRuntime& runtime,
                            const server::ServerBootstrapConfig& config,
                            const server::HostedEngineState& engine,
                            std::vector<server::ServerDiagnostic>& diagnostics);

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
  // SEARCH_KEY: SERVER_AGENT_STARTUP_AND_CLEANUP_FAILURE
  if (cleanup_failure_mode) {
    const auto failed = runtime.Snapshot().stop_result;
    diagnostics.clear();
    const bool restarted = runtime.Start(config, engine, &diagnostics);
    const auto repeated = runtime.Stop();
    bool retained = failed.attempted && !failed.ok() && !failed.durable_cleanup_complete &&
        !failed.diagnostics.empty() && repeated.attempted && !repeated.ok() &&
        !repeated.durable_cleanup_complete &&
        repeated.diagnostics.size() == failed.diagnostics.size() &&
        diagnostics.size() == failed.diagnostics.size();
    for (std::size_t i = 0; retained && i < failed.diagnostics.size(); ++i) {
      retained = repeated.diagnostics[i].code == failed.diagnostics[i].code &&
          repeated.diagnostics[i].occurrence_uuid == failed.diagnostics[i].occurrence_uuid &&
          diagnostics[i].occurrence_uuid == failed.diagnostics[i].occurrence_uuid;
    }
    for (const auto& diagnostic : failed.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
      for (const auto& field : diagnostic.fields) {
        if (field.key == "shutdown_phase") std::cerr << field.key << '=' << field.value << '\n';
      }
    }
    const auto stopped = runtime.Snapshot();
    std::cout << "cleanup_sync_failures=" << cleanup_sync_failures
              << " cleanup_after_joins=" << cleanup_after_joins
              << " retained_occurrences=" << retained << " restarted=" << restarted << '\n';
    return unwound && cleanup_sync_failures == 1 && cleanup_after_joins &&
        retained && !restarted && !stopped.started && !stopped.stopping;
  }
  // SEARCH_KEY: SERVER_AGENT_REUSE_AFTER_NATIVE_STARTUP_FAILURE
  // Reuse this exact object and database after the injected launch failure.
  // The replacement probe disables injection and independently accounts for
  // every actual native create/join in three replacement cohorts. Never let a
  // later successful start conceal a failed original unwind.
  if (!unwound) return false;
  return CheckSequentialRestart(runtime, config, engine, diagnostics);
}

// SEARCH_KEY: SERVER_AGENT_SEQUENTIAL_THREAD_RESTART
// Qualify native cohort replacement only, not durable generation/clean-node
// receipts or concurrent Start. Every create/join below reaches the real OS.
bool CheckSequentialRestart(server::ServerAgentRuntime& runtime,
                            const server::ServerBootstrapConfig& config,
                            const server::HostedEngineState& engine,
                            std::vector<server::ServerDiagnostic>& diagnostics) {
  startup_runtime = &runtime;
  capture_state_unlock = true;
  (void)runtime.Snapshot();
  capture_state_unlock = false;
  Require(runtime_state_mutex != nullptr, "could not observe restart state mutex");
  for (unsigned cycle = 0; cycle < 3; ++cycle) {
    launch_attempts = 0;
    launched_threads = 0;
    startup_joined.fill(false);
    runtime_threads_ready = false;
    startup_failure_injected = false;
    fail_launch = 0;  // Observe native creates; inject no failures.
    track_native_creates = true;
    startup_probe = true;
    diagnostics.clear();
    bool started = false;
    try {
      started = runtime.Start(config, engine, &diagnostics);
    } catch (const std::exception& error) {
      std::cerr << "restart exception: " << error.what() << '\n';
    }
    track_native_creates = false;  // Stop's dependency helpers are not cohort threads.
    const auto active = runtime.Snapshot();
    runtime.Stop();
    startup_probe = false;
    bool joined = launched_threads == startup_threads.size();
    for (unsigned i = 0; i < launched_threads; ++i) {
      joined = joined && startup_joined[i];
    }
    const auto stopped = runtime.Snapshot();
    std::cout << "restart_cycle=" << cycle << " started=" << started
              << " native_creates=" << launched_threads << " all_joined=" << joined
              << " stopped=" << (!stopped.started && !stopped.stopping) << '\n';
    for (const auto& diagnostic : diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
    if (!started || !active.started || !HasBinarySnapshotIdentities(active, engine) ||
        active.worker_thread_count != 2 ||
        active.durable_lease_count < 2 || active.durable_catalog_root_digest.empty() ||
        launch_attempts != 3 || !joined || stopped.started || stopped.stopping) {
      return false;
    }
  }
  return true;
}

// SEARCH_KEY: SERVER_AGENT_ACTIVE_DESTRUCTOR_JOINS
bool CheckActiveDestruction(const server::ServerBootstrapConfig& config,
                            const server::HostedEngineState& engine,
                            std::vector<server::ServerDiagnostic>& diagnostics) {
  bool active_valid = false;
  std::thread release_worker;
  {
    server::ServerAgentRuntime owned;
    startup_runtime = &owned;
    capture_state_unlock = true;
    (void)owned.Snapshot();
    capture_state_unlock = false;
    Require(runtime_state_mutex != nullptr, "could not observe destructor state mutex");
    startup_probe = true;
    Require(owned.Start(config, engine, &diagnostics), "destructor fixture Start failed");
    track_native_creates = false;
    Wait(waiter_at_park, "destructor fixture worker did not reach native park");
    const auto active = owned.Snapshot();
    active_valid = active.started && active.worker_thread_count == 2 &&
        active.durable_lease_count >= 2 && HasBinarySnapshotIdentities(active, engine);
    release_worker = std::thread([] {
      Wait(stop_boundary, "destructor did not publish through the predicate mutex");
      Signal(allow_park);
    });
    stop_thread = true;
    // No explicit Stop: the actual destructor must wake and join this cohort
    // before its owned condition variable, mutexes and worker records disappear.
  }
  stop_thread = false;
  startup_probe = false;
  startup_runtime = nullptr;
  runtime_state_mutex = nullptr;
  release_worker.join();
  bool joined = launch_attempts == 3 && launched_threads == startup_threads.size();
  for (unsigned i = 0; i < launched_threads; ++i) joined = joined && startup_joined[i];
  std::cout << "destructor_native_creates=" << launched_threads
            << " all_joined_before_scope_exit=" << joined << '\n';
  return active_valid && joined && publication_serialized.load();
}

int main(int argc, char** argv) {
  spurious_wake = argc == 2 && std::string_view(argv[1]) == "--spurious-wake";
  scheduler_timeout_mode = argc == 2 && std::string_view(argv[1]) == "--scheduler-timeout";
  const bool concurrent_stop = argc == 2 && std::string_view(argv[1]) == "--concurrent-stop";
  cleanup_failure_mode = argc == 3 && std::string_view(argv[1]) == "--startup-cleanup-failure";
  const bool startup_failure = cleanup_failure_mode ||
      (argc == 3 && std::string_view(argv[1]) == "--startup-failure");
  const bool sequential_restart = argc == 2 && std::string_view(argv[1]) == "--sequential-restart";
  const bool active_destruction = argc == 2 && std::string_view(argv[1]) == "--active-destruction";
  if (startup_failure) {
    const std::string_view index(argv[2]);
    Require(index == "1" || index == "2" || index == "3", "invalid failed launch index");
    fail_launch = static_cast<unsigned>(index[0] - '0');
  }
  Require(argc == 1 || concurrent_stop || startup_failure || sequential_restart || spurious_wake || scheduler_timeout_mode || active_destruction,
          "unknown shutdown test mode");
  const auto events = {&waiter_at_park, &allow_park, &stop_boundary, &stop_finished,
                       &final_join_reached, &allow_cleanup, &second_stop_boundary,
                       &second_stop_finished, &scheduler_at_entry, &allow_scheduler,
                       &worker_repark, &scheduler_after_tick, &scheduler_timeout,
                       &scheduler_next_wait};
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
  bool restarted = false;
  bool destruction_joined = false;
  armed.store(!concurrent_stop && !startup_failure && !sequential_restart && !scheduler_timeout_mode,
              std::memory_order_release);
  if (active_destruction) {
    destruction_joined = CheckActiveDestruction(config, engine, diagnostics);
  } else if (sequential_restart) {
    restarted = CheckSequentialRestart(runtime, config, engine, diagnostics);
  } else if (startup_failure) {
    startup_unwound = CheckStartupFailure(runtime, config, engine, diagnostics);
  } else if (!runtime.Start(config, engine, &diagnostics)) {
    for (const auto& diagnostic : diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
    Fail("actual runtime Start failed");
  }
  const auto active = runtime.Snapshot();
  Require(active_destruction || startup_failure || sequential_restart || (active.started && active.worker_thread_count == 2),
          "real runtime did not start both workers");
  Require(active_destruction || startup_failure || sequential_restart || active.durable_lease_count >= 2,
          "real worker leases were not created");
  bool completion_serialized = false;
  bool spurious_rechecked = false;
  bool scheduler_timeout_checked = false;
  if (scheduler_timeout_mode) {
    scheduler_timeout_checked = CheckSchedulerTimeout(runtime);
  } else if (spurious_wake) {
    spurious_rechecked = CheckSpuriousWake(runtime);
  } else if (concurrent_stop) {
    completion_serialized = CheckConcurrentStop(runtime, active.worker_thread_count);
  } else if (!startup_failure && !sequential_restart && !active_destruction) {
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
  if (active_destruction) {
    Require(destruction_joined, "active destruction did not join its native cohort");
    std::cout << "server_agent_active_destruction_gate=passed\n";
  } else if (scheduler_timeout_mode) {
    Require(scheduler_timeout_checked, "scheduler timeout or shutdown predicate was violated");
    std::cout << "server_agent_scheduler_timeout_gate=passed\n";
  } else if (spurious_wake) {
    Require(spurious_rechecked, "notification admitted work with a false predicate");
    std::cout << "server_agent_spurious_wake_gate=passed notifications=8\n";
  } else if (sequential_restart) {
    Require(restarted, "sequential runtime restart did not replace and join native cohorts");
    std::cout << "server_agent_sequential_thread_restart_gate=passed\n";
  } else if (startup_failure) {
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
