// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "contextual_text_literal_v2_codec.hpp"

#include "hash_digest.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <string_view>
#include <tuple>

namespace scratchbird::engine::sblr {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr ContextualTextUuidV2 kTextDescriptorUuid = {
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x18};
constexpr ContextualTextUuidV2 kTextTypeUuid = {
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x19};
constexpr ContextualTextUuidV2 kTextCodecUuid = {
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x1a};
constexpr ContextualTextUuidV2 kDatatypeCatalogUuid = {
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x01};

bool Fail(ContextualTextCodecDiagnosticV2* diagnostic,
          std::string code,
          std::string detail) {
  if (diagnostic != nullptr) {
    diagnostic->code = std::move(code);
    diagnostic->detail = std::move(detail);
  }
  return false;
}

void Clear(ContextualTextCodecDiagnosticV2* diagnostic) {
  if (diagnostic != nullptr) *diagnostic = {};
}

bool Nonzero(const ContextualTextUuidV2& value) {
  return std::any_of(value.begin(), value.end(),
                     [](std::uint8_t byte) { return byte != 0; });
}

bool CheckedAdd(std::size_t left, std::size_t right, std::size_t* out) {
  if (out == nullptr || right > std::numeric_limits<std::size_t>::max() - left)
    return false;
  *out = left + right;
  return true;
}

std::uint16_t GetU16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8);
}

std::uint32_t GetU32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i != 4; ++i)
    value |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
  return value;
}

std::uint64_t GetU64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i != 8; ++i)
    value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
  return value;
}

void PutU16(Bytes* bytes, std::size_t offset, std::uint16_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void PutU32(Bytes* bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned i = 0; i != 4; ++i)
    (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void PutU64(Bytes* bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned i = 0; i != 8; ++i)
    (*bytes)[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

template <std::size_t N>
void GetArray(const std::uint8_t* bytes, std::array<std::uint8_t, N>* out) {
  std::copy_n(bytes, N, out->begin());
}

template <std::size_t N>
void PutArray(Bytes* bytes,
              std::size_t offset,
              const std::array<std::uint8_t, N>& value) {
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

bool AllZero(const std::uint8_t* bytes, std::size_t size) {
  return std::all_of(bytes, bytes + size,
                     [](std::uint8_t byte) { return byte == 0; });
}

bool ExactMagic(const std::uint8_t* bytes, const char (&magic)[9]) {
  return std::equal(bytes, bytes + 8,
                    reinterpret_cast<const std::uint8_t*>(magic));
}

void PutMagic(Bytes* bytes, const char (&magic)[9]) {
  std::copy_n(reinterpret_cast<const std::uint8_t*>(magic), 8,
              bytes->begin());
}

void AppendU16(Bytes* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value));
  bytes->push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(Bytes* bytes, std::uint32_t value) {
  for (unsigned i = 0; i != 4; ++i)
    bytes->push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

void AppendU64(Bytes* bytes, std::uint64_t value) {
  for (unsigned i = 0; i != 8; ++i)
    bytes->push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

template <std::size_t N>
void AppendArray(Bytes* bytes, const std::array<std::uint8_t, N>& value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

void AppendDomain(Bytes* bytes, std::string_view domain) {
  bytes->insert(bytes->end(), domain.begin(), domain.end());
  bytes->push_back(0);
}

ContextualTextSha256V2 Sha256(const Bytes& bytes) {
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

ContextualTextSha256V2 HashRawToken(
    const ContextualTextLiteralDemandV2& demand) {
  Bytes material;
  AppendDomain(&material, "ScratchBird.ContextualText.RawToken.V2");
  AppendU64(&material, demand.literal_occurrence);
  AppendU64(&material, demand.node_id);
  AppendU32(&material, demand.literal_descriptor_handle);
  AppendU32(&material, static_cast<std::uint32_t>(demand.raw_token.size()));
  material.insert(material.end(), demand.raw_token.begin(),
                  demand.raw_token.end());
  return Sha256(material);
}

ContextualTextSha256V2 HashLexicalValue(
    const ContextualTextLiteralDemandV2& demand) {
  Bytes material;
  AppendDomain(&material, "ScratchBird.ContextualText.LexicalValue.V2");
  AppendU64(&material, demand.literal_occurrence);
  AppendU64(&material, demand.node_id);
  AppendU32(&material, demand.literal_descriptor_handle);
  AppendU32(&material,
            static_cast<std::uint32_t>(demand.lexical_value.size()));
  AppendU32(&material, demand.scalar_count);
  material.insert(material.end(), demand.lexical_value.begin(),
                  demand.lexical_value.end());
  return Sha256(material);
}

ContextualTextSha256V2 HashCanonicalBody(
    const ContextualTextLiteralProfileV2& profile) {
  Bytes material;
  AppendDomain(&material, "ScratchBird.ContextualText.CanonicalBody.V2");
  AppendArray(&material, profile.descriptor_uuid);
  AppendU64(&material, profile.descriptor_generation);
  AppendArray(&material, profile.codec_uuid);
  AppendU16(&material, profile.codec_version);
  AppendU64(&material, profile.codec_generation);
  AppendU32(&material,
            static_cast<std::uint32_t>(profile.canonical_body.size()));
  AppendU32(&material, profile.scalar_count);
  material.insert(material.end(), profile.canonical_body.begin(),
                  profile.canonical_body.end());
  return Sha256(material);
}

bool DecodeOneUtf8(const std::uint8_t* bytes,
                   std::size_t available,
                   std::size_t* width) {
  if (bytes == nullptr || width == nullptr || available == 0) return false;
  const std::uint8_t first = bytes[0];
  if (first <= 0x7f) {
    if (first == 0) return false;
    *width = 1;
    return true;
  }
  if (first >= 0xc2 && first <= 0xdf) {
    if (available < 2 || (bytes[1] & 0xc0) != 0x80) return false;
    *width = 2;
    return true;
  }
  if (first >= 0xe0 && first <= 0xef) {
    if (available < 3 || (bytes[2] & 0xc0) != 0x80) return false;
    const std::uint8_t second = bytes[1];
    if ((first == 0xe0 && (second < 0xa0 || second > 0xbf)) ||
        (first == 0xed && (second < 0x80 || second > 0x9f)) ||
        ((first != 0xe0 && first != 0xed) && (second & 0xc0) != 0x80))
      return false;
    *width = 3;
    return true;
  }
  if (first >= 0xf0 && first <= 0xf4) {
    if (available < 4 || (bytes[2] & 0xc0) != 0x80 ||
        (bytes[3] & 0xc0) != 0x80)
      return false;
    const std::uint8_t second = bytes[1];
    if ((first == 0xf0 && (second < 0x90 || second > 0xbf)) ||
        (first == 0xf4 && (second < 0x80 || second > 0x8f)) ||
        ((first != 0xf0 && first != 0xf4) && (second & 0xc0) != 0x80))
      return false;
    *width = 4;
    return true;
  }
  return false;
}

bool ValidateUtf8Body(const Bytes& bytes, std::uint32_t* scalar_count) {
  std::uint64_t count = 0;
  for (std::size_t offset = 0; offset < bytes.size();) {
    std::size_t width = 0;
    if (!DecodeOneUtf8(bytes.data() + offset, bytes.size() - offset, &width))
      return false;
    offset += width;
    if (++count > kContextualTextMaximumBodyBytesV2) return false;
  }
  if (scalar_count != nullptr) *scalar_count = static_cast<std::uint32_t>(count);
  return true;
}

bool ValidOptionalPair(const ContextualTextUuidV2& uuid,
                       std::uint64_t generation) {
  return Nonzero(uuid) == (generation != 0);
}

bool ValidRequiredPair(const ContextualTextUuidV2& uuid,
                       std::uint64_t generation) {
  return Nonzero(uuid) && generation != 0;
}

}  // namespace

bool DecodeContextualTextRawTokenV2(
    const std::uint8_t* bytes,
    std::size_t size,
    ContextualTextRawTokenV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr || bytes == nullptr || size < 2 ||
      size > kContextualTextMaximumRawTokenBytesV2)
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "contextual TEXT raw token extent is invalid");
  *out = {};
  if (bytes[0] != 0x27 || bytes[size - 1] != 0x27)
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "contextual TEXT raw token is not exactly single quoted");
  std::uint64_t scalar_count = 0;
  for (std::size_t offset = 1; offset < size - 1;) {
    if (bytes[offset] == 0x27) {
      if (offset + 1 >= size - 1 || bytes[offset + 1] != 0x27)
        return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                    "contextual TEXT raw token has a lone interior quote");
      out->decoded_utf8.push_back(0x27);
      offset += 2;
      ++scalar_count;
    } else {
      std::size_t width = 0;
      if (!DecodeOneUtf8(bytes + offset, size - 1 - offset, &width))
        return Fail(diagnostic, "CTB.TEXT.INVALID_ENCODING",
                    "contextual TEXT raw token is not shortest-form UTF-8");
      if (bytes[offset] == 0)
        return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                    "contextual TEXT raw token contains U+0000");
      out->decoded_utf8.insert(out->decoded_utf8.end(), bytes + offset,
                               bytes + offset + width);
      offset += width;
      ++scalar_count;
    }
    if (out->decoded_utf8.size() > kContextualTextMaximumBodyBytesV2 ||
        scalar_count > kContextualTextMaximumBodyBytesV2)
      return Fail(diagnostic, "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED",
                  "contextual TEXT decoded token exceeds the V2 body bound");
  }
  out->scalar_count = static_cast<std::uint32_t>(scalar_count);
  return true;
}

ContextualTextSha256V2 ComputeContextualTextDemandSequenceSha256V2(
    const std::vector<std::uint8_t>& exact_record_vector,
    std::uint32_t count) {
  Bytes material;
  AppendDomain(&material, "ScratchBird.ContextualText.DemandSequence.V2");
  AppendU32(&material, count);
  AppendU32(&material,
            static_cast<std::uint32_t>(exact_record_vector.size()));
  material.insert(material.end(), exact_record_vector.begin(),
                  exact_record_vector.end());
  return Sha256(material);
}

ContextualTextSha256V2 ComputeContextualTextRawTokenSha256V2(
    const ContextualTextLiteralDemandV2& demand) {
  return HashRawToken(demand);
}

ContextualTextSha256V2 ComputeContextualTextLexicalValueSha256V2(
    const ContextualTextLiteralDemandV2& demand) {
  return HashLexicalValue(demand);
}

ContextualTextSha256V2 ComputeContextualTextCanonicalBodySha256V2(
    const ContextualTextLiteralProfileV2& profile) {
  return HashCanonicalBody(profile);
}

ContextualTextSha256V2 ComputeContextualTextTargetProjectionSha256V2(
    const ContextualTextLiteralDemandV2& demand,
    const ContextualTextUuidV2& source_occurrence_uuid,
    std::uint64_t source_generation,
    const std::vector<std::uint8_t>& exact_public_relation_projection_v3) {
  Bytes material;
  AppendDomain(&material, "ScratchBird.ContextualText.TargetProjection.V2");
  AppendU64(&material, demand.source_node_id);
  AppendU32(&material, demand.source_operand_ordinal);
  AppendU32(&material, demand.source_ordinal);
  AppendArray(&material, source_occurrence_uuid);
  AppendU64(&material, source_generation);
  AppendU32(&material, demand.target_descriptor_handle);
  AppendU32(&material, demand.literal_descriptor_handle);
  AppendU32(&material, static_cast<std::uint32_t>(
                           exact_public_relation_projection_v3.size()));
  material.insert(material.end(), exact_public_relation_projection_v3.begin(),
                  exact_public_relation_projection_v3.end());
  return Sha256(material);
}

ContextualTextSha256V2 ComputeContextualTextTargetContextSha256V2(
    const ContextualTextLiteralNegotiationRequestV2& request,
    const ContextualTextLiteralDemandV2& demand,
    const ContextualTextUuidV2& source_occurrence_uuid,
    std::uint64_t source_generation,
    const ContextualTextDescriptorV2& descriptor,
    const ContextualTextSha256V2& target_projection_sha256,
    const ContextualTextSha256V2& descriptor_evidence_sha256) {
  Bytes material;
  AppendDomain(&material, "ScratchBird.ContextualText.TargetContext.V2");
  AppendArray(&material, request.statement_receipt_uuid);
  AppendArray(&material, request.catalog_snapshot_uuid);
  AppendU64(&material, request.catalog_generation);
  AppendU64(&material, request.datatype_registry_generation);
  AppendU64(&material, request.security_generation);
  AppendU64(&material, request.resource_epoch);
  AppendArray(&material, request.mga_snapshot_uuid);
  AppendU64(&material, demand.source_node_id);
  AppendU32(&material, demand.source_operand_ordinal);
  AppendU32(&material, demand.source_ordinal);
  AppendArray(&material, source_occurrence_uuid);
  AppendU64(&material, source_generation);
  AppendArray(&material, demand.relation_uuid);
  AppendArray(&material, demand.relation_descriptor_uuid);
  AppendU64(&material, demand.relation_descriptor_generation);
  AppendArray(&material, demand.column_uuid);
  AppendU32(&material, demand.column_ordinal);
  AppendU64(&material, demand.comparison_occurrence);
  AppendU32(&material, demand.parent_operand_ordinal);
  material.push_back(demand.literal_argument_ordinal);
  material.push_back(demand.target_argument_ordinal);
  AppendU64(&material, demand.literal_occurrence);
  AppendU64(&material, demand.node_id);
  AppendU32(&material, demand.target_descriptor_handle);
  AppendU32(&material, demand.literal_descriptor_handle);
  AppendArray(&material, descriptor.descriptor_uuid);
  AppendU64(&material, descriptor.descriptor_generation);
  AppendArray(&material, descriptor.type_uuid);
  AppendU64(&material, descriptor.type_generation);
  AppendArray(&material, descriptor.codec_uuid);
  AppendU16(&material, kContextualTextCodecIdentifierBytesV2);
  material.insert(material.end(),
                  reinterpret_cast<const std::uint8_t*>(
                      kContextualTextCodecIdentifierV2),
                  reinterpret_cast<const std::uint8_t*>(
                      kContextualTextCodecIdentifierV2) +
                      kContextualTextCodecIdentifierBytesV2);
  AppendU16(&material, descriptor.codec_version);
  AppendU64(&material, descriptor.codec_generation);
  AppendArray(&material, target_projection_sha256);
  AppendArray(&material, descriptor_evidence_sha256);
  return Sha256(material);
}

ContextualTextSha256V2 ComputeContextualTextPreContextualOperandVectorSha256V2(
    const std::vector<std::uint8_t>& exact_top_level_operand_records,
    std::uint32_t operand_count) {
  Bytes material;
  AppendDomain(
      &material,
      "ScratchBird.ContextualText.PreContextualQueryOperandVector.V2");
  AppendU16(&material, 4615);
  AppendU16(&material, 1);
  AppendU16(&material, 1);
  AppendU32(&material, operand_count);
  material.insert(material.end(), exact_top_level_operand_records.begin(),
                  exact_top_level_operand_records.end());
  return Sha256(material);
}

ContextualTextSha256V2 ComputeContextualTextDescriptorEvidenceSha256V2(
    const std::vector<std::uint8_t>& exact_descriptor) {
  Bytes material;
  AppendDomain(&material, "ScratchBird.ContextualText.TextDescriptor.V2");
  Bytes canonical = exact_descriptor;
  if (canonical.size() >= 480)
    std::fill(canonical.begin() + 448, canonical.begin() + 480, 0);
  material.insert(material.end(), canonical.begin(), canonical.end());
  return Sha256(material);
}

ContextualTextSha256V2 ComputeContextualTextBindingSha256V2(
    const std::vector<std::uint8_t>& exact_profile) {
  Bytes material;
  AppendDomain(&material, "ScratchBird.ContextualText.BindingProfile.V2");
  Bytes canonical = exact_profile;
  if (canonical.size() >= 912)
    std::fill(canonical.begin() + 880, canonical.begin() + 912, 0);
  material.insert(material.end(), canonical.begin(), canonical.end());
  return Sha256(material);
}

ContextualTextSha256V2 ComputeContextualTextResultCarrierSha256V2(
    const std::vector<std::uint8_t>& exact_result) {
  Bytes material;
  AppendDomain(&material, "ScratchBird.ContextualText.ResultCarrier.V2");
  Bytes canonical = exact_result;
  if (canonical.size() >= 304)
    std::fill(canonical.begin() + 272, canonical.begin() + 304, 0);
  material.insert(material.end(), canonical.begin(), canonical.end());
  return Sha256(material);
}

ContextualTextSha256V2 ComputeContextualTextExecuteCarrierSha256V2(
    const std::vector<std::uint8_t>& exact_execute) {
  Bytes material;
  AppendDomain(&material, "ScratchBird.ContextualText.ExecuteCarrier.V2");
  Bytes canonical = exact_execute;
  if (canonical.size() >= 368)
    std::fill(canonical.begin() + 336, canonical.begin() + 368, 0);
  material.insert(material.end(), canonical.begin(), canonical.end());
  return Sha256(material);
}

ContextualTextSha256V2 ComputeContextualTextSbxnSha256V2(
    const std::vector<std::uint8_t>& exact_sbxn) {
  return Sha256(exact_sbxn);
}

bool EncodeContextualTextLiteralNegotiationRequestV2(
    const ContextualTextLiteralNegotiationRequestV2& value,
    std::vector<std::uint8_t>* out,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr)
    return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                "contextual TEXT request output is null");
  out->clear();
  if (!Nonzero(value.statement_receipt_uuid) ||
      !Nonzero(value.catalog_snapshot_uuid) || value.catalog_generation == 0 ||
      value.datatype_registry_generation == 0 ||
      value.security_generation == 0 || value.resource_epoch == 0 ||
      !Nonzero(value.mga_snapshot_uuid) || value.demands.empty() ||
      value.demands.size() > kContextualTextMaximumProfileCountV2)
    return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                "contextual TEXT request header claims are invalid");

  Bytes records;
  std::uint64_t previous_occurrence = 0;
  std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>,
           std::tuple<ContextualTextUuidV2, ContextualTextUuidV2, std::uint64_t>>
      source_claims;
  std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint32_t>>
      identities;
  std::vector<std::uint32_t> literal_handles;
  for (const auto& demand : value.demands) {
    if (demand.literal_occurrence == 0 ||
        demand.literal_occurrence > std::numeric_limits<std::uint32_t>::max() ||
        demand.literal_occurrence <= previous_occurrence ||
        demand.literal_occurrence != demand.parent_operand_ordinal ||
        demand.comparison_occurrence == 0 ||
        demand.comparison_occurrence >
            std::numeric_limits<std::uint32_t>::max() ||
        demand.source_node_id == 0 ||
        demand.source_node_id > std::numeric_limits<std::uint32_t>::max() ||
        demand.source_operand_ordinal == 0 ||
        (demand.literal_argument_ordinal != 1 &&
         demand.literal_argument_ordinal != 2) ||
        (demand.target_argument_ordinal != 1 &&
         demand.target_argument_ordinal != 2) ||
        demand.literal_argument_ordinal == demand.target_argument_ordinal ||
        !Nonzero(demand.relation_uuid) ||
        !Nonzero(demand.relation_descriptor_uuid) ||
        demand.relation_descriptor_generation == 0 ||
        !Nonzero(demand.column_uuid) || demand.node_id == 0 ||
        demand.target_descriptor_handle == 0 ||
        demand.literal_descriptor_handle == 0 ||
        demand.literal_descriptor_handle == demand.target_descriptor_handle ||
        demand.raw_token.size() < 2 ||
        demand.raw_token.size() > kContextualTextMaximumRawTokenBytesV2 ||
        demand.lexical_value.size() > kContextualTextMaximumBodyBytesV2)
      return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                  "contextual TEXT demand claims are invalid");
    previous_occurrence = demand.literal_occurrence;

    ContextualTextRawTokenV2 decoded;
    ContextualTextCodecDiagnosticV2 raw_diagnostic;
    if (!DecodeContextualTextRawTokenV2(demand.raw_token.data(),
                                       demand.raw_token.size(), &decoded,
                                       &raw_diagnostic)) {
      if (diagnostic != nullptr) *diagnostic = std::move(raw_diagnostic);
      return false;
    }
    if (decoded.decoded_utf8 != demand.lexical_value ||
        decoded.scalar_count != demand.scalar_count)
      return Fail(diagnostic,
                  "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
                  "raw token does not independently decode to the lexical body");

    const auto identity = std::make_tuple(
        demand.literal_occurrence, demand.node_id,
        demand.literal_descriptor_handle);
    if (std::find(identities.begin(), identities.end(), identity) !=
            identities.end() ||
        std::find(literal_handles.begin(), literal_handles.end(),
                  demand.literal_descriptor_handle) != literal_handles.end())
      return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                  "contextual TEXT demand identity is duplicated");
    identities.push_back(identity);
    literal_handles.push_back(demand.literal_descriptor_handle);

    const auto source_key = std::make_tuple(
        demand.source_node_id, demand.source_operand_ordinal,
        demand.source_ordinal);
    const auto source_value = std::make_tuple(
        demand.relation_uuid, demand.relation_descriptor_uuid,
        demand.relation_descriptor_generation);
    const auto existing = source_claims.find(source_key);
    if (existing != source_claims.end() && existing->second != source_value)
      return Fail(diagnostic,
                  "SBLR.CONTEXTUAL_TEXT_LITERAL.TARGET_MISMATCH",
                  "one structural source key has inconsistent relation claims");
    source_claims.emplace(source_key, source_value);

    std::size_t record_size = kContextualTextDemandPrefixBytesV2;
    if (!CheckedAdd(record_size, demand.raw_token.size(), &record_size) ||
        !CheckedAdd(record_size, demand.lexical_value.size(), &record_size) ||
        record_size > std::numeric_limits<std::uint32_t>::max())
      return Fail(diagnostic, "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED",
                  "contextual TEXT demand extent overflowed");
    const std::size_t base = records.size();
    if (!CheckedAdd(base, record_size, &record_size) ||
        record_size > kContextualTextMaximumLogicalCarrierBytesV2)
      return Fail(diagnostic, "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED",
                  "contextual TEXT request record vector exceeds its limit");
    records.resize(record_size, 0);
    const std::uint32_t extent = static_cast<std::uint32_t>(record_size - base);
    PutU32(&records, base + 0, extent);
    PutU16(&records, base + 4, kContextualTextDemandPrefixBytesV2);
    PutU64(&records, base + 8, demand.literal_occurrence);
    PutU16(&records, base + 16, 3);
    PutU16(&records, base + 18, 2);
    records[base + 20] = 0;
    records[base + 21] = demand.literal_argument_ordinal;
    records[base + 22] = demand.target_argument_ordinal;
    PutU64(&records, base + 24, demand.comparison_occurrence);
    PutU64(&records, base + 32, demand.source_node_id);
    PutU32(&records, base + 40, demand.source_operand_ordinal);
    PutU32(&records, base + 44, demand.source_ordinal);
    PutArray(&records, base + 56, demand.relation_uuid);
    PutArray(&records, base + 72, demand.relation_descriptor_uuid);
    PutU64(&records, base + 88, demand.relation_descriptor_generation);
    PutArray(&records, base + 96, demand.column_uuid);
    PutU32(&records, base + 112, demand.column_ordinal);
    PutU32(&records, base + 116, demand.parent_operand_ordinal);
    PutU64(&records, base + 120, demand.node_id);
    PutU32(&records, base + 128, demand.target_descriptor_handle);
    PutU32(&records, base + 132, demand.literal_descriptor_handle);
    PutU32(&records, base + 136,
           static_cast<std::uint32_t>(demand.raw_token.size()));
    PutU32(&records, base + 140,
           static_cast<std::uint32_t>(demand.lexical_value.size()));
    PutU32(&records, base + 144, demand.scalar_count);
    PutArray(&records, base + 152, HashRawToken(demand));
    PutArray(&records, base + 184, HashLexicalValue(demand));
    std::copy(demand.raw_token.begin(), demand.raw_token.end(),
              records.begin() + base + kContextualTextDemandPrefixBytesV2);
    std::copy(demand.lexical_value.begin(), demand.lexical_value.end(),
              records.begin() + base + kContextualTextDemandPrefixBytesV2 +
                  demand.raw_token.size());
  }

  std::size_t total = kContextualTextRequestHeaderBytesV2;
  if (!CheckedAdd(total, records.size(), &total) ||
      total > kContextualTextMaximumLogicalCarrierBytesV2)
    return Fail(diagnostic, "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED",
                "contextual TEXT request exceeds its logical limit");
  out->assign(total, 0);
  PutMagic(out, "SBTLNR02");
  PutU16(out, 8, 2);
  PutU16(out, 10, kContextualTextRequestHeaderBytesV2);
  PutU32(out, 12, static_cast<std::uint32_t>(total));
  PutU32(out, 20, static_cast<std::uint32_t>(value.demands.size()));
  PutU32(out, 24, static_cast<std::uint32_t>(records.size()));
  PutU32(out, 28, kContextualTextDemandPrefixBytesV2);
  PutArray(out, 32, value.statement_receipt_uuid);
  PutArray(out, 48, value.catalog_snapshot_uuid);
  PutU64(out, 64, value.catalog_generation);
  PutU64(out, 72, value.datatype_registry_generation);
  PutU64(out, 80, value.security_generation);
  PutU64(out, 88, value.resource_epoch);
  PutArray(out, 96, value.mga_snapshot_uuid);
  PutArray(out, 160, ComputeContextualTextDemandSequenceSha256V2(
                         records, static_cast<std::uint32_t>(value.demands.size())));
  std::copy(records.begin(), records.end(),
            out->begin() + kContextualTextRequestHeaderBytesV2);
  return true;
}

bool DecodeContextualTextLiteralNegotiationRequestV2(
    const std::uint8_t* bytes,
    std::size_t size,
    ContextualTextLiteralNegotiationRequestV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr || bytes == nullptr ||
      size < kContextualTextRequestHeaderBytesV2 ||
      size > kContextualTextMaximumLogicalCarrierBytesV2 ||
      !ExactMagic(bytes, "SBTLNR02") || GetU16(bytes + 8) != 2 ||
      GetU16(bytes + 10) != kContextualTextRequestHeaderBytesV2 ||
      GetU32(bytes + 12) != size || GetU32(bytes + 16) != 0 ||
      GetU32(bytes + 28) != kContextualTextDemandPrefixBytesV2 ||
      !AllZero(bytes + 112, 48))
    return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                "SBTLNR02 header is not canonical");
  const std::uint32_t count = GetU32(bytes + 20);
  const std::uint32_t vector_bytes = GetU32(bytes + 24);
  if (count == 0 || count > kContextualTextMaximumProfileCountV2 ||
      vector_bytes != size - kContextualTextRequestHeaderBytesV2)
    return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                "SBTLNR02 count or vector extent is invalid");

  ContextualTextLiteralNegotiationRequestV2 decoded;
  GetArray(bytes + 32, &decoded.statement_receipt_uuid);
  GetArray(bytes + 48, &decoded.catalog_snapshot_uuid);
  decoded.catalog_generation = GetU64(bytes + 64);
  decoded.datatype_registry_generation = GetU64(bytes + 72);
  decoded.security_generation = GetU64(bytes + 80);
  decoded.resource_epoch = GetU64(bytes + 88);
  GetArray(bytes + 96, &decoded.mga_snapshot_uuid);
  GetArray(bytes + 160, &decoded.demand_sequence_sha256);

  std::size_t offset = kContextualTextRequestHeaderBytesV2;
  for (std::uint32_t index = 0; index != count; ++index) {
    if (size - offset < kContextualTextDemandPrefixBytesV2)
      return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                  "SBTLNR02 demand prefix is truncated");
    const std::uint32_t extent = GetU32(bytes + offset);
    const std::uint32_t raw_bytes = GetU32(bytes + offset + 136);
    const std::uint32_t lexical_bytes = GetU32(bytes + offset + 140);
    std::size_t expected = kContextualTextDemandPrefixBytesV2;
    if (!CheckedAdd(expected, raw_bytes, &expected) ||
        !CheckedAdd(expected, lexical_bytes, &expected) || extent != expected ||
        extent > size - offset ||
        GetU16(bytes + offset + 4) != kContextualTextDemandPrefixBytesV2 ||
        GetU16(bytes + offset + 6) != 0 ||
        GetU16(bytes + offset + 16) != 3 ||
        GetU16(bytes + offset + 18) != 2 || bytes[offset + 20] != 0 ||
        bytes[offset + 23] != 0 || !AllZero(bytes + offset + 48, 8) ||
        GetU32(bytes + offset + 148) != 0)
      return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                  "SBTLNR02 demand framing is not canonical");
    ContextualTextLiteralDemandV2 demand;
    demand.literal_occurrence = GetU64(bytes + offset + 8);
    demand.literal_argument_ordinal = bytes[offset + 21];
    demand.target_argument_ordinal = bytes[offset + 22];
    demand.comparison_occurrence = GetU64(bytes + offset + 24);
    demand.source_node_id = GetU64(bytes + offset + 32);
    demand.source_operand_ordinal = GetU32(bytes + offset + 40);
    demand.source_ordinal = GetU32(bytes + offset + 44);
    GetArray(bytes + offset + 56, &demand.relation_uuid);
    GetArray(bytes + offset + 72, &demand.relation_descriptor_uuid);
    demand.relation_descriptor_generation = GetU64(bytes + offset + 88);
    GetArray(bytes + offset + 96, &demand.column_uuid);
    demand.column_ordinal = GetU32(bytes + offset + 112);
    demand.parent_operand_ordinal = GetU32(bytes + offset + 116);
    demand.node_id = GetU64(bytes + offset + 120);
    demand.target_descriptor_handle = GetU32(bytes + offset + 128);
    demand.literal_descriptor_handle = GetU32(bytes + offset + 132);
    demand.scalar_count = GetU32(bytes + offset + 144);
    GetArray(bytes + offset + 152, &demand.raw_token_sha256);
    GetArray(bytes + offset + 184, &demand.lexical_value_sha256);
    demand.raw_token.assign(
        bytes + offset + kContextualTextDemandPrefixBytesV2,
        bytes + offset + kContextualTextDemandPrefixBytesV2 + raw_bytes);
    demand.lexical_value.assign(
        bytes + offset + kContextualTextDemandPrefixBytesV2 + raw_bytes,
        bytes + offset + extent);
    decoded.demands.push_back(std::move(demand));
    offset += extent;
  }
  if (offset != size)
    return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                "SBTLNR02 has an unclaimed trailer");
  Bytes canonical;
  ContextualTextCodecDiagnosticV2 canonical_diagnostic;
  if (!EncodeContextualTextLiteralNegotiationRequestV2(
          decoded, &canonical, &canonical_diagnostic)) {
    if (diagnostic != nullptr) *diagnostic = std::move(canonical_diagnostic);
    return false;
  }
  if (canonical.size() != size ||
      !std::equal(canonical.begin(), canonical.end(), bytes))
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
                "SBTLNR02 does not equal its canonical re-encoding");
  *out = std::move(decoded);
  return true;
}

bool EncodeContextualTextDescriptorV2(
    const ContextualTextDescriptorV2& value,
    std::vector<std::uint8_t>* out,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr)
    return Fail(diagnostic, "CTB.TEXT.DESCRIPTOR_INVALID",
                "contextual TEXT descriptor output is null");
  out->clear();
  if ((value.flags & ~std::uint32_t{7}) != 0 ||
      (value.flags & std::uint32_t{1}) == 0 ||
      value.malformed_sequence_policy != 1 || value.null_encoding != 1 ||
      value.descriptor_uuid != kTextDescriptorUuid ||
      value.descriptor_generation != 1 || value.type_uuid != kTextTypeUuid ||
      value.type_generation != 1 || value.codec_uuid != kTextCodecUuid ||
      value.codec_version != 1 || value.codec_generation != 1 ||
      !ValidRequiredPair(value.charset_uuid, value.charset_generation) ||
      !ValidRequiredPair(value.collation_uuid, value.collation_generation) ||
      !ValidRequiredPair(value.normalization_policy_uuid,
                         value.normalization_policy_generation) ||
      !ValidOptionalPair(value.padding_policy_uuid,
                         value.padding_policy_generation) ||
      !ValidOptionalPair(value.case_accent_policy_uuid,
                         value.case_accent_policy_generation) ||
      !ValidRequiredPair(value.render_policy_uuid,
                         value.render_policy_generation) ||
      !ValidRequiredPair(value.canonicalization_profile_uuid,
                         value.canonicalization_profile_generation) ||
      !ValidRequiredPair(value.comparison_contract_uuid,
                         value.comparison_contract_generation) ||
      !ValidRequiredPair(value.equality_operation_uuid,
                         value.equality_operation_generation) ||
      value.datatype_catalog_snapshot_uuid != kDatatypeCatalogUuid ||
      value.datatype_catalog_generation != 1 ||
      value.datatype_registry_generation != 1 || value.resource_epoch == 0)
    return Fail(diagnostic, "CTB.TEXT.DESCRIPTOR_INVALID",
                "SBTLTD02 identity or resource fields are invalid");
  if (((value.flags & 2) != 0) != Nonzero(value.padding_policy_uuid) ||
      ((value.flags & 4) != 0) != Nonzero(value.case_accent_policy_uuid))
    return Fail(diagnostic, "CTB.TEXT.DESCRIPTOR_INVALID",
                "SBTLTD02 optional-policy flags do not match their identities");

  out->assign(kContextualTextDescriptorBytesV2, 0);
  PutMagic(out, "SBTLTD02");
  PutU16(out, 8, 2);
  PutU16(out, 10, 512);
  PutU32(out, 12, kContextualTextDescriptorBytesV2);
  PutU32(out, 16, value.flags);
  (*out)[20] = value.malformed_sequence_policy;
  (*out)[21] = value.null_encoding;
  PutArray(out, 24, value.descriptor_uuid);
  PutU64(out, 40, value.descriptor_generation);
  PutArray(out, 48, value.type_uuid);
  PutU64(out, 64, value.type_generation);
  PutArray(out, 72, value.codec_uuid);
  PutU16(out, 88, kContextualTextCodecIdentifierBytesV2);
  PutU16(out, 90, value.codec_version);
  PutU64(out, 96, value.codec_generation);
  PutU64(out, 104, value.character_limit);
  PutU64(out, 112, value.byte_limit);
  PutArray(out, 120, value.charset_uuid);
  PutU64(out, 136, value.charset_generation);
  PutArray(out, 144, value.collation_uuid);
  PutU64(out, 160, value.collation_generation);
  PutArray(out, 168, value.normalization_policy_uuid);
  PutU64(out, 184, value.normalization_policy_generation);
  PutArray(out, 192, value.padding_policy_uuid);
  PutU64(out, 208, value.padding_policy_generation);
  PutArray(out, 216, value.case_accent_policy_uuid);
  PutU64(out, 232, value.case_accent_policy_generation);
  PutArray(out, 240, value.render_policy_uuid);
  PutU64(out, 256, value.render_policy_generation);
  PutArray(out, 264, value.canonicalization_profile_uuid);
  PutU64(out, 280, value.canonicalization_profile_generation);
  PutArray(out, 288, value.comparison_contract_uuid);
  PutU64(out, 304, value.comparison_contract_generation);
  PutArray(out, 312, value.equality_operation_uuid);
  PutU64(out, 328, value.equality_operation_generation);
  PutArray(out, 408, value.datatype_catalog_snapshot_uuid);
  PutU64(out, 424, value.datatype_catalog_generation);
  PutU64(out, 432, value.datatype_registry_generation);
  PutU64(out, 440, value.resource_epoch);
  std::copy_n(reinterpret_cast<const std::uint8_t*>(
                  kContextualTextCodecIdentifierV2),
              kContextualTextCodecIdentifierBytesV2, out->begin() + 512);
  PutArray(out, 448,
           ComputeContextualTextDescriptorEvidenceSha256V2(*out));
  return true;
}

bool DecodeContextualTextDescriptorV2(
    const std::uint8_t* bytes,
    std::size_t size,
    ContextualTextDescriptorV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr || bytes == nullptr ||
      size != kContextualTextDescriptorBytesV2 ||
      !ExactMagic(bytes, "SBTLTD02") || GetU16(bytes + 8) != 2 ||
      GetU16(bytes + 10) != 512 || GetU32(bytes + 12) != size ||
      !AllZero(bytes + 22, 2) || !AllZero(bytes + 92, 4) ||
      !AllZero(bytes + 336, 72) || !AllZero(bytes + 480, 32) ||
      GetU16(bytes + 88) != kContextualTextCodecIdentifierBytesV2 ||
      !std::equal(bytes + 512, bytes + size,
                  reinterpret_cast<const std::uint8_t*>(
                      kContextualTextCodecIdentifierV2)))
    return Fail(diagnostic, "CTB.TEXT.DESCRIPTOR_INVALID",
                "SBTLTD02 framing is invalid");
  ContextualTextDescriptorV2 decoded;
  decoded.flags = GetU32(bytes + 16);
  decoded.malformed_sequence_policy = bytes[20];
  decoded.null_encoding = bytes[21];
  GetArray(bytes + 24, &decoded.descriptor_uuid);
  decoded.descriptor_generation = GetU64(bytes + 40);
  GetArray(bytes + 48, &decoded.type_uuid);
  decoded.type_generation = GetU64(bytes + 64);
  GetArray(bytes + 72, &decoded.codec_uuid);
  decoded.codec_version = GetU16(bytes + 90);
  decoded.codec_generation = GetU64(bytes + 96);
  decoded.character_limit = GetU64(bytes + 104);
  decoded.byte_limit = GetU64(bytes + 112);
  GetArray(bytes + 120, &decoded.charset_uuid);
  decoded.charset_generation = GetU64(bytes + 136);
  GetArray(bytes + 144, &decoded.collation_uuid);
  decoded.collation_generation = GetU64(bytes + 160);
  GetArray(bytes + 168, &decoded.normalization_policy_uuid);
  decoded.normalization_policy_generation = GetU64(bytes + 184);
  GetArray(bytes + 192, &decoded.padding_policy_uuid);
  decoded.padding_policy_generation = GetU64(bytes + 208);
  GetArray(bytes + 216, &decoded.case_accent_policy_uuid);
  decoded.case_accent_policy_generation = GetU64(bytes + 232);
  GetArray(bytes + 240, &decoded.render_policy_uuid);
  decoded.render_policy_generation = GetU64(bytes + 256);
  GetArray(bytes + 264, &decoded.canonicalization_profile_uuid);
  decoded.canonicalization_profile_generation = GetU64(bytes + 280);
  GetArray(bytes + 288, &decoded.comparison_contract_uuid);
  decoded.comparison_contract_generation = GetU64(bytes + 304);
  GetArray(bytes + 312, &decoded.equality_operation_uuid);
  decoded.equality_operation_generation = GetU64(bytes + 328);
  GetArray(bytes + 408, &decoded.datatype_catalog_snapshot_uuid);
  decoded.datatype_catalog_generation = GetU64(bytes + 424);
  decoded.datatype_registry_generation = GetU64(bytes + 432);
  decoded.resource_epoch = GetU64(bytes + 440);
  GetArray(bytes + 448, &decoded.descriptor_evidence_sha256);
  decoded.exact_bytes.assign(bytes, bytes + size);
  Bytes canonical;
  ContextualTextCodecDiagnosticV2 canonical_diagnostic;
  if (!EncodeContextualTextDescriptorV2(decoded, &canonical,
                                         &canonical_diagnostic)) {
    if (diagnostic != nullptr) *diagnostic = std::move(canonical_diagnostic);
    return false;
  }
  if (canonical != decoded.exact_bytes)
    return Fail(diagnostic, "CTB.TEXT.DESCRIPTOR_INVALID",
                "SBTLTD02 does not equal its canonical re-encoding");
  *out = std::move(decoded);
  return true;
}

bool EncodeContextualTextLiteralProfileV2(
    const ContextualTextLiteralProfileV2& value,
    std::vector<std::uint8_t>* out,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr)
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
                "contextual TEXT profile output is null");
  out->clear();
  std::uint32_t decoded_scalars = 0;
  if (!ValidateUtf8Body(value.canonical_body, &decoded_scalars))
    return Fail(diagnostic, "CTB.TEXT.INVALID_ENCODING",
                "contextual TEXT canonical body is not shortest-form UTF-8");
  const bool valid_arguments =
      (value.literal_argument_ordinal == 1 &&
       value.target_argument_ordinal == 2) ||
      (value.literal_argument_ordinal == 2 &&
       value.target_argument_ordinal == 1);
  if (!Nonzero(value.profile_uuid) || !Nonzero(value.profile_set_uuid) ||
      value.profile_set_generation == 0 ||
      !Nonzero(value.literal_binding_uuid) ||
      value.literal_binding_generation == 0 || value.literal_occurrence == 0 ||
      value.literal_occurrence > std::numeric_limits<std::uint32_t>::max() ||
      value.node_id == 0 || value.comparison_occurrence == 0 ||
      value.comparison_occurrence > std::numeric_limits<std::uint32_t>::max() ||
      !Nonzero(value.statement_receipt_uuid) ||
      !Nonzero(value.catalog_snapshot_uuid) || value.catalog_generation == 0 ||
      value.datatype_registry_generation == 0 ||
      value.security_generation == 0 || value.resource_epoch == 0 ||
      !Nonzero(value.mga_snapshot_uuid) ||
      value.descriptor_uuid != kTextDescriptorUuid ||
      value.descriptor_generation != 1 || value.type_uuid != kTextTypeUuid ||
      value.type_generation != 1 || value.codec_uuid != kTextCodecUuid ||
      value.codec_version != 1 || value.codec_generation != 1 ||
      !valid_arguments || !ValidRequiredPair(value.source_occurrence_uuid,
                                             value.source_generation) ||
      !Nonzero(value.relation_uuid) ||
      !Nonzero(value.relation_descriptor_uuid) ||
      value.relation_descriptor_generation == 0 ||
      !Nonzero(value.column_uuid) || value.parent_operand_ordinal == 0 ||
      value.parent_operand_ordinal != value.literal_occurrence ||
      value.target_descriptor_handle == 0 ||
      value.literal_descriptor_handle == 0 ||
      value.target_descriptor_handle == value.literal_descriptor_handle ||
      value.scalar_count != decoded_scalars ||
      value.canonical_body.size() > kContextualTextMaximumBodyBytesV2 ||
      (value.target_character_limit !=
           std::numeric_limits<std::uint64_t>::max() &&
       value.scalar_count > value.target_character_limit) ||
      (value.target_byte_limit != std::numeric_limits<std::uint64_t>::max() &&
       value.canonical_body.size() > value.target_byte_limit) ||
      !ValidRequiredPair(value.charset_uuid, value.charset_generation) ||
      !ValidRequiredPair(value.collation_uuid, value.collation_generation) ||
      !ValidRequiredPair(value.normalization_policy_uuid,
                         value.normalization_policy_generation) ||
      !ValidOptionalPair(value.padding_policy_uuid,
                         value.padding_policy_generation) ||
      !ValidOptionalPair(value.case_accent_policy_uuid,
                         value.case_accent_policy_generation) ||
      !ValidRequiredPair(value.render_policy_uuid,
                         value.render_policy_generation) ||
      !ValidRequiredPair(value.canonicalization_profile_uuid,
                         value.canonicalization_profile_generation) ||
      !ValidRequiredPair(value.comparison_contract_uuid,
                         value.comparison_contract_generation) ||
      !ValidRequiredPair(value.equality_operation_uuid,
                         value.equality_operation_generation) ||
      !ValidRequiredPair(value.literal_budget_uuid,
                         value.literal_budget_generation) ||
      value.literal_negotiation_byte_grant < 4096 ||
      value.literal_negotiation_byte_grant >
          kContextualTextMaximumLogicalCarrierBytesV2 ||
      value.canonical_body_aggregate_grant == 0 ||
      value.canonical_body_aggregate_grant >
          kContextualTextMaximumBodyBytesV2 ||
      value.canonical_body_aggregate_grant >
          value.literal_negotiation_byte_grant ||
      value.canonical_body.size() > value.canonical_body_aggregate_grant)
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
                "SBTLPR02 identity, resource, or body fields are invalid");
  std::size_t profile_size = kContextualTextProfileHeaderBytesV2 +
                             kContextualTextCodecIdentifierBytesV2;
  if (!CheckedAdd(profile_size, value.canonical_body.size(), &profile_size) ||
      profile_size > kContextualTextMaximumProfileBytesV2)
    return Fail(diagnostic, "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED",
                "SBTLPR02 exceeds its extent limit");
  out->assign(profile_size, 0);
  PutMagic(out, "SBTLPR02");
  PutU16(out, 8, 2);
  PutU16(out, 10, kContextualTextProfileHeaderBytesV2);
  PutU32(out, 12, static_cast<std::uint32_t>(profile_size));
  PutArray(out, 24, value.profile_uuid);
  PutArray(out, 40, value.profile_set_uuid);
  PutU64(out, 56, value.profile_set_generation);
  PutArray(out, 64, value.literal_binding_uuid);
  PutU64(out, 80, value.literal_binding_generation);
  PutU64(out, 88, value.literal_occurrence);
  PutU64(out, 96, value.node_id);
  PutU64(out, 104, value.comparison_occurrence);
  PutArray(out, 112, value.statement_receipt_uuid);
  PutArray(out, 128, value.catalog_snapshot_uuid);
  PutU64(out, 144, value.catalog_generation);
  PutU64(out, 152, value.datatype_registry_generation);
  PutU64(out, 160, value.security_generation);
  PutU64(out, 168, value.resource_epoch);
  PutArray(out, 176, value.mga_snapshot_uuid);
  PutArray(out, 192, value.descriptor_uuid);
  PutU64(out, 208, value.descriptor_generation);
  PutArray(out, 216, value.type_uuid);
  PutU64(out, 232, value.type_generation);
  PutArray(out, 240, value.codec_uuid);
  PutU16(out, 256, kContextualTextCodecIdentifierBytesV2);
  PutU16(out, 258, value.codec_version);
  PutU64(out, 264, value.codec_generation);
  PutU16(out, 272, 3);
  PutU16(out, 274, 2);
  (*out)[276] = 0;
  (*out)[277] = 1;
  (*out)[278] = value.literal_argument_ordinal;
  (*out)[279] = value.target_argument_ordinal;
  PutArray(out, 280, value.source_occurrence_uuid);
  PutU64(out, 296, value.source_generation);
  PutArray(out, 304, value.relation_uuid);
  PutArray(out, 320, value.relation_descriptor_uuid);
  PutU64(out, 336, value.relation_descriptor_generation);
  PutArray(out, 344, value.column_uuid);
  PutU32(out, 360, value.column_ordinal);
  PutU32(out, 364, value.parent_operand_ordinal);
  PutU32(out, 368, value.target_descriptor_handle);
  PutU32(out, 372, value.literal_descriptor_handle);
  PutU32(out, 376,
           static_cast<std::uint32_t>(value.canonical_body.size()));
  PutU32(out, 380, value.scalar_count);
  PutU64(out, 384, value.target_character_limit);
  PutU64(out, 392, value.target_byte_limit);
  PutArray(out, 400, value.charset_uuid);
  PutU64(out, 416, value.charset_generation);
  PutArray(out, 424, value.collation_uuid);
  PutU64(out, 440, value.collation_generation);
  PutArray(out, 448, value.normalization_policy_uuid);
  PutU64(out, 464, value.normalization_policy_generation);
  PutArray(out, 472, value.padding_policy_uuid);
  PutU64(out, 488, value.padding_policy_generation);
  PutArray(out, 496, value.case_accent_policy_uuid);
  PutU64(out, 512, value.case_accent_policy_generation);
  PutArray(out, 520, value.render_policy_uuid);
  PutU64(out, 536, value.render_policy_generation);
  PutArray(out, 544, value.canonicalization_profile_uuid);
  PutU64(out, 560, value.canonicalization_profile_generation);
  PutArray(out, 568, value.comparison_contract_uuid);
  PutU64(out, 584, value.comparison_contract_generation);
  PutArray(out, 592, value.equality_operation_uuid);
  PutU64(out, 608, value.equality_operation_generation);
  PutArray(out, 616, value.literal_budget_uuid);
  PutU64(out, 632, value.literal_budget_generation);
  PutU64(out, 640, value.literal_negotiation_byte_grant);
  PutU64(out, 648, value.canonical_body_aggregate_grant);
  PutArray(out, 656, value.raw_token_sha256);
  PutArray(out, 688, value.lexical_value_sha256);
  PutArray(out, 720, HashCanonicalBody(value));
  PutArray(out, 752, value.target_projection_sha256);
  PutArray(out, 784, value.descriptor_evidence_sha256);
  PutArray(out, 816, value.target_context_sha256);
  PutArray(out, 848, value.demand_sequence_sha256);
  std::copy_n(reinterpret_cast<const std::uint8_t*>(
                  kContextualTextCodecIdentifierV2),
              kContextualTextCodecIdentifierBytesV2,
              out->begin() + kContextualTextProfileHeaderBytesV2);
  std::copy(value.canonical_body.begin(), value.canonical_body.end(),
            out->begin() + kContextualTextProfileHeaderBytesV2 +
                kContextualTextCodecIdentifierBytesV2);
  PutArray(out, 880, ComputeContextualTextBindingSha256V2(*out));
  return true;
}

bool DecodeContextualTextLiteralProfileV2(
    const std::uint8_t* bytes,
    std::size_t size,
    ContextualTextLiteralProfileV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr || bytes == nullptr ||
      size < kContextualTextProfileHeaderBytesV2 +
                 kContextualTextCodecIdentifierBytesV2 ||
      size > kContextualTextMaximumProfileBytesV2 ||
      !ExactMagic(bytes, "SBTLPR02") || GetU16(bytes + 8) != 2 ||
      GetU16(bytes + 10) != kContextualTextProfileHeaderBytesV2 ||
      GetU32(bytes + 12) != size || GetU32(bytes + 16) != 0 ||
      GetU32(bytes + 20) != 0 ||
      GetU16(bytes + 256) != kContextualTextCodecIdentifierBytesV2 ||
      GetU32(bytes + 260) != 0 || GetU16(bytes + 272) != 3 ||
      GetU16(bytes + 274) != 2 || bytes[276] != 0 || bytes[277] != 1 ||
      !std::equal(bytes + kContextualTextProfileHeaderBytesV2,
                  bytes + kContextualTextProfileHeaderBytesV2 +
                      kContextualTextCodecIdentifierBytesV2,
                  reinterpret_cast<const std::uint8_t*>(
                      kContextualTextCodecIdentifierV2)) ||
      GetU32(bytes + 376) !=
          size - kContextualTextProfileHeaderBytesV2 -
              kContextualTextCodecIdentifierBytesV2)
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
                "SBTLPR02 framing is invalid");
  ContextualTextLiteralProfileV2 decoded;
  GetArray(bytes + 24, &decoded.profile_uuid);
  GetArray(bytes + 40, &decoded.profile_set_uuid);
  decoded.profile_set_generation = GetU64(bytes + 56);
  GetArray(bytes + 64, &decoded.literal_binding_uuid);
  decoded.literal_binding_generation = GetU64(bytes + 80);
  decoded.literal_occurrence = GetU64(bytes + 88);
  decoded.node_id = GetU64(bytes + 96);
  decoded.comparison_occurrence = GetU64(bytes + 104);
  GetArray(bytes + 112, &decoded.statement_receipt_uuid);
  GetArray(bytes + 128, &decoded.catalog_snapshot_uuid);
  decoded.catalog_generation = GetU64(bytes + 144);
  decoded.datatype_registry_generation = GetU64(bytes + 152);
  decoded.security_generation = GetU64(bytes + 160);
  decoded.resource_epoch = GetU64(bytes + 168);
  GetArray(bytes + 176, &decoded.mga_snapshot_uuid);
  GetArray(bytes + 192, &decoded.descriptor_uuid);
  decoded.descriptor_generation = GetU64(bytes + 208);
  GetArray(bytes + 216, &decoded.type_uuid);
  decoded.type_generation = GetU64(bytes + 232);
  GetArray(bytes + 240, &decoded.codec_uuid);
  decoded.codec_version = GetU16(bytes + 258);
  decoded.codec_generation = GetU64(bytes + 264);
  decoded.literal_argument_ordinal = bytes[278];
  decoded.target_argument_ordinal = bytes[279];
  GetArray(bytes + 280, &decoded.source_occurrence_uuid);
  decoded.source_generation = GetU64(bytes + 296);
  GetArray(bytes + 304, &decoded.relation_uuid);
  GetArray(bytes + 320, &decoded.relation_descriptor_uuid);
  decoded.relation_descriptor_generation = GetU64(bytes + 336);
  GetArray(bytes + 344, &decoded.column_uuid);
  decoded.column_ordinal = GetU32(bytes + 360);
  decoded.parent_operand_ordinal = GetU32(bytes + 364);
  decoded.target_descriptor_handle = GetU32(bytes + 368);
  decoded.literal_descriptor_handle = GetU32(bytes + 372);
  decoded.scalar_count = GetU32(bytes + 380);
  decoded.target_character_limit = GetU64(bytes + 384);
  decoded.target_byte_limit = GetU64(bytes + 392);
  GetArray(bytes + 400, &decoded.charset_uuid);
  decoded.charset_generation = GetU64(bytes + 416);
  GetArray(bytes + 424, &decoded.collation_uuid);
  decoded.collation_generation = GetU64(bytes + 440);
  GetArray(bytes + 448, &decoded.normalization_policy_uuid);
  decoded.normalization_policy_generation = GetU64(bytes + 464);
  GetArray(bytes + 472, &decoded.padding_policy_uuid);
  decoded.padding_policy_generation = GetU64(bytes + 488);
  GetArray(bytes + 496, &decoded.case_accent_policy_uuid);
  decoded.case_accent_policy_generation = GetU64(bytes + 512);
  GetArray(bytes + 520, &decoded.render_policy_uuid);
  decoded.render_policy_generation = GetU64(bytes + 536);
  GetArray(bytes + 544, &decoded.canonicalization_profile_uuid);
  decoded.canonicalization_profile_generation = GetU64(bytes + 560);
  GetArray(bytes + 568, &decoded.comparison_contract_uuid);
  decoded.comparison_contract_generation = GetU64(bytes + 584);
  GetArray(bytes + 592, &decoded.equality_operation_uuid);
  decoded.equality_operation_generation = GetU64(bytes + 608);
  GetArray(bytes + 616, &decoded.literal_budget_uuid);
  decoded.literal_budget_generation = GetU64(bytes + 632);
  decoded.literal_negotiation_byte_grant = GetU64(bytes + 640);
  decoded.canonical_body_aggregate_grant = GetU64(bytes + 648);
  GetArray(bytes + 656, &decoded.raw_token_sha256);
  GetArray(bytes + 688, &decoded.lexical_value_sha256);
  GetArray(bytes + 720, &decoded.canonical_body_sha256);
  GetArray(bytes + 752, &decoded.target_projection_sha256);
  GetArray(bytes + 784, &decoded.descriptor_evidence_sha256);
  GetArray(bytes + 816, &decoded.target_context_sha256);
  GetArray(bytes + 848, &decoded.demand_sequence_sha256);
  GetArray(bytes + 880, &decoded.binding_sha256);
  decoded.canonical_body.assign(
      bytes + kContextualTextProfileHeaderBytesV2 +
          kContextualTextCodecIdentifierBytesV2,
      bytes + size);
  decoded.exact_bytes.assign(bytes, bytes + size);
  Bytes canonical;
  ContextualTextCodecDiagnosticV2 canonical_diagnostic;
  if (!EncodeContextualTextLiteralProfileV2(decoded, &canonical,
                                             &canonical_diagnostic)) {
    if (diagnostic != nullptr) *diagnostic = std::move(canonical_diagnostic);
    return false;
  }
  if (canonical != decoded.exact_bytes)
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
                "SBTLPR02 does not equal its canonical re-encoding");
  *out = std::move(decoded);
  return true;
}

namespace {

ContextualTextSha256V2 HashTargetContextSequence(
    const std::vector<ContextualTextLiteralProfileMappingV2>& mappings) {
  Bytes material;
  AppendDomain(&material,
               "ScratchBird.ContextualText.TargetContextSequence.V2");
  AppendU32(&material, static_cast<std::uint32_t>(mappings.size()));
  for (const auto& mapping : mappings) {
    AppendU64(&material, mapping.literal_occurrence);
    AppendU64(&material, mapping.node_id);
    AppendU32(&material, mapping.literal_descriptor_handle);
    AppendU32(&material, mapping.target_descriptor_handle);
    AppendArray(&material, mapping.profile.target_context_sha256);
  }
  return Sha256(material);
}

ContextualTextSha256V2 HashOrderedProfiles(const Bytes& mappings,
                                           std::uint32_t count) {
  Bytes material;
  AppendDomain(&material, "ScratchBird.ContextualText.OrderedProfiles.V2");
  AppendU32(&material, count);
  AppendU32(&material, static_cast<std::uint32_t>(mappings.size()));
  material.insert(material.end(), mappings.begin(), mappings.end());
  return Sha256(material);
}

bool EncodeMappings(
    const ContextualTextLiteralProfileSetV2& value,
    Bytes* mappings,
    ContextualTextSha256V2* target_context_sequence,
    ContextualTextSha256V2* ordered_profiles,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  if (mappings == nullptr || target_context_sequence == nullptr ||
      ordered_profiles == nullptr || value.mappings.empty() ||
      value.mappings.size() > kContextualTextMaximumProfileCountV2)
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
                "contextual TEXT profile mapping count is invalid");
  mappings->clear();
  std::uint64_t body_sum = 0;
  std::uint64_t previous_occurrence = 0;
  std::vector<std::uint64_t> nodes;
  std::vector<std::uint32_t> handles;
  for (const auto& mapping : value.mappings) {
    Bytes profile;
    ContextualTextCodecDiagnosticV2 profile_diagnostic;
    if (!EncodeContextualTextLiteralProfileV2(mapping.profile, &profile,
                                               &profile_diagnostic)) {
      if (diagnostic != nullptr) *diagnostic = std::move(profile_diagnostic);
      return false;
    }
    if (mapping.literal_occurrence == 0 ||
        mapping.literal_occurrence <= previous_occurrence ||
        mapping.literal_occurrence != mapping.profile.literal_occurrence ||
        mapping.node_id == 0 || mapping.node_id != mapping.profile.node_id ||
        mapping.literal_binding_uuid !=
            mapping.profile.literal_binding_uuid ||
        mapping.literal_binding_generation !=
            mapping.profile.literal_binding_generation ||
        mapping.literal_descriptor_handle !=
            mapping.profile.literal_descriptor_handle ||
        mapping.target_descriptor_handle !=
            mapping.profile.target_descriptor_handle ||
        mapping.profile.statement_receipt_uuid !=
            value.statement_receipt_uuid ||
        mapping.profile.profile_set_uuid != value.profile_set_uuid ||
        mapping.profile.profile_set_generation != value.profile_set_generation ||
        mapping.profile.catalog_snapshot_uuid != value.catalog_snapshot_uuid ||
        mapping.profile.catalog_generation != value.catalog_generation ||
        mapping.profile.datatype_registry_generation !=
            value.datatype_registry_generation ||
        mapping.profile.security_generation != value.security_generation ||
        mapping.profile.resource_epoch != value.resource_epoch ||
        mapping.profile.mga_snapshot_uuid != value.mga_snapshot_uuid ||
        mapping.profile.literal_budget_uuid != value.literal_budget_uuid ||
        mapping.profile.literal_budget_generation !=
            value.literal_budget_generation ||
        mapping.profile.literal_negotiation_byte_grant !=
            value.literal_negotiation_byte_grant ||
        mapping.profile.canonical_body_aggregate_grant !=
            value.canonical_body_aggregate_grant ||
        mapping.profile.demand_sequence_sha256 != value.demand_sequence_sha256 ||
        std::find(nodes.begin(), nodes.end(), mapping.node_id) != nodes.end() ||
        std::find(handles.begin(), handles.end(),
                  mapping.literal_descriptor_handle) != handles.end())
      return Fail(diagnostic,
                  "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
                  "contextual TEXT mapping and embedded profile disagree");
    previous_occurrence = mapping.literal_occurrence;
    nodes.push_back(mapping.node_id);
    handles.push_back(mapping.literal_descriptor_handle);
    if (mapping.profile.canonical_body.size() >
            std::numeric_limits<std::uint64_t>::max() - body_sum)
      return Fail(diagnostic, "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED",
                  "contextual TEXT canonical-body aggregate overflowed");
    body_sum += mapping.profile.canonical_body.size();
    const std::size_t base = mappings->size();
    std::size_t extent = kContextualTextMappingPrefixBytesV2;
    if (!CheckedAdd(extent, profile.size(), &extent) ||
        !CheckedAdd(base, extent, &extent) ||
        extent > kContextualTextMaximumLogicalCarrierBytesV2)
      return Fail(diagnostic, "PARSER_SERVER_IPC.RESOURCE_LIMIT_EXCEEDED",
                  "contextual TEXT mapping vector overflowed");
    mappings->resize(extent, 0);
    PutU64(mappings, base + 0, mapping.literal_occurrence);
    PutU64(mappings, base + 8, mapping.node_id);
    PutArray(mappings, base + 16, mapping.literal_binding_uuid);
    PutU64(mappings, base + 32, mapping.literal_binding_generation);
    PutU32(mappings, base + 40, mapping.literal_descriptor_handle);
    PutU32(mappings, base + 44, mapping.target_descriptor_handle);
    PutU32(mappings, base + 48, static_cast<std::uint32_t>(profile.size()));
    std::copy(profile.begin(), profile.end(),
              mappings->begin() + base + kContextualTextMappingPrefixBytesV2);
  }
  if (body_sum > value.canonical_body_aggregate_grant)
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.BUDGET_EXCEEDED",
                "contextual TEXT canonical-body aggregate exceeds its grant");
  *target_context_sequence = HashTargetContextSequence(value.mappings);
  *ordered_profiles = HashOrderedProfiles(
      *mappings, static_cast<std::uint32_t>(value.mappings.size()));
  return true;
}

bool ValidProfileSetHeader(const ContextualTextLiteralProfileSetV2& value) {
  return Nonzero(value.statement_receipt_uuid) &&
         Nonzero(value.profile_set_uuid) && value.profile_set_generation != 0 &&
         Nonzero(value.catalog_snapshot_uuid) && value.catalog_generation != 0 &&
         value.datatype_registry_generation != 0 &&
         value.security_generation != 0 && value.resource_epoch != 0 &&
         Nonzero(value.mga_snapshot_uuid) && Nonzero(value.literal_budget_uuid) &&
         value.literal_budget_generation != 0 &&
         value.literal_negotiation_byte_grant >= 4096 &&
         value.literal_negotiation_byte_grant <=
             kContextualTextMaximumLogicalCarrierBytesV2 &&
         value.canonical_body_aggregate_grant != 0 &&
         value.canonical_body_aggregate_grant <=
             kContextualTextMaximumBodyBytesV2 &&
         value.canonical_body_aggregate_grant <=
             value.literal_negotiation_byte_grant;
}

bool DecodeMappings(const std::uint8_t* bytes,
                    std::size_t size,
                    std::size_t offset,
                    std::uint32_t count,
                    std::vector<ContextualTextLiteralProfileMappingV2>* out,
                    ContextualTextCodecDiagnosticV2* diagnostic) {
  if (out == nullptr) return false;
  out->clear();
  for (std::uint32_t index = 0; index != count; ++index) {
    if (size - offset < kContextualTextMappingPrefixBytesV2)
      return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                  "contextual TEXT mapping prefix is truncated");
    const std::uint32_t profile_bytes = GetU32(bytes + offset + 48);
    std::size_t extent = kContextualTextMappingPrefixBytesV2;
    if (!CheckedAdd(extent, profile_bytes, &extent) || extent > size - offset ||
        GetU32(bytes + offset + 52) != 0)
      return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                  "contextual TEXT mapping extent is invalid");
    ContextualTextLiteralProfileMappingV2 mapping;
    mapping.literal_occurrence = GetU64(bytes + offset + 0);
    mapping.node_id = GetU64(bytes + offset + 8);
    GetArray(bytes + offset + 16, &mapping.literal_binding_uuid);
    mapping.literal_binding_generation = GetU64(bytes + offset + 32);
    mapping.literal_descriptor_handle = GetU32(bytes + offset + 40);
    mapping.target_descriptor_handle = GetU32(bytes + offset + 44);
    ContextualTextCodecDiagnosticV2 profile_diagnostic;
    if (!DecodeContextualTextLiteralProfileV2(
            bytes + offset + kContextualTextMappingPrefixBytesV2,
            profile_bytes, &mapping.profile, &profile_diagnostic)) {
      if (diagnostic != nullptr) *diagnostic = std::move(profile_diagnostic);
      return false;
    }
    out->push_back(std::move(mapping));
    offset += extent;
  }
  if (offset != size)
    return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                "contextual TEXT carrier has an unclaimed trailer");
  return true;
}

}  // namespace

bool EncodeContextualTextLiteralProfileSetV2(
    const ContextualTextLiteralProfileSetV2& value,
    std::vector<std::uint8_t>* out,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr || !ValidProfileSetHeader(value))
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
                "SBTLNS02 header authority fields are invalid");
  Bytes mappings;
  ContextualTextSha256V2 target_context_sequence{};
  ContextualTextSha256V2 ordered_profiles{};
  if (!EncodeMappings(value, &mappings, &target_context_sequence,
                      &ordered_profiles, diagnostic))
    return false;
  std::size_t total = kContextualTextResultHeaderBytesV2;
  if (!CheckedAdd(total, mappings.size(), &total) ||
      total > kContextualTextMaximumLogicalCarrierBytesV2 ||
      total > value.literal_negotiation_byte_grant)
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.BUDGET_EXCEEDED",
                "SBTLNS02 exceeds its receipt-owned byte grant");
  out->assign(total, 0);
  PutMagic(out, "SBTLNS02");
  PutU16(out, 8, 2);
  PutU16(out, 10, kContextualTextResultHeaderBytesV2);
  PutU32(out, 12, static_cast<std::uint32_t>(total));
  PutU32(out, 20, static_cast<std::uint32_t>(value.mappings.size()));
  PutU32(out, 24, static_cast<std::uint32_t>(mappings.size()));
  PutU32(out, 28, kContextualTextMappingPrefixBytesV2);
  PutArray(out, 32, value.statement_receipt_uuid);
  PutArray(out, 48, value.profile_set_uuid);
  PutU64(out, 64, value.profile_set_generation);
  PutArray(out, 72, value.catalog_snapshot_uuid);
  PutU64(out, 88, value.catalog_generation);
  PutU64(out, 96, value.datatype_registry_generation);
  PutU64(out, 104, value.security_generation);
  PutU64(out, 112, value.resource_epoch);
  PutArray(out, 120, value.mga_snapshot_uuid);
  PutArray(out, 136, value.literal_budget_uuid);
  PutU64(out, 152, value.literal_budget_generation);
  PutU64(out, 160, value.literal_negotiation_byte_grant);
  PutU64(out, 168, value.canonical_body_aggregate_grant);
  PutArray(out, 176, value.demand_sequence_sha256);
  PutArray(out, 208, target_context_sequence);
  PutArray(out, 240, ordered_profiles);
  std::copy(mappings.begin(), mappings.end(),
            out->begin() + kContextualTextResultHeaderBytesV2);
  PutArray(out, 272, ComputeContextualTextResultCarrierSha256V2(*out));
  return true;
}

bool DecodeContextualTextLiteralProfileSetV2(
    const std::uint8_t* bytes,
    std::size_t size,
    ContextualTextLiteralProfileSetV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr || bytes == nullptr ||
      size < kContextualTextResultHeaderBytesV2 ||
      size > kContextualTextMaximumLogicalCarrierBytesV2 ||
      !ExactMagic(bytes, "SBTLNS02") || GetU16(bytes + 8) != 2 ||
      GetU16(bytes + 10) != kContextualTextResultHeaderBytesV2 ||
      GetU32(bytes + 12) != size || GetU32(bytes + 16) != 0 ||
      GetU32(bytes + 28) != kContextualTextMappingPrefixBytesV2 ||
      !AllZero(bytes + 304, 32))
    return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                "SBTLNS02 header is not canonical");
  const std::uint32_t count = GetU32(bytes + 20);
  if (count == 0 || count > kContextualTextMaximumProfileCountV2 ||
      GetU32(bytes + 24) != size - kContextualTextResultHeaderBytesV2)
    return Fail(diagnostic, "PARSER_SERVER_IPC.FRAME_PAYLOAD_INVALID",
                "SBTLNS02 mapping count or extent is invalid");
  ContextualTextLiteralProfileSetV2 decoded;
  GetArray(bytes + 32, &decoded.statement_receipt_uuid);
  GetArray(bytes + 48, &decoded.profile_set_uuid);
  decoded.profile_set_generation = GetU64(bytes + 64);
  GetArray(bytes + 72, &decoded.catalog_snapshot_uuid);
  decoded.catalog_generation = GetU64(bytes + 88);
  decoded.datatype_registry_generation = GetU64(bytes + 96);
  decoded.security_generation = GetU64(bytes + 104);
  decoded.resource_epoch = GetU64(bytes + 112);
  GetArray(bytes + 120, &decoded.mga_snapshot_uuid);
  GetArray(bytes + 136, &decoded.literal_budget_uuid);
  decoded.literal_budget_generation = GetU64(bytes + 152);
  decoded.literal_negotiation_byte_grant = GetU64(bytes + 160);
  decoded.canonical_body_aggregate_grant = GetU64(bytes + 168);
  GetArray(bytes + 176, &decoded.demand_sequence_sha256);
  GetArray(bytes + 208, &decoded.target_context_sequence_sha256);
  GetArray(bytes + 240, &decoded.ordered_profiles_sha256);
  GetArray(bytes + 272, &decoded.carrier_sha256);
  if (!DecodeMappings(bytes, size, kContextualTextResultHeaderBytesV2, count,
                      &decoded.mappings, diagnostic))
    return false;
  decoded.exact_bytes.assign(bytes, bytes + size);
  Bytes canonical;
  ContextualTextCodecDiagnosticV2 canonical_diagnostic;
  if (!EncodeContextualTextLiteralProfileSetV2(decoded, &canonical,
                                               &canonical_diagnostic)) {
    if (diagnostic != nullptr) *diagnostic = std::move(canonical_diagnostic);
    return false;
  }
  if (canonical != decoded.exact_bytes)
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
                "SBTLNS02 does not equal its canonical re-encoding");
  *out = std::move(decoded);
  return true;
}

bool EncodeContextualTextLiteralExecuteV2(
    const ContextualTextLiteralExecuteV2& value,
    std::vector<std::uint8_t>* out,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr || !ValidProfileSetHeader(value))
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "SBTLXE02 header authority fields are invalid");
  Bytes mappings;
  ContextualTextSha256V2 target_context_sequence{};
  ContextualTextSha256V2 ordered_profiles{};
  if (!EncodeMappings(value, &mappings, &target_context_sequence,
                      &ordered_profiles, diagnostic))
    return false;
  std::size_t total = kContextualTextExecuteHeaderBytesV2;
  if (!CheckedAdd(total, mappings.size(), &total) ||
      total > kContextualTextMaximumLogicalCarrierBytesV2 ||
      total > value.literal_negotiation_byte_grant)
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.BUDGET_EXCEEDED",
                "SBTLXE02 exceeds its receipt-owned byte grant");
  out->assign(total, 0);
  PutMagic(out, "SBTLXE02");
  PutU16(out, 8, 2);
  PutU16(out, 10, kContextualTextExecuteHeaderBytesV2);
  PutU32(out, 12, static_cast<std::uint32_t>(total));
  PutU32(out, 20, static_cast<std::uint32_t>(value.mappings.size()));
  PutU32(out, 24, static_cast<std::uint32_t>(mappings.size()));
  PutU32(out, 28, kContextualTextMappingPrefixBytesV2);
  PutArray(out, 32, value.statement_receipt_uuid);
  PutArray(out, 48, value.profile_set_uuid);
  PutU64(out, 64, value.profile_set_generation);
  PutArray(out, 72, value.catalog_snapshot_uuid);
  PutU64(out, 88, value.catalog_generation);
  PutU64(out, 96, value.datatype_registry_generation);
  PutU64(out, 104, value.security_generation);
  PutU64(out, 112, value.resource_epoch);
  PutArray(out, 120, value.mga_snapshot_uuid);
  PutArray(out, 136, value.literal_budget_uuid);
  PutU64(out, 152, value.literal_budget_generation);
  PutU64(out, 160, value.literal_negotiation_byte_grant);
  PutU64(out, 168, value.canonical_body_aggregate_grant);
  PutArray(out, 176, value.demand_sequence_sha256);
  PutArray(out, 208, target_context_sequence);
  PutArray(out, 240, ordered_profiles);
  PutArray(out, 272, value.pre_contextual_operand_vector_sha256);
  PutArray(out, 304, value.sbxn_sha256);
  std::copy(mappings.begin(), mappings.end(),
            out->begin() + kContextualTextExecuteHeaderBytesV2);
  PutArray(out, 336, ComputeContextualTextExecuteCarrierSha256V2(*out));
  return true;
}

bool DecodeContextualTextLiteralExecuteV2(
    const std::uint8_t* bytes,
    std::size_t size,
    ContextualTextLiteralExecuteV2* out,
    ContextualTextCodecDiagnosticV2* diagnostic) {
  Clear(diagnostic);
  if (out == nullptr || bytes == nullptr ||
      size < kContextualTextExecuteHeaderBytesV2 ||
      size > kContextualTextMaximumLogicalCarrierBytesV2 ||
      !ExactMagic(bytes, "SBTLXE02") || GetU16(bytes + 8) != 2 ||
      GetU16(bytes + 10) != kContextualTextExecuteHeaderBytesV2 ||
      GetU32(bytes + 12) != size || GetU32(bytes + 16) != 0 ||
      GetU32(bytes + 28) != kContextualTextMappingPrefixBytesV2 ||
      !AllZero(bytes + 368, 32))
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "SBTLXE02 header is not canonical");
  const std::uint32_t count = GetU32(bytes + 20);
  if (count == 0 || count > kContextualTextMaximumProfileCountV2 ||
      GetU32(bytes + 24) != size - kContextualTextExecuteHeaderBytesV2)
    return Fail(diagnostic, "SBLR.OPERAND_INVALID",
                "SBTLXE02 mapping count or extent is invalid");
  ContextualTextLiteralExecuteV2 decoded;
  GetArray(bytes + 32, &decoded.statement_receipt_uuid);
  GetArray(bytes + 48, &decoded.profile_set_uuid);
  decoded.profile_set_generation = GetU64(bytes + 64);
  GetArray(bytes + 72, &decoded.catalog_snapshot_uuid);
  decoded.catalog_generation = GetU64(bytes + 88);
  decoded.datatype_registry_generation = GetU64(bytes + 96);
  decoded.security_generation = GetU64(bytes + 104);
  decoded.resource_epoch = GetU64(bytes + 112);
  GetArray(bytes + 120, &decoded.mga_snapshot_uuid);
  GetArray(bytes + 136, &decoded.literal_budget_uuid);
  decoded.literal_budget_generation = GetU64(bytes + 152);
  decoded.literal_negotiation_byte_grant = GetU64(bytes + 160);
  decoded.canonical_body_aggregate_grant = GetU64(bytes + 168);
  GetArray(bytes + 176, &decoded.demand_sequence_sha256);
  GetArray(bytes + 208, &decoded.target_context_sequence_sha256);
  GetArray(bytes + 240, &decoded.ordered_profiles_sha256);
  GetArray(bytes + 272, &decoded.pre_contextual_operand_vector_sha256);
  GetArray(bytes + 304, &decoded.sbxn_sha256);
  GetArray(bytes + 336, &decoded.carrier_sha256);
  if (!DecodeMappings(bytes, size, kContextualTextExecuteHeaderBytesV2, count,
                      &decoded.mappings, diagnostic))
    return false;
  decoded.exact_bytes.assign(bytes, bytes + size);
  Bytes canonical;
  ContextualTextCodecDiagnosticV2 canonical_diagnostic;
  if (!EncodeContextualTextLiteralExecuteV2(decoded, &canonical,
                                            &canonical_diagnostic)) {
    if (diagnostic != nullptr) *diagnostic = std::move(canonical_diagnostic);
    return false;
  }
  if (canonical != decoded.exact_bytes)
    return Fail(diagnostic,
                "SBLR.CONTEXTUAL_TEXT_LITERAL.NON_CANONICAL",
                "SBTLXE02 does not equal its canonical re-encoding");
  *out = std::move(decoded);
  return true;
}

}  // namespace scratchbird::engine::sblr
