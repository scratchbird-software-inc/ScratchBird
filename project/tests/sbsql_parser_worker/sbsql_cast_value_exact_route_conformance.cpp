// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "canonical_sblr_admission_test_helper.hpp"
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
constexpr std::string_view kOperationId = "engine.op.cast";
constexpr std::string_view kApiOperationId = "query.cast_value";
constexpr std::string_view kOpcode = "SBLR_CAST";
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

std::string DiagnosticField(const Diagnostic& diagnostic,
                            std::string_view name) {
  for (const auto& field : diagnostic.fields) {
    if (field.name == name) return field.value;
  }
  return {};
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

void RequireExactRefusal(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "CAST CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CAST AST failed");
  Require(artifacts.bound.bound, "CAST bind failed");
  Require(!artifacts.verifier.admitted,
          "CAST was admitted without the exact engine-bound CSDO carrier");
  Require(artifacts.envelope.operation_family == kFamily,
          "CAST operation family mismatch");
  Require(artifacts.envelope.sblr_operation_key == kFamily,
          "CAST SBLR operation key mismatch");
  Require(artifacts.envelope.operation_id == "engine.op.diagnostic_refusal" &&
              artifacts.envelope.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
              artifacts.envelope.engine_api_operation_id == "not_admitted",
          "CAST did not use the exact non-executable refusal tuple");
  Require(artifacts.envelope.result_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.diagnostic_shape_key ==
                  "diagnostic_vector.v1" &&
              artifacts.envelope.resource_contract_key ==
                  "sbsql.command.no_execution.v1",
          "CAST refusal contract metadata drifted");
  Require(artifacts.envelope.payload.empty() &&
              artifacts.envelope.operands.empty() &&
              artifacts.envelope.resolved_object_uuids.empty() &&
              !artifacts.envelope.parser_executes_sql &&
              !artifacts.envelope.real_file_effects,
          "CAST refusal emitted executable or parser-owned authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.syntax_evidence_only") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_executable_sblr") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_sql_text_execution") &&
              HasValue(artifacts.envelope.required_authority_steps,
                       "authority.parser.no_storage_or_finality"),
          "CAST refusal omitted parser non-authority evidence");
  Require(artifacts.envelope.messages.diagnostics.size() == 1,
          "CAST did not emit one exact refusal diagnostic");
  const auto& diagnostic = artifacts.envelope.messages.diagnostics.front();
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
              diagnostic.severity == "ERROR" &&
              DiagnosticField(diagnostic, "canonical_parent_operation_id") ==
                  kOperationId &&
              DiagnosticField(diagnostic, "canonical_parent_sblr_opcode") ==
                  kOpcode &&
              DiagnosticField(diagnostic, "executable_sblr_emitted") ==
                  "false",
          "CAST refusal identity or parent mapping drifted");
}

void RequireOpcodeRegistryContract() {
  const auto* opcode_entry = sblr::LookupSblrOperation(kOperationId);
  Require(opcode_entry != nullptr, "CAST opcode registry row missing");
  Require(opcode_entry->opcode == kOpcode && opcode_entry->code == 1026 &&
              opcode_entry->operand_contract == "cast_descriptor" &&
              opcode_entry->result_contract == "typed_value" &&
              opcode_entry->executor_id == kOperationId,
          "CAST exact registry tuple drifted");
  Require(opcode_entry->requires_security_context,
          "CAST opcode registry security context drifted");
  Require(opcode_entry->requires_transaction_context,
          "CAST opcode registry transaction context drifted");
  Require(opcode_entry->executor_evidence_required &&
              opcode_entry->executor_evidence_accepted,
          "CAST opcode registry executor evidence drifted");
  const auto* stale_alias = sblr::LookupSblrOperation("query.cast_value");
  Require(stale_alias == nullptr || stale_alias->code == 0,
          "unallocated CAST alias became canonical executable authority");
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

    RequireExactRefusal(artifacts);
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

    RequireExactRefusal(artifacts);
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
    Require(result.operation_id == kApiOperationId,
            "direct EngineCastValue operation id mismatch");
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
  RequireExactRefusal(artifacts);
  RequireOpcodeRegistryContract();
  RequireBooleanCastExactRoutes();
  RequireSafeTryCastExactRoutes();
  RequireDirectRuntimeValues();
  std::cout << "sbsql_cast_value_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
