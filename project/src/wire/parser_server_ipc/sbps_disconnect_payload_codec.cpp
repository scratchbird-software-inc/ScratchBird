// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbps_disconnect_payload_codec.hpp"

#include <algorithm>
#include <new>

namespace scratchbird::parser::ipc {
namespace {
using Bytes = std::span<const std::uint8_t>;
using Status = PsDisconnectCodecStatusV1;
using Diagnostic = PsDisconnectCodecDiagnosticV1;

std::uint16_t U16(Bytes b) {
  return static_cast<std::uint16_t>(b[0]) |
         (static_cast<std::uint16_t>(b[1]) << 8);
}
std::uint32_t U32(Bytes b) {
  return static_cast<std::uint32_t>(b[0]) |
         (static_cast<std::uint32_t>(b[1]) << 8) |
         (static_cast<std::uint32_t>(b[2]) << 16) |
         (static_cast<std::uint32_t>(b[3]) << 24);
}
void Put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
  b.push_back(static_cast<std::uint8_t>(v));
  b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void Put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
  for (unsigned n = 0; n < 32; n += 8)
    b.push_back(static_cast<std::uint8_t>(v >> n));
}
void Field(std::vector<std::uint8_t>& b, std::uint16_t id, std::uint32_t size) {
  Put16(b, id);
  Put32(b, size);
}
bool SystemUuid(Bytes b) {
  return b.size() == 16 && (b[6] & 0xf0) == 0x70 &&
         (b[8] & 0xc0) == 0x80;
}
bool Less(Bytes left, Bytes right) {
  return std::lexicographical_compare(left.begin(), left.end(),
                                      right.begin(), right.end());
}
PsDisconnectLimitsV1 Effective(PsDisconnectLimitsV1 v) {
  const PsDisconnectLimitsV1 ceiling;
  v.maximum_payload_bytes = std::min(v.maximum_payload_bytes, ceiling.maximum_payload_bytes);
  v.maximum_message_vector_bytes = std::min(v.maximum_message_vector_bytes, ceiling.maximum_message_vector_bytes);
  v.maximum_active_requests = std::min(v.maximum_active_requests, ceiling.maximum_active_requests);
  v.maximum_finality_tokens = std::min(v.maximum_finality_tokens, ceiling.maximum_finality_tokens);
  v.maximum_finality_token_bytes = std::min(v.maximum_finality_token_bytes, ceiling.maximum_finality_token_bytes);
  return v;
}
Diagnostic ValidateFields(const std::array<Bytes, 5>& f,
                          const PsDisconnectLimitsV1& limits) {
  if (f[0].size() != 1 || f[0][0] < 1 || f[0][0] > 7)
    return {Status::frame_payload_invalid, 1};
  if (f[1].size() != 1 || f[1][0] < 1 || f[1][0] > 4)
    return {Status::frame_payload_invalid, 2};
  if (f[2].size() < 2) return {Status::frame_payload_invalid, 3};
  const auto active_count = U16(f[2]);
  if (active_count > limits.maximum_active_requests)
    return {Status::resource_limit_exceeded, 3};
  if (f[2].size() != 2u + 16u * active_count)
    return {Status::frame_payload_invalid, 3};
  Bytes previous;
  for (unsigned i = 0; i < active_count; ++i) {
    const auto id = f[2].subspan(2 + 16 * i, 16);
    if (!SystemUuid(id) || (i != 0 && !Less(previous, id)))
      return {Status::frame_payload_invalid, 3};
    previous = id;
  }
  if (f[3].size() < 2) return {Status::frame_payload_invalid, 4};
  const auto token_count = U16(f[3]);
  if (token_count > limits.maximum_finality_tokens)
    return {Status::resource_limit_exceeded, 4};
  std::size_t offset = 2;
  for (unsigned i = 0; i < token_count; ++i) {
    if (f[3].size() - offset < 4) return {Status::frame_payload_invalid, 4};
    const auto size = U32(f[3].subspan(offset));
    offset += 4;
    if (size > limits.maximum_finality_token_bytes)
      return {Status::resource_limit_exceeded, 4};
    if (size == 0 || size > f[3].size() - offset)
      return {Status::frame_payload_invalid, 4};
    offset += size;
  }
  if (offset != f[3].size()) return {Status::frame_payload_invalid, 4};
  if (f[4].size() > limits.maximum_message_vector_bytes)
    return {Status::resource_limit_exceeded, 5};
  return {Status::ok, 0};
}
}  // namespace

PsDisconnectDecodeResultV1 DecodePsDisconnectPayloadV1(
    Bytes bytes, const PsDisconnectLimitsV1& policy) {
  const auto limits = Effective(policy);
  if (bytes.size() > limits.maximum_payload_bytes)
    return {{Status::resource_limit_exceeded, 0}, {}};
  if (bytes.size() < 2 || U16(bytes) != 1) return {};
  std::array<Bytes, 5> fields;
  std::size_t offset = 2;
  for (std::uint16_t id = 1; id <= 5; ++id) {
    if (bytes.size() - offset < 6 || U16(bytes.subspan(offset)) != id)
      return {{Status::frame_payload_invalid, id}, {}};
    const auto size = U32(bytes.subspan(offset + 2));
    offset += 6;
    // A declared over-policy component is rejected without allocating it,
    // including when the sender omitted the declared oversized body.
    if ((id == 3 && size > 2ull + 16ull * limits.maximum_active_requests) ||
        (id == 4 && size > 2ull +
             (4ull + limits.maximum_finality_token_bytes) * limits.maximum_finality_tokens) ||
        (id == 5 && size > limits.maximum_message_vector_bytes))
      return {{Status::resource_limit_exceeded, id}, {}};
    if (size > bytes.size() - offset)
      return {{Status::frame_payload_invalid, id}, {}};
    fields[id - 1] = bytes.subspan(offset, size);
    offset += size;
  }
  if (offset != bytes.size()) return {{Status::frame_payload_invalid, 0}, {}};
  const auto diagnostic = ValidateFields(fields, limits);
  if (!diagnostic.ok()) return {diagnostic, {}};
  try {
    PsDisconnectPayloadV1 payload;
    payload.reason = static_cast<PsDisconnectReasonV1>(fields[0][0]);
    payload.initiator = static_cast<PsDisconnectInitiatorV1>(fields[1][0]);
    payload.active_requests.resize(U16(fields[2]));
    for (std::size_t i = 0; i < payload.active_requests.size(); ++i)
      std::copy_n(fields[2].begin() + 2 + 16 * i, 16, payload.active_requests[i].begin());
    payload.finality_tokens.reserve(U16(fields[3]));
    offset = 2;
    for (unsigned i = 0; i < U16(fields[3]); ++i) {
      const auto size = U32(fields[3].subspan(offset));
      offset += 4;
      const auto token = fields[3].subspan(offset, size);
      payload.finality_tokens.emplace_back(token.begin(), token.end());
      offset += size;
    }
    payload.message_vector_set.assign(fields[4].begin(), fields[4].end());
    return {{Status::ok, 0}, std::move(payload)};
  } catch (const std::bad_alloc&) {
    return {{Status::resource_limit_exceeded, 0}, {}};
  }
}

PsDisconnectEncodeResultV1 EncodePsDisconnectPayloadV1(
    const PsDisconnectPayloadV1& payload, const PsDisconnectLimitsV1& policy) {
  const auto limits = Effective(policy);
  const auto reason = static_cast<std::uint8_t>(payload.reason);
  const auto initiator = static_cast<std::uint8_t>(payload.initiator);
  if (reason < 1 || reason > 7) return {{Status::frame_payload_invalid, 1}, {}};
  if (initiator < 1 || initiator > 4) return {{Status::frame_payload_invalid, 2}, {}};
  if (payload.active_requests.size() > limits.maximum_active_requests)
    return {{Status::resource_limit_exceeded, 3}, {}};
  for (std::size_t i = 0; i < payload.active_requests.size(); ++i)
    if (!SystemUuid(payload.active_requests[i]) ||
        (i != 0 && !Less(payload.active_requests[i - 1], payload.active_requests[i])))
      return {{Status::frame_payload_invalid, 3}, {}};
  if (payload.finality_tokens.size() > limits.maximum_finality_tokens)
    return {{Status::resource_limit_exceeded, 4}, {}};
  std::uint64_t token_bytes = 2;
  for (const auto& token : payload.finality_tokens) {
    if (token.empty()) return {{Status::frame_payload_invalid, 4}, {}};
    if (token.size() > limits.maximum_finality_token_bytes)
      return {{Status::resource_limit_exceeded, 4}, {}};
    token_bytes += 4 + token.size();
  }
  if (payload.message_vector_set.size() > limits.maximum_message_vector_bytes)
    return {{Status::resource_limit_exceeded, 5}, {}};
  const auto active_bytes = 2 + 16 * payload.active_requests.size();
  const std::uint64_t total = 2 + 5 * 6 + 2 + active_bytes + token_bytes +
                              payload.message_vector_set.size();
  if (total > limits.maximum_payload_bytes)
    return {{Status::resource_limit_exceeded, 0}, {}};
  try {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(total));
    Put16(bytes, 1);
    Field(bytes, 1, 1); bytes.push_back(reason);
    Field(bytes, 2, 1); bytes.push_back(initiator);
    Field(bytes, 3, static_cast<std::uint32_t>(active_bytes));
    Put16(bytes, static_cast<std::uint16_t>(payload.active_requests.size()));
    for (const auto& uuid : payload.active_requests)
      bytes.insert(bytes.end(), uuid.begin(), uuid.end());
    Field(bytes, 4, static_cast<std::uint32_t>(token_bytes));
    Put16(bytes, static_cast<std::uint16_t>(payload.finality_tokens.size()));
    for (const auto& token : payload.finality_tokens) {
      Put32(bytes, static_cast<std::uint32_t>(token.size()));
      bytes.insert(bytes.end(), token.begin(), token.end());
    }
    Field(bytes, 5, static_cast<std::uint32_t>(payload.message_vector_set.size()));
    bytes.insert(bytes.end(), payload.message_vector_set.begin(), payload.message_vector_set.end());
    return {{Status::ok, 0}, std::move(bytes)};
  } catch (const std::bad_alloc&) {
    return {{Status::resource_limit_exceeded, 0}, {}};
  }
}
}  // namespace scratchbird::parser::ipc
