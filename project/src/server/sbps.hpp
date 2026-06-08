// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_IPC_FOUNDATION_SBPS

#pragma once

#include "diagnostics.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::server::sbps {

constexpr std::uint32_t kFrameMagic = 0x53504253;  // SBPS
constexpr std::uint32_t kMessageVectorMagic = 0x564d4253;  // SBMV
constexpr std::uint16_t kHeaderBytes = 96;
constexpr std::uint16_t kProtocolMajor = 1;
constexpr std::uint16_t kProtocolMinor = 0;
constexpr std::uint16_t kProtocolMajorMinSupported = 1;
constexpr std::uint16_t kProtocolMajorMaxSupported = 1;
constexpr std::uint16_t kProtocolMinorMinSupported = 0;
constexpr std::uint16_t kProtocolMinorMaxSupported = 0;
constexpr std::uint32_t kAllowedFrameFlags = 0x000003ff;
constexpr std::uint32_t kFlagResponse = 1u << 0;
constexpr std::uint32_t kFlagError = 1u << 1;
constexpr std::uint32_t kFlagFinal = 1u << 2;
constexpr std::uint32_t kFlagPayloadChunk = 1u << 3;
constexpr std::uint32_t kSchemaNone = 0;
constexpr std::uint32_t kSchemaHelloRequestV1 = 1001;
constexpr std::uint32_t kSchemaHelloAcceptV1 = 1002;
constexpr std::uint32_t kSchemaMessageVectorSetV1 = 2001;
constexpr std::uint32_t kSchemaManagementRequestV1 = 6001;
constexpr std::uint32_t kSchemaManagementResponseV1 = 6002;
constexpr std::uint32_t kSchemaResolveNameRequestV1 = 7001;
constexpr std::uint32_t kSchemaResolveNameResultV1 = 7002;
constexpr std::uint32_t kSchemaRenderUuidRequestV1 = 7003;
constexpr std::uint32_t kSchemaRenderUuidResultV1 = 7004;
constexpr std::uint32_t kSchemaEventSubscribeRequestV1 = 5001;
constexpr std::uint32_t kSchemaEventSubscribeResultV1 = 5002;
constexpr std::uint32_t kSchemaEventUnsubscribeRequestV1 = 5003;
constexpr std::uint32_t kSchemaEventUnsubscribeResultV1 = 5004;
constexpr std::uint32_t kSchemaEventNotificationV1 = 5005;
constexpr std::uint32_t kSchemaEventAckV1 = 5006;
constexpr std::uint32_t kSchemaEventBackpressureV1 = 5007;

enum class MessageType : std::uint16_t {
  kReserved = 0,
  kHello = 1,
  kHelloAccept = 2,
  kHelloReject = 3,
  kAuthHandoff = 10,
  kAuthResult = 11,
  kAuthChallenge = 12,
  kAttachDatabase = 20,
  kAttachResult = 21,
  kManagementRequest = 30,
  kManagementResult = 31,
  kResolveNameRequest = 32,
  kResolveNameResult = 33,
  kRenderUuidRequest = 34,
  kRenderUuidResult = 35,
  kPrepareSblr = 40,
  kPrepareResult = 41,
  kExecuteSblr = 42,
  kExecuteResult = 43,
  kFetch = 44,
  kFetchResult = 45,
  kCloseCursor = 46,
  kCloseCursorResult = 47,
  kDiagnostic = 60,
  kPing = 70,
  kPong = 71,
  kDisconnectNotice = 74,
  kEventSubscribeRequest = 80,
  kEventSubscribeResult = 81,
  kEventUnsubscribeRequest = 82,
  kEventUnsubscribeResult = 83,
  kEventNotification = 84,
  kEventAck = 85,
  kEventBackpressure = 86,
  kEventSubscriptionInvalidate = 87,
  kEventChannelClosed = 88,
};

struct FrameHeader {
  std::uint16_t protocol_major = kProtocolMajor;
  std::uint16_t protocol_minor = kProtocolMinor;
  std::uint16_t message_type = 0;
  std::uint32_t flags = 0;
  std::uint32_t payload_schema_id = 0;
  std::uint32_t payload_len = 0;
  std::uint32_t header_crc32c = 0;
  std::uint32_t payload_crc32c = 0;
  std::uint64_t stream_id = 0;
  std::uint64_t sequence_number = 1;
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
};

struct Frame {
  FrameHeader header;
  std::vector<std::uint8_t> payload;
};

struct DecodeResult {
  std::optional<Frame> frame;
  std::vector<ServerDiagnostic> diagnostics;
  bool ok() const { return diagnostics.empty() && frame.has_value(); }
};

struct ChunkAssemblyResult {
  std::optional<Frame> frame;
  std::vector<ServerDiagnostic> diagnostics;
  bool ok() const { return diagnostics.empty() && frame.has_value(); }
};

struct HelloRequest {
  std::array<std::uint8_t, 16> parser_instance_uuid{};
  std::array<std::uint8_t, 16> parser_package_uuid{};
  std::array<std::uint8_t, 16> parser_family_uuid{};
  std::array<std::uint8_t, 16> dialect_profile_uuid{};
  std::uint32_t parser_api_major = 0;
  std::uint32_t parser_api_minor = 0;
  std::string protocol;
  std::string profile_id;
  std::string bundle_contract_id;
  std::array<std::uint8_t, 32> resource_bundle_hash{};
  std::array<std::uint8_t, 16> launch_uuid{};
  std::array<std::uint8_t, 16> listener_uuid{};
  std::uint64_t launch_generation = 0;
  std::array<std::uint8_t, 32> capability_bitmap{};
};

struct HelloAccept {
  std::array<std::uint8_t, 16> server_uuid{};
  std::array<std::uint8_t, 16> channel_uuid{};
  std::uint16_t protocol_minor = kProtocolMinor;
  std::uint32_t max_frame_bytes = 0;
  std::uint32_t max_streams = 0;
  std::array<std::uint8_t, 32> accepted_capability_bitmap{};
  std::uint64_t server_policy_generation = 1;
  std::array<std::uint8_t, 16> registry_snapshot_uuid{};
  std::uint32_t heartbeat_interval_ms = 30000;
};

std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size);
std::array<std::uint8_t, 16> MakeUuidV7Bytes();
bool IsZeroUuid(const std::array<std::uint8_t, 16>& uuid);
bool IsKnownMessageType(std::uint16_t message_type);
bool HasUnknownCapabilityBits(const std::array<std::uint8_t, 32>& capability_bitmap);

std::vector<std::uint8_t> EncodeFrame(const FrameHeader& header,
                                      const std::vector<std::uint8_t>& payload);
std::vector<std::vector<std::uint8_t>> EncodeFrameSequence(
    const FrameHeader& header,
    const std::vector<std::uint8_t>& payload,
    std::uint64_t max_payload_bytes);
DecodeResult DecodeFrameBytes(const std::vector<std::uint8_t>& bytes,
                              std::uint32_t max_payload_bytes);
ChunkAssemblyResult AssembleDecodedChunkSequence(const std::vector<Frame>& chunks,
                                                 std::uint64_t max_total_payload_bytes);
std::optional<std::uint32_t> PayloadLengthFromHeader(const std::vector<std::uint8_t>& header_bytes);

std::vector<std::uint8_t> EncodeMessageVectorSet(
    const std::vector<ServerDiagnostic>& diagnostics,
    const std::array<std::uint8_t, 16>& request_uuid);
std::vector<std::string> DecodeMessageVectorDiagnosticCodes(const std::vector<std::uint8_t>& payload);

std::vector<std::uint8_t> EncodeHelloRequestForTest();
std::optional<HelloRequest> DecodeHelloRequest(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> EncodeHelloAccept(const HelloAccept& accept);
bool IsBuiltInTestHello(const HelloRequest& hello);

ServerDiagnostic IpcDiagnostic(std::string code,
                               std::string key,
                               std::string safe_message,
                               std::vector<ServerDiagnosticField> fields = {});

}  // namespace scratchbird::server::sbps
