// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_engine_envelope.hpp"
#include "sblr_to_sbsql.hpp"
#include "sbu_sbsql_parser_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::engine::sblr::EncodeSblrEnvelope;
using scratchbird::engine::sblr::MakeSblrEnvelope;
using scratchbird::engine::sblr::RenderSblrEnvelopeToSbsql;
using scratchbird::engine::sblr::SblrOperationEnvelope;
using scratchbird::engine::sblr::SblrOperand;
using scratchbird::engine::sblr::SblrSourceSymbolArtifact;
using scratchbird::engine::sblr::SblrToSbsqlOptions;
using scratchbird::udr::sbsql_parser_support::sbu_sbsql_decompile_sblr;
using scratchbird::udr::sbsql_parser_support::sbu_sbsql_validate_syntax;

constexpr std::string_view kSourcePreservingPolicy =
    "allow_debug_artifacts=true;decompile_policy=source_preserving";
constexpr std::string_view kRelationUuid =
    "019dffbb-f000-7000-8000-000000000101";
constexpr std::string_view kQueryParameterUuid =
    "019dffbb-f000-7000-8000-000000000201";
constexpr std::string_view kQueryAliasUuid =
    "019dffbb-f000-7000-8000-000000000202";
constexpr std::string_view kCatalogObjectUuid =
    "019dffbb-f000-7000-8000-000000000301";
constexpr std::string_view kTransactionContextUuid =
    "019dffbb-f000-7000-8000-000000000401";
constexpr std::string_view kSavepointUuid =
    "019dffbb-f000-7000-8000-000000000402";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "require_failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

SblrOperand Operand(std::string name, std::string value) {
  SblrOperand operand;
  operand.type = "text";
  operand.name = std::move(name);
  operand.value = std::move(value);
  return operand;
}

SblrSourceSymbolArtifact Symbol(std::string symbol_kind,
                                std::string stable_key,
                                std::string resolved_uuid,
                                std::string render_hint,
                                std::string scope) {
  SblrSourceSymbolArtifact symbol;
  symbol.symbol_kind = std::move(symbol_kind);
  symbol.stable_key = std::move(stable_key);
  symbol.resolved_uuid = std::move(resolved_uuid);
  symbol.render_hint = std::move(render_hint);
  symbol.scope = std::move(scope);
  symbol.source_hash = "sha256:phase1c-source-symbol";
  return symbol;
}

void AttachSourcePolicy(SblrOperationEnvelope* envelope,
                        std::string_view identity,
                        std::string_view hash) {
  envelope->source_artifact_map.policy_status = "non_authoritative_render_metadata";
  envelope->source_artifact_map.source_identity = std::string(identity);
  envelope->source_artifact_map.source_hash = std::string(hash);
  envelope->source_artifact_map.render_metadata_only = true;
  envelope->source_artifact_map.contains_sql_text = false;
  envelope->source_artifact_map.raw_sql_text_authoritative = false;
}

SblrOperationEnvelope BuildConvertibleEnvelope() {
  auto envelope = MakeSblrEnvelope("general.procedural_operation",
                                   "SBLR_GENERAL_PROCEDURAL_OPERATION",
                                   "CBQ-021-SBLR-TO-SBSQL-CONVERSION");
  envelope.requires_transaction_context = true;
  envelope.operands.push_back(Operand("sbsql_render_family",
                                      "source_preserving_procedural_bundle_v1"));
  envelope.operands.push_back(Operand("authority_descriptor_uuid",
                                      "019dffbb-f000-7000-8000-000000000102"));
  envelope.operands.push_back(Operand("relation_object_uuid",
                                      std::string(kRelationUuid)));
  envelope.operands.push_back(Operand("variable_type", "INT"));

  AttachSourcePolicy(&envelope, "CBQ-021-source-preserving-route",
                     "sha256:cbq021-source-map");
  envelope.source_artifact_map.symbols.push_back(
      Symbol("variable", "var.v_running_total", "", "v_running_total", "procedure.local"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("parameter", "param.p_customer_id", "", ":p_customer_id", "routine.input"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("cursor", "cursor.customer_scan", "", "customer_scan", "procedure.cursor"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("label", "label.retry_block", "", "retry_block", "procedure.label"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("exception_handler", "handler.not_found", "", "not_found", "procedure.handler"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("relation_alias", "alias.customer.c", "", "c", "query.range"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("column_alias", "alias.column.customer_id", "", "customer_id", "query.projection"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("object_display_name", "object.customer", std::string(kRelationUuid), "customer", "query.from"));
  return envelope;
}

SblrOperationEnvelope BuildQueryProjectionEnvelope() {
  auto envelope = MakeSblrEnvelope("query.evaluate_projection",
                                   "SBLR_QUERY_EVALUATE_PROJECTION",
                                   "CBQ-021-query-projection-route");
  envelope.requires_transaction_context = true;
  envelope.operands.push_back(Operand("sbsql_render_family",
                                      "source_preserving_query_projection_v1"));
  envelope.operands.push_back(Operand("authority_descriptor_uuid",
                                      "019dffbb-f000-7000-8000-000000000203"));
  envelope.operands.push_back(Operand("projection_descriptor_uuid",
                                      "019dffbb-f000-7000-8000-000000000204"));
  envelope.operands.push_back(Operand("parameter_slot_uuid",
                                      std::string(kQueryParameterUuid)));
  envelope.operands.push_back(Operand("projection_alias_uuid",
                                      std::string(kQueryAliasUuid)));
  envelope.operands.push_back(Operand("projection_expr_kind",
                                      "parameter_reference"));
  AttachSourcePolicy(&envelope, "CBQ-021-query-projection-source-map",
                     "sha256:cbq021-query-projection");
  envelope.source_artifact_map.symbols.push_back(
      Symbol("parameter", "param.p_limit", std::string(kQueryParameterUuid),
             ":p_limit", "query.parameter"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("column_alias", "alias.column.limit_value", std::string(kQueryAliasUuid),
             "limit_value", "query.projection"));
  return envelope;
}

SblrOperationEnvelope BuildCatalogDescriptorEnvelope() {
  auto envelope = MakeSblrEnvelope("catalog.get_descriptor",
                                   "SBLR_CATALOG_GET_DESCRIPTOR",
                                   "CBQ-021-catalog-descriptor-route");
  envelope.requires_transaction_context = false;
  envelope.operands.push_back(Operand("sbsql_render_family",
                                      "source_preserving_catalog_descriptor_v1"));
  envelope.operands.push_back(Operand("authority_descriptor_uuid",
                                      "019dffbb-f000-7000-8000-000000000302"));
  envelope.operands.push_back(Operand("target_object_uuid",
                                      std::string(kCatalogObjectUuid)));
  envelope.operands.push_back(Operand("target_object_kind", "TABLE"));
  AttachSourcePolicy(&envelope, "CBQ-021-catalog-descriptor-source-map",
                     "sha256:cbq021-catalog-descriptor");
  envelope.source_artifact_map.symbols.push_back(
      Symbol("object_display_name", "object.replay_target",
             std::string(kCatalogObjectUuid), "replay_target", "catalog.target"));
  return envelope;
}

SblrOperationEnvelope BuildSavepointEnvelope(std::string operation_id,
                                             std::string opcode) {
  auto envelope = MakeSblrEnvelope(std::move(operation_id),
                                   std::move(opcode),
                                   "CBQ-021-transaction-savepoint-route");
  envelope.requires_transaction_context = true;
  envelope.operands.push_back(Operand("sbsql_render_family",
                                      "source_preserving_transaction_control_v1"));
  envelope.operands.push_back(Operand("transaction_context_uuid",
                                      std::string(kTransactionContextUuid)));
  envelope.operands.push_back(Operand("savepoint_authority_uuid",
                                      std::string(kSavepointUuid)));
  AttachSourcePolicy(&envelope, "CBQ-021-transaction-savepoint-source-map",
                     "sha256:cbq021-transaction-savepoint");
  envelope.source_artifact_map.symbols.push_back(
      Symbol("label", "savepoint.batch_guard", std::string(kSavepointUuid),
             "batch_guard", "transaction.savepoint"));
  return envelope;
}

void EraseOperand(SblrOperationEnvelope* envelope, std::string_view name) {
  envelope->operands.erase(
      std::remove_if(envelope->operands.begin(),
                     envelope->operands.end(),
                     [&](const SblrOperand& operand) {
                       return operand.name == name;
                     }),
      envelope->operands.end());
}

std::vector<std::string> RenderedStatements(std::string_view rendered) {
  std::vector<std::string> statements;
  std::istringstream input{std::string(rendered)};
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) statements.push_back(line);
  }
  return statements;
}

void ExpectRefusal(const scratchbird::udr::sbsql_parser_support::UdrResult& result,
                   std::string_view code,
                   std::string_view label) {
  Require(!result.ok, std::string(label) + " unexpectedly succeeded");
  Require(Contains(result.message_vector_json, code),
          std::string(label) + " did not return deterministic diagnostic " +
              std::string(code));
}

void ExpectApiRefusal(const SblrOperationEnvelope& envelope,
                      std::string_view code,
                      std::string_view label) {
  const SblrToSbsqlOptions options{.source_preserving = true};
  const auto result = RenderSblrEnvelopeToSbsql(envelope, options);
  Require(!result.ok, std::string(label) + " unexpectedly succeeded");
  Require(!result.diagnostics.empty(), std::string(label) + " did not return diagnostics");
  Require(result.diagnostics.front().code == code,
          std::string(label) + " did not return exact diagnostic " + std::string(code));
}

std::string CheckRouteThroughApiParserAndUdr(const SblrOperationEnvelope& envelope,
                                             std::string_view expected_fragment,
                                             std::size_t expected_statement_count,
                                             std::string_view label) {
  const SblrToSbsqlOptions options{.source_preserving = true};
  const auto api_result = RenderSblrEnvelopeToSbsql(envelope, options);
  Require(api_result.ok, std::string(label) + " API route rejected valid envelope");
  Require(Contains(api_result.sbsql_text, expected_fragment),
          std::string(label) + " API route did not render expected SBsql");

  const auto udr_result =
      sbu_sbsql_decompile_sblr(EncodeSblrEnvelope(envelope), kSourcePreservingPolicy);
  Require(udr_result.ok, std::string(label) + " UDR route rejected valid envelope");
  Require(udr_result.payload == api_result.sbsql_text,
          std::string(label) + " UDR route drifted from engine API render");

  const auto statements = RenderedStatements(udr_result.payload);
  Require(statements.size() == expected_statement_count,
          std::string(label) + " rendered SBsql statement count mismatch");
  for (const auto& statement : statements) {
    const auto syntax = sbu_sbsql_validate_syntax(statement, "sbsql");
    Require(syntax.ok,
            std::string(label) + " rendered SBsql statement did not validate through parser path: " +
                statement);
  }
  return api_result.sbsql_text;
}

void CheckValidSourcePreservingRoute() {
  const auto envelope = BuildConvertibleEnvelope();
  const SblrToSbsqlOptions options{.source_preserving = true};
  const auto api_result = RenderSblrEnvelopeToSbsql(envelope, options);
  Require(api_result.ok, "engine SBLR-to-SBsql API rejected valid envelope");
  Require(Contains(api_result.sbsql_text, "DECLARE VARIABLE v_running_total INT;"),
          "rendered SBsql did not preserve variable name");
  Require(Contains(api_result.sbsql_text, "PARAM LIST p_customer_id;"),
          "rendered SBsql did not preserve parameter name");
  Require(Contains(api_result.sbsql_text, "DECLARE customer_scan CURSOR;"),
          "rendered SBsql did not preserve cursor name");
  Require(Contains(api_result.sbsql_text, "PSQL LEAVE retry_block;"),
          "rendered SBsql did not preserve label name");
  Require(Contains(api_result.sbsql_text, "EXCEPTION HANDLER WHEN not_found;"),
          "rendered SBsql did not preserve exception handler name");
  Require(Contains(api_result.sbsql_text, "FROM customer AS c"),
          "rendered SBsql did not preserve relation alias");
  Require(Contains(api_result.sbsql_text, "AS customer_id"),
          "rendered SBsql did not preserve column alias");

  const auto encoded = EncodeSblrEnvelope(envelope);
  ExpectRefusal(sbu_sbsql_decompile_sblr(encoded, "normal"),
                "SBU_SBSQL.DECOMPILE_POLICY_REFUSED",
                "normal decompile policy");

  const auto debug = sbu_sbsql_decompile_sblr("not-an-envelope", "allow_debug_artifacts");
  Require(debug.ok, "legacy debug decompile path should remain available");
  Require(debug.payload == "<sblr-debug-text-redacted>",
          "legacy debug decompile path should remain redacted for non-source-preserving packets");

  const auto udr_result = sbu_sbsql_decompile_sblr(encoded, kSourcePreservingPolicy);
  Require(udr_result.ok, "UDR source-preserving decompile rejected valid envelope");
  Require(udr_result.payload == api_result.sbsql_text,
          "UDR source-preserving decompile drifted from engine API render");
  Require(!Contains(udr_result.payload, "<sblr-debug-text-redacted>"),
          "source-preserving policy returned redacted debug placeholder");

  const auto statements = RenderedStatements(udr_result.payload);
  Require(statements.size() == 6, "rendered SBsql statement count mismatch");
  for (const auto& statement : statements) {
    const auto syntax = sbu_sbsql_validate_syntax(statement, "sbsql");
    Require(syntax.ok, "rendered SBsql statement did not validate through parser path");
  }
}

void CheckCoreOperationFamilyRoutes() {
  const auto query_rendered =
      CheckRouteThroughApiParserAndUdr(BuildQueryProjectionEnvelope(),
                                       "SELECT :p_limit AS limit_value;",
                                       1,
                                       "query projection");
  Require(!Contains(query_rendered, "projection_descriptor_uuid"),
          "query projection rendered authority metadata as SBsql text");

  const auto catalog_rendered =
      CheckRouteThroughApiParserAndUdr(BuildCatalogDescriptorEnvelope(),
                                       "SHOW CREATE TABLE replay_target;",
                                       1,
                                       "catalog descriptor");
  Require(!Contains(catalog_rendered, std::string(kCatalogObjectUuid)),
          "catalog descriptor rendered UUID authority as SBsql text");

  CheckRouteThroughApiParserAndUdr(
      BuildSavepointEnvelope("transaction.create_savepoint",
                             "SBLR_TRANSACTION_CREATE_SAVEPOINT"),
      "SAVEPOINT batch_guard;",
      1,
      "transaction create savepoint");
  CheckRouteThroughApiParserAndUdr(
      BuildSavepointEnvelope("transaction.release_savepoint",
                             "SBLR_TRANSACTION_RELEASE_SAVEPOINT"),
      "RELEASE SAVEPOINT batch_guard;",
      1,
      "transaction release savepoint");
  CheckRouteThroughApiParserAndUdr(
      BuildSavepointEnvelope("transaction.rollback_to_savepoint",
                             "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT"),
      "ROLLBACK TO SAVEPOINT batch_guard;",
      1,
      "transaction rollback-to-savepoint");
}

void CheckDeterministicRefusals() {
  auto absent = BuildConvertibleEnvelope();
  absent.source_artifact_map = {};
  ExpectRefusal(sbu_sbsql_decompile_sblr(EncodeSblrEnvelope(absent), kSourcePreservingPolicy),
                "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REQUIRED",
                "absent source artifacts");

  auto redacted = BuildConvertibleEnvelope();
  redacted.source_artifact_map.policy_status = "redacted_render_metadata";
  ExpectRefusal(sbu_sbsql_decompile_sblr(EncodeSblrEnvelope(redacted), kSourcePreservingPolicy),
                "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REDACTED",
                "redacted source artifacts");

  auto invalid = BuildConvertibleEnvelope();
  invalid.source_artifact_map.contains_sql_text = true;
  ExpectRefusal(sbu_sbsql_decompile_sblr(EncodeSblrEnvelope(invalid), kSourcePreservingPolicy),
                "SB_SBLR_SOURCE_ARTIFACT_SQL_TEXT_FORBIDDEN",
                "invalid source artifacts");

  auto missing_authority = BuildConvertibleEnvelope();
  EraseOperand(&missing_authority, "relation_object_uuid");
  ExpectRefusal(sbu_sbsql_decompile_sblr(EncodeSblrEnvelope(missing_authority),
                                         kSourcePreservingPolicy),
                "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
                "missing UUID authority operand");

  auto mismatched_authority = BuildConvertibleEnvelope();
  for (auto& operand : mismatched_authority.operands) {
    if (operand.name == "relation_object_uuid") {
      operand.value = "019dffbb-f000-7000-8000-000000000199";
    }
  }
  ExpectRefusal(sbu_sbsql_decompile_sblr(EncodeSblrEnvelope(mismatched_authority),
                                         kSourcePreservingPolicy),
                "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
                "mismatched UUID authority operand");

  auto query_missing_descriptor = BuildQueryProjectionEnvelope();
  EraseOperand(&query_missing_descriptor, "projection_descriptor_uuid");
  ExpectApiRefusal(query_missing_descriptor,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
                   "query missing projection descriptor authority");

  auto query_mismatched_parameter = BuildQueryProjectionEnvelope();
  for (auto& operand : query_mismatched_parameter.operands) {
    if (operand.name == "parameter_slot_uuid") {
      operand.value = "019dffbb-f000-7000-8000-000000000299";
    }
  }
  ExpectApiRefusal(query_mismatched_parameter,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
                   "query mismatched parameter UUID authority");

  auto catalog_missing_object = BuildCatalogDescriptorEnvelope();
  EraseOperand(&catalog_missing_object, "target_object_uuid");
  ExpectApiRefusal(catalog_missing_object,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
                   "catalog missing target object UUID authority");

  auto catalog_mismatched_object = BuildCatalogDescriptorEnvelope();
  for (auto& operand : catalog_mismatched_object.operands) {
    if (operand.name == "target_object_uuid") {
      operand.value = "019dffbb-f000-7000-8000-000000000399";
    }
  }
  ExpectApiRefusal(catalog_mismatched_object,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
                   "catalog mismatched target UUID authority");

  auto transaction_missing_context =
      BuildSavepointEnvelope("transaction.create_savepoint",
                             "SBLR_TRANSACTION_CREATE_SAVEPOINT");
  EraseOperand(&transaction_missing_context, "transaction_context_uuid");
  ExpectApiRefusal(transaction_missing_context,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
                   "transaction missing context authority");

  auto transaction_mismatched_savepoint =
      BuildSavepointEnvelope("transaction.create_savepoint",
                             "SBLR_TRANSACTION_CREATE_SAVEPOINT");
  for (auto& operand : transaction_mismatched_savepoint.operands) {
    if (operand.name == "savepoint_authority_uuid") {
      operand.value = "019dffbb-f000-7000-8000-000000000499";
    }
  }
  ExpectApiRefusal(transaction_mismatched_savepoint,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
                   "transaction mismatched savepoint authority");

  auto unsupported = BuildQueryProjectionEnvelope();
  unsupported.operation_id = "query.unsupported_projection";
  unsupported.opcode = "SBLR_QUERY_UNSUPPORTED_PROJECTION";
  ExpectApiRefusal(unsupported,
                   "SB_SBLR_TO_SBSQL_UNSUPPORTED_OPERATION",
                   "unsupported operation family");
}

}  // namespace

int main() {
  CheckValidSourcePreservingRoute();
  CheckCoreOperationFamilyRoutes();
  CheckDeterministicRefusals();
  std::cout << "sbsql_sblr_to_sbsql_conversion_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
