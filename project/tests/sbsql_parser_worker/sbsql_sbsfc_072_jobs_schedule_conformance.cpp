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

struct ScheduleRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view canonical_family;
  std::string_view route_family;
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
  session.session_uuid = "019f7200-0000-7000-8000-000000000301";
  session.connection_uuid = "019f7200-0000-7000-8000-000000000302";
  session.database_uuid = "019f7200-0000-7000-8000-000000000303";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 72;
  session.security_policy_epoch = 73;
  session.descriptor_epoch = 74;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_072_jobs_schedule";
  config.parser_uuid = "019f7200-0000-7000-8000-000000000304";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-072-jobs-schedule";
  config.build_id = "sbsql-sbsfc-072-jobs-schedule";
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
                         const ScheduleRow& row) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-072 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-072 AST failed");
  Require(artifacts.bound.bound && !artifacts.bound.messages.has_errors(),
          "SBSFC-072 component binding failed");
  Require(!artifacts.verifier.admitted && artifacts.verifier.messages.has_errors(),
          "SBSFC-072 unavailable scheduler command was admitted");
  Require(artifacts.envelope.operation_family == row.route_family,
          "SBSFC-072 operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == row.route_family,
          "SBSFC-072 operation key mismatch");
  Require(artifacts.envelope.operation_id == "engine.op.diagnostic_refusal" &&
              artifacts.envelope.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
              artifacts.envelope.engine_api_operation_id == "not_admitted",
          "SBSFC-072 exact refusal tuple mismatch");
  Require(artifacts.envelope.result_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.diagnostic_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.resource_contract_key ==
                  "sbsql.command.no_execution.v1" &&
              artifacts.envelope.trace_key ==
                  "trace.sbsql.jobs_scheduler_exact_refusal",
          "SBSFC-072 exact refusal contract metadata drifted");
  Require(!artifacts.envelope.parser_executes_sql,
          "SBSFC-072 allowed parser SQL execution");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.syntax_evidence_only"),
          "SBSFC-072 missing syntax-only authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_executable_sblr"),
          "SBSFC-072 missing no-SBLR authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "SBSFC-072 missing no SQL execution authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "SBSFC-072 missing no storage/finality authority");
  Require(artifacts.envelope.payload.empty() &&
              artifacts.envelope.operands.empty() &&
              artifacts.envelope.resolved_object_uuids.empty() &&
              !artifacts.envelope.real_file_effects,
          "SBSFC-072 refusal retained executable or parser-owned authority");
  Require(artifacts.envelope.messages.diagnostics.size() == 1,
          "SBSFC-072 did not emit one exact diagnostic");
  const auto& diagnostic = artifacts.envelope.messages.diagnostics.front();
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
              diagnostic.severity == "ERROR" &&
              DiagnosticField(diagnostic, "executor_operation_id") ==
                  "not_admitted" &&
              DiagnosticField(diagnostic, "executable_sblr_emitted") ==
                  "false" &&
              Contains(DiagnosticField(diagnostic, "recognized_surface_ids"),
                       row.surface_id),
          "SBSFC-072 exact refusal diagnostic drifted");
}

constexpr ScheduleRow kRows[] = {
    {"SBSQL-B6172AC78E5B", "alter_job_stmt", "sblr.catalog.mutation.v3",
     "sblr.catalog.mutation.v3", "ALTER JOB route_job SCHEDULE EVERY 60;"},
    {"SBSQL-F0AB7D8C3DF6", "create_schedule_stmt", "sblr.catalog.mutation.v3",
     "sblr.catalog.mutation.v3",
     "CREATE SCHEDULE route_sched FOR JOB scheduled_job EVERY 60 STARTS 120 ENDS 180;"},
    {"SBSQL-BE0E4DA525D0", "alter_schedule_stmt", "sblr.catalog.mutation.v3",
     "sblr.catalog.mutation.v3", "ALTER SCHEDULE route_sched EVERY 120;"},
    {"SBSQL-0779CE111C6C", "schedule_spec", "sblr.jobs.operation.v3",
     "sblr.catalog.mutation.v3",
     "CREATE SCHEDULE route_sched FOR JOB scheduled_job EVERY 60 STARTS 120 ENDS 180;"},
    {"SBSQL-01E1D3EF37D8", "event_schedule", "sblr.jobs.operation.v3",
     "sblr.catalog.mutation.v3",
     "CREATE SCHEDULE route_sched FOR JOB scheduled_job EVERY 60 STARTS 120 ENDS 180;"},
    {"SBSQL-0725D825F5F3", "event_schedule_offset_list", "sblr.jobs.operation.v3",
     "sblr.catalog.mutation.v3",
     "CREATE SCHEDULE route_sched FOR JOB scheduled_job EVERY 60 STARTS 120 ENDS 180;"},
};

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-072 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-072 generated registry canonical name drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-072 generated registry source status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-072 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.canonical_family,
            "SBSFC-072 generated registry SBLR family drifted");
  }
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
