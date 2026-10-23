#include "wire/binary_status_packet.hpp"
#include "uuid.hpp"
#include <algorithm>
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_ENGINE_HOST_DATABASE_SUPERVISION

#include "engine_host.hpp"
#include "hosted_process_admission.hpp"
#include "native_hosted_database_runtime.hpp"
#include "startup_trace.hpp"

#include "config_policy_security_lifecycle.hpp"
#include "database_ownership.hpp"
#include "database_lifecycle.hpp"
#include "sblr_bulk_import_stream_registry.hpp"
#include "sblr_prepared_coordination_registry.hpp"
#include "sblr_startup_descriptor_recovery.hpp"
#include "scratchbird/engine/engine.h"
#include "startup_credential_binding.hpp"
#include "startup_credential_provisioning.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <sstream>

namespace scratchbird::server {

namespace {

scratchbird::core::platform::Uuid BinaryIdentity(std::string_view bytes) {
  scratchbird::core::platform::Uuid id;
  if (bytes.size() != id.bytes.size()) return {};
  std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()), id.bytes.size(), id.bytes.begin());
  return scratchbird::core::uuid::IsEngineIdentityUuid(id) ? id : scratchbird::core::platform::Uuid{};
}


// Server process lifetime, not a copyable HostedEngineState or a path namespace.
// Reserve before filesystem activity without holding a registry mutex across
// I/O, provider work or governor callbacks. Failed startup can retry; a completed
// open permanently binds this actual process, independently of copied status.
HostedProcessAdmission hosted_process_admission;

std::string JsonEscape(const std::string& value) {
  return EscapeMessageVectorText(value);
}

ServerDiagnostic EngineHostDiagnostic(std::string code,
                                      std::string key,
                                      std::string message,
                                      const std::string& database_path) {
  return ServerDiagnostic{std::move(code),
                          std::move(key),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          {{"database_path", database_path}}};
}

sb_engine_open_mode_t OpenModeForConfig(const ServerBootstrapConfig& config) {
  if (config.mode == ServerMode::kMaintenance || config.database_open_mode == "maintenance") {
    return SB_ENGINE_OPEN_MAINTENANCE;
  }
  if (config.mode == ServerMode::kReadOnly || config.database_open_mode == "read_only") {
    return SB_ENGINE_OPEN_READ_ONLY;
  }
  if (config.database_open_mode == "restricted" || config.database_open_mode == "restricted_open") {
    return SB_ENGINE_OPEN_MAINTENANCE;
  }
  return SB_ENGINE_OPEN_NORMAL;
}

HostedDatabaseState StateForConfig(const ServerBootstrapConfig& config) {
  if (config.database_open_mode == "restricted" || config.database_open_mode == "restricted_open") {
    return HostedDatabaseState::kRestrictedOpen;
  }
  if (config.mode == ServerMode::kMaintenance || config.database_open_mode == "maintenance") {
    return HostedDatabaseState::kMaintenance;
  }
  if (config.mode == ServerMode::kReadOnly || config.database_open_mode == "read_only") {
    return HostedDatabaseState::kReadOnly;
  }
  return HostedDatabaseState::kOpen;
}

}  // namespace

HostedProcessAdmission& ServerHostedProcessAdmission() noexcept {
  return hosted_process_admission;
}

const char* HostedDatabaseStateName(HostedDatabaseState state) {
  switch (state) {
    case HostedDatabaseState::kNotConfigured:
      return "not_configured";
    case HostedDatabaseState::kOpening:
      return "opening";
    case HostedDatabaseState::kOpen:
      return "open";
    case HostedDatabaseState::kReadOnly:
      return "read_only";
    case HostedDatabaseState::kRestrictedOpen:
      return "restricted_open";
    case HostedDatabaseState::kMaintenance:
      return "maintenance";
    case HostedDatabaseState::kFailed:
      return "failed";
    case HostedDatabaseState::kDetached:
      return "detached";
    case HostedDatabaseState::kQuarantined:
      return "quarantined";
  }
  return "failed";
}

std::shared_ptr<const HostedDatabaseRuntime> FindHostedDatabaseRuntime(
    const HostedEngineState& state,
    const scratchbird::core::platform::Uuid& database_uuid,
    std::string_view canonical_database_path) {
  for (const auto& runtime : state.database_runtimes) {
    if (!runtime) {
      continue;
    }
    if (!database_uuid.is_nil() && runtime->database_uuid != database_uuid) {
      continue;
    }
    if (!canonical_database_path.empty() &&
        runtime->canonical_database_path != canonical_database_path) {
      continue;
    }
    if (!database_uuid.is_nil() || !canonical_database_path.empty()) {
      return runtime;
    }
  }
  return {};
}

HostedEngineResult StartHostedEngine(const ServerBootstrapConfig& config,
                                    void (*before_failure_cleanup)() noexcept,
                                    ServerProcessCryptoOwner* crypto) {
  // A forked child must exec a fresh host. Refuse before constructing ordinary
  // diagnostics, which could enter an inherited crypto provider or its mutex.
  if (!hosted_process_admission.InOwningProcess()) std::terminate();
  HostedEngineResult result;
  result.state.engine_context_active = true;

  HostedDatabaseSnapshot snapshot;
  snapshot.state = HostedDatabaseState::kOpening;
  snapshot.database_path = config.database_default_path.string();
  snapshot.resource_seed_pack_root =
      config.database_resource_seed_pack_root.string();
  snapshot.policy_seed_pack_root =
      config.database_policy_seed_pack_root.string();
  snapshot.lifecycle_mode = config.database_open_mode;

  if (config.database_daemon_scope != "dedicated") {
    result.state.engine_context_active = false;
    snapshot.state = HostedDatabaseState::kFailed;
    snapshot.diagnostic_code = "SERVER.DAEMON.SCOPE_INVALID";
    snapshot.diagnostic_message_key = "server.daemon.scope_invalid";
    result.diagnostics.push_back(EngineHostDiagnostic(
        snapshot.diagnostic_code, snapshot.diagnostic_message_key,
        "A server process requires dedicated single-database ownership.",
        snapshot.database_path));
    result.state.databases.push_back(snapshot);
    return result;
  }

  if (snapshot.database_path.empty()) {
    snapshot.state = HostedDatabaseState::kNotConfigured;
    result.state.databases.push_back(snapshot);
    return result;
  }

  auto process_admission = hosted_process_admission.TryReserve();
  if (!process_admission.ok()) {
    result.state.engine_context_active = false;
    snapshot.state = HostedDatabaseState::kFailed;
    snapshot.diagnostic_code = "SERVER.STARTUP.DATABASE_OPEN_FAILED";
    snapshot.diagnostic_message_key = "server.startup.database_open_failed";
    result.diagnostics.push_back(EngineHostDiagnostic(
        snapshot.diagnostic_code, snapshot.diagnostic_message_key,
        "This server process is already opening or bound to a database; use a separate server process.",
        snapshot.database_path));
    result.state.databases.push_back(snapshot);
    return result;
  }

  std::shared_ptr<DatabaseOwnershipLock> database_ownership_lock;
  bool startup_completed = false;
  struct FailureCleanupArm {
    const bool& completed;
    void (*callback)() noexcept;
    int exceptions = std::uncaught_exceptions();
    ~FailureCleanupArm() {
      if (callback && (std::uncaught_exceptions() > exceptions || !completed)) callback();
    }
  } failure_cleanup_arm{startup_completed, before_failure_cleanup};
  if (!config.database_ownership_prelocked) {
    DatabaseOwnershipRequest ownership_request;
    ownership_request.database_path = config.database_default_path;
    ownership_request.owner_kind = config.database_ownership_owner_kind.empty()
                                       ? "server"
                                       : config.database_ownership_owner_kind;
    ownership_request.sbps_endpoint = config.sbps_endpoint;
    // Select the initial physical capability explicitly. A later native
    // transfer cannot upgrade the read-only default to writable custody.
    ownership_request.storage_access = OpenModeForConfig(config) == SB_ENGINE_OPEN_NORMAL
        ? storage::disk::RouteStorageAccess::read_write
        : storage::disk::RouteStorageAccess::read_only;
    auto ownership = AcquireDatabaseOwnership(ownership_request);
    if (!ownership.acquired) {
      snapshot.state = HostedDatabaseState::kFailed;
      snapshot.diagnostic_code =
          ownership.diagnostic_code.empty() ? "ARCH.DATABASE_MULTI_OWNER" : ownership.diagnostic_code;
      snapshot.diagnostic_message_key = "arch.database_multi_owner";
      result.diagnostics.push_back(ServerDiagnostic{
          snapshot.diagnostic_code,
          snapshot.diagnostic_code,
          ServerDiagnosticSeverity::kError,
          "The configured database is already owned by another engine instance.",
          {{"database_path", snapshot.database_path},
           {"lock_path", ownership.lock_path.string()},
           {"owner_kind", ownership.incumbent.owner_kind},
           {"owner_pid", ownership.incumbent.pid},
           {"owner_sbps_endpoint", ownership.incumbent.sbps_endpoint},
           {"detail", ownership.diagnostic_detail}}});
      result.state.databases.push_back(snapshot);
      return result;
    }
    database_ownership_lock = std::move(ownership.lock);
  }

  bool exists = std::filesystem::exists(config.database_default_path);
  if (!exists && !config.database_auto_create) {
    snapshot.state = HostedDatabaseState::kFailed;
    snapshot.diagnostic_code = "SERVER.ENGINE_HOST.DATABASE_NOT_FOUND";
    snapshot.diagnostic_message_key = "server.engine_host.database_not_found";
    result.diagnostics.push_back(EngineHostDiagnostic(snapshot.diagnostic_code,
                                                      snapshot.diagnostic_message_key,
                                                      "The configured database does not exist and auto-create is disabled.",
                                                      snapshot.database_path));
    result.state.databases.push_back(snapshot);
    return result;
  }

  if (!exists && config.database_auto_create) {
    snapshot.state = HostedDatabaseState::kFailed;
    snapshot.diagnostic_code = "BOOTSTRAP.SERVICE_START_FORBIDDEN";
    snapshot.diagnostic_message_key = "bootstrap.service_start_forbidden";
    result.diagnostics.push_back(EngineHostDiagnostic(
        snapshot.diagnostic_code,
        snapshot.diagnostic_message_key,
        "Public server startup cannot create a missing database; run the approved embedded bootstrap first.",
        snapshot.database_path));
    result.state.databases.push_back(snapshot);
    return result;
  }

  // Retained for startup credential framing below. Only the committed read
  // populates it, so a database that skipped the check never supplies a
  // binding and cannot be credentialed by accident.
  std::optional<scratchbird::storage::database::DatabaseBootstrapSecurityCatalogState>
      committed_bootstrap_security;
  if (exists && config.security_provider_family == "local_password" &&
      !config.allow_uncredentialed_fixture_database) {
    const auto bootstrap_security =
        scratchbird::storage::database::ReadDatabaseBootstrapSecurityCatalog(
            snapshot.database_path);
    constexpr scratchbird::core::platform::Uuid canonical_sysarch_role{{
        0x01,0x8f,0x7a,0x10,0x12,0x80,0x70,0x00,0x80,0,0,0,0,0,0x01,0x05}};
    const auto sysarch_role_uuid = bootstrap_security.state.sysarch_role_uuid.value;
    if (!bootstrap_security.ok() || !bootstrap_security.state.present ||
        !bootstrap_security.state.committed_by_inventory ||
        bootstrap_security.state.credential_fingerprint.empty() ||
        sysarch_role_uuid != canonical_sysarch_role) {
      snapshot.state = HostedDatabaseState::kFailed;
      snapshot.diagnostic_code = "BOOTSTRAP.SECURITY_DATABASE_UNAVAILABLE";
      snapshot.diagnostic_message_key =
          "bootstrap.security_database_unavailable";
      result.diagnostics.push_back(ServerDiagnostic{
          snapshot.diagnostic_code,
          snapshot.diagnostic_message_key,
          ServerDiagnosticSeverity::kError,
          snapshot.diagnostic_code,
          {}});
      result.state.databases.push_back(snapshot);
      return result;
    }
    // SEARCH_KEY: DATABASE_COMMITTED_STARTUP_AUTHORITY_MODE
    // The database committed its own startup authority source at creation,
    // under the bootstrap transaction. Honour it rather than letting a server
    // configuration select an authority the database never agreed to: a
    // configuration that disagrees is a misconfigured deployment, not an
    // override. An unset mode means a committed binding that carried none, or
    // no binding at all, which is tolerated -- see the refusal condition.
    {
      const auto committed_mode = bootstrap_security.state.authority_mode;
      const char* committed_name =
          !committed_mode ? ""
          : *committed_mode == scratchbird::core::catalog::RuntimeAuthorityMode::database_local
              ? "database_local"
          : *committed_mode == scratchbird::core::catalog::RuntimeAuthorityMode::security_database
              ? "security_database"
          : *committed_mode == scratchbird::core::catalog::RuntimeAuthorityMode::cluster
              ? "cluster" : "";
      // Absence is not a conflict. An internal service principal is optional
      // at create (`require_internal_service_principal` defaults false), so a
      // credentialed database can legitimately carry no authority binding;
      // refusing those would reject databases this server opens today. Only a
      // committed mode that contradicts the configuration is a refusal.
      if (committed_name[0] != '\0' &&
          config.security_authority_mode != committed_name) {
        snapshot.state = HostedDatabaseState::kFailed;
        snapshot.diagnostic_code = "BOOTSTRAP.SECURITY_AUTHORITY_MODE_CONFLICT";
        snapshot.diagnostic_message_key =
            "bootstrap.security_authority_mode_conflict";
        result.diagnostics.push_back(ServerDiagnostic{
            snapshot.diagnostic_code,
            snapshot.diagnostic_message_key,
            ServerDiagnosticSeverity::kError,
            snapshot.diagnostic_code,
            {{"configured_authority_mode", config.security_authority_mode},
             {"committed_authority_mode", committed_name}}});
        result.state.databases.push_back(snapshot);
        return result;
      }
      // Empty when the committed binding carried no mode, which keeps the
      // snapshot honest about what the database actually committed.
      snapshot.security_authority_mode = committed_name;
    }
    committed_bootstrap_security = bootstrap_security.state;
  }

  namespace db = scratchbird::storage::database;
  db::DatabaseOpenConfig lifecycle_open_config;
  lifecycle_open_config.path = snapshot.database_path;
  lifecycle_open_config.cluster_authority_available = false;
  lifecycle_open_config.decryption_available = false;
  lifecycle_open_config.read_only = OpenModeForConfig(config) != SB_ENGINE_OPEN_NORMAL;
  lifecycle_open_config.suppress_background_agents = config.embedded_direct_mode;
  const auto lifecycle_open = db::OpenDatabaseFile(lifecycle_open_config);
  if (!lifecycle_open.ok()) {
    snapshot.state = HostedDatabaseState::kFailed;
    snapshot.diagnostic_code = lifecycle_open.diagnostic.diagnostic_code.empty()
        ? "SERVER.ENGINE_HOST.DATABASE_LIFECYCLE_OPEN_FAILED"
        : lifecycle_open.diagnostic.diagnostic_code;
    snapshot.diagnostic_message_key = lifecycle_open.diagnostic.message_key.empty()
        ? "server.engine_host.database_lifecycle_open_failed"
        : lifecycle_open.diagnostic.message_key;
    result.diagnostics.push_back(EngineHostDiagnostic(
        snapshot.diagnostic_code,
        snapshot.diagnostic_message_key,
        "The configured database could not enter the storage lifecycle open path.",
        snapshot.database_path));
    result.state.databases.push_back(snapshot);
    return result;
  }

  // The host never uses a temporary public ABI handle for sessions/execution.
  // Perform its actual recovery work directly, sharing the ABI's exact ordered
  // operation, rather than allocate an unused governor and discard a handle.
  if (OpenModeForConfig(config) == SB_ENGINE_OPEN_NORMAL) {
    engine::internal_api::EngineRequestContext recovery_context;
    recovery_context.database_path = snapshot.database_path;
    recovery_context.database_uuid = lifecycle_open.state.database_uuid.value;
    recovery_context.security_context_present = true;
    recovery_context.statement_metadata_snapshot_engine_owned = true;
    auto recovered = engine::internal_api::RecoverSblrStartupDescriptorsV1(recovery_context);
    if (!recovered.ok()) {
      result.descriptor_recovery_failure.emplace(std::move(recovered.diagnostic));
      const auto& diagnostic = *result.descriptor_recovery_failure;
      snapshot.state = HostedDatabaseState::kQuarantined;
      snapshot.diagnostic_code = diagnostic.code;
      snapshot.diagnostic_message_key = diagnostic.message_key;
      result.diagnostics.push_back(ServerDiagnostic{
          .code = diagnostic.code,
          .message_key = diagnostic.message_key,
          .severity = ServerDiagnosticSeverity::kError,
          .safe_message = "Hosted startup descriptor recovery did not complete.",
          .fields = {{"database_path", snapshot.database_path}, {"detail", diagnostic.detail}},
          .occurrence_uuid = diagnostic.occurrence_uuid});
      result.state.databases.push_back(snapshot);
      return result;
    }
  }
  snapshot.database_created = false;
  snapshot.database_open = true;
  snapshot.database_uuid = lifecycle_open.state.database_uuid.value;
  snapshot.filespace_uuid = lifecycle_open.state.filespace_uuid.value;
  snapshot.page_size_bytes = lifecycle_open.state.header.page_size;
  snapshot.state = StateForConfig(config);
  snapshot.startup_recovery_classification =
      lifecycle_open.state.startup_recovery_classification.empty()
          ? "hosted_storage_open"
          : lifecycle_open.state.startup_recovery_classification;

  std::error_code database_path_error;
  const auto database_path_status =
      std::filesystem::symlink_status(config.database_default_path,
                                      database_path_error);
  if (database_path_error || std::filesystem::is_symlink(database_path_status)) {
    snapshot.state = HostedDatabaseState::kQuarantined;
    snapshot.database_open = false;
    snapshot.write_admission_fenced = true;
    snapshot.diagnostic_code = "BULK.IMPORT.RECOVERY_CONFLICT";
    snapshot.diagnostic_message_key = "bulk.import.recovery_conflict";
    result.diagnostics.push_back(ServerDiagnostic{
        snapshot.diagnostic_code,
        snapshot.diagnostic_message_key,
        ServerDiagnosticSeverity::kError,
        "The database path cannot own the durable bulk-import stream registry.",
        {{"database_path", snapshot.database_path},
         {"detail", database_path_error ? "database_path_status_failed"
                                         : "database_path_symlink_forbidden"}}});
    result.state.databases.push_back(snapshot);
    return result;
  }
  database_path_error.clear();
  const auto canonical_database_path =
      std::filesystem::canonical(config.database_default_path,
                                 database_path_error);
  if (database_path_error || canonical_database_path.empty()) {
    snapshot.state = HostedDatabaseState::kQuarantined;
    snapshot.database_open = false;
    snapshot.write_admission_fenced = true;
    snapshot.diagnostic_code = "BULK.IMPORT.RECOVERY_CONFLICT";
    snapshot.diagnostic_message_key = "bulk.import.recovery_conflict";
    result.diagnostics.push_back(ServerDiagnostic{
        snapshot.diagnostic_code,
        snapshot.diagnostic_message_key,
        ServerDiagnosticSeverity::kError,
        "The canonical database path for the durable bulk-import stream registry is unavailable.",
        {{"database_path", snapshot.database_path},
         {"detail", "database_path_canonicalization_failed"}}});
    result.state.databases.push_back(snapshot);
    return result;
  }

  // Prepared parameter coordination is deliberately durable so a torn
  // prepare/execute cannot be mistaken for a fresh statement.  Once this
  // process owns and has opened the database, however, every persisted
  // begun/acquired coordination is necessarily orphaned from its former
  // engine session.  Recover that journal before admitting any new session;
  // sealed prepared capabilities remain intact while orphaned coordination
  // identities are durably revoked and the generation high-water mark is
  // retained.
  engine::internal_api::EngineRequestContext prepared_recovery_context;
  prepared_recovery_context.database_path = snapshot.database_path;
  prepared_recovery_context.database_uuid = snapshot.database_uuid;
  prepared_recovery_context.security_context_present = true;
  prepared_recovery_context.trace_tags.push_back(
      "right:SBLR_PREPARED_COORDINATION_ADMIN");
  const auto prepared_recovery =
      engine::internal_api::RecoverSblrPreparedCoordinationRegistry(
          prepared_recovery_context);
  if (prepared_recovery.code != "OK") {
    snapshot.state = HostedDatabaseState::kQuarantined;
    snapshot.database_open = false;
    snapshot.write_admission_fenced = true;
    snapshot.diagnostic_code = prepared_recovery.code.empty()
                                   ? "SBLR.PARAMETER.STALE"
                                   : prepared_recovery.code;
    snapshot.diagnostic_message_key =
        prepared_recovery.message_key.empty()
            ? "sblr.prepared_coordination.startup_recovery_failed"
            : prepared_recovery.message_key;
    result.diagnostics.push_back(ServerDiagnostic{
        snapshot.diagnostic_code,
        snapshot.diagnostic_message_key,
        ServerDiagnosticSeverity::kError,
        "The durable prepared-parameter coordination journal could not be "
        "recovered before session admission.",
        {{"database_path", snapshot.database_path},
         {"detail", prepared_recovery.detail}}});
    result.state.databases.push_back(snapshot);
    return result;
  }

  auto database_runtime = std::make_shared<HostedDatabaseRuntime>();
  database_runtime->canonical_database_path = canonical_database_path.string();
  database_runtime->database_uuid = snapshot.database_uuid;

  // Acquire the server's own startup credential when one is configured.
  //
  // Fail-closed by construction: a configured source that cannot produce a
  // credential quarantines the database instead of continuing. Starting while
  // reporting success is the outcome this whole path exists to prevent, so the
  // refusal is here and not at first use.
  //
  // Embedded operation never reaches a configured source: it leaves the
  // default "none", and in-process authorization is not a boundary there.
  if (config.startup_credential_source != "none" &&
      !config.allow_uncredentialed_fixture_database) {
    const auto fail_credential = [&](const char* code, const char* key) {
      database_runtime.reset();
      snapshot.state = HostedDatabaseState::kQuarantined;
      snapshot.database_open = false;
      snapshot.diagnostic_code = code;
      snapshot.diagnostic_message_key = key;
      result.diagnostics.push_back(ServerDiagnostic{
          snapshot.diagnostic_code, snapshot.diagnostic_message_key,
          ServerDiagnosticSeverity::kError, snapshot.diagnostic_code, {}});
      result.state.databases.push_back(snapshot);
      return result;
    };

    if (!committed_bootstrap_security) {
      return fail_credential("BOOTSTRAP.RUNTIME_AUTHORITY_BINDING_ABSENT",
                             "bootstrap.runtime_authority_binding_absent");
    }
    // Identities come only from what this database committed. Nothing here
    // derives, defaults or synthesizes an identity.
    const auto bound = BuildStartupCredentialBinding(*committed_bootstrap_security,
                                                     snapshot.database_uuid);
    if (!bound.ok()) {
      return fail_credential(StartupCredentialBindingDiagnosticCode(bound.error),
                             "bootstrap.runtime_authority_binding_unusable");
    }

    StartupCredentialProvisionRequest provision;
    provision.binding = bound.binding;
    // Expected CURRENT generation of a freshly constructed source, which is
    // zero. The generation being published is carried by the binding.
    provision.publish = {snapshot.database_uuid, bound.binding.principal,
                         0, {}, {}};
    if (config.startup_credential_source == "service_manager") {
      provision.source = StartupCredentialProvisionSource::service_manager;
      provision.name_or_path = config.startup_credential_name;
    } else {
      provision.source = StartupCredentialProvisionSource::operator_file;
      provision.name_or_path = config.startup_credential_file.string();
    }

    auto credential_memory =
        std::make_shared<scratchbird::core::memory::MemoryManager>(
            scratchbird::core::memory::DefaultLocalEngineMemoryPolicy());
    auto credential_source =
        std::make_shared<StartupCredentialSource>(snapshot.database_uuid, 4);
    scratchbird::core::memory::MemoryTag credential_tag;
    credential_tag.category = scratchbird::core::memory::MemoryCategory::core_runtime;
    credential_tag.purpose = "server startup credential custody";
    credential_tag.callsite = "engine host startup credential acquisition";
    credential_tag.binary_ownership
        [scratchbird::core::memory::MemoryBinaryScopeKind::database] =
            bound.binding.database.bytes;
    credential_tag.binary_ownership
        [scratchbird::core::memory::MemoryBinaryScopeKind::owner] =
            bound.binding.principal.bytes;
    credential_tag.binary_ownership
        [scratchbird::core::memory::MemoryBinaryScopeKind::context] =
            bound.binding.database.bytes;

    const auto provisioned = ProvisionStartupCredential(
        provision, *credential_source, *credential_memory, credential_tag);
    if (!provisioned.ok()) {
      credential_source->Close();
      return fail_credential(
          StartupCredentialProvisionDiagnosticCode(provisioned.error),
          "bootstrap.startup_credential_unavailable");
    }
    // Manager assigned first so it is destroyed last.
    database_runtime->startup_credential_memory = std::move(credential_memory);
    database_runtime->startup_credential_source = std::move(credential_source);
  }

  std::filesystem::path bulk_import_registry_root = canonical_database_path;
  bulk_import_registry_root += ".bulk_import_streams.v1";
  database_runtime->bulk_import_stream_registry =
      std::make_shared<engine::internal_api::SblrBulkImportStreamRegistry>(
          bulk_import_registry_root);
  if (!database_runtime->bulk_import_stream_registry->healthy()) {
    const auto detail =
        database_runtime->bulk_import_stream_registry->startup_error();
    database_runtime.reset();
    snapshot.state = HostedDatabaseState::kQuarantined;
    snapshot.database_open = false;
    snapshot.write_admission_fenced = true;
    snapshot.diagnostic_code = "BULK.IMPORT.RECOVERY_CONFLICT";
    snapshot.diagnostic_message_key = "bulk.import.recovery_conflict";
    result.diagnostics.push_back(ServerDiagnostic{
        snapshot.diagnostic_code,
        snapshot.diagnostic_message_key,
        ServerDiagnosticSeverity::kError,
        "The durable bulk-import stream registry could not be opened or recovered.",
        {{"database_path", snapshot.database_path},
         {"registry_root", bulk_import_registry_root.string()},
         {"detail", detail}}});
    result.state.databases.push_back(snapshot);
    return result;
  }
  if (lifecycle_open.state.engine_agent_health_present) {
    snapshot.database_engine_agent_instance_uuid =
        BinaryIdentity(lifecycle_open.state.engine_agent_health.engine_instance_uuid);
    snapshot.selected_agent_type_ids =
        lifecycle_open.state.engine_agent_health.selected_agent_type_ids;
    // SEARCH_KEY: DATABASE_COMMITTED_AGENT_POLICY_READ
    // The database committed its agent policy at creation
    // (SB_AGENT_POLICY_BOOTSTRAP_BASELINE) and that policy is now read back at
    // open. It is reported rather than made authoritative for selection: the
    // canonical agent set is still being negotiated, so a database created
    // against an earlier registry legitimately commits a different set than the
    // running build would select. Refusing on that divergence would reject
    // databases that are not defective. Surfacing it is what lets the
    // difference be seen instead of silently ignored.
    if (committed_bootstrap_security) {
      const auto& committed = committed_bootstrap_security->committed_agent_policies;
      snapshot.committed_agent_registration_records =
          committed_bootstrap_security->committed_agent_registration_records;
      snapshot.committed_agent_policy_types.reserve(committed.size());
      for (const auto& policy : committed) {
        snapshot.committed_agent_policy_types.push_back(policy.agent_type);
        if (policy.enabled)
          snapshot.committed_enabled_agent_type_ids.push_back(policy.agent_type);
      }
      for (const auto& selected : snapshot.selected_agent_type_ids) {
        const bool committed_for_selected =
            std::find_if(committed.begin(), committed.end(),
                         [&](const auto& policy) {
                           return policy.agent_type == selected;
                         }) != committed.end();
        if (!committed_for_selected)
          snapshot.agent_policy_selected_without_committed_policy.push_back(selected);
      }
    }
    snapshot.database_engine_agent_state =
        scratchbird::core::agents::DatabaseEngineAgentLifecycleStateName(
            lifecycle_open.state.engine_agent_health.agent_state);
    snapshot.database_engine_agent_health_generation =
        lifecycle_open.state.engine_agent_health.health_generation;
    snapshot.database_engine_agent_ordinary_admission_allowed =
        lifecycle_open.state.engine_agent_health.ordinary_admission_allowed;
    snapshot.database_engine_agent_health_json =
        lifecycle_open.state.engine_agent_health_json;
  }
  const auto config_policy_security = StartConfigPolicySecurityLifecycle(
      BuildConfigPolicySecurityLifecycleInput(config,
                                              snapshot.database_path,
                                              snapshot.database_uuid,
                                              snapshot.database_open,
                                              snapshot.cluster_authority_required));
  if (!config_policy_security.ok()) {
    snapshot.state = HostedDatabaseState::kFailed;
    snapshot.diagnostic_code = config_policy_security.diagnostic.code.empty()
        ? "SERVER.ENGINE_HOST.CONFIG_POLICY_SECURITY_FAILED"
        : config_policy_security.diagnostic.code;
    snapshot.diagnostic_message_key = config_policy_security.diagnostic.message_key.empty()
        ? "server.engine_host.config_policy_security_failed"
        : config_policy_security.diagnostic.message_key;
    result.diagnostics.push_back(config_policy_security.diagnostic);
    result.state.databases.push_back(snapshot);
    return result;
  }
  snapshot.config_policy_security_lifecycle_present = true;
  snapshot.config_source_epoch = config_policy_security.lifecycle.config_source_epoch;
  snapshot.config_reload_generation = config_policy_security.lifecycle.config_reload_generation;
  snapshot.capability_policy_generation =
      config_policy_security.lifecycle.capability_policy_generation;
  snapshot.policy_generation = config_policy_security.lifecycle.policy_generation;
  snapshot.security_epoch = config_policy_security.lifecycle.security_epoch;
  snapshot.security_provider_family = config_policy_security.lifecycle.provider_family;
  snapshot.security_provider_generation = config_policy_security.lifecycle.provider_generation;
  snapshot.security_provider_state =
      SecurityProviderLifecycleStateName(config_policy_security.lifecycle.provider_state);
  snapshot.default_policy_installed = config_policy_security.lifecycle.default_policy_installed;
  snapshot.cache_invalidation_epoch = config_policy_security.lifecycle.cache_invalidation_epoch;
  snapshot.config_policy_security_lifecycle_json =
      SerializeConfigPolicySecurityLifecycleJson(config_policy_security.lifecycle);
  if (snapshot.state == HostedDatabaseState::kReadOnly ||
      snapshot.state == HostedDatabaseState::kRestrictedOpen ||
      snapshot.state == HostedDatabaseState::kMaintenance) {
    snapshot.read_only = true;
    snapshot.write_admission_fenced = true;
  } else {
    snapshot.write_admission_fenced = false;
  }
  if (database_ownership_lock) {
    result.state.database_ownership_locks.push_back(std::move(database_ownership_lock));
  }
  result.state.database_runtimes.push_back(std::move(database_runtime));

  // SEARCH_KEY: SERVER_NATIVE_HOSTED_RUNTIME_INTEGRATION
  // Open the native hosted runtime under this path's already-held process
  // admission, which is what makes engine-owned startup verification and an
  // admitted local-service session reachable at all. Three gates, by design:
  //
  //  * a crypto owner must have been supplied (decision 1a) -- every existing
  //    caller passes none and is unaffected;
  //  * embedded direct mode is excluded (decision 2a; the call site also
  //    supplies no owner, which is decision 2b). The embedded security model
  //    starts no agent runtime and cannot host a server-mode boundary, so an
  //    embedded caller must never acquire this;
  //  * the owner must report installed().
  //
  // ADDITIVE: a refusal here is recorded as a warning and changes nothing.
  // This open has already succeeded, and its reservation is committed below --
  // the native runtime BORROWS that admission and must not take it, because
  // the commit immediately after terminates the process if its reservation
  // was moved away.
  // SEARCH_KEY: SERVER_NATIVE_HOSTED_RUNTIME_INTEGRATION
  // DISABLED 2026-10-21 after it broke a live server. Keeping the code, the
  // decisions and the trace, and disabling activation, because the failure is
  // informative and the next attempt needs it.
  //
  // What happened: the gate below is reached in a real server (SBsrv supplies a
  // crypto owner), the native open ran, and it refused with
  // ownership_failure (NativeHostedRuntimeError 4) -- the native runtime
  // acquires DATABASE OWNERSHIP of its own, and StartHostedEngine already
  // holds that lock. Same collision class as the process admission B1 solved,
  // on a second shared resource I had not accounted for.
  //
  // Worse, the refusal was NOT additive in practice: the server reported
  // serving=false and never published its socket, so
  // live_server_agent_storage_benchmark_gate went PASS -> FAIL. A native open
  // that touches shared ownership on its way to failing cannot degrade
  // harmlessly, which invalidates the additive premise rather than the gating.
  //
  // The next attempt must give the native runtime a borrow-not-acquire path for
  // database ownership, exactly as B1 did for process admission, and must be
  // re-verified against a LIVE server test -- no unit suite produces this,
  // because only the real SBsrv passes a crypto owner.
  // DISABLED 2026-10-21, second time, on evidence. See FINDING-15.
  //
  // The ownership borrow worked: adopting the caller's lock moved the native
  // open past AcquireDatabaseOwnership and past custody Begin, so the error
  // went 4 (ownership_failure) -> 6 (bootstrap_failure). What did NOT change is
  // the thing that matters: the server still reported serving=false and never
  // published its socket, with the live gate failing either way.
  //
  // So coexistence is the defect, not the collisions. Both a clean early
  // refusal and a later one leave this server unable to serve, which means an
  // "additive" native open alongside the live open path is not achievable by
  // borrowing more resources -- the native runtime is built to BE the open
  // path, not to sit beside one. Finishing this needs the delegation
  // restructure (FINDING-11 option A), not another borrow.
  // DISABLED pending the delegation restructure. See FINDING-16.
  //
  // Option 2 worked: the production-lineage probe moved the native open from
  // bootstrap_failure(6) to adoption_failure(10), past identity, memory and
  // crypto. The format mismatch is genuinely solved.
  //
  // What remains is not a format or a resource-borrow problem. With the
  // startup-fatal diagnostic bug fixed (a "warning" in result.diagnostics
  // makes main() return 2 before serving), the live server now reports the
  // actual conflict: SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD, "The coordinator
  // could not read its database configuration." The native custody holds the
  // route owner lock exclusively, so the live coordinator cannot read through
  // it. Two route owners for one database is the irreducible conflict.
  //
  // So option 2 is a necessary component but not sufficient: it unblocks the
  // delegation restructure (there must be exactly ONE route owner, the native
  // runtime, with the live path consuming its observation) rather than
  // enabling coexistence.
  //
  // Attempt 5, 2026-10-23, and the earlier conclusion above is WRONG on its
  // central claim. "Two route owners for one database" was inferred from
  // SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD, but that diagnostic cannot mean what
  // it says: ProbeRouteOwnerLock reports kHeldByOtherProcess whenever flock on
  // a freshly opened descriptor fails, and flock is per open-file-description,
  // so a lock this very process holds on another descriptor is indistinguishable
  // from a foreign one.
  //
  // Measured instead of inferred. With the activation flag on, a probe inside
  // the lease registry during the failing coordinator read reports
  // `leases=0` -- the process-local RouteOwnershipLease registry is EMPTY. The
  // route owner lock is held on disk while no borrowable lease was ever
  // published, so Borrow, BorrowAlias and delegated read all miss for the same
  // reason, and the probe then misreports our own lock as another process's.
  //
  // So there are not two leases competing; there is one unpublished lock, and
  // in-process delegation is structurally impossible rather than fenced off.
  // The restructure needs the single owner to PUBLISH its lease
  // (DatabaseOwnershipLock::PublishStorageLease, called only after the checked
  // durable descriptor write) before any consumer can read through it.
  // Relaxing the transition fence -- which is what BorrowDelegatedRead below
  // does, and which is still required once publication happens -- is necessary
  // and not sufficient.
  // DISABLED pending lease publication by the single route owner. See FINDING-17.
  constexpr bool kNativeHostedRuntimeActivationEnabled = false;
  if (kNativeHostedRuntimeActivationEnabled &&
      crypto != nullptr && !config.embedded_direct_mode && crypto->installed()) {
    // Decision 3b: the limit shape and multipliers are taken from the one
    // caller proven to open successfully
    // (native_local_service_enrollment_test.cpp:1318,1361,1368), parameterised
    // by this database's page size rather than its profile table. They are
    // provisional production defaults, not a tuned policy; deriving them from
    // [server.memory] is the intended follow-up (decision 3a).
    const auto page_size_bytes = snapshot.page_size_bytes != 0
        ? static_cast<std::uint32_t>(snapshot.page_size_bytes) : std::uint32_t{16384};
    const auto page_size = static_cast<std::size_t>(page_size_bytes);
    // `catalog.maximum_profile` is a canonical filespace PAGE PROFILE uuid, not
    // a database identity: the workspace sizers resolve it through
    // FindCanonicalFilespacePageProfile and dereference the result without a
    // null check, so a non-profile uuid here is a crash rather than a refusal.
    // Look the profile up from this database's page size and refuse the native
    // open if the size is not a canonical one.
    const auto* page_profile =
        storage::disk::FindCanonicalFilespacePageProfileForSize(page_size_bytes);
    if (page_profile == nullptr) {
      // No duplicate commit/return path here: fall through to the single exit
      // below, which commits and returns exactly once.
      // NOT result.diagnostics: main() returns 2 on ANY non-empty
      // engine_host.diagnostics before it ever serves, so a "warning" here
      // aborts startup. That is what made every native-open refusal break the
      // server regardless of how far the open got. Trace it instead.
      TraceServerStartupPhase(config, "native_hosted_runtime",
                              "refused_no_page_profile");
    } else {
    NativeHostedRuntimeLimits native_limits;
    native_limits.writer.maximum_owner_backing_bytes = 64u * 1024u;
    native_limits.writer.source.maximum_backing_bytes = 512u * page_size;
    native_limits.writer.source.maximum_verification_image_bytes = 32u * 1024u * 1024u;
    native_limits.database_crypto_bytes = 2u * 1024u * 1024u;
    native_limits.catalog = {page_profile->uuid, 1, 32, 128, 128,
                             {16, 32u * page_size}};
    native_limits.authorization = {128, 128, 1000000};

    DatabaseOwnershipRequest native_request;
    native_request.database_path = snapshot.database_path;
    native_request.owner_kind = "server";
    native_request.storage_access = storage::disk::RouteStorageAccess::read_write;

    std::vector<std::unique_ptr<storage::disk::FileDevice>> native_secondary;
    // Both shared resources are adopted rather than re-acquired: the process
    // admission reservation and this database's ownership lock. Acquiring
    // either a second time is what refused before.
    const auto native = NativeHostedDatabaseRuntime::OpenUnderHeldAdmission(
        *crypto, native_request, native_limits, native_secondary,
        process_admission.reservation,
        // NOT `database_ownership_lock`: it was moved into
        // result.state.database_ownership_locks above, so reading it here
        // would pass a null pointer, silently fall back to acquiring a second
        // lock, and reproduce the original ownership_failure. Take a shared
        // reference to the retained one instead.
        result.state.database_ownership_locks.empty()
            ? nullptr
            : result.state.database_ownership_locks.back(),
        // SEARCH_KEY: NATIVE_PROBE_ACCEPTS_PRODUCTION_LINEAGE
        // Stated, not inferred. This caller opens a database written by the
        // production create path, whose offset 0 is a DatabaseHeader
        // (SBDBV001) rather than a native FilespaceBootstrap preamble. Before,
        // the same acceptance was derived from the ownership lock passed
        // above; the two are now independent, so changing who owns the route
        // cannot silently change which lineages the probe will decode.
        NativeBootstrapLineagePolicy::accept_production_lineage);
    if (native.error != NativeHostedRuntimeError::none || !native.runtime) {
      // See above: a diagnostic here is fatal to startup, not advisory.
      TraceServerStartupPhase(config, "native_hosted_runtime_error",
          std::to_string(static_cast<unsigned>(native.error)));
    } else {
      snapshot.native_hosted_runtime_open = true;
    }
    }
    // A silently-degrading path needs a success marker, or a clean run proves
    // nothing about whether it ran. Traced beside the other startup phases.
    TraceServerStartupPhase(config, "native_hosted_runtime",
        snapshot.native_hosted_runtime_open ? "accepted" : "refused");
  } else {
    TraceServerStartupPhase(config, "native_hosted_runtime", "not_selected");
  }

  result.state.databases.push_back(snapshot);
  if (!process_admission.reservation.Commit()) std::terminate();
  startup_completed = true;
  return result;
}

std::string HostedEngineStatusJson(const HostedEngineState& state) {
  scratchbird::wire::binary_status::Stream out;
  out << "{\"engine_host\":{\"active\":" << (state.engine_context_active ? "true" : "false")
      << ",\"databases\":[";
  for (std::size_t i = 0; i < state.databases.size(); ++i) {
    const auto& database = state.databases[i];
    if (i != 0) out << ',';
    out << "{\"state\":\"" << HostedDatabaseStateName(database.state) << "\","
        << "\"database_path\":\"" << JsonEscape(database.database_path) << "\","
        << "\"database_uuid\":\""
        << scratchbird::wire::binary_status::Identity(database.database_uuid)
        << "\","
        << "\"filespace_uuid\":\""
        << scratchbird::wire::binary_status::Identity(database.filespace_uuid)
        << "\","
        << "\"page_size_bytes\":" << database.page_size_bytes << ","
        << "\"database_created\":" << (database.database_created ? "true" : "false") << ","
        << "\"database_open\":" << (database.database_open ? "true" : "false") << ","
        << "\"read_only\":" << (database.read_only ? "true" : "false") << ","
        << "\"write_admission_fenced\":" << (database.write_admission_fenced ? "true" : "false") << ","
        << "\"cluster_authority_required\":"
        << (database.cluster_authority_required ? "true" : "false") << ","
        << "\"database_engine_agent_state\":\""
        << JsonEscape(database.database_engine_agent_state) << "\","
        << "\"database_engine_agent_instance_uuid\":\""
        << scratchbird::wire::binary_status::Identity(database.database_engine_agent_instance_uuid) << "\","
        << "\"database_engine_agent_health_generation\":"
        << database.database_engine_agent_health_generation << ","
        << "\"database_engine_agent_ordinary_admission_allowed\":"
        << (database.database_engine_agent_ordinary_admission_allowed ? "true" : "false") << ","
        << "\"database_engine_agent_health\":"
        << (database.database_engine_agent_health_json.empty()
                ? "{\"database_engine_agent\":{\"agent_state\":\"not_started\"}}"
                : database.database_engine_agent_health_json)
        << ","
        << "\"config_policy_security_lifecycle\":"
        << (database.config_policy_security_lifecycle_json.empty()
                ? "{\"config_policy_security_lifecycle\":{\"present\":false}}"
                : database.config_policy_security_lifecycle_json)
        << ","
        << "\"startup_recovery_classification\":\""
        << JsonEscape(database.startup_recovery_classification) << "\","
        << "\"diagnostic_code\":\"" << JsonEscape(database.diagnostic_code) << "\"}";
  }
  out << "]}}\n";
  return out.str();
}

}  // namespace scratchbird::server
