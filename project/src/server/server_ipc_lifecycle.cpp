// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_IPC_LIFECYCLE_CLOSURE

#include "server_ipc_lifecycle.hpp"

#include "sbps.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace scratchbird::server {

namespace {

ServerDiagnostic IpcLifecycleDiagnostic(std::string code,
                                        std::string message,
                                        std::vector<ServerDiagnosticField> fields = {}) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(message),
                          std::move(fields)};
}

bool RequiresAuthentication(ServerIpcEndpointOperation operation) {
  return operation == ServerIpcEndpointOperation::kAuthenticatedRequest ||
         operation == ServerIpcEndpointOperation::kManagementRequest ||
         operation == ServerIpcEndpointOperation::kEventRequest;
}

bool RequiresAuthorization(ServerIpcEndpointOperation operation) {
  return operation == ServerIpcEndpointOperation::kManagementRequest;
}

bool RequiresSession(ServerIpcEndpointOperation operation) {
  return operation == ServerIpcEndpointOperation::kManagementRequest ||
         operation == ServerIpcEndpointOperation::kEventRequest;
}

bool AllowsDuringDrain(ServerIpcEndpointOperation operation) {
  return operation == ServerIpcEndpointOperation::kDrain ||
         operation == ServerIpcEndpointOperation::kShutdown;
}

bool AllowsUnauthenticated(ServerIpcEndpointOperation operation) {
  return operation == ServerIpcEndpointOperation::kConnect ||
         operation == ServerIpcEndpointOperation::kParserHello ||
         operation == ServerIpcEndpointOperation::kShutdown;
}

std::string FirstOpenDatabaseUuid(const HostedEngineState& engine_state) {
  for (const auto& database : engine_state.databases) {
    if (database.database_open && !database.database_uuid.empty()) return database.database_uuid;
  }
  return {};
}

std::string FirstOpenDatabasePath(const HostedEngineState& engine_state) {
  for (const auto& database : engine_state.databases) {
    if (database.database_open && !database.database_path.empty()) return database.database_path;
  }
  return {};
}

bool HasFailedHostedDatabase(const HostedEngineState& engine_state) {
  return std::any_of(engine_state.databases.begin(),
                     engine_state.databases.end(),
                     [](const HostedDatabaseSnapshot& database) {
                       return database.state == HostedDatabaseState::kFailed ||
                              database.state == HostedDatabaseState::kQuarantined;
                     });
}

const HostedDatabaseSnapshot* FirstOpenDatabase(const HostedEngineState& engine_state) {
  for (const auto& database : engine_state.databases) {
    if (database.database_open) return &database;
  }
  return nullptr;
}

bool IsSupportedServerArtifactMigrationPlan(const ServerLifecycleArtifactMigrationRequest& request) {
  if (request.migration_plan_id.empty()) {
    return false;
  }
  const std::string expected = request.artifact_kind + "_v" +
      std::to_string(request.format_version) + "_to_v" +
      std::to_string(request.current_version) + "_explicit_plan_v1";
  return request.migration_plan_id == expected;
}

}  // namespace

const char* ServerIpcEndpointClassName(ServerIpcEndpointClass endpoint_class) {
  switch (endpoint_class) {
    case ServerIpcEndpointClass::kParserServer:
      return "parser_server";
    case ServerIpcEndpointClass::kServerManagement:
      return "server_management";
    case ServerIpcEndpointClass::kListenerManagement:
      return "listener_management";
    case ServerIpcEndpointClass::kManagerInternal:
      return "manager_internal";
    case ServerIpcEndpointClass::kEventInternal:
      return "event_internal";
  }
  return "unknown";
}

const char* ServerIpcEndpointOperationName(ServerIpcEndpointOperation operation) {
  switch (operation) {
    case ServerIpcEndpointOperation::kConnect:
      return "connect";
    case ServerIpcEndpointOperation::kParserHello:
      return "parser_hello";
    case ServerIpcEndpointOperation::kAuthenticatedRequest:
      return "authenticated_request";
    case ServerIpcEndpointOperation::kManagementRequest:
      return "management_request";
    case ServerIpcEndpointOperation::kEventRequest:
      return "event_request";
    case ServerIpcEndpointOperation::kDrain:
      return "drain";
    case ServerIpcEndpointOperation::kShutdown:
      return "shutdown";
  }
  return "unknown";
}

const char* ServerLifecycleArtifactCompatibilityClassName(
    ServerLifecycleArtifactCompatibilityClass compatibility_class) {
  switch (compatibility_class) {
    case ServerLifecycleArtifactCompatibilityClass::kSupportedCurrent:
      return "supported-current";
    case ServerLifecycleArtifactCompatibilityClass::kSupportedMigration:
      return "supported-migration";
    case ServerLifecycleArtifactCompatibilityClass::kUnsupportedOld:
      return "unsupported-old";
    case ServerLifecycleArtifactCompatibilityClass::kUnsupportedNew:
      return "unsupported-new";
    case ServerLifecycleArtifactCompatibilityClass::kDowngradeRefused:
      return "downgrade-refused";
    case ServerLifecycleArtifactCompatibilityClass::kNewerThanSupportedRefused:
      return "newer-than-supported-refused";
    case ServerLifecycleArtifactCompatibilityClass::kAmbiguousIdentityRefused:
      return "ambiguous-identity-refused";
    case ServerLifecycleArtifactCompatibilityClass::kMissingMigrationPlanRefused:
      return "missing-migration-plan-refused";
    case ServerLifecycleArtifactCompatibilityClass::kMigrationRequiredWithoutPlanRefused:
      return "migration-required-without-plan-refused";
  }
  return "unsupported-new";
}

ServerLifecycleArtifactMigrationEvaluation EvaluateServerLifecycleArtifactMigration(
    const ServerLifecycleArtifactMigrationRequest& request) {
  auto failure = [&](ServerLifecycleArtifactCompatibilityClass compatibility_class,
                     std::string code,
                     std::string message) {
    ServerLifecycleArtifactMigrationEvaluation result;
    result.accepted = false;
    result.migration_required =
        compatibility_class ==
            ServerLifecycleArtifactCompatibilityClass::kMissingMigrationPlanRefused ||
        compatibility_class ==
            ServerLifecycleArtifactCompatibilityClass::kMigrationRequiredWithoutPlanRefused;
    result.compatibility_class = compatibility_class;
    result.diagnostic = IpcLifecycleDiagnostic(
        std::move(code),
        std::move(message),
        {{"artifact_kind", request.artifact_kind},
         {"format_version", std::to_string(request.format_version)},
         {"supported_min", std::to_string(request.min_supported_version)},
         {"current", std::to_string(request.current_version)},
         {"supported_max", std::to_string(request.max_supported_version)}});
    return result;
  };

  if (!request.identity_proven) {
    return failure(ServerLifecycleArtifactCompatibilityClass::kAmbiguousIdentityRefused,
                   "IPC.LIFECYCLE.AMBIGUOUS_IDENTITY_REFUSED",
                   "The lifecycle artifact identity evidence is ambiguous.");
  }
  if (request.downgrade_requested) {
    return failure(ServerLifecycleArtifactCompatibilityClass::kDowngradeRefused,
                   "IPC.LIFECYCLE.DOWNGRADE_REFUSED",
                   "The lifecycle artifact would require an unsafe downgrade.");
  }
  if (request.migration_plan_required && request.migration_plan_id.empty()) {
    return failure(ServerLifecycleArtifactCompatibilityClass::kMissingMigrationPlanRefused,
                   "IPC.LIFECYCLE.MIGRATION_PLAN_MISSING",
                   "The lifecycle artifact migration requires an explicit plan.");
  }
  if (request.format_version < request.min_supported_version) {
    return failure(ServerLifecycleArtifactCompatibilityClass::kUnsupportedOld,
                   "IPC.LIFECYCLE.VERSION_TOO_OLD_UNSUPPORTED",
                   "The lifecycle artifact format version is too old to migrate safely.");
  }
  if (request.format_version > request.max_supported_version) {
    return failure(ServerLifecycleArtifactCompatibilityClass::kNewerThanSupportedRefused,
                   "IPC.LIFECYCLE.VERSION_NEWER_THAN_SUPPORTED",
                   "The lifecycle artifact format version is newer than this server supports.");
  }
  if (request.format_version == request.current_version) {
    ServerLifecycleArtifactMigrationEvaluation result;
    result.accepted = true;
    result.compatibility_class =
        ServerLifecycleArtifactCompatibilityClass::kSupportedCurrent;
    return result;
  }
  if (request.format_version > request.current_version) {
    return failure(ServerLifecycleArtifactCompatibilityClass::kUnsupportedNew,
                   "IPC.LIFECYCLE.VERSION_UNSUPPORTED_NEW",
                   "The lifecycle artifact format version is unsupported.");
  }
  if (request.migration_plan_id.empty()) {
    return failure(ServerLifecycleArtifactCompatibilityClass::kMigrationRequiredWithoutPlanRefused,
                   "IPC.LIFECYCLE.MIGRATION_REQUIRED_WITHOUT_PLAN",
                   "The lifecycle artifact requires a migration plan and must fail closed.");
  }
  if (!IsSupportedServerArtifactMigrationPlan(request)) {
    return failure(ServerLifecycleArtifactCompatibilityClass::kMissingMigrationPlanRefused,
                   "IPC.LIFECYCLE.MIGRATION_PLAN_MISSING",
                   "The lifecycle artifact migration plan is not supported.");
  }

  ServerLifecycleArtifactMigrationEvaluation result;
  result.accepted = true;
  result.migration_required = true;
  result.compatibility_class =
      ServerLifecycleArtifactCompatibilityClass::kSupportedMigration;
  return result;
}

std::filesystem::path ParserServerEndpointDescriptorPath(const ServerBootstrapConfig& config) {
  return config.control_dir / "sb_server.sbps.endpoint";
}

ServerIpcEndpointDescriptor BuildParserServerEndpointDescriptor(
    const ServerBootstrapConfig& config,
    const ServerLifecycleArtifacts& artifacts,
    const HostedEngineState& engine_state) {
  ServerIpcEndpointDescriptor descriptor;
  descriptor.endpoint_class = ServerIpcEndpointClass::kParserServer;
  descriptor.protocol_family = "parser_server_ipc";
  descriptor.transport = "af_unix";
  descriptor.endpoint_path = config.sbps_endpoint;
  descriptor.descriptor_path = ParserServerEndpointDescriptorPath(config);
  descriptor.descriptor_format_version = kServerIpcEndpointDescriptorFormatCurrent;
  descriptor.protocol_major = sbps::kProtocolMajor;
  descriptor.protocol_minor = sbps::kProtocolMinor;
  descriptor.database_uuid = FirstOpenDatabaseUuid(engine_state);
  descriptor.database_path = FirstOpenDatabasePath(engine_state);
  descriptor.lifecycle_generation = artifacts.generation;
  descriptor.descriptor_generation = artifacts.generation;
  descriptor.config_source_epoch = config.config_source_epoch;
  descriptor.current_config_source_epoch = config.config_source_epoch;
  descriptor.config_reload_generation = config.config_reload_generation;
  descriptor.current_config_reload_generation = config.config_reload_generation;
  descriptor.capability_policy_generation = config.capability_policy_generation;
  descriptor.current_capability_policy_generation = config.capability_policy_generation;
  descriptor.policy_generation = config.security_policy_generation;
  descriptor.current_policy_generation = config.security_policy_generation;
  descriptor.security_epoch = config.security_epoch;
  descriptor.current_security_epoch = config.security_epoch;
  if (const auto* database = FirstOpenDatabase(engine_state)) {
    descriptor.config_source_epoch = database->config_source_epoch;
    descriptor.current_config_source_epoch = database->config_source_epoch;
    descriptor.config_reload_generation = database->config_reload_generation;
    descriptor.current_config_reload_generation = database->config_reload_generation;
    descriptor.capability_policy_generation = database->capability_policy_generation;
    descriptor.current_capability_policy_generation = database->capability_policy_generation;
    descriptor.policy_generation = database->policy_generation;
    descriptor.current_policy_generation = database->policy_generation;
    descriptor.security_epoch = database->security_epoch;
    descriptor.current_security_epoch = database->security_epoch;
    descriptor.resource_epoch = std::max<std::uint64_t>(1, database->cache_invalidation_epoch);
    descriptor.current_resource_epoch = descriptor.resource_epoch;
    descriptor.cache_invalidation_epoch = database->cache_invalidation_epoch;
  } else {
    descriptor.cache_invalidation_epoch = config.cache_invalidation_epoch;
  }
  descriptor.file_mode = 0600;
  descriptor.max_frame_bytes = config.sbps_max_frame_bytes;
  descriptor.max_streams = config.sbps_max_streams;
  descriptor.max_active_channels = std::max<std::uint64_t>(1, config.sbps_max_streams);
  descriptor.max_queued_frames = std::max<std::uint64_t>(1, config.sbps_max_streams * 2);
  descriptor.max_queued_bytes = config.sbps_max_frame_bytes * std::max<std::uint64_t>(1, config.sbps_max_streams);
  descriptor.service_ready = engine_state.engine_context_active && !HasFailedHostedDatabase(engine_state);
  descriptor.quarantine_requested = HasFailedHostedDatabase(engine_state);
  descriptor.endpoint_id = "parser_server_ipc:" + descriptor.endpoint_path.string();
  return descriptor;
}

std::string ServerIpcEndpointDescriptorText(const ServerIpcEndpointDescriptor& descriptor) {
  std::ostringstream out;
  out << "format=SBPS_ENDPOINT_V1\n";
  out << "descriptor_format_version=" << descriptor.descriptor_format_version << "\n";
  out << "endpoint_class=" << ServerIpcEndpointClassName(descriptor.endpoint_class) << "\n";
  out << "endpoint_id=" << descriptor.endpoint_id << "\n";
  out << "protocol_family=" << descriptor.protocol_family << "\n";
  out << "transport=" << descriptor.transport << "\n";
  out << "endpoint=" << descriptor.endpoint_path.string() << "\n";
  out << "database_uuid=" << descriptor.database_uuid << "\n";
  out << "database_path=" << descriptor.database_path << "\n";
  out << "protocol_major=" << descriptor.protocol_major << "\n";
  out << "protocol_minor=" << descriptor.protocol_minor << "\n";
  out << "protocol_supported_min=" << sbps::kProtocolMajorMinSupported << "."
      << sbps::kProtocolMinorMinSupported << "\n";
  out << "protocol_supported_max=" << sbps::kProtocolMajorMaxSupported << "."
      << sbps::kProtocolMinorMaxSupported << "\n";
  out << "config_source_epoch=" << descriptor.config_source_epoch << "\n";
  out << "config_reload_generation=" << descriptor.config_reload_generation << "\n";
  out << "capability_policy_generation=" << descriptor.capability_policy_generation << "\n";
  out << "policy_generation=" << descriptor.policy_generation << "\n";
  out << "security_epoch=" << descriptor.security_epoch << "\n";
  out << "resource_epoch=" << descriptor.resource_epoch << "\n";
  out << "cache_invalidation_epoch=" << descriptor.cache_invalidation_epoch << "\n";
  out << "max_frame_bytes=" << descriptor.max_frame_bytes << "\n";
  out << "max_streams=" << descriptor.max_streams << "\n";
  out << "lifecycle_generation=" << descriptor.lifecycle_generation << "\n";
  out << "descriptor_generation=" << descriptor.descriptor_generation << "\n";
  out << "file_mode_octal=" << std::oct << descriptor.file_mode << std::dec << "\n";
  out << "service_ready=" << (descriptor.service_ready ? "true" : "false") << "\n";
  out << "cluster_private=" << (descriptor.cluster_private ? "true" : "false") << "\n";
  return out.str();
}

bool WriteServerIpcEndpointDescriptor(const ServerIpcEndpointDescriptor& descriptor,
                                      std::vector<ServerDiagnostic>* diagnostics) {
  std::error_code ec;
  std::filesystem::create_directories(descriptor.descriptor_path.parent_path(), ec);
  if (ec) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(IpcLifecycleDiagnostic(
          "PARSER_SERVER_IPC.ENDPOINT_DESCRIPTOR_WRITE_FAILED",
          "The parser-server endpoint descriptor directory could not be created.",
          {{"endpoint_descriptor", descriptor.descriptor_path.string()}}));
    }
    return false;
  }

  std::ofstream out(descriptor.descriptor_path, std::ios::trunc);
  if (!out) {
    if (diagnostics != nullptr) {
      diagnostics->push_back(IpcLifecycleDiagnostic(
          "PARSER_SERVER_IPC.ENDPOINT_DESCRIPTOR_WRITE_FAILED",
          "The parser-server endpoint descriptor could not be written.",
          {{"endpoint_descriptor", descriptor.descriptor_path.string()}}));
    }
    return false;
  }
  out << ServerIpcEndpointDescriptorText(descriptor);
  out.close();
#ifndef _WIN32
  ::chmod(descriptor.descriptor_path.c_str(), descriptor.file_mode);
#endif
  return true;
}

ServerIpcEndpointLifecycleEvaluation EvaluateServerIpcEndpointLifecycle(
    const ServerIpcEndpointDescriptor& descriptor,
    ServerIpcEndpointOperation operation) {
  ServerIpcEndpointLifecycleEvaluation result;

  auto fail = [&](std::string code, std::string message) {
    result.diagnostics.push_back(IpcLifecycleDiagnostic(
        std::move(code),
        std::move(message),
        {{"endpoint_class", ServerIpcEndpointClassName(descriptor.endpoint_class)},
         {"operation", ServerIpcEndpointOperationName(operation)}}));
  };

  if (!descriptor.descriptor_present) {
    fail("IPC.LIFECYCLE.DESCRIPTOR_MISSING",
         "The IPC endpoint descriptor is missing.");
    result.stale_cleanup_required = true;
  }
  ServerLifecycleArtifactMigrationRequest descriptor_format;
  descriptor_format.artifact_kind = "ipc_endpoint_descriptor";
  descriptor_format.format_version = descriptor.descriptor_format_version;
  descriptor_format.min_supported_version = kServerIpcEndpointDescriptorFormatMinSupported;
  descriptor_format.current_version = kServerIpcEndpointDescriptorFormatCurrent;
  descriptor_format.max_supported_version = kServerIpcEndpointDescriptorFormatMaxSupported;
  descriptor_format.identity_proven =
      descriptor.descriptor_owner_matches && !descriptor.endpoint_id.empty();
  const auto descriptor_migration =
      EvaluateServerLifecycleArtifactMigration(descriptor_format);
  if (!descriptor_migration.accepted) {
    result.diagnostics.push_back(descriptor_migration.diagnostic);
    if (descriptor_migration.compatibility_class ==
            ServerLifecycleArtifactCompatibilityClass::kAmbiguousIdentityRefused ||
        descriptor_migration.compatibility_class ==
            ServerLifecycleArtifactCompatibilityClass::kUnsupportedOld ||
        descriptor_migration.compatibility_class ==
            ServerLifecycleArtifactCompatibilityClass::kMigrationRequiredWithoutPlanRefused ||
        descriptor_migration.compatibility_class ==
            ServerLifecycleArtifactCompatibilityClass::kMissingMigrationPlanRefused) {
      result.stale_cleanup_required = true;
    }
  }
  if (descriptor.descriptor_format_version < kServerIpcEndpointDescriptorFormatMinSupported) {
    fail("IPC.LIFECYCLE.DESCRIPTOR_VERSION_TOO_OLD",
         "The IPC endpoint descriptor format version is too old.");
  }
  if (descriptor.descriptor_format_version > kServerIpcEndpointDescriptorFormatMaxSupported) {
    fail("IPC.LIFECYCLE.DESCRIPTOR_VERSION_FUTURE",
         "The IPC endpoint descriptor format version is newer than this server supports.");
  }
  if (descriptor.protocol_major < sbps::kProtocolMajorMinSupported ||
      (descriptor.protocol_major == sbps::kProtocolMajor &&
       descriptor.protocol_minor < sbps::kProtocolMinorMinSupported)) {
    fail("IPC.LIFECYCLE.PROTOCOL_VERSION_TOO_OLD",
         "The IPC endpoint protocol version is too old.");
  }
  if (descriptor.protocol_major > sbps::kProtocolMajorMaxSupported ||
      (descriptor.protocol_major == sbps::kProtocolMajor &&
       descriptor.protocol_minor > sbps::kProtocolMinorMaxSupported)) {
    fail("IPC.LIFECYCLE.PROTOCOL_VERSION_FUTURE",
         "The IPC endpoint protocol version is newer than this server supports.");
  }
  if (descriptor.protocol_major != sbps::kProtocolMajor) {
    fail("IPC.LIFECYCLE.PROTOCOL_VERSION_UNSUPPORTED",
         "The IPC endpoint protocol major version is unsupported.");
  }
  if (descriptor.endpoint_id.empty() || descriptor.protocol_family.empty() ||
      descriptor.transport.empty() || descriptor.endpoint_path.empty()) {
    fail("IPC.LIFECYCLE.DESCRIPTOR_INCOMPLETE",
         "The IPC endpoint descriptor is incomplete.");
  }
  if (!descriptor.descriptor_owner_matches) {
    fail("IPC.LIFECYCLE.DESCRIPTOR_OWNER_MISMATCH",
         "The IPC endpoint descriptor owner does not match the active lifecycle owner.");
    result.stale_cleanup_required = true;
  }
  if (descriptor.lifecycle_generation == 0 ||
      descriptor.descriptor_generation != descriptor.lifecycle_generation ||
      descriptor.stale) {
    fail("IPC.LIFECYCLE.DESCRIPTOR_STALE",
         "The IPC endpoint descriptor generation is stale.");
    result.stale_cleanup_required = true;
  }
  const auto required_cache_epoch =
      std::max({descriptor.config_source_epoch,
                descriptor.config_reload_generation,
                descriptor.capability_policy_generation,
                descriptor.policy_generation,
                descriptor.security_epoch,
                descriptor.resource_epoch});
  if (descriptor.config_source_epoch == 0 || descriptor.config_reload_generation == 0 ||
      descriptor.capability_policy_generation == 0 || descriptor.policy_generation == 0 ||
      descriptor.security_epoch == 0 || descriptor.resource_epoch == 0 ||
      descriptor.cache_invalidation_epoch == 0) {
    fail("IPC.LIFECYCLE.EPOCH_DESCRIPTOR_MALFORMED",
         "The IPC endpoint descriptor contains a malformed epoch descriptor.");
  } else if (descriptor.cache_invalidation_epoch < required_cache_epoch ||
             descriptor.config_source_epoch != descriptor.current_config_source_epoch ||
             descriptor.config_reload_generation != descriptor.current_config_reload_generation ||
             descriptor.capability_policy_generation != descriptor.current_capability_policy_generation ||
             descriptor.policy_generation != descriptor.current_policy_generation ||
             descriptor.security_epoch != descriptor.current_security_epoch ||
             descriptor.resource_epoch != descriptor.current_resource_epoch) {
    fail("IPC.LIFECYCLE.EPOCH_STALE",
         "The IPC endpoint descriptor was produced against a stale config, policy, security, or resource epoch.");
    result.stale_cleanup_required = true;
  }
  if ((descriptor.file_mode & 0777u) != 0600u) {
    fail("IPC.LIFECYCLE.PERMISSION_INVALID",
         "The IPC endpoint descriptor or socket has unsafe permissions.");
  }
  if (!descriptor.bound) {
    fail("IPC.LIFECYCLE.ENDPOINT_UNBOUND",
         "The IPC endpoint is not bound.");
  }
  if (!descriptor.service_ready && operation != ServerIpcEndpointOperation::kShutdown) {
    fail("IPC.LIFECYCLE.SERVICE_NOT_READY",
         "The IPC endpoint is not service-ready.");
  }
  if (descriptor.cluster_private && !descriptor.cluster_authority_available) {
    fail("IPC.LIFECYCLE.CLUSTER_AUTHORITY_REQUIRED",
         "The IPC endpoint is private to cluster authority and must fail closed.");
  }
  if (!descriptor.frame_valid) {
    fail("IPC.LIFECYCLE.FRAME_INVALID",
         "The IPC frame failed physical validation.");
  }
  if (!descriptor.frame_schema_valid) {
    fail("IPC.LIFECYCLE.FRAME_SCHEMA_INVALID",
         "The IPC frame failed schema validation.");
  }
  if (descriptor.quarantine_requested || descriptor.failed) {
    fail("IPC.LIFECYCLE.QUARANTINE_REQUIRED",
         "The IPC endpoint must be quarantined after endpoint failure.");
    result.quarantine_required = true;
  }
  if (descriptor.shutting_down) {
    result.shutdown_required = true;
    if (operation != ServerIpcEndpointOperation::kShutdown) {
      fail("IPC.LIFECYCLE.SHUTTING_DOWN",
           "The IPC endpoint is shutting down and refuses new work.");
    }
  }
  if (descriptor.draining && !AllowsDuringDrain(operation)) {
    result.drain_required = true;
    fail("IPC.LIFECYCLE.DRAINING",
         "The IPC endpoint is draining and refuses new work.");
  }
  if ((descriptor.max_active_channels != 0 &&
       descriptor.active_channels >= descriptor.max_active_channels) ||
      (descriptor.max_queued_frames != 0 &&
       descriptor.queued_frames >= descriptor.max_queued_frames) ||
      (descriptor.max_queued_bytes != 0 &&
       descriptor.queued_bytes >= descriptor.max_queued_bytes)) {
    result.backpressure_required = true;
    if (!AllowsDuringDrain(operation)) {
      fail("IPC.LIFECYCLE.BACKPRESSURE",
           "The IPC endpoint is applying backpressure and refuses new work.");
    }
  }
  if (RequiresAuthentication(operation) && !descriptor.authenticated &&
      !AllowsUnauthenticated(operation)) {
    fail("IPC.LIFECYCLE.AUTHENTICATION_REQUIRED",
         "The IPC endpoint requires engine-authenticated session context.");
  }
  if (RequiresAuthorization(operation) && !descriptor.authorized) {
    fail("IPC.LIFECYCLE.AUTHORIZATION_REQUIRED",
         "The IPC endpoint requires engine-authorized management rights.");
  }
  if (RequiresSession(operation) && !descriptor.session_bound) {
    fail("IPC.LIFECYCLE.SESSION_REQUIRED",
         "The IPC endpoint requires a bound engine session.");
  }

  result.descriptor_valid = descriptor.descriptor_present &&
                            !descriptor.endpoint_id.empty() &&
                            !descriptor.protocol_family.empty() &&
                            !descriptor.transport.empty() &&
                            !descriptor.endpoint_path.empty() &&
                            descriptor.descriptor_format_version >= kServerIpcEndpointDescriptorFormatMinSupported &&
                            descriptor.descriptor_format_version <= kServerIpcEndpointDescriptorFormatMaxSupported &&
                            descriptor.protocol_major == sbps::kProtocolMajor &&
                            descriptor.protocol_minor >= sbps::kProtocolMinorMinSupported &&
                            descriptor.protocol_minor <= sbps::kProtocolMinorMaxSupported &&
                            descriptor.descriptor_owner_matches &&
                            descriptor.lifecycle_generation != 0 &&
                            descriptor.descriptor_generation == descriptor.lifecycle_generation &&
                            (descriptor.file_mode & 0777u) == 0600u &&
                            descriptor.bound &&
                            !descriptor.stale;
  result.admitted = result.diagnostics.empty();
  return result;
}

}  // namespace scratchbird::server
