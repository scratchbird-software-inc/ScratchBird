// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_engine_envelope.hpp"

#include "hash_digest.hpp"
#include "sblr_opcode_registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint16_t kFormatMajor = 1;
constexpr std::uint16_t kFormatMinor = 0;
constexpr std::size_t kTrailerSize = 16;
constexpr std::array<std::uint16_t, kSblrOperationSectionCount> kSectionTags{
    0x0001, 0x0002, 0x0003, 0x0004, 0x0005,
    0x0006, 0x0007, 0x0008, 0x0009};
constexpr char kProvenanceDomain[] =
    "ScratchBird.SBOP.ProducerProvenance.V1\0";

SblrEnvelopeDiagnostic Diagnostic(std::string code, std::string message) {
  return SblrEnvelopeDiagnostic{std::move(code), std::move(message), true};
}

SblrDecodeResult DecodeFailure(std::string code, std::string message) {
  SblrDecodeResult result;
  result.diagnostics.push_back(Diagnostic(std::move(code), std::move(message)));
  return result;
}

void Append16(Bytes* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value));
  out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void Append32(Bytes* out, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void Append64(Bytes* out, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void Store16(Bytes* out, std::size_t offset, std::uint16_t value) {
  (*out)[offset] = static_cast<std::uint8_t>(value);
  (*out)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void Store32(Bytes* out, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    (*out)[offset + (shift / 8)] = static_cast<std::uint8_t>(value >> shift);
  }
}

void Store64(Bytes* out, std::size_t offset, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    (*out)[offset + (shift / 8)] = static_cast<std::uint8_t>(value >> shift);
  }
}

std::uint16_t Load16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>(data[0]) |
         static_cast<std::uint16_t>(data[1]) << 8;
}

std::uint32_t Load32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         static_cast<std::uint32_t>(data[1]) << 8 |
         static_cast<std::uint32_t>(data[2]) << 16 |
         static_cast<std::uint32_t>(data[3]) << 24;
}

std::uint64_t Load64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift != 64; shift += 8) {
    value |= static_cast<std::uint64_t>(data[shift / 8]) << shift;
  }
  return value;
}

class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool Read16(std::uint16_t* value) {
    if (!Take(2, &last_)) return false;
    *value = Load16(last_);
    return true;
  }

  bool Read32(std::uint32_t* value) {
    if (!Take(4, &last_)) return false;
    *value = Load32(last_);
    return true;
  }

  bool Read64(std::uint64_t* value) {
    if (!Take(8, &last_)) return false;
    *value = Load64(last_);
    return true;
  }

  bool Take(std::size_t count, const std::uint8_t** value) {
    if (count > size_ - offset_) return false;
    *value = data_ + offset_;
    offset_ += count;
    return true;
  }

  std::size_t remaining() const { return size_ - offset_; }
  std::size_t offset() const { return offset_; }

 private:
  const std::uint8_t* data_ = nullptr;
  const std::uint8_t* last_ = nullptr;
  std::size_t size_ = 0;
  std::size_t offset_ = 0;
};

bool IsValidUtf8(std::string_view value) {
  const auto* data = reinterpret_cast<const std::uint8_t*>(value.data());
  std::size_t i = 0;
  while (i < value.size()) {
    const std::uint8_t first = data[i];
    if (first <= 0x7f) {
      if (first == 0 || first < 0x20) return false;
      ++i;
      continue;
    }
    std::size_t width = 0;
    std::uint32_t codepoint = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      width = 2;
      codepoint = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      width = 3;
      codepoint = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      width = 4;
      codepoint = first & 0x07;
    } else {
      return false;
    }
    if (width > value.size() - i) return false;
    for (std::size_t j = 1; j < width; ++j) {
      if ((data[i + j] & 0xc0) != 0x80) return false;
      codepoint = (codepoint << 6) | (data[i + j] & 0x3f);
    }
    if ((width == 3 && codepoint < 0x800) ||
        (width == 4 && codepoint < 0x10000) ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
        codepoint > 0x10ffff) {
      return false;
    }
    i += width;
  }
  return true;
}

bool IsOperationKey(std::string_view value, std::size_t maximum) {
  if (value.empty() || value.size() > maximum ||
      value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  bool component_start = false;
  for (std::size_t i = 1; i < value.size(); ++i) {
    const char ch = value[i];
    if (ch == '.') {
      if (component_start || i + 1 == value.size()) return false;
      component_start = true;
      continue;
    }
    if (component_start) {
      if (ch < 'a' || ch > 'z') return false;
      component_start = false;
      continue;
    }
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
      return false;
    }
  }
  return true;
}

bool IsOpcodeMnemonic(std::string_view value) {
  if (value.empty() || value.size() > 256 ||
      value.front() < 'A' || value.front() > 'Z') {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
  });
}

void AppendText(Bytes* out, std::string_view value) {
  Append32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool DecodeText(const std::uint8_t* data,
                std::size_t size,
                std::size_t maximum,
                std::string* value) {
  if (size < 4) return false;
  const std::uint32_t count = Load32(data);
  if (count == 0 || count > maximum || size != static_cast<std::size_t>(count) + 4) {
    return false;
  }
  std::string decoded(reinterpret_cast<const char*>(data + 4), count);
  if (!IsValidUtf8(decoded)) return false;
  *value = std::move(decoded);
  return true;
}

int HexNibble(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  return -1;
}

bool ParseUuid(std::string_view text, std::array<std::uint8_t, 16>* uuid) {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    return false;
  }
  std::size_t out = 0;
  bool nonzero = false;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '-') {
      ++i;
      continue;
    }
    if (i + 1 >= text.size()) return false;
    const int high = HexNibble(text[i]);
    const int low = HexNibble(text[i + 1]);
    if (high < 0 || low < 0 || out == uuid->size()) return false;
    (*uuid)[out] = static_cast<std::uint8_t>((high << 4) | low);
    nonzero = nonzero || (*uuid)[out] != 0;
    ++out;
    i += 2;
  }
  return out == uuid->size() && nonzero;
}

bool IsNonzeroUuidBytes(const std::uint8_t* data) {
  for (std::size_t i = 0; i < 16; ++i) {
    if (data[i] != 0) return true;
  }
  return false;
}

std::string FormatUuid(const std::uint8_t* uuid) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back('-');
    text.push_back(kHex[uuid[i] >> 4]);
    text.push_back(kHex[uuid[i] & 0x0f]);
  }
  return text;
}

bool ValidateValueBody(SblrValueKind kind,
                       const std::uint8_t* data,
                       std::size_t size,
                       std::uint32_t depth,
                       std::uint64_t* value_count,
                       bool* limit_exceeded);

bool ValidateNestedValue(Reader* reader,
                         std::uint32_t depth,
                         std::uint64_t* value_count,
                         bool* limit_exceeded) {
  std::uint16_t raw_kind = 0;
  std::uint16_t flags = 0;
  std::uint64_t size = 0;
  if (!reader->Read16(&raw_kind) || !reader->Read16(&flags) ||
      !reader->Read64(&size) || flags != 0 || size > reader->remaining()) {
    return false;
  }
  const std::uint8_t* body = nullptr;
  if (!reader->Take(static_cast<std::size_t>(size), &body)) return false;
  return ValidateValueBody(static_cast<SblrValueKind>(raw_kind), body,
                           static_cast<std::size_t>(size), depth,
                           value_count, limit_exceeded);
}

bool ValidateValueBody(SblrValueKind kind,
                       const std::uint8_t* data,
                       std::size_t size,
                       std::uint32_t depth,
                       std::uint64_t* value_count,
                       bool* limit_exceeded) {
  if (depth > kSblrOperationMaximumDepth ||
      ++(*value_count) > kSblrOperationMaximumValues) {
    *limit_exceeded = true;
    return false;
  }
  switch (kind) {
    case SblrValueKind::uuid_ref:
    case SblrValueKind::descriptor_ref:
    case SblrValueKind::policy_ref:
    case SblrValueKind::principal_ref:
    case SblrValueKind::udr_ref:
      return size == 16 && IsNonzeroUuidBytes(data);
    case SblrValueKind::literal_typed:
    case SblrValueKind::proof_token: {
      if (size < 24 || !IsNonzeroUuidBytes(data)) return false;
      const std::uint64_t count = Load64(data + 16);
      if (count > kSblrOperationMaximumScalarBytes) {
        *limit_exceeded = true;
        return false;
      }
      return count == size - 24;
    }
    case SblrValueKind::parameter_slot:
    case SblrValueKind::result_target:
      return size == 20 && Load32(data) != 0 && IsNonzeroUuidBytes(data + 4);
    case SblrValueKind::epoch_token:
      return size == 12 && Load16(data) != 0 && Load16(data + 2) == 0;
    case SblrValueKind::profile_ref:
      return size == 24 && IsNonzeroUuidBytes(data);
    case SblrValueKind::artifact_ref:
      if (size < 29 || !IsNonzeroUuidBytes(data)) return false;
      if (data[24] == 1) return size == 29;
      if (data[24] == 2) return size == 57;
      return false;
    case SblrValueKind::list: {
      if (size < 4) return false;
      Reader reader(data, size);
      std::uint32_t count = 0;
      if (!reader.Read32(&count)) return false;
      if (count > kSblrOperationMaximumValues - *value_count) {
        *limit_exceeded = true;
        return false;
      }
      for (std::uint32_t i = 0; i < count; ++i) {
        if (!ValidateNestedValue(&reader, depth + 1, value_count, limit_exceeded)) {
          return false;
        }
      }
      return reader.remaining() == 0;
    }
    case SblrValueKind::map: {
      if (size < 4) return false;
      Reader reader(data, size);
      std::uint32_t count = 0;
      if (!reader.Read32(&count)) return false;
      if (count > kSblrOperationMaximumValues - *value_count) {
        *limit_exceeded = true;
        return false;
      }
      std::string previous;
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t key_size = 0;
        if (!reader.Read32(&key_size) || key_size == 0 || key_size > 256 ||
            key_size > reader.remaining()) {
          return false;
        }
        const std::uint8_t* key_data = nullptr;
        if (!reader.Take(key_size, &key_data)) return false;
        std::string key(reinterpret_cast<const char*>(key_data), key_size);
        if (!IsValidUtf8(key) ||
            (!previous.empty() &&
             !std::lexicographical_compare(previous.begin(), previous.end(),
                                           key.begin(), key.end(),
                                           [](unsigned char lhs, unsigned char rhs) {
                                             return lhs < rhs;
                                           }))) {
          return false;
        }
        previous = std::move(key);
        if (!ValidateNestedValue(&reader, depth + 1, value_count, limit_exceeded)) {
          return false;
        }
      }
      return reader.remaining() == 0;
    }
    case SblrValueKind::null_value:
      return size == 0;
  }
  return false;
}

Bytes EncodeOperandVector(const std::vector<SblrOperand>& operands) {
  Bytes section;
  Append32(&section, static_cast<std::uint32_t>(operands.size()));
  for (const auto& operand : operands) {
    Append32(&section, operand.ordinal);
    AppendText(&section, operand.type);
    AppendText(&section, operand.name);
    Append16(&section, static_cast<std::uint16_t>(operand.value_kind));
    Append16(&section, operand.value_flags);
    Append64(&section, operand.value_body.size());
    section.insert(section.end(), operand.value_body.begin(), operand.value_body.end());
  }
  return section;
}

bool DecodeOperandVector(const std::uint8_t* data,
                         std::size_t size,
                         std::vector<SblrOperand>* operands,
                         bool* limit_exceeded) {
  Reader reader(data, size);
  std::uint32_t count = 0;
  if (!reader.Read32(&count)) return false;
  if (count > kSblrOperationMaximumOperands) {
    *limit_exceeded = true;
    return false;
  }
  operands->reserve(count);
  std::uint64_t value_count = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    SblrOperand operand;
    std::uint32_t type_size = 0;
    std::uint32_t slot_size = 0;
    std::uint16_t raw_kind = 0;
    std::uint64_t value_size = 0;
    if (!reader.Read32(&operand.ordinal) || operand.ordinal != index + 1 ||
        !reader.Read32(&type_size) || type_size == 0 || type_size > 256 ||
        type_size > reader.remaining()) {
      return false;
    }
    const std::uint8_t* text_data = nullptr;
    if (!reader.Take(type_size, &text_data)) return false;
    operand.type.assign(reinterpret_cast<const char*>(text_data), type_size);
    if (!IsValidUtf8(operand.type) || !IsOperationKey(operand.type, 256) ||
        !reader.Read32(&slot_size) || slot_size == 0 || slot_size > 256 ||
        slot_size > reader.remaining()) {
      return false;
    }
    if (!reader.Take(slot_size, &text_data)) return false;
    operand.name.assign(reinterpret_cast<const char*>(text_data), slot_size);
    if (!IsValidUtf8(operand.name) || !IsOperationKey(operand.name, 256) ||
        !reader.Read16(&raw_kind) || !reader.Read16(&operand.value_flags) ||
        !reader.Read64(&value_size) || operand.value_flags != 0 ||
        value_size > reader.remaining()) {
      return false;
    }
    operand.value_kind = static_cast<SblrValueKind>(raw_kind);
    const std::uint8_t* value_data = nullptr;
    if (!reader.Take(static_cast<std::size_t>(value_size), &value_data) ||
        !ValidateValueBody(operand.value_kind, value_data,
                           static_cast<std::size_t>(value_size), 1,
                           &value_count, limit_exceeded)) {
      return false;
    }
    operand.value_body.assign(value_data, value_data + value_size);
    operands->push_back(std::move(operand));
  }
  return reader.remaining() == 0;
}

Bytes ProducerIdentity(const SblrOperationEnvelope& envelope,
                       const std::array<std::uint8_t, 16>& uuid) {
  Bytes section(uuid.begin(), uuid.end());
  Append32(&section, envelope.parser_package_version_major);
  Append32(&section, envelope.parser_package_version_minor);
  Append32(&section, envelope.parser_package_version_patch);
  return section;
}

std::array<std::uint8_t, 32> ProvenanceDigest(
    const SblrOperationEnvelope& envelope,
    const Bytes& producer,
    const Bytes& registry,
    const Bytes& operation_key,
    const Bytes& mnemonic,
    bool* ok) {
  Bytes input;
  input.insert(input.end(), std::begin(kProvenanceDomain),
               std::end(kProvenanceDomain) - 1);
  input.insert(input.end(), producer.begin(), producer.end());
  input.insert(input.end(), registry.begin(), registry.end());
  Append16(&input, envelope.opcode_code);
  Append16(&input, envelope.operation_version_major);
  Append16(&input, envelope.operation_version_minor);
  input.insert(input.end(), operation_key.begin(), operation_key.end());
  input.insert(input.end(), mnemonic.begin(), mnemonic.end());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(input);
  *ok = digest.ok();
  return digest.digest;
}

bool HasSourceArtifactMetadata(const SblrSourceArtifactMap& map) {
  return map.policy_status != "absent" || !map.source_identity.empty() ||
         !map.source_hash.empty() || !map.symbols.empty() ||
         !map.operation_render_hints.empty() || map.contains_sql_text ||
         map.raw_sql_text_authoritative || !map.render_metadata_only;
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00" << kHex[(ch >> 4) & 0x0f] << kHex[ch & 0x0f];
        } else {
          out << ch;
        }
    }
  }
  return out.str();
}

}  // namespace

std::uint32_t SblrCrc32c(const std::uint8_t* data, std::size_t size) noexcept {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

SblrOperationEnvelope MakeSblrEnvelope(std::string operation_id,
                                       std::string opcode,
                                       std::string trace_key) {
  SblrOperationEnvelope envelope;
  envelope.operation_id = std::move(operation_id);
  envelope.opcode = std::move(opcode);
  envelope.trace_key = std::move(trace_key);
  envelope.result_shape = "engine.api.result.v1";
  envelope.diagnostic_shape = "engine.diagnostic.v1";
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

SblrEnvelopeValidationResult ValidateSblrEnvelope(const SblrOperationEnvelope& envelope) {
  SblrEnvelopeValidationResult result;
  result.ok = true;
  const auto fail = [&result](std::string code, std::string message) {
    result.ok = false;
    result.diagnostics.push_back(Diagnostic(std::move(code), std::move(message)));
  };

  if (envelope.envelope_major != kEngineSblrEnvelopeMajor ||
      envelope.envelope_minor != kEngineSblrEnvelopeMinor ||
      envelope.operation_version_major != 1 || envelope.operation_version_minor != 0) {
    fail("SBLR.OPERATION.VERSION_INVALID", "SBOP v1.0 is the only admitted operation encoding");
  }
  if (!IsOperationKey(envelope.operation_id, 1024) ||
      !IsOpcodeMnemonic(envelope.opcode)) {
    fail("SBLR.OPERATION.TEXT_INVALID", "operation key or opcode mnemonic is not canonical");
  }
  if (envelope.result_shape.empty() || envelope.result_shape.size() > 1024 ||
      !IsValidUtf8(envelope.result_shape) || envelope.diagnostic_shape.empty() ||
      envelope.diagnostic_shape.size() > 1024 || !IsValidUtf8(envelope.diagnostic_shape) ||
      envelope.trace_key.empty() || envelope.trace_key.size() > 4096 ||
      !IsValidUtf8(envelope.trace_key)) {
    fail("SBLR.OPERATION.TEXT_INVALID", "result, diagnostic, or trace identity is not canonical");
  }
  std::array<std::uint8_t, 16> uuid{};
  if (!ParseUuid(envelope.parser_package_uuid, &uuid) ||
      !ParseUuid(envelope.registry_snapshot_uuid, &uuid)) {
    fail("SBLR.OPERATION.HEADER_INVALID", "producer and registry identities require nonzero canonical UUIDs");
  }
  const auto identity = ValidateSblrOpcodeIdentity(envelope.opcode_code,
                                                   envelope.operation_id,
                                                   envelope.opcode);
  if (!identity.ok) {
    fail("SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH", identity.detail);
  }
  if (envelope.contains_sql_text || HasSourceArtifactMetadata(envelope.source_artifact_map)) {
    fail("SBLR.OPERATION.DUPLICATE_INGRESS_AUTHORITY",
         "SBOP cannot carry SQL text, source artifacts, or SBEE-owned authority");
  }
  if (envelope.operands.size() > kSblrOperationMaximumOperands) {
    fail("SBLR.OPERATION.LIMIT_EXCEEDED", "operand count exceeds the v1 limit");
  }
  std::uint64_t value_count = 0;
  for (std::size_t i = 0; i < envelope.operands.size(); ++i) {
    const auto& operand = envelope.operands[i];
    bool limit_exceeded = false;
    if (operand.ordinal != i + 1 || !operand.value.empty() ||
        operand.value_flags != 0 || !IsOperationKey(operand.type, 256) ||
        !IsOperationKey(operand.name, 256) ||
        !ValidateValueBody(operand.value_kind, operand.value_body.data(),
                           operand.value_body.size(), 1, &value_count,
                           &limit_exceeded)) {
      fail(limit_exceeded ? "SBLR.OPERATION.LIMIT_EXCEEDED"
                          : "SBLR.OPERATION.OPERAND_INVALID",
           "operand vector is not canonical typed SBOP data");
      break;
    }
  }
  return result;
}

std::string EncodeSblrEnvelope(const SblrOperationEnvelope& envelope) {
  if (!ValidateSblrEnvelope(envelope).ok) return {};

  std::array<std::uint8_t, 16> producer_uuid{};
  std::array<std::uint8_t, 16> registry_uuid{};
  if (!ParseUuid(envelope.parser_package_uuid, &producer_uuid) ||
      !ParseUuid(envelope.registry_snapshot_uuid, &registry_uuid)) {
    return {};
  }

  std::array<Bytes, kSblrOperationSectionCount> sections;
  AppendText(&sections[0], envelope.operation_id);
  AppendText(&sections[1], envelope.opcode);
  sections[2] = ProducerIdentity(envelope, producer_uuid);
  sections[3].assign(registry_uuid.begin(), registry_uuid.end());
  sections[4] = EncodeOperandVector(envelope.operands);
  AppendText(&sections[5], envelope.result_shape);
  AppendText(&sections[6], envelope.diagnostic_shape);
  AppendText(&sections[7], envelope.trace_key);
  bool digest_ok = false;
  const auto provenance = ProvenanceDigest(envelope, sections[2], sections[3],
                                           sections[0], sections[1], &digest_ok);
  if (!digest_ok) return {};
  sections[8].assign(provenance.begin(), provenance.end());

  std::uint64_t payload_size = 0;
  for (const auto& section : sections) {
    if (section.empty() || section.size() > kSblrOperationMaximumBytes - payload_size) {
      return {};
    }
    payload_size += section.size();
  }
  const std::uint64_t total_size = kSblrOperationSectionPayloadOffset +
                                   payload_size + kTrailerSize;
  if (total_size > kSblrOperationMaximumBytes ||
      total_size > std::numeric_limits<std::size_t>::max()) {
    return {};
  }

  Bytes encoded(static_cast<std::size_t>(total_size), 0);
  Store32(&encoded, 0, kSblrOperationMagic);
  Store16(&encoded, 4, kFormatMajor);
  Store16(&encoded, 6, kFormatMinor);
  Store16(&encoded, 8, kSblrOperationHeaderSize);
  Store16(&encoded, 10, kSblrOperationSectionCount);
  Store16(&encoded, 16, envelope.opcode_code);
  Store16(&encoded, 18, envelope.operation_version_major);
  Store16(&encoded, 20, envelope.operation_version_minor);
  Store32(&encoded, 24, kSblrOperationHeaderSize);
  Store32(&encoded, 28, kSblrOperationSectionTableSize);
  Store32(&encoded, 32, kSblrOperationSectionPayloadOffset);
  Store64(&encoded, 40, payload_size);
  Store64(&encoded, 48, total_size);

  std::uint64_t section_offset = kSblrOperationSectionPayloadOffset;
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const std::size_t entry = kSblrOperationHeaderSize + i * 24;
    Store16(&encoded, entry, kSectionTags[i]);
    Store16(&encoded, entry + 2, 1);
    Store32(&encoded, entry + 4, 1);
    Store64(&encoded, entry + 8, section_offset);
    Store64(&encoded, entry + 16, sections[i].size());
    std::copy(sections[i].begin(), sections[i].end(),
              encoded.begin() + static_cast<std::ptrdiff_t>(section_offset));
    section_offset += sections[i].size();
  }
  const std::size_t trailer = encoded.size() - kTrailerSize;
  Store32(&encoded, trailer, kSblrOperationTrailerMagic);
  Store32(&encoded, trailer + 4, SblrCrc32c(encoded.data(), trailer));
  Store64(&encoded, trailer + 8, total_size);
  return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

SblrDecodeResult DecodeSblrEnvelope(std::string_view encoded) {
  if (encoded.size() > kSblrOperationMaximumBytes) {
    return DecodeFailure("SBLR.OPERATION.LIMIT_EXCEEDED", "operation exceeds the v1 byte limit");
  }
  if (encoded.size() < kSblrOperationSectionPayloadOffset + kTrailerSize) {
    return DecodeFailure("SBLR.OPERATION.HEADER_INVALID", "operation is smaller than the v1 fixed structure");
  }
  const auto* data = reinterpret_cast<const std::uint8_t*>(encoded.data());
  if (Load32(data) != kSblrOperationMagic) {
    return DecodeFailure("SBLR.OPERATION.MAGIC_INVALID", "operation magic is not literal SBOP");
  }
  if (Load16(data + 4) != kFormatMajor || Load16(data + 6) != kFormatMinor ||
      Load16(data + 18) != 1 || Load16(data + 20) != 0) {
    return DecodeFailure("SBLR.OPERATION.VERSION_INVALID", "operation version is unsupported");
  }
  const std::uint64_t payload_size = Load64(data + 40);
  const std::uint64_t declared_total = Load64(data + 48);
  if (declared_total != encoded.size() ||
      payload_size != encoded.size() - kSblrOperationSectionPayloadOffset - kTrailerSize) {
    return DecodeFailure("SBLR.OPERATION.TOTAL_SIZE_MISMATCH", "header total size does not match the byte stream");
  }
  if (Load16(data + 8) != kSblrOperationHeaderSize ||
      Load16(data + 10) != kSblrOperationSectionCount || Load32(data + 12) != 0 ||
      Load16(data + 16) == 0 || Load16(data + 22) != 0 ||
      Load32(data + 24) != kSblrOperationHeaderSize ||
      Load32(data + 28) != kSblrOperationSectionTableSize ||
      Load32(data + 32) != kSblrOperationSectionPayloadOffset ||
      Load32(data + 36) != 0 || Load64(data + 56) != 0) {
    return DecodeFailure("SBLR.OPERATION.HEADER_INVALID", "operation header contains a noncanonical field");
  }

  const std::size_t trailer = encoded.size() - kTrailerSize;
  if (Load32(data + trailer) != kSblrOperationTrailerMagic) {
    return DecodeFailure("SBLR.OPERATION.HEADER_INVALID", "operation trailer magic is invalid");
  }
  if (Load64(data + trailer + 8) != declared_total) {
    return DecodeFailure("SBLR.OPERATION.TOTAL_SIZE_MISMATCH", "trailer total size does not match the header");
  }
  if (Load32(data + trailer + 4) != SblrCrc32c(data, trailer)) {
    return DecodeFailure("SBLR.OPERATION.CRC_MISMATCH", "operation CRC-32C differs");
  }

  struct SectionView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
  };
  std::array<SectionView, kSblrOperationSectionCount> sections;
  std::uint64_t expected_offset = kSblrOperationSectionPayloadOffset;
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const std::size_t entry = kSblrOperationHeaderSize + i * 24;
    const std::uint64_t offset = Load64(data + entry + 8);
    const std::uint64_t size = Load64(data + entry + 16);
    if (Load16(data + entry) != kSectionTags[i] || Load16(data + entry + 2) != 1 ||
        Load32(data + entry + 4) != 1) {
      return DecodeFailure("SBLR.OPERATION.SECTION_TABLE_INVALID", "section table identity, version, or flags differ");
    }
    if (size == 0) {
      return DecodeFailure("SBLR.OPERATION.SECTION_MISSING", "a required section is absent");
    }
    if (offset != expected_offset || size > trailer - expected_offset) {
      return DecodeFailure("SBLR.OPERATION.SECTION_OVERLAP_OR_GAP", "section offsets are not a contiguous ordinal concatenation");
    }
    sections[i] = {data + static_cast<std::size_t>(offset), static_cast<std::size_t>(size)};
    expected_offset += size;
  }
  if (expected_offset != trailer) {
    return DecodeFailure("SBLR.OPERATION.SECTION_OVERLAP_OR_GAP", "section payload has a gap, padding, or size mismatch");
  }

  SblrOperationEnvelope envelope;
  envelope.opcode_code = Load16(data + 16);
  envelope.operation_version_major = Load16(data + 18);
  envelope.operation_version_minor = Load16(data + 20);
  if (!DecodeText(sections[0].data, sections[0].size, 1024, &envelope.operation_id) ||
      !IsOperationKey(envelope.operation_id, 1024) ||
      !DecodeText(sections[1].data, sections[1].size, 256, &envelope.opcode) ||
      !IsOpcodeMnemonic(envelope.opcode) ||
      !DecodeText(sections[5].data, sections[5].size, 1024, &envelope.result_shape) ||
      !DecodeText(sections[6].data, sections[6].size, 1024, &envelope.diagnostic_shape) ||
      !DecodeText(sections[7].data, sections[7].size, 4096, &envelope.trace_key)) {
    return DecodeFailure("SBLR.OPERATION.TEXT_INVALID", "operation contains noncanonical text");
  }
  if (sections[2].size != 28 || !IsNonzeroUuidBytes(sections[2].data) ||
      sections[3].size != 16 || !IsNonzeroUuidBytes(sections[3].data)) {
    return DecodeFailure("SBLR.OPERATION.HEADER_INVALID", "producer or registry section is invalid");
  }
  envelope.parser_package_uuid = FormatUuid(sections[2].data);
  envelope.parser_package_version_major = Load32(sections[2].data + 16);
  envelope.parser_package_version_minor = Load32(sections[2].data + 20);
  envelope.parser_package_version_patch = Load32(sections[2].data + 24);
  envelope.registry_snapshot_uuid = FormatUuid(sections[3].data);
  envelope.parser_resolved_names_to_uuids = true;
  bool limit_exceeded = false;
  if (!DecodeOperandVector(sections[4].data, sections[4].size,
                           &envelope.operands, &limit_exceeded)) {
    return DecodeFailure(limit_exceeded ? "SBLR.OPERATION.LIMIT_EXCEEDED"
                                        : "SBLR.OPERATION.OPERAND_INVALID",
                         "operand vector is malformed or noncanonical");
  }
  if (sections[8].size != 32) {
    return DecodeFailure("SBLR.OPERATION.PROVENANCE_MISMATCH", "producer provenance has the wrong size");
  }
  Bytes producer(sections[2].data, sections[2].data + sections[2].size);
  Bytes registry(sections[3].data, sections[3].data + sections[3].size);
  Bytes operation_key(sections[0].data, sections[0].data + sections[0].size);
  Bytes mnemonic(sections[1].data, sections[1].data + sections[1].size);
  bool digest_ok = false;
  const auto provenance = ProvenanceDigest(envelope, producer, registry,
                                           operation_key, mnemonic, &digest_ok);
  if (!digest_ok || !std::equal(provenance.begin(), provenance.end(), sections[8].data)) {
    return DecodeFailure("SBLR.OPERATION.PROVENANCE_MISMATCH", "producer provenance SHA-256 differs");
  }
  const auto validation = ValidateSblrEnvelope(envelope);
  if (!validation.ok) {
    SblrDecodeResult result;
    result.diagnostics = validation.diagnostics;
    return result;
  }
  const std::string canonical = EncodeSblrEnvelope(envelope);
  if (canonical.size() != encoded.size() ||
      !std::equal(canonical.begin(), canonical.end(), encoded.begin())) {
    return DecodeFailure("SBLR.OPERATION.NONCANONICAL", "decoded operation does not re-encode byte-for-byte");
  }

  SblrDecodeResult result;
  result.ok = true;
  result.envelope = std::move(envelope);
  result.canonical_bytes.assign(data, data + encoded.size());
  return result;
}

std::string SerializeSblrEnvelopeToJson(const SblrOperationEnvelope& envelope) {
  std::ostringstream out;
  out << "{\n"
      << "  \"format\": \"SBOP\",\n"
      << "  \"format_version\": \"1.0\",\n"
      << "  \"opcode_code\": " << envelope.opcode_code << ",\n"
      << "  \"operation_id\": \"" << JsonEscape(envelope.operation_id) << "\",\n"
      << "  \"opcode\": \"" << JsonEscape(envelope.opcode) << "\",\n"
      << "  \"producer_uuid\": \"" << JsonEscape(envelope.parser_package_uuid) << "\",\n"
      << "  \"registry_snapshot_uuid\": \"" << JsonEscape(envelope.registry_snapshot_uuid) << "\",\n"
      << "  \"result_shape\": \"" << JsonEscape(envelope.result_shape) << "\",\n"
      << "  \"diagnostic_shape\": \"" << JsonEscape(envelope.diagnostic_shape) << "\",\n"
      << "  \"trace_key\": \"" << JsonEscape(envelope.trace_key) << "\",\n"
      << "  \"operand_count\": " << envelope.operands.size() << "\n"
      << "}\n";
  return out.str();
}

std::string SerializeSblrValidationToJson(const SblrEnvelopeValidationResult& result) {
  std::ostringstream out;
  out << "{\n  \"ok\": " << (result.ok ? "true" : "false")
      << ",\n  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < result.diagnostics.size(); ++i) {
    const auto& diagnostic = result.diagnostics[i];
    out << "    {\"code\": \"" << JsonEscape(diagnostic.code)
        << "\", \"message\": \"" << JsonEscape(diagnostic.message)
        << "\", \"error\": " << (diagnostic.error ? "true" : "false") << "}";
    if (i + 1 != result.diagnostics.size()) out << ',';
    out << '\n';
  }
  out << "  ]\n}\n";
  return out.str();
}

}  // namespace scratchbird::engine::sblr
