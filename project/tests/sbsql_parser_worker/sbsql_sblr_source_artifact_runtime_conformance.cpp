// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_engine_envelope.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using scratchbird::engine::sblr::DecodeSblrEnvelope;
using scratchbird::engine::sblr::EncodeSblrEnvelope;
using scratchbird::engine::sblr::MakeSblrEnvelope;
using scratchbird::engine::sblr::SblrOperationEnvelope;
using scratchbird::engine::sblr::SblrOperationRenderHint;
using scratchbird::engine::sblr::SblrSourceSymbolArtifact;
using scratchbird::engine::sblr::SerializeSblrEnvelopeToJson;
using scratchbird::engine::sblr::ValidateSblrEnvelope;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "require_failed: " << message << '\n';
    std::exit(1);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const scratchbird::engine::sblr::SblrEnvelopeValidationResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
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
  symbol.source_hash = "sha256:source-map-segment";
  return symbol;
}

SblrOperationRenderHint RenderHint(std::string hint_kind,
                                   std::string stable_key,
                                   std::string value) {
  SblrOperationRenderHint hint;
  hint.hint_kind = std::move(hint_kind);
  hint.stable_key = std::move(stable_key);
  hint.value = std::move(value);
  return hint;
}

SblrOperationEnvelope BuildEnvelope() {
  auto envelope = MakeSblrEnvelope("query.plan_operation",
                                   "SBLR_QUERY_PLAN_OPERATION",
                                   "CBQ-002-SBLR-SOURCE-ARTIFACT-RUNTIME");
  envelope.source_artifact_map.policy_status = "non_authoritative_render_metadata";
  envelope.source_artifact_map.source_identity = "SBSQL-SOURCE-ARTIFACT-CBQ-002";
  envelope.source_artifact_map.source_hash = "sha256:cbq002-source-artifact-map";
  envelope.source_artifact_map.render_metadata_only = true;
  envelope.source_artifact_map.contains_sql_text = false;
  envelope.source_artifact_map.raw_sql_text_authoritative = false;
  envelope.source_artifact_map.symbols.push_back(
      Symbol("variable", "var.v_running_total", "", "v_running_total", "procedure.local"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("parameter", "param.p_customer_id", "019dffbb-f000-7000-8000-000000000002", ":p_customer_id", "predicate"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("cursor", "cursor.customer_scan", "019dffbb-f000-7000-8000-000000000003", "customer_scan", "procedure"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("label", "label.retry_block", "", "retry_block", "procedure"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("block_name", "block.retry", "", "retry", "procedure"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("routine", "routine.apply_credit", "019dffbb-f000-7000-8000-000000000004", "apply_credit", "routine"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("routine_argument", "routine.apply_credit.arg.amount", "", "amount", "routine"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("exception_handler", "handler.not_found", "", "not_found", "exception"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("cte", "cte.active_customers", "", "active_customers", "query"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("relation_alias", "alias.customer.c", "", "c", "range"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("column_alias", "alias.column.customer_id", "", "customer_id", "projection"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("object_display_name", "object.customer", "019dffbb-f000-7000-8000-000000000001", "customer", "from"));
  envelope.source_artifact_map.symbols.push_back(
      Symbol("generated_temp", "generated.local.1", "", "__sb_local_1", "generated"));
  envelope.source_artifact_map.operation_render_hints.push_back(
      RenderHint("operation", "query.plan_operation", "render_as_select_projection"));
  envelope.source_artifact_map.operation_render_hints.push_back(
      RenderHint("result_shape", "projection.customer_id", "render_column_alias_customer_id"));
  return envelope;
}

void CheckRoundTrip() {
  const auto envelope = BuildEnvelope();
  const auto validation = ValidateSblrEnvelope(envelope);
  Require(validation.ok, "non-authoritative source artifact metadata should validate");

  const auto encoded = EncodeSblrEnvelope(envelope);
  Require(Contains(encoded, "source_artifact_policy_status=non_authoritative_render_metadata"),
          "encoded envelope includes source artifact policy");
  Require(Contains(encoded, "source_symbol=variable"),
          "encoded envelope includes variable source symbol");
  Require(Contains(encoded, "source_symbol=relation_alias"),
          "encoded envelope includes relation alias source symbol");
  Require(Contains(encoded, "source_symbol=column_alias"),
          "encoded envelope includes column alias source symbol");
  Require(Contains(encoded, "source_symbol=exception_handler"),
          "encoded envelope includes exception handler source symbol");
  Require(Contains(encoded, "source_operation_render_hint=operation"),
          "encoded envelope includes operation render hint");

  const auto decoded = DecodeSblrEnvelope(encoded);
  Require(decoded.ok, "decoded envelope should validate");
  Require(decoded.envelope.source_artifact_map.policy_status ==
              "non_authoritative_render_metadata",
          "policy survives decode");
  Require(decoded.envelope.source_artifact_map.source_hash ==
              "sha256:cbq002-source-artifact-map",
          "source hash survives decode");
  Require(decoded.envelope.source_artifact_map.render_metadata_only,
          "render metadata marker survives decode");
  Require(!decoded.envelope.source_artifact_map.raw_sql_text_authoritative,
          "raw SQL authority stays forbidden after decode");
  Require(decoded.envelope.source_artifact_map.symbols.size() == 13,
          "all symbol artifacts survive decode");
  Require(decoded.envelope.source_artifact_map.operation_render_hints.size() == 2,
          "all operation render hints survive decode");
  constexpr std::array<std::string_view, 13> kExpectedSymbolKinds = {
      "variable",
      "parameter",
      "cursor",
      "label",
      "block_name",
      "routine",
      "routine_argument",
      "exception_handler",
      "cte",
      "relation_alias",
      "column_alias",
      "object_display_name",
      "generated_temp"};
  for (std::size_t i = 0; i < kExpectedSymbolKinds.size(); ++i) {
    Require(decoded.envelope.source_artifact_map.symbols[i].symbol_kind == kExpectedSymbolKinds[i],
            "appendix symbol kind survives decode");
  }

  const auto json = SerializeSblrEnvelopeToJson(decoded.envelope);
  Require(Contains(json, "\"source_artifact_map\""),
          "json includes source artifact map object");
  Require(Contains(json, "\"policy_status\": \"non_authoritative_render_metadata\""),
          "json includes policy status");
  Require(Contains(json, "\"source_hash\": \"sha256:cbq002-source-artifact-map\""),
          "json includes source hash");
  Require(Contains(json, "\"symbols\""),
          "json includes symbol artifact array");
  Require(Contains(json, "\"symbol_kind\": \"variable\""),
          "json includes variable source symbol");
  Require(Contains(json, "\"symbol_kind\": \"relation_alias\""),
          "json includes relation alias source symbol");
  Require(Contains(json, "\"symbol_kind\": \"column_alias\""),
          "json includes column alias source symbol");
  Require(Contains(json, "\"symbol_kind\": \"exception_handler\""),
          "json includes exception handler source symbol");
  Require(Contains(json, "\"symbol_kind\": \"generated_temp\""),
          "json includes generated local source symbol");
  Require(Contains(json, "\"operation_render_hints\""),
          "json includes operation render hint array");
  Require(Contains(json, "\"raw_sql_text_authoritative\": false"),
          "json records raw SQL authority is forbidden");
}

void CheckValidationRejectsSourceAuthorityMisuse() {
  auto sql_text = BuildEnvelope();
  sql_text.source_artifact_map.contains_sql_text = true;
  const auto sql_text_validation = ValidateSblrEnvelope(sql_text);
  Require(!sql_text_validation.ok, "source artifact SQL text marker is rejected");
  Require(HasDiagnostic(sql_text_validation, "SB_SBLR_SOURCE_ARTIFACT_SQL_TEXT_FORBIDDEN"),
          "source artifact SQL text diagnostic is deterministic");

  auto authoritative_source = BuildEnvelope();
  authoritative_source.source_artifact_map.raw_sql_text_authoritative = true;
  const auto authoritative_source_validation = ValidateSblrEnvelope(authoritative_source);
  Require(!authoritative_source_validation.ok, "authoritative source artifacts are rejected");
  Require(HasDiagnostic(authoritative_source_validation, "SB_SBLR_SOURCE_ARTIFACT_AUTHORITY_FORBIDDEN"),
          "source artifact authority diagnostic is deterministic");

  auto symbol_authority = BuildEnvelope();
  symbol_authority.source_artifact_map.symbols[0].authoritative = true;
  const auto symbol_authority_validation = ValidateSblrEnvelope(symbol_authority);
  Require(!symbol_authority_validation.ok, "authoritative symbol artifacts are rejected");
  Require(HasDiagnostic(symbol_authority_validation, "SB_SBLR_SOURCE_SYMBOL_AUTHORITY_FORBIDDEN"),
          "source symbol authority diagnostic is deterministic");

  auto hint_sql = BuildEnvelope();
  hint_sql.source_artifact_map.operation_render_hints[0].contains_sql_text = true;
  const auto hint_sql_validation = ValidateSblrEnvelope(hint_sql);
  Require(!hint_sql_validation.ok, "operation render hint SQL text is rejected");
  Require(HasDiagnostic(hint_sql_validation, "SB_SBLR_OPERATION_RENDER_HINT_SQL_TEXT_FORBIDDEN"),
          "operation render hint SQL text diagnostic is deterministic");

  auto invalid_symbol = BuildEnvelope();
  invalid_symbol.source_artifact_map.symbols[0].symbol_kind = "sql_text";
  const auto invalid_symbol_validation = ValidateSblrEnvelope(invalid_symbol);
  Require(!invalid_symbol_validation.ok, "invalid source symbol kind is rejected");
  Require(HasDiagnostic(invalid_symbol_validation, "SB_SBLR_SOURCE_SYMBOL_ARTIFACT_INVALID"),
          "invalid source symbol diagnostic is deterministic");

  auto missing_policy = BuildEnvelope();
  missing_policy.source_artifact_map.policy_status = "absent";
  const auto missing_policy_validation = ValidateSblrEnvelope(missing_policy);
  Require(!missing_policy_validation.ok, "source artifacts without policy are rejected");
  Require(HasDiagnostic(missing_policy_validation, "SB_SBLR_SOURCE_ARTIFACT_POLICY_REQUIRED"),
          "source artifact missing policy diagnostic is deterministic");

  auto unresolved_names = BuildEnvelope();
  unresolved_names.parser_resolved_names_to_uuids = false;
  const auto unresolved_names_validation = ValidateSblrEnvelope(unresolved_names);
  Require(!unresolved_names_validation.ok, "unresolved parser names remain rejected");
  Require(HasDiagnostic(unresolved_names_validation, "SB_SBLR_NAMES_NOT_RESOLVED_TO_UUIDS"),
          "name-to-UUID diagnostic is preserved");

  auto envelope_sql = BuildEnvelope();
  envelope_sql.contains_sql_text = true;
  const auto envelope_sql_validation = ValidateSblrEnvelope(envelope_sql);
  Require(!envelope_sql_validation.ok, "engine envelope SQL text remains rejected");
  Require(HasDiagnostic(envelope_sql_validation, "SB_SBLR_SQL_TEXT_FORBIDDEN"),
          "envelope SQL text diagnostic is preserved");
}

}  // namespace

int main() {
  CheckRoundTrip();
  CheckValidationRejectsSourceAuthorityMisuse();
  std::cout << "sbsql_sblr_source_artifact_runtime_conformance=passed\n";
  return 0;
}
