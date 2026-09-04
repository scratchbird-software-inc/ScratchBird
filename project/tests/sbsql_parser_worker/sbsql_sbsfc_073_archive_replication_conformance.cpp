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

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

struct ArchiveRow {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view canonical_operation_id;
  std::string_view canonical_sblr_opcode;
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

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f7300-0000-7000-8000-000000000301";
  session.connection_uuid = "019f7300-0000-7000-8000-000000000302";
  session.database_uuid = "019f7300-0000-7000-8000-000000000303";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 73;
  session.security_policy_epoch = 74;
  session.descriptor_epoch = 75;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_073_archive_replication";
  config.parser_uuid = "019f7300-0000-7000-8000-000000000304";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-073-archive-replication";
  config.build_id = "sbsql-sbsfc-073-archive-replication";
  return config;
}

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound =
      BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

constexpr ArchiveRow kRows[] = {
    {"SBSQL-01F52A6E564D", "backup_stmt", "engine.op.backup_start",
     "SBLR_BACKUP_START", "BACKUP DATABASE TO '/tmp/source.sblbak';"},
    {"SBSQL-9854186AEBB5", "backup_options", "engine.op.backup_start",
     "SBLR_BACKUP_START",
     "BACKUP DATABASE TO '/tmp/source.sblbak' WITH CHECKSUM;"},
    {"SBSQL-57D59EB5A619", "restore_stmt", "engine.op.restore_backup",
     "SBLR_RESTORE_BACKUP", "RESTORE DATABASE FROM '/tmp/source.sblbak';"},
    {"SBSQL-3F340C178247", "restore_options", "engine.op.restore_backup",
     "SBLR_RESTORE_BACKUP",
     "RESTORE DATABASE FROM '/tmp/source.sblbak' WITH VERIFY;"},
    {"SBSQL-A5F3182B0ED9", "archive_stmt", "engine.op.archive_export",
     "SBLR_ARCHIVE_EXPORT", "ARCHIVE DATABASE TO '/tmp/source.sbdelta';"},
    {"SBSQL-A767F5172F1E", "archive_replication_stmt",
     "engine.op.archive_export", "SBLR_ARCHIVE_EXPORT",
     "ARCHIVE DATABASE TO '/tmp/source.sbdelta';"},
    {"SBSQL-F0FB51E3734D", "replication_stmt", "not_admitted",
     "SBLR_DIAGNOSTIC_REFUSAL", "REPLICATE DATABASE TO replica_a;"},
    {"SBSQL-C85DE125F4AF", "changefeed_stmt", "not_admitted",
     "SBLR_DIAGNOSTIC_REFUSAL", "CHANGEFEED DATABASE TO feed_a;"},
    {"SBSQL-A42E75DE4695", "changefeed_options", "not_admitted",
     "SBLR_DIAGNOSTIC_REFUSAL",
     "CHANGEFEED DATABASE TO feed_json WITH FORMAT JSON;"},
};

void RequireRegistryEvidence() {
  for (const auto& row : kRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-073 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-073 generated registry canonical name drifted");
    Require(registry_row->source_status == "native_now",
            "SBSFC-073 generated registry source status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "SBSFC-073 generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family ==
                "sblr.archive_replication.operation.v3",
            "SBSFC-073 generated registry SBLR family drifted");
  }
}

void RequireExactRefusal(const PipelineArtifacts& artifacts,
                         const ArchiveRow& row) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "SBSFC-073 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "SBSFC-073 AST failed");
  Require(artifacts.bound.bound && !artifacts.bound.messages.has_errors(),
          "SBSFC-073 component binding failed");
  Require(!artifacts.verifier.admitted && artifacts.verifier.messages.has_errors(),
          "SBSFC-073 authority-incomplete archive route was admitted");
  Require(artifacts.envelope.operation_family ==
                  "sblr.archive_replication.operation.v3" &&
              artifacts.envelope.sblr_operation_key ==
                  "sblr.archive_replication.operation.v3" &&
              artifacts.envelope.operation_id ==
                  "engine.op.diagnostic_refusal" &&
              artifacts.envelope.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
              artifacts.envelope.engine_api_operation_id == "not_admitted",
          "SBSFC-073 exact refusal tuple mismatch");
  Require(artifacts.envelope.result_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.diagnostic_shape_key ==
                  "diagnostic_vector.v1" &&
              artifacts.envelope.resource_contract_key ==
                  "sbsql.command.no_execution.v1" &&
              artifacts.envelope.trace_key ==
                  "trace.sbsql.archive_replication_exact_refusal",
          "SBSFC-073 exact refusal contract metadata drifted");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.syntax_evidence_only") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_executable_sblr") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_sql_text_execution") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_storage_or_finality"),
          "SBSFC-073 refusal omitted parser non-authority evidence");
  Require(artifacts.envelope.payload.empty() &&
              artifacts.envelope.operands.empty() &&
              artifacts.envelope.resolved_object_uuids.empty() &&
              !artifacts.envelope.parser_executes_sql &&
              !artifacts.envelope.real_file_effects,
          "SBSFC-073 refusal retained executable or parser-owned authority");
  Require(artifacts.envelope.messages.diagnostics.size() == 1,
          "SBSFC-073 did not emit one exact diagnostic");
  const auto& diagnostic = artifacts.envelope.messages.diagnostics.front();
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
              diagnostic.severity == "ERROR" &&
              DiagnosticField(diagnostic, "canonical_operation_id") ==
                  row.canonical_operation_id &&
              DiagnosticField(diagnostic, "canonical_sblr_opcode") ==
                  row.canonical_sblr_opcode &&
              DiagnosticField(diagnostic, "executable_sblr_emitted") ==
                  "false" &&
              Contains(DiagnosticField(diagnostic, "recognized_surface_ids"),
                       row.surface_id),
          "SBSFC-073 exact refusal diagnostic drifted");
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  for (const auto& row : kRows) {
    RequireExactRefusal(RunPipeline(row.sql), row);
  }
  return EXIT_SUCCESS;
}
