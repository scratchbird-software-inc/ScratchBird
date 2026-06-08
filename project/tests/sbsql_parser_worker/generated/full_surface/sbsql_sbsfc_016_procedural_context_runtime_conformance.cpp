// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::engine::functions::BuildStandardFunctionSeedPackage;
using scratchbird::engine::functions::DispatchFunctionCall;
using scratchbird::engine::functions::FunctionArgument;
using scratchbird::engine::functions::FunctionCallRequest;
using scratchbird::engine::functions::FunctionRegistry;
using scratchbird::engine::sblr::SblrStatusCode;
using scratchbird::engine::sblr::SblrValue;
using scratchbird::engine::sblr::SblrValuePayloadKind;

struct PolicyRefusalRuntimeRow {
  const char* case_id;
  const char* function_id;
};

constexpr PolicyRefusalRuntimeRow kPolicyRefusalRuntimeRows[] = {
    {"SBSQL-2DB83873C621-system-variable-autocommit", "sb.scalar.refusal_system_variable_autocommit"},
    {"SBSQL-5FBC168DF633-system-variable-session-autocommit", "sb.scalar.refusal_system_variable_session_autocommit"},
    {"SBSQL-7269DD8C7658-system-variable-session-var", "sb.scalar.refusal_system_variable_session_var"},
    {"SBSQL-9637EA2DFC5A-system-variable-global-var", "sb.scalar.refusal_system_variable_global_var"},
    {"SBSQL-7697E4BCE46F-system-variable-hostname", "sb.scalar.refusal_system_variable_hostname"},
    {"SBSQL-EED4041BEB12-system-variable-servername", "sb.scalar.refusal_system_variable_servername"},
    {"SBSQL-F89DC25F25BC-current-query", "sb.scalar.refusal_current_query"},
    {"SBSQL-243B7D1E55C5-pg-read-binary-file", "sb.scalar.refusal_pg_read_binary_file"},
    {"SBSQL-4C8128BF2BAA-pg-read-file", "sb.scalar.refusal_pg_read_file"},
    {"SBSQL-C57AB769021B-pg-read-server-files", "sb.scalar.refusal_pg_read_server_files"},
    {"SBSQL-ED59B532C7C6-pg-ls-dir", "sb.scalar.refusal_pg_ls_dir"},
    {"SBSQL-AAAAAA49C991-lo-import", "sb.scalar.refusal_lo_import"},
    {"SBSQL-45B0362E386F-lo-export", "sb.scalar.refusal_lo_export"},
    {"SBSQL-CEA7A25D6025-pg-reload-conf", "sb.scalar.refusal_pg_reload_conf"},
    {"SBSQL-4F59D2264C2C-pg-rotate-logfile", "sb.scalar.refusal_pg_rotate_logfile"},
    {"SBSQL-4189F1BE1A5A-wal", "sb.scalar.refusal_wal"},
    {"SBSQL-1E628CF3F25A-pg-sleep", "sb.scalar.refusal_pg_sleep"},
    {"SBSQL-0EEF9D0588DE-pg-terminate-session", "sb.scalar.refusal_pg_terminate_session"},
    {"SBSQL-1D7526609BC7-pg-cron", "sb.scalar.refusal_pg_cron"},
    {"SBSQL-E0DDB9379496-pg-backend-pid", "sb.scalar.refusal_pg_backend_pid"},
    {"SBSQL-3E730331C624-sql-bulk-exceptions", "sb.scalar.refusal_sql_bulk_exceptions"},
    {"SBSQL-366A50FD36A0-idempotency", "sb.scalar.refusal_idempotency"},
    {"SBSQL-1C4864BC8A40-existsnode", "sb.scalar.refusal_existsnode"},
    {"SBSQL-D2F4FCC62B8D-updatexml", "sb.scalar.refusal_updatexml"},
    {"SBSQL-24B221BB6B46-xmlelement", "sb.scalar.refusal_xmlelement"},
    {"SBSQL-219F078BDF7C-model", "sb.scalar.refusal_model"},
    {"SBSQL-1EC5E3273B4D-event", "sb.scalar.refusal_event"},
    {"SBSQL-EBD034715D41-operator", "sb.scalar.refusal_operator"},
    {"SBSQL-82082E76658A-sys-context-client-info", "sb.scalar.refusal_sys_context_client_info"},
    {"SBSQL-93B790433D9D-rdb-get-context-client-pid", "sb.scalar.refusal_rdb_get_context_client_pid"},
    {"SBSQL-18E22DA52C20-with-readpast", "sb.scalar.refusal_with_readpast"},
    {"SBSQL-1CB047ADD7B5-with-nolock", "sb.scalar.refusal_with_nolock"},
    {"SBSQL-F9144106E9F2-explain-waltrue", "sb.scalar.refusal_explain_waltrue"},
    {"SBSQL-E9D1FF70EC0D-explain-donor-log-compatibilitytrue", "sb.scalar.refusal_explain_donor_log_compatibilitytrue"},
    {"SBSQL-BEB64FF33809-explain-evidencetrue", "sb.scalar.refusal_explain_evidencetrue"},
    {"SBSQL-5D9C952A3697-system-variable-trancount", "sb.scalar.refusal_system_variable_trancount"},
    {"SBSQL-511BC12EEF82-atomic", "sb.scalar.refusal_atomic"},
    {"SBSQL-75CF9FACACE2-call", "sb.scalar.refusal_call"},
    {"SBSQL-7B58373BC23A-forall", "sb.scalar.refusal_forall"},
    {"SBSQL-83401CA819BA-attach", "sb.scalar.refusal_attach"},
    {"SBSQL-88DF354F5A82-describe", "sb.scalar.refusal_describe"},
    {"SBSQL-A43A427BA70A-execute-dynamic-sql", "sb.scalar.refusal_execute_dynamic_sql"},
    {"SBSQL-B200BD7425CA-operator-manifest-csv", "sb.scalar.refusal_operator_manifest_csv"},
    {"SBSQL-B9BFDB8979B1-detach", "sb.scalar.refusal_detach"},
    {"SBSQL-C10FBE98B475-into", "sb.scalar.refusal_into"},
    {"SBSQL-CDDDEF1DE9AB-suspend", "sb.scalar.refusal_suspend"},
    {"SBSQL-D3442C734EE8-returning", "sb.scalar.refusal_returning"},
    {"SBSQL-D6ACEF2C5FE1-over", "sb.scalar.refusal_over"},
    {"SBSQL-DE49BA68D87E-exclude", "sb.scalar.refusal_exclude"},
    {"SBSQL-E7A6869DB4C1-collate", "sb.scalar.refusal_collate"},
    {"SBSQL-EE837B9B5297-desc", "sb.scalar.refusal_desc"},
    {"SBSQL-73AAF62A5CC3-sys-context-userenv-name", "sb.scalar.refusal_sys_context_userenv_name"},
    {"SBSQL-B26CC22AE57C-rdb-get-context-user-session-name", "sb.scalar.refusal_rdb_get_context_user_session_name"},
    {"SBSQL-33A632BC9375-overlaps", "sb.scalar.refusal_overlaps"},
    {"SBSQL-F89F3F1D7CBB-identity", "sb.scalar.refusal_identity"},
    {"SBSQL-8EA33E3F97F0-array-subquery", "sb.scalar.refusal_array_subquery"},
    {"SBSQL-93C5DE08981B-customer-table", "sb.scalar.refusal_customer_table"},
    {"SBSQL-DCA62654CB0F-at-at", "sb.scalar.refusal_at_at"},
    {"SBSQL-6860D73D6667-named-argument", "sb.scalar.refusal_named_argument"},
    {"SBSQL-971C709406A0-at", "sb.scalar.refusal_at"},
    {"SBSQL-6637546ABDF0-donor-log-mode", "sb.scalar.refusal_donor_log_mode"},
    {"SBSQL-17068E518638-checkpoint-donor-log", "sb.scalar.refusal_checkpoint_donor_log"},
};

SblrValue TextValue(std::string input) {
  SblrValue value;
  value.descriptor_id = "character";
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue BoolValue(bool input) {
  SblrValue value;
  value.descriptor_id = "boolean";
  value.payload_kind = SblrValuePayloadKind::boolean;
  value.is_null = false;
  value.has_int64_value = true;
  value.int64_value = input ? 1 : 0;
  value.encoded_value = input ? "TRUE" : "FALSE";
  value.text_value = value.encoded_value;
  return value;
}

bool HasDiagnostic(const scratchbird::engine::sblr::SblrResult& result, std::string_view id) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == id) return true;
  }
  return false;
}

bool HasDiagnosticDetail(const scratchbird::engine::sblr::SblrResult& result,
                         std::string_view id,
                         std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == id && diagnostic.detail == detail) return true;
  }
  return false;
}

scratchbird::engine::sblr::SblrResult Run(const FunctionRegistry& registry,
                                          std::string function_id,
                                          std::vector<SblrValue> values = {},
                                          std::optional<std::uint64_t> last_row_count = std::nullopt,
                                          std::optional<std::string> current_sqlstate = std::nullopt) {
  FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.session_uuid = "019e1600-0000-7000-8000-000000000016";
  request.context.sblr_context.node_uuid = "019e1600-0000-7000-8000-0000000000dd";
  request.context.sblr_context.database_uuid = "019e1600-0000-7000-8000-0000000000db";
  request.context.sblr_context.user_uuid = "019e1600-0000-7000-8000-000000000001";
  request.context.sblr_context.transaction_uuid = "019e1600-0000-7000-8000-0000000000aa";
  request.context.sblr_context.statement_uuid = "019e1600-0000-7000-8000-0000000000bb";
  request.context.sblr_context.current_diagnostic_uuid = "019e1600-0000-7000-8000-0000000000cc";
  request.context.sblr_context.local_transaction_id = 16016;
  request.context.sblr_context.transaction_isolation_level = "snapshot";
  request.context.sblr_context.current_sqlstate = "00000";
  if (current_sqlstate.has_value()) {
    request.context.sblr_context.current_sqlstate = *current_sqlstate;
  }
  request.context.sblr_context.client_protocol_uuid = "019e1600-0000-7000-8000-0000000000ee";
  request.context.sblr_context.application_name = "sbsql_conformance";
  request.context.sblr_context.transaction_context_present = true;
  if (last_row_count.has_value()) {
    request.context.sblr_context.last_row_count = *last_row_count;
    request.context.sblr_context.last_row_count_present = true;
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    request.arguments.push_back(FunctionArgument{"arg" + std::to_string(i), std::move(values[i])});
  }
  return DispatchFunctionCall(registry, std::move(request)).result;
}

bool ExpectOkScalar(const scratchbird::engine::sblr::SblrResult& result, std::string_view case_id) {
  if (!result.ok() || result.scalar_values.size() != 1) {
    std::cerr << case_id << ": expected one successful scalar result\n";
    return false;
  }
  return true;
}

bool ExpectText(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view expected,
                std::string_view descriptor = "character") {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.text_value != expected || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected " << descriptor << " " << expected << ", got "
              << value.descriptor_id << " " << value.text_value << "\n";
    return false;
  }
  return true;
}

bool ExpectUint64(std::string_view case_id,
                  const scratchbird::engine::sblr::SblrResult& result,
                  std::uint64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_uint64_value || value.uint64_value != expected || value.descriptor_id != "uint64") {
    std::cerr << case_id << ": expected uint64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectInt64(std::string_view case_id,
                 const scratchbird::engine::sblr::SblrResult& result,
                 std::int64_t expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (value.is_null || !value.has_int64_value || value.int64_value != expected || value.descriptor_id != "int64") {
    std::cerr << case_id << ": expected int64 " << expected << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectBoolean(std::string_view case_id,
                   const scratchbird::engine::sblr::SblrResult& result,
                   bool expected) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  const std::int64_t expected_value = expected ? 1 : 0;
  if (value.is_null || !value.has_int64_value || value.int64_value != expected_value ||
      value.descriptor_id != "boolean") {
    std::cerr << case_id << ": expected boolean " << expected_value << ", got "
              << value.descriptor_id << " " << value.encoded_value << "\n";
    return false;
  }
  return true;
}

bool ExpectNull(std::string_view case_id,
                const scratchbird::engine::sblr::SblrResult& result,
                std::string_view descriptor) {
  if (!ExpectOkScalar(result, case_id)) return false;
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != descriptor) {
    std::cerr << case_id << ": expected NULL descriptor " << descriptor << ", got "
              << value.descriptor_id << "\n";
    return false;
  }
  return true;
}

bool ExpectFailureDetail(std::string_view case_id,
                         const scratchbird::engine::sblr::SblrResult& result,
                         std::string_view diagnostic_id,
                         std::string_view detail) {
  if (result.status == SblrStatusCode::ok || !HasDiagnostic(result, diagnostic_id) ||
      !HasDiagnosticDetail(result, diagnostic_id, detail)) {
    std::cerr << case_id << ": expected diagnostic " << diagnostic_id << " detail " << detail << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  const auto package = BuildStandardFunctionSeedPackage();
  const auto& registry = package.registry;
  bool ok = true;

  ok = ExpectText("session_id",
                  Run(registry, "sb.session.session_id"),
                  "019e1600-0000-7000-8000-000000000016",
                  "uuid") && ok;
  ok = ExpectUint64("transaction_id", Run(registry, "sb.session.transaction_id"), 16016) && ok;
  ok = ExpectText("transaction_uuid",
                  Run(registry, "sb.session.transaction_uuid"),
                  "019e1600-0000-7000-8000-0000000000aa",
                  "uuid") && ok;
  ok = ExpectText("current_statement_uuid",
                  Run(registry, "sb.session.current_statement_uuid"),
                  "019e1600-0000-7000-8000-0000000000bb",
                  "uuid") && ok;
  ok = ExpectUint64("row_count",
                    Run(registry, "sb.fn.diagnostic.row_count", {}, std::uint64_t{7}),
                    7) && ok;
  ok = ExpectNull("row_count_no_context",
                  Run(registry, "sb.fn.diagnostic.row_count"),
                  "uint64") && ok;
  ok = ExpectText("session_user",
                  Run(registry, "sb.session.session_user"),
                  "019e1600-0000-7000-8000-000000000001",
                  "uuid") && ok;
  ok = ExpectText("system_user",
                  Run(registry, "sb.session.system_user"),
                  "019e1600-0000-7000-8000-000000000001",
                  "uuid") && ok;
  ok = ExpectText("user",
                  Run(registry, "sb.session.user"),
                  "019e1600-0000-7000-8000-000000000001",
                  "uuid") && ok;
  ok = ExpectText("current_server",
                  Run(registry, "sb.session.current_server"),
                  "019e1600-0000-7000-8000-0000000000dd",
                  "uuid") && ok;

  ok = ExpectText("server_version", Run(registry, "sb.scalar.server_version"), "ScratchBird 0.1.0") && ok;
  ok = ExpectUint64("server_version_num", Run(registry, "sb.scalar.server_version_num"), 100) && ok;
  ok = ExpectText("current_setting_timezone", Run(registry, "sb.scalar.current_setting_timezone"), "UTC") && ok;
  ok = ExpectText("current_setting_bare_known",
                  Run(registry, "sb.scalar.current_setting", {TextValue("timezone")}),
                  "UTC") && ok;
  ok = ExpectText("current_setting_known", Run(registry, "sb.scalar.current_setting", {TextValue("timezone")}), "UTC") && ok;
  ok = ExpectText("application_name",
                  Run(registry, "sb.scalar.application_name"),
                  "sbsql_conformance") && ok;
  ok = ExpectText("mga_isolation_profile",
                  Run(registry, "sb.scalar.mga_isolation_profile"),
                  "snapshot_transaction") && ok;
  ok = ExpectFailureDetail("current_setting_unknown_refusal",
                           Run(registry, "sb.scalar.current_setting", {TextValue("not_a_setting")}),
                           "SB_DIAG_FUNCTION_INVALID_INPUT",
                           "current_setting setting is unknown") && ok;
  ok = ExpectFailureDetail("current_setting_var_literal_refusal",
                           Run(registry, "sb.scalar.current_setting", {TextValue("var")}),
                           "SB_DIAG_FUNCTION_INVALID_INPUT",
                           "current_setting setting is unknown") && ok;
  ok = ExpectFailureDetail("current_setting_autocommit_literal_refusal",
                           Run(registry, "sb.scalar.current_setting", {TextValue("autocommit")}),
                           "SB_DIAG_FUNCTION_INVALID_INPUT",
                           "current_setting setting is unknown") && ok;
  ok = ExpectNull("current_setting_unknown_missing_ok",
                  Run(registry, "sb.scalar.current_setting", {TextValue("not_a_setting"), BoolValue(true)}),
                  "character") && ok;
  for (const auto& row : kPolicyRefusalRuntimeRows) {
    ok = ExpectFailureDetail(row.case_id,
                             Run(registry, row.function_id),
                             "SB_DIAG_FUNCTION_RUNTIME_REFUSAL",
                             "public SBsql surface is refused by fixed policy before engine side effects") && ok;
  }

  ok = ExpectUint64("SBSQL-F824C47A36A5-array_max_dimension-fixed_policy_limit",
                    Run(registry, "sb.scalar.array_max_dimension"),
                    6) && ok;
  ok = ExpectUint64("SBSQL-BCAC432F4C75-array_max_element_count-fixed_policy_limit",
                    Run(registry, "sb.scalar.array_max_element_count"),
                    1048576) && ok;
  ok = ExpectUint64("SBSQL-6C698B54A7CB-case_when_max_branches-fixed_policy_limit",
                    Run(registry, "sb.scalar.case_when_max_branches"),
                    1024) && ok;
  ok = ExpectUint64("SBSQL-8EAA8898DBEB-cte_max_count_per_statement-fixed_policy_limit",
                    Run(registry, "sb.scalar.cte_max_count_per_statement"),
                    1024) && ok;
  ok = ExpectUint64("SBSQL-2CF7F4318343-nested_subquery_max_depth-fixed_policy_limit",
                    Run(registry, "sb.scalar.nested_subquery_max_depth"),
                    256) && ok;
  ok = ExpectUint64("SBSQL-8A442BFCB429-recursive_cte_max_depth-fixed_policy_limit",
                    Run(registry, "sb.scalar.recursive_cte_max_depth"),
                    1024) && ok;
  ok = ExpectUint64("SBSQL-189B5E58953A-result_set_max_columns-fixed_policy_limit",
                    Run(registry, "sb.scalar.result_set_max_columns"),
                    4096) && ok;
  ok = ExpectUint64("SBSQL-7998B79486A5-union_max_arms-fixed_policy_limit",
                    Run(registry, "sb.scalar.union_max_arms"),
                    1024) && ok;
  ok = ExpectText("SBSQL-0A96CD6009B0-numeric_division_by_zero-language_policy",
                  Run(registry, "sb.scalar.numeric_division_by_zero"),
                  "error") && ok;
  ok = ExpectUint64("SBSQL-1076274FA39A-localized_label_max_length_bytes-language_policy",
                    Run(registry, "sb.scalar.localized_label_max_length_bytes"),
                    1024) && ok;
  ok = ExpectText("SBSQL-1DD19CC14460-default_schema_resolution-language_policy",
                  Run(registry, "sb.scalar.default_schema_resolution"),
                  "session_search_path_ambiguous_context_refusal") && ok;
  ok = ExpectUint64("SBSQL-38F7E6CE50C4-result_set_max_rows_in_response-language_policy",
                    Run(registry, "sb.scalar.result_set_max_rows_in_response"),
                    0) && ok;
  ok = ExpectUint64("SBSQL-3B0F02549442-identifier_max_length_bytes-language_policy",
                    Run(registry, "sb.scalar.identifier_max_length_bytes"),
                    255) && ok;
  ok = ExpectUint64("SBSQL-566F25C6A1DC-statement_timeout-language_policy",
                    Run(registry, "sb.scalar.statement_timeout"),
                    0) && ok;
  ok = ExpectUint64("SBSQL-63F49DEC1864-statement_timeout_default-language_policy",
                    Run(registry, "sb.scalar.statement_timeout_default"),
                    0) && ok;
  ok = ExpectUint64("SBSQL-48F297900CCF-statement_timeout_ms-language_policy",
                    Run(registry, "sb.scalar.statement_timeout_ms"),
                    0) && ok;
  ok = ExpectText("SBSQL-61AA83D5AF3B-client_min_messages_default-language_policy",
                  Run(registry, "sb.scalar.client_min_messages_default"),
                  "NOTICE") && ok;
  ok = ExpectText("SBSQL-62293C2E8E6C-numeric_overflow_behavior-language_policy",
                  Run(registry, "sb.scalar.numeric_overflow_behavior"),
                  "error") && ok;
  ok = ExpectText("SBSQL-6A71D5F290FE-null_ordering_default_for_asc-language_policy",
                  Run(registry, "sb.scalar.null_ordering_default_for_asc"),
                  "NULLS LAST") && ok;
  ok = ExpectUint64("SBSQL-6DFFFC9C7143-statement_max_length_bytes-language_policy",
                    Run(registry, "sb.scalar.statement_max_length_bytes"),
                    33554432) && ok;
  ok = ExpectBoolean("SBSQL-76AE668F5B32-null_concat_returns_null-language_policy",
                     Run(registry, "sb.scalar.null_concat_returns_null"),
                     true) && ok;
  ok = ExpectUint64("SBSQL-885A9826C368-recursion_max_depth-language_policy",
                    Run(registry, "sb.scalar.recursion_max_depth"),
                    1024) && ok;
  ok = ExpectUint64("SBSQL-8F4843478BD9-parameter_marker_max_count-language_policy",
                    Run(registry, "sb.scalar.parameter_marker_max_count"),
                    262144) && ok;
  ok = ExpectUint64("SBSQL-94568295C808-delimited_identifier_max_length_bytes-language_policy",
                    Run(registry, "sb.scalar.delimited_identifier_max_length_bytes"),
                    255) && ok;
  ok = ExpectText("SBSQL-A0D8F5B44E98-case_resolution_for_quoted_identifiers-language_policy",
                  Run(registry, "sb.scalar.case_resolution_for_quoted_identifiers"),
                  "exact_spelling_match_not_identity") && ok;
  ok = ExpectUint64("SBSQL-A727EFD5293F-temporal_default_precision-language_policy",
                    Run(registry, "sb.scalar.temporal_default_precision"),
                    6) && ok;
  ok = ExpectText("SBSQL-AB0BCEEA8CAD-string_truncation_behavior-language_policy",
                  Run(registry, "sb.scalar.string_truncation_behavior"),
                  "error") && ok;
  ok = ExpectText("SBSQL-ADF52458FA79-timezone_resolution-language_policy",
                  Run(registry, "sb.scalar.timezone_resolution"),
                  "session_timezone_explicit_zone_conversion") && ok;
  ok = ExpectBoolean("SBSQL-B2ACB81B5943-null_in_aggregate_skipped-language_policy",
                     Run(registry, "sb.scalar.null_in_aggregate_skipped"),
                     true) && ok;
  ok = ExpectText("SBSQL-C8724AE0B69E-null_ordering_default_for_desc-language_policy",
                  Run(registry, "sb.scalar.null_ordering_default_for_desc"),
                  "NULLS FIRST") && ok;
  ok = ExpectText("SBSQL-C9B937A0E6A4-interval_default_precision-language_policy",
                  Run(registry, "sb.scalar.interval_default_precision"),
                  "qualifier_driven") && ok;
  ok = ExpectText("SBSQL-CB465EA2790A-name_resolution-language_policy",
                  Run(registry, "sb.scalar.name_resolution"),
                  "uuid_bound_at_bind_time") && ok;
  ok = ExpectText("SBSQL-D231D788EACF-recursive_schema_path_separator-language_policy",
                  Run(registry, "sb.scalar.recursive_schema_path_separator"),
                  ".") && ok;
  ok = ExpectUint64("SBSQL-DBEE94C525A5-identifier_max_length_chars-language_policy",
                    Run(registry, "sb.scalar.identifier_max_length_chars"),
                    63) && ok;
  ok = ExpectUint64("SBSQL-04636D3CDEF5-lock_timeout-language_policy",
                    Run(registry, "sb.scalar.lock_timeout"),
                    0) && ok;
  ok = ExpectUint64("SBSQL-0080B3C2968B-lock_timeout_default-language_policy",
                    Run(registry, "sb.scalar.lock_timeout_default"),
                    0) && ok;
  ok = ExpectUint64("SBSQL-E6D97E527535-lock_timeout_ms-language_policy",
                    Run(registry, "sb.scalar.lock_timeout_ms"),
                    0) && ok;
  ok = ExpectUint64("SBSQL-678A3FA8960F-idle_in_transaction_session_timeout-language_policy",
                    Run(registry, "sb.scalar.idle_in_transaction_session_timeout"),
                    0) && ok;
  ok = ExpectUint64("SBSQL-EF6F8A3F935F-idle_in_transaction_timeout_default-language_policy",
                    Run(registry, "sb.scalar.idle_in_transaction_timeout_default"),
                    0) && ok;
  ok = ExpectUint64("SBSQL-ED07CA49F7D2-idle_in_transaction_session_timeout_ms-language_policy",
                    Run(registry, "sb.scalar.idle_in_transaction_session_timeout_ms"),
                    0) && ok;
  ok = ExpectUint64("SBSQL-902AB93C9666-transaction_timeout-language_policy",
                    Run(registry, "sb.scalar.transaction_timeout"),
                    0) && ok;
  ok = ExpectUint64("SBSQL-9842FF657243-transaction_timeout_default-language_policy",
                    Run(registry, "sb.scalar.transaction_timeout_default"),
                    0) && ok;
  ok = ExpectText("SBSQL-EF1565632553-null_in_unique_constraint-language_policy",
                  Run(registry, "sb.scalar.null_in_unique_constraint"),
                  "multiple_nulls_allowed") && ok;
  ok = ExpectUint64("SBSQL-FFCEFC0CEF1E-qualified_name_max_segments-language_policy",
                    Run(registry, "sb.scalar.qualified_name_max_segments"),
                    16) && ok;
  ok = ExpectBoolean("SBSQL-F53140C3E231-empty_string_equals_null-language_policy",
                     Run(registry, "sb.scalar.empty_string_equals_null"),
                     false) && ok;
  ok = ExpectBoolean("SBSQL-E37283B0B5BD-count_distinct_includes_null-language_policy",
                     Run(registry, "sb.scalar.count_distinct_includes_null"),
                     false) && ok;
  ok = ExpectBoolean("SBSQL-00A225C7BC09-operation_evidence_required-metadata",
                     Run(registry, "sb.scalar.operation_evidence_required"),
                     true) && ok;
  ok = ExpectBoolean("SBSQL-5E9BF65CABA9-decision_proof_required-metadata",
                     Run(registry, "sb.scalar.decision_proof_required"),
                     true) && ok;
  ok = ExpectText("SBSQL-EA1055FD778D-current_capability_set-metadata",
                  Run(registry, "sb.scalar.current_capability_set"),
                  "public_noncluster_alpha") && ok;
  ok = ExpectText("SBSQL-96AC6D338681-current_engine_version-metadata",
                  Run(registry, "sb.scalar.current_engine_version"),
                  "ScratchBird 0.1.0") && ok;
  ok = ExpectText("SBSQL-C69205E8B7F5-application_name-metadata",
                  Run(registry, "sb.scalar.application_name"),
                  "sbsql_conformance") && ok;
  ok = ExpectText("SBSQL-52A87230C51F-current_locale-metadata",
                  Run(registry, "sb.scalar.current_locale"),
                  "en-US") && ok;
  ok = ExpectText("SBSQL-9F79AF739250-client_protocol-metadata",
                  Run(registry, "sb.scalar.client_protocol"),
                  "019e1600-0000-7000-8000-0000000000ee",
                  "uuid") && ok;
  ok = ExpectBoolean("SBSQL-8EF55DAAC17F-private_profile_active-metadata",
                     Run(registry, "sb.scalar.private_profile_active"),
                     false) && ok;
  ok = ExpectText("SBSQL-C41110D1C709-built_in_function_shadow_rule-metadata",
                  Run(registry, "sb.scalar.built_in_function_shadow_rule"),
                  "deny_builtin_shadowing") && ok;
  ok = ExpectText("SBSQL-3D23D96C580E-current_isolation_level-metadata",
                  Run(registry, "sb.scalar.current_isolation_level"),
                  "snapshot") && ok;
  ok = ExpectText("SBSQL-0D3AF5689337-mga_isolation_profile-metadata",
                  Run(registry, "sb.scalar.mga_isolation_profile"),
                  "snapshot_transaction") && ok;
  ok = ExpectBoolean("SBSQL-33ABD23561B2-tx_read_only-metadata",
                     Run(registry, "sb.scalar.tx_read_only"),
                     false) && ok;
  ok = ExpectBoolean("SBSQL-6E6DF1F1D3F5-read_only_session-metadata",
                     Run(registry, "sb.scalar.read_only_session"),
                     false) && ok;
  ok = ExpectBoolean("SBSQL-48630666FF4B-request_key_required-metadata",
                     Run(registry, "sb.scalar.request_key_required"),
                     true) && ok;
  ok = ExpectText("SBSQL-493876D2FFDB-sbsql_v3-metadata",
                  Run(registry, "sb.scalar.sbsql_v3"),
                  "sbsql.v3") && ok;
  ok = ExpectText("SBSQL-95EFD6F591B0-sqlstate-metadata",
                  Run(registry, "sb.scalar.sqlstate"),
                  "00000") && ok;
  ok = ExpectInt64("SBSQL-B5B0AC8A7813-sqlcode-metadata",
                   Run(registry, "sb.scalar.sqlcode"),
                   0) && ok;
  ok = ExpectText("SBSQL-9487E723F6DB-sqlerrm-metadata",
                  Run(registry, "sb.scalar.sqlerrm"),
                  "OK") && ok;
  ok = ExpectBoolean("SBSQL-26096EC6FBAF-not-found-default-context",
                     Run(registry, "sb.scalar.not_found"),
                     false) && ok;
  ok = ExpectBoolean("SBSQL-26096EC6FBAF-not-found-no-data-context",
                     Run(registry, "sb.scalar.not_found", {}, std::nullopt, std::string("02000")),
                     true) && ok;
  ok = ExpectFailureDetail("SBSQL-B7E4638E5F7C-signal-diagnostic",
                           Run(registry, "sb.scalar.signal"),
                           "SB_DIAG_PROCEDURAL_SIGNAL",
                           "SIGNAL emitted SBsql procedural diagnostic") && ok;
  ok = ExpectFailureDetail("SBSQL-22A8D96173B3-raise-diagnostic",
                           Run(registry, "sb.scalar.raise"),
                           "SB_DIAG_PROCEDURAL_RAISE",
                           "RAISE emitted SBsql procedural diagnostic") && ok;
  ok = ExpectFailureDetail("SBSQL-7390531C3071-resignal-diagnostic",
                           Run(registry, "sb.scalar.resignal"),
                           "SB_DIAG_PROCEDURAL_RESIGNAL",
                           "RESIGNAL emitted SBsql procedural diagnostic") && ok;
  ok = ExpectText("SBSQL-14D576F7019D-deprecated_keyword-metadata",
                  Run(registry, "sb.scalar.deprecated_keyword"),
                  "keyword_class.deprecated") && ok;
  ok = ExpectText("SBSQL-21B3D26B555C-donor_contextual_keyword-metadata",
                  Run(registry, "sb.scalar.donor_contextual_keyword"),
                  "keyword_class.donor_contextual") && ok;
  ok = ExpectText("SBSQL-7036F89856D2-reserved_native_keyword-metadata",
                  Run(registry, "sb.scalar.reserved_native_keyword"),
                  "keyword_class.reserved_native") && ok;
  ok = ExpectText("SBSQL-FA8B706E49D0-contextual_native_keyword-metadata",
                  Run(registry, "sb.scalar.contextual_native_keyword"),
                  "keyword_class.contextual_native") && ok;
  ok = ExpectText("SBSQL-813817A7EDFD-donor_reserved_keyword-metadata",
                  Run(registry, "sb.scalar.donor_reserved_keyword"),
                  "keyword_class.donor_reserved") && ok;
  ok = ExpectText("SBSQL-C1E6BF629293-meta_command_keyword-metadata",
                  Run(registry, "sb.scalar.meta_command_keyword"),
                  "keyword_class.meta_command") && ok;
  ok = ExpectText("SBSQL-92C51F4C0F42-private_only_keyword-metadata",
                  Run(registry, "sb.scalar.private_only_keyword"),
                  "keyword_class.private_only") && ok;
  ok = ExpectText("SBSQL-2150B810CBA5-refusal_only_keyword-metadata",
                  Run(registry, "sb.scalar.refusal_only_keyword"),
                  "keyword_class.refusal_only") && ok;
  ok = ExpectText("SBSQL-D8C32B223686-statement_terminator-metadata",
                  Run(registry, "sb.scalar.statement_terminator"),
                  "lexeme.statement_terminator") && ok;
  ok = ExpectText("SBSQL-9FB7E7E0066C-comment_line-metadata",
                  Run(registry, "sb.scalar.comment_line"),
                  "lexeme.comment_line") && ok;
  ok = ExpectText("SBSQL-BB0BB989E8B2-comment_block-metadata",
                  Run(registry, "sb.scalar.comment_block"),
                  "lexeme.comment_block") && ok;
  ok = ExpectText("SBSQL-80864EB79EEB-current_request_uuid-metadata",
                  Run(registry, "sb.scalar.current_request_uuid"),
                  "019e1600-0000-7000-8000-0000000000bb",
                  "uuid") && ok;
  ok = ExpectText("SBSQL-5CDBD2168B18-current_dialect_version-metadata",
                  Run(registry, "sb.scalar.current_dialect_version"),
                  "sbsql.v3") && ok;
  ok = ExpectText("SBSQL-26BCBFF1FEED-cardinality_violation-metadata",
                  Run(registry, "sb.scalar.cardinality_violation"),
                  "21000") && ok;
  ok = ExpectText("SBSQL-0032FE107FA7-currency-metadata",
                  Run(registry, "sb.scalar.currency"),
                  "datatype.currency") && ok;
  ok = ExpectText("SBSQL-0382B5F327AA-client_min_messages-metadata",
                  Run(registry, "sb.scalar.client_min_messages"),
                  "NOTICE") && ok;
  ok = ExpectText("SBSQL-03FC991DBA65-merge_action-metadata",
                  Run(registry, "sb.scalar.merge_action"),
                  "dml.merge_action") && ok;
  ok = ExpectText("SBSQL-0D36181A09C4-colocation-metadata",
                  Run(registry, "sb.scalar.colocation"),
                  "physical_layout.colocation") && ok;
  ok = ExpectText("SBSQL-1FF40A008189-identifier_bare-metadata",
                  Run(registry, "sb.scalar.identifier_bare"),
                  "identifier_class.bare") && ok;
  ok = ExpectText("SBSQL-2477F077886D-search_path-metadata",
                  Run(registry, "sb.scalar.search_path"),
                  "users.public") && ok;
  ok = ExpectText("SBSQL-27CC3A88C9CA-sbsql_psql-metadata",
                  Run(registry, "sb.scalar.sbsql_psql"),
                  "sbsql.psql") && ok;
  ok = ExpectText("SBSQL-416D56ED915F-sql_variant-metadata",
                  Run(registry, "sb.scalar.sql_variant"),
                  "datatype.sql_variant") && ok;
  ok = ExpectUint64("SBSQL-460F18A49506-random_seed-metadata",
                    Run(registry, "sb.scalar.random_seed"),
                    0ULL) && ok;
  ok = ExpectUint64("SBSQL-461B56193B9B-recursion_limit-metadata",
                    Run(registry, "sb.scalar.recursion_limit"),
                    1024ULL) && ok;
  ok = ExpectText("SBSQL-6DE72BA55723-performance-metadata",
                  Run(registry, "sb.scalar.performance"),
                  "management.performance") && ok;
  ok = ExpectText("SBSQL-72FA26BECAC5-parser_only-metadata",
                  Run(registry, "sb.scalar.parser_only"),
                  "scope.parser_only") && ok;
  ok = ExpectText("SBSQL-73BA9BC6005B-deprecated-metadata",
                  Run(registry, "sb.scalar.deprecated"),
                  "keyword_class.deprecated") && ok;
  ok = ExpectText("SBSQL-7D66C2236A2B-filesystem-metadata",
                  Run(registry, "sb.scalar.filesystem"),
                  "scope.filesystem_public_refused") && ok;
  ok = ExpectText("SBSQL-80B16048C80F-refuse-metadata",
                  Run(registry, "sb.scalar.refuse"),
                  "decision.refuse") && ok;
  ok = ExpectText("SBSQL-823D51652BD0-metrics-metadata",
                  Run(registry, "sb.scalar.metrics"),
                  "management.metrics") && ok;
  ok = ExpectText("SBSQL-85D62227A882-catalog_read-metadata",
                  Run(registry, "sb.scalar.catalog_read"),
                  "authority.catalog_read") && ok;
  ok = ExpectText("SBSQL-87F8BED4D9EE-donor_log_compatibility-metadata",
                  Run(registry, "sb.scalar.donor_log_compatibility"),
                  "compatibility.donor_log_non_authority") && ok;
  ok = ExpectText("SBSQL-92C226CF5A7A-fail_closed-metadata",
                  Run(registry, "sb.scalar.fail_closed"),
                  "decision.fail_closed") && ok;
  ok = ExpectText("SBSQL-980131EAA57E-requires_new_function-metadata",
                  Run(registry, "sb.scalar.requires_new_function"),
                  "implementation.requires_new_function") && ok;
  ok = ExpectText("SBSQL-A069D1ED14C0-random_seed_control-metadata",
                  Run(registry, "sb.scalar.random_seed_control"),
                  "policy.random_seed_control") && ok;
  ok = ExpectText("SBSQL-B777E985366D-evidence-metadata",
                  Run(registry, "sb.scalar.evidence"),
                  "evidence.required") && ok;
  ok = ExpectText("SBSQL-B845A701EF3C-private_profile_read-metadata",
                  Run(registry, "sb.scalar.private_profile_read"),
                  "authority.private_profile_read") && ok;
  ok = ExpectText("SBSQL-C9883DF74D82-evidence_chain_uuid-metadata",
                  Run(registry, "sb.scalar.evidence_chain_uuid"),
                  "019e1600-0000-7000-8000-0000000000bb",
                  "uuid") && ok;
  ok = ExpectText("SBSQL-CE7F2EE0D34E-parameter_marker-metadata",
                  Run(registry, "sb.scalar.parameter_marker"),
                  "token.parameter_marker") && ok;
  ok = ExpectText("SBSQL-D437EC74B872-security-metadata",
                  Run(registry, "sb.scalar.security"),
                  "management.security") && ok;
  ok = ExpectText("SBSQL-DC3ADB63538F-localized_label-metadata",
                  Run(registry, "sb.scalar.localized_label"),
                  "label.localized") && ok;
  ok = ExpectText("SBSQL-E302317C73E2-policy_blocked-metadata",
                  Run(registry, "sb.scalar.policy_blocked"),
                  "decision.policy_blocked") && ok;
  ok = ExpectText("SBSQL-E9EC607BA6D8-notice-metadata",
                  Run(registry, "sb.scalar.notice"),
                  "NOTICE") && ok;
  ok = ExpectText("SBSQL-F1C822127E64-dictionary_encoded-metadata",
                  Run(registry, "sb.scalar.dictionary_encoded"),
                  "encoding.dictionary") && ok;
  ok = ExpectText("SBSQL-06DAC31C3A89-unresolved-metadata",
                  Run(registry, "sb.scalar.unresolved"),
                  "decision.unresolved") && ok;
  ok = ExpectText("SBSQL-0FEC8BF7CD54-public-metadata",
                  Run(registry, "sb.scalar.public"),
                  "schema.public") && ok;
  ok = ExpectText("SBSQL-112E5B307AE1-tablegroup-metadata",
                  Run(registry, "sb.scalar.tablegroup"),
                  "physical_layout.tablegroup") && ok;
  ok = ExpectText("SBSQL-131758ECDAEC-none-metadata",
                  Run(registry, "sb.scalar.none"),
                  "value.none") && ok;
  ok = ExpectText("SBSQL-15828625DD4A-descriptor-metadata",
                  Run(registry, "sb.scalar.descriptor"),
                  "metadata.descriptor") && ok;
  ok = ExpectText("SBSQL-18E2D41E637B-engine-metadata",
                  Run(registry, "sb.scalar.engine"),
                  "authority.engine") && ok;
  ok = ExpectText("SBSQL-3456DC1DC1E6-catalog-metadata",
                  Run(registry, "sb.scalar.catalog"),
                  "authority.catalog") && ok;
  ok = ExpectText("SBSQL-456D4BF70496-unsupported-metadata",
                  Run(registry, "sb.scalar.unsupported"),
                  "decision.unsupported") && ok;
  ok = ExpectText("SBSQL-46D0BFB6ED61-mergetree-metadata",
                  Run(registry, "sb.scalar.mergetree"),
                  "physical_layout.mergetree") && ok;
  ok = ExpectText("SBSQL-6AA173CC38F1-refused-metadata",
                  Run(registry, "sb.scalar.refused"),
                  "decision.refused") && ok;
  ok = ExpectText("SBSQL-70117DFD73D9-innodb-metadata",
                  Run(registry, "sb.scalar.innodb"),
                  "donor_storage.innodb") && ok;
  ok = ExpectText("SBSQL-902E2EF680C8-hnsw-metadata",
                  Run(registry, "sb.scalar.hnsw"),
                  "index_method.hnsw") && ok;
  ok = ExpectText("SBSQL-98F7C17D42D9-sessions-metadata",
                  Run(registry, "sb.scalar.sessions"),
                  "management.sessions") && ok;
  ok = ExpectText("SBSQL-9B1E3F7A4C5F-asof-metadata",
                  Run(registry, "sb.scalar.asof"),
                  "temporal.asof") && ok;
  ok = ExpectText("SBSQL-A19CEADCF8F5-post_event-metadata",
                  Run(registry, "sb.scalar.post_event"),
                  "event.post") && ok;
  ok = ExpectText("SBSQL-A770228DEC74-regional-metadata",
                  Run(registry, "sb.scalar.regional"),
                  "locality.regional") && ok;
  ok = ExpectText("SBSQL-B6E1C5B44AAC-ivf_flat-metadata",
                  Run(registry, "sb.scalar.ivf_flat"),
                  "index_method.ivf_flat") && ok;
  ok = ExpectText("SBSQL-BB09AA368A93-tsquery-metadata",
                  Run(registry, "sb.scalar.tsquery"),
                  "datatype.tsquery") && ok;
  ok = ExpectText("SBSQL-C549A9F3CF89-client_address-metadata",
                  Run(registry, "sb.scalar.client_address"),
                  "session.client_address") && ok;
  ok = ExpectText("SBSQL-CD402A856EE1-txn-metadata",
                  Run(registry, "sb.scalar.txn"),
                  "transaction.alias") && ok;
  ok = ExpectText("SBSQL-D7D779AE16BC-hierarchyid-metadata",
                  Run(registry, "sb.scalar.hierarchyid"),
                  "datatype.hierarchyid") && ok;
  ok = ExpectText("SBSQL-E217738F653C-locality-metadata",
                  Run(registry, "sb.scalar.locality"),
                  "physical_layout.locality") && ok;
  ok = ExpectText("SBSQL-FD4C6500CBC7-unknown-metadata",
                  Run(registry, "sb.scalar.unknown"),
                  "value.unknown") && ok;
  ok = ExpectText("SBSQL-FE93CA721F82-sortop-metadata",
                  Run(registry, "sb.scalar.sortop"),
                  "operator.sortop") && ok;
  ok = ExpectText("SBSQL-0E3B7964D1F9-customer_id-metadata",
                  Run(registry, "sb.scalar.customer_id"),
                  "fixture.identifier.customer_id") && ok;
  ok = ExpectText("SBSQL-5EF1F41AA6DE-customers-metadata",
                  Run(registry, "sb.scalar.customers"),
                  "fixture.identifier.customers") && ok;
  ok = ExpectText("SBSQL-64EA78126DC3-sep-metadata",
                  Run(registry, "sb.scalar.sep"),
                  "fixture.identifier.sep") && ok;
  ok = ExpectText("SBSQL-976C8ECD388C-part-metadata",
                  Run(registry, "sb.scalar.part"),
                  "fixture.identifier.part") && ok;
  ok = ExpectText("SBSQL-9EF349862A64-value-metadata",
                  Run(registry, "sb.scalar.value"),
                  "fixture.identifier.value") && ok;
  ok = ExpectText("SBSQL-C6DE029570B1-expr-metadata",
                  Run(registry, "sb.scalar.expr"),
                  "fixture.identifier.expr") && ok;
  ok = ExpectText("SBSQL-132BACF604F3-private_surface_refused-metadata",
                  Run(registry, "sb.scalar.private_surface_refused"),
                  "diagnostic.private_surface_refused") && ok;
  ok = ExpectText("SBSQL-2E1DD952FDAF-no_request-metadata",
                  Run(registry, "sb.scalar.no_request"),
                  "diagnostic.no_request") && ok;
  ok = ExpectText("SBSQL-38565DFB4027-no_statement-metadata",
                  Run(registry, "sb.scalar.no_statement"),
                  "diagnostic.no_statement") && ok;
  ok = ExpectText("SBSQL-3AA8E65D9971-unsupported_refused_by_design-metadata",
                  Run(registry, "sb.scalar.unsupported_refused_by_design"),
                  "decision.unsupported_refused_by_design") && ok;
  ok = ExpectText("SBSQL-406B1DB66B8A-operator_overload_unresolved-metadata",
                  Run(registry, "sb.scalar.operator_overload_unresolved"),
                  "diagnostic.operator_overload_unresolved") && ok;
  ok = ExpectText("SBSQL-4762703600F0-no_transaction-metadata",
                  Run(registry, "sb.scalar.no_transaction"),
                  "diagnostic.no_transaction") && ok;
  ok = ExpectText("SBSQL-6ED46344E647-requires_function_authoring-metadata",
                  Run(registry, "sb.scalar.requires_function_authoring"),
                  "implementation.requires_function_authoring") && ok;
  ok = ExpectText("SBSQL-7A84E49081E9-event_trigger_authority_unavailable-metadata",
                  Run(registry, "sb.scalar.event_trigger_authority_unavailable"),
                  "authority.event_trigger_unavailable") && ok;
  ok = ExpectText("SBSQL-7F00278E0723-capability_required-metadata",
                  Run(registry, "sb.scalar.capability_required"),
                  "diagnostic.capability_required") && ok;
  ok = ExpectText("SBSQL-882F0BF84BCC-psql_case_not_found-metadata",
                  Run(registry, "sb.scalar.psql_case_not_found"),
                  "psql.case_not_found") && ok;
  ok = ExpectText("SBSQL-95878BFEDF43-syntax_parser_only-metadata",
                  Run(registry, "sb.scalar.syntax_parser_only"),
                  "syntax.parser_only") && ok;
  ok = ExpectText("SBSQL-A371FE7C3BAA-donor_only_rewrite-metadata",
                  Run(registry, "sb.scalar.donor_only_rewrite"),
                  "donor.rewrite_only") && ok;
  ok = ExpectText("SBSQL-A57396612A09-object_resolution_failed-metadata",
                  Run(registry, "sb.scalar.object_resolution_failed"),
                  "diagnostic.object_resolution_failed") && ok;
  ok = ExpectText("SBSQL-B8E49C049ECB-error_diagnostic_uuid-metadata",
                  Run(registry, "sb.scalar.error_diagnostic_uuid"),
                  "019e1600-0000-7000-8000-0000000000cc",
                  "uuid") && ok;
  ok = ExpectText("SBSQL-91F466E96DE4-transaction-metadata",
                  Run(registry, "sb.scalar.transaction"),
                  "fixture.identifier.transaction") && ok;
  ok = ExpectText("SBSQL-BB49C3D09E24-context_ambiguous-metadata",
                  Run(registry, "sb.scalar.context_ambiguous"),
                  "diagnostic.context_ambiguous") && ok;
  ok = ExpectText("SBSQL-CE3790BA0486-policy_blocked_diagnostic-metadata",
                  Run(registry, "sb.scalar.policy_blocked_diagnostic"),
                  "decision.policy_blocked") && ok;
  ok = ExpectText("SBSQL-CB2705E35D88-diag_sqlstate-metadata",
                  Run(registry, "sb.scalar.diag_sqlstate"),
                  "00000") && ok;
  ok = ExpectText("SBSQL-D2A2D11E9991-canonical_function_idempotency_requirement-metadata",
                  Run(registry, "sb.scalar.canonical_function_idempotency_requirement"),
                  "metadata.idempotency_requirement") && ok;
  ok = ExpectText("SBSQL-D4C7802D088A-deprecation_warning-metadata",
                  Run(registry, "sb.scalar.deprecation_warning"),
                  "warning.deprecation") && ok;
  ok = ExpectText("SBSQL-E01305A870F7-syntax_unsupported-metadata",
                  Run(registry, "sb.scalar.syntax_unsupported"),
                  "syntax.unsupported") && ok;
  ok = ExpectText("SBSQL-E41591430FF1-dynamic_sql_untrusted-metadata",
                  Run(registry, "sb.scalar.dynamic_sql_untrusted"),
                  "sblr.dynamic_sql.untrusted") && ok;
  ok = ExpectText("SBSQL-F4443D288A35-udr_admission_denied-metadata",
                  Run(registry, "sb.scalar.udr_admission_denied"),
                  "sblr.udr.admission_denied") && ok;
  ok = ExpectText("SBSQL-279F22AD9A6E-statement-metadata",
                  Run(registry, "sb.scalar.statement"),
                  "fixture.identifier.statement") && ok;
  ok = ExpectText("SBSQL-4555F205B04B-table-metadata",
                  Run(registry, "sb.scalar.table"),
                  "fixture.identifier.table") && ok;
  ok = ExpectText("SBSQL-4A468A1D2EF5-a-metadata",
                  Run(registry, "sb.scalar.a"),
                  "fixture.identifier.a") && ok;
  ok = ExpectText("SBSQL-4E297DAEBA5B-x-metadata",
                  Run(registry, "sb.scalar.x"),
                  "fixture.identifier.x") && ok;
  ok = ExpectText("SBSQL-ADBBF56B1E71-t-metadata",
                  Run(registry, "sb.scalar.t"),
                  "fixture.identifier.t") && ok;
  ok = ExpectText("SBSQL-C643D29F39E1-name-metadata",
                  Run(registry, "sb.scalar.name"),
                  "fixture.identifier.name") && ok;
  ok = ExpectText("SBSQL-B4D68D87A882-customer-metadata",
                  Run(registry, "sb.scalar.customer"),
                  "fixture.identifier.customer") && ok;
  ok = ExpectText("SBSQL-F81E6A7D24D3-session-metadata",
                  Run(registry, "sb.scalar.session"),
                  "fixture.identifier.session") && ok;
  ok = ExpectText("SBSQL-6B594AD9CCAB-alter-contextual-keyword",
                  Run(registry, "sb.scalar.alter"),
                  "keyword.contextual.alter") && ok;
  ok = ExpectText("SBSQL-0F2568295099-as-contextual-keyword",
                  Run(registry, "sb.scalar.as"),
                  "keyword.contextual.as") && ok;
  ok = ExpectText("SBSQL-054E79ED816E-begin-contextual-keyword",
                  Run(registry, "sb.scalar.begin"),
                  "keyword.contextual.begin") && ok;
  ok = ExpectText("SBSQL-52AD0BC41D43-create-contextual-keyword",
                  Run(registry, "sb.scalar.create"),
                  "keyword.contextual.create") && ok;
  ok = ExpectText("SBSQL-3E7F642D05FF-cross-contextual-keyword",
                  Run(registry, "sb.scalar.cross"),
                  "keyword.contextual.cross") && ok;
  ok = ExpectText("SBSQL-AA7E9547F110-distinct-contextual-keyword",
                  Run(registry, "sb.scalar.distinct"),
                  "keyword.contextual.distinct") && ok;
  ok = ExpectText("SBSQL-D94ED65E33DB-drop-contextual-keyword",
                  Run(registry, "sb.scalar.drop"),
                  "keyword.contextual.drop") && ok;
  ok = ExpectText("SBSQL-6FA47CF68727-else-contextual-keyword",
                  Run(registry, "sb.scalar.else"),
                  "keyword.contextual.else") && ok;
  ok = ExpectText("SBSQL-DAFA3EAF3212-end-contextual-keyword",
                  Run(registry, "sb.scalar.end"),
                  "keyword.contextual.end") && ok;
  ok = ExpectText("SBSQL-09C5727E3E1B-events-contextual-keyword",
                  Run(registry, "sb.scalar.events"),
                  "keyword.contextual.events") && ok;
  ok = ExpectText("SBSQL-169A3A38AFD4-exists-contextual-keyword",
                  Run(registry, "sb.scalar.exists"),
                  "keyword.contextual.exists") && ok;
  ok = ExpectText("SBSQL-003C1703274A-full-contextual-keyword",
                  Run(registry, "sb.scalar.full"),
                  "keyword.contextual.full") && ok;
  ok = ExpectText("SBSQL-DE3A507DF9C4-index-contextual-keyword",
                  Run(registry, "sb.scalar.index"),
                  "keyword.contextual.index") && ok;
  ok = ExpectText("SBSQL-D7351B1C962A-inner-contextual-keyword",
                  Run(registry, "sb.scalar.inner"),
                  "keyword.contextual.inner") && ok;
  ok = ExpectText("SBSQL-E2F9B4091D9B-is-contextual-keyword",
                  Run(registry, "sb.scalar.is"),
                  "keyword.contextual.is") && ok;
  ok = ExpectText("SBSQL-999C058E0160-lateral-contextual-keyword",
                  Run(registry, "sb.scalar.lateral"),
                  "keyword.contextual.lateral") && ok;
  ok = ExpectText("SBSQL-ACA52E0AA80F-natural-contextual-keyword",
                  Run(registry, "sb.scalar.natural"),
                  "keyword.contextual.natural") && ok;
  ok = ExpectText("SBSQL-A1491C2B87CE-on-contextual-keyword",
                  Run(registry, "sb.scalar.on"),
                  "keyword.contextual.on") && ok;
  ok = ExpectText("SBSQL-11D5D2852576-outer-contextual-keyword",
                  Run(registry, "sb.scalar.outer"),
                  "keyword.contextual.outer") && ok;
  ok = ExpectText("SBSQL-907D2DCB4E7A-sequence-contextual-keyword",
                  Run(registry, "sb.scalar.sequence"),
                  "keyword.contextual.sequence") && ok;
  ok = ExpectText("SBSQL-94CD1F009C96-show-contextual-keyword",
                  Run(registry, "sb.scalar.show"),
                  "keyword.contextual.show") && ok;
  ok = ExpectText("SBSQL-05EAA3104951-similar-contextual-keyword",
                  Run(registry, "sb.scalar.similar"),
                  "keyword.contextual.similar") && ok;
  ok = ExpectText("SBSQL-0B5204549537-then-contextual-keyword",
                  Run(registry, "sb.scalar.then"),
                  "keyword.contextual.then") && ok;
  ok = ExpectText("SBSQL-540CB1A2A056-upsert-contextual-keyword",
                  Run(registry, "sb.scalar.upsert"),
                  "keyword.contextual.upsert") && ok;
  ok = ExpectText("SBSQL-00E6C0F7766E-using-contextual-keyword",
                  Run(registry, "sb.scalar.using"),
                  "keyword.contextual.using") && ok;
  ok = ExpectText("SBSQL-AE7A4534FAF9-view-contextual-keyword",
                  Run(registry, "sb.scalar.view"),
                  "keyword.contextual.view") && ok;
  ok = ExpectText("SBSQL-A5E97200C0D9-wait-contextual-keyword",
                  Run(registry, "sb.scalar.wait"),
                  "keyword.contextual.wait") && ok;
  ok = ExpectText("SBSQL-3666F7699C5E-when-contextual-keyword",
                  Run(registry, "sb.scalar.when"),
                  "keyword.contextual.when") && ok;
  ok = ExpectText("SBSQL-CC9235354362-with-contextual-keyword",
                  Run(registry, "sb.scalar.with"),
                  "keyword.contextual.with") && ok;
  ok = ExpectText("SBSQL-853009230194-stmt_null-contextual-keyword",
                  Run(registry, "sb.scalar.stmt_null"),
                  "statement.null") && ok;

  return ok ? 0 : 1;
}
