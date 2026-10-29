// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_ENGINE_HOST_DATABASE_SUPERVISION

#pragma once

#include "config.hpp"
#include "startup_credential_source.hpp"
#include "database_ownership.hpp"
#include "diagnostics.hpp"
#include "api_types.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {
class SblrBulkImportStreamRegistry;
}

namespace scratchbird::server {

enum class HostedDatabaseState {
  kNotConfigured,
  kOpening,
  kOpen,
  kReadOnly,
  kRestrictedOpen,
  kMaintenance,
  kFailed,
  kDetached,
  kQuarantined,
};

struct HostedDatabaseSnapshot {
  HostedDatabaseState state = HostedDatabaseState::kNotConfigured;
  std::string database_path;
  scratchbird::core::platform::Uuid database_uuid;
  scratchbird::core::platform::Uuid filespace_uuid;
  std::uint64_t page_size_bytes = 0;
  std::string resource_seed_pack_root;
  std::string policy_seed_pack_root;
  bool database_created = false;
  bool database_open = false;
  bool read_only = false;
  bool write_admission_fenced = true;
  bool cluster_structures_present = false;
  bool cluster_authority_required = false;
  std::string startup_recovery_classification;
  std::string lifecycle_mode;
  std::string database_engine_agent_state = "not_started";
  scratchbird::core::platform::Uuid database_engine_agent_instance_uuid;
  std::vector<std::string> selected_agent_type_ids;
  std::uint64_t database_engine_agent_health_generation = 0;
  bool database_engine_agent_ordinary_admission_allowed = false;
  std::string database_engine_agent_health_json;
  bool config_policy_security_lifecycle_present = false;
  std::uint64_t config_source_epoch = 1;
  std::uint64_t config_reload_generation = 1;
  std::uint64_t capability_policy_generation = 1;
  std::uint64_t policy_generation = 1;
  std::uint64_t security_epoch = 1;
  // SEARCH_KEY: SERVER_NATIVE_HOSTED_RUNTIME_INTEGRATION
  // True when the native hosted runtime opened alongside this database, which
  // is the precondition for engine-owned startup verification and an admitted
  // local-service session. False is a served database without them, not a
  // failure.
  bool native_hosted_runtime_open = false;
  // SEARCH_KEY: DATABASE_COMMITTED_AGENT_POLICY_READ
  // Agent policy this database committed at creation, as read back at open.
  // Reported, not yet authoritative for selection: `selected_agent_type_ids`
  // above still comes from the opened database's agent health. These fields
  // make the committed set observable and let a divergence from it be seen.
  std::vector<std::string> committed_agent_policy_types;
  std::vector<std::string> committed_enabled_agent_type_ids;
  std::uint32_t committed_agent_registration_records = 0;
  // Agent types the runtime would select that the database did not commit a
  // policy for, and vice versa. Empty when the committed set covers the
  // selection exactly.
  std::vector<std::string> agent_policy_selected_without_committed_policy;
  // SEARCH_KEY: DATABASE_COMMITTED_STARTUP_AUTHORITY_MODE
  // The authority source this database committed at creation, once the open has
  // confirmed it matches the configured mode. Empty when the credentialed
  // bootstrap read did not run for this database.
  std::string security_authority_mode;
  std::string security_provider_family = "local_password";
  std::uint64_t security_provider_generation = 1;
  std::string security_provider_state = "healthy";
  bool default_policy_installed = true;
  std::uint64_t cache_invalidation_epoch = 1;
  std::string config_policy_security_lifecycle_json;
  std::string diagnostic_code;
  std::string diagnostic_message_key;
};

// The authenticated internal startup session the engine retains at open.
//
// Every field is read from what this database COMMITTED. Nothing here is
// derived, defaulted or synthesized from configuration: an absent committed
// identity leaves `present` false, which fails the coordinator closed rather
// than inventing an identity -- required by
// `chapters/agents/appendix-node-agent-coordinator.md:144`.
//
// Authority comes from `sysarch_role_uuid`. Owner direction 2026-10-24: the
// master agent is sysarch, i.e. full rights, and that is already implemented --
// `agent_authorization_context.hpp:144-148` grants every KnownSecurityRights()
// when the engine-owned bootstrap role is present. So no new principal and no
// new grants are needed; the session only has to carry the role.
struct InternalStartupSession {
  bool present = false;
  scratchbird::core::platform::Uuid principal_uuid{};
  scratchbird::core::platform::Uuid session_uuid{};
  scratchbird::core::platform::Uuid sysarch_role_uuid{};
  scratchbird::core::platform::Uuid database_uuid{};
  // Authority generations. `security_epoch` is the only one with a real
  // supplier today (the committed config-policy security lifecycle).
  //
  // KNOWN DEVIATION from `DR-LOCAL-SERVICE-ADMISSION-001`'s `Real epochs`
  // condition, recorded here rather than hidden: catalog generation, resource
  // epoch and name-resolution epoch have NO database supplier anywhere in the
  // serving path. They are declared `= 1` on ServerSessionRecord and either
  // copied onward or self-incremented, so ordinary CLIENT sessions already run
  // on constants and ValidateBeginTransactionAdmission's
  // `authority_generation_required` check passes on initializers rather than on
  // authority. This session is no worse than a client session, but it is NOT
  // conforming, and must not be described as satisfying `Real epochs` until
  // those three gain a real supplier. Tracked as C17 in the corrections
  // register.
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  bool epochs_are_conforming = false;
};

// Process-lifetime resources for one opened database.  This object is kept
// separate from the copyable public/server snapshot so session records and
// copied snapshots cannot acquire ownership of durable engine registries.
struct HostedDatabaseRuntime {
  std::string canonical_database_path;
  scratchbird::core::platform::Uuid database_uuid;
  std::shared_ptr<engine::internal_api::SblrBulkImportStreamRegistry>
      bulk_import_stream_registry;
  // The server's own startup credential, published at open under the identities
  // this database committed. Declared manager-first so the source, which holds
  // protected material allocated from it, is destroyed first.
  //
  // Present only when a credential source was configured and the credential was
  // actually obtained. Publishing it is transport custody, not authentication:
  // the engine still verifies it against the committed catalog.
  std::shared_ptr<scratchbird::core::memory::MemoryManager>
      startup_credential_memory;
  std::shared_ptr<StartupCredentialSource> startup_credential_source;
  // The authenticated internal startup session, minted at open.
  //
  // SEARCH_KEY: SECURITY-INTERNAL-SESSION-ADMISSION
  // `chapters/agents/appendix-node-agent-coordinator.md:117-118` requires the
  // ENGINE to retain "the authenticated internal startup session" before
  // constructing its database UUID issuer, so it is minted here at open rather
  // than by whoever later starts the coordinator. The coordinator consumes it;
  // it never authenticates.
  //
  // It lives on the RUNTIME, deliberately, not on HostedDatabaseSnapshot.
  // The snapshot is a copyable status record, and
  // server_agent_authority_admission_gate asserts across four modes that a
  // copied status with healthy-looking fields and arbitrary epochs cannot
  // supply authority. Reaching this requires FindHostedDatabaseRuntime plus a
  // live ownership lock, which a copied status does not carry -- so putting the
  // session here keeps that refusal intact. Putting it on the snapshot would
  // silently hand authority to any copy.
  InternalStartupSession internal_startup_session;
};

struct HostedEngineState {
  bool engine_context_active = false;
  std::vector<HostedDatabaseSnapshot> databases;
  std::vector<std::shared_ptr<DatabaseOwnershipLock>> database_ownership_locks;
  // Declared after the ownership locks so runtime resources are destroyed
  // first, while database ownership is still held.
  std::vector<std::shared_ptr<HostedDatabaseRuntime>> database_runtimes;
};

struct HostedEngineResult {
  HostedEngineState state;
  std::vector<ServerDiagnostic> diagnostics;
  // Exact private recovery failure, including binary identities, occurrence,
  // native cause and canonical metadata. The public server summary is not a
  // replacement for this source record.
  std::optional<engine::internal_api::EngineApiDiagnostic> descriptor_recovery_failure;

  bool ok() const { return diagnostics.empty(); }
};

const char* HostedDatabaseStateName(HostedDatabaseState state);
class ServerProcessCryptoOwner;

// SEARCH_KEY: SERVER_NATIVE_HOSTED_RUNTIME_INTEGRATION
// `crypto` is optional (decision 1a): the process crypto owner lives in main()
// and only on POSIX, and this entry point is platform-neutral with many
// callers, so it cannot be required. Supplying it opts this open into the
// native hosted runtime, which is what makes engine-owned startup verification
// and an admitted service session reachable. Omitting it -- which every
// existing caller does, and which the embedded client must keep doing -- opens
// exactly as before. A native open that refuses is recorded and does not fail
// the server: it degrades to this path's own result.
HostedEngineResult StartHostedEngine(const ServerBootstrapConfig& config,
                                    void (*before_failure_cleanup)() noexcept = nullptr,
                                    ServerProcessCryptoOwner* crypto = nullptr);
std::string HostedEngineStatusJson(const HostedEngineState& state);
std::shared_ptr<const HostedDatabaseRuntime> FindHostedDatabaseRuntime(
    const HostedEngineState& state,
    const scratchbird::core::platform::Uuid& database_uuid,
    std::string_view canonical_database_path = {});

}  // namespace scratchbird::server
