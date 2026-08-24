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
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kSql = "COMMENT ON TABLE replay_target IS 'primary table';";
constexpr std::string_view kNullSql = "COMMENT ON TABLE replay_target IS NULL;";
constexpr std::string_view kOperationId = "ddl.comment_on_object";
constexpr std::string_view kOpcode = "SBLR_DDL_COMMENT_ON";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kSurfaceId = "SBSQL-B5E19A3E35A7";
constexpr std::string_view kObjectClassSurfaceId = "SBSQL-5CCF87EB0C5C";
constexpr std::string_view kQualifiedNameSurfaceId = "SBSQL-ADEF20254494";
constexpr std::string_view kSurfaceName = "comment_on_stmt";
constexpr std::string_view kFixtureId = "SBSQL-SURFACE-716DFE515307";
constexpr std::string_view kTargetUuid = "019f0000-0000-7000-8000-000000b5e101";

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
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

bool ResultPayloadContains(const api::EngineApiResult& result, std::string_view expected) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& [name, value] : row.fields) {
      if (name == "payload" && Contains(value.encoded_value, expected)) return true;
    }
  }
  return false;
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    for (const auto& field : diagnostic.fields) {
      std::cerr << "  " << field.name << '=' << field.value << '\n';
    }
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000b5e111";
  session.connection_uuid = "019f0000-0000-7000-8000-000000b5e112";
  session.database_uuid = "019f0000-0000-7000-8000-000000b5e113";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 81;
  session.security_policy_epoch = 82;
  session.descriptor_epoch = 83;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000b5e114";
  config.bundle_contract_id = "sbp_sbsql@comment-on-route-test";
  config.build_id = "sbsql-comment-on-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {std::string(kTargetUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryRow(std::string_view surface_id,
                        std::string_view canonical_name,
                        std::string_view family,
                        std::string_view sblr_family) {
  const auto* row = FindGeneratedSurfaceRegistryRowById(surface_id);
  Require(row != nullptr, "generated registry row missing");
  Require(row->canonical_name == canonical_name, "generated registry canonical name drifted");
  Require(row->surface_kind == "grammar_production", "generated registry surface kind drifted");
  Require(row->family == family, "generated registry family drifted");
  Require(row->source_status == "native_now", "generated registry status drifted");
  Require(row->cluster_scope == "noncluster_or_profile_scoped",
          "generated registry cluster scope drifted");
  Require(row->sblr_operation_family == sblr_family,
          "generated registry SBLR family drifted");
}

void RequireRegistryEvidence() {
  RequireRegistryRow(kSurfaceId, kSurfaceName, "ddl_catalog", kFamily);
  const auto* comment_row = FindGeneratedSurfaceRegistryRowById(kSurfaceId);
  Require(comment_row->validation_fixture_id == kFixtureId,
          "COMMENT ON generated registry fixture id drifted");
  RequireRegistryRow(kObjectClassSurfaceId, "object_class", "general",
                     "sblr.general.operation.v3");
  RequireRegistryRow(kQualifiedNameSurfaceId, "qualified_name", "general",
                     "sblr.general.operation.v3");
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "COMMENT ON CST failed");
  Require(!artifacts.ast.messages.has_errors(), "COMMENT ON AST failed");
  Require(artifacts.bound.bound, "COMMENT ON bind failed");
  Require(artifacts.verifier.admitted, "COMMENT ON verifier rejected exact route");
  Require(artifacts.envelope.operation_id == kOperationId,
          "COMMENT ON operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "COMMENT ON engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode, "COMMENT ON opcode mismatch");
  Require(artifacts.envelope.operation_family == kFamily,
          "COMMENT ON operation family mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "COMMENT ON catalog mutation right missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_comment_on_object_api_required"),
          "COMMENT ON API authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "COMMENT ON MGA catalog authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "COMMENT ON parser no-storage authority step missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "COMMENT ON parser no-SQL-execution authority step missing");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"comment_on_object_ddl\""),
          "COMMENT ON payload missing catalog envelope kind");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_authority\":\"sys.catalog.object_comment\""),
          "COMMENT ON payload missing object comment catalog authority");
  Require(Contains(artifacts.envelope.payload, "\"target_object_kind\":\"table\""),
          "COMMENT ON payload missing target object kind");
  Require(Contains(artifacts.envelope.payload,
                   "\"target_object_uuid\":\"019f0000-0000-7000-8000-000000b5e101\""),
          "COMMENT ON payload missing target UUID");
  Require(Contains(artifacts.envelope.payload, "\"comment_text\":\"primary table\""),
          "COMMENT ON payload missing user comment text");
  Require(Contains(artifacts.envelope.payload, "\"comment_text_is_user_payload\":true"),
          "COMMENT ON payload did not classify comment text as user payload");
  Require(Contains(artifacts.envelope.payload, kSurfaceId),
          "COMMENT ON payload missing row-identifiable statement evidence");
  Require(Contains(artifacts.envelope.payload, kObjectClassSurfaceId),
          "COMMENT ON payload missing object_class row evidence");
  Require(Contains(artifacts.envelope.payload, kQualifiedNameSurfaceId),
          "COMMENT ON payload missing qualified_name row evidence");
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          "COMMENT ON payload did not prove no name text authority");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "COMMENT ON payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "COMMENT ON payload did not prove parser_executes_sql=false");
  Require(!Contains(artifacts.envelope.payload, "replay_target") &&
              !Contains(artifacts.envelope.payload, std::string(kSql)),
          "COMMENT ON payload embedded SQL text or identifier names as authority");
  Require(!Contains(artifacts.envelope.payload, "reference"),
          "COMMENT ON payload carried reference authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "COMMENT ON payload carried WAL/recovery authority");
}

void RequireNullCommentLowering(const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(artifacts.bound.bound, "COMMENT ON NULL bind failed");
  Require(artifacts.verifier.admitted, "COMMENT ON NULL verifier rejected exact route");
  Require(artifacts.envelope.operation_id == kOperationId,
          "COMMENT ON NULL operation id mismatch");
  Require(Contains(artifacts.envelope.payload, "\"comment_is_null\":true"),
          "COMMENT ON NULL payload missing null-comment evidence");
  Require(!Contains(artifacts.envelope.payload, "\"comment_text\""),
          "COMMENT ON NULL payload carried comment text");
}

using CanonicalBytes = std::vector<std::uint8_t>;

std::array<std::uint8_t, 16> AdmissionUuid(std::uint8_t suffix) {
  std::array<std::uint8_t, 16> value{};
  value[0] = 0x12;
  value[1] = 0x34;
  value[6] = 0x70;
  value[8] = 0x80;
  value[15] = suffix;
  return value;
}

std::string AdmissionUuidText(const std::array<std::uint8_t, 16>& value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back('-');
    text.push_back(kHex[value[i] >> 4]);
    text.push_back(kHex[value[i] & 0x0f]);
  }
  return text;
}

CanonicalBytes AdmissionUuidField(const std::array<std::uint8_t, 16>& value) {
  return {value.begin(), value.end()};
}

CanonicalBytes AdmissionU16(std::uint16_t value) {
  CanonicalBytes out;
  scratchbird::engine::SblrAppendU16(out, value);
  return out;
}

CanonicalBytes AdmissionU32(std::uint32_t value) {
  CanonicalBytes out;
  scratchbird::engine::SblrAppendU32(out, value);
  return out;
}

CanonicalBytes AdmissionU64(std::uint64_t value) {
  CanonicalBytes out;
  scratchbird::engine::SblrAppendU64(out, value);
  return out;
}

CanonicalBytes AdmissionStruct(std::uint32_t tag, std::uint8_t value) {
  CanonicalBytes out;
  scratchbird::engine::SblrAppendU32(out, tag);
  scratchbird::engine::SblrAppendU16(out, 1);
  scratchbird::engine::SblrAppendU16(out, 0);
  scratchbird::engine::SblrAppendU64(out, 1);
  out.push_back(value);
  return out;
}

CanonicalBytes AdmissionInline(const CanonicalBytes& payload) {
  CanonicalBytes out{1};
  scratchbird::engine::SblrAppendU64(out, payload.size());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

CanonicalBytes AdmissionOptionalUuid(const std::array<std::uint8_t, 16>& value) {
  CanonicalBytes out{1};
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

scratchbird::server::ServerSblrAdmissionRequest CanonicalAdmissionRequest() {
  namespace public_sblr = scratchbird::engine;
  const auto parser_uuid = AdmissionUuid(0x31);
  const auto dialect_uuid = AdmissionUuid(0x22);
  const auto registry_uuid = AdmissionUuid(0x32);
  const auto user_uuid = AdmissionUuid(0x42);
  auto operation = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                           std::string(kOpcode),
                                           "comment-on-canonical-admission");
  const auto* registry = sblr::LookupSblrOperation(std::string(kOperationId));
  Require(registry != nullptr, "COMMENT ON opcode registry row missing");
  operation.opcode_code = registry->code;
  operation.parser_package_uuid = AdmissionUuidText(parser_uuid);
  operation.registry_snapshot_uuid = AdmissionUuidText(registry_uuid);
  operation.requires_security_context = true;
  operation.requires_transaction_context = true;
  operation.parser_resolved_names_to_uuids = true;
  const auto validation = sblr::ValidateSblrEnvelope(operation);
  if (!validation.ok) {
    for (const auto& diagnostic : validation.diagnostics) {
      std::cerr << "COMMENT ON canonical operation: " << diagnostic.code << ':'
                << diagnostic.message << '\n';
    }
  }
  const auto sbop_text = sblr::EncodeSblrEnvelope(operation);
  const CanonicalBytes sbop(sbop_text.begin(), sbop_text.end());

  public_sblr::SblrCanonicalContainer container;
  const auto engine_uuid = AdmissionUuid(0x21);
  const auto bundle_uuid = AdmissionUuid(0x24);
  const auto request_uuid = AdmissionUuid(0x25);
  std::copy(engine_uuid.begin(), engine_uuid.end(), container.canonical_anchor.begin());
  std::copy(dialect_uuid.begin(), dialect_uuid.end(),
            container.canonical_anchor.begin() + 16);
  std::copy(parser_uuid.begin(), parser_uuid.end(),
            container.canonical_anchor.begin() + 32);
  container.canonical_anchor[48] = 1;
  container.canonical_anchor[52] = 1;
  container.canonical_anchor[60] = 1;
  container.canonical_anchor[68] = 1;
  std::copy(bundle_uuid.begin(), bundle_uuid.end(), container.canonical_anchor.begin() + 76);
  container.canonical_anchor[92] = 1;
  container.canonical_anchor[100] = 1;
  std::copy(request_uuid.begin(), request_uuid.end(), container.canonical_anchor.begin() + 116);
  container.operation_payload = sbop;
  const auto container_bytes = public_sblr::EncodeSblrContainer(container);

  public_sblr::SblrExecutionEnvelopeV1 ingress;
  auto& fields = ingress.fields;
  fields[0] = AdmissionUuidField(AdmissionUuid(0x41));
  fields[1] = AdmissionU16(1);
  fields[2] = AdmissionU16(0);
  fields[3] = AdmissionU32(0x00010001);
  fields[4] = AdmissionU16(1);
  fields[5] = {0};
  fields[6] = AdmissionInline(sbop);
  fields[7] = {1};
  const auto crc = AdmissionU32(public_sblr::SblrCrc32c(sbop.data(), sbop.size()));
  fields[7].insert(fields[7].end(), crc.begin(), crc.end());
  fields[8] = AdmissionU64(sbop.size());
  fields[9] = AdmissionU16(1);
  fields[10] = AdmissionOptionalUuid(dialect_uuid);
  fields[11] = AdmissionOptionalUuid(user_uuid);
  fields[12] = AdmissionStruct(0x1001, 1);
  fields[13] = AdmissionStruct(0x1002, 2);
  fields[14] = {0};
  fields[15] = AdmissionU64(1);
  fields[16] = AdmissionU32(0);
  fields[17] = AdmissionU32(0);
  fields[18] = AdmissionU32(0);
  fields[19] = {0};
  fields[20] = AdmissionU32(0);
  fields[21] = AdmissionStruct(0x1005, 5);
  fields[22] = {0};
  fields[23] = {0};
  fields[24] = {0};
  fields[25] = AdmissionU16(0);
  fields[26] = {0};
  fields[27] = {0};
  const auto ingress_bytes = public_sblr::EncodeSblrExecutionEnvelopeV1(ingress);

  scratchbird::server::ServerSblrAdmissionRequest request;
  request.encoded_sblr_container.assign(container_bytes.begin(), container_bytes.end());
  request.encoded_execution_envelope.assign(ingress_bytes.begin(), ingress_bytes.end());
  request.admitted_parser_package_uuid = AdmissionUuidText(parser_uuid);
  request.admitted_parser_package_version_major = 1;
  request.admitted_registry_snapshot_uuid = AdmissionUuidText(registry_uuid);
  request.authenticated_principal_uuid = AdmissionUuidText(user_uuid);
  request.catalog_snapshot_uuid = AdmissionUuidText(AdmissionUuid(0x43));
  request.engine_mga_statement_uuid = AdmissionUuidText(AdmissionUuid(0x44));
  request.engine_mga_snapshot_uuid = AdmissionUuidText(AdmissionUuid(0x45));
  request.catalog_epoch = 7;
  request.security_epoch = 8;
  request.resource_epoch = 9;
  return request;
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  (void)envelope;
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      CanonicalAdmissionRequest());
  Require(admission.admitted, "server admission rejected COMMENT ON exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for COMMENT ON");
  Require(admission.operation_id == kOperationId,
          "server admission COMMENT ON operation id mismatch");
  Require(admission.operation_family == kFamily,
          "server admission COMMENT ON operation family mismatch");
  const auto* opcode_entry = sblr::LookupSblrOperation(std::string(kOperationId));
  Require(opcode_entry != nullptr, "COMMENT ON opcode registry row missing");
  Require(opcode_entry->opcode == kOpcode, "COMMENT ON opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "COMMENT ON opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "COMMENT ON opcode registry transaction context drifted");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_comment_on_exact_route_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.name_events",
                            ".sb.mga_savepoints",
                            ".sb.transaction_inventory",
                            ".dirty.manifest",
                            ".recovery.evidence",
                            ".sb.owner.lock"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1780107802000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1780107802001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1780107802002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "COMMENT ON engine dispatch test database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-comment-on-exact-route";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000b5e121";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000b5e122";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-B5E19A3E35A7");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.comment_on.exact_route.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest request{context, envelope, api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "transaction begin envelope did not validate");
  Require(result.accepted, "transaction begin dispatch did not accept");
  Require(result.api_result.ok, "transaction begin did not return success");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  return context;
}

api::EngineApiRequest EngineCommentApiRequest(bool null_comment = false) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "table";
  request.option_envelopes.push_back("comment_target_kind:table");
  request.option_envelopes.push_back("comment_language:en");
  request.option_envelopes.push_back(null_comment ? "comment_is_null:true"
                                                  : "comment_is_null:false");
  if (!null_comment) request.option_envelopes.push_back("comment_text:primary table");
  return request;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.comment_on.exact_route.SBSQL-B5E19A3E35A7");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireEngineDispatch() {
  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = BeginEngineTransaction(path, database_uuid);

  const sblr::SblrDispatchRequest comment_request{
      context, EngineEnvelope(), EngineCommentApiRequest()};
  const auto result = sblr::DispatchSblrOperation(comment_request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept COMMENT ON");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route to internal API");
  Require(result.api_result.ok, "EngineCommentOnObject did not return success");
  Require(result.api_result.operation_id == kOperationId,
          "EngineCommentOnObject returned wrong operation id");
  Require(result.api_result.primary_object.object_kind == "object_comment",
          "EngineCommentOnObject did not return object_comment primary object");
  Require(result.api_result.primary_object.uuid.canonical == kTargetUuid,
          "EngineCommentOnObject returned wrong target UUID");
  Require(HasEvidence(result.api_result, "api_behavior_event", kOperationId),
          "EngineCommentOnObject missing API behavior event evidence");
  Require(HasEvidence(result.api_result, "object_comment", kTargetUuid),
          "EngineCommentOnObject missing object comment evidence");
  Require(ResultPayloadContains(result.api_result, "comment_text:primary table"),
          "EngineCommentOnObject did not persist comment text payload");
  Require(ResultPayloadContains(result.api_result, "comment_language:en"),
          "EngineCommentOnObject did not persist comment language payload");
  Require(!result.api_result.catalog_row_uuid.canonical.empty(),
          "EngineCommentOnObject missing catalog row UUID evidence");

  const sblr::SblrDispatchRequest null_comment_request{
      context, EngineEnvelope(), EngineCommentApiRequest(true)};
  const auto null_result = sblr::DispatchSblrOperation(null_comment_request);
  Require(null_result.api_result.ok, "EngineCommentOnObject did not accept NULL comment");
  Require(ResultPayloadContains(null_result.api_result, "comment_is_null:true"),
          "EngineCommentOnObject did not persist NULL comment evidence");
  RemoveDatabaseArtifacts(path);
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline(kSql);
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  const auto null_artifacts = RunPipeline(kNullSql);
  RequireNullCommentLowering(null_artifacts);
  RequireEngineDispatch();
  std::cout << "sbsql_comment_on_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
