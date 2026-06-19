// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipc/sbps_client.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace scratchbird::parser::sbsql {
namespace {

constexpr std::uint32_t kFrameMagic = 0x53504253;  // SBPS
constexpr std::uint32_t kMessageVectorMagic = 0x564d4253;  // SBMV
constexpr std::uint16_t kHeaderBytes = 96;
constexpr std::uint16_t kProtocolMajor = 1;
constexpr std::uint16_t kProtocolMinor = 0;
constexpr std::uint32_t kFlagResponse = 1u << 0;
constexpr std::uint32_t kFlagError = 1u << 1;
constexpr std::uint32_t kFlagFinal = 1u << 2;
constexpr std::uint32_t kFlagPayloadChunk = 1u << 3;
constexpr std::uint32_t kSchemaHelloRequestV1 = 1001;
constexpr std::uint32_t kSchemaAuthHandoffV1 = 3001;
constexpr std::uint32_t kSchemaAttachRequestV1 = 3003;
constexpr std::uint32_t kSchemaPrepareSblrV1 = 4001;
constexpr std::uint32_t kSchemaExecuteSblrV1 = 4003;
constexpr std::uint32_t kSchemaFetchV1 = 4005;
constexpr std::uint32_t kSchemaCloseCursorV1 = 4007;
constexpr std::uint32_t kSchemaManagementRequestV1 = 6001;
constexpr std::uint32_t kSchemaManagementResponseV1 = 6002;
constexpr std::uint32_t kSchemaResolveNameRequestV1 = 7001;
constexpr std::uint32_t kSchemaRenderUuidRequestV1 = 7003;
constexpr std::uint16_t kMessageHello = 1;
constexpr std::uint16_t kMessageHelloAccept = 2;
constexpr std::uint16_t kMessageAuthHandoff = 10;
constexpr std::uint16_t kMessageAuthResult = 11;
constexpr std::uint16_t kMessageAttachDatabase = 20;
constexpr std::uint16_t kMessageAttachResult = 21;
constexpr std::uint16_t kMessageManagementRequest = 30;
constexpr std::uint16_t kMessageManagementResult = 31;
constexpr std::uint16_t kMessageResolveNameRequest = 32;
constexpr std::uint16_t kMessageResolveNameResult = 33;
constexpr std::uint16_t kMessageRenderUuidRequest = 34;
constexpr std::uint16_t kMessageRenderUuidResult = 35;
constexpr std::uint16_t kMessagePrepareSblr = 40;
constexpr std::uint16_t kMessagePrepareResult = 41;
constexpr std::uint16_t kMessageExecuteSblr = 42;
constexpr std::uint16_t kMessageExecuteResult = 43;
constexpr std::uint16_t kMessageFetch = 44;
constexpr std::uint16_t kMessageFetchResult = 45;
constexpr std::uint16_t kMessageCloseCursor = 46;
constexpr std::uint16_t kMessageCloseCursorResult = 47;
constexpr std::uint16_t kMessageDiagnostic = 60;
constexpr std::uint16_t kMessageDisconnectNotice = 74;
constexpr std::uint32_t kMaxFramePayload = 1024 * 1024;
constexpr std::uint64_t kMaxChunkedPayload = static_cast<std::uint64_t>(kMaxFramePayload) * 16u;
constexpr std::uint32_t kCursorCloseFlagCancel = 1u << 0;
constexpr std::uint16_t kLongStringSentinel = 0xffff;
constexpr std::uint32_t kDefaultSbpsRequestTimeoutMs = 300000;
constexpr std::size_t kPortableAfUnixPathLimit = 108;

std::string NormalizeLanguageTag(std::string_view value) {
  return value.empty() ? "en" : std::string(value);
}

std::string LanguageProfileForTag(std::string_view value) {
  const std::string tag = NormalizeLanguageTag(value);
  if (tag == "en") return "sbsql.builtin.recovery.en";
  return "sbsql.language-profile." + tag;
}

std::string InputFallbackTagForTag(std::string_view value) {
  const std::string tag = NormalizeLanguageTag(value);
  return tag == "en" ? std::string{} : "en";
}

std::uint64_t SteadyNowMicros() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void AppendSbpsClientPhaseTrace(std::uint16_t message_type,
                                std::uint32_t schema_id,
                                std::size_t request_payload_bytes,
                                std::size_t encoded_frame_bytes,
                                std::size_t response_payload_bytes,
                                bool ok,
                                std::uint64_t connect_us,
                                std::uint64_t encode_us,
                                std::uint64_t write_us,
                                std::uint64_t read_us,
                                std::uint64_t assemble_us,
                                std::uint64_t total_us) {
  const char* path = std::getenv("SCRATCHBIRD_SBPS_CLIENT_PHASE_TRACE_FILE");
  if (path == nullptr || *path == '\0') return;
  std::ofstream out(path, std::ios::app);
  if (!out) return;
  out << "{\"component\":\"sbps_client\","
      << "\"message_type\":" << message_type << ','
      << "\"schema_id\":" << schema_id << ','
      << "\"ok\":" << (ok ? "true" : "false") << ','
      << "\"request_payload_bytes\":" << request_payload_bytes << ','
      << "\"encoded_frame_bytes\":" << encoded_frame_bytes << ','
      << "\"response_payload_bytes\":" << response_payload_bytes << ','
      << "\"phases\":{\"connect_us\":" << connect_us
      << ",\"encode_us\":" << encode_us
      << ",\"write_us\":" << write_us
      << ",\"read_us\":" << read_us
      << ",\"assemble_us\":" << assemble_us
      << "},\"total_us\":" << total_us << "}\n";
}

void ApplySbpsLanguageContext(SessionContext* session,
                              std::string_view requested_language_tag,
                              std::uint64_t language_resource_epoch,
                              std::uint64_t localized_name_epoch) {
  if (session == nullptr) return;
  session->default_language = "en";
  session->language_tag = NormalizeLanguageTag(requested_language_tag);
  session->language_profile = LanguageProfileForTag(session->language_tag);
  session->input_syntax_profile = "sbsql.syntax.standard";
  session->input_language_fallback_tag =
      InputFallbackTagForTag(session->language_tag);
  session->common_resource_hash = "builtin.common.sbsql.v1";
  session->resource_compatibility_identity = "sbsql.resource.compat.v1";
  session->resource_version_identity = "sbsql.resource-pack.v1";
  session->language_resource_epoch = language_resource_epoch;
  session->localized_name_epoch = localized_name_epoch;
  if (session->message_resource_epoch == 0) session->message_resource_epoch = 1;
}

struct FrameHeader {
  std::uint16_t message_type{0};
  std::uint32_t flags{0};
  std::uint32_t schema_id{0};
  std::uint32_t payload_len{0};
  std::uint64_t stream_id{0};
  std::uint64_t sequence_number{1};
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
};

struct Frame {
  FrameHeader header;
  std::vector<std::uint8_t> payload;
};

void PutU8(std::vector<std::uint8_t>* out, std::uint8_t value) {
  out->push_back(value);
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutAtU32(std::vector<std::uint8_t>* out, std::size_t offset, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    (*out)[offset + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xffu);
  }
}

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint32_t GetU32(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint32_t value = 0;
  for (int byte = 3; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

std::uint64_t GetU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int byte = 7; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  if (offset + uuid.size() <= data.size()) {
    std::memcpy(uuid.data(), data.data() + offset, uuid.size());
  }
  return uuid;
}

void PutBytes32(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 32>& bytes) {
  out->insert(out->end(), bytes.begin(), bytes.end());
}

void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  if (value.size() >= kLongStringSentinel) {
    PutU16(out, kLongStringSentinel);
    PutU64(out, static_cast<std::uint64_t>(value.size()));
  } else {
    PutU16(out, static_cast<std::uint16_t>(value.size()));
  }
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadString(const std::vector<std::uint8_t>& data, std::size_t* offset, std::string* out) {
  if (*offset + 2 > data.size()) return false;
  auto length = static_cast<std::uint64_t>(GetU16(data, *offset));
  *offset += 2;
  if (length == kLongStringSentinel) {
    if (*offset + 8 > data.size()) return false;
    length = GetU64(data, *offset);
    *offset += 8;
  }
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset),
              static_cast<std::size_t>(length));
  *offset += static_cast<std::size_t>(length);
  return true;
}

std::string TextLineValue(std::string_view encoded, std::string_view key) {
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    const std::size_t equals = line.find('=');
    if (equals != std::string_view::npos && line.substr(0, equals) == key) {
      return std::string(line.substr(equals + 1));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return {};
}

bool TextLineU64(std::string_view encoded, std::string_view key, std::uint64_t* out) {
  const auto value = TextLineValue(encoded, key);
  if (value.empty()) return false;
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (!std::isdigit(ch)) return false;
    parsed = parsed * 10 + static_cast<std::uint64_t>(ch - '0');
  }
  if (out != nullptr) *out = parsed;
  return true;
}

std::string EvidenceValue(std::string_view encoded, std::string_view evidence_kind) {
  const std::string prefix = "evidence=" + std::string(evidence_kind) + ":";
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const std::size_t end = encoded.find('\n', start);
    const std::string_view line =
        encoded.substr(start, end == std::string_view::npos ? encoded.size() - start : end - start);
    if (line.size() >= prefix.size() &&
        line.substr(0, prefix.size()) == prefix) {
      return std::string(line.substr(prefix.size()));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return {};
}

void PopulateTransactionStateFromPayload(std::string_view payload,
                                         ServerExecutionResult* result) {
  if (result == nullptr) return;
  std::uint64_t local_transaction_id = 0;
  if (!TextLineU64(payload, "replacement_local_transaction_id", &local_transaction_id) &&
      !TextLineU64(payload, "local_transaction_id", &local_transaction_id)) {
    return;
  }
  result->transaction_state_present = true;
  result->local_transaction_id = local_transaction_id;
  std::uint64_t snapshot = 0;
  if (!TextLineU64(payload, "replacement_snapshot_visible_through_local_transaction_id", &snapshot)) {
    (void)TextLineU64(payload, "snapshot_visible_through_local_transaction_id", &snapshot);
  }
  result->snapshot_visible_through_local_transaction_id = snapshot;
  result->transaction_uuid = TextLineValue(payload, "replacement_transaction_uuid");
  if (result->transaction_uuid.empty()) {
    result->transaction_uuid = TextLineValue(payload, "transaction_uuid");
  }
  result->transaction_timestamp = TextLineValue(payload, "replacement_transaction_timestamp");
  if (result->transaction_timestamp.empty()) {
    result->transaction_timestamp = TextLineValue(payload, "transaction_timestamp");
  }
  if (result->transaction_timestamp.empty()) {
    result->transaction_timestamp = EvidenceValue(payload, "transaction_timestamp");
  }
}

std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = static_cast<std::uint32_t>(0u - (crc & 1u));
      crc = (crc >> 1u) ^ (0x82f63b78u & mask);
    }
  }
  return ~crc;
}

std::array<std::uint8_t, 16> MakeUuidV7Bytes() {
  static std::random_device rd;
  static std::mt19937_64 rng(rd());
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto timestamp = static_cast<std::uint64_t>(now);
  const auto r1 = rng();
  const auto r2 = rng();
  std::array<std::uint8_t, 16> uuid{};
  uuid[0] = static_cast<std::uint8_t>((timestamp >> 40u) & 0xffu);
  uuid[1] = static_cast<std::uint8_t>((timestamp >> 32u) & 0xffu);
  uuid[2] = static_cast<std::uint8_t>((timestamp >> 24u) & 0xffu);
  uuid[3] = static_cast<std::uint8_t>((timestamp >> 16u) & 0xffu);
  uuid[4] = static_cast<std::uint8_t>((timestamp >> 8u) & 0xffu);
  uuid[5] = static_cast<std::uint8_t>(timestamp & 0xffu);
  for (int i = 6; i < 14; ++i) {
    uuid[static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((r1 >> ((i - 6) * 8)) & 0xffu);
  }
  uuid[14] = static_cast<std::uint8_t>(r2 & 0xffu);
  uuid[15] = static_cast<std::uint8_t>((r2 >> 8u) & 0xffu);
  uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x70u);
  uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
  return uuid;
}

std::string UuidToText(const std::array<std::uint8_t, 16>& uuid) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(kHex[(uuid[i] >> 4u) & 0x0fu]);
    out.push_back(kHex[uuid[i] & 0x0fu]);
  }
  return out;
}

bool UuidPresent(const std::array<std::uint8_t, 16>& uuid) {
  return std::any_of(uuid.begin(), uuid.end(), [](std::uint8_t value) {
    return value != 0;
  });
}

std::array<std::uint8_t, 16> TextToUuid(std::string_view text) {
  std::array<std::uint8_t, 16> out{};
  auto hex_value = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::size_t nibble = 0;
  for (const char ch : text) {
    if (ch == '-') continue;
    const int value = hex_value(ch);
    if (value < 0 || nibble >= 32) return {};
    if ((nibble % 2) == 0) {
      out[nibble / 2] = static_cast<std::uint8_t>(value << 4);
    } else {
      out[nibble / 2] = static_cast<std::uint8_t>(out[nibble / 2] | value);
    }
    ++nibble;
  }
  return nibble == 32 ? out : std::array<std::uint8_t, 16>{};
}

bool IsErrorFrame(const Frame& frame) {
  return frame.header.message_type == kMessageDiagnostic || (frame.header.flags & kFlagError) != 0;
}

void AddDiagnostic(MessageVectorSet* messages,
                   std::string code,
                   std::string message,
                   std::string component = "sbp_sbsql.sbps_client",
                   std::vector<Field> fields = {}) {
  if (messages == nullptr) return;
  messages->diagnostics.push_back(MakeDiagnostic(std::move(code),
                                                 "ERROR",
                                                 std::move(message),
                                                 std::move(component),
                                                 std::move(fields)));
}

struct DecodedMessageVector {
  std::string code;
  std::string message;
  std::vector<Field> fields;
};

std::vector<DecodedMessageVector> DecodeMessageVectors(const std::vector<std::uint8_t>& payload) {
  std::vector<DecodedMessageVector> vectors;
  if (payload.size() < 64 || GetU32(payload, 0) != kMessageVectorMagic ||
      GetU16(payload, 4) != 64 || GetU16(payload, 6) != 1) {
    return vectors;
  }
  const auto vector_count = GetU32(payload, 12);
  std::size_t offset = 64;
  for (std::uint32_t index = 0; index < vector_count && offset + 112 <= payload.size(); ++index) {
    const auto record_bytes = GetU32(payload, offset);
    if (record_bytes < 112 || offset + record_bytes > payload.size()) break;
    const auto language_len = GetU16(payload, offset + 92);
    const auto code_len = GetU16(payload, offset + 94);
    const auto message_key_len = GetU16(payload, offset + 96);
    const auto admin_detail_key_len = GetU16(payload, offset + 98);
    const auto safe_message_len = GetU16(payload, offset + 100);
    const auto field_count = GetU16(payload, offset + 102);
    std::size_t cursor = offset + 112;
    const auto record_end = offset + record_bytes;
    if (cursor + language_len + code_len + message_key_len + admin_detail_key_len + safe_message_len <=
        record_end) {
      cursor += language_len;
      DecodedMessageVector vector;
      vector.code.assign(reinterpret_cast<const char*>(payload.data() + cursor), code_len);
      cursor += code_len + message_key_len + admin_detail_key_len;
      vector.message.assign(reinterpret_cast<const char*>(payload.data() + cursor), safe_message_len);
      cursor += safe_message_len;
      for (std::uint16_t field_index = 0; field_index < field_count && cursor + 8 <= record_end; ++field_index) {
        const auto key_len = GetU16(payload, cursor);
        const auto value_len = GetU32(payload, cursor + 4);
        cursor += 8;
        if (cursor + key_len + value_len > record_end) break;
        Field field;
        field.name.assign(reinterpret_cast<const char*>(payload.data() + cursor), key_len);
        cursor += key_len;
        field.value.assign(reinterpret_cast<const char*>(payload.data() + cursor), value_len);
        cursor += value_len;
        while ((cursor % 4u) != 0u && cursor < record_end) ++cursor;
        vector.fields.push_back(std::move(field));
      }
      vectors.push_back(std::move(vector));
    }
    offset += record_bytes;
  }
  return vectors;
}

void AddFrameDiagnostics(const Frame& frame, MessageVectorSet* messages) {
  auto vectors = DecodeMessageVectors(frame.payload);
  if (vectors.empty()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.REQUEST_FAILED", "The parser-server IPC request failed.");
    return;
  }
  for (auto& vector : vectors) {
    AddDiagnostic(messages,
                  std::move(vector.code),
                  vector.message.empty() ? "The server returned a message vector for this request."
                                         : std::move(vector.message),
                  "sbp_sbsql.sbps_client",
                  std::move(vector.fields));
  }
}

std::vector<std::uint8_t> EncodeFrame(const FrameHeader& input,
                                      const std::vector<std::uint8_t>& payload) {
  FrameHeader header = input;
  header.payload_len = static_cast<std::uint32_t>(payload.size());
  const auto payload_crc = payload.empty() ? 0 : Crc32c(payload.data(), payload.size());
  std::vector<std::uint8_t> out;
  out.reserve(kHeaderBytes + payload.size());
  PutU32(&out, kFrameMagic);
  PutU16(&out, kHeaderBytes);
  PutU16(&out, kProtocolMajor);
  PutU16(&out, kProtocolMinor);
  PutU16(&out, header.message_type);
  PutU32(&out, header.flags);
  PutU32(&out, header.schema_id);
  PutU32(&out, header.payload_len);
  PutU32(&out, 0);
  PutU32(&out, payload_crc);
  PutU64(&out, header.stream_id);
  PutU64(&out, header.sequence_number);
  PutUuid(&out, header.request_uuid);
  PutUuid(&out, header.connection_uuid);
  PutUuid(&out, header.session_uuid);
  const auto header_crc = Crc32c(out.data(), out.size());
  PutAtU32(&out, 24, header_crc);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<std::vector<std::uint8_t>> EncodeFrameSequence(
    const FrameHeader& input,
    const std::vector<std::uint8_t>& payload) {
  if (payload.size() <= kMaxFramePayload) {
    return {EncodeFrame(input, payload)};
  }

  std::vector<std::vector<std::uint8_t>> frames;
  frames.reserve((payload.size() + kMaxFramePayload - 1) / kMaxFramePayload);
  const auto stream_id = input.stream_id == 0 ? 1 : input.stream_id;
  auto sequence_number = input.sequence_number == 0 ? 1 : input.sequence_number;
  std::size_t offset = 0;
  while (offset < payload.size()) {
    const auto chunk_size = std::min<std::size_t>(kMaxFramePayload, payload.size() - offset);
    std::vector<std::uint8_t> chunk(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                    payload.begin() + static_cast<std::ptrdiff_t>(offset + chunk_size));
    FrameHeader header = input;
    header.stream_id = stream_id;
    header.sequence_number = sequence_number++;
    header.flags = input.flags | kFlagPayloadChunk;
    if (offset + chunk_size >= payload.size()) {
      header.flags |= kFlagFinal;
    } else {
      header.flags &= ~kFlagFinal;
    }
    frames.push_back(EncodeFrame(header, chunk));
    offset += chunk_size;
  }
  return frames;
}

bool DecodeFrame(const std::vector<std::uint8_t>& bytes, Frame* frame, MessageVectorSet* messages) {
  if (bytes.size() < kHeaderBytes) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID", "The SBPS response header is incomplete.");
    return false;
  }
  if (GetU32(bytes, 0) != kFrameMagic || GetU16(bytes, 4) != kHeaderBytes) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_HEADER_INVALID", "The SBPS response header is invalid.");
    return false;
  }
  if (GetU16(bytes, 6) != kProtocolMajor) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.PROTOCOL_VERSION_UNSUPPORTED", "The SBPS protocol version is unsupported.");
    return false;
  }
  const auto payload_len = GetU32(bytes, 20);
  if (payload_len > kMaxFramePayload || bytes.size() != kHeaderBytes + payload_len) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID", "The SBPS response frame length is invalid.");
    return false;
  }
  auto header_for_crc = std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + kHeaderBytes);
  PutAtU32(&header_for_crc, 24, 0);
  if (Crc32c(header_for_crc.data(), header_for_crc.size()) != GetU32(bytes, 24)) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_HEADER_CRC_INVALID", "The SBPS response header CRC is invalid.");
    return false;
  }
  if (payload_len != 0 &&
      Crc32c(bytes.data() + kHeaderBytes, payload_len) != GetU32(bytes, 28)) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_PAYLOAD_CRC_INVALID", "The SBPS response payload CRC is invalid.");
    return false;
  }
  frame->header.message_type = GetU16(bytes, 10);
  frame->header.flags = GetU32(bytes, 12);
  frame->header.schema_id = GetU32(bytes, 16);
  frame->header.payload_len = payload_len;
  frame->header.stream_id = GetU64(bytes, 32);
  frame->header.sequence_number = GetU64(bytes, 40);
  frame->header.request_uuid = GetUuid(bytes, 48);
  frame->header.connection_uuid = GetUuid(bytes, 64);
  frame->header.session_uuid = GetUuid(bytes, 80);
  frame->payload.assign(bytes.begin() + kHeaderBytes, bytes.end());
  return true;
}

bool CompatibleChunk(const Frame& first, const Frame& next, std::uint64_t expected_sequence) {
  return (next.header.flags & kFlagPayloadChunk) != 0 &&
         next.header.message_type == first.header.message_type &&
         next.header.schema_id == first.header.schema_id &&
         next.header.stream_id == first.header.stream_id &&
         next.header.sequence_number == expected_sequence &&
         next.header.request_uuid == first.header.request_uuid &&
         next.header.connection_uuid == first.header.connection_uuid &&
         next.header.session_uuid == first.header.session_uuid;
}

std::string EndpointPath(std::string endpoint) {
  constexpr std::string_view unix_prefix = "unix:";
  if (endpoint.starts_with(unix_prefix)) endpoint.erase(0, unix_prefix.size());
  return endpoint;
}

bool ValidateEndpointPath(std::string_view path, MessageVectorSet* messages) {
  if (path.empty()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ENDPOINT_MISSING", "No parser-server IPC endpoint was assigned.");
    return false;
  }
  if (path.size() >= kPortableAfUnixPathLimit) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ENDPOINT_PATH_TOO_LONG", "The parser-server IPC endpoint path is too long.");
    return false;
  }
  return true;
}

#ifdef _WIN32
using SbpsSocketHandle = SOCKET;
constexpr SbpsSocketHandle kInvalidSbpsSocket = INVALID_SOCKET;

bool EnsureWinsockInitialized() {
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}

bool SbpsSocketInterrupted() {
  return ::WSAGetLastError() == WSAEINTR;
}

void CloseSbpsSocket(SbpsSocketHandle fd) {
  if (fd != kInvalidSbpsSocket) {
    ::closesocket(fd);
  }
}

int WriteSocketBytes(SbpsSocketHandle fd, const std::uint8_t* data, std::size_t size) {
  const auto chunk = static_cast<int>(
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  return ::send(fd, reinterpret_cast<const char*>(data), chunk, 0);
}

int ReadSocketBytes(SbpsSocketHandle fd, std::uint8_t* data, std::size_t size) {
  const auto chunk = static_cast<int>(
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
  return ::recv(fd, reinterpret_cast<char*>(data), chunk, 0);
}

bool SetSocketTimeouts(SbpsSocketHandle fd, std::uint32_t timeout_ms) {
  const DWORD timeout = timeout_ms;
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0 &&
         ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
}
#else
using SbpsSocketHandle = int;
constexpr SbpsSocketHandle kInvalidSbpsSocket = -1;

bool SbpsSocketInterrupted() {
  return errno == EINTR;
}

void CloseSbpsSocket(SbpsSocketHandle fd) {
  if (fd >= 0) {
    ::close(fd);
  }
}

int WriteSocketBytes(SbpsSocketHandle fd, const std::uint8_t* data, std::size_t size) {
  const auto chunk =
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
  return static_cast<int>(::write(fd, data, chunk));
}

int ReadSocketBytes(SbpsSocketHandle fd, std::uint8_t* data, std::size_t size) {
  const auto chunk =
      std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
  return static_cast<int>(::read(fd, data, chunk));
}

bool SetSocketTimeouts(SbpsSocketHandle fd, std::uint32_t timeout_ms) {
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000u);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
  return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
         ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}
#endif

class Fd {
 public:
  explicit Fd(SbpsSocketHandle fd = kInvalidSbpsSocket) : fd_(fd) {}
  ~Fd() { CloseSbpsSocket(fd_); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  SbpsSocketHandle get() const { return fd_; }
  bool valid() const { return fd_ != kInvalidSbpsSocket; }
 private:
  SbpsSocketHandle fd_;
};

bool WriteAll(SbpsSocketHandle fd, const std::vector<std::uint8_t>& bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto rc = WriteSocketBytes(fd, bytes.data() + offset, bytes.size() - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && SbpsSocketInterrupted()) continue;
    return false;
  }
  return true;
}

bool ReadExact(SbpsSocketHandle fd, std::vector<std::uint8_t>* out, std::size_t bytes) {
  out->assign(bytes, 0);
  std::size_t offset = 0;
  while (offset < bytes) {
    const auto rc = ReadSocketBytes(fd, out->data() + offset, bytes - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && SbpsSocketInterrupted()) continue;
    return false;
  }
  return true;
}

bool ReadPhysicalFrame(SbpsSocketHandle fd, Frame* frame, MessageVectorSet* messages) {
  std::vector<std::uint8_t> header;
  if (!ReadExact(fd, &header, kHeaderBytes)) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.READ_FAILED", "The parser could not read the SBPS response header.");
    return false;
  }
  const auto payload_len = GetU32(header, 20);
  if (payload_len > kMaxFramePayload) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID", "The SBPS response frame exceeds the negotiated physical frame limit.");
    return false;
  }
  std::vector<std::uint8_t> payload;
  if (payload_len > 0 && !ReadExact(fd, &payload, payload_len)) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.READ_FAILED", "The parser could not read the SBPS response payload.");
    return false;
  }
  header.insert(header.end(), payload.begin(), payload.end());
  return DecodeFrame(header, frame, messages);
}

bool AssembleChunkedFrame(SbpsSocketHandle fd, Frame* frame, MessageVectorSet* messages) {
  if ((frame->header.flags & kFlagPayloadChunk) == 0) return true;
  if (frame->header.stream_id == 0 || frame->header.sequence_number == 0) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID", "The SBPS chunk sequence header is invalid.");
    return false;
  }

  Frame assembled = *frame;
  std::vector<std::uint8_t> payload = frame->payload;
  std::uint64_t expected_sequence = frame->header.sequence_number + 1;
  while ((assembled.header.flags & kFlagFinal) == 0) {
    Frame next;
    if (!ReadPhysicalFrame(fd, &next, messages)) return false;
    if (!CompatibleChunk(*frame, next, expected_sequence)) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID", "The SBPS chunk sequence is not contiguous.");
      return false;
    }
    if (payload.size() + next.payload.size() > kMaxChunkedPayload) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.PAYLOAD_TOO_LARGE", "The assembled SBPS payload exceeds the protocol limit.");
      return false;
    }
    payload.insert(payload.end(), next.payload.begin(), next.payload.end());
    assembled = next;
    ++expected_sequence;
  }
  frame->payload = std::move(payload);
  frame->header.payload_len = static_cast<std::uint32_t>(frame->payload.size());
  frame->header.flags = (assembled.header.flags & ~kFlagPayloadChunk) | kFlagFinal;
  return true;
}

bool SendRequest(const std::string& endpoint,
                 const FrameHeader& header,
                 const std::vector<std::uint8_t>& payload,
                 Frame* response,
                 MessageVectorSet* messages,
                 std::uint32_t timeout_ms = kDefaultSbpsRequestTimeoutMs) {
  const auto trace_total_start_us = SteadyNowMicros();
  std::uint64_t connect_us = 0;
  std::uint64_t encode_us = 0;
  std::uint64_t write_us = 0;
  std::uint64_t read_us = 0;
  std::uint64_t assemble_us = 0;
  std::size_t encoded_frame_bytes = 0;
  auto trace_and_return = [&](bool ok) {
    AppendSbpsClientPhaseTrace(header.message_type,
                               header.schema_id,
                               payload.size(),
                               encoded_frame_bytes,
                               response == nullptr ? 0 : response->payload.size(),
                               ok,
                               connect_us,
                               encode_us,
                               write_us,
                               read_us,
                               assemble_us,
                               SteadyNowMicros() - trace_total_start_us);
    return ok;
  };
  const auto path = EndpointPath(endpoint);
  if (!ValidateEndpointPath(path, messages)) return trace_and_return(false);
#ifdef _WIN32
  if (!EnsureWinsockInitialized()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.SOCKET_CREATE_FAILED", "Winsock initialization failed for the SBPS client.");
    return trace_and_return(false);
  }
#endif
  Fd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!fd.valid()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.SOCKET_CREATE_FAILED", "The parser could not create an SBPS socket.");
    return trace_and_return(false);
  }
  (void)SetSocketTimeouts(fd.get(), timeout_ms);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ENDPOINT_PATH_TOO_LONG", "The parser-server IPC endpoint path is too long.");
    return trace_and_return(false);
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  const auto connect_start_us = SteadyNowMicros();
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    connect_us = SteadyNowMicros() - connect_start_us;
    AddDiagnostic(messages, "PARSER_SERVER_IPC.CONNECT_FAILED", "The parser could not connect to sb_server.");
    return trace_and_return(false);
  }
  connect_us = SteadyNowMicros() - connect_start_us;
  const auto encode_start_us = SteadyNowMicros();
  const auto encoded_frames = EncodeFrameSequence(header, payload);
  for (const auto& encoded : encoded_frames) {
    encoded_frame_bytes += encoded.size();
  }
  encode_us = SteadyNowMicros() - encode_start_us;
  const auto write_start_us = SteadyNowMicros();
  for (const auto& encoded : encoded_frames) {
    if (!WriteAll(fd.get(), encoded)) {
      write_us = SteadyNowMicros() - write_start_us;
      AddDiagnostic(messages, "PARSER_SERVER_IPC.WRITE_FAILED", "The parser could not write to sb_server.");
      return trace_and_return(false);
    }
  }
  write_us = SteadyNowMicros() - write_start_us;
  const auto read_start_us = SteadyNowMicros();
  if (!ReadPhysicalFrame(fd.get(), response, messages)) {
    read_us = SteadyNowMicros() - read_start_us;
    return trace_and_return(false);
  }
  read_us = SteadyNowMicros() - read_start_us;
  const auto assemble_start_us = SteadyNowMicros();
  const bool assembled = AssembleChunkedFrame(fd.get(), response, messages);
  assemble_us = SteadyNowMicros() - assemble_start_us;
  return trace_and_return(assembled);
}

FrameHeader BaseHeader(std::uint16_t message_type,
                       std::uint32_t schema_id,
                       const std::array<std::uint8_t, 16>& session_uuid = {},
                       const std::array<std::uint8_t, 16>& connection_uuid = {}) {
  FrameHeader header;
  header.message_type = message_type;
  header.schema_id = schema_id;
  header.flags = 0;
  header.sequence_number = 1;
  header.request_uuid = MakeUuidV7Bytes();
  header.connection_uuid = connection_uuid;
  header.session_uuid = session_uuid;
  return header;
}

std::vector<std::uint8_t> EncodeBuiltInHelloPayload() {
  std::vector<std::uint8_t> out;
  PutUuid(&out, MakeUuidV7Bytes());
  PutUuid(&out, MakeUuidV7Bytes());
  PutUuid(&out, MakeUuidV7Bytes());
  PutUuid(&out, MakeUuidV7Bytes());
  PutU32(&out, 3);
  PutU32(&out, 0);
  PutString(&out, "SBPS");
  PutString(&out, "sif.test");
  PutString(&out, "sif.test.bundle");
  PutBytes32(&out, {});
  PutUuid(&out, MakeUuidV7Bytes());
  PutUuid(&out, MakeUuidV7Bytes());
  PutU64(&out, 1);
  std::array<std::uint8_t, 32> capabilities{};
  capabilities[0] = 1;
  PutBytes32(&out, capabilities);
  return out;
}

AuthCredentialEnvelope CredentialsFromTestWirePayload(std::string_view auth_payload) {
  const auto text = TrimAscii(auth_payload);
  AuthCredentialEnvelope credentials;
  const auto split = text.find_first_of(" \t\r\n");
  credentials.principal = std::string(split == std::string_view::npos ? text : text.substr(0, split));
  credentials.credential_evidence = split == std::string_view::npos
      ? std::string{}
      : TrimAscii(text.substr(split + 1));
  credentials.credential_evidence_present = !credentials.credential_evidence.empty();
  return credentials;
}

std::vector<std::uint8_t> EncodeAuthPayload(const AuthCredentialEnvelope& credentials,
                                            const std::array<std::uint8_t, 16>& connection_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  PutU8(&out, credentials.credential_evidence_present ? 1 : 0);
  PutU8(&out, credentials.credential_invalid ? 1 : 0);
  PutU8(&out, credentials.mfa_required ? 1 : 0);
  PutU8(&out, credentials.mfa_evidence_present ? 1 : 0);
  PutString(&out, credentials.provider_family.empty() ? "local_password" : credentials.provider_family);
  PutString(&out, credentials.principal);
  PutString(&out, credentials.requested_database.empty() ? "default" : credentials.requested_database);
  PutString(&out, credentials.requested_language.empty() ? "en" : credentials.requested_language);
  PutString(&out, credentials.credential_evidence);
  PutString(&out, credentials.application_name);
  PutString(&out, credentials.requested_role);
  return out;
}

std::vector<std::uint8_t> EncodeAttachPayload(const std::array<std::uint8_t, 16>& connection_uuid,
                                              const std::array<std::uint8_t, 16>& auth_context_uuid,
                                              std::string_view requested_database) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  PutUuid(&out, auth_context_uuid);
  PutString(&out, requested_database.empty() ? "default" : requested_database);
  PutString(&out, "read_write");
  return out;
}

std::vector<std::uint8_t> EncodeExecutePayload(const std::array<std::uint8_t, 16>& session_uuid,
                                               std::string_view encoded_sblr_envelope,
                                               bool cursor_requested) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, {});
  PutU8(&out, cursor_requested ? 1 : 0);
  PutString(&out, encoded_sblr_envelope);
  return out;
}

std::vector<std::uint8_t> EncodePreparePayload(const SessionContext& session,
                                               const std::array<std::uint8_t, 16>& session_uuid,
                                               std::string_view encoded_sblr_envelope) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, MakeUuidV7Bytes());
  PutU64(&out, session.catalog_epoch);
  PutU64(&out, session.security_policy_epoch);
  PutU64(&out, session.security_policy_epoch);
  PutString(&out, encoded_sblr_envelope);
  return out;
}

std::vector<std::uint8_t> EncodeExecutePreparedPayload(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid,
    std::string_view encoded_sblr_envelope,
    bool cursor_requested) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, prepared_statement_uuid);
  PutU8(&out, cursor_requested ? 1 : 0);
  PutString(&out, encoded_sblr_envelope);
  return out;
}

std::vector<std::uint8_t> EncodeCursorPayload(const std::array<std::uint8_t, 16>& session_uuid,
                                              std::string_view cursor_uuid,
                                              std::uint64_t max_rows = 1,
                                              std::uint64_t max_bytes = 0,
                                              std::uint32_t fetch_flags = 0) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, TextToUuid(cursor_uuid));
  PutU64(&out, max_rows);
  PutU64(&out, max_bytes);
  PutU32(&out, fetch_flags);
  return out;
}

std::string JoinSearchPath(const SessionContext& session) {
  std::string out;
  for (const auto& item : session.search_path) {
    if (!out.empty()) out.push_back(',');
    out += item;
  }
  return out;
}

std::vector<std::uint8_t> EncodeResolveNamePayload(const SessionContext& session,
                                                   std::string_view presented_name,
                                                   bool quoted,
                                                   std::string_view object_class,
                                                   const ParserConfig& config) {
  std::vector<std::uint8_t> out;
  PutString(&out, presented_name);
  PutU8(&out, quoted ? 1 : 0);
  const std::string identifier_profile =
      session.dialect_profile_uuid.empty() ? "sbsql_v3" : session.dialect_profile_uuid;
  PutString(&out, identifier_profile);
  PutString(&out, session.default_language.empty() ? "en" : session.default_language);
  PutString(&out, JoinSearchPath(session));
  PutString(&out, object_class);
  (void)config;
  return out;
}

std::vector<std::uint8_t> EncodeRenderUuidPayload(std::string_view object_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, TextToUuid(object_uuid));
  return out;
}

std::vector<std::uint8_t> EncodeManagementPayload(std::string_view operation_key,
                                                  std::string_view target_uuid,
                                                  std::string_view mode,
                                                  std::string_view audit_reason,
                                                  std::uint64_t timeout_ms,
                                                  bool include_history) {
  const std::vector<std::pair<std::string, std::string>> fields{
      {"operation_key", std::string(operation_key)},
      {"target_uuid", std::string(target_uuid)},
      {"mode", std::string(mode)},
      {"audit_reason", std::string(audit_reason)},
      {"timeout_ms", std::to_string(timeout_ms)},
      {"include_history", include_history ? "true" : "false"},
  };
  std::vector<std::uint8_t> out;
  PutU16(&out, static_cast<std::uint16_t>(fields.size()));
  for (const auto& [key, value] : fields) {
    PutString(&out, key);
    PutString(&out, value);
  }
  return out;
}

PublicNameResolutionResult DecodePublicNameResultPayload(const Frame& response,
                                                         std::string_view success_outcome) {
  PublicNameResolutionResult result;
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || offset + 16 > response.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The public name/UUID response payload is malformed.",
        "sbp_sbsql.sbps_client"));
    return result;
  }
  const auto object_uuid = GetUuid(response.payload, offset);
  offset += 16;
  std::string canonical_name;
  std::string object_class;
  if (!ReadString(response.payload, &offset, &canonical_name) ||
      !ReadString(response.payload, &offset, &object_class) ||
      offset + 16 > response.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The public name/UUID response payload is malformed.",
        "sbp_sbsql.sbps_client"));
    return result;
  }
  result.catalog_epoch = GetU64(response.payload, offset);
  offset += 8;
  result.security_epoch = GetU64(response.payload, offset);
  offset += 8;
  std::string detail;
  (void)ReadString(response.payload, &offset, &detail);
  if (outcome != success_outcome) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.NAME_RESOLUTION.NOT_FOUND_OR_NOT_VISIBLE",
        "ERROR",
        "object name could not be resolved or is not visible",
        "sbp_sbsql.sbps_client"));
    return result;
  }
  result.resolved = true;
  result.object_uuid = UuidToText(object_uuid);
  result.canonical_name = canonical_name;
  result.object_class = object_class;
  return result;
}

} // namespace

SbpsClient::SbpsClient(std::string endpoint) : endpoint_(std::move(endpoint)) {}

bool SbpsClient::SendHello(MessageVectorSet* messages) const {
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageHello, kSchemaHelloRequestV1),
                   EncodeBuiltInHelloPayload(),
                   &response,
                   messages)) {
    return false;
  }
  if (response.header.message_type != kMessageHelloAccept || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, messages);
    return false;
  }
  return true;
}

bool SbpsClient::AuthenticateAndAttach(std::string_view auth_payload,
                                       const ParserConfig& config,
                                       SessionContext* session,
                                       MessageVectorSet* messages) const {
  auto credentials = CredentialsFromTestWirePayload(auth_payload);
  if (!config.database_token.empty() && credentials.requested_database == "default") {
    credentials.requested_database = config.database_token;
  }
  return AuthenticateAndAttach(credentials, config, session, messages);
}

bool SbpsClient::AuthenticateAndAttach(const AuthCredentialEnvelope& credentials,
                                       const ParserConfig& config,
                                       SessionContext* session,
                                       MessageVectorSet* messages) const {
  (void)config;
  if (session == nullptr) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.SESSION_CONTEXT_MISSING", "The parser session context is unavailable.");
    return false;
  }
  if (!SendHello(messages)) return false;

  Frame auth_response;
  const auto connection_uuid = MakeUuidV7Bytes();
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageAuthHandoff, kSchemaAuthHandoffV1, {}, connection_uuid),
                   EncodeAuthPayload(credentials, connection_uuid),
                   &auth_response,
                   messages)) {
    return false;
  }
  if (auth_response.header.message_type != kMessageAuthResult) {
    AddFrameDiagnostics(auth_response, messages);
    return false;
  }
  std::size_t offset = 0;
  std::string auth_outcome;
  if (!ReadString(auth_response.payload, &offset, &auth_outcome) || auth_outcome != "accepted") {
    if (IsErrorFrame(auth_response)) AddFrameDiagnostics(auth_response, messages);
    else AddDiagnostic(messages, "SECURITY.AUTHENTICATION.FAILED", "Authentication failed.");
    return false;
  }
  if (offset + 16 * 4 + 8 > auth_response.payload.size()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.AUTH_RESULT_INVALID", "The server authentication result payload is malformed.");
    return false;
  }
  const auto auth_context_uuid = GetUuid(auth_response.payload, offset);
  offset += 16;
  const auto auth_session_uuid = GetUuid(auth_response.payload, offset);
  offset += 16;
  const auto principal_uuid = GetUuid(auth_response.payload, offset);
  offset += 16;
  const auto effective_user_uuid = GetUuid(auth_response.payload, offset);
  offset += 16;
  const auto security_epoch = GetU64(auth_response.payload, offset);
  (void)auth_session_uuid;
  (void)principal_uuid;
  (void)effective_user_uuid;
  (void)security_epoch;

  Frame attach_response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageAttachDatabase,
                              kSchemaAttachRequestV1,
                              {},
                              connection_uuid),
                   EncodeAttachPayload(connection_uuid, auth_context_uuid, credentials.requested_database),
                   &attach_response,
                   messages)) {
    return false;
  }
  if (attach_response.header.message_type != kMessageAttachResult) {
    AddFrameDiagnostics(attach_response, messages);
    return false;
  }
  offset = 0;
  std::string attach_outcome;
  if (!ReadString(attach_response.payload, &offset, &attach_outcome) || attach_outcome != "accepted") {
    if (IsErrorFrame(attach_response)) AddFrameDiagnostics(attach_response, messages);
    else AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_DATABASE_FAILED", "Database attach failed.");
    return false;
  }
  if (offset + 16 + 16 > attach_response.payload.size()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach result payload is malformed.");
    return false;
  }
  const auto session_uuid = GetUuid(attach_response.payload, offset);
  offset += 16;
  const auto user_uuid = GetUuid(attach_response.payload, offset);
  offset += 16;
  std::string database_path;
  std::string database_uuid;
  std::string attach_mode;
  if (!ReadString(attach_response.payload, &offset, &database_path) ||
      !ReadString(attach_response.payload, &offset, &database_uuid) ||
      !ReadString(attach_response.payload, &offset, &attach_mode) ||
      offset + 8 * 5 > attach_response.payload.size()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach result payload is malformed.");
    return false;
  }
  const auto catalog_generation = GetU64(attach_response.payload, offset);
  offset += 8;
  const auto attach_security_epoch = GetU64(attach_response.payload, offset);
  offset += 8;
  const auto policy_generation = GetU64(attach_response.payload, offset);
  offset += 8;
  const auto name_resolution_epoch = GetU64(attach_response.payload, offset);
  offset += 8;
  const auto descriptor_epoch = GetU64(attach_response.payload, offset);
  offset += 8;
  std::string attach_detail;
  std::string engine_health;
  if (offset < attach_response.payload.size() &&
      (!ReadString(attach_response.payload, &offset, &attach_detail) ||
       !ReadString(attach_response.payload, &offset, &engine_health))) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach result payload is malformed.");
    return false;
  }
  std::uint64_t local_transaction_id = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
  std::string transaction_uuid;
  std::string transaction_timestamp;
  std::vector<std::string> effective_role_uuids;
  std::vector<std::string> effective_group_uuids;
  if (offset < attach_response.payload.size()) {
    if (offset + 16 > attach_response.payload.size()) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach result payload is malformed.");
      return false;
    }
    local_transaction_id = GetU64(attach_response.payload, offset);
    offset += 8;
    snapshot_visible_through_local_transaction_id = GetU64(attach_response.payload, offset);
    offset += 8;
    if (!ReadString(attach_response.payload, &offset, &transaction_uuid) ||
        !ReadString(attach_response.payload, &offset, &transaction_timestamp)) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach result payload is malformed.");
      return false;
    }
  }
  if (offset < attach_response.payload.size()) {
    auto add_unique_uuid_text = [](std::vector<std::string>* values,
                                   const std::array<std::uint8_t, 16>& uuid) {
      if (values == nullptr || !UuidPresent(uuid)) return;
      const std::string text = UuidToText(uuid);
      if (std::find(values->begin(), values->end(), text) == values->end()) {
        values->push_back(text);
      }
    };
    if (offset + 4 > attach_response.payload.size()) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach role payload is malformed.");
      return false;
    }
    const auto role_count = GetU32(attach_response.payload, offset);
    offset += 4;
    for (std::uint32_t index = 0; index < role_count; ++index) {
      if (offset + 16 > attach_response.payload.size()) {
        AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach role payload is malformed.");
        return false;
      }
      add_unique_uuid_text(&effective_role_uuids, GetUuid(attach_response.payload, offset));
      offset += 16;
    }
    if (offset + 16 > attach_response.payload.size()) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach active-role payload is malformed.");
      return false;
    }
    add_unique_uuid_text(&effective_role_uuids, GetUuid(attach_response.payload, offset));
    offset += 16;
    if (offset + 4 > attach_response.payload.size()) {
      AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach group payload is malformed.");
      return false;
    }
    const auto group_count = GetU32(attach_response.payload, offset);
    offset += 4;
    for (std::uint32_t index = 0; index < group_count; ++index) {
      if (offset + 16 > attach_response.payload.size()) {
        AddDiagnostic(messages, "PARSER_SERVER_IPC.ATTACH_RESULT_INVALID", "The server attach group payload is malformed.");
        return false;
      }
      add_unique_uuid_text(&effective_group_uuids, GetUuid(attach_response.payload, offset));
      offset += 16;
    }
  }
  (void)database_path;
  (void)attach_mode;
  (void)attach_detail;
  (void)engine_health;

  if (local_transaction_id == 0) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_TRANSACTION_REQUIRED",
                  "Accepted database attach did not publish the required active transaction.");
    return false;
  }

  session->authenticated = true;
  session->session_uuid = UuidToText(session_uuid);
  session->connection_uuid = UuidToText(connection_uuid);
  session->database_uuid = database_uuid;
  session->authenticated_user_uuid = UuidToText(user_uuid);
  session->principal_claim = credentials.principal;
  session->auth_provider_family =
      credentials.provider_family.empty() ? "local_password" : credentials.provider_family;
  session->effective_role_uuids = std::move(effective_role_uuids);
  session->effective_group_uuids = std::move(effective_group_uuids);
  ApplySbpsLanguageContext(session,
                           credentials.requested_language,
                           descriptor_epoch == 0 ? name_resolution_epoch
                                                 : descriptor_epoch,
                           name_resolution_epoch);
  session->dialect_profile_uuid = "sbsql_v3";
  session->search_path = {"sys", "public"};
  session->transaction_context = "always_active";
  session->local_transaction_id = local_transaction_id;
  session->snapshot_visible_through_local_transaction_id =
      snapshot_visible_through_local_transaction_id;
  session->transaction_uuid = transaction_uuid;
  session->transaction_timestamp = transaction_timestamp;
  session->catalog_epoch = catalog_generation;
  session->security_policy_epoch = attach_security_epoch == 0 ? policy_generation : attach_security_epoch;
  session->descriptor_epoch = descriptor_epoch == 0 ? name_resolution_epoch : descriptor_epoch;
  return true;
}

PublicNameResolutionResult SbpsClient::ResolveNamePublic(const SessionContext& session,
                                                        std::string_view presented_name,
                                                        bool quoted,
                                                        std::string_view object_class,
                                                        const ParserConfig& config) const {
  PublicNameResolutionResult result;
  if (!session.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED",
        "ERROR",
        "public name resolution requires an authenticated server session",
        "sbp_sbsql.sbps_client"));
    return result;
  }
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageResolveNameRequest,
                              kSchemaResolveNameRequestV1,
                              session_uuid,
                              connection_uuid),
                   EncodeResolveNamePayload(session, presented_name, quoted, object_class, config),
                   &response,
                   &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageResolveNameResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  return DecodePublicNameResultPayload(response, "resolved");
}

PublicNameResolutionResult SbpsClient::RenderUuidPublic(const SessionContext& session,
                                                       std::string_view object_uuid) const {
  PublicNameResolutionResult result;
  if (!session.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED",
        "ERROR",
        "public UUID rendering requires an authenticated server session",
        "sbp_sbsql.sbps_client"));
    return result;
  }
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageRenderUuidRequest,
                              kSchemaRenderUuidRequestV1,
                              session_uuid,
                              connection_uuid),
                   EncodeRenderUuidPayload(object_uuid),
                   &response,
                   &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageRenderUuidResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  return DecodePublicNameResultPayload(response, "rendered");
}

ServerExecutionResult SbpsClient::ExecuteSblr(const SessionContext& session,
                                             std::string_view encoded_sblr_envelope,
                                             bool cursor_requested) const {
  ServerExecutionResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageExecuteSblr,
                              kSchemaExecuteSblrV1,
                              session_uuid,
                              connection_uuid),
                   EncodeExecutePayload(session_uuid, encoded_sblr_envelope, cursor_requested),
                   &response,
                   &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageExecuteResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || outcome != "accepted") {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_REJECTED", "The server rejected SBLR execution.");
    result.messages = std::move(messages);
    return result;
  }
  if (offset + 16 + 16 + 8 > response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID", "The server execute result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  offset += 16; // server request UUID
  result.cursor_uuid = UuidToText(GetUuid(response.payload, offset));
  offset += 16;
  result.row_count = GetU64(response.payload, offset);
  offset += 8;
  if (!ReadString(response.payload, &offset, &result.operation_id) ||
      !ReadString(response.payload, &offset, &result.row_packet)) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID", "The server execute result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  PopulateTransactionStateFromPayload(result.row_packet, &result);
  result.accepted = true;
  return result;
}

ServerPrepareSblrResult SbpsClient::PrepareSblr(
    const SessionContext& session,
    std::string_view encoded_sblr_envelope) const {
  ServerPrepareSblrResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessagePrepareSblr,
                              kSchemaPrepareSblrV1,
                              session_uuid,
                              connection_uuid),
                   EncodePreparePayload(session, session_uuid, encoded_sblr_envelope),
                   &response,
                   &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessagePrepareResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || outcome != "accepted") {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.PREPARE_REJECTED", "The server rejected SBLR prepare.");
    result.messages = std::move(messages);
    return result;
  }
  if (offset + 16 > response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID", "The server prepare result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.prepared_statement_uuid = UuidToText(GetUuid(response.payload, offset));
  offset += 16;
  if (!ReadString(response.payload, &offset, &result.operation_id) ||
      !ReadString(response.payload, &offset, &result.detail)) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID", "The server prepare result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  return result;
}

ServerExecutionResult SbpsClient::ExecutePreparedSblr(
    const SessionContext& session,
    std::string_view prepared_statement_uuid,
    std::string_view encoded_sblr_envelope,
    bool cursor_requested) const {
  ServerExecutionResult result;
  if (prepared_statement_uuid.empty()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.PREPARED_HANDLE_REQUIRED",
                  "Prepared SBLR execution requires a prepared statement UUID.");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageExecuteSblr,
                              kSchemaExecuteSblrV1,
                              session_uuid,
                              connection_uuid),
                   EncodeExecutePreparedPayload(session_uuid,
                                                TextToUuid(prepared_statement_uuid),
                                                encoded_sblr_envelope,
                                                cursor_requested),
                   &response,
                   &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageExecuteResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || outcome != "accepted") {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_REJECTED", "The server rejected prepared SBLR execution.");
    result.messages = std::move(messages);
    return result;
  }
  if (offset + 16 + 16 + 8 > response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID", "The server execute result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  offset += 16; // server request UUID
  result.cursor_uuid = UuidToText(GetUuid(response.payload, offset));
  offset += 16;
  result.row_count = GetU64(response.payload, offset);
  offset += 8;
  if (!ReadString(response.payload, &offset, &result.operation_id) ||
      !ReadString(response.payload, &offset, &result.row_packet)) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID", "The server execute result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  PopulateTransactionStateFromPayload(result.row_packet, &result);
  result.accepted = true;
  return result;
}

ServerFetchResult SbpsClient::FetchCursor(const SessionContext& session,
                                          std::string_view cursor_uuid,
                                          std::uint64_t max_rows,
                                          std::uint64_t max_bytes,
                                          std::uint32_t fetch_flags) const {
  ServerFetchResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageFetch, kSchemaFetchV1, session_uuid, connection_uuid),
                   EncodeCursorPayload(session_uuid, cursor_uuid, max_rows, max_bytes, fetch_flags),
                   &response,
                   &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageFetchResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  if (response.payload.size() < 16 + 8) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.FETCH_RESULT_INVALID", "The server fetch result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  result.cursor_uuid = UuidToText(GetUuid(response.payload, offset));
  offset += 16;
  result.row_count = GetU64(response.payload, offset);
  offset += 8;
  if (!ReadString(response.payload, &offset, &result.row_packet) || offset >= response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.FETCH_RESULT_INVALID", "The server fetch result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.end_of_cursor = response.payload[offset++] != 0;
  if (!ReadString(response.payload, &offset, &result.detail)) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.FETCH_RESULT_INVALID", "The server fetch result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  return result;
}

ServerCloseCursorResult SbpsClient::CloseCursor(const SessionContext& session,
                                                std::string_view cursor_uuid) const {
  ServerCloseCursorResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageCloseCursor,
                              kSchemaCloseCursorV1,
                              session_uuid,
                              connection_uuid),
                   EncodeCursorPayload(session_uuid, cursor_uuid, 1, 0, 0),
                   &response,
                   &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageCloseCursorResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || offset + 16 > response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.CLOSE_CURSOR_RESULT_INVALID", "The server close-cursor result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = outcome == "accepted";
  result.cursor_uuid = UuidToText(GetUuid(response.payload, offset));
  offset += 16;
  (void)ReadString(response.payload, &offset, &result.detail);
  return result;
}

ServerCloseCursorResult SbpsClient::CancelCursor(const SessionContext& session,
                                                 std::string_view cursor_uuid) const {
  ServerCloseCursorResult result;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageCloseCursor,
                              kSchemaCloseCursorV1,
                              session_uuid,
                              connection_uuid),
                   EncodeCursorPayload(session_uuid, cursor_uuid, 1, 0, kCursorCloseFlagCancel),
                   &response,
                   &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageCloseCursorResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) || offset + 16 > response.payload.size()) {
    AddDiagnostic(&messages, "PARSER_SERVER_IPC.CLOSE_CURSOR_RESULT_INVALID", "The server close-cursor result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = outcome == "accepted";
  result.cursor_uuid = UuidToText(GetUuid(response.payload, offset));
  offset += 16;
  (void)ReadString(response.payload, &offset, &result.detail);
  return result;
}

ServerManagementResult SbpsClient::Manage(const SessionContext& session,
                                          std::string_view operation_key,
                                          std::string_view target_uuid,
                                          std::string_view mode,
                                          std::string_view audit_reason,
                                          std::uint64_t timeout_ms,
                                          bool include_history) const {
  ServerManagementResult result;
  result.operation_key = std::string(operation_key);
  if (!session.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED",
        "ERROR",
        "server management requests require an authenticated server session",
        "sbp_sbsql.sbps_client"));
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageManagementRequest,
                              kSchemaManagementRequestV1,
                              session_uuid,
                              connection_uuid),
                   EncodeManagementPayload(operation_key,
                                           target_uuid,
                                           mode,
                                           audit_reason,
                                           timeout_ms,
                                           include_history),
                   &response,
                   &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageManagementResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.schema_id != kSchemaManagementResponseV1) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.MANAGEMENT_RESULT_INVALID",
                  "The server management response schema is not supported.");
    result.messages = std::move(messages);
    return result;
  }
  result.payload.assign(reinterpret_cast<const char*>(response.payload.data()),
                        response.payload.size());
  result.accepted = true;
  return result;
}

bool SbpsClient::DisconnectSession(const SessionContext& session, MessageVectorSet* messages) const {
  if (!session.authenticated || session.session_uuid.empty()) return true;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageDisconnectNotice, 0, session_uuid, connection_uuid),
                   {},
                   &response,
                   messages)) {
    return false;
  }
  if (response.header.message_type != kMessageDisconnectNotice || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, messages);
    return false;
  }
  return true;
}

} // namespace scratchbird::parser::sbsql
