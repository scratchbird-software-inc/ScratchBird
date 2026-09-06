// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_source_artifact_runtime.hpp"
#include "sblr_opcode_stream.hpp"
#include "sblr_to_sbsql.hpp"
#include "sblr_transaction_begin_runtime.hpp"

#include "scratchbird/engine/sblr_envelope.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace public_sblr = scratchbird::engine;
namespace sblr = scratchbird::engine::sblr;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "require_failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

sblr::SblrSourceArtifactUuidV1 Uuid(std::uint8_t suffix) {
  sblr::SblrSourceArtifactUuidV1 value{};
  value[0] = 0x01;
  value[6] = 0x70;
  value[8] = 0x80;
  value[15] = suffix;
  return value;
}

sblr::SblrSourceArtifactMapV1 MakeArtifact() {
  sblr::SblrSourceArtifactMapV1 artifact;
  artifact.artifact_uuid = Uuid(1);
  artifact.container_request_uuid = Uuid(2);
  artifact.dialect_family_uuid = Uuid(3);
  artifact.parser_package_uuid = Uuid(4);
  artifact.language_tag = "en-CA";
  artifact.redaction_class =
      sblr::SblrSourceArtifactRedactionClassV1::none;
  artifact.decompile_policy =
      sblr::SblrSourceArtifactDecompilePolicyV1::source_preserving;

  constexpr std::array<std::string_view, 13> kNames{{
      "local_value", "input_value", "result_cursor", "retry_label",
      "worker_block", "calculate_total", "customer_id", "not_found",
      "recent_orders", "orders_alias", "amount_alias", "customer_table",
      "generated_slot",
  }};
  for (std::size_t index = 0; index < kNames.size(); ++index) {
    sblr::SblrSourceArtifactSymbolV1 symbol;
    symbol.symbol_id = index + 1;
    symbol.symbol_key = "symbol." + std::to_string(index + 1);
    symbol.symbol_kind = static_cast<sblr::SblrSourceArtifactSymbolKindV1>(
        index + 1);
    if (symbol.symbol_kind !=
        sblr::SblrSourceArtifactSymbolKindV1::object_display_name) {
      symbol.declaration_node_id = 1;
      symbol.scope_node_id = 1;
      symbol.use_node_ids = {2};
    } else {
      symbol.related_object_uuid = Uuid(20);
    }
    symbol.raw_name_utf8 = std::string(kNames[index]);
    symbol.normalized_lookup_key = std::string(kNames[index]);
    symbol.language_tag = "en-CA";
    symbol.ordinal = static_cast<std::uint32_t>(index + 1);
    symbol.source_span_id = index < 2 ? index + 1 : 0;
    symbol.generated = symbol.symbol_kind ==
                       sblr::SblrSourceArtifactSymbolKindV1::generated_temp;
    artifact.symbols.push_back(std::move(symbol));
  }

  for (std::uint64_t index = 1; index <= 2; ++index) {
    sblr::SblrSourceArtifactSpanV1 span;
    span.source_span_id = index;
    span.node_id = index;
    span.byte_start = (index - 1) * 8;
    span.byte_length = 8;
    span.line_start = 1;
    span.column_start = static_cast<std::uint32_t>((index - 1) * 8 + 1);
    span.line_end = 1;
    span.column_end = static_cast<std::uint32_t>(index * 8 + 1);
    span.span_kind = index == 1
                         ? sblr::SblrSourceArtifactSpanKindV1::statement
                         : sblr::SblrSourceArtifactSpanKindV1::identifier;
    artifact.source_spans.push_back(std::move(span));
  }

  for (std::size_t index = 0; index < artifact.symbols.size(); ++index) {
    sblr::SblrSourceArtifactRenderHintV1 hint;
    hint.render_hint_id = index + 1;
    hint.node_id = index == 0 ? 1 : 2;
    hint.symbol_id = index + 1;
    hint.dialect_family_uuid = artifact.dialect_family_uuid;
    hint.format_group = "source_preserving.identifier";
    artifact.render_hints.push_back(std::move(hint));
  }
  return artifact;
}

sblr::SblrSourceArtifactValidationContextV1 MakeContext(
    const sblr::SblrSourceArtifactMapV1& artifact) {
  sblr::SblrSourceArtifactValidationContextV1 context;
  context.operation_validated_without_artifact = true;
  context.source_preserving_requested = true;
  context.expected_container_request_uuid = artifact.container_request_uuid;
  context.expected_dialect_family_uuid = artifact.dialect_family_uuid;
  context.expected_parser_package_uuid = artifact.parser_package_uuid;
  context.admitted_node_ids = {1, 2};
  context.admitted_object_uuids = {Uuid(20)};
  return context;
}

public_sblr::SblrCanonicalContainer MakeContainer(
    const sblr::SblrSourceArtifactMapV1& artifact,
    std::vector<std::uint8_t> source_map) {
  public_sblr::SblrCanonicalContainer container;
  const auto engine_uuid = Uuid(5);
  const auto bundle_uuid = Uuid(6);
  std::copy(engine_uuid.begin(), engine_uuid.end(),
            container.canonical_anchor.begin());
  std::copy(artifact.dialect_family_uuid.begin(),
            artifact.dialect_family_uuid.end(),
            container.canonical_anchor.begin() + 16);
  std::copy(artifact.parser_package_uuid.begin(),
            artifact.parser_package_uuid.end(),
            container.canonical_anchor.begin() + 32);
  container.canonical_anchor[48] = 1;
  container.canonical_anchor[52] = 1;
  container.canonical_anchor[60] = 1;
  container.canonical_anchor[68] = 1;
  std::copy(bundle_uuid.begin(), bundle_uuid.end(),
            container.canonical_anchor.begin() + 76);
  container.canonical_anchor[92] = 1;
  container.canonical_anchor[100] = 2;
  std::copy(artifact.container_request_uuid.begin(),
            artifact.container_request_uuid.end(),
            container.canonical_anchor.begin() + 116);
  container.operation_payload = {0x53, 0x42, 0x4f, 0x50};
  container.source_map = std::move(source_map);
  return container;
}

sblr::SblrOperationEnvelope PackageFrame(bool begin) {
  auto frame = sblr::MakeSblrEnvelope(
      begin ? "engine.op.package_begin" : "engine.op.package_end",
      begin ? "SBLR_PACKAGE_BEGIN" : "SBLR_PACKAGE_END",
      begin ? "source_artifact.package.begin"
            : "source_artifact.package.end");
  frame.opcode_code = begin ? 1 : 2;
  frame.result_shape = "void";
  frame.diagnostic_shape = "diagnostic_vector";
  frame.parser_package_uuid = "01000000-0000-7000-8000-000000000004";
  frame.registry_snapshot_uuid = "01000000-0000-7000-8000-000000000008";
  frame.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = begin ? "package.header" : "package.footer";
  operand.name = "package_descriptor";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  const auto package_uuid = Uuid(7);
  operand.value_body.assign(package_uuid.begin(), package_uuid.end());
  frame.operands.push_back(std::move(operand));
  return frame;
}

sblr::SblrOperationEnvelope TransactionBeginMember() {
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.txn_begin", "SBLR_TXN_BEGIN",
      "source_artifact.transaction.begin");
  member.opcode_code = 256;
  member.result_shape = "transaction_handle";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = "01000000-0000-7000-8000-000000000004";
  member.registry_snapshot_uuid = "01000000-0000-7000-8000-000000000008";
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrTransactionBeginOptionsV1 options;
  options.isolation_profile_uuid = Uuid(9);
  options.isolation_profile_generation = 1;
  options.read_mode = 1;
  options.authority_scope = 1;
  options.wait_policy = 1;
  options.transaction_policy_snapshot_uuid = Uuid(10);
  options.transaction_policy_generation = 1;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "transaction.begin_options";
  operand.name = "options";
  operand.value_kind = sblr::SblrValueKind::transaction_begin_options;
  operand.value_body = sblr::EncodeSblrTransactionBeginOptionsV1(&options);
  Require(!operand.value_body.empty(),
          "transaction-begin options did not encode");
  member.operands.push_back(std::move(operand));
  return member;
}

sblr::SblrSourceArtifactMapV1 MakeTransactionArtifact() {
  sblr::SblrSourceArtifactMapV1 artifact;
  artifact.artifact_uuid = Uuid(1);
  artifact.container_request_uuid = Uuid(2);
  artifact.dialect_family_uuid = Uuid(3);
  artifact.parser_package_uuid = Uuid(4);
  artifact.language_tag = "en-CA";
  artifact.redaction_class =
      sblr::SblrSourceArtifactRedactionClassV1::none;
  artifact.decompile_policy =
      sblr::SblrSourceArtifactDecompilePolicyV1::source_preserving;
  sblr::SblrSourceArtifactSpanV1 span;
  span.source_span_id = 1;
  span.node_id = 1;
  span.byte_length = 17;
  span.line_start = 1;
  span.column_start = 1;
  span.line_end = 1;
  span.column_end = 18;
  span.span_kind = sblr::SblrSourceArtifactSpanKindV1::statement;
  artifact.source_spans.push_back(std::move(span));
  sblr::SblrSourceArtifactRenderHintV1 hint;
  hint.render_hint_id = 1;
  hint.node_id = 1;
  hint.dialect_family_uuid = artifact.dialect_family_uuid;
  hint.keyword_case = sblr::SblrSourceArtifactKeywordCaseV1::preserve;
  hint.identifier_render_policy =
      sblr::SblrSourceArtifactIdentifierPolicyV1::preserve_source;
  hint.comment_policy = sblr::SblrSourceArtifactCommentPolicyV1::preserve;
  hint.format_group = "source_preserving_transaction_control_v1";
  artifact.render_hints.push_back(std::move(hint));
  return artifact;
}

std::vector<std::uint8_t> MakeTransactionOpcodeStream(
    bool include_second_executable_member) {
  sblr::SblrOpcodeStream stream;
  stream.package_descriptor_uuid =
      "01000000-0000-7000-8000-000000000007";
  stream.registry_snapshot_uuid =
      "01000000-0000-7000-8000-000000000008";
  stream.operations.push_back(PackageFrame(true));
  stream.operations.push_back(TransactionBeginMember());
  if (include_second_executable_member) {
    stream.operations.push_back(TransactionBeginMember());
  }
  stream.operations.push_back(PackageFrame(false));
  return sblr::EncodeSblrOpcodeStream(stream);
}

void CheckSingleExecutableMemberOpcodeStreamRendering() {
  const auto artifact = MakeTransactionArtifact();
  std::string detail;
  const auto encoded_artifact =
      sblr::EncodeSblrSourceArtifactMapV1(artifact, &detail);
  Require(!encoded_artifact.empty(),
          "transaction source artifact did not encode");
  auto container = MakeContainer(artifact, encoded_artifact);
  container.canonical_anchor[100] = 1;
  container.operation_payload = MakeTransactionOpcodeStream(false);
  Require(!container.operation_payload.empty(),
          "single-member transaction package did not encode");
  const auto bytes = public_sblr::EncodeSblrContainer(container);
  Require(!bytes.empty(), "single-member transaction container did not encode");
  const auto rendered = sblr::RenderSblrContainerToSbsql(
      bytes.data(), bytes.size(),
      sblr::SblrToSbsqlOptions{.source_preserving = true});
  Require(rendered.ok && rendered.diagnostics.empty() &&
              rendered.sbsql_text == "BEGIN TRANSACTION;",
          "single-executable-member package did not source-render");

  container.operation_payload = MakeTransactionOpcodeStream(true);
  Require(!container.operation_payload.empty(),
          "multi-member transaction package did not encode");
  const auto multi_bytes = public_sblr::EncodeSblrContainer(container);
  Require(!multi_bytes.empty(), "multi-member container did not encode");
  const auto refused = sblr::RenderSblrContainerToSbsql(
      multi_bytes.data(), multi_bytes.size(),
      sblr::SblrToSbsqlOptions{.source_preserving = true});
  Require(!refused.ok && !refused.diagnostics.empty() &&
              refused.diagnostics.front().code ==
                  "SBLR.SOURCE_ARTIFACT.INVALID" &&
              refused.diagnostics.front().message ==
                  "source_artifact.opcode_stream_node_profile_unsupported",
          "multi-executable-member package was source-rendered under V1");
}

void CheckCanonicalCodecAndContainer() {
  const auto artifact = MakeArtifact();
  std::string detail;
  const auto encoded =
      sblr::EncodeSblrSourceArtifactMapV1(artifact, &detail);
  Require(!encoded.empty(), "canonical source artifact did not encode");
  const auto decoded =
      sblr::DecodeSblrSourceArtifactMapV1(encoded.data(), encoded.size());
  Require(decoded.status == sblr::SblrSourceArtifactDecodeStatusV1::ok,
          "canonical source artifact did not decode");
  Require(decoded.canonical_bytes == encoded,
          "canonical source artifact decode was not byte-identical");
  Require(decoded.artifact.symbols.size() == 13,
          "source artifact did not preserve all thirteen symbol kinds");
  for (std::size_t index = 0; index < decoded.artifact.symbols.size(); ++index) {
    Require(static_cast<std::uint8_t>(
                decoded.artifact.symbols[index].symbol_kind) == index + 1,
            "source artifact symbol-kind identity drifted");
  }

  auto context = MakeContext(decoded.artifact);
  Require(sblr::ValidateSblrSourceArtifactMapV1(decoded.artifact, context,
                                                &detail),
          "canonical source artifact association validation failed");

  const auto container = MakeContainer(decoded.artifact, encoded);
  const auto container_bytes = public_sblr::EncodeSblrContainer(container);
  Require(!container_bytes.empty(),
          "canonical source-artifact container did not encode");
  const auto decoded_container = public_sblr::DecodeSblrContainerBytes(
      container_bytes.data(), container_bytes.size());
  Require(decoded_container.status == public_sblr::SblrCodecStatus::ok &&
              decoded_container.container.source_map == encoded &&
              decoded_container.container.lowering_metadata.empty(),
          "tag-0x30 source artifact did not round-trip independently");
  Require(public_sblr::EncodeSblrContainer(decoded_container.container) ==
              container_bytes,
          "canonical source-artifact container was not byte-identical");
}

void CheckExternalRetainCarrierCodec() {
  auto artifact = MakeTransactionArtifact();
  artifact.container_request_uuid = {};
  artifact.sblr_envelope_uuid = Uuid(30);
  std::string detail;
  const auto artifact_bytes =
      sblr::EncodeSblrSourceArtifactMapV1(artifact, &detail);
  Require(!artifact_bytes.empty(),
          "external source artifact did not encode");
  sblr::SblrSourceArtifactValidationContextV1 context;
  context.operation_validated_without_artifact = true;
  context.source_preserving_requested = true;
  context.expected_sblr_envelope_uuid = artifact.sblr_envelope_uuid;
  context.expected_dialect_family_uuid = artifact.dialect_family_uuid;
  context.expected_parser_package_uuid = artifact.parser_package_uuid;
  context.admitted_node_ids = {1, 2};
  Require(sblr::ValidateSblrSourceArtifactMapV1(artifact, context, &detail),
          "external source artifact did not validate against SBEE identity");

  sblr::SblrSourceArtifactRetainRequestV1 request;
  request.authenticated_receipt_uuid = Uuid(31);
  request.sblr_envelope_uuid = artifact.sblr_envelope_uuid;
  request.artifact_uuid = artifact.artifact_uuid;
  request.declared_size = artifact_bytes.size();
  request.crc32c =
      public_sblr::SblrCrc32c(artifact_bytes.data(), artifact_bytes.size());
  request.redaction_class = artifact.redaction_class;
  request.decompile_policy = artifact.decompile_policy;
  request.artifact_sha256 = sblr::HashSblrSourceArtifactBytesV1(
      artifact_bytes.data(), artifact_bytes.size());
  request.canonical_artifact_bytes = artifact_bytes;
  const auto request_bytes =
      sblr::EncodeSblrSourceArtifactRetainRequestV1(request, &detail);
  Require(!request_bytes.empty(), "SARQ retain request did not encode");
  sblr::SblrSourceArtifactRetainRequestV1 decoded_request;
  Require(sblr::DecodeSblrSourceArtifactRetainRequestV1(
              request_bytes.data(), request_bytes.size(), &decoded_request,
              &detail) &&
              decoded_request.authenticated_receipt_uuid ==
                  request.authenticated_receipt_uuid &&
              decoded_request.sblr_envelope_uuid ==
                  request.sblr_envelope_uuid &&
              decoded_request.artifact_uuid == request.artifact_uuid &&
              decoded_request.canonical_artifact_bytes == artifact_bytes,
          "SARQ retain request did not round-trip exactly");
  Require(sblr::EncodeSblrSourceArtifactRetainRequestV1(decoded_request,
                                                         &detail) ==
              request_bytes,
          "SARQ retain request was not byte-identical after decode");

  sblr::SblrSourceArtifactRetainAckV1 ack;
  ack.authenticated_receipt_uuid = request.authenticated_receipt_uuid;
  ack.sblr_envelope_uuid = request.sblr_envelope_uuid;
  ack.artifact_uuid = request.artifact_uuid;
  ack.declared_size = request.declared_size;
  ack.crc32c = request.crc32c;
  ack.redaction_class = request.redaction_class;
  ack.decompile_policy = request.decompile_policy;
  ack.artifact_sha256 = request.artifact_sha256;
  ack.retention_generation = 1;
  const auto ack_bytes =
      sblr::EncodeSblrSourceArtifactRetainAckV1(ack, &detail);
  Require(ack_bytes.size() == sblr::kSblrSourceArtifactRetainAckSizeV1,
          "SARA retain acknowledgement did not encode at exact extent");
  sblr::SblrSourceArtifactRetainAckV1 decoded_ack;
  Require(sblr::DecodeSblrSourceArtifactRetainAckV1(
              ack_bytes.data(), ack_bytes.size(), &decoded_ack, &detail) &&
              decoded_ack.artifact_uuid == request.artifact_uuid &&
              decoded_ack.retention_generation == 1 &&
              decoded_ack.artifact_sha256 == request.artifact_sha256,
          "SARA retain acknowledgement did not round-trip exactly");
  Require(sblr::EncodeSblrSourceArtifactRetainAckV1(decoded_ack, &detail) ==
              ack_bytes,
          "SARA retain acknowledgement was not byte-identical after decode");

  auto corrupt_request = request_bytes;
  corrupt_request.back() ^= 0x01;
  Require(!sblr::DecodeSblrSourceArtifactRetainRequestV1(
              corrupt_request.data(), corrupt_request.size(),
              &decoded_request, &detail),
          "tampered SARQ artifact bytes were accepted");
  auto corrupt_ack = ack_bytes;
  corrupt_ack.back() ^= 0x01;
  Require(!sblr::DecodeSblrSourceArtifactRetainAckV1(
              corrupt_ack.data(), corrupt_ack.size(), &decoded_ack, &detail),
          "tampered SARA acknowledgement evidence was accepted");
}

void CheckCodecRefusals() {
  auto artifact = MakeArtifact();
  std::string detail;

  auto non_dense = artifact;
  non_dense.symbols[1].symbol_id = 7;
  Require(sblr::EncodeSblrSourceArtifactMapV1(non_dense, &detail).empty() &&
              detail == "id",
          "non-dense source symbol IDs were accepted");

  auto invalid_utf8 = artifact;
  invalid_utf8.symbols[0].raw_name_utf8 = std::string("\xc0\x80", 2);
  Require(sblr::EncodeSblrSourceArtifactMapV1(invalid_utf8, &detail).empty() &&
              detail == "utf8",
          "invalid source-artifact UTF-8 was accepted");

  auto object_on_local_symbol = artifact;
  object_on_local_symbol.symbols[0].related_object_uuid = Uuid(20);
  Require(sblr::EncodeSblrSourceArtifactMapV1(object_on_local_symbol, &detail)
                  .empty() &&
              detail == "object_ref",
          "local source symbol accepted catalog-object authority");

  auto object_display_without_object = artifact;
  object_display_without_object.symbols[11].related_object_uuid = {};
  Require(sblr::EncodeSblrSourceArtifactMapV1(
              object_display_without_object, &detail)
                  .empty() &&
              detail == "object_ref",
          "object display symbol without admitted object identity was accepted");

  auto hash_only_name_leak = artifact;
  hash_only_name_leak.symbols[0].redaction_state =
      sblr::SblrSourceArtifactSymbolRedactionV1::hash_only;
  Require(sblr::EncodeSblrSourceArtifactMapV1(hash_only_name_leak, &detail)
                  .empty() &&
              detail == "redaction",
          "hash-only source symbol retained protected name bytes");

  const auto encoded =
      sblr::EncodeSblrSourceArtifactMapV1(artifact, &detail);
  Require(!encoded.empty(), "source artifact negative baseline did not encode");
  auto corrupt = encoded;
  corrupt.back() ^= 0x01;
  const auto corrupt_decoded =
      sblr::DecodeSblrSourceArtifactMapV1(corrupt.data(), corrupt.size());
  Require(corrupt_decoded.status ==
              sblr::SblrSourceArtifactDecodeStatusV1::invalid &&
              corrupt_decoded.detail == "record_hash",
          "source artifact record-hash corruption was accepted");

  auto artifact_hash_corrupt = encoded;
  artifact_hash_corrupt[sblr::kSblrSourceArtifactHeaderSizeV1] ^= 0x01;
  const auto artifact_hash_decoded =
      sblr::DecodeSblrSourceArtifactMapV1(artifact_hash_corrupt.data(),
                                         artifact_hash_corrupt.size());
  Require(artifact_hash_decoded.status ==
              sblr::SblrSourceArtifactDecodeStatusV1::invalid &&
              artifact_hash_decoded.detail == "artifact_hash",
          "source artifact packet-hash corruption was accepted");

  auto trailing = encoded;
  trailing.push_back(0);
  const auto trailing_decoded =
      sblr::DecodeSblrSourceArtifactMapV1(trailing.data(), trailing.size());
  Require(trailing_decoded.status ==
              sblr::SblrSourceArtifactDecodeStatusV1::invalid,
          "source artifact trailing bytes were accepted");

  std::vector<std::uint8_t> oversized(
      sblr::kSblrSourceArtifactMaximumBytesV1 + 1, 0);
  const auto oversized_decoded = sblr::DecodeSblrSourceArtifactMapV1(
      oversized.data(), oversized.size());
  Require(oversized_decoded.status ==
              sblr::SblrSourceArtifactDecodeStatusV1::resource_exceeded,
          "source artifact resource limit was not enforced first");
}

void CheckAssociationAndPolicyRefusals() {
  const auto artifact = MakeArtifact();
  std::string detail;
  auto context = MakeContext(artifact);

  auto wrong_binding = context;
  wrong_binding.expected_container_request_uuid = Uuid(99);
  Require(!sblr::ValidateSblrSourceArtifactMapV1(
              artifact, wrong_binding, &detail) &&
              detail == "binding",
          "source artifact container-binding mismatch was accepted");

  auto wrong_node = artifact;
  wrong_node.symbols[0].use_node_ids = {999};
  Require(!sblr::ValidateSblrSourceArtifactMapV1(
              wrong_node, context, &detail) &&
              detail == "node_ref",
          "source artifact unknown node reference was accepted");

  auto wrong_object = artifact;
  wrong_object.symbols[11].related_object_uuid = Uuid(98);
  Require(!sblr::ValidateSblrSourceArtifactMapV1(
              wrong_object, context, &detail) &&
              detail == "object_ref",
          "source artifact unknown object reference was accepted");

  auto redacted = artifact;
  redacted.redaction_class =
      sblr::SblrSourceArtifactRedactionClassV1::diagnostic_safe;
  Require(!sblr::ValidateSblrSourceArtifactMapV1(
              redacted, context, &detail) &&
              detail == "redaction",
          "redacted artifact was accepted for source-preserving reversal");

  auto individually_redacted = artifact;
  individually_redacted.symbols[0].redaction_state =
      sblr::SblrSourceArtifactSymbolRedactionV1::placeholder;
  Require(!sblr::ValidateSblrSourceArtifactMapV1(
              individually_redacted, context, &detail) &&
              detail == "redaction",
          "individually redacted symbol was accepted for source-preserving reversal");

  auto unmarked_security_node = artifact;
  unmarked_security_node.redaction_class =
      sblr::SblrSourceArtifactRedactionClassV1::security_redacted;
  unmarked_security_node.symbols[0].use_node_ids = {999};
  auto non_render_context = context;
  non_render_context.source_preserving_requested = false;
  Require(!sblr::ValidateSblrSourceArtifactMapV1(
              unmarked_security_node, non_render_context, &detail) &&
              detail == "node_ref",
          "unmarked security-redacted node placeholder was accepted");
  unmarked_security_node.symbols[0].redaction_state =
      sblr::SblrSourceArtifactSymbolRedactionV1::placeholder;
  Require(sblr::ValidateSblrSourceArtifactMapV1(
              unmarked_security_node, non_render_context, &detail),
          "explicit security-redacted node placeholder was refused");

  auto unvalidated_context = context;
  unvalidated_context.operation_validated_without_artifact = false;
  Require(!sblr::ValidateSblrSourceArtifactMapV1(
              artifact, unvalidated_context, &detail) &&
              detail == "authority",
          "source artifact repaired missing operation authority");
}

void CheckCarrierBoundaryRefusals() {
  const auto artifact = MakeArtifact();
  std::string detail;
  const auto encoded =
      sblr::EncodeSblrSourceArtifactMapV1(artifact, &detail);
  Require(!encoded.empty(), "carrier-boundary artifact did not encode");

  auto duplicate_channel = MakeContainer(artifact, encoded);
  duplicate_channel.lowering_metadata = encoded;
  const auto duplicate_bytes =
      public_sblr::EncodeSblrContainer(duplicate_channel);
  Require(!duplicate_bytes.empty(),
          "duplicate-channel container fixture did not encode");
  const auto duplicate_result = sblr::RenderSblrContainerToSbsql(
      duplicate_bytes.data(), duplicate_bytes.size(),
      sblr::SblrToSbsqlOptions{.source_preserving = true});
  Require(!duplicate_result.ok && !duplicate_result.diagnostics.empty() &&
              duplicate_result.diagnostics.front().code ==
                  "SBLR.SOURCE_ARTIFACT.INVALID" &&
              duplicate_result.diagnostics.front().message ==
                  "source_artifact.channel_duplicate_or_misplaced",
          "tag-0x12 source artifact duplication was not refused");

  auto invalid_operation = MakeContainer(artifact, encoded);
  invalid_operation.operation_payload = {'n', 'o', 't', '-', 's', 'b', 'o', 'p'};
  const auto invalid_bytes =
      public_sblr::EncodeSblrContainer(invalid_operation);
  Require(!invalid_bytes.empty(),
          "invalid-operation container fixture did not encode");
  const auto invalid_result = sblr::RenderSblrContainerToSbsql(
      invalid_bytes.data(), invalid_bytes.size(),
      sblr::SblrToSbsqlOptions{.source_preserving = true});
  Require(!invalid_result.ok && !invalid_result.diagnostics.empty() &&
              invalid_result.diagnostics.front().code !=
                  "SBLR.SOURCE_ARTIFACT.INVALID",
          "source artifact replaced invalid operation authority");
}

}  // namespace

int main() {
  CheckCanonicalCodecAndContainer();
  CheckExternalRetainCarrierCodec();
  CheckCodecRefusals();
  CheckAssociationAndPolicyRefusals();
  CheckCarrierBoundaryRefusals();
  CheckSingleExecutableMemberOpcodeStreamRendering();
  std::cout << "sbsql_sblr_source_artifact_container_conformance=passed\n";
  return EXIT_SUCCESS;
}
