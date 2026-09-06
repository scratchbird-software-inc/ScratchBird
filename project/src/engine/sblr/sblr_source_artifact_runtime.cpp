// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_source_artifact_runtime.hpp"

#include "hash_digest.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace scratchbird::engine::sblr {
namespace {

constexpr std::size_t kSymbolFixedSize = 112;
constexpr std::size_t kSpanFixedSize = 96;
constexpr std::size_t kHintFixedSize = 88;
constexpr std::size_t kMaximumNameBytes = 1'024;
constexpr std::size_t kMaximumKeyBytes = 256;
constexpr std::size_t kMaximumLanguageBytes = 35;

bool Fail(std::string* detail, std::string_view reason) {
  if (detail != nullptr) {
    *detail = std::string(reason);
  }
  return false;
}

template <typename T>
bool NonZero(const T& value) {
  return std::any_of(value.begin(), value.end(), [](std::uint8_t byte) {
    return byte != 0;
  });
}

bool IsUuidV7(const SblrSourceArtifactUuidV1& value) {
  return NonZero(value) && (value[6] >> 4U) == 7U &&
         (value[8] & 0xc0U) == 0x80U;
}

void StoreU16(std::vector<std::uint8_t>* bytes,
              std::size_t offset,
              std::uint16_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void StoreU32(std::vector<std::uint8_t>* bytes,
              std::size_t offset,
              std::uint32_t value) {
  for (unsigned index = 0; index != 4; ++index) {
    (*bytes)[offset + index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

void StoreU64(std::vector<std::uint8_t>* bytes,
              std::size_t offset,
              std::uint64_t value) {
  for (unsigned index = 0; index != 8; ++index) {
    (*bytes)[offset + index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

std::uint16_t ReadU16(const std::uint8_t* bytes) {
  return static_cast<std::uint16_t>(bytes[0]) |
         (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t ReadU32(const std::uint8_t* bytes) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index != 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

std::uint64_t ReadU64(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index != 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

template <typename T>
void CopyTo(std::vector<std::uint8_t>* bytes,
            std::size_t offset,
            const T& value) {
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

template <typename T>
void CopyFrom(const std::uint8_t* bytes, T* value) {
  std::copy_n(bytes, value->size(), value->begin());
}

SblrSourceArtifactSha256V1 HashParts(
    std::string_view domain,
    const std::uint8_t* fixed,
    std::size_t fixed_size,
    const std::uint8_t* variable,
    std::size_t variable_size) {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(domain.size() + fixed_size + variable_size);
  bytes.insert(bytes.end(), domain.begin(), domain.end());
  if (fixed_size != 0) {
    bytes.insert(bytes.end(), fixed, fixed + fixed_size);
  }
  if (variable_size != 0) {
    bytes.insert(bytes.end(), variable, variable + variable_size);
  }
  return scratchbird::core::hash::ComputeSha256Digest(bytes).digest;
}

bool ValidUtf8(std::string_view value,
               std::size_t maximum,
               bool allow_empty) {
  if ((!allow_empty && value.empty()) || value.size() > maximum) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto lead = static_cast<std::uint8_t>(value[offset++]);
    if (lead == 0) {
      return false;
    }
    if (lead < 0x80) {
      continue;
    }
    if (lead >= 0xc2 && lead <= 0xdf) {
      if (offset >= value.size() ||
          (static_cast<std::uint8_t>(value[offset]) & 0xc0U) != 0x80U) {
        return false;
      }
      ++offset;
      continue;
    }
    if (lead >= 0xe0 && lead <= 0xef) {
      if (offset + 1 >= value.size()) {
        return false;
      }
      const auto first = static_cast<std::uint8_t>(value[offset]);
      const auto second = static_cast<std::uint8_t>(value[offset + 1]);
      if ((first & 0xc0U) != 0x80U || (second & 0xc0U) != 0x80U ||
          (lead == 0xe0 && first < 0xa0U) ||
          (lead == 0xed && first >= 0xa0U)) {
        return false;
      }
      offset += 2;
      continue;
    }
    if (lead >= 0xf0 && lead <= 0xf4) {
      if (offset + 2 >= value.size()) {
        return false;
      }
      const auto first = static_cast<std::uint8_t>(value[offset]);
      const auto second = static_cast<std::uint8_t>(value[offset + 1]);
      const auto third = static_cast<std::uint8_t>(value[offset + 2]);
      if ((first & 0xc0U) != 0x80U || (second & 0xc0U) != 0x80U ||
          (third & 0xc0U) != 0x80U ||
          (lead == 0xf0 && first < 0x90U) ||
          (lead == 0xf4 && first >= 0x90U)) {
        return false;
      }
      offset += 3;
      continue;
    }
    return false;
  }
  return true;
}

bool ValidLanguageTag(std::string_view value) {
  if (!ValidUtf8(value, kMaximumLanguageBytes, false)) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-';
  });
}

bool ValidSymbolKey(std::string_view value) {
  if (value.empty() || value.size() > kMaximumKeyBytes) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' ||
           byte == '.' || byte == ':';
  });
}

template <typename Enum>
bool EnumInRange(Enum value, std::uint8_t minimum, std::uint8_t maximum) {
  const auto encoded = static_cast<std::uint8_t>(value);
  return encoded >= minimum && encoded <= maximum;
}

bool SymbolShape(const SblrSourceArtifactSymbolV1& symbol,
                 std::uint64_t expected_id,
                 std::string* detail) {
  if (symbol.symbol_id != expected_id ||
      !EnumInRange(symbol.symbol_kind, 1, 13) ||
      !EnumInRange(symbol.quote_style, 0, 4) ||
      !EnumInRange(symbol.redaction_state, 0, 3) ||
      !ValidSymbolKey(symbol.symbol_key)) {
    return Fail(detail, "id");
  }
  const bool object_display_name =
      symbol.symbol_kind ==
      SblrSourceArtifactSymbolKindV1::object_display_name;
  if (NonZero(symbol.related_object_uuid) != object_display_name) {
    return Fail(detail, "object_ref");
  }
  const bool name_must_be_absent =
      symbol.redaction_state ==
          SblrSourceArtifactSymbolRedactionV1::hash_only ||
      symbol.redaction_state ==
          SblrSourceArtifactSymbolRedactionV1::removed;
  if (!ValidUtf8(symbol.raw_name_utf8, kMaximumNameBytes,
                 name_must_be_absent) ||
      !ValidUtf8(symbol.normalized_lookup_key, kMaximumNameBytes, true) ||
      !ValidLanguageTag(symbol.language_tag)) {
    return Fail(detail, "utf8");
  }
  if (symbol.use_node_ids.size() >
      kSblrSourceArtifactMaximumRecordsV1) {
    return Fail(detail, "resource_limit");
  }
  if (!object_display_name && symbol.declaration_node_id == 0) {
    return Fail(detail, "node_ref");
  }
  if (symbol.was_quoted !=
      (symbol.quote_style != SblrSourceArtifactQuoteStyleV1::none)) {
    return Fail(detail, "quote");
  }
  if (name_must_be_absent &&
      (!symbol.raw_name_utf8.empty() ||
       !symbol.normalized_lookup_key.empty())) {
    return Fail(detail, "redaction");
  }
  if (!std::is_sorted(symbol.use_node_ids.begin(),
                      symbol.use_node_ids.end()) ||
      std::adjacent_find(symbol.use_node_ids.begin(),
                         symbol.use_node_ids.end()) !=
          symbol.use_node_ids.end() ||
      std::find(symbol.use_node_ids.begin(), symbol.use_node_ids.end(), 0) !=
          symbol.use_node_ids.end()) {
    return Fail(detail, "node_ref");
  }
  return true;
}

bool SpanShape(const SblrSourceArtifactSpanV1& span,
               std::uint64_t expected_id,
               std::string* detail) {
  if (span.source_span_id != expected_id ||
      !EnumInRange(span.span_kind, 1, 8)) {
    return Fail(detail, "id");
  }
  const bool empty_permitted =
      span.span_kind == SblrSourceArtifactSpanKindV1::generated ||
      span.span_kind == SblrSourceArtifactSpanKindV1::redacted;
  if (!empty_permitted && span.byte_length == 0) {
    return Fail(detail, "extent");
  }
  if (span.byte_start >
      std::numeric_limits<std::uint64_t>::max() - span.byte_length) {
    return Fail(detail, "extent");
  }
  const bool starts_known = span.line_start != 0 || span.column_start != 0;
  const bool ends_known = span.line_end != 0 || span.column_end != 0;
  if ((starts_known && (span.line_start == 0 || span.column_start == 0)) ||
      (ends_known && (span.line_end == 0 || span.column_end == 0)) ||
      (starts_known != ends_known) ||
      (starts_known && span.line_end < span.line_start)) {
    return Fail(detail, "extent");
  }
  return true;
}

bool HintShape(const SblrSourceArtifactRenderHintV1& hint,
               std::uint64_t expected_id,
               std::string* detail) {
  if (hint.render_hint_id != expected_id ||
      (hint.node_id == 0 && hint.symbol_id == 0) ||
      !NonZero(hint.dialect_family_uuid) ||
      !EnumInRange(hint.keyword_case, 0, 3) ||
      !EnumInRange(hint.identifier_render_policy, 0, 4) ||
      !EnumInRange(hint.delimiter_hint, 0, 4) ||
      !EnumInRange(hint.comment_policy, 0, 3)) {
    return Fail(detail, "id");
  }
  if (!ValidUtf8(hint.format_group, kMaximumKeyBytes, true)) {
    return Fail(detail, "utf8");
  }
  return true;
}

bool ArtifactShape(const SblrSourceArtifactMapV1& artifact,
                   std::string* detail) {
  if (!IsUuidV7(artifact.artifact_uuid) ||
      !NonZero(artifact.dialect_family_uuid) ||
      !NonZero(artifact.parser_package_uuid) ||
      (!NonZero(artifact.sblr_envelope_uuid) &&
       !NonZero(artifact.container_request_uuid)) ||
      !EnumInRange(artifact.redaction_class, 0, 4) ||
      !EnumInRange(artifact.decompile_policy, 1, 4)) {
    return Fail(detail, "header");
  }
  if (!ValidLanguageTag(artifact.language_tag)) {
    return Fail(detail, "utf8");
  }
  if (artifact.symbols.size() > kSblrSourceArtifactMaximumRecordsV1 ||
      artifact.source_spans.size() > kSblrSourceArtifactMaximumRecordsV1 ||
      artifact.render_hints.size() > kSblrSourceArtifactMaximumRecordsV1) {
    return Fail(detail, "resource_limit");
  }
  if (artifact.redaction_class == SblrSourceArtifactRedactionClassV1::absent &&
      (!artifact.symbols.empty() || !artifact.source_spans.empty() ||
       !artifact.render_hints.empty() || artifact.source_text_ref.present)) {
    return Fail(detail, "redaction");
  }
  if (artifact.source_text_ref.present) {
    if (!NonZero(artifact.source_text_ref.uuid) ||
        artifact.source_text_ref.declared_size == 0 ||
        artifact.source_text_ref.crc32c == 0 ||
        !NonZero(artifact.source_text_ref.sha256)) {
      return Fail(detail, "header");
    }
  } else if (NonZero(artifact.source_text_ref.uuid) ||
             artifact.source_text_ref.declared_size != 0 ||
             artifact.source_text_ref.crc32c != 0 ||
             NonZero(artifact.source_text_ref.sha256)) {
    return Fail(detail, "header");
  }

  std::unordered_set<std::string> keys;
  for (std::size_t index = 0; index < artifact.symbols.size(); ++index) {
    if (!SymbolShape(artifact.symbols[index], index + 1, detail) ||
        !keys.insert(artifact.symbols[index].symbol_key).second) {
      if (detail != nullptr && detail->empty()) {
        *detail = "id";
      }
      return false;
    }
  }
  for (std::size_t index = 0; index < artifact.source_spans.size(); ++index) {
    if (!SpanShape(artifact.source_spans[index], index + 1, detail)) {
      return false;
    }
  }
  for (std::size_t index = 0; index < artifact.render_hints.size(); ++index) {
    if (!HintShape(artifact.render_hints[index], index + 1, detail)) {
      return false;
    }
  }
  return true;
}

std::vector<std::uint8_t> EncodeSymbol(
    const SblrSourceArtifactSymbolV1& symbol,
    std::string* detail) {
  if (!SymbolShape(symbol, symbol.symbol_id, detail)) {
    return {};
  }
  const std::uint64_t variable_size =
      symbol.use_node_ids.size() * sizeof(std::uint64_t) +
      symbol.symbol_key.size() + symbol.raw_name_utf8.size() +
      symbol.normalized_lookup_key.size() + symbol.language_tag.size();
  if (variable_size > std::numeric_limits<std::uint32_t>::max() ||
      kSymbolFixedSize + variable_size > kSblrSourceArtifactMaximumBytesV1) {
    Fail(detail, "resource_limit");
    return {};
  }
  std::vector<std::uint8_t> bytes(kSymbolFixedSize + variable_size, 0);
  StoreU32(&bytes, 0, static_cast<std::uint32_t>(bytes.size()));
  StoreU64(&bytes, 4, symbol.symbol_id);
  StoreU64(&bytes, 12, symbol.declaration_node_id);
  StoreU64(&bytes, 20, symbol.scope_node_id);
  StoreU64(&bytes, 28, symbol.source_span_id);
  CopyTo(&bytes, 36, symbol.related_object_uuid);
  StoreU32(&bytes, 52, symbol.ordinal);
  bytes[56] = static_cast<std::uint8_t>(symbol.symbol_kind);
  bytes[57] = (symbol.was_quoted ? 1U : 0U) |
              (symbol.generated ? 2U : 0U);
  bytes[58] = static_cast<std::uint8_t>(symbol.quote_style);
  bytes[59] = static_cast<std::uint8_t>(symbol.redaction_state);
  StoreU32(&bytes, 60,
           static_cast<std::uint32_t>(symbol.use_node_ids.size()));
  StoreU16(&bytes, 64,
           static_cast<std::uint16_t>(symbol.symbol_key.size()));
  StoreU16(&bytes, 66,
           static_cast<std::uint16_t>(symbol.raw_name_utf8.size()));
  StoreU16(&bytes, 68,
           static_cast<std::uint16_t>(symbol.normalized_lookup_key.size()));
  StoreU16(&bytes, 70,
           static_cast<std::uint16_t>(symbol.language_tag.size()));
  StoreU32(&bytes, 72,
           static_cast<std::uint32_t>(symbol.use_node_ids.size() * 8U));

  std::size_t offset = kSymbolFixedSize;
  for (const auto node_id : symbol.use_node_ids) {
    StoreU64(&bytes, offset, node_id);
    offset += 8;
  }
  const auto append = [&](std::string_view value) {
    std::copy(value.begin(), value.end(), bytes.begin() + offset);
    offset += value.size();
  };
  append(symbol.symbol_key);
  append(symbol.raw_name_utf8);
  append(symbol.normalized_lookup_key);
  append(symbol.language_tag);

  const auto hash = HashParts(
      "ScratchBird.SblrSourceArtifactSymbol.V1", bytes.data() + 4, 76,
      bytes.data() + kSymbolFixedSize, bytes.size() - kSymbolFixedSize);
  CopyTo(&bytes, 80, hash);
  return bytes;
}

std::vector<std::uint8_t> EncodeSpan(const SblrSourceArtifactSpanV1& span,
                                     std::string* detail) {
  if (!SpanShape(span, span.source_span_id, detail)) {
    return {};
  }
  std::vector<std::uint8_t> bytes(kSpanFixedSize, 0);
  StoreU32(&bytes, 0, kSpanFixedSize);
  StoreU64(&bytes, 4, span.source_span_id);
  StoreU64(&bytes, 12, span.node_id);
  StoreU64(&bytes, 20, span.byte_start);
  StoreU64(&bytes, 28, span.byte_length);
  StoreU32(&bytes, 36, span.line_start);
  StoreU32(&bytes, 40, span.column_start);
  StoreU32(&bytes, 44, span.line_end);
  StoreU32(&bytes, 48, span.column_end);
  bytes[52] = static_cast<std::uint8_t>(span.span_kind);
  const auto hash = HashParts("ScratchBird.SblrSourceArtifactSpan.V1",
                              bytes.data() + 4, 60, nullptr, 0);
  CopyTo(&bytes, 64, hash);
  return bytes;
}

std::vector<std::uint8_t> EncodeHint(
    const SblrSourceArtifactRenderHintV1& hint,
    std::string* detail) {
  if (!HintShape(hint, hint.render_hint_id, detail)) {
    return {};
  }
  std::vector<std::uint8_t> bytes(kHintFixedSize + hint.format_group.size(), 0);
  StoreU32(&bytes, 0, static_cast<std::uint32_t>(bytes.size()));
  StoreU64(&bytes, 4, hint.render_hint_id);
  StoreU64(&bytes, 12, hint.node_id);
  StoreU64(&bytes, 20, hint.symbol_id);
  CopyTo(&bytes, 28, hint.dialect_family_uuid);
  bytes[44] = static_cast<std::uint8_t>(hint.keyword_case);
  bytes[45] = static_cast<std::uint8_t>(hint.identifier_render_policy);
  bytes[46] = static_cast<std::uint8_t>(hint.delimiter_hint);
  bytes[47] = static_cast<std::uint8_t>(hint.comment_policy);
  StoreU16(&bytes, 48,
           static_cast<std::uint16_t>(hint.format_group.size()));
  std::copy(hint.format_group.begin(), hint.format_group.end(),
            bytes.begin() + kHintFixedSize);
  const auto hash = HashParts(
      "ScratchBird.SblrSourceArtifactRenderHint.V1", bytes.data() + 4, 52,
      bytes.data() + kHintFixedSize, bytes.size() - kHintFixedSize);
  CopyTo(&bytes, 56, hash);
  return bytes;
}

bool ParseSymbol(const std::uint8_t* data,
                 std::size_t size,
                 std::uint64_t expected_id,
                 SblrSourceArtifactSymbolV1* symbol,
                 std::size_t* consumed,
                 std::string* detail) {
  if (data == nullptr || symbol == nullptr || consumed == nullptr ||
      size < kSymbolFixedSize) {
    return Fail(detail, "extent");
  }
  const auto record_size = ReadU32(data);
  if (record_size < kSymbolFixedSize || record_size > size ||
      ReadU32(data + 76) != 0) {
    return Fail(detail, "extent");
  }
  const auto use_count = ReadU32(data + 60);
  const auto use_size = ReadU32(data + 72);
  const auto key_size = ReadU16(data + 64);
  const auto raw_size = ReadU16(data + 66);
  const auto normalized_size = ReadU16(data + 68);
  const auto language_size = ReadU16(data + 70);
  const std::uint64_t variable_size =
      static_cast<std::uint64_t>(use_size) + key_size + raw_size +
      normalized_size + language_size;
  if (use_size != static_cast<std::uint64_t>(use_count) * 8U ||
      kSymbolFixedSize + variable_size != record_size) {
    return Fail(detail, "extent");
  }

  SblrSourceArtifactSymbolV1 value;
  value.symbol_id = ReadU64(data + 4);
  value.declaration_node_id = ReadU64(data + 12);
  value.scope_node_id = ReadU64(data + 20);
  value.source_span_id = ReadU64(data + 28);
  CopyFrom(data + 36, &value.related_object_uuid);
  value.ordinal = ReadU32(data + 52);
  value.symbol_kind =
      static_cast<SblrSourceArtifactSymbolKindV1>(data[56]);
  if ((data[57] & ~0x03U) != 0) {
    return Fail(detail, "header");
  }
  value.was_quoted = (data[57] & 0x01U) != 0;
  value.generated = (data[57] & 0x02U) != 0;
  value.quote_style =
      static_cast<SblrSourceArtifactQuoteStyleV1>(data[58]);
  value.redaction_state =
      static_cast<SblrSourceArtifactSymbolRedactionV1>(data[59]);
  CopyFrom(data + 80, &value.record_sha256);
  std::size_t offset = kSymbolFixedSize;
  for (std::uint32_t index = 0; index != use_count; ++index) {
    value.use_node_ids.push_back(ReadU64(data + offset));
    offset += 8;
  }
  const auto read_string = [&](std::size_t extent, std::string* output) {
    output->assign(reinterpret_cast<const char*>(data + offset), extent);
    offset += extent;
  };
  read_string(key_size, &value.symbol_key);
  read_string(raw_size, &value.raw_name_utf8);
  read_string(normalized_size, &value.normalized_lookup_key);
  read_string(language_size, &value.language_tag);
  if (!SymbolShape(value, expected_id, detail)) {
    return false;
  }
  const auto canonical = EncodeSymbol(value, detail);
  if (canonical.size() != record_size ||
      !std::equal(canonical.begin(), canonical.end(), data)) {
    return Fail(detail, "record_hash");
  }
  *symbol = std::move(value);
  *consumed = record_size;
  return true;
}

bool ParseSpan(const std::uint8_t* data,
               std::size_t size,
               std::uint64_t expected_id,
               SblrSourceArtifactSpanV1* span,
               std::size_t* consumed,
               std::string* detail) {
  if (data == nullptr || span == nullptr || consumed == nullptr ||
      size < kSpanFixedSize || ReadU32(data) != kSpanFixedSize ||
      data[53] != 0 || ReadU16(data + 54) != 0 ||
      ReadU64(data + 56) != 0) {
    return Fail(detail, "extent");
  }
  SblrSourceArtifactSpanV1 value;
  value.source_span_id = ReadU64(data + 4);
  value.node_id = ReadU64(data + 12);
  value.byte_start = ReadU64(data + 20);
  value.byte_length = ReadU64(data + 28);
  value.line_start = ReadU32(data + 36);
  value.column_start = ReadU32(data + 40);
  value.line_end = ReadU32(data + 44);
  value.column_end = ReadU32(data + 48);
  value.span_kind = static_cast<SblrSourceArtifactSpanKindV1>(data[52]);
  CopyFrom(data + 64, &value.record_sha256);
  if (!SpanShape(value, expected_id, detail)) {
    return false;
  }
  const auto canonical = EncodeSpan(value, detail);
  if (!std::equal(canonical.begin(), canonical.end(), data)) {
    return Fail(detail, "record_hash");
  }
  *span = std::move(value);
  *consumed = kSpanFixedSize;
  return true;
}

bool ParseHint(const std::uint8_t* data,
               std::size_t size,
               std::uint64_t expected_id,
               SblrSourceArtifactRenderHintV1* hint,
               std::size_t* consumed,
               std::string* detail) {
  if (data == nullptr || hint == nullptr || consumed == nullptr ||
      size < kHintFixedSize) {
    return Fail(detail, "extent");
  }
  const auto record_size = ReadU32(data);
  const auto format_size = ReadU16(data + 48);
  if (record_size != kHintFixedSize + format_size || record_size > size ||
      ReadU16(data + 50) != 0 || ReadU32(data + 52) != 0) {
    return Fail(detail, "extent");
  }
  SblrSourceArtifactRenderHintV1 value;
  value.render_hint_id = ReadU64(data + 4);
  value.node_id = ReadU64(data + 12);
  value.symbol_id = ReadU64(data + 20);
  CopyFrom(data + 28, &value.dialect_family_uuid);
  value.keyword_case =
      static_cast<SblrSourceArtifactKeywordCaseV1>(data[44]);
  value.identifier_render_policy =
      static_cast<SblrSourceArtifactIdentifierPolicyV1>(data[45]);
  value.delimiter_hint =
      static_cast<SblrSourceArtifactQuoteStyleV1>(data[46]);
  value.comment_policy =
      static_cast<SblrSourceArtifactCommentPolicyV1>(data[47]);
  value.format_group.assign(
      reinterpret_cast<const char*>(data + kHintFixedSize), format_size);
  CopyFrom(data + 56, &value.record_sha256);
  if (!HintShape(value, expected_id, detail)) {
    return false;
  }
  const auto canonical = EncodeHint(value, detail);
  if (canonical.size() != record_size ||
      !std::equal(canonical.begin(), canonical.end(), data)) {
    return Fail(detail, "record_hash");
  }
  *hint = std::move(value);
  *consumed = record_size;
  return true;
}

template <typename T>
bool Contains(const std::vector<T>& values, const T& wanted) {
  return std::find(values.begin(), values.end(), wanted) != values.end();
}

}  // namespace

std::vector<std::uint8_t> EncodeSblrSourceArtifactMapV1(
    const SblrSourceArtifactMapV1& artifact,
    std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (!ArtifactShape(artifact, detail)) {
    return {};
  }

  std::vector<std::uint8_t> symbol_records;
  std::vector<std::uint8_t> span_records;
  std::vector<std::uint8_t> hint_records;
  for (const auto& symbol : artifact.symbols) {
    auto record = EncodeSymbol(symbol, detail);
    if (record.empty()) {
      return {};
    }
    symbol_records.insert(symbol_records.end(), record.begin(), record.end());
  }
  for (const auto& span : artifact.source_spans) {
    auto record = EncodeSpan(span, detail);
    if (record.empty()) {
      return {};
    }
    span_records.insert(span_records.end(), record.begin(), record.end());
  }
  for (const auto& hint : artifact.render_hints) {
    auto record = EncodeHint(hint, detail);
    if (record.empty()) {
      return {};
    }
    hint_records.insert(hint_records.end(), record.begin(), record.end());
  }

  const std::uint64_t payload_size = artifact.language_tag.size() +
                                     symbol_records.size() +
                                     span_records.size() + hint_records.size();
  const std::uint64_t total_size =
      kSblrSourceArtifactHeaderSizeV1 + payload_size;
  if (total_size > kSblrSourceArtifactMaximumBytesV1 ||
      payload_size > std::numeric_limits<std::uint32_t>::max()) {
    Fail(detail, "resource_limit");
    return {};
  }

  std::vector<std::uint8_t> bytes(total_size, 0);
  std::copy_n("SAM1", 4, bytes.begin());
  StoreU16(&bytes, 4, 1);
  StoreU16(&bytes, 6, kSblrSourceArtifactHeaderSizeV1);
  StoreU32(&bytes, 8, static_cast<std::uint32_t>(total_size));
  std::uint32_t flags = 0;
  if (NonZero(artifact.sblr_envelope_uuid)) flags |= 1U;
  if (NonZero(artifact.container_request_uuid)) flags |= 2U;
  if (artifact.source_text_ref.present) flags |= 4U;
  StoreU32(&bytes, 12, flags);
  CopyTo(&bytes, 16, artifact.artifact_uuid);
  CopyTo(&bytes, 32, artifact.sblr_envelope_uuid);
  CopyTo(&bytes, 48, artifact.container_request_uuid);
  CopyTo(&bytes, 64, artifact.dialect_family_uuid);
  CopyTo(&bytes, 80, artifact.parser_package_uuid);
  bytes[96] = static_cast<std::uint8_t>(artifact.redaction_class);
  bytes[97] = static_cast<std::uint8_t>(artifact.decompile_policy);
  StoreU16(&bytes, 100,
           static_cast<std::uint16_t>(artifact.language_tag.size()));
  bytes[102] = artifact.source_text_ref.present ? 1U : 0U;
  StoreU32(&bytes, 104,
           static_cast<std::uint32_t>(artifact.symbols.size()));
  StoreU32(&bytes, 108,
           static_cast<std::uint32_t>(artifact.source_spans.size()));
  StoreU32(&bytes, 112,
           static_cast<std::uint32_t>(artifact.render_hints.size()));
  CopyTo(&bytes, 116, artifact.source_text_ref.uuid);
  StoreU64(&bytes, 132, artifact.source_text_ref.declared_size);
  StoreU32(&bytes, 140, artifact.source_text_ref.crc32c);
  CopyTo(&bytes, 144, artifact.source_text_ref.sha256);
  StoreU32(&bytes, 176,
           static_cast<std::uint32_t>(symbol_records.size()));
  StoreU32(&bytes, 180,
           static_cast<std::uint32_t>(span_records.size()));
  StoreU32(&bytes, 184,
           static_cast<std::uint32_t>(hint_records.size()));
  StoreU32(&bytes, 188, static_cast<std::uint32_t>(payload_size));

  std::size_t offset = kSblrSourceArtifactHeaderSizeV1;
  const auto append = [&](const auto& value) {
    std::copy(value.begin(), value.end(), bytes.begin() + offset);
    offset += value.size();
  };
  append(artifact.language_tag);
  append(symbol_records);
  append(span_records);
  append(hint_records);
  const auto artifact_hash = HashParts(
      "ScratchBird.SblrSourceArtifactMap.V1", bytes.data() + 16, 176,
      bytes.data() + kSblrSourceArtifactHeaderSizeV1,
      bytes.size() - kSblrSourceArtifactHeaderSizeV1);
  CopyTo(&bytes, 192, artifact_hash);
  return bytes;
}

SblrSourceArtifactDecodeResultV1 DecodeSblrSourceArtifactMapV1(
    const std::uint8_t* data,
    std::size_t size) {
  SblrSourceArtifactDecodeResultV1 result;
  const auto fail = [&](SblrSourceArtifactDecodeStatusV1 status,
                        std::string_view detail) {
    result.status = status;
    result.detail = std::string(detail);
    result.artifact = {};
    result.canonical_bytes.clear();
    return result;
  };
  if (data == nullptr || size < kSblrSourceArtifactHeaderSizeV1) {
    return fail(SblrSourceArtifactDecodeStatusV1::invalid, "extent");
  }
  if (size > kSblrSourceArtifactMaximumBytesV1) {
    return fail(SblrSourceArtifactDecodeStatusV1::resource_exceeded,
                "resource_limit");
  }
  if (!std::equal(data, data + 4, "SAM1") || ReadU16(data + 4) != 1 ||
      ReadU16(data + 6) != kSblrSourceArtifactHeaderSizeV1 ||
      ReadU32(data + 8) != size || (ReadU32(data + 12) & ~0x07U) != 0 ||
      ReadU16(data + 98) != 0 || data[103] != 0) {
    return fail(SblrSourceArtifactDecodeStatusV1::invalid, "header");
  }
  const auto flags = ReadU32(data + 12);
  const auto language_size = ReadU16(data + 100);
  const auto source_ref_kind = data[102];
  const auto symbol_count = ReadU32(data + 104);
  const auto span_count = ReadU32(data + 108);
  const auto hint_count = ReadU32(data + 112);
  const auto symbol_size = ReadU32(data + 176);
  const auto span_size = ReadU32(data + 180);
  const auto hint_size = ReadU32(data + 184);
  const auto payload_size = ReadU32(data + 188);
  const std::uint64_t calculated_payload =
      static_cast<std::uint64_t>(language_size) + symbol_size + span_size +
      hint_size;
  if (source_ref_kind > 1 ||
      ((flags & 4U) != 0) != (source_ref_kind == 1) ||
      symbol_count > kSblrSourceArtifactMaximumRecordsV1 ||
      span_count > kSblrSourceArtifactMaximumRecordsV1 ||
      hint_count > kSblrSourceArtifactMaximumRecordsV1 ||
      calculated_payload != payload_size ||
      kSblrSourceArtifactHeaderSizeV1 + calculated_payload != size) {
    return fail(SblrSourceArtifactDecodeStatusV1::invalid, "extent");
  }

  SblrSourceArtifactMapV1 artifact;
  CopyFrom(data + 16, &artifact.artifact_uuid);
  CopyFrom(data + 32, &artifact.sblr_envelope_uuid);
  CopyFrom(data + 48, &artifact.container_request_uuid);
  CopyFrom(data + 64, &artifact.dialect_family_uuid);
  CopyFrom(data + 80, &artifact.parser_package_uuid);
  artifact.redaction_class =
      static_cast<SblrSourceArtifactRedactionClassV1>(data[96]);
  artifact.decompile_policy =
      static_cast<SblrSourceArtifactDecompilePolicyV1>(data[97]);
  artifact.source_text_ref.present = source_ref_kind == 1;
  CopyFrom(data + 116, &artifact.source_text_ref.uuid);
  artifact.source_text_ref.declared_size = ReadU64(data + 132);
  artifact.source_text_ref.crc32c = ReadU32(data + 140);
  CopyFrom(data + 144, &artifact.source_text_ref.sha256);
  CopyFrom(data + 192, &artifact.artifact_sha256);
  artifact.language_tag.assign(
      reinterpret_cast<const char*>(data + kSblrSourceArtifactHeaderSizeV1),
      language_size);

  std::size_t offset = kSblrSourceArtifactHeaderSizeV1 + language_size;
  const auto symbol_end = offset + symbol_size;
  for (std::uint32_t index = 0; index != symbol_count; ++index) {
    SblrSourceArtifactSymbolV1 symbol;
    std::size_t consumed = 0;
    std::string detail;
    if (!ParseSymbol(data + offset, symbol_end - offset, index + 1, &symbol,
                     &consumed, &detail)) {
      return fail(SblrSourceArtifactDecodeStatusV1::invalid, detail);
    }
    artifact.symbols.push_back(std::move(symbol));
    offset += consumed;
  }
  if (offset != symbol_end) {
    return fail(SblrSourceArtifactDecodeStatusV1::invalid, "extent");
  }
  const auto span_end = offset + span_size;
  for (std::uint32_t index = 0; index != span_count; ++index) {
    SblrSourceArtifactSpanV1 span;
    std::size_t consumed = 0;
    std::string detail;
    if (!ParseSpan(data + offset, span_end - offset, index + 1, &span,
                   &consumed, &detail)) {
      return fail(SblrSourceArtifactDecodeStatusV1::invalid, detail);
    }
    artifact.source_spans.push_back(std::move(span));
    offset += consumed;
  }
  if (offset != span_end) {
    return fail(SblrSourceArtifactDecodeStatusV1::invalid, "extent");
  }
  const auto hint_end = offset + hint_size;
  for (std::uint32_t index = 0; index != hint_count; ++index) {
    SblrSourceArtifactRenderHintV1 hint;
    std::size_t consumed = 0;
    std::string detail;
    if (!ParseHint(data + offset, hint_end - offset, index + 1, &hint,
                   &consumed, &detail)) {
      return fail(SblrSourceArtifactDecodeStatusV1::invalid, detail);
    }
    artifact.render_hints.push_back(std::move(hint));
    offset += consumed;
  }
  if (offset != hint_end || offset != size) {
    return fail(SblrSourceArtifactDecodeStatusV1::invalid, "extent");
  }

  std::string detail;
  if (!ArtifactShape(artifact, &detail)) {
    return fail(detail == "resource_limit"
                    ? SblrSourceArtifactDecodeStatusV1::resource_exceeded
                    : SblrSourceArtifactDecodeStatusV1::invalid,
                detail);
  }
  const auto canonical = EncodeSblrSourceArtifactMapV1(artifact, &detail);
  if (canonical.size() != size) {
    return fail(SblrSourceArtifactDecodeStatusV1::invalid,
                detail.empty() ? "noncanonical" : detail);
  }
  if (!std::equal(canonical.begin() + 192, canonical.begin() + 224,
                  data + 192)) {
    return fail(SblrSourceArtifactDecodeStatusV1::invalid,
                "artifact_hash");
  }
  if (!std::equal(canonical.begin(), canonical.end(), data)) {
    return fail(SblrSourceArtifactDecodeStatusV1::invalid,
                detail.empty() ? "noncanonical" : detail);
  }
  result.status = SblrSourceArtifactDecodeStatusV1::ok;
  result.artifact = std::move(artifact);
  result.canonical_bytes = std::move(canonical);
  return result;
}

bool ValidateSblrSourceArtifactMapV1(
    const SblrSourceArtifactMapV1& artifact,
    const SblrSourceArtifactValidationContextV1& context,
    std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (!context.operation_validated_without_artifact) {
    return Fail(detail, "authority");
  }
  if (!ArtifactShape(artifact, detail)) {
    return false;
  }
  if (artifact.sblr_envelope_uuid !=
          context.expected_sblr_envelope_uuid ||
      artifact.container_request_uuid !=
          context.expected_container_request_uuid ||
      artifact.dialect_family_uuid !=
          context.expected_dialect_family_uuid ||
      artifact.parser_package_uuid !=
          context.expected_parser_package_uuid) {
    return Fail(detail, "binding");
  }
  if (context.source_preserving_requested) {
    const bool redaction_permits_names =
        artifact.redaction_class == SblrSourceArtifactRedactionClassV1::none ||
        (artifact.redaction_class ==
             SblrSourceArtifactRedactionClassV1::admin_only &&
         context.admin_artifact_access);
    const bool all_names_visible = std::all_of(
        artifact.symbols.begin(), artifact.symbols.end(),
        [](const SblrSourceArtifactSymbolV1& symbol) {
          return symbol.redaction_state ==
                 SblrSourceArtifactSymbolRedactionV1::visible;
        });
    if (artifact.decompile_policy !=
            SblrSourceArtifactDecompilePolicyV1::source_preserving ||
        !redaction_permits_names || !all_names_visible) {
      return Fail(detail, "redaction");
    }
  }

  const bool security_redacted =
      artifact.redaction_class ==
      SblrSourceArtifactRedactionClassV1::security_redacted;
  const auto admitted_node = [&](std::uint64_t node_id) {
    return node_id == 0 || Contains(context.admitted_node_ids, node_id);
  };
  for (const auto& span : artifact.source_spans) {
    if (!admitted_node(span.node_id) &&
        !(security_redacted &&
          span.span_kind == SblrSourceArtifactSpanKindV1::redacted)) {
      return Fail(detail, "node_ref");
    }
  }
  for (const auto& symbol : artifact.symbols) {
    const bool explicit_redacted_placeholder =
        security_redacted &&
        symbol.redaction_state !=
            SblrSourceArtifactSymbolRedactionV1::visible;
    const auto valid_symbol_node = [&](std::uint64_t node_id) {
      return admitted_node(node_id) || explicit_redacted_placeholder;
    };
    if (!valid_symbol_node(symbol.declaration_node_id) ||
        !valid_symbol_node(symbol.scope_node_id) ||
        std::any_of(symbol.use_node_ids.begin(), symbol.use_node_ids.end(),
                    [&](std::uint64_t node_id) {
                      return !valid_symbol_node(node_id);
                    }) ||
        (symbol.source_span_id != 0 &&
         symbol.source_span_id > artifact.source_spans.size())) {
      return Fail(detail, "node_ref");
    }
    if (NonZero(symbol.related_object_uuid) &&
        !Contains(context.admitted_object_uuids,
                  symbol.related_object_uuid)) {
      return Fail(detail, "object_ref");
    }
  }
  for (const auto& hint : artifact.render_hints) {
    const bool explicit_redacted_placeholder =
        security_redacted &&
        (hint.identifier_render_policy ==
             SblrSourceArtifactIdentifierPolicyV1::redacted_placeholder ||
         hint.comment_policy ==
             SblrSourceArtifactCommentPolicyV1::redacted);
    if ((!admitted_node(hint.node_id) &&
         !explicit_redacted_placeholder) ||
        (hint.symbol_id != 0 && hint.symbol_id > artifact.symbols.size()) ||
        hint.dialect_family_uuid != artifact.dialect_family_uuid) {
      return Fail(detail, hint.dialect_family_uuid !=
                              artifact.dialect_family_uuid
                          ? "binding"
                          : "node_ref");
    }
  }
  return true;
}

}  // namespace scratchbird::engine::sblr
