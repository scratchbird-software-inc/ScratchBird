// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "message_vector_value_codec.hpp"

#include <array>

namespace scratchbird::wire::message_vector {
using MessageUuid=std::array<std::uint8_t,16>;
struct MessageRecord {
  std::uint8_t message_class=0;
  std::uint8_t severity=2;
  std::uint32_t flags=1;
  MessageUuid vector_uuid{};
  MessageUuid canonical_source_uuid{};
  MessageUuid request_uuid{};
  MessageUuid correlation_uuid{};
  std::uint64_t policy_generation=0;
  std::uint32_t source_component=0;
  // language, diagnostic code, safe message key, admin detail key, rendered text.
  std::array<std::string,5> text;
  std::uint8_t retryability=0;
  std::uint8_t redaction_state=0;
  ValueTlv context{"$context",ValueType::context,{}};
  std::vector<ValueTlv> parameters;
  std::vector<ValueTlv> details;
  std::vector<ValueTlv> causes;
  bool operator==(const MessageRecord&) const = default;
};
struct MessageSet {
  std::uint32_t flags=0;
  std::uint64_t registry_generation=0;
  MessageUuid set_uuid{};
  std::uint32_t max_render_bytes=0;
  std::vector<MessageRecord> records;
  bool operator==(const MessageSet&) const = default;
};

// Exact Core SBMV v1 structural codecs. Source/template/descriptor registration
// and the active channel's authorization remain bridge/consumer obligations.
// Caller output is unchanged on every failure, including allocation failure.
[[nodiscard]] ValueCodecError EncodeMessageSet(const MessageSet& input,
    std::vector<std::uint8_t>* output) noexcept;
[[nodiscard]] ValueCodecError DecodeMessageSet(std::span<const std::uint8_t> input,
    MessageSet* output) noexcept;
} // namespace scratchbird::wire::message_vector
