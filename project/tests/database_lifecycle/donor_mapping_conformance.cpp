// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "firebird_dialect.hpp"
#include "lowering/lowering.hpp"
#include "statement/statement_catalog.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fb = scratchbird::parser::firebird;
namespace sb = scratchbird::parser::sbsql;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

bool Contains(std::string_view value, std::string_view token) {
  return value.find(token) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view token) {
  for (const auto& value : values) {
    if (value == token) return true;
  }
  return false;
}

sb::SessionContext Session() {
  sb::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019e1059-db14-7000-8000-000000000001";
  session.connection_uuid = "019e1059-db14-7000-8000-000000000002";
  session.database_uuid = "019e1059-db14-7000-8000-000000000003";
  session.catalog_epoch = 14;
  session.security_policy_epoch = 15;
  session.descriptor_epoch = 16;
  return session;
}

sb::ParserConfig Config() {
  sb::ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "unix:/tmp/sb_server.sbps.sock";
  config.parser_uuid = "019e1059-db14-7000-8000-000000000004";
  config.bundle_contract_id = "sbp_sbsql@dblc14";
  config.build_id = "dblc-014-donor-mapping";
  return config;
}

sb::SblrEnvelope LowerSbsql(std::string_view sql) {
  const auto cst = sb::BuildCst(sql);
  const auto ast = sb::BuildAst(cst);
  const auto session = Session();
  const auto bound = sb::BindAst(ast, cst, Config(), session);
  return sb::LowerToSblr(bound, cst, session);
}

void CheckSbsqlMappingInventory() {
  std::size_t lifecycle_api = 0;
  std::size_t exact_diagnostic = 0;
  for (const auto& mapping : sb::BuiltinSbsqlLifecycleMappings()) {
    Require(!mapping.mapping_key.empty(), "DBLC-014 SBSQL mapping key missing");
    Require(!mapping.source_dialect.empty(), "DBLC-014 SBSQL source dialect missing");
    Require(!mapping.produces_file_effects, "DBLC-014 SBSQL mapping permits file effects");
    Require(!mapping.parser_executes_sql, "DBLC-014 SBSQL mapping permits parser SQL execution");
    if (mapping.disposition == sb::LifecycleMappingDisposition::kScratchBirdLifecycleApi) {
      ++lifecycle_api;
      Require(!mapping.operation_id.empty(), "DBLC-014 SBSQL lifecycle operation missing");
      Require(!mapping.sblr_operation.empty(), "DBLC-014 SBSQL lifecycle opcode missing");
      Require(!mapping.engine_api_function.empty(), "DBLC-014 SBSQL engine API missing");
      Require(Contains(mapping.operation_id, "lifecycle."), "DBLC-014 SBSQL operation is not lifecycle");
      Require(Contains(mapping.sblr_operation, "SBLR_LIFECYCLE_"), "DBLC-014 SBSQL opcode is not lifecycle");
    } else {
      ++exact_diagnostic;
      Require(mapping.exact_emulated_diagnostic, "DBLC-014 SBSQL emulation diagnostic is not exact");
      Require(!mapping.diagnostic_code.empty(), "DBLC-014 SBSQL diagnostic code missing");
    }
  }
  Require(lifecycle_api >= 15, "DBLC-014 SBSQL lifecycle API coverage incomplete");
  Require(exact_diagnostic >= 4, "DBLC-014 SBSQL non-file diagnostic coverage incomplete");
}

void CheckSbsqlLifecycleLowering(std::string_view sql,
                                 std::string_view operation_id,
                                 std::string_view sblr_operation,
                                 std::string_view engine_api_function) {
  const auto envelope = LowerSbsql(sql);
  const auto verified = sb::VerifySblrEnvelope(envelope);
  Require(verified.admitted, "DBLC-014 SBSQL lifecycle envelope was not admitted");
  Require(envelope.lifecycle_mapping, "DBLC-014 SBSQL lifecycle flag missing");
  Require(envelope.operation_id == operation_id, "DBLC-014 SBSQL operation id mismatch");
  Require(envelope.sblr_opcode == sblr_operation, "DBLC-014 SBSQL opcode mismatch");
  Require(envelope.engine_api_function == engine_api_function, "DBLC-014 SBSQL engine API mismatch");
  Require(envelope.operation_family == "sblr.management.runtime_operation.v3",
          "DBLC-014 SBSQL lifecycle family mismatch");
  Require(!envelope.real_file_effects, "DBLC-014 SBSQL lifecycle permits file effects");
  Require(!envelope.parser_executes_sql, "DBLC-014 SBSQL lifecycle permits parser SQL execution");
  Require(HasValue(envelope.required_authority_steps, "authority.engine.lifecycle_api_required"),
          "DBLC-014 SBSQL engine lifecycle authority step missing");
  Require(HasValue(envelope.required_authority_steps, "authority.engine.mga_lifecycle_evidence_required"),
          "DBLC-014 SBSQL MGA lifecycle authority step missing");
  Require(Contains(envelope.payload, "\"sql_text_included\":false"),
          "DBLC-014 SBSQL lifecycle payload missing SQL text exclusion");
  Require(!Contains(envelope.payload, std::string(sql)), "DBLC-014 SBSQL lifecycle leaked source SQL");
}

void CheckSbsqlExactDiagnostic(std::string_view sql) {
  const auto envelope = LowerSbsql(sql);
  const auto verified = sb::VerifySblrEnvelope(envelope);
  Require(!verified.admitted, "DBLC-014 SBSQL non-file diagnostic was admitted to SBLR");
  Require(envelope.payload.empty(), "DBLC-014 SBSQL non-file diagnostic produced payload");
  Require(envelope.exact_emulated_diagnostic, "DBLC-014 SBSQL non-file diagnostic flag missing");
  Require(envelope.messages.has_errors(), "DBLC-014 SBSQL non-file diagnostic missing error");
}

void CheckSbsqlLifecycleMappings() {
  CheckSbsqlMappingInventory();
  CheckSbsqlLifecycleLowering("CREATE DATABASE 'safe.sbdb'",
                              "lifecycle.create_database",
                              "SBLR_LIFECYCLE_CREATE_DATABASE",
                              "EngineCreateLifecycle");
  CheckSbsqlLifecycleLowering("OPEN DATABASE safe",
                              "lifecycle.open_database",
                              "SBLR_LIFECYCLE_OPEN_DATABASE",
                              "EngineOpenLifecycle");
  CheckSbsqlLifecycleLowering("ATTACH DATABASE safe",
                              "lifecycle.attach_database",
                              "SBLR_LIFECYCLE_ATTACH_DATABASE",
                              "EngineAttachLifecycle");
  CheckSbsqlLifecycleLowering("DETACH DATABASE safe",
                              "lifecycle.detach_database",
                              "SBLR_LIFECYCLE_DETACH_DATABASE",
                              "EngineDetachLifecycle");
  CheckSbsqlLifecycleLowering("ENTER DATABASE MAINTENANCE safe",
                              "lifecycle.enter_maintenance",
                              "SBLR_LIFECYCLE_ENTER_MAINTENANCE",
                              "EngineEnterMaintenanceLifecycle");
  CheckSbsqlLifecycleLowering("EXIT DATABASE MAINTENANCE safe",
                              "lifecycle.exit_maintenance",
                              "SBLR_LIFECYCLE_EXIT_MAINTENANCE",
                              "EngineExitMaintenanceLifecycle");
  CheckSbsqlLifecycleLowering("ENTER DATABASE RESTRICTED OPEN safe",
                              "lifecycle.enter_restricted_open",
                              "SBLR_LIFECYCLE_ENTER_RESTRICTED_OPEN",
                              "EngineEnterRestrictedOpenLifecycle");
  CheckSbsqlLifecycleLowering("EXIT DATABASE RESTRICTED OPEN safe",
                              "lifecycle.exit_restricted_open",
                              "SBLR_LIFECYCLE_EXIT_RESTRICTED_OPEN",
                              "EngineExitRestrictedOpenLifecycle");
  CheckSbsqlLifecycleLowering("INSPECT DATABASE safe",
                              "lifecycle.inspect_database",
                              "SBLR_LIFECYCLE_INSPECT_DATABASE",
                              "EngineInspectLifecycle");
  CheckSbsqlLifecycleLowering("VERIFY DATABASE safe",
                              "lifecycle.verify_database",
                              "SBLR_LIFECYCLE_VERIFY_DATABASE",
                              "EngineVerifyLifecycle");
  CheckSbsqlLifecycleLowering("REPAIR DATABASE safe",
                              "lifecycle.repair_database",
                              "SBLR_LIFECYCLE_REPAIR_DATABASE",
                              "EngineRepairLifecycle");
  CheckSbsqlLifecycleLowering("SHUTDOWN DATABASE safe",
                              "lifecycle.shutdown_database",
                              "SBLR_LIFECYCLE_SHUTDOWN_DATABASE",
                              "EngineShutdownLifecycle");
  CheckSbsqlLifecycleLowering("SHUTDOWN DATABASE safe FORCE",
                              "lifecycle.shutdown_force",
                              "SBLR_LIFECYCLE_SHUTDOWN_FORCE",
                              "EngineForceShutdownLifecycle");
  CheckSbsqlLifecycleLowering("ACKNOWLEDGE SHUTDOWN DATABASE safe",
                              "lifecycle.shutdown_acknowledge",
                              "SBLR_LIFECYCLE_SHUTDOWN_ACKNOWLEDGE",
                              "EngineAcknowledgeShutdownLifecycle");
  CheckSbsqlLifecycleLowering("DROP DATABASE safe",
                              "lifecycle.drop_database",
                              "SBLR_LIFECYCLE_DROP_DATABASE",
                              "EngineDropLifecycle");
  CheckSbsqlExactDiagnostic("CREATE SHADOW 1 'safe.shd'");
  CheckSbsqlExactDiagnostic("BACKUP DATABASE safe TO 'safe.fbk'");
}

void CheckFirebirdMappingInventory() {
  std::size_t lifecycle_api = 0;
  std::size_t exact_diagnostic = 0;
  for (const auto& mapping : fb::FirebirdLifecycleMappings()) {
    Require(!mapping.mapping_key.empty(), "DBLC-014 Firebird mapping key missing");
    Require(!mapping.produces_file_effects, "DBLC-014 Firebird mapping permits file effects");
    Require(!mapping.donor_engine_sql_executed, "DBLC-014 Firebird mapping permits donor SQL execution");
    if (mapping.disposition == fb::FirebirdMappingDisposition::kScratchBirdLifecycleApi) {
      ++lifecycle_api;
      Require(Contains(mapping.operation_id, "lifecycle."),
              "DBLC-014 Firebird operation is not lifecycle");
      Require(Contains(mapping.sblr_operation, "SBLR_LIFECYCLE_"),
              "DBLC-014 Firebird opcode is not lifecycle");
      Require(!mapping.engine_api_function.empty(), "DBLC-014 Firebird engine API missing");
    } else {
      ++exact_diagnostic;
      Require(mapping.exact_emulated_diagnostic,
              "DBLC-014 Firebird emulation diagnostic is not exact");
    }
  }
  Require(lifecycle_api >= 6, "DBLC-014 Firebird lifecycle API coverage incomplete");
  Require(exact_diagnostic >= 6, "DBLC-014 Firebird non-file diagnostic coverage incomplete");
}

void CheckFirebirdParse(std::string_view sql,
                        std::string_view operation_family,
                        std::string_view operation_id,
                        std::string_view sblr_operation,
                        std::string_view engine_api_function,
                        bool exact_diagnostic) {
  const auto parsed = fb::ParseStatement(sql);
  Require(parsed.ok, "DBLC-014 Firebird parse failed");
  Require(parsed.statement_family == "non_file_emulation" ||
              parsed.statement_family == "isql_frontend",
          "DBLC-014 Firebird statement family mismatch");
  Require(parsed.operation_family == operation_family,
          "DBLC-014 Firebird operation family mismatch");
  Require(parsed.lifecycle_operation_id == operation_id,
          "DBLC-014 Firebird lifecycle operation mismatch");
  Require(parsed.sblr_operation == sblr_operation,
          "DBLC-014 Firebird SBLR operation mismatch");
  Require(parsed.engine_api_function == engine_api_function,
          "DBLC-014 Firebird engine API mismatch");
  Require(parsed.exact_emulated_diagnostic == exact_diagnostic,
          "DBLC-014 Firebird exact diagnostic mismatch");
  Require(!parsed.real_firebird_file_effects,
          "DBLC-014 Firebird parse permits file effects");
  Require(!parsed.donor_engine_sql_executed,
          "DBLC-014 Firebird parse permits donor SQL execution");
  Require(Contains(parsed.sblr_envelope, "\"real_firebird_file_effects\":false"),
          "DBLC-014 Firebird envelope missing file-effect exclusion");
  Require(Contains(parsed.sblr_envelope, "\"donor_engine_sql_executed\":false"),
          "DBLC-014 Firebird envelope missing donor SQL exclusion");
  Require(!Contains(parsed.sblr_envelope, std::string(sql)),
          "DBLC-014 Firebird envelope leaked donor SQL text");
}

void CheckFirebirdExactDiagnostic(std::string_view sql,
                                  std::string_view operation_family) {
  const auto parsed = fb::ParseStatement(sql);
  Require(parsed.ok, "DBLC-014 Firebird diagnostic parse failed");
  Require(parsed.statement_family == "non_file_emulation",
          "DBLC-014 Firebird diagnostic family mismatch");
  Require(parsed.operation_family == operation_family,
          "DBLC-014 Firebird diagnostic operation family mismatch");
  Require(parsed.lifecycle_operation_id.empty(),
          "DBLC-014 Firebird diagnostic unexpectedly mapped lifecycle operation");
  Require(parsed.exact_emulated_diagnostic,
          "DBLC-014 Firebird diagnostic exact flag missing");
  Require(Contains(parsed.message_vector_json, "FIREBIRD.EMULATION.NON_FILE_SURFACE"),
          "DBLC-014 Firebird diagnostic code missing");
  Require(Contains(parsed.message_vector_json, "real_firebird_file_effects"),
          "DBLC-014 Firebird diagnostic file-effect field missing");
  Require(Contains(parsed.sblr_envelope, "\"exact_emulated_diagnostic\":true"),
          "DBLC-014 Firebird diagnostic envelope flag missing");
}

void CheckFirebirdLifecycleMappings() {
  CheckFirebirdMappingInventory();
  CheckFirebirdParse("CREATE DATABASE 'safe.fdb'",
                     "firebird.emulated.database_lifecycle",
                     "lifecycle.create_database",
                     "SBLR_LIFECYCLE_CREATE_DATABASE",
                     "EngineCreateLifecycle",
                     false);
  CheckFirebirdParse("DROP DATABASE",
                     "firebird.emulated.database_lifecycle",
                     "lifecycle.drop_database",
                     "SBLR_LIFECYCLE_DROP_DATABASE",
                     "EngineDropLifecycle",
                     false);
  CheckFirebirdParse("VALIDATE DATABASE",
                     "firebird.emulated.validation_repair_sweep",
                     "lifecycle.verify_database",
                     "SBLR_LIFECYCLE_VERIFY_DATABASE",
                     "EngineVerifyLifecycle",
                     false);
  CheckFirebirdParse("REPAIR DATABASE",
                     "firebird.emulated.validation_repair_sweep",
                     "lifecycle.repair_database",
                     "SBLR_LIFECYCLE_REPAIR_DATABASE",
                     "EngineRepairLifecycle",
                     false);
  CheckFirebirdParse("CONNECT 'safe.fdb'",
                     "firebird.isql.connect",
                     "lifecycle.attach_database",
                     "SBLR_LIFECYCLE_ATTACH_DATABASE",
                     "EngineAttachLifecycle",
                     false);
  CheckFirebirdParse("DISCONNECT",
                     "firebird.isql.disconnect",
                     "lifecycle.detach_database",
                     "SBLR_LIFECYCLE_DETACH_DATABASE",
                     "EngineDetachLifecycle",
                     false);
  CheckFirebirdExactDiagnostic("CREATE SHADOW 1 'safe.shd'",
                               "firebird.emulated.shadow_storage");
  CheckFirebirdExactDiagnostic("BACKUP DATABASE 'safe.fdb' TO 'safe.fbk'",
                               "firebird.emulated.backup_restore");
  CheckFirebirdExactDiagnostic("SERVICE START",
                               "firebird.emulated.service_api");
  Require(Contains(fb::FirebirdLifecycleMappingReportJson(), "DBLC_P14_DONOR_MAPPING_COMPLETE"),
          "DBLC-014 Firebird mapping report missing gate marker");
}

}  // namespace

int main() {
  CheckSbsqlLifecycleMappings();
  CheckFirebirdLifecycleMappings();
  std::cout << "DBLC_P14_DONOR_MAPPING_COMPLETE=passed\n";
  return EXIT_SUCCESS;
}
