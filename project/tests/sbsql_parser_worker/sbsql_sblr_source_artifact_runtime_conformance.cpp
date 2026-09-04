// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_engine_envelope.hpp"
#include "sblr_source_map_runtime.hpp"

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

namespace sblr = scratchbird::engine::sblr;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "require_failed: " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

template <typename Result>
bool HasDiagnostic(const Result& result, std::string_view code) {
  return std::ranges::any_of(result.diagnostics, [&](const auto& diagnostic) {
    return diagnostic.code == code;
  });
}

sblr::SblrSourceMapUuidV1 UuidBytes(std::uint8_t seed) {
  sblr::SblrSourceMapUuidV1 value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

sblr::SblrSourceMapSha256V1 HashBytes(std::uint8_t seed) {
  sblr::SblrSourceMapSha256V1 value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

void AppendU64Le(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

sblr::SblrSourceMapDescriptorVectorV1 SourceMapVector() {
  sblr::SblrSourceMapDescriptorVectorV1 vector;
  vector.descriptor_uuid = UuidBytes(0x10);
  vector.descriptor_generation = 1;
  vector.registry_snapshot_uuid = UuidBytes(0x30);
  vector.registry_generation = 7;
  vector.statement_receipt_uuid = UuidBytes(0x50);
  vector.bound_ast_sha256 = HashBytes(0x70);
  for (std::uint64_t ordinal = 1; ordinal <= 13; ++ordinal) {
    sblr::SblrSourceMapEntryV1 entry;
    entry.node_id = ordinal;
    entry.source_artifact_uuid =
        UuidBytes(static_cast<std::uint8_t>(0x90 + ordinal));
    entry.source_artifact_generation = 1;
    entry.byte_offset = (ordinal - 1) * 8;
    entry.byte_length = 8;
    entry.line = static_cast<std::uint32_t>(ordinal);
    entry.column = 1;
    entry.redaction_class = 0;
    vector.entries.push_back(entry);
  }
  return vector;
}

sblr::SblrOperationEnvelope SourceMapCarrier(
    const sblr::SblrSourceMapDescriptorVectorV1& vector) {
  auto envelope = sblr::MakeSblrEnvelope(
      "engine.op.source_map", "SBLR_SOURCE_MAP",
      "SBLR-SOURCE-MAP-SBOP-BOUNDARY-V1");
  envelope.opcode_code = 6;
  envelope.result_shape = "void";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      "019dffbb-f000-7000-8000-000000000010";
  envelope.registry_snapshot_uuid =
      "019dffbb-f000-7000-8000-000000000011";
  envelope.parser_resolved_names_to_uuids = true;

  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "source_map.vector";
  operand.name = "source_map";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body.assign(vector.descriptor_uuid.begin(),
                            vector.descriptor_uuid.end());
  AppendU64Le(&operand.value_body, vector.descriptor_generation);
  envelope.operands.push_back(std::move(operand));
  return envelope;
}

void CheckCanonicalSourceMapCarrier() {
  auto vector = SourceMapVector();
  const auto encoded_vector =
      sblr::EncodeSblrSourceMapDescriptorVectorV1(&vector);
  Require(encoded_vector.size() == 152 + 13 * 104,
          "SMVD did not use the exact header and entry extents");
  const auto decoded_vector = sblr::DecodeSblrSourceMapDescriptorVectorV1(
      encoded_vector.data(), encoded_vector.size());
  Require(decoded_vector.status == sblr::SblrSourceMapDecodeStatusV1::ok &&
              decoded_vector.canonical_bytes == encoded_vector &&
              decoded_vector.vector.entries.size() == 13 &&
              decoded_vector.vector.vector_sha256 == vector.vector_sha256,
          "canonical SMVD source map did not round trip byte-identically");

  auto tampered_vector = encoded_vector;
  tampered_vector[120] ^= 0x01U;
  Require(sblr::DecodeSblrSourceMapDescriptorVectorV1(
              tampered_vector.data(), tampered_vector.size())
              .status != sblr::SblrSourceMapDecodeStatusV1::ok,
          "tampered SMVD vector hash was accepted");

  const auto envelope = SourceMapCarrier(vector);
  const auto validation = sblr::ValidateSblrEnvelope(envelope);
  Require(validation.ok,
          "exact SBLR_SOURCE_MAP descriptor-reference carrier was rejected");

  const auto encoded_envelope = sblr::EncodeSblrEnvelope(envelope);
  Require(!encoded_envelope.empty(),
          "exact source-map SBOP carrier did not encode");
  const auto decoded_envelope = sblr::DecodeSblrEnvelope(encoded_envelope);
  Require(decoded_envelope.ok &&
              decoded_envelope.envelope.operation_id ==
                  "engine.op.source_map" &&
              decoded_envelope.envelope.opcode == "SBLR_SOURCE_MAP" &&
              decoded_envelope.envelope.opcode_code == 6 &&
              decoded_envelope.envelope.operands.size() == 1 &&
              decoded_envelope.envelope.operands.front().value_body ==
                  envelope.operands.front().value_body &&
              decoded_envelope.envelope.source_artifact_map.policy_status ==
                  "absent" &&
              decoded_envelope.envelope.source_artifact_map.symbols.empty() &&
              decoded_envelope.envelope.source_artifact_map
                  .operation_render_hints.empty(),
          "source-map descriptor carrier changed identity or gained SBOP metadata");
}

void RequireDuplicateIngressRefusal(
    const sblr::SblrOperationEnvelope& envelope,
    std::string_view detail) {
  const auto validation = sblr::ValidateSblrEnvelope(envelope);
  Require(!validation.ok &&
              HasDiagnostic(
                  validation,
                  "SBLR.OPERATION.DUPLICATE_INGRESS_AUTHORITY"),
          detail);
}

void CheckLegacySbopArtifactRefusal() {
  const auto vector = SourceMapVector();

  auto metadata = SourceMapCarrier(vector);
  metadata.source_artifact_map.policy_status =
      "non_authoritative_render_metadata";
  metadata.source_artifact_map.source_identity =
      "SBSQL-SOURCE-ARTIFACT-LEGACY";
  metadata.source_artifact_map.source_hash =
      "sha256:legacy-source-artifact-map";
  RequireDuplicateIngressRefusal(
      metadata, "legacy source metadata was accepted inside SBOP");

  auto symbol = SourceMapCarrier(vector);
  symbol.source_artifact_map.symbols.push_back(
      {"variable", "var.v", {}, "v", "procedure.local",
       "sha256:legacy-source-map-segment", false, false});
  RequireDuplicateIngressRefusal(
      symbol, "legacy source symbol was accepted inside SBOP");

  auto render_hint = SourceMapCarrier(vector);
  render_hint.source_artifact_map.operation_render_hints.push_back(
      {"operation", "engine.op.source_map", "render_as_source", false,
       false});
  RequireDuplicateIngressRefusal(
      render_hint, "legacy render hint was accepted inside SBOP");

  auto authoritative = SourceMapCarrier(vector);
  authoritative.source_artifact_map.raw_sql_text_authoritative = true;
  RequireDuplicateIngressRefusal(
      authoritative, "source-text authority was accepted inside SBOP");

  auto contained_sql = SourceMapCarrier(vector);
  contained_sql.source_artifact_map.contains_sql_text = true;
  RequireDuplicateIngressRefusal(
      contained_sql, "source SQL marker was accepted inside SBOP");

  auto envelope_sql = SourceMapCarrier(vector);
  envelope_sql.contains_sql_text = true;
  RequireDuplicateIngressRefusal(
      envelope_sql, "SQL text was accepted as SBOP authority");

  const auto encoded_legacy = sblr::EncodeSblrEnvelope(metadata);
  Require(encoded_legacy.empty(),
          "invalid legacy source metadata was serialized as canonical SBOP");
}

}  // namespace

int main() {
  CheckCanonicalSourceMapCarrier();
  CheckLegacySbopArtifactRefusal();
  std::cout << "sbsql_sblr_source_map_sbop_boundary=passed\n";
  return EXIT_SUCCESS;
}
