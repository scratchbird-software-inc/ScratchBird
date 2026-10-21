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
HostedEngineResult StartHostedEngine(const ServerBootstrapConfig& config,
                                    void (*before_failure_cleanup)() noexcept = nullptr);
std::string HostedEngineStatusJson(const HostedEngineState& state);
std::shared_ptr<const HostedDatabaseRuntime> FindHostedDatabaseRuntime(
    const HostedEngineState& state,
    const scratchbird::core::platform::Uuid& database_uuid,
    std::string_view canonical_database_path = {});

}  // namespace scratchbird::server
