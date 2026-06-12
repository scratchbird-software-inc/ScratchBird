// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "statement/statement_catalog.hpp"

#include "common/common.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"

#include <array>
#include <string_view>
#include <string>
#include <vector>

namespace scratchbird::parser::sbsql {
namespace {

constexpr std::string_view kSbsqlLifecycleFamily = "sblr.management.runtime_operation.v3";
constexpr std::string_view kLifecycleDiagnosticShape = "diagnostic.lifecycle.message_vector";
constexpr std::string_view kLifecycleAuthorityDomain = "engine_lifecycle";
constexpr std::string_view kLifecycleSecurityAuthority = "engine_lifecycle";
constexpr std::string_view kLifecycleCommandFamily = "database_lifecycle";
constexpr std::string_view kLifecycleStatusShape = "result.shape.lifecycle_status";
constexpr std::string_view kLifecycleReportShape = "result.shape.management_report";
constexpr std::string_view kLifecycleManagementResource = "resource.contract.lifecycle_management";
constexpr std::string_view kLifecycleReadResource = "resource.contract.lifecycle_read";
constexpr std::string_view kDatabaseUuidFromContext = "database_uuid_from_context";
constexpr std::string_view kDatabaseUuidGenerated = "database_uuid_generated_by_engine_lifecycle";

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

bool HasSuffix(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool IsCommandBoundary(char ch) {
  return ch == '\0' || ch == ' ' || ch == '\t' || ch == '\r' ||
         ch == '\n' || ch == ';' || ch == '(' || ch == '\'' || ch == '"';
}

bool StartsWithCommand(std::string_view value, std::string_view prefix) {
  if (!StartsWith(value, prefix)) return false;
  return value.size() == prefix.size() || IsCommandBoundary(value[prefix.size()]);
}

std::string NormalizeCommandText(std::string_view sql_text) {
  const auto upper = ToUpperAscii(sql_text);
  std::string normalized;
  normalized.reserve(upper.size());
  bool previous_space = false;
  for (const char ch : upper) {
    if (ch == ';') break;
    const bool space = ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    if (space) {
      if (!previous_space && !normalized.empty()) normalized.push_back(' ');
      previous_space = true;
      continue;
    }
    normalized.push_back(ch);
    previous_space = false;
  }
  while (!normalized.empty() && normalized.back() == ' ') normalized.pop_back();
  return normalized;
}

bool IsStatementRegistryRow(const GeneratedSurfaceRegistryRow& row) {
  return row.family != "expression_runtime";
}

std::string_view StatementFormFor(const GeneratedSurfaceRegistryRow& row) {
  if (row.surface_kind == "canonical_surface") return "canonical_surface";
  if (HasSuffix(row.canonical_name, "_stmt") || Contains(row.canonical_name, "statement")) {
    return "statement_production";
  }
  return "grammar_fragment";
}

bool TopLevelCandidateFor(const GeneratedSurfaceRegistryRow& row) {
  return row.surface_kind == "canonical_surface" ||
         HasSuffix(row.canonical_name, "_stmt") ||
         Contains(row.canonical_name, "statement");
}

std::string_view AstNodeKindFor(const GeneratedSurfaceRegistryRow& row) {
  if (row.surface_kind == "canonical_surface") return "statement_surface";
  if (TopLevelCandidateFor(row)) return "statement_production";
  return "statement_grammar_fragment";
}

bool RequiresAuthorityResolution(const GeneratedSurfaceRegistryRow& row) {
  return row.family == "query" || row.family == "dml" || row.family == "ddl_catalog" ||
         row.family == "security" || row.family == "multi_model" ||
         row.family == "runtime_management" || row.family == "jobs_scheduler" ||
         row.family == "acceleration" || row.family == "storage_management";
}

std::string_view BindingContractFor(const GeneratedSurfaceRegistryRow& row) {
  if (row.cluster_scope == "cluster_private" || row.source_status == "cluster_private") {
    return "binder.statement.cluster_profile_gate";
  }
  if (RequiresAuthorityResolution(row)) return "binder.statement.public_authority_required";
  if (row.family == "transaction") return "binder.statement.transaction_context";
  if (row.family == "observability") return "binder.statement.observability_context";
  return "binder.statement.syntax_artifact_only";
}

std::string_view AdmissionContractFor(const GeneratedSurfaceRegistryRow& row) {
  if (row.cluster_scope == "cluster_private" || row.source_status == "cluster_private") {
    return "admission.statement.cluster_profile_or_fail_closed";
  }
  return "admission.statement.server_revalidation_required";
}

std::string_view BehaviorDescriptorFor(const GeneratedSurfaceRegistryRow& row) {
  if (row.cluster_scope == "cluster_private" || row.source_status == "cluster_private") {
    return "behavior.statement.cluster_private.fail_closed_or_profile_gate";
  }
  if (Contains(row.final_acceptance_rule, "refusal") ||
      Contains(row.engine_rule_key, "fail_closed")) {
    return "behavior.statement.exact_refusal";
  }
  return "behavior.statement.parse_ast_bind_lower_engine_rule";
}

bool RequiresExactRefusal(const GeneratedSurfaceRegistryRow& row) {
  return row.source_status == "cluster_private" || row.cluster_scope == "cluster_private" ||
         Contains(row.final_acceptance_rule, "refusal") ||
         Contains(row.engine_rule_key, "fail_closed");
}

StatementSurfaceDescriptor MakeDescriptor(const GeneratedSurfaceRegistryRow& row) {
  return {
      row.surface_id,
      row.fixed_uuid_v7,
      row.canonical_name,
      ParseStatementSurfaceKind(row.surface_kind).value_or(StatementSurfaceKind::kGrammarProduction),
      ParseStatementParserCategory(row.family).value_or(StatementParserCategory::kGeneral),
      row.family,
      row.source_status,
      row.cluster_scope,
      row.owner_lane,
      row.sblr_operation_family,
      row.parser_handler_key,
      row.lowering_handler_key,
      row.server_admission_key,
      row.engine_rule_key,
      row.diagnostic_key,
      row.final_acceptance_rule,
      AstNodeKindFor(row),
      StatementFormFor(row),
      BindingContractFor(row),
      AdmissionContractFor(row),
      BehaviorDescriptorFor(row),
      TopLevelCandidateFor(row),
      RequiresExactRefusal(row),
      RequiresAuthorityResolution(row),
  };
}

std::vector<StatementSurfaceDescriptor> BuildStatementDescriptors() {
  std::vector<StatementSurfaceDescriptor> descriptors;
  for (const auto& row : GeneratedSurfaceRegistryRows()) {
    if (!IsStatementRegistryRow(row)) continue;
    descriptors.push_back(MakeDescriptor(row));
  }
  return descriptors;
}

const std::vector<StatementSurfaceDescriptor>& DescriptorStorage() {
  static const auto descriptors = BuildStatementDescriptors();
  return descriptors;
}

const std::array<LifecycleMappingDescriptor, 20>& LifecycleMappingStorage() {
  static constexpr std::array<LifecycleMappingDescriptor, 20> mappings{{
      {"sbsql.lifecycle.create_database",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.create_database",
       "SBLR_LIFECYCLE_CREATE_DATABASE",
       kSbsqlLifecycleFamily,
       "EngineCreateLifecycle",
       "EngineCreateLifecycleRequest",
       "EngineCreateLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird engine lifecycle create authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_create",
       kDatabaseUuidGenerated,
       false,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.open_database",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.open_database",
       "SBLR_LIFECYCLE_OPEN_DATABASE",
       kSbsqlLifecycleFamily,
       "EngineOpenLifecycle",
       "EngineOpenLifecycleRequest",
       "EngineOpenLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird engine lifecycle open authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_open",
       kDatabaseUuidFromContext,
       false,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.attach_database",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.attach_database",
       "SBLR_LIFECYCLE_ATTACH_DATABASE",
       kSbsqlLifecycleFamily,
       "EngineAttachLifecycle",
       "EngineAttachLifecycleRequest",
       "EngineAttachLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird engine lifecycle attach authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_attach",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.use_database_alias",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.attach_database",
       "SBLR_LIFECYCLE_ATTACH_DATABASE",
       kSbsqlLifecycleFamily,
       "EngineAttachLifecycle",
       "EngineAttachLifecycleRequest",
       "EngineAttachLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL USE database alias maps to ScratchBird engine lifecycle attach/session-selection authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_attach",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.detach_database",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.detach_database",
       "SBLR_LIFECYCLE_DETACH_DATABASE",
       kSbsqlLifecycleFamily,
       "EngineDetachLifecycle",
       "EngineDetachLifecycleRequest",
       "EngineDetachLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird engine lifecycle detach authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_detach",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.enter_maintenance",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.enter_maintenance",
       "SBLR_LIFECYCLE_ENTER_MAINTENANCE",
       kSbsqlLifecycleFamily,
       "EngineEnterMaintenanceLifecycle",
       "EngineEnterMaintenanceLifecycleRequest",
       "EngineEnterMaintenanceLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird maintenance-mode authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_maintenance",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.exit_maintenance",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.exit_maintenance",
       "SBLR_LIFECYCLE_EXIT_MAINTENANCE",
       kSbsqlLifecycleFamily,
       "EngineExitMaintenanceLifecycle",
       "EngineExitMaintenanceLifecycleRequest",
       "EngineExitMaintenanceLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird maintenance-exit authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_maintenance",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.enter_restricted_open",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.enter_restricted_open",
       "SBLR_LIFECYCLE_ENTER_RESTRICTED_OPEN",
       kSbsqlLifecycleFamily,
       "EngineEnterRestrictedOpenLifecycle",
       "EngineEnterRestrictedOpenLifecycleRequest",
       "EngineEnterRestrictedOpenLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird restricted-open authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_restricted_open",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.exit_restricted_open",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.exit_restricted_open",
       "SBLR_LIFECYCLE_EXIT_RESTRICTED_OPEN",
       kSbsqlLifecycleFamily,
       "EngineExitRestrictedOpenLifecycle",
       "EngineExitRestrictedOpenLifecycleRequest",
       "EngineExitRestrictedOpenLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird restricted-open exit authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_restricted_open",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.inspect_database",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.inspect_database",
       "SBLR_LIFECYCLE_INSPECT_DATABASE",
       kSbsqlLifecycleFamily,
       "EngineInspectLifecycle",
       "EngineInspectLifecycleRequest",
       "EngineInspectLifecycleResult",
       kLifecycleReportShape,
       kLifecycleDiagnosticShape,
       kLifecycleReadResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird inspect authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_inspect",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.verify_database",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.verify_database",
       "SBLR_LIFECYCLE_VERIFY_DATABASE",
       kSbsqlLifecycleFamily,
       "EngineVerifyLifecycle",
       "EngineVerifyLifecycleRequest",
       "EngineVerifyLifecycleResult",
       kLifecycleReportShape,
       kLifecycleDiagnosticShape,
       kLifecycleReadResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird verify authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_verify",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.repair_database",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.repair_database",
       "SBLR_LIFECYCLE_REPAIR_DATABASE",
       kSbsqlLifecycleFamily,
       "EngineRepairLifecycle",
       "EngineRepairLifecycleRequest",
       "EngineRepairLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird repair-plan authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_repair",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.shutdown_database",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.shutdown_database",
       "SBLR_LIFECYCLE_SHUTDOWN_DATABASE",
       kSbsqlLifecycleFamily,
       "EngineShutdownLifecycle",
       "EngineShutdownLifecycleRequest",
       "EngineShutdownLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps to ScratchBird shutdown authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_shutdown",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.shutdown_force",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.shutdown_force",
       "SBLR_LIFECYCLE_SHUTDOWN_FORCE",
       kSbsqlLifecycleFamily,
       "EngineForceShutdownLifecycle",
       "EngineForceShutdownLifecycleRequest",
       "EngineForceShutdownLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps explicit force shutdown to ScratchBird target-scope authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_force_shutdown",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.shutdown_acknowledge",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.shutdown_acknowledge",
       "SBLR_LIFECYCLE_SHUTDOWN_ACKNOWLEDGE",
       kSbsqlLifecycleFamily,
       "EngineAcknowledgeShutdownLifecycle",
       "EngineAcknowledgeShutdownLifecycleRequest",
       "EngineAcknowledgeShutdownLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps shutdown acknowledgement to ScratchBird drain authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_shutdown_acknowledge",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.lifecycle.drop_database",
       "sbsql",
       kLifecycleCommandFamily,
       LifecycleMappingDisposition::kScratchBirdLifecycleApi,
       "lifecycle.drop_database",
       "SBLR_LIFECYCLE_DROP_DATABASE",
       kSbsqlLifecycleFamily,
       "EngineDropLifecycle",
       "EngineDropLifecycleRequest",
       "EngineDropLifecycleResult",
       kLifecycleStatusShape,
       kLifecycleDiagnosticShape,
       kLifecycleManagementResource,
       "SBSQL.LIFECYCLE.MAPPED",
       "INFO",
       "SBSQL lifecycle command maps drop to ScratchBird safe-drop authority.",
       kLifecycleAuthorityDomain,
       kLifecycleSecurityAuthority,
       "right.lifecycle_drop",
       kDatabaseUuidFromContext,
       true,
       false,
       false,
       false,
       false,
       false},
      {"sbsql.emulated.shadow_non_file",
       "sbsql",
       "donor_file_emulation",
       LifecycleMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "",
       "diagnostic.lifecycle.message_vector",
       "",
       "SBSQL.EMULATION.NON_FILE_OPERATION",
       "ERROR",
       "Donor shadow/file-management syntax is diagnostic-only in SBSQL and has no filesystem side effect.",
       "parser_emulation_boundary",
       "engine_lifecycle",
       "right.lifecycle_admin",
       "",
       true,
       false,
       false,
       false,
       false,
       true},
      {"sbsql.emulated.backup_restore_non_file",
       "sbsql",
       "donor_file_emulation",
       LifecycleMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "",
       "diagnostic.lifecycle.message_vector",
       "",
       "SBSQL.EMULATION.NON_FILE_OPERATION",
       "ERROR",
       "Donor backup/restore file syntax is diagnostic-only in SBSQL and must route through ScratchBird lifecycle authority.",
       "parser_emulation_boundary",
       "engine_lifecycle",
       "right.lifecycle_admin",
       "",
       true,
       false,
       false,
       false,
       false,
       true},
      {"sbsql.emulated.database_file_management",
       "sbsql",
       "donor_file_emulation",
       LifecycleMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "",
       "diagnostic.lifecycle.message_vector",
       "",
       "SBSQL.EMULATION.NON_FILE_OPERATION",
       "ERROR",
       "Database file-management syntax is diagnostic-only and cannot perform parser-side file changes.",
       "parser_emulation_boundary",
       "engine_lifecycle",
       "right.lifecycle_admin",
       "",
       true,
       false,
       false,
       false,
       false,
       true},
      {"sbsql.emulated.donor_tool_non_file",
       "sbsql",
       "donor_tool_emulation",
       LifecycleMappingDisposition::kEmulatedNonFileDiagnostic,
       "",
       "",
       "",
       "",
       "",
       "",
       "",
       "diagnostic.lifecycle.message_vector",
       "",
       "SBSQL.EMULATION.DONOR_TOOL_NOT_EXECUTED",
       "ERROR",
       "Donor native tools are not invoked by the SBSQL parser; use ScratchBird management routes.",
       "parser_emulation_boundary",
       "engine_lifecycle",
       "right.lifecycle_admin",
       "",
       true,
       false,
       false,
       false,
       false,
       true},
  }};
  return mappings;
}

const LifecycleMappingDescriptor* MappingByKey(std::string_view key) {
  for (const auto& mapping : LifecycleMappingStorage()) {
    if (mapping.mapping_key == key) return &mapping;
  }
  return nullptr;
}

} // namespace

std::span<const StatementSurfaceDescriptor> BuiltinStatementSurfaceDescriptors() {
  const auto& descriptors = DescriptorStorage();
  return {descriptors.data(), descriptors.size()};
}

std::span<const LifecycleMappingDescriptor> BuiltinSbsqlLifecycleMappings() {
  const auto& mappings = LifecycleMappingStorage();
  return {mappings.data(), mappings.size()};
}

const StatementSurfaceDescriptor* FindStatementSurfaceById(std::string_view surface_id) {
  for (const auto& descriptor : BuiltinStatementSurfaceDescriptors()) {
    if (descriptor.surface_id == surface_id) return &descriptor;
  }
  return nullptr;
}

const StatementSurfaceDescriptor* FindStatementSurfaceByName(std::string_view canonical_name) {
  const auto wanted = ToUpperAscii(canonical_name);
  for (const auto& descriptor : BuiltinStatementSurfaceDescriptors()) {
    if (ToUpperAscii(descriptor.canonical_name) == wanted) return &descriptor;
  }
  return nullptr;
}

const LifecycleMappingDescriptor* FindSbsqlLifecycleMappingByOperationId(std::string_view operation_id) {
  for (const auto& mapping : BuiltinSbsqlLifecycleMappings()) {
    if (mapping.operation_id == operation_id) return &mapping;
  }
  return nullptr;
}

const LifecycleMappingDescriptor* FindSbsqlLifecycleMappingBySblrOperation(std::string_view sblr_operation) {
  for (const auto& mapping : BuiltinSbsqlLifecycleMappings()) {
    if (mapping.sblr_operation == sblr_operation) return &mapping;
  }
  return nullptr;
}

const LifecycleMappingDescriptor* MapSbsqlLifecycleCommand(std::string_view sql_text) {
  const auto command = NormalizeCommandText(sql_text);
  if (command.empty()) return nullptr;
  if (StartsWithCommand(command, "CREATE DATABASE")) {
    return MappingByKey("sbsql.lifecycle.create_database");
  }
  if (StartsWithCommand(command, "ALTER DATABASE")) {
    if (Contains(command, " REPAIR")) {
      return MappingByKey("sbsql.lifecycle.repair_database");
    }
    if (Contains(command, " VERIFY")) {
      return MappingByKey("sbsql.lifecycle.verify_database");
    }
    if (Contains(command, " RESTRICTED")) {
      if (Contains(command, " EXIT") || Contains(command, " CLEAR") ||
          Contains(command, " OFF")) {
        return MappingByKey("sbsql.lifecycle.exit_restricted_open");
      }
      return MappingByKey("sbsql.lifecycle.enter_restricted_open");
    }
    if (Contains(command, " MAINTENANCE")) {
      if (Contains(command, " EXIT") || Contains(command, " CLEAR") ||
          Contains(command, " OFF")) {
        return MappingByKey("sbsql.lifecycle.exit_maintenance");
      }
      return MappingByKey("sbsql.lifecycle.enter_maintenance");
    }
    if (Contains(command, " SHUTDOWN") && Contains(command, " FORCE")) {
      return MappingByKey("sbsql.lifecycle.shutdown_force");
    }
    if (Contains(command, " SHUTDOWN")) {
      return MappingByKey("sbsql.lifecycle.shutdown_database");
    }
  }
  if (StartsWithCommand(command, "OPEN DATABASE")) {
    if (Contains(command, " RESTRICTED")) {
      return MappingByKey("sbsql.lifecycle.enter_restricted_open");
    }
    return MappingByKey("sbsql.lifecycle.open_database");
  }
  if (StartsWithCommand(command, "ATTACH POLICY")) {
    return nullptr;
  }
  if (StartsWithCommand(command, "ATTACH DATABASE") ||
      StartsWithCommand(command, "ATTACH")) {
    return MappingByKey("sbsql.lifecycle.attach_database");
  }
  if (StartsWithCommand(command, "USE DATABASE") ||
      StartsWithCommand(command, "USE")) {
    return MappingByKey("sbsql.lifecycle.use_database_alias");
  }
  if (StartsWithCommand(command, "DETACH DATABASE")) {
    return MappingByKey("sbsql.lifecycle.detach_database");
  }
  if (StartsWithCommand(command, "ENTER DATABASE MAINTENANCE") ||
      StartsWithCommand(command, "ENTER MAINTENANCE") ||
      StartsWithCommand(command, "SET DATABASE MAINTENANCE") ||
      StartsWithCommand(command, "MAINTENANCE DATABASE")) {
    return MappingByKey("sbsql.lifecycle.enter_maintenance");
  }
  if (StartsWithCommand(command, "EXIT DATABASE MAINTENANCE") ||
      StartsWithCommand(command, "EXIT MAINTENANCE") ||
      StartsWithCommand(command, "CLEAR DATABASE MAINTENANCE")) {
    return MappingByKey("sbsql.lifecycle.exit_maintenance");
  }
  if (StartsWithCommand(command, "ENTER DATABASE RESTRICTED OPEN") ||
      StartsWithCommand(command, "ENTER RESTRICTED OPEN") ||
      StartsWithCommand(command, "RESTRICTED OPEN DATABASE")) {
    return MappingByKey("sbsql.lifecycle.enter_restricted_open");
  }
  if (StartsWithCommand(command, "EXIT DATABASE RESTRICTED OPEN") ||
      StartsWithCommand(command, "EXIT RESTRICTED OPEN") ||
      StartsWithCommand(command, "CLEAR DATABASE RESTRICTED OPEN")) {
    return MappingByKey("sbsql.lifecycle.exit_restricted_open");
  }
  if (StartsWithCommand(command, "INSPECT DATABASE") ||
      StartsWithCommand(command, "DIAGNOSE DATABASE")) {
    return MappingByKey("sbsql.lifecycle.inspect_database");
  }
  if (StartsWithCommand(command, "VERIFY DATABASE")) {
    return MappingByKey("sbsql.lifecycle.verify_database");
  }
  if (StartsWithCommand(command, "REPAIR DATABASE")) {
    return MappingByKey("sbsql.lifecycle.repair_database");
  }
  if (StartsWithCommand(command, "FORCE SHUTDOWN DATABASE") ||
      (StartsWithCommand(command, "SHUTDOWN DATABASE") && Contains(command, " FORCE"))) {
    return MappingByKey("sbsql.lifecycle.shutdown_force");
  }
  if (StartsWithCommand(command, "SHUTDOWN DATABASE")) {
    return MappingByKey("sbsql.lifecycle.shutdown_database");
  }
  if (StartsWithCommand(command, "ACKNOWLEDGE SHUTDOWN DATABASE") ||
      StartsWithCommand(command, "SHUTDOWN ACKNOWLEDGE DATABASE")) {
    return MappingByKey("sbsql.lifecycle.shutdown_acknowledge");
  }
  if (StartsWithCommand(command, "DROP DATABASE")) {
    return MappingByKey("sbsql.lifecycle.drop_database");
  }
  if (StartsWithCommand(command, "CREATE SHADOW FILESPACE") ||
      StartsWithCommand(command, "REFRESH SHADOW FILESPACE") ||
      StartsWithCommand(command, "VALIDATE SHADOW FILESPACE") ||
      (StartsWithCommand(command, "ALTER SHADOW") &&
       Contains(command, " PROMOTE") && Contains(command, " PRIMARY"))) {
    return nullptr;
  }
  if (StartsWithCommand(command, "CREATE SHADOW") ||
      StartsWithCommand(command, "ALTER SHADOW") ||
      StartsWithCommand(command, "DROP SHADOW")) {
    return MappingByKey("sbsql.emulated.shadow_non_file");
  }
  if ((StartsWithCommand(command, "BACKUP DATABASE") &&
       !StartsWithCommand(command, "BACKUP DATABASE TO")) ||
      (StartsWithCommand(command, "RESTORE DATABASE") &&
       !StartsWithCommand(command, "RESTORE DATABASE FROM"))) {
    return MappingByKey("sbsql.emulated.backup_restore_non_file");
  }
  if (StartsWithCommand(command, "NBACKUP")) {
    return MappingByKey("sbsql.emulated.backup_restore_non_file");
  }
  if (StartsWithCommand(command, "ALTER DATABASE") && Contains(command, " FILE")) {
    return MappingByKey("sbsql.emulated.database_file_management");
  }
  if (StartsWithCommand(command, "GBAK") ||
      StartsWithCommand(command, "GFIX") ||
      StartsWithCommand(command, "GSTAT") ||
      StartsWithCommand(command, "GSEC") ||
      StartsWithCommand(command, "FBSVCMGR") ||
      StartsWithCommand(command, "FBTRACEMGR")) {
    return MappingByKey("sbsql.emulated.donor_tool_non_file");
  }
  return nullptr;
}

std::optional<StatementSurfaceKind> ParseStatementSurfaceKind(std::string_view kind) {
  if (kind == "grammar_production") return StatementSurfaceKind::kGrammarProduction;
  if (kind == "canonical_surface") return StatementSurfaceKind::kCanonicalSurface;
  return std::nullopt;
}

std::optional<StatementParserCategory> ParseStatementParserCategory(std::string_view family) {
  if (family == "general") return StatementParserCategory::kGeneral;
  if (family == "query") return StatementParserCategory::kQuery;
  if (family == "dml") return StatementParserCategory::kDml;
  if (family == "ddl_catalog") return StatementParserCategory::kDdlCatalog;
  if (family == "security") return StatementParserCategory::kSecurity;
  if (family == "transaction") return StatementParserCategory::kTransaction;
  if (family == "observability") return StatementParserCategory::kObservability;
  if (family == "runtime_management") return StatementParserCategory::kRuntimeManagement;
  if (family == "storage_management") return StatementParserCategory::kStorageManagement;
  if (family == "jobs_scheduler") return StatementParserCategory::kJobsScheduler;
  if (family == "archive_replication") return StatementParserCategory::kArchiveReplication;
  if (family == "acceleration") return StatementParserCategory::kAcceleration;
  if (family == "multi_model") return StatementParserCategory::kMultiModel;
  if (family == "migration") return StatementParserCategory::kMigration;
  if (family == "cluster_private") return StatementParserCategory::kClusterPrivate;
  return std::nullopt;
}

std::string_view StatementSurfaceKindName(StatementSurfaceKind kind) {
  switch (kind) {
    case StatementSurfaceKind::kGrammarProduction: return "grammar_production";
    case StatementSurfaceKind::kCanonicalSurface: return "canonical_surface";
  }
  return "grammar_production";
}

std::string_view StatementParserCategoryName(StatementParserCategory category) {
  switch (category) {
    case StatementParserCategory::kGeneral: return "general";
    case StatementParserCategory::kQuery: return "query";
    case StatementParserCategory::kDml: return "dml";
    case StatementParserCategory::kDdlCatalog: return "ddl_catalog";
    case StatementParserCategory::kSecurity: return "security";
    case StatementParserCategory::kTransaction: return "transaction";
    case StatementParserCategory::kObservability: return "observability";
    case StatementParserCategory::kRuntimeManagement: return "runtime_management";
    case StatementParserCategory::kStorageManagement: return "storage_management";
    case StatementParserCategory::kJobsScheduler: return "jobs_scheduler";
    case StatementParserCategory::kArchiveReplication: return "archive_replication";
    case StatementParserCategory::kAcceleration: return "acceleration";
    case StatementParserCategory::kMultiModel: return "multi_model";
    case StatementParserCategory::kMigration: return "migration";
    case StatementParserCategory::kClusterPrivate: return "cluster_private";
  }
  return "general";
}

std::string_view LifecycleMappingDispositionName(LifecycleMappingDisposition disposition) {
  switch (disposition) {
    case LifecycleMappingDisposition::kScratchBirdLifecycleApi:
      return "scratchbird_lifecycle_api";
    case LifecycleMappingDisposition::kEmulatedNonFileDiagnostic:
      return "emulated_non_file_diagnostic";
  }
  return "emulated_non_file_diagnostic";
}

} // namespace scratchbird::parser::sbsql
