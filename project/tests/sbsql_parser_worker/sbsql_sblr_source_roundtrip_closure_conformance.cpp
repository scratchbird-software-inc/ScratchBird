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
#include "sblr_transaction_begin_runtime.hpp"
#include "sblr_transaction_commit_runtime.hpp"
#include "sblr_transaction_rollback_runtime.hpp"
#include "sbu_sbsql_parser_support.hpp"

#include "scratchbird/engine/sblr_envelope.hpp"

#include <algorithm>
#include <array>
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

sblr::SblrOperationEnvelope CanonicalOperation(
    sblr::SblrOperationEnvelope envelope) {
  const auto* entry = sblr::LookupSblrOperation(envelope.operation_id);
  Require(entry != nullptr && entry->opcode == envelope.opcode && entry->code != 0,
          "source round-trip fixture lacks an exact canonical registry identity");
  envelope.opcode_code = entry->code;
  envelope.parser_package_uuid = "019f1000-0000-7000-8000-000000000501";
  envelope.registry_snapshot_uuid = "019f1000-0000-7000-8000-000000000502";
  envelope.parser_resolved_names_to_uuids = true;
  for (std::size_t index = 0; index < envelope.operands.size(); ++index) {
    auto& operand = envelope.operands[index];
    operand.ordinal = static_cast<std::uint32_t>(index + 1);
    if (operand.value_kind != sblr::SblrValueKind::null_value ||
        !operand.value_body.empty()) {
      continue;
    }
    operand.value_kind = sblr::SblrValueKind::literal_typed;
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

void RenumberOperands(sblr::SblrOperationEnvelope* envelope) {
  for (std::size_t index = 0; index < envelope->operands.size(); ++index) {
    envelope->operands[index].ordinal = static_cast<std::uint32_t>(index + 1);
  }
}

void SetOperandText(sblr::SblrOperationEnvelope* envelope,
                    std::string_view name,
                    std::string_view value) {
  for (auto& operand : envelope->operands) {
    if (operand.name != name) continue;
    Require(operand.value_kind == sblr::SblrValueKind::literal_typed &&
                operand.value_body.size() >= 24,
            "source round-trip typed operand is malformed");
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
  Require(false, "source round-trip operand to mutate is missing");
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
  return CanonicalOperation(std::move(envelope));
}

sblr::SblrOperationEnvelope BuildTransactionEnvelope(std::string operation_id,
                                                     std::string opcode) {
  auto envelope = sblr::MakeSblrEnvelope(std::move(operation_id),
                                         std::move(opcode),
                                         "CBQ-038-TRANSACTION-ROUNDTRIP");
  if (envelope.operation_id == "engine.op.txn_begin") {
    envelope.requires_transaction_context = false;
    envelope.result_shape = "transaction_handle";
    envelope.diagnostic_shape = "diagnostic_vector";
    sblr::SblrTransactionBeginOptionsV1 options;
    options.isolation_profile_uuid[0] = 1;
    options.isolation_profile_generation = 1;
    options.transaction_policy_snapshot_uuid[0] = 2;
    options.transaction_policy_generation = 1;
    options.read_mode = 1;
    options.authority_scope = 1;
    options.wait_policy = 1;
    sblr::SblrOperand operand;
    operand.ordinal = 1;
    operand.type = "transaction.begin_options";
    operand.name = "options";
    operand.value_kind = sblr::SblrValueKind::transaction_begin_options;
    operand.value_body = sblr::EncodeSblrTransactionBeginOptionsV1(&options);
    Require(!operand.value_body.empty(),
            "canonical transaction begin options did not encode");
    envelope.operands.push_back(std::move(operand));
  } else if (envelope.operation_id == "engine.op.txn_commit") {
    envelope.requires_transaction_context = true;
    envelope.result_shape = "commit_result";
    envelope.diagnostic_shape = "diagnostic_vector";
    sblr::SblrTransactionCommitOptionsV1 options;
    options.transaction_uuid[0] = 1;
    options.local_transaction_id = 1;
    options.admitted_handle_evidence_sha256[0] = 2;
    options.commit_mode = 1;
    options.authority_scope = 1;
    options.wait_policy = 1;
    sblr::SblrOperand operand;
    operand.ordinal = 1;
    operand.type = "transaction.commit.options";
    operand.name = "options";
    operand.value_kind = sblr::SblrValueKind::transaction_commit_options;
    operand.value_body = sblr::EncodeSblrTransactionCommitOptionsV1(&options);
    Require(!operand.value_body.empty(),
            "canonical transaction commit options did not encode");
    envelope.operands.push_back(std::move(operand));
  } else if (envelope.operation_id == "engine.op.txn_rollback") {
    envelope.requires_transaction_context = true;
    envelope.result_shape = "rollback_result";
    envelope.diagnostic_shape = "diagnostic_vector";
    sblr::SblrTransactionRollbackOptionsV1 options;
    options.transaction_uuid[0] = 1;
    options.local_transaction_id = 1;
    options.admitted_handle_evidence_sha256[0] = 2;
    options.rollback_mode = 1;
    options.authority_scope = 1;
    options.wait_policy = 1;
    sblr::SblrOperand operand;
    operand.ordinal = 1;
    operand.type = "transaction.rollback.options";
    operand.name = "options";
    operand.value_kind = sblr::SblrValueKind::transaction_rollback_options;
    operand.value_body = sblr::EncodeSblrTransactionRollbackOptionsV1(&options);
    Require(!operand.value_body.empty(),
            "canonical transaction rollback options did not encode");
    envelope.operands.push_back(std::move(operand));
  } else {
    envelope.operands.push_back(Operand(
        "sbsql_render_family", "source_preserving_transaction_control_v1"));
    envelope.operands.push_back(Operand("session_context_uuid",
                                        std::string(kSessionContextUuid)));
    if (envelope.operation_id == "transaction.set_characteristics") {
      envelope.operands.push_back(
          Operand("transaction_read_mode", "read_only"));
    }
  }
  AttachSourcePolicy(&envelope, "CBQ-038-transaction-source-map");
  return CanonicalOperation(std::move(envelope));
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
    const sblr::SblrOperationEnvelope& envelope) {
  Require(sblr::EncodeSblrEnvelope(envelope).empty(),
          "source metadata was serialized inside SBOP");
  auto operation = envelope;
  operation.source_artifact_map = {};
  const auto validation = sblr::ValidateSblrEnvelope(operation);
  Require(validation.ok,
          "canonical operation without source metadata is invalid: " +
              sblr::SerializeSblrValidationToJson(validation));
  const auto encoded_text = sblr::EncodeSblrEnvelope(operation);
  Require(!encoded_text.empty(),
          "canonical operation without source metadata did not encode");
  const auto uuid = [](std::uint8_t suffix) {
    std::array<std::uint8_t, 16> value{};
    value[0] = 0x12;
    value[6] = 0x70;
    value[8] = 0x80;
    value[15] = suffix;
    return value;
  };
  public_sblr::SblrCanonicalContainer container;
  const auto engine_uuid = uuid(0x21);
  const auto dialect_uuid = uuid(0x22);
  const auto parser_uuid = uuid(0x31);
  const auto bundle_uuid = uuid(0x24);
  const auto request_uuid = uuid(0x25);
  std::copy(engine_uuid.begin(), engine_uuid.end(),
            container.canonical_anchor.begin());
  std::copy(dialect_uuid.begin(), dialect_uuid.end(),
            container.canonical_anchor.begin() + 16);
  std::copy(parser_uuid.begin(), parser_uuid.end(),
            container.canonical_anchor.begin() + 32);
  container.canonical_anchor[48] = 1;
  container.canonical_anchor[52] = 1;
  container.canonical_anchor[60] = 1;
  container.canonical_anchor[68] = 1;
  std::copy(bundle_uuid.begin(), bundle_uuid.end(),
            container.canonical_anchor.begin() + 76);
  container.canonical_anchor[92] = 1;
  container.canonical_anchor[100] = 2;
  std::copy(request_uuid.begin(), request_uuid.end(),
            container.canonical_anchor.begin() + 116);
  container.operation_payload.assign(encoded_text.begin(), encoded_text.end());
  const auto binary = public_sblr::EncodeSblrContainer(container);
  Require(!binary.empty(), "canonical SBLR container did not encode");
  const auto decoded =
      public_sblr::DecodeSblrContainerBytes(binary.data(), binary.size());
  Require(decoded.status == public_sblr::SblrCodecStatus::ok,
          "binary SBLR envelope did not decode");
  Require(decoded.container.source_map.empty(),
          "operation-only container acquired an unvalidated source map");

  const std::string canonical_text(
      reinterpret_cast<const char*>(decoded.container.operation_payload.data()),
      decoded.container.operation_payload.size());
  Require(canonical_text == encoded_text,
          "binary SBLR canonical bytes did not preserve source envelope");

  const auto reencoded = public_sblr::EncodeSblrContainer(decoded.container);
  Require(reencoded == binary,
          "binary SBLR encode/decode was not byte-identical");
  return canonical_text;
}

void CheckRenderReparseRoundTrip(const sblr::SblrOperationEnvelope& envelope,
                                 std::string_view expected_fragment,
                                 std::string_view label) {
  const auto canonical_text = BinaryRoundTripCanonicalText(envelope);
  auto decoded_text = sblr::DecodeSblrEnvelope(canonical_text);
  Require(decoded_text.ok, std::string(label) + " textual envelope did not decode");
  decoded_text.envelope.source_artifact_map = envelope.source_artifact_map;

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
  Require(!udr_result.ok &&
              Contains(udr_result.message_vector_json,
                       "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REQUIRED"),
          std::string(label) +
              " bare-SBOP UDR reversal did not fail closed");

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
  CheckRenderReparseRoundTrip(
      BuildDmlEnvelope("engine.op.insert", "SBLR_INSERT"),
      "INSERT INTO roundtrip_customer (amount) VALUES (:p_amount);",
      "DML insert");
  CheckRenderReparseRoundTrip(
      BuildDmlEnvelope("engine.op.update", "SBLR_UPDATE"),
      "UPDATE roundtrip_customer SET amount = :p_amount WHERE customer_id = :p_customer_id;",
      "DML update");
  CheckRenderReparseRoundTrip(
      BuildDmlEnvelope("engine.op.delete", "SBLR_DELETE"),
      "DELETE FROM roundtrip_customer WHERE customer_id = :p_customer_id;",
      "DML delete");
  CheckRenderReparseRoundTrip(
      BuildTransactionEnvelope("engine.op.txn_begin", "SBLR_TXN_BEGIN"),
      "BEGIN TRANSACTION;",
      "transaction begin");
  CheckRenderReparseRoundTrip(
      BuildTransactionEnvelope("transaction.set_characteristics",
                               "SBLR_TRANSACTION_SET_CHARACTERISTICS"),
      "SET TRANSACTION READ ONLY;",
      "transaction set characteristics");
  CheckRenderReparseRoundTrip(
      BuildTransactionEnvelope("engine.op.txn_commit", "SBLR_TXN_COMMIT"),
      "COMMIT;",
      "transaction commit");
  CheckRenderReparseRoundTrip(
      BuildTransactionEnvelope("engine.op.txn_rollback", "SBLR_TXN_ROLLBACK"),
      "ROLLBACK;",
      "transaction rollback");
}

void CheckAuthorityAndPolicyRefusals() {
  auto missing_authority =
      BuildDmlEnvelope("engine.op.update", "SBLR_UPDATE");
  missing_authority.operands.erase(
      std::remove_if(missing_authority.operands.begin(),
                     missing_authority.operands.end(),
                     [](const sblr::SblrOperand& operand) {
                       return operand.name == "value_column_uuid";
                     }),
      missing_authority.operands.end());
  RenumberOperands(&missing_authority);
  ExpectApiRefusal(missing_authority,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_OPERAND_REQUIRED",
                   "missing value column descriptor authority");

  auto mismatched_authority =
      BuildDmlEnvelope("engine.op.update", "SBLR_UPDATE");
  SetOperandText(&mismatched_authority, "value_column_uuid",
                 "019f1000-0000-7000-8000-00000000ffff");
  ExpectApiRefusal(mismatched_authority,
                   "SB_SBLR_TO_SBSQL_AUTHORITY_MISMATCH",
                   "mismatched value column authority");

  auto missing_source = BuildDmlEnvelope("engine.op.insert", "SBLR_INSERT");
  missing_source.source_artifact_map = {};
  ExpectApiRefusal(missing_source,
                   "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REQUIRED",
                   "missing source artifact sidecar");

  auto redacted = BuildDmlEnvelope("engine.op.insert", "SBLR_INSERT");
  redacted.source_artifact_map.policy_status = "redacted_render_metadata";
  ExpectApiRefusal(redacted,
                   "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_REDACTED",
                   "redacted source artifact sidecar");

  auto sql_text_artifact =
      BuildDmlEnvelope("engine.op.update", "SBLR_UPDATE");
  sql_text_artifact.source_artifact_map.contains_sql_text = true;
  ExpectApiRefusal(sql_text_artifact,
                   "SB_SBLR_SOURCE_ARTIFACT_SQL_TEXT_FORBIDDEN",
                   "source artifact SQL text misuse");

  auto authoritative = BuildDmlEnvelope("engine.op.insert", "SBLR_INSERT");
  authoritative.source_artifact_map.raw_sql_text_authoritative = true;
  ExpectApiRefusal(authoritative,
                   "SB_SBLR_TO_SBSQL_SOURCE_ARTIFACT_POLICY_UNSUPPORTED",
                   "authoritative source artifact misuse");

  auto known_no_contract = CanonicalOperation(sblr::MakeSblrEnvelope(
      "query.bind_expression", "SBLR_QUERY_BIND_EXPRESSION",
      "CBQ-038-NONREVERSIBLE"));
  AttachSourcePolicy(&known_no_contract, "CBQ-038-known-no-contract");
  ExpectApiRefusal(known_no_contract,
                   "SB_SBLR_TO_SBSQL_NO_SOURCE_PRESERVING_RENDER_CONTRACT",
                   "known operation without render contract");

  auto cluster = CanonicalOperation(sblr::MakeSblrEnvelope(
      "engine.op.cluster_write_admission", "SBLR_CLUSTER_WRITE_ADMISSION",
      "CBQ-038-CLUSTER-REFUSAL"));
  cluster.requires_cluster_authority = true;
  AttachSourcePolicy(&cluster, "CBQ-038-cluster-refusal");
  ExpectApiRefusal(cluster,
                   "SB_SBLR_TO_SBSQL_NON_CORE_OPERATION_REFUSED",
                   "cluster operation refusal");

  const auto& registry = sblr::StaticSblrOpcodeRegistry();
  const auto provider_entry = std::find_if(
      registry.begin(), registry.end(), [](const auto& entry) {
        return entry.category == sblr::SblrOpcodeCategory::extensibility &&
               entry.code != 0;
      });
  Require(provider_entry != registry.end(),
          "canonical optional-provider registry entry is missing");
  auto provider = CanonicalOperation(sblr::MakeSblrEnvelope(
      provider_entry->operation_id, provider_entry->opcode,
      "CBQ-038-OPTIONAL-PROVIDER-REFUSAL"));
  AttachSourcePolicy(&provider, "CBQ-038-provider-refusal");
  ExpectApiRefusal(provider,
                   "SB_SBLR_TO_SBSQL_OPTIONAL_PROVIDER_OPERATION_REFUSED",
                   "optional provider operation refusal");

  auto unknown = BuildDmlEnvelope("engine.op.insert", "SBLR_INSERT");
  unknown.operation_id = "query.unknown_roundtrip";
  unknown.opcode = "SBLR_QUERY_UNKNOWN_ROUNDTRIP";
  ExpectApiRefusal(unknown,
                   "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH",
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
