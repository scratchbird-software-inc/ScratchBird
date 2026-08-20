// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "sblr_engine_envelope.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

// IA10C-EVENT-NOTIFICATION-WIRE-V1
enum class SblrEventNotificationOpcode : std::uint16_t {
  channel_create = 0x0f00, channel_alter = 0x0f01, channel_drop = 0x0f02,
  channel_listen = 0x0f03, channel_unlisten = 0x0f04,
  channel_unlisten_all = 0x0f05, channel_notify = 0x0f06,
  subscription_list = 0x0f07, delivery_poll = 0x0f08,
  delivery_ack = 0x0f09,
};

struct SblrEventNotificationField { std::uint16_t tag = 0; std::vector<std::uint8_t> value; };
struct SblrEventNotificationRecord {
  SblrEventNotificationOpcode opcode = SblrEventNotificationOpcode::channel_create;
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> security_context_uuid{};
  std::uint64_t policy_epoch = 0;
  std::uint64_t transaction_id = 0;
  std::vector<SblrEventNotificationField> fields;
};
struct SblrEventNotificationCodecResult {
  bool ok = false; SblrEventNotificationRecord record; std::vector<std::uint8_t> canonical_bytes;
  std::string sha256_hex; std::string diagnostic_id; std::string detail;
};
struct SblrEventNotificationDispatchResult {
  bool accepted = false; SblrEventNotificationRecord record; std::string diagnostic_id; std::string detail;
  std::vector<scratchbird::engine::internal_api::EngineEvidenceReference> evidence;
};

bool IsSblrEventNotificationOperation(std::string_view operation_id) noexcept;
std::string_view SblrEventNotificationOperationId(SblrEventNotificationOpcode opcode) noexcept;
std::string_view SblrEventNotificationMnemonic(SblrEventNotificationOpcode opcode) noexcept;
std::string_view SblrEventNotificationOperandType(SblrEventNotificationOpcode opcode) noexcept;
SblrEventNotificationCodecResult EncodeSblrEventNotificationRecord(const SblrEventNotificationRecord& record);
SblrEventNotificationCodecResult DecodeSblrEventNotificationRecord(const std::uint8_t* data, std::size_t size);
SblrEventNotificationCodecResult DecodeSblrEventNotificationOperand(const SblrOperationEnvelope& envelope);
SblrOperand MakeSblrEventNotificationOperand(const SblrEventNotificationCodecResult& encoded);
SblrEventNotificationDispatchResult DispatchSblrEventNotification(const SblrOperationEnvelope& envelope, const scratchbird::engine::internal_api::EngineRequestContext& context);

}  // namespace scratchbird::engine::sblr
