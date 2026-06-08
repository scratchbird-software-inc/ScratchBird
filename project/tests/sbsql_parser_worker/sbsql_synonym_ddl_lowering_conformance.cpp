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
#include "registry/generated/sbsql_generated_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace sbsql = scratchbird::parser::sbsql;

struct SynonymRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view validation_fixture_id;
};

constexpr std::array<SynonymRowEvidence, 3> kSynonymRows{{
    {"SBSQL-A8E627E27375",
     "create_object",
     "canonical_surface",
     "SBSQL-SURFACE-6FF2B844EB94"},
    {"SBSQL-40CAFAB37942",
     "drop_object",
     "canonical_surface",
     "SBSQL-SURFACE-0E678BFF423E"},
    {"SBSQL-CFFCCDEF6AC4",
     "drop_object_stmt",
     "grammar_production",
     "SBSQL-SURFACE-6AF1F9269F93"},
}};

std::string EvidenceMessage(const SynonymRowEvidence& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string rendered(row.surface_id);
  rendered += ' ';
  rendered += row.canonical_name;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

void RequireRegistryEvidence(const SynonymRowEvidence& row) {
  const auto* registry_row = sbsql::FindGeneratedSurfaceRegistryRowById(row.surface_id);
  Require(registry_row != nullptr,
          EvidenceMessage(row, "registry", "missing generated registry row"));
  Require(registry_row->canonical_name == row.canonical_name,
          EvidenceMessage(row, "registry", "canonical name mismatch"));
  Require(registry_row->surface_kind == row.surface_kind,
          EvidenceMessage(row, "registry", "surface kind mismatch"));
  Require(registry_row->family == "ddl_catalog",
          EvidenceMessage(row, "registry", "family mismatch"));
  Require(registry_row->source_status == "native_now",
          EvidenceMessage(row, "registry", "source status mismatch"));
  Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
          EvidenceMessage(row, "registry", "cluster scope mismatch"));
  Require(registry_row->sblr_operation_family == "sblr.catalog.mutation.v3",
          EvidenceMessage(row, "registry", "SBLR operation family mismatch"));
  Require(registry_row->parser_handler_key == "parser.statement_family.ddl_catalog",
          EvidenceMessage(row, "registry", "parser handler key mismatch"));
  Require(registry_row->lowering_handler_key ==
              "lowering.sblr_family.sblr_catalog_mutation_v3",
          EvidenceMessage(row, "registry", "lowering handler key mismatch"));
  Require(registry_row->server_admission_key == "server.admission.sblr_catalog_mutation_v3",
          EvidenceMessage(row, "registry", "server admission key mismatch"));
  Require(registry_row->engine_rule_key == "engine.rule.sblr_catalog_mutation_v3",
          EvidenceMessage(row, "registry", "engine rule key mismatch"));
  Require(registry_row->validation_fixture_id == row.validation_fixture_id,
          EvidenceMessage(row, "registry", "validation fixture id mismatch"));
}

void DumpDiagnostics(const sbsql::MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ":" << diagnostic.message << '\n';
  }
}

sbsql::SessionContext Session() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000000101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000000102";
  session.database_uuid = "019f0000-0000-7000-8000-000000000103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 21;
  session.security_policy_epoch = 22;
  session.descriptor_epoch = 23;
  return session;
}

sbsql::SblrEnvelope Lower(std::string sql,
                          std::vector<std::string> resolved,
                          std::string_view expected_surface_id,
                          std::string_view expected_surface_name) {
  const auto cst = sbsql::BuildCst(sql);
  Require(!cst.messages.has_errors(), "synonym DDL CST failed");
  const auto ast = sbsql::BuildAst(cst);
  if (ast.messages.has_errors()) DumpDiagnostics(ast.messages);
  Require(!ast.messages.has_errors(), "synonym DDL AST failed");
  sbsql::ParserConfig config;
  config.server_endpoint = "sb_server_name_resolver";
  const auto bound = sbsql::BindAst(ast, cst, config, Session(), resolved);
  if (bound.messages.has_errors()) DumpDiagnostics(bound.messages);
  Require(!bound.messages.has_errors(), "synonym DDL bind failed");
  Require(ast.statement_surface_id == expected_surface_id,
          "synonym DDL AST changed canonical statement surface id");
  Require(ast.statement_surface_name == expected_surface_name,
          "synonym DDL AST changed canonical statement surface");
  Require(bound.statement_surface_id == expected_surface_id,
          "synonym DDL bind changed canonical statement surface id");
  Require(bound.statement_surface_name == expected_surface_name,
          "synonym DDL changed canonical statement surface");
  Require(bound.surface_key == expected_surface_id,
          "synonym DDL bind changed canonical surface key");
  auto envelope = sbsql::LowerToSblr(bound, cst, Session());
  Require(envelope.surface_key == expected_surface_id,
          "synonym DDL lowering changed canonical surface key");
  const auto verified = sbsql::VerifySblrEnvelope(envelope);
  if (!verified.admitted) DumpDiagnostics(verified.messages);
  Require(verified.admitted, "synonym DDL SBLR verifier rejected envelope");
  return envelope;
}

void RequireSynonymEnvelope(const sbsql::SblrEnvelope& envelope) {
  Require(envelope.operation_family == "sblr.catalog.mutation.v3",
          "synonym DDL did not remain a catalog mutation");
  Require(Contains(envelope.payload, "\"catalog_envelope_kind\":\"synonym_ddl\""),
          "synonym DDL payload missing synonym catalog envelope kind");
  Require(Contains(envelope.payload, "\"catalog_authority\":\"sys.catalog.synonym\""),
          "synonym DDL payload missing synonym catalog authority");
  Require(Contains(envelope.payload, "\"target_object_kind\":\"synonym\""),
          "synonym DDL payload missing synonym target object kind");
  Require(Contains(envelope.payload, "\"target_uuid_resolution\":\"engine_name_registry_required\""),
          "synonym DDL payload missing engine target UUID resolution intent");
  Require(Contains(envelope.payload, "\"target_kind_resolution\":\"engine_catalog_required\""),
          "synonym DDL payload missing engine target kind resolution intent");
  Require(Contains(envelope.payload, "\"parser_executes_sql\":false"),
          "synonym DDL payload allowed parser SQL execution");
  Require(Contains(envelope.payload, "\"name_text_included\":false"),
          "synonym DDL payload allowed name text authority");
  Require(Contains(envelope.payload, "\"sql_text_included\":false"),
          "synonym DDL payload allowed SQL text authority");
  Require(!Contains(envelope.payload, "orders_syn"),
          "synonym DDL payload embedded human-readable synonym name");
  Require(!Contains(envelope.payload, "orders_table"),
          "synonym DDL payload embedded human-readable target name");
}

}  // namespace

int main() {
  const auto create = Lower("CREATE OR REPLACE PUBLIC SYNONYM orders_syn FOR app.orders_table;",
                            {"019f0000-0000-7000-8000-000000000201"},
                            "SBSQL-A8E627E27375",
                            "create_object");
  Require(create.operation_id == "ddl.create_synonym", "CREATE SYNONYM operation id mismatch");
  Require(Contains(create.payload, "\"catalog_action\":\"create_or_replace_synonym_descriptor\""),
          "CREATE OR REPLACE SYNONYM payload missing catalog action");
  Require(Contains(create.payload, "\"public_synonym\":true"),
          "CREATE PUBLIC SYNONYM payload missing public flag");
  Require(Contains(create.payload, "\"target_name_parts\":2"),
          "CREATE SYNONYM payload missing qualified target shape");
  RequireSynonymEnvelope(create);

  const auto drop = Lower("DROP PUBLIC SYNONYM orders_syn;",
                          {"019f0000-0000-7000-8000-000000000301"},
                          "SBSQL-40CAFAB37942",
                          "drop_object");
  Require(drop.operation_id == "ddl.drop_object", "DROP SYNONYM operation id mismatch");
  Require(Contains(drop.payload, "\"catalog_action\":\"drop_synonym_descriptor\""),
          "DROP SYNONYM payload missing catalog action");
  Require(Contains(drop.payload, "\"target_object_kind\":\"synonym\""),
          "DROP SYNONYM payload did not request synonym target object kind");
  RequireSynonymEnvelope(drop);

  for (const auto& row : kSynonymRows) {
    RequireRegistryEvidence(row);
  }
  Require(drop.surface_key == "SBSQL-40CAFAB37942",
          "DROP SYNONYM grammar proof changed canonical statement route");

  std::cout << "sbsql_synonym_ddl_lowering_conformance=passed\n";
  return EXIT_SUCCESS;
}
