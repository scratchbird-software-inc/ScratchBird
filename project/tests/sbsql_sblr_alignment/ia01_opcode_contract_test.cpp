// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_opcode_registry.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace sblr = scratchbird::engine::sblr;

namespace {
struct Expected {
  std::string_view opcode;
  std::uint16_t code;
  std::string_view operation;
  std::string_view operand;
  std::string_view result;
  std::string_view executor;
};

constexpr std::array<Expected, 76> kExpected{{
    {"SBLR_PACKAGE_BEGIN", 0x0001u, "engine.op.package_begin", "package_header", "void"},
    {"SBLR_PACKAGE_END", 0x0002u, "engine.op.package_end", "package_footer", "void"},
    {"SBLR_LITERAL", 0x0003u, "engine.op.literal", "typed_literal", "typed_value"},
    {"SBLR_PARAMETER", 0x0004u, "engine.op.parameter", "parameter_descriptor_ref", "typed_value"},
    {"SBLR_VARIABLE", 0x0005u, "engine.op.variable", "variable_descriptor_ref", "typed_value"},
    {"SBLR_SOURCE_MAP", 0x0006u, "engine.op.source_map", "source_map_entry_vector", "void"},
    {"SBLR_ERROR_VECTOR", 0x0007u, "engine.op.error_vector", "diagnostic_vector", "void"},
    {"SBLR_PROJECT", 0x0500u, "engine.op.project", "projection_descriptor", "rowset_descriptor"},
    {"SBLR_AGGREGATE", 0x0501u, "engine.op.aggregate", "aggregate_descriptor", "rowset_descriptor"},
    {"SBLR_GROUP", 0x0502u, "engine.op.group", "group_descriptor", "rowset_descriptor"},
    {"SBLR_SORT", 0x0503u, "engine.op.sort", "sort_descriptor", "rowset_descriptor"},
    {"SBLR_LIMIT", 0x0504u, "engine.op.limit", "limit_descriptor", "rowset_descriptor"},
    {"SBLR_WINDOW", 0x0505u, "engine.op.window", "window_descriptor", "rowset_descriptor"},
    {"SBLR_RETURN_RESULT_SET", 0x0506u, "engine.op.return_result_set", "result_set_return_descriptor", "result_set_handle"},
    {"SBLR_DIAGNOSTIC_REFUSAL", 0x1900u, "engine.op.diagnostic_refusal", "diagnostic_refusal_descriptor", "diagnostic_refusal_result"},
    {"SBLR_DIAGNOSTIC_RESET", 0x1901u, "engine.op.diagnostic_reset", "diagnostic_reset_descriptor", "diagnostic_reset_result"},
    {"SBLR_DESCRIPTOR_TRANSFORM", 0x1902u, "engine.op.descriptor_transform", "descriptor_transform_descriptor", "descriptor_transform_result"},
    {"SBLR_OBSERVABILITY_SHOW_VERSION", 0x0D06u, "observability.show_version", "observability_show_version_descriptor", "observability_show_version_result", "engine.op.observability_show_version"},
    {"SBLR_OBSERVABILITY_SHOW_DATABASE", 0x0D07u, "observability.show_database", "observability_show_database_descriptor", "observability_show_database_result", "engine.op.observability_show_database"},
    {"SBLR_OBSERVABILITY_SHOW_SYSTEM", 0x0D08u, "observability.show_system", "observability_show_system_descriptor", "observability_show_system_result", "engine.op.observability_show_system"},
    {"SBLR_OBSERVABILITY_SHOW_CATALOG", 0x0D09u, "observability.show_catalog", "observability_show_catalog_descriptor", "observability_show_catalog_result"},
    {"SBLR_OBSERVABILITY_SHOW_SESSIONS", 0x0D0Au, "observability.show_sessions", "observability_show_sessions_descriptor", "observability_show_sessions_result"},
    {"SBLR_OBSERVABILITY_SHOW_TRANSACTIONS", 0x0D0Bu, "observability.show_transactions", "observability_show_transactions_descriptor", "observability_show_transactions_result"},
    {"SBLR_OBSERVABILITY_SHOW_LOCKS", 0x0D0Cu, "observability.show_locks", "observability_show_locks_descriptor", "observability_show_locks_result"},
    {"SBLR_OBSERVABILITY_SHOW_STATEMENTS", 0x0D0Du, "observability.show_statements", "observability_show_statements_descriptor", "observability_show_statements_result"},
    {"SBLR_OBSERVABILITY_SHOW_JOBS", 0x0D0Eu, "observability.show_jobs", "observability_show_jobs_descriptor", "observability_show_jobs_result"},
    {"SBLR_OBSERVABILITY_SHOW_MANAGEMENT", 0x0D0Fu, "observability.show_management", "observability_show_management_descriptor", "observability_show_management_result"},
    {"SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS", 0x0D20u, "observability.show_diagnostics", "observability_show_diagnostics_descriptor", "observability_show_diagnostics_result"},
    {"SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS_EXTENDED", 0x0D21u, "observability.show_diagnostics_extended", "observability_show_diagnostics_extended_descriptor", "observability_show_diagnostics_extended_result"},
    {"SBLR_OBSERVABILITY_SHOW_ARCHIVE_REPLICATION", 0x0D22u, "observability.show_archive_replication", "observability_show_archive_replication_descriptor", "observability_show_archive_replication_result"},
    {"SBLR_OBSERVABILITY_SHOW_AGENTS_EXTENDED", 0x0D23u, "observability.show_agents_extended", "observability_show_agents_extended_descriptor", "observability_show_agents_extended_result"},
    {"SBLR_OBSERVABILITY_SHOW_FILESPACE_EXTENDED", 0x0D24u, "observability.show_filespace_extended", "observability_show_filespace_extended_descriptor", "observability_show_filespace_extended_result"},
    {"SBLR_OBSERVABILITY_SHOW_ACCELERATION", 0x0D25u, "observability.show_acceleration", "observability_show_acceleration_descriptor", "observability_show_acceleration_result"},
    {"SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED", 0x0D26u, "observability.show_acceleration_extended", "observability_show_acceleration_extended_descriptor", "observability_show_acceleration_extended_result"},
    {"SBLR_OBSERVABILITY_SHOW_DECISION_SERVICE", 0x0D27u, "observability.show_decision_service", "observability_show_decision_service_descriptor", "observability_show_decision_service_result"},
    {"SBLR_OBSERVABILITY_EXPLAIN_OPERATION", 0x0D29u, "observability.explain_operation", "observability_explain_operation_descriptor", "observability_explain_operation_result"},
    {"SBLR_LIFECYCLE_CREATE_DATABASE", 0x1408u, "engine.op.lifecycle_create_database", "lifecycle_create_database_descriptor", "lifecycle_create_database_result"},
    {"SBLR_LIFECYCLE_OPEN_DATABASE", 0x1409u, "lifecycle.open_database", "lifecycle_open_database_descriptor", "lifecycle_open_database_result", "engine.op.lifecycle_open_database"},
    {"SBLR_LIFECYCLE_ATTACH_DATABASE", 0x140Au, "engine.op.lifecycle_attach_database", "lifecycle_attach_database_descriptor", "lifecycle_attach_database_result"},
    {"SBLR_LIFECYCLE_DETACH_DATABASE", 0x140Bu, "engine.op.lifecycle_detach_database", "lifecycle_detach_database_descriptor", "lifecycle_detach_database_result"},
    {"SBLR_LIFECYCLE_ENTER_MAINTENANCE", 0x140Cu, "engine.op.lifecycle_enter_maintenance", "lifecycle_enter_maintenance_descriptor", "lifecycle_mode_transition_result"},
    {"SBLR_DDL_ALTER_DOMAIN", 0x060Bu, "engine.op.ddl_alter_domain", "alter_domain_descriptor", "ddl_result"},
    {"SBLR_DDL_ALTER_VIEW", 0x060Du, "engine.op.ddl_alter_view", "alter_view_descriptor", "ddl_result"},
    {"SBLR_DDL_DROP_INDEX", 0x0605u, "engine.op.ddl_drop_index", "drop_index_descriptor", "ddl_result"},
    {"SBLR_DDL_ALTER_TRIGGER", 0x0610u, "engine.op.ddl_alter_trigger", "alter_trigger_descriptor", "ddl_result"},
    {"SBLR_DDL_CREATE_PROCEDURE", 0x0612u, "engine.op.ddl_create_procedure", "create_procedure_descriptor", "ddl_result"},
    {"SBLR_DDL_DROP_PROCEDURE", 0x0614u, "engine.op.ddl_drop_procedure", "drop_procedure_descriptor", "ddl_result"},
    {"SBLR_DDL_DROP_TABLE", 0x0603u, "engine.op.ddl_drop_table", "drop_table_descriptor", "ddl_result"},
    {"SBLR_DDL_CREATE_SCHEMA", 0x0600u, "engine.op.ddl_create_schema", "create_schema_descriptor", "ddl_result"},
    {"SBLR_LIFECYCLE_EXIT_MAINTENANCE", 0x140Du, "engine.op.lifecycle_exit_maintenance", "lifecycle_exit_maintenance_descriptor", "lifecycle_mode_transition_result"},
    {"SBLR_DDL_CREATE_DOMAIN", 0x0606u, "engine.op.ddl_create_domain", "create_domain_descriptor", "ddl_result"},
    {"SBLR_DDL_CREATE_VIEW", 0x060Cu, "engine.op.ddl_create_view", "create_view_descriptor", "ddl_result"},
    {"SBLR_DDL_DROP_VIEW", 0x060Eu, "engine.op.ddl_drop_view", "drop_view_descriptor", "ddl_result"},
    {"SBLR_DDL_DROP_SCHEMA", 0x0608u, "engine.op.ddl_drop_schema", "drop_schema_descriptor", "ddl_result"},
    {"SBLR_DDL_CREATE_INDEX", 0x0604u, "engine.op.ddl_create_index", "create_index_descriptor", "ddl_result"},
    {"SBLR_DDL_CREATE_TRIGGER", 0x060Fu, "engine.op.ddl_create_trigger", "create_trigger_descriptor", "ddl_result"},
    {"SBLR_DDL_ALTER_PROCEDURE", 0x0613u, "engine.op.ddl_alter_procedure", "alter_procedure_descriptor", "ddl_result"},
    {"SBLR_DDL_CREATE_FUNCTION", 0x0615u, "engine.op.ddl_create_function", "create_function_descriptor", "ddl_result"},
    {"SBLR_DDL_DROP_FUNCTION", 0x0617u, "engine.op.ddl_drop_function", "drop_function_descriptor", "ddl_result"},
    {"SBLR_DDL_ALTER_PACKAGE", 0x0619u, "engine.op.ddl_alter_package", "alter_package_descriptor", "ddl_result"},
    {"SBLR_DDL_CREATE_SEQUENCE", 0x0687u, "engine.op.ddl_create_sequence", "create_sequence_descriptor", "ddl_result"},
    {"SBLR_DDL_DROP_SEQUENCE", 0x061Du, "engine.op.ddl_drop_sequence", "drop_sequence_descriptor", "ddl_result"},
    {"SBLR_DDL_REFRESH_MATERIALIZED_VIEW", 0x061Fu, "engine.op.ddl_refresh_materialized_view", "refresh_materialized_view_descriptor", "ddl_result"},
    {"SBLR_DDL_DROP_MATERIALIZED_VIEW", 0x0620u, "engine.op.ddl_drop_materialized_view", "drop_materialized_view_descriptor", "ddl_result"},
    {"SBLR_DDL_DROP_TYPE", 0x0623u, "engine.op.ddl_drop_type", "drop_type_descriptor", "ddl_result"},
    {"SBLR_DDL_ALTER_FUNCTION", 0x0616u, "engine.op.ddl_alter_function", "alter_function_descriptor", "ddl_result"},
    {"SBLR_DDL_CREATE_PACKAGE", 0x0618u, "engine.op.ddl_create_package", "create_package_descriptor", "ddl_result"},
    {"SBLR_DDL_DROP_PACKAGE", 0x061Au, "engine.op.ddl_drop_package", "drop_package_descriptor", "ddl_result"},
    {"SBLR_DDL_ALTER_SEQUENCE", 0x061Cu, "engine.op.ddl_alter_sequence", "alter_sequence_descriptor", "ddl_result"},
    {"SBLR_DDL_CREATE_MATERIALIZED_VIEW", 0x061Eu, "engine.op.ddl_create_materialized_view", "create_materialized_view_descriptor", "ddl_result"},
    {"SBLR_DDL_CREATE_TYPE", 0x0621u, "engine.op.ddl_create_type", "create_type_descriptor", "ddl_result"},
    {"SBLR_DDL_ALTER_TYPE", 0x0622u, "engine.op.ddl_alter_type", "alter_type_descriptor", "ddl_result"},
    {"SBLR_DDL_DROP_TRIGGER", 0x0611u, "engine.op.ddl_drop_trigger", "drop_trigger_descriptor", "ddl_result"},
    {"SBLR_DDL_ALTER_TABLE", 0x0602u, "engine.op.ddl_alter_table", "alter_table_descriptor", "ddl_result"},
    {"SBLR_OBSERVABILITY_SHOW_METRICS", 0x0D2Au, "observability.show_metrics", "observability_show_metrics_descriptor", "observability_show_metrics_result"},
    {"SBLR_CLUSTER_INSPECT_PROVIDER", 0x0B3Du, "cluster.inspect_provider", "none", "cluster_provider_inspection_result"},
}};

struct OptimizerClusterExpected {
  std::string_view opcode;
  std::uint16_t code;
  std::string_view operation;
  std::string_view family;
  std::string_view operand;
  std::string_view result;
  sblr::SblrOpcodeTransactionEffect transaction_effect;
  sblr::SblrOpcodeSecurityClass security_class;
};

constexpr std::array<OptimizerClusterExpected, 17>
    kOptimizerClusterExpected{{
        {"SBLR_DDL_CREATE_VECTOR_RESOURCE_GROUP", 0x0675u,
         "engine.op.ddl_create_vector_resource_group", "vector-serving",
         "vector_resource_group_descriptor", "ddl_result",
         sblr::SblrOpcodeTransactionEffect::local_or_cluster_write,
         sblr::SblrOpcodeSecurityClass::admin_authorized},
        {"SBLR_DDL_ALTER_VECTOR_RESOURCE_GROUP", 0x0676u,
         "engine.op.ddl_alter_vector_resource_group", "vector-serving",
         "vector_resource_group_alter_descriptor", "ddl_result",
         sblr::SblrOpcodeTransactionEffect::local_or_cluster_write,
         sblr::SblrOpcodeSecurityClass::admin_authorized},
        {"SBLR_VECTOR_SERVING_LOAD", 0x0677u,
         "engine.op.vector_serving_load", "vector-serving",
         "vector_serving_load_descriptor", "vector_serving_lease_result",
         sblr::SblrOpcodeTransactionEffect::local_or_cluster_write,
         sblr::SblrOpcodeSecurityClass::object_authorized},
        {"SBLR_VECTOR_SERVING_RELEASE", 0x0678u,
         "engine.op.vector_serving_release", "vector-serving",
         "vector_serving_release_descriptor", "vector_serving_lease_result",
         sblr::SblrOpcodeTransactionEffect::local_or_cluster_write,
         sblr::SblrOpcodeSecurityClass::object_authorized},
        {"SBLR_VECTOR_SERVING_STATUS", 0x0679u,
         "engine.op.vector_serving_status", "vector-serving",
         "vector_serving_status_descriptor", "vector_serving_lease_result",
         sblr::SblrOpcodeTransactionEffect::read,
         sblr::SblrOpcodeSecurityClass::object_authorized},
        {"SBLR_DDL_CREATE_QUERY_WORKLOAD_PROTECTION", 0x067Au,
         "engine.op.ddl_create_query_workload_protection",
         "query_workload_protection",
         "query_workload_protection_create_descriptor",
         "query_workload_protection_result",
         sblr::SblrOpcodeTransactionEffect::local_or_cluster_write,
         sblr::SblrOpcodeSecurityClass::admin_authorized},
        {"SBLR_DDL_ALTER_QUERY_WORKLOAD_PROTECTION", 0x067Bu,
         "engine.op.ddl_alter_query_workload_protection",
         "query_workload_protection",
         "query_workload_protection_alter_descriptor",
         "query_workload_protection_result",
         sblr::SblrOpcodeTransactionEffect::local_or_cluster_write,
         sblr::SblrOpcodeSecurityClass::admin_authorized},
        {"SBLR_DDL_DROP_QUERY_WORKLOAD_PROTECTION", 0x067Cu,
         "engine.op.ddl_drop_query_workload_protection",
         "query_workload_protection",
         "query_workload_protection_drop_descriptor",
         "query_workload_protection_result",
         sblr::SblrOpcodeTransactionEffect::local_or_cluster_write,
         sblr::SblrOpcodeSecurityClass::admin_authorized},
        {"SBLR_QUERY_WORKLOAD_PROTECTION_STATUS", 0x067Du,
         "engine.op.query_workload_protection_status",
         "query_workload_protection",
         "query_workload_protection_status_descriptor",
         "query_workload_protection_status_rowset",
         sblr::SblrOpcodeTransactionEffect::read,
         sblr::SblrOpcodeSecurityClass::object_authorized},
        {"SBLR_QUERY_WORKLOAD_PROTECTION_APPLY", 0x067Eu,
         "engine.op.query_workload_protection_apply",
         "query_workload_protection",
         "query_workload_protection_apply_descriptor",
         "query_workload_protection_action_result",
         sblr::SblrOpcodeTransactionEffect::local_or_cluster_write,
         sblr::SblrOpcodeSecurityClass::internal_only},
        {"SBLR_DDL_CREATE_EXTERNAL_RANGE_MATERIALIZATION", 0x067Fu,
         "engine.op.ddl_create_external_range_materialization",
         "external_range_materialization",
         "external_range_materialization_create_descriptor",
         "external_range_materialization_result",
         sblr::SblrOpcodeTransactionEffect::local_or_cluster_write,
         sblr::SblrOpcodeSecurityClass::admin_authorized},
        {"SBLR_DDL_ALTER_EXTERNAL_RANGE_MATERIALIZATION", 0x0680u,
         "engine.op.ddl_alter_external_range_materialization",
         "external_range_materialization",
         "external_range_materialization_alter_descriptor",
         "external_range_materialization_result",
         sblr::SblrOpcodeTransactionEffect::local_or_cluster_write,
         sblr::SblrOpcodeSecurityClass::admin_authorized},
        {"SBLR_DDL_DROP_EXTERNAL_RANGE_MATERIALIZATION", 0x0681u,
         "engine.op.ddl_drop_external_range_materialization",
         "external_range_materialization",
         "external_range_materialization_drop_descriptor",
         "external_range_materialization_result",
         sblr::SblrOpcodeTransactionEffect::local_or_cluster_write,
         sblr::SblrOpcodeSecurityClass::admin_authorized},
        {"SBLR_EXTERNAL_RANGE_MATERIALIZATION_BUILD", 0x0682u,
         "engine.op.external_range_materialization_build",
         "external_range_materialization",
         "external_range_materialization_build_descriptor",
         "external_range_materialization_result",
         sblr::SblrOpcodeTransactionEffect::management,
         sblr::SblrOpcodeSecurityClass::internal_only},
        {"SBLR_EXTERNAL_RANGE_MATERIALIZATION_STATUS", 0x0683u,
         "engine.op.external_range_materialization_status",
         "external_range_materialization",
         "external_range_materialization_status_descriptor",
         "external_range_materialization_status_rowset",
         sblr::SblrOpcodeTransactionEffect::read,
         sblr::SblrOpcodeSecurityClass::object_authorized},
        {"SBLR_EXTERNAL_RANGE_MATERIALIZATION_RESOLVE_READ", 0x0684u,
         "engine.op.external_range_materialization_resolve_read",
         "external_range_materialization",
         "external_range_materialization_read_resolution_descriptor",
         "external_range_materialization_read_resolution",
         sblr::SblrOpcodeTransactionEffect::read,
         sblr::SblrOpcodeSecurityClass::internal_only},
        {"SBLR_QUERY_PLAN_CONSTRAIN", 0x0E0Au,
         "engine.op.query_plan_constrain", "optimizer-plan",
         "constrained_plan_request", "constrained_plan_result",
         sblr::SblrOpcodeTransactionEffect::read,
         sblr::SblrOpcodeSecurityClass::object_authorized},
    }};

using Bytes = std::vector<std::uint8_t>;
void U32(Bytes* out, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8)
    out->push_back(static_cast<std::uint8_t>(value >> shift));
}
void U64(Bytes* out, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8)
    out->push_back(static_cast<std::uint8_t>(value >> shift));
}
void Store32(Bytes* bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8)
    (*bytes)[offset++] = static_cast<std::uint8_t>(value >> shift);
}
void RefreshCrc(Bytes* bytes) {
  Store32(bytes, bytes->size() - 12,
          sblr::SblrCrc32c(bytes->data(), bytes->size() - 12));
}
Bytes EncodeUncheckedStream(
    const std::vector<sblr::SblrOperationEnvelope>& operations,
    const std::array<std::uint8_t, 16>& package,
    const std::array<std::uint8_t, 16>& registry) {
  Bytes records;
  for (const auto& operation : operations) {
    const auto encoded = sblr::EncodeSblrEnvelope(operation);
    U64(&records, encoded.size());
    records.insert(records.end(), encoded.begin(), encoded.end());
  }
  Bytes bytes{'S', 'B', 'O', 'S', 1, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0};
  U32(&bytes, static_cast<std::uint32_t>(operations.size()));
  U32(&bytes, 0);
  bytes.insert(bytes.end(), package.begin(), package.end());
  bytes.insert(bytes.end(), registry.begin(), registry.end());
  U64(&bytes, records.size());
  bytes.insert(bytes.end(), records.begin(), records.end());
  bytes.insert(bytes.end(), {'S', 'B', 'S', 'T'});
  U32(&bytes, sblr::SblrCrc32c(bytes.data(), bytes.size()));
  U64(&bytes, bytes.size() + 8);
  return bytes;
}

bool HasAcceptedExecutorEvidence(std::string_view opcode) {
  static constexpr std::array<std::string_view, 19> kObservabilityExecutors{{
      "SBLR_OBSERVABILITY_SHOW_VERSION",
      "SBLR_OBSERVABILITY_SHOW_DATABASE",
      "SBLR_OBSERVABILITY_SHOW_SYSTEM",
      "SBLR_OBSERVABILITY_SHOW_CATALOG",
      "SBLR_OBSERVABILITY_SHOW_SESSIONS",
      "SBLR_OBSERVABILITY_SHOW_TRANSACTIONS",
      "SBLR_OBSERVABILITY_SHOW_LOCKS",
      "SBLR_OBSERVABILITY_SHOW_STATEMENTS",
      "SBLR_OBSERVABILITY_SHOW_JOBS",
      "SBLR_OBSERVABILITY_SHOW_MANAGEMENT",
      "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS",
      "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS_EXTENDED",
      "SBLR_OBSERVABILITY_SHOW_ARCHIVE_REPLICATION",
      "SBLR_OBSERVABILITY_SHOW_AGENTS_EXTENDED",
      "SBLR_OBSERVABILITY_SHOW_FILESPACE_EXTENDED",
      "SBLR_OBSERVABILITY_SHOW_DECISION_SERVICE",
      "SBLR_OBSERVABILITY_SHOW_ACCELERATION",
      "SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED",
      "SBLR_OBSERVABILITY_SHOW_METRICS",
  }};
  if (std::find(kObservabilityExecutors.begin(),
                kObservabilityExecutors.end(), opcode) !=
      kObservabilityExecutors.end()) {
    return true;
  }
  return opcode == "SBLR_PACKAGE_BEGIN" ||
         opcode == "SBLR_PACKAGE_END" ||
         opcode == "SBLR_LIFECYCLE_CREATE_DATABASE" ||
         opcode == "SBLR_LIFECYCLE_OPEN_DATABASE" ||
         opcode == "SBLR_LIFECYCLE_ATTACH_DATABASE" ||
         opcode == "SBLR_LIFECYCLE_DETACH_DATABASE" ||
         opcode == "SBLR_LIFECYCLE_ENTER_MAINTENANCE" ||
         opcode == "SBLR_LIFECYCLE_EXIT_MAINTENANCE" ||
         opcode == "SBLR_CLUSTER_INSPECT_PROVIDER";
}
}

int main() {
  int failures = 0;
  for (const auto& expected : kExpected) {
    const bool executor_evidence_accepted =
        HasAcceptedExecutorEvidence(expected.opcode);
    const std::string_view expected_executor =
        expected.executor.empty() ? expected.operation : expected.executor;
    const auto* entry = sblr::LookupSblrOpcode(expected.opcode);
    const bool ok = entry && entry->code == expected.code &&
                    entry->operation_id == expected.operation &&
                    entry->operand_contract == expected.operand &&
                    entry->result_contract == expected.result &&
                    entry->executor_id == expected_executor &&
                    entry->executor_evidence_required &&
                    entry->executor_evidence_accepted ==
                        executor_evidence_accepted &&
                    entry->missing_executor_evidence_diagnostic ==
                        "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
    if (!ok) {
      std::cerr << "IA-01 contract mismatch: " << expected.opcode << '\n';
      ++failures;
    }
    const auto identity = sblr::ValidateSblrOpcodeIdentity(
        expected.code, expected.operation, expected.opcode);
    if (!identity.ok || identity.entry != entry) {
      std::cerr << "IA-01 canonical identity rejected: " << expected.opcode << '\n';
      ++failures;
    }
    const auto wrong_code = sblr::ValidateSblrOpcodeIdentity(
        static_cast<std::uint16_t>(expected.code ^ 0x8000u),
        expected.operation, expected.opcode);
    const auto wrong_operation = sblr::ValidateSblrOpcodeIdentity(
        expected.code, "engine.op.not_admitted", expected.opcode);
    if (wrong_code.ok || wrong_operation.ok ||
        wrong_code.diagnostic_id != "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH" ||
        wrong_operation.diagnostic_id != "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH") {
      std::cerr << "IA-01 malformed identity did not fail closed: "
                << expected.opcode << '\n';
      ++failures;
    }
    sblr::SblrOperationEnvelope envelope;
    envelope.opcode_code = expected.code;
    envelope.operation_id = expected.operation;
    envelope.opcode = expected.opcode;
    envelope.requires_security_context = true;
    envelope.requires_transaction_context = entry && entry->requires_transaction_context;
    envelope.requires_cluster_authority = entry && entry->requires_cluster_authority;
    const auto admission = sblr::ValidateSblrOpcodeForEnvelope(envelope);
    if ((executor_evidence_accepted &&
         (!admission.ok || admission.entry != entry)) ||
        (!executor_evidence_accepted &&
         (admission.ok || admission.entry != entry ||
          admission.diagnostic_id != "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" ||
          admission.detail != "executor_evidence_not_accepted:" +
                                  std::string(expected.operation)))) {
      std::cerr << "IA-01 executor evidence state mismatch: "
                << expected.opcode << '\n';
      ++failures;
    }
  }

  for (const auto& expected : kOptimizerClusterExpected) {
    const auto* entry = sblr::LookupSblrOpcode(expected.opcode);
    const bool exact_contract =
        entry != nullptr && entry->code == expected.code &&
        entry->operation_id == expected.operation &&
        entry->family == expected.family &&
        entry->operand_contract == expected.operand &&
        entry->result_contract == expected.result &&
        entry->executor_id == expected.operation &&
        entry->support == sblr::SblrOpcodeSupport::implemented &&
        entry->transaction_effect == expected.transaction_effect &&
        entry->security_class == expected.security_class &&
        entry->requires_transaction_context &&
        !entry->requires_cluster_authority &&
        entry->executor_evidence_required &&
        !entry->executor_evidence_accepted &&
        entry->missing_executor_evidence_diagnostic ==
            "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
    if (!exact_contract) {
      std::cerr << "IA-01 optimizer/cluster contract mismatch: "
                << expected.opcode << '\n';
      ++failures;
      continue;
    }
    const auto identity = sblr::ValidateSblrOpcodeIdentity(
        expected.code, expected.operation, expected.opcode);
    if (!identity.ok || identity.entry != entry ||
        sblr::LookupSblrOpcodeCode(expected.code) != entry) {
      std::cerr << "IA-01 optimizer/cluster identity rejected: "
                << expected.opcode << '\n';
      ++failures;
    }
    sblr::SblrOperationEnvelope envelope;
    envelope.opcode_code = expected.code;
    envelope.operation_id = expected.operation;
    envelope.opcode = expected.opcode;
    envelope.requires_security_context = true;
    envelope.requires_transaction_context = true;
    const auto admission = sblr::ValidateSblrOpcodeForEnvelope(envelope);
    if (admission.ok || admission.entry != entry ||
        admission.diagnostic_id !=
            "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" ||
        admission.detail !=
            "executor_evidence_not_accepted:" +
                std::string(expected.operation)) {
      std::cerr << "IA-01 optimizer/cluster fail-closed state mismatch: "
                << expected.opcode << '\n';
      ++failures;
    }
  }

  // SEARCH_KEY: SBLR-DML-PLAN-IMPORT-ROWS-ZERO-GREY-V1
  // This additive identity has no code-zero, mnemonic, operation, or
  // neighboring-import compatibility alias.
  {
    constexpr std::uint16_t kCode = 0x0319u;
    constexpr std::string_view kOperation = "dml.plan_import_rows";
    constexpr std::string_view kOpcode = "SBLR_DML_PLAN_IMPORT_ROWS";
    const auto* entry = sblr::LookupSblrOpcode(kOpcode);
    std::size_t opcode_rows = 0;
    std::size_t operation_rows = 0;
    std::size_t code_rows = 0;
    for (const auto& candidate : sblr::StaticSblrOpcodeRegistry()) {
      opcode_rows += candidate.opcode == kOpcode ? 1u : 0u;
      operation_rows += candidate.operation_id == kOperation ? 1u : 0u;
      code_rows += candidate.code == kCode ? 1u : 0u;
    }
    const bool exact_contract =
        entry != nullptr && entry->code == kCode &&
        entry->operation_id == kOperation && entry->opcode == kOpcode &&
        entry->family == "data-mutation" &&
        entry->category == sblr::SblrOpcodeCategory::dml &&
        entry->support == sblr::SblrOpcodeSupport::implemented &&
        entry->transaction_effect == sblr::SblrOpcodeTransactionEffect::read &&
        entry->security_class ==
            sblr::SblrOpcodeSecurityClass::object_authorized &&
        entry->requires_security_context &&
        entry->requires_transaction_context &&
        !entry->requires_cluster_authority && !entry->cluster_private &&
        entry->refusal_diagnostic ==
            "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN" &&
        entry->operand_contract == "import_rows_plan_descriptor" &&
        entry->result_contract == "import_plan_result" &&
        entry->executor_id == kOperation && entry->executor_evidence_required &&
        entry->executor_evidence_accepted &&
        entry->missing_executor_evidence_diagnostic ==
            "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
        opcode_rows == 1u && operation_rows == 1u && code_rows == 1u &&
        sblr::LookupSblrOperation(kOperation) == entry &&
        sblr::LookupSblrOpcodeCode(kCode) == entry;
    if (!exact_contract) {
      std::cerr << "SBLR-PLAN-IMPORT-003 exact identity contract mismatch\n";
      ++failures;
    }

    const auto exact =
        sblr::ValidateSblrOpcodeIdentity(kCode, kOperation, kOpcode);
    const auto code_zero =
        sblr::ValidateSblrOpcodeIdentity(0u, kOperation, kOpcode);
    const auto wrong_code =
        sblr::ValidateSblrOpcodeIdentity(0x0318u, kOperation, kOpcode);
    const auto wrong_mnemonic = sblr::ValidateSblrOpcodeIdentity(
        kCode, kOperation, "SBLR_DML_EXECUTE_IMPORT_ROWS");
    const auto wrong_operation = sblr::ValidateSblrOpcodeIdentity(
        kCode, "dml.execute_import_rows", kOpcode);
    const auto neighbor_substitution = sblr::ValidateSblrOpcodeIdentity(
        0x0316u, kOperation, "SBLR_DML_EXECUTE_IMPORT_ROWS");
    if (!exact.ok || exact.entry != entry || code_zero.ok || wrong_code.ok ||
        wrong_mnemonic.ok || wrong_operation.ok || neighbor_substitution.ok ||
        code_zero.diagnostic_id != "SBLR.OPCODE_INVALID" ||
        wrong_code.diagnostic_id != "SBLR.OPCODE_INVALID" ||
        wrong_mnemonic.diagnostic_id != "SBLR.OPCODE_INVALID" ||
        wrong_operation.diagnostic_id != "SBLR.OPCODE_INVALID" ||
        neighbor_substitution.diagnostic_id != "SBLR.OPCODE_INVALID") {
      std::cerr << "SBLR-PLAN-IMPORT-003 identity alias did not fail closed\n";
      ++failures;
    }

    sblr::SblrOperationEnvelope envelope;
    envelope.opcode_code = kCode;
    envelope.operation_id = std::string(kOperation);
    envelope.opcode = std::string(kOpcode);
    envelope.requires_security_context = true;
    envelope.requires_transaction_context = true;
    const auto admission = sblr::ValidateSblrOpcodeForEnvelope(envelope);
    if (!admission.ok || admission.entry != entry) {
      std::cerr << "SBLR-PLAN-IMPORT-003 exact registry admission refused\n";
      ++failures;
    }

    auto wrong_code_envelope = envelope;
    wrong_code_envelope.opcode_code = 0x0318u;
    auto wrong_identity_and_context_envelope = wrong_code_envelope;
    wrong_identity_and_context_envelope.requires_security_context = false;
    wrong_identity_and_context_envelope.requires_transaction_context = false;
    auto wrong_mnemonic_envelope = envelope;
    wrong_mnemonic_envelope.opcode = "SBLR_DML_EXECUTE_IMPORT_ROWS";
    auto wrong_operation_envelope = envelope;
    wrong_operation_envelope.operation_id = "dml.not_admitted_import_plan";
    auto missing_security_envelope = envelope;
    missing_security_envelope.requires_security_context = false;
    auto missing_security_and_transaction_envelope =
        missing_security_envelope;
    missing_security_and_transaction_envelope.requires_transaction_context =
        false;
    auto missing_transaction_envelope = envelope;
    missing_transaction_envelope.requires_transaction_context = false;
    const auto wrong_code_admission =
        sblr::ValidateSblrOpcodeForEnvelope(wrong_code_envelope);
    const auto wrong_identity_and_context_admission =
        sblr::ValidateSblrOpcodeForEnvelope(
            wrong_identity_and_context_envelope);
    const auto wrong_mnemonic_admission =
        sblr::ValidateSblrOpcodeForEnvelope(wrong_mnemonic_envelope);
    const auto wrong_operation_admission =
        sblr::ValidateSblrOpcodeForEnvelope(wrong_operation_envelope);
    const auto missing_security_admission =
        sblr::ValidateSblrOpcodeForEnvelope(missing_security_envelope);
    const auto missing_security_and_transaction_admission =
        sblr::ValidateSblrOpcodeForEnvelope(
            missing_security_and_transaction_envelope);
    const auto missing_transaction_admission =
        sblr::ValidateSblrOpcodeForEnvelope(missing_transaction_envelope);
    if (wrong_code_admission.ok || wrong_identity_and_context_admission.ok ||
        wrong_mnemonic_admission.ok ||
        wrong_operation_admission.ok || missing_security_admission.ok ||
        missing_security_and_transaction_admission.ok ||
        missing_transaction_admission.ok ||
        wrong_code_admission.diagnostic_id != "SBLR.OPCODE_INVALID" ||
        wrong_identity_and_context_admission.diagnostic_id !=
            "SBLR.OPCODE_INVALID" ||
        wrong_mnemonic_admission.diagnostic_id != "SBLR.OPCODE_INVALID" ||
        wrong_operation_admission.diagnostic_id != "SBLR.OPCODE_INVALID" ||
        missing_security_admission.diagnostic_id !=
            "SECURITY.ACCESS_DENIED" ||
        missing_security_and_transaction_admission.diagnostic_id !=
            "SECURITY.ACCESS_DENIED" ||
        missing_transaction_admission.diagnostic_id !=
            "MGA.TRANSACTION_INVALID") {
      std::cerr <<
          "SBLR-PLAN-IMPORT-003 canonical admission diagnostic mismatch\n";
      ++failures;
    }
  }

  // A canonical operation id disambiguates bridge operations that share the
  // same wire code and mnemonic.  Exact registered identities must pass;
  // borrowing either field from a different alias must still fail closed.
  const auto* bridge_connect = sblr::LookupSblrOperation("bridge.connect");
  const auto* bridge_attach = sblr::LookupSblrOperation("bridge.attach");
  const auto* bridge_authenticate =
      sblr::LookupSblrOperation("bridge.authenticate");
  if (bridge_connect == nullptr || bridge_attach == nullptr ||
      bridge_authenticate == nullptr ||
      bridge_connect->operation_id == bridge_attach->operation_id ||
      bridge_connect->code != bridge_attach->code ||
      bridge_connect->opcode != bridge_attach->opcode) {
    std::cerr << "IA-01 shared bridge identity fixture drifted\n";
    ++failures;
  } else {
    const auto connect_identity = sblr::ValidateSblrOpcodeIdentity(
        bridge_connect->code, bridge_connect->operation_id,
        bridge_connect->opcode);
    const auto attach_identity = sblr::ValidateSblrOpcodeIdentity(
        bridge_attach->code, bridge_attach->operation_id,
        bridge_attach->opcode);
    const auto mismatched_mnemonic = sblr::ValidateSblrOpcodeIdentity(
        bridge_attach->code, bridge_attach->operation_id,
        bridge_authenticate->opcode);
    const auto mismatched_code = sblr::ValidateSblrOpcodeIdentity(
        bridge_authenticate->code, bridge_connect->operation_id,
        bridge_connect->opcode);
    const auto unregistered_alias = sblr::ValidateSblrOpcodeIdentity(
        bridge_connect->code, "bridge.open_channel_alias",
        bridge_connect->opcode);
    if (!connect_identity.ok || connect_identity.entry != bridge_connect ||
        !attach_identity.ok || attach_identity.entry != bridge_attach ||
        mismatched_mnemonic.ok || mismatched_code.ok ||
        unregistered_alias.ok ||
        mismatched_mnemonic.diagnostic_id !=
            "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH" ||
        mismatched_code.diagnostic_id !=
            "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH" ||
        unregistered_alias.diagnostic_id !=
            "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH") {
      std::cerr << "IA-01 shared-code operation identity did not fail closed\n";
      ++failures;
    }
  }

  constexpr std::string_view package_uuid =
      "018f1234-5678-7abc-8def-0123456789ab";
  constexpr std::string_view registry_uuid =
      "018f4321-8765-7cba-8fed-ba9876543210";
  constexpr std::string_view parser_uuid =
      "018faaaa-bbbb-7ccc-8ddd-eeeeeeeeeeee";
  const std::array<std::uint8_t, 16> package_bytes{
      0x01, 0x8f, 0x12, 0x34, 0x56, 0x78, 0x7a, 0xbc,
      0x8d, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab};
  const std::array<std::uint8_t, 16> registry_bytes{
      0x01, 0x8f, 0x43, 0x21, 0x87, 0x65, 0x7c, 0xba,
      0x8f, 0xed, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
  const auto frame = [&](bool begin) {
    auto operation = sblr::MakeSblrEnvelope(
        begin ? "engine.op.package_begin" : "engine.op.package_end",
        begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END",
        begin ? "ia01.package.begin" : "ia01.package.end");
    operation.opcode_code = begin ? 0x0001u : 0x0002u;
    operation.result_shape = "void";
    operation.diagnostic_shape = "diagnostic_vector";
    operation.parser_package_uuid = parser_uuid;
    operation.registry_snapshot_uuid = registry_uuid;
    operation.parser_resolved_names_to_uuids = true;
    sblr::SblrOperand operand;
    operand.ordinal = 1;
    operand.type = begin ? "package.header" : "package.footer";
    operand.name = "package_descriptor";
    operand.value_kind = sblr::SblrValueKind::descriptor_ref;
    operand.value_body.assign(package_bytes.begin(), package_bytes.end());
    operation.operands.push_back(std::move(operand));
    return operation;
  };
  sblr::SblrOpcodeStream package;
  package.package_descriptor_uuid = package_uuid;
  package.registry_snapshot_uuid = registry_uuid;
  package.operations = {frame(true), frame(false)};
  const auto bytes = sblr::EncodeSblrOpcodeStream(package);
  if (bytes.empty()) {
    std::cerr << "CSC-TEST-002317 canonical atomic package did not encode\n";
    ++failures;
  } else {
    sblr::SblrOpcodeStreamAdmission admitted;
    admitted.admitted_registry_snapshot_uuid = registry_uuid;
    admitted.authenticated = true;
    admitted.descriptor_class_accepted = true;
    admitted.gateway_pass_through = true;
    admitted.executor_evidence_accepted = true;
    const std::string_view encoded(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto canonical = sblr::AdmitSblrOpcodeStream(encoded, admitted);
    if (!canonical.ok || canonical.stream.operations.size() != 2) {
      std::cerr << "CSC-TEST-002317/CSC-TEST-002321 canonical atomic pair refused: "
                << canonical.diagnostic_id << ' ' << canonical.detail << '\n';
      ++failures;
    }
    auto malformed = bytes;
    malformed[16] = 1;
    const auto malformed_result = sblr::DecodeSblrOpcodeStream(
        {reinterpret_cast<const char*>(malformed.data()), malformed.size()});
    if (malformed_result.ok ||
        malformed_result.diagnostic_id != "SBLR.OPERAND_INVALID") {
      std::cerr << "CSC-TEST-002318/CSC-TEST-002322 malformed count did not fail closed\n";
      ++failures;
    }
    auto missing_evidence = admitted;
    missing_evidence.executor_evidence_accepted = false;
    const auto evidence_result =
        sblr::AdmitSblrOpcodeStream(encoded, missing_evidence);
    if (evidence_result.ok || evidence_result.diagnostic_id !=
                                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING") {
      std::cerr << "CSC-TEST-002319/CSC-TEST-002323 missing evidence did not fail closed\n";
      ++failures;
    }
    auto cancelled = admitted;
    cancelled.cancelled = true;
    const auto cancelled_result = sblr::AdmitSblrOpcodeStream(encoded, cancelled);
    if (cancelled_result.ok ||
        cancelled_result.diagnostic_id != "PROCESS.CANCELLED") {
      std::cerr << "CSC-TEST-002320/CSC-TEST-002324 cancellation did not remain atomic\n";
      ++failures;
    }

    const auto expect_decode = [&](const Bytes& candidate,
                                   std::string_view diagnostic,
                                   std::string_view case_name) {
      const auto result = sblr::DecodeSblrOpcodeStream(
          {reinterpret_cast<const char*>(candidate.data()), candidate.size()});
      if (result.ok || result.diagnostic_id != diagnostic) {
        std::cerr << "CSC-TEST-002318/CSC-TEST-002322 " << case_name
                  << " expected " << diagnostic << " got "
                  << result.diagnostic_id << '\n';
        ++failures;
      }
    };
    auto bad_version = bytes;
    bad_version[4] = 2;
    RefreshCrc(&bad_version);
    expect_decode(bad_version, "SBLR.OPERAND_INVALID", "unknown version");

    auto bad_crc = bytes;
    bad_crc[bad_crc.size() - 12] ^= 1;
    expect_decode(bad_crc, "SBLR.OPERAND_INVALID", "CRC mismatch");

    auto trailing = bytes;
    trailing.push_back(0);
    expect_decode(trailing, "SBLR.OPERAND_INVALID", "trailing byte");

    auto nested = EncodeUncheckedStream(
        {frame(true), frame(true), frame(false)}, package_bytes, registry_bytes);
    expect_decode(nested, "SBLR.OPERAND_INVALID", "nested package");

    auto mismatched_end = frame(false);
    mismatched_end.operands[0].value_body.back() ^= 1;
    auto mismatched_descriptor = EncodeUncheckedStream(
        {frame(true), mismatched_end}, package_bytes, registry_bytes);
    expect_decode(mismatched_descriptor, "DATATYPE.DESCRIPTOR_INVALID",
                  "mismatched footer descriptor");

    auto stale_member = frame(false);
    stale_member.registry_snapshot_uuid =
        "018f4321-8765-7cba-8fed-ba9876543211";
    auto mismatched_registry = EncodeUncheckedStream(
        {frame(true), stale_member}, package_bytes, registry_bytes);
    expect_decode(mismatched_registry, "DATATYPE.DESCRIPTOR_INVALID",
                  "member registry mismatch");

    auto descriptor_and_auth = admitted;
    descriptor_and_auth.descriptor_class_accepted = false;
    descriptor_and_auth.authenticated = false;
    const auto descriptor_precedence =
        sblr::AdmitSblrOpcodeStream(encoded, descriptor_and_auth);
    if (descriptor_precedence.ok || descriptor_precedence.diagnostic_id !=
                                        "DATATYPE.DESCRIPTOR_INVALID") {
      std::cerr << "CSC-TEST-002318/CSC-TEST-002322 descriptor/security precedence changed\n";
      ++failures;
    }
    auto resource_and_cancel = admitted;
    resource_and_cancel.resource_budget_available = false;
    resource_and_cancel.cancelled = true;
    const auto resource_precedence =
        sblr::AdmitSblrOpcodeStream(encoded, resource_and_cancel);
    if (resource_precedence.ok || resource_precedence.diagnostic_id !=
                                      "RESOURCE.BUDGET_EXCEEDED") {
      std::cerr << "CSC-TEST-002320/CSC-TEST-002324 resource/cancel precedence changed\n";
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
