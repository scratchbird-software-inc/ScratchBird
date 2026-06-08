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
#include "query/expression_api.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kSql = "SELECT CAST('42' AS int64) AS cast_value;";
constexpr std::string_view kOperationId = "query.cast_value";
constexpr std::string_view kOpcode = "SBLR_QUERY_CAST_VALUE";
constexpr std::string_view kFamily = "sblr.expression.runtime.v3";

struct CastRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view sblr_family;
};

constexpr std::array<CastRowEvidence, 6> kCastRows{{
    {"SBSQL-6F701227513B", "cast_expr", "grammar_production", "sblr.general.operation.v3"},
    {"SBSQL-D63D7D939A15", "cast_form", "grammar_production", "sblr.general.operation.v3"},
    {"SBSQL-4E6D7545B4DF", "sb.special.cast", "function", "sblr.expression.runtime.v3"},
    {"SBSQL-73103A84DE7B", "CAST(...AS...)", "function", "sblr.expression.runtime.v3"},
    {"SBSQL-C6EDE941F4E9", "CAST", "function", "sblr.expression.runtime.v3"},
    {"SBSQL-FBCBEC94EB19", "CAST(exprAStype)", "function", "sblr.expression.runtime.v3"},
}};

constexpr std::array<CastRowEvidence, 2> kBooleanCastRows{{
    {"SBSQL-03BB09995C18", "boolean_cast_from_text", "function", "sblr.expression.runtime.v3"},
    {"SBSQL-C8EF9E3713E5", "boolean_cast_from_integer", "function", "sblr.expression.runtime.v3"},
}};

struct CastRuntimeCase {
  std::string_view source_type;
  std::string_view source_value;
  std::string_view target_type;
  std::string_view expected_value;
};

constexpr std::array<CastRuntimeCase, 5> kRuntimeCases{{
    {"character", "42", "int64", "42"},
    {"character", "true", "boolean", "true"},
    {"int64", "1", "boolean", "true"},
    {"int64", "17", "character", "17"},
    {"character", "550e8400-e29b-41d4-a716-446655440000", "uuid",
     "550e8400-e29b-41d4-a716-446655440000"},
}};

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

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000024101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000024102";
  session.database_uuid = "019f0000-0000-7000-8000-000000024103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 41;
  session.security_policy_epoch = 42;
  session.descriptor_epoch = 43;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_cast_value_route";
  config.parser_uuid = "019f0000-0000-7000-8000-000000024104";
  config.bundle_contract_id = "sbp_sbsql@cast-value-route-test";
  config.build_id = "sbsql-cast-value-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline() {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(kSql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  for (const auto& row : kCastRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "CAST generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "CAST generated registry canonical name drifted");
    Require(registry_row->surface_kind == row.surface_kind,
            "CAST generated registry kind drifted");
    Require(registry_row->source_status == "native_now",
            "CAST generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "CAST generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.sblr_family,
            "CAST generated registry SBLR family drifted");
  }
  for (const auto& row : kBooleanCastRows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "boolean CAST generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "boolean CAST generated registry canonical name drifted");
    Require(registry_row->surface_kind == row.surface_kind,
            "boolean CAST generated registry kind drifted");
    Require(registry_row->source_status == "native_now",
            "boolean CAST generated registry status drifted");
    Require(registry_row->cluster_scope == "noncluster_or_profile_scoped",
            "boolean CAST generated registry cluster scope drifted");
    Require(registry_row->sblr_operation_family == row.sblr_family,
            "boolean CAST generated registry SBLR family drifted");
  }
}

void RequireExactLowering(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "CAST CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CAST AST failed");
  Require(artifacts.bound.bound, "CAST bind failed");
  Require(artifacts.verifier.admitted, "CAST verifier rejected exact route");
  Require(artifacts.envelope.operation_family == kFamily,
          "CAST operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kFamily,
          "CAST SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == kOperationId,
          "CAST operation id mismatch");
  Require(artifacts.envelope.engine_api_operation_id == kOperationId,
          "CAST engine API operation id mismatch");
  Require(artifacts.envelope.sblr_opcode == kOpcode,
          "CAST SBLR opcode mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.datatype_cast_api_required"),
          "CAST datatype API authority step missing");
  Require(!HasValue(artifacts.envelope.required_authority_steps,
                    "authority.server.transaction_context_required"),
          "CAST route incorrectly required transaction context");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "CAST parser no-SQL-execution authority step missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "CAST lowering allowed parser SQL execution");
  Require(!artifacts.envelope.real_file_effects,
          "CAST lowering allowed donor/file effects");
  Require(Contains(artifacts.envelope.payload, "\"operation_id\":\"query.cast_value\""),
          "CAST payload missing exact operation id");
  Require(Contains(artifacts.envelope.payload,
                   "\"sblr_operation\":\"SBLR_QUERY_CAST_VALUE\""),
          "CAST payload missing exact SBLR opcode");
  Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"cast_value\""),
          "CAST payload missing cast envelope kind");
  Require(Contains(artifacts.envelope.payload, "\"source_descriptor_type\":\"character\""),
          "CAST payload missing source descriptor type");
  Require(Contains(artifacts.envelope.payload, "\"target_descriptor_type\":\"int64\""),
          "CAST payload missing target descriptor type");
  Require(Contains(artifacts.envelope.payload, "\"requires_transaction_context\":false"),
          "CAST payload did not prove no transaction context requirement");
  for (const auto& row : kCastRows) {
    Require(Contains(artifacts.envelope.payload, row.surface_id),
            "CAST payload missing row-identifiable surface evidence");
  }
  Require(Contains(artifacts.envelope.payload, "\"name_text_included\":false"),
          "CAST payload did not prove no name text authority");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          "CAST payload did not prove no SQL text authority");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "CAST payload did not prove parser_executes_sql=false");
  Require(!Contains(artifacts.envelope.payload, "SELECT") &&
              !Contains(artifacts.envelope.payload, "CAST("),
          "CAST payload embedded source SQL text as authority");
  Require(!Contains(artifacts.envelope.payload, "donor"),
          "CAST payload carried donor authority");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal") &&
              !Contains(artifacts.envelope.payload, "recovery"),
          "CAST payload carried WAL/recovery authority");
}

void RequireServerAdmission(const SblrEnvelope& envelope) {
  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{envelope.payload, false});
  Require(admission.admitted, "server admission rejected CAST exact route");
  Require(admission.requires_public_abi_dispatch,
          "server admission did not require public ABI dispatch for CAST");
  Require(admission.operation_id == kOperationId,
          "server admission CAST operation id mismatch");
  Require(admission.operation_family == kFamily,
          "server admission CAST operation family mismatch");
  const auto* opcode_entry = sblr::LookupSblrOperation(kOperationId);
  Require(opcode_entry != nullptr, "CAST opcode registry row missing");
  Require(opcode_entry->opcode == kOpcode, "CAST opcode registry opcode drifted");
  Require(opcode_entry->requires_security_context,
          "CAST opcode registry security context drifted");
  Require(!opcode_entry->requires_transaction_context,
          "CAST opcode registry transaction context drifted");
}

void RequireBooleanCastExactRoutes() {
  struct BooleanCastFixture {
    std::string_view sql;
    std::string_view source_descriptor_type;
    std::string_view surface_id;
  };
  constexpr std::array<BooleanCastFixture, 2> fixtures{{
      {"SELECT CAST('true' AS boolean) AS cast_value", "character", "SBSQL-03BB09995C18"},
      {"SELECT CAST(1 AS boolean) AS cast_value", "int64", "SBSQL-C8EF9E3713E5"},
  }};

  for (const auto& fixture : fixtures) {
    PipelineArtifacts artifacts;
    const auto session = ParserSession();
    artifacts.cst = BuildCst(fixture.sql);
    artifacts.ast = BuildAst(artifacts.cst);
    artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session);
    artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
    artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);

    Require(!artifacts.cst.messages.has_errors(), "boolean CAST CST failed");
    Require(!artifacts.ast.messages.has_errors(), "boolean CAST AST failed");
    Require(artifacts.bound.bound, "boolean CAST bind failed");
    Require(artifacts.verifier.admitted, "boolean CAST verifier rejected exact route");
    Require(artifacts.envelope.operation_family == kFamily,
            "boolean CAST operation family mismatch");
    Require(artifacts.envelope.operation_id == kOperationId,
            "boolean CAST operation id mismatch");
    Require(artifacts.envelope.sblr_opcode == kOpcode,
            "boolean CAST SBLR opcode mismatch");
    Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"cast_value\""),
            "boolean CAST payload missing cast route marker");
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"source_descriptor_type\":\"") +
                         std::string(fixture.source_descriptor_type) + "\""),
            "boolean CAST payload missing source descriptor proof");
    Require(Contains(artifacts.envelope.payload, "\"target_descriptor_type\":\"boolean\""),
            "boolean CAST payload missing boolean target descriptor proof");
    Require(Contains(artifacts.envelope.payload, fixture.surface_id),
            "boolean CAST payload missing row-identifiable semantic surface id");
    Require(Contains(artifacts.envelope.payload, "\"cast_semantic_surface_ids\""),
            "boolean CAST payload missing semantic surface id list");
    Require(Contains(artifacts.envelope.payload, "\"requires_transaction_context\":false"),
            "boolean CAST payload claimed transaction context");
    Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
            "boolean CAST payload did not prove no SQL text authority");
    Require(!Contains(artifacts.envelope.payload, fixture.sql),
            "boolean CAST payload embedded source SQL text");
    Require(!Contains(artifacts.envelope.payload, "WAL") &&
                !Contains(artifacts.envelope.payload, "wal"),
            "boolean CAST payload carried WAL authority");

    RequireServerAdmission(artifacts.envelope);
  }
}

void RequireSafeTryCastExactRoutes() {
  struct SafeTryCastFixture {
    std::string_view sql;
    std::string_view function_id;
    std::string_view bare_surface_id;
    std::string_view argument_surface_id;
    std::string_view target_descriptor_type;
  };
  constexpr std::array<SafeTryCastFixture, 2> fixtures{{
      {"SELECT SAFE_CAST('123' AS int64) AS safe_value", "sb.scalar.safe_cast",
       "SBSQL-D6FBF57E26FC", "SBSQL-6A962F180717", "int64"},
      {"SELECT TRY_CAST('bad' AS int64) AS try_value", "sb.scalar.try_cast",
       "SBSQL-78EE8FA84A8F", "SBSQL-77A5EAFF0CD5", "int64"},
  }};

  for (const auto& fixture : fixtures) {
    PipelineArtifacts artifacts;
    const auto session = ParserSession();
    artifacts.cst = BuildCst(fixture.sql);
    artifacts.ast = BuildAst(artifacts.cst);
    artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session);
    artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
    artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);

    Require(!artifacts.cst.messages.has_errors(), "SAFE/TRY CAST CST failed");
    Require(!artifacts.ast.messages.has_errors(), "SAFE/TRY CAST AST failed");
    Require(artifacts.bound.bound, "SAFE/TRY CAST bind failed");
    Require(artifacts.verifier.admitted, "SAFE/TRY CAST verifier rejected exact route");
    Require(artifacts.envelope.operation_family == kFamily,
            "SAFE/TRY CAST operation family mismatch");
    Require(artifacts.envelope.operation_id == kOperationId,
            "SAFE/TRY CAST operation id mismatch");
    Require(artifacts.envelope.sblr_opcode == kOpcode,
            "SAFE/TRY CAST SBLR opcode mismatch");
    Require(Contains(artifacts.envelope.payload, "\"query_envelope_kind\":\"cast_value\""),
            "SAFE/TRY CAST payload missing cast route marker");
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"cast_function_id\":\"") +
                         std::string(fixture.function_id) + "\""),
            "SAFE/TRY CAST payload missing canonical function id");
    Require(Contains(artifacts.envelope.payload,
                     std::string("\"target_descriptor_type\":\"") +
                         std::string(fixture.target_descriptor_type) + "\""),
            "SAFE/TRY CAST payload missing target descriptor proof");
    Require(Contains(artifacts.envelope.payload, fixture.bare_surface_id),
            "SAFE/TRY CAST payload missing bare surface id");
    Require(Contains(artifacts.envelope.payload, fixture.argument_surface_id),
            "SAFE/TRY CAST payload missing argument surface id");
    Require(Contains(artifacts.envelope.payload, "\"cast_semantic_surface_ids\""),
            "SAFE/TRY CAST payload missing semantic surface id list");
    Require(Contains(artifacts.envelope.payload, "\"requires_transaction_context\":false"),
            "SAFE/TRY CAST payload claimed transaction context");
    Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
            "SAFE/TRY CAST payload did not prove no SQL text authority");
    Require(!Contains(artifacts.envelope.payload, fixture.sql),
            "SAFE/TRY CAST payload embedded source SQL text");
    Require(!Contains(artifacts.envelope.payload, "WAL") &&
                !Contains(artifacts.envelope.payload, "wal"),
            "SAFE/TRY CAST payload carried WAL authority");

    RequireServerAdmission(artifacts.envelope);
  }
}

api::EngineRequestContext EngineContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-cast-value-exact-route";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000024201";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000024202";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000024203";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-6F701227513B");
  context.trace_tags.push_back("sbsql_surface_id:SBSQL-D63D7D939A15");
  return context;
}

api::EngineDescriptor Descriptor(std::string_view type) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::string(type);
  descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineTypedValue Value(std::string_view type, std::string_view encoded) {
  api::EngineTypedValue value;
  value.descriptor = Descriptor(type);
  value.encoded_value = std::string(encoded);
  return value;
}

api::EngineApiRequest GenericCastApiRequest(const CastRuntimeCase& item) {
  api::EngineApiRequest request;
  api::EngineRowValue row;
  row.fields.push_back({"cast_input", Value(item.source_type, item.source_value)});
  request.rows.push_back(std::move(row));
  request.descriptors.push_back(Descriptor(item.target_type));
  return request;
}

sblr::SblrOperationEnvelope EngineEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope(std::string(kOperationId),
                                         std::string(kOpcode),
                                         "trace.cast_value.exact_route.SBSQL-6F701227513B");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

void RequireSblrDispatch(const CastRuntimeCase& item) {
  const sblr::SblrDispatchRequest request{
      EngineContext(),
      EngineEnvelope(),
      GenericCastApiRequest(item)};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated, "engine SBLR envelope did not validate");
  Require(result.accepted, "engine SBLR dispatch did not accept CAST");
  Require(result.dispatched_to_api, "engine SBLR dispatch did not route CAST to internal API");
  Require(result.api_result.ok, "EngineCastValue dispatch did not return success");
  Require(result.api_result.operation_id == kOperationId,
          "EngineCastValue dispatch returned wrong operation id");
  Require(HasEvidence(result.api_result, "datatype_cast", "lossless_explicit") ||
              HasEvidence(result.api_result, "datatype_cast", "lossless_implicit"),
          "EngineCastValue dispatch missing datatype cast evidence");
  Require(!result.api_result.result_shape.columns.empty(),
          "EngineCastValue dispatch returned no result descriptor");
  Require(result.api_result.result_shape.columns.front().canonical_type_name ==
              item.target_type,
          "EngineCastValue dispatch result descriptor mismatch");
}

void RequireDirectRuntimeValues() {
  for (const auto& item : kRuntimeCases) {
    api::EngineCastValueRequest request;
    request.context = EngineContext();
    request.input_value = Value(item.source_type, item.source_value);
    request.target_descriptor = Descriptor(item.target_type);
    request.explicit_cast = true;
    const auto result = api::EngineCastValue(request);
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Require(result.ok, "direct EngineCastValue returned failure");
    Require(result.operation_id == kOperationId, "direct EngineCastValue operation id mismatch");
    Require(result.value.descriptor.canonical_type_name == item.target_type,
            "direct EngineCastValue target descriptor mismatch");
    Require(result.value.encoded_value == item.expected_value,
            "direct EngineCastValue encoded value mismatch");
    Require(HasEvidence(result, "datatype_cast", result.cast_category),
            "direct EngineCastValue missing cast category evidence");
  }
}

}  // namespace

int main() {
  RequireRegistryEvidence();
  const auto artifacts = RunPipeline();
  RequireExactLowering(artifacts);
  RequireServerAdmission(artifacts.envelope);
  RequireBooleanCastExactRoutes();
  RequireSafeTryCastExactRoutes();
  RequireSblrDispatch(kRuntimeCases.front());
  RequireDirectRuntimeValues();
  std::cout << "sbsql_cast_value_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
