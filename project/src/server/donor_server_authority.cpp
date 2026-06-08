// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "donor_server_authority.hpp"

#include <array>

namespace scratchbird::server {
namespace {

struct SurfaceRule {
  std::string_view surface_key;
  std::string_view donor_engine;
  DonorServerAuthorityAction action;
  std::string_view diagnostic_code;
};

constexpr std::string_view kSblrAction =
    "none_for_external_authority; admitted donor DML/DDL lowers separately when allowed";
constexpr std::string_view kMgaRule =
    "no donor finality; server route preserves ScratchBird MGA transaction authority";

constexpr std::array<std::string_view, 24> kKnownDonorEngines{{
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
    {"database_create", "", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.DATABASE_AUTHORITY_DENIED"},
    {"database_alter", "", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.DATABASE_AUTHORITY_DENIED"},
    {"database_drop", "", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.DATABASE_AUTHORITY_DENIED"},
    {"tablespace_storage", "", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"extension_install", "", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"external_file_read", "", DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_IO_DENIED"},
    {"external_file_write", "", DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_IO_DENIED"},
    {"os_command", "", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.OS_COMMAND_DENIED"},
    {"physical_backup_restore", "", DonorServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.PHYSICAL_OPERATION_ROUTED"},
    {"replication_endpoint", "", DonorServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"cluster_topology", "", DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.CLUSTER_TOPOLOGY_DENIED"},
    {"privileged_catalog_write", "", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.CATALOG_WRITE_DENIED"},
    {"cassandra_keyspace_create", "cassandra",
     DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.DATABASE_AUTHORITY_DENIED"},
    {"cassandra_udf", "cassandra", DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"cassandra_cdc", "cassandra", DonorServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"duckdb_attach_file", "duckdb", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"duckdb_extension_install", "duckdb",
     DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"duckdb_copy_external", "duckdb", DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_IO_DENIED"},
    {"firebird_services_backup", "firebird",
     DonorServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.PHYSICAL_OPERATION_ROUTED"},
    {"firebird_database_file_attach", "firebird",
     DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"firebird_udf_module", "firebird", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"mariadb_plugin_install", "mariadb",
     DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"mariadb_replication_control", "mariadb",
     DonorServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"mariadb_sequence_engine", "mariadb",
     DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"mongodb_admin_commands", "mongodb",
     DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.CLUSTER_TOPOLOGY_DENIED"},
    {"mongodb_change_stream", "mongodb",
     DonorServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"mysql_load_data_file", "mysql", DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_IO_DENIED"},
    {"mysql_plugin_install", "mysql", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"mysql_replication_control", "mysql",
     DonorServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"opensearch_cluster_settings", "opensearch",
     DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.CLUSTER_TOPOLOGY_DENIED"},
    {"opensearch_plugin", "opensearch", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"opensearch_snapshot", "opensearch",
     DonorServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.PHYSICAL_OPERATION_ROUTED"},
    {"pg_copy_program", "postgresql", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.OS_COMMAND_DENIED"},
    {"pg_extension_ddl", "postgresql", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"pg_tablespace_ddl", "postgresql", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"pg_fdw_server", "postgresql", DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_ENDPOINT_DENIED"},
    {"redis_config_rewrite", "redis", DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"redis_module", "redis", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"redis_replication", "redis", DonorServerAuthorityAction::kMigrationServiceRoute,
     "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED"},
    {"sqlite_attach_file", "sqlite", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
    {"sqlite_load_extension", "sqlite", DonorServerAuthorityAction::kSecurityDenial,
     "SB.SECURITY.EXTENSION_LOAD_DENIED"},
    {"sqlite_pragma_files", "sqlite", DonorServerAuthorityAction::kServerPolicyGate,
     "SB.SECURITY.EXTERNAL_STORAGE_DENIED"},
}};

std::string_view ServerActionName(DonorServerAuthorityAction action) {
  switch (action) {
    case DonorServerAuthorityAction::kSecurityDenial: return "security_denial";
    case DonorServerAuthorityAction::kServerPolicyGate: return "server_policy_gate";
    case DonorServerAuthorityAction::kMigrationServiceRoute:
      return "migration_service_route";
  }
  return "security_denial";
}

std::string_view Execution_PlanLane(DonorServerAuthorityAction action) {
  switch (action) {
    case DonorServerAuthorityAction::kSecurityDenial:
      return "security_denial_and_exact_refusal";
    case DonorServerAuthorityAction::kServerPolicyGate:
      return "server_policy_gate_and_connector_authorization";
    case DonorServerAuthorityAction::kMigrationServiceRoute:
      return "migration_service_route_and_live_migration_protocol";
  }
  return "security_denial_and_exact_refusal";
}

std::string_view RouteContractId(DonorServerAuthorityAction action) {
  switch (action) {
    case DonorServerAuthorityAction::kSecurityDenial:
      return "server_authority.security_denial.exact_refusal.v1";
    case DonorServerAuthorityAction::kServerPolicyGate:
      return "server_authority.policy_gate.connector_authorization.v1";
    case DonorServerAuthorityAction::kMigrationServiceRoute:
      return "server_authority.migration_service.route_contract.v1";
  }
  return "server_authority.security_denial.exact_refusal.v1";
}

bool DonorAllowedForRule(std::string_view donor_engine,
                         const SurfaceRule& rule) {
  return rule.donor_engine.empty() || rule.donor_engine == donor_engine;
}

const SurfaceRule* FindSurfaceRule(std::string_view engine_id,
                                   std::string_view surface_key) {
  if (!IsKnownDonorEngineForServerAuthority(engine_id)) return nullptr;
  for (const auto& rule : kSurfaceRules) {
    if (rule.surface_key == surface_key && DonorAllowedForRule(engine_id, rule)) {
      return &rule;
    }
  }
  return nullptr;
}

DonorServerAuthorityDecision DecisionForRule(const SurfaceRule& rule) {
  const bool is_security_denial =
      rule.action == DonorServerAuthorityAction::kSecurityDenial;
  const bool is_policy_gate =
      rule.action == DonorServerAuthorityAction::kServerPolicyGate;
  const bool is_migration =
      rule.action == DonorServerAuthorityAction::kMigrationServiceRoute;
  return DonorServerAuthorityDecision{
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

void AddEvidence(DonorServerAuthorityRouteResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.emplace_back(std::move(key), std::move(value));
}

void AddEvidence(DonorMigrationRouteResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.emplace_back(std::move(key), std::move(value));
}

std::optional<DonorMigrationRouteKind> MigrationKindForDiagnostic(
    std::string_view diagnostic_code) {
  if (diagnostic_code == "SB.MIGRATION.PHYSICAL_OPERATION_ROUTED") {
    return DonorMigrationRouteKind::kPhysicalOperation;
  }
  if (diagnostic_code == "SB.MIGRATION.REPLICATION_ENDPOINT_ROUTED") {
    return DonorMigrationRouteKind::kLiveReplicationEndpoint;
  }
  return std::nullopt;
}

std::string_view MigrationRouteContractId(DonorMigrationRouteKind kind) {
  switch (kind) {
    case DonorMigrationRouteKind::kPhysicalOperation:
      return "server_authority.migration.physical_operation.route_contract.v1";
    case DonorMigrationRouteKind::kLiveReplicationEndpoint:
      return "server_authority.migration.live_replication.route_contract.v1";
  }
  return "server_authority.migration.physical_operation.route_contract.v1";
}

std::string_view MigrationUnavailableDiagnostic(DonorMigrationRouteKind kind) {
  switch (kind) {
    case DonorMigrationRouteKind::kPhysicalOperation:
      return "SB.MIGRATION.PHYSICAL_SERVICE_UNAVAILABLE";
    case DonorMigrationRouteKind::kLiveReplicationEndpoint:
      return "SB.MIGRATION.REPLICATION_SERVICE_UNAVAILABLE";
  }
  return "SB.MIGRATION.PHYSICAL_SERVICE_UNAVAILABLE";
}

std::string_view MigrationCheckpointDescriptor(DonorMigrationRouteKind kind) {
  switch (kind) {
    case DonorMigrationRouteKind::kPhysicalOperation:
      return "scratchbird.migration.physical.checkpoint.v1";
    case DonorMigrationRouteKind::kLiveReplicationEndpoint:
      return "scratchbird.migration.live_replication.checkpoint.v1";
  }
  return "scratchbird.migration.physical.checkpoint.v1";
}

std::string_view MigrationResumeToken(DonorMigrationRouteKind kind) {
  switch (kind) {
    case DonorMigrationRouteKind::kPhysicalOperation:
      return "scratchbird.migration.physical.resume_token.v1";
    case DonorMigrationRouteKind::kLiveReplicationEndpoint:
      return "scratchbird.migration.live_replication.resume_token.v1";
  }
  return "scratchbird.migration.physical.resume_token.v1";
}

std::string_view MigrationMechanismClass(DonorMigrationRouteKind kind) {
  switch (kind) {
    case DonorMigrationRouteKind::kPhysicalOperation:
      return "donor_physical_backup_restore_snapshot_file_copy";
    case DonorMigrationRouteKind::kLiveReplicationEndpoint:
      return "donor_replication_cdc_binlog_oplog_changefeed";
  }
  return "donor_physical_backup_restore_snapshot_file_copy";
}

}  // namespace

bool IsKnownDonorEngineForServerAuthority(std::string_view engine_id) {
  for (const auto known_engine : kKnownDonorEngines) {
    if (known_engine == engine_id) return true;
  }
  return false;
}

std::optional<DonorServerAuthorityDecision> ResolveDonorServerAuthoritySurface(
    std::string_view engine_id,
    std::string_view surface_key) {
  const auto* rule = FindSurfaceRule(engine_id, surface_key);
  if (rule == nullptr) return std::nullopt;
  return DecisionForRule(*rule);
}

DonorServerAuthorityRouteResult EvaluateDonorServerAuthorityRoute(
    const DonorServerAuthorityRequest& request) {
  DonorServerAuthorityRouteResult result;
  const auto decision =
      ResolveDonorServerAuthoritySurface(request.engine_id, request.surface_key);
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
  result.donor_finality_accepted = decision->accepts_donor_finality;
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
  AddEvidence(&result, "donor_finality_accepted", "false");
  AddEvidence(&result,
              "materialized_authorization_context",
              request.materialized_authorization_context ? "true" : "false");
  AddEvidence(&result,
              "migration_service_available",
              request.migration_service_available ? "true" : "false");
  return result;
}

std::optional<DonorMigrationRouteContract> ResolveDonorMigrationRouteContract(
    std::string_view engine_id,
    std::string_view surface_key) {
  const auto decision = ResolveDonorServerAuthoritySurface(engine_id, surface_key);
  if (!decision.has_value() ||
      decision->action != DonorServerAuthorityAction::kMigrationServiceRoute) {
    return std::nullopt;
  }
  const auto kind = MigrationKindForDiagnostic(decision->diagnostic_code);
  if (!kind.has_value()) return std::nullopt;
  return DonorMigrationRouteContract{
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

DonorMigrationRouteResult EvaluateDonorMigrationRoute(
    const DonorMigrationRouteRequest& request) {
  DonorMigrationRouteResult result;
  const auto contract =
      ResolveDonorMigrationRouteContract(request.engine_id, request.surface_key);
  if (!contract.has_value()) {
    result.recognized = false;
    result.routed = false;
    result.accepted = false;
    result.service_unavailable = true;
    result.diagnostic_code = "SB.MIGRATION.UNKNOWN_ROUTE";
    result.route_contract_id = "server_authority.migration.unknown.fail_closed.v1";
    AddEvidence(&result, "migration_route_known", "false");
    AddEvidence(&result, "sblr_execution_attempted", "false");
    AddEvidence(&result, "donor_storage_authority_accepted", "false");
    AddEvidence(&result, "donor_finality_accepted", "false");
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
  result.donor_storage_authority_accepted =
      contract->donor_storage_authority_accepted;
  result.donor_finality_accepted = contract->donor_finality_accepted;
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
              "donor_mechanism_class",
              std::string(contract->donor_mechanism_class));
  AddEvidence(&result, "sblr_execution_attempted", "false");
  AddEvidence(&result, "donor_storage_authority_accepted", "false");
  AddEvidence(&result, "donor_finality_accepted", "false");
  AddEvidence(&result, "scratchbird_mga_authority_preserved", "true");
  return result;
}

}  // namespace scratchbird::server
