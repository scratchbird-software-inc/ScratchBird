// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/identity_manager.hpp"
#include "agents/job_control_manager.hpp"
#include "agents/session_control_manager.hpp"
#include "agent_background_jobs.hpp"
#include "agent_local_workflow.hpp"
#include "agent_session_control_route_bridge.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "security/identity_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace impl = scratchbird::core::agents::implemented_agents;
namespace mga = scratchbird::transaction::mga;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

struct TestDatabase {
  std::filesystem::path path;
  std::string database_uuid;
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
};

void Cleanup(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const char* suffix : {".dirty.manifest",
                             ".sb.api_events",
                             ".sb.agent_catalog",
                             ".sb.mga_event_sequence_allocator",
                             ".sb.mga_index_entries",
                             ".sb.mga_large_values",
                             ".sb.mga_relation_descriptors",
                             ".sb.mga_relation_metadata",
                             ".sb.mga_row_versions",
                             ".sb.mga_savepoints",
                             ".sb.mga_secondary_index_delta_ledger"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

TestDatabase CreateActiveDatabase(const char* basename) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / basename;
  Cleanup(path);

  const auto database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1800000002001ull);
  const auto filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1800000002002ull);
  Require(database_uuid.ok(), "database UUID generation failed");
  Require(filespace_uuid.ok(), "filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1800000002003ull;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(), "database creation failed");

  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  const auto transaction_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::transaction, 1800000002004ull);
  Require(transaction_uuid.ok(), "transaction UUID generation failed");
  auto begun = mga::BeginLocalTransaction(std::move(inventory),
                                          transaction_uuid.value,
                                          1800000002005ull);
  Require(begun.ok(), "local transaction begin failed");
  Require(db::PersistLocalTransactionInventoryToDatabase(path.string(),
                                                         begun.inventory)
              .ok(),
          "local transaction inventory persist failed");

  TestDatabase result;
  result.path = path;
  result.database_uuid = uuid::UuidToString(database_uuid.value.value);
  result.transaction_uuid = uuid::UuidToString(transaction_uuid.value.value);
  result.local_transaction_id = begun.entry.identity.local_id.value;
  return result;
}

api::EngineRequestContext Context(const TestDatabase& database) {
  api::EngineRequestContext context;
  context.request_id = "aeic-identity-session-job";
  context.database_path = database.path.string();
  context.database_uuid.canonical = database.database_uuid;
  context.transaction_uuid.canonical = database.transaction_uuid;
  context.local_transaction_id = database.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      database.local_transaction_id;
  context.security_context_present = true;
  context.principal_uuid.canonical =
      "019f0900-0000-7000-8000-000000002901";
  context.trace_tags.push_back("security.bootstrap");
  return context;
}

agents::DurableAgentCatalogImage DurableWorkflowCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid =
      "019f0900-0000-7000-8000-000000002903";
  image.authority.transaction_generation = 29;
  image.authority.evidence_uuid = "019f0900-0000-7000-8000-000000002904";
  image.authority.database_uuid = "019f0900-0000-7000-8000-000000002900";
  image.authority.catalog_storage_uuid =
      "019f0900-0000-7000-8000-000000002911";
  image.authority.storage_commit_evidence_uuid = image.authority.evidence_uuid;
  image.authority.catalog_generation = 29;
  image.authority.local_transaction_id = 29001;
  image.authority.storage_catalog_record_evidence = true;
  image.authority.transaction_inventory_bound = true;
  image.authority.fsync_or_checkpoint_evidence = true;
  const auto refresh = agents::RefreshDurableAgentCatalogAuthorityDigest(
      &image, image.authority.evidence_uuid);
  Require(refresh.ok, "durable workflow catalog root refresh failed");
  return image;
}

template <typename TRequest>
void SetGenericWorkflowAuthority(TRequest* request, const std::string& subject) {
  request->database_uuid = "019f0900-0000-7000-8000-000000002900";
  request->principal_uuid = "019f0900-0000-7000-8000-000000002901";
  request->mga_transaction_uuid = "019f0900-0000-7000-8000-000000002903";
  request->evidence_uuid = "019f0900-0000-7000-8000-000000002904";
  request->idempotency_key = "idem:aeic029:" + subject;
  request->local_transaction_id = 29001;
  request->catalog_generation = 29;
  request->durable_catalog_bound = true;
  request->transaction_inventory_bound = true;
  request->intended_state_observed = true;
}

agents::WorkloadResourceVector Resources(std::uint64_t value) {
  agents::WorkloadResourceVector resources;
  resources.memory_bytes = value;
  resources.worker_slots = value;
  resources.temp_bytes = value;
  resources.filespace_bytes = value;
  resources.active_requests = value;
  resources.open_cursors = value;
  resources.transaction_slots = value;
  resources.buffer_bytes = value;
  resources.udr_bytes = value;
  return resources;
}

agents::WorkloadResourceQuotaController Quota() {
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = "background";
  pool.workload_class = agents::WorkloadClass::background;
  pool.limits.hard = Resources(10);
  pool.limits.soft = Resources(10);
  agents::WorkloadResourceQuotaController quota;
  Require(quota.RegisterPool(pool).ok, "quota pool registration failed");
  return quota;
}

agents::DatabaseLocalBackgroundJobScheduler StartedScheduler() {
  agents::DatabaseLocalBackgroundJobScheduler scheduler;
  agents::BackgroundJobSchedulerStartup startup;
  startup.database_uuid = "019f0900-0000-7000-8000-000000002900";
  startup.policy_generation = 29;
  startup.tx2_activation_committed = true;
  startup.startup_admitted = true;
  startup.scheduler_catalog_visible = true;
  startup.monotonic_now_microseconds = 29001;
  agents::BackgroundJobPolicy policy;
  policy.policy_uuid = "policy:aeic029:background_jobs";
  policy.max_attempts = 3;
  policy.initial_backoff_microseconds = 10;
  policy.max_backoff_microseconds = 80;
  Require(scheduler.Start(startup, policy).ok, "scheduler start failed");
  return scheduler;
}

agents::BackgroundJobDefinition Job(std::string job_uuid) {
  agents::BackgroundJobDefinition job;
  job.job_uuid = std::move(job_uuid);
  job.job_type = "aeic029_job";
  job.database_uuid = "019f0900-0000-7000-8000-000000002900";
  job.pool_id = "background";
  job.workload_class = agents::WorkloadClass::background;
  job.source = agents::WorkloadAdmissionSource::engine;
  job.resource_request = Resources(1);
  return job;
}

void TestIdentityManagerRoutesThroughEngineSecurityApi() {
  const auto database =
      CreateActiveDatabase("scratchbird_aeic_identity_route.sbdb");
  auto context = Context(database);

  impl::IdentityManagerRequest identity;
  identity.principal_uuid = "019f0900-0000-7000-8000-000000002a01";
  SetGenericWorkflowAuthority(&identity, identity.principal_uuid);
  identity.operator_principal_uuid = context.principal_uuid.canonical;
  identity.lock_requested = true;
  identity.identity_metrics_authoritative = true;
  identity.explicit_admin_request = true;
  identity.redaction_policy_valid = true;

  auto catalog = DurableWorkflowCatalog();
  agents::AgentLocalWorkflowLedger ledger(&catalog);
  const auto decision = impl::EvaluateIdentityManagerRequest(&ledger, identity);
  Require(decision.ok() &&
              decision.decision == impl::IdentityManagerDecisionKind::lock_user,
          "identity manager did not approve local lock workflow");

  api::EngineAlterIdentityRequest alter;
  alter.context = context;
  alter.option_envelopes.push_back("agent_decision:lock_user");
  alter.option_envelopes.push_back("identity_uuid:" + identity.principal_uuid);
  const auto altered = api::EngineAlterIdentity(alter);
  Require(altered.ok, "identity manager route did not reach engine security API");

  api::EngineAlterIdentityRequest denied = alter;
  denied.context.security_context_present = false;
  const auto denied_result = api::EngineAlterIdentity(denied);
  Require(!denied_result.ok,
          "identity engine API accepted missing security context");
  Require(catalog.evidence.size() == 1 && catalog.actions.size() == 1,
          "identity workflow did not persist durable action evidence");
  Cleanup(database.path);
}

void TestSessionControlMutatesServerRegistry() {
  auto catalog = DurableWorkflowCatalog();
  agents::AgentLocalWorkflowLedger ledger(&catalog);
  server::ServerSessionRegistry registry;
  server::ServerSessionRecord session;
  session.database_uuid = "019f0900-0000-7000-8000-000000002900";
  session.engine_authorization_trace_tags.push_back("right:OBS_AGENT_CONTROL");
  const std::string session_uuid = "019f0900-0000-7000-8000-000000002b01";
  registry.sessions_by_uuid.emplace(session_uuid, session);

  impl::SessionControlManagerRequest request;
  request.session_uuid = session_uuid;
  SetGenericWorkflowAuthority(&request, request.session_uuid);
  request.disconnect_requested = true;
  request.session_visible = true;
  request.disconnect_allowed = true;
  request.security_metrics_authoritative = true;
  const auto disconnected =
      server::ApplySessionControlAgentRoute(&registry, &ledger, request);
  Require(disconnected.ok() && disconnected.registry_mutated &&
              disconnected.session_removed,
          "session control bridge did not remove disconnected session");
  Require(registry.sessions_by_uuid.empty(),
          "session control bridge left disconnected session visible");

  registry.sessions_by_uuid.emplace(session_uuid, session);
  request.disconnect_requested = false;
  request.reauth_requested = true;
  request.idempotency_key = "idem:aeic029:reauth";
  const auto reauth =
      server::ApplySessionControlAgentRoute(&registry, &ledger, request);
  Require(reauth.ok() && reauth.registry_mutated && reauth.reauth_required,
          "session control bridge did not mark reauth required");
  Require(!registry.sessions_by_uuid.at(session_uuid)
               .engine_authorization_trace_tags.empty(),
          "session reauth evidence was not attached to server registry");

  request.client_authority = true;
  request.idempotency_key = "idem:aeic029:client-refused";
  const auto refused =
      server::ApplySessionControlAgentRoute(&registry, &ledger, request);
  Require(!refused.manager_result.ok(),
          "session control bridge accepted client authority");
}

void TestJobControlMutatesBackgroundScheduler() {
  auto catalog = DurableWorkflowCatalog();
  agents::AgentLocalWorkflowLedger ledger(&catalog);
  auto scheduler = StartedScheduler();
  auto quota = Quota();
  Require(scheduler.RegisterJob(Job("job-cancel")).ok,
          "cancel job registration failed");
  const auto started = scheduler.RunNextDue(&quota, 29002);
  Require(started.admitted(), "cancel job did not start");

  impl::JobControlManagerRequest cancel;
  cancel.job_uuid = "job-cancel";
  SetGenericWorkflowAuthority(&cancel, cancel.job_uuid);
  cancel.cancel_requested = true;
  cancel.job_visible = true;
  cancel.job_cancellable = true;
  cancel.job_metrics_authoritative = true;
  const auto cancel_decision =
      impl::EvaluateJobControlManagerRequest(&ledger, cancel);
  Require(cancel_decision.ok() &&
              cancel_decision.decision == impl::JobControlManagerDecisionKind::cancel_job,
          "job manager did not approve cancel");
  Require(scheduler.CancelJobControlAction(cancel.job_uuid, &quota, 29003).ok,
          "background scheduler did not cancel running job");

  Require(scheduler.RegisterJob(Job("job-suppress")).ok,
          "suppress job registration failed");
  impl::JobControlManagerRequest suppress;
  suppress.job_uuid = "job-suppress";
  SetGenericWorkflowAuthority(&suppress, suppress.job_uuid);
  suppress.suppress_requested = true;
  suppress.job_visible = true;
  suppress.suppression_scope_valid = true;
  suppress.job_metrics_authoritative = true;
  const auto suppress_decision =
      impl::EvaluateJobControlManagerRequest(&ledger, suppress);
  Require(suppress_decision.ok() &&
              suppress_decision.decision == impl::JobControlManagerDecisionKind::suppress_job,
          "job manager did not approve suppression");
  Require(scheduler.SuppressJobControlAction(suppress.job_uuid, 29004).ok,
          "background scheduler did not suppress job");
  const auto suppressed = scheduler.FindJob(suppress.job_uuid);
  Require(suppressed.has_value() &&
              suppressed->state == agents::BackgroundJobState::denied,
          "suppressed job did not enter denied state");

  impl::JobControlManagerRequest client_owned = cancel;
  client_owned.client_authority = true;
  const auto refused =
      impl::EvaluateJobControlManagerRequest(&ledger, client_owned);
  Require(!refused.ok(), "job control accepted client authority");
}

}  // namespace

int main() {
  TestIdentityManagerRoutesThroughEngineSecurityApi();
  TestSessionControlMutatesServerRegistry();
  TestJobControlMutatesBackgroundScheduler();
  return EXIT_SUCCESS;
}
