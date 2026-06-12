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
#include "memory.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "scratchbird/engine/value.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
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
namespace engine = scratchbird::engine;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";
constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000360001";

struct MutationCase {
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view object_kind;
  std::string_view descriptor_ref;
  std::string_view surface_id;
  std::string_view surface_name;
};

constexpr std::array<MutationCase, 3> kMutations{{
    {"CREATE CAST cast_one;", "catalog.mutation.create_cast",
     "SBLR_CATALOG_MUTATION_CREATE_CAST", "cast",
     "sys.catalog.cast_descriptor", "SBSQL-0D79A271D250",
     "create_cast_stmt"},
    {"CREATE OPERATION normalize_email;", "catalog.mutation.create_operation",
     "SBLR_CATALOG_MUTATION_CREATE_OPERATION", "operation",
     "sys.catalog.operation_descriptor", "SBSQL-EDR036000001",
     "create_operation_stmt"},
    {"CREATE OPERATOR op_add;", "catalog.mutation.create_operator",
     "SBLR_CATALOG_MUTATION_CREATE_OPERATOR", "operator",
     "sys.catalog.operator", "SBSQL-43B8DD8E30F4",
     "create_operator_stmt"},
}};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasDiagnostic(const MessageVectorSet& messages,
                   std::string_view code,
                   std::string_view feature = {}) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code != code) continue;
    if (feature.empty()) return true;
    for (const auto& field : diagnostic.fields) {
      if (field.name == "feature" && field.value == feature) return true;
    }
  }
  return false;
}

void PrintMessages(const MessageVectorSet& messages) {
  if (!messages.diagnostics.empty()) {
    std::cerr << RenderMessageVectorSet(messages);
  }
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000360101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000360102";
  session.database_uuid = "019f0000-0000-7000-8000-000000360103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 360;
  session.security_policy_epoch = 361;
  session.descriptor_epoch = 362;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_domain_language_expansion";
  config.parser_uuid = "019f0000-0000-7000-8000-000000360104";
  config.bundle_contract_id = "sbp_sbsql@edr-036-domain-language-expansion";
  config.build_id = "sbsql-domain-language-expansion";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

void PrintIfFailed(const PipelineArtifacts& artifacts) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
}

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(),
                            session, {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireAcceptedPipeline(const PipelineArtifacts& artifacts,
                             std::string_view label) {
  if (artifacts.cst.messages.has_errors() || artifacts.ast.messages.has_errors() ||
      !artifacts.bound.bound || !artifacts.verifier.admitted) {
    PrintIfFailed(artifacts);
  }
  Require(!artifacts.cst.messages.has_errors(), std::string(label).append(" CST failed"));
  Require(!artifacts.ast.messages.has_errors(), std::string(label).append(" AST failed"));
  Require(artifacts.bound.bound, std::string(label).append(" bind failed"));
  Require(artifacts.verifier.admitted,
          std::string(label).append(" verifier rejected route"));
}

void RequireServerAdmission(const SblrEnvelope& envelope,
                            std::string_view operation_id,
                            std::string_view opcode) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "EDR-036 server admission rejected route");
  Require(admission.requires_public_abi_dispatch,
          "EDR-036 server admission did not require public ABI dispatch");
  Require(admission.operation_id == operation_id,
          "EDR-036 server admission operation id mismatch");
  Require(admission.operation_family == kFamily,
          "EDR-036 server admission operation family mismatch");

  const auto* opcode_entry = sblr::LookupSblrOperation(std::string(operation_id));
  Require(opcode_entry != nullptr, "EDR-036 opcode registry row missing");
  Require(opcode_entry->opcode == opcode, "EDR-036 opcode registry mismatch");
  Require(opcode_entry->requires_security_context,
          "EDR-036 opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "EDR-036 opcode registry transaction context drifted");
}

void RequireCreateDomainRoute() {
  const auto artifacts = RunPipeline("CREATE DOMAIN customer.email_address AS TEXT;");
  RequireAcceptedPipeline(artifacts, "EDR-036 CREATE DOMAIN");
  Require(artifacts.envelope.operation_family == kFamily,
          "EDR-036 CREATE DOMAIN operation family mismatch");
  Require(artifacts.envelope.operation_id == "ddl.create_domain",
          "EDR-036 CREATE DOMAIN operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DDL_CREATE_DOMAIN",
          "EDR-036 CREATE DOMAIN opcode mismatch");
  Require(HasValue(artifacts.envelope.required_rights, "right.catalog_mutate"),
          "EDR-036 CREATE DOMAIN missing catalog mutation right");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.ddl_create_domain_api_required"),
          "EDR-036 CREATE DOMAIN missing engine domain authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "EDR-036 CREATE DOMAIN missing MGA catalog commit authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "EDR-036 CREATE DOMAIN allowed parser SQL execution authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "EDR-036 CREATE DOMAIN allowed parser storage/finality authority");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.catalog.domain"),
          "EDR-036 CREATE DOMAIN missing domain descriptor ref");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.type_descriptor"),
          "EDR-036 CREATE DOMAIN missing type descriptor ref");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.name_registry"),
          "EDR-036 CREATE DOMAIN missing name registry ref");
  Require(!artifacts.envelope.parser_executes_sql,
          "EDR-036 CREATE DOMAIN parser executes SQL");
  Require(!artifacts.envelope.real_file_effects,
          "EDR-036 CREATE DOMAIN has parser file effects");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"create_domain_ddl\""),
          "EDR-036 CREATE DOMAIN payload missing domain envelope");
  Require(Contains(artifacts.envelope.payload, "\"domain_name_parts\":2"),
          "EDR-036 CREATE DOMAIN payload missing qualified-name evidence");
  Require(Contains(artifacts.envelope.payload,
                   "\"base_canonical_type_name\":\"text\""),
          "EDR-036 CREATE DOMAIN payload missing base type descriptor");
  Require(Contains(artifacts.envelope.payload,
                   "\"base_descriptor_embedded\":true"),
          "EDR-036 CREATE DOMAIN payload missing descriptor transport");
  Require(Contains(artifacts.envelope.payload,
                   "\"domain_constraints_included\":false"),
          "EDR-036 CREATE DOMAIN overclaimed inline constraint support");
  Require(Contains(artifacts.envelope.payload,
                   "\"domain_methods_included\":false"),
          "EDR-036 CREATE DOMAIN overclaimed inline method support");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "EDR-036 CREATE DOMAIN payload included SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          "EDR-036 CREATE DOMAIN payload included name text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "EDR-036 CREATE DOMAIN payload missing parser execution proof");
  Require(!Contains(artifacts.envelope.payload, "customer.email_address"),
          "EDR-036 CREATE DOMAIN payload embedded qualified name text");
  RequireServerAdmission(artifacts.envelope, "ddl.create_domain",
                         "SBLR_DDL_CREATE_DOMAIN");
}

void RequireMutationRoute(const MutationCase& row) {
  const auto artifacts = RunPipeline(row.sql);
  RequireAcceptedPipeline(artifacts, row.operation_id);
  Require(artifacts.envelope.operation_family == kFamily,
          "EDR-036 mutation operation family mismatch");
  Require(artifacts.envelope.operation_id == row.operation_id,
          "EDR-036 mutation operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == row.operation_id,
          "EDR-036 mutation engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          "EDR-036 mutation opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.catalog_descriptor_mutation_api_required"),
          "EDR-036 mutation missing descriptor mutation authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_catalog_commit_required"),
          "EDR-036 mutation missing MGA catalog authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "EDR-036 mutation allowed parser SQL execution authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "EDR-036 mutation allowed parser storage/finality authority");
  Require(HasValue(artifacts.envelope.descriptor_refs, row.descriptor_ref),
          "EDR-036 mutation missing specific descriptor ref");
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.catalog.descriptor_mutation_request"),
          "EDR-036 mutation missing mutation request descriptor ref");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.name_registry"),
          "EDR-036 mutation missing name registry descriptor ref");
  Require(!artifacts.envelope.parser_executes_sql,
          "EDR-036 mutation parser executes SQL");
  Require(!artifacts.envelope.real_file_effects,
          "EDR-036 mutation has parser file effects");
  Require(Contains(artifacts.envelope.payload,
                   "\"catalog_envelope_kind\":\"catalog_descriptor_mutation\""),
          "EDR-036 mutation payload missing descriptor mutation envelope");
  Require(Contains(artifacts.envelope.payload, row.operation_id),
          "EDR-036 mutation payload missing operation id");
  Require(Contains(artifacts.envelope.payload, row.surface_id),
          "EDR-036 mutation payload missing surface id");
  Require(Contains(artifacts.envelope.payload, row.surface_name),
          "EDR-036 mutation payload missing surface name");
  Require(Contains(artifacts.envelope.payload,
                   std::string("\"target_object_kind\":\"")
                       .append(row.object_kind)
                       .append("\"")),
          "EDR-036 mutation payload missing object kind");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "EDR-036 mutation payload missing parser execution proof");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal"),
          "EDR-036 mutation payload carried WAL authority");
  RequireServerAdmission(artifacts.envelope, row.operation_id, row.opcode);
}

void RequireUnsupportedDomainShape(std::string_view sql) {
  const auto artifacts = RunPipeline(sql);
  Require(artifacts.envelope.operation_id == "ddl.create_domain",
          "EDR-036 unsupported domain shape did not stay on domain route");
  Require(HasDiagnostic(artifacts.envelope.messages,
                        "SBSQL.CREATE_DOMAIN_DDL.UNSUPPORTED_SHAPE",
                        "domain_constraints_methods_or_options_out_of_slice"),
          "EDR-036 unsupported domain shape missing exact diagnostic");
  Require(!artifacts.verifier.admitted,
          "EDR-036 unsupported domain shape was admitted");
  Require(!artifacts.envelope.parser_executes_sql,
          "EDR-036 unsupported domain shape parser executes SQL");
  Require(!artifacts.envelope.real_file_effects,
          "EDR-036 unsupported domain shape had parser file effects");
}

engine::Uuid Uuid(std::uint8_t seed) {
  engine::Uuid uuid;
  for (std::size_t index = 0; index < 16; ++index) {
    uuid.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return uuid;
}

engine::ExecutionTypeDescriptor TypeDescriptor(std::uint8_t seed,
                                               std::string_view name) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 36;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::character;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  descriptor.length = 255;
  descriptor.modifier_flags =
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::length);
  return descriptor;
}

engine::ExecutionTypeDescriptor DomainTypeDescriptor(std::uint8_t seed,
                                                     std::string_view name,
                                                     engine::Uuid domain_uuid) {
  auto descriptor = TypeDescriptor(seed, name);
  descriptor.domain_uuid = domain_uuid;
  descriptor.domain_stack.push_back(domain_uuid);
  descriptor.modifier_flags |=
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid) |
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_stack);
  return descriptor;
}

engine::DomainElementPathSegment ElementPathSegment(
    engine::Uuid element_descriptor_uuid) {
  engine::DomainElementPathSegment segment;
  segment.segment_kind = engine::DomainElementPathSegmentKind::field_uuid;
  segment.canonical_token = "segment";
  segment.field_uuid = Uuid(0x40);
  segment.element_descriptor_uuid = element_descriptor_uuid;
  return segment;
}

engine::DomainElementPath ValidElementPath() {
  engine::DomainElementPath path;
  path.path_uuid = Uuid(0x10);
  path.domain_uuid = Uuid(0x11);
  path.path_descriptor_uuid = Uuid(0x12);
  path.element_descriptor_uuid = Uuid(0x13);
  path.descriptor_epoch = 36;
  path.root_descriptor =
      DomainTypeDescriptor(0x20, "edr036.domain.root", path.domain_uuid);
  path.element_descriptor = TypeDescriptor(0x13, "edr036.domain.element");
  path.segments.push_back(ElementPathSegment(path.element_descriptor_uuid));
  path.canonical_path = "/segment";
  return path;
}

engine::DomainCastRuleDescriptor ValidCastRule() {
  engine::DomainCastRuleDescriptor descriptor;
  descriptor.cast_rule_uuid = Uuid(0x30);
  descriptor.cast_policy_uuid = Uuid(0x31);
  descriptor.descriptor_epoch = 36;
  descriptor.cast_policy_epoch = 36;
  descriptor.stable_name = "edr036.cast";
  descriptor.source_domain_uuid = Uuid(0x32);
  descriptor.target_domain_uuid = Uuid(0x33);
  descriptor.source_descriptor =
      DomainTypeDescriptor(0x34, "edr036.cast.source",
                           descriptor.source_domain_uuid);
  descriptor.target_descriptor =
      DomainTypeDescriptor(0x35, "edr036.cast.target",
                           descriptor.target_domain_uuid);
  descriptor.source_descriptor_uuid =
      descriptor.source_descriptor.descriptor_uuid;
  descriptor.target_descriptor_uuid =
      descriptor.target_descriptor.descriptor_uuid;
  return descriptor;
}

engine::DomainOperationOperandDescriptor OperationOperand(
    std::uint8_t seed,
    engine::Uuid domain_uuid) {
  engine::DomainOperationOperandDescriptor operand;
  operand.domain_uuid = domain_uuid;
  operand.descriptor =
      DomainTypeDescriptor(seed, "edr036.operation.operand", domain_uuid);
  operand.operand_descriptor_uuid = operand.descriptor.descriptor_uuid;
  return operand;
}

engine::DomainOperationDescriptor ValidOperationDescriptor() {
  engine::DomainOperationDescriptor descriptor;
  descriptor.operation_uuid = Uuid(0x50);
  descriptor.operation_policy_uuid = Uuid(0x51);
  descriptor.domain_uuid = Uuid(0x52);
  descriptor.descriptor_epoch = 36;
  descriptor.operation_policy_epoch = 36;
  descriptor.stable_name = "edr036.operation";
  descriptor.operation_kind = engine::DomainOperationKind::comparison;
  descriptor.min_arity = 2;
  descriptor.max_arity = 2;
  descriptor.operands.push_back(OperationOperand(0x53, descriptor.domain_uuid));
  descriptor.operands.push_back(OperationOperand(0x54, descriptor.domain_uuid));
  descriptor.result_domain_uuid = Uuid(0x55);
  descriptor.result_descriptor =
      DomainTypeDescriptor(0x56, "edr036.operation.result",
                           descriptor.result_domain_uuid);
  descriptor.result_descriptor_uuid =
      descriptor.result_descriptor.descriptor_uuid;
  return descriptor;
}

void RequireDescriptorValidators() {
  Require(engine::ValidateDomainElementPath(ValidElementPath()).ok(),
          "EDR-036 domain element path descriptor validator rejected valid path");
  Require(engine::ValidateDomainCastRuleDescriptor(ValidCastRule()).ok(),
          "EDR-036 domain cast descriptor validator rejected valid cast");
  Require(engine::ValidateDomainOperationDescriptor(
              ValidOperationDescriptor())
              .ok(),
          "EDR-036 domain operation descriptor validator rejected valid operation");
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_domain_language_expansion_conformance";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_domain_language_expansion_conformance");
  Require(configured.ok(), "EDR-036 memory fixture configuration failed");
  Require(configured.fixture_mode, "EDR-036 memory fixture mode was not active");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_domain_language_expansion_" +
          std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events",
                            ".sb.crud_events",
                            ".sb.name_events",
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
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1780000360000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1780000360001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1780000360002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "EDR-036 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-domain-language-expansion";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000360201";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000360202";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid) {
  auto context = EngineContext(path, database_uuid);
  auto envelope = sblr::MakeSblrEnvelope("transaction.begin",
                                         "SBLR_TRANSACTION_BEGIN",
                                         "trace.edr036.transaction.begin");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.contains_sql_text = false;
  const sblr::SblrDispatchRequest request{context, envelope,
                                          api::EngineApiRequest{}};
  const auto result = sblr::DispatchSblrOperation(request);
  Require(result.envelope_validated, "EDR-036 transaction begin envelope rejected");
  Require(result.accepted, "EDR-036 transaction begin not accepted");
  Require(result.api_result.ok, "EDR-036 transaction begin failed");
  context.local_transaction_id = result.api_result.local_transaction_id;
  context.transaction_uuid = result.api_result.transaction_uuid;
  return context;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

std::string TargetUuidFor(std::size_t index) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer),
                "019f0000-0000-7000-8000-%012zu",
                static_cast<std::size_t>(360000 + index));
  return buffer;
}

sblr::SblrOperationEnvelope EngineEnvelope(const MutationCase& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         "trace.edr036.catalog_mutation");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

api::EngineApiRequest EngineMutationRequest(const MutationCase& row,
                                            std::size_t index) {
  api::EngineApiRequest request;
  request.target_schema.uuid.canonical = std::string(kSchemaUuid);
  request.target_schema.object_kind = "schema";
  request.target_object.uuid.canonical = TargetUuidFor(index);
  request.target_object.object_kind = std::string(row.object_kind);
  request.localized_names.push_back(
      {"en", "primary", "", "domain_language_descriptor_target", true});
  request.option_envelopes.push_back(
      std::string("catalog_authority:sys.catalog.") +
      std::string(row.object_kind));
  request.option_envelopes.push_back(
      std::string("descriptor_ref:") + std::string(row.descriptor_ref));
  request.option_envelopes.push_back("mga_catalog_commit_required:true");
  request.option_envelopes.push_back("parser_executes_sql:false");
  return request;
}

void RequireEngineDispatch(const std::filesystem::path& path,
                           const std::string& database_uuid) {
  auto context = BeginEngineTransaction(path, database_uuid);
  for (std::size_t index = 0; index < kMutations.size(); ++index) {
    const auto& row = kMutations[index];
    const sblr::SblrDispatchRequest request{
        context,
        EngineEnvelope(row),
        EngineMutationRequest(row, index)};
    const auto result = sblr::DispatchSblrOperation(request);
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Require(result.envelope_validated, "EDR-036 engine envelope rejected");
    Require(result.accepted, "EDR-036 engine dispatch did not accept route");
    Require(result.dispatched_to_api, "EDR-036 engine did not dispatch to API");
    Require(result.api_result.ok, "EDR-036 catalog descriptor mutation failed");
    Require(result.api_result.operation_id == row.operation_id,
            "EDR-036 engine operation id drifted");
    Require(result.api_result.primary_object.object_kind == row.object_kind,
            "EDR-036 engine object kind drifted");
    Require(HasEvidence(result.api_result, "api_behavior_event",
                        row.operation_id),
            "EDR-036 engine missing behavior event evidence");
    Require(HasEvidence(result.api_result, "catalog_descriptor_mutation",
                        row.operation_id),
            "EDR-036 engine missing descriptor mutation evidence");
    Require(HasEvidence(result.api_result, "mga_catalog_commit",
                        std::to_string(context.local_transaction_id)),
            "EDR-036 engine missing MGA catalog commit evidence");
  }
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  RequireDescriptorValidators();
  RequireCreateDomainRoute();
  for (const auto& row : kMutations) {
    RequireMutationRoute(row);
  }
  RequireUnsupportedDomainShape(
      "CREATE DOMAIN positive_int AS INTEGER CHECK (VALUE > 0);");
  RequireUnsupportedDomainShape(
      "CREATE DOMAIN email_default AS TEXT DEFAULT 'n/a';");
  RequireUnsupportedDomainShape(
      "CREATE DOMAIN email_method AS TEXT METHOD normalize;");

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  RequireEngineDispatch(path, database_uuid);
  RemoveDatabaseArtifacts(path);

  std::cout << "sbsql_domain_language_expansion_conformance=passed\n";
  return EXIT_SUCCESS;
}
