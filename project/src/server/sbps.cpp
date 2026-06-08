// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_IPC_FOUNDATION_SBPS

#include "sbps.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <random>

namespace scratchbird::server::sbps {

namespace {

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
  const std::size_t offset = out->size();
  out->resize(offset + uuid.size());
  std::copy(uuid.begin(), uuid.end(), out->begin() + static_cast<std::ptrdiff_t>(offset));
}

std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  std::memcpy(uuid.data(), data.data() + offset, uuid.size());
  return uuid;
}

void PutBytes(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 32>& bytes) {
  out->insert(out->end(), bytes.begin(), bytes.end());
}

std::array<std::uint8_t, 32> GetBytes32(const std::vector<std::uint8_t>& data,
                                        std::size_t offset) {
  std::array<std::uint8_t, 32> bytes{};
  std::memcpy(bytes.data(), data.data() + offset, bytes.size());
  return bytes;
}

void PutString(std::vector<std::uint8_t>* out, const std::string& value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadString(const std::vector<std::uint8_t>& data, std::size_t* offset, std::string* value) {
  if (*offset + 2 > data.size()) return false;
  const auto length = GetU16(data, *offset);
  *offset += 2;
  if (*offset + length > data.size()) return false;
  value->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
  *offset += length;
  return true;
}

void PutAtU16(std::vector<std::uint8_t>* out, std::size_t offset, std::uint16_t value) {
  (*out)[offset] = static_cast<std::uint8_t>(value & 0xffu);
  (*out)[offset + 1] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
}

void PutAtU32(std::vector<std::uint8_t>* out, std::size_t offset, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    (*out)[offset + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xffu);
  }
}

void PutAtU64(std::vector<std::uint8_t>* out, std::size_t offset, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    (*out)[offset + static_cast<std::size_t>(shift / 8)] =
        static_cast<std::uint8_t>((value >> shift) & 0xffu);
  }
}

void Pad4(std::vector<std::uint8_t>* out) {
  while ((out->size() % 4u) != 0u) {
    out->push_back(0);
  }
}

void PutTlv(std::vector<std::uint8_t>* out,
            const std::string& key,
            std::uint16_t type_code,
            const std::string& value) {
  PutU16(out, static_cast<std::uint16_t>(key.size()));
  PutU16(out, type_code);
  PutU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), key.begin(), key.end());
  out->insert(out->end(), value.begin(), value.end());
  Pad4(out);
}

ServerDiagnostic FrameDiagnostic(const std::string& code, const std::string& message) {
  return IpcDiagnostic(code, code, message);
}

std::optional<ServerDiagnostic> ProtocolVersionDiagnostic(std::uint16_t major,
                                                          std::uint16_t minor) {
  if (major < kProtocolMajorMinSupported) {
    return FrameDiagnostic("PARSER_SERVER_IPC.PROTOCOL_VERSION_TOO_OLD",
                           "The SBPS protocol version is too old and was refused before admission.");
  }
  if (major > kProtocolMajorMaxSupported ||
      (major == kProtocolMajor && minor > kProtocolMinorMaxSupported)) {
    return FrameDiagnostic("PARSER_SERVER_IPC.PROTOCOL_VERSION_FUTURE",
                           "The SBPS protocol version is newer than this server supports.");
  }
  if (major == kProtocolMajor && minor < kProtocolMinorMinSupported) {
    return FrameDiagnostic("PARSER_SERVER_IPC.PROTOCOL_VERSION_DOWNGRADE_REFUSED",
                           "The SBPS protocol version would require a downgrade path that is not supported.");
  }
  if (major != kProtocolMajor) {
    return FrameDiagnostic("PARSER_SERVER_IPC.PROTOCOL_VERSION_UNSUPPORTED",
                           "The SBPS protocol version is unsupported.");
  }
  return std::nullopt;
}

}  // namespace

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
  std::array<std::uint8_t, 16> uuid{};
  const auto timestamp = static_cast<std::uint64_t>(now);
  uuid[0] = static_cast<std::uint8_t>((timestamp >> 40u) & 0xffu);
  uuid[1] = static_cast<std::uint8_t>((timestamp >> 32u) & 0xffu);
  uuid[2] = static_cast<std::uint8_t>((timestamp >> 24u) & 0xffu);
  uuid[3] = static_cast<std::uint8_t>((timestamp >> 16u) & 0xffu);
  uuid[4] = static_cast<std::uint8_t>((timestamp >> 8u) & 0xffu);
  uuid[5] = static_cast<std::uint8_t>(timestamp & 0xffu);
  const auto r1 = rng();
  const auto r2 = rng();
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

bool IsZeroUuid(const std::array<std::uint8_t, 16>& uuid) {
  for (const auto byte : uuid) {
    if (byte != 0) return false;
  }
  return true;
}

bool IsKnownMessageType(std::uint16_t message_type) {
  switch (message_type) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 10:
    case 11:
    case 12:
    case 20:
    case 21:
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 40:
    case 41:
    case 42:
    case 43:
    case 44:
    case 45:
    case 46:
    case 47:
    case 50:
    case 51:
    case 60:
    case 61:
    case 70:
    case 71:
    case 72:
    case 73:
    case 74:
    case 80:
    case 81:
    case 82:
    case 83:
    case 84:
    case 85:
    case 86:
    case 87:
    case 88:
      return true;
    default:
      return false;
  }
}

bool HasUnknownCapabilityBits(const std::array<std::uint8_t, 32>& capability_bitmap) {
  constexpr std::uint8_t kKnownByte0 = 0x01;
  if ((capability_bitmap[0] & static_cast<std::uint8_t>(~kKnownByte0)) != 0) return true;
  for (std::size_t i = 1; i < capability_bitmap.size(); ++i) {
    if (capability_bitmap[i] != 0) return true;
  }
  return false;
}

std::vector<std::uint8_t> EncodeFrame(const FrameHeader& input_header,
                                      const std::vector<std::uint8_t>& payload) {
  auto header = input_header;
  header.payload_len = static_cast<std::uint32_t>(payload.size());
  header.payload_crc32c = payload.empty() ? 0 : Crc32c(payload.data(), payload.size());

  std::vector<std::uint8_t> out;
  out.reserve(kHeaderBytes + payload.size());
  PutU32(&out, kFrameMagic);
  PutU16(&out, kHeaderBytes);
  PutU16(&out, header.protocol_major);
  PutU16(&out, header.protocol_minor);
  PutU16(&out, header.message_type);
  PutU32(&out, header.flags);
  PutU32(&out, header.payload_schema_id);
  PutU32(&out, header.payload_len);
  PutU32(&out, 0);
  PutU32(&out, header.payload_crc32c);
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
    const FrameHeader& input_header,
    const std::vector<std::uint8_t>& payload,
    std::uint64_t max_payload_bytes) {
  if (max_payload_bytes == 0 || payload.size() <= max_payload_bytes) {
    return {EncodeFrame(input_header, payload)};
  }

  const auto chunk_payload_bytes = static_cast<std::size_t>(
      std::min<std::uint64_t>(max_payload_bytes, 0xffffffffu));
  std::vector<std::vector<std::uint8_t>> frames;
  frames.reserve((payload.size() + chunk_payload_bytes - 1) / chunk_payload_bytes);

  const auto stream_id = input_header.stream_id == 0 ? 1 : input_header.stream_id;
  auto sequence_number = input_header.sequence_number == 0 ? 1 : input_header.sequence_number;
  std::size_t offset = 0;
  while (offset < payload.size()) {
    const auto remaining = payload.size() - offset;
    const auto chunk_size = std::min(chunk_payload_bytes, remaining);
    std::vector<std::uint8_t> chunk(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                    payload.begin() + static_cast<std::ptrdiff_t>(offset + chunk_size));
    FrameHeader chunk_header = input_header;
    chunk_header.stream_id = stream_id;
    chunk_header.sequence_number = sequence_number++;
    chunk_header.flags = input_header.flags | kFlagPayloadChunk;
    if (offset + chunk_size >= payload.size()) {
      chunk_header.flags |= kFlagFinal;
    } else {
      chunk_header.flags &= ~kFlagFinal;
    }
    frames.push_back(EncodeFrame(chunk_header, chunk));
    offset += chunk_size;
  }
  return frames;
}

ChunkAssemblyResult AssembleDecodedChunkSequence(const std::vector<Frame>& chunks,
                                                 std::uint64_t max_total_payload_bytes) {
  ChunkAssemblyResult result;
  if (chunks.empty()) {
    result.diagnostics.push_back(IpcDiagnostic(
        "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID",
        "parser_server_ipc.chunk_sequence_invalid",
        "The SBPS chunk sequence is empty."));
    return result;
  }

  const Frame& first = chunks.front();
  if ((first.header.flags & kFlagPayloadChunk) == 0 ||
      first.header.stream_id == 0 ||
      first.header.sequence_number == 0) {
    result.diagnostics.push_back(IpcDiagnostic(
        "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID",
        "parser_server_ipc.chunk_sequence_invalid",
        "The SBPS chunk sequence header is invalid."));
    return result;
  }

  std::vector<std::uint8_t> payload;
  std::uint64_t expected_sequence = first.header.sequence_number;
  bool saw_final = false;
  for (const auto& chunk : chunks) {
    if (saw_final) {
      result.diagnostics.push_back(IpcDiagnostic(
          "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID",
          "parser_server_ipc.chunk_sequence_invalid",
          "The SBPS chunk sequence contains data after the final chunk."));
      return result;
    }
    const bool compatible =
        (chunk.header.flags & kFlagPayloadChunk) != 0 &&
        chunk.header.message_type == first.header.message_type &&
        chunk.header.payload_schema_id == first.header.payload_schema_id &&
        chunk.header.stream_id == first.header.stream_id &&
        chunk.header.sequence_number == expected_sequence &&
        chunk.header.request_uuid == first.header.request_uuid &&
        chunk.header.connection_uuid == first.header.connection_uuid &&
        chunk.header.session_uuid == first.header.session_uuid;
    if (!compatible) {
      result.diagnostics.push_back(IpcDiagnostic(
          "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID",
          "parser_server_ipc.chunk_sequence_invalid",
          "The SBPS chunk sequence is not contiguous."));
      return result;
    }
    if (payload.size() + chunk.payload.size() > max_total_payload_bytes) {
      result.diagnostics.push_back(IpcDiagnostic(
          "PARSER_SERVER_IPC.PAYLOAD_TOO_LARGE",
          "parser_server_ipc.payload_too_large",
          "The assembled SBPS payload exceeds the configured protocol limit."));
      return result;
    }
    payload.insert(payload.end(), chunk.payload.begin(), chunk.payload.end());
    saw_final = (chunk.header.flags & kFlagFinal) != 0;
    ++expected_sequence;
  }
  if (!saw_final) {
    result.diagnostics.push_back(IpcDiagnostic(
        "PARSER_SERVER_IPC.CHUNK_SEQUENCE_INVALID",
        "parser_server_ipc.chunk_sequence_invalid",
        "The SBPS chunk sequence is not final."));
    return result;
  }

  Frame assembled = first;
  assembled.payload = std::move(payload);
  assembled.header.payload_len = static_cast<std::uint32_t>(assembled.payload.size());
  assembled.header.flags = (chunks.back().header.flags & ~kFlagPayloadChunk) | kFlagFinal;
  result.frame = std::move(assembled);
  return result;
}

std::optional<std::uint32_t> PayloadLengthFromHeader(const std::vector<std::uint8_t>& header_bytes) {
  if (header_bytes.size() < kHeaderBytes) return std::nullopt;
  return GetU32(header_bytes, 20);
}

DecodeResult DecodeFrameBytes(const std::vector<std::uint8_t>& bytes,
                              std::uint32_t max_payload_bytes) {
  DecodeResult result;
  if (bytes.size() < kHeaderBytes) {
    result.diagnostics.push_back(FrameDiagnostic("PARSER_SERVER_IPC.FRAME_LENGTH_INVALID",
                                                 "The SBPS frame header is incomplete."));
    return result;
  }
  if (GetU32(bytes, 0) != kFrameMagic) {
    result.diagnostics.push_back(FrameDiagnostic("PARSER_SERVER_IPC.FRAME_MAGIC_INVALID",
                                                 "The SBPS frame magic is invalid."));
    return result;
  }
  if (GetU16(bytes, 4) != kHeaderBytes) {
    result.diagnostics.push_back(FrameDiagnostic("PARSER_SERVER_IPC.FRAME_HEADER_INVALID",
                                                 "The SBPS frame header size is invalid."));
    return result;
  }
  const auto protocol_major = GetU16(bytes, 6);
  const auto protocol_minor = GetU16(bytes, 8);
  if (const auto protocol_diagnostic = ProtocolVersionDiagnostic(protocol_major, protocol_minor)) {
    result.diagnostics.push_back(*protocol_diagnostic);
    return result;
  }
  const auto message_type = GetU16(bytes, 10);
  if (!IsKnownMessageType(message_type)) {
    result.diagnostics.push_back(FrameDiagnostic("PARSER_SERVER_IPC.MESSAGE_TYPE_UNKNOWN",
                                                 "The SBPS message type is not registered."));
    return result;
  }
  const auto flags = GetU32(bytes, 12);
  if ((flags & ~kAllowedFrameFlags) != 0) {
    result.diagnostics.push_back(FrameDiagnostic("PARSER_SERVER_IPC.FRAME_FLAGS_INVALID",
                                                 "The SBPS frame flags contain reserved bits."));
    return result;
  }
  const auto payload_len = GetU32(bytes, 20);
  if (payload_len > max_payload_bytes || bytes.size() != kHeaderBytes + payload_len) {
    result.diagnostics.push_back(FrameDiagnostic("PARSER_SERVER_IPC.FRAME_LENGTH_INVALID",
                                                 "The SBPS frame length is invalid."));
    return result;
  }
  auto header_for_crc = std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + kHeaderBytes);
  PutAtU32(&header_for_crc, 24, 0);
  if (Crc32c(header_for_crc.data(), header_for_crc.size()) != GetU32(bytes, 24)) {
    result.diagnostics.push_back(FrameDiagnostic("PARSER_SERVER_IPC.FRAME_HEADER_CRC_INVALID",
                                                 "The SBPS frame header CRC is invalid."));
    return result;
  }
  if (payload_len > 0) {
    const auto payload_crc =
        Crc32c(bytes.data() + kHeaderBytes, static_cast<std::size_t>(payload_len));
    if (payload_crc != GetU32(bytes, 28)) {
      result.diagnostics.push_back(FrameDiagnostic("PARSER_SERVER_IPC.FRAME_PAYLOAD_CRC_INVALID",
                                                   "The SBPS frame payload CRC is invalid."));
      return result;
    }
  } else if (GetU32(bytes, 28) != 0) {
    result.diagnostics.push_back(FrameDiagnostic("PARSER_SERVER_IPC.FRAME_PAYLOAD_CRC_INVALID",
                                                 "An empty SBPS payload must have zero payload CRC."));
    return result;
  }

  Frame frame;
  frame.header.protocol_major = protocol_major;
  frame.header.protocol_minor = protocol_minor;
  frame.header.message_type = message_type;
  frame.header.flags = flags;
  frame.header.payload_schema_id = GetU32(bytes, 16);
  frame.header.payload_len = payload_len;
  frame.header.header_crc32c = GetU32(bytes, 24);
  frame.header.payload_crc32c = GetU32(bytes, 28);
  frame.header.stream_id = GetU64(bytes, 32);
  frame.header.sequence_number = GetU64(bytes, 40);
  frame.header.request_uuid = GetUuid(bytes, 48);
  frame.header.connection_uuid = GetUuid(bytes, 64);
  frame.header.session_uuid = GetUuid(bytes, 80);
  frame.payload.assign(bytes.begin() + kHeaderBytes, bytes.end());
  result.frame = std::move(frame);
  return result;
}

std::vector<std::uint8_t> EncodeMessageVectorSet(
    const std::vector<ServerDiagnostic>& diagnostics,
    const std::array<std::uint8_t, 16>& request_uuid) {
  std::vector<std::uint8_t> records;
  for (const auto& diagnostic : diagnostics) {
    std::vector<std::uint8_t> record(112, 0);
    PutAtU16(&record, 8, 1);
    record[10] = 1;
    record[11] = diagnostic.severity == ServerDiagnosticSeverity::kWarning ? 1 :
                 diagnostic.severity == ServerDiagnosticSeverity::kInfo ? 0 : 2;
    const auto vector_uuid = MakeUuidV7Bytes();
    std::memcpy(record.data() + 16, vector_uuid.data(), vector_uuid.size());
    std::memcpy(record.data() + 48, request_uuid.data(), request_uuid.size());
    PutAtU32(&record, 88, 0x52565253u);
    const std::string language = "en";
    const std::string admin_detail_key;
    std::vector<ServerDiagnosticField> public_fields;
    for (const auto& field : diagnostic.fields) {
      if (IsPublicDiagnosticFieldAllowed(field.key, field.value)) {
        public_fields.push_back(field);
      }
    }
    PutAtU16(&record, 92, static_cast<std::uint16_t>(language.size()));
    PutAtU16(&record, 94, static_cast<std::uint16_t>(diagnostic.code.size()));
    PutAtU16(&record, 96, static_cast<std::uint16_t>(diagnostic.message_key.size()));
    PutAtU16(&record, 98, static_cast<std::uint16_t>(admin_detail_key.size()));
    PutAtU16(&record, 100, static_cast<std::uint16_t>(diagnostic.safe_message.size()));
    PutAtU16(&record, 102, static_cast<std::uint16_t>(public_fields.size()));
    record[109] = 1;
    record.insert(record.end(), language.begin(), language.end());
    record.insert(record.end(), diagnostic.code.begin(), diagnostic.code.end());
    record.insert(record.end(), diagnostic.message_key.begin(), diagnostic.message_key.end());
    record.insert(record.end(), admin_detail_key.begin(), admin_detail_key.end());
    record.insert(record.end(), diagnostic.safe_message.begin(), diagnostic.safe_message.end());
    for (const auto& field : public_fields) {
      PutTlv(&record, field.key, 1, field.value);
    }
    Pad4(&record);
    PutAtU32(&record, 0, static_cast<std::uint32_t>(record.size()));
    PutAtU32(&record, 4, 0);
    const auto record_crc = Crc32c(record.data(), record.size());
    PutAtU32(&record, 4, record_crc);
    records.insert(records.end(), record.begin(), record.end());
  }

  std::vector<std::uint8_t> out(64, 0);
  PutAtU32(&out, 0, kMessageVectorMagic);
  PutAtU16(&out, 4, 64);
  PutAtU16(&out, 6, 1);
  PutAtU32(&out, 8, 1);
  PutAtU32(&out, 12, static_cast<std::uint32_t>(diagnostics.size()));
  PutAtU32(&out, 16, static_cast<std::uint32_t>(64 + records.size()));
  const auto records_crc = records.empty() ? 0 : Crc32c(records.data(), records.size());
  PutAtU32(&out, 20, records_crc);
  PutAtU64(&out, 24, 1);
  const auto set_uuid = MakeUuidV7Bytes();
  std::memcpy(out.data() + 32, set_uuid.data(), set_uuid.size());
  PutAtU32(&out, 48, 65536);
  PutAtU32(&out, 52, 0);
  const auto header_crc = Crc32c(out.data(), out.size());
  PutAtU32(&out, 52, header_crc);
  out.insert(out.end(), records.begin(), records.end());
  return out;
}

std::vector<std::string> DecodeMessageVectorDiagnosticCodes(const std::vector<std::uint8_t>& payload) {
  std::vector<std::string> codes;
  if (payload.size() < 64 || GetU32(payload, 0) != kMessageVectorMagic ||
      GetU16(payload, 4) != 64 || GetU16(payload, 6) != 1) {
    return codes;
  }
  const auto vector_count = GetU32(payload, 12);
  std::size_t offset = 64;
  for (std::uint32_t index = 0; index < vector_count && offset + 112 <= payload.size(); ++index) {
    const auto record_bytes = GetU32(payload, offset);
    if (record_bytes < 112 || offset + record_bytes > payload.size()) break;
    const auto language_len = GetU16(payload, offset + 92);
    const auto code_len = GetU16(payload, offset + 94);
    std::size_t code_offset = offset + 112 + language_len;
    if (code_offset + code_len <= offset + record_bytes) {
      codes.emplace_back(reinterpret_cast<const char*>(payload.data() + code_offset), code_len);
    }
    offset += record_bytes;
  }
  return codes;
}

std::vector<std::uint8_t> EncodeHelloRequestForTest() {
  HelloRequest hello;
  hello.parser_instance_uuid = MakeUuidV7Bytes();
  hello.parser_package_uuid = MakeUuidV7Bytes();
  hello.parser_family_uuid = MakeUuidV7Bytes();
  hello.dialect_profile_uuid = MakeUuidV7Bytes();
  hello.parser_api_major = 3;
  hello.parser_api_minor = 0;
  hello.protocol = "SBPS";
  hello.profile_id = "sif.test";
  hello.bundle_contract_id = "sif.test.bundle";
  hello.launch_uuid = MakeUuidV7Bytes();
  hello.listener_uuid = MakeUuidV7Bytes();
  hello.launch_generation = 1;
  hello.capability_bitmap[0] = 1;

  std::vector<std::uint8_t> out;
  PutUuid(&out, hello.parser_instance_uuid);
  PutUuid(&out, hello.parser_package_uuid);
  PutUuid(&out, hello.parser_family_uuid);
  PutUuid(&out, hello.dialect_profile_uuid);
  PutU32(&out, hello.parser_api_major);
  PutU32(&out, hello.parser_api_minor);
  PutString(&out, hello.protocol);
  PutString(&out, hello.profile_id);
  PutString(&out, hello.bundle_contract_id);
  PutBytes(&out, hello.resource_bundle_hash);
  PutUuid(&out, hello.launch_uuid);
  PutUuid(&out, hello.listener_uuid);
  PutU64(&out, hello.launch_generation);
  PutBytes(&out, hello.capability_bitmap);
  return out;
}

std::optional<HelloRequest> DecodeHelloRequest(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 16 * 4 + 4 + 4 + 32 + 16 + 16 + 8 + 32) return std::nullopt;
  std::size_t offset = 0;
  HelloRequest hello;
  hello.parser_instance_uuid = GetUuid(payload, offset);
  offset += 16;
  hello.parser_package_uuid = GetUuid(payload, offset);
  offset += 16;
  hello.parser_family_uuid = GetUuid(payload, offset);
  offset += 16;
  hello.dialect_profile_uuid = GetUuid(payload, offset);
  offset += 16;
  hello.parser_api_major = GetU32(payload, offset);
  offset += 4;
  hello.parser_api_minor = GetU32(payload, offset);
  offset += 4;
  if (!ReadString(payload, &offset, &hello.protocol)) return std::nullopt;
  if (!ReadString(payload, &offset, &hello.profile_id)) return std::nullopt;
  if (!ReadString(payload, &offset, &hello.bundle_contract_id)) return std::nullopt;
  if (offset + 32 + 16 + 16 + 8 + 32 > payload.size()) return std::nullopt;
  hello.resource_bundle_hash = GetBytes32(payload, offset);
  offset += 32;
  hello.launch_uuid = GetUuid(payload, offset);
  offset += 16;
  hello.listener_uuid = GetUuid(payload, offset);
  offset += 16;
  hello.launch_generation = GetU64(payload, offset);
  offset += 8;
  hello.capability_bitmap = GetBytes32(payload, offset);
  return hello;
}

std::vector<std::uint8_t> EncodeHelloAccept(const HelloAccept& accept) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, accept.server_uuid);
  PutUuid(&out, accept.channel_uuid);
  PutU16(&out, accept.protocol_minor);
  PutU32(&out, accept.max_frame_bytes);
  PutU32(&out, accept.max_streams);
  PutBytes(&out, accept.accepted_capability_bitmap);
  PutU64(&out, accept.server_policy_generation);
  PutUuid(&out, accept.registry_snapshot_uuid);
  PutU32(&out, accept.heartbeat_interval_ms);
  return out;
}

bool IsBuiltInTestHello(const HelloRequest& hello) {
  return !IsZeroUuid(hello.parser_instance_uuid) && hello.parser_api_major == 3 &&
         hello.protocol == "SBPS" && hello.profile_id == "sif.test" &&
         hello.bundle_contract_id == "sif.test.bundle" &&
         !HasUnknownCapabilityBits(hello.capability_bitmap);
}

ServerDiagnostic IpcDiagnostic(std::string code,
                               std::string key,
                               std::string safe_message,
                               std::vector<ServerDiagnosticField> fields) {
  return ServerDiagnostic{std::move(code),
                          std::move(key),
                          ServerDiagnosticSeverity::kError,
                          std::move(safe_message),
                          std::move(fields)};
}

}  // namespace scratchbird::server::sbps
