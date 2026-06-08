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

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
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

sbsql::SblrEnvelope Lower(std::string sql, std::vector<std::string> resolved = {}) {
  const auto cst = sbsql::BuildCst(sql);
  Require(!cst.messages.has_errors(), "constraint DDL CST failed");
  const auto ast = sbsql::BuildAst(cst);
  if (ast.messages.has_errors()) { DumpDiagnostics(ast.messages); }
  Require(!ast.messages.has_errors(), "constraint DDL AST failed");
  sbsql::ParserConfig config;
  config.server_endpoint = "sb_server_name_resolver";
  const auto bound = sbsql::BindAst(ast, cst, config, Session(), resolved);
  if (bound.messages.has_errors()) { DumpDiagnostics(bound.messages); }
  Require(!bound.messages.has_errors(), "constraint DDL bind failed");
  auto envelope = sbsql::LowerToSblr(bound, cst, Session());
  const auto verified = sbsql::VerifySblrEnvelope(envelope);
  if (!verified.admitted) { DumpDiagnostics(verified.messages); }
  Require(verified.admitted, "constraint DDL SBLR verifier rejected envelope");
  return envelope;
}

void RequireConstraintEnvelope(const sbsql::SblrEnvelope& envelope,
                               std::initializer_list<std::string_view> classes) {
  Require(envelope.operation_family == "sblr.catalog.mutation.v3",
          "constraint DDL did not remain a catalog mutation");
  Require(Contains(envelope.payload, "\"catalog_envelope_kind\":\"constraint_ddl\""),
          "constraint DDL payload missing catalog envelope kind");
  Require(Contains(envelope.payload, "\"catalog_authority\":\"sys.constraint_descriptor\""),
          "constraint DDL payload missing constraint descriptor authority");
  Require(Contains(envelope.payload, "\"logical_constraint_authority\":true"),
          "constraint DDL payload did not mark logical constraint authority");
  Require(Contains(envelope.payload, "\"index_is_derivative_support\":true"),
          "constraint DDL payload did not keep indexes derivative");
  Require(Contains(envelope.payload, "\"parser_executes_sql\":false"),
          "constraint DDL payload allowed parser SQL execution");
  Require(!Contains(envelope.payload, "\"source_text\""),
          "constraint DDL payload embedded source text");
  for (const auto klass : classes) {
    Require(Contains(envelope.payload, std::string("\"") + std::string(klass) + "\""),
            "constraint DDL payload missing expected constraint class");
  }
}

}  // namespace

int main() {
  const auto create = Lower(
      "CREATE TABLE orders (id int NOT NULL PRIMARY KEY, customer_id int REFERENCES customers(id), "
      "amount int DEFAULT 0 CHECK amount);");
  Require(create.operation_id == "ddl.constraint.create",
          "CREATE constraint DDL operation id mismatch");
  RequireConstraintEnvelope(create,
                            {"primary_key",
                             "not_null_constraint",
                             "foreign_key",
                             "default_constraint",
                             "check_constraint"});

  const auto alter = Lower(
      "ALTER TABLE orders ADD CONSTRAINT orders_customer_fk FOREIGN KEY (customer_id) REFERENCES customers(id) DEFERRABLE",
      {"019f0000-0000-7000-8000-000000000201"});
  Require(alter.operation_id == "ddl.constraint.alter",
          "ALTER constraint DDL operation id mismatch");
  Require(Contains(alter.payload, "\"enforcement_timing\":\"transaction_end\""),
          "DEFERRABLE constraint DDL did not mark transaction-end enforcement");
  Require(Contains(alter.payload, "\"deferred_pending_check_store_required\":true"),
          "DEFERRABLE constraint DDL did not require pending-check store");
  RequireConstraintEnvelope(alter, {"foreign_key"});

  std::cout << "constraint_ddl_lowering_conformance=passed\n";
  return EXIT_SUCCESS;
}
