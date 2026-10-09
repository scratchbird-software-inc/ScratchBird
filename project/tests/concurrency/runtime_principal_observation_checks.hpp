// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "security/runtime_principal_observation.hpp"
#include "security/durable_authorization_projection.hpp"
#include "security/authentication_api.hpp"
#include "security/runtime_principal_credential.hpp"
#include "physical_mga_cow_store.hpp"
#include "memory.hpp"
#include "../support/durable_authorization_fixture.hpp"
#include <future>
#include <thread>
#include <type_traits>

// Included after the inventory fixture's Check/NewId helpers. These checks
// exercise real principal publication, observation and authentication identity,
// not a startup admission/recovery receipt or a selected-provider fence.
namespace runtime_principal_checks {
using O = api::RuntimePrincipalObservationOutcome;
using CredentialError = api::RuntimePrincipalCredentialError;
static_assert(!std::is_default_constructible_v<api::RuntimePrincipalCredentialLease>);
static_assert(!std::is_copy_constructible_v<api::RuntimePrincipalCredentialLease>);
static_assert(!std::is_move_constructible_v<api::RuntimePrincipalCredentialLease>);

inline api::RuntimePrincipalCredentialRequest CredentialRequest(
    const api::RuntimePrincipalObservationRequest& source) {
  return {source.database_path, source.database_uuid, source.principal_uuid,
          "DBLC013G-fixture-password"};
}

inline api::EngineAuthenticateRequest AuthenticationRequest(
    const api::RuntimePrincipalObservationRequest& source) {
  api::EngineAuthenticateRequest request;
  request.context.database_path = source.database_path;
  request.context.database_uuid = source.database_uuid;
  request.provider_family = "local_password";
  request.principal_claim = "runtime_service";
  request.credential_evidence = "DBLC013G-fixture-password";
  request.durable_principal_uuid = source.principal_uuid;
  return request;
}

inline api::EngineRequestContext Begin(api::EngineRequestContext context,
                                      std::uint64_t begin_millis = 100) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(context.database_path);
  Check(loaded.ok(), "principal fixture inventory load");
  const auto begun = mga::BeginLocalTransaction(loaded.inventory,
      NewId(platform::UuidKind::transaction), begin_millis);
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
  const auto authentication = api::EngineAuthenticate(AuthenticationRequest(request));
  const auto credential = api::VerifyRuntimePrincipalCredential(CredentialRequest(request));
  if (expect_active != 0) {
    if (!authentication.ok || !authentication.authenticated ||
        !authentication.durable_security_state ||
        authentication.connection_security_context.effective_user_uuid != request.principal_uuid)
      return 6;
    if (!credential.ok() || credential.lease->principal_uuid() != request.principal_uuid ||
        credential.lease->security_context_generation() != context_generation) return 8;
  } else if (authentication.ok || authentication.authenticated || authentication.durable_security_state) {
    return 7;
  } else if (credential.ok() || credential.lease || credential.error != CredentialError::credential_rejected) {
    return 9;
  }
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

inline void AuthenticationIdentityChecks(const api::RuntimePrincipalObservationRequest& source) {
  auto request = AuthenticationRequest(source);
  const auto accepted = api::EngineAuthenticate(request);
  Check(accepted.ok && accepted.authenticated && accepted.durable_security_state &&
        accepted.connection_security_context.effective_user_uuid == source.principal_uuid,
        "real service password authenticates exact binary principal");
  request.durable_principal_uuid = NewId(platform::UuidKind::principal).value;
  const auto mismatched = api::EngineAuthenticate(request);
  Check(!mismatched.ok && !mismatched.authenticated && !mismatched.durable_security_state &&
        mismatched.diagnostics.size() == 1 &&
        mismatched.diagnostics.front().detail == "durable_principal_identity_mismatch",
        "raw password must not overwrite a different binary principal candidate");
  request.durable_principal_uuid = {};
  const auto by_name = api::EngineAuthenticate(request);
  Check(by_name.ok && by_name.authenticated &&
        by_name.connection_security_context.effective_user_uuid == source.principal_uuid,
        "absent candidate still resolves the actual durable identity");
  for (int candidate = 0; candidate < 3; ++candidate) {
    for (const auto* option : {"durable_principal_uuid:text", "durable_principal_uuid:"}) {
      request = AuthenticationRequest(source);
      if (candidate == 1) request.durable_principal_uuid = {};
      if (candidate == 2) request.durable_principal_uuid = NewId(platform::UuidKind::principal).value;
      request.option_envelopes.emplace_back(option);
      const auto refused = api::EngineAuthenticate(request);
      Check(!refused.ok && !refused.authenticated && !refused.durable_security_state &&
            refused.diagnostics.size() == 1 &&
            refused.diagnostics.front().detail == "binary_principal_identity_required",
            "text or empty identity option cannot become name-only authentication");
    }
  }
  request = AuthenticationRequest(source);
  request.credential_evidence = "scheme=local_password_v1;principal=runtime_service;principal_uuid=text";
  const auto text_evidence = api::EngineAuthenticate(request);
  Check(!text_evidence.ok && !text_evidence.authenticated &&
        text_evidence.diagnostics.size() == 1 &&
        text_evidence.diagnostics.front().detail == "binary_principal_identity_required",
        "text credential identity is rejected rather than treated as absent");
  for (int invalid = 0; invalid < 2; ++invalid) {
    request = AuthenticationRequest(source);
    if (invalid == 0) request.durable_principal_uuid.bytes[6] &= 0x0f;
    else request.durable_principal_uuid.bytes[8] &= 0x3f;
    const auto refused = api::EngineAuthenticate(request);
    Check(!refused.ok && !refused.authenticated && !refused.durable_security_state &&
          refused.diagnostics.size() == 1 &&
          refused.diagnostics.front().detail == "binary_principal_identity_required",
          "malformed binary identity never falls back to name resolution");
  }
  for (bool with_candidate : {false, true}) {
    request = AuthenticationRequest(source);
    if (!with_candidate) request.durable_principal_uuid = {};
    request.credential_evidence = "incorrect-fixture-password";
    const auto refused = api::EngineAuthenticate(request);
    Check(!refused.ok && !refused.authenticated && !refused.durable_security_state,
          "identity validation cannot replace actual credential verification");
  }
}

inline void CredentialFenceChecks(const api::EngineRequestContext& admin,
                                  const api::RuntimePrincipalObservationRequest& source) {
  const auto request = CredentialRequest(source);
  const auto refuse = [&](const auto& input, CredentialError expected) {
    auto result = api::VerifyRuntimePrincipalCredential(input);
    Check(!result.ok() && !result.lease && result.error == expected,
          "credential refusal has exact disposition and no retained fence");
  };
  for (int invalid = 0; invalid < 7; ++invalid) {
    auto input = request;
    if (invalid == 0) input.database_path.clear();
    if (invalid == 1) input.database_path.push_back('\0');
    if (invalid == 2) input.database_uuid = {};
    if (invalid == 3) input.principal_uuid.bytes[6] &= 0x0f;
    if (invalid == 4) input.password = {};
    const std::string oversized(1025, 'x');
    if (invalid == 5) input.password = oversized;
    if (invalid == 6) input.password = std::string_view("x\0y", 3);
#ifdef SB_STARTUP_INVENTORY_GUARD_PROBE
    const auto guards_before = inventory_guard_calls.load();
#endif
    refuse(input, CredentialError::invalid_request);
#ifdef SB_STARTUP_INVENTORY_GUARD_PROBE
    Check(inventory_guard_calls.load() == guards_before, "invalid credential input acquires no inventory guard");
#endif
  }
  auto wrong = request;
  wrong.password = "wrong-password";
  refuse(wrong, CredentialError::credential_rejected);
  wrong = request; wrong.principal_uuid = admin.principal_uuid;
  refuse(wrong, CredentialError::credential_rejected); // no bootstrap fallback
  wrong = request; wrong.principal_uuid = NewId(platform::UuidKind::principal).value;
  refuse(wrong, CredentialError::credential_rejected);
  wrong = request; wrong.database_uuid = NewId(platform::UuidKind::database).value;
  refuse(wrong, CredentialError::source_failure);
#ifdef SB_RUNTIME_CREDENTIAL_FAULT_PROBE
  auto fault_lookup = api::AcquireTransactionInventoryGuard(source.database_path);
  auto* fault_mutex = fault_lookup.mutex(); fault_lookup.unlock();
  fail_credential_mutex = fault_mutex->native_handle();
  refuse(request, CredentialError::lock_failure);
  Check(!fail_credential_mutex, "exact owning publication lock fault consumed");
  fail_after_password_verification = true;
  refuse(request, CredentialError::resource_exhausted);
  Check(!fail_after_password_verification && !fail_credential_allocation,
        "actual post-verification lease allocation fault consumed");
  bool fault_unlocked = false;
  std::thread fault_probe([&] {
    fault_unlocked = fault_mutex->try_lock();
    if (fault_unlocked) fault_mutex->unlock();
  });
  fault_probe.join();
  Check(fault_unlocked, "failed credential issuance releases the owning mutex");
#endif

  // Stage a real disable before the lease: it must still authenticate against
  // the current committed state, then exclude the actual engine commit path.
  api::EngineSecurityAlterPrincipalRequest alter;
  const auto clock = scratchbird::core::time::ReadLocalNodeClockSnapshot();
  Check(clock.ok(), "credential fixture actual clock");
  const auto millis = scratchbird::core::time::WallClockToUuidV7Millis(clock.value.wall_clock);
  Check(millis.ok(), "credential fixture actual begin milliseconds");
  alter.context = Begin(admin, millis.unix_epoch_millis); alter.principal_uuid = source.principal_uuid;
  const auto session = uuid::IssueRuntimeIdentityV7();
  Check(session.has_value(), "credential fixture binary runtime session identity");
  alter.context.session_uuid = *session;
  alter.lifecycle_state = "disabled";
  Check(api::EngineSecurityAlterPrincipal(alter).ok, "credential fixture staged disable");
  const auto observation = api::InspectRuntimePrincipal(source);
  Check(observation.observation.has_value(), "committed service remains active before fence");
  auto admitted = api::VerifyRuntimePrincipalCredential(request);
  Check(admitted.ok() && admitted.lease->database_uuid() == source.database_uuid &&
        admitted.lease->principal_uuid() == source.principal_uuid &&
        admitted.lease->security_context_generation() == observation.observation->security_context_generation &&
        admitted.lease->security_generation() == observation.observation->security_generation &&
        admitted.lease->policy_generation() == observation.observation->policy_generation,
        "credential lease binds actual committed binary principal and all source generations");
  auto lookup = api::AcquireTransactionInventoryGuard(source.database_path);
  auto* publication_mutex = lookup.mutex(); lookup.unlock();
  std::promise<bool> probed;
  auto observed_lock = probed.get_future();
  api::EngineCommitTransactionResult committed;
  std::exception_ptr worker_error;
  std::thread worker([&] {
    const bool available = publication_mutex->try_lock();
    if (available) publication_mutex->unlock();
    probed.set_value(available);
    try {
      api::EngineCommitTransactionRequest commit;
      commit.context = alter.context;
      committed = api::EngineCommitTransaction(commit);
    } catch (...) { worker_error = std::current_exception(); }
  });
  // No assertion/throw while the worker could be blocked on our retained guard.
  const bool blocked = !observed_lock.get();
  admitted.lease.reset();
  worker.join();
  if (worker_error) std::rethrow_exception(worker_error);
  Check(blocked, "credential lease retains the real security/finality publication mutex");
  if (!committed.ok) for (const auto& diagnostic : committed.diagnostics)
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  Check(committed.ok && committed.engine_finality_known &&
        committed.commit_finality_state == "committed_by_engine_inventory",
        "actual engine commit of disable completes after credential fence release");
  refuse(request, CredentialError::credential_rejected);
  ColdRead(source, *observation.observation, false);
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
  request.principal_uuid.bytes[9] = 0;
  request.principal_uuid.bytes[10] = '\n';
  request.principal_uuid.bytes[11] = 255;
  Check(api::InspectRuntimePrincipal(request).outcome == O::no_active_principal, "absent is not a service grant");
  auto transaction = Begin(admin);
  api::EngineSecurityCreatePrincipalRequest provision;
  provision.context = transaction; provision.principal_uuid = request.principal_uuid;
  provision.principal_name = "runtime_service"; provision.principal_kind = "service";
  provision.credential_fingerprint = create.bootstrap_credential_fingerprint;
  const auto provisioned = api::EngineSecurityCreatePrincipal(provision);
  if (!provisioned.ok) for (const auto& d : provisioned.diagnostics) std::cerr << d.code << ':' << d.detail << '\n';
  Check(provisioned.ok && provisioned.principal_created, "actual service principal create");
  Check(api::InspectRuntimePrincipal(request).outcome == O::no_active_principal, "uncommitted principal hidden");
  Check(api::VerifyRuntimePrincipalCredential(CredentialRequest(request)).error ==
        CredentialError::credential_rejected, "uncommitted principal cannot issue credential fence");
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
  AuthenticationIdentityChecks(request);
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
  Check(api::EngineAuthenticate(AuthenticationRequest(request)).authenticated,
        "uncommitted disable does not replace committed authentication state");
  Finish(transaction, false);
  const auto rolled_back = api::InspectRuntimePrincipal(request);
  Check(rolled_back.observation && rolled_back.observation->lifecycle_state == "active", "rollback preserves active state");
  Check(api::EngineAuthenticate(AuthenticationRequest(request)).authenticated,
        "rolled back disable preserves actual password authentication");
  // This final disable now goes through actual engine commit while a retained
  // credential lease excludes publication, rather than direct fixture finality.
  CredentialFenceChecks(admin, request);
  const auto disabled = api::InspectRuntimePrincipal(request);
  Check(disabled.outcome == O::no_active_principal && !disabled.observation && !disabled.source_diagnostic,
        "committed disable removes active principal without cached authority");
  const auto lifecycle = api::LoadSecurityPrincipalLifecycleState(admin);
  Check(lifecycle.ok, "disabled lifecycle remains readable");
  bool retained_disabled = false;
  for (const auto& principal : lifecycle.state.principals)
    if (principal.principal_uuid == request.principal_uuid)
      retained_disabled = !principal.deleted && principal.lifecycle_state == "disabled";
  Check(retained_disabled, "disable retains principal identity for lifecycle and unlock");
  const auto disabled_authentication = api::EngineAuthenticate(AuthenticationRequest(request));
  Check(!disabled_authentication.ok && !disabled_authentication.authenticated &&
        !disabled_authentication.durable_security_state,
        "committed disable revokes fresh authentication without cached success");
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
