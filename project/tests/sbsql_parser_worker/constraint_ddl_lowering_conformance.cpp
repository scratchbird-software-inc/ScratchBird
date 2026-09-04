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
#include "lowering/lowering.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace sbsql = scratchbird::parser::sbsql;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void DumpDiagnostics(const sbsql::MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ":" << diagnostic.message << '\n';
  }
}

sbsql::SessionContext Session() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000000001";
  session.connection_uuid = "019f0000-0000-7000-8000-000000000002";
  session.database_uuid = "019f0000-0000-7000-8000-000000000003";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 11;
  session.security_policy_epoch = 12;
  session.descriptor_epoch = 13;
  return session;
}

std::string DiagnosticField(const sbsql::Diagnostic& diagnostic,
                            std::string_view name) {
  for (const auto& field : diagnostic.fields) {
    if (field.name == name) return field.value;
  }
  return {};
}

bool HasAuthorityStep(const sbsql::SblrEnvelope& envelope,
                      std::string_view expected) {
  for (const auto& step : envelope.required_authority_steps) {
    if (step == expected) return true;
  }
  return false;
}

sbsql::SblrEnvelope LowerRefused(
    std::string sql,
    std::vector<std::string> resolver_fixture_uuids = {}) {
  const auto cst = sbsql::BuildCst(sql);
  Require(!cst.messages.has_errors(), "constraint DDL CST failed");
  const auto ast = sbsql::BuildAst(cst);
  if (ast.messages.has_errors()) { DumpDiagnostics(ast.messages); }
  Require(!ast.messages.has_errors(), "constraint DDL AST failed");
  sbsql::ParserConfig config;
  config.server_endpoint = "sb_server_name_resolver";
  const auto bound = sbsql::BindAst(ast, cst, config, Session(),
                                    resolver_fixture_uuids);
  if (bound.messages.has_errors()) { DumpDiagnostics(bound.messages); }
  Require(!bound.messages.has_errors(), "constraint DDL bind failed");
  auto envelope = sbsql::LowerToSblr(bound, cst, Session());
  const auto verified = sbsql::VerifySblrEnvelope(envelope);
  if (verified.admitted) {
    std::cerr << "admitted constraint DDL fixture: " << sql << '\n';
  }
  Require(!verified.admitted,
          "constraint DDL without an exact parent descriptor was admitted");
  Require(verified.messages.diagnostics.size() == 1,
          "constraint DDL refusal did not preserve one exact diagnostic");
  return envelope;
}

sbsql::SblrEnvelope LowerUnrelatedCatalogDdl(std::string sql) {
  const auto cst = sbsql::BuildCst(sql);
  Require(!cst.messages.has_errors(), "unrelated catalog DDL CST failed");
  const auto ast = sbsql::BuildAst(cst);
  Require(!ast.messages.has_errors(), "unrelated catalog DDL AST failed");
  sbsql::ParserConfig config;
  config.server_endpoint = "sb_server_name_resolver";
  const auto bound = sbsql::BindAst(ast, cst, config, Session());
  return sbsql::LowerToSblr(bound, cst, Session());
}

void RequireConstraintRefusal(const sbsql::SblrEnvelope& envelope,
                              std::string_view parent_operation,
                              std::string_view parent_opcode) {
  Require(envelope.operation_family == "sblr.catalog.mutation.v3",
          "constraint DDL did not remain a catalog mutation");
  Require(envelope.sblr_operation_key == "sblr.catalog.mutation.v3" &&
              envelope.operation_id == "engine.op.diagnostic_refusal" &&
              envelope.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
              envelope.engine_api_operation_id == "not_admitted",
          "constraint DDL did not use the exact non-executable refusal tuple");
  Require(envelope.result_shape_key == "diagnostic_vector.v1" &&
              envelope.diagnostic_shape_key == "diagnostic_vector.v1" &&
              envelope.resource_contract_key ==
                  "sbsql.command.no_execution.v1",
          "constraint DDL refusal contract metadata drifted");
  Require(envelope.payload.empty() && envelope.operands.empty() &&
              envelope.resolved_object_uuids.empty() &&
              !envelope.parser_executes_sql && !envelope.real_file_effects,
          "constraint DDL refusal emitted executable or parser-owned authority");
  Require(HasAuthorityStep(envelope,
                           "authority.parser.syntax_evidence_only") &&
              HasAuthorityStep(envelope,
                               "authority.parser.no_executable_sblr") &&
              HasAuthorityStep(envelope,
                               "authority.parser.no_sql_text_execution") &&
              HasAuthorityStep(envelope,
                               "authority.parser.no_storage_or_finality"),
          "constraint DDL refusal omitted parser non-authority evidence");
  Require(envelope.messages.diagnostics.size() == 1,
          "constraint DDL lowering did not emit one exact diagnostic");
  const auto& diagnostic = envelope.messages.diagnostics.front();
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
              diagnostic.severity == "ERROR" &&
              DiagnosticField(diagnostic, "canonical_parent_operation_id") ==
                  parent_operation &&
              DiagnosticField(diagnostic, "canonical_parent_sblr_opcode") ==
                  parent_opcode &&
              DiagnosticField(diagnostic, "executable_sblr_emitted") ==
                  "false",
          "constraint DDL refusal identity or parent-table mapping drifted");
}

}  // namespace

int main() {
  const auto create = LowerRefused(
      "CREATE TABLE orders (id int NOT NULL PRIMARY KEY, customer_id int REFERENCES customers(id), "
      "amount int DEFAULT 0 CHECK amount);");
  RequireConstraintRefusal(create, "engine.op.ddl_create_table",
                           "SBLR_DDL_CREATE_TABLE");

  const auto default_then_check = LowerRefused(
      "CREATE TABLE check_only (amount int DEFAULT 0 CHECK (amount > 0));");
  RequireConstraintRefusal(default_then_check, "engine.op.ddl_create_table",
                           "SBLR_DDL_CREATE_TABLE");

  const auto alter = LowerRefused(
      "ALTER TABLE orders ADD CONSTRAINT orders_customer_fk FOREIGN KEY "
      "(customer_id) REFERENCES customers(id) DEFERRABLE",
      {"019f0000-0000-7000-8000-000000000201"});
  RequireConstraintRefusal(alter, "engine.op.ddl_alter_table",
                           "SBLR_DDL_ALTER_TABLE");

  const auto unrelated = LowerUnrelatedCatalogDdl(
      "CREATE UNIQUE INDEX orders_customer_unique ON orders (customer_id);");
  Require(unrelated.trace_key != "trace.sbsql.constraint_ddl_exact_refusal",
          "unrelated catalog DDL was captured as table-constraint DDL");
  for (const auto& diagnostic : unrelated.messages.diagnostics) {
    Require(DiagnosticField(diagnostic, "canonical_parent_operation_id") !=
                "engine.op.ddl_alter_table",
            "unrelated catalog DDL was mislabeled as ALTER TABLE");
  }

  std::cout << "constraint_ddl_fail_closed_conformance=passed\n";
  return EXIT_SUCCESS;
}
