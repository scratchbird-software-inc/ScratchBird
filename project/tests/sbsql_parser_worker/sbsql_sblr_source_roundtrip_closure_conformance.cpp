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

#include "scratchbird/engine/sblr/lowering.hpp"

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

namespace public_sblr = scratchbird::engine;
namespace sblr = scratchbird::engine::sblr;
namespace udr = scratchbird::udr::sbsql_parser_support;

constexpr std::string_view kSourcePreservingPolicy =
    "allow_debug_artifacts=true;decompile_policy=source_preserving";
constexpr std::string_view kDescriptorUuid =
    "019f1000-0000-7000-8000-000000000001";
constexpr std::string_view kTableUuid =
    "019f1000-0000-7000-8000-000000000101";
constexpr std::string_view kIndexUuid =
    "019f1000-0000-7000-8000-000000000102";
constexpr std::string_view kValueColumnUuid =
    "019f1000-0000-7000-8000-000000000201";
constexpr std::string_view kPredicateColumnUuid =
    "019f1000-0000-7000-8000-000000000202";
constexpr std::string_view kValueParameterUuid =
    "019f1000-0000-7000-8000-000000000301";
constexpr std::string_view kPredicateParameterUuid =
    "019f1000-0000-7000-8000-000000000302";
constexpr std::string_view kSessionContextUuid =
    "019f1000-0000-7000-8000-000000000401";
constexpr std::string_view kTransactionContextUuid =
    "019f1000-0000-7000-8000-000000000402";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "require_failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

sblr::SblrOperand Operand(std::string name, std::string value) {
  sblr::SblrOperand operand;
  operand.type = "text";
  operand.name = std::move(name);
  operand.value = std::move(value);
  return operand;
}

sblr::SblrSourceSymbolArtifact Symbol(std::string symbol_kind,
                                      std::string stable_key,
                                      std::string resolved_uuid,
                                      std::string render_hint,
                                      std::string scope) {
  sblr::SblrSourceSymbolArtifact symbol;
  symbol.symbol_kind = std::move(symbol_kind);
  symbol.stable_key = std::move(stable_key);
  symbol.resolved_uuid = std::move(resolved_uuid);
  symbol.render_hint = std::move(render_hint);
  symbol.scope = std::move(scope);
  symbol.source_hash = "sha256:phase1f-source-symbol";
  return symbol;
}

sblr::SblrOperationRenderHint RenderHint(std::string stable_key,
                                         std::string value) {
  sblr::SblrOperationRenderHint hint;
  hint.hint_kind = "operation";
  hint.stable_key = std::move(stable_key);
  hint.value = std::move(value);
  return hint;
}

void AttachSourcePolicy(sblr::SblrOperationEnvelope* envelope,
                        std::string_view identity) {
  envelope->source_artifact_map.policy_status = "non_authoritative_render_metadata";
  envelope->source_artifact_map.source_identity = std::string(identity);
  envelope->source_artifact_map.source_hash =
      "sha256:phase1f-source-preserving-roundtrip";
  envelope->source_artifact_map.render_metadata_only = true;
  envelope->source_artifact_map.contains_sql_text = false;
  envelope->source_artifact_map.raw_sql_text_authoritative = false;
  envelope->source_artifact_map.operation_render_hints.push_back(
      RenderHint(envelope->operation_id, "structured_source_preserving_render"));
}

void AttachTableAndColumnSymbols(sblr::SblrOperationEnvelope* envelope) {
  envelope->source_artifact_map.symbols.push_back(
      Symbol("object_display_name", "object.roundtrip_customer",
             std::string(kTableUuid), "roundtrip_customer", "catalog.object"));
  envelope->source_artifact_map.symbols.push_back(
      Symbol("column_alias", "column.amount", std::string(kValueColumnUuid),
             "amount", "descriptor.column"));
  envelope->source_artifact_map.symbols.push_back(
      Symbol("column_alias", "column.customer_id",
             std::string(kPredicateColumnUuid), "customer_id",
             "descriptor.column"));
  envelope->source_artifact_map.symbols.push_back(
      Symbol("parameter", "param.amount", std::string(kValueParameterUuid),
             ":p_amount", "parameter.value"));
  envelope->source_artifact_map.symbols.push_back(
      Symbol("parameter", "param.customer_id",
             std::string(kPredicateParameterUuid), ":p_customer_id",
             "parameter.predicate"));
}

sblr::SblrOperationEnvelope BuildDmlEnvelope(std::string operation_id,
                                             std::string opcode) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         "CBQ-038-DML-SOURCE-ROUNDTRIP");
  envelope.requires_transaction_context = true;
  envelope.operands.push_back(Operand("sbsql_render_family",
                                      "source_preserving_dml_single_row_v1"));
  envelope.operands.push_back(Operand("authority_descriptor_uuid",
                                      std::string(kDescriptorUuid)));
  envelope.operands.push_back(Operand("target_object_uuid",
                                      std::string(kTableUuid)));
  envelope.operands.push_back(Operand("value_column_symbol_key",
                                      "column.amount"));
  envelope.operands.push_back(Operand("value_column_uuid",
                                      std::string(kValueColumnUuid)));
  envelope.operands.push_back(Operand("value_parameter_symbol_key",
                                      "param.amount"));
  envelope.operands.push_back(Operand("value_parameter_uuid",
                                      std::string(kValueParameterUuid)));
  envelope.operands.push_back(Operand("predicate_column_symbol_key",
                                      "column.customer_id"));
  envelope.operands.push_back(Operand("predicate_column_uuid",
                                      std::string(kPredicateColumnUuid)));
  envelope.operands.push_back(Operand("predicate_parameter_symbol_key",
                                      "param.customer_id"));
  envelope.operands.push_back(Operand("predicate_parameter_uuid",
                                      std::string(kPredicateParameterUuid)));
  AttachSourcePolicy(&envelope, "CBQ-038-dml-source-map");
  AttachTableAndColumnSymbols(&envelope);
  return envelope;
}

sblr::SblrOperationEnvelope BuildCreateTableEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("ddl.create_table",
                                         "SBLR_DDL_CREATE_TABLE",
                                         "CBQ-038-DDL-CREATE-TABLE-ROUNDTRIP");
  envelope.requires_transaction_context = true;
  envelope.operands.push_back(Operand("sbsql_render_family",
                                      "source_preserving_ddl_create_table_v1"));
  envelope.operands.push_back(Operand("authority_descriptor_uuid",
                                      std::string(kDescriptorUuid)));
  envelope.operands.push_back(Operand("table_symbol_key",
                                      "object.roundtrip_customer"));
  envelope.operands.push_back(Operand("target_object_uuid",
                                      std::string(kTableUuid)));
  envelope.operands.push_back(Operand("column_symbol_key", "column.amount"));
  envelope.operands.push_back(Operand("column_descriptor_uuid",
                                      std::string(kValueColumnUuid)));
  envelope.operands.push_back(Operand("column_type", "INT"));
  AttachSourcePolicy(&envelope, "CBQ-038-create-table-source-map");
  AttachTableAndColumnSymbols(&envelope);
  return envelope;
}

sblr::SblrOperationEnvelope BuildCreateIndexEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("ddl.create_index",
                                         "SBLR_DDL_CREATE_INDEX",
                                         "CBQ-038-DDL-CREATE-INDEX-ROUNDTRIP");
  envelope.requires_transaction_context = true;
  envelope.operands.push_back(Operand("sbsql_render_family",
                                      "source_preserving_ddl_create_index_v1"));
  envelope.operands.push_back(Operand("authority_descriptor_uuid",
                                      std::string(kDescriptorUuid)));
  envelope.operands.push_back(Operand("index_symbol_key",
                                      "object.idx_roundtrip_customer_amount"));
  envelope.operands.push_back(Operand("index_object_uuid",
                                      std::string(kIndexUuid)));
  envelope.operands.push_back(Operand("table_symbol_key",
                                      "object.roundtrip_customer"));
  envelope.operands.push_back(Operand("relation_object_uuid",
                                      std::string(kTableUuid)));
  envelope.operands.push_back(Operand("column_symbol_key", "column.amount"));
  envelope.operands.push_back(Operand("column_descriptor_uuid",
                                      std::string(kValueColumnUuid)));
  AttachSourcePolicy(&envelope, "CBQ-038-create-index-source-map");
  envelope.source_artifact_map.symbols.push_back(
      Symbol("object_display_name", "object.idx_roundtrip_customer_amount",
             std::string(kIndexUuid), "idx_roundtrip_customer_amount",
             "catalog.index"));
  AttachTableAndColumnSymbols(&envelope);
  return envelope;
}

sblr::SblrOperationEnvelope BuildTransactionEnvelope(std::string operation_id,
                                                     std::string opcode) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         "CBQ-038-TRANSACTION-ROUNDTRIP");
  envelope.operands.push_back(Operand("sbsql_render_family",
                                      "source_preserving_transaction_control_v1"));
  if (envelope.operation_id == "transaction.begin" ||
      envelope.operation_id == "transaction.set_characteristics" ||
      envelope.operation_id == "transaction.txn_begin") {
    envelope.operands.push_back(Operand("session_context_uuid",
                                        std::string(kSessionContextUuid)));
  } else {
    envelope.requires_transaction_context = true;
    envelope.operands.push_back(Operand("transaction_context_uuid",
                                        std::string(kTransactionContextUuid)));
  }
  if (envelope.operation_id == "transaction.set_characteristics") {
    envelope.operands.push_back(Operand("transaction_read_mode", "read_only"));
  }
  AttachSourcePolicy(&envelope, "CBQ-038-transaction-source-map");
  return envelope;
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

std::string BinaryRoundTripCanonicalText(
    const sblr::SblrOperationEnvelope& envelope,
    public_sblr::SblrOperationFamily family) {
  const auto encoded_text = sblr::EncodeSblrEnvelope(envelope);
  const std::string descriptor = "descriptor_authority=" +
                                 std::string(kDescriptorUuid);
  const auto binary =
      sblr::EnvelopeBuilder()
          .operation(family, 1)
          .payload_kind(public_sblr::SblrPayloadKind::operation_envelope)
          .descriptor(1,
                      reinterpret_cast<const std::uint8_t*>(descriptor.data()),
                      descriptor.size())
          .append_bytes(reinterpret_cast<const std::uint8_t*>(encoded_text.data()),
                        encoded_text.size())
          .encode();
  const auto decoded =
      public_sblr::DecodeSblrEnvelopeBytes(binary.data(), binary.size());
  Require(decoded.status == public_sblr::SblrCodecStatus::ok,
          "binary SBLR envelope did not decode");
  Require(decoded.envelope.payload_kind ==
              public_sblr::SblrPayloadKind::operation_envelope,
          "binary SBLR payload kind drifted");
  Require(decoded.envelope.family == family,
          "binary SBLR operation family drifted");
  Require(decoded.envelope.descriptors.size() == 1,
          "binary SBLR descriptor authority missing");

  const std::string canonical_text(
      reinterpret_cast<const char*>(decoded.envelope.canonical_bytes.data()),
      decoded.envelope.canonical_bytes.size());
  Require(canonical_text == encoded_text,
          "binary SBLR canonical bytes did not preserve source envelope");

  const auto reencoded = public_sblr::EncodeSblrEnvelope(decoded.envelope);
  Require(reencoded == binary,
          "binary SBLR encode/decode was not byte-identical");
  return canonical_text;
}

void CheckRenderReparseRoundTrip(const sblr::SblrOperationEnvelope& envelope,
                                 public_sblr::SblrOperationFamily family,
                                 std::string_view expected_fragment,
                                 std::string_view label) {
  const auto canonical_text = BinaryRoundTripCanonicalText(envelope, family);
  const auto decoded_text = sblr::DecodeSblrEnvelope(canonical_text);
  Require(decoded_text.ok, std::string(label) + " textual envelope did not decode");

  const sblr::SblrToSbsqlOptions options{.source_preserving = true};
  const auto rendered = sblr::RenderSblrEnvelopeToSbsql(decoded_text.envelope, options);
  Require(rendered.ok, std::string(label) + " did not render SBsql");
  Require(Contains(rendered.sbsql_text, expected_fragment),
          std::string(label) + " did not preserve expected render text");
  Require(!Contains(rendered.sbsql_text, std::string(kDescriptorUuid)) &&
              !Contains(rendered.sbsql_text, std::string(kTableUuid)) &&
              !Contains(rendered.sbsql_text, "source_artifact") &&
              !Contains(rendered.sbsql_text, "operation_id="),
          std::string(label) + " leaked authority metadata as SBsql text");

  const auto udr_result =
      udr::sbu_sbsql_decompile_sblr(canonical_text, kSourcePreservingPolicy);
  Require(udr_result.ok, std::string(label) + " UDR decompile failed");
  Require(udr_result.payload == rendered.sbsql_text,
          std::string(label) + " UDR decompile drifted from engine render");

  for (const auto& statement : RenderedStatements(rendered.sbsql_text)) {
    const auto syntax = udr::sbu_sbsql_validate_syntax(statement, "sbsql");
    Require(syntax.ok, std::string(label) + " rendered statement did not reparse: " +
                           statement);
  }
}

void ExpectApiRefusal(const sblr::SblrOperationEnvelope& envelope,
                      std::string_view code,
                      std::string_view label) {
  const sblr::SblrToSbsqlOptions options{.source_preserving = true};
  const auto rendered = sblr::RenderSblrEnvelopeToSbsql(envelope, options);
  Require(!rendered.ok, std::string(label) + " unexpectedly rendered");
  Require(!rendered.diagnostics.empty(),
          std::string(label) + " did not return diagnostics");
  Require(rendered.diagnostics.front().code == code,
          std::string(label) + " returned " +
              (rendered.diagnostics.empty() ? std::string("<none>")
                                            : rendered.diagnostics.front().code) +
              " instead of " + std::string(code));
}

void CheckRouteCoverage() {
  CheckRenderReparseRoundTrip(BuildCreateTableEnvelope(),
                              public_sblr::SblrOperationFamily::catalog_mutation,
                              "CREATE TABLE roundtrip_customer (amount INT);",
                              "DDL create table");
  CheckRenderReparseRoundTrip(BuildCreateIndexEnvelope(),
                              public_sblr::SblrOperationFamily::catalog_mutation,
                              "CREATE INDEX idx_roundtrip_customer_amount ON roundtrip_customer (amount);",
                              "DDL create index");
  CheckRenderReparseRoundTrip(
      BuildDmlEnvelope("dml.insert_rows", "SBLR_DML_INSERT_ROWS"),
      public_sblr::SblrOperationFamily::dml_insert,
      "INSERT INTO roundtrip_customer (amount) VALUES (:p_amount);",
      "DML insert");
  CheckRenderReparseRoundTrip(
      BuildDmlEnvelope("dml.select_rows", "SBLR_DML_SELECT_ROWS"),
      public_sblr::SblrOperationFamily::relational_query,
      "SELECT amount FROM roundtrip_customer WHERE customer_id = :p_customer_id;",
      "DML select");
  CheckRenderReparseRoundTrip(
      BuildDmlEnvelope("dml.update_rows", "SBLR_DML_UPDATE_ROWS"),
      public_sblr::SblrOperationFamily::dml_update,
      "UPDATE roundtrip_customer SET amount = :p_amount WHERE customer_id = :p_customer_id;",
      "DML update");
  CheckRenderReparseRoundTrip(
      BuildDmlEnvelope("dml.delete_rows", "SBLR_DML_DELETE_ROWS"),
      public_sblr::SblrOperationFamily::dml_delete,
      "DELETE FROM roundtrip_customer WHERE customer_id = :p_customer_id;",
      "DML delete");
  CheckRenderReparseRoundTrip(
      BuildTransactionEnvelope("transaction.begin", "SBLR_TRANSACTION_BEGIN"),
      public_sblr::SblrOperationFamily::transaction_control,
      "BEGIN TRANSACTION;",
      "transaction begin");
  CheckRenderReparseRoundTrip(
      BuildTransactionEnvelope("transaction.set_characteristics",
                               "SBLR_TRANSACTION_SET_CHARACTERISTICS"),
      public_sblr::SblrOperationFamily::transaction_control,
      "SET TRANSACTION READ ONLY;",
      "transaction set characteristics");
  CheckRenderReparseRoundTrip(
      BuildTransactionEnvelope("transaction.commit", "SBLR_TRANSACTION_COMMIT"),
      public_sblr::SblrOperationFamily::transaction_control,
      "COMMIT;",
      "transaction commit");
  CheckRenderReparseRoundTrip(
      BuildTransactionEnvelope("transaction.rollback", "SBLR_TRANSACTION_ROLLBACK"),
      public_sblr::SblrOperationFamily::transaction_control,
      "ROLLBACK;",
      "transaction rollback");
}

void CheckAuthorityAndPolicyRefusals() {
  auto missing_authority = BuildCreateTableEnvelope();
  missing_authority.operands.erase(
      std::remove_if(missing_authority.operands.begin(),
                     missing_authority.operands.end(),
                     [](const sblr::SblrOperand& operand) {
                       return operand.name == "column_descriptor_uuid";
                     }),
      missing_authority.operands.end());
  ExpectApiRefusal(missing_authority,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
                   "missing column descriptor authority");

  auto mismatched_authority = BuildDmlEnvelope("dml.update_rows",
                                               "SBLR_DML_UPDATE_ROWS");
  for (auto& operand : mismatched_authority.operands) {
    if (operand.name == "value_column_uuid") {
      operand.value = "019f1000-0000-7000-8000-00000000ffff";
    }
  }
  ExpectApiRefusal(mismatched_authority,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
                   "mismatched value column authority");

  auto sql_text_artifact = BuildDmlEnvelope("dml.select_rows",
                                            "SBLR_DML_SELECT_ROWS");
  sql_text_artifact.source_artifact_map.contains_sql_text = true;
  ExpectApiRefusal(sql_text_artifact,
                   "SB_SBLR_SOURCE_ARTIFACT_SQL_TEXT_FORBIDDEN",
                   "source artifact SQL text misuse");

  auto known_no_contract = sblr::MakeSblrEnvelope("query.plan_operation",
                                                  "SBLR_QUERY_PLAN_OPERATION",
                                                  "CBQ-038-NONREVERSIBLE");
  AttachSourcePolicy(&known_no_contract, "CBQ-038-known-no-contract");
  ExpectApiRefusal(known_no_contract,
                   "SB_SBLR_TO_SBSQL_NO_SOURCE_PRESERVING_RENDER_CONTRACT",
                   "known operation without render contract");

  auto cluster = sblr::MakeSblrEnvelope("cluster.place_object",
                                        "SBLR_CLUSTER_PLACE_OBJECT",
                                        "CBQ-038-CLUSTER-REFUSAL");
  AttachSourcePolicy(&cluster, "CBQ-038-cluster-refusal");
  ExpectApiRefusal(cluster,
                   "SB_SBLR_TO_SBSQL_NON_CORE_OPERATION_REFUSED",
                   "cluster operation refusal");

  auto provider = sblr::MakeSblrEnvelope("op.show.gpu",
                                         "SBLR_OP_SHOW_GPU",
                                         "CBQ-038-OPTIONAL-PROVIDER-REFUSAL");
  AttachSourcePolicy(&provider, "CBQ-038-provider-refusal");
  ExpectApiRefusal(provider,
                   "SB_SBLR_TO_SBSQL_OPTIONAL_PROVIDER_OPERATION_REFUSED",
                   "optional provider operation refusal");

  auto unknown = sblr::MakeSblrEnvelope("query.unknown_roundtrip",
                                        "SBLR_QUERY_UNKNOWN_ROUNDTRIP",
                                        "CBQ-038-UNKNOWN-REFUSAL");
  AttachSourcePolicy(&unknown, "CBQ-038-unknown-refusal");
  ExpectApiRefusal(unknown,
                   "SB_SBLR_TO_SBSQL_UNSUPPORTED_OPERATION",
                   "unknown operation refusal");
}

void CheckRegistryInventoryBasis() {
  std::size_t implemented = 0;
  std::size_t cluster_refusal = 0;
  std::size_t optional_provider = 0;
  for (const auto& entry : sblr::StaticSblrOpcodeRegistry()) {
    if (entry.support == sblr::SblrOpcodeSupport::implemented) ++implemented;
    if (entry.support == sblr::SblrOpcodeSupport::cluster_refusal ||
        entry.category == sblr::SblrOpcodeCategory::cluster) {
      ++cluster_refusal;
    }
    if (entry.category == sblr::SblrOpcodeCategory::extensibility) {
      ++optional_provider;
    }
  }
  Require(implemented > 0, "SBLR opcode registry inventory is empty");
  Require(cluster_refusal > 0, "SBLR opcode registry cluster boundary inventory missing");
  Require(optional_provider > 0, "SBLR opcode registry optional-provider inventory missing");
}

}  // namespace

int main() {
  CheckRegistryInventoryBasis();
  CheckRouteCoverage();
  CheckAuthorityAndPolicyRefusals();
  std::cout << "sbsql_sblr_source_roundtrip_closure_conformance=passed\n";
  return EXIT_SUCCESS;
}
