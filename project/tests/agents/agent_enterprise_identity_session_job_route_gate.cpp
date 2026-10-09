// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/database_fixture_cleanup.hpp"
#include "../support/durable_authorization_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "agent_binary_identity_fixture.hpp"
#include "../support/binary_uuid_fixture.hpp"
using scratchbird::tests::BinaryFixtureIdentity;
using scratchbird::tests::NativeFixtureIdentity;
using scratchbird::tests::FixtureIdentityForLabel;
#include "agents/identity_manager.hpp"
#include "agents/job_control_manager.hpp"
#include "agents/session_control_manager.hpp"
#include "agent_background_jobs.hpp"
#include "agent_local_workflow.hpp"
#include "agent_session_control_route_bridge.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "physical_mga_cow_store.hpp"
#include "security/authentication_api.hpp"
#include "security/durable_authorization_projection.hpp"
#include "security/runtime_principal_observation.hpp"
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
  throw std::runtime_error(message);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

std::string Diagnostics(const api::EngineApiResult& result) {
  std::string details;
  for (const auto& diagnostic : result.diagnostics)
    details += diagnostic.code + ":" + diagnostic.detail + ";";
  return details;
}

struct TestDatabase {
  std::filesystem::path path;
  std::string database_uuid;
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t resource_epoch = 0;
};

TestDatabase CreateActiveDatabase(const std::filesystem::path& path) {

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
  create.resource_seed_pack_root = (std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
      "resources/seed-packs/initial-resource-pack").string();
  create.require_resource_seed_pack = true;
  create.bootstrap_principal_name = "agent_identity_owner";
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=0358b60b6875c81e17d3e0ab67f8b785f49d4146547c79da401f21dc641c2c16";
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "database creation failed: " + created.diagnostic.diagnostic_code);

  auto initial_inventory = db::LoadLocalTransactionInventoryFromDatabase(path.string());
  Require(initial_inventory.ok() && initial_inventory.inventory.publication_base.has_value(),
          "lifecycle-published transaction inventory unavailable");
  auto inventory = std::move(initial_inventory.inventory);
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
  result.database_uuid = BinaryFixtureIdentity(database_uuid.value.value);
  result.transaction_uuid = BinaryFixtureIdentity(transaction_uuid.value.value);
  result.local_transaction_id = begun.entry.identity.local_id.value;
  result.resource_epoch = created.state.resource_seed_catalog.resource_epoch;
  return result;
}

api::EngineRequestContext Context(const TestDatabase& database) {
  api::EngineRequestContext context;
  context.request_id = "aeic-identity-session-job";
  context.database_path = database.path.string();
  context.database_uuid = NativeFixtureIdentity(database.database_uuid);
  context.transaction_uuid = NativeFixtureIdentity(database.transaction_uuid);
  context.local_transaction_id = database.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      database.local_transaction_id;
  context.security_context_present = true;
  context.resource_epoch = database.resource_epoch;
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(context.database_path);
  Require(bootstrap.ok() && bootstrap.state.present, "bootstrap principal unavailable");
  context.principal_uuid = bootstrap.state.principal_uuid.value;
  context.node_uuid = NativeFixtureIdentity(FixtureIdentityForLabel("identity-route-node"));
  context.session_uuid = NativeFixtureIdentity(FixtureIdentityForLabel("identity-route-session"));
  context.catalog_generation_id = 1;
  context.trust_mode = api::EngineTrustMode::embedded_in_process;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  return context;
}

agents::DurableAgentCatalogImage DurableWorkflowCatalog() {
  agents::DurableAgentCatalogImage image;
  image.source = agents::AgentCatalogStateSource::durable_catalog_image;
  image.schema_version = 1;
  image.authority.durable_catalog_authority = true;
  image.authority.mga_transaction_evidence = true;
  image.authority.mga_transaction_uuid =
      BinaryFixtureIdentity(scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002903"));
  image.authority.transaction_generation = 29;
  image.authority.evidence_uuid = BinaryFixtureIdentity(scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002904"));
  image.authority.database_uuid = BinaryFixtureIdentity(scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002900"));
  image.authority.catalog_storage_uuid =
      BinaryFixtureIdentity(scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002911"));
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
  request->database_uuid = BinaryFixtureIdentity(scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002900"));
  request->principal_uuid = BinaryFixtureIdentity(scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002901"));
  request->mga_transaction_uuid = BinaryFixtureIdentity(scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002903"));
  request->evidence_uuid = BinaryFixtureIdentity(scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002904"));
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
  startup.database_uuid = BinaryFixtureIdentity(scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002900"));
  startup.policy_generation = 29;
  startup.tx2_activation_committed = true;
  startup.startup_admitted = true;
  startup.scheduler_catalog_visible = true;
  startup.monotonic_now_microseconds = 29001;
  agents::BackgroundJobPolicy policy;
  policy.policy_uuid = FixtureIdentityForLabel("aeic029-background-policy");
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
  job.database_uuid = BinaryFixtureIdentity(scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002900"));
  job.pool_id = "background";
  job.workload_class = agents::WorkloadClass::background;
  job.source = agents::WorkloadAdmissionSource::engine;
  job.resource_request = Resources(1);
  return job;
}

void TestIdentityManagerRoutesThroughEngineSecurityApi() {
  scratchbird::tests::OwnedTempDirectory temporary;
  const auto database =
      CreateActiveDatabase(temporary.path() / "identity-route.sbdb");
  auto context = Context(database);

  impl::IdentityManagerRequest identity;
  const auto target = scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002a01");
  SetGenericWorkflowAuthority(&identity, BinaryFixtureIdentity(target));
  identity.principal_uuid = BinaryFixtureIdentity(target);
  identity.operator_principal_uuid = BinaryFixtureIdentity(context.principal_uuid);
  identity.lock_requested = true;
  identity.identity_metrics_authoritative = true;
  identity.explicit_admin_request = true;
  identity.redaction_policy_valid = true;

  api::EngineSecurityCreatePrincipalRequest create;
  create.context = context;
  create.principal_uuid = target;
  create.principal_name = "lock_target";
  create.credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=0358b60b6875c81e17d3e0ab67f8b785f49d4146547c79da401f21dc641c2c16";
  const auto created = api::EngineSecurityCreatePrincipal(create);
  Require(created.ok && created.principal_created,
          "target principal creation failed: " + Diagnostics(created));
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  api::EngineSecurityGrantPrivilegeRequest grant;
  grant.context = context;
  grant.grant_uuid = NativeFixtureIdentity(FixtureIdentityForLabel("identity-route-grant"));
  grant.grantee_uuid = target;
  grant.target_object_uuid = NativeFixtureIdentity(FixtureIdentityForLabel("identity-route-table"));
  grant.target_object_kind = "table";
  grant.privilege = "SELECT";
  const auto granted = api::EngineSecurityGrantPrivilege(grant);
  Require(granted.ok && granted.privilege_granted, "target grant failed: " + Diagnostics(granted));
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  api::EngineSecurityEvaluatePrivilegeRequest privilege;
  privilege.context = context;
  privilege.principal_uuid = target;
  privilege.target_object_uuid = grant.target_object_uuid;
  privilege.target_object_kind = grant.target_object_kind;
  privilege.privilege = grant.privilege;
  Require(api::EngineSecurityEvaluatePrivilege(privilege).authorized, "active target grant was not effective");
  const auto before = api::LoadSecurityPrincipalLifecycleState(context);
  Require(before.ok, "principal state unavailable before lock");
  api::EngineAuthenticateRequest authenticate;
  authenticate.context = context;
  authenticate.provider_family = "local_password";
  authenticate.principal_claim = create.principal_name;
  authenticate.durable_principal_uuid = target;
  authenticate.credential_evidence = "intentionally-wrong-fixture-password";
  authenticate.credential_evidence_present = true;
  const auto initially_wrong_password = api::EngineAuthenticate(authenticate);
  Require(!initially_wrong_password.ok && Diagnostics(initially_wrong_password).find(
              "credential_verifier_mismatch") != std::string::npos,
          "active target did not reach password verification: " + Diagnostics(initially_wrong_password));

  auto catalog = DurableWorkflowCatalog();
  agents::AgentLocalWorkflowLedger ledger(&catalog);
  const auto decision = impl::EvaluateIdentityManagerRequest(&ledger, identity);
  Require(decision.ok() &&
              decision.decision == impl::IdentityManagerDecisionKind::lock_user,
          "identity manager did not approve local lock workflow");

  api::EngineAlterIdentityRequest alter;
  alter.context = context;
  alter.option_envelopes.push_back("agent_decision:lock_user");
  alter.target_object.uuid = target;
  const auto altered = api::EngineAlterIdentity(alter);
  Require(altered.ok && altered.primary_object.uuid == target &&
              altered.operation_id == "security.alter_identity",
          "identity manager route did not reach engine security API: " + Diagnostics(altered));

  const auto locked = api::LoadSecurityPrincipalLifecycleState(context);
  Require(locked.ok && locked.state.security_generation > before.state.security_generation,
          "lock did not publish a new security generation");
  bool found_target = false;
  for (const auto& principal : locked.state.principals) {
    if (principal.deleted) continue;
    if (principal.principal_uuid == target) {
      Require(principal.lifecycle_state == "disabled", "target was not actually locked");
      found_target = true;
    } else {
      Require(principal.lifecycle_state == "active", "lock changed an unrelated principal");
    }
  }
  Require(found_target, "locked target disappeared from principal catalog");
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  alter.context = context;
  privilege.context = context;
  Require(!api::EngineSecurityEvaluatePrivilege(privilege).authorized,
          "disabled principal retained effective grants");
  const auto projected = api::ProjectDurableAuthorizationState(locked.state,
      {context.database_uuid, target, context.security_epoch,
       locked.state.policy_generation, context.catalog_generation_id});
  for (const auto& principal : projected.principals)
    Require(principal.principal_uuid != target, "disabled principal entered authorization projection");
  create.context = context;
  Require(!api::EngineSecurityCreatePrincipal(create).ok,
          "disabled principal identity was reused as a new principal");

  for (const auto& options : std::vector<std::vector<std::string>>{
           {"agent_decision:unknown"}, {"agent_decision:lock_user", "agent_decision:lock_user"},
           {"agent_decision:lock_user", "identity_uuid:" + identity.principal_uuid}}) {
    auto invalid = alter;
    invalid.option_envelopes = options;
    Require(!api::EngineAlterIdentity(invalid).ok, "invalid action fell through to metadata success");
  }
  auto wrong_version = target;
  wrong_version.bytes[6] = 0x40;
  for (auto invalid_target : std::vector<api::EngineUuid>{
           {}, wrong_version, NativeFixtureIdentity(FixtureIdentityForLabel("missing-identity-target")), context.principal_uuid}) {
    auto invalid = alter;
    invalid.target_object.uuid = invalid_target;
    Require(!api::EngineAlterIdentity(invalid).ok, "invalid or protected principal lock accepted");
  }

  api::EngineAlterIdentityRequest denied = alter;
  denied.context.security_context_present = false;
  const auto denied_result = api::EngineAlterIdentity(denied);
  Require(!denied_result.ok,
          "identity engine API accepted missing security context");
  denied = alter;
  denied.context.authorization_context = {};
  denied.context.trace_tags = {"security.bootstrap", "security.fixture_trace_authority", "right:SEC_IDENTITY_ADMIN"};
  Require(!api::EngineAlterIdentity(denied).ok, "trace assertions conferred lock authority");
  const auto after_refusals = api::LoadSecurityPrincipalLifecycleState(context);
  Require(after_refusals.ok && after_refusals.state.security_generation == locked.state.security_generation,
          "refused lock changed security state");

  api::EngineSecurityAlterPrincipalRequest unlock;
  unlock.context = context;
  unlock.principal_uuid = target;
  unlock.lifecycle_state = "active";
  const auto unlocked = api::EngineSecurityAlterPrincipal(unlock);
  Require(unlocked.ok && unlocked.principal_altered, "disabled principal could not be unlocked: " + Diagnostics(unlocked));
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  privilege.context = context;
  Require(api::EngineSecurityEvaluatePrivilege(privilege).authorized,
          "unlock failed to restore existing grant without recreating identity");
  alter.context = context;
  Require(api::EngineAlterIdentity(alter).ok, "unlocked principal could not be relocked");

  db::PhysicalMgaCowFinalizeRequest commit;
  commit.database_path = database.path.string();
  commit.transaction = {mga::MakeLocalTransactionId(database.local_transaction_id),
      {UuidKind::transaction, NativeFixtureIdentity(database.transaction_uuid)}, mga::TransactionScope::local_node};
  commit.decision = db::PhysicalMgaCowFinalizeDecision::commit;
  commit.final_unix_epoch_millis = 1800000002010ull;
  Require(db::FinalizePhysicalMgaCowTransaction(commit).ok(), "lock commit publication failed");
  auto observer = context;
  observer.local_transaction_id = 0;
  observer.transaction_uuid = {};
  observer.snapshot_visible_through_local_transaction_id = database.local_transaction_id;
  const auto reopened = api::LoadSecurityPrincipalLifecycleState(observer);
  Require(reopened.ok, "committed principal catalog did not reopen");
  found_target = false;
  for (const auto& principal : reopened.state.principals)
    if (!principal.deleted && principal.principal_uuid == target)
      found_target = principal.lifecycle_state == "disabled";
  Require(found_target, "committed lock lost on independent catalog reload");
  const auto observation = api::InspectRuntimePrincipal(
      {observer.database_path, observer.database_uuid, target});
  Require(observation.outcome == api::RuntimePrincipalObservationOutcome::no_active_principal &&
              !observation.observation.has_value(), "runtime observation exposed a disabled principal");
  authenticate.context = observer;
  const auto refused_login = api::EngineAuthenticate(authenticate);
  Require(!refused_login.ok && !refused_login.authenticated &&
              Diagnostics(refused_login).find("durable_principal_disabled") != std::string::npos,
          "authentication did not enforce committed lock: " + Diagnostics(refused_login));
  Require(catalog.evidence.size() == 1 && catalog.actions.size() == 1,
          "identity workflow did not persist durable action evidence");
  temporary.Cleanup();
}

void TestSessionControlRejectsInvalidBinaryIdentity() {
  server::ServerSessionRegistry registry;
  const auto identity = scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002b01");
  server::ServerSessionRecord session;
  session.session_uuid = identity.bytes;
  registry.sessions_by_uuid.emplace(identity, session);
  auto wrong_version = identity;
  wrong_version.bytes[6] = 0x60;
  const std::vector<std::string> invalid{
      {}, std::string(15, '\0'), std::string(17, '\0'), std::string(16, '\0'),
      "019f0900-0000-7000-8000-000000002b01", BinaryFixtureIdentity(wrong_version)};
  for (const auto& bytes : invalid) {
    impl::SessionControlManagerRequest request;
    request.session_uuid = bytes;
    request.disconnect_requested = true;
    const auto refused = server::ApplySessionControlAgentRoute(&registry, nullptr, request);
    Require(!refused.ok() && !refused.registry_mutated &&
                refused.diagnostic_code == "SB_AGENT_SESSION_CONTROL_ROUTE.SESSION_IDENTITY_INVALID" &&
                registry.sessions_by_uuid.size() == 1 && registry.sessions_by_uuid.contains(identity),
            "session bridge did not reject malformed identity before ledger/registry mutation");
  }
}

void TestSessionControlMutatesServerRegistry() {
  auto catalog = DurableWorkflowCatalog();
  agents::AgentLocalWorkflowLedger ledger(&catalog);
  server::ServerSessionRegistry registry;
  server::ServerSessionRecord session;
  session.database_uuid = scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002900");
  session.engine_authorization_trace_tags.push_back("right:OBS_AGENT_CONTROL");
  const std::string session_uuid = BinaryFixtureIdentity(scratchbird::tests::FixtureUuidLiteral("019f0900-0000-7000-8000-000000002b01"));
  session.session_uuid = NativeFixtureIdentity(session_uuid).bytes;
  registry.sessions_by_uuid.emplace(NativeFixtureIdentity(session_uuid), session);

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

  session.session_uuid = NativeFixtureIdentity(session_uuid).bytes;
  registry.sessions_by_uuid.emplace(NativeFixtureIdentity(session_uuid), session);
  request.disconnect_requested = false;
  request.reauth_requested = true;
  request.idempotency_key = "idem:aeic029:reauth";
  const auto reauth =
      server::ApplySessionControlAgentRoute(&registry, &ledger, request);
  Require(reauth.ok() && reauth.registry_mutated && reauth.reauth_required,
          "session control bridge did not mark reauth required");
  Require(!registry.sessions_by_uuid.at(NativeFixtureIdentity(session_uuid))
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
  const auto cancel_uuid = FixtureIdentityForLabel("aeic029-job-cancel");
  const auto suppress_uuid = FixtureIdentityForLabel("aeic029-job-suppress");
  Require(scheduler.RegisterJob(Job(cancel_uuid)).ok,
          "cancel job registration failed");
  const auto started = scheduler.RunNextDue(&quota, 29002);
  Require(started.admitted(), "cancel job did not start");

  impl::JobControlManagerRequest cancel;
  cancel.job_uuid = cancel_uuid;
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

  Require(scheduler.RegisterJob(Job(suppress_uuid)).ok,
          "suppress job registration failed");
  impl::JobControlManagerRequest suppress;
  suppress.job_uuid = suppress_uuid;
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
  try {
  const auto memory = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(), "agent-identity-route");
  Require(memory.ok() && memory.fixture_mode, "memory fixture admission failed");
  TestSessionControlRejectsInvalidBinaryIdentity();
  TestIdentityManagerRoutesThroughEngineSecurityApi();
  TestSessionControlMutatesServerRegistry();
  TestJobControlMutatesBackgroundScheduler();
  return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
