// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compatibility_server_authority.hpp"

#include <array>

namespace scratchbird::server {
namespace {

struct SurfaceRule {
  std::string_view surface_key;
  std::string_view compatibility_engine;
  CompatibilityServerAuthorityAction action;
  std::string_view diagnostic_code;
};

constexpr std::string_view kSblrAction =
    "none_for_external_authority; admitted external DML/DDL lowers separately when allowed";
constexpr std::string_view kMgaRule =
    "no external finality; server route preserves ScratchBird MGA transaction authority";

constexpr std::array<std::string_view, 24> kKnownCompatibilityEngines{{
    "apache_ignite",
    "cassandra",
    "clickhouse",
    "cockroachdb",
    "dolt",
    "duckdb",
    "firebird",
    "foundationdb",
    "immudb",
    "influxdb",
    "mariadb",
    "milvus",
    "mongodb",
    "mysql",
    "neo4j",
    "opensearch",
    "postgresql",
    "redis",
    "sqlite",
    "tidb",
    "tikv",
    "vitess",
    "xtdb",
    "yugabytedb",
}};

constexpr std::array<SurfaceRule, 42> kSurfaceRules{{
    {"database_create", "", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.DATABASE_AUTHORITY_DENIED"},
    {"database_alter", "", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.DATABASE_AUTHORITY_DENIED"},
    {"database_drop", "", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.DATABASE_AUTHORITY_DENIED"},
    {"tablespace_storage", "", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"extension_install", "", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"external_file_read", "", CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_IO_DENIED"},
    {"external_file_write", "", CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_IO_DENIED"},
    {"os_command", "", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.OS_COMMAND_DENIED"},
    {"physical_backup_restore", "", CompatibilityServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.PHYSICAL_OPERATION_ROUTED"},
    {"replication_endpoint", "", CompatibilityServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"cluster_topology", "", CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.CLUSTER_TOPOLOGY_DENIED"},
    {"privileged_catalog_write", "", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.CATALOG_WRITE_DENIED"},
    {"cassandra_keyspace_create", "cassandra",
     CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.DATABASE_AUTHORITY_DENIED"},
    {"cassandra_udf", "cassandra", CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"cassandra_cdc", "cassandra", CompatibilityServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"duckdb_attach_file", "duckdb", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"duckdb_extension_install", "duckdb",
     CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"duckdb_copy_external", "duckdb", CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_IO_DENIED"},
    {"firebird_services_backup", "firebird",
     CompatibilityServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.PHYSICAL_OPERATION_ROUTED"},
    {"firebird_database_file_attach", "firebird",
     CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"firebird_udf_module", "firebird", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"mariadb_plugin_install", "mariadb",
     CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"mariadb_replication_control", "mariadb",
     CompatibilityServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"mariadb_sequence_engine", "mariadb",
     CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"mongodb_admin_commands", "mongodb",
     CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.CLUSTER_TOPOLOGY_DENIED"},
    {"mongodb_change_stream", "mongodb",
     CompatibilityServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"mysql_load_data_file", "mysql", CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_IO_DENIED"},
    {"mysql_plugin_install", "mysql", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"mysql_replication_control", "mysql",
     CompatibilityServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"opensearch_cluster_settings", "opensearch",
     CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.CLUSTER_TOPOLOGY_DENIED"},
    {"opensearch_plugin", "opensearch", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"opensearch_snapshot", "opensearch",
     CompatibilityServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.PHYSICAL_OPERATION_ROUTED"},
    {"pg_copy_program", "postgresql", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.OS_COMMAND_DENIED"},
    {"pg_extension_ddl", "postgresql", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"pg_tablespace_ddl", "postgresql", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"pg_fdw_server", "postgresql", CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_ENDPOINT_DENIED"},
    {"redis_config_rewrite", "redis", CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"redis_module", "redis", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"redis_replication", "redis", CompatibilityServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"sqlite_attach_file", "sqlite", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"sqlite_load_extension", "sqlite", CompatibilityServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"sqlite_pragma_files", "sqlite", CompatibilityServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
}};

std::string_view ServerActionName(CompatibilityServerAuthorityAction action) {
  switch (action) {
    case CompatibilityServerAuthorityAction::kSecurityDenial: return "security_denial";
    case CompatibilityServerAuthorityAction::kServerPolicyGate: return "server_policy_gate";
    case CompatibilityServerAuthorityAction::kMigrationServiceRoute:
      return "migration_service_route";
  }
  return "security_denial";
}

std::string_view Execution_PlanLane(CompatibilityServerAuthorityAction action) {
  switch (action) {
    case CompatibilityServerAuthorityAction::kSecurityDenial:
      return "security_denial_and_exact_refusal";
    case CompatibilityServerAuthorityAction::kServerPolicyGate:
      return "server_policy_gate_and_connector_authorization";
    case CompatibilityServerAuthorityAction::kMigrationServiceRoute:
      return "migration_service_route_and_live_migration_protocol";
  }
  return "security_denial_and_exact_refusal";
}

std::string_view RouteContractId(CompatibilityServerAuthorityAction action) {
  switch (action) {
    case CompatibilityServerAuthorityAction::kSecurityDenial:
      return "server_authority.security_denial.exact_refusal.v1";
    case CompatibilityServerAuthorityAction::kServerPolicyGate:
      return "server_authority.policy_gate.connector_authorization.v1";
    case CompatibilityServerAuthorityAction::kMigrationServiceRoute:
      return "server_authority.migration_service.route_contract.v1";
  }
  return "server_authority.security_denial.exact_refusal.v1";
}

bool CompatibilityAllowedForRule(std::string_view compatibility_engine,
                         const SurfaceRule& rule) {
  return rule.compatibility_engine.empty() || rule.compatibility_engine == compatibility_engine;
}

const SurfaceRule* FindSurfaceRule(std::string_view engine_id,
                                   std::string_view surface_key) {
  if (!IsKnownCompatibilityEngineForServerAuthority(engine_id)) return nullptr;
  for (const auto& rule : kSurfaceRules) {
    if (rule.surface_key == surface_key && CompatibilityAllowedForRule(engine_id, rule)) {
      return &rule;
    }
  }
  return nullptr;
}

CompatibilityServerAuthorityDecision DecisionForRule(const SurfaceRule& rule) {
  const bool is_security_denial =
      rule.action == CompatibilityServerAuthorityAction::kSecurityDenial;
  const bool is_policy_gate =
      rule.action == CompatibilityServerAuthorityAction::kServerPolicyGate;
  const bool is_migration =
      rule.action == CompatibilityServerAuthorityAction::kMigrationServiceRoute;
  return CompatibilityServerAuthorityDecision{
      rule.surface_key,
      rule.action,
      ServerActionName(rule.action),
      rule.diagnostic_code,
      RouteContractId(rule.action),
      Execution_PlanLane(rule.action),
      kSblrAction,
      kMgaRule,
      true,
      false,
      true,
      false,
      is_security_denial,
      is_policy_gate,
      is_migration,
  };
}

void AddEvidence(CompatibilityServerAuthorityRouteResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.emplace_back(std::move(key), std::move(value));
}

void AddEvidence(CompatibilityMigrationRouteResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.emplace_back(std::move(key), std::move(value));
}

std::optional<CompatibilityMigrationRouteKind> MigrationKindForDiagnostic(
    std::string_view diagnostic_code) {
  if (diagnostic_code == "SB.MIGRATION.PHYSICAL_OPERATION_ROUTED") {
    return CompatibilityMigrationRouteKind::kPhysicalOperation;
  }
  if (diagnostic_code == "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED") {
    return CompatibilityMigrationRouteKind::kLiveReplicationEndpoint;
  }
  return std::nullopt;
}

std::string_view MigrationRouteContractId(CompatibilityMigrationRouteKind kind) {
  switch (kind) {
    case CompatibilityMigrationRouteKind::kPhysicalOperation:
      return "server_authority.migration.physical_operation.route_contract.v1";
    case CompatibilityMigrationRouteKind::kLiveReplicationEndpoint:
      return "server_authority.migration.live_replication.route_contract.v1";
  }
  return "server_authority.migration.physical_operation.route_contract.v1";
}

std::string_view MigrationUnavailableDiagnostic(CompatibilityMigrationRouteKind kind) {
  switch (kind) {
    case CompatibilityMigrationRouteKind::kPhysicalOperation:
      return "SB.MIGRATION.PHYSICAL_SERVICE_UNAVAILABLE";
    case CompatibilityMigrationRouteKind::kLiveReplicationEndpoint:
      return "SB.MIGRATION.REPLICATION_SERVICE_UNAVAILABLE";
  }
  return "SB.MIGRATION.PHYSICAL_SERVICE_UNAVAILABLE";
}

std::string_view MigrationCheckpointDescriptor(CompatibilityMigrationRouteKind kind) {
  switch (kind) {
    case CompatibilityMigrationRouteKind::kPhysicalOperation:
      return "scratchbird.migration.physical.checkpoint.v1";
    case CompatibilityMigrationRouteKind::kLiveReplicationEndpoint:
      return "scratchbird.migration.live_replication.checkpoint.v1";
  }
  return "scratchbird.migration.physical.checkpoint.v1";
}

std::string_view MigrationResumeToken(CompatibilityMigrationRouteKind kind) {
  switch (kind) {
    case CompatibilityMigrationRouteKind::kPhysicalOperation:
      return "scratchbird.migration.physical.resume_token.v1";
    case CompatibilityMigrationRouteKind::kLiveReplicationEndpoint:
      return "scratchbird.migration.live_replication.resume_token.v1";
  }
  return "scratchbird.migration.physical.resume_token.v1";
}

std::string_view MigrationMechanismClass(CompatibilityMigrationRouteKind kind) {
  switch (kind) {
    case CompatibilityMigrationRouteKind::kPhysicalOperation:
      return "external_physical_backup_restore_snapshot_file_copy";
    case CompatibilityMigrationRouteKind::kLiveReplicationEndpoint:
      return "external_replication_cdc_binlog_oplog_changefeed";
  }
  return "external_physical_backup_restore_snapshot_file_copy";
}

}  // namespace

bool IsKnownCompatibilityEngineForServerAuthority(std::string_view engine_id) {
  for (const auto known_engine : kKnownCompatibilityEngines) {
    if (known_engine == engine_id) return true;
  }
  return false;
}

std::optional<CompatibilityServerAuthorityDecision> ResolveCompatibilityServerAuthoritySurface(
    std::string_view engine_id,
    std::string_view surface_key) {
  const auto* rule = FindSurfaceRule(engine_id, surface_key);
  if (rule == nullptr) return std::nullopt;
  return DecisionForRule(*rule);
}

CompatibilityServerAuthorityRouteResult EvaluateCompatibilityServerAuthorityRoute(
    const CompatibilityServerAuthorityRequest& request) {
  CompatibilityServerAuthorityRouteResult result;
  const auto decision =
      ResolveCompatibilityServerAuthoritySurface(request.engine_id, request.surface_key);
  if (!decision.has_value()) {
    result.recognized = false;
    result.routed = false;
    result.accepted = false;
    result.denied = true;
    result.diagnostic_code = "SB.SERVER_AUTHORITY.UNKNOWN_SURFACE";
    result.route_contract_id = "server_authority.unknown.fail_closed.v1";
    AddEvidence(&result, "server_authority_known", "false");
    AddEvidence(&result, "sblr_execution_attempted", "false");
    AddEvidence(&result, "scratchbird_mga_authority_preserved", "true");
    return result;
  }

  result.recognized = true;
  result.routed = true;
  result.action = decision->action;
  result.policy_gate = decision->requires_connector_authorization;
  result.migration_route = decision->requires_migration_service;
  result.sblr_execution_attempted = false;
  result.sblr_execution_blocked = true;
  result.scratchbird_mga_authority_preserved =
      decision->preserves_scratchbird_mga_authority;
  result.external_finality_accepted = decision->accepts_external_finality;
  result.diagnostic_code = std::string(decision->diagnostic_code);
  result.route_contract_id = std::string(decision->route_contract_id);
  result.accepted =
      result.migration_route && request.migration_service_available;
  result.denied = !result.accepted;

  AddEvidence(&result, "server_authority_known", "true");
  AddEvidence(&result, "engine_id", std::string(request.engine_id));
  AddEvidence(&result, "surface_key", std::string(request.surface_key));
  AddEvidence(&result, "server_action", std::string(decision->server_action));
  AddEvidence(&result, "diagnostic_code", result.diagnostic_code);
  AddEvidence(&result, "sblr_action", std::string(decision->sblr_action));
  AddEvidence(&result, "sblr_execution_surface", "false");
  AddEvidence(&result, "sblr_execution_attempted", "false");
  AddEvidence(&result, "sblr_execution_blocked", "true");
  AddEvidence(&result, "scratchbird_mga_authority_preserved", "true");
  AddEvidence(&result, "external_finality_accepted", "false");
  AddEvidence(&result,
              "materialized_authorization_context",
              request.materialized_authorization_context ? "true" : "false");
  AddEvidence(&result,
              "migration_service_available",
              request.migration_service_available ? "true" : "false");
  return result;
}

std::optional<CompatibilityMigrationRouteContract> ResolveCompatibilityMigrationRouteContract(
    std::string_view engine_id,
    std::string_view surface_key) {
  const auto decision = ResolveCompatibilityServerAuthoritySurface(engine_id, surface_key);
  if (!decision.has_value() ||
      decision->action != CompatibilityServerAuthorityAction::kMigrationServiceRoute) {
    return std::nullopt;
  }
  const auto kind = MigrationKindForDiagnostic(decision->diagnostic_code);
  if (!kind.has_value()) return std::nullopt;
  return CompatibilityMigrationRouteContract{
      decision->surface_key,
      *kind,
      MigrationRouteContractId(*kind),
      decision->diagnostic_code,
      MigrationUnavailableDiagnostic(*kind),
      MigrationCheckpointDescriptor(*kind),
      MigrationResumeToken(*kind),
      MigrationMechanismClass(*kind),
      decision->mga_rule,
      true,
      false,
      false,
      false,
  };
}

CompatibilityMigrationRouteResult EvaluateCompatibilityMigrationRoute(
    const CompatibilityMigrationRouteRequest& request) {
  CompatibilityMigrationRouteResult result;
  const auto contract =
      ResolveCompatibilityMigrationRouteContract(request.engine_id, request.surface_key);
  if (!contract.has_value()) {
    result.recognized = false;
    result.routed = false;
    result.accepted = false;
    result.service_unavailable = true;
    result.diagnostic_code = "SB.MIGRATION.UNKNOWN_ROUTE";
    result.route_contract_id = "server_authority.migration.unknown.fail_closed.v1";
    AddEvidence(&result, "migration_route_known", "false");
    AddEvidence(&result, "sblr_execution_attempted", "false");
    AddEvidence(&result, "external_storage_authority_accepted", "false");
    AddEvidence(&result, "external_finality_accepted", "false");
    AddEvidence(&result, "scratchbird_mga_authority_preserved", "true");
    return result;
  }

  result.recognized = true;
  result.routed = request.migration_service_available;
  result.accepted = request.migration_service_available;
  result.service_unavailable = !request.migration_service_available;
  result.sblr_execution_attempted = false;
  result.scratchbird_mga_authority_preserved =
      contract->scratchbird_mga_authority_preserved;
  result.external_storage_authority_accepted =
      contract->external_storage_authority_accepted;
  result.external_finality_accepted = contract->external_finality_accepted;
  result.route_kind = contract->route_kind;
  result.diagnostic_code = request.migration_service_available
                               ? std::string(contract->route_diagnostic_code)
                               : std::string(contract->unavailable_diagnostic_code);
  result.route_contract_id = std::string(contract->route_contract_id);
  result.checkpoint_descriptor_kind =
      std::string(contract->checkpoint_descriptor_kind);
  result.resume_token_kind = std::string(contract->resume_token_kind);
  AddEvidence(&result, "migration_route_known", "true");
  AddEvidence(&result, "engine_id", std::string(request.engine_id));
  AddEvidence(&result, "surface_key", std::string(request.surface_key));
  AddEvidence(&result, "route_contract_id", result.route_contract_id);
  AddEvidence(&result,
              "route_diagnostic_code",
              std::string(contract->route_diagnostic_code));
  AddEvidence(&result,
              "unavailable_diagnostic_code",
              std::string(contract->unavailable_diagnostic_code));
  AddEvidence(&result,
              "checkpoint_descriptor_kind",
              result.checkpoint_descriptor_kind);
  AddEvidence(&result, "resume_token_kind", result.resume_token_kind);
  AddEvidence(&result,
              "external_mechanism_class",
              std::string(contract->external_mechanism_class));
  AddEvidence(&result, "sblr_execution_attempted", "false");
  AddEvidence(&result, "external_storage_authority_accepted", "false");
  AddEvidence(&result, "external_finality_accepted", "false");
  AddEvidence(&result, "scratchbird_mga_authority_preserved", "true");
  return result;
}

}  // namespace scratchbird::server
