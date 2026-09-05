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
#include "database_lifecycle.hpp"
#include "lowering/lowering.hpp"
#include "uuid.hpp"
#include "wire/sbsql_test_wire.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

using namespace scratchbird::parser::sbsql;

namespace {

namespace database = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

bool Require(bool condition, const std::string& message) {
  if (!condition) std::cerr << message << "\n";
  return condition;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasDiagnostic(const MessageVectorSet& messages, std::string_view code) {
  return std::ranges::any_of(messages.diagnostics, [code](const auto& diagnostic) {
    return diagnostic.code == code;
  });
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << "\n";
  }
}

SessionContext Session() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "00000000-0000-7000-8000-000000000007";
  session.connection_uuid = "00000000-0000-7000-8000-000000000107";
  session.database_uuid = "00000000-0000-7000-8000-000000000207";
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

ParserConfig ConfigWithResolver() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "00000000-0000-7000-8000-00000000b007";
  config.bundle_contract_id = "sbp_sbsql@lowering-test";
  config.build_id = "sblr-lowering-test";
  config.server_endpoint = "unix:/tmp/sb_server.sbps.sock";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunParserOnlyPipeline(
    std::string_view sql,
    const std::vector<std::string>& resolved_object_uuids = {}) {
  PipelineArtifacts artifacts;
  const auto session = Session();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ConfigWithResolver(), session,
                            resolved_object_uuids);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

std::filesystem::path MakeFixtureDatabase() {
  static std::atomic<std::uint64_t> identity_time{1788202000000ULL};
  std::string template_path = "/tmp/sbp_sbsql_lowering_verifier.XXXXXX";
  std::vector<char> writable(template_path.begin(), template_path.end());
  writable.push_back('\0');
  char* directory = ::mkdtemp(writable.data());
  if (directory == nullptr) return {};

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database, identity_time.fetch_add(2));
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::filespace, identity_time.fetch_add(2));
  if (!database_uuid.ok() || !filespace_uuid.ok()) {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return {};
  }

  const std::filesystem::path path =
      std::filesystem::path(directory) / "lowering_verifier.sbdb";
  database::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = identity_time.fetch_add(2);
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  const auto created = database::CreateDatabaseFile(create);
  if (!created.ok() ||
      created.create_finality != database::DatabaseCreateFinalityClass::committed) {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return {};
  }
  return path;
}

PipelineResult RunEnginePipelineForConformance(
    SbsqlTestWireSession* session,
    std::string_view sql,
    SbsqlPipelineConformanceSummary* summary) {
  return session->RunPipeline(sql, true, false, 0, false, {}, nullptr, false,
                              nullptr, {}, 0, nullptr, nullptr, summary);
}

bool ValidateAdmittedSelectEnvelope() {
  const auto fixture_database = MakeFixtureDatabase();
  bool ok = true;
  ok &= Require(!fixture_database.empty(),
                "lowering verifier fixture database creation failed");
  if (fixture_database.empty()) return false;

  ParserConfig config;
  config.probe_mode = true;
  config.embedded_engine_direct = true;
  config.allow_uncredentialed_fixture_database = true;
  config.embedded_auth_bypass_sysarch = true;
  config.embedded_database_path = fixture_database.string();
  ParserMetrics metrics;
  SblrTemplateCache cache;
  {
    SbsqlTestWireSession session(config, &metrics, &cache);
    const auto authenticated = session.HandleLine("AUTH");
    ok &= Require(authenticated.text.find("OK AUTHENTICATED") != std::string::npos,
                  "lowering verifier fixture did not authenticate");
    if (authenticated.text.find("OK AUTHENTICATED") == std::string::npos) {
      std::error_code cleanup_error;
      std::filesystem::remove_all(fixture_database.parent_path(), cleanup_error);
      return false;
    }

    SbsqlPipelineConformanceSummary summary;
    const auto result =
        RunEnginePipelineForConformance(&session, "SELECT 1", &summary);
    ok &= Require(result.accepted && !result.messages.has_errors(),
                  "engine-projected SELECT 1 did not execute");
    ok &= Require(summary.captured, "SELECT 1 final parser artifacts were not captured");
    ok &= Require(summary.bound && !summary.bound_has_errors,
                  "SELECT 1 did not bind under engine descriptor authority");
    ok &= Require(summary.payload_nonempty && !result.sblr_payload.empty(),
                  "SELECT 1 produced empty SBLR payload");
    ok &= Require(summary.verifier_admitted,
                  "SELECT 1 SBLR envelope was not verifier-admitted");
    ok &= Require(!summary.verifier_has_errors,
                  "SELECT 1 verifier emitted diagnostics");
    ok &= Require(summary.envelope_version == 3, "SBLR envelope version mismatch");
    ok &= Require(summary.operation_family == summary.bound_operation_family,
                "operation family not preserved");
    ok &= Require(summary.sblr_operation_key == summary.bound_sblr_operation_key,
                "SBLR operation key not preserved");
    ok &= Require(summary.surface_key == summary.bound_surface_key,
                "surface key not preserved");
    ok &= Require(summary.operation_family == "sblr.query.relational.v3" &&
                      summary.operation_id == "query.execute" &&
                      summary.sblr_operation_key == "sblr.query.relational.v3" &&
                      summary.sblr_opcode == "SBLR_QUERY_EXECUTE",
                  "canonical query operation tuple mismatch");
    ok &= Require(result.server_operation_id == "query.execute",
                  "engine execution did not retain query.execute");
    ok &= Require(summary.command_family == "query", "command family mismatch");
    ok &= Require(summary.result_shape_key == "query_execute_result",
                "result shape mismatch");
    ok &= Require(summary.diagnostic_shape_key == "diagnostic_vector",
                  "diagnostic shape mismatch");
    ok &= Require(summary.resource_contract_key == "resource.contract.query_read",
                "resource contract mismatch");
    ok &= Require(summary.catalog_epoch != 0 &&
                      summary.catalog_epoch == session.session().catalog_epoch,
                  "catalog epoch did not come from the live engine context");
    ok &= Require(summary.security_policy_epoch != 0 &&
                      summary.security_policy_epoch ==
                          session.session().security_policy_epoch,
                  "security policy epoch did not come from the live engine context");
    ok &= Require(summary.descriptor_epoch != 0 &&
                      summary.descriptor_epoch == session.session().descriptor_epoch,
                  "descriptor epoch did not come from the live engine context");
    ok &= Require(summary.has_read_right, "required right missing");
    ok &= Require(summary.has_syntax_authority, "syntax authority step missing");
    ok &= Require(result.sblr_payload.find("SELECT 1") == std::string::npos,
                  "SBLR payload embedded SQL text");
    ok &= Require(result.sblr_payload.find("\"source_text\"") == std::string::npos,
                  "SBLR payload embedded source_text field");

    const auto created = session.RunPipeline(
        "CREATE TABLE customer (id INT)", true);
    if (!created.accepted) PrintMessages(created.messages);
    ok &= Require(created.accepted && !created.messages.has_errors(),
                  "engine-resolved fixture relation creation failed");

    SbsqlPipelineConformanceSummary resolved_summary;
    const auto resolved = RunEnginePipelineForConformance(
        &session, "SELECT * FROM customer", &resolved_summary);
    if (!resolved_summary.captured || !resolved_summary.verifier_admitted) {
      PrintMessages(resolved.messages);
    }
    ok &= Require(resolved_summary.captured && resolved_summary.bound &&
                      resolved_summary.verifier_admitted &&
                      !resolved_summary.verifier_has_errors,
                  "engine-resolved SELECT FROM was not bound and verified");
    ok &= Require(resolved_summary.payload_nonempty &&
                      !resolved.sblr_payload.empty(),
                  "engine-resolved SELECT FROM produced no canonical SBLR");
    ok &= Require(resolved_summary.resolved_object_uuids.size() == 1,
                  "engine-resolved UUID count mismatch");
    ok &= Require(resolved_summary.has_resolver_authority,
                  "resolver authority step missing");
    ok &= Require(resolved_summary.has_descriptor_refs,
                  "engine descriptor refs missing");
  }

  std::error_code cleanup_error;
  std::filesystem::remove_all(fixture_database.parent_path(), cleanup_error);
  return ok;
}

bool ValidateResolvedNameEnvelope() {
  const auto artifacts = RunParserOnlyPipeline(
      "SELECT * FROM customer", {"00000000-0000-7000-8000-00000000c007"});
  bool ok = true;
  ok &= Require(!artifacts.bound.bound,
                "parser-owned resolved UUID bypassed native binding authority");
  ok &= Require(!artifacts.verifier.admitted,
                "parser-owned resolved UUID was verifier-admitted");
  ok &= Require(artifacts.envelope.resolved_object_uuids.empty(),
                "parser-owned resolved UUID was promoted into the envelope");
  ok &= Require(HasDiagnostic(artifacts.bound.messages,
                              "QOW-DIAG-BOUNDAST-SCOPE"),
                "parser-owned UUID did not fail at the binding-context boundary");
  ok &= Require(HasValue(artifacts.bound.required_authority_steps,
                         "authority.server.resolve_name_registry_public"),
                "resolver authority step missing");
  ok &= Require(artifacts.envelope.descriptor_refs.size() == 1 &&
                    artifacts.envelope.descriptor_refs[0] ==
                        "descriptor.pending_server_or_engine_authority",
                "native refusal lost its non-authoritative descriptor marker");
  return ok;
}

bool ValidateMissingNativeContextRefusal() {
  const auto artifacts = RunParserOnlyPipeline("SELECT 1");
  bool ok = true;
  ok &= Require(!artifacts.bound.bound,
                "SELECT 1 unexpectedly bound without engine descriptor authority");
  ok &= Require(HasDiagnostic(artifacts.bound.messages,
                              "QOW-DIAG-BOUNDAST-SCOPE"),
                "SELECT 1 lacked the binding-context refusal");
  ok &= Require(artifacts.envelope.payload.empty(),
                "context-free SELECT 1 produced SBLR payload");
  ok &= Require(!artifacts.verifier.admitted,
                "context-free SELECT 1 was verifier-admitted");
  ok &= Require(artifacts.verifier.messages.has_errors(),
                "context-free SELECT 1 lacked verifier diagnostics");
  return ok;
}

bool ValidateSecurityEnvelope() {
  const auto artifacts = RunParserOnlyPipeline(
      "GRANT SELECT ON customer TO app_role",
      {"00000000-0000-7000-8000-00000000c107",
       "00000000-0000-7000-8000-00000000c207"});
  bool ok = true;
  ok &= Require(artifacts.bound.bound, "GRANT did not bind");
  ok &= Require(artifacts.verifier.admitted, "GRANT envelope not verifier-admitted");
  ok &= Require(artifacts.envelope.command_family == "security", "GRANT command family mismatch");
  ok &= Require(HasValue(artifacts.envelope.required_rights, "right.security_admin"),
                "GRANT required right missing");
  ok &= Require(HasValue(artifacts.envelope.required_authority_steps,
                         "authority.server.security_policy_context_required"),
                "GRANT security authority step missing");
  ok &= Require(!artifacts.envelope.policy_refs.empty(), "GRANT policy refs missing");
  return ok;
}

bool ValidateUnboundRefusal() {
  ParserConfig config = ConfigWithResolver();
  config.server_endpoint.clear();
  const auto session = Session();
  const auto cst = BuildCst("SELECT * FROM customer");
  const auto ast = BuildAst(cst);
  const auto bound = BindAst(ast, cst, config, session);
  const auto envelope = LowerToSblr(bound, cst, session);
  const auto verifier = VerifySblrEnvelope(envelope);
  bool ok = true;
  ok &= Require(!bound.bound, "unresolved SELECT FROM unexpectedly bound");
  ok &= Require(envelope.payload.empty(), "unbound statement produced SBLR payload");
  ok &= Require(!verifier.admitted, "unbound statement was verifier-admitted");
  ok &= Require(verifier.messages.has_errors(), "unbound statement lacked verifier diagnostics");
  return ok;
}

bool ValidateMalformedEnvelopeRejected() {
  SblrEnvelope envelope;
  envelope.envelope_version = 3;
  envelope.operation_family = "sblr.query.relational.v3";
  envelope.sblr_operation_key = "sblr.query.relational.v3";
  envelope.statement_hash = 1;
  envelope.surface_key = "SBSQL-INVALID";
  envelope.command_family = "query";
  envelope.operation_id = "query.execute";
  envelope.sblr_operation_key = "sblr.query.relational.v3";
  envelope.sblr_opcode = "SBLR_QUERY_EXECUTE";
  envelope.engine_api_operation_id = "query.execute";
  envelope.result_shape_key = "query_execute_result";
  envelope.diagnostic_shape_key = "diagnostic_vector";
  envelope.resource_contract_key = "resource.contract.query_read";
  envelope.required_authority_steps.push_back("authority.parser.syntax_evidence_only");
  envelope.payload = "{\"sql\":\"SELECT 1\"}";
  const auto verifier = VerifySblrEnvelope(envelope);
  bool ok = true;
  ok &= Require(!verifier.admitted, "malformed envelope was verifier-admitted");
  ok &= Require(verifier.messages.has_errors(), "malformed envelope lacked diagnostics");
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= ValidateAdmittedSelectEnvelope();
  ok &= ValidateResolvedNameEnvelope();
  ok &= ValidateMissingNativeContextRefusal();
  ok &= ValidateSecurityEnvelope();
  ok &= ValidateUnboundRefusal();
  ok &= ValidateMalformedEnvelopeRejected();
  return ok ? 0 : 1;
}
