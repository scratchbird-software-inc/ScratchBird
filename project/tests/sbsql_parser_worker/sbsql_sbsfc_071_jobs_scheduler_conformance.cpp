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
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

struct JobRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view expected_family;
  std::string_view sql;
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

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  for (const auto& value : values) {
    if (value == expected) return true;
  }
  return false;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f7100-0000-7000-8000-000000000301";
  session.connection_uuid = "019f7100-0000-7000-8000-000000000302";
  session.database_uuid = "019f7100-0000-7000-8000-000000000303";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 71;
  session.security_policy_epoch = 72;
  session.descriptor_epoch = 73;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_071_jobs_scheduler";
  config.parser_uuid = "019f7100-0000-7000-8000-000000000304";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-071-jobs-scheduler";
  config.build_id = "sbsql-sbsfc-071-jobs-scheduler";
  return config;
}

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

std::string DiagnosticField(const Diagnostic& diagnostic,
                            std::string_view expected_name) {
  for (const auto& field : diagnostic.fields) {
    if (field.name == expected_name) return field.value;
  }
  return {};
}

void RequireExactRefusal(const PipelineArtifacts& artifacts,
                         const JobRow& row) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-071 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-071 AST failed");
  Require(artifacts.bound.bound && !artifacts.bound.messages.has_errors(),
          "SBSFC-071 component binding failed");
  Require(!artifacts.verifier.admitted && artifacts.verifier.messages.has_errors(),
          "SBSFC-071 unavailable scheduler command was admitted");
  Require(artifacts.envelope.operation_family == row.expected_family,
          "SBSFC-071 operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == row.expected_family,
          "SBSFC-071 operation key mismatch");
  Require(artifacts.envelope.operation_id == "engine.op.diagnostic_refusal" &&
              artifacts.envelope.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
              artifacts.envelope.engine_api_operation_id == "not_admitted",
          "SBSFC-071 exact refusal tuple mismatch");
  Require(artifacts.envelope.result_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.diagnostic_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.resource_contract_key ==
                  "sbsql.command.no_execution.v1" &&
              artifacts.envelope.trace_key ==
                  "trace.sbsql.jobs_scheduler_exact_refusal",
          "SBSFC-071 exact refusal contract metadata drifted");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-071 allowed parser SQL execution");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.syntax_evidence_only"),
          "SBSFC-071 missing syntax-only authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_executable_sblr"),
          "SBSFC-071 missing no-SBLR authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-071 missing no SQL execution authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-071 missing no storage/finality authority");
  Require(artifacts.envelope.payload.empty() &&
              artifacts.envelope.operands.empty() &&
              artifacts.envelope.resolved_object_uuids.empty() &&
              !artifacts.envelope.real_file_effects,
          "SBSFC-071 refusal retained executable or parser-owned authority");
  Require(artifacts.envelope.messages.diagnostics.size() == 1,
          "SBSFC-071 did not emit one exact diagnostic");
  const auto& diagnostic = artifacts.envelope.messages.diagnostics.front();
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
              diagnostic.severity == "ERROR" &&
              DiagnosticField(diagnostic, "executor_operation_id") ==
                  "not_admitted" &&
              DiagnosticField(diagnostic, "executable_sblr_emitted") ==
                  "false" &&
              Contains(DiagnosticField(diagnostic, "recognized_surface_ids"),
                       row.surface_id),
          "SBSFC-071 exact refusal diagnostic drifted");
}

constexpr JobRow kRows[] = {
    {"SBSQL-D0D4CF68E123", "create_job_stmt", "sblr.catalog.mutation.v3",
     "CREATE JOB route_job;"},
    {"SBSQL-A9EF7570082E", "run_job_stmt", "sblr.jobs.operation.v3",
     "RUN JOB route_job;"},
    {"SBSQL-425396530806", "pause_job_stmt", "sblr.jobs.operation.v3",
     "PAUSE JOB route_job;"},
    {"SBSQL-43B6C7986FE3", "resume_job_stmt", "sblr.jobs.operation.v3",
     "RESUME JOB route_job;"},
    {"SBSQL-16CF52731255", "cancel_job_stmt", "sblr.jobs.operation.v3",
     "CANCEL JOB route_job;"},
};

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-071 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-071 generated registry canonical name drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-071 generated registry source status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-071 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.expected_family,
            "SBSFC-071 generated registry SBLR family drifted");
  }
  const auto* root = FindGeneratedSurfaceRegistryRowById("SBSQL-A39DC4358469");
  Require(root != nullptr, "SBSFC-071 jobs scheduler root row missing");
  Require(root->canonical_name == "jobs_scheduler_stmt",
          "SBSFC-071 jobs scheduler root canonical name drifted");
  Require(root->sblr_operation_family == "sblr.jobs.operation.v3",
          "SBSFC-071 jobs scheduler root family drifted");
}

}  // namespace

int main() {
  RequireRegistryEvidence();

  for (const auto& row : kRows) {
    const auto artifacts = RunPipeline(row.sql);
    RequireExactRefusal(artifacts, row);
  }
  return EXIT_SUCCESS;
}
