// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "ast/ast.hpp"
#include "canonical_sblr_admission_test_helper.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "query/expression_api.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "core/datatypes/datatype_catalog_manifest.hpp"
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

constexpr std::array<CastRuntimeCase, 4> kRuntimeCases{{
    {"character", "42", "int64", "42"},
    {"character", "true", "boolean", "true"},
    {"int64", "1", "boolean", "true"},
    {"int64", "17", "character", "17"},
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
    if (evidence.evidence_kind == kind && (std::holds_alternative<std::string>(evidence.evidence_id) && std::get<std::string>(evidence.evidence_id) == id)) return true;
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
  session.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000024101");
  session.connection_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000024102");
  session.database_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000024103");
  session.dialect_profile_uuid = scratchbird::tests::FixtureUuid(1027, 1);
  session.catalog_epoch = 41;
  session.security_policy_epoch = 42;
  session.descriptor_epoch = 43;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_cast_value_route";
  config.parser_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000024104");
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
  context.database_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000024201");
  context.session_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000024202");
  context.principal_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7000-8000-000000024203");
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

api::EngineDescriptor BoundDescriptor(std::string_view type) {
  namespace dt = scratchbird::core::datatypes;
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "cast fixture datatype catalog unavailable");
  const auto row = dt::LookupDatatypeCatalogRow(
      manifest.manifest, dt::CanonicalTypeIdFromStableName(std::string(type)));
  Require(row.ok() && row.manifest.descriptor_rows.size() == 1,
          "cast fixture datatype absent from catalog");
  auto descriptor = Descriptor(type);
  const auto& datatype = row.manifest.descriptor_rows.front();
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      api::kBootstrapDatatypeCatalogUuid,
      api::kBootstrapDatatypeCatalogGeneration,
      api::kBootstrapDatatypeRegistryGeneration,
      datatype.descriptor_uuid.value, datatype.descriptor_epoch);
  Require(identity.ok, "cast fixture datatype identity is not admitted");
  descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(
      2053, type == "uuid" ? 1 : 2);
  descriptor.type_uuid = identity.row.type_uuid;
  descriptor.datatype_descriptor_uuid = datatype.descriptor_uuid.value;
  descriptor.datatype_descriptor_generation = datatype.descriptor_epoch;
  descriptor.encoded_descriptor = "nullability=nullable";
  return descriptor;
}

void RequireUuidBinaryCasts() {
  // UUID text is parser/client syntax, not an engine UUID value. Retain the
  // old v4 sample, but assert its exact bytes and the refusal of its textual
  // engine carrier. Also exercise every version nibble, nil and all-one data.
  const auto v4 = scratchbird::tests::FixtureUuidLiteral(
      "550e8400-e29b-41d4-a716-446655440000");
  std::vector<std::vector<std::uint8_t>> values;
  values.emplace_back(v4.bytes.begin(), v4.bytes.end());
  for (unsigned version = 0; version < 16; ++version) {
    auto bytes = values.front();
    bytes[6] = static_cast<std::uint8_t>((version << 4) | (bytes[6] & 15));
    values.push_back(std::move(bytes));
  }
  values.emplace_back(16, 0);
  values.emplace_back(16, 255);
  for (const bool bound : {false, true}) {
    const auto descriptor = [&](std::string_view type) {
      return bound ? BoundDescriptor(type) : Descriptor(type);
    };
    const auto cast = [&](api::EngineTypedValue input, std::string_view target,
                          bool explicit_cast = true) {
      api::EngineCastValueRequest request;
      request.context = EngineContext();
      request.input_value = std::move(input);
      request.target_descriptor = descriptor(target);
      request.explicit_cast = explicit_cast;
      return api::EngineCastValue(request);
    };
    const auto require_refused = [&](api::EngineTypedValue input,
                                      std::string_view target) {
      const auto result = cast(std::move(input), target);
      Require(!result.ok && !result.diagnostics.empty() &&
                  result.value.encoded_value.empty() && result.value.binary_value.empty(),
              "malformed UUID cast published a value or lost its refusal");
    };
    for (const auto& bytes : values) {
      for (const auto source : {"uuid", "binary"}) {
        for (const auto target : {"uuid", "binary"}) {
          api::EngineTypedValue input;
          input.descriptor = descriptor(source);
          input.binary_value = bytes;
          const auto result = cast(input, target);
          Require(result.ok && result.value.binary_value == bytes &&
                      result.value.encoded_value.empty() &&
                      result.value.state == api::EngineValueState::value &&
                      !result.value.isSqlNull() &&
                      result.value.descriptor.descriptor_uuid == descriptor(target).descriptor_uuid &&
                      result.value.descriptor.canonical_type_name == target &&
                      HasEvidence(result, "datatype_cast", result.cast_category),
                  "UUID/binary cast did not preserve binary16 data and target descriptor");
          const auto roundtrip = cast(result.value, source);
          Require(roundtrip.ok && roundtrip.value.binary_value == bytes &&
                      roundtrip.value.encoded_value.empty(),
                  "UUID/binary roundtrip lost bits or produced text");
          if (bound) {
            api::EngineTypedValue output;
            std::string category, refusal;
            Require(api::QowApplyCanonicalDescriptorCoercionV1(
                        input, descriptor(target), true, &output, &category, &refusal) &&
                        refusal.empty() && !category.empty() &&
                        output.binary_value == bytes && output.encoded_value.empty(),
                    "descriptor coercion lost native UUID/binary result bytes");
            auto unbound = input;
            unbound.descriptor.descriptor_uuid = {};
            Require(!api::QowApplyCanonicalDescriptorCoercionV1(
                        unbound, descriptor(target), true, &output, &category, &refusal),
                    "UUID data coercion accepted missing system descriptor authority");
            unbound = input;
            unbound.descriptor.type_uuid.bytes[6] = 0x40;
            Require(!api::QowApplyCanonicalDescriptorCoercionV1(
                        unbound, descriptor(target), true, &output, &category, &refusal),
                    "UUID data coercion accepted a v4 system type identity");
          }
          const auto implicit = cast(input, target, false);
          Require(implicit.ok == (std::string_view(source) == target),
                  "UUID cast changed explicit-versus-identity admission");
        }
      }
    }
    for (const auto source : {"uuid", "binary"}) {
      api::EngineTypedValue input;
      input.descriptor = descriptor(source);
      input.state = api::EngineValueState::sql_null;
      input.is_null = true;
      const auto null_cast = cast(input, "uuid");
      if (bound) {
        Require(null_cast.ok &&
                    null_cast.value.state == api::EngineValueState::sql_null &&
                    null_cast.value.is_null &&
                    null_cast.value.encoded_value.empty() &&
                    null_cast.value.binary_value.empty(),
                "bound UUID NULL cast retained a payload");
      } else {
        Require(!null_cast.ok && !null_cast.diagnostics.empty(),
                "unbound UUID NULL cast bypassed descriptor authority");
      }
      input.is_null = false;
      require_refused(input, "uuid");
      input.is_null = true;
      input.binary_value = values.front();
      require_refused(input, "uuid");
      input.state = api::EngineValueState::value;
      input.encoded_value = "ambiguous";
      require_refused(input, "uuid");
      input.encoded_value.clear();
      for (const auto size : {0u, 15u, 17u, 36u}) {
        input.binary_value.resize(size);
        require_refused(input, "uuid");
      }
      input.binary_value.clear();
      input.encoded_value = "550e8400-e29b-41d4-a716-446655440000";
      require_refused(input, "uuid");
    }
    auto text = Value("character", "550e8400-e29b-41d4-a716-446655440000");
    text.descriptor = descriptor("character");
    require_refused(text, "uuid");
    api::EngineTypedValue uuid;
    uuid.descriptor = descriptor("uuid");
    uuid.binary_value = values.front();
    require_refused(uuid, "character");
    for (const auto state : {api::EngineValueState::error, api::EngineValueState::missing,
                             api::EngineValueState::unknown,
                             api::EngineValueState::default_requested}) {
      uuid.state = state;
      require_refused(uuid, "uuid");
    }
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
  RequireUuidBinaryCasts();
  std::cout << "sbsql_cast_value_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
