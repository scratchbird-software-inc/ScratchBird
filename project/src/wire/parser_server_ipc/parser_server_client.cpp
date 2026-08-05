// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parser_server_client.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
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

namespace scratchbird::parser::ipc {
namespace {

constexpr std::uint32_t kFrameMagic = 0x53504253;  // SBPS
constexpr std::uint32_t kMessageVectorMagic = 0x564d4253;  // SBMV
constexpr std::uint16_t kHeaderBytes = 96;
constexpr std::uint16_t kProtocolMajor = 1;
constexpr std::uint16_t kProtocolMinor = 0;
constexpr std::uint8_t kCapabilityBaseline = 0x01u;
constexpr std::uint8_t kCapabilityTransactionRoutingV2 = 0x02u;
constexpr std::uint8_t kCapabilityPreparedMetadataTransferV1 = 0x04u;
constexpr std::uint8_t kCapabilityRelationDescriptorProjectionV3 = 0x08u;
constexpr std::size_t kHelloAcceptCapabilityOffset = 42;
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
constexpr std::uint32_t kSchemaPrepareSblrV2 = 4009;
constexpr std::uint32_t kSchemaPrepareResultV2 = 4010;
constexpr std::uint32_t kSchemaExecuteSblrV2 = 4011;
constexpr std::uint32_t kSchemaExecuteResultV2 = 4012;
constexpr std::uint32_t kSchemaExecuteCanonicalSblrV1 = 4015;
constexpr std::uint32_t kSchemaClosePreparedSblrV1 = 4013;
constexpr std::uint32_t kSchemaClosePreparedSblrResultV1 = 4014;
constexpr std::uint32_t kSchemaManagementRequestV1 = 6001;
constexpr std::uint32_t kSchemaManagementResponseV1 = 6002;
constexpr std::uint32_t kSchemaResolveNameRequestV1 = 7001;
constexpr std::uint32_t kSchemaResolveNameRequestV2 = 7005;
constexpr std::uint32_t kSchemaResolveNameResultV2 = 7006;
constexpr std::uint32_t kSchemaResolveNameRequestV3 = 7007;
constexpr std::uint32_t kSchemaResolveNameResultV3 = 7008;
constexpr std::uint32_t kSchemaRenderUuidRequestV1 = 7003;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV1 = 7011;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV1 = 7012;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV2 = 7013;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV2 = 7014;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV3 = 7015;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV3 = 7016;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV4 = 7017;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV4 = 7018;
constexpr std::uint32_t kSchemaAcquireStatementContextRequestV5 = 7019;
constexpr std::uint32_t kSchemaAcquireStatementContextResultV5 = 7020;
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
constexpr std::uint16_t kMessageAcquireStatementContextRequest = 36;
constexpr std::uint16_t kMessageAcquireStatementContextResult = 37;
constexpr std::uint16_t kMessagePrepareSblr = 40;
constexpr std::uint16_t kMessagePrepareResult = 41;
constexpr std::uint16_t kMessageExecuteSblr = 42;
constexpr std::uint16_t kMessageExecuteResult = 43;
constexpr std::uint16_t kMessageFetch = 44;
constexpr std::uint16_t kMessageFetchResult = 45;
constexpr std::uint16_t kMessageCloseCursor = 46;
constexpr std::uint16_t kMessageCloseCursorResult = 47;
constexpr std::uint16_t kMessageClosePreparedSblr = 48;
constexpr std::uint16_t kMessageClosePreparedSblrResult = 49;
constexpr std::uint16_t kMessageDiagnostic = 60;
constexpr std::uint16_t kMessageDisconnectNotice = 74;
constexpr std::uint32_t kMaxFramePayload = 1024 * 1024;
constexpr std::uint64_t kMaxChunkedPayload = static_cast<std::uint64_t>(kMaxFramePayload) * 16u;
constexpr std::uint32_t kCursorCloseFlagCancel = 1u << 0;
constexpr std::uint16_t kLongStringSentinel = 0xffff;
constexpr std::uint32_t kDefaultSbpsRequestTimeoutMs = 300000;
constexpr std::size_t kPortableAfUnixPathLimit = 108;
constexpr std::size_t kMaxSbpsClientPublicResolutionCacheEntries = 8192;
constexpr std::uint8_t kResolveNameProjectionRelationDescriptorV1 = 0x01u;
constexpr std::uint8_t kRelationDescriptorExtensionKind = 0x02u;
constexpr std::uint8_t kRelationDescriptorExtensionVersion = 0x01u;
constexpr std::uint8_t kRelationDescriptorExtensionVersionV2 = 0x02u;
constexpr std::size_t kMaxPublicRelationProjectionBytes = 512u * 1024u;
constexpr std::uint32_t kMaxPublicRelationProjectionColumns = 4096;
constexpr std::size_t kMaxPublicRelationMetadataTextBytes = 4096;
constexpr std::size_t kMaxPublicEncodedTypeDescriptorBytes = 65534;

bool EncodedDescriptorHasExactField(std::string_view descriptor,
                                    std::string_view key,
                                    std::string_view expected_value) {
  const std::string expected =
      std::string(key) + "=" + std::string(expected_value);
  const std::string prefix = std::string(key) + "=";
  bool matched = false;
  std::size_t offset = 0;
  while (offset <= descriptor.size()) {
    const auto delimiter = descriptor.find(';', offset);
    const auto field = descriptor.substr(
        offset,
        delimiter == std::string_view::npos
            ? descriptor.size() - offset
            : delimiter - offset);
    if (field == expected) {
      if (matched) return false;
      matched = true;
    } else if (field.starts_with(prefix)) {
      return false;
    }
    if (delimiter == std::string_view::npos) break;
    offset = delimiter + 1;
  }
  return matched;
}

using SbpsClientTraceClock = std::chrono::steady_clock;

std::uint64_t SbpsClientElapsedMicros(
    SbpsClientTraceClock::time_point begin,
    SbpsClientTraceClock::time_point end = SbpsClientTraceClock::now()) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
}

void WriteSbpsClientPhaseTrace(std::string_view endpoint_path,
                               std::uint16_t message_type,
                               std::uint32_t schema_id,
                               int attempt_index,
                               std::size_t request_payload_bytes,
                               std::size_t encoded_frame_count,
                               std::size_t encoded_frame_bytes,
                               std::size_t response_payload_bytes,
                               bool success,
                               std::uint64_t endpoint_us,
                               std::uint64_t lock_wait_us,
                               std::uint64_t connect_us,
                               std::uint64_t encode_us,
                               std::uint64_t write_us,
                               std::uint64_t read_response_us,
                               std::uint64_t attempt_us,
                               std::uint64_t total_us) {
  const char* path = std::getenv("SCRATCHBIRD_SBPS_CLIENT_PHASE_TRACE_FILE");
  if (path == nullptr || *path == '\0') return;
  static std::mutex trace_mutex;
  std::lock_guard<std::mutex> guard(trace_mutex);
  std::ofstream out(path, std::ios::app);
  if (!out) return;
  out << "endpoint=" << endpoint_path
      << '\t' << "message_type=" << message_type
      << '\t' << "schema_id=" << schema_id
      << '\t' << "attempt=" << attempt_index
      << '\t' << "request_payload_bytes=" << request_payload_bytes
      << '\t' << "encoded_frame_count=" << encoded_frame_count
      << '\t' << "encoded_frame_bytes=" << encoded_frame_bytes
      << '\t' << "response_payload_bytes=" << response_payload_bytes
      << '\t' << "success=" << (success ? "true" : "false")
      << '\t' << "endpoint_us=" << endpoint_us
      << '\t' << "lock_wait_us=" << lock_wait_us
      << '\t' << "connect_us=" << connect_us
      << '\t' << "encode_us=" << encode_us
      << '\t' << "write_us=" << write_us
      << '\t' << "read_response_us=" << read_response_us
      << '\t' << "attempt_us=" << attempt_us
      << '\t' << "total_us=" << total_us
      << '\n';
}

std::string NormalizeLanguageTag(std::string_view value) {
  return value.empty() ? "en" : std::string(value);
}

std::string TrimAsciiLocal(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string InputFallbackTagForTag(std::string_view value) {
  const std::string tag = NormalizeLanguageTag(value);
  return tag == "en" ? std::string{} : "en";
}

void ApplySbpsLanguageContext(ParserSessionContext* session,
                              const ParserClientConfig& config,
                              std::string_view requested_language_tag,
                              std::uint64_t language_resource_epoch,
                              std::uint64_t localized_name_epoch) {
  if (session == nullptr) return;
  session->default_language = "en";
  session->language_tag = NormalizeLanguageTag(requested_language_tag);
  session->language_profile = config.default_language_profile;
  session->input_syntax_profile = config.input_syntax_profile;
  session->input_language_fallback_tag =
      InputFallbackTagForTag(session->language_tag);
  session->common_resource_hash = config.common_resource_hash;
  session->resource_compatibility_identity =
      config.resource_compatibility_identity;
  session->resource_version_identity = config.resource_version_identity;
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

void PutBytes(std::vector<std::uint8_t>* out, const std::vector<std::uint8_t>& value) {
  PutU64(out, static_cast<std::uint64_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
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

bool ReadStringWithin(const std::vector<std::uint8_t>& data,
                      std::size_t* offset,
                      std::string* out,
                      std::size_t end,
                      std::size_t max_bytes) {
  if (offset == nullptr || out == nullptr || end > data.size() ||
      *offset > end || end - *offset < 2) {
    return false;
  }
  auto length = static_cast<std::uint64_t>(GetU16(data, *offset));
  *offset += 2;
  if (length == kLongStringSentinel) {
    if (*offset > end || end - *offset < 8) return false;
    length = GetU64(data, *offset);
    *offset += 8;
  }
  if (length > static_cast<std::uint64_t>(max_bytes) ||
      *offset > end ||
      length > static_cast<std::uint64_t>(end - *offset) ||
      length > static_cast<std::uint64_t>(
                   std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  out->assign(reinterpret_cast<const char*>(data.data() + *offset),
              static_cast<std::size_t>(length));
  *offset += static_cast<std::size_t>(length);
  return true;
}

bool ReadString(const std::vector<std::uint8_t>& data,
                std::size_t* offset,
                std::string* out) {
  return ReadStringWithin(data,
                          offset,
                          out,
                          data.size(),
                          std::numeric_limits<std::size_t>::max());
}

std::string UuidToText(const std::array<std::uint8_t, 16>& uuid);
bool UuidPresent(const std::array<std::uint8_t, 16>& uuid);
std::string OptionalUuidToText(const std::array<std::uint8_t, 16>& uuid);
void AddDiagnostic(
    MessageVectorSet* messages,
    std::string code,
    std::string message,
    std::string component = "parser_server_ipc.sbps_client",
    std::vector<Field> fields = {});
bool AppendTypedExecuteDiagnostics(
    const std::vector<std::uint8_t>& encoded_diagnostics,
    std::string_view expected_first_code,
    const std::array<std::uint8_t, 16>& expected_request_uuid,
    MessageVectorSet* messages);

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
  std::uint64_t affected_rows = 0;
  if (TextLineU64(payload, "server_affected_rows", &affected_rows)) {
    result->affected_rows = affected_rows;
    result->affected_rows_present = true;
  }
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

bool ReadTransactionSelector(const std::vector<std::uint8_t>& payload,
                             std::size_t* offset,
                             ParserTransactionSelector* selector) {
  if (offset == nullptr || selector == nullptr || *offset + 8 > payload.size()) {
    return false;
  }
  selector->local_transaction_id = GetU64(payload, *offset);
  *offset += 8;
  return ReadString(payload, offset, &selector->transaction_uuid);
}

bool DecodeExecuteResultPayloadV2(const Frame& response,
                                  ServerExecutionResult* result,
                                  MessageVectorSet* messages) {
  if (result == nullptr) return false;
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) ||
      offset + 16 + 16 + 8 > response.payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 execute result payload is malformed.");
    return false;
  }
  const auto result_request_uuid = GetUuid(response.payload, offset);
  if (!UuidPresent(result_request_uuid) ||
      (UuidPresent(response.header.request_uuid) &&
       response.header.request_uuid != result_request_uuid)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 execute result request identity is malformed or does not match its frame.");
    return false;
  }
  offset += 16;
  result->cursor_uuid = OptionalUuidToText(GetUuid(response.payload, offset));
  offset += 16;
  result->row_count = GetU64(response.payload, offset);
  offset += 8;
  std::string legacy_detail;
  if (!ReadString(response.payload, &offset, &result->operation_id) ||
      !ReadString(response.payload, &offset, &result->row_packet) ||
      !ReadString(response.payload, &offset, &legacy_detail) ||
      offset >= response.payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 execute result payload is malformed.");
    return false;
  }
  const std::uint8_t transaction_flags = response.payload[offset++];
  if ((transaction_flags & 0xe0u) != 0) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 transaction flags contain unknown bits.");
    return false;
  }
  result->selected_transaction_present = (transaction_flags & (1u << 0)) != 0;
  const bool encoded_finality_applied =
      (transaction_flags & (1u << 1)) != 0;
  result->finalized_transaction_present = (transaction_flags & (1u << 2)) != 0;
  result->replacement_transaction_present = (transaction_flags & (1u << 3)) != 0;
  result->catalog_invalidation_applied =
      (transaction_flags & (1u << 4)) != 0;
  if (offset >= response.payload.size() || response.payload[offset] > 3) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 transaction finality state is malformed.");
    return false;
  }
  result->finality_state =
      static_cast<ParserTransactionFinality>(response.payload[offset++]);
  const bool finality_is_applied =
      result->finality_state == ParserTransactionFinality::kKnownApplied;
  if (encoded_finality_applied != finality_is_applied) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 applied-finality flag contradicts the typed finality state.");
    return false;
  }
  result->finality_applied = finality_is_applied;
  if (offset >= response.payload.size() || response.payload[offset] > 3) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 transaction replacement reason is malformed.");
    return false;
  }
  result->replacement_reason =
      static_cast<ParserTransactionReplacementReason>(response.payload[offset++]);
  if (result->selected_transaction_present &&
      !ReadTransactionSelector(response.payload, &offset,
                               &result->selected_transaction)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 selected transaction selector is malformed.");
    return false;
  }
  if (result->finalized_transaction_present &&
      !ReadTransactionSelector(response.payload, &offset,
                               &result->finalized_transaction)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 finalized transaction selector is malformed.");
    return false;
  }
  if (result->replacement_transaction_present &&
      !ReadTransactionSelector(response.payload, &offset,
                               &result->replacement_transaction)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 replacement transaction selector is malformed.");
    return false;
  }
  if (!ReadString(response.payload, &offset,
                  &result->transaction_outcome_detail) ||
      !ReadString(response.payload, &offset,
                  &result->transaction_diagnostic_code)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 transaction outcome is malformed.");
    return false;
  }
  // Schema 4012 keeps the transaction outcome typed even when statement
  // execution is rejected.  Its optional trailer is one length-delimited
  // ordinary SBPS message-vector payload; legacy V2 payloads without a
  // trailer remain decodable during the in-tree protocol transition.
  std::vector<std::uint8_t> encoded_diagnostics;
  if (offset != response.payload.size()) {
    constexpr std::uint64_t kMaximumTypedDiagnosticBytes = 1024u * 1024u;
    if (offset + 8 > response.payload.size()) {
      AddDiagnostic(messages,
                    "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                    "The server V2 typed diagnostic trailer is malformed.");
      return false;
    }
    const std::uint64_t diagnostic_bytes = GetU64(response.payload, offset);
    offset += 8;
    if (diagnostic_bytes > kMaximumTypedDiagnosticBytes ||
        diagnostic_bytes >
            static_cast<std::uint64_t>(response.payload.size() - offset) ||
        offset + static_cast<std::size_t>(diagnostic_bytes) !=
            response.payload.size()) {
      AddDiagnostic(messages,
                    "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                    "The server V2 typed diagnostic trailer is malformed.");
      return false;
    }
    encoded_diagnostics.assign(
        response.payload.begin() + static_cast<std::ptrdiff_t>(offset),
        response.payload.end());
    offset = response.payload.size();
  }
  const auto selector_valid = [](bool present,
                                 const ParserTransactionSelector& selector) {
    return !present || selector.present();
  };
  const bool reason_present =
      result->replacement_reason !=
      ParserTransactionReplacementReason::kNone;
  const bool selected_matches_finalized =
      !result->selected_transaction_present ||
      !result->finalized_transaction_present ||
      (result->selected_transaction.local_transaction_id ==
           result->finalized_transaction.local_transaction_id &&
       result->selected_transaction.transaction_uuid ==
           result->finalized_transaction.transaction_uuid);
  const bool replacement_differs_from_finalized =
      !result->replacement_transaction_present ||
      !result->finalized_transaction_present ||
      result->replacement_transaction.local_transaction_id !=
          result->finalized_transaction.local_transaction_id ||
      result->replacement_transaction.transaction_uuid !=
          result->finalized_transaction.transaction_uuid;
  bool coherent =
      selector_valid(result->selected_transaction_present,
                     result->selected_transaction) &&
      selector_valid(result->finalized_transaction_present,
                     result->finalized_transaction) &&
      selector_valid(result->replacement_transaction_present,
                     result->replacement_transaction) &&
      (reason_present == result->replacement_transaction_present) &&
      selected_matches_finalized && replacement_differs_from_finalized &&
      result->replacement_reason !=
          ParserTransactionReplacementReason::kAutocommitReady &&
      (!result->catalog_invalidation_applied ||
       (result->finality_state == ParserTransactionFinality::kKnownApplied &&
        result->finalized_transaction_present &&
        result->operation_id == "transaction.commit"));
  switch (result->finality_state) {
    case ParserTransactionFinality::kNotApplicable:
      coherent = coherent && !result->finalized_transaction_present &&
                 !result->replacement_transaction_present;
      break;
    case ParserTransactionFinality::kKnownApplied:
      coherent = coherent && result->finalized_transaction_present &&
                 (!result->replacement_transaction_present || reason_present);
      break;
    case ParserTransactionFinality::kKnownNotApplied:
      coherent = coherent && !result->finalized_transaction_present &&
                 !result->replacement_transaction_present;
      break;
    case ParserTransactionFinality::kUnknown:
      coherent = coherent && !result->finalized_transaction_present &&
                 !result->replacement_transaction_present;
      break;
  }
  if (!coherent) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 transaction outcome contains contradictory selector, finality, or replacement state.");
    return false;
  }
  if (outcome == "accepted" &&
      (result->finality_state ==
           ParserTransactionFinality::kKnownNotApplied ||
       result->finality_state == ParserTransactionFinality::kUnknown)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "An accepted V2 outcome cannot report unknown or known-not-applied finality.");
    return false;
  }
  PopulateTransactionStateFromPayload(result->row_packet, result);
  // Free-form row payload is never transaction authority in V2.  Preserve its
  // affected-row projection above, then rebuild active transaction state only
  // from the validated typed selector matrix.
  result->transaction_state_present = false;
  result->local_transaction_id = 0;
  result->snapshot_visible_through_local_transaction_id = 0;
  result->transaction_uuid.clear();
  result->transaction_timestamp.clear();
  // V2 selectors intentionally remain in their exact typed fields.  They do
  // not carry an MGA snapshot, so projecting one into the legacy session
  // state would publish a zero snapshot and silently corrupt caller state.
  if (outcome != "accepted" && outcome != "rejected") {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server V2 execute outcome is not recognized.");
    return false;
  }
  result->accepted = outcome == "accepted";
  if (result->accepted && !encoded_diagnostics.empty()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "An accepted V2 outcome cannot carry rejection diagnostics.");
    return false;
  }
  if (!result->accepted) {
    if (!encoded_diagnostics.empty()) {
      if (!AppendTypedExecuteDiagnostics(encoded_diagnostics,
                                         result->transaction_diagnostic_code,
                                         result_request_uuid,
                                         messages)) {
        AddDiagnostic(messages,
                      "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                      "The server V2 typed diagnostic vector is malformed or contradicts its transaction diagnostic code.");
        return false;
      }
    } else {
      AddDiagnostic(messages,
                    !result->transaction_diagnostic_code.empty()
                        ? result->transaction_diagnostic_code
                        : (result->finality_applied
                               ? "PARSER_SERVER_IPC.FINALITY_APPLIED_REPLACEMENT_FAILED"
                               : "PARSER_SERVER_IPC.EXECUTE_REJECTED"),
                    result->transaction_outcome_detail.empty()
                        ? (legacy_detail.empty() ? "The server rejected V2 SBLR execution."
                                                 : legacy_detail)
                        : result->transaction_outcome_detail);
    }
  }
  return true;
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

std::string OptionalUuidToText(const std::array<std::uint8_t, 16>& uuid) {
  return UuidPresent(uuid) ? UuidToText(uuid) : std::string{};
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

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV1(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  std::vector<std::uint8_t> out;
  out.reserve(2 + 16 + 8 + 16);
  PutU16(&out, 1);
  PutUuid(&out, TextToUuid(session.session_uuid));
  PutU64(&out, transaction.local_transaction_id);
  PutUuid(&out, TextToUuid(transaction.transaction_uuid));
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV2(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 2;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV3(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 3;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV4(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 4;
  return out;
}

std::vector<std::uint8_t> EncodeAcquireStatementContextPayloadV5(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeAcquireStatementContextPayloadV1(session, transaction);
  out[0] = 5;
  return out;
}

bool DecodeAcquireStatementContextPayloadV1(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  constexpr std::size_t kResultBytes = 2 + 1 + (6 * 16) + (2 * 8);
  if (context == nullptr || payload.size() != kResultBytes ||
      GetU16(payload, 0) != 1 || payload[2] != 1) {
    return false;
  }
  std::size_t offset = 3;
  const auto statement_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto local_transaction_id = GetU64(payload, offset);
  offset += 8;
  const auto transaction_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto statement_snapshot_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto statement_metadata_snapshot_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto catalog_epoch_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto security_context_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto high_watermark = GetU64(payload, offset);
  if (!UuidPresent(statement_uuid) || local_transaction_id == 0 ||
      !UuidPresent(transaction_uuid) ||
      !UuidPresent(statement_snapshot_uuid) ||
      !UuidPresent(statement_metadata_snapshot_uuid) ||
      !UuidPresent(catalog_epoch_uuid) ||
      !UuidPresent(security_context_uuid)) {
    return false;
  }
  ParserStatementContext decoded;
  decoded.acquired = true;
  decoded.statement_uuid = UuidToText(statement_uuid);
  decoded.transaction.local_transaction_id = local_transaction_id;
  decoded.transaction.transaction_uuid = UuidToText(transaction_uuid);
  decoded.statement_snapshot_uuid = UuidToText(statement_snapshot_uuid);
  decoded.statement_metadata_snapshot_uuid =
      UuidToText(statement_metadata_snapshot_uuid);
  decoded.catalog_epoch_uuid = UuidToText(catalog_epoch_uuid);
  decoded.security_context_uuid = UuidToText(security_context_uuid);
  decoded.snapshot_visible_through_local_transaction_id = high_watermark;
  if (!decoded.complete()) return false;
  *context = std::move(decoded);
  return true;
}

bool DecodeAcquireStatementContextPayloadNative(
    const std::vector<std::uint8_t>& payload,
    const std::uint16_t expected_version,
    const bool extended_aggregate_registry,
    const bool complete_aggregate_registry,
    const std::uint8_t maximum_profile_kind,
    ParserStatementContext* context) {
  constexpr std::size_t kBaseBytes = 2 + 1 + (6 * 16) + (2 * 8);
  constexpr std::size_t kProfileBytes = 1 + 2 + (3 * 16) + 1 + (3 * 4);
  const std::size_t native_prefix_bytes =
      (extended_aggregate_registry ? 6U : 3U) * 16U + 2U;
  if (context == nullptr || payload.size() < kBaseBytes + native_prefix_bytes ||
      GetU16(payload, 0) != expected_version || payload[2] != 1) {
    return false;
  }

  std::vector<std::uint8_t> base(payload.begin(),
                                 payload.begin() + kBaseBytes);
  base[0] = 1;
  ParserStatementContext decoded;
  if (!DecodeAcquireStatementContextPayloadV1(base, &decoded)) return false;

  std::size_t offset = kBaseBytes;
  const auto bound_ast_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto count_function_uuid = GetUuid(payload, offset);
  offset += 16;
  const auto sum_function_uuid = GetUuid(payload, offset);
  offset += 16;
  std::array<std::uint8_t, 16> avg_function_uuid{};
  std::array<std::uint8_t, 16> min_function_uuid{};
  std::array<std::uint8_t, 16> max_function_uuid{};
  if (extended_aggregate_registry) {
    avg_function_uuid = GetUuid(payload, offset);
    offset += 16;
    min_function_uuid = GetUuid(payload, offset);
    offset += 16;
    max_function_uuid = GetUuid(payload, offset);
    offset += 16;
  }
  if (complete_aggregate_registry) {
    if (offset + 2 > payload.size()) return false;
    const auto aggregate_count = GetU16(payload, offset);
    offset += 2;
    if (aggregate_count != 43) return false;
    std::set<std::string> builtin_ids;
    std::set<std::string> function_uuids;
    decoded.aggregate_function_profiles.reserve(aggregate_count);
    for (std::uint16_t index = 0; index < aggregate_count; ++index) {
      if (offset + 2 > payload.size()) return false;
      ParserStatementContext::AggregateFunctionProfile profile;
      profile.abi_version = GetU16(payload, offset);
      offset += 2;
      if (!ReadString(payload, &offset, &profile.builtin_id) ||
          offset + 17 > payload.size()) {
        return false;
      }
      const auto function_uuid = GetUuid(payload, offset);
      offset += 16;
      const auto executable = payload[offset++];
      profile.function_uuid = UuidToText(function_uuid);
      profile.executable = executable == 1;
      if (profile.abi_version != 1 ||
          !profile.builtin_id.starts_with("sb.aggregate.") ||
          profile.builtin_id.size() <= std::string_view("sb.aggregate.").size() ||
          !UuidPresent(function_uuid) || executable != 1 ||
          !builtin_ids.insert(profile.builtin_id).second ||
          !function_uuids.insert(profile.function_uuid).second) {
        return false;
      }
      decoded.aggregate_function_profiles.push_back(std::move(profile));
    }
  }
  if (offset + 2 > payload.size()) return false;
  const auto profile_count = GetU16(payload, offset);
  offset += 2;
  if (!UuidPresent(bound_ast_uuid) || !UuidPresent(count_function_uuid) ||
      !UuidPresent(sum_function_uuid) ||
      (extended_aggregate_registry &&
       (!UuidPresent(avg_function_uuid) || !UuidPresent(min_function_uuid) ||
        !UuidPresent(max_function_uuid))) ||
      profile_count == 0 ||
      profile_count > static_cast<std::uint16_t>(maximum_profile_kind) * 32u ||
      payload.size() != offset +
                            static_cast<std::size_t>(profile_count) *
                                kProfileBytes) {
    return false;
  }

  std::array<std::uint16_t, 11> expected_slots{};
  std::set<std::string> descriptor_uuids;
  decoded.descriptor_profiles.reserve(profile_count);
  for (std::uint16_t index = 0; index < profile_count; ++index) {
    ParserStatementContext::DescriptorProfile profile;
    profile.profile_kind = payload[offset++];
    profile.slot = GetU16(payload, offset);
    offset += 2;
    const auto descriptor_uuid = GetUuid(payload, offset);
    offset += 16;
    const auto type_uuid = GetUuid(payload, offset);
    offset += 16;
    const auto collation_uuid = GetUuid(payload, offset);
    offset += 16;
    const auto nullable = payload[offset++];
    profile.width = GetU32(payload, offset);
    offset += 4;
    profile.precision = GetU32(payload, offset);
    offset += 4;
    profile.scale = GetU32(payload, offset);
    offset += 4;
    if (profile.profile_kind < 1 ||
        profile.profile_kind > maximum_profile_kind ||
        profile.slot != expected_slots[profile.profile_kind]++ ||
        !UuidPresent(descriptor_uuid) || !UuidPresent(type_uuid) ||
        nullable > 1 ||
        (profile.profile_kind % 2 == 0) != (nullable == 1) ||
        profile.scale > profile.precision) {
      return false;
    }
    profile.descriptor_uuid = UuidToText(descriptor_uuid);
    profile.type_uuid = UuidToText(type_uuid);
    profile.collation_uuid = OptionalUuidToText(collation_uuid);
    profile.nullable = nullable == 1;
    if (!descriptor_uuids.insert(profile.descriptor_uuid).second) return false;
    decoded.descriptor_profiles.push_back(std::move(profile));
  }
  if (offset != payload.size()) return false;
  for (std::size_t kind = 1; kind <= maximum_profile_kind; ++kind) {
    if (expected_slots[kind] == 0) return false;
  }
  decoded.bound_ast_uuid = UuidToText(bound_ast_uuid);
  decoded.count_function_uuid = UuidToText(count_function_uuid);
  decoded.sum_function_uuid = UuidToText(sum_function_uuid);
  if (extended_aggregate_registry) {
    decoded.avg_function_uuid = UuidToText(avg_function_uuid);
    decoded.min_function_uuid = UuidToText(min_function_uuid);
    decoded.max_function_uuid = UuidToText(max_function_uuid);
  }
  *context = std::move(decoded);
  return true;
}

bool DecodeAcquireStatementContextPayloadV2(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 2, false,
                                                     false, 6,
                                                     context);
}

bool DecodeAcquireStatementContextPayloadV3(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 3, true,
                                                     false, 6,
                                                     context);
}

bool DecodeAcquireStatementContextPayloadV4(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 4, true, true,
                                                     6,
                                                     context);
}

bool DecodeAcquireStatementContextPayloadV5(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadNative(payload, 5, true, true,
                                                     10,
                                                     context);
}

bool IsCanonicalNonzeroUuidText(std::string_view text) {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') {
    return false;
  }
  const auto parsed = TextToUuid(text);
  if (!UuidPresent(parsed)) return false;
  std::string normalized(text);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return UuidToText(parsed) == normalized;
}

bool IsErrorFrame(const Frame& frame) {
  return frame.header.message_type == kMessageDiagnostic || (frame.header.flags & kFlagError) != 0;
}

void AddDiagnostic(MessageVectorSet* messages,
                   std::string code,
                   std::string message,
                   std::string component,
                   std::vector<Field> fields) {
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

std::optional<std::vector<DecodedMessageVector>> DecodeMessageVectors(
    const std::vector<std::uint8_t>& payload,
    const std::array<std::uint8_t, 16>& expected_request_uuid) {
  constexpr std::size_t kHeaderBytes = 64;
  constexpr std::size_t kRecordHeaderBytes = 112;
  constexpr std::size_t kMaximumSetBytes = 1024u * 1024u;
  constexpr std::size_t kMaximumRecordBytes = 256u * 1024u;
  std::vector<DecodedMessageVector> vectors;
  if (payload.size() < kHeaderBytes || payload.size() > kMaximumSetBytes ||
      GetU32(payload, 0) != kMessageVectorMagic ||
      GetU16(payload, 4) != kHeaderBytes || GetU16(payload, 6) != 1 ||
      (GetU32(payload, 8) & 0xfffffff0u) != 0 ||
      GetU32(payload, 16) != payload.size()) {
    return std::nullopt;
  }
  const auto vector_count = GetU32(payload, 12);
  if (vector_count > 1024u) return std::nullopt;
  for (std::size_t index = 56; index < kHeaderBytes; ++index) {
    if (payload[index] != 0) return std::nullopt;
  }
  auto header = std::vector<std::uint8_t>(
      payload.begin(),
      payload.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes));
  const auto expected_header_crc = GetU32(header, 52);
  for (std::size_t index = 52; index < 56; ++index) header[index] = 0;
  if (Crc32c(header.data(), header.size()) != expected_header_crc) {
    return std::nullopt;
  }
  const auto records_crc = GetU32(payload, 20);
  if (vector_count == 0) {
    if (records_crc != 0 || payload.size() != kHeaderBytes) {
      return std::nullopt;
    }
  } else {
    if (records_crc == 0 ||
        Crc32c(payload.data() + kHeaderBytes,
               payload.size() - kHeaderBytes) != records_crc) {
      return std::nullopt;
    }
  }

  std::size_t offset = kHeaderBytes;
  for (std::uint32_t index = 0; index < vector_count; ++index) {
    if (offset + kRecordHeaderBytes > payload.size()) return std::nullopt;
    const auto record_start = offset;
    const auto record_bytes = GetU32(payload, record_start);
    if (record_bytes < kRecordHeaderBytes ||
        record_bytes > kMaximumRecordBytes ||
        record_bytes > payload.size() - record_start) {
      return std::nullopt;
    }
    const auto record_end = record_start + record_bytes;
    auto record = std::vector<std::uint8_t>(
        payload.begin() + static_cast<std::ptrdiff_t>(record_start),
        payload.begin() + static_cast<std::ptrdiff_t>(record_end));
    const auto expected_record_crc = GetU32(record, 4);
    for (std::size_t byte = 4; byte < 8; ++byte) record[byte] = 0;
    if (Crc32c(record.data(), record.size()) != expected_record_crc ||
        GetU16(payload, record_start + 8) != 1 ||
        payload[record_start + 10] > 7 ||
        payload[record_start + 11] > 6 ||
        GetU32(payload, record_start + 12) != 0 ||
        !std::equal(expected_request_uuid.begin(),
                    expected_request_uuid.end(),
                    payload.begin() +
                        static_cast<std::ptrdiff_t>(record_start + 48)) ||
        payload[record_start + 108] > 2 ||
        payload[record_start + 109] > 3 ||
        GetU16(payload, record_start + 110) != 0) {
      return std::nullopt;
    }
    const auto language_len = GetU16(payload, offset + 92);
    const auto code_len = GetU16(payload, offset + 94);
    const auto message_key_len = GetU16(payload, offset + 96);
    const auto admin_detail_key_len = GetU16(payload, offset + 98);
    const auto safe_message_len = GetU16(payload, offset + 100);
    const auto field_count = GetU16(payload, offset + 102);
    const auto detail_count = GetU16(payload, offset + 104);
    const auto cause_count = GetU16(payload, offset + 106);
    if (field_count > 64 || detail_count > 64 || cause_count > 16) {
      return std::nullopt;
    }

    std::size_t cursor = record_start + kRecordHeaderBytes;
    auto read_padded_string = [&](std::uint16_t length,
                                  std::string* value) {
      if (value == nullptr || length > record_end - cursor) return false;
      value->assign(reinterpret_cast<const char*>(payload.data() + cursor),
                    length);
      cursor += length;
      while ((cursor % 4u) != 0u) {
        if (cursor >= record_end || payload[cursor++] != 0) return false;
      }
      return true;
    };
    std::string language;
    std::string message_key;
    std::string admin_detail_key;
    DecodedMessageVector vector;
    if (!read_padded_string(language_len, &language) ||
        !read_padded_string(code_len, &vector.code) ||
        !read_padded_string(message_key_len, &message_key) ||
        !read_padded_string(admin_detail_key_len, &admin_detail_key) ||
        !read_padded_string(safe_message_len, &vector.message)) {
      return std::nullopt;
    }

    auto read_tlv = [&](Field* field) {
      if (field == nullptr || cursor + 8 > record_end) return false;
      const auto key_len = GetU16(payload, cursor);
      const auto type_code = GetU16(payload, cursor + 2);
      const auto value_len = GetU32(payload, cursor + 4);
      cursor += 8;
      if (key_len == 0 || type_code != 1 ||
          key_len > record_end - cursor ||
          value_len > record_end - cursor - key_len) {
        return false;
      }
      field->name.assign(
          reinterpret_cast<const char*>(payload.data() + cursor), key_len);
      cursor += key_len;
      field->value.assign(
          reinterpret_cast<const char*>(payload.data() + cursor), value_len);
      cursor += value_len;
      while ((cursor % 4u) != 0u) {
        if (cursor >= record_end || payload[cursor++] != 0) return false;
      }
      return IsPublicDiagnosticFieldAllowed(field->name, field->value);
    };
    for (std::uint16_t field_index = 0; field_index < field_count;
         ++field_index) {
      Field field;
      if (!read_tlv(&field)) return std::nullopt;
      vector.fields.push_back(std::move(field));
    }
    for (std::uint32_t ignored = 0;
         ignored < static_cast<std::uint32_t>(detail_count) + cause_count;
         ++ignored) {
      Field field;
      if (!read_tlv(&field)) return std::nullopt;
    }
    if (cursor != record_end) return std::nullopt;
    vectors.push_back(std::move(vector));
    offset = record_end;
  }
  if (offset != payload.size() || vectors.size() != vector_count) {
    return std::nullopt;
  }
  return vectors;
}

bool AppendTypedExecuteDiagnostics(
    const std::vector<std::uint8_t>& encoded_diagnostics,
    std::string_view expected_first_code,
    const std::array<std::uint8_t, 16>& expected_request_uuid,
    MessageVectorSet* messages) {
  auto decoded = DecodeMessageVectors(encoded_diagnostics,
                                      expected_request_uuid);
  if (!decoded || decoded->empty() ||
      (!expected_first_code.empty() &&
       decoded->front().code != expected_first_code)) {
    return false;
  }
  for (auto& vector : *decoded) {
    AddDiagnostic(messages,
                  std::move(vector.code),
                  vector.message.empty()
                      ? "The server returned a typed execution diagnostic."
                      : std::move(vector.message),
                  "parser_server_ipc.sbps_client",
                  std::move(vector.fields));
  }
  return true;
}

void AddFrameDiagnostics(const Frame& frame, MessageVectorSet* messages) {
  auto decoded = DecodeMessageVectors(frame.payload,
                                      frame.header.request_uuid);
  if (!decoded || decoded->empty()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.REQUEST_FAILED", "The parser-server IPC request failed.");
    return;
  }
  for (auto& vector : *decoded) {
    AddDiagnostic(messages,
                  std::move(vector.code),
                  vector.message.empty() ? "The server returned a message vector for this request."
                                         : std::move(vector.message),
                  "parser_server_ipc.sbps_client",
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
#ifdef MSG_NOSIGNAL
  return static_cast<int>(::send(fd, data, chunk, MSG_NOSIGNAL));
#else
  return static_cast<int>(::send(fd, data, chunk, 0));
#endif
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

std::mutex& CachedSbpsSocketMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, SbpsSocketHandle>& CachedSbpsSockets() {
  static std::map<std::string, SbpsSocketHandle> sockets;
  return sockets;
}

void AppendDiagnostics(MessageVectorSet* target, const MessageVectorSet& source) {
  if (target == nullptr || source.diagnostics.empty()) return;
  target->diagnostics.insert(target->diagnostics.end(),
                             source.diagnostics.begin(),
                             source.diagnostics.end());
}

std::string JoinStable(const std::vector<std::string>& values) {
  std::vector<std::string> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  std::string out;
  for (const auto& value : sorted) {
    if (!out.empty()) out.push_back(',');
    out += value;
  }
  return out;
}

bool ExecutionInvalidatesPublicResolutionCache(std::string_view operation_id) {
  return operation_id.rfind("ddl.", 0) == 0 ||
         operation_id.rfind("catalog.", 0) == 0 ||
         operation_id.rfind("security.", 0) == 0 ||
         operation_id.rfind("language.", 0) == 0 ||
         operation_id.rfind("policy.", 0) == 0 ||
         operation_id.rfind("auth.", 0) == 0;
}

struct SbpsClientPublicResolutionCacheRecord {
  std::string object_uuid;
  std::string canonical_name;
  std::string object_class;
  std::uint64_t catalog_epoch{0};
  std::uint64_t security_epoch{0};
};

bool IsPublicResourceObjectClass(std::string_view object_class) {
  std::string normalized(object_class);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return normalized == "charset" || normalized == "collation";
}

std::mutex& SbpsClientPublicResolutionCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, SbpsClientPublicResolutionCacheRecord>&
SbpsClientPublicResolutionCache() {
  static std::map<std::string, SbpsClientPublicResolutionCacheRecord> cache;
  return cache;
}

std::deque<std::string>& SbpsClientPublicResolutionLru() {
  static std::deque<std::string> lru;
  return lru;
}

std::string SbpsClientPublicResolutionScopeKey(std::string_view endpoint,
                                               const ParserSessionContext& session) {
  std::ostringstream key;
  key << "endpoint=" << endpoint
      << "|session=" << session.session_uuid
      << "|connection=" << session.connection_uuid
      << "|database=" << session.database_uuid
      << "|user=" << session.authenticated_user_uuid
      << "|principal=" << session.principal_claim
      << "|auth_provider=" << session.auth_provider_family
      << "|catalog=" << session.catalog_epoch
      << "|security=" << session.security_policy_epoch
      << "|grant=" << session.grant_epoch
      << "|descriptor=" << session.descriptor_epoch
      << "|localized_name=" << session.localized_name_epoch
      << "|language_resource=" << session.language_resource_epoch
      << "|message_resource=" << session.message_resource_epoch
      << "|roles=" << JoinStable(session.effective_role_uuids)
      << "|groups=" << JoinStable(session.effective_group_uuids)
      << "|search_path=" << JoinStable(session.search_path)
      << "|default_language=" << session.default_language
      << "|language_profile=" << session.language_profile
      << "|language_tag=" << session.language_tag
      << "|input_syntax=" << session.input_syntax_profile
      << "|input_fallback=" << session.input_language_fallback_tag
      << "|common_resource=" << session.common_resource_hash
      << "|dialect_profile=" << session.dialect_profile_uuid
      << "|policy_profile=" << session.policy_profile_uuid
      << "|resource_compat=" << session.resource_compatibility_identity
      << "|resource_version=" << session.resource_version_identity;
  return key.str();
}

std::string SbpsClientResolveNameCacheKey(std::string_view endpoint,
                                          const ParserSessionContext& session,
                                          std::string_view presented_name,
                                          bool quoted,
                                          std::string_view object_class,
                                          const ParserClientConfig& config) {
  std::ostringstream key;
  key << SbpsClientPublicResolutionScopeKey(endpoint, session)
      << "|kind=resolve_name"
      << "|name=" << presented_name
      << "|quoted=" << (quoted ? "1" : "0")
      << "|object_class=" << object_class
      << "|parser_profile=" << config.profile_id
      << "|parser_dialect=" << config.dialect
      << "|registry=" << config.registry_version;
  return key.str();
}

std::string SbpsClientRenderUuidCacheKey(std::string_view endpoint,
                                         const ParserSessionContext& session,
                                         std::string_view object_uuid) {
  std::ostringstream key;
  key << SbpsClientPublicResolutionScopeKey(endpoint, session)
      << "|kind=render_uuid"
      << "|object_uuid=" << object_uuid;
  return key.str();
}

PublicNameResolutionResult PublicResolutionResultFromCache(
    const SbpsClientPublicResolutionCacheRecord& cached) {
  PublicNameResolutionResult result;
  result.resolved = true;
  result.object_uuid = cached.object_uuid;
  result.canonical_name = cached.canonical_name;
  result.object_class = cached.object_class;
  result.catalog_epoch = cached.catalog_epoch;
  result.security_epoch = cached.security_epoch;
  return result;
}

std::optional<SbpsClientPublicResolutionCacheRecord>
LookupSbpsClientPublicResolutionCache(const std::string& cache_key) {
  std::lock_guard<std::mutex> guard(SbpsClientPublicResolutionCacheMutex());
  const auto found = SbpsClientPublicResolutionCache().find(cache_key);
  if (found == SbpsClientPublicResolutionCache().end()) return std::nullopt;
  return found->second;
}

void StoreSbpsClientPublicResolutionCacheEntry(
    const std::string& cache_key,
    const PublicNameResolutionResult& result) {
  if (cache_key.empty() || !result.resolved || result.object_uuid.empty() ||
      IsPublicResourceObjectClass(result.object_class)) {
    return;
  }
  std::lock_guard<std::mutex> guard(SbpsClientPublicResolutionCacheMutex());
  auto& cache = SbpsClientPublicResolutionCache();
  auto& lru = SbpsClientPublicResolutionLru();
  cache[cache_key] = SbpsClientPublicResolutionCacheRecord{
      result.object_uuid,
      result.canonical_name,
      result.object_class,
      result.catalog_epoch,
      result.security_epoch,
  };
  lru.erase(std::remove(lru.begin(), lru.end(), cache_key), lru.end());
  lru.push_back(cache_key);
  while (cache.size() > kMaxSbpsClientPublicResolutionCacheEntries && !lru.empty()) {
    cache.erase(lru.front());
    lru.pop_front();
  }
}

void ClearSbpsClientPublicResolutionCacheForSession(std::string_view endpoint,
                                                    const ParserSessionContext& session) {
  const std::string scope = SbpsClientPublicResolutionScopeKey(endpoint, session);
  std::lock_guard<std::mutex> guard(SbpsClientPublicResolutionCacheMutex());
  auto& cache = SbpsClientPublicResolutionCache();
  auto& lru = SbpsClientPublicResolutionLru();
  for (auto it = cache.begin(); it != cache.end();) {
    if (it->first.rfind(scope, 0) == 0) {
      it = cache.erase(it);
    } else {
      ++it;
    }
  }
  lru.erase(std::remove_if(lru.begin(), lru.end(), [&](const std::string& key) {
              return key.rfind(scope, 0) == 0;
            }),
            lru.end());
}

void CloseCachedSbpsSocket(std::string_view cache_key) {
  auto& sockets = CachedSbpsSockets();
  const auto found = sockets.find(std::string(cache_key));
  if (found == sockets.end()) return;
  CloseSbpsSocket(found->second);
  sockets.erase(found);
}

SbpsSocketHandle ConnectCachedSbpsSocket(std::string_view path,
                                         std::string_view cache_key,
                                         bool allow_fresh_connection,
                                         MessageVectorSet* messages,
                                         std::uint32_t timeout_ms) {
  auto& sockets = CachedSbpsSockets();
  const auto key = std::string(cache_key);
  if (const auto found = sockets.find(key);
      found != sockets.end() && found->second != kInvalidSbpsSocket) {
    return found->second;
  }
  // Session UUIDs are meaningful only on the physical route that negotiated
  // and authenticated them.  A caller with a session-bound frame must never
  // turn a missing cached route into a raw, unnegotiated replacement socket.
  if (!allow_fresh_connection) return kInvalidSbpsSocket;
#ifdef _WIN32
  if (!EnsureWinsockInitialized()) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.SOCKET_CREATE_FAILED", "Winsock initialization failed for the SBPS client.");
    return kInvalidSbpsSocket;
  }
#endif
  const SbpsSocketHandle fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == kInvalidSbpsSocket) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.SOCKET_CREATE_FAILED", "The parser could not create an SBPS socket.");
    return kInvalidSbpsSocket;
  }
  (void)SetSocketTimeouts(fd, timeout_ms);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    CloseSbpsSocket(fd);
    AddDiagnostic(messages, "PARSER_SERVER_IPC.ENDPOINT_PATH_TOO_LONG", "The parser-server IPC endpoint path is too long.");
    return kInvalidSbpsSocket;
  }
  std::memcpy(addr.sun_path, path.data(), path.size());
  addr.sun_path[path.size()] = '\0';
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseSbpsSocket(fd);
    AddDiagnostic(messages, "PARSER_SERVER_IPC.CONNECT_FAILED", "The parser could not connect to sb_server.");
    return kInvalidSbpsSocket;
  }
  sockets[key] = fd;
  return fd;
}

bool ReadExpectedResponse(SbpsSocketHandle fd,
                          const std::array<std::uint8_t, 16>& request_uuid,
                          Frame* response,
                          MessageVectorSet* messages) {
  constexpr int kMaxStaleFrames = 64;
  for (int index = 0; index < kMaxStaleFrames; ++index) {
    Frame candidate;
    if (!ReadPhysicalFrame(fd, &candidate, messages)) return false;
    if (!AssembleChunkedFrame(fd, &candidate, messages)) return false;
    if (candidate.header.request_uuid == request_uuid) {
      *response = std::move(candidate);
      return true;
    }
  }
  AddDiagnostic(messages,
                "PARSER_SERVER_IPC.STALE_RESPONSE_LIMIT",
                "The parser-server IPC response stream did not produce the requested response before the stale-frame limit.");
  return false;
}

bool V2RequestIsNonReplayableAfterWrite(std::uint32_t schema_id) {
  return schema_id == kSchemaPrepareSblrV2 ||
         schema_id == kSchemaExecuteSblrV2 ||
         schema_id == kSchemaExecuteCanonicalSblrV1 ||
         schema_id == kSchemaResolveNameRequestV2 ||
         schema_id == kSchemaResolveNameRequestV3;
}

bool SessionBoundRequest(const FrameHeader& header) {
  return UuidPresent(header.session_uuid);
}

bool RequestMayRetryAfterTransportLoss(const FrameHeader& header) {
  return !SessionBoundRequest(header) &&
         !V2RequestIsNonReplayableAfterWrite(header.schema_id);
}

void AddTransportOutcomeUnknown(MessageVectorSet* messages,
                                const FrameHeader& header,
                                std::string phase) {
  AddDiagnostic(
      messages,
      "PARSER_SERVER_IPC.OUTCOME_UNKNOWN",
      "A request may have reached the server; its physical route is unusable and the request was not replayed.",
      "parser_server_ipc.sbps_client",
      {{"schema_id", std::to_string(header.schema_id)},
       {"transport_phase", std::move(phase)},
       {"session_bound", SessionBoundRequest(header) ? "true" : "false"},
       {"request_replayed", "false"},
       {"route_fatal", "true"},
       {"caller_cleanup_required", "true"}});
}

void AddSessionRouteUnavailable(MessageVectorSet* messages,
                                const FrameHeader& header) {
  AddDiagnostic(
      messages,
      "PARSER_SERVER_IPC.SESSION_ROUTE_UNAVAILABLE",
      "The session-bound request was not sent because its negotiated physical route is unavailable.",
      "parser_server_ipc.sbps_client",
      {{"schema_id", std::to_string(header.schema_id)},
       {"transport_phase", "before_write"},
       {"session_bound", "true"},
       {"request_written", "false"},
       {"request_replayed", "false"},
       {"route_fatal", "true"},
       {"caller_cleanup_required", "true"}});
}

bool HasDiagnosticCode(const MessageVectorSet& messages,
                       std::string_view code) {
  return std::any_of(messages.diagnostics.begin(),
                     messages.diagnostics.end(),
                     [&](const Diagnostic& diagnostic) {
                       return diagnostic.code == code;
                     });
}

template <typename CloseResult>
void ProjectCloseTransportFailure(const MessageVectorSet& messages,
                                  CloseResult* result) {
  if (result == nullptr) return;
  const bool outcome_unknown =
      HasDiagnosticCode(messages, "PARSER_SERVER_IPC.OUTCOME_UNKNOWN");
  const bool route_unavailable = HasDiagnosticCode(
      messages, "PARSER_SERVER_IPC.SESSION_ROUTE_UNAVAILABLE");
  if (!outcome_unknown && !route_unavailable) return;
  result->accepted = false;
  result->outcome_unknown = outcome_unknown;
  result->caller_cleanup_required = true;
  result->route_fatal = true;
  result->detail = outcome_unknown ? "transport_outcome_unknown"
                                   : "physical_session_route_unavailable";
}

void ProjectV2PrepareOutcomeUnknown(MessageVectorSet* messages,
                                    ServerPrepareSblrResult* result,
                                    std::string phase) {
  if (result == nullptr) return;
  if (messages != nullptr &&
      !HasDiagnosticCode(*messages, "PARSER_SERVER_IPC.OUTCOME_UNKNOWN")) {
    AddDiagnostic(
        messages,
        "PARSER_SERVER_IPC.OUTCOME_UNKNOWN",
        "A transaction-routed prepare may have created an engine-owned prepared object; the request was not replayed.",
        "parser_server_ipc.sbps_client",
        {{"transport_phase", phase},
         {"request_replayed", "false"},
         {"caller_cleanup_required", "true"}});
  }
  result->accepted = false;
  result->outcome_unknown = true;
  result->caller_cleanup_required = true;
  result->prepared_statement_uuid.clear();
  result->operation_id.clear();
  result->detail = std::move(phase);
}

void ProjectV2PrepareTransportOutcomeUnknown(
    const MessageVectorSet& messages,
    ServerPrepareSblrResult* result) {
  if (HasDiagnosticCode(messages, "PARSER_SERVER_IPC.OUTCOME_UNKNOWN")) {
    ProjectV2PrepareOutcomeUnknown(
        nullptr, result, "transport_outcome_unknown");
  }
}

bool DecodePrepareResultPayloadV2(
    const std::vector<std::uint8_t>& payload,
    ServerPrepareSblrResult* result,
    MessageVectorSet* messages) {
  if (result == nullptr) return false;
  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(payload, &offset, &outcome)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID",
                  "The server V2 prepare result payload is malformed.");
    ProjectV2PrepareOutcomeUnknown(
        messages, result, "malformed_outcome");
    return false;
  }
  if (outcome != "accepted" && outcome != "rejected") {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID",
                  "The server V2 prepare outcome is not recognized.");
    ProjectV2PrepareOutcomeUnknown(
        messages, result, "malformed_outcome_value");
    return false;
  }
  if (offset + 16 > payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID",
                  "The server V2 prepare result payload is malformed.");
    if (outcome == "accepted") {
      ProjectV2PrepareOutcomeUnknown(
          messages, result, "malformed_success_identity");
    }
    return false;
  }
  const auto prepared_uuid = GetUuid(payload, offset);
  offset += 16;
  if (!ReadString(payload, &offset, &result->operation_id) ||
      !ReadString(payload, &offset, &result->detail) ||
      offset != payload.size()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID",
                  "The server V2 prepare result payload is malformed.");
    if (outcome == "accepted") {
      ProjectV2PrepareOutcomeUnknown(
          messages, result, "malformed_success_payload");
    }
    return false;
  }
  if (outcome == "rejected") {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_REJECTED",
                  "The server rejected transaction-routed SBLR prepare.");
    return false;
  }
  if (std::all_of(prepared_uuid.begin(), prepared_uuid.end(),
                  [](std::uint8_t byte) { return byte == 0; }) ||
      result->operation_id.empty()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_INVALID",
                  "The server accepted V2 prepare without a usable prepared identity or operation identity.");
    ProjectV2PrepareOutcomeUnknown(
        messages, result, "malformed_success_identity");
    return false;
  }
  result->prepared_statement_uuid = UuidToText(prepared_uuid);
  result->accepted = true;
  return true;
}

void ProjectV2TransportOutcomeUnknown(const MessageVectorSet& messages,
                                      ServerExecutionResult* result) {
  if (result == nullptr ||
      !HasDiagnosticCode(messages, "PARSER_SERVER_IPC.OUTCOME_UNKNOWN")) {
    return;
  }
  result->finality_state = ParserTransactionFinality::kUnknown;
  result->finality_applied = false;
  result->transaction_state_present = false;
  result->local_transaction_id = 0;
  result->snapshot_visible_through_local_transaction_id = 0;
  result->transaction_uuid.clear();
  result->transaction_timestamp.clear();
  result->transaction_outcome_detail = "transport_outcome_unknown";
}

void ProjectV2ResponseOutcomeUnknown(MessageVectorSet* messages,
                                     ServerExecutionResult* result,
                                     std::string phase) {
  if (messages != nullptr &&
      !HasDiagnosticCode(*messages,
                         "PARSER_SERVER_IPC.OUTCOME_UNKNOWN")) {
    AddDiagnostic(
        messages,
        "PARSER_SERVER_IPC.OUTCOME_UNKNOWN",
        "A written transaction-routed request returned a response whose finality could not be authoritatively decoded.",
        "parser_server_ipc.sbps_client",
        {{"transport_phase", std::move(phase)},
         {"request_replayed", "false"},
         {"caller_cleanup_required", "true"}});
  }
  if (messages != nullptr) {
    ProjectV2TransportOutcomeUnknown(*messages, result);
  }
  if (result != nullptr) {
    result->transaction_outcome_detail =
        "response_finality_outcome_unknown";
  }
}

bool SendRequest(const std::string& endpoint,
                 const FrameHeader& header,
                 const std::vector<std::uint8_t>& payload,
                 Frame* response,
                 MessageVectorSet* messages,
                 std::string_view requested_socket_cache_key = {},
                 std::uint32_t timeout_ms = kDefaultSbpsRequestTimeoutMs) {
  const auto total_begin = SbpsClientTraceClock::now();
  const auto endpoint_begin = total_begin;
  const auto path = EndpointPath(endpoint);
  const std::string socket_cache_key = requested_socket_cache_key.empty()
                                           ? path
                                           : std::string(requested_socket_cache_key);
  const auto endpoint_us = SbpsClientElapsedMicros(endpoint_begin);
  if (!ValidateEndpointPath(path, messages)) return false;
  const auto lock_begin = SbpsClientTraceClock::now();
  std::unique_lock<std::mutex> lock(CachedSbpsSocketMutex());
  const auto lock_wait_us = SbpsClientElapsedMicros(lock_begin);
  const bool session_bound = SessionBoundRequest(header);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto attempt_begin = SbpsClientTraceClock::now();
    MessageVectorSet attempt_messages;
    const auto connect_begin = SbpsClientTraceClock::now();
    const SbpsSocketHandle fd = ConnectCachedSbpsSocket(
        path, socket_cache_key, !session_bound, &attempt_messages, timeout_ms);
    const auto connect_us = SbpsClientElapsedMicros(connect_begin);
    if (fd == kInvalidSbpsSocket) {
      if (session_bound) {
        AddSessionRouteUnavailable(&attempt_messages, header);
      }
      WriteSbpsClientPhaseTrace(path,
                                header.message_type,
                                header.schema_id,
                                attempt,
                                payload.size(),
                                0,
                                0,
                                0,
                                false,
                                endpoint_us,
                                lock_wait_us,
                                connect_us,
                                0,
                                0,
                                0,
                                SbpsClientElapsedMicros(attempt_begin),
                                SbpsClientElapsedMicros(total_begin));
      AppendDiagnostics(messages, attempt_messages);
      return false;
    }
    const auto encode_begin = SbpsClientTraceClock::now();
    const auto encoded_frames = EncodeFrameSequence(header, payload);
    const auto encode_us = SbpsClientElapsedMicros(encode_begin);
    std::size_t encoded_frame_bytes = 0;
    for (const auto& encoded : encoded_frames) encoded_frame_bytes += encoded.size();
    bool wrote_all = true;
    const auto write_begin = SbpsClientTraceClock::now();
    for (const auto& encoded : encoded_frames) {
      if (!WriteAll(fd, encoded)) {
        wrote_all = false;
        break;
      }
    }
    const auto write_us = SbpsClientElapsedMicros(write_begin);
    if (!wrote_all) {
      CloseCachedSbpsSocket(socket_cache_key);
      if (!RequestMayRetryAfterTransportLoss(header)) {
        WriteSbpsClientPhaseTrace(path,
                                  header.message_type,
                                  header.schema_id,
                                  attempt,
                                  payload.size(),
                                  encoded_frames.size(),
                                  encoded_frame_bytes,
                                  0,
                                  false,
                                  endpoint_us,
                                  lock_wait_us,
                                  connect_us,
                                  encode_us,
                                  write_us,
                                  0,
                                  SbpsClientElapsedMicros(attempt_begin),
                                  SbpsClientElapsedMicros(total_begin));
        AppendDiagnostics(messages, attempt_messages);
        AddTransportOutcomeUnknown(
            messages, header, "write_attempt_failed");
        return false;
      }
      if (attempt == 0) continue;
      WriteSbpsClientPhaseTrace(path,
                                header.message_type,
                                header.schema_id,
                                attempt,
                                payload.size(),
                                encoded_frames.size(),
                                encoded_frame_bytes,
                                0,
                                false,
                                endpoint_us,
                                lock_wait_us,
                                connect_us,
                                encode_us,
                                write_us,
                                0,
                                SbpsClientElapsedMicros(attempt_begin),
                                SbpsClientElapsedMicros(total_begin));
      AddDiagnostic(messages, "PARSER_SERVER_IPC.WRITE_FAILED", "The parser could not write to sb_server.");
      return false;
    }
    const auto read_begin = SbpsClientTraceClock::now();
    if (ReadExpectedResponse(fd, header.request_uuid, response, &attempt_messages)) {
      const auto read_us = SbpsClientElapsedMicros(read_begin);
      if (header.message_type == kMessageDisconnectNotice) {
        CloseCachedSbpsSocket(socket_cache_key);
      }
      WriteSbpsClientPhaseTrace(path,
                                header.message_type,
                                header.schema_id,
                                attempt,
                                payload.size(),
                                encoded_frames.size(),
                                encoded_frame_bytes,
                                response == nullptr ? 0 : response->payload.size(),
                                true,
                                endpoint_us,
                                lock_wait_us,
                                connect_us,
                                encode_us,
                                write_us,
                                read_us,
                                SbpsClientElapsedMicros(attempt_begin),
                                SbpsClientElapsedMicros(total_begin));
      return true;
    }
    const auto read_us = SbpsClientElapsedMicros(read_begin);
    CloseCachedSbpsSocket(socket_cache_key);
    if (!RequestMayRetryAfterTransportLoss(header)) {
      WriteSbpsClientPhaseTrace(path,
                                header.message_type,
                                header.schema_id,
                                attempt,
                                payload.size(),
                                encoded_frames.size(),
                                encoded_frame_bytes,
                                response == nullptr ? 0 : response->payload.size(),
                                false,
                                endpoint_us,
                                lock_wait_us,
                                connect_us,
                                encode_us,
                                write_us,
                                read_us,
                                SbpsClientElapsedMicros(attempt_begin),
                                SbpsClientElapsedMicros(total_begin));
      AppendDiagnostics(messages, attempt_messages);
      AddTransportOutcomeUnknown(
          messages, header, "response_unavailable_after_write");
      return false;
    }
    if (attempt == 0) continue;
    WriteSbpsClientPhaseTrace(path,
                              header.message_type,
                              header.schema_id,
                              attempt,
                              payload.size(),
                              encoded_frames.size(),
                              encoded_frame_bytes,
                              response == nullptr ? 0 : response->payload.size(),
                              false,
                              endpoint_us,
                              lock_wait_us,
                              connect_us,
                              encode_us,
                              write_us,
                              read_us,
                              SbpsClientElapsedMicros(attempt_begin),
                              SbpsClientElapsedMicros(total_begin));
    AppendDiagnostics(messages, attempt_messages);
    return false;
  }
  WriteSbpsClientPhaseTrace(path,
                            header.message_type,
                            header.schema_id,
                            2,
                            payload.size(),
                            0,
                            0,
                            0,
                            false,
                            endpoint_us,
                            lock_wait_us,
                            0,
                            0,
                            0,
                            0,
                            0,
                            SbpsClientElapsedMicros(total_begin));
  AddDiagnostic(messages, "PARSER_SERVER_IPC.REQUEST_FAILED", "The parser-server IPC request failed.");
  return false;
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

std::vector<std::uint8_t> EncodeBuiltInHelloPayload(
    bool require_transaction_routing_v2 = false,
    bool require_prepared_metadata_transfer_v1 = false,
    bool require_relation_descriptor_projection_v3 = false) {
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
  capabilities[0] = kCapabilityBaseline;
  if (require_transaction_routing_v2 ||
      require_prepared_metadata_transfer_v1 ||
      require_relation_descriptor_projection_v3) {
    capabilities[0] |= kCapabilityTransactionRoutingV2;
  }
  if (require_prepared_metadata_transfer_v1) {
    capabilities[0] |= kCapabilityPreparedMetadataTransferV1;
  }
  if (require_relation_descriptor_projection_v3) {
    capabilities[0] |= kCapabilityRelationDescriptorProjectionV3;
  }
  PutBytes32(&out, capabilities);
  return out;
}

AuthCredentialEnvelope CredentialsFromTestWirePayload(std::string_view auth_payload) {
  const auto text = TrimAsciiLocal(auth_payload);
  AuthCredentialEnvelope credentials;
  const auto split = text.find_first_of(" \t\r\n");
  credentials.principal = std::string(split == std::string_view::npos ? text : text.substr(0, split));
  credentials.credential_evidence = split == std::string_view::npos
      ? std::string{}
      : TrimAsciiLocal(text.substr(split + 1));
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
                                               bool cursor_requested,
                                               const std::vector<std::uint8_t>& data_packet = {}) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, {});
  PutU8(&out, cursor_requested ? 1 : 0);
  PutString(&out, encoded_sblr_envelope);
  if (!data_packet.empty()) {
    PutBytes(&out, data_packet);
  }
  return out;
}

void PutTransactionRouting(std::vector<std::uint8_t>* out,
                           const ParserTransactionRouting& transaction) {
  PutU8(out, static_cast<std::uint8_t>(transaction.route));
  PutU64(out, transaction.selector.local_transaction_id);
  PutString(out, transaction.selector.transaction_uuid);
}

void PutTransactionSelector(std::vector<std::uint8_t>* out,
                            const ParserTransactionSelector& transaction) {
  PutU64(out, transaction.local_transaction_id);
  PutString(out, transaction.transaction_uuid);
}

bool ValidateTransactionRouting(const ParserTransactionRouting& transaction,
                                MessageVectorSet* messages) {
  const bool selector_present = transaction.selector.present();
  const bool selector_partially_present =
      transaction.selector.local_transaction_id != 0 ||
      !transaction.selector.transaction_uuid.empty();
  switch (transaction.route) {
    case ParserTransactionRoute::kLegacyDefault:
      if (!selector_partially_present) return true;
      break;
    case ParserTransactionRoute::kSelected:
      if (selector_present) return true;
      break;
    case ParserTransactionRoute::kBeginAdditional:
      if (!selector_partially_present) return true;
      break;
  }
  AddDiagnostic(messages,
                "PARSER_SERVER_IPC.TRANSACTION_ROUTING_INVALID",
                "The transaction route and engine-issued selector are inconsistent.");
  return false;
}

std::vector<std::uint8_t> EncodeExecutePayloadV2(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid,
    std::string_view encoded_sblr_envelope,
    bool cursor_requested,
    const std::vector<std::uint8_t>& data_packet,
    const ParserTransactionRouting& transaction) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, prepared_statement_uuid);
  PutU8(&out, cursor_requested ? 1 : 0);
  PutTransactionRouting(&out, transaction);
  PutString(&out, encoded_sblr_envelope);
  PutBytes(&out, data_packet);
  return out;
}

std::vector<std::uint8_t> EncodeCanonicalExecutePayloadV1(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, TextToUuid(session.session_uuid));
  PutUuid(&out, {});  // Prepared reuse is outside the Packet 7 live route.
  PutU8(&out, cursor_requested ? 1 : 0);
  PutU8(&out, static_cast<std::uint8_t>(ParserTransactionRoute::kSelected));
  PutU64(&out, statement_context.transaction.local_transaction_id);
  PutUuid(&out, TextToUuid(statement_context.transaction.transaction_uuid));
  PutUuid(&out, TextToUuid(statement_context.statement_uuid));
  PutBytes(&out, submission.canonical_container_bytes);
  PutBytes(&out, submission.canonical_execution_envelope_bytes);
  PutBytes(&out, data_packet);
  return out;
}

std::vector<std::uint8_t> EncodePreparePayload(const ParserSessionContext& session,
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

std::vector<std::uint8_t> EncodePreparePayloadV2(
    const ParserSessionContext& session,
    const std::array<std::uint8_t, 16>& session_uuid,
    std::string_view encoded_sblr_envelope,
    const ParserTransactionSelector& transaction) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, MakeUuidV7Bytes());
  PutU64(&out, session.catalog_epoch);
  PutU64(&out, session.security_policy_epoch);
  PutU64(&out, session.security_policy_epoch);
  PutTransactionSelector(&out, transaction);
  PutString(&out, encoded_sblr_envelope);
  return out;
}

std::vector<std::uint8_t> EncodeExecutePreparedPayload(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid,
    std::string_view encoded_sblr_envelope,
    bool cursor_requested,
    const std::vector<std::uint8_t>& data_packet = {}) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, prepared_statement_uuid);
  PutU8(&out, cursor_requested ? 1 : 0);
  PutString(&out, encoded_sblr_envelope);
  if (!data_packet.empty()) {
    PutBytes(&out, data_packet);
  }
  return out;
}

std::vector<std::uint8_t> EncodeClosePreparedSblrPayload(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, session_uuid);
  PutUuid(&out, prepared_statement_uuid);
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

std::string JoinSearchPath(const ParserSessionContext& session) {
  std::string out;
  for (const auto& item : session.search_path) {
    if (!out.empty()) out.push_back(',');
    out += item;
  }
  return out;
}

std::vector<std::uint8_t> EncodeResolveNamePayload(const ParserSessionContext& session,
                                                   std::string_view presented_name,
                                                   bool quoted,
                                                   std::string_view object_class,
                                                   const ParserClientConfig& config,
                                                   bool bypass_cache = false) {
  std::vector<std::uint8_t> out;
  PutString(&out, presented_name);
  PutU8(&out, quoted ? 1 : 0);
  const std::string identifier_profile =
      session.dialect_profile_uuid.empty() ? config.dialect_profile_uuid
                                           : session.dialect_profile_uuid;
  PutString(&out, identifier_profile);
  PutString(&out, session.default_language.empty() ? "en" : session.default_language);
  PutString(&out, JoinSearchPath(session));
  PutString(&out, object_class);
  PutU8(&out, bypass_cache ? 1 : 0);
  return out;
}

std::vector<std::uint8_t> EncodeResolveNamePayloadV2(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction) {
  auto out = EncodeResolveNamePayload(session,
                                      presented_name,
                                      quoted,
                                      object_class,
                                      config,
                                      true);
  PutUuid(&out, TextToUuid(session.session_uuid));
  PutTransactionSelector(&out, transaction);
  return out;
}

std::vector<std::uint8_t> EncodeResolveNamePayloadV3(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction,
    std::uint8_t projection_flags) {
  auto out = EncodeResolveNamePayloadV2(session,
                                        presented_name,
                                        quoted,
                                        object_class,
                                        config,
                                        transaction);
  PutU8(&out, projection_flags);
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
        "parser_server_ipc.sbps_client"));
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
        "parser_server_ipc.sbps_client"));
    return result;
  }
  result.catalog_epoch = GetU64(response.payload, offset);
  offset += 8;
  result.security_epoch = GetU64(response.payload, offset);
  offset += 8;
  std::string detail;
  if (!ReadString(response.payload, &offset, &detail)) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The public name/UUID response payload is malformed.",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  if (outcome != success_outcome) {
    if (offset != response.payload.size()) {
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
          "ERROR",
          "The failed public name/UUID response has trailing bytes.",
          "parser_server_ipc.sbps_client"));
      return result;
    }
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_NOT_FOUND_OR_NOT_VISIBLE",
        "ERROR",
        "object name could not be resolved or is not visible",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  result.object_uuid = UuidToText(object_uuid);
  result.canonical_name = canonical_name;
  result.object_class = object_class;
  result.resolution_detail = detail;
  const bool resource_result = IsPublicResourceObjectClass(object_class);
  if (offset < response.payload.size()) {
    constexpr std::uint8_t kResourceDescriptorExtensionV1 = 1;
    const std::uint8_t extension_version = response.payload[offset++];
    auto& descriptor = result.resource_descriptor;
    if (extension_version != kResourceDescriptorExtensionV1 ||
        !ReadString(response.payload, &offset, &descriptor.resource_family) ||
        !ReadString(response.payload, &offset, &descriptor.canonical_name) ||
        !ReadString(response.payload, &offset, &descriptor.parent_resource_uuid) ||
        !ReadString(response.payload, &offset, &descriptor.parent_canonical_name) ||
        !ReadString(response.payload, &offset, &descriptor.default_collation_uuid) ||
        !ReadString(response.payload, &offset, &descriptor.default_collation_name) ||
        offset + 16 > response.payload.size()) {
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.RESOURCE_DESCRIPTOR_INVALID",
          "ERROR",
          "The engine resource descriptor extension is malformed.",
          "parser_server_ipc.sbps_client"));
      return result;
    }
    descriptor.resource_epoch = GetU64(response.payload, offset);
    offset += 8;
    descriptor.family_epoch = GetU64(response.payload, offset);
    offset += 8;
    if (!ReadString(response.payload, &offset, &descriptor.family_version) ||
        offset + 9 > response.payload.size()) {
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.RESOURCE_DESCRIPTOR_INVALID",
          "ERROR",
          "The engine resource descriptor extension is malformed.",
          "parser_server_ipc.sbps_client"));
      return result;
    }
    descriptor.min_bytes = GetU32(response.payload, offset);
    offset += 4;
    descriptor.max_bytes = GetU32(response.payload, offset);
    offset += 4;
    const std::uint8_t attributes = response.payload[offset++];
    descriptor.variable_width = (attributes & 0x01u) != 0;
    descriptor.default_for_parent = (attributes & 0x02u) != 0;
    descriptor.case_insensitive = (attributes & 0x04u) != 0;
    descriptor.accent_insensitive = (attributes & 0x08u) != 0;
    descriptor.present = true;
    const bool descriptor_valid =
        resource_result && offset == response.payload.size() &&
        IsPublicResourceObjectClass(descriptor.resource_family) &&
        descriptor.resource_family == object_class &&
        descriptor.canonical_name == canonical_name &&
        descriptor.resource_epoch != 0 && descriptor.family_epoch != 0 &&
        !descriptor.family_version.empty() &&
        (object_class != "charset" ||
         (descriptor.min_bytes != 0 &&
          descriptor.max_bytes >= descriptor.min_bytes)) &&
        (object_class != "collation" ||
         !descriptor.parent_resource_uuid.empty());
    if (!descriptor_valid) {
      descriptor.present = false;
      result.messages.diagnostics.push_back(MakeDiagnostic(
          "PARSER_SERVER_IPC.RESOURCE_DESCRIPTOR_INVALID",
          "ERROR",
          "The engine resource descriptor extension failed validation.",
          "parser_server_ipc.sbps_client"));
      return result;
    }
  } else if (resource_result) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.RESOURCE_DESCRIPTOR_REQUIRED",
        "ERROR",
        "The server did not return required engine-owned resource metadata.",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  if (offset != response.payload.size()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.NAME_RESULT_INVALID",
        "ERROR",
        "The public name/UUID response payload has trailing bytes.",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  result.resolved = true;
  return result;
}

PublicNameResolutionResult DecodePublicNameResultPayloadV3(
    const Frame& response,
    std::string_view success_outcome,
    bool require_relation_descriptor) {
  PublicNameResolutionResult result;
  auto invalid = [&](std::string code, std::string message) {
    result.resolved = false;
    result.object_uuid.clear();
    result.canonical_name.clear();
    result.object_class.clear();
    result.resolution_detail.clear();
    result.catalog_epoch = 0;
    result.security_epoch = 0;
    result.resource_descriptor = {};
    result.relation_descriptor = {};
    AddDiagnostic(&result.messages, std::move(code), std::move(message));
  };
  std::size_t offset = 0;
  auto read_base_string = [&](std::string* value,
                              std::size_t max_bytes) {
    return ReadStringWithin(response.payload,
                            &offset,
                            value,
                            response.payload.size(),
                            max_bytes);
  };
  std::string outcome;
  if (!read_base_string(&outcome, 64) ||
      offset + 16 > response.payload.size()) {
    invalid("PARSER_SERVER_IPC.NAME_RESULT_INVALID",
            "The V3 public name response payload is malformed.");
    return result;
  }
  const auto object_uuid = GetUuid(response.payload, offset);
  offset += 16;
  std::string canonical_name;
  std::string object_class;
  if (!read_base_string(&canonical_name, kMaxPublicRelationMetadataTextBytes) ||
      !read_base_string(&object_class, kMaxPublicRelationMetadataTextBytes) ||
      offset + 16 > response.payload.size()) {
    invalid("PARSER_SERVER_IPC.NAME_RESULT_INVALID",
            "The V3 public name response payload is malformed.");
    return result;
  }
  result.catalog_epoch = GetU64(response.payload, offset);
  offset += 8;
  result.security_epoch = GetU64(response.payload, offset);
  offset += 8;
  std::string detail;
  if (!read_base_string(&detail, kMaxPublicRelationMetadataTextBytes)) {
    invalid("PARSER_SERVER_IPC.NAME_RESULT_INVALID",
            "The V3 public name response payload is malformed.");
    return result;
  }
  if (outcome != success_outcome) {
    if (offset >= response.payload.size() ||
        response.payload.size() - offset != 1 ||
        response.payload[offset++] != 0) {
      invalid("PARSER_SERVER_IPC.NAME_RESULT_INVALID",
              "The failed V3 public name response has an invalid extension envelope.");
      return result;
    }
    invalid("PARSER_SERVER_IPC.NAME_NOT_FOUND_OR_NOT_VISIBLE",
            "object name could not be resolved or is not visible");
    return result;
  }
  if (!UuidPresent(object_uuid) || canonical_name.empty() ||
      object_class.empty()) {
    invalid("PARSER_SERVER_IPC.NAME_RESULT_INVALID",
            "The V3 public name response has an incomplete object identity.");
    return result;
  }
  result.object_uuid = UuidToText(object_uuid);
  result.canonical_name = std::move(canonical_name);
  result.object_class = std::move(object_class);
  result.resolution_detail = std::move(detail);

  if (offset >= response.payload.size()) {
    invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUIRED",
            "The V3 public name response omitted its extension envelope.");
    return result;
  }
  const std::uint8_t extension_count = response.payload[offset++];
  if (extension_count > 1) {
    invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
            "The V3 relation descriptor extension count is invalid.");
    return result;
  }
  for (std::uint8_t extension_index = 0;
       extension_index < extension_count;
       ++extension_index) {
    if (offset + 6 > response.payload.size()) {
      invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor extension header is truncated.");
      return result;
    }
    const std::uint8_t extension_kind = response.payload[offset++];
    const std::uint8_t extension_version = response.payload[offset++];
    const std::uint32_t extension_bytes = GetU32(response.payload, offset);
    offset += 4;
    if (extension_kind != kRelationDescriptorExtensionKind ||
        (extension_version != kRelationDescriptorExtensionVersion &&
         extension_version != kRelationDescriptorExtensionVersionV2) ||
        extension_bytes > kMaxPublicRelationProjectionBytes ||
        extension_bytes > response.payload.size() - offset) {
      invalid(extension_bytes > kMaxPublicRelationProjectionBytes
                  ? "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_TOO_LARGE"
                  : "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor extension header is invalid.");
      return result;
    }
    const std::size_t extension_end = offset + extension_bytes;
    auto read_bounded_string = [&](std::string* value,
                                   std::size_t max_bytes) {
      return ReadStringWithin(response.payload,
                              &offset,
                              value,
                              extension_end,
                              max_bytes);
    };
    auto& descriptor = result.relation_descriptor;
    if (offset + (extension_version == kRelationDescriptorExtensionVersionV2
                      ? 64
                      : 48) > extension_end) {
      invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor identity is truncated.");
      return result;
    }
    const auto descriptor_uuid = GetUuid(response.payload, offset);
    offset += 16;
    const auto relation_uuid = GetUuid(response.payload, offset);
    offset += 16;
    std::array<std::uint8_t, 16> schema_uuid{};
    if (extension_version == kRelationDescriptorExtensionVersionV2) {
      schema_uuid = GetUuid(response.payload, offset);
      offset += 16;
    }
    descriptor.descriptor_generation = GetU64(response.payload, offset);
    offset += 8;
    descriptor.validated_resource_epoch = GetU64(response.payload, offset);
    offset += 8;
    if (offset + 4 > extension_end) {
      invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor column count is truncated.");
      return result;
    }
    const std::uint32_t column_count = GetU32(response.payload, offset);
    offset += 4;
    if (!UuidPresent(descriptor_uuid) || !UuidPresent(relation_uuid) ||
        (extension_version == kRelationDescriptorExtensionVersionV2 &&
         !UuidPresent(schema_uuid)) ||
        descriptor.descriptor_generation == 0 ||
        descriptor.validated_resource_epoch == 0 || column_count == 0 ||
        column_count > kMaxPublicRelationProjectionColumns) {
      invalid(column_count > kMaxPublicRelationProjectionColumns
                  ? "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_TOO_LARGE"
                  : "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor identity or column count is invalid.");
      return result;
    }
    descriptor.descriptor_uuid = UuidToText(descriptor_uuid);
    descriptor.relation_uuid = UuidToText(relation_uuid);
    descriptor.schema_uuid = OptionalUuidToText(schema_uuid);
    if (descriptor.relation_uuid != result.object_uuid) {
      invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RELATION_MISMATCH",
              "The projected descriptor does not identify the resolved relation.");
      return result;
    }
    std::set<std::string> column_uuids;
    std::set<std::uint32_t> ordinals;
    descriptor.columns.reserve(column_count);
    for (std::uint32_t column_index = 0; column_index < column_count;
         ++column_index) {
      if (offset + 20 > extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column identity is truncated.");
        return result;
      }
      PublicRelationColumnDescriptor column;
      const auto column_uuid = GetUuid(response.payload, offset);
      offset += 16;
      column.ordinal = GetU32(response.payload, offset);
      offset += 4;
      if (!read_bounded_string(&column.canonical_name_key,
                               kMaxPublicRelationMetadataTextBytes) ||
          offset + 16 > extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column name or type identity is malformed.");
        return result;
      }
      const auto type_descriptor_uuid = GetUuid(response.payload, offset);
      offset += 16;
      if (!read_bounded_string(&column.type_descriptor_kind,
                               kMaxPublicRelationMetadataTextBytes) ||
          !read_bounded_string(&column.canonical_type_name,
                               kMaxPublicRelationMetadataTextBytes) ||
          !read_bounded_string(&column.encoded_type_descriptor,
                               kMaxPublicEncodedTypeDescriptorBytes) ||
          offset >= extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column type descriptor is malformed.");
        return result;
      }
      const std::uint8_t attributes = response.payload[offset++];
      if ((attributes & 0xf0u) != 0 || offset + 16 > extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column attribute set is invalid.");
        return result;
      }
      column.nullable = (attributes & 0x01u) != 0;
      column.generated = (attributes & 0x02u) != 0;
      column.identity_column = (attributes & 0x04u) != 0;
      column.charset_variable_width = (attributes & 0x08u) != 0;
      const auto charset_uuid = GetUuid(response.payload, offset);
      offset += 16;
      if (!read_bounded_string(&column.charset_canonical_name,
                               kMaxPublicRelationMetadataTextBytes) ||
          offset + 16 > extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column charset descriptor is malformed.");
        return result;
      }
      const auto collation_uuid = GetUuid(response.payload, offset);
      offset += 16;
      if (!read_bounded_string(&column.collation_canonical_name,
                               kMaxPublicRelationMetadataTextBytes) ||
          offset + 12 > extension_end) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column collation descriptor is malformed.");
        return result;
      }
      column.character_length = GetU32(response.payload, offset);
      offset += 4;
      column.charset_min_bytes = GetU32(response.payload, offset);
      offset += 4;
      column.charset_max_bytes = GetU32(response.payload, offset);
      offset += 4;

      if (!UuidPresent(column_uuid) || !UuidPresent(type_descriptor_uuid) ||
          column.canonical_name_key.empty() ||
          column.type_descriptor_kind.empty() ||
          column.canonical_type_name.empty() ||
          column.encoded_type_descriptor.empty()) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "A V3 relation column has incomplete canonical metadata.");
        return result;
      }
      column.column_uuid = UuidToText(column_uuid);
      column.type_descriptor_uuid = UuidToText(type_descriptor_uuid);
      if (!column_uuids.insert(column.column_uuid).second ||
          !ordinals.insert(column.ordinal).second) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
                "The V3 relation descriptor repeats a column identity or ordinal.");
        return result;
      }
      const bool has_charset = UuidPresent(charset_uuid);
      const bool has_collation = UuidPresent(collation_uuid);
      const bool text_large_object = EncodedDescriptorHasExactField(
          column.encoded_type_descriptor,
          "text_resource_storage",
          "large_object");
      if (has_charset) column.charset_uuid = UuidToText(charset_uuid);
      if (has_collation) column.collation_uuid = UuidToText(collation_uuid);
      const bool resource_shape_valid =
          (!has_collation || has_charset) &&
          (has_charset
               ? (!column.charset_canonical_name.empty() &&
                  (text_large_object ? column.character_length == 0
                                     : column.character_length != 0) &&
                  column.charset_min_bytes != 0 &&
                  column.charset_max_bytes >= column.charset_min_bytes)
               : (column.charset_canonical_name.empty() &&
                  column.collation_canonical_name.empty() &&
                  column.character_length == 0 &&
                  column.charset_min_bytes == 0 &&
                  column.charset_max_bytes == 0 &&
                  !column.charset_variable_width)) &&
          (!has_collation || !column.collation_canonical_name.empty());
      if (!resource_shape_valid) {
        invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_RESOURCE_MISMATCH",
                "A V3 relation column has inconsistent canonical resource metadata.");
        return result;
      }
      descriptor.columns.push_back(std::move(column));
    }
    if (offset != extension_end) {
      invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
              "The V3 relation descriptor extension has trailing bytes.");
      return result;
    }
    descriptor.present = true;
  }
  if (offset != response.payload.size()) {
    invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_INVALID",
            "The V3 relation descriptor envelope has trailing bytes.");
    return result;
  }
  if (require_relation_descriptor && !result.relation_descriptor.present) {
    invalid("PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUIRED",
            "The server did not return the requested persisted relation descriptor.");
    return result;
  }
  result.resolved = true;
  return result;
}

bool RequireTransactionRoutingV2(const ParserSessionContext& session,
                                 MessageVectorSet* messages) {
  if (session.transaction_routing_v2_negotiated) return true;
  AddDiagnostic(messages,
                "PARSER_SERVER_IPC.TRANSACTION_ROUTING_V2_NOT_NEGOTIATED",
                "Independent transaction routing was not negotiated during hello.");
  return false;
}

bool RequireRelationDescriptorProjectionV3(
    const ParserSessionContext& session,
    MessageVectorSet* messages) {
  if (session.relation_descriptor_projection_v3_negotiated) return true;
  AddDiagnostic(
      messages,
      "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_V3_NOT_NEGOTIATED",
      "Persisted relation projection was not negotiated during hello.");
  return false;
}

} // namespace

bool DecodeDiagnosticFrameForTest(
    const std::vector<std::uint8_t>& encoded_frame,
    MessageVectorSet* messages) {
  if (messages == nullptr) return false;
  Frame frame;
  if (!DecodeFrame(encoded_frame, &frame, messages) || !IsErrorFrame(frame)) {
    return false;
  }
  const std::size_t diagnostic_count = messages->diagnostics.size();
  AddFrameDiagnostics(frame, messages);
  return messages->diagnostics.size() > diagnostic_count;
}

bool DecodeAcquireStatementContextResultPayloadV1ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadV1(payload, context);
}

bool DecodeAcquireStatementContextResultPayloadV4ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadV4(payload, context);
}

bool DecodeAcquireStatementContextResultPayloadV5ForTest(
    const std::vector<std::uint8_t>& payload,
    ParserStatementContext* context) {
  return DecodeAcquireStatementContextPayloadV5(payload, context);
}

std::vector<std::uint8_t>
EncodeAcquireStatementContextRequestPayloadV1ForTest(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) {
  return EncodeAcquireStatementContextPayloadV1(session, transaction);
}

std::vector<std::uint8_t> EncodeCanonicalExecutePayloadV1ForTest(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) {
  return EncodeCanonicalExecutePayloadV1(session,
                                         statement_context,
                                         submission,
                                         data_packet,
                                         cursor_requested);
}

std::vector<std::uint8_t> EncodeResolveNameRequestPayloadV2ForTest(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction) {
  return EncodeResolveNamePayloadV2(session,
                                    presented_name,
                                    quoted,
                                    object_class,
                                    config,
                                    transaction);
}

std::vector<std::uint8_t> EncodeResolveNameRequestPayloadV3ForTest(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction,
    std::uint8_t projection_flags) {
  return EncodeResolveNamePayloadV3(session,
                                    presented_name,
                                    quoted,
                                    object_class,
                                    config,
                                    transaction,
                                    projection_flags);
}

bool DecodeResolveNameResultPayloadV3ForTest(
    const std::vector<std::uint8_t>& payload,
    bool require_relation_descriptor,
    PublicNameResolutionResult* result) {
  if (result == nullptr) return false;
  Frame frame;
  frame.payload = payload;
  *result = DecodePublicNameResultPayloadV3(
      frame, "resolved", require_relation_descriptor);
  return result->resolved && result->messages.diagnostics.empty();
}

struct SbpsClientChannelState {
  bool dedicated_v2_channel_enabled{false};
  std::string dedicated_v2_socket_cache_key;
  std::vector<std::uint8_t> stable_baseline_hello_payload;
  std::vector<std::uint8_t> stable_v2_hello_payload;
  std::vector<std::uint8_t> stable_prepared_metadata_transfer_v1_hello_payload;
  std::vector<std::uint8_t> stable_relation_descriptor_v3_hello_payload;
  std::vector<std::uint8_t>
      stable_prepared_metadata_transfer_relation_descriptor_v3_hello_payload;
};

namespace {

void ReleaseDedicatedV2Channel(SbpsClientChannelState* state) {
  if (state == nullptr || !state->dedicated_v2_channel_enabled ||
      state->dedicated_v2_socket_cache_key.empty()) {
    return;
  }
  std::lock_guard<std::mutex> guard(CachedSbpsSocketMutex());
  CloseCachedSbpsSocket(state->dedicated_v2_socket_cache_key);
  state->dedicated_v2_channel_enabled = false;
}

}  // namespace

bool DecodeExecuteResultPayloadV2ForTest(
    const std::vector<std::uint8_t>& payload,
    ServerExecutionResult* result,
    MessageVectorSet* messages) {
  Frame frame;
  frame.payload = payload;
  return DecodeExecuteResultPayloadV2(frame, result, messages);
}

bool DecodePrepareResultPayloadV2ForTest(
    const std::vector<std::uint8_t>& payload,
    ServerPrepareSblrResult* result,
    MessageVectorSet* messages) {
  return DecodePrepareResultPayloadV2(payload, result, messages);
}

bool V2RequestMayRetryAfterWriteForTest(std::uint32_t schema_id) {
  return !V2RequestIsNonReplayableAfterWrite(schema_id);
}

bool SessionBoundRequestMayRetryAfterWriteForTest(std::uint32_t schema_id) {
  FrameHeader header;
  header.schema_id = schema_id;
  header.session_uuid.front() = 1;
  return RequestMayRetryAfterTransportLoss(header);
}

SbpsClient::SbpsClient(std::string endpoint)
    : endpoint_(std::move(endpoint)),
      channel_state_(std::make_unique<SbpsClientChannelState>()) {
  channel_state_->dedicated_v2_socket_cache_key =
      endpoint_ + "|sbps-v2-client|" + UuidToText(MakeUuidV7Bytes());
  channel_state_->stable_baseline_hello_payload =
      EncodeBuiltInHelloPayload();
  channel_state_->stable_v2_hello_payload = EncodeBuiltInHelloPayload(true);
  channel_state_->stable_prepared_metadata_transfer_v1_hello_payload =
      EncodeBuiltInHelloPayload(true, true);
  channel_state_->stable_relation_descriptor_v3_hello_payload =
      EncodeBuiltInHelloPayload(true, false, true);
  channel_state_
      ->stable_prepared_metadata_transfer_relation_descriptor_v3_hello_payload =
      EncodeBuiltInHelloPayload(true, true, true);
}

SbpsClient::~SbpsClient() {
  ReleaseDedicatedV2Channel(channel_state_.get());
}

SbpsClient::SbpsClient(SbpsClient&& other) noexcept = default;

SbpsClient& SbpsClient::operator=(SbpsClient&& other) noexcept {
  if (this == &other) return *this;
  ReleaseDedicatedV2Channel(channel_state_.get());
  endpoint_ = std::move(other.endpoint_);
  channel_state_ = std::move(other.channel_state_);
  return *this;
}

void SbpsClient::EnableDedicatedV2Channel() const {
  if (channel_state_ != nullptr) {
    channel_state_->dedicated_v2_channel_enabled = true;
  }
}

const std::string& SbpsClient::ActiveSocketCacheKey() const {
  static const std::string empty;
  if (channel_state_ == nullptr ||
      !channel_state_->dedicated_v2_channel_enabled) {
    return empty;
  }
  return channel_state_->dedicated_v2_socket_cache_key;
}

const std::vector<std::uint8_t>& SbpsClient::StableV2HelloPayload() const {
  static const std::vector<std::uint8_t> empty;
  return channel_state_ == nullptr ? empty
                                   : channel_state_->stable_v2_hello_payload;
}

const std::vector<std::uint8_t>&
SbpsClient::StablePreparedMetadataTransferV1HelloPayload() const {
  static const std::vector<std::uint8_t> empty;
  return channel_state_ == nullptr
             ? empty
             : channel_state_
                   ->stable_prepared_metadata_transfer_v1_hello_payload;
}

const std::vector<std::uint8_t>&
SbpsClient::StableRelationDescriptorV3HelloPayload() const {
  static const std::vector<std::uint8_t> empty;
  return channel_state_ == nullptr
             ? empty
             : channel_state_->stable_relation_descriptor_v3_hello_payload;
}

const std::vector<std::uint8_t>&
SbpsClient::StablePreparedMetadataTransferRelationDescriptorV3HelloPayload()
    const {
  static const std::vector<std::uint8_t> empty;
  return channel_state_ == nullptr
             ? empty
             : channel_state_
                   ->stable_prepared_metadata_transfer_relation_descriptor_v3_hello_payload;
}

std::string SbpsClient::V2ChannelCacheKeyForTest() const {
  return channel_state_ == nullptr
             ? std::string{}
             : channel_state_->dedicated_v2_socket_cache_key;
}

std::vector<std::uint8_t> SbpsClient::V2HelloPayloadForTest() const {
  return StableV2HelloPayload();
}

std::vector<std::uint8_t>
SbpsClient::PreparedMetadataTransferV1HelloPayloadForTest() const {
  return StablePreparedMetadataTransferV1HelloPayload();
}

std::vector<std::uint8_t>
SbpsClient::RelationDescriptorV3HelloPayloadForTest() const {
  return StableRelationDescriptorV3HelloPayload();
}

bool SbpsClient::UsesDedicatedV2ChannelForTest() const {
  return channel_state_ != nullptr &&
         channel_state_->dedicated_v2_channel_enabled;
}

bool SbpsClient::SendHello(MessageVectorSet* messages) const {
  return SendHelloWithRequirements(false, nullptr, false, nullptr, false,
                                   nullptr, messages);
}

bool SbpsClient::SendHelloWithRequirements(
    bool require_transaction_routing_v2,
    bool* transaction_routing_v2_accepted,
    bool require_prepared_metadata_transfer_v1,
    bool* prepared_metadata_transfer_v1_accepted,
    bool require_relation_descriptor_projection_v3,
    bool* relation_descriptor_projection_v3_accepted,
    MessageVectorSet* messages) const {
  if (transaction_routing_v2_accepted != nullptr) {
    *transaction_routing_v2_accepted = false;
  }
  if (prepared_metadata_transfer_v1_accepted != nullptr) {
    *prepared_metadata_transfer_v1_accepted = false;
  }
  if (relation_descriptor_projection_v3_accepted != nullptr) {
    *relation_descriptor_projection_v3_accepted = false;
  }
  if (require_transaction_routing_v2 ||
      require_prepared_metadata_transfer_v1 ||
      require_relation_descriptor_projection_v3) {
    EnableDedicatedV2Channel();
  }
  const std::vector<std::uint8_t>* hello_payload = nullptr;
  if (require_prepared_metadata_transfer_v1 &&
      require_relation_descriptor_projection_v3) {
    hello_payload =
        &StablePreparedMetadataTransferRelationDescriptorV3HelloPayload();
  } else if (require_prepared_metadata_transfer_v1) {
    hello_payload = &StablePreparedMetadataTransferV1HelloPayload();
  } else if (require_relation_descriptor_projection_v3) {
    hello_payload = &StableRelationDescriptorV3HelloPayload();
  } else if (require_transaction_routing_v2) {
    hello_payload = &StableV2HelloPayload();
  } else {
    hello_payload = channel_state_ == nullptr
                        ? nullptr
                        : &channel_state_->stable_baseline_hello_payload;
  }
  if (hello_payload == nullptr || hello_payload->empty()) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.HELLO_IDENTITY_INVALID",
                  "The stable parser HELLO identity is unavailable.");
    return false;
  }
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageHello, kSchemaHelloRequestV1),
                   *hello_payload,
                   &response,
                   messages,
                   ActiveSocketCacheKey())) {
    return false;
  }
  if (response.header.message_type != kMessageHelloAccept || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, messages);
    return false;
  }
  const bool transaction_routing_v2_accepted_by_server =
      response.payload.size() > kHelloAcceptCapabilityOffset &&
      (response.payload[kHelloAcceptCapabilityOffset] &
       kCapabilityTransactionRoutingV2) != 0;
  if (transaction_routing_v2_accepted != nullptr) {
    *transaction_routing_v2_accepted =
        transaction_routing_v2_accepted_by_server;
  }
  const bool prepared_metadata_transfer_v1_accepted_by_server =
      response.payload.size() > kHelloAcceptCapabilityOffset &&
      (response.payload[kHelloAcceptCapabilityOffset] &
       kCapabilityPreparedMetadataTransferV1) != 0;
  if (prepared_metadata_transfer_v1_accepted != nullptr) {
    *prepared_metadata_transfer_v1_accepted =
        prepared_metadata_transfer_v1_accepted_by_server;
  }
  const bool relation_descriptor_projection_v3_accepted_by_server =
      response.payload.size() > kHelloAcceptCapabilityOffset &&
      (response.payload[kHelloAcceptCapabilityOffset] &
       kCapabilityRelationDescriptorProjectionV3) != 0;
  if (relation_descriptor_projection_v3_accepted != nullptr) {
    *relation_descriptor_projection_v3_accepted =
        relation_descriptor_projection_v3_accepted_by_server;
  }
  if (require_transaction_routing_v2 &&
      !transaction_routing_v2_accepted_by_server) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.TRANSACTION_ROUTING_V2_REQUIRED",
                  "The server did not negotiate required independent transaction routing.");
    return false;
  }
  if (require_prepared_metadata_transfer_v1 &&
      !prepared_metadata_transfer_v1_accepted_by_server) {
    AddDiagnostic(
        messages,
        "PARSER_SERVER_IPC.PREPARED_METADATA_TRANSFER_V1_REQUIRED",
        "The server did not negotiate required prepared metadata transfer.");
    return false;
  }
  if (require_relation_descriptor_projection_v3 &&
      !relation_descriptor_projection_v3_accepted_by_server) {
    AddDiagnostic(
        messages,
        "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_V3_REQUIRED",
        "The server did not negotiate required persisted relation projection.");
    return false;
  }
  return true;
}

bool SbpsClient::AuthenticateAndAttach(std::string_view auth_payload,
                                       const ParserClientConfig& config,
                                       ParserSessionContext* session,
                                       MessageVectorSet* messages) const {
  auto credentials = CredentialsFromTestWirePayload(auth_payload);
  if (!config.database_token.empty() && credentials.requested_database == "default") {
    credentials.requested_database = config.database_token;
  }
  return AuthenticateAndAttach(credentials, config, session, messages);
}

bool SbpsClient::AuthenticateAndAttach(const AuthCredentialEnvelope& credentials,
                                       const ParserClientConfig& config,
                                       ParserSessionContext* session,
                                       MessageVectorSet* messages) const {
  if (session == nullptr) {
    AddDiagnostic(messages, "PARSER_SERVER_IPC.SESSION_CONTEXT_MISSING", "The parser session context is unavailable.");
    return false;
  }
  bool transaction_routing_v2_accepted = false;
  bool prepared_metadata_transfer_v1_accepted = false;
  bool relation_descriptor_projection_v3_accepted = false;
  const bool require_transaction_routing_v2 =
      config.require_transaction_routing_v2 ||
      config.require_prepared_metadata_transfer_v1 ||
      config.require_relation_descriptor_projection_v3;
  const std::vector<std::uint8_t>* admitted_hello_payload = nullptr;
  if (config.require_prepared_metadata_transfer_v1 &&
      config.require_relation_descriptor_projection_v3) {
    admitted_hello_payload =
        &StablePreparedMetadataTransferRelationDescriptorV3HelloPayload();
  } else if (config.require_prepared_metadata_transfer_v1) {
    admitted_hello_payload = &StablePreparedMetadataTransferV1HelloPayload();
  } else if (config.require_relation_descriptor_projection_v3) {
    admitted_hello_payload = &StableRelationDescriptorV3HelloPayload();
  } else if (require_transaction_routing_v2) {
    admitted_hello_payload = &StableV2HelloPayload();
  } else if (channel_state_ != nullptr) {
    admitted_hello_payload =
        &channel_state_->stable_baseline_hello_payload;
  }
  if (admitted_hello_payload == nullptr || admitted_hello_payload->size() < 72) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.HELLO_IDENTITY_INVALID",
                  "The stable parser HELLO identity is unavailable.");
    return false;
  }
  const auto admitted_parser_package_uuid =
      GetUuid(*admitted_hello_payload, 16);
  const auto admitted_dialect_profile_uuid =
      GetUuid(*admitted_hello_payload, 48);
  const auto admitted_parser_api_major =
      GetU32(*admitted_hello_payload, 64);
  const auto admitted_parser_api_minor =
      GetU32(*admitted_hello_payload, 68);
  if (!UuidPresent(admitted_parser_package_uuid) ||
      !UuidPresent(admitted_dialect_profile_uuid) ||
      admitted_parser_api_major == 0) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.HELLO_IDENTITY_INVALID",
                  "The stable parser HELLO did not contain canonical package and dialect identities.");
    return false;
  }
  if (!SendHelloWithRequirements(require_transaction_routing_v2,
                                 &transaction_routing_v2_accepted,
                                 config.require_prepared_metadata_transfer_v1,
                                 &prepared_metadata_transfer_v1_accepted,
                                 config.require_relation_descriptor_projection_v3,
                                 &relation_descriptor_projection_v3_accepted,
                                 messages)) {
    return false;
  }
  session->transaction_routing_v2_negotiated =
      transaction_routing_v2_accepted;
  session->prepared_metadata_transfer_v1_negotiated =
      prepared_metadata_transfer_v1_accepted;
  session->relation_descriptor_projection_v3_negotiated =
      relation_descriptor_projection_v3_accepted;

  Frame auth_response;
  const auto connection_uuid = MakeUuidV7Bytes();
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageAuthHandoff, kSchemaAuthHandoffV1, {}, connection_uuid),
                   EncodeAuthPayload(credentials, connection_uuid),
                   &auth_response,
                   messages,
                   ActiveSocketCacheKey())) {
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
                   messages,
                   ActiveSocketCacheKey())) {
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

  if (local_transaction_id == 0 ||
      !IsCanonicalNonzeroUuidText(transaction_uuid)) {
    AddDiagnostic(messages,
                  "PARSER_SERVER_IPC.ATTACH_TRANSACTION_IDENTITY_INVALID",
                  "Accepted database attach did not publish a complete engine-issued transaction ID and UUID.");
    return false;
  }

  session->authenticated = true;
  session->admitted_parser_package_uuid =
      UuidToText(admitted_parser_package_uuid);
  session->admitted_dialect_profile_uuid =
      UuidToText(admitted_dialect_profile_uuid);
  session->admitted_parser_package_version_major =
      admitted_parser_api_major;
  session->admitted_parser_package_version_minor =
      admitted_parser_api_minor;
  session->admitted_parser_package_version_patch = 0;
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
                           config,
                           credentials.requested_language,
                           descriptor_epoch == 0 ? name_resolution_epoch
                                                 : descriptor_epoch,
                           name_resolution_epoch);
  // The admitted dialect UUID identifies the negotiated parser package and
  // is carried separately in canonical SBLR/SBEE binding. Public name
  // resolution uses the parser family's semantic identifier profile.
  session->dialect_profile_uuid = config.dialect_profile_uuid;
  session->search_path = config.default_search_path;
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

PublicNameResolutionResult ResolveNamePublicWithCachePolicy(std::string_view endpoint,
                                                            std::string_view socket_cache_key,
                                                            const ParserSessionContext& session,
                                                            std::string_view presented_name,
                                                            bool quoted,
                                                            std::string_view object_class,
                                                            const ParserClientConfig& config,
                                                            bool use_cache) {
  PublicNameResolutionResult result;
  use_cache = use_cache && !IsPublicResourceObjectClass(object_class);
  const std::string endpoint_string(endpoint);
  if (!session.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.AUTH.REQUIRED",
        "ERROR",
        "public name resolution requires an authenticated server session",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  const auto cache_key =
      SbpsClientResolveNameCacheKey(
          endpoint_string, session, presented_name, quoted, object_class, config);
  if (use_cache) {
    if (const auto cached = LookupSbpsClientPublicResolutionCache(cache_key)) {
      return PublicResolutionResultFromCache(*cached);
    }
  }
  MessageVectorSet messages;
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!SendRequest(endpoint_string,
                   BaseHeader(kMessageResolveNameRequest,
                              kSchemaResolveNameRequestV1,
                              session_uuid,
                              connection_uuid),
                   EncodeResolveNamePayload(session,
                                            presented_name,
                                            quoted,
                                            object_class,
                                            config,
                                            !use_cache),
                   &response,
                   &messages,
                   socket_cache_key)) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageResolveNameResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result = DecodePublicNameResultPayload(response, "resolved");
  if (use_cache) {
    StoreSbpsClientPublicResolutionCacheEntry(cache_key, result);
  }
  return result;
}

PublicNameResolutionResult SbpsClient::ResolveNamePublic(const ParserSessionContext& session,
                                                        std::string_view presented_name,
                                                        bool quoted,
                                                        std::string_view object_class,
                                                        const ParserClientConfig& config) const {
  return ResolveNamePublicWithCachePolicy(endpoint_,
                                          ActiveSocketCacheKey(),
                                          session,
                                          presented_name,
                                          quoted,
                                          object_class,
                                          config,
                                          true);
}

PublicNameResolutionResult SbpsClient::ResolveNamePublicUncached(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config) const {
  return ResolveNamePublicWithCachePolicy(endpoint_,
                                          ActiveSocketCacheKey(),
                                          session,
                                          presented_name,
                                          quoted,
                                          object_class,
                                          config,
                                          false);
}

PublicNameResolutionResult SbpsClient::ResolveNamePublicOnTransaction(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction) const {
  PublicNameResolutionResult result;
  if (!session.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.AUTH.REQUIRED",
        "ERROR",
        "transaction-routed name resolution requires an authenticated server session",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  if (!RequireTransactionRoutingV2(session, &result.messages)) {
    return result;
  }
  if (!transaction.present()) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_REQUIRED",
        "ERROR",
        "transaction-routed name resolution requires an engine-issued selector",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageResolveNameRequest,
                              kSchemaResolveNameRequestV2,
                              session_uuid,
                              connection_uuid),
                   EncodeResolveNamePayloadV2(session,
                                              presented_name,
                                              quoted,
                                              object_class,
                                              config,
                                              transaction),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageResolveNameResult ||
      response.header.schema_id != kSchemaResolveNameResultV2 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(&messages,
                    "PARSER_SERVER_IPC.NAME_RESULT_SCHEMA_MISMATCH",
                    "The server did not return the transaction-routed name-result schema.");
    }
    result.messages = std::move(messages);
    return result;
  }
  return DecodePublicNameResultPayload(response, "resolved");
}

PublicNameResolutionResult
SbpsClient::ResolveNameSemanticPublicOnTransaction(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction) const {
  PublicNameResolutionResult result;
  if (!session.authenticated) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.AUTH.REQUIRED",
                  "semantic name resolution requires an authenticated server session");
    return result;
  }
  if (!RequireTransactionRoutingV2(session, &result.messages) ||
      !RequireRelationDescriptorProjectionV3(session, &result.messages)) {
    return result;
  }
  if (!transaction.present()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_REQUIRED",
                  "semantic name resolution requires an engine-issued selector");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(
          endpoint_,
          BaseHeader(kMessageResolveNameRequest,
                     kSchemaResolveNameRequestV3,
                     session_uuid,
                     connection_uuid),
          EncodeResolveNamePayloadV3(session,
                                     presented_name,
                                     quoted,
                                     object_class,
                                     config,
                                     transaction,
                                     0),
          &response,
          &messages,
          ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageResolveNameResult ||
      response.header.schema_id != kSchemaResolveNameResultV3 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(
          &messages,
          "PARSER_SERVER_IPC.NAME_RESULT_SCHEMA_MISMATCH",
          "The server did not return the semantic V3 name-result schema.");
    }
    result.messages = std::move(messages);
    return result;
  }
  return DecodePublicNameResultPayloadV3(response, "resolved", false);
}

PublicNameResolutionResult
SbpsClient::ResolveRelationDescriptorPublicOnTransaction(
    const ParserSessionContext& session,
    std::string_view presented_name,
    bool quoted,
    std::string_view object_class,
    const ParserClientConfig& config,
    const ParserTransactionSelector& transaction) const {
  PublicNameResolutionResult result;
  if (!session.authenticated) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.AUTH.REQUIRED",
                  "persisted relation projection requires an authenticated server session");
    return result;
  }
  if (!RequireTransactionRoutingV2(session, &result.messages)) {
    return result;
  }
  if (!RequireRelationDescriptorProjectionV3(session, &result.messages)) {
    return result;
  }
  if (!transaction.present()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_REQUIRED",
                  "persisted relation projection requires an engine-issued selector");
    return result;
  }
  if (object_class != "relation" && object_class != "table") {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.RELATION_DESCRIPTOR_REQUEST_INVALID",
                  "persisted relation projection is valid only for a relation or table");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(
          endpoint_,
          BaseHeader(kMessageResolveNameRequest,
                     kSchemaResolveNameRequestV3,
                     session_uuid,
                     connection_uuid),
          EncodeResolveNamePayloadV3(
              session,
              presented_name,
              quoted,
              object_class,
              config,
              transaction,
              kResolveNameProjectionRelationDescriptorV1),
          &response,
          &messages,
          ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageResolveNameResult ||
      response.header.schema_id != kSchemaResolveNameResultV3 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(
          &messages,
          "PARSER_SERVER_IPC.NAME_RESULT_SCHEMA_MISMATCH",
          "The server did not return the persisted relation projection schema.");
    }
    result.messages = std::move(messages);
    return result;
  }
  return DecodePublicNameResultPayloadV3(response, "resolved", true);
}

PublicNameResolutionResult SbpsClient::RenderUuidPublic(const ParserSessionContext& session,
                                                       std::string_view object_uuid) const {
  PublicNameResolutionResult result;
  if (!session.authenticated) {
    result.messages.diagnostics.push_back(MakeDiagnostic(
        "PARSER_SERVER_IPC.AUTH.REQUIRED",
        "ERROR",
        "public UUID rendering requires an authenticated server session",
        "parser_server_ipc.sbps_client"));
    return result;
  }
  const auto cache_key = SbpsClientRenderUuidCacheKey(endpoint_, session, object_uuid);
  if (const auto cached = LookupSbpsClientPublicResolutionCache(cache_key)) {
    return PublicResolutionResultFromCache(*cached);
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
                   &messages,
                   ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageRenderUuidResult || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  result = DecodePublicNameResultPayload(response, "rendered");
  StoreSbpsClientPublicResolutionCacheEntry(cache_key, result);
  return result;
}

ServerStatementContextResult SbpsClient::AcquireStatementContext(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) const {
  ServerStatementContextResult result;
  if (!session.authenticated) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.AUTH.REQUIRED",
                  "statement-context acquisition requires an authenticated server session");
    return result;
  }
  if (!transaction.present()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_REQUIRED",
                  "statement-context acquisition requires an engine-issued selector");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!UuidPresent(session_uuid) || !UuidPresent(connection_uuid) ||
      !UuidPresent(TextToUuid(transaction.transaction_uuid))) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_IDENTITY_INVALID",
                  "statement-context acquisition requires canonical nonzero UUID identities");
    return result;
  }
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(
          endpoint_,
          BaseHeader(kMessageAcquireStatementContextRequest,
                     kSchemaAcquireStatementContextRequestV1,
                     session_uuid,
                     connection_uuid),
          EncodeAcquireStatementContextPayloadV1(session, transaction),
          &response,
          &messages,
          ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type !=
          kMessageAcquireStatementContextResult ||
      response.header.schema_id != kSchemaAcquireStatementContextResultV1 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(
          &messages,
          "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RESULT_SCHEMA_MISMATCH",
          "The server did not return the statement-context V1 result schema.");
    }
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodeAcquireStatementContextPayloadV1(response.payload,
                                               &result.context) ||
      result.context.transaction.local_transaction_id !=
          transaction.local_transaction_id ||
      result.context.transaction.transaction_uuid !=
          transaction.transaction_uuid) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RESULT_INVALID",
                  "The engine-issued statement context did not match the requested transaction.");
    result.context = {};
    return result;
  }
  result.accepted = true;
  return result;
}

ServerStatementContextResult SbpsClient::AcquireNativeStatementContext(
    const ParserSessionContext& session,
    const ParserTransactionSelector& transaction) const {
  ServerStatementContextResult result;
  if (!session.authenticated) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.AUTH.REQUIRED",
                  "native statement-context acquisition requires an authenticated server session");
    return result;
  }
  if (!session.transaction_routing_v2_negotiated ||
      !session.relation_descriptor_projection_v3_negotiated ||
      !transaction.present()) {
    AddDiagnostic(
        &result.messages,
        "PARSER_SERVER_IPC.NATIVE_STATEMENT_CONTEXT_CAPABILITY_REQUIRED",
        "native statement-context acquisition requires the negotiated exact-transaction and relation-descriptor capabilities");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!UuidPresent(session_uuid) || !UuidPresent(connection_uuid) ||
      !UuidPresent(TextToUuid(transaction.transaction_uuid))) {
    AddDiagnostic(
        &result.messages,
        "PARSER_SERVER_IPC.STATEMENT_CONTEXT_IDENTITY_INVALID",
        "native statement-context acquisition requires canonical nonzero UUID identities");
    return result;
  }

  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(
          endpoint_,
          BaseHeader(kMessageAcquireStatementContextRequest,
                     kSchemaAcquireStatementContextRequestV5,
                     session_uuid,
                     connection_uuid),
          EncodeAcquireStatementContextPayloadV5(session, transaction),
          &response,
          &messages,
          ActiveSocketCacheKey())) {
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type !=
          kMessageAcquireStatementContextResult ||
      response.header.schema_id != kSchemaAcquireStatementContextResultV5 ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(
          &messages,
          "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RESULT_SCHEMA_MISMATCH",
          "The server did not return the native statement-context V5 result schema.");
    }
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodeAcquireStatementContextPayloadV5(response.payload,
                                               &result.context) ||
      result.context.transaction.local_transaction_id !=
          transaction.local_transaction_id ||
      result.context.transaction.transaction_uuid !=
          transaction.transaction_uuid) {
    AddDiagnostic(
        &result.messages,
        "PARSER_SERVER_IPC.STATEMENT_CONTEXT_RESULT_INVALID",
        "The engine-issued native statement context was malformed or did not match the requested transaction.");
    result.context = {};
    return result;
  }
  result.accepted = true;
  return result;
}

ServerExecutionResult SbpsClient::ExecuteSblr(const ParserSessionContext& session,
                                             std::string_view encoded_sblr_envelope,
                                             bool cursor_requested) const {
  return ExecuteSblrWithDataPacket(session, encoded_sblr_envelope, {}, cursor_requested);
}

ServerExecutionResult SbpsClient::ExecuteSblrRouted(
    const ParserSessionContext& session,
    std::string_view encoded_sblr_envelope,
    const ParserTransactionRouting& transaction,
    bool cursor_requested) const {
  return ExecuteSblrWithDataPacketRouted(session,
                                         encoded_sblr_envelope,
                                         {},
                                         transaction,
                                         cursor_requested);
}

ServerExecutionResult SbpsClient::ExecuteSblrWithDataPacket(
    const ParserSessionContext& session,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
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
                   EncodeExecutePayload(session_uuid,
                                        encoded_sblr_envelope,
                                        cursor_requested,
                                        data_packet),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
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
  result.cursor_uuid = OptionalUuidToText(GetUuid(response.payload, offset));
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
  if (ExecutionInvalidatesPublicResolutionCache(result.operation_id)) {
    ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  }
  return result;
}

ServerExecutionResult SbpsClient::ExecuteSblrWithDataPacketRouted(
    const ParserSessionContext& session,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    const ParserTransactionRouting& transaction,
    bool cursor_requested) const {
  ServerExecutionResult result;
  MessageVectorSet messages;
  if (!RequireTransactionRoutingV2(session, &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  if (!ValidateTransactionRouting(transaction, &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageExecuteSblr,
                              kSchemaExecuteSblrV2,
                              session_uuid,
                              connection_uuid),
                   EncodeExecutePayloadV2(session_uuid,
                                          {},
                                          encoded_sblr_envelope,
                                          cursor_requested,
                                          data_packet,
                                          transaction),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectV2TransportOutcomeUnknown(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageExecuteResult) {
    AddFrameDiagnostics(response, &messages);
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_INVALID",
                  "The server returned the wrong message type for V2 execution.");
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "unexpected_response_type");
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.schema_id != kSchemaExecuteResultV2) {
    if (IsErrorFrame(response)) AddFrameDiagnostics(response, &messages);
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_SCHEMA_MISMATCH",
                  "The server did not return the required V2 execute-result schema.");
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "unexpected_response_schema");
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodeExecuteResultPayloadV2(response, &result, &messages)) {
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "malformed_typed_response");
    result.messages = std::move(messages);
    return result;
  }
  result.messages = std::move(messages);
  if (result.catalog_invalidation_applied ||
      ((result.accepted ||
        result.finality_state == ParserTransactionFinality::kKnownApplied) &&
       ExecutionInvalidatesPublicResolutionCache(result.operation_id))) {
    ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  }
  return result;
}

ServerExecutionResult SbpsClient::ExecuteCanonicalSblrWithDataPacket(
    const ParserSessionContext& session,
    const ParserStatementContext& statement_context,
    const ParserCanonicalSblrSubmission& submission,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) const {
  ServerExecutionResult result;
  MessageVectorSet messages;
  if (!RequireTransactionRoutingV2(session, &messages) ||
      !statement_context.complete() || !submission.complete() ||
      submission.statement_uuid != statement_context.statement_uuid) {
    if (messages.diagnostics.empty()) {
      AddDiagnostic(&messages,
                    "PARSER_SERVER_IPC.CANONICAL_STATEMENT_CONTEXT_INVALID",
                    "Canonical execution requires the exact acquired statement and active transaction context.");
    }
    result.messages = std::move(messages);
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  if (!UuidPresent(session_uuid) || !UuidPresent(connection_uuid) ||
      !UuidPresent(TextToUuid(statement_context.statement_uuid)) ||
      !UuidPresent(TextToUuid(statement_context.transaction.transaction_uuid))) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.CANONICAL_STATEMENT_CONTEXT_INVALID",
                  "Canonical execution identities must be canonical nonzero UUIDs.");
    result.messages = std::move(messages);
    return result;
  }
  Frame response;
  if (!SendRequest(
          endpoint_,
          BaseHeader(kMessageExecuteSblr,
                     kSchemaExecuteCanonicalSblrV1,
                     session_uuid,
                     connection_uuid),
          EncodeCanonicalExecutePayloadV1(session,
                                          statement_context,
                                          submission,
                                          data_packet,
                                          cursor_requested),
          &response,
          &messages,
          ActiveSocketCacheKey())) {
    ProjectV2TransportOutcomeUnknown(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageExecuteResult ||
      response.header.schema_id != kSchemaExecuteResultV2) {
    AddFrameDiagnostics(response, &messages);
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_SCHEMA_MISMATCH",
                  "The server did not return the canonical-route execute result schema.");
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "unexpected_canonical_response");
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodeExecuteResultPayloadV2(response, &result, &messages)) {
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "malformed_canonical_response");
  }
  result.messages = std::move(messages);
  return result;
}

ServerPrepareSblrResult SbpsClient::PrepareSblr(
    const ParserSessionContext& session,
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
                   &messages,
                   ActiveSocketCacheKey())) {
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

ServerPrepareSblrResult SbpsClient::PrepareSblrRouted(
    const ParserSessionContext& session,
    std::string_view encoded_sblr_envelope,
    const ParserTransactionSelector& transaction) const {
  ServerPrepareSblrResult result;
  if (!RequireTransactionRoutingV2(session, &result.messages)) {
    return result;
  }
  if (!transaction.present()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.TRANSACTION_SELECTOR_REQUIRED",
                  "V2 prepare requires an engine-issued transaction selector.");
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessagePrepareSblr,
                              kSchemaPrepareSblrV2,
                              session_uuid,
                              connection_uuid),
                   EncodePreparePayloadV2(session,
                                          session_uuid,
                                          encoded_sblr_envelope,
                                          transaction),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectV2PrepareTransportOutcomeUnknown(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessagePrepareResult ||
      response.header.schema_id != kSchemaPrepareResultV2) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.PREPARE_RESULT_SCHEMA_MISMATCH",
                  "The server did not return the required V2 prepare-result schema.");
    ProjectV2PrepareOutcomeUnknown(
        &messages, &result, "unexpected_response_type_or_schema");
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodePrepareResultPayloadV2(response.payload, &result, &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  result.messages = std::move(messages);
  return result;
}

ServerExecutionResult SbpsClient::ExecutePreparedSblr(
    const ParserSessionContext& session,
    std::string_view prepared_statement_uuid,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
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
                                                cursor_requested,
                                                data_packet),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
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
  result.cursor_uuid = OptionalUuidToText(GetUuid(response.payload, offset));
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
  if (ExecutionInvalidatesPublicResolutionCache(result.operation_id)) {
    ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  }
  return result;
}

ServerExecutionResult SbpsClient::ExecutePreparedSblrRouted(
    const ParserSessionContext& session,
    std::string_view prepared_statement_uuid,
    const ParserTransactionSelector& transaction,
    std::string_view encoded_sblr_envelope,
    const std::vector<std::uint8_t>& data_packet,
    bool cursor_requested) const {
  ServerExecutionResult result;
  if (!RequireTransactionRoutingV2(session, &result.messages)) {
    return result;
  }
  if (prepared_statement_uuid.empty()) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.PREPARED_HANDLE_REQUIRED",
                  "Prepared SBLR execution requires a prepared statement UUID.");
    return result;
  }
  ParserTransactionRouting routing;
  routing.route = ParserTransactionRoute::kSelected;
  routing.selector = transaction;
  MessageVectorSet messages;
  if (!ValidateTransactionRouting(routing, &messages)) {
    result.messages = std::move(messages);
    return result;
  }
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageExecuteSblr,
                              kSchemaExecuteSblrV2,
                              session_uuid,
                              connection_uuid),
                   EncodeExecutePayloadV2(session_uuid,
                                          TextToUuid(prepared_statement_uuid),
                                          encoded_sblr_envelope,
                                          cursor_requested,
                                          data_packet,
                                          routing),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectV2TransportOutcomeUnknown(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageExecuteResult ||
      response.header.schema_id != kSchemaExecuteResultV2) {
    if (IsErrorFrame(response)) AddFrameDiagnostics(response, &messages);
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.EXECUTE_RESULT_SCHEMA_MISMATCH",
                  "The server did not return the required V2 execute-result schema.");
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "unexpected_response_type_or_schema");
    result.messages = std::move(messages);
    return result;
  }
  if (!DecodeExecuteResultPayloadV2(response, &result, &messages)) {
    ProjectV2ResponseOutcomeUnknown(
        &messages, &result, "malformed_typed_response");
    result.messages = std::move(messages);
    return result;
  }
  result.messages = std::move(messages);
  if (result.catalog_invalidation_applied ||
      ((result.accepted ||
        result.finality_state == ParserTransactionFinality::kKnownApplied) &&
       ExecutionInvalidatesPublicResolutionCache(result.operation_id))) {
    ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  }
  return result;
}

ServerClosePreparedSblrResult SbpsClient::ClosePreparedSblr(
    const ParserSessionContext& session,
    std::string_view prepared_statement_uuid) const {
  ServerClosePreparedSblrResult result;
  if (!IsCanonicalNonzeroUuidText(session.session_uuid)) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.SESSION_REQUIRED",
                  "Prepared SBLR close requires a canonical nonzero session UUID.");
    return result;
  }
  if (!IsCanonicalNonzeroUuidText(prepared_statement_uuid)) {
    AddDiagnostic(&result.messages,
                  "PARSER_SERVER_IPC.PREPARED_HANDLE_REQUIRED",
                  "Prepared SBLR close requires a canonical nonzero prepared statement UUID.");
    return result;
  }

  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto prepared_uuid = TextToUuid(prepared_statement_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  MessageVectorSet messages;
  Frame response;
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageClosePreparedSblr,
                              kSchemaClosePreparedSblrV1,
                              session_uuid,
                              connection_uuid),
                   EncodeClosePreparedSblrPayload(session_uuid,
                                                  prepared_uuid),
                   &response,
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectCloseTransportFailure(messages, &result);
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.message_type != kMessageClosePreparedSblrResult ||
      IsErrorFrame(response)) {
    AddFrameDiagnostics(response, &messages);
    if (!IsErrorFrame(response)) {
      AddDiagnostic(&messages,
                    "PARSER_SERVER_IPC.CLOSE_PREPARED_RESULT_INVALID",
                    "The server returned the wrong message type for prepared SBLR close.");
    }
    result.messages = std::move(messages);
    return result;
  }
  if (response.header.schema_id != kSchemaClosePreparedSblrResultV1) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.CLOSE_PREPARED_RESULT_SCHEMA_MISMATCH",
                  "The server returned the wrong schema for prepared SBLR close.");
    result.messages = std::move(messages);
    return result;
  }

  std::size_t offset = 0;
  std::string outcome;
  if (!ReadString(response.payload, &offset, &outcome) ||
      outcome != "accepted" || offset + 16 > response.payload.size()) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.CLOSE_PREPARED_RESULT_INVALID",
                  "The server prepared-close result payload is malformed.");
    result.messages = std::move(messages);
    return result;
  }
  const auto response_uuid = GetUuid(response.payload, offset);
  offset += 16;
  if (response_uuid != prepared_uuid ||
      !ReadString(response.payload, &offset, &result.detail) ||
      offset != response.payload.size()) {
    AddDiagnostic(&messages,
                  "PARSER_SERVER_IPC.CLOSE_PREPARED_RESULT_INVALID",
                  "The server prepared-close result did not echo the requested identity or contained trailing data.");
    result.messages = std::move(messages);
    return result;
  }
  result.accepted = true;
  result.prepared_statement_uuid = UuidToText(response_uuid);
  result.messages = std::move(messages);
  return result;
}

ServerFetchResult SbpsClient::FetchCursor(const ParserSessionContext& session,
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
                   &messages,
                   ActiveSocketCacheKey())) {
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

ServerCloseCursorResult SbpsClient::CloseCursor(const ParserSessionContext& session,
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
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectCloseTransportFailure(messages, &result);
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
  result.messages = std::move(messages);
  return result;
}

ServerCloseCursorResult SbpsClient::CancelCursor(const ParserSessionContext& session,
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
                   &messages,
                   ActiveSocketCacheKey())) {
    ProjectCloseTransportFailure(messages, &result);
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
  result.messages = std::move(messages);
  return result;
}

ServerManagementResult SbpsClient::Manage(const ParserSessionContext& session,
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
        "PARSER_SERVER_IPC.AUTH.REQUIRED",
        "ERROR",
        "server management requests require an authenticated server session",
        "parser_server_ipc.sbps_client"));
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
                   &messages,
                   ActiveSocketCacheKey())) {
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
  ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  return result;
}

bool SbpsClient::DisconnectSession(const ParserSessionContext& session, MessageVectorSet* messages) const {
  if (!session.authenticated || session.session_uuid.empty()) return true;
  ClearSbpsClientPublicResolutionCacheForSession(endpoint_, session);
  Frame response;
  const auto session_uuid = TextToUuid(session.session_uuid);
  const auto connection_uuid = TextToUuid(session.connection_uuid);
  std::vector<std::uint8_t> disconnect_payload;
  PutUuid(&disconnect_payload, session_uuid);
  PutString(&disconnect_payload, "parser_disconnect_notice");
  if (!SendRequest(endpoint_,
                   BaseHeader(kMessageDisconnectNotice, 0, session_uuid, connection_uuid),
                   disconnect_payload,
                   &response,
                   messages,
                   ActiveSocketCacheKey())) {
    return false;
  }
  if (response.header.message_type != kMessageDisconnectNotice || IsErrorFrame(response)) {
    AddFrameDiagnostics(response, messages);
    return false;
  }
  return true;
}

} // namespace scratchbird::parser::ipc
