// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "openpgp_packet.hpp"
#include <openssl/crypto.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace scratchbird::core::crypto {
namespace {
constexpr std::size_t kSlice = 4096;
bool Extent(PgpInput input) noexcept {
  const auto at = reinterpret_cast<std::uintptr_t>(input.data);
  return (!input.size || input.data) &&
      input.size <= std::numeric_limits<std::uintptr_t>::max() - at;
}
bool Disjoint(PgpInput input, PgpOutput output) noexcept {
  if (!Extent(input) || !Extent({output.data, output.size})) return false;
  if (!input.size || !output.size) return true;
  const auto a = reinterpret_cast<std::uintptr_t>(input.data);
  const auto b = reinterpret_cast<std::uintptr_t>(output.data);
  return a >= b + output.size || b >= a + input.size;
}
bool Cancelled(PgpCancellation probe) {
  return probe.requested && probe.requested(probe.context);
}
struct Pending {
  PgpOutput output;
  bool accepted = false;
  ~Pending() { if (!accepted && output.size) OPENSSL_cleanse(output.data, output.size); }
};
std::size_t LengthBytes(std::size_t size) noexcept {
  return size < 192 ? 1 : size < 8384 ? 2 : 5;
}
std::size_t Header(std::uint8_t type, std::size_t size, std::uint8_t* out) noexcept {
  out[0] = 0xc0 | type;
  if (size < 192) { out[1] = static_cast<std::uint8_t>(size); return 2; }
  if (size < 8384) {
    out[1] = static_cast<std::uint8_t>(((size - 192) >> 8) + 192);
    out[2] = static_cast<std::uint8_t>(size - 192); return 3;
  }
  out[1] = 255;
  for (unsigned i = 0; i < 4; ++i)
    out[2 + i] = static_cast<std::uint8_t>(size >> (24 - 8*i));
  return 6;
}
struct Segment { std::size_t size = 0; bool partial = false; };
bool ReadLength(PgpInput in, std::size_t& at, Segment& out) noexcept {
  if (at >= in.size) return false;
  const auto first = in.data[at++];
  out.partial = false;
  if (first < 192) { out.size = first; return true; }
  if (first < 224) {
    if (at == in.size) return false;
    out.size = (std::size_t(first) - 192)*256 + in.data[at++] + 192;
    return true;
  }
  if (first < 255) {
    out.partial = true; out.size = std::size_t{1} << (first & 31); return true;
  }
  if (in.size - at < 4) return false;
  std::uint32_t size = 0;
  for (unsigned i = 0; i < 4; ++i) size = (size << 8) | in.data[at++];
  out.size = size; return true;
}
// Reuse a single validated scanner for size inspection and reassembly. Large
// definite payloads are skipped O(1) during inspection; attacker-controlled tiny
// partial-segment chains poll at each length header.
PgpPacketDescription Scan(PgpInput in, PgpOutput* output, PgpCancellation probe) {
  if (!Extent(in)) return {PgpCode::invalid_extent};
  if (Cancelled(probe)) return {PgpCode::cancelled};
  if (in.size < 2 || (in.data[0] & 0xc0) != 0xc0 || !(in.data[0] & 63)) return {};
  const auto type = static_cast<std::uint8_t>(in.data[0] & 63);
  const bool allow_partial = type == 8 || type == 9 || type == 11 || type == 18;
  std::size_t at = 1, total = 0;
  bool partial = false, first = true;
  for (;;) {
    if (Cancelled(probe)) return {PgpCode::cancelled};
    Segment segment;
    if (!ReadLength(in, at, segment) || segment.size > in.size - at ||
        (segment.partial && (!allow_partial || (first && segment.size < 512)))) return {};
    // Each segment occupies distinct input, so total cannot exceed in.size;
    // retain an explicit addition guard as part of the extent contract.
    if (segment.size > std::numeric_limits<std::size_t>::max() - total)
      return {PgpCode::size_overflow};
    if (output) {
      if (total > output->size || segment.size > output->size - total) return {};
      for (std::size_t copied = 0; copied < segment.size;) {
        if (Cancelled(probe)) return {PgpCode::cancelled};
        const auto n = std::min(kSlice, segment.size - copied);
        std::memcpy(output->data + total + copied, in.data + at + copied, n);
        copied += n;
      }
    }
    total += segment.size; at += segment.size;
    partial |= segment.partial; first = false;
    if (!segment.partial) break;
  }
  if (Cancelled(probe)) return {PgpCode::cancelled};
  return {PgpCode::ok, type, total, at, partial};
}
PgpCode Copy(PgpInput in, std::uint8_t* out, PgpCancellation probe) {
  for (std::size_t at = 0; at < in.size;) {
    if (Cancelled(probe)) return PgpCode::cancelled;
    const auto n = std::min(kSlice, in.size - at);
    std::memcpy(out + at, in.data + at, n); at += n;
  }
  return PgpCode::ok;
}
}  // namespace

PgpSize PacketEncodedSize(std::uint8_t type, std::size_t bytes) noexcept {
  if (!type || type > 63) return {PgpCode::invalid_profile, 0};
  if (bytes > std::numeric_limits<std::uint32_t>::max() ||
      bytes > std::numeric_limits<std::size_t>::max() - 1 - LengthBytes(bytes))
    return {PgpCode::size_overflow, 0};
  return {PgpCode::ok, bytes + 1 + LengthBytes(bytes)};
}
PgpPacketDescription InspectPacket(PgpInput input, PgpCancellation probe) {
  return Scan(input, nullptr, probe);
}
PgpCode EncodePacket(std::uint8_t type, PgpInput input, PgpOutput output,
                     PgpCancellation probe) {
  const auto size = PacketEncodedSize(type, input.size);
  if (size.code != PgpCode::ok) return size.code;
  if (output.size != size.bytes || !Disjoint(input, output)) return PgpCode::invalid_extent;
  Pending pending{output};
  if (Cancelled(probe)) return PgpCode::cancelled;
  const auto header = Header(type, input.size, output.data);
  const auto code = Copy(input, output.data + header, probe);
  if (code != PgpCode::ok) return code;
  if (Cancelled(probe)) return PgpCode::cancelled;
  pending.accepted = true; return PgpCode::ok;
}
PgpCode CopyPacketBody(PgpInput input, PgpOutput output, PgpCancellation probe) {
  if (!Disjoint(input, output)) return PgpCode::invalid_extent;
  // Shape is established without effects first. Cancellation during this pass
  // leaves output untouched because no private output has yet been accepted.
  const auto description = Scan(input, nullptr, probe);
  if (description.code != PgpCode::ok) return description.code;
  if (output.size != description.body_bytes) return PgpCode::invalid_extent;
  Pending pending{output};
  const auto copied = Scan(input, &output, probe);
  if (copied.code != PgpCode::ok) return copied.code;
  pending.accepted = true; return PgpCode::ok;
}
PgpLiteralDescription InspectLiteralBody(PgpInput body) noexcept {
  if (!Extent(body)) return {PgpCode::invalid_extent};
  if (body.size < 6 || (body.data[0] != 'b' && body.data[0] != 't' && body.data[0] != 'u')) return {};
  const std::size_t prefix = 6 + body.data[1];
  if (prefix > body.size) return {};
  return {PgpCode::ok, body.data[0], {body.data + prefix, body.size - prefix}};
}
PgpSize LiteralPacketEncodedSize(std::size_t bytes) noexcept {
  if (bytes > std::numeric_limits<std::size_t>::max() - 6)
    return {PgpCode::size_overflow, 0};
  return PacketEncodedSize(11, bytes + 6);
}
PgpCode EncodeLiteralPacket(PgpInput input, PgpOutput output, PgpCancellation probe) {
  const auto size = LiteralPacketEncodedSize(input.size);
  if (size.code != PgpCode::ok) return size.code;
  if (output.size != size.bytes || !Disjoint(input, output)) return PgpCode::invalid_extent;
  Pending pending{output};
  if (Cancelled(probe)) return PgpCode::cancelled;
  const auto header = Header(11, input.size + 6, output.data);
  output.data[header] = 'b'; std::memset(output.data + header + 1, 0, 5);
  const auto code = Copy(input, output.data + header + 6, probe);
  if (code != PgpCode::ok) return code;
  if (Cancelled(probe)) return PgpCode::cancelled;
  pending.accepted = true; return PgpCode::ok;
}
}  // namespace scratchbird::core::crypto
