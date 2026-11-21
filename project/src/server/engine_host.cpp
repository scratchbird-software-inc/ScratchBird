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
#include "native_database_loop.hpp"
#include "native_hosted_database_runtime.hpp"
#include "native_database_open_discovery.hpp"
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

#include <chrono>
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

std::string UnsignedArrayJson(const std::vector<std::uint64_t>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) out += (i ? "," : "") + std::to_string(values[i]);
  return out + "]";
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

  // SEARCH_KEY: SB_LEGACY_CUT_NATIVE_OPEN (owner 2026-11-16; thin-slice S3a).
  // "The moment you can create a valid database, the legacy support is
  // dropped." This process assumes the file is native and discovers it
  // (SB_NATIVE_DATABASE_OPEN_DISCOVERY) BEFORE any other step: no legacy
  // ownership, no lock file, no legacy security read, no OpenDatabaseFile, no
  // descriptor recovery, no bulk-import directory, no agent. Order:
  //  1. a prelocked handoff carries no native lease and refuses;
  //  2. a missing file refuses (stat only, nothing created);
  //  3. native data-file ownership (S1: flock on the data descriptor, no lock
  //     file), the first block, then page zero through the owner's own device;
  //  4. an invalid file refuses with DATABASE.OPEN_NOT_A_VALID_DATABASE
  //     (detail = the bootstrap/page-zero error, the file untouched) and the
  //     ownership is released; a valid native file keeps the ownership and
  //     goes to the database loop (S3b, below), which opens and verifies it
  //     read-only and reports it serving. (Was, S3a: ownership released and a
  //     valid native file refused with the temporary
  //     DATABASE.OPEN_NATIVE_RUNTIME_PENDING; that code now remains only for
  //     the lifecycle operations and the engine-handle open that are not
  //     built: S3e-S3f.)
  // No decryption key reaches this entry point today (no configuration field
  // carries one), so Q253's refusal is applied where a key can be given (the
  // lifecycle operations' decryption options), not invented here.
  // The legacy hosted open below this block is kept, unreachable from this
  // entry point, until it is deleted family by family.
  {
    const auto refuse = [&](HostedDatabaseState state, std::string code, std::string detail,
                            const char* message) {
      snapshot.state = state;
      snapshot.database_open = false;
      snapshot.diagnostic_code = code;
      snapshot.diagnostic_message_key = code;
      result.diagnostics.push_back(ServerDiagnostic{
          std::move(code), snapshot.diagnostic_message_key, ServerDiagnosticSeverity::kError, message,
          {{"database_path", snapshot.database_path}, {"detail", std::move(detail)}}});
      result.state.databases.push_back(snapshot);
      return result;
    };
    if (config.database_ownership_prelocked) {
      return refuse(HostedDatabaseState::kFailed, "ARCH.DATABASE_OWNERSHIP_LEGACY_REFUSED",
                    "prelocked_ownership_handoff_carries_no_native_lease",
                    "A prelocked ownership handoff is the legacy route; native open takes its own ownership.");
    }
    std::error_code exists_error;
    const bool database_exists = std::filesystem::exists(config.database_default_path, exists_error);
    if (!database_exists && !config.database_auto_create) {
      return refuse(HostedDatabaseState::kFailed, "SERVER.ENGINE_HOST.DATABASE_NOT_FOUND",
                    exists_error ? "database_path_status_failed" : "database_not_found",
                    "The configured database does not exist and auto-create is disabled.");
    }
    if (!database_exists) {
      return refuse(HostedDatabaseState::kFailed, "BOOTSTRAP.SERVICE_START_FORBIDDEN",
                    "server_start_never_creates",
                    "Public server startup cannot create a missing database; run the approved embedded bootstrap first.");
    }
    DatabaseOwnershipRequest native_request;
    native_request.database_path = config.database_default_path;
    native_request.owner_kind = config.database_ownership_owner_kind.empty()
                                    ? "server"
                                    : config.database_ownership_owner_kind;
    native_request.ownership = storage::disk::DatabaseFileOwnership::native_data_file;
    native_request.storage_access = OpenModeForConfig(config) == SB_ENGINE_OPEN_NORMAL
        ? storage::disk::RouteStorageAccess::read_write
        : storage::disk::RouteStorageAccess::read_only;
    auto native_ownership = AcquireDatabaseOwnership(native_request);
    if (!native_ownership.acquired || !native_ownership.lock) {
      return refuse(HostedDatabaseState::kFailed,
                    native_ownership.diagnostic_code.empty() ? "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED"
                                                             : native_ownership.diagnostic_code,
                    native_ownership.diagnostic_detail,
                    "The configured database file could not be owned for native open.");
    }
    scratchbird::storage::database::NativeOpenDiscoveryResult discovered;
    {
      storage::disk::FileDevice device;
      const auto served = device.OpenServed(native_ownership.lock->ServedStorageLease(),
                                            snapshot.database_path,
                                            storage::disk::FileOpenMode::open_existing_read_only);
      if (!served.ok()) {
        discovered.code = served.diagnostic.diagnostic_code.empty() ? "ARCH.DATABASE_OWNERSHIP_LOCK_FAILED"
                                                                    : served.diagnostic.diagnostic_code;
        discovered.detail = "native_owner_device_refused";
      } else {
        discovered = scratchbird::storage::database::DiscoverNativeDatabaseOnOpenDevice(device);
        (void)device.Close();
      }
    }
    // Every refusal below releases the native ownership (the flock) before it
    // returns, so a refused open leaves the file ownable.
    const auto release_ownership = [&] {
      native_ownership.lock->release();
      native_ownership.lock.reset();
    };
    if (!discovered.ok()) {
      release_ownership();
      return refuse(HostedDatabaseState::kFailed, discovered.code, discovered.detail,
                    "The configured file is not a valid database.");
    }
    snapshot.database_uuid = discovered.page_zero->bootstrap.database_uuid;
    snapshot.filespace_uuid = discovered.page_zero->bootstrap.filespace_uuid;
    snapshot.page_size_bytes = discovered.page_zero->bootstrap.page_size_bytes;

    // SEARCH_KEY: SB_NATIVE_SERVING_OPEN (thin-slice S3b, plan 38). A valid
    // native file is opened and verified READ-ONLY on the database loop thread
    // (SB_NATIVE_DATABASE_LOOP), the transaction-store Restart runs once on that
    // thread (S3d, SB_NATIVE_RESTART), and the database is reported serving |
    // restricted | failed; write admission stays fenced (no statement path until
    // S3e/S3f). Refusals that precede the loop,
    // each specific:
    //  * a configured startup credential source: under Q281 an outside party
    //    hands the server a secret ONLY as the decryption key of an encrypted
    //    database; L5's credential is always derived inside the server from
    //    the file. Decryption is not built (S3a), so a configured source is
    //    DATABASE.OPEN_DECRYPTION_UNAVAILABLE, exactly as a key given at the
    //    lifecycle operations is. (Was: the host read L5's password from an
    //    operator file or a service-manager credential.)
    //  * a non-NORMAL open mode: the native runtime owns the file read-write;
    //    read-only / maintenance / restricted native opens are not built.
    //  * no process crypto owner (the embedded entry): the native runtime needs
    //    the process memory and crypto custody a server process owns.
    if (config.startup_credential_source != "none") {
      release_ownership();
      return refuse(HostedDatabaseState::kFailed,
                    std::string(scratchbird::storage::database::kOpenDecryptionUnavailableCode),
                    "startup_credential_source_configured_is_the_decryption_key_only",
                    "A decryption key was supplied; decryption is not implemented.");
    }
    if (OpenModeForConfig(config) != SB_ENGINE_OPEN_NORMAL) {
      release_ownership();
      return refuse(HostedDatabaseState::kFailed, "DATABASE.OPEN_NATIVE_MODE_UNAVAILABLE",
                    "native_open_supports_the_normal_mode_only",
                    "The native database can only be opened in the normal mode.");
    }
    if (crypto == nullptr || config.embedded_direct_mode) {
      release_ownership();
      return refuse(HostedDatabaseState::kFailed, "DATABASE.OPEN_NATIVE_CRYPTO_OWNER_REQUIRED",
                    config.embedded_direct_mode ? "embedded_native_open_not_built" : "no_process_crypto_owner_supplied",
                    "Serving a native database requires the server process's crypto and memory custody.");
    }
    auto native_loop = NativeDatabaseLoop::Create();
    NativeLoopOpenRequest loop_request;
    loop_request.crypto = crypto;
    loop_request.ownership = native_ownership.lock;
    loop_request.database_uuid = snapshot.database_uuid;
    loop_request.filespace_uuid = snapshot.filespace_uuid;
    loop_request.page_size_bytes = static_cast<std::uint32_t>(snapshot.page_size_bytes);
    loop_request.total_pages = discovered.page_zero->total_pages;
    TraceServerStartupPhase(config, "native_database_loop", "opening");
    const auto loop_opened = native_loop->SubmitOpen(std::move(loop_request)).get();
    if (!loop_opened.ok) {
      // The loop already closed everything it acquired on its own thread;
      // join it, then release the ownership.
      (void)native_loop->Close();
      native_loop.reset();
      release_ownership();
      snapshot.state = HostedDatabaseState::kFailed;
      snapshot.database_open = false;
      snapshot.diagnostic_code = loop_opened.diagnostic.code;
      snapshot.diagnostic_message_key = loop_opened.diagnostic.code;
      std::vector<ServerDiagnosticField> fields{{"database_path", snapshot.database_path},
                                                {"step", loop_opened.diagnostic.step}};
      for (const auto& field : loop_opened.diagnostic.fields) fields.push_back({field.first, field.second});
      result.diagnostics.push_back(ServerDiagnostic{loop_opened.diagnostic.code, loop_opened.diagnostic.code,
                                                    ServerDiagnosticSeverity::kError,
                                                    "The native database could not be opened and verified.",
                                                    std::move(fields)});
      result.state.databases.push_back(snapshot);
      return result;
    }
    TraceServerStartupPhase(config, "native_database_loop", "serving");
    std::error_code canonical_error;
    const auto native_canonical = std::filesystem::canonical(config.database_default_path, canonical_error);
    auto native_runtime = std::make_shared<HostedDatabaseRuntime>();
    native_runtime->canonical_database_path = canonical_error ? snapshot.database_path : native_canonical.string();
    native_runtime->database_uuid = snapshot.database_uuid;
    native_runtime->native_loop = std::move(native_loop);
    native_runtime->native_served_facts = loop_opened.facts.served_facts;
    snapshot.state = HostedDatabaseState::kOpen;
    snapshot.database_created = false;
    snapshot.database_open = true;
    snapshot.native_hosted_runtime_open = true;
    // S3d: the database's serving state after the loop's Restart (serving | restricted | failed).
    // (Was: NativeLoopStateName(serving), always "serving", with Restart pending.)
    const auto& restart = loop_opened.facts.restart;
    snapshot.native_serving_state = NativeServingStateName(loop_opened.facts.serving_state);
    snapshot.native_restart_pending = loop_opened.facts.restart_pending;
    snapshot.native_verified_transactions = loop_opened.facts.transactions;
    snapshot.native_service_principal_uuid = loop_opened.facts.service_principal_uuid;
    snapshot.native_engine_instance_uuid = loop_opened.facts.engine_instance_uuid;
    snapshot.native_restart_ran = restart.ran;
    snapshot.native_writer_failed = restart.writer_failed;
    snapshot.native_restart_next_transaction = restart.next_transaction;
    snapshot.native_restart_scanned_from = restart.scanned_from;
    snapshot.native_restart_marked_dead = restart.marked_dead;
    snapshot.native_restart_writes = restart.writes;
    snapshot.native_restart_syncs = restart.syncs;
    snapshot.native_uncertain_transactions = restart.uncertain_transactions;
    snapshot.native_restart_unresolved = restart.unresolved;
    snapshot.native_horizon_oit = restart.horizon_oit;
    snapshot.native_horizon_oat = restart.horizon_oat;
    snapshot.native_horizon_ost = restart.horizon_ost;
    snapshot.native_horizon_oct = restart.horizon_oct;
    snapshot.native_horizon_oar = restart.horizon_oar;
    snapshot.native_horizon_olr = restart.horizon_olr;
    // Classification (Was, S3b/S3c: native_verified_restart_pending).
    switch (loop_opened.facts.serving_state) {
      case NativeServingState::serving:
        snapshot.startup_recovery_classification =
            restart.marked_dead == 0 ? "native_verified_restart_clean" : "native_verified_restarted";
        break;
      case NativeServingState::restricted:
        snapshot.startup_recovery_classification = "native_verified_restart_unresolved_restricted";
        snapshot.state = HostedDatabaseState::kRestrictedOpen;
        break;
      case NativeServingState::failed:
        snapshot.startup_recovery_classification = "native_restart_write_failed_reopen_required";
        snapshot.state = HostedDatabaseState::kFailed;
        break;
    }
    if (loop_opened.facts.serving_state != NativeServingState::serving) {
      snapshot.diagnostic_code = loop_opened.facts.serving_diagnostic.code;
      snapshot.diagnostic_message_key = loop_opened.facts.serving_diagnostic.code;
    }
    // Writes stay fenced: no statement exists yet (S3e/S3f), and a restricted or failed database
    // never admits one. A read-only database reports it.
    snapshot.read_only = restart.read_only_skipped || snapshot.state == HostedDatabaseState::kRestrictedOpen;
    snapshot.write_admission_fenced = true;
    snapshot.lifecycle_mode = config.database_open_mode;
    result.state.database_ownership_locks.push_back(std::move(native_ownership.lock));
    result.state.database_runtimes.push_back(std::move(native_runtime));
    result.state.databases.push_back(snapshot);
    if (loop_opened.facts.serving_state == NativeServingState::failed) {
      // A write failed inside Restart: the database is open (so the failure is reportable through the
      // status and closes cleanly) but the process must not serve it; requires a reopen. main() exits
      // on any diagnostic, with this specific code. The state above is kept for the caller to read.
      std::vector<ServerDiagnosticField> fields{{"database_path", snapshot.database_path},
                                                {"step", loop_opened.facts.serving_diagnostic.step}};
      for (const auto& field : loop_opened.facts.serving_diagnostic.fields) fields.push_back({field.first, field.second});
      result.diagnostics.push_back(ServerDiagnostic{loop_opened.facts.serving_diagnostic.code,
                                                    loop_opened.facts.serving_diagnostic.code,
                                                    ServerDiagnosticSeverity::kError,
                                                    "The native database's transaction-store restart failed to write; reopen it.",
                                                    std::move(fields)});
    }
    if (!process_admission.reservation.Commit()) std::terminate();
    startup_completed = true;
    return result;
  }

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
            snapshot.database_path,
            database_ownership_lock ? database_ownership_lock->ServedStorageLease() : nullptr);
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
  // C9 step 7: sbsrv verifies the file through the owner it just acquired.
  if (database_ownership_lock)
    lifecycle_open_config.served_owner = database_ownership_lock->ServedStorageLease();
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

  // SEARCH_KEY: SECURITY-INTERNAL-SESSION-ADMISSION
  // Mint the authenticated internal startup session the coordinator consumes.
  //
  // Placed here because the spec requires the engine to retain it
  // (`appendix-node-agent-coordinator.md:117-118`) and because this is the
  // point where the committed identities are already in hand: the credential
  // above is transport custody only, as its own declaration says, so this is
  // the authentication step that was missing -- nothing previously read
  // `startup_credential_source` at all.
  //
  // Identities come only from what the database COMMITTED. An absent committed
  // service principal or sysarch role leaves the session absent, which fails
  // the coordinator closed (`:144`) instead of synthesizing authority. That is
  // the same rule `ServerAgentRequestMetadata` states: request metadata may not
  // issue a principal or manufacture grants.
  if (committed_bootstrap_security &&
      committed_bootstrap_security->runtime_authority_binding_present &&
      !committed_bootstrap_security->service_principal_uuid.is_nil() &&
      !committed_bootstrap_security->sysarch_role_uuid.value.is_nil()) {
    // A session is a RUNTIME identity, not a durable one, and the two have
    // different issuers.
    //
    // `UuidKindAllowsDurableIdentity()` deliberately EXCLUDES
    // `UuidKind::session` (`uuid.cpp:339-341`) while `IsEngineIdentityKind()`
    // includes it -- that pair is the distinction, not an oversight. And
    // `GenerateEngineIdentityV7` is an unconditional alias of the durable
    // variant (`uuid.cpp:655-657`), so calling it for `session` is refused
    // categorically with `SB-UUID-DURABLE-IDENTITY-KIND`. That is what made
    // this block take its `refused_no_session_identity` branch on EVERY open,
    // observed by trace -- the session had never once minted.
    //
    // `IssueRuntimeIdentityV7` is the issuer for a fresh runtime object: it
    // applies the local clock checks and strict regression rejection that a
    // caller-supplied timestamp does not establish (and whose header says so).
    // It is also not a hand-stamped v7: `06-sblr-...-contract.md:999` states
    // that a value "with UUID version bits added is not valid runtime identity
    // issuance", which rules out building one from
    // `GenerateCompatibilityUnixTimeV7` + `MakeTypedUuid`.
    const auto session_identity = scratchbird::core::uuid::IssueRuntimeIdentityV7();
    if (session_identity.has_value()) {
      auto& session = database_runtime->internal_startup_session;
      session.principal_uuid = committed_bootstrap_security->service_principal_uuid;
      session.sysarch_role_uuid = committed_bootstrap_security->sysarch_role_uuid.value;
      session.session_uuid = *session_identity;
      session.database_uuid = snapshot.database_uuid;
      // KNOWN DEVIATION, measured 2026-10-24: this reads `security_epoch`
      // BEFORE the committed config-policy lifecycle assigns it (that happens
      // further down this same function), so the value taken here is the
      // struct's initializer, not the committed epoch. All four generations on
      // this session are therefore defaulted today, not just the three C17
      // names -- `epochs_are_conforming` stays false and now covers all four.
      // Do not describe `security_epoch` as the one real supplier at this
      // point in the open; it is not one yet here.
      session.security_epoch = snapshot.security_epoch;
      // The other three have no database supplier anywhere -- see the KNOWN
      // DEVIATION note on InternalStartupSession. Carry the same values an
      // ordinary client session carries so admission behaves identically, and
      // record that they are NOT conforming rather than implying they are.
      session.catalog_generation_id = 1;
      session.resource_epoch = 1;
      session.name_resolution_epoch = 1;
      session.epochs_are_conforming = false;
      session.present = session.security_epoch != 0;
      TraceServerStartupPhase(config, "internal_startup_session",
                              session.present ? "minted" : "refused_no_security_epoch");
    } else {
      TraceServerStartupPhase(config, "internal_startup_session",
                              "refused_no_session_identity");
    }
  } else {
    // Not an error: an internal service principal is optional at create, so a
    // database that committed none simply has no internal session. The
    // coordinator then refuses rather than running unauthenticated.
    TraceServerStartupPhase(config, "internal_startup_session",
                            "absent_no_committed_authority");
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

  // (Deleted, thin-slice S3b: the C9 comment block and the disabled
  // kNativeHostedRuntimeActivationEnabled path with it, including the one use
  // of NativeBootstrapLineagePolicy::accept_production_lineage. The native
  // runtime is now opened by the database loop above, native lineage only.
  // The history it recorded (FINDING-11/15/16/17, the delegated-read attempts,
  // BorrowDelegatedRead withdrawn 2026-10-29) is in the corrections register
  // and the plans; none of the code under it was reachable.)

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
        << "\"native_serving_state\":\"" << JsonEscape(database.native_serving_state) << "\","
        << "\"native_restart_pending\":" << (database.native_restart_pending ? "true" : "false") << ","
        << "\"native_restart_ran\":" << (database.native_restart_ran ? "true" : "false") << ","
        << "\"native_writer_failed\":" << (database.native_writer_failed ? "true" : "false") << ","
        << "\"native_restart_next_transaction\":" << database.native_restart_next_transaction << ","
        << "\"native_restart_scanned_from\":" << database.native_restart_scanned_from << ","
        << "\"native_restart_marked_dead\":" << database.native_restart_marked_dead << ","
        << "\"native_restart_writes\":" << database.native_restart_writes << ","
        << "\"native_restart_syncs\":" << database.native_restart_syncs << ","
        << "\"native_uncertain_transactions\":" << database.native_uncertain_transactions << ","
        << "\"native_restart_unresolved\":" << UnsignedArrayJson(database.native_restart_unresolved) << ","
        << "\"native_stored_horizons\":{\"oit\":" << database.native_horizon_oit
        << ",\"oat\":" << database.native_horizon_oat << ",\"ost\":" << database.native_horizon_ost
        << ",\"oct\":" << database.native_horizon_oct << ",\"oar\":" << database.native_horizon_oar
        << ",\"olr\":" << database.native_horizon_olr << "},"
        << "\"native_service_principal_uuid\":\""
        << scratchbird::wire::binary_status::Identity(database.native_service_principal_uuid) << "\","
        << "\"diagnostic_code\":\"" << JsonEscape(database.diagnostic_code) << "\"}";
  }
  out << "]}}\n";
  return out.str();
}

std::shared_ptr<storage::disk::RouteOwnershipLease> ServedRouteOwnerFor(
    const HostedEngineState& engine_state, const std::string& database_path) {
  if (database_path.empty()) return {};
  // SEARCH_KEY: SB_SERVED_OWNER_LEASE_IDENTITY (S3a): keyed on the owner's
  // lease identity (a native owner's data-file st_dev/st_ino; a legacy owner's
  // lock path), so a native owner, which has no lock path, is found.
  // (Was: matched lock->lock_path() against DatabaseOwnershipLockPath, which
  // never matched a native owner.)
  for (const auto& lock : engine_state.database_ownership_locks) {
    if (lock && lock->OwnsDatabasePath(database_path)) return lock->ServedStorageLease();
  }
  return {};
}

namespace {
// The hosted runtime of the native database at database_path (found by its canonical path), or null. ONE lookup for the
// served facts and for the database loop (S4e: the loop lookup was a second copy of this loop).
std::shared_ptr<HostedDatabaseRuntime> FindServedNativeRuntime(const HostedEngineState& engine_state,
                                                               const std::string& database_path) {
  if (database_path.empty()) return {};
  std::error_code error;
  const auto canonical = std::filesystem::canonical(database_path, error);
  const std::string wanted = error ? database_path : canonical.string();
  for (const auto& runtime : engine_state.database_runtimes)
    if (runtime && runtime->native_served_facts &&
        (runtime->canonical_database_path == wanted || runtime->canonical_database_path == database_path))
      return runtime;
  return {};
}
}  // namespace

std::shared_ptr<const engine::internal_api::NativeServedDatabaseFacts> ServedNativeFactsFor(
    const HostedEngineState& engine_state, const std::string& database_path) {
  const auto runtime = FindServedNativeRuntime(engine_state, database_path);
  return runtime ? runtime->native_served_facts : nullptr;
}

std::shared_ptr<NativeDatabaseLoop> ServedNativeLoopFor(const HostedEngineState& engine_state, const std::string& database_path) {
  const auto runtime = FindServedNativeRuntime(engine_state, database_path);
  return runtime ? runtime->native_loop : nullptr;
}

}  // namespace scratchbird::server
