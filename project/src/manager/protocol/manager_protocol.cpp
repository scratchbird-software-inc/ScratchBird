// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBMI_MANAGER_PROTOCOL_LIBRARY

#include "manager_protocol.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace scratchbird::manager::protocol {
namespace {

constexpr std::uint32_t kSbdbMagic = 0x42444253u;
constexpr std::uint16_t kSbdbVersion = 0x0101u;
constexpr std::size_t kSbdbHeaderSize = 12;
constexpr std::size_t kSbdbMaxPayloadSize = 16u * 1024u * 1024u;
constexpr std::uint32_t kControlPlaneMagic = 0x54434253u;
constexpr std::uint16_t kControlPlaneVersion = 1u;
constexpr std::size_t kControlPlaneHeaderSize = 28;
constexpr std::size_t kControlPlaneMaxPayloadSize = 64u * 1024u;
constexpr std::uint32_t kMessageVectorMagic = 0x564D4253u;
constexpr std::uint16_t kMessageVectorHeaderBytes = 64u;
constexpr std::uint16_t kMessageVectorVersion = 1u;
constexpr std::uint16_t kMessageVectorRecordHeaderBytes = 112u;
constexpr std::size_t kMessageVectorMaxSetBytes = 1024u * 1024u;
constexpr std::size_t kMessageVectorMaxRecordBytes = 256u * 1024u;
constexpr std::uint32_t kLprefaceMagic = 0x504C4253u;
constexpr std::uint16_t kLprefaceVersion1 = 1u;
constexpr std::uint16_t kLprefaceVersion = 2u;
constexpr std::size_t kLprefaceMaxDbbtSize = 8192u;
constexpr std::size_t kLprefaceMaxTextSize = 1024u;
constexpr std::string_view kLprefaceHandoffClaimPrefix = "SB-LPREFACE-CLAIM/1";
constexpr std::size_t kLprefaceClaimMinNonceBytes = 16u;
constexpr std::size_t kLprefaceClaimMaxNonceBytes = 32u;

constexpr std::array<std::uint32_t, 64> kSha256K{
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

std::uint32_t RotR(std::uint32_t value, std::uint32_t bits) {
  return (value >> bits) | (value << (32u - bits));
}

void PutU16(Bytes* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(Bytes* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
}

void PutU64(Bytes* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
}

void PutBytesAtU16(Bytes* out, std::size_t off, std::uint16_t value) {
  (*out)[off] = static_cast<std::uint8_t>(value & 0xffu);
  (*out)[off + 1] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
}

void PutBytesAtU32(Bytes* out, std::size_t off, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) (*out)[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu);
}

void PutBytesAtU64(Bytes* out, std::size_t off, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) (*out)[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu);
}

std::uint16_t ReadU16(const Bytes& data, std::size_t off) {
  return static_cast<std::uint16_t>(data[off]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[off + 1]) << 8u);
}

std::uint32_t ReadU32(const Bytes& data, std::size_t off) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data[off + i]) << (8 * i);
  return v;
}

std::uint64_t ReadU64(const Bytes& data, std::size_t off) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(data[off + i]) << (8 * i);
  return v;
}

std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1u) ^ (0x82f63b78u & static_cast<std::uint32_t>(0u - (crc & 1u)));
    }
  }
  return ~crc;
}

std::uint32_t Crc32c(const Bytes& data) {
  return Crc32c(data.data(), data.size());
}

void Pad4(Bytes* out) {
  while ((out->size() % 4u) != 0) out->push_back(0);
}

std::uint8_t MessageClassForSeverity(const std::string& severity) {
  if (severity == "warning") return 1;
  if (severity == "info" || severity == "notice") return 2;
  return 0;
}

std::uint8_t SeverityCode(const std::string& severity) {
  if (severity == "info" || severity == "notice") return 0;
  if (severity == "warning") return 1;
  if (severity == "security") return 3;
  if (severity == "corruption") return 4;
  if (severity == "panic") return 5;
  if (severity == "fatal_transport") return 6;
  return 2;
}

std::string SeverityText(std::uint8_t severity) {
  switch (severity) {
    case 0: return "info";
    case 1: return "warning";
    case 3: return "security";
    case 4: return "corruption";
    case 5: return "panic";
    case 6: return "fatal_transport";
    default: return "error";
  }
}

bool AppendTlv(Bytes* out, const std::string& key, const std::string& value) {
  if (key.empty() || key.size() > 65535u || value.size() > 0xffffffffull) return false;
  PutU16(out, static_cast<std::uint16_t>(key.size()));
  PutU16(out, 1);
  PutU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), key.begin(), key.end());
  out->insert(out->end(), value.begin(), value.end());
  Pad4(out);
  return true;
}

void AppendPaddedString(Bytes* out, const std::string& value) {
  out->insert(out->end(), value.begin(), value.end());
  Pad4(out);
}

std::string Trim(std::string value) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

bool ParseU64(std::string_view text, std::uint64_t* out) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') return false;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) return false;
    value = value * 10u + digit;
  }
  *out = value;
  return true;
}

bool ValidKeyId(std::string_view key_id) {
  if (key_id.empty() || key_id.size() > 64) return false;
  for (char ch : key_id) {
    const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
    if (!ok) return false;
  }
  return true;
}

Bytes DecodeKeyHex(std::string value) {
  value = Trim(std::move(value));
  if (value.rfind("hex:", 0) == 0) value.erase(0, 4);
  std::string compact;
  compact.reserve(value.size());
  for (char ch : value) {
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == ':') continue;
    compact.push_back(ch);
  }
  return FromHex(compact);
}

Diagnostic KeyringInvalid(std::string message) {
  return MakeDiagnostic("MCP.DBBT_KEYRING_INVALID", std::move(message));
}

}  // namespace

Diagnostic MakeDiagnostic(std::string code,
                          std::string message,
                          std::vector<Field> fields,
                          std::string severity) {
  std::string key = code;
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
    if (c == '.') return '.';
    if (c == '_') return '_';
    return static_cast<char>(std::tolower(c));
  });
  return Diagnostic{std::move(code), std::move(key), std::move(severity), std::move(message), std::move(fields)};
}

std::string JsonEscape(std::string_view value) {
  std::ostringstream out;
  for (char ch : value) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch; break;
    }
  }
  return out.str();
}

std::string ToMessageVectorJsonLine(const Diagnostic& diagnostic) {
  std::ostringstream out;
  out << "{\"message_vector\":{\"code\":\"" << JsonEscape(diagnostic.code)
      << "\",\"message_key\":\"" << JsonEscape(diagnostic.message_key)
      << "\",\"severity\":\"" << JsonEscape(diagnostic.severity)
      << "\",\"safe_message\":\"" << JsonEscape(diagnostic.safe_message) << "\",\"fields\":{";
  for (std::size_t i = 0; i < diagnostic.fields.size(); ++i) {
    if (i) out << ',';
    out << '"' << JsonEscape(diagnostic.fields[i].key) << "\":\""
        << JsonEscape(diagnostic.fields[i].value) << '"';
  }
  out << "}}}";
  return out.str();
}

std::string ToMessageVectorSetJson(const MessageVectorSet& set) {
  std::ostringstream out;
  out << "{\"message_vector_set\":{\"request_uuid\":\"" << Hex(set.request_uuid)
      << "\",\"diagnostics\":[";
  for (std::size_t i = 0; i < set.diagnostics.size(); ++i) {
    if (i) out << ',';
    out << ToMessageVectorJsonLine(set.diagnostics[i]);
  }
  out << "]}}";
  return out.str();
}

DiagnosticResult EncodeMessageVectorSetV1(const MessageVectorSet& set,
                                          Bytes* encoded_out,
                                          std::uint64_t registry_generation,
                                          std::uint32_t max_render_bytes) {
  if (!encoded_out) return {false, {MakeDiagnostic("MESSAGE_VECTOR.MALFORMED", "MessageVectorSet output is null.")}};
  if (set.diagnostics.size() > 1024u) {
    return {false, {MakeDiagnostic("MESSAGE_VECTOR.MALFORMED", "MessageVectorSet vector count is too large.")}};
  }
  Bytes records;
  for (const auto& diagnostic : set.diagnostics) {
    if (diagnostic.code.size() > 65535u || diagnostic.message_key.size() > 65535u ||
        diagnostic.safe_message.size() > 65535u || diagnostic.fields.size() > 64u) {
      return {false, {MakeDiagnostic("MESSAGE_VECTOR.MALFORMED", "MessageVector record size limit exceeded.")}};
    }
    Bytes record(kMessageVectorRecordHeaderBytes, 0);
    const auto record_start = records.size();
    const auto mv_uuid = MakePseudoUuidV7();
    std::copy(mv_uuid.begin(), mv_uuid.end(), record.begin() + 16);
    std::copy(set.request_uuid.begin(), set.request_uuid.end(), record.begin() + 48);
    record[8] = 1;
    record[10] = MessageClassForSeverity(diagnostic.severity);
    record[11] = SeverityCode(diagnostic.severity);
    PutBytesAtU16(&record, 92, 3);
    PutBytesAtU16(&record, 94, static_cast<std::uint16_t>(diagnostic.code.size()));
    PutBytesAtU16(&record, 96, static_cast<std::uint16_t>(diagnostic.message_key.size()));
    PutBytesAtU16(&record, 98, 0);
    PutBytesAtU16(&record, 100, static_cast<std::uint16_t>(diagnostic.safe_message.size()));
    PutBytesAtU16(&record, 102, static_cast<std::uint16_t>(diagnostic.fields.size()));
    record[108] = 0;
    record[109] = 0;
    records.insert(records.end(), record.begin(), record.end());
    const std::string language = "und";
    AppendPaddedString(&records, language);
    AppendPaddedString(&records, diagnostic.code);
    AppendPaddedString(&records, diagnostic.message_key);
    AppendPaddedString(&records, std::string{});
    AppendPaddedString(&records, diagnostic.safe_message);
    for (const auto& field : diagnostic.fields) {
      if (!AppendTlv(&records, field.key, field.value)) {
        return {false, {MakeDiagnostic("MESSAGE_VECTOR.MALFORMED", "MessageVector parameter is invalid.")}};
      }
    }
    Pad4(&records);
    const auto record_bytes = records.size() - record_start;
    if (record_bytes > kMessageVectorMaxRecordBytes) {
      return {false, {MakeDiagnostic("MESSAGE_VECTOR.MALFORMED", "MessageVector record exceeds maximum size.")}};
    }
    PutBytesAtU32(&records, record_start, static_cast<std::uint32_t>(record_bytes));
    Bytes crc_record(records.begin() + static_cast<std::ptrdiff_t>(record_start),
                     records.begin() + static_cast<std::ptrdiff_t>(record_start + record_bytes));
    PutBytesAtU32(&crc_record, 4, 0);
    PutBytesAtU32(&records, record_start + 4, Crc32c(crc_record));
  }
  const auto total_bytes = kMessageVectorHeaderBytes + records.size();
  if (total_bytes > kMessageVectorMaxSetBytes) {
    return {false, {MakeDiagnostic("MESSAGE_VECTOR.MALFORMED", "MessageVectorSet exceeds maximum size.")}};
  }
  Bytes out(kMessageVectorHeaderBytes, 0);
  PutBytesAtU32(&out, 0, kMessageVectorMagic);
  PutBytesAtU16(&out, 4, kMessageVectorHeaderBytes);
  PutBytesAtU16(&out, 6, kMessageVectorVersion);
  PutBytesAtU32(&out, 12, static_cast<std::uint32_t>(set.diagnostics.size()));
  PutBytesAtU32(&out, 16, static_cast<std::uint32_t>(total_bytes));
  PutBytesAtU32(&out, 20, records.empty() ? 0 : Crc32c(records));
  PutBytesAtU64(&out, 24, registry_generation);
  const auto message_set_uuid = MakePseudoUuidV7();
  std::copy(message_set_uuid.begin(), message_set_uuid.end(), out.begin() + 32);
  PutBytesAtU32(&out, 48, max_render_bytes);
  out.insert(out.end(), records.begin(), records.end());
  Bytes header_crc(out.begin(), out.begin() + kMessageVectorHeaderBytes);
  PutBytesAtU32(&header_crc, 52, 0);
  PutBytesAtU32(&out, 52, Crc32c(header_crc));
  *encoded_out = std::move(out);
  return {};
}

std::optional<MessageVectorSet> DecodeMessageVectorSetV1(const Bytes& encoded,
                                                         std::vector<Diagnostic>* diagnostics) {
  auto fail = [&](std::string message) -> std::optional<MessageVectorSet> {
    if (diagnostics) diagnostics->push_back(MakeDiagnostic("MESSAGE_VECTOR.MALFORMED", std::move(message)));
    return std::nullopt;
  };
  if (encoded.size() < kMessageVectorHeaderBytes) return fail("MessageVectorSet header is truncated.");
  if (ReadU32(encoded, 0) != kMessageVectorMagic) return fail("MessageVectorSet magic is invalid.");
  if (ReadU16(encoded, 4) != kMessageVectorHeaderBytes) return fail("MessageVectorSet header length is invalid.");
  if (ReadU16(encoded, 6) != kMessageVectorVersion) return fail("MessageVectorSet version is unsupported.");
  if (ReadU32(encoded, 8) & 0xfffffff0u) return fail("MessageVectorSet flags contain reserved bits.");
  const auto vector_count = ReadU32(encoded, 12);
  const auto total_bytes = ReadU32(encoded, 16);
  if (total_bytes != encoded.size()) return fail("MessageVectorSet total length is invalid.");
  Bytes header_crc(encoded.begin(), encoded.begin() + kMessageVectorHeaderBytes);
  const auto expected_header_crc = ReadU32(header_crc, 52);
  PutBytesAtU32(&header_crc, 52, 0);
  if (Crc32c(header_crc) != expected_header_crc) return fail("MessageVectorSet header CRC is invalid.");
  const auto records_crc = ReadU32(encoded, 20);
  if (vector_count == 0 && records_crc != 0) return fail("MessageVectorSet empty records CRC is invalid.");
  if (vector_count != 0) {
    Bytes records(encoded.begin() + kMessageVectorHeaderBytes, encoded.end());
    if (Crc32c(records) != records_crc) return fail("MessageVectorSet records CRC is invalid.");
  }
  MessageVectorSet set;
  std::size_t off = kMessageVectorHeaderBytes;
  for (std::uint32_t i = 0; i < vector_count; ++i) {
    if (off + kMessageVectorRecordHeaderBytes > encoded.size()) return fail("MessageVector record is truncated.");
    const auto record_start = off;
    const auto record_bytes = ReadU32(encoded, off);
    if (record_bytes < kMessageVectorRecordHeaderBytes || record_bytes > kMessageVectorMaxRecordBytes ||
        off + record_bytes > encoded.size()) {
      return fail("MessageVector record length is invalid.");
    }
    Bytes record_crc(encoded.begin() + static_cast<std::ptrdiff_t>(off),
                     encoded.begin() + static_cast<std::ptrdiff_t>(off + record_bytes));
    const auto expected_record_crc = ReadU32(record_crc, 4);
    PutBytesAtU32(&record_crc, 4, 0);
    if (Crc32c(record_crc) != expected_record_crc) return fail("MessageVector record CRC is invalid.");
    if (ReadU16(encoded, off + 8) != 1) return fail("MessageVector record version is invalid.");
    if (encoded[off + 10] > 7) return fail("MessageVector record message class is invalid.");
    if (ReadU32(encoded, off + 12) != 0) return fail("MessageVector record flags are invalid.");
    const auto severity = encoded[off + 11];
    const auto language_len = ReadU16(encoded, off + 92);
    const auto diagnostic_code_len = ReadU16(encoded, off + 94);
    const auto message_key_len = ReadU16(encoded, off + 96);
    const auto admin_detail_len = ReadU16(encoded, off + 98);
    const auto rendered_len = ReadU16(encoded, off + 100);
    const auto parameter_count = ReadU16(encoded, off + 102);
    const auto detail_count = ReadU16(encoded, off + 104);
    const auto cause_count = ReadU16(encoded, off + 106);
    if (ReadU16(encoded, off + 110) != 0 || detail_count != 0 || cause_count != 0) {
      return fail("MessageVector record unsupported reserved/detail/cause fields are nonzero.");
    }
    off += kMessageVectorRecordHeaderBytes;
    auto read_padded_string = [&](std::uint16_t len, std::string* out) -> bool {
      if (off + len > record_start + record_bytes) return false;
      out->assign(encoded.begin() + static_cast<std::ptrdiff_t>(off),
                  encoded.begin() + static_cast<std::ptrdiff_t>(off + len));
      off += len;
      while ((off % 4u) != 0 && off < record_start + record_bytes) {
        if (encoded[off++] != 0) return false;
      }
      return true;
    };
    std::string language;
    std::string code;
    std::string message_key;
    std::string admin_detail;
    std::string rendered;
    if (!read_padded_string(language_len, &language) ||
        !read_padded_string(diagnostic_code_len, &code) ||
        !read_padded_string(message_key_len, &message_key) ||
        !read_padded_string(admin_detail_len, &admin_detail) ||
        !read_padded_string(rendered_len, &rendered)) {
      return fail("MessageVector record strings are truncated.");
    }
    if (i == 0) std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(record_start + 48), 16, set.request_uuid.begin());
    Diagnostic diagnostic = MakeDiagnostic(code, rendered, {}, SeverityText(severity));
    diagnostic.message_key = message_key;
    for (std::uint16_t p = 0; p < parameter_count; ++p) {
      if (off + 8 > record_start + record_bytes) return fail("MessageVector parameter is truncated.");
      const auto key_len = ReadU16(encoded, off); off += 2;
      const auto type_code = ReadU16(encoded, off); off += 2;
      const auto value_len = ReadU32(encoded, off); off += 4;
      if (key_len == 0 || type_code != 1 || off + key_len + value_len > record_start + record_bytes) {
        return fail("MessageVector parameter is invalid.");
      }
      std::string key(encoded.begin() + static_cast<std::ptrdiff_t>(off),
                      encoded.begin() + static_cast<std::ptrdiff_t>(off + key_len));
      off += key_len;
      std::string value(encoded.begin() + static_cast<std::ptrdiff_t>(off),
                        encoded.begin() + static_cast<std::ptrdiff_t>(off + value_len));
      off += value_len;
      while ((off % 4u) != 0 && off < record_start + record_bytes) {
        if (encoded[off++] != 0) return fail("MessageVector parameter padding is invalid.");
      }
      diagnostic.fields.push_back(Field{std::move(key), std::move(value)});
    }
    while (off < record_start + record_bytes) {
      if (encoded[off++] != 0) return fail("MessageVector record padding is invalid.");
    }
    set.diagnostics.push_back(std::move(diagnostic));
  }
  if (off != encoded.size()) return fail("MessageVectorSet trailing bytes are invalid.");
  return set;
}

UuidBytes MakePseudoUuidV7() {
  const auto now_ms = static_cast<std::uint64_t>(CurrentEpochMilliseconds());
  std::array<std::uint8_t, 10> random_bytes{};
  if (RAND_bytes(random_bytes.data(), static_cast<int>(random_bytes.size())) != 1) {
    throw std::runtime_error("OpenSSL RAND_bytes failed while creating UUIDv7");
  }

  UuidBytes uuid{};
  uuid[0] = static_cast<std::uint8_t>((now_ms >> 40u) & 0xffu);
  uuid[1] = static_cast<std::uint8_t>((now_ms >> 32u) & 0xffu);
  uuid[2] = static_cast<std::uint8_t>((now_ms >> 24u) & 0xffu);
  uuid[3] = static_cast<std::uint8_t>((now_ms >> 16u) & 0xffu);
  uuid[4] = static_cast<std::uint8_t>((now_ms >> 8u) & 0xffu);
  uuid[5] = static_cast<std::uint8_t>(now_ms & 0xffu);
  uuid[6] = static_cast<std::uint8_t>(0x70u | (random_bytes[0] & 0x0fu));
  uuid[7] = random_bytes[1];
  uuid[8] = static_cast<std::uint8_t>(0x80u | (random_bytes[2] & 0x3fu));
  for (std::size_t i = 9; i < uuid.size(); ++i) {
    uuid[i] = random_bytes[i - 6];
  }
  return uuid;
}

std::string Hex(const std::uint8_t* data, std::size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) out << std::setw(2) << static_cast<unsigned>(data[i]);
  return out.str();
}

std::string Hex(const Bytes& data) { return Hex(data.data(), data.size()); }
std::string Hex(const UuidBytes& data) { return Hex(data.data(), data.size()); }

Bytes FromHex(std::string_view hex) {
  Bytes out;
  if (hex.size() % 2 != 0) return out;
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = val(hex[i]);
    const int lo = val(hex[i + 1]);
    if (hi < 0 || lo < 0) return {};
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return out;
}

DbbtReplayCache::DbbtReplayCache(std::size_t max_entries)
    : max_entries_(std::max<std::size_t>(1, max_entries)) {}

void DbbtReplayCache::PruneExpired(std::uint64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [now_ms](const Entry& entry) {
                   return entry.expires_at_ms < now_ms;
                 }),
                 entries_.end());
}

bool DbbtReplayCache::CheckAndInsert(const Sha256Digest& token_id,
                                     std::uint64_t expires_at_ms,
                                     std::uint64_t now_ms) {
  if (expires_at_ms < now_ms) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [now_ms](const Entry& entry) {
                   return entry.expires_at_ms < now_ms;
                 }),
                 entries_.end());
  for (const auto& entry : entries_) {
    if (ConstantTimeEqual(entry.token_id.data(), token_id.data(), token_id.size())) return false;
  }
  while (entries_.size() >= max_entries_) {
    const auto evict = std::min_element(entries_.begin(), entries_.end(), [](const Entry& lhs, const Entry& rhs) {
      return lhs.expires_at_ms < rhs.expires_at_ms;
    });
    if (evict == entries_.end()) break;
    entries_.erase(evict);
  }
  entries_.push_back(Entry{token_id, expires_at_ms});
  return true;
}

std::size_t DbbtReplayCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

Bytes EncodeSbdbFrame(const SbdbFrame& frame) {
  Bytes out;
  PutU32(&out, kSbdbMagic);
  PutU16(&out, kSbdbVersion);
  out.push_back(frame.type);
  out.push_back(frame.flags);
  if (frame.payload.size() > 0xffffffffull) return {};
  PutU32(&out, static_cast<std::uint32_t>(frame.payload.size()));
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  return out;
}

std::optional<SbdbFrame> DecodeSbdbFrame(const Bytes& encoded, std::vector<Diagnostic>* diagnostics) {
  auto fail = [&](std::string code, std::string message) -> std::optional<SbdbFrame> {
    if (diagnostics) diagnostics->push_back(MakeDiagnostic(std::move(code), std::move(message)));
    return std::nullopt;
  };
  if (encoded.size() < kSbdbHeaderSize) {
    return fail("SBDB.FRAME_TRUNCATED", "SBDB frame header is truncated.");
  }
  const auto magic = ReadU32(encoded, 0);
  if (magic != kSbdbMagic) {
    return fail("SBDB.MAGIC_INVALID", "SBDB frame magic is invalid.");
  }
  const auto version = ReadU16(encoded, 4);
  if (version != kSbdbVersion) {
    return fail("SBDB.VERSION_UNSUPPORTED", "SBDB frame version is unsupported.");
  }
  SbdbFrame frame;
  frame.type = encoded[6];
  frame.flags = encoded[7];
  const auto payload_size = ReadU32(encoded, 8);
  if (payload_size > kSbdbMaxPayloadSize) {
    return fail("SBDB.PAYLOAD_TOO_LARGE", "SBDB frame payload is too large.");
  }
  if (encoded.size() != kSbdbHeaderSize + static_cast<std::size_t>(payload_size)) {
    return fail("SBDB.FRAME_LENGTH_INVALID", "SBDB frame length does not match payload length.");
  }
  frame.payload.assign(encoded.begin() + static_cast<std::ptrdiff_t>(kSbdbHeaderSize), encoded.end());
  return frame;
}

Bytes EncodeControlPlaneMessage(const ControlPlaneMessage& message) {
  Bytes out;
  if (message.payload.size() > kControlPlaneMaxPayloadSize) return {};
  PutU32(&out, kControlPlaneMagic);
  PutU16(&out, kControlPlaneVersion);
  PutU16(&out, message.message_type);
  PutU16(&out, message.flags);
  PutU16(&out, 0);
  PutU64(&out, message.request_id);
  PutU64(&out, static_cast<std::uint64_t>(message.payload.size()));
  out.insert(out.end(), message.payload.begin(), message.payload.end());
  return out;
}

std::optional<ControlPlaneMessage> DecodeControlPlaneMessage(const Bytes& encoded, std::vector<Diagnostic>* diagnostics) {
  auto fail = [&](std::string code, std::string message) -> std::optional<ControlPlaneMessage> {
    if (diagnostics) diagnostics->push_back(MakeDiagnostic(std::move(code), std::move(message)));
    return std::nullopt;
  };
  if (encoded.size() < kControlPlaneHeaderSize) {
    return fail("CONTROL.FRAME_TRUNCATED", "Control-plane frame header is truncated.");
  }
  const auto magic = ReadU32(encoded, 0);
  if (magic != kControlPlaneMagic) return fail("CONTROL.MAGIC_INVALID", "Control-plane frame magic is invalid.");
  const auto version = ReadU16(encoded, 4);
  if (version != kControlPlaneVersion) {
    return fail("CONTROL.VERSION_UNSUPPORTED", "Control-plane frame version is unsupported.");
  }
  ControlPlaneMessage message;
  message.message_type = ReadU16(encoded, 6);
  message.flags = ReadU16(encoded, 8);
  const auto reserved = ReadU16(encoded, 10);
  if (reserved != 0) return fail("CONTROL.RESERVED_INVALID", "Control-plane frame reserved field is invalid.");
  message.request_id = ReadU64(encoded, 12);
  const auto payload_len = ReadU64(encoded, 20);
  if (payload_len > kControlPlaneMaxPayloadSize) {
    return fail("CONTROL.PAYLOAD_TOO_LARGE", "Control-plane frame payload is too large.");
  }
  if (encoded.size() != kControlPlaneHeaderSize + static_cast<std::size_t>(payload_len)) {
    return fail("CONTROL.FRAME_LENGTH_INVALID", "Control-plane frame length does not match payload length.");
  }
  message.payload.assign(encoded.begin() + static_cast<std::ptrdiff_t>(kControlPlaneHeaderSize), encoded.end());
  return message;
}

bool ConstantTimeEqual(const std::uint8_t* lhs, const std::uint8_t* rhs, std::size_t size) {
  std::uint8_t diff = 0;
  for (std::size_t i = 0; i < size; ++i) diff = static_cast<std::uint8_t>(diff | (lhs[i] ^ rhs[i]));
  return diff == 0;
}

Sha256Digest Sha256(const Bytes& bytes) {
  Sha256Digest digest{};
  unsigned int digest_len = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &digest_len, EVP_sha256(), nullptr) != 1 ||
      digest_len != digest.size()) {
    return {};
  }
  return digest;
}

Sha256Digest HmacSha256(const Bytes& key, const Bytes& body) {
  Sha256Digest digest{};
  unsigned int digest_len = 0;
  if (HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()), body.data(), body.size(), digest.data(), &digest_len) == nullptr ||
      digest_len != digest.size()) {
    return {};
  }
  return digest;
}

Bytes EncodeDbbtBody(const DbbtToken& token) {
  Bytes out;
  out.push_back(token.version);
  out.insert(out.end(), token.db_uuid.begin(), token.db_uuid.end());
  PutU32(&out, token.listener_id);
  PutU64(&out, token.issued_at_ms);
  PutU64(&out, token.expires_at_ms);
  out.insert(out.end(), token.manager_session_id.begin(), token.manager_session_id.end());
  PutU16(&out, static_cast<std::uint16_t>(token.client_nonce.size()));
  out.insert(out.end(), token.client_nonce.begin(), token.client_nonce.end());
  PutU16(&out, static_cast<std::uint16_t>(token.server_nonce.size()));
  out.insert(out.end(), token.server_nonce.begin(), token.server_nonce.end());
  PutU32(&out, token.flags);
  return out;
}

Bytes EncodeDbbt(const DbbtToken& token, const Bytes& key) {
  Bytes body = EncodeDbbtBody(token);
  const auto mac = HmacSha256(key, body);
  Bytes out = body;
  PutU16(&out, static_cast<std::uint16_t>(mac.size()));
  out.insert(out.end(), mac.begin(), mac.end());
  return out;
}

std::optional<DbbtToken> DecodeDbbt(const Bytes& encoded, std::vector<Diagnostic>* diagnostics) {
  if (encoded.size() < 63 || encoded.size() > 8192) {
    diagnostics->push_back(MakeDiagnostic("MCP.DBBT_LENGTH_INVALID", "DBBT length is invalid."));
    return std::nullopt;
  }
  DbbtToken token;
  std::size_t off = 0;
  token.version = encoded[off++];
  if (off + 16 + 4 + 8 + 8 + 16 + 2 > encoded.size()) return std::nullopt;
  std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(off), 16, token.db_uuid.begin()); off += 16;
  token.listener_id = ReadU32(encoded, off); off += 4;
  token.issued_at_ms = ReadU64(encoded, off); off += 8;
  token.expires_at_ms = ReadU64(encoded, off); off += 8;
  std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(off), 16, token.manager_session_id.begin()); off += 16;
  const auto cn_len = ReadU16(encoded, off); off += 2;
  if (cn_len > 1024 || off + cn_len > encoded.size()) {
    diagnostics->push_back(MakeDiagnostic("MCP.DBBT_NONCE_TOO_LARGE", "DBBT nonce is too large."));
    return std::nullopt;
  }
  token.client_nonce.assign(encoded.begin() + static_cast<std::ptrdiff_t>(off), encoded.begin() + static_cast<std::ptrdiff_t>(off + cn_len)); off += cn_len;
  if (off + 2 > encoded.size()) return std::nullopt;
  const auto sn_len = ReadU16(encoded, off); off += 2;
  if (sn_len > 1024 || off + sn_len > encoded.size()) {
    diagnostics->push_back(MakeDiagnostic("MCP.DBBT_NONCE_TOO_LARGE", "DBBT nonce is too large."));
    return std::nullopt;
  }
  token.server_nonce.assign(encoded.begin() + static_cast<std::ptrdiff_t>(off), encoded.begin() + static_cast<std::ptrdiff_t>(off + sn_len)); off += sn_len;
  if (off + 4 + 2 > encoded.size()) return std::nullopt;
  token.flags = ReadU32(encoded, off); off += 4;
  const auto mac_len = ReadU16(encoded, off); off += 2;
  if (mac_len != 32 || off + mac_len != encoded.size()) {
    diagnostics->push_back(MakeDiagnostic("MCP.DBBT_MAC_LENGTH_INVALID", "DBBT MAC length is invalid."));
    return std::nullopt;
  }
  std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(off), 32, token.mac.begin());
  return token;
}

DiagnosticResult LoadDbbtKeyring(const std::filesystem::path& path, DbbtKeyring* keyring_out) {
  if (!keyring_out) return {false, {KeyringInvalid("DBBT keyring output is null.")}};
  std::ifstream in(path);
  if (!in) return {false, {KeyringInvalid("DBBT keyring file could not be opened.")}};

  struct LineEntry {
    std::string key;
    std::string value;
    bool inline_active = false;
  };
  std::vector<LineEntry> entries;
  std::string line;
  while (std::getline(in, line)) {
    const auto comment = line.find_first_of("#;");
    if (comment != std::string::npos) line.resize(comment);
    line = Trim(std::move(line));
    if (line.empty()) continue;
    bool inline_active = false;
    if (line[0] == '*') {
      inline_active = true;
      line.erase(line.begin());
      line = Trim(std::move(line));
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= line.size()) {
      return {false, {KeyringInvalid("DBBT keyring line is invalid.")}};
    }
    entries.push_back(LineEntry{Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)), inline_active});
  }
  if (entries.empty()) return {false, {KeyringInvalid("DBBT keyring is empty.")}};

  const bool v1_format = std::any_of(entries.begin(), entries.end(), [](const LineEntry& entry) {
    return entry.key == "format";
  });

  DbbtKeyring keyring;
  if (v1_format) {
    bool seen_format = false;
    bool seen_active_key_id = false;
    bool seen_active_key_hex = false;
    bool seen_not_before = false;
    bool seen_not_after = false;
    for (const auto& entry : entries) {
      if (entry.inline_active) return {false, {KeyringInvalid("DBBT keyring v1 does not allow inline active markers.")}};
      if (entry.key == "format") {
        if (seen_format || entry.value != "SBMN_DBBT_KEYRING_V1") return {false, {KeyringInvalid("DBBT keyring format is invalid.")}};
        seen_format = true;
      } else if (entry.key == "active_key_id") {
        if (seen_active_key_id || !ValidKeyId(entry.value)) return {false, {KeyringInvalid("DBBT keyring active key id is invalid.")}};
        seen_active_key_id = true;
        keyring.active_key_id = entry.value;
      } else if (entry.key == "active_key_hex") {
        if (seen_active_key_hex) return {false, {KeyringInvalid("DBBT keyring active key is duplicated.")}};
        auto key = DecodeKeyHex(entry.value);
        if (key.size() < 32 || key.size() > 128) return {false, {KeyringInvalid("DBBT keyring active key length is invalid.")}};
        seen_active_key_hex = true;
        keyring.active_key = std::move(key);
      } else if (entry.key == "previous_key_hex") {
        auto key = DecodeKeyHex(entry.value);
        if (key.size() < 32 || key.size() > 128) return {false, {KeyringInvalid("DBBT keyring previous key length is invalid.")}};
        keyring.verification_keys.push_back(DbbtKeyringKey{"previous." + std::to_string(keyring.verification_keys.size() + 1), std::move(key), false});
      } else if (entry.key == "not_before_ms") {
        if (seen_not_before || !ParseU64(entry.value, &keyring.not_before_ms)) return {false, {KeyringInvalid("DBBT keyring not_before_ms is invalid.")}};
        seen_not_before = true;
      } else if (entry.key == "not_after_ms") {
        if (seen_not_after || !ParseU64(entry.value, &keyring.not_after_ms)) return {false, {KeyringInvalid("DBBT keyring not_after_ms is invalid.")}};
        seen_not_after = true;
      } else {
        return {false, {KeyringInvalid("DBBT keyring contains an unknown key.")}};
      }
    }
    if (!seen_format || !seen_active_key_id || !seen_active_key_hex) {
      return {false, {KeyringInvalid("DBBT keyring is missing required fields.")}};
    }
    if (keyring.not_after_ms != 0 && keyring.not_before_ms != 0 && keyring.not_after_ms < keyring.not_before_ms) {
      return {false, {KeyringInvalid("DBBT keyring time window is invalid.")}};
    }
    keyring.verification_keys.insert(keyring.verification_keys.begin(),
                                     DbbtKeyringKey{keyring.active_key_id, keyring.active_key, true});
  } else {
    std::string explicit_active;
    for (const auto& entry : entries) {
      if (entry.key == "active") {
        if (entry.value.empty()) return {false, {KeyringInvalid("DBBT keyring active key id is invalid.")}};
        explicit_active = entry.value;
        continue;
      }
      auto key = DecodeKeyHex(entry.value);
      if (key.size() < 16 || key.size() > 128) return {false, {KeyringInvalid("DBBT keyring key length is invalid.")}};
      if (keyring.active_key_id.empty() || entry.inline_active) {
        keyring.active_key_id = entry.key;
        keyring.active_key = key;
      }
      keyring.verification_keys.push_back(DbbtKeyringKey{entry.key, std::move(key), false});
    }
    if (!explicit_active.empty()) {
      const auto match = std::find_if(keyring.verification_keys.begin(), keyring.verification_keys.end(), [&](const DbbtKeyringKey& entry) {
        return entry.key_id == explicit_active;
      });
      if (match == keyring.verification_keys.end()) return {false, {KeyringInvalid("DBBT keyring active key was not found.")}};
      keyring.active_key_id = match->key_id;
      keyring.active_key = match->key;
    }
    if (keyring.active_key_id.empty() || keyring.active_key.empty()) return {false, {KeyringInvalid("DBBT keyring has no active key.")}};
    for (auto& entry : keyring.verification_keys) entry.active = (entry.key_id == keyring.active_key_id);
  }

  *keyring_out = std::move(keyring);
  return {};
}

Sha256Digest DbbtTokenId(const DbbtToken& token, const Bytes& key) {
  return HmacSha256(key, EncodeDbbtBody(token));
}

DiagnosticResult ValidateDbbtWithKeyring(const Bytes& encoded,
                                         const DbbtKeyring& keyring,
                                         const DbbtValidationOptions& options,
                                         DbbtReplayCache* replay_cache,
                                         DbbtToken* token_out,
                                         std::string* matched_key_id_out) {
  std::vector<Diagnostic> diagnostics;
  auto token = DecodeDbbt(encoded, &diagnostics);
  if (!token) return {false, diagnostics};
  if (token->version != 1) return {false, {MakeDiagnostic("MCP.DBBT_VERSION_INVALID", "DBBT version is invalid.")}};
  if (token->flags != 0) return {false, {MakeDiagnostic("MCP.DBBT_FLAGS_RESERVED", "DBBT reserved flags are set.")}};
  if (options.expected_listener_id != 0 && token->listener_id != options.expected_listener_id) {
    return {false, {MakeDiagnostic("MCP.DBBT_LISTENER_MISMATCH", "DBBT listener id does not match.")}};
  }
  if (keyring.verification_keys.empty()) {
    return {false, {MakeDiagnostic("MCP.DBBT_KEYRING_INVALID", "DBBT keyring has no verification keys.")}};
  }

  Bytes body(encoded.begin(), encoded.end() - 34);
  const DbbtKeyringKey* matched_key = nullptr;
  for (const auto& key : keyring.verification_keys) {
    const auto mac = HmacSha256(key.key, body);
    if (ConstantTimeEqual(mac.data(), token->mac.data(), mac.size())) {
      matched_key = &key;
      break;
    }
  }
  if (!matched_key) return {false, {MakeDiagnostic("MCP.DBBT_VERIFY_FAILED", "DBBT keyring verification failed.")}};

  const auto now = options.now_ms == 0 ? CurrentEpochMilliseconds() : options.now_ms;
  const auto skew = options.clock_skew_ms;
  if (keyring.not_before_ms != 0 && now + skew < keyring.not_before_ms) {
    return {false, {MakeDiagnostic("MCP.DBBT_KEYRING_NOT_YET_VALID", "DBBT keyring is not yet valid.")}};
  }
  if (keyring.not_after_ms != 0 && keyring.not_after_ms + skew < now) {
    return {false, {MakeDiagnostic("MCP.DBBT_KEYRING_EXPIRED", "DBBT keyring is expired.")}};
  }
  if (token->expires_at_ms + skew < now) return {false, {MakeDiagnostic("MCP.DBBT_EXPIRED", "DBBT is expired.")}};
  if (token->issued_at_ms > now + skew) return {false, {MakeDiagnostic("MCP.DBBT_NOT_YET_VALID", "DBBT is not yet valid.")}};
  if (replay_cache) {
    const auto token_id = DbbtTokenId(*token, matched_key->key);
    if (!replay_cache->CheckAndInsert(token_id, token->expires_at_ms, now)) {
      return {false, {MakeDiagnostic("MCP.DBBT_REPLAY_DETECTED", "DBBT replay was detected.")}};
    }
  }
  if (matched_key_id_out) *matched_key_id_out = matched_key->key_id;
  if (token_out) *token_out = *token;
  return {};
}

DiagnosticResult EncodeLpreface(const Lpreface& preface, Bytes* encoded_out) {
  if (!encoded_out) return {false, {MakeDiagnostic("LPREFACE.OUTPUT_NULL", "LPREFACE output is null.")}};
  if (preface.listener_id == 0) {
    return {false, {MakeDiagnostic("LPREFACE.LISTENER_ID_REQUIRED", "LPREFACE listener id is required.")}};
  }
  if (preface.dbbt.empty() || preface.dbbt.size() > kLprefaceMaxDbbtSize) {
    return {false, {MakeDiagnostic("LPREFACE.DBBT_LENGTH_INVALID", "LPREFACE DBBT length is invalid.")}};
  }
  if (preface.db_selector.size() > kLprefaceMaxTextSize) {
    return {false, {MakeDiagnostic("LPREFACE.SELECTOR_TOO_LARGE", "LPREFACE database selector is too large.")}};
  }
  if (preface.requested_profile.size() > kLprefaceMaxTextSize) {
    return {false, {MakeDiagnostic("LPREFACE.PROFILE_TOO_LARGE", "LPREFACE requested profile is too large.")}};
  }
  if (preface.auth_provider_family.size() > kLprefaceMaxTextSize) {
    return {false, {MakeDiagnostic("LPREFACE.AUTH_PROVIDER_TOO_LARGE", "LPREFACE auth provider is too large.")}};
  }
  if (preface.auth_principal.size() > kLprefaceMaxTextSize) {
    return {false, {MakeDiagnostic("LPREFACE.AUTH_PRINCIPAL_TOO_LARGE", "LPREFACE auth principal is too large.")}};
  }
  if (preface.auth_token.size() > kLprefaceMaxTextSize) {
    return {false, {MakeDiagnostic("LPREFACE.AUTH_TOKEN_TOO_LARGE", "LPREFACE auth token is too large.")}};
  }
  if (preface.flags != 0) {
    return {false, {MakeDiagnostic("LPREFACE.FLAGS_RESERVED", "LPREFACE flags are reserved.")}};
  }
  Bytes out;
  PutU32(&out, kLprefaceMagic);
  PutU16(&out, kLprefaceVersion);
  PutU16(&out, preface.reserved);
  PutU32(&out, preface.listener_id);
  PutU32(&out, static_cast<std::uint32_t>(preface.dbbt.size()));
  out.insert(out.end(), preface.dbbt.begin(), preface.dbbt.end());
  PutU16(&out, static_cast<std::uint16_t>(preface.db_selector.size()));
  out.insert(out.end(), preface.db_selector.begin(), preface.db_selector.end());
  PutU16(&out, static_cast<std::uint16_t>(preface.requested_profile.size()));
  out.insert(out.end(), preface.requested_profile.begin(), preface.requested_profile.end());
  PutU16(&out, static_cast<std::uint16_t>(preface.auth_provider_family.size()));
  out.insert(out.end(), preface.auth_provider_family.begin(), preface.auth_provider_family.end());
  PutU16(&out, static_cast<std::uint16_t>(preface.auth_principal.size()));
  out.insert(out.end(), preface.auth_principal.begin(), preface.auth_principal.end());
  PutU16(&out, static_cast<std::uint16_t>(preface.auth_token.size()));
  out.insert(out.end(), preface.auth_token.begin(), preface.auth_token.end());
  PutU32(&out, preface.flags);
  *encoded_out = std::move(out);
  return {};
}

std::optional<Lpreface> DecodeLpreface(const Bytes& encoded, std::vector<Diagnostic>* diagnostics) {
  auto fail = [&](std::string code, std::string message) -> std::optional<Lpreface> {
    if (diagnostics) diagnostics->push_back(MakeDiagnostic(std::move(code), std::move(message)));
    return std::nullopt;
  };
  if (encoded.size() < 24) return fail("LPREFACE.TRUNCATED", "LPREFACE frame is truncated.");
  std::size_t off = 0;
  const auto magic = ReadU32(encoded, off); off += 4;
  const auto version = ReadU16(encoded, off); off += 2;
  Lpreface preface;
  preface.reserved = ReadU16(encoded, off); off += 2;
  preface.listener_id = ReadU32(encoded, off); off += 4;
  if (magic != kLprefaceMagic || (version != kLprefaceVersion1 && version != kLprefaceVersion)) {
    return fail("LPREFACE.VERSION_MISMATCH", "LPREFACE magic or version is unsupported.");
  }
  if (preface.listener_id == 0) return fail("LPREFACE.LISTENER_ID_REQUIRED", "LPREFACE listener id is required.");
  const auto dbbt_len = ReadU32(encoded, off); off += 4;
  if (dbbt_len == 0 || dbbt_len > kLprefaceMaxDbbtSize || off + dbbt_len > encoded.size()) {
    return fail("LPREFACE.DBBT_LENGTH_INVALID", "LPREFACE DBBT length is invalid.");
  }
  preface.dbbt.assign(encoded.begin() + static_cast<std::ptrdiff_t>(off),
                      encoded.begin() + static_cast<std::ptrdiff_t>(off + dbbt_len));
  off += dbbt_len;
  if (off + 2 > encoded.size()) return fail("LPREFACE.TRUNCATED", "LPREFACE selector length is truncated.");
  const auto selector_len = ReadU16(encoded, off); off += 2;
  if (selector_len > kLprefaceMaxTextSize || off + selector_len > encoded.size()) {
    return fail("LPREFACE.SELECTOR_INVALID", "LPREFACE selector is invalid.");
  }
  preface.db_selector.assign(encoded.begin() + static_cast<std::ptrdiff_t>(off),
                             encoded.begin() + static_cast<std::ptrdiff_t>(off + selector_len));
  off += selector_len;
  if (off + 2 > encoded.size()) return fail("LPREFACE.TRUNCATED", "LPREFACE profile length is truncated.");
  const auto profile_len = ReadU16(encoded, off); off += 2;
  if (profile_len > kLprefaceMaxTextSize || off + profile_len > encoded.size()) {
    return fail("LPREFACE.PROFILE_INVALID", "LPREFACE requested profile is invalid.");
  }
  preface.requested_profile.assign(encoded.begin() + static_cast<std::ptrdiff_t>(off),
                                   encoded.begin() + static_cast<std::ptrdiff_t>(off + profile_len));
  off += profile_len;
  if (version >= kLprefaceVersion) {
    if (off + 2 > encoded.size()) return fail("LPREFACE.TRUNCATED", "LPREFACE auth provider length is truncated.");
    const auto provider_len = ReadU16(encoded, off); off += 2;
    if (provider_len > kLprefaceMaxTextSize || off + provider_len > encoded.size()) {
      return fail("LPREFACE.AUTH_PROVIDER_INVALID", "LPREFACE auth provider is invalid.");
    }
    preface.auth_provider_family.assign(encoded.begin() + static_cast<std::ptrdiff_t>(off),
                                        encoded.begin() + static_cast<std::ptrdiff_t>(off + provider_len));
    off += provider_len;
    if (off + 2 > encoded.size()) return fail("LPREFACE.TRUNCATED", "LPREFACE auth principal length is truncated.");
    const auto principal_len = ReadU16(encoded, off); off += 2;
    if (principal_len > kLprefaceMaxTextSize || off + principal_len > encoded.size()) {
      return fail("LPREFACE.AUTH_PRINCIPAL_INVALID", "LPREFACE auth principal is invalid.");
    }
    preface.auth_principal.assign(encoded.begin() + static_cast<std::ptrdiff_t>(off),
                                  encoded.begin() + static_cast<std::ptrdiff_t>(off + principal_len));
    off += principal_len;
    if (off + 2 > encoded.size()) return fail("LPREFACE.TRUNCATED", "LPREFACE auth token length is truncated.");
    const auto token_len = ReadU16(encoded, off); off += 2;
    if (token_len > kLprefaceMaxTextSize || off + token_len > encoded.size()) {
      return fail("LPREFACE.AUTH_TOKEN_INVALID", "LPREFACE auth token is invalid.");
    }
    preface.auth_token.assign(encoded.begin() + static_cast<std::ptrdiff_t>(off),
                              encoded.begin() + static_cast<std::ptrdiff_t>(off + token_len));
    off += token_len;
  }
  if (off + 4 != encoded.size()) return fail("LPREFACE.TRUNCATED", "LPREFACE flags or trailing bytes are invalid.");
  preface.flags = ReadU32(encoded, off);
  if (preface.flags != 0) return fail("LPREFACE.FLAGS_RESERVED", "LPREFACE flags are reserved.");
  return preface;
}

DiagnosticResult EncodeLprefaceAck(const LprefaceAck& ack, Bytes* encoded_out) {
  if (!encoded_out) return {false, {MakeDiagnostic("LPREFACE.ACK_OUTPUT_NULL", "LPREFACE ack output is null.")}};
  if (ack.message.size() > kLprefaceMaxTextSize) {
    return {false, {MakeDiagnostic("LPREFACE.ACK_MESSAGE_TOO_LARGE", "LPREFACE ack message is too large.")}};
  }
  if (ack.accepted && ack.nack_code != 0) {
    return {false, {MakeDiagnostic("LPREFACE.ACK_NACK_CODE_INVALID", "LPREFACE accepted ack must use nack_code 0.")}};
  }
  if (!ack.accepted && ack.nack_code == 0) {
    return {false, {MakeDiagnostic("LPREFACE.ACK_NACK_CODE_INVALID", "LPREFACE rejected ack requires a nonzero nack_code.")}};
  }
  Bytes out;
  out.push_back(ack.accepted ? 1 : 0);
  PutU16(&out, ack.nack_code);
  PutU16(&out, static_cast<std::uint16_t>(ack.message.size()));
  out.insert(out.end(), ack.message.begin(), ack.message.end());
  *encoded_out = std::move(out);
  return {};
}

std::optional<LprefaceAck> DecodeLprefaceAck(const Bytes& encoded, std::vector<Diagnostic>* diagnostics) {
  auto fail = [&](std::string code, std::string message) -> std::optional<LprefaceAck> {
    if (diagnostics) diagnostics->push_back(MakeDiagnostic(std::move(code), std::move(message)));
    return std::nullopt;
  };
  if (encoded.size() < 5) return fail("LPREFACE.ACK_TRUNCATED", "LPREFACE ack is truncated.");
  LprefaceAck ack;
  if (encoded[0] > 1) return fail("LPREFACE.ACK_ACCEPTED_INVALID", "LPREFACE ack accepted flag is invalid.");
  ack.accepted = encoded[0] == 1;
  ack.nack_code = ReadU16(encoded, 1);
  const auto message_len = ReadU16(encoded, 3);
  if (message_len > kLprefaceMaxTextSize || 5u + message_len != encoded.size()) {
    return fail("LPREFACE.ACK_MESSAGE_INVALID", "LPREFACE ack message is invalid.");
  }
  if (ack.accepted && ack.nack_code != 0) {
    return fail("LPREFACE.ACK_NACK_CODE_INVALID", "LPREFACE accepted ack must use nack_code 0.");
  }
  if (!ack.accepted && ack.nack_code == 0) {
    return fail("LPREFACE.ACK_NACK_CODE_INVALID", "LPREFACE rejected ack requires a nonzero nack_code.");
  }
  ack.message.assign(encoded.begin() + 5, encoded.end());
  return ack;
}

std::string EncodeLprefaceHandoffClaim(const Bytes& client_nonce, const Bytes& server_nonce) {
  return std::string(kLprefaceHandoffClaimPrefix) +
         " client_nonce=" + Hex(client_nonce) +
         " server_nonce=" + Hex(server_nonce) + "\n";
}

bool IsLprefaceHandoffClaimPrefix(std::string_view text) {
  if (text.empty()) return false;
  const auto prefix_size = std::min(text.size(), kLprefaceHandoffClaimPrefix.size());
  return text.substr(0, prefix_size) == kLprefaceHandoffClaimPrefix.substr(0, prefix_size);
}

std::optional<LprefaceHandoffClaim> DecodeLprefaceHandoffClaim(std::string_view line,
                                                               std::vector<Diagnostic>* diagnostics) {
  auto fail = [&](std::string code, std::string message) -> std::optional<LprefaceHandoffClaim> {
    if (diagnostics) diagnostics->push_back(MakeDiagnostic(std::move(code), std::move(message)));
    return std::nullopt;
  };
  std::string trimmed = Trim(std::string(line));
  if (!trimmed.empty() && trimmed.back() == '\r') trimmed.pop_back();
  std::istringstream in(trimmed);
  std::string prefix;
  in >> prefix;
  if (prefix != kLprefaceHandoffClaimPrefix) {
    return fail("LPREFACE.CLAIM_PREFIX_INVALID", "LPREFACE handoff claim prefix is invalid.");
  }
  std::string token;
  std::string client_nonce_hex;
  std::string server_nonce_hex;
  while (in >> token) {
    const auto eq = token.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= token.size()) {
      return fail("LPREFACE.CLAIM_FIELD_INVALID", "LPREFACE handoff claim field is invalid.");
    }
    const auto key = token.substr(0, eq);
    const auto value = token.substr(eq + 1);
    if (key == "client_nonce" && client_nonce_hex.empty()) {
      client_nonce_hex = value;
    } else if (key == "server_nonce" && server_nonce_hex.empty()) {
      server_nonce_hex = value;
    } else {
      return fail("LPREFACE.CLAIM_FIELD_INVALID", "LPREFACE handoff claim field is invalid.");
    }
  }
  if (client_nonce_hex.empty() || server_nonce_hex.empty() ||
      (client_nonce_hex.size() % 2u) != 0 || (server_nonce_hex.size() % 2u) != 0) {
    return fail("LPREFACE.CLAIM_NONCE_INVALID", "LPREFACE handoff claim nonces are invalid.");
  }
  LprefaceHandoffClaim claim;
  claim.client_nonce = FromHex(client_nonce_hex);
  claim.server_nonce = FromHex(server_nonce_hex);
  if (claim.client_nonce.size() < kLprefaceClaimMinNonceBytes ||
      claim.client_nonce.size() > kLprefaceClaimMaxNonceBytes ||
      claim.server_nonce.size() < kLprefaceClaimMinNonceBytes ||
      claim.server_nonce.size() > kLprefaceClaimMaxNonceBytes) {
    return fail("LPREFACE.CLAIM_NONCE_LENGTH", "LPREFACE handoff claim nonce length is invalid.");
  }
  return claim;
}

DiagnosticResult ValidateDbbt(const Bytes& encoded,
                              const Bytes& key,
                              const DbbtValidationOptions& options,
                              DbbtToken* token_out) {
  DiagnosticResult result;
  std::vector<Diagnostic> diagnostics;
  auto token = DecodeDbbt(encoded, &diagnostics);
  if (!token) return {false, diagnostics};
  if (token->version != 1) return {false, {MakeDiagnostic("MCP.DBBT_VERSION_INVALID", "DBBT version is invalid.")}};
  if (token->flags != 0) return {false, {MakeDiagnostic("MCP.DBBT_FLAGS_RESERVED", "DBBT reserved flags are set.")}};
  if (options.expected_listener_id != 0 && token->listener_id != options.expected_listener_id) {
    return {false, {MakeDiagnostic("MCP.DBBT_LISTENER_MISMATCH", "DBBT listener id does not match.")}};
  }
  Bytes body(encoded.begin(), encoded.end() - 34);
  const auto mac = HmacSha256(key, body);
  if (!ConstantTimeEqual(mac.data(), token->mac.data(), mac.size())) {
    return {false, {MakeDiagnostic("MCP.DBBT_MAC_INVALID", "DBBT MAC verification failed.")}};
  }
  const auto now = options.now_ms == 0 ? CurrentEpochMilliseconds() : options.now_ms;
  const auto skew = options.clock_skew_ms;
  if (token->expires_at_ms + skew < now) return {false, {MakeDiagnostic("MCP.DBBT_EXPIRED", "DBBT is expired.")}};
  if (token->issued_at_ms > now + skew) return {false, {MakeDiagnostic("MCP.DBBT_NOT_YET_VALID", "DBBT is not yet valid.")}};
  if (token_out) *token_out = *token;
  result.ok = true;
  return result;
}

std::uint64_t CurrentEpochMilliseconds() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace scratchbird::manager::protocol
