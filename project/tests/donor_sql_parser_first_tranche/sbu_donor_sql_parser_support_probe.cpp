// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbu_cassandra_parser_support.hpp"
#include "sbu_clickhouse_parser_support.hpp"
#include "sbu_duckdb_parser_support.hpp"
#include "sbu_firebird_parser_support.hpp"
#include "sbu_mariadb_parser_support.hpp"
#include "sbu_mongodb_parser_support.hpp"
#include "sbu_neo4j_parser_support.hpp"
#include "sbu_opensearch_parser_support.hpp"
#include "sbu_dolt_parser_support.hpp"
#include "sbu_apache_ignite_parser_support.hpp"
#include "sbu_tikv_parser_support.hpp"
#include "sbu_foundationdb_parser_support.hpp"
#include "sbu_immudb_parser_support.hpp"
#include "sbu_xtdb_parser_support.hpp"
#include "sbu_mysql_parser_support.hpp"
#include "sbu_opensearch_sql_ppl_parser_support.hpp"
#include "sbu_postgresql_parser_support.hpp"
#include "sbu_redis_parser_support.hpp"
#include "sbu_influxdb_parser_support.hpp"
#include "sbu_milvus_parser_support.hpp"
#include "sbu_sqlite_parser_support.hpp"
#include "sbu_tidb_parser_support.hpp"
#include "sbu_vitess_parser_support.hpp"
#include "sbu_cockroachdb_parser_support.hpp"
#include "sbu_yugabytedb_parser_support.hpp"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kManagementOperations[] = {
    "describe_package",
    "validate_package",
    "get_capabilities",
    "classify_management_request",
    "render_management_diagnostic",
    "setup_pseudo_server",
    "alter_pseudo_server",
    "drop_pseudo_server",
    "validate_pseudo_server",
    "list_pseudo_servers",
    "setup_database",
    "alter_database",
    "drop_database",
    "rename_database",
    "attach_database",
    "detach_database",
    "validate_database",
    "create_catalog_projection",
    "refresh_catalog_projection",
    "validate_catalog_projection",
    "seed_catalog_rowsets",
    "export_catalog_projection",
    "install_domain_emulation",
    "refresh_domain_emulation",
    "validate_domain_emulation",
    "install_helper_routines",
    "validate_helper_routines",
    "normalize_login_identity",
    "validate_auth_evidence",
    "add_user",
    "alter_user",
    "drop_user",
    "map_external_identity",
    "validate_user_mapping",
    "create_role",
    "alter_role",
    "drop_role",
    "grant_role",
    "revoke_role",
    "grant_privilege",
    "revoke_privilege",
    "set_security_policy",
    "export_security_policy",
    "validate_security_policy",
    "set_session_option",
    "set_database_option",
    "set_server_option",
    "run_admin_command",
    "classify_external_authority_command",
    "render_status_report",
    "prepare_migration_context",
    "apply_migration_batch",
    "finalize_migration_context",
    "abort_migration_context",
    "start_replication_channel",
    "stop_replication_channel",
    "apply_replication_event",
    "get_replication_status",
    "validate_emulation_state",
    "get_operational_status",
    "collect_support_bundle",
    "export_metadata_snapshot",
    "import_metadata_snapshot",
    "retire_emulation_profile",
};

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool ExpectField(std::string_view json,
                 std::string_view field,
                 std::string_view value) {
  return Expect(Contains(json, "\"" + std::string(field) + "\":\"" +
                                  std::string(value) + "\""),
                "missing source-retention field: " + std::string(field));
}

bool ExpectBool(std::string_view json,
                std::string_view field,
                bool value) {
  return Expect(Contains(json, "\"" + std::string(field) + "\":" +
                                  std::string(value ? "true" : "false")),
                "missing source-retention bool: " + std::string(field));
}

template <typename Result>
bool ExpectOk(const Result& result, std::string_view label) {
  if (result.ok) return true;
  std::cerr << label << " failed: " << result.message_vector_json << '\n';
  return false;
}

template <typename Result>
bool ExpectDiagnostic(const Result& result,
                      std::string_view code,
                      std::string_view label) {
  if (!result.ok && Contains(result.message_vector_json, code)) return true;
  std::cerr << label << " diagnostic mismatch: ok=" << result.ok
            << " payload=" << result.payload
            << " message_vector_json=" << result.message_vector_json << '\n';
  return false;
}

bool ExpectProceduralSourceRetentionPayload(
    std::string_view label,
    std::string_view payload,
    std::initializer_list<std::string_view> redacted_markers) {
  const bool firebird_payload = Contains(payload, "\"dialect\":\"firebird\"");
  bool ok = true;
  ok &= Expect(Contains(payload, "SBLRExecutionEnvelope.v3"),
               std::string(label) + " missing SBLR execution envelope");
  ok &= Expect(Contains(payload, "\"procedural_body_source_retention_evidence\":{"),
               std::string(label) + " missing procedural source-retention evidence");
  ok &= ExpectBool(payload, "raw_sql_body_embedded_in_sblr_envelope", false);
  ok &= ExpectBool(payload, "body_text_redacted_from_parser_evidence", true);
  ok &= ExpectBool(payload, "uuid_binding_required", true);
  ok &= ExpectField(payload, "execution_authority", "scratchbird_engine_sblr");
  ok &= ExpectBool(payload, "donor_sql_executed", false);
  ok &= ExpectBool(payload, "parser_transaction_authority", false);
  ok &= ExpectBool(payload, "parser_storage_authority", false);
  ok &= ExpectBool(payload, "parser_bound_sblr_body_instruction_stream",
                   firebird_payload);
  ok &= ExpectBool(payload, "uuid_dependency_bindings_bound",
                   firebird_payload);
  ok &= ExpectField(payload, "body_lowering_status",
                    firebird_payload
                        ? "parser_bound_sblr_instruction_stream_encoded"
                        : "lowering_pending");
  if (firebird_payload) {
    ok &= Expect(Contains(payload,
                          "\"firebird_psql_functional_encoding_evidence\":{"),
                 std::string(label) +
                     " missing Firebird PSQL functional encoding evidence");
    ok &= ExpectField(payload, "functional_encoding_status",
                      "firebird_psql_parser_bound_sblr_encoded");
  }
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready");
  for (const auto marker : redacted_markers) {
    ok &= Expect(!Contains(payload, marker),
                 std::string(label) +
                     " leaked routine/body marker into UDR SBLR payload: " +
                     std::string(marker));
  }
  return ok;
}

bool ExpectDatatypeDescriptorPayload(
    std::string_view label,
    std::string_view payload,
    std::initializer_list<std::string_view> redacted_markers) {
  bool ok = true;
  ok &= Expect(Contains(payload, "SBLRExecutionEnvelope.v3"),
               std::string(label) + " missing SBLR execution envelope");
  ok &= Expect(Contains(payload, "\"datatype_descriptor_evidence\":{"),
               std::string(label) + " missing datatype descriptor evidence");
  ok &= ExpectField(payload, "evidence_contract",
                    "donor_datatype_descriptor_evidence.v1");
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required");
  ok &= Expect(Contains(payload, "\"datatype_reference_count\":1") ||
                   Contains(payload, "\"datatype_surface_matched\":true"),
               std::string(label) + " missing datatype surface proof");
  ok &= ExpectBool(payload, "catalog_descriptor_required", true);
  ok &= ExpectBool(payload, "wire_literal_cast_comparison_required", true);
  ok &= Expect(Contains(payload, "\"collation_charset_profile_required\":true") ||
                   Contains(payload, "\"donor_datatype_profile_required\":true"),
               std::string(label) + " missing datatype profile requirement");
  ok &= ExpectBool(payload, "generic_text_fallback_allowed", false);
  ok &= ExpectBool(payload, "parser_storage_authority", false);
  ok &= ExpectBool(payload, "parser_transaction_authority", false);
  ok &= ExpectBool(payload, "donor_sql_executed", false);
  ok &= ExpectField(payload, "exactness_status",
                    "descriptor_surface_recorded_exactness_proof_pending");
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready");
  ok &= ExpectField(payload, "datatype_exactness_status",
                    "surface_cataloged_exactness_proof_pending");
  for (const auto marker : redacted_markers) {
    ok &= Expect(!Contains(payload, marker),
                 std::string(label) +
                     " leaked DDL marker into UDR SBLR payload: " +
                     std::string(marker));
  }
  return ok;
}

bool MutatingManagementOperation(std::string_view operation_name) {
  if (operation_name == "classify_external_authority_command") return false;
  if (operation_name == "describe_package" ||
      operation_name == "validate_package" ||
      operation_name == "get_capabilities" ||
      operation_name == "classify_management_request" ||
      operation_name == "render_management_diagnostic" ||
      operation_name == "list_pseudo_servers" ||
      operation_name == "normalize_login_identity" ||
      operation_name == "render_status_report" ||
      operation_name == "get_replication_status" ||
      operation_name == "get_operational_status" ||
      operation_name == "collect_support_bundle") {
    return false;
  }
  if (operation_name.starts_with("validate_") ||
      operation_name.starts_with("export_")) {
    return false;
  }
  return true;
}

template <typename InventoryFn, typename RequestFn>
bool ManagementAbiChecks(std::string_view label,
                         std::string_view package_uuid,
                         std::string_view exact_file_effects_key,
                         std::string_view missing_context_code,
                         std::string_view security_denied_code,
                         InventoryFn inventory_fn,
                         RequestFn request_fn) {
  const auto inventory = inventory_fn("release_evidence");
  if (!ExpectOk(inventory, std::string(label) + " management inventory")) {
    return false;
  }
  if (!Expect(Contains(inventory.payload, "\"routine_count\":64") &&
                  Contains(inventory.payload, "\"native_sbsql_excluded\":true") &&
                  Contains(inventory.payload, "\"parser_authority\":false") &&
                  Contains(inventory.payload, "\"engine_authorizes_before_udr\":true") &&
                  Contains(inventory.payload, "\"mga_transaction_authority\":\"scratchbird_engine\"") &&
                  Contains(inventory.payload, exact_file_effects_key) &&
                  !Contains(inventory.payload, "TODO") &&
                  !Contains(inventory.payload, "STUB") &&
                  !Contains(inventory.payload, "PLACEHOLDER") &&
                  !Contains(inventory.payload, "NOT_IMPLEMENTED"),
              std::string(label) + " management inventory payload mismatch")) {
    return false;
  }

  const std::string trusted_context =
      "engine_context=trusted;package_uuid=" + std::string(package_uuid) +
      ";request_uuid=019e13c0-1111-7000-8000-000000000001"
      ";operation_policy_ref=019e13c0-2222-7000-8000-000000000002"
      ";transaction_uuid=019e13c0-3333-7000-8000-000000000003";
  const std::string missing_tx_context =
      "engine_context=trusted;package_uuid=" + std::string(package_uuid) +
      ";request_uuid=019e13c0-1111-7000-8000-000000000001"
      ";operation_policy_ref=019e13c0-2222-7000-8000-000000000002";

  if (!ExpectDiagnostic(request_fn("setup_database", missing_tx_context),
                        missing_context_code,
                        std::string(label) + " management missing transaction")) {
    return false;
  }
  if (!ExpectDiagnostic(request_fn("setup_database",
                                   "engine_context=untrusted;package_uuid=x"),
                        security_denied_code,
                        std::string(label) + " management untrusted context")) {
    return false;
  }

  for (const auto operation : kManagementOperations) {
    const auto result = request_fn(operation, trusted_context);
    if (!ExpectOk(result, std::string(label) + " management " + std::string(operation))) {
      return false;
    }
    const bool mutates = MutatingManagementOperation(operation);
    if (!Expect(Contains(result.payload, "\"operation_name\":\"" + std::string(operation) + "\"") &&
                    Contains(result.payload, "\"engine_authorized_context\":true") &&
                    Contains(result.payload, "\"package_policy_can_only_tighten_engine_decision\":true") &&
                    Contains(result.payload, "\"parser_authority\":false") &&
                    Contains(result.payload, "\"parser_selected_package_authority\":false") &&
                    Contains(result.payload, "\"sbsql_management_route\":false") &&
                    Contains(result.payload, "\"native_sbsql_excluded\":true") &&
                    Contains(result.payload, "\"mga_transaction_authority\":\"scratchbird_engine\"") &&
                    Contains(result.payload, "\"requires_mga_transaction\":" +
                                             std::string(mutates ? "true" : "false")) &&
                    Contains(result.payload, exact_file_effects_key) &&
                    !Contains(result.payload, "TODO") &&
                    !Contains(result.payload, "STUB") &&
                    !Contains(result.payload, "PLACEHOLDER") &&
                    !Contains(result.payload, "NOT_IMPLEMENTED"),
                std::string(label) + " management payload mismatch for " +
                    std::string(operation))) {
      return false;
    }
    if (operation == "classify_external_authority_command") {
      if (!Expect(Contains(result.payload,
                           "\"result_class\":\"REFUSED_EXTERNAL_AUTHORITY\"") &&
                      Contains(result.payload, "\"exact_refusal\":true"),
                  std::string(label) + " external authority refusal mismatch")) {
        return false;
      }
    }
  }
  return true;
}

bool FirebirdUdrChecks() {
  using namespace scratchbird::udr::firebird_parser_support;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  if (!ExpectOk(sbu_firebird_validate_syntax("select 1", "firebird"),
                "firebird validate")) return false;
  if (!ExpectDiagnostic(sbu_firebird_parse_to_sblr("select 1", ""),
                        "UDR.FIREBIRD.CONTEXT_MISSING",
                        "firebird missing context")) return false;
  const auto parsed = sbu_firebird_parse_to_sblr("select 1", trusted);
  if (!ExpectOk(parsed, "firebird parse_to_sblr")) return false;
  if (!Expect(Contains(parsed.payload, "SBLRExecutionEnvelope.v3") &&
                  !Contains(parsed.payload, "select 1"),
              "firebird parse_to_sblr payload mismatch")) return false;
  const auto ddl_sblr = sbu_firebird_parse_to_sblr(
      "create table udr_fb_sentinel_type_table (udr_fb_col_int integer, udr_fb_col_v varchar(40), udr_fb_col_ts timestamp, udr_fb_col_blob blob)",
      trusted);
  if (!ExpectOk(ddl_sblr, "firebird DDL datatype parse_to_sblr")) return false;
  if (!ExpectDatatypeDescriptorPayload(
          "firebird DDL datatype parse_to_sblr", ddl_sblr.payload,
          {"udr_fb_sentinel_type_table", "udr_fb_col_int", "udr_fb_col_v",
           "udr_fb_col_ts", "udr_fb_col_blob"})) {
    return false;
  }
  const auto procedure_sblr = sbu_firebird_parse_to_sblr(
      "create procedure udr_fb_retention_proc as begin post_event 'udr_fb_secret_body'; end",
      trusted);
  if (!ExpectOk(procedure_sblr, "firebird procedure parse_to_sblr")) return false;
  if (!ExpectProceduralSourceRetentionPayload(
          "firebird procedure parse_to_sblr", procedure_sblr.payload,
          {"udr_fb_retention_proc", "udr_fb_secret_body", "post_event"})) {
    return false;
  }
  const auto descriptor = sbu_firebird_package_descriptor();
  if (!Expect(descriptor.package_uuid == kSbuFirebirdPackageUuid &&
                  descriptor.package_name == kSbuFirebirdPackageName &&
                  descriptor.trusted_cpp && descriptor.entrypoints.size() >= 11,
              "firebird package descriptor mismatch")) return false;
  if (!Expect(descriptor.init != nullptr &&
                  descriptor.init(kSbuFirebirdPackageUuid).ok,
              "firebird package lifecycle mismatch")) return false;
  if (!ManagementAbiChecks(
          "firebird", kSbuFirebirdPackageUuid,
          "\"real_firebird_file_effects\":false",
          "UDR.FIREBIRD.CONTEXT_MISSING",
          "UDR.FIREBIRD.SECURITY_DENIED",
          sbu_firebird_management_operation_inventory,
          sbu_firebird_management_package_request)) {
    return false;
  }
  return true;
}

bool MysqlUdrChecks() {
  using namespace scratchbird::udr::mysql_parser_support;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view trusted_catalog =
      "engine_context=trusted;catalog_uuid=11111111-2222-3333-4444-555555555555";
  if (!ExpectOk(sbu_mysql_validate_syntax("select 1", "mysql"),
                "mysql validate")) return false;
  if (!ExpectDiagnostic(sbu_mysql_validate_syntax("select 1", "wrong_profile"),
                        "UDR.MYSQL.PROFILE_MISMATCH",
                        "mysql profile mismatch")) return false;
  if (!ExpectDiagnostic(sbu_mysql_parse_to_sblr("select 1", ""),
                        "UDR.MYSQL.CONTEXT_MISSING",
                        "mysql missing context")) return false;
  if (!ExpectDiagnostic(sbu_mysql_parse_to_sblr(
                            "select 1", "engine_context=untrusted;resolver=uuid"),
                        "UDR.MYSQL.SECURITY_DENIED",
                        "mysql untrusted context")) return false;
  const auto parsed = sbu_mysql_parse_to_sblr("select 1", trusted);
  if (!ExpectOk(parsed, "mysql parse_to_sblr")) return false;
  if (!Expect(Contains(parsed.payload, "SBLRExecutionEnvelope.v3") &&
                  Contains(parsed.payload, "\"dialect\":\"mysql\"") &&
                  !Contains(parsed.payload, "select 1"),
              "mysql parse_to_sblr payload mismatch")) return false;
  const auto ddl_sblr = sbu_mysql_parse_to_sblr(
      "create table udr_mysql_sentinel_type_table (udr_mysql_col_int int, udr_mysql_col_v varchar(40), udr_mysql_col_dec decimal(10,2), udr_mysql_col_ts timestamp, udr_mysql_col_json json)",
      trusted);
  if (!ExpectOk(ddl_sblr, "mysql DDL datatype parse_to_sblr")) return false;
  if (!ExpectDatatypeDescriptorPayload(
          "mysql DDL datatype parse_to_sblr", ddl_sblr.payload,
          {"udr_mysql_sentinel_type_table", "udr_mysql_col_int",
           "udr_mysql_col_v", "udr_mysql_col_dec", "udr_mysql_col_ts",
           "udr_mysql_col_json"})) {
    return false;
  }
  const auto procedure_sblr = sbu_mysql_parse_to_sblr(
      "create procedure udr_mysql_retention_proc() begin select 'udr_mysql_secret_body'; end",
      trusted);
  if (!ExpectOk(procedure_sblr, "mysql procedure parse_to_sblr")) return false;
  if (!ExpectProceduralSourceRetentionPayload(
          "mysql procedure parse_to_sblr", procedure_sblr.payload,
          {"udr_mysql_retention_proc", "udr_mysql_secret_body"})) {
    return false;
  }
  const auto normalized = sbu_mysql_normalize("  select    1 ; ", "mysql");
  if (!ExpectOk(normalized, "mysql normalize")) return false;
  if (!Expect(normalized.payload == "select 1", "mysql normalize mismatch")) return false;
  const auto described = sbu_mysql_describe_statement("create user 'a'@'%'", trusted);
  if (!ExpectOk(described, "mysql describe")) return false;
  if (!Expect(Contains(described.payload, "mysql.security.create_user") &&
                  Contains(described.payload, "parser_support_udr"),
              "mysql describe payload mismatch")) return false;
  if (!ExpectDiagnostic(sbu_mysql_install_environment("", "install"),
                        "UDR.MYSQL.CONTEXT_MISSING",
                        "mysql install missing context")) return false;
  const auto installed = sbu_mysql_install_environment(trusted_catalog, "install");
  if (!ExpectOk(installed, "mysql install")) return false;
  if (!Expect(Contains(installed.payload, "\"catalog_mutation_applied\":true") &&
                  Contains(installed.payload, "\"real_mysql_file_effects\":false"),
              "mysql install payload mismatch")) return false;
  const auto verified = sbu_mysql_verify_environment(trusted_catalog);
  if (!ExpectOk(verified, "mysql verify")) return false;
  if (!Expect(Contains(verified.payload, "\"catalog_overlay_installed\":true"),
              "mysql verify payload mismatch")) return false;
  if (!ExpectDiagnostic(sbu_mysql_debug_capabilities(""),
                        "SBU_MYSQL.DEBUG_POLICY_DENIED",
                        "mysql debug denied")) return false;
  const auto debug = sbu_mysql_debug_capabilities("allow_debug_artifacts");
  if (!ExpectOk(debug, "mysql debug")) return false;
  if (!Expect(Contains(debug.payload, "\"parser_package\":\"sbp_mysql\""),
              "mysql debug payload mismatch")) return false;
  const auto descriptor = sbu_mysql_package_descriptor();
  if (!Expect(descriptor.package_uuid == kSbuMysqlPackageUuid &&
                  descriptor.package_name == kSbuMysqlPackageName &&
                  descriptor.trusted_cpp && descriptor.entrypoints.size() >= 9,
              "mysql package descriptor mismatch")) return false;
  if (!Expect(descriptor.init != nullptr &&
                  descriptor.init(kSbuMysqlPackageUuid).ok,
              "mysql package lifecycle mismatch")) return false;
  if (!ManagementAbiChecks(
          "mysql", kSbuMysqlPackageUuid,
          "\"real_mysql_file_effects\":false",
          "UDR.MYSQL.CONTEXT_MISSING",
          "UDR.MYSQL.SECURITY_DENIED",
          sbu_mysql_management_operation_inventory,
          sbu_mysql_management_package_request)) {
    return false;
  }
  return true;
}

bool PostgresqlUdrChecks() {
  using namespace scratchbird::udr::postgresql_parser_support;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view trusted_catalog =
      "engine_context=trusted;catalog_uuid=22222222-3333-4444-5555-666666666666";
  if (!ExpectOk(sbu_postgresql_validate_syntax("select 1", "postgresql"),
                "postgresql validate")) return false;
  if (!ExpectDiagnostic(sbu_postgresql_validate_syntax("select 1", "wrong_profile"),
                        "UDR.POSTGRESQL.PROFILE_MISMATCH",
                        "postgresql profile mismatch")) return false;
  if (!ExpectDiagnostic(sbu_postgresql_parse_to_sblr("select 1", ""),
                        "UDR.POSTGRESQL.CONTEXT_MISSING",
                        "postgresql missing context")) return false;
  if (!ExpectDiagnostic(sbu_postgresql_parse_to_sblr(
                            "select 1", "engine_context=untrusted;resolver=uuid"),
                        "UDR.POSTGRESQL.SECURITY_DENIED",
                        "postgresql untrusted context")) return false;
  const auto parsed = sbu_postgresql_parse_to_sblr("select 1", trusted);
  if (!ExpectOk(parsed, "postgresql parse_to_sblr")) return false;
  if (!Expect(Contains(parsed.payload, "SBLRExecutionEnvelope.v3") &&
                  Contains(parsed.payload, "\"dialect\":\"postgresql\"") &&
                  !Contains(parsed.payload, "select 1"),
              "postgresql parse_to_sblr payload mismatch")) return false;
  const auto ddl_sblr = sbu_postgresql_parse_to_sblr(
      "create table udr_pg_sentinel_type_table (udr_pg_col_int integer, udr_pg_col_text text, udr_pg_col_num numeric(10,2), udr_pg_col_tz timestamptz, udr_pg_col_json jsonb, udr_pg_col_uuid uuid)",
      trusted);
  if (!ExpectOk(ddl_sblr, "postgresql DDL datatype parse_to_sblr")) return false;
  if (!ExpectDatatypeDescriptorPayload(
          "postgresql DDL datatype parse_to_sblr", ddl_sblr.payload,
          {"udr_pg_sentinel_type_table", "udr_pg_col_int",
           "udr_pg_col_text", "udr_pg_col_num", "udr_pg_col_tz",
           "udr_pg_col_json", "udr_pg_col_uuid"})) {
    return false;
  }
  const auto procedure_sblr = sbu_postgresql_parse_to_sblr(
      "create procedure udr_pg_retention_proc() language sql as 'select 1 /* udr_pg_secret_body */'",
      trusted);
  if (!ExpectOk(procedure_sblr, "postgresql procedure parse_to_sblr")) return false;
  if (!ExpectProceduralSourceRetentionPayload(
          "postgresql procedure parse_to_sblr", procedure_sblr.payload,
          {"udr_pg_retention_proc", "udr_pg_secret_body"})) {
    return false;
  }
  const auto normalized = sbu_postgresql_normalize("  select    1 ; ", "postgresql");
  if (!ExpectOk(normalized, "postgresql normalize")) return false;
  if (!Expect(normalized.payload == "select 1",
              "postgresql normalize mismatch")) return false;
  const auto described =
      sbu_postgresql_describe_statement("create extension pgcrypto", trusted);
  if (!ExpectOk(described, "postgresql describe")) return false;
  if (!Expect(Contains(described.payload, "postgresql.extension.create") &&
                  Contains(described.payload, "parser_support_udr"),
              "postgresql describe payload mismatch")) return false;
  if (!ExpectDiagnostic(sbu_postgresql_install_environment("", "install"),
                        "UDR.POSTGRESQL.CONTEXT_MISSING",
                        "postgresql install missing context")) return false;
  const auto installed = sbu_postgresql_install_environment(trusted_catalog, "install");
  if (!ExpectOk(installed, "postgresql install")) return false;
  if (!Expect(Contains(installed.payload, "\"catalog_mutation_applied\":true") &&
                  Contains(installed.payload, "\"real_postgresql_file_effects\":false"),
              "postgresql install payload mismatch")) return false;
  const auto verified = sbu_postgresql_verify_environment(trusted_catalog);
  if (!ExpectOk(verified, "postgresql verify")) return false;
  if (!Expect(Contains(verified.payload, "\"catalog_overlay_installed\":true"),
              "postgresql verify payload mismatch")) return false;
  if (!ExpectDiagnostic(sbu_postgresql_debug_capabilities(""),
                        "SBU_POSTGRESQL.DEBUG_POLICY_DENIED",
                        "postgresql debug denied")) return false;
  const auto debug = sbu_postgresql_debug_capabilities("allow_debug_artifacts");
  if (!ExpectOk(debug, "postgresql debug")) return false;
  if (!Expect(Contains(debug.payload, "\"parser_package\":\"sbp_postgresql\""),
              "postgresql debug payload mismatch")) return false;
  const auto descriptor = sbu_postgresql_package_descriptor();
  if (!Expect(descriptor.package_uuid == kSbuPostgresqlPackageUuid &&
                  descriptor.package_name == kSbuPostgresqlPackageName &&
                  descriptor.trusted_cpp && descriptor.entrypoints.size() >= 9,
              "postgresql package descriptor mismatch")) return false;
  if (!Expect(descriptor.init != nullptr &&
                  descriptor.init(kSbuPostgresqlPackageUuid).ok,
              "postgresql package lifecycle mismatch")) return false;
  if (!ManagementAbiChecks(
          "postgresql", kSbuPostgresqlPackageUuid,
          "\"real_postgresql_file_effects\":false",
          "UDR.POSTGRESQL.CONTEXT_MISSING",
          "UDR.POSTGRESQL.SECURITY_DENIED",
          sbu_postgresql_management_operation_inventory,
          sbu_postgresql_management_package_request)) {
    return false;
  }
  return true;
}

bool SqliteUdrChecks() {
  using namespace scratchbird::udr::sqlite_parser_support;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view trusted_catalog =
      "engine_context=trusted;catalog_uuid=33333333-4444-5555-6666-777777777777";
  if (!ExpectOk(sbu_sqlite_validate_syntax("select 1", "sqlite"),
                "sqlite validate")) return false;
  if (!ExpectDiagnostic(sbu_sqlite_validate_syntax("select 1", "wrong_profile"),
                        "UDR.SQLITE.PROFILE_MISMATCH",
                        "sqlite profile mismatch")) return false;
  if (!ExpectDiagnostic(sbu_sqlite_parse_to_sblr("select 1", ""),
                        "UDR.SQLITE.CONTEXT_MISSING",
                        "sqlite missing context")) return false;
  if (!ExpectDiagnostic(sbu_sqlite_parse_to_sblr(
                            "select 1", "engine_context=untrusted;resolver=uuid"),
                        "UDR.SQLITE.SECURITY_DENIED",
                        "sqlite untrusted context")) return false;
  const auto parsed = sbu_sqlite_parse_to_sblr("select 1", trusted);
  if (!ExpectOk(parsed, "sqlite parse_to_sblr")) return false;
  if (!Expect(Contains(parsed.payload, "SBLRExecutionEnvelope.v3") &&
                  Contains(parsed.payload, "\"dialect\":\"sqlite\"") &&
                  !Contains(parsed.payload, "select 1"),
              "sqlite parse_to_sblr payload mismatch")) return false;
  const auto normalized = sbu_sqlite_normalize("  select    1 ; ", "sqlite");
  if (!ExpectOk(normalized, "sqlite normalize")) return false;
  if (!Expect(normalized.payload == "select 1", "sqlite normalize mismatch")) return false;
  const auto described = sbu_sqlite_describe_statement("pragma table_info(t)", trusted);
  if (!ExpectOk(described, "sqlite describe")) return false;
  if (!Expect(Contains(described.payload, "sqlite.pragma.generic") &&
                  Contains(described.payload, "parser_support_udr"),
              "sqlite describe payload mismatch")) return false;
  if (!ExpectDiagnostic(sbu_sqlite_install_environment("", "install"),
                        "UDR.SQLITE.CONTEXT_MISSING",
                        "sqlite install missing context")) return false;
  const auto installed = sbu_sqlite_install_environment(trusted_catalog, "install");
  if (!ExpectOk(installed, "sqlite install")) return false;
  if (!Expect(Contains(installed.payload, "\"catalog_mutation_applied\":true") &&
                  Contains(installed.payload, "\"overlay_row_count\":7") &&
                  Contains(installed.payload, "SQLITE_SCHEMA") &&
                  Contains(installed.payload, "\"real_sqlite_file_effects\":false"),
              "sqlite install payload mismatch")) return false;
  const auto verified = sbu_sqlite_verify_environment(trusted_catalog);
  if (!ExpectOk(verified, "sqlite verify")) return false;
  if (!Expect(Contains(verified.payload, "\"catalog_overlay_installed\":true"),
              "sqlite verify payload mismatch")) return false;
  if (!ExpectDiagnostic(sbu_sqlite_debug_capabilities(""),
                        "SBU_SQLITE.DEBUG_POLICY_DENIED",
                        "sqlite debug denied")) return false;
  const auto debug = sbu_sqlite_debug_capabilities("allow_debug_artifacts");
  if (!ExpectOk(debug, "sqlite debug")) return false;
  if (!Expect(Contains(debug.payload, "\"parser_package\":\"sbp_sqlite\""),
              "sqlite debug payload mismatch")) return false;
  const auto descriptor = sbu_sqlite_package_descriptor();
  if (!Expect(descriptor.package_uuid == kSbuSqlitePackageUuid &&
                  descriptor.package_name == kSbuSqlitePackageName &&
                  descriptor.trusted_cpp && descriptor.entrypoints.size() >= 9,
              "sqlite package descriptor mismatch")) return false;
  if (!Expect(descriptor.init != nullptr &&
                  descriptor.init(kSbuSqlitePackageUuid).ok,
              "sqlite package lifecycle mismatch")) return false;
  if (!ManagementAbiChecks(
          "sqlite", kSbuSqlitePackageUuid,
          "\"real_sqlite_file_effects\":false",
          "UDR.SQLITE.CONTEXT_MISSING",
          "UDR.SQLITE.SECURITY_DENIED",
          sbu_sqlite_management_operation_inventory,
          sbu_sqlite_management_package_request)) {
    return false;
  }
  return true;
}

bool MariadbUdrChecks() {
  using namespace scratchbird::udr::mariadb_parser_support;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view trusted_catalog =
      "engine_context=trusted;catalog_uuid=44444444-5555-6666-7777-888888888888";
  if (!ExpectOk(sbu_mariadb_validate_syntax("select 1", "mariadb"),
                "mariadb validate")) return false;
  if (!ExpectDiagnostic(sbu_mariadb_validate_syntax("select 1", "wrong_profile"),
                        "UDR.MARIADB.PROFILE_MISMATCH",
                        "mariadb profile mismatch")) return false;
  if (!ExpectDiagnostic(sbu_mariadb_parse_to_sblr("select 1", ""),
                        "UDR.MARIADB.CONTEXT_MISSING",
                        "mariadb missing context")) return false;
  const auto parsed = sbu_mariadb_parse_to_sblr("select 1", trusted);
  if (!ExpectOk(parsed, "mariadb parse_to_sblr")) return false;
  if (!Expect(Contains(parsed.payload, "SBLRExecutionEnvelope.v3") &&
                  Contains(parsed.payload, "\"dialect\":\"mariadb\"") &&
                  !Contains(parsed.payload, "select 1"),
              "mariadb parse_to_sblr payload mismatch")) return false;
  const auto described = sbu_mariadb_describe_statement("handler t open", trusted);
  if (!ExpectOk(described, "mariadb describe")) return false;
  if (!Expect(Contains(described.payload, "mariadb.handler.cursor") &&
                  Contains(described.payload, "parser_support_udr"),
              "mariadb describe payload mismatch")) return false;
  const auto installed = sbu_mariadb_install_environment(trusted_catalog, "install");
  if (!ExpectOk(installed, "mariadb install")) return false;
  if (!Expect(Contains(installed.payload, "\"overlay_row_count\":9") &&
                  Contains(installed.payload, "\"MYSQL\"") &&
                  Contains(installed.payload, "\"real_mariadb_file_effects\":false"),
              "mariadb install payload mismatch")) return false;
  const auto descriptor = sbu_mariadb_package_descriptor();
  if (!Expect(descriptor.package_uuid == kSbuMariadbPackageUuid &&
                  descriptor.package_name == kSbuMariadbPackageName &&
                  descriptor.trusted_cpp && descriptor.entrypoints.size() >= 9,
              "mariadb package descriptor mismatch")) return false;
  if (!Expect(descriptor.init != nullptr &&
                  descriptor.init(kSbuMariadbPackageUuid).ok,
              "mariadb package lifecycle mismatch")) return false;
  if (!ManagementAbiChecks(
          "mariadb", kSbuMariadbPackageUuid,
          "\"real_mariadb_file_effects\":false",
          "UDR.MARIADB.CONTEXT_MISSING",
          "UDR.MARIADB.SECURITY_DENIED",
          sbu_mariadb_management_operation_inventory,
          sbu_mariadb_management_package_request)) {
    return false;
  }
  return true;
}

bool DuckdbUdrChecks() {
  using namespace scratchbird::udr::duckdb_parser_support;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view trusted_catalog =
      "engine_context=trusted;catalog_uuid=55555555-6666-7777-8888-999999999999";
  if (!ExpectOk(sbu_duckdb_validate_syntax("select 1", "duckdb"),
                "duckdb validate")) return false;
  if (!ExpectDiagnostic(sbu_duckdb_validate_syntax("select 1", "wrong_profile"),
                        "UDR.DUCKDB.PROFILE_MISMATCH",
                        "duckdb profile mismatch")) return false;
  if (!ExpectDiagnostic(sbu_duckdb_parse_to_sblr("select 1", ""),
                        "UDR.DUCKDB.CONTEXT_MISSING",
                        "duckdb missing context")) return false;
  const auto parsed = sbu_duckdb_parse_to_sblr("select 1", trusted);
  if (!ExpectOk(parsed, "duckdb parse_to_sblr")) return false;
  if (!Expect(Contains(parsed.payload, "SBLRExecutionEnvelope.v3") &&
                  Contains(parsed.payload, "\"dialect\":\"duckdb\"") &&
                  !Contains(parsed.payload, "select 1"),
              "duckdb parse_to_sblr payload mismatch")) return false;
  const auto described =
      sbu_duckdb_describe_statement("create secret scratchbird_secret", trusted);
  if (!ExpectOk(described, "duckdb describe")) return false;
  if (!Expect(Contains(described.payload, "duckdb.security.create_secret") &&
                  Contains(described.payload, "parser_support_udr"),
              "duckdb describe payload mismatch")) return false;
  const auto installed = sbu_duckdb_install_environment(trusted_catalog, "install");
  if (!ExpectOk(installed, "duckdb install")) return false;
  if (!Expect(Contains(installed.payload, "\"overlay_row_count\":8") &&
                  Contains(installed.payload, "DUCKDB_TABLES") &&
                  Contains(installed.payload, "\"real_duckdb_file_effects\":false"),
              "duckdb install payload mismatch")) return false;
  const auto descriptor = sbu_duckdb_package_descriptor();
  if (!Expect(descriptor.package_uuid == kSbuDuckdbPackageUuid &&
                  descriptor.package_name == kSbuDuckdbPackageName &&
                  descriptor.trusted_cpp && descriptor.entrypoints.size() >= 9,
              "duckdb package descriptor mismatch")) return false;
  if (!Expect(descriptor.init != nullptr &&
                  descriptor.init(kSbuDuckdbPackageUuid).ok,
              "duckdb package lifecycle mismatch")) return false;
  if (!ManagementAbiChecks(
          "duckdb", kSbuDuckdbPackageUuid,
          "\"real_duckdb_file_effects\":false",
          "UDR.DUCKDB.CONTEXT_MISSING",
          "UDR.DUCKDB.SECURITY_DENIED",
          sbu_duckdb_management_operation_inventory,
          sbu_duckdb_management_package_request)) {
    return false;
  }
  return true;
}

bool ClickhouseUdrChecks() {
  using namespace scratchbird::udr::clickhouse_parser_support;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view trusted_catalog =
      "engine_context=trusted;catalog_uuid=66666666-7777-8888-9999-aaaaaaaaaaaa";
  if (!ExpectOk(sbu_clickhouse_validate_syntax("select 1", "clickhouse"),
                "clickhouse validate")) return false;
  if (!ExpectDiagnostic(sbu_clickhouse_validate_syntax("select 1", "wrong_profile"),
                        "UDR.CLICKHOUSE.PROFILE_MISMATCH",
                        "clickhouse profile mismatch")) return false;
  if (!ExpectDiagnostic(sbu_clickhouse_parse_to_sblr("select 1", ""),
                        "UDR.CLICKHOUSE.CONTEXT_MISSING",
                        "clickhouse missing context")) return false;
  const auto parsed = sbu_clickhouse_parse_to_sblr("select 1", trusted);
  if (!ExpectOk(parsed, "clickhouse parse_to_sblr")) return false;
  if (!Expect(Contains(parsed.payload, "SBLRExecutionEnvelope.v3") &&
                  Contains(parsed.payload, "\"dialect\":\"clickhouse\"") &&
                  !Contains(parsed.payload, "select 1"),
              "clickhouse parse_to_sblr payload mismatch")) return false;
  const auto described =
      sbu_clickhouse_describe_statement("create dictionary d", trusted);
  if (!ExpectOk(described, "clickhouse describe")) return false;
  if (!Expect(Contains(described.payload, "clickhouse.connector.dictionary.create") &&
                  Contains(described.payload, "policy_refusal"),
              "clickhouse describe payload mismatch")) return false;
  const auto installed = sbu_clickhouse_install_environment(trusted_catalog, "install");
  if (!ExpectOk(installed, "clickhouse install")) return false;
  if (!Expect(Contains(installed.payload, "\"overlay_row_count\":10") &&
                  Contains(installed.payload, "SYSTEM.TABLES") &&
                  Contains(installed.payload, "\"real_clickhouse_file_effects\":false"),
              "clickhouse install payload mismatch")) return false;
  const auto descriptor = sbu_clickhouse_package_descriptor();
  if (!Expect(descriptor.package_uuid == kSbuClickhousePackageUuid &&
                  descriptor.package_name == kSbuClickhousePackageName &&
                  descriptor.trusted_cpp && descriptor.entrypoints.size() >= 9,
              "clickhouse package descriptor mismatch")) return false;
  if (!Expect(descriptor.init != nullptr &&
                  descriptor.init(kSbuClickhousePackageUuid).ok,
              "clickhouse package lifecycle mismatch")) return false;
  if (!ManagementAbiChecks(
          "clickhouse", kSbuClickhousePackageUuid,
          "\"real_clickhouse_file_effects\":false",
          "UDR.CLICKHOUSE.CONTEXT_MISSING",
          "UDR.CLICKHOUSE.SECURITY_DENIED",
          sbu_clickhouse_management_operation_inventory,
          sbu_clickhouse_management_package_request)) {
    return false;
  }
  return true;
}

template <typename ValidateFn,
          typename ParseFn,
          typename NormalizeFn,
          typename DescribeFn,
          typename InstallFn,
          typename VerifyFn,
          typename DebugFn,
          typename DescriptorFn,
          typename InventoryFn,
          typename RequestFn>
bool StandardDonorUdrChecks(std::string_view label,
                            std::string_view profile,
                            std::string_view package_uuid,
                            std::string_view package_name,
                            std::string_view parse_sql,
                            std::string_view describe_sql,
                            std::string_view expected_operation_family,
                            std::string_view trusted_catalog,
                            std::string_view overlay_marker,
                            std::string_view exact_file_effects_key,
                            std::string_view missing_context_code,
                            std::string_view security_denied_code,
                            std::string_view profile_mismatch_code,
                            std::string_view debug_policy_denied_code,
                            std::string_view parser_package_fragment,
                            ValidateFn validate_fn,
                            ParseFn parse_fn,
                            NormalizeFn normalize_fn,
                            DescribeFn describe_fn,
                            InstallFn install_fn,
                            VerifyFn verify_fn,
                            DebugFn debug_fn,
                            DescriptorFn descriptor_fn,
                            InventoryFn inventory_fn,
                            RequestFn request_fn) {
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  if (!ExpectOk(validate_fn(parse_sql, profile),
                std::string(label) + " validate")) return false;
  if (!ExpectDiagnostic(validate_fn(parse_sql, "wrong_profile"),
                        profile_mismatch_code,
                        std::string(label) + " profile mismatch")) return false;
  if (!ExpectDiagnostic(parse_fn(parse_sql, ""),
                        missing_context_code,
                        std::string(label) + " missing context")) return false;
  if (!ExpectDiagnostic(parse_fn(parse_sql, "engine_context=untrusted;resolver=uuid"),
                        security_denied_code,
                        std::string(label) + " untrusted context")) return false;
  const auto parsed = parse_fn(parse_sql, trusted);
  if (!ExpectOk(parsed, std::string(label) + " parse_to_sblr")) return false;
  if (!Expect(Contains(parsed.payload, "SBLRExecutionEnvelope.v3") &&
                  Contains(parsed.payload, "\"dialect\":\"" + std::string(profile) + "\"") &&
                  !Contains(parsed.payload, std::string(parse_sql)),
              std::string(label) + " parse_to_sblr payload mismatch")) return false;
  const auto normalized = normalize_fn(parse_sql, profile);
  if (!ExpectOk(normalized, std::string(label) + " normalize")) return false;
  if (!Expect(!normalized.payload.empty() &&
                  !Contains(normalized.payload, "  "),
              std::string(label) + " normalize mismatch")) return false;
  const auto described = describe_fn(describe_sql, trusted);
  if (!ExpectOk(described, std::string(label) + " describe")) return false;
  if (!Expect(Contains(described.payload, expected_operation_family),
              std::string(label) + " describe payload mismatch")) return false;
  if (!ExpectDiagnostic(install_fn("", "install"),
                        missing_context_code,
                        std::string(label) + " install missing context")) return false;
  const auto installed = install_fn(trusted_catalog, "install");
  if (!ExpectOk(installed, std::string(label) + " install")) return false;
  if (!Expect(Contains(installed.payload, "\"catalog_mutation_applied\":true") &&
                  Contains(installed.payload, exact_file_effects_key) &&
                  Contains(installed.payload, overlay_marker),
              std::string(label) + " install payload mismatch")) return false;
  const auto verified = verify_fn(trusted_catalog);
  if (!ExpectOk(verified, std::string(label) + " verify")) return false;
  if (!Expect(Contains(verified.payload, "\"catalog_overlay_installed\":true"),
              std::string(label) + " verify payload mismatch")) return false;
  if (!ExpectDiagnostic(debug_fn(""),
                        debug_policy_denied_code,
                        std::string(label) + " debug denied")) return false;
  const auto debug = debug_fn("allow_debug_artifacts");
  if (!ExpectOk(debug, std::string(label) + " debug")) return false;
  if (!Expect(Contains(debug.payload, parser_package_fragment),
              std::string(label) + " debug payload mismatch")) return false;
  const auto descriptor = descriptor_fn();
  if (!Expect(descriptor.package_uuid == package_uuid &&
                  descriptor.package_name == package_name &&
                  descriptor.trusted_cpp && descriptor.entrypoints.size() >= 9,
              std::string(label) + " package descriptor mismatch")) return false;
  if (!Expect(descriptor.init != nullptr &&
                  descriptor.init(package_uuid).ok,
              std::string(label) + " package lifecycle mismatch")) return false;
  if (!ManagementAbiChecks(
          label, package_uuid, exact_file_effects_key,
          missing_context_code, security_denied_code,
          inventory_fn, request_fn)) {
    return false;
  }
  return true;
}

bool TidbUdrChecks() {
  using namespace scratchbird::udr::tidb_parser_support;
  return StandardDonorUdrChecks(
      "tidb", "tidb", kSbuTidbPackageUuid, kSbuTidbPackageName,
      "select tidb_version()", "split table t between (1) and (100) regions 4",
      "tidb.placement.split_table",
      "engine_context=trusted;catalog_uuid=77777777-8888-9999-aaaa-bbbbbbbbbbbb",
      "TIDB_SERVERS", "\"real_tidb_file_effects\":false",
      "UDR.TIDB.CONTEXT_MISSING", "UDR.TIDB.SECURITY_DENIED",
      "UDR.TIDB.PROFILE_MISMATCH", "SBU_TIDB.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_tidb\"",
      sbu_tidb_validate_syntax, sbu_tidb_parse_to_sblr,
      sbu_tidb_normalize, sbu_tidb_describe_statement,
      sbu_tidb_install_environment, sbu_tidb_verify_environment,
      sbu_tidb_debug_capabilities, sbu_tidb_package_descriptor,
      sbu_tidb_management_operation_inventory,
      sbu_tidb_management_package_request);
}

bool VitessUdrChecks() {
  using namespace scratchbird::udr::vitess_parser_support;
  return StandardDonorUdrChecks(
      "vitess", "vitess", kSbuVitessPackageUuid, kSbuVitessPackageName,
      "select keyspace_id from customer", "move tables commerce.customer",
      "vitess.vreplication.move_tables",
      "engine_context=trusted;catalog_uuid=88888888-9999-aaaa-bbbb-cccccccccccc",
      "VT_KEYSPACES", "\"real_vitess_file_effects\":false",
      "UDR.VITESS.CONTEXT_MISSING", "UDR.VITESS.SECURITY_DENIED",
      "UDR.VITESS.PROFILE_MISMATCH", "SBU_VITESS.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_vitess\"",
      sbu_vitess_validate_syntax, sbu_vitess_parse_to_sblr,
      sbu_vitess_normalize, sbu_vitess_describe_statement,
      sbu_vitess_install_environment, sbu_vitess_verify_environment,
      sbu_vitess_debug_capabilities, sbu_vitess_package_descriptor,
      sbu_vitess_management_operation_inventory,
      sbu_vitess_management_package_request);
}

bool CockroachdbUdrChecks() {
  using namespace scratchbird::udr::cockroachdb_parser_support;
  return StandardDonorUdrChecks(
      "cockroachdb", "cockroachdb", kSbuCockroachdbPackageUuid,
      kSbuCockroachdbPackageName,
      "select crdb_internal.node_id()", "create changefeed for table t",
      "cockroachdb.changefeed.create",
      "engine_context=trusted;catalog_uuid=99999999-aaaa-bbbb-cccc-dddddddddddd",
      "CRDB_INTERNAL", "\"real_cockroachdb_file_effects\":false",
      "UDR.COCKROACHDB.CONTEXT_MISSING", "UDR.COCKROACHDB.SECURITY_DENIED",
      "UDR.COCKROACHDB.PROFILE_MISMATCH",
      "SBU_COCKROACHDB.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_cockroachdb\"",
      sbu_cockroachdb_validate_syntax, sbu_cockroachdb_parse_to_sblr,
      sbu_cockroachdb_normalize, sbu_cockroachdb_describe_statement,
      sbu_cockroachdb_install_environment,
      sbu_cockroachdb_verify_environment,
      sbu_cockroachdb_debug_capabilities,
      sbu_cockroachdb_package_descriptor,
      sbu_cockroachdb_management_operation_inventory,
      sbu_cockroachdb_management_package_request);
}

bool YugabytedbUdrChecks() {
  using namespace scratchbird::udr::yugabytedb_parser_support;
  return StandardDonorUdrChecks(
      "yugabytedb", "yugabytedb", kSbuYugabytedbPackageUuid,
      kSbuYugabytedbPackageName,
      "select yb_server_region()", "create keyspace ks",
      "yugabytedb.ycql.keyspace.create",
      "engine_context=trusted;catalog_uuid=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
      "YB_TABLETS", "\"real_yugabytedb_file_effects\":false",
      "UDR.YUGABYTEDB.CONTEXT_MISSING", "UDR.YUGABYTEDB.SECURITY_DENIED",
      "UDR.YUGABYTEDB.PROFILE_MISMATCH",
      "SBU_YUGABYTEDB.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_yugabytedb\"",
      sbu_yugabytedb_validate_syntax,
      sbu_yugabytedb_parse_to_sblr,
      sbu_yugabytedb_normalize,
      sbu_yugabytedb_describe_statement,
      sbu_yugabytedb_install_environment,
      sbu_yugabytedb_verify_environment,
      sbu_yugabytedb_debug_capabilities,
      sbu_yugabytedb_package_descriptor,
      sbu_yugabytedb_management_operation_inventory,
      sbu_yugabytedb_management_package_request);
}

bool CassandraUdrChecks() {
  using namespace scratchbird::udr::cassandra_parser_support;
  return StandardDonorUdrChecks(
      "cassandra", "cassandra", kSbuCassandraPackageUuid,
      kSbuCassandraPackageName,
      "select json * from ks.t", "create keyspace ks",
      "cassandra.keyspace.create",
      "engine_context=trusted;catalog_uuid=bbbbbbbb-cccc-dddd-eeee-ffffffffffff",
      "SYSTEM_SCHEMA", "\"real_cassandra_file_effects\":false",
      "UDR.CASSANDRA.CONTEXT_MISSING", "UDR.CASSANDRA.SECURITY_DENIED",
      "UDR.CASSANDRA.PROFILE_MISMATCH",
      "SBU_CASSANDRA.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_cassandra\"",
      sbu_cassandra_validate_syntax,
      sbu_cassandra_parse_to_sblr,
      sbu_cassandra_normalize,
      sbu_cassandra_describe_statement,
      sbu_cassandra_install_environment,
      sbu_cassandra_verify_environment,
      sbu_cassandra_debug_capabilities,
      sbu_cassandra_package_descriptor,
      sbu_cassandra_management_operation_inventory,
      sbu_cassandra_management_package_request);
}

bool MongodbUdrChecks() {
  using namespace scratchbird::udr::mongodb_parser_support;
  return StandardDonorUdrChecks(
      "mongodb", "mongodb", kSbuMongodbPackageUuid,
      kSbuMongodbPackageName,
      "find users { status: 'A' }", "aggregate orders [ {$out:'archived'} ]",
      "mongodb.aggregate.out",
      "engine_context=trusted;catalog_uuid=cccccccc-dddd-eeee-ffff-000000000000",
      "system.users", "\"real_mongodb_file_effects\":false",
      "UDR.MONGODB.CONTEXT_MISSING", "UDR.MONGODB.SECURITY_DENIED",
      "UDR.MONGODB.PROFILE_MISMATCH",
      "SBU_MONGODB.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_mongodb\"",
      sbu_mongodb_validate_syntax,
      sbu_mongodb_parse_to_sblr,
      sbu_mongodb_normalize,
      sbu_mongodb_describe_statement,
      sbu_mongodb_install_environment,
      sbu_mongodb_verify_environment,
      sbu_mongodb_debug_capabilities,
      sbu_mongodb_package_descriptor,
      sbu_mongodb_management_operation_inventory,
      sbu_mongodb_management_package_request);
}

bool RedisUdrChecks() {
  using namespace scratchbird::udr::redis_parser_support;
  return StandardDonorUdrChecks(
      "redis", "redis", kSbuRedisPackageUuid,
      kSbuRedisPackageName,
      "set account:1 active", "eval \"return 1\" 0",
      "redis.script.eval",
      "engine_context=trusted;catalog_uuid=dddddddd-eeee-ffff-0000-111111111111",
      "COMMAND", "\"real_redis_file_effects\":false",
      "UDR.REDIS.CONTEXT_MISSING", "UDR.REDIS.SECURITY_DENIED",
      "UDR.REDIS.PROFILE_MISMATCH",
      "SBU_REDIS.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_redis\"",
      sbu_redis_validate_syntax,
      sbu_redis_parse_to_sblr,
      sbu_redis_normalize,
      sbu_redis_describe_statement,
      sbu_redis_install_environment,
      sbu_redis_verify_environment,
      sbu_redis_debug_capabilities,
      sbu_redis_package_descriptor,
      sbu_redis_management_operation_inventory,
      sbu_redis_management_package_request);
}

bool OpensearchSqlPplUdrChecks() {
  using namespace scratchbird::udr::opensearch_sql_ppl_parser_support;
  return StandardDonorUdrChecks(
      "opensearch_sql_ppl", "opensearch_sql_ppl",
      kSbuOpensearchSqlPplPackageUuid,
      kSbuOpensearchSqlPplPackageName,
      "select count(*) from accounts", "create index accounts",
      "opensearch_sql_ppl.index.create",
      "engine_context=trusted;catalog_uuid=eeeeeeee-ffff-0000-1111-222222222222",
      "DATASOURCES", "\"real_opensearch_sql_ppl_file_effects\":false",
      "UDR.OPENSEARCH_SQL_PPL.CONTEXT_MISSING",
      "UDR.OPENSEARCH_SQL_PPL.SECURITY_DENIED",
      "UDR.OPENSEARCH_SQL_PPL.PROFILE_MISMATCH",
      "SBU_OPENSEARCH_SQL_PPL.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_opensearch_sql_ppl\"",
      sbu_opensearch_sql_ppl_validate_syntax,
      sbu_opensearch_sql_ppl_parse_to_sblr,
      sbu_opensearch_sql_ppl_normalize,
      sbu_opensearch_sql_ppl_describe_statement,
      sbu_opensearch_sql_ppl_install_environment,
      sbu_opensearch_sql_ppl_verify_environment,
      sbu_opensearch_sql_ppl_debug_capabilities,
      sbu_opensearch_sql_ppl_package_descriptor,
      sbu_opensearch_sql_ppl_management_operation_inventory,
      sbu_opensearch_sql_ppl_management_package_request);
}

bool OpensearchUdrChecks() {
  using namespace scratchbird::udr::opensearch_parser_support;
  return StandardDonorUdrChecks(
      "opensearch", "opensearch", kSbuOpensearchPackageUuid,
      kSbuOpensearchPackageName,
      "POST /accounts/_search {\"query\":{\"match_all\":{}}}",
      "PUT /accounts {\"mappings\":{\"properties\":{\"id\":{\"type\":\"keyword\"}}}}",
      "opensearch.index.create",
      "engine_context=trusted;catalog_uuid=ffffffff-0000-1111-2222-333333333333",
      "OPENSEARCH_INDICES", "\"real_opensearch_file_effects\":false",
      "UDR.OPENSEARCH.CONTEXT_MISSING",
      "UDR.OPENSEARCH.SECURITY_DENIED",
      "UDR.OPENSEARCH.PROFILE_MISMATCH",
      "SBU_OPENSEARCH.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_opensearch\"",
      sbu_opensearch_validate_syntax,
      sbu_opensearch_parse_to_sblr,
      sbu_opensearch_normalize,
      sbu_opensearch_describe_statement,
      sbu_opensearch_install_environment,
      sbu_opensearch_verify_environment,
      sbu_opensearch_debug_capabilities,
      sbu_opensearch_package_descriptor,
      sbu_opensearch_management_operation_inventory,
      sbu_opensearch_management_package_request);
}

bool Neo4jUdrChecks() {
  using namespace scratchbird::udr::neo4j_parser_support;
  return StandardDonorUdrChecks(
      "neo4j", "neo4j", kSbuNeo4jPackageUuid,
      kSbuNeo4jPackageName,
      "match (n:Account) return n",
      "create constraint account_id if not exists for (a:Account) require a.id is unique",
      "neo4j.schema.constraint.create",
      "engine_context=trusted;catalog_uuid=11111111-2222-3333-4444-666666666666",
      "NEO4J_SYSTEM_GRAPH", "\"real_neo4j_file_effects\":false",
      "UDR.NEO4J.CONTEXT_MISSING",
      "UDR.NEO4J.SECURITY_DENIED",
      "UDR.NEO4J.PROFILE_MISMATCH",
      "SBU_NEO4J.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_neo4j\"",
      sbu_neo4j_validate_syntax,
      sbu_neo4j_parse_to_sblr,
      sbu_neo4j_normalize,
      sbu_neo4j_describe_statement,
      sbu_neo4j_install_environment,
      sbu_neo4j_verify_environment,
      sbu_neo4j_debug_capabilities,
      sbu_neo4j_package_descriptor,
      sbu_neo4j_management_operation_inventory,
      sbu_neo4j_management_package_request);
}

bool InfluxdbUdrChecks() {
  using namespace scratchbird::udr::influxdb_parser_support;
  return StandardDonorUdrChecks(
      "influxdb", "influxdb", kSbuInfluxdbPackageUuid,
      kSbuInfluxdbPackageName,
      "select mean(value) from cpu",
      "create retention policy rp on metrics duration 7d replication 1",
      "influxdb.retention_policy.create",
      "engine_context=trusted;catalog_uuid=22222222-3333-4444-5555-777777777777",
      "INFLUXDB_BUCKETS", "\"real_influxdb_file_effects\":false",
      "UDR.INFLUXDB.CONTEXT_MISSING",
      "UDR.INFLUXDB.SECURITY_DENIED",
      "UDR.INFLUXDB.PROFILE_MISMATCH",
      "SBU_INFLUXDB.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_influxdb\"",
      sbu_influxdb_validate_syntax,
      sbu_influxdb_parse_to_sblr,
      sbu_influxdb_normalize,
      sbu_influxdb_describe_statement,
      sbu_influxdb_install_environment,
      sbu_influxdb_verify_environment,
      sbu_influxdb_debug_capabilities,
      sbu_influxdb_package_descriptor,
      sbu_influxdb_management_operation_inventory,
      sbu_influxdb_management_package_request);
}

bool MilvusUdrChecks() {
  using namespace scratchbird::udr::milvus_parser_support;
  return StandardDonorUdrChecks(
      "milvus", "milvus", kSbuMilvusPackageUuid,
      kSbuMilvusPackageName,
      "search collection accounts vector [0.1,0.2] topk 10",
      "create collection accounts id int64 vector float_vector dim 128",
      "milvus.collection.create",
      "engine_context=trusted;catalog_uuid=33333333-4444-5555-6666-888888888888",
      "MILVUS_COLLECTIONS", "\"real_milvus_file_effects\":false",
      "UDR.MILVUS.CONTEXT_MISSING",
      "UDR.MILVUS.SECURITY_DENIED",
      "UDR.MILVUS.PROFILE_MISMATCH",
      "SBU_MILVUS.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_milvus\"",
      sbu_milvus_validate_syntax,
      sbu_milvus_parse_to_sblr,
      sbu_milvus_normalize,
      sbu_milvus_describe_statement,
      sbu_milvus_install_environment,
      sbu_milvus_verify_environment,
      sbu_milvus_debug_capabilities,
      sbu_milvus_package_descriptor,
      sbu_milvus_management_operation_inventory,
      sbu_milvus_management_package_request);
}

bool DoltUdrChecks() {
  using namespace scratchbird::udr::dolt_parser_support;
  return StandardDonorUdrChecks(
      "dolt", "dolt", kSbuDoltPackageUuid, kSbuDoltPackageName,
      "select * from dolt_log", "call dolt_commit('-Am','msg')",
      "dolt.version.commit",
      "engine_context=trusted;catalog_uuid=44444444-5555-6666-7777-999999999999",
      "DOLT_BRANCHES", "\"real_dolt_file_effects\":false",
      "UDR.DOLT.CONTEXT_MISSING",
      "UDR.DOLT.SECURITY_DENIED",
      "UDR.DOLT.PROFILE_MISMATCH",
      "SBU_DOLT.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_dolt\"",
      sbu_dolt_validate_syntax,
      sbu_dolt_parse_to_sblr,
      sbu_dolt_normalize,
      sbu_dolt_describe_statement,
      sbu_dolt_install_environment,
      sbu_dolt_verify_environment,
      sbu_dolt_debug_capabilities,
      sbu_dolt_package_descriptor,
      sbu_dolt_management_operation_inventory,
      sbu_dolt_management_package_request);
}

bool ApacheIgniteUdrChecks() {
  using namespace scratchbird::udr::apache_ignite_parser_support;
  return StandardDonorUdrChecks(
      "apache_ignite", "apache_ignite", kSbuApacheIgnitePackageUuid,
      kSbuApacheIgnitePackageName,
      "select * from City", "create cache CityCache",
      "apache_ignite.cache.create",
      "engine_context=trusted;catalog_uuid=55555555-6666-7777-8888-aaaaaaaaaaaa",
      "IGNITE_CACHES", "\"real_apache_ignite_file_effects\":false",
      "UDR.APACHE_IGNITE.CONTEXT_MISSING",
      "UDR.APACHE_IGNITE.SECURITY_DENIED",
      "UDR.APACHE_IGNITE.PROFILE_MISMATCH",
      "SBU_APACHE_IGNITE.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_apache_ignite\"",
      sbu_apache_ignite_validate_syntax,
      sbu_apache_ignite_parse_to_sblr,
      sbu_apache_ignite_normalize,
      sbu_apache_ignite_describe_statement,
      sbu_apache_ignite_install_environment,
      sbu_apache_ignite_verify_environment,
      sbu_apache_ignite_debug_capabilities,
      sbu_apache_ignite_package_descriptor,
      sbu_apache_ignite_management_operation_inventory,
      sbu_apache_ignite_management_package_request);
}

bool TikvUdrChecks() {
  using namespace scratchbird::udr::tikv_parser_support;
  return StandardDonorUdrChecks(
      "tikv", "tikv", kSbuTikvPackageUuid, kSbuTikvPackageName,
      "RAW_GET account:1", "TXN_PREWRITE account:1 active",
      "tikv.txn.prewrite",
      "engine_context=trusted;catalog_uuid=66666666-7777-8888-9999-bbbbbbbbbbbb",
      "TIKV_REGIONS", "\"real_tikv_file_effects\":false",
      "UDR.TIKV.CONTEXT_MISSING",
      "UDR.TIKV.SECURITY_DENIED",
      "UDR.TIKV.PROFILE_MISMATCH",
      "SBU_TIKV.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_tikv\"",
      sbu_tikv_validate_syntax,
      sbu_tikv_parse_to_sblr,
      sbu_tikv_normalize,
      sbu_tikv_describe_statement,
      sbu_tikv_install_environment,
      sbu_tikv_verify_environment,
      sbu_tikv_debug_capabilities,
      sbu_tikv_package_descriptor,
      sbu_tikv_management_operation_inventory,
      sbu_tikv_management_package_request);
}

bool FoundationdbUdrChecks() {
  using namespace scratchbird::udr::foundationdb_parser_support;
  return StandardDonorUdrChecks(
      "foundationdb", "foundationdb", kSbuFoundationdbPackageUuid,
      kSbuFoundationdbPackageName,
      "GET account:1", "DIRECTORY_CREATE app users",
      "foundationdb.directory.create",
      "engine_context=trusted;catalog_uuid=77777777-8888-9999-aaaa-cccccccccccc",
      "FOUNDATIONDB_STATUS", "\"real_foundationdb_file_effects\":false",
      "UDR.FOUNDATIONDB.CONTEXT_MISSING",
      "UDR.FOUNDATIONDB.SECURITY_DENIED",
      "UDR.FOUNDATIONDB.PROFILE_MISMATCH",
      "SBU_FOUNDATIONDB.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_foundationdb\"",
      sbu_foundationdb_validate_syntax,
      sbu_foundationdb_parse_to_sblr,
      sbu_foundationdb_normalize,
      sbu_foundationdb_describe_statement,
      sbu_foundationdb_install_environment,
      sbu_foundationdb_verify_environment,
      sbu_foundationdb_debug_capabilities,
      sbu_foundationdb_package_descriptor,
      sbu_foundationdb_management_operation_inventory,
      sbu_foundationdb_management_package_request);
}

bool ImmudbUdrChecks() {
  using namespace scratchbird::udr::immudb_parser_support;
  return StandardDonorUdrChecks(
      "immudb", "immudb", kSbuImmudbPackageUuid,
      kSbuImmudbPackageName,
      "VERIFIED_GET account:1", "VERIFIED_SET account:1 active",
      "immudb.kv.verified_set",
      "engine_context=trusted;catalog_uuid=88888888-9999-aaaa-bbbb-dddddddddddd",
      "PG_TYPE", "\"real_immudb_file_effects\":false",
      "UDR.IMMUDB.CONTEXT_MISSING",
      "UDR.IMMUDB.SECURITY_DENIED",
      "UDR.IMMUDB.PROFILE_MISMATCH",
      "SBU_IMMUDB.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_immudb\"",
      sbu_immudb_validate_syntax,
      sbu_immudb_parse_to_sblr,
      sbu_immudb_normalize,
      sbu_immudb_describe_statement,
      sbu_immudb_install_environment,
      sbu_immudb_verify_environment,
      sbu_immudb_debug_capabilities,
      sbu_immudb_package_descriptor,
      sbu_immudb_management_operation_inventory,
      sbu_immudb_management_package_request);
}

bool XtdbUdrChecks() {
  using namespace scratchbird::udr::xtdb_parser_support;
  return StandardDonorUdrChecks(
      "xtdb", "xtdb", kSbuXtdbPackageUuid,
      kSbuXtdbPackageName,
      "XTDB_Q [:find ?e :where [?e :name \"Ada\"]]",
      "XTDB_SUBMIT_TX [{:xt/id :account/1 :name \"Ada\"}]",
      "xtdb.entity.submit_tx",
      "engine_context=trusted;catalog_uuid=99999999-aaaa-bbbb-cccc-eeeeeeeeeeee",
      "XT.LIVE_TABLES", "\"real_xtdb_file_effects\":false",
      "UDR.XTDB.CONTEXT_MISSING",
      "UDR.XTDB.SECURITY_DENIED",
      "UDR.XTDB.PROFILE_MISMATCH",
      "SBU_XTDB.DEBUG_POLICY_DENIED",
      "\"parser_package\":\"sbp_xtdb\"",
      sbu_xtdb_validate_syntax,
      sbu_xtdb_parse_to_sblr,
      sbu_xtdb_normalize,
      sbu_xtdb_describe_statement,
      sbu_xtdb_install_environment,
      sbu_xtdb_verify_environment,
      sbu_xtdb_debug_capabilities,
      sbu_xtdb_package_descriptor,
      sbu_xtdb_management_operation_inventory,
      sbu_xtdb_management_package_request);
}

} // namespace

int main() {
  if (!FirebirdUdrChecks()) return EXIT_FAILURE;
  if (!MysqlUdrChecks()) return EXIT_FAILURE;
  if (!PostgresqlUdrChecks()) return EXIT_FAILURE;
  if (!SqliteUdrChecks()) return EXIT_FAILURE;
  if (!MariadbUdrChecks()) return EXIT_FAILURE;
  if (!DuckdbUdrChecks()) return EXIT_FAILURE;
  if (!ClickhouseUdrChecks()) return EXIT_FAILURE;
  if (!TidbUdrChecks()) return EXIT_FAILURE;
  if (!VitessUdrChecks()) return EXIT_FAILURE;
  if (!CockroachdbUdrChecks()) return EXIT_FAILURE;
  if (!YugabytedbUdrChecks()) return EXIT_FAILURE;
  if (!CassandraUdrChecks()) return EXIT_FAILURE;
  if (!MongodbUdrChecks()) return EXIT_FAILURE;
  if (!RedisUdrChecks()) return EXIT_FAILURE;
  if (!OpensearchSqlPplUdrChecks()) return EXIT_FAILURE;
  if (!OpensearchUdrChecks()) return EXIT_FAILURE;
  if (!Neo4jUdrChecks()) return EXIT_FAILURE;
  if (!InfluxdbUdrChecks()) return EXIT_FAILURE;
  if (!MilvusUdrChecks()) return EXIT_FAILURE;
  if (!DoltUdrChecks()) return EXIT_FAILURE;
  if (!ApacheIgniteUdrChecks()) return EXIT_FAILURE;
  if (!TikvUdrChecks()) return EXIT_FAILURE;
  if (!FoundationdbUdrChecks()) return EXIT_FAILURE;
  if (!ImmudbUdrChecks()) return EXIT_FAILURE;
  if (!XtdbUdrChecks()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
