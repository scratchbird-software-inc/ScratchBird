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
#include "memory.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace scratchbird::parser::sbsql;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kSql = "CREATE SCHEMA qa_schema;";
constexpr std::string_view kOperationId = "engine.op.ddl_create_schema";
constexpr std::string_view kOpcode = "SBLR_DDL_CREATE_SCHEMA";
constexpr std::string_view kFamily = "sblr.catalog.mutation.v3";

struct CreateSchemaRowEvidence {
  std::string_view surface_id;
  std::string_view canonical_name;
  std::string_view validation_fixture_id;
};

constexpr std::array<CreateSchemaRowEvidence, 2> kCreateSchemaRows{{
    {"SBSQL-DE4B8AAF6326", "create_schema_stmt",
     "SBSQL-SURFACE-4F9512A05B14"},
    {"SBSQL-7BA0B928798B", "schema_name",
     "SBSQL-SURFACE-DF3A68E8CA6C"},
}};

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_create_schema_exact_route_conformance";
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
      MemoryPolicy(), "sbsql_create_schema_exact_route_conformance");
  Require(configured.ok(), "CREATE SCHEMA memory fixture configuration failed");
  Require(configured.fixture_mode,
          "CREATE SCHEMA memory fixture mode was not active");
}

std::string DiagnosticField(const Diagnostic& diagnostic,
                            std::string_view name) {
  for (const auto& field : diagnostic.fields) {
    if (field.name == name) return field.value;
  }
  return {};
}

bool HasAuthorityStep(const SblrEnvelope& envelope,
                      std::string_view expected) {
  return std::ranges::find(envelope.required_authority_steps, expected) !=
         envelope.required_authority_steps.end();
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000023101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000023102";
  session.database_uuid = "019f0000-0000-7000-8000-000000023103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 23;
  session.security_policy_epoch = 24;
  session.descriptor_epoch = 25;
  return session;
}

ParserConfig ParserConfigForAuthorityRefusal() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000023104";
  config.bundle_contract_id = "sbp_sbsql@create-schema-route-test";
  config.build_id = "sbsql-create-schema-route-test";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunParserOnlyPipeline() {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(kSql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst,
                            ParserConfigForAuthorityRefusal(), session);
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireRegistryEvidence() {
  for (const auto& row : kCreateSchemaRows) {
    const auto* registry_row =
        FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr,
            "CREATE SCHEMA generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "CREATE SCHEMA generated registry canonical name drifted");
    Require(registry_row->surface_kind == "grammar_production" &&
                registry_row->family == "ddl_catalog" &&
                registry_row->source_status == "native_now" &&
                registry_row->cluster_scope ==
                    "noncluster_or_profile_scoped",
            "CREATE SCHEMA generated registry classification drifted");
    Require(registry_row->sblr_operation_family == kFamily &&
                registry_row->parser_handler_key ==
                    "parser.statement_family.ddl_catalog" &&
                registry_row->lowering_handler_key ==
                    "lowering.sblr_family.sblr_catalog_mutation_v3" &&
                registry_row->server_admission_key ==
                    "server.admission.sblr_catalog_mutation_v3" &&
                registry_row->engine_rule_key ==
                    "engine.rule.sblr_catalog_mutation_v3" &&
                registry_row->validation_fixture_id ==
                    row.validation_fixture_id,
            "CREATE SCHEMA generated registry route metadata drifted");
  }
}

void RequireParserOnlyAuthorityRefusal(const PipelineArtifacts& artifacts) {
  Require(!artifacts.cst.messages.has_errors(), "CREATE SCHEMA CST failed");
  Require(!artifacts.ast.messages.has_errors(), "CREATE SCHEMA AST failed");
  Require(artifacts.bound.bound, "CREATE SCHEMA parser bind failed");
  Require(!artifacts.verifier.admitted,
          "CREATE SCHEMA was admitted without an engine-issued CSDO");
  Require(artifacts.envelope.operation_family == kFamily &&
              artifacts.envelope.sblr_operation_key == kFamily,
          "CREATE SCHEMA parser-only family drifted");
  Require(artifacts.envelope.operation_id ==
                  "engine.op.diagnostic_refusal" &&
              artifacts.envelope.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
              artifacts.envelope.engine_api_operation_id == "not_admitted" &&
              artifacts.envelope.result_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.diagnostic_shape_key ==
                  "diagnostic_vector.v1" &&
              artifacts.envelope.resource_contract_key ==
                  "sbsql.command.no_execution.v1",
          "CREATE SCHEMA parser-only refusal tuple drifted");
  Require(artifacts.envelope.payload.empty() &&
              artifacts.envelope.operands.empty() &&
              artifacts.envelope.resolved_object_uuids.empty() &&
              !artifacts.envelope.parser_executes_sql &&
              !artifacts.envelope.real_file_effects,
          "CREATE SCHEMA parser-only refusal leaked authority");
  Require(HasAuthorityStep(artifacts.envelope,
                           "authority.parser.syntax_evidence_only") &&
              HasAuthorityStep(artifacts.envelope,
                               "authority.parser.no_executable_sblr") &&
              HasAuthorityStep(artifacts.envelope,
                               "authority.parser.no_sql_text_execution") &&
              HasAuthorityStep(artifacts.envelope,
                               "authority.parser.no_storage_or_finality"),
          "CREATE SCHEMA parser-only refusal omitted non-authority evidence");
  Require(artifacts.envelope.messages.diagnostics.size() == 1,
          "CREATE SCHEMA parser-only refusal diagnostic count drifted");
  const auto& diagnostic = artifacts.envelope.messages.diagnostics.front();
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
              diagnostic.severity == "ERROR" &&
              DiagnosticField(diagnostic,
                              "canonical_parent_operation_id") ==
                  kOperationId &&
              DiagnosticField(diagnostic,
                              "canonical_parent_sblr_opcode") == kOpcode &&
              DiagnosticField(diagnostic, "recognized_surface_ids") ==
                  "SBSQL-DE4B8AAF6326,SBSQL-7BA0B928798B" &&
              DiagnosticField(diagnostic, "executable_sblr_emitted") ==
                  "false",
          "CREATE SCHEMA parser-only refusal identity drifted");
}

void RequireOpcodeRegistryContract() {
  const auto* entry = sblr::LookupSblrOperation(kOperationId);
  Require(entry != nullptr, "CREATE SCHEMA opcode registry row missing");
  Require(entry->opcode == kOpcode && entry->code == 1536 &&
              entry->operand_contract == "create_schema_descriptor" &&
              entry->result_contract == "ddl_result" &&
              entry->executor_id == kOperationId &&
              entry->requires_security_context &&
              entry->requires_transaction_context,
          "CREATE SCHEMA exact opcode tuple drifted");
}

}  // namespace

int main() {
  try {
    ConfigureMemoryFixture();
    RequireRegistryEvidence();
    RequireParserOnlyAuthorityRefusal(RunParserOnlyPipeline());
    RequireOpcodeRegistryContract();
    std::cout << "sbsql_create_schema_exact_route_conformance=passed "
                 "parser_authority_refusal=true\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
