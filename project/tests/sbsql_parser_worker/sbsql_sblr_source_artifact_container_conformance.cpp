// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_source_artifact_runtime.hpp"
#include "sblr_to_sbsql.hpp"

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
  CheckCodecRefusals();
  CheckAssociationAndPolicyRefusals();
  CheckCarrierBoundaryRefusals();
  std::cout << "sbsql_sblr_source_artifact_container_conformance=passed\n";
  return EXIT_SUCCESS;
}
