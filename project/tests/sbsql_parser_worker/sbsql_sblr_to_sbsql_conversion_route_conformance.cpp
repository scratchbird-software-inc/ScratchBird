// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_to_sbsql.hpp"
#include "sbu_sbsql_parser_support.hpp"

#include <algorithm>
#include <cstdint>
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

SblrOperationEnvelope CanonicalOperation(SblrOperationEnvelope envelope) {
  const auto* entry =
      scratchbird::engine::sblr::LookupSblrOperation(envelope.operation_id);
  Require(entry != nullptr && entry->opcode == envelope.opcode && entry->code != 0,
          "renderer fixture operation lacks an exact canonical registry identity");
  envelope.opcode_code = entry->code;
  envelope.parser_package_uuid = "019dffbb-f000-7000-8000-000000000501";
  envelope.registry_snapshot_uuid = "019dffbb-f000-7000-8000-000000000502";
  envelope.parser_resolved_names_to_uuids = true;
  for (std::size_t index = 0; index < envelope.operands.size(); ++index) {
    auto& operand = envelope.operands[index];
    operand.ordinal = static_cast<std::uint32_t>(index + 1);
    if (operand.value_kind !=
            scratchbird::engine::sblr::SblrValueKind::null_value ||
        !operand.value_body.empty()) {
      continue;
    }
    operand.value_kind =
        scratchbird::engine::sblr::SblrValueKind::literal_typed;
    operand.value_body.assign(16, 0);
    operand.value_body[0] = 0x12;
    operand.value_body[6] = 0x70;
    operand.value_body[8] = 0x80;
    operand.value_body[15] = 0x51;
    const auto value_size = static_cast<std::uint64_t>(operand.value.size());
    for (std::uint32_t shift = 0; shift < 64; shift += 8) {
      operand.value_body.push_back(
          static_cast<std::uint8_t>((value_size >> shift) & 0xffu));
    }
    operand.value_body.insert(operand.value_body.end(), operand.value.begin(),
                              operand.value.end());
    operand.value.clear();
  }
  return envelope;
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
  auto envelope = MakeSblrEnvelope("engine.op.insert",
                                   "SBLR_INSERT",
                                   "CBQ-021-SBLR-TO-SBSQL-CONVERSION");
  envelope.requires_transaction_context = true;
  envelope.operands.push_back(Operand("sbsql_render_family",
                                      "source_preserving_dml_single_row_v1"));
  envelope.operands.push_back(Operand("authority_descriptor_uuid",
                                      "019dffbb-f000-7000-8000-000000000102"));
  envelope.operands.push_back(Operand("target_object_uuid",
                                      std::string(kRelationUuid)));
  envelope.operands.push_back(
      Operand("value_column_symbol_key", "alias.column.customer_id"));
  envelope.operands.push_back(
      Operand("value_column_uuid", std::string(kQueryAliasUuid)));
  envelope.operands.push_back(
      Operand("value_parameter_symbol_key", "param.p_customer_id"));
  envelope.operands.push_back(
      Operand("value_parameter_uuid", std::string(kQueryParameterUuid)));

  AttachSourcePolicy(&envelope, "CBQ-021-source-preserving-route",
                     "sha256:cbq021-source-map");
  envelope.source_artifact_map.symbols.push_back(
      Symbol("parameter", "param.p_customer_id",
             std::string(kQueryParameterUuid), ":p_customer_id",
             "dml.parameter"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("column_alias", "alias.column.customer_id",
             std::string(kQueryAliasUuid), "customer_id", "dml.value"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("object_display_name", "object.customer",
             std::string(kRelationUuid), "customer", "dml.target"));
  return CanonicalOperation(std::move(envelope));
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
  return CanonicalOperation(std::move(envelope));
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
  return CanonicalOperation(std::move(envelope));
}

void EraseOperand(SblrOperationEnvelope* envelope, std::string_view name) {
  envelope->operands.erase(
      std::remove_if(envelope->operands.begin(),
                     envelope->operands.end(),
                     [&](const SblrOperand& operand) {
                       return operand.name == name;
                     }),
      envelope->operands.end());
  for (std::size_t index = 0; index < envelope->operands.size(); ++index) {
    envelope->operands[index].ordinal = static_cast<std::uint32_t>(index + 1);
  }
}

void SetOperandText(SblrOperationEnvelope* envelope,
                    std::string_view name,
                    std::string_view value) {
  for (auto& operand : envelope->operands) {
    if (operand.name != name) continue;
    Require(operand.value_kind ==
                    scratchbird::engine::sblr::SblrValueKind::literal_typed &&
                operand.value_body.size() >= 24,
            "renderer fixture typed operand is malformed");
    operand.value.clear();
    operand.value_body.resize(16);
    const auto value_size = static_cast<std::uint64_t>(value.size());
    for (std::uint32_t shift = 0; shift < 64; shift += 8) {
      operand.value_body.push_back(
          static_cast<std::uint8_t>((value_size >> shift) & 0xffu));
    }
    operand.value_body.insert(operand.value_body.end(), value.begin(),
                              value.end());
    return;
  }
  Require(false, "renderer fixture operand to mutate is missing");
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

std::string EncodeOperationWithoutSourceArtifacts(
    const SblrOperationEnvelope& envelope) {
  auto operation = envelope;
  operation.source_artifact_map = {};
  const auto encoded = EncodeSblrEnvelope(operation);
  Require(!encoded.empty(),
          "canonical operation without source artifacts did not encode");
  return encoded;
}

std::string CheckRouteThroughApiAndParser(const SblrOperationEnvelope& envelope,
                                          std::string_view expected_fragment,
                                          std::size_t expected_statement_count,
                                          std::string_view label) {
  const SblrToSbsqlOptions options{.source_preserving = true};
  const auto api_result = RenderSblrEnvelopeToSbsql(envelope, options);
  Require(api_result.ok, std::string(label) + " API route rejected valid envelope");
  Require(Contains(api_result.sbsql_text, expected_fragment),
          std::string(label) + " API route did not render expected SBsql");

  Require(EncodeSblrEnvelope(envelope).empty(),
          std::string(label) + " embedded source artifacts entered SBOP");
  ExpectRefusal(sbu_sbsql_decompile_sblr(
                    EncodeOperationWithoutSourceArtifacts(envelope),
                    kSourcePreservingPolicy),
                "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REQUIRED",
                std::string(label) + " bare-SBOP reversal");

  const auto statements = RenderedStatements(api_result.sbsql_text);
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
  Require(api_result.sbsql_text ==
              "INSERT INTO customer (customer_id) VALUES (:p_customer_id);",
          "rendered SBsql did not preserve canonical DML symbols");

  Require(EncodeSblrEnvelope(envelope).empty(),
          "source artifacts were serialized inside SBOP");
  const auto encoded = EncodeOperationWithoutSourceArtifacts(envelope);
  ExpectRefusal(sbu_sbsql_decompile_sblr(encoded, "normal"),
                "SBU_SBSQL.DECOMPILE_POLICY_REFUSED",
                "normal decompile policy");

  const auto debug = sbu_sbsql_decompile_sblr("not-an-envelope", "allow_debug_artifacts");
  Require(debug.ok, "legacy debug decompile path should remain available");
  Require(debug.payload == "<sblr-debug-text-redacted>",
          "legacy debug decompile path should remain redacted for non-source-preserving packets");

  ExpectRefusal(sbu_sbsql_decompile_sblr(encoded, kSourcePreservingPolicy),
                "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REQUIRED",
                "bare-SBOP source-preserving decompile");

  const auto statements = RenderedStatements(api_result.sbsql_text);
  Require(statements.size() == 1, "rendered SBsql statement count mismatch");
  for (const auto& statement : statements) {
    const auto syntax = sbu_sbsql_validate_syntax(statement, "sbsql");
    Require(syntax.ok, "rendered SBsql statement did not validate through parser path");
  }
}

void CheckCoreOperationFamilyRoutes() {
  const auto query_rendered =
      CheckRouteThroughApiAndParser(BuildQueryProjectionEnvelope(),
                                    "SELECT :p_limit AS limit_value;",
                                    1,
                                    "query projection");
  Require(!Contains(query_rendered, "projection_descriptor_uuid"),
          "query projection rendered authority metadata as SBsql text");

  const auto catalog_rendered =
      CheckRouteThroughApiAndParser(BuildCatalogDescriptorEnvelope(),
                                    "SHOW CREATE TABLE replay_target;",
                                    1,
                                    "catalog descriptor");
  Require(!Contains(catalog_rendered, std::string(kCatalogObjectUuid)),
          "catalog descriptor rendered UUID authority as SBsql text");

}

void CheckDeterministicRefusals() {
  auto absent = BuildConvertibleEnvelope();
  absent.source_artifact_map = {};
  ExpectRefusal(sbu_sbsql_decompile_sblr(EncodeSblrEnvelope(absent),
                                         kSourcePreservingPolicy),
                "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REQUIRED",
                "absent source artifacts");

  auto redacted = BuildConvertibleEnvelope();
  redacted.source_artifact_map.policy_status = "redacted_render_metadata";
  ExpectApiRefusal(redacted,
                   "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REDACTED",
                   "redacted source artifacts");

  auto invalid = BuildConvertibleEnvelope();
  invalid.source_artifact_map.contains_sql_text = true;
  ExpectApiRefusal(invalid,
                   "SB_SBLR_SOURCE_ARTIFACT_SQL_TEXT_FORBIDDEN",
                   "invalid source artifacts");

  auto authoritative = BuildConvertibleEnvelope();
  authoritative.source_artifact_map.raw_sql_text_authoritative = true;
  ExpectApiRefusal(authoritative,
                   "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_POLICY_UNSUPPORTED",
                   "authoritative source artifacts");

  auto missing_authority = BuildConvertibleEnvelope();
  EraseOperand(&missing_authority, "target_object_uuid");
  ExpectApiRefusal(missing_authority,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
                   "missing UUID authority operand");

  auto mismatched_authority = BuildConvertibleEnvelope();
  SetOperandText(&mismatched_authority, "target_object_uuid",
                 "019dffbb-f000-7000-8000-000000000199");
  ExpectApiRefusal(mismatched_authority,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
                   "mismatched UUID authority operand");

  auto query_missing_descriptor = BuildQueryProjectionEnvelope();
  EraseOperand(&query_missing_descriptor, "projection_descriptor_uuid");
  ExpectApiRefusal(query_missing_descriptor,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
                   "query missing projection descriptor authority");

  auto query_mismatched_parameter = BuildQueryProjectionEnvelope();
  SetOperandText(&query_mismatched_parameter, "parameter_slot_uuid",
                 "019dffbb-f000-7000-8000-000000000299");
  ExpectApiRefusal(query_mismatched_parameter,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
                   "query mismatched parameter UUID authority");

  auto catalog_missing_object = BuildCatalogDescriptorEnvelope();
  EraseOperand(&catalog_missing_object, "target_object_uuid");
  ExpectApiRefusal(catalog_missing_object,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
                   "catalog missing target object UUID authority");

  auto catalog_mismatched_object = BuildCatalogDescriptorEnvelope();
  SetOperandText(&catalog_mismatched_object, "target_object_uuid",
                 "019dffbb-f000-7000-8000-000000000399");
  ExpectApiRefusal(catalog_mismatched_object,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
                   "catalog mismatched target UUID authority");

  auto unsupported = BuildQueryProjectionEnvelope();
  unsupported.operation_id = "query.unsupported_projection";
  unsupported.opcode = "SBLR_QUERY_UNSUPPORTED_PROJECTION";
  ExpectApiRefusal(unsupported,
                   "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH",
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
