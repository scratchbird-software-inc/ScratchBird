// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "listener_diagnostics.hpp"

namespace scratchbird::listener {

enum class ListenerControlOpcode : std::uint16_t {
  kHello = 0x0001,
  kHelloAck = 0x0002,
  kSpawnRequest = 0x0010,
  kSpawnReady = 0x0011,
  kHandoffSocket = 0x0020,
  kHandoffAck = 0x0021,
  kHealthCheck = 0x0030,
  kHealthReport = 0x0031,
  kSessionBindingReport = 0x0032,
  kSessionBindingClear = 0x0033,
  kTakeoverRequest = 0x0034,
  kTakeoverDecision = 0x0035,
  kTakeoverProbe = 0x0036,
  kTakeoverProbeResult = 0x0037,
  kRecycle = 0x0050,
  kShutdown = 0x0051,
  kManagementCommand = 0x0060,
  kManagementResponse = 0x0061,
  kErrorMessage = 0x00FF,
};

constexpr std::uint16_t kControlFlagHasHandle = 0x0001;
constexpr std::uint16_t kControlFlagWindowsSocketInfo = 0x0002;
constexpr std::uint32_t kCurrentParserApiMajor = 1;
constexpr std::uint32_t kMinimumCompatibleParserApiMajor = 1;
constexpr std::uint32_t kMaximumCompatibleParserApiMajor = 1;

struct ListenerControlFrame {
  ListenerControlOpcode opcode{ListenerControlOpcode::kManagementCommand};
  std::uint16_t flags{0};
  std::uint64_t sequence{0};
  std::vector<std::uint8_t> payload;
  std::string payload_json;
};

constexpr std::uint32_t kListenerManagementRightRead = 0x00000001u;
constexpr std::uint32_t kListenerManagementRightLifecycle = 0x00000002u;
constexpr std::uint32_t kListenerManagementRightPool = 0x00000004u;
constexpr std::uint32_t kListenerManagementRightDbbt = 0x00000008u;
constexpr std::uint32_t kListenerManagementRightLpreface = 0x00000010u;
constexpr std::uint32_t kListenerManagementRightSupport = 0x00000020u;
constexpr std::uint32_t kListenerManagementRightsAll =
    kListenerManagementRightRead |
    kListenerManagementRightLifecycle |
    kListenerManagementRightPool |
    kListenerManagementRightDbbt |
    kListenerManagementRightLpreface |
    kListenerManagementRightSupport;

constexpr const char* kListenerManagementAuthHmacSha256 = "hmac-sha256";
constexpr const char* kListenerManagementAuthPeerOwner = "peer-owner-v1";

struct ListenerManagementArgument {
  std::string key;
  std::string value;
};

struct ListenerManagementEnvelope {
  std::uint16_t version{1};
  std::string operation;
  std::string role{"manager"};
  std::string authority_class{"listener_manager"};
  std::string request_id;
  std::vector<std::uint8_t> nonce;
  std::uint64_t issued_at_ms{0};
  std::uint64_t expires_at_ms{0};
  std::uint32_t rights{kListenerManagementRightsAll};
  std::string authenticator_scheme{kListenerManagementAuthPeerOwner};
  std::vector<std::uint8_t> authenticator;
  std::vector<ListenerManagementArgument> arguments;
};

struct ListenerControlDecodeResult {
  ListenerControlFrame frame;
  proto::MessageVectorSet messages;
  bool ok{false};
};

struct ParserHelloPayload {
  std::string protocol;
#ifndef _WIN32
  std::uint32_t pid{0};
#else
  std::uint32_t pid{1};
#endif
  std::uint64_t worker_id{0};
  std::uint32_t dialect_protocol_version{1};
  std::uint32_t parser_api_major{kCurrentParserApiMajor};
  std::string profile_id;
  std::string bundle_contract_id;
};

struct HelloAckPayload {
  bool accepted{false};
  std::uint32_t supported_parser_api_floor{kMinimumCompatibleParserApiMajor};
  std::uint32_t supported_parser_api_ceiling{kMaximumCompatibleParserApiMajor};
  std::string reason;
};

struct HandoffSocketPayload {
  std::uint64_t connection_id{0};
  std::string protocol;
  std::string client_addr;
  std::uint16_t client_port{0};
  bool tls_active{false};
  std::array<std::uint8_t, 64> tls_state{};
  std::array<std::uint8_t, 16> db_uuid{};
  std::array<std::uint8_t, 16> dbbt_id{};
  std::array<std::uint8_t, 16> manager_session_id{};
  std::uint32_t listener_id{1};
  std::string auth_provider_family;
  std::string auth_principal;
  std::string auth_token;
};

struct HandoffAckPayload {
  std::uint64_t connection_id_echo{0};
  bool accepted{false};
  std::string reason;
};

struct WindowsSocketHandoffPayload {
  HandoffSocketPayload handoff;
  std::vector<std::uint8_t> socket_protocol_info;
};

struct HealthReportPayload {
  std::uint64_t request_id_echo{0};
  std::uint8_t state{0};
  std::uint32_t last_error{0};
};

struct SessionBindingReportPayload {
  std::array<std::uint8_t, 16> attachment_id{};
  std::array<std::uint8_t, 16> catalog_session_id{};
  std::array<std::uint8_t, 16> transaction_uuid{};
  std::array<std::uint8_t, 16> protocol_session_id{};
  std::array<std::uint8_t, 16> authenticated_principal_id{};
  std::array<std::uint8_t, 16> session_user_id{};
  std::array<std::uint8_t, 16> active_role_id{};
  std::array<std::uint8_t, 16> authkey_id{};
  std::uint64_t current_txn_id{0};
  std::vector<std::array<std::uint8_t, 16>> effective_group_ids;
};

constexpr std::uint16_t kTakeoverClaimAttachmentId = 0x0001;
constexpr std::uint16_t kTakeoverClaimCatalogSessionId = 0x0002;
constexpr std::uint16_t kTakeoverClaimProtocolSessionId = 0x0004;
constexpr std::uint16_t kTakeoverClaimAuthkeyId = 0x0008;
constexpr std::uint16_t kTakeoverClaimAuthenticatedPrincipalId = 0x0010;
constexpr std::uint16_t kTakeoverClaimSessionUserId = 0x0020;
constexpr std::uint16_t kTakeoverClaimActiveRoleId = 0x0040;
constexpr std::uint16_t kTakeoverClaimCurrentTxnId = 0x0080;

struct TakeoverRequestPayload {
  std::uint16_t mask{0};
  std::array<std::uint8_t, 16> attachment_id{};
  std::array<std::uint8_t, 16> catalog_session_id{};
  std::array<std::uint8_t, 16> protocol_session_id{};
  std::array<std::uint8_t, 16> authkey_id{};
  std::array<std::uint8_t, 16> authenticated_principal_id{};
  std::array<std::uint8_t, 16> session_user_id{};
  std::array<std::uint8_t, 16> active_role_id{};
  std::uint64_t current_txn_id{0};
  std::vector<std::array<std::uint8_t, 16>> group_ids;
};

struct TakeoverDecisionPayload {
  bool allowed{false};
  std::string reason;
};

struct TakeoverProbeResultPayload {
  std::uint8_t flags{0};
};

struct ErrorMessagePayload {
  std::string reason;
};

std::vector<std::uint8_t> EncodeControlFrame(const ListenerControlFrame& frame);
ListenerControlDecodeResult DecodeControlFrame(const std::vector<std::uint8_t>& bytes);
std::string ControlOpcodeName(ListenerControlOpcode opcode);
bool IsListenerManagementEnvelopePayload(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> EncodeListenerManagementEnvelope(const ListenerManagementEnvelope& envelope);
std::vector<std::uint8_t> ListenerManagementEnvelopeSigningBody(const ListenerManagementEnvelope& envelope);
void SignListenerManagementEnvelopeHmacSha256(ListenerManagementEnvelope* envelope,
                                              const proto::Bytes& key);
std::optional<ListenerManagementEnvelope> DecodeListenerManagementEnvelope(
    const std::vector<std::uint8_t>& payload,
    proto::MessageVectorSet* messages);
std::optional<ListenerManagementEnvelope> BuildListenerManagementEnvelopeFromCommand(
    const std::string& command,
    std::uint64_t request_id,
    std::uint64_t issued_at_ms,
    std::uint64_t expires_at_ms,
    const std::string& role = "manager",
    const std::string& authority_class = "listener_manager",
    const std::string& authenticator_scheme = kListenerManagementAuthPeerOwner);

std::vector<std::uint8_t> EncodeHelloPayload(const ParserHelloPayload& hello);
std::optional<ParserHelloPayload> DecodeHelloPayload(const std::vector<std::uint8_t>& payload,
                                                     proto::MessageVectorSet* messages);
std::vector<std::uint8_t> EncodeHelloAckPayload(const HelloAckPayload& ack);
std::optional<HelloAckPayload> DecodeHelloAckPayload(const std::vector<std::uint8_t>& payload,
                                                     proto::MessageVectorSet* messages);
std::vector<std::uint8_t> EncodeHandoffSocketPayload(const HandoffSocketPayload& handoff);
std::optional<HandoffSocketPayload> DecodeHandoffSocketPayload(const std::vector<std::uint8_t>& payload,
                                                               proto::MessageVectorSet* messages);
std::vector<std::uint8_t> EncodeWindowsSocketHandoffPayload(
    const HandoffSocketPayload& handoff,
    const std::uint8_t* socket_protocol_info,
    std::size_t socket_protocol_info_size);
std::optional<WindowsSocketHandoffPayload> DecodeWindowsSocketHandoffPayload(
    const std::vector<std::uint8_t>& payload,
    proto::MessageVectorSet* messages);
std::optional<HandoffAckPayload> DecodeHandoffAckPayload(const std::vector<std::uint8_t>& payload,
                                                         proto::MessageVectorSet* messages);
std::vector<std::uint8_t> EncodeHandoffAckPayload(const HandoffAckPayload& ack);
std::vector<std::uint8_t> EncodeHealthReportPayload(const HealthReportPayload& report);
std::optional<HealthReportPayload> DecodeHealthReportPayload(const std::vector<std::uint8_t>& payload,
                                                             proto::MessageVectorSet* messages);
std::vector<std::uint8_t> EncodeSessionBindingReportPayload(const SessionBindingReportPayload& report);
std::optional<SessionBindingReportPayload> DecodeSessionBindingReportPayload(
    const std::vector<std::uint8_t>& payload,
    proto::MessageVectorSet* messages);
std::vector<std::uint8_t> EncodeTakeoverRequestPayload(const TakeoverRequestPayload& request);
std::optional<TakeoverRequestPayload> DecodeTakeoverRequestPayload(const std::vector<std::uint8_t>& payload,
                                                                   proto::MessageVectorSet* messages);
std::vector<std::uint8_t> EncodeTakeoverDecisionPayload(const TakeoverDecisionPayload& decision);
std::optional<TakeoverDecisionPayload> DecodeTakeoverDecisionPayload(const std::vector<std::uint8_t>& payload,
                                                                     proto::MessageVectorSet* messages);
std::vector<std::uint8_t> EncodeTakeoverProbeResultPayload(const TakeoverProbeResultPayload& result);
std::optional<TakeoverProbeResultPayload> DecodeTakeoverProbeResultPayload(
    const std::vector<std::uint8_t>& payload,
    proto::MessageVectorSet* messages);
std::vector<std::uint8_t> EncodeRecyclePayload(std::uint16_t reason_code);
std::optional<std::uint16_t> DecodeRecyclePayload(const std::vector<std::uint8_t>& payload,
                                                  proto::MessageVectorSet* messages);
std::vector<std::uint8_t> EncodeErrorMessagePayload(const ErrorMessagePayload& error);
std::optional<ErrorMessagePayload> DecodeErrorMessagePayload(const std::vector<std::uint8_t>& payload,
                                                             proto::MessageVectorSet* messages);

#ifdef _WIN32
bool SendControlFrame(std::intptr_t fd,
                      const ListenerControlFrame& frame,
                      std::intptr_t fd_to_transfer = -1);
bool ReadControlFrame(std::intptr_t fd,
                      ListenerControlDecodeResult* decoded,
                      std::intptr_t* received_fd,
                      std::uint32_t timeout_ms);
bool ReadControlFrame(std::intptr_t fd,
                      ListenerControlDecodeResult* decoded,
                      int* received_fd,
                      std::uint32_t timeout_ms);
#else
bool SendControlFrame(int fd, const ListenerControlFrame& frame, int fd_to_transfer = -1);
bool ReadControlFrame(int fd,
                      ListenerControlDecodeResult* decoded,
                      int* received_fd,
                      std::uint32_t timeout_ms);
#endif

} // namespace scratchbird::listener
