// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "server_agent_runtime.hpp"
#include "agents/agent_durable_catalog_store_api.hpp"
#include "transaction/transaction_api.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <fstream>
#include <iostream>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <pthread.h>
#include <cerrno>

namespace api=scratchbird::engine::internal_api;
namespace server=scratchbird::server;
namespace db=scratchbird::storage::database;
namespace uuid=scratchbird::core::uuid;
using Uuid=scratchbird::core::platform::Uuid;
std::atomic<unsigned> begins=0,commits=0,rollbacks=0,persists=0;
std::atomic<bool> metadata_only=true;
std::array<std::uint8_t,16> source_occurrence{};
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
Uuid Id(){const auto id=uuid::IssueRuntimeIdentityV7();Check(bool(id),"actual runtime identity");return *id;}
std::vector<std::uint8_t> Read(const std::filesystem::path& path){
  std::ifstream file(path,std::ios::binary);Check(bool(file),"actual database read");
  return {std::istreambuf_iterator<char>(file),{}};
}
extern "C" api::EngineBeginTransactionResult
__real__ZN11scratchbird6engine12internal_api22EngineBeginTransactionERKNS1_29EngineBeginTransactionRequestE(const api::EngineBeginTransactionRequest&);
extern "C" api::EngineBeginTransactionResult
__wrap__ZN11scratchbird6engine12internal_api22EngineBeginTransactionERKNS1_29EngineBeginTransactionRequestE(const api::EngineBeginTransactionRequest& request){
  ++begins;const auto& c=request.context;const auto& a=c.authorization_context;
  if(c.security_context_present||!c.principal_uuid.is_nil()||!c.session_uuid.is_nil()||
      c.catalog_generation_id||c.security_epoch||c.resource_epoch||c.name_resolution_epoch||
      c.local_transaction_id||!c.transaction_uuid.is_nil()||a.present||!a.authority_uuid.is_nil()||
      !a.principal_uuid.is_nil()||a.security_context_generation||a.security_epoch||a.policy_epoch||a.catalog_generation_id||
      !a.effective_subjects.empty()||!a.grants.empty()||!a.policies.empty()||!a.evidence_tags.empty()||
      !a.engine_owned_bootstrap_role_uuid.is_nil()||
      std::find(c.trace_tags.begin(),c.trace_tags.end(),"server.agent.engine_materialized_authority")!=c.trace_tags.end())
    metadata_only=false;
  auto result=__real__ZN11scratchbird6engine12internal_api22EngineBeginTransactionERKNS1_29EngineBeginTransactionRequestE(request);
  if(!result.diagnostics.empty())source_occurrence=result.diagnostics.front().occurrence_uuid;
  return result;
}
extern "C" api::EngineCommitTransactionResult
__real__ZN11scratchbird6engine12internal_api23EngineCommitTransactionERKNS1_30EngineCommitTransactionRequestE(const api::EngineCommitTransactionRequest&);
extern "C" api::EngineCommitTransactionResult
__wrap__ZN11scratchbird6engine12internal_api23EngineCommitTransactionERKNS1_30EngineCommitTransactionRequestE(const api::EngineCommitTransactionRequest& request){
  ++commits;return __real__ZN11scratchbird6engine12internal_api23EngineCommitTransactionERKNS1_30EngineCommitTransactionRequestE(request);
}
extern "C" api::EngineRollbackTransactionResult
__real__ZN11scratchbird6engine12internal_api25EngineRollbackTransactionERKNS1_32EngineRollbackTransactionRequestE(const api::EngineRollbackTransactionRequest&);
extern "C" api::EngineRollbackTransactionResult
__wrap__ZN11scratchbird6engine12internal_api25EngineRollbackTransactionERKNS1_32EngineRollbackTransactionRequestE(const api::EngineRollbackTransactionRequest& request){
  ++rollbacks;return __real__ZN11scratchbird6engine12internal_api25EngineRollbackTransactionERKNS1_32EngineRollbackTransactionRequestE(request);
}
extern "C" api::AgentDurableCatalogStoreResult
__real__ZN11scratchbird6engine12internal_api31PersistAgentDurableCatalogImageERKNS1_31AgentDurableCatalogStoreRequestE(const api::AgentDurableCatalogStoreRequest&);
extern "C" api::AgentDurableCatalogStoreResult
__wrap__ZN11scratchbird6engine12internal_api31PersistAgentDurableCatalogImageERKNS1_31AgentDurableCatalogStoreRequestE(const api::AgentDurableCatalogStoreRequest& request){
  ++persists;return __real__ZN11scratchbird6engine12internal_api31PersistAgentDurableCatalogImageERKNS1_31AgentDurableCatalogStoreRequestE(request);
}
server::ServerAgentRuntime* join_failure_runtime = nullptr;
std::atomic<bool> fail_coordinator_join = false;
std::atomic<bool> fail_coordinator_launch = false;
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                                     void* (*start)(void*), void* data) {
  if (fail_coordinator_launch.exchange(false)) return EAGAIN;
  return __real_pthread_create(thread, attr, start, data);
}
extern "C" int __real_pthread_join(pthread_t, void**);
extern "C" int __wrap_pthread_join(pthread_t thread, void** value) {
  if (fail_coordinator_join.exchange(false)) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (join_failure_runtime->Snapshot().shutdown_agent_state != "completed" &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // The handle remains joinable even though the coordinator has completed
    // its durable cleanup. The next Stop must retry the actual native join.
    return EDEADLK;
  }
  return __real_pthread_join(thread, value);
}
int main(int argc,char** argv)try{
  Check(argc==2,"mode");const std::string mode=argv[1];
  Check(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(),"agent_service_admission").ok(),"memory policy");
  scratchbird::tests::OwnedTempDirectory artifacts;
  {
    db::DatabaseCreateConfig create;create.path=(artifacts.path()/"node.sbdb").string();create.page_size=16384;
    create.database_uuid=uuid::MakeTypedUuid(scratchbird::core::platform::UuidKind::database,Id()).value;
    create.filespace_uuid=uuid::MakeTypedUuid(scratchbird::core::platform::UuidKind::filespace,Id()).value;
    scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
    Check(db::CreateDatabaseFile(create).ok(),"real credentialed database fixture");
    if (mode == "coordinator" || mode == "coordinator-join-retry" || mode == "coordinator-drain" ||
        mode == "coordinator-race" || mode == "coordinator-start-failure" ||
        mode == "coordinator-launch-failure") {
      server::ServerBootstrapConfig config;
      config.database_default_path = create.path;
      config.control_dir = artifacts.path() / "control";
      config.sbps_endpoint = config.control_dir / "ipc.sock";
      auto opened = server::StartHostedEngine(config);
      Check(opened.ok(), "actual exclusive server database open");
      // Old lifecycle observations must not turn into component configuration.
      opened.state.databases.front().selected_agent_type_ids = {
          "page_allocation_manager", "filespace_capacity_manager"};
      server::HostedEngineState copied_status;
      copied_status.engine_context_active = true;
      copied_status.databases = opened.state.databases;
      server::ServerAgentRuntime unowned;
      std::vector<server::ServerDiagnostic> refused;
      Check(!unowned.StartCoordinator(config, copied_status, &refused),
            "copied status cannot start the coordinator");
      Check(refused.size() == 1 && refused.front().code ==
            "SERVER.AGENT_RUNTIME.OWNERSHIP_REQUIRED", "exact ownership refusal");
      if (mode == "coordinator-start-failure") {
        auto failed_config = config;
        failed_config.control_dir = artifacts.path() / "not-a-directory";
        { std::ofstream file(failed_config.control_dir); file << "occupied"; }
        server::ServerAgentRuntime failed;
        std::vector<server::ServerDiagnostic> failures;
        Check(!failed.StartCoordinator(failed_config, opened.state, &failures) &&
              failures.size() == 1 && failures.front().code == "SERVER.AGENT_RUNTIME.STATUS_PATH_FAILED",
              "failure before native launch releases the coordinator reservation");
      }
      if (mode == "coordinator-launch-failure") {
        server::ServerAgentRuntime failed;
        std::vector<server::ServerDiagnostic> failures;
        fail_coordinator_launch = true;
        bool threw = false;
        try { (void)failed.StartCoordinator(config, opened.state, &failures); }
        catch (const std::system_error&) { threw = true; }
        Check(threw && failed.Snapshot().startup_failure && !failed.Snapshot().started &&
              failed.Stop().ok(), "native launch failure retains evidence and unwinds its reservation");
        Check(failed.StartCoordinator(config, opened.state, &failures) && failed.Stop().ok(),
              "the original runtime can retry after a failed coordinator launch");
      }
      if (mode == "coordinator-race") {
        server::ServerAgentRuntime first, second;
        std::array<bool, 2> accepted{};
        std::array<std::exception_ptr, 2> errors{};
        std::array<std::vector<server::ServerDiagnostic>, 2> messages;
        std::barrier start(3);
        const auto launch = [&](server::ServerAgentRuntime& runtime, unsigned index) {
          start.arrive_and_wait();
          try { accepted[index] = runtime.StartCoordinator(config, opened.state, &messages[index]); }
          catch (...) { errors[index] = std::current_exception(); }
        };
        std::thread a(launch, std::ref(first), 0), b(launch, std::ref(second), 1);
        start.arrive_and_wait();
        a.join(); b.join();
        Check(!errors[0] && !errors[1] && accepted[0] != accepted[1],
              "exactly one concurrent coordinator acquires the retained owner");
        const unsigned loser = accepted[0] ? 1 : 0;
        Check(messages[loser].size() == 1 && messages[loser].front().code ==
              "SB_DIAG_AGENT_COORDINATOR_DUPLICATE", "exact duplicate-coordinator refusal");
        Check(first.Stop().ok() && second.Stop().ok(), "racing runtimes drain cleanly");
      }
      const auto bytes = Read(create.path);
      const auto inventory = db::LoadLocalTransactionInventoryFromDatabase(create.path);
      Check(inventory.ok(), "actual coordinator inventory baseline");
      const auto before_begins = begins.load();
      server::ServerAgentRuntime coordinator;
      std::vector<server::ServerDiagnostic> diagnostics;
      Check(coordinator.StartCoordinator(config, opened.state, &diagnostics),
            "coordinator starts without an agent catalog or client principal");
      Check(diagnostics.empty(), "unconfigured coordinator starts cleanly");
      Check(coordinator.StartCoordinator(config, opened.state, &diagnostics),
            "coordinator start is idempotent");
      server::ServerAgentRuntime contender;
      const auto duplicate_refused = [&] {
        std::vector<server::ServerDiagnostic> duplicate;
        Check(!contender.StartCoordinator(config, opened.state, &duplicate) &&
              duplicate.size() == 1 && duplicate.front().code == "SB_DIAG_AGENT_COORDINATOR_DUPLICATE",
              "another runtime cannot start before the coordinator is fully joined");
      };
      duplicate_refused();
      // Exceed the former 750 ms + 5 s timer once. An idle coordinator must
      // remain event-driven rather than periodically rewriting its status file.
      const auto status_before_idle = Read(config.control_dir / "sb_server.agent_runtime.json");
      if (mode == "coordinator") std::this_thread::sleep_for(std::chrono::seconds(6));
      const auto running = coordinator.Snapshot();
      Check(running.started && running.scheduler_ticks == 0 &&
            running.worker_thread_count == 0 && running.workers.empty() &&
            running.background_worker_slots == 0 && running.durable_lease_count == 0,
            "only the coordinator runs; no idle ticks, unconfigured worker or lease");
      Check(Read(config.control_dir / "sb_server.agent_runtime.json") == status_before_idle,
            "idle coordinator does not rewrite status");
      Check(running.startup_agent_state == "completed" &&
            running.shutdown_agent_state == "not_started" &&
            !running.coordinator_thread_id.empty() &&
            running.startup_agent_thread_id == running.coordinator_thread_id,
            "coordinator executes startup agent on its own thread");
      if (mode == "coordinator-drain") {
        coordinator.BeginDrain();
        const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (coordinator.Snapshot().shutdown_agent_state != "running" &&
               std::chrono::steady_clock::now() < drain_deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto draining = coordinator.Snapshot();
        Check(draining.started && draining.stopping &&
              draining.shutdown_agent_state == "running" &&
              draining.coordinator_thread_id == running.coordinator_thread_id,
              "coordinator remains alive while its owner drains IPC components");
        duplicate_refused();
      }
      if (mode == "coordinator-join-retry") {
        join_failure_runtime = &coordinator;
        fail_coordinator_join = true;
        bool failed = false;
        try { (void)coordinator.Stop(); }
        catch (const std::system_error&) { failed = true; }
        Check(failed && coordinator.Snapshot().shutdown_agent_state == "completed",
              "injected join failure must follow coordinator cleanup completion");
        duplicate_refused();
        std::vector<server::ServerDiagnostic> restart;
        Check(!coordinator.StartCoordinator(config, opened.state, &restart) &&
              restart.size() == 1 && restart.front().code == "AGENT.INVALID_STATE",
              "completed service cleanup cannot replace an unjoined native coordinator");
      }
      Check(coordinator.Stop().ok() && coordinator.Stop().ok(),
            "coordinator joins cleanly and repeated stop is safe");
      Check(!coordinator.Snapshot().started, "coordinator stopped");
      const auto stopped = coordinator.Snapshot();
      Check(stopped.shutdown_agent_state == "completed" &&
            stopped.shutdown_agent_thread_id == running.coordinator_thread_id,
            "same coordinator executes shutdown agent before terminating");
      Check(contender.StartCoordinator(config, opened.state, &diagnostics) && contender.Stop().ok(),
            "successful native join permits the next coordinator");
      const auto after = db::LoadLocalTransactionInventoryFromDatabase(create.path);
      Check(after.ok() && after.inventory.next_local_transaction_id ==
            inventory.inventory.next_local_transaction_id && Read(create.path) == bytes,
            "coordinator start, idle and stop do not mutate the database");
      Check(begins == before_begins && commits == 0 && rollbacks == 0 && persists == 0,
            "unconfigured coordinator does no background transaction or catalog seed");
      std::cout << "server_configured_coordinator=PASS\n";
      return 0;
    }
    // A copyable status record is deliberately NOT an admitted service owner.
    // Even healthy-looking state and arbitrary epochs cannot supply authority.
    server::HostedEngineState hosted;hosted.engine_context_active=true;
    server::HostedDatabaseSnapshot snapshot;snapshot.state=server::HostedDatabaseState::kOpen;
    snapshot.database_path=create.path;snapshot.database_uuid=create.database_uuid.value;snapshot.filespace_uuid=create.filespace_uuid.value;
    snapshot.database_open=true;snapshot.write_admission_fenced=false;snapshot.config_policy_security_lifecycle_present=true;
    if(mode=="zero-epochs")snapshot.policy_generation=snapshot.security_epoch=snapshot.cache_invalidation_epoch=0;
    else if(mode=="unrelated-epochs"){
      snapshot.policy_generation=79;snapshot.security_epoch=73;snapshot.cache_invalidation_epoch=83;
      snapshot.config_source_epoch=101;snapshot.config_reload_generation=103;snapshot.capability_policy_generation=107;
    }else if(mode=="health-claims"){
      snapshot.database_engine_agent_state="active";snapshot.database_engine_agent_ordinary_admission_allowed=true;
      snapshot.security_provider_state="healthy";snapshot.security_provider_generation=97;
    }else Check(mode=="defaults","supported mode");
    hosted.databases.push_back(snapshot);
    server::ServerBootstrapConfig config;config.database_default_path=create.path;config.control_dir=artifacts.path()/"control";
    const auto inventory=db::LoadLocalTransactionInventoryFromDatabase(create.path);
    Check(inventory.ok(),"actual pre-admission inventory");const auto bytes=Read(create.path);
    server::ServerAgentRuntime runtime;
    for(unsigned attempt=1;attempt<=2;++attempt){
      std::vector<server::ServerDiagnostic> diagnostics;
      Check(!runtime.Start(config,hosted,&diagnostics),"unadmitted service cannot start agent runtime");
      Check(begins==attempt&&metadata_only,"every BEGIN request has no fabricated identity, epoch, grants or bootstrap role");
      Check(diagnostics.size()==1&&diagnostics.front().code=="ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED"&&
          diagnostics.front().occurrence_uuid==source_occurrence,"retain exact engine-owned missing-security denial and occurrence");
      bool missing=false;
      for(const auto& field:diagnostics.front().fields)
        missing|=field.key=="authorization_context_failure"&&field.value=="security_context_missing";
      Check(missing,"absence is not reported as expired credentials");
      const auto after=db::LoadLocalTransactionInventoryFromDatabase(create.path);
      Check(after.ok()&&after.inventory.next_local_transaction_id==inventory.inventory.next_local_transaction_id&&
          after.inventory.entries.size()==inventory.inventory.entries.size()&&Read(create.path)==bytes,
          "authority refusal changes neither transaction inventory nor actual database bytes");
      Check(commits==0&&rollbacks==0&&persists==0,"no seed or finality work after failed BEGIN admission");
      const auto state=runtime.Snapshot();
      Check(!state.started&&!state.stopping&&state.worker_thread_count==0&&state.scheduler_ticks==0&&state.total_worker_ticks==0,
          "planned worker records are not reported as started native worker threads");
      const auto projected=server::BuildIparAgentLifecycleProjectionSource(state);
      Check(!projected.started&&projected.worker_thread_count==0,"agent lifecycle projection retains actual zero native workers");
      Check(!runtime.Stop().attempted,"no durable shutdown transaction for a runtime that never started");
      Check(!std::filesystem::exists(config.control_dir/"sb_server.agent_runtime.json"),"failed admission publishes no running status file");
    }
  }
  artifacts.Cleanup();std::cout<<"server_agent_authority_admission=PASS "<<mode<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
