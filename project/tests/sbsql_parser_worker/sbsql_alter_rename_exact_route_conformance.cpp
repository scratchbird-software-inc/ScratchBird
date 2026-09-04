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

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;

constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kTargetUuid =
    "019f0000-0000-7000-8000-000000a17e01";
constexpr std::string_view kSchemaUuid =
    "019f0000-0000-7000-8000-000000a17e02";

struct RouteCase {
  std::string_view sql;
  std::string_view canonical_parent_operation_id;
  std::string_view canonical_parent_opcode;
  std::vector<std::string_view> surface_ids;
};

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values,
              std::string_view expected) {
  for (const auto& value : values) {
    if (value == expected) return true;
  }
  return false;
}

std::string DiagnosticField(const Diagnostic& diagnostic,
                            std::string_view expected_name) {
  for (const auto& field : diagnostic.fields) {
    if (field.name == expected_name) return field.value;
  }
  return {};
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000a17e11";
  session.connection_uuid = "019f0000-0000-7000-8000-000000a17e12";
  session.database_uuid = "019f0000-0000-7000-8000-000000a17e13";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 91;
  session.security_policy_epoch = 92;
  session.descriptor_epoch = 93;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000a17e14";
  config.bundle_contract_id = "sbp_sbsql@alter-rename-refusal-test";
  config.build_id = "sbsql-alter-rename-refusal-test";
  return config;
}

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst,
                            ParserConfigForTest(), session,
                            {std::string(kTargetUuid),
                             std::string(kSchemaUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryRow(std::string_view surface_id,
                        std::string_view canonical_name,
                        std::string_view surface_kind,
                        std::string_view family,
                        std::string_view sblr_family) {
  const auto* row = FindGeneratedSurfaceRegistryRowById(surface_id);
  Require(row != nullptr, "ALTER/RENAME generated registry row missing");
  Require(row->canonical_name == canonical_name &&
              row->surface_kind == surface_kind &&
              row->family == family && row->source_status == "native_now" &&
              row->cluster_scope == "noncluster_or_profile_scoped" &&
              row->sblr_operation_family == sblr_family,
          "ALTER/RENAME generated registry row drifted");
}

void RequireRegistryEvidence() {
  RequireRegistryRow("SBSQL-472ECFA63673", "alter_object",
                     "canonical_surface", "ddl_catalog", kFamily);
  RequireRegistryRow("SBSQL-6824451E6988", "alter_object_stmt",
                     "grammar_production", "ddl_catalog", kFamily);
  RequireRegistryRow("SBSQL-CFDD65DE9EA6", "alter_action",
                     "grammar_production", "ddl_catalog", kFamily);
  RequireRegistryRow("SBSQL-58224DEE5BCA", "rename_object_stmt",
                     "grammar_production", "ddl_catalog", kFamily);
  RequireRegistryRow("SBSQL-5CCF87EB0C5C", "object_class",
                     "grammar_production", "general",
                     "sblr.general.operation.v3");
  RequireRegistryRow("SBSQL-ADEF20254494", "qualified_name",
                     "grammar_production", "general",
                     "sblr.general.operation.v3");
}

void RequireExactRefusal(const RouteCase& route) {
  const auto artifacts = RunPipeline(route.sql);
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "ALTER/RENAME CST failed");
  Require(!artifacts.ast.messages.has_errors(), "ALTER/RENAME AST failed");
  Require(artifacts.bound.bound && !artifacts.bound.messages.has_errors(),
          "ALTER/RENAME component binding failed");
  Require(!artifacts.verifier.admitted && artifacts.verifier.messages.has_errors(),
          "ALTER/RENAME unavailable route was admitted");
  Require(artifacts.envelope.operation_family == kFamily &&
              artifacts.envelope.sblr_operation_key == kFamily,
          "ALTER/RENAME refusal family drifted");
  Require(artifacts.envelope.operation_id == "engine.op.diagnostic_refusal" &&
              artifacts.envelope.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
              artifacts.envelope.engine_api_operation_id == "not_admitted",
          "ALTER/RENAME exact refusal tuple drifted");
  Require(artifacts.envelope.result_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.diagnostic_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.resource_contract_key ==
                  "sbsql.command.no_execution.v1" &&
              artifacts.envelope.trace_key ==
                  "trace.sbsql.alter_rename_exact_refusal",
          "ALTER/RENAME refusal metadata drifted");
  Require(!artifacts.envelope.parser_executes_sql &&
              !artifacts.envelope.real_file_effects &&
              artifacts.envelope.payload.empty() &&
              artifacts.envelope.operands.empty() &&
              artifacts.envelope.resolved_object_uuids.empty(),
          "ALTER/RENAME refusal retained executable authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.syntax_evidence_only") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_executable_sblr") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_sql_text_execution") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_storage_or_finality"),
          "ALTER/RENAME refusal authority steps drifted");
  Require(artifacts.envelope.messages.diagnostics.size() == 1,
          "ALTER/RENAME did not emit one exact diagnostic");
  const auto& diagnostic = artifacts.envelope.messages.diagnostics.front();
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
              diagnostic.severity == "ERROR" &&
              DiagnosticField(diagnostic, "canonical_parent_operation_id") ==
                  route.canonical_parent_operation_id &&
              DiagnosticField(diagnostic, "canonical_parent_sblr_opcode") ==
                  route.canonical_parent_opcode &&
              DiagnosticField(diagnostic, "executable_sblr_emitted") == "false",
          "ALTER/RENAME exact refusal diagnostic drifted");
  const auto recognized =
      DiagnosticField(diagnostic, "recognized_surface_ids");
  for (const auto surface_id : route.surface_ids) {
    Require(Contains(recognized, surface_id),
            "ALTER/RENAME diagnostic omitted a surface identity");
  }
}

const RouteCase kRoutes[] = {
    {"RENAME TABLE replay_target TO renamed_target;",
     "engine.op.ddl_rename_object", "SBLR_DDL_RENAME_OBJECT",
     {"SBSQL-58224DEE5BCA", "SBSQL-5CCF87EB0C5C",
      "SBSQL-ADEF20254494"}},
    {"ALTER TABLE replay_target RENAME TO renamed_target;", "not_admitted",
     "SBLR_DIAGNOSTIC_REFUSAL",
     {"SBSQL-472ECFA63673", "SBSQL-6824451E6988",
      "SBSQL-CFDD65DE9EA6", "SBSQL-5CCF87EB0C5C",
      "SBSQL-ADEF20254494"}},
};

}  // namespace

int main() {
  RequireRegistryEvidence();
  for (const auto& route : kRoutes) RequireExactRefusal(route);
  std::cout << "sbsql_alter_rename_exact_refusal_conformance=passed\n";
  return EXIT_SUCCESS;
}
