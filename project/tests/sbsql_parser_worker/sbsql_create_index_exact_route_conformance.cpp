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

constexpr std::string_view kSql =
    "CREATE INDEX replay_target_id_idx ON replay_target (id);";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kSurfaceId = "SBSQL-D09825658F68";
constexpr std::string_view kSurfaceName = "create_index_stmt";
constexpr std::string_view kFixtureId = "SBSQL-SURFACE-F247E52B8B35";
constexpr std::string_view kResolvedTableUuid =
    "019f0000-0000-7000-8000-000000d09801";

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
  session.session_uuid = "019f0000-0000-7000-8000-000000d09901";
  session.connection_uuid = "019f0000-0000-7000-8000-000000d09902";
  session.database_uuid = "019f0000-0000-7000-8000-000000d09903";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 57;
  session.security_policy_epoch = 58;
  session.descriptor_epoch = 59;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000d09904";
  config.bundle_contract_id = "sbp_sbsql@create-index-refusal-test";
  config.build_id = "sbsql-create-index-refusal-test";
  return config;
}

PipelineArtifacts RunPipeline() {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(kSql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst,
                            ParserConfigForTest(), session,
                            {std::string(kResolvedTableUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  const auto* row = FindGeneratedSurfaceRegistryRowById(kSurfaceId);
  Require(row != nullptr, "CREATE INDEX generated registry row missing");
  Require(row->canonical_name == kSurfaceName,
          "CREATE INDEX generated registry canonical name drifted");
  Require(row->surface_kind == "grammar_production",
          "CREATE INDEX generated registry kind drifted");
  Require(row->family == "ddl_catalog",
          "CREATE INDEX generated registry family drifted");
  Require(row->source_status == "native_now",
          "CREATE INDEX generated registry status drifted");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "CREATE INDEX generated registry cluster scope drifted");
  Require(row->sblr_operation_family == kFamily,
          "CREATE INDEX generated registry SBLR family drifted");
  Require(row->validation_fixture_id == kFixtureId,
          "CREATE INDEX generated registry fixture id drifted");
}

void RequireExactRefusal(const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "CREATE INDEX CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CREATE INDEX AST failed");
  Require(artifacts.bound.bound && !artifacts.bound.messages.has_errors(),
          "CREATE INDEX component binding failed");
  Require(!artifacts.verifier.admitted && artifacts.verifier.messages.has_errors(),
          "CREATE INDEX unavailable route was admitted");
  Require(artifacts.envelope.operation_family == kFamily &&
              artifacts.envelope.sblr_operation_key == kFamily,
          "CREATE INDEX refusal family drifted");
  Require(artifacts.envelope.operation_id == "engine.op.diagnostic_refusal" &&
              artifacts.envelope.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
              artifacts.envelope.engine_api_operation_id == "not_admitted",
          "CREATE INDEX exact refusal tuple drifted");
  Require(artifacts.envelope.result_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.diagnostic_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.resource_contract_key ==
                  "sbsql.command.no_execution.v1" &&
              artifacts.envelope.trace_key ==
                  "trace.sbsql.create_index_exact_refusal",
          "CREATE INDEX refusal contract metadata drifted");
  Require(!artifacts.envelope.parser_executes_sql &&
              !artifacts.envelope.real_file_effects &&
              artifacts.envelope.payload.empty() &&
              artifacts.envelope.operands.empty() &&
              artifacts.envelope.resolved_object_uuids.empty(),
          "CREATE INDEX refusal retained executable authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.syntax_evidence_only") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_executable_sblr") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_sql_text_execution") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_storage_or_finality"),
          "CREATE INDEX refusal authority steps drifted");
  Require(artifacts.envelope.messages.diagnostics.size() == 1,
          "CREATE INDEX did not emit one exact diagnostic");
  const auto& diagnostic = artifacts.envelope.messages.diagnostics.front();
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
              diagnostic.severity == "ERROR" &&
              DiagnosticField(diagnostic, "canonical_parent_operation_id") ==
                  "engine.op.ddl_create_index" &&
              DiagnosticField(diagnostic, "canonical_parent_sblr_opcode") ==
                  "SBLR_DDL_CREATE_INDEX" &&
              DiagnosticField(diagnostic, "executable_sblr_emitted") == "false" &&
              Contains(DiagnosticField(diagnostic, "recognized_surface_ids"),
                       kSurfaceId),
          "CREATE INDEX exact refusal diagnostic drifted");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  RequireExactRefusal(RunPipeline());
  std::cout << "sbsql_create_index_exact_refusal_conformance=passed\n";
  return EXIT_SUCCESS;
}
