// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/contextual_text_literal_v2_codec.hpp"
#include "engine/sblr/sblr_engine_envelope.hpp"
#include "engine/sblr/sblr_literal_runtime.hpp"
#include "hash_digest.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace sblr = scratchbird::engine::sblr;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr sblr::ContextualTextUuidV2 kTextDescriptorUuid{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x18};
constexpr sblr::ContextualTextUuidV2 kTextTypeUuid{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x19};
constexpr sblr::ContextualTextUuidV2 kTextCodecUuid{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x1a};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

void RequireExactOperandInvalid(
    const sblr::SblrOperationEnvelope& envelope,
    std::string_view message) {
  const auto validation = sblr::ValidateSblrEnvelope(envelope);
  if (!validation.ok && validation.diagnostics.size() == 1 &&
      validation.diagnostics.front().code == "SBLR.OPERAND_INVALID") {
    return;
  }
  for (const auto& diagnostic : validation.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Fail(message);
}

void RequireExactOperandInvalid(
    const sblr::SblrCanonicalOperandRecordsDecodeResult& validation,
    std::string_view message) {
  if (!validation.ok && validation.diagnostics.size() == 1 &&
      validation.diagnostics.front().code == "SBLR.OPERAND_INVALID") {
    return;
  }
  for (const auto& diagnostic : validation.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  Fail(message);
}

sblr::ContextualTextUuidV2 Uuid(std::uint8_t seed) {
  sblr::ContextualTextUuidV2 value{};
  value[0] = seed;
  value[6] = 0x70;
  value[8] = 0x80;
  value[15] = static_cast<std::uint8_t>(seed + 1);
  return value;
}

void Put16(Bytes* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value));
  bytes->push_back(static_cast<std::uint8_t>(value >> 8));
}

void Put32(Bytes* bytes, std::uint32_t value) {
  for (unsigned index = 0; index != 4; ++index) {
    bytes->push_back(static_cast<std::uint8_t>(value >> (8U * index)));
  }
}

void Put64(Bytes* bytes, std::uint64_t value) {
  for (unsigned index = 0; index != 8; ++index) {
    bytes->push_back(static_cast<std::uint8_t>(value >> (8U * index)));
  }
}

void PutText(Bytes* bytes, const std::string_view value) {
  Put16(bytes, static_cast<std::uint16_t>(value.size()));
  bytes->insert(bytes->end(), value.begin(), value.end());
}

Bytes EncodeMaximumComposedSbxn() {
  constexpr std::uint32_t count = 10;
  constexpr std::size_t body_size = 61000;
  constexpr std::size_t record_fixed_bytes = 125;
  const std::size_t total =
      32 + count * (record_fixed_bytes + body_size);
  Bytes bytes;
  bytes.reserve(total);
  bytes.insert(bytes.end(), {'S', 'B', 'X', 'N'});
  Put16(&bytes, 1);
  Put16(&bytes, 32);
  Put32(&bytes, count);
  Put32(&bytes, 0);
  Put64(&bytes, total);
  Put64(&bytes, 32);
  for (std::uint32_t index = 0; index != count; ++index) {
    Put32(&bytes, record_fixed_bytes + body_size);
    Put64(&bytes, index + 1);
    Put64(&bytes, 0);
    Put32(&bytes, index + 1);
    Put16(&bytes, 3);
    Put16(&bytes, 1);
    Put16(&bytes, 0);
    Put64(&bytes, 1);
    bytes.insert(bytes.end(), kTextDescriptorUuid.begin(),
                 kTextDescriptorUuid.end());
    Put64(&bytes, body_size);
    PutText(&bytes, "SBLR_LITERAL");
    PutText(&bytes, "typed_literal");
    bytes.insert(bytes.end(), body_size, 'x');
    PutText(&bytes, "engine.op.literal");
    PutText(&bytes, "typed_value");
    Put16(&bytes, 1);
  }
  Require(bytes.size() == total, "composed SBXN extent differs");
  return bytes;
}

Bytes EncodeReference(std::uint64_t node_id, const Bytes& sbxn) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(sbxn);
  Require(digest.ok(), "SBXN SHA-256 failed");
  Bytes bytes;
  bytes.reserve(72);
  Put16(&bytes, 1);
  Put16(&bytes, 0);
  Put32(&bytes, 1);
  Put64(&bytes, node_id);
  bytes.insert(bytes.end(), digest.digest.begin(), digest.digest.end());
  bytes.insert(bytes.end(), kTextDescriptorUuid.begin(),
               kTextDescriptorUuid.end());
  Put64(&bytes, 1);
  Require(bytes.size() == 72, "reference extent differs");
  return bytes;
}

Bytes EncodeExecute(
    std::uint64_t node_id,
    std::uint8_t body,
    const Bytes& pre_contextual_records,
    const Bytes& sbxn) {
  sblr::ContextualTextLiteralExecuteV2 execute;
  execute.statement_receipt_uuid = Uuid(1);
  execute.profile_set_uuid = Uuid(2);
  execute.profile_set_generation = 1;
  execute.catalog_snapshot_uuid = Uuid(3);
  execute.catalog_generation = 1;
  execute.datatype_registry_generation = 1;
  execute.security_generation = 1;
  execute.resource_epoch = 1;
  execute.mga_snapshot_uuid = Uuid(4);
  execute.literal_budget_uuid = Uuid(5);
  execute.literal_budget_generation = 1;
  execute.literal_negotiation_byte_grant = 65536;
  execute.canonical_body_aggregate_grant = 32563;
  execute.pre_contextual_operand_vector_sha256 =
      sblr::ComputeContextualTextPreContextualOperandVectorSha256V2(
          pre_contextual_records, 2);
  execute.sbxn_sha256 = sblr::ComputeContextualTextSbxnSha256V2(sbxn);

  sblr::ContextualTextLiteralProfileV2 profile;
  profile.profile_uuid = Uuid(6);
  profile.profile_set_uuid = execute.profile_set_uuid;
  profile.profile_set_generation = execute.profile_set_generation;
  profile.literal_binding_uuid = Uuid(7);
  profile.literal_binding_generation = 1;
  profile.literal_occurrence = 1;
  profile.node_id = node_id;
  profile.comparison_occurrence = 21;
  profile.statement_receipt_uuid = execute.statement_receipt_uuid;
  profile.catalog_snapshot_uuid = execute.catalog_snapshot_uuid;
  profile.catalog_generation = execute.catalog_generation;
  profile.datatype_registry_generation = execute.datatype_registry_generation;
  profile.security_generation = execute.security_generation;
  profile.resource_epoch = execute.resource_epoch;
  profile.mga_snapshot_uuid = execute.mga_snapshot_uuid;
  profile.descriptor_uuid = kTextDescriptorUuid;
  profile.descriptor_generation = 1;
  profile.type_uuid = kTextTypeUuid;
  profile.type_generation = 1;
  profile.codec_uuid = kTextCodecUuid;
  profile.codec_version = 1;
  profile.codec_generation = 1;
  profile.literal_argument_ordinal = 1;
  profile.target_argument_ordinal = 2;
  profile.source_occurrence_uuid = Uuid(8);
  profile.source_generation = 1;
  profile.relation_uuid = Uuid(9);
  profile.relation_descriptor_uuid = Uuid(10);
  profile.relation_descriptor_generation = 1;
  profile.column_uuid = Uuid(11);
  profile.column_ordinal = 2;
  profile.parent_operand_ordinal = 1;
  profile.target_descriptor_handle = 3;
  profile.literal_descriptor_handle = 9;
  profile.scalar_count = 1;
  profile.target_character_limit = 256;
  profile.target_byte_limit = 1024;
  profile.charset_uuid = Uuid(12);
  profile.charset_generation = 1;
  profile.collation_uuid = Uuid(13);
  profile.collation_generation = 1;
  profile.normalization_policy_uuid = Uuid(14);
  profile.normalization_policy_generation = 1;
  profile.render_policy_uuid = Uuid(15);
  profile.render_policy_generation = 1;
  profile.canonicalization_profile_uuid = Uuid(16);
  profile.canonicalization_profile_generation = 1;
  profile.comparison_contract_uuid = Uuid(17);
  profile.comparison_contract_generation = 1;
  profile.equality_operation_uuid = Uuid(18);
  profile.equality_operation_generation = 1;
  profile.literal_budget_uuid = execute.literal_budget_uuid;
  profile.literal_budget_generation = execute.literal_budget_generation;
  profile.literal_negotiation_byte_grant =
      execute.literal_negotiation_byte_grant;
  profile.canonical_body_aggregate_grant =
      execute.canonical_body_aggregate_grant;
  profile.canonical_body = {body};

  sblr::ContextualTextLiteralProfileMappingV2 mapping;
  mapping.literal_occurrence = profile.literal_occurrence;
  mapping.node_id = profile.node_id;
  mapping.literal_binding_uuid = profile.literal_binding_uuid;
  mapping.literal_binding_generation = profile.literal_binding_generation;
  mapping.literal_descriptor_handle = profile.literal_descriptor_handle;
  mapping.target_descriptor_handle = profile.target_descriptor_handle;
  mapping.profile = std::move(profile);
  execute.mappings.push_back(std::move(mapping));

  Bytes encoded;
  sblr::ContextualTextCodecDiagnosticV2 diagnostic;
  Require(sblr::EncodeContextualTextLiteralExecuteV2(execute, &encoded,
                                                     &diagnostic),
          diagnostic.detail.empty() ? "SBTLXE02 encode failed"
                                    : diagnostic.detail);
  return encoded;
}

sblr::SblrOperationEnvelope MakeEnvelope(std::uint64_t node_id,
                                         std::uint8_t body) {
  auto envelope = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE", "contextual-text-v2");
  envelope.opcode_code = 4615;
  envelope.operation_version_minor = 1;
  envelope.result_shape = "query_execute_result";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      "019d0000-0000-7000-8000-00000000f201";
  envelope.registry_snapshot_uuid =
      "019d0000-0000-7000-8000-00000000f202";

  sblr::SblrExpressionNodeTableV1 table;
  sblr::SblrExpressionLiteralNodeV1 node;
  node.node_id = node_id;
  node.parent_operand_ordinal = 1;
  node.descriptor_uuid = kTextDescriptorUuid;
  node.descriptor_generation = 1;
  node.literal_body = {body};
  table.nodes.push_back(std::move(node));
  const auto sbxn = sblr::EncodeSblrExpressionNodeTableV1(table);
  Require(!sbxn.empty(), "SBXN encode failed");

  sblr::SblrOperand table_operand;
  table_operand.ordinal = 1;
  table_operand.type = "expression.node_table.v1";
  table_operand.name = "expression_nodes";
  table_operand.value_kind = sblr::SblrValueKind::expression_node_table;
  table_operand.value_body = sbxn;
  envelope.operands.push_back(std::move(table_operand));

  sblr::SblrOperand reference;
  reference.ordinal = 2;
  reference.type = "relational_expression_v1";
  reference.name = "21";
  reference.value_kind = sblr::SblrValueKind::expression_node_ref;
  reference.value_body = EncodeReference(node_id, sbxn);
  envelope.operands.push_back(std::move(reference));

  const auto pre_contextual_records =
      sblr::EncodeSblrCanonicalOperandRecords(envelope.operands);
  Require(!pre_contextual_records.empty(),
          "canonical pre-contextual operand records did not encode");

  sblr::SblrOperand contextual;
  contextual.ordinal = 3;
  contextual.type = "literal.contextual_text_profile_set.v2";
  contextual.name = "contextual_text_profiles";
  contextual.value_kind =
      sblr::SblrValueKind::contextual_text_literal_profile_set;
  contextual.value_body =
      EncodeExecute(node_id, body, pre_contextual_records, sbxn);
  envelope.operands.push_back(std::move(contextual));
  return envelope;
}

}  // namespace

int main() {
  const auto admitted = MakeEnvelope(17, 'x');

  std::vector<sblr::SblrOperand> exact_prefix(
      admitted.operands.begin(), admitted.operands.end() - 1);
  const auto exact_prefix_records =
      sblr::EncodeSblrCanonicalOperandRecords(exact_prefix);
  const auto decoded_prefix = sblr::DecodeSblrCanonicalOperandRecords(
      exact_prefix_records.data(), exact_prefix_records.size(), 2,
      sblr::SblrOperandRecordDecodeProfile::
          contextual_query_execute_v1_1_pre_kind206);
  Require(decoded_prefix.ok && decoded_prefix.operands.size() == 2 &&
              decoded_prefix.canonical_bytes == exact_prefix_records,
          "canonical pre-kind206 operand records did not round-trip");
  auto trailing_prefix = exact_prefix_records;
  trailing_prefix.push_back(0);
  const auto trailing_prefix_validation =
      sblr::DecodeSblrCanonicalOperandRecords(
          trailing_prefix.data(), trailing_prefix.size(), 2,
          sblr::SblrOperandRecordDecodeProfile::
              contextual_query_execute_v1_1_pre_kind206);
  RequireExactOperandInvalid(
      trailing_prefix_validation,
      "trailing operand-record byte did not return the exact registered "
      "diagnostic");
  const auto mismatched_count_validation =
      sblr::DecodeSblrCanonicalOperandRecords(
          exact_prefix_records.data(), exact_prefix_records.size(), 1,
          sblr::SblrOperandRecordDecodeProfile::
              contextual_query_execute_v1_1_pre_kind206);
  RequireExactOperandInvalid(
      mismatched_count_validation,
      "mismatched operand-record count did not return the exact registered "
      "diagnostic");
  const auto records_with_kind206 =
      sblr::EncodeSblrCanonicalOperandRecords(admitted.operands);
  const auto premature_kind206_validation =
      sblr::DecodeSblrCanonicalOperandRecords(
          records_with_kind206.data(), records_with_kind206.size(), 3,
          sblr::SblrOperandRecordDecodeProfile::
              contextual_query_execute_v1_1_pre_kind206);
  RequireExactOperandInvalid(
      premature_kind206_validation,
      "premature kind 206 did not return the exact registered diagnostic");

  const auto composed_sbxn = EncodeMaximumComposedSbxn();
  Require(composed_sbxn.size() >
              sblr::kSblrExpressionNodeTableMaximumBytesV1 &&
              composed_sbxn.size() <=
                  sblr::kSblrContextualComposedExpressionNodeTableMaximumBytesV2,
          "composed SBXN fixture does not cross the ordinary limit");
  Require(!sblr::DecodeSblrExpressionNodeTableV1(
               composed_sbxn.data(), composed_sbxn.size()).ok &&
              sblr::DecodeSblrContextualComposedExpressionNodeTableV2(
                  composed_sbxn.data(), composed_sbxn.size()).ok,
          "ordinary and composed SBXN limits were not kept distinct");
  sblr::SblrOperand composed_table;
  composed_table.ordinal = 1;
  composed_table.type = "expression.node_table.v1";
  composed_table.name = "expression_nodes";
  composed_table.value_kind = sblr::SblrValueKind::expression_node_table;
  composed_table.value_body = composed_sbxn;
  const auto composed_records =
      sblr::EncodeSblrCanonicalOperandRecords({composed_table});
  Require(sblr::DecodeSblrCanonicalOperandRecords(
              composed_records.data(), composed_records.size(), 1,
              sblr::SblrOperandRecordDecodeProfile::
                  contextual_query_execute_v1_1_pre_kind206)
              .ok,
          "contextual operand-record decoder inherited the ordinary SBXN cap");

  const auto admitted_validation = sblr::ValidateSblrEnvelope(admitted);
  if (!admitted_validation.ok) {
    for (const auto& diagnostic : admitted_validation.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    Fail("exact query.execute 1.1 contextual carrier was refused");
  }
  const auto encoded_admitted = sblr::EncodeSblrEnvelope(admitted);
  Require(!encoded_admitted.empty(),
          "exact query.execute 1.1 contextual carrier did not encode");
  const Bytes encoded_admitted_bytes(encoded_admitted.begin(),
                                      encoded_admitted.end());
  const auto decoded_admitted = sblr::DecodeSblrEnvelope(encoded_admitted);
  if (!decoded_admitted.ok) {
    for (const auto& diagnostic : decoded_admitted.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
  }
  Require(decoded_admitted.ok &&
              decoded_admitted.envelope.operation_version_minor == 1 &&
              decoded_admitted.envelope.result_shape ==
                  "query_execute_result" &&
              decoded_admitted.envelope.diagnostic_shape ==
                  "diagnostic_vector" &&
              decoded_admitted.canonical_bytes == encoded_admitted_bytes,
          "exact query.execute 1.1 contextual carrier did not round-trip");

  auto stale_result_shape = admitted;
  stale_result_shape.result_shape = "query_execute_result.v1";
  RequireExactOperandInvalid(
      stale_result_shape,
      "query.execute 1.1 admitted a stale result-shape alias");

  auto stale_diagnostic_shape = admitted;
  stale_diagnostic_shape.diagnostic_shape = "engine.diagnostic.v1";
  RequireExactOperandInvalid(
      stale_diagnostic_shape,
      "query.execute 1.1 admitted a stale diagnostic-shape alias");

  auto missing = admitted;
  missing.operands.pop_back();
  RequireExactOperandInvalid(
      missing, "query.execute 1.1 without kind 206 did not return the exact "
               "registered diagnostic");

  auto legacy = admitted;
  legacy.operation_version_minor = 0;
  RequireExactOperandInvalid(
      legacy, "query.execute 1.0 with kind 206 did not return the exact "
              "registered diagnostic");

  auto unknown_minor = admitted;
  unknown_minor.operation_version_minor = 2;
  RequireExactOperandInvalid(
      unknown_minor,
      "contextual query unknown minor did not return the exact registered "
      "diagnostic");

  auto nonfinal = admitted;
  nonfinal.operands.push_back(nonfinal.operands[1]);
  nonfinal.operands.back().ordinal = 4;
  RequireExactOperandInvalid(
      nonfinal,
      "nonfinal kind 206 did not return the exact registered diagnostic");

  auto malformed_operand = admitted;
  malformed_operand.operands.front().value_flags = 1;
  RequireExactOperandInvalid(
      malformed_operand,
      "malformed contextual operand did not return the exact registered "
      "diagnostic");

  auto stale_pre_context = admitted;
  stale_pre_context.operands[1].name = "22";
  RequireExactOperandInvalid(
      stale_pre_context,
      "stale pre-contextual hash did not return the exact registered "
      "diagnostic");

  auto wrong_node = admitted;
  std::vector<sblr::SblrOperand> pre_contextual(
      admitted.operands.begin(), admitted.operands.end() - 1);
  const auto pre_contextual_records =
      sblr::EncodeSblrCanonicalOperandRecords(pre_contextual);
  const auto& sbxn = admitted.operands.front().value_body;
  wrong_node.operands.back().value_body =
      EncodeExecute(18, 'x', pre_contextual_records, sbxn);
  RequireExactOperandInvalid(
      wrong_node,
      "cross-node mapping did not return the exact registered diagnostic");

  auto wrong_body = admitted;
  wrong_body.operands.back().value_body =
      EncodeExecute(17, 'y', pre_contextual_records, sbxn);
  RequireExactOperandInvalid(
      wrong_body,
      "cross-body mapping did not return the exact registered diagnostic");

  auto bare = admitted;
  bare.operands.erase(bare.operands.begin(), bare.operands.begin() + 2);
  bare.operands.front().ordinal = 1;
  RequireExactOperandInvalid(
      bare, "kind 206 without contextual SBXN did not return the exact "
            "registered diagnostic");
  return EXIT_SUCCESS;
}
