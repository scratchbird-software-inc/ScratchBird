// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "security/runtime_principal_observation.hpp"
#include "security/durable_authorization_projection.hpp"
#include "physical_mga_cow_store.hpp"
#include "memory.hpp"
#include "../support/durable_authorization_fixture.hpp"

// Included after the inventory fixture's Check/NewId helpers. These checks
// exercise real principal publication and observation, not authentication or
// a startup admission/recovery receipt.
namespace runtime_principal_checks {
using O = api::RuntimePrincipalObservationOutcome;

inline api::EngineRequestContext Begin(api::EngineRequestContext context) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(context.database_path);
  Check(loaded.ok(), "principal fixture inventory load");
  const auto begun = mga::BeginLocalTransaction(loaded.inventory,
      NewId(platform::UuidKind::transaction), 100);
  Check(begun.ok(), "principal fixture begin");
  Check(db::PersistLocalTransactionInventoryToDatabase(context.database_path, begun.inventory).ok(),
        "principal fixture durable begin");
  context.local_transaction_id = begun.entry.identity.local_id.value;
  context.transaction_uuid = begun.entry.identity.transaction_uuid.value;
  context.snapshot_visible_through_local_transaction_id = begun.entry.begin_visible_through_local_transaction_id;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  return context;
}

inline void Finish(const api::EngineRequestContext& context, bool commit) {
  db::PhysicalMgaCowFinalizeRequest request;
  request.database_path = context.database_path;
  request.transaction = {mga::MakeLocalTransactionId(context.local_transaction_id),
      {platform::UuidKind::transaction, context.transaction_uuid}, mga::TransactionScope::local_node};
  request.decision = commit ? db::PhysicalMgaCowFinalizeDecision::commit
                            : db::PhysicalMgaCowFinalizeDecision::rollback;
  request.final_unix_epoch_millis = 101;
  Check(db::FinalizePhysicalMgaCowTransaction(request).ok(), "principal fixture finality");
}

inline int ColdMain(const char* path) {
  api::RuntimePrincipalObservationRequest request;
  request.database_path = path;
  std::uint64_t principal_generation = 0, context_generation = 0;
  std::uint8_t expect_active = 0;
  std::cin.read(reinterpret_cast<char*>(request.database_uuid.bytes.data()), 16);
  std::cin.read(reinterpret_cast<char*>(request.principal_uuid.bytes.data()), 16);
  std::cin.read(reinterpret_cast<char*>(&principal_generation), sizeof(principal_generation));
  std::cin.read(reinterpret_cast<char*>(&context_generation), sizeof(context_generation));
  std::cin.read(reinterpret_cast<char*>(&expect_active), sizeof(expect_active));
  if (!std::cin) return 2;
  const auto unconfigured = api::InspectRuntimePrincipal(request);
  const auto native = db::ReadDatabaseBootstrapSecurityCatalog(path);
  if (unconfigured.outcome != O::authority_unavailable || unconfigured.observation ||
      !unconfigured.source_diagnostic || native.ok() ||
      native.diagnostic.diagnostic_code != "SB-MEMORY-ALLOC-DEFAULT-MANAGER-UNCONFIGURED") return 3;
  const auto configured = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(),
      "runtime_principal_cold_read");
  if (!configured.ok() || !configured.fixture_mode) return 4;
  const auto read = api::InspectRuntimePrincipal(request);
  if (expect_active == 0) return read.outcome == O::no_active_principal &&
      !read.observation && !read.source_diagnostic ? 0 : 5;
  if (read.outcome != O::observed || !read.observation ||
      read.observation->principal_generation != principal_generation ||
      read.observation->security_context_generation != context_generation) {
    std::cerr << "cold principal outcome=" << static_cast<int>(read.outcome)
              << " expected generations=" << principal_generation << '/' << context_generation;
    if (read.observation) std::cerr << " actual generations=" << read.observation->principal_generation
        << '/' << read.observation->security_context_generation;
    if (read.source_diagnostic) std::cerr << " source=" << read.source_diagnostic->code
        << ':' << read.source_diagnostic->detail;
    const auto native = db::ReadDatabaseBootstrapSecurityCatalog(path);
    if (!native.ok()) std::cerr << " native=" << native.diagnostic.diagnostic_code
        << ':' << native.diagnostic.message_key;
    std::cerr << '\n';
  }
  return read.outcome == O::observed && read.observation &&
      read.observation->principal_uuid == request.principal_uuid &&
      read.observation->database_uuid == request.database_uuid &&
      read.observation->principal_kind == "service" &&
      read.observation->lifecycle_state == "active" && !read.observation->deleted &&
      read.observation->principal_generation == principal_generation &&
      read.observation->security_context_generation == context_generation ? 0 : 1;
}

inline void ColdRead(const api::RuntimePrincipalObservationRequest& request,
                     const api::RuntimePrincipalObservation& expected,
                     bool active = true) {
  int fds[2];
  Check(pipe(fds) == 0, "principal probe pipe");
  posix_spawn_file_actions_t actions;
  Check(posix_spawn_file_actions_init(&actions) == 0, "principal spawn actions");
  Check(posix_spawn_file_actions_adddup2(&actions, fds[0], STDIN_FILENO) == 0, "principal spawn input");
  Check(posix_spawn_file_actions_addclose(&actions, fds[1]) == 0, "principal spawn writer close");
  char* args[]{const_cast<char*>(executable), const_cast<char*>("--cold-principal"),
      const_cast<char*>(request.database_path.c_str()), nullptr};
  pid_t pid = 0;
  const int spawned = posix_spawn(&pid, executable, &actions, nullptr, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[0]);
  if (spawned != 0) { close(fds[1]); Check(false, "principal fresh exec"); }
  const auto send = [&](const auto& value) {
    Check(write(fds[1], &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value)), "principal binary input");
  };
  send(request.database_uuid.bytes); send(request.principal_uuid.bytes);
  send(expected.principal_generation); send(expected.security_context_generation);
  send(static_cast<std::uint8_t>(active));
  close(fds[1]);
  int status = 0;
  Check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "fresh-process durable principal observation");
}

// Qualify the shared projection against actual MGA-published grants. These
// checks do not authenticate a runtime or issue startup admission.
inline void ProjectionChecks(const api::EngineRequestContext& admin,
                             const api::RuntimePrincipalObservationRequest& request) {
  const auto project = [&] {
    api::EngineRequestContext source;
    source.database_path = request.database_path;
    source.database_uuid = request.database_uuid;
    const auto loaded = api::LoadSecurityPrincipalLifecycleState(source);
    Check(loaded.ok, "projection durable source");
    const api::DurableAuthorizationProjectionBinding binding{
        request.database_uuid, request.principal_uuid, loaded.state.security_generation,
        loaded.state.policy_generation, admin.catalog_generation_id};
    auto result = api::ProjectDurableAuthorizationState(loaded.state, binding);
    Check(result.authority_uuid == request.database_uuid &&
          result.security_context_generation == loaded.state.security_context_generation &&
          result.security_epoch == binding.security_epoch && result.policy_epoch == binding.policy_epoch &&
          result.catalog_generation_id == binding.catalog_generation_id &&
          result.engine_owned_sysarch_role_uuid.is_nil(), "projection preserves source and explicit bindings");
    const auto missing = api::ProjectDurableAuthorizationState(loaded.state, {});
    Check(missing.authority_uuid.is_nil() && missing.security_epoch == 0 &&
          missing.policy_epoch == 0 && missing.catalog_generation_id == 0,
          "projection never invents authority or epochs");
    api::DurableAuthorizationMaterializeRequest materialize;
    materialize.principal_uuid = request.principal_uuid;
    Check(!api::MaterializeDurableAuthorizationContext(missing, materialize).ok,
          "owning materializer refuses unbound projection");
    Check(api::MaterializeDurableAuthorizationContext(result, materialize).ok,
          "owning materializer consumes real active principal projection");
    return result;
  };
  const auto selected = [&](const api::DurableAuthorizationState& state) {
    std::vector<api::DurableAuthorizationGrantRecord> grants;
    for (const auto& grant : state.grants)
      if (grant.subject_uuid == request.principal_uuid && grant.right == "OBS_AGENT_STATE_READ")
        grants.push_back(grant);
    return grants;
  };
  Check(selected(project()).empty(), "projection supplies no implicit service grant");
  api::EngineSecurityGrantPrivilegeRequest grant;
  grant.context = Begin(admin);
  grant.grant_uuid = NewId(platform::UuidKind::object).value;
  grant.grantee_uuid = request.principal_uuid; grant.grantee_kind = "service";
  grant.target_object_uuid = request.database_uuid; grant.target_object_kind = "database";
  grant.privilege = "OBS_AGENT_STATE_READ";
  const auto granted = api::EngineSecurityGrantPrivilege(grant);
  Check(granted.ok && granted.privilege_granted, "actual service inspection grant");
  Check(selected(project()).empty(), "uncommitted grant excluded from projection");
  Finish(grant.context, true);
  auto grants = selected(project());
  Check(grants.size() == 1 && grants[0].grant_uuid == grant.grant_uuid &&
        grants[0].subject_kind == "principal" && grants[0].target_uuid == request.database_uuid &&
        grants[0].active && !grants[0].deny, "committed binary grant projected exactly");
  grant.context = Begin(admin); grant.grant_effect = "deny";
  Check(api::EngineSecurityGrantPrivilege(grant).ok, "actual service denial staged");
  grants = selected(project());
  Check(grants.size() == 1 && !grants[0].deny, "uncommitted denial excluded");
  Finish(grant.context, false);
  grants = selected(project());
  Check(grants.size() == 1 && !grants[0].deny, "rolled back denial excluded");
  grant.context = Begin(admin);
  Check(api::EngineSecurityGrantPrivilege(grant).ok, "actual service denial restaged");
  Finish(grant.context, true);
  grants = selected(project());
  Check(grants.size() == 1 && grants[0].deny && grants[0].grant_uuid == grant.grant_uuid,
        "committed denial projected");
  api::EngineSecurityRevokePrivilegeRequest revoke;
  revoke.context = Begin(admin); revoke.grantee_uuid = request.principal_uuid;
  revoke.target_object_uuid = request.database_uuid; revoke.privilege = grant.privilege;
  Check(api::EngineSecurityRevokePrivilege(revoke).ok, "actual grant revoke");
  Finish(revoke.context, true);
  Check(selected(project()).empty(), "committed revoke removes projected grant");
}

inline void Run(const std::filesystem::path& root, std::uint32_t page_size) {
  const auto path = root / ("principal-" + std::to_string(page_size) + ".sbdb");
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = NewId(platform::UuidKind::database);
  create.filespace_uuid = NewId(platform::UuidKind::filespace);
  create.page_size = page_size;
  create.creation_unix_epoch_millis = 1;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.require_bootstrap_principal = true;
  create.bootstrap_principal_name = "fixture_admin";
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:salt=0123456789abcdef0123456789abcdef:"
      "verifier=4ce03aa5a5657aaf221192635ed9c63acdb76d78a0994ec6e6ab55286e29e6a5";
  const auto created = db::CreateDatabaseFile(create);
  Check(created.ok(), "principal fixture database creation");
  const auto bytes = [&] { std::ifstream input(path, std::ios::binary);
    Check(input.good(), "principal file byte oracle");
    return std::vector<char>(std::istreambuf_iterator<char>(input), {}); };
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(path.string());
  Check(bootstrap.ok() && bootstrap.state.present, "principal fixture native bootstrap");
  api::EngineRequestContext admin;
  admin.database_path = path.string(); admin.database_uuid = create.database_uuid.value;
  admin.principal_uuid = bootstrap.state.principal_uuid.value;
  admin.trust_mode = api::EngineTrustMode::embedded_in_process;
  admin.security_context_present = true; admin.catalog_generation_id = 1;

  api::RuntimePrincipalObservationRequest request{path.string(), create.database_uuid.value,
      NewId(platform::UuidKind::principal).value};
  Check(api::InspectRuntimePrincipal(request).outcome == O::no_active_principal, "absent is not a service grant");
  auto transaction = Begin(admin);
  api::EngineSecurityCreatePrincipalRequest provision;
  provision.context = transaction; provision.principal_uuid = request.principal_uuid;
  provision.principal_name = "runtime_service"; provision.principal_kind = "service";
  const auto provisioned = api::EngineSecurityCreatePrincipal(provision);
  if (!provisioned.ok) for (const auto& d : provisioned.diagnostics) std::cerr << d.code << ':' << d.detail << '\n';
  Check(provisioned.ok && provisioned.principal_created, "actual service principal create");
  Check(api::InspectRuntimePrincipal(request).outcome == O::no_active_principal, "uncommitted principal hidden");
  Finish(transaction, true);
  const auto committed_bytes = bytes();
  const auto observed = api::InspectRuntimePrincipal(request);
  Check(observed.outcome == O::observed && observed.observation && !observed.source_diagnostic,
        "committed principal observed");
  const auto original = *observed.observation;
  Check(original.principal_kind == "service" && original.lifecycle_state == "active" && !original.deleted &&
      original.principal_uuid == request.principal_uuid && original.database_uuid == request.database_uuid &&
      original.principal_generation == provisioned.security_generation &&
      original.security_context_generation > bootstrap.state.security_context_generation,
      "exact actual principal and generation facts");
  ColdRead(request, original);
  Check(bytes() == committed_bytes, "active warm and cold observation writes no bytes");
  ProjectionChecks(admin, request);
  auto invalid = request;
  invalid.database_uuid = NewId(platform::UuidKind::database).value;
  auto refused = api::InspectRuntimePrincipal(invalid);
  Check(refused.outcome == O::authority_unavailable && !refused.observation && refused.source_diagnostic,
        "wrong database cannot supply principal facts");
  for (int which = 0; which < 6; ++which) {
    invalid = request;
    if (which == 0) invalid.principal_uuid = {};
    if (which == 1) invalid.principal_uuid.bytes[6] &= 0x0f;
    if (which == 2) invalid.database_path.push_back('\0');
    if (which == 3) invalid.principal_uuid.bytes[8] &= 0x3f;
    if (which == 4) invalid.database_uuid = {};
    if (which == 5) invalid.database_path.clear();
    refused = api::InspectRuntimePrincipal(invalid);
    Check(refused.outcome == O::invalid_request && !refused.observation, "invalid identity/path refused");
  }
  transaction = Begin(admin);
  api::EngineSecurityAlterPrincipalRequest alter;
  alter.context = transaction; alter.principal_uuid = request.principal_uuid;
  alter.lifecycle_state = "disabled";
  Check(api::EngineSecurityAlterPrincipal(alter).ok, "actual service disable");
  const auto uncommitted = api::InspectRuntimePrincipal(request);
  Check(uncommitted.observation && uncommitted.observation->lifecycle_state == "active", "uncommitted disable hidden");
  Finish(transaction, false);
  const auto rolled_back = api::InspectRuntimePrincipal(request);
  Check(rolled_back.observation && rolled_back.observation->lifecycle_state == "active", "rollback preserves active state");
  transaction = Begin(admin); alter.context = transaction;
  Check(api::EngineSecurityAlterPrincipal(alter).ok, "actual repeated disable");
  Finish(transaction, true);
  const auto disabled = api::InspectRuntimePrincipal(request);
  Check(disabled.outcome == O::no_active_principal && !disabled.observation && !disabled.source_diagnostic,
        "committed disable removes active principal without cached authority");
  ColdRead(request, original, false);
  const auto before = bytes();
  Check(api::InspectRuntimePrincipal(request).outcome == O::no_active_principal, "repeat disabled read");
  Check(bytes() == before, "principal observation writes no bytes");
  const auto held = path.string() + ".held";
  std::filesystem::rename(path, held);
  refused = api::InspectRuntimePrincipal(request);
  Check(refused.outcome == O::authority_unavailable && !refused.observation && refused.source_diagnostic,
        "warm cache cannot replace absent durable state");
  std::filesystem::rename(held, path);
}
} // namespace runtime_principal_checks
