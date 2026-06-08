// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbu_firebird_parser_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool ExpectOk(const scratchbird::udr::firebird_parser_support::UdrResult& result,
              std::string_view label) {
  if (result.ok) return true;
  std::cerr << label << " failed: " << result.message_vector_json << '\n';
  return false;
}

bool ExpectDiagnostic(const scratchbird::udr::firebird_parser_support::UdrResult& result,
                      std::string_view code,
                      std::string_view label) {
  if (!result.ok && Contains(result.message_vector_json, code)) return true;
  std::cerr << label << " diagnostic mismatch: ok=" << result.ok
            << " payload=" << result.payload
            << " message_vector_json=" << result.message_vector_json << '\n';
  return false;
}

constexpr std::string_view kTrustedBridgeContext =
    "engine_context=trusted;bridge_authority=engine;"
    "user_uuid=019e13c0-0000-7000-8000-00000000f001;"
    "request_uuid=019e13c0-0000-7000-8000-00000000f002;"
    "operation_policy_ref=019e13c0-0000-7000-8000-00000000f003;"
    "bridge_connection_uuid=019e13c0-0000-7000-8000-00000000f004;"
    "local_transaction_uuid=019e13c0-0000-7000-8000-00000000f005;"
    "remote_transaction_ref=firebird-remote-txn-1;remote_supports_prepare=true;"
    "cutover_evidence=validated";

} // namespace

int main() {
  using namespace scratchbird::udr::firebird_parser_support;

  constexpr std::string_view trusted_status_context =
      "engine_context=trusted;diagnostic_render=firebird_status_vector";
  constexpr std::string_view trusted_sblr_context =
      "engine_context=trusted;resolver=uuid";
  constexpr std::string_view trusted_dynamic_context =
      "engine_context=trusted;dynamic_sql=psql_execute_statement;resolver=uuid";

  const auto descriptor = sbu_firebird_package_descriptor();
  bool has_bridge_capabilities = false;
  bool has_bridge_dispatch = false;
  for (const auto& entrypoint : descriptor.entrypoints) {
    has_bridge_capabilities =
        has_bridge_capabilities ||
        (entrypoint.name == "sbu_firebird_bridge_capabilities" &&
         entrypoint.role == "bridge.describe_capabilities" &&
         entrypoint.callback != nullptr);
    has_bridge_dispatch =
        has_bridge_dispatch ||
        (entrypoint.name == "sbu_firebird_bridge_dispatch" &&
         entrypoint.role == "bridge.dispatch" &&
         entrypoint.callback != nullptr);
  }
  if (!has_bridge_capabilities || !has_bridge_dispatch) {
    std::cerr << "Firebird package descriptor missing bridge entrypoints\n";
    return EXIT_FAILURE;
  }

  if (!ExpectOk(sbu_firebird_validate_syntax("select 1", "firebird"),
                "validate_syntax")) {
    return EXIT_FAILURE;
  }

  const auto valid_messages = sbu_firebird_validate_syntax("select 1", "firebird");
  const auto success_status =
      sbu_firebird_render_status_vector(valid_messages.message_vector_json,
                                        trusted_status_context);
  if (!ExpectOk(success_status, "status vector success render")) return EXIT_FAILURE;
  if (!Contains(success_status.payload, "\"diagnostic_count\":0") ||
      !Contains(success_status.payload, "\"has_errors\":false") ||
      !Contains(success_status.payload, "\"has_warnings\":false") ||
      !Contains(success_status.payload, "\"primary_sqlcode\":0") ||
      !Contains(success_status.payload, "\"primary_sqlstate\":\"00000\"") ||
      !Contains(success_status.payload, "\"primary_gds_code\":0") ||
      !Contains(success_status.payload, "\"primary_gds_symbol\":\"isc_success\"") ||
      !Contains(success_status.payload, "\"primary_status_arg\":1") ||
      !Contains(success_status.payload, "\"primary_sqlcode_compat\":0") ||
      !Contains(success_status.payload, "\"primary_sqlstate_compat\":\"00000\"") ||
      !Contains(success_status.payload, "\"status_vector_exact\":[{\"arg\":\"isc_arg_gds\",\"value\":0}") ||
      !Contains(success_status.payload, "{\"arg\":\"isc_arg_end\",\"value\":0}")) {
    std::cerr << "status vector success payload mismatch: "
              << success_status.payload << '\n';
    return EXIT_FAILURE;
  }

  constexpr std::string_view warning_message_vector =
      "{\"diagnostics\":[{\"code\":\"FIREBIRD.TEST.WARNING\","
      "\"severity\":\"WARNING\",\"message\":\"seed warning\","
      "\"component\":\"firebird_parser_probe\",\"fields\":{}}]}";
  const auto warning_status =
      sbu_firebird_render_status_vector(warning_message_vector, trusted_status_context);
  if (!ExpectOk(warning_status, "status vector warning render")) return EXIT_FAILURE;
  if (!Contains(warning_status.payload, "\"has_errors\":false") ||
      !Contains(warning_status.payload, "\"has_warnings\":true") ||
      !Contains(warning_status.payload, "\"primary_sqlcode\":100") ||
      !Contains(warning_status.payload, "\"primary_sqlstate\":\"01000\"") ||
      !Contains(warning_status.payload, "\"primary_gds_code\":336003080") ||
      !Contains(warning_status.payload,
                "\"primary_gds_symbol\":\"isc_dsql_warning_number_ambiguous\"") ||
      !Contains(warning_status.payload, "\"primary_status_arg\":18") ||
      !Contains(warning_status.payload, "\"primary_sqlcode_compat\":100") ||
      !Contains(warning_status.payload, "\"primary_sqlstate_compat\":\"01000\"") ||
      !Contains(warning_status.payload, "{\"arg\":\"isc_arg_warning\",\"value\":18}") ||
      !Contains(warning_status.payload,
                "{\"symbol\":\"isc_dsql_warning_number_ambiguous\",\"value\":336003080}") ||
      !Contains(warning_status.payload, "{\"arg\":\"isc_arg_sql_state\",\"value\":19}") ||
      !Contains(warning_status.payload, "\"diagnostic:FIREBIRD.TEST.WARNING\"")) {
    std::cerr << "status vector warning payload mismatch: "
              << warning_status.payload << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectDiagnostic(
          sbu_firebird_render_status_vector(valid_messages.message_vector_json,
                                            "engine_context=trusted"),
          "UDR.FIREBIRD.CONTEXT_MISSING",
          "status vector missing render context")) {
    return EXIT_FAILURE;
  }

  constexpr std::string_view package_message_vector =
      "{\"diagnostics\":[{\"code\":\"FIREBIRD.PACKAGE.PARSER_MISSING\","
      "\"severity\":\"ERROR\",\"message\":\"package missing\","
      "\"component\":\"firebird_package_policy\",\"fields\":{}}]}";
  const auto package_status =
      sbu_firebird_render_status_vector(package_message_vector, trusted_status_context);
  if (!ExpectOk(package_status, "package status render")) return EXIT_FAILURE;
  if (!Contains(package_status.payload, "\"primary_sqlcode\":-901") ||
      !Contains(package_status.payload, "\"primary_sqlstate\":\"0A000\"") ||
      !Contains(package_status.payload, "\"primary_gds_code\":335544378") ||
      !Contains(package_status.payload, "\"primary_gds_symbol\":\"isc_wish_list\"") ||
      !Contains(package_status.payload, "\"status_class\":\"package_admission\"")) {
    std::cerr << "package status payload mismatch: "
              << package_status.payload << '\n';
    return EXIT_FAILURE;
  }

  constexpr std::string_view wire_message_vector =
      "{\"diagnostics\":[{\"code\":\"FIREBIRD.WIRE.BLR_TRUNCATED\","
      "\"severity\":\"ERROR\",\"message\":\"wire malformed\","
      "\"component\":\"firebird_wire_descriptor\",\"fields\":{}}]}";
  const auto wire_status =
      sbu_firebird_render_status_vector(wire_message_vector, trusted_status_context);
  if (!ExpectOk(wire_status, "wire status render")) return EXIT_FAILURE;
  if (!Contains(wire_status.payload, "\"primary_sqlstate\":\"HY000\"") ||
      !Contains(wire_status.payload, "\"primary_gds_code\":335544382") ||
      !Contains(wire_status.payload, "\"primary_gds_symbol\":\"isc_random\"") ||
      !Contains(wire_status.payload, "\"status_class\":\"wire_protocol\"")) {
    std::cerr << "wire status payload mismatch: "
              << wire_status.payload << '\n';
    return EXIT_FAILURE;
  }

  constexpr std::string_view catalog_readonly_message_vector =
      "{\"diagnostics\":[{\"code\":\"FIREBIRD.CATALOG_OVERLAY.READ_ONLY\","
      "\"severity\":\"ERROR\",\"message\":\"catalog readonly\","
      "\"component\":\"sbp_firebird\",\"fields\":{}}]}";
  const auto catalog_status =
      sbu_firebird_render_status_vector(catalog_readonly_message_vector,
                                        trusted_status_context);
  if (!ExpectOk(catalog_status, "catalog status render")) return EXIT_FAILURE;
  if (!Contains(catalog_status.payload, "\"primary_sqlcode\":-551") ||
      !Contains(catalog_status.payload, "\"primary_sqlstate\":\"28000\"") ||
      !Contains(catalog_status.payload, "\"primary_gds_symbol\":\"isc_no_priv\"")) {
    std::cerr << "catalog status payload mismatch: "
              << catalog_status.payload << '\n';
    return EXIT_FAILURE;
  }

  constexpr std::string_view debug_policy_message_vector =
      "{\"diagnostics\":[{\"code\":\"SBU_FIREBIRD.DEBUG_POLICY_DENIED\","
      "\"severity\":\"ERROR\",\"message\":\"debug denied\","
      "\"component\":\"sbu_firebird_parser_support\",\"fields\":{}}]}";
  const auto debug_policy_status =
      sbu_firebird_render_status_vector(debug_policy_message_vector,
                                        trusted_status_context);
  if (!ExpectOk(debug_policy_status, "debug policy status render")) return EXIT_FAILURE;
  if (!Contains(debug_policy_status.payload, "\"primary_sqlcode\":-551") ||
      !Contains(debug_policy_status.payload, "\"primary_sqlstate\":\"28000\"") ||
      !Contains(debug_policy_status.payload, "\"primary_gds_symbol\":\"isc_no_priv\"")) {
    std::cerr << "debug policy status payload mismatch: "
              << debug_policy_status.payload << '\n';
    return EXIT_FAILURE;
  }

  const auto missing_parse_context = sbu_firebird_parse_to_sblr("select 1", "");
  const auto missing_context_status =
      sbu_firebird_render_status_vector(missing_parse_context.message_vector_json,
                                        trusted_status_context);
  if (!ExpectOk(missing_context_status, "missing context status render")) {
    return EXIT_FAILURE;
  }
  if (!Contains(missing_context_status.payload, "\"primary_sqlcode\":-804") ||
      !Contains(missing_context_status.payload, "\"primary_sqlstate\":\"07002\"") ||
      !Contains(missing_context_status.payload, "\"primary_gds_code\":335544380") ||
      !Contains(missing_context_status.payload,
                "\"primary_gds_symbol\":\"isc_wronumarg\"") ||
      !Contains(missing_context_status.payload, "\"status_class\":\"context_missing\"") ||
      !Contains(missing_context_status.payload,
                "\"diagnostic:UDR.FIREBIRD.CONTEXT_MISSING\"")) {
    std::cerr << "missing context status payload mismatch: "
              << missing_context_status.payload << '\n';
    return EXIT_FAILURE;
  }

  const auto normalized = sbu_firebird_normalize("  select    1  ", "firebird");
  if (!ExpectOk(normalized, "normalize")) return EXIT_FAILURE;
  if (normalized.payload != "select 1") {
    std::cerr << "normalize mismatch: " << normalized.payload << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectDiagnostic(sbu_firebird_parse_to_sblr("select 1", ""),
                        "UDR.FIREBIRD.CONTEXT_MISSING",
                        "parse_to_sblr missing context")) {
    return EXIT_FAILURE;
  }

  if (!ExpectDiagnostic(
          sbu_firebird_parse_to_sblr("select 1",
                                     "not_engine_context=trusted;resolver=uuid"),
          "UDR.FIREBIRD.CONTEXT_MISSING",
          "parse_to_sblr spoofed context")) {
    return EXIT_FAILURE;
  }

  if (!ExpectDiagnostic(
          sbu_firebird_parse_to_sblr("select 1",
                                     "engine_context=untrusted;resolver=uuid"),
          "UDR.FIREBIRD.SECURITY_DENIED",
          "parse_to_sblr untrusted context")) {
    return EXIT_FAILURE;
  }

  const auto sblr = sbu_firebird_parse_to_sblr("select 1", trusted_sblr_context);
  if (!ExpectOk(sblr, "parse_to_sblr trusted")) return EXIT_FAILURE;
  if (!Contains(sblr.payload, "SBLRExecutionEnvelope.v3") ||
      Contains(sblr.payload, "select 1")) {
    std::cerr << "SBLR payload mismatch: " << sblr.payload << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectDiagnostic(
          sbu_firebird_parse_concatenated_dynamic_sql("sel", "ect 1", "engine_context=trusted"),
          "UDR.FIREBIRD.CONTEXT_MISSING",
          "concatenated dynamic SQL missing dynamic context")) {
    return EXIT_FAILURE;
  }
  const auto dynamic_sblr = sbu_firebird_parse_concatenated_dynamic_sql(
      "sel", "ect 1", trusted_dynamic_context);
  if (!ExpectOk(dynamic_sblr, "concatenated dynamic SQL")) return EXIT_FAILURE;
  if (!Contains(dynamic_sblr.payload, "\"dynamic_sql_parser\":\"sbu_firebird_parser_support\"") ||
      !Contains(dynamic_sblr.payload, "\"psql_runtime_context\":\"statement\"") ||
      !Contains(dynamic_sblr.payload, "\"uuid_resolution\":\"resolver_context_verified\"") ||
      !Contains(dynamic_sblr.payload, "\"security_verification\":\"trusted_context_default\"") ||
      !Contains(dynamic_sblr.payload, "\"generated_code_admitted\":true") ||
      !Contains(dynamic_sblr.payload, "SBLRExecutionEnvelope.v3") ||
      Contains(dynamic_sblr.payload, "select 1")) {
    std::cerr << "dynamic SQL payload mismatch: " << dynamic_sblr.payload << '\n';
    return EXIT_FAILURE;
  }

  constexpr std::string_view trusted_procedure_dynamic_context =
      "engine_context=trusted;dynamic_sql=psql_execute_statement;resolver=uuid;"
      "psql_runtime=procedure;"
      "caller_procedure_uuid=11111111-2222-3333-4444-555555555555;"
      "security_policy=allow_dynamic_sql";
  const auto procedure_dynamic_sblr = sbu_firebird_parse_concatenated_dynamic_sql(
      "execute block as begin ", "suspend; end", trusted_procedure_dynamic_context);
  if (!ExpectOk(procedure_dynamic_sblr, "procedure concatenated dynamic SQL")) {
    return EXIT_FAILURE;
  }
  if (!Contains(procedure_dynamic_sblr.payload,
                "\"psql_runtime_context\":\"procedure\"") ||
      !Contains(procedure_dynamic_sblr.payload,
                "\"caller_procedure_uuid\":\"11111111-2222-3333-4444-555555555555\"") ||
      !Contains(procedure_dynamic_sblr.payload,
                "\"security_verification\":\"context_policy_allow\"") ||
      !Contains(procedure_dynamic_sblr.payload,
                "\"generated_code_admitted\":true") ||
      Contains(procedure_dynamic_sblr.payload, "execute block")) {
    std::cerr << "procedure dynamic SQL payload mismatch: "
              << procedure_dynamic_sblr.payload << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectDiagnostic(
          sbu_firebird_parse_concatenated_dynamic_sql(
              "sel", "ect 1",
              "engine_context=trusted;dynamic_sql=psql_execute_statement;resolver=uuid;"
              "psql_runtime=procedure"),
          "UDR.FIREBIRD.CONTEXT_MISSING",
          "procedure dynamic SQL missing caller UUID")) {
    return EXIT_FAILURE;
  }

  const auto invalid_dynamic = sbu_firebird_parse_concatenated_dynamic_sql(
      "not", " recognized", trusted_dynamic_context);
  if (!ExpectDiagnostic(invalid_dynamic, "FIREBIRD.PARSE.INVALID_INPUT",
                        "concatenated dynamic SQL invalid input")) {
    return EXIT_FAILURE;
  }
  const auto invalid_dynamic_status =
      sbu_firebird_render_status_vector(invalid_dynamic.message_vector_json,
                                        trusted_status_context);
  if (!ExpectOk(invalid_dynamic_status, "invalid dynamic status render")) {
    return EXIT_FAILURE;
  }
  if (!Contains(invalid_dynamic_status.payload, "\"has_errors\":true") ||
      !Contains(invalid_dynamic_status.payload, "\"has_warnings\":false") ||
      !Contains(invalid_dynamic_status.payload, "\"primary_sqlcode\":-104") ||
      !Contains(invalid_dynamic_status.payload, "\"primary_sqlstate\":\"42000\"") ||
      !Contains(invalid_dynamic_status.payload, "\"primary_gds_code\":335544390") ||
      !Contains(invalid_dynamic_status.payload,
                "\"primary_gds_symbol\":\"isc_syntaxerr\"") ||
      !Contains(invalid_dynamic_status.payload, "\"status_class\":\"parse_syntax\"") ||
      !Contains(invalid_dynamic_status.payload, "\"primary_sqlcode_compat\":-104") ||
      !Contains(invalid_dynamic_status.payload, "\"primary_sqlstate_compat\":\"42000\"") ||
      !Contains(invalid_dynamic_status.payload, "{\"arg\":\"isc_arg_gds\",\"value\":1}") ||
      !Contains(invalid_dynamic_status.payload,
                "{\"symbol\":\"isc_syntaxerr\",\"value\":335544390}") ||
      !Contains(invalid_dynamic_status.payload,
                "\"diagnostic:FIREBIRD.PARSE.INVALID_INPUT\"")) {
    std::cerr << "invalid dynamic status payload mismatch: "
              << invalid_dynamic_status.payload << '\n';
    return EXIT_FAILURE;
  }

  const auto denied_dynamic = sbu_firebird_parse_concatenated_dynamic_sql(
      "sel", "ect 1",
      "engine_context=trusted;dynamic_sql=psql_execute_statement;resolver=uuid;"
      "security_policy=deny_dynamic_sql");
  if (!ExpectDiagnostic(denied_dynamic, "UDR.FIREBIRD.SECURITY_DENIED",
                        "concatenated dynamic SQL security denial")) {
    return EXIT_FAILURE;
  }
  if (!denied_dynamic.payload.empty()) {
    std::cerr << "security denial leaked payload: " << denied_dynamic.payload << '\n';
    return EXIT_FAILURE;
  }
  const auto denial_status =
      sbu_firebird_render_status_vector(denied_dynamic.message_vector_json,
                                        trusted_status_context);
  if (!ExpectOk(denial_status, "security denial status render")) return EXIT_FAILURE;
  if (!Contains(denial_status.payload, "\"has_errors\":true") ||
      !Contains(denial_status.payload, "\"primary_sqlcode\":-551") ||
      !Contains(denial_status.payload, "\"primary_sqlstate\":\"28000\"") ||
      !Contains(denial_status.payload, "\"primary_gds_code\":335544352") ||
      !Contains(denial_status.payload, "\"primary_gds_symbol\":\"isc_no_priv\"") ||
      !Contains(denial_status.payload, "\"status_class\":\"security_denied\"") ||
      !Contains(denial_status.payload, "\"primary_sqlcode_compat\":-551") ||
      !Contains(denial_status.payload, "\"primary_sqlstate_compat\":\"28000\"") ||
      !Contains(denial_status.payload,
                "{\"symbol\":\"isc_no_priv\",\"value\":335544352}") ||
      !Contains(denial_status.payload,
                "\"diagnostic:UDR.FIREBIRD.SECURITY_DENIED\"")) {
    std::cerr << "security denial status payload mismatch: "
              << denial_status.payload << '\n';
    return EXIT_FAILURE;
  }

  const auto described = sbu_firebird_describe_statement(
      "CREATE DATABASE 'x.fdb'", "engine_context=trusted");
  if (!ExpectOk(described, "describe_statement")) return EXIT_FAILURE;
  if (!Contains(described.payload, "\"statement_family\":\"non_file_emulation\"")) {
    std::cerr << "describe_statement payload mismatch: " << described.payload << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectDiagnostic(sbu_firebird_install_environment("", "install"),
                        "UDR.FIREBIRD.CONTEXT_MISSING",
                        "install_environment missing context")) {
    return EXIT_FAILURE;
  }
  const auto installed =
      sbu_firebird_install_environment("engine_context=trusted;catalog_uuid=test", "install");
  if (!ExpectOk(installed, "install_environment")) return EXIT_FAILURE;
  if (!Contains(installed.payload, "\"idempotent\":true") ||
      !Contains(installed.payload, "\"atomic\":true") ||
      !Contains(installed.payload, "\"silent_repair\":false") ||
      !Contains(installed.payload, "\"real_firebird_file_effects\":false")) {
    std::cerr << "install_environment payload mismatch: " << installed.payload << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectDiagnostic(sbu_firebird_install_environment("engine_context=trusted", "install"),
                        "UDR.FIREBIRD.CONTEXT_MISSING",
                        "install_environment missing catalog context")) {
    return EXIT_FAILURE;
  }

  if (!ExpectDiagnostic(
          sbu_firebird_install_environment("engine_context=trusted", "silent_repair"),
          "UDR.FIREBIRD.INSTALL_MODE_INVALID",
          "install_environment invalid mode")) {
    return EXIT_FAILURE;
  }
  const auto invalid_install_mode =
      sbu_firebird_install_environment("engine_context=trusted", "silent_repair");
  const auto invalid_install_status =
      sbu_firebird_render_status_vector(invalid_install_mode.message_vector_json,
                                        trusted_status_context);
  if (!ExpectOk(invalid_install_status, "invalid install mode status render")) {
    return EXIT_FAILURE;
  }
  if (!Contains(invalid_install_status.payload, "\"primary_sqlcode\":-804") ||
      !Contains(invalid_install_status.payload, "\"primary_sqlstate\":\"07002\"") ||
      !Contains(invalid_install_status.payload, "\"primary_gds_code\":335544380") ||
      !Contains(invalid_install_status.payload,
                "\"primary_gds_symbol\":\"isc_wronumarg\"") ||
      !Contains(invalid_install_status.payload, "\"status_class\":\"call_contract\"") ||
      !Contains(invalid_install_status.payload,
                "\"diagnostic:UDR.FIREBIRD.INSTALL_MODE_INVALID\"")) {
    std::cerr << "invalid install mode status payload mismatch: "
              << invalid_install_status.payload << '\n';
    return EXIT_FAILURE;
  }

  const auto verified =
      sbu_firebird_verify_environment("engine_context=trusted;catalog_uuid=test");
  if (!ExpectOk(verified, "verify_environment")) return EXIT_FAILURE;
  if (!Contains(verified.payload, "\"version_drift_detected\":false")) {
    std::cerr << "verify_environment payload mismatch: " << verified.payload << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectDiagnostic(sbu_firebird_debug_capabilities("normal"),
                        "SBU_FIREBIRD.DEBUG_POLICY_DENIED",
                        "debug capabilities policy")) {
    return EXIT_FAILURE;
  }
  if (!ExpectOk(sbu_firebird_debug_capabilities("allow_debug_artifacts"),
                "debug capabilities allowed")) {
    return EXIT_FAILURE;
  }

  const auto bridge_capabilities =
      sbu_firebird_bridge_capabilities("release_evidence");
  if (!ExpectOk(bridge_capabilities, "bridge capabilities")) return EXIT_FAILURE;
  if (!Contains(bridge_capabilities.payload,
                "\"bridge_abi\":\"sb_universal_bridge_v1\"") ||
      !Contains(bridge_capabilities.payload, "\"provider_family\":\"firebird\"") ||
      !Contains(bridge_capabilities.payload, "\"firebird_only\":true") ||
      !Contains(bridge_capabilities.payload,
                "\"logical_backup_restore\":\"remote_stream_only\"") ||
      !Contains(bridge_capabilities.payload,
                "\"physical_page_copy_backup_restore\":\"denied\"") ||
      !Contains(bridge_capabilities.payload, "\"SBLR_BRIDGE_OPEN_SESSION\"") ||
      !Contains(bridge_capabilities.payload,
                "\"cluster_public_implementation\":false")) {
    std::cerr << "bridge capabilities payload mismatch: "
              << bridge_capabilities.payload << '\n';
    return EXIT_FAILURE;
  }
  if (!ExpectDiagnostic(sbu_firebird_bridge_capabilities("normal"),
                        "UDR.BRIDGE.CAPABILITY_POLICY_DENIED",
                        "bridge capabilities policy")) {
    return EXIT_FAILURE;
  }
  if (!ExpectDiagnostic(sbu_firebird_bridge_dispatch("operation=open_session", ""),
                        "UDR.BRIDGE.CONTEXT_MISSING",
                        "bridge dispatch missing context")) {
    return EXIT_FAILURE;
  }
  if (!ExpectDiagnostic(
          sbu_firebird_bridge_dispatch("operation=open_session;password=cleartext",
                                      kTrustedBridgeContext),
          "UDR.BRIDGE.SECRET_MATERIAL_DENIED",
          "bridge dispatch raw secret")) {
    return EXIT_FAILURE;
  }

  const auto bridge_open =
      sbu_firebird_bridge_dispatch("operation=open_session", kTrustedBridgeContext);
  if (!ExpectOk(bridge_open, "bridge open session")) return EXIT_FAILURE;
  if (!Contains(bridge_open.payload, "\"provider_family\":\"firebird\"") ||
      !Contains(bridge_open.payload, "\"firebird_only\":true") ||
      !Contains(bridge_open.payload, "\"operation\":\"open_session\"") ||
      !Contains(bridge_open.payload, "\"SBLR_BRIDGE_OPEN_SESSION\"") ||
      !Contains(bridge_open.payload, "\"parser_authority\":false") ||
      !Contains(bridge_open.payload, "\"udr_transaction_authority\":false") ||
      !Contains(bridge_open.payload,
                "\"mga_transaction_authority\":\"per_database_engine_mga\"") ||
      !Contains(bridge_open.payload, "\"real_firebird_file_effects\":false") ||
      !Contains(bridge_open.payload, "\"raw_secret_material_included\":false")) {
    std::cerr << "bridge open session payload mismatch: "
              << bridge_open.payload << '\n';
    return EXIT_FAILURE;
  }

  const auto bridge_prepare =
      sbu_firebird_bridge_dispatch("operation=prepare", kTrustedBridgeContext);
  if (!ExpectOk(bridge_prepare, "bridge prepare")) return EXIT_FAILURE;
  if (!Contains(bridge_prepare.payload,
                "\"remote_transaction_ref\":\"firebird-remote-txn-1\"")) {
    std::cerr << "bridge prepare missing remote transaction evidence: "
              << bridge_prepare.payload << '\n';
    return EXIT_FAILURE;
  }
  if (!ExpectDiagnostic(
          sbu_firebird_bridge_dispatch(
              "operation=commit",
              "engine_context=trusted;bridge_authority=engine;"
              "user_uuid=u;request_uuid=r;operation_policy_ref=p"),
          "UDR.BRIDGE.CONTEXT_MISSING",
          "bridge commit without local transaction")) {
    return EXIT_FAILURE;
  }

  const auto logical_restore = sbu_firebird_bridge_dispatch(
      "operation=stream_open;stream_kind=gbak_logical_restore",
      kTrustedBridgeContext);
  if (!ExpectOk(logical_restore, "bridge gbak logical restore")) {
    return EXIT_FAILURE;
  }
  if (!Contains(logical_restore.payload,
                "\"firebird_logical_service_stream\":\"remote_stream_admitted\"") ||
      !Contains(logical_restore.payload,
                "\"stream_kind\":\"gbak_logical_restore\"")) {
    std::cerr << "bridge logical restore payload mismatch: "
              << logical_restore.payload << '\n';
    return EXIT_FAILURE;
  }
  if (!ExpectDiagnostic(
          sbu_firebird_bridge_dispatch("operation=stream_open;stream_kind=nbackup",
                                      kTrustedBridgeContext),
          "UDR.BRIDGE.SANDBOX_DENIED",
          "bridge nbackup physical stream")) {
    return EXIT_FAILURE;
  }
  if (!ExpectDiagnostic(
          sbu_firebird_bridge_dispatch("operation=execute;service_action=repair",
                                      kTrustedBridgeContext),
          "UDR.BRIDGE.UNSUPPORTED",
          "bridge repair service action")) {
    return EXIT_FAILURE;
  }
  if (!ExpectDiagnostic(
          sbu_firebird_bridge_dispatch("operation=cdc_apply", kTrustedBridgeContext),
          "UDR.BRIDGE.IDEMPOTENCY_MISSING",
          "bridge cdc apply without idempotency")) {
    return EXIT_FAILURE;
  }
  const auto cdc_apply = sbu_firebird_bridge_dispatch(
      "operation=cdc_apply;idempotency_key=firebird-cdc-1", kTrustedBridgeContext);
  if (!ExpectOk(cdc_apply, "bridge cdc apply")) return EXIT_FAILURE;
  if (!Contains(cdc_apply.payload, "\"SBLR_BRIDGE_CDC_APPLY\"")) {
    std::cerr << "bridge cdc apply payload mismatch: " << cdc_apply.payload << '\n';
    return EXIT_FAILURE;
  }
  if (!ExpectDiagnostic(
          sbu_firebird_bridge_dispatch("operation=cluster.route", kTrustedBridgeContext),
          "UDR.BRIDGE.UNSUPPORTED",
          "bridge cluster disabled")) {
    return EXIT_FAILURE;
  }
  if (!ExpectDiagnostic(
          sbu_firebird_bridge_dispatch(
              "operation=cluster.route",
              std::string(kTrustedBridgeContext) + ";cluster_provider_gate=admitted"),
          "UDR.BRIDGE.UNLICENSED",
          "bridge cluster admitted public stub")) {
    return EXIT_FAILURE;
  }

  const auto physical_denial =
      sbu_firebird_bridge_dispatch("operation=stream_open;stream_kind=nbackup",
                                  kTrustedBridgeContext);
  const auto physical_denial_status =
      sbu_firebird_render_status_vector(physical_denial.message_vector_json,
                                        trusted_status_context);
  if (!ExpectOk(physical_denial_status, "bridge denial status render")) {
    return EXIT_FAILURE;
  }
  if (!Contains(physical_denial_status.payload, "\"primary_sqlcode\":-551") ||
      !Contains(physical_denial_status.payload, "\"primary_sqlstate\":\"28000\"") ||
      !Contains(physical_denial_status.payload,
                "\"diagnostic:UDR.BRIDGE.SANDBOX_DENIED\"")) {
    std::cerr << "bridge denial status payload mismatch: "
              << physical_denial_status.payload << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
