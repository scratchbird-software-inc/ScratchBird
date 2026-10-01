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
#include "wire/binary_status_packet.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// SEARCH_KEY: SERVER_AGENT_STATUS_FILE_BINARY_IDENTITIES
bool ReadStatusIdentities(const server::ServerAgentRuntimeSnapshot& snapshot,
                          std::vector<std::string>* identities) {
  // Read the actual production sidecar, not a test-encoded substitute. Opening
  // the file pins one complete inode even if the scheduler atomically replaces it.
  std::ifstream input(snapshot.status_path, std::ios::binary | std::ios::ate);
  if (!input) return false;
  const auto length = input.tellg();
  if (length <= 0 || length > 1024 * 1024) return false;
  std::string packet(static_cast<std::size_t>(length), '\0');
  input.seekg(0);
  if (!input.read(packet.data(), static_cast<std::streamsize>(packet.size()))) return false;
  namespace result = scratchbird::wire::public_result;
  std::vector<result::Field> fields;
  if (!scratchbird::wire::binary_status::Decode(packet, &fields)) return false;
  // SEARCH_KEY: SERVER_AGENT_STATUS_STOP_RESULT_PROJECTION
  // Compare the actual packet's lifecycle projection with the public snapshot.
  // This text fragment contains flags/codes, never rendered UUID identities.
  const auto boolean = [](bool value) { return value ? "true" : "false"; };
  std::string expected = "{\"server_agent_runtime\":{\"started\":";
  expected += boolean(snapshot.started);
  expected += ",\"stopping\":";
  expected += boolean(snapshot.stopping);
  expected += ",\"stop_attempted\":";
  expected += boolean(snapshot.stop_result.attempted);
  expected += ",\"durable_cleanup_complete\":";
  expected += boolean(snapshot.stop_result.durable_cleanup_complete);
  expected += ",\"stop_failed\":";
  expected += boolean(!snapshot.stop_result.ok());
  expected += ",\"stop_diagnostic_code\":\"";
  expected += snapshot.stop_result.diagnostics.empty()
      ? "" : snapshot.stop_result.diagnostics.front().code;
  expected += "\",";
  // Decode retains the contract discriminator at index zero.
  if (fields.size() < 2 || fields[1].kind != result::Kind::text ||
      !fields[1].value.starts_with(expected)) return false;
  identities->clear();
  for (std::size_t i = 1; i < fields.size(); ++i) {
    if (fields[i].kind != result::Kind::uuid) continue;
    const auto index = identities->size();
    const auto& bytes = fields[i].value;
    if (bytes.size() != 16 || fields[i - 1].kind != result::Kind::text) return false;
    std::string_view label;
    if (index == 0) {
      label = "\"database_uuid\":\"";
      if (bytes != snapshot.database_uuid) return false;
    } else if (index == 1) {
      label = "\"filespace_uuid\":\"";
      if (bytes != snapshot.filespace_uuid) return false;
    } else {
      const auto worker = (index - 2) / 2;
      if (worker >= snapshot.workers.size()) return false;
      if (index % 2 == 0) {
        label = "\"instance_uuid\":\"";
        if (bytes != snapshot.workers[worker].instance_uuid) return false;
      } else {
        label = "\"lease_uuid\":\"";
      }
    }
    if (!fields[i - 1].value.ends_with(label)) return false;
    platform::Uuid identity;
    for (std::size_t byte = 0; byte < identity.bytes.size(); ++byte)
      identity.bytes[byte] = static_cast<unsigned char>(bytes[byte]);
    if (!uuid::IsEngineIdentityUuid(identity)) return false;
    identities->push_back(bytes);
  }
  return identities->size() == 2 + 2 * snapshot.workers.size();
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
bool CheckConcurrentStop(server::ServerAgentRuntime& runtime, unsigned worker_count,
                         bool fail_cleanup = false) {
  server::ServerAgentRuntimeStopResult first_result;
  server::ServerAgentRuntimeStopResult second_result;
  expected_joins = worker_count + 1;  // Workers plus scheduler.
  std::thread first([&] {
    stop_caller = 1;
    first_result = runtime.Stop();
    stop_caller = 0;
    Signal(stop_finished);
  });
  Wait(final_join_reached, "first Stop did not complete its native joins");
  const auto stopping = runtime.Snapshot();
  Require(stopping.started && stopping.stopping,
          "first Stop was not paused before completion publication");
  const std::filesystem::path database_path(stopping.database_path);
  const auto held_path = database_path.string() + ".held";
  if (fail_cleanup) {
    // All runtime threads really joined; only the first Stop's cleanup remains.
    // Make its actual MGA Begin fail, without inventing a storage result.
    std::filesystem::rename(database_path, held_path);
  }
  std::thread second([&] {
    stop_caller = 2;
    second_result = runtime.Stop();
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
  if (fail_cleanup) {
    Require(!std::filesystem::exists(database_path), "cleanup recreated missing database");
    std::filesystem::rename(held_path, database_path);
  }
  const auto snapshot = runtime.Snapshot();
  const auto repeated = runtime.Stop();
  const auto same_result = [&](const server::ServerAgentRuntimeStopResult& other) {
    if (other.attempted != first_result.attempted ||
        other.durable_cleanup_complete != first_result.durable_cleanup_complete ||
        other.diagnostics.size() != first_result.diagnostics.size()) return false;
    for (std::size_t i = 0; i < other.diagnostics.size(); ++i) {
      const auto& a = first_result.diagnostics[i];
      const auto& b = other.diagnostics[i];
      if (a.code != b.code || a.message_key != b.message_key || a.severity != b.severity ||
          a.safe_message != b.safe_message || a.occurrence_uuid != b.occurrence_uuid ||
          a.fields.size() != b.fields.size()) return false;
      for (std::size_t field = 0; field < a.fields.size(); ++field) {
        if (a.fields[field].key != b.fields[field].key ||
            a.fields[field].value != b.fields[field].value) return false;
      }
    }
    return true;
  };
  bool expected_result = first_result.attempted;
  if (fail_cleanup) {
    expected_result = expected_result && !first_result.ok() &&
        !first_result.durable_cleanup_complete && first_result.diagnostics.size() == 1 &&
        first_result.diagnostics.front().code == "SB-STORAGE-DISK-OPEN-MISSING";
    bool begin_failure = false;
    for (const auto& diagnostic : first_result.diagnostics) {
      for (const auto& field : diagnostic.fields) {
        begin_failure = begin_failure ||
            (field.key == "shutdown_phase" && field.value == "service-drain:begin");
      }
    }
    expected_result = expected_result && begin_failure;
  } else {
    expected_result = expected_result && first_result.ok() && first_result.durable_cleanup_complete;
  }
  const bool retained = same_result(second_result) && same_result(snapshot.stop_result) &&
      same_result(repeated);
  std::vector<std::string> status_identities;
  const bool status_matches = ReadStatusIdentities(snapshot, &status_identities);
  // Negative oracle controls: the same real file must not match an inverted
  // completion or attempted flag. Never rewrite production status evidence.
  auto wrong_completion = snapshot;
  wrong_completion.stop_result.durable_cleanup_complete =
      !snapshot.stop_result.durable_cleanup_complete;
  auto wrong_attempt = snapshot;
  wrong_attempt.stop_result.attempted = !snapshot.stop_result.attempted;
  const bool rejects_false_claims = !ReadStatusIdentities(wrong_completion, &status_identities) &&
      !ReadStatusIdentities(wrong_attempt, &status_identities);
  std::cout << "concurrent_stop_failure=" << fail_cleanup
            << " exact_result_retained=" << retained
            << " status_result_preserved=" << status_matches
            << " rejects_false_claims=" << rejects_false_claims << '\n';
  return second_waiting.load() && !second_returned_early.load() && expected_result && retained &&
      status_matches && rejects_false_claims && !snapshot.started && !snapshot.stopping;
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
                            std::vector<server::ServerDiagnostic>& diagnostics,
                            bool repeat_active_start = false);

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
                            std::vector<server::ServerDiagnostic>& diagnostics,
                            bool repeat_active_start) {
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
    std::vector<std::string> active_identities;
    const bool active_status_valid = ReadStatusIdentities(active, &active_identities);
    // SEARCH_KEY: SERVER_AGENT_ACTIVE_START_RETAINS_COHORT
    // Sequential same-input calls only: Start/destruction still require external
    // lifecycle ownership. Native observation remains armed to catch new threads.
    bool active_start_preserved = true;
    if (repeat_active_start && started) {
      track_native_creates = true;
      for (unsigned repeat = 0; repeat < 3; ++repeat) {
        std::vector<server::ServerDiagnostic> repeat_diagnostics;
        bool repeated_start = false;
        try {
          repeated_start = runtime.Start(config, engine, &repeat_diagnostics);
        } catch (const std::exception& error) {
          std::cerr << "active Start exception: " << error.what() << '\n';
        }
        const auto same_cohort = runtime.Snapshot();
        std::vector<std::string> same_identities;
        active_start_preserved = active_start_preserved && repeated_start &&
            repeat_diagnostics.empty() && same_cohort.started && !same_cohort.stopping &&
            same_cohort.worker_thread_count == active.worker_thread_count &&
            same_cohort.durable_lease_count == active.durable_lease_count &&
            HasBinarySnapshotIdentities(same_cohort, engine) &&
            ReadStatusIdentities(same_cohort, &same_identities) &&
            same_identities == active_identities && launch_attempts == 3;
      }
      track_native_creates = false;
    }
    runtime.Stop();
    startup_probe = false;
    bool joined = launched_threads == startup_threads.size();
    for (unsigned i = 0; i < launched_threads; ++i) {
      joined = joined && startup_joined[i];
    }
    const auto stopped = runtime.Snapshot();
    std::vector<std::string> stopped_identities;
    const bool status_preserved = active_status_valid &&
        ReadStatusIdentities(stopped, &stopped_identities) &&
        active_identities == stopped_identities;
    std::cout << "restart_cycle=" << cycle << " started=" << started
              << " native_creates=" << launched_threads << " all_joined=" << joined
              << " binary_status_preserved=" << status_preserved
              << " active_start_repeats=" << (repeat_active_start ? 3 : 0)
              << " active_start_preserved=" << active_start_preserved
              << " stopped=" << (!stopped.started && !stopped.stopping) << '\n';
    for (const auto& diagnostic : diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
    if (!started || !active.started || !HasBinarySnapshotIdentities(active, engine) ||
        active.worker_thread_count != 2 ||
        active.durable_lease_count < 2 || active.durable_catalog_root_digest.empty() ||
        launch_attempts != 3 || !joined || !status_preserved || !active_start_preserved ||
        stopped.started || stopped.stopping) {
      return false;
    }
  }
  return true;
}

// SEARCH_KEY: SERVER_AGENT_SETUP_PATH_FAILURE_RECOVERY
bool CheckSetupPathFailure(server::ServerAgentRuntime& runtime,
                           const server::ServerBootstrapConfig& config,
                           const server::HostedEngineState& engine,
                           std::vector<server::ServerDiagnostic>& diagnostics,
                           bool database_unavailable = false) {
  // SEARCH_KEY: SERVER_AGENT_SETUP_DATABASE_FAILURE_RECOVERY
  const std::filesystem::path database_path = engine.databases.front().database_path;
  const auto held_path = database_path.string() + ".held";
  // A real regular file prevents directory creation, before service setup or
  // native runtime thread launch. Alternatively hold the real database aside
  // so catalog-seed transaction admission fails. Neither path mocks engine I/O.
  if (database_unavailable) {
    std::filesystem::rename(database_path, held_path);
  } else {
    std::ofstream obstruction(config.control_dir);
    obstruction << "runtime setup obstruction\n";
    obstruction.close();
    Require(!obstruction.fail(), "could not create status-path obstruction");
  }
  startup_runtime = &runtime;
  capture_state_unlock = true;
  (void)runtime.Snapshot();
  capture_state_unlock = false;
  Require(runtime_state_mutex != nullptr, "could not observe setup state mutex");
  startup_probe = true;
  const bool started = runtime.Start(config, engine, &diagnostics);
  startup_probe = false;
  const auto failed = runtime.Snapshot();
  bool expected_diagnostic = false;
  for (const auto& diagnostic : diagnostics) {
    std::cout << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    expected_diagnostic = expected_diagnostic || (database_unavailable
        ? diagnostic.code == "SB-STORAGE-DISK-OPEN-MISSING" &&
          diagnostic.safe_message == "The server agent runtime could not begin an MGA transaction for catalog seeding."
        : diagnostic.code == "SERVER.AGENT_RUNTIME.STATUS_PATH_FAILED");
  }
  const bool refused_cleanly = !started && expected_diagnostic && !failed.started &&
      !failed.stopping && failed.durable_lease_count == 0 &&
      failed.scheduler_ticks == 0 && launch_attempts == 0 && launched_threads == 0;
  // Stop before any admitted service/native cohort is a harmless no-op, not
  // a fabricated durable cleanup receipt. It must not create a missing database.
  const auto stop_result = runtime.Stop();
  const bool stopped_cleanly = stop_result.ok() && !stop_result.attempted &&
      !stop_result.durable_cleanup_complete;
  std::error_code error;
  if (database_unavailable) {
    Require(!std::filesystem::exists(database_path), "failed startup recreated missing database");
    std::filesystem::rename(held_path, database_path);
  } else {
    const bool removed = std::filesystem::remove(config.control_dir, error);
    Require(removed && !error, "could not remove status-path obstruction");
  }
  std::cout << "setup_refused_before_native_cohort=" << refused_cleanly << '\n';
  return refused_cleanly && stopped_cleanly &&
      CheckSequentialRestart(runtime, config, engine, diagnostics);
}

// SEARCH_KEY: SERVER_AGENT_MISSING_IDENTITY_BEFORE_EFFECTS
bool CheckIdentityAdmission(server::ServerAgentRuntime& runtime,
                          const server::ServerBootstrapConfig& config,
                          const server::HostedEngineState& engine,
                          std::vector<server::ServerDiagnostic>& diagnostics,
                          std::string_view target,
                          std::string_view malformed = {}) {
  auto invalid_engine = engine;
  auto& database = invalid_engine.databases.front();
  auto& identity = target == "database" ? database.database_uuid : database.filespace_uuid;
  std::string expected_code = "SERVER.AGENT_RUNTIME.IDENTITY_MISSING";
  if (malformed == "variant") {
    identity.bytes[8] &= 0x3fu;
    expected_code = "SB-UUID-TYPED-VARIANT";
  } else if (malformed == "version") {
    identity.bytes[6] = (identity.bytes[6] & 0x0fu) | 0x40u;
    expected_code = "SB-UUID-TYPED-ENGINE-IDENTITY-NOT-V7";
  } else {
    identity = {};
  }
  const auto read_fixture = [&] {
    std::ifstream input(database.database_path, std::ios::binary | std::ios::ate);
    Require(input.good(), "could not open identity fixture for byte comparison");
    const auto size = input.tellg();
    Require(size > 0 && size <= 64 * 1024 * 1024, "unexpected identity fixture size");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    Require(static_cast<bool>(input.read(bytes.data(), bytes.size())), "identity fixture read failed");
    return bytes;
  };
  const auto before = read_fixture();
  const bool started = runtime.Start(config, invalid_engine, &diagnostics);
  const auto refused = runtime.Snapshot();
  const auto stopped = runtime.Stop();
  const bool silent_refusal = malformed.empty() || !runtime.Start(config, invalid_engine, nullptr);
  const bool unchanged = before == read_fixture() && !std::filesystem::exists(config.control_dir);
  const bool correct_refusal = !started && !refused.started && !refused.stopping &&
      refused.worker_thread_count == 0 && refused.durable_lease_count == 0 &&
      refused.scheduler_ticks == 0 && diagnostics.size() == 1 &&
      diagnostics.front().code == expected_code &&
      stopped.ok() && !stopped.attempted && !stopped.durable_cleanup_complete;
  std::cout << "identity_target=" << target << " refused=" << correct_refusal
            << " malformed=" << malformed << " fixture_bytes_unchanged=" << unchanged << '\n';
  // Repair only the hosted descriptor by reusing the original valid engine
  // state. The actual database never had its on-disk identity changed.
  return correct_refusal && silent_refusal && unchanged &&
      CheckSequentialRestart(runtime, config, engine, diagnostics);
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
  const bool concurrent_stop_failure = argc == 2 &&
      std::string_view(argv[1]) == "--concurrent-stop-failure";
  const bool concurrent_stop = concurrent_stop_failure ||
      (argc == 2 && std::string_view(argv[1]) == "--concurrent-stop");
  cleanup_failure_mode = argc == 3 && std::string_view(argv[1]) == "--startup-cleanup-failure";
  const bool startup_failure = cleanup_failure_mode ||
      (argc == 3 && std::string_view(argv[1]) == "--startup-failure");
  const bool binary_boundary = argc == 3 && std::string_view(argv[1]) == "--binary-status-boundary";
  const bool malformed_identity = argc == 4 && std::string_view(argv[1]) == "--malformed-identity";
  const bool missing_identity = malformed_identity ||
      (argc == 3 && std::string_view(argv[1]) == "--missing-identity");
  if (malformed_identity) {
    Require(std::string_view(argv[3]) == "variant" || std::string_view(argv[3]) == "version",
            "unknown malformed identity profile");
  }
  if (missing_identity) {
    Require(std::string_view(argv[2]) == "database" || std::string_view(argv[2]) == "filespace",
            "unknown missing identity target");
  }
  const bool active_start_repeat = argc == 2 &&
      std::string_view(argv[1]) == "--active-start-repeat";
  const bool sequential_restart = binary_boundary || active_start_repeat ||
      (argc == 2 && std::string_view(argv[1]) == "--sequential-restart");
  const bool active_destruction = argc == 2 && std::string_view(argv[1]) == "--active-destruction";
  const bool setup_database_failure = argc == 2 && std::string_view(argv[1]) == "--setup-database-failure";
  const bool setup_path_failure = setup_database_failure ||
      (argc == 2 && std::string_view(argv[1]) == "--setup-path-failure");
  const bool lifecycle_case = active_destruction || startup_failure ||
      sequential_restart || setup_path_failure || missing_identity;
  if (startup_failure) {
    const std::string_view index(argv[2]);
    Require(index == "1" || index == "2" || index == "3", "invalid failed launch index");
    fail_launch = static_cast<unsigned>(index[0] - '0');
  }
  Require(argc == 1 || concurrent_stop || lifecycle_case || spurious_wake || scheduler_timeout_mode,
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
  // SEARCH_KEY: SERVER_AGENT_BINARY_UUID_ZERO_BOUNDARIES
  // Historical fixture creation times exercise zero bytes in UUIDv7's timestamp
  // prefix. Identities are issued by the real engine generator; no identity is
  // hand-edited and the runtime clock/deadlines remain the actual node clock.
  auto creation_millis = millis.unix_epoch_millis;
  if (binary_boundary) {
    const std::string_view profile(argv[2]);
    Require(profile == "leading" || profile == "embedded", "unknown binary boundary profile");
    creation_millis = profile == "leading" ? 0x100ULL : 0x010000000001ULL;
  }
  db::DatabaseCreateConfig create;
  create.path = (directory / "runtime.sbdb").string();
  create.database_uuid = NewIdentity(platform::UuidKind::database, creation_millis);
  create.filespace_uuid = NewIdentity(platform::UuidKind::filespace, creation_millis);
  create.creation_unix_epoch_millis = creation_millis;
  if (binary_boundary) {
    const bool leading = std::string_view(argv[2]) == "leading";
    for (const auto& id : {create.database_uuid.value, create.filespace_uuid.value}) {
      Require(uuid::IsEngineIdentityUuid(id) && id.bytes[1] == 0 &&
                  id.bytes[0] == (leading ? 0 : 1) &&
                  id.bytes[5] == (leading ? 0 : 1) && (id.bytes[8] & 0x80) != 0,
              "engine-issued fixture did not exercise zero/high-bit byte boundary");
    }
    std::cout << "binary_uuid_boundary=" << argv[2] << '\n';
  }
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
  bool setup_recovered = false;
  armed.store(!concurrent_stop && !startup_failure && !sequential_restart && !scheduler_timeout_mode && !setup_path_failure && !missing_identity,
              std::memory_order_release);
  if (missing_identity) {
    setup_recovered = CheckIdentityAdmission(runtime, config, engine, diagnostics, argv[2],
                                          malformed_identity ? argv[3] : "");
  } else if (setup_path_failure) {
    setup_recovered = CheckSetupPathFailure(runtime, config, engine, diagnostics,
                                           setup_database_failure);
  } else if (active_destruction) {
    destruction_joined = CheckActiveDestruction(config, engine, diagnostics);
  } else if (sequential_restart) {
    restarted = CheckSequentialRestart(runtime, config, engine, diagnostics, active_start_repeat);
  } else if (startup_failure) {
    startup_unwound = CheckStartupFailure(runtime, config, engine, diagnostics);
  } else if (!runtime.Start(config, engine, &diagnostics)) {
    for (const auto& diagnostic : diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
    Fail("actual runtime Start failed");
  }
  const auto active = runtime.Snapshot();
  Require(lifecycle_case || (active.started && active.worker_thread_count == 2),
          "real runtime did not start both workers");
  Require(lifecycle_case || active.durable_lease_count >= 2,
          "real worker leases were not created");
  bool completion_serialized = false;
  bool spurious_rechecked = false;
  bool scheduler_timeout_checked = false;
  if (scheduler_timeout_mode) {
    scheduler_timeout_checked = CheckSchedulerTimeout(runtime);
  } else if (spurious_wake) {
    spurious_rechecked = CheckSpuriousWake(runtime);
  } else if (concurrent_stop) {
    completion_serialized = CheckConcurrentStop(runtime, active.worker_thread_count,
                                                concurrent_stop_failure);
  } else if (!lifecycle_case) {
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
  if (missing_identity) {
    Require(setup_recovered, "missing identity was not refused before effects or repair failed");
    std::cout << "server_agent_missing_identity_gate=passed\n";
  } else if (setup_path_failure) {
    Require(setup_recovered, "early setup failure did not permit clean recovery");
    std::cout << (setup_database_failure ? "server_agent_setup_database_failure_gate=passed\n"
                                       : "server_agent_setup_path_failure_gate=passed\n");
  } else if (active_destruction) {
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
