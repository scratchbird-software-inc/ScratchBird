// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "server_agent_runtime.hpp"
#include "ipc_server.hpp"
#include "time.hpp"
#include "uuid.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction/startup_transaction_inventory.hpp"
#include "agents/agent_durable_catalog_store_api.hpp"
#include "wire/binary_status_packet.hpp"

#include <cerrno>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <pthread.h>
#include <semaphore.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace server = scratchbird::server;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace engine_api = scratchbird::engine::internal_api;

// Restrict an actual native fsync failure to the chosen real stop transaction.
// All engine functions execute; no service result, catalog or receipt is faked.
thread_local bool fault_armed = false;
thread_local bool fault_at_commit = false;
thread_local bool inside_commit = false;
thread_local bool selected_phase = false;
thread_local unsigned sync_failures = 0;
thread_local std::string fault_phase;
thread_local bool post_commit_exception = false;
thread_local bool opaque_exception = false;
thread_local unsigned post_commit_exceptions = 0;
thread_local engine_api::StartupTransactionInventoryRequest committed_attempt;
thread_local bool report_allocation_mode = false;
thread_local bool initial_diagnostic_allocation_mode = false;
thread_local bool report_allocation_armed = false;
thread_local unsigned diagnostic_allocations = 0;
thread_local unsigned diagnostic_allocation_failures = 0;
thread_local unsigned cleanup_begin_attempts = 0;

void* operator new(std::size_t bytes) {
  if (report_allocation_armed && bytes == sizeof(server::ServerDiagnostic) &&
      ++diagnostic_allocations == (initial_diagnostic_allocation_mode ? 1U : 2U)) {
    report_allocation_armed = false;
    ++diagnostic_allocation_failures;
    throw std::bad_alloc();
  }
  if (void* pointer = std::malloc(bytes ? bytes : 1)) return pointer;
  throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
bool catalog_load_fault = false;
bool catalog_restore_before_rollback = false;
unsigned catalog_seed_attempts = 0;
unsigned catalog_begin_attempts = 0;
unsigned catalog_thread_attempts = 0;
std::filesystem::path catalog_path;
std::filesystem::path catalog_held_path;

extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attributes,
                                      void* (*entry)(void*), void* argument) {
  if (catalog_load_fault) ++catalog_thread_attempts;
  return __real_pthread_create(thread, attributes, entry, argument);
}

extern "C" engine_api::AgentDurableCatalogStoreResult RealCatalogPersist(
    const engine_api::AgentDurableCatalogStoreRequest&)
    asm("__real__ZN11scratchbird6engine12internal_api31PersistAgentDurableCatalogImageERKNS1_31AgentDurableCatalogStoreRequestE");
extern "C" engine_api::AgentDurableCatalogStoreResult WrapCatalogPersist(
    const engine_api::AgentDurableCatalogStoreRequest&)
    asm("__wrap__ZN11scratchbird6engine12internal_api31PersistAgentDurableCatalogImageERKNS1_31AgentDurableCatalogStoreRequestE");
engine_api::AgentDurableCatalogStoreResult WrapCatalogPersist(
    const engine_api::AgentDurableCatalogStoreRequest& request) {
  if (catalog_load_fault) ++catalog_seed_attempts;
  return RealCatalogPersist(request);
}
extern "C" engine_api::EngineRollbackTransactionResult RealRollback(
    const engine_api::EngineRollbackTransactionRequest&)
    asm("__real__ZN11scratchbird6engine12internal_api25EngineRollbackTransactionERKNS1_32EngineRollbackTransactionRequestE");
extern "C" engine_api::EngineRollbackTransactionResult WrapRollback(
    const engine_api::EngineRollbackTransactionRequest&)
    asm("__wrap__ZN11scratchbird6engine12internal_api25EngineRollbackTransactionERKNS1_32EngineRollbackTransactionRequestE");
engine_api::EngineRollbackTransactionResult WrapRollback(
    const engine_api::EngineRollbackTransactionRequest& request) {
  if (catalog_load_fault && catalog_restore_before_rollback)
    std::filesystem::rename(catalog_held_path, catalog_path);
  return RealRollback(request);
}

// SEARCH_KEY: SERVER_AGENT_STOP_INFLIGHT_ENGINE_CALL
// Hold a real worker after successful engine transaction admission. This proves
// native join ordering, not storage action acceptance or physical completion.
std::atomic<bool> inflight_armed{false};
std::atomic<bool> worker_held{false};
std::atomic<bool> inflight_selected{false};
std::atomic<bool> drain_while_held{false};
std::atomic<bool> drain_before_join{false};
std::atomic<bool> drain_observed{false};
std::atomic<bool> join_returned_while_held{false};
std::atomic<bool> worker_joined{false};
std::string inflight_purpose;
pthread_t inflight_worker{};
sem_t worker_entered;
sem_t release_worker;
sem_t join_entered;
thread_local bool observing_stop = false;

void SignalEvent(sem_t& event) {
  if (sem_post(&event) != 0) std::abort();
}

void WaitEvent(sem_t& event) {
  timespec limit{};
  if (clock_gettime(CLOCK_REALTIME, &limit) != 0) std::abort();
  limit.tv_sec += 30;
  int result;
  do { result = sem_timedwait(&event, &limit); } while (result < 0 && errno == EINTR);
  if (result != 0) {
    std::cerr << "inflight engine-call boundary timed out\n";
    std::abort();
  }
}

extern "C" int __real_pthread_join(pthread_t, void**);
extern "C" int __wrap_pthread_join(pthread_t thread, void** result) {
  const bool target = observing_stop && pthread_equal(thread, inflight_worker);
  if (target) SignalEvent(join_entered);
  const int joined = __real_pthread_join(thread, result);
  if (target) {
    join_returned_while_held.store(worker_held.load());
    worker_joined.store(joined == 0);
  }
  return joined;
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if (fault_armed && !post_commit_exception && selected_phase && inside_commit == fault_at_commit && sync_failures == 0) {
    ++sync_failures;
    errno = EIO;
    return -1;
  }
  return __real_fsync(fd);
}
extern "C" engine_api::EngineBeginTransactionResult RealBegin(
    const engine_api::EngineBeginTransactionRequest&)
    asm("__real__ZN11scratchbird6engine12internal_api22EngineBeginTransactionERKNS1_29EngineBeginTransactionRequestE");
extern "C" engine_api::EngineBeginTransactionResult WrapBegin(
    const engine_api::EngineBeginTransactionRequest&)
    asm("__wrap__ZN11scratchbird6engine12internal_api22EngineBeginTransactionERKNS1_29EngineBeginTransactionRequestE");
engine_api::EngineBeginTransactionResult WrapBegin(const engine_api::EngineBeginTransactionRequest& request) {
  if (request.context.request_id.starts_with("server-agent-service-drain-") ||
      request.context.request_id.starts_with("server-agent-service-shutdown-"))
    ++cleanup_begin_attempts;
  selected_phase = false;
  if (worker_held.load() && request.context.request_id.starts_with("server-agent-service-drain-"))
    drain_while_held.store(true);
  if (inflight_selected.load() && request.context.request_id.starts_with("server-agent-service-drain-")) {
    drain_observed.store(true);
    if (!worker_joined.load()) drain_before_join.store(true);
  }
  auto result = RealBegin(request);
  if (catalog_load_fault && request.context.request_id.starts_with("server-agent-catalog-seed-")) {
    ++catalog_begin_attempts;
    if (result.ok) std::filesystem::rename(catalog_path, catalog_held_path);
  }
  if (result.ok && inflight_armed.load() &&
      request.context.request_id.starts_with("server-agent-" + inflight_purpose + "-") &&
      inflight_armed.exchange(false)) {
    inflight_worker = pthread_self();
    inflight_selected.store(true);
    worker_held.store(true);
    SignalEvent(worker_entered);
    WaitEvent(release_worker);
    worker_held.store(false);
  }
  selected_phase = fault_armed && request.context.request_id.starts_with("server-agent-" + fault_phase + "-");
  return result;
}
extern "C" engine_api::EngineCommitTransactionResult RealCommit(
    const engine_api::EngineCommitTransactionRequest&)
    asm("__real__ZN11scratchbird6engine12internal_api23EngineCommitTransactionERKNS1_30EngineCommitTransactionRequestE");
extern "C" engine_api::EngineCommitTransactionResult WrapCommit(
    const engine_api::EngineCommitTransactionRequest&)
    asm("__wrap__ZN11scratchbird6engine12internal_api23EngineCommitTransactionERKNS1_30EngineCommitTransactionRequestE");
engine_api::EngineCommitTransactionResult WrapCommit(const engine_api::EngineCommitTransactionRequest& request) {
  inside_commit = true;
  auto result = RealCommit(request);
  inside_commit = false;
  if (fault_armed && selected_phase && post_commit_exception && result.ok &&
      post_commit_exceptions == 0) {
    committed_attempt = {request.context.database_path, request.context.database_uuid,
                         request.context.transaction_uuid, request.context.local_transaction_id};
    ++post_commit_exceptions;
    report_allocation_armed = report_allocation_mode;
    if (opaque_exception) throw 73;
    throw std::runtime_error("injected exception after actual durable commit");
  }
  return result;
}

namespace {
struct StopObservation {
  bool observed = false;
  bool ok = false;
  bool attempted = false;
  bool durable_cleanup_complete = false;
  std::vector<server::ServerDiagnostic> diagnostics;
};

StopObservation ObserveStop(server::ServerAgentRuntime& runtime) {
  const auto result = runtime.Stop();
  return {true, result.ok(), result.attempted,
          result.durable_cleanup_complete, result.diagnostics};
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool HasShutdownObservation(const std::filesystem::path& path, const std::string& expected) {
  const auto bytes = Read(path);
  bool found = false;
  for (std::size_t pos = 0; pos < bytes.size();) {
    if (bytes.size() - pos < 16 || bytes.compare(pos, 8, "SBOBS002") != 0) return false;
    std::uint64_t size = 0;
    for (unsigned i = 0; i < 8; ++i)
      size |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[pos + 8 + i])) << (8 * i);
    pos += 16;
    if (size > bytes.size() - pos) return false;
    std::vector<scratchbird::wire::public_result::Field> fields;
    if (!scratchbird::wire::binary_status::Decode(std::string_view(bytes).substr(pos, size), &fields)) return false;
    std::string text;
    for (const auto& f : fields) if (f.name == "text") text += f.value;
    const auto event = text.find("\"event_type\":\"server.shutdown\"");
    if (event != std::string::npos) {
      const auto end = text.find("}}", event);
      if (text.substr(event, end - event).find(expected) == std::string::npos) return false;
      found = true;
    }
    pos += size;
  }
  return found;
}
}

// SEARCH_KEY: SERVER_AGENT_DURABLE_STOP_FAILURE_PROPAGATION
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string mode(argv[1]);
  const bool inflight = mode == "inflight-page" || mode == "inflight-growth";
  const bool ipc = mode == "ipc-failure" || mode == "ipc-success";
  const bool sync_fault = mode == "drain-persist-failure" || mode == "shutdown-persist-failure" ||
                          mode == "drain-commit-failure" || mode == "shutdown-commit-failure";
  const bool missing_file = mode == "failure" || mode == "ipc-failure";
  const bool catalog_failure = mode.starts_with("catalog-load-");
  initial_diagnostic_allocation_mode = mode == "drain-commit-initial-diagnostic-allocation" ||
      mode == "shutdown-commit-initial-diagnostic-allocation";
  report_allocation_mode = initial_diagnostic_allocation_mode ||
      mode == "drain-commit-report-allocation" || mode == "shutdown-commit-report-allocation";
  const bool exception_fault = report_allocation_mode || mode == "drain-commit-exception" || mode == "shutdown-commit-exception" ||
      mode == "drain-commit-opaque-exception" || mode == "shutdown-commit-opaque-exception";
  post_commit_exception = exception_fault;
  opaque_exception = report_allocation_mode || mode.find("opaque") != std::string::npos;
  const bool fail = missing_file || sync_fault || exception_fault;
  if (!ipc && !fail && !inflight && !catalog_failure && mode != "success") return 2;
  if (inflight) {
    if (sem_init(&worker_entered, 0, 0) || sem_init(&release_worker, 0, 0) ||
        sem_init(&join_entered, 0, 0)) return 2;
    inflight_purpose = mode == "inflight-page" ? "page-preallocation" : "filespace-growth";
    inflight_armed.store(true);
  }
  fault_at_commit = mode.find("commit") != std::string::npos;
  fault_phase = mode.starts_with("shutdown") ? "service-shutdown" : "service-drain";
  std::string pattern = (std::filesystem::temp_directory_path() / "sb-stop-result-XXXXXX").string();
  if (!mkdtemp(pattern.data())) return 2;
  const std::filesystem::path root(pattern);
  const auto path = root / "runtime.sbdb";
  const auto held = root / "runtime.sbdb.held";
  bool passed = false;
  try {
    const auto clock = scratchbird::core::time::ReadLocalNodeClockSnapshot();
    if (!clock.ok()) throw std::runtime_error("clock unavailable");
    const auto millis = scratchbird::core::time::WallClockToUuidV7Millis(clock.value.wall_clock);
    const auto database_id = uuid::GenerateEngineIdentityV7(platform::UuidKind::database, millis.unix_epoch_millis);
    const auto filespace_id = uuid::GenerateEngineIdentityV7(platform::UuidKind::filespace, millis.unix_epoch_millis);
    const auto server_id = uuid::GenerateEngineIdentityV7(platform::UuidKind::object, millis.unix_epoch_millis);
    if (!millis.ok() || !database_id.ok() || !filespace_id.ok() || !server_id.ok())
      throw std::runtime_error("identity unavailable");
    db::DatabaseCreateConfig create;
    create.path = path.string();
    create.database_uuid = database_id.value;
    create.filespace_uuid = filespace_id.value;
    create.creation_unix_epoch_millis = millis.unix_epoch_millis;
    create.allow_minimal_resource_bootstrap = true;
    create.require_resource_seed_pack = false;
    const auto created = db::CreateDatabaseFile(create);
    if (!created.ok()) throw std::runtime_error(created.diagnostic.diagnostic_code);
    server::HostedDatabaseSnapshot database;
    database.state = server::HostedDatabaseState::kOpen;
    database.database_path = path.string();
    database.database_uuid = database_id.value.value;
    database.filespace_uuid = filespace_id.value.value;
    database.database_created = true;
    database.database_open = true;
    database.write_admission_fenced = false;
    database.config_policy_security_lifecycle_present = true;
    database.selected_agent_type_ids = {"page_allocation_manager", "filespace_capacity_manager"};
    server::HostedEngineState engine;
    engine.engine_context_active = true;
    engine.databases.push_back(database);
    server::ServerBootstrapConfig config;
    config.control_dir = root / "control";
    config.data_dir = root;
    config.database_default_path = path;
    config.sbps_endpoint = root / "ipc.sock";
    config.lifecycle_state_file = root / "state";
    config.lifecycle_journal_file = root / "journal";
    config.log_file = (root / "server.log").string();
    std::filesystem::create_directories(config.control_dir);

    if (catalog_failure) {
      server::ServerAgentRuntime runtime;
      std::vector<server::ServerDiagnostic> diagnostics;
      if (!runtime.Start(config, engine, &diagnostics) || !runtime.Stop().ok())
        throw std::runtime_error("catalog fault initial startup/stop failed");
      engine_api::EngineRequestContext observer;
      observer.database_path = path.string();
      observer.database_uuid = database.database_uuid;
      const auto before = engine_api::LoadAgentDurableCatalogImage(observer, true);
      if (!before.ok) throw std::runtime_error("catalog fault initial image unavailable");
      catalog_path = path;
      catalog_held_path = held;
      catalog_restore_before_rollback = mode == "catalog-load-failure";
      const bool no_sink = mode == "catalog-load-rollback-no-sink";
      diagnostics.clear();
      catalog_load_fault = true;
      const bool started = runtime.Start(config, engine, no_sink ? nullptr : &diagnostics);
      catalog_load_fault = false;
      if (std::filesystem::exists(held)) std::filesystem::rename(held, path);
      const auto snapshot = runtime.Snapshot();
      const auto after = engine_api::LoadAgentDurableCatalogImage(observer, true);
      const bool failed_rollback = !catalog_restore_before_rollback;
      passed = !started && !snapshot.started && catalog_thread_attempts == 0 &&
               catalog_begin_attempts == 1 && catalog_seed_attempts == 0 &&
               after.ok && after.version_uuid == before.version_uuid &&
               snapshot.stop_result.ok() == !failed_rollback;
      if (!no_sink) {
        passed = passed && diagnostics.size() == (failed_rollback ? 2u : 1u) &&
                 diagnostics.front().safe_message.find("no seed was attempted") != std::string::npos;
      }
      if (failed_rollback) {
        passed = passed && snapshot.stop_result.diagnostics.size() == 2 &&
                 !runtime.Stop().ok() && !runtime.Start(config, engine, nullptr);
      } else {
        passed = passed && runtime.Start(config, engine, &diagnostics) && runtime.Stop().ok();
      }
      std::cout << "catalog_load_failed=" << !started << " seed_attempts=" << catalog_seed_attempts
                << " native_thread_attempts=" << catalog_thread_attempts
                << " begin_attempts=" << catalog_begin_attempts
                << " diagnostics=" << diagnostics.size()
                << " original_preserved=" << (after.version_uuid == before.version_uuid)
                << " rollback_failed=" << failed_rollback << " no_sink=" << no_sink << '\n';
    } else if (ipc) {
      server::ServerLifecycleArtifacts artifacts;
      artifacts.server_uuid = server_id.value.value;
      artifacts.generation = 1;
      bool ready = false;
      bool fault = false;
      server::ResetParserServerStopRequest();
      server::ParserServerIpcLifecycleCallbacks callbacks;
      callbacks.on_ready = [&] { ready = true; server::RequestParserServerStop(); };
      callbacks.on_stopping = [&] {
        // Real file-unavailable failure, after real runtime startup; no fabricated result.
        if (missing_file) {
          std::filesystem::rename(path, held);
          fault = true;
        }
      };
      const auto result = server::RunParserServerIpcEndpoint(config, artifacts, engine, callbacks);
      const auto state = Read(config.lifecycle_state_file);
      const auto journal = Read(config.lifecycle_journal_file);
      bool stop_failure = false;
      for (const auto& d : result.diagnostics) {
        std::cerr << d.code << ':' << d.safe_message << '\n';
        for (const auto& f : d.fields) if (f.key == "shutdown_phase") stop_failure = true;
      }
      passed = ready && fault == fail && (result.exit_code != 0) == fail && stop_failure == fail &&
               state.find(fail ? "state=failed" : "state=stopped") != std::string::npos &&
               (!fail || (state.find("stopped") == std::string::npos &&
                          journal.find("stopped") == std::string::npos)) &&
               HasShutdownObservation(config.log_file, fail ? "\"severity\":\"error\"" : "\"severity\":\"info\"") &&
               HasShutdownObservation(config.control_dir / "sb_server.audit.sbobs",
                                      fail ? "\"outcome\":\"failed\"" : "\"outcome\":\"completed\"");
      std::cout << "ready=" << ready << " fault=" << fault << " exit=" << result.exit_code
                << " stop_failure=" << stop_failure << " state=" << state << '\n';
    } else {
      server::ServerAgentRuntime runtime;
      std::vector<server::ServerDiagnostic> diagnostics;
      if (!runtime.Start(config, engine, &diagnostics) || !runtime.Snapshot().started)
        throw std::runtime_error("runtime startup failed");
      if (missing_file) std::filesystem::rename(path, held);
      fault_armed = sync_fault || exception_fault;
      StopObservation stopped;
      bool inflight_ordering = true;
      if (inflight) {
        WaitEvent(worker_entered);
        std::atomic<bool> stop_returned{false};
        std::thread stopper([&] {
          observing_stop = true;
          stopped = ObserveStop(runtime);
          observing_stop = false;
          stop_returned.store(true);
        });
        WaitEvent(join_entered);
        // The native join target is the exact admitted worker held above.
        // Snapshot must remain callable while Stop is waiting for this worker.
        const auto during = runtime.Snapshot();
        inflight_ordering = during.started && during.stopping &&
                            !stop_returned.load() && !worker_joined.load() &&
                            worker_held.load() && !drain_while_held.load();
        SignalEvent(release_worker);
        stopper.join();
        inflight_ordering = inflight_ordering && worker_joined.load() &&
                            !join_returned_while_held.load() && !drain_while_held.load() &&
                            drain_observed.load() && !drain_before_join.load();
      } else {
        if (report_allocation_mode) {
          bool delivery_failed = false;
          try { stopped = ObserveStop(runtime); }
          catch (const std::bad_alloc&) { delivery_failed = true; }
          report_allocation_armed = false;
          const auto cleanup_attempts_before_repeat = cleanup_begin_attempts;
          const auto retained = runtime.Snapshot();
          std::cout << "diagnostic_allocations=" << diagnostic_allocations
                    << " delivery_failed=" << delivery_failed
                    << " retained_diagnostics=" << retained.stop_result.diagnostics.size()
                    << " stopping=" << retained.stopping << '\n';
          inflight_ordering = delivery_failed && diagnostic_allocation_failures == 1 &&
              !retained.started && !retained.stopping && retained.stop_result.attempted &&
              !retained.stop_result.durable_cleanup_complete &&
              !retained.stop_result.ok() &&
              retained.stop_result.diagnostics.size() == (initial_diagnostic_allocation_mode ? 0U : 1U);
          stopped = ObserveStop(runtime);
          inflight_ordering = inflight_ordering &&
              cleanup_begin_attempts == cleanup_attempts_before_repeat;
        } else {
          stopped = ObserveStop(runtime);
        }
      }
      fault_armed = false;
      const auto repeated = ObserveStop(runtime);
      const auto snapshot = runtime.Snapshot();
      passed = inflight_ordering && stopped.observed && stopped.attempted && !snapshot.started && !snapshot.stopping &&
               stopped.ok == !fail && stopped.durable_cleanup_complete == !fail &&
               stopped.diagnostics.empty() == (!fail || initial_diagnostic_allocation_mode) && repeated.ok == stopped.ok &&
               repeated.durable_cleanup_complete == stopped.durable_cleanup_complete &&
               repeated.diagnostics.size() == stopped.diagnostics.size();
      for (std::size_t i = 0; i < stopped.diagnostics.size() && i < repeated.diagnostics.size(); ++i) {
        passed = passed && stopped.diagnostics[i].code == repeated.diagnostics[i].code &&
                 !stopped.diagnostics[i].code.empty() &&
                 stopped.diagnostics[i].message_key == stopped.diagnostics[i].code &&
                 repeated.diagnostics[i].message_key == stopped.diagnostics[i].message_key &&
                 stopped.diagnostics[i].occurrence_uuid == repeated.diagnostics[i].occurrence_uuid;
      }
      if (sync_fault) {
        bool exact_phase = false;
        for (const auto& d : stopped.diagnostics) {
          for (const auto& field : d.fields) {
            if (field.key == "shutdown_phase" &&
                field.value == fault_phase + (fault_at_commit ? ":commit" : ""))
              exact_phase = true;
          }
        }
        passed = passed && sync_failures == 1 && exact_phase;
      }
      if (exception_fault) {
        const auto inventory = engine_api::InspectStartupTransactionInventory(committed_attempt);
        bool exact_phase = false;
        bool exact_detail = false;
        for (const auto& d : stopped.diagnostics) {
          if (d.code != "SB_DIAG_AGENT_COORDINATOR_SHUTDOWN_NOT_CLEAN") continue;
          for (const auto& field : d.fields) {
            exact_phase |= field.key == "shutdown_phase" && field.value == fault_phase + ":commit";
            exact_detail |= field.key == "detail" && field.value == (opaque_exception
                ? "non-standard exception during durable cleanup"
                : "injected exception after actual durable commit");
          }
        }
        passed = passed && post_commit_exceptions == 1 && sync_failures == 0 &&
            inventory.outcome == engine_api::StartupTransactionInventoryOutcome::committed &&
            inventory.publication_base.has_value() &&
            (initial_diagnostic_allocation_mode || (exact_phase && exact_detail));
        std::cout << "post_commit_exceptions=" << post_commit_exceptions
                  << " actual_inventory_committed=" <<
            (inventory.outcome == engine_api::StartupTransactionInventoryOutcome::committed) << '\n';
      }
      passed = passed && snapshot.stop_result.ok() == stopped.ok &&
               snapshot.stop_result.durable_cleanup_complete == stopped.durable_cleanup_complete &&
               (!fail || server::BuildIparAgentLifecycleProjectionSource(snapshot).lifecycle_state == "failed");
      if (fail) {
        if (missing_file) std::filesystem::rename(held, path);
        // Restoring the file does not erase an unacknowledged durable stop failure.
        diagnostics.clear();
        passed = passed && !runtime.Start(config, engine, &diagnostics) && !diagnostics.empty();
      }
      for (const auto& d : stopped.diagnostics) std::cerr << d.code << ':' << d.safe_message << '\n';
      std::cout << "observed=" << stopped.observed << " attempted=" << stopped.attempted
                << " ok=" << stopped.ok << " durable=" << stopped.durable_cleanup_complete
                << " native_sync_failures=" << sync_failures << '\n';
      if (inflight) std::cout << "inflight_ordering=" << inflight_ordering
                              << " worker_joined=" << worker_joined.load() << '\n';
    }
  } catch (const std::exception& error) {
    report_allocation_armed = false;
    std::cerr << error.what() << '\n';
  } catch (...) {
    report_allocation_armed = false;
    fault_armed = false;
    std::cerr << "non-standard cleanup exception escaped runtime; original committed attempt retained\n";
  }
  // Fixtures are disposable and all actual runtime/IPC objects have left scope.
  if (passed) std::filesystem::remove_all(root);
  else std::cerr << "retained_failed_fixture=" << root << '\n';
  if (inflight) {
    sem_destroy(&worker_entered);
    sem_destroy(&release_worker);
    sem_destroy(&join_entered);
  }
  std::cout << "server_agent_durable_stop_gate=" << (passed ? "passed" : "failed") << '\n';
  return passed ? 0 : 1;
}
