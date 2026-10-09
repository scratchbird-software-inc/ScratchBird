// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../../../support/binary_uuid_fixture.hpp"
#include "../../../support/owned_temp_directory.hpp"
#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "core/memory/memory.hpp"
#include "lowering/lowering.hpp"
#include "sbps.hpp"
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

constexpr std::string_view kPrincipal = "qow_packet7_user";
constexpr std::string_view kPassword = "QOW-Packet7-live-route-password";
constexpr std::string_view kCredential =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=7b622f17d15a5e6f2d5122c606937dfc"
    "76f5982782adb52b03f7b1ca024f72c9";

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
    for (const auto& field : diagnostic.fields)
      std::cerr << "  " << field.name << '=' << field.value << '\n';
  }
}

SessionContext Session() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = scratchbird::tests::FixtureUuidLiteral("00000000-0000-7000-8000-000000000007");
  session.connection_uuid = scratchbird::tests::FixtureUuidLiteral("00000000-0000-7000-8000-000000000107");
  session.database_uuid = scratchbird::tests::FixtureUuidLiteral("00000000-0000-7000-8000-000000000207");
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

ParserConfig ConfigWithResolver() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = scratchbird::tests::FixtureUuidLiteral("00000000-0000-7000-8000-00000000b007");
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
    const std::vector<scratchbird::core::platform::Uuid>& resolved_object_uuids = {}) {
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

std::filesystem::path MakeFixtureDatabase(const std::filesystem::path& directory,
                                         bool credentialed = true) {
  static std::atomic<std::uint64_t> identity_time{1788202000000ULL};

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database, identity_time.fetch_add(2));
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::filespace, identity_time.fetch_add(2));
  if (!database_uuid.ok() || !filespace_uuid.ok()) {
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
  create.allow_minimal_resource_bootstrap = !credentialed;
  create.require_resource_seed_pack = credentialed;
  if (credentialed) {
    create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
    create.bootstrap_principal_name = kPrincipal;
    create.bootstrap_credential_fingerprint = kCredential;
    create.require_bootstrap_principal = true;
    create.allow_uncredentialed_bootstrap = false;
  }
  const auto created = database::CreateDatabaseFile(create);
  if (!created.ok() ||
      created.create_finality != database::DatabaseCreateFinalityClass::committed) {
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
  scratchbird::tests::OwnedTempDirectory directory;
  const auto fixture_database = MakeFixtureDatabase(directory.path());
  bool ok = true;
  ok &= Require(!fixture_database.empty(),
                "lowering verifier fixture database creation failed");
  if (fixture_database.empty()) return false;

  ParserConfig config;
  config.probe_mode = true;
  config.embedded_engine_direct = true;
  config.embedded_database_path = fixture_database.string();
  ParserMetrics metrics;
  SblrTemplateCache cache;
  {
    SbsqlTestWireSession session(config, &metrics, &cache);
    AuthCredentialEnvelope credentials;
    credentials.provider_family = "local_password";
    credentials.principal = kPrincipal;
    credentials.requested_database = fixture_database.string();
    credentials.application_name = "lowering-verifier";
    credentials.credential_evidence = kPassword;
    credentials.credential_evidence_present = true;
    MessageVectorSet authentication_messages;
    const bool authenticated = session.AuthenticateCredentials(credentials, &authentication_messages);
    if (!authenticated) PrintMessages(authentication_messages);
    ok &= Require(authenticated && session.session().authenticated,
                  "lowering verifier fixture did not authenticate");
    if (!authenticated) {
      return false;
    }
    ok &= Require(uuid::IsEngineIdentityUuid(session.session().dialect_profile_uuid) &&
                      session.session().dialect_profile_uuid ==
                          session.session().admitted_dialect_profile_uuid,
                  "embedded attach did not publish its actual admitted dialect identity");

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
    if (!resolved.accepted || !resolved_summary.captured || !resolved_summary.verifier_admitted) {
      PrintMessages(resolved.messages);
    }
    ok &= Require(resolved.accepted && !resolved.messages.has_errors() &&
                      resolved.server_operation_id == "query.execute" &&
                      resolved.server_row_count == 0 &&
                      !resolved.server_result_payload.empty(),
                  "engine-resolved empty relation did not execute its real rowset");
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

  directory.Cleanup();
  return ok;
}

bool ValidateFixtureAttachDialectIdentity() {
  scratchbird::tests::OwnedTempDirectory directory;
  const auto database = MakeFixtureDatabase(directory.path(), false);
  if (!Require(!database.empty(), "fixture attach database creation failed")) return false;
  ParserConfig config;
  config.probe_mode = true;
  config.embedded_engine_direct = true;
  config.allow_uncredentialed_fixture_database = true;
  config.embedded_auth_bypass_sysarch = true;
  config.embedded_database_path = database.string();
  ParserMetrics metrics;
  SblrTemplateCache cache;
  bool ok = false;
  {
    SbsqlTestWireSession session(config, &metrics, &cache);
    const auto attached = session.HandleLine("AUTH");
    ok = Require(attached.text.find("OK AUTHENTICATED") != std::string::npos &&
                     uuid::IsEngineIdentityUuid(session.session().dialect_profile_uuid) &&
                     session.session().dialect_profile_uuid == session.session().admitted_dialect_profile_uuid,
                 "fixture attach published a nil or unadmitted dialect identity");
  }
  directory.Cleanup();
  return ok;
}

bool ValidateDiagnosticFrameDecoding() {
  namespace sbps = scratchbird::server::sbps;
  namespace ipc = scratchbird::parser::ipc;
  auto diagnostic = sbps::IpcDiagnostic(
      "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUEST_INVALID",
      "parser_server_ipc.relation_descriptor_request_invalid", "invalid relation request");
  diagnostic.fields.push_back({"detail", "exact_request_binding_required"});
  sbps::FrameHeader header;
  header.request_uuid = sbps::MakeUuidV7Bytes();
  header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kResolveNameResult);
  header.payload_schema_id = sbps::kSchemaMessageVectorSetV1;
  header.flags = sbps::kFlagResponse | sbps::kFlagError | sbps::kFlagFinal;
  const auto payload = sbps::EncodeMessageVectorSet({diagnostic}, header.request_uuid);
  const auto frame = sbps::EncodeFrame(header, payload);
  MessageVectorSet decoded;
  bool ok = Require(ipc::DecodeDiagnosticFrame(frame, &decoded) &&
                        decoded.diagnostics.size() == 1 &&
                        decoded.diagnostics.front().code == diagnostic.code &&
                        std::ranges::any_of(decoded.diagnostics.front().fields, [](const auto& field) {
                          return field.name == "detail" && field.value == "exact_request_binding_required";
                        }),
                    "name response lost its owning diagnostic or typed fields");
  auto corrupted = frame;
  corrupted.back() ^= 1;
  MessageVectorSet invalid;
  ok &= Require(!ipc::DecodeDiagnosticFrame(corrupted, &invalid) && invalid.has_errors(),
                "corrupt diagnostic frame was accepted");
  header.flags &= ~sbps::kFlagError;
  MessageVectorSet success;
  ok &= Require(!ipc::DecodeDiagnosticFrame(sbps::EncodeFrame(header, payload), &success),
                "successful name frame was interpreted as a diagnostic");
  return ok;
}

bool ValidateResolvedNameEnvelope() {
  const auto artifacts = RunParserOnlyPipeline(
      "SELECT * FROM customer", {scratchbird::tests::FixtureUuidLiteral("00000000-0000-7000-8000-00000000c007")});
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
  ok &= Require(artifacts.envelope.descriptor_refs.empty() &&
                    artifacts.envelope.operands.empty() && artifacts.envelope.payload.empty() &&
                    artifacts.envelope.messages.has_errors() &&
                    HasDiagnostic(artifacts.envelope.messages, "QOW-DIAG-BOUNDAST-SCOPE"),
                "native refusal emitted an executable descriptor or lost its binding diagnostic");
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
      {scratchbird::tests::FixtureUuidLiteral("00000000-0000-7000-8000-00000000c107"),
       scratchbird::tests::FixtureUuidLiteral("00000000-0000-7000-8000-00000000c207")});
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

int main(int argc, char** argv) {
  bool ok = true;
  namespace memory = scratchbird::core::memory;
  const auto policy = memory::DefaultLocalEngineMemoryPolicy();
  if (!Require(memory::ConfigureDefaultMemoryManagerForFixture(policy, "lowering-verifier").ok(),
               "lowering verifier memory admission failed")) return 1;
  // Each hosted database has its own process lifetime and ownership fence.
  // The fixture-admission variant is registered as a separate process.
  if (argc == 2 && std::string_view(argv[1]) == "--fixture-attach")
    return ValidateFixtureAttachDialectIdentity() ? 0 : 1;
  if (argc != 1) return 2;
  ok &= ValidateDiagnosticFrameDecoding();
  ok &= ValidateAdmittedSelectEnvelope();
  ok &= ValidateResolvedNameEnvelope();
  ok &= ValidateMissingNativeContextRefusal();
  ok &= ValidateSecurityEnvelope();
  ok &= ValidateUnboundRefusal();
  ok &= ValidateMalformedEnvelopeRejected();
  return ok ? 0 : 1;
}
