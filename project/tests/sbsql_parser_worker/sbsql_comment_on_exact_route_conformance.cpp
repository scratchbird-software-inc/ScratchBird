// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "../support/component_authorization_fixture.hpp"
#include "binary_identity_operand_oracle.hpp"
#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/catalog_object_lifecycle_codec.hpp"
#include "catalog/catalog_journal_batch.hpp"
#include "catalog/sys_information_projection.hpp"
#include "transaction/transaction_api.hpp"
#include "query/plan_api.hpp"
#include "observability/show_api.hpp"
#include "memory.hpp"
#include "local_transaction_store.hpp"
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
#include <stdexcept>
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
    throw std::runtime_error(std::string(message));
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
    if (evidence.evidence_kind == kind && (std::holds_alternative<std::string>(evidence.evidence_id) && std::get<std::string>(evidence.evidence_id) == id)) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result, std::string_view kind,
                 const api::EngineUuid& id) {
  return std::any_of(result.evidence.begin(), result.evidence.end(), [&](const auto& evidence) {
    const auto* binary = std::get_if<api::EngineUuid>(&evidence.evidence_id);
    return evidence.evidence_kind == kind && binary && *binary == id;
  });
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
  session.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e111");
  session.connection_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e112");
  session.database_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e113");
  session.dialect_profile_uuid = scratchbird::tests::FixtureUuid(1027, 1);
  session.catalog_epoch = 81;
  session.security_policy_epoch = 82;
  session.descriptor_epoch = 83;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e114");
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
                            {scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e101")});
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
  Require(scratchbird::test::sbsql::HasExactBinaryIdentityOperand(
              artifacts.envelope, "target_object_uuid",
              scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e101")) &&
              !Contains(artifacts.envelope.payload, kTargetUuid),
          "COMMENT ON lost its exact binary target or emitted a text UUID");
  Require(Contains(artifacts.envelope.payload, "\"comment_text\":\"primary table\""),
          "COMMENT ON payload missing user comment text");
  Require(Contains(artifacts.envelope.payload, "\"comment_text_is_user_payload\":true"),
          "COMMENT ON payload did not classify comment text as user payload");
  Require(Contains(artifacts.envelope.payload,
                   "\"comment_language_source\":\"admitted_session_then_database_default\"") &&
          !Contains(artifacts.envelope.payload, "\"comment_language\":\"en\""),
          "COMMENT lowering hardcoded English rather than admitted language selection");
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

scratchbird::core::platform::Uuid AdmissionIdentity(const std::array<std::uint8_t,16>& value) {
  return {value};
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
  operation.parser_package_uuid = AdmissionIdentity(parser_uuid);
  operation.registry_snapshot_uuid = AdmissionIdentity(registry_uuid);
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
  container.canonical_anchor[100] = 2;
  std::copy(request_uuid.begin(), request_uuid.end(), container.canonical_anchor.begin() + 116);
  container.operation_payload = sbop;
  const auto container_bytes = public_sblr::EncodeSblrContainer(container);

  public_sblr::SblrExecutionEnvelopeV1 ingress;
  auto& fields = ingress.fields;
  fields[0] = AdmissionUuidField(AdmissionUuid(0x41));
  fields[1] = AdmissionU16(1);
  fields[2] = AdmissionU16(0);
  fields[3] = AdmissionU32(0x00010001);
  fields[4] = AdmissionU16(2);
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
  request.admitted_parser_package_uuid = AdmissionIdentity(parser_uuid);
  request.admitted_parser_package_version_major = 1;
  request.admitted_registry_snapshot_uuid = AdmissionIdentity(registry_uuid);
  request.authenticated_principal_uuid = AdmissionIdentity(user_uuid);
  request.catalog_snapshot_uuid = AdmissionIdentity(AdmissionUuid(0x43));
  request.engine_mga_statement_uuid = AdmissionIdentity(AdmissionUuid(0x44));
  request.engine_mga_snapshot_uuid = AdmissionIdentity(AdmissionUuid(0x45));
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

scratchbird::core::platform::Uuid CreateMinimalDatabase(const std::filesystem::path& path) {
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
  return create.database_uuid.value;
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const scratchbird::core::platform::Uuid& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-comment-on-exact-route";
  context.database_path = path.string();
  context.database_uuid = database_uuid;
  context.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e121");
  context.principal_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e122");
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  scratchbird::tests::MaterializeComponentAuthorization(context, {"CATALOG_MUTATE"});
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-B5E19A3E35A7");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const scratchbird::core::platform::Uuid& database_uuid,
                                                 std::string isolation = "read_committed") {
  auto context = EngineContext(path, database_uuid);
  // Fixture setup owns real native BEGIN. Canonical engine.op.txn_begin is
  // acquisition preflight, not a second durable transaction publication.
  api::EngineBeginTransactionRequest begin_request;
  begin_request.context = context;
  begin_request.isolation_level = isolation;
  const auto result = api::EngineBeginTransaction(begin_request);
  Require(result.ok, "COMMENT fixture native BEGIN did not return success");
  context.local_transaction_id = result.local_transaction_id;
  context.transaction_uuid = result.transaction_uuid;
  const auto inventory = db::AcquireLocalTransactionInventorySnapshot(path.string());
  Require(inventory.ok(), "COMMENT fixture could not read its actual BEGIN inventory");
  const auto entry = scratchbird::transaction::mga::LookupLocalTransaction(inventory.snapshot->inventory,
      scratchbird::transaction::mga::MakeLocalTransactionId(context.local_transaction_id));
  Require(entry.ok() && entry.entry.identity.transaction_uuid.value == context.transaction_uuid,
          "COMMENT fixture BEGIN owner differs from native inventory");
  context.snapshot_visible_through_local_transaction_id = entry.entry.begin_visible_through_local_transaction_id;
  Require(result.isolation_level == isolation, "COMMENT BEGIN isolation receipt differs");
  context.transaction_isolation_level = isolation;
  return context;
}

api::EngineApiRequest EngineCommentApiRequest(bool null_comment = false) {
  api::EngineApiRequest request;
  request.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e101");
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
  envelope.parser_package_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e131");
  envelope.registry_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e132");
  if (const auto* entry = sblr::LookupSblrOperation(std::string(kOperationId))) {
    envelope.opcode_code = entry->code;
  }
  return envelope;
}

void RequireEngineDispatch() {
  scratchbird::tests::OwnedTempDirectory temporary;
  const auto path = temporary.path() / TestDatabasePath().filename();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  auto context = BeginEngineTransaction(path, database_uuid);
  api::EngineCatalogCreateObjectRequest schema;
  schema.context = context;
  schema.target_object.uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000b5e100");
  schema.target_object.object_kind = "schema";
  api::EngineLocalizedName schema_name;
  schema_name.language_tag = "en"; schema_name.name = "comments"; schema_name.default_name = true;
  schema.localized_names.push_back(schema_name);
  Require(api::EngineCatalogCreateObject(schema).ok, "COMMENT fixture actual schema publication failed");
  api::EngineCatalogCreateObjectRequest target;
  target.context = context;
  target.target_schema = schema.target_object;
  target.target_object = EngineCommentApiRequest().target_object;
  api::EngineLocalizedName name;
  name.language_tag = "en"; name.name = "replay_target"; name.default_name = true;
  target.localized_names.push_back(name);
  const auto created_target = api::EngineCatalogCreateObject(target);
  for (const auto& diagnostic : created_target.diagnostics)
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  Require(created_target.ok, "COMMENT fixture could not publish its actual target");
  api::EngineCommitTransactionRequest target_commit;
  target_commit.context = context;
  Require(api::EngineCommitTransaction(target_commit).ok, "COMMENT fixture target commit failed");
  context = BeginEngineTransaction(path, database_uuid);

  auto unauthenticated = context;
  unauthenticated.security_context_present = false;
  unauthenticated.authorization_context = {};
  Require(!sblr::DispatchSblrOperation({unauthenticated, EngineEnvelope(), EngineCommentApiRequest()}).api_result.ok,
          "COMMENT accepted an unauthenticated writer");
  auto denied = context;
  scratchbird::tests::MaterializeComponentAuthorization(denied, {}, {"CATALOG_MUTATE"});
  Require(!sblr::DispatchSblrOperation({denied, EngineEnvelope(), EngineCommentApiRequest()}).api_result.ok,
          "COMMENT accepted an authenticated but denied writer");

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
  Require(HasEvidence(result.api_result, "comment_catalog_record", result.api_result.primary_object.uuid),
          "EngineCommentOnObject missing actual centralized record evidence");
  const auto stored = api::LoadCatalogObjectLifecycleState(context);
  Require(stored.ok && stored.state.comments.size() == 1,
          "COMMENT did not persist one actual resolver record");
  const auto original = stored.state.comments.front();
  Require(original.comment_text == "primary table" && original.language_tag == "en" &&
          original.target_object_uuid == target.target_object.uuid &&
          original.catalog_row_uuid == result.api_result.catalog_row_uuid &&
          original.comment_uuid == result.api_result.primary_object.uuid,
          "COMMENT stored text, language, target or identities differ from published result");
  Require(!result.api_result.catalog_row_uuid.is_nil(),
          "EngineCommentOnObject missing catalog row UUID evidence");
  auto conflicting = BeginEngineTransaction(path, database_uuid);
  const auto conflict = sblr::DispatchSblrOperation({conflicting, EngineEnvelope(), EngineCommentApiRequest()});
  for (const auto& diagnostic : conflict.api_result.diagnostics)
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  Require(!conflict.api_result.ok && std::any_of(conflict.api_result.diagnostics.begin(),
      conflict.api_result.diagnostics.end(), [](const auto& d) {
        return d.code == "SB_ENGINE_API_INVALID_REQUEST" &&
               d.detail == "ddl.comment_on_object:comment_concurrent_key_conflict";
      }), "COMMENT accepted a second active writer for the same key");
  api::EngineRollbackTransactionRequest conflicting_rollback;
  conflicting_rollback.context = conflicting;
  Require(api::EngineRollbackTransaction(conflicting_rollback).ok, "COMMENT conflicting writer rollback failed");
  std::string encoded;
  auto direct = original;
  direct.creator_tx = context.local_transaction_id;
  Require(api::EncodeCatalogLifecycleRecord(direct, &encoded), "COMMENT direct receiving control failed to encode");
  api::ApiBehaviorRecord direct_frame;
  Require(api::DecodeApiBehaviorRecord({reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()},
      &direct_frame), "COMMENT direct receiving control failed to decode");
  auto malformed = original;
  malformed.metadata_epoch = 0;
  Require(!api::EncodeCatalogLifecycleRecord(malformed, &encoded), "COMMENT codec accepted zero metadata epoch");
  malformed = original;
  malformed.deleted = true;
  Require(!api::EncodeCatalogLifecycleRecord(malformed, &encoded), "COMMENT codec accepted text in a retirement row");
  malformed = original;
  malformed.comment_text = std::string(1, static_cast<char>(0xff));
  Require(!api::EncodeCatalogLifecycleRecord(malformed, &encoded), "COMMENT codec accepted malformed UTF8");
  malformed = original;
  malformed.language_tag = "en\n";
  Require(!api::EncodeCatalogLifecycleRecord(malformed, &encoded), "COMMENT codec accepted a control-byte language tag");
  Require(api::AppendSequencedCatalogJournalRecord(context, direct_frame, api::CatalogJournalKind::lifecycle).error &&
      api::PublishPreparedCatalogJournalBatch(context, kOperationId,
          {{api::CatalogJournalKind::lifecycle, direct_frame}}).error,
      "COMMENT generic journal writer bypassed owned admission");

  const auto check_projection = [&](const api::EngineRequestContext& reader,
                                    std::string_view expected, bool null_expected) {
    const auto state = api::LoadCatalogObjectLifecycleState(reader);
    Require(state.ok, "COMMENT readable source load failed");
    std::vector<api::SysInformationCatalogObjectSource> objects;
    std::vector<api::SysInformationResolverNameSource> names;
    std::vector<api::SysInformationCommentSource> comments;
    for (const auto& object : state.state.objects) {
      api::SysInformationCatalogObjectSource source;
      source.object_uuid = object.object_uuid; source.object_class = object.object_kind;
      source.schema_uuid = object.schema_uuid;
      source.parent_object_uuid = object.schema_uuid;
      objects.push_back(source);
    }
    for (const auto& name : state.state.names) {
      api::SysInformationResolverNameSource source;
      source.object_uuid = name.object_uuid; source.object_class = name.object_kind;
      source.language_tag = name.language_tag; source.display_name = name.display_name;
      source.raw_name_text = name.raw_name_text; source.scope_uuid = name.schema_uuid;
      names.push_back(source);
    }
    for (const auto& comment : state.state.comments) {
      api::SysInformationCommentSource source;
      source.object_uuid = comment.target_object_uuid; source.object_class = comment.target_object_kind;
      source.language_tag = comment.language_tag; source.comment_text = comment.comment_text;
      source.catalog_generation_id = comment.metadata_epoch; source.row_ordinal = comment.event_sequence;
      comments.push_back(source);
    }
    api::SysInformationProjectionContext projection_context;
    projection_context.session_language = reader.language_context.language_tag;
    projection_context.default_language = reader.language_context.default_language_tag;
    const auto projection = api::BuildSysInformationProjection("sys.catalog_readable.objects",
        projection_context, objects, names, comments);
    Require(projection.ok && projection.rows.size() == objects.size(),
            "COMMENT readable projection did not resolve its real schema and target");
    const api::SysInformationProjectionRow* target_projection = nullptr;
    for (const auto& row : projection.rows) {
      const auto* projected_name = api::SysInformationField(row, "object_name");
      if (projected_name && *projected_name == api::SysInformationProjectionValue{std::string("replay_target")})
        target_projection = &row;
    }
    Require(target_projection, "COMMENT readable projection omitted its actual target");
    const auto* value = api::SysInformationField(*target_projection, "comment_text");
    Require(value && api::SysInformationFieldIsNull(value) == null_expected,
            "COMMENT readable projection conflated missing and empty text");
    const auto typed = api::SysInformationTypedValue(*value);
    Require(typed.is_null == null_expected && (null_expected || typed.encoded_value == expected),
            "COMMENT readable projection lost its typed NULL/text value");
    api::EnginePlanOperationRequest query;
    query.context = reader;
    query.context.catalog_generation_id = state.state.metadata_epoch;
    scratchbird::tests::MaterializeComponentAuthorization(query.context, {"CATALOG_MUTATE"});
    query.execute = true;
    query.query_operation = "values";
    query.option_envelopes.push_back("catalog_projection:sys.catalog_readable.objects");
    const auto live = api::EnginePlanOperation(query);
    for (const auto& diagnostic : live.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Require(live.ok, "COMMENT production query projection failed");
    bool found = false;
    for (const auto& row : live.result_shape.rows) {
      bool target_row = false;
      const api::EngineTypedValue* comment = nullptr;
      for (const auto& [field, cell] : row.fields) {
        if (field == "object_name" && cell.encoded_value == "replay_target") target_row = true;
        if (field == "comment_text") comment = &cell;
      }
      if (!target_row) continue;
      Require(comment && comment->is_null == null_expected &&
              (null_expected || comment->encoded_value == expected),
              "COMMENT production query did not publish the actual stored nullable text");
      found = true;
    }
    Require(found, "COMMENT production query omitted its actual target");
    api::EngineShowCatalogRequest show;
    show.context = query.context;
    show.option_envelopes = {"projection:sys.catalog_readable.objects",
                            "predicate_kind:column_like", "predicate_column:comment_text",
                            "predicate_value:%"};
    const auto shown = api::EngineShowCatalog(show);
    for (const auto& diagnostic : shown.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Require(shown.ok, "COMMENT production SHOW projection failed");
    bool show_target = false;
    for (const auto& row : shown.result_shape.rows)
      for (const auto& [field, cell] : row.fields)
        if (field == "object_name" && cell.encoded_value == "replay_target") show_target = true;
    Require(show_target == !null_expected,
            "COMMENT SHOW LIKE rejected present-empty text or admitted missing NULL");
    show.option_envelopes = {"projection:sys.catalog_readable.object_comments"};
    const auto listed = api::EngineShowCatalog(show);
    Require(listed.ok && listed.result_shape.rows.size() == state.state.comments.size(),
            "COMMENT SHOW listing did not preserve every visible language record");
    show.option_envelopes = {"projection:sys.catalog_readable.objects",
        "predicate_kind:column_in_projection", "predicate_column:comment_text",
        "subquery_projection:sys.catalog_readable.object_comments", "subquery_select_column:comment_text",
        "subquery_predicate_kind:column_like", "subquery_predicate_column:comment_text", "subquery_predicate_value:%"};
    const auto nested = api::EngineShowCatalog(show);
    Require(nested.ok && (nested.result_shape.rows.size() == 1) == !null_expected,
            "COMMENT SHOW nested projection lost stored text, present-empty or NULL distinction");
  };

  auto replacement_request = EngineCommentApiRequest();
  replacement_request.option_envelopes.back() = "comment_text:replacement:with;framing";
  const auto replacement = sblr::DispatchSblrOperation({context, EngineEnvelope(), replacement_request});
  Require(replacement.api_result.ok && replacement.api_result.primary_object.uuid == original.comment_uuid &&
          replacement.api_result.catalog_row_uuid != original.catalog_row_uuid,
          "COMMENT replacement failed to preserve logical identity with a new row version");
  auto replaced = api::LoadCatalogObjectLifecycleState(context);
  Require(replaced.ok && replaced.state.comments.size() == 1 &&
          replaced.state.comments.front().comment_text == "replacement:with;framing",
          "COMMENT replacement was not losslessly stored");
  auto default_context = context;
  default_context.language_context.language_tag.clear();
  auto default_request = replacement_request;
  default_request.option_envelopes.erase(default_request.option_envelopes.begin() + 1);
  const auto default_replacement = sblr::DispatchSblrOperation({default_context, EngineEnvelope(), default_request});
  Require(default_replacement.api_result.ok &&
          default_replacement.api_result.primary_object.uuid == original.comment_uuid,
          "COMMENT omitted language did not fall back to the database default");

  auto french_context = context;
  french_context.language_context.language_tag = "fr";
  auto french_request = EngineCommentApiRequest();
  french_request.option_envelopes.erase(french_request.option_envelopes.begin() + 1);
  french_request.option_envelopes.back() = "comment_text:bonjour";
  const auto french = sblr::DispatchSblrOperation({french_context, EngineEnvelope(), french_request});
  Require(french.api_result.ok, "COMMENT omitted-language session selection failed");
  const auto localized = api::LoadCatalogObjectLifecycleState(context);
  Require(localized.ok && localized.state.comments.size() == 2,
          "COMMENT session language overwrote the default language");
  Require(std::any_of(localized.state.comments.begin(), localized.state.comments.end(),
      [](const auto& row) { return row.language_tag == "fr" && row.comment_text == "bonjour"; }),
      "COMMENT omitted language did not persist the admitted session language");
  check_projection(french_context, "bonjour", false);
  auto french_null = EngineCommentApiRequest(true);
  french_null.option_envelopes.erase(french_null.option_envelopes.begin() + 1);
  Require(sblr::DispatchSblrOperation({french_context, EngineEnvelope(), french_null}).api_result.ok,
          "COMMENT session language retirement failed");
  check_projection(french_context, "replacement:with;framing", false);
  french_request.option_envelopes.back() = "comment_text:";
  Require(sblr::DispatchSblrOperation({french_context, EngineEnvelope(), french_request}).api_result.ok,
          "COMMENT empty text publication failed");
  check_projection(french_context, "", false);

  const sblr::SblrDispatchRequest null_comment_request{
      context, EngineEnvelope(), EngineCommentApiRequest(true)};
  const auto null_result = sblr::DispatchSblrOperation(null_comment_request);
  Require(null_result.api_result.ok, "EngineCommentOnObject did not accept NULL comment");
  const auto retired = api::LoadCatalogObjectLifecycleState(context);
  Require(retired.ok && retired.state.comments.size() == 1 &&
          retired.state.comments.front().language_tag == "fr",
          "COMMENT NULL did not retire only the exact language key");
  const auto noop = sblr::DispatchSblrOperation(null_comment_request);
  Require(noop.api_result.ok && noop.api_result.catalog_row_uuid.is_nil(),
          "COMMENT NULL on a missing key manufactured a catalog row");
  auto invalid_noop = EngineCommentApiRequest(true);
  invalid_noop.option_envelopes[1] = "comment_language:en\n";
  Require(!sblr::DispatchSblrOperation({context, EngineEnvelope(), invalid_noop}).api_result.ok,
          "COMMENT NULL no-op bypassed language validation");
  check_projection(context, "", true);
  const auto recreated = sblr::DispatchSblrOperation(comment_request);
  Require(recreated.api_result.ok && recreated.api_result.primary_object.uuid != original.comment_uuid,
          "COMMENT recreation reused a retired logical identity");
  auto sibling = BeginEngineTransaction(path, database_uuid);
  const auto invisible = api::LoadCatalogObjectLifecycleState(sibling);
  Require(invisible.ok && invisible.state.comments.empty(), "COMMENT exposed uncommitted rows to sibling");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = sibling;
  Require(api::EngineRollbackTransaction(rollback).ok, "COMMENT sibling rollback failed");
  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  Require(api::EngineCommitTransaction(commit).ok, "COMMENT transaction commit failed");
  auto reopened = BeginEngineTransaction(path, database_uuid);
  const auto durable = api::LoadCatalogObjectLifecycleState(reopened);
  Require(durable.ok && durable.state.comments.size() == 2,
          "COMMENT commit/fresh-reader did not retain both language records");
  auto stale = BeginEngineTransaction(path, database_uuid, "repeatable_read");
  auto rollback_request = EngineCommentApiRequest();
  rollback_request.option_envelopes.back() = "comment_text:must roll back";
  Require(sblr::DispatchSblrOperation({reopened, EngineEnvelope(), rollback_request}).api_result.ok,
          "COMMENT rollback fixture mutation failed");
  api::EngineCommitTransactionRequest update_commit;
  update_commit.context = reopened;
  Require(api::EngineCommitTransaction(update_commit).ok, "COMMENT concurrent update commit failed");
  check_projection(stale, "primary table", false);
  const auto stale_mutation = sblr::DispatchSblrOperation({stale, EngineEnvelope(), EngineCommentApiRequest()});
  Require(!stale_mutation.api_result.ok, "COMMENT stale snapshot overwrote a newer committed version");
  auto forged_metadata_override = stale;
  forged_metadata_override.statement_metadata_snapshot_engine_owned = true;
  Require(!sblr::DispatchSblrOperation({forged_metadata_override, EngineEnvelope(), EngineCommentApiRequest()}).api_result.ok,
          "COMMENT caller observation flag overrode the actual native transaction boundary");
  rollback.context = stale;
  Require(api::EngineRollbackTransaction(rollback).ok, "COMMENT stale reader rollback failed");
  reopened = BeginEngineTransaction(path, database_uuid);
  rollback_request.option_envelopes.back() = "comment_text:must roll back again";
  Require(sblr::DispatchSblrOperation({reopened, EngineEnvelope(), rollback_request}).api_result.ok,
          "COMMENT second rollback fixture mutation failed");
  rollback.context = reopened;
  Require(api::EngineRollbackTransaction(rollback).ok, "COMMENT reopened reader rollback failed");
  auto after_rollback = BeginEngineTransaction(path, database_uuid);
  check_projection(after_rollback, "must roll back", false);
  rollback.context = after_rollback;
  Require(api::EngineRollbackTransaction(rollback).ok, "COMMENT final reader rollback failed");
  RemoveDatabaseArtifacts(path);
  temporary.Cleanup();
}

}  // namespace

int main() try {
  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      scratchbird::core::memory::DefaultLocalEngineMemoryPolicy(), "comment_exact_route").ok(),
      "COMMENT fixture memory policy configuration failed");
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline(kSql);
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  const auto null_artifacts = RunPipeline(kNullSql);
  RequireNullCommentLowering(null_artifacts);
  RequireEngineDispatch();
  std::cout << "sbsql_comment_on_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
