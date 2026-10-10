// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "openpgp_seipd.hpp"
#include <array>

namespace scratchbird::core::crypto {
// Allocation-free RFC9580 packet framing. This is structural parsing, never
// authentication, algorithm admission, permission to allocate, or publication.
// Input is immutable and borrowed for the complete synchronous call. Descriptors
// are observations only; CopyPacketBody reparses the supplied bytes.
struct PgpPacketDescription {
  PgpCode code = PgpCode::invalid_packet;
  std::uint8_t type = 0;
  std::size_t body_bytes = 0, packet_bytes = 0;
  bool partial = false;
  std::size_t first_body_offset = 0;
};
struct PgpPacketPrefix {
  PgpCode code = PgpCode::invalid_packet;
  std::array<std::uint8_t, 6> bytes{};
  std::size_t size = 0;
};
// Header only, for an owning encoder writing directly into a private body
// slice. Does not make a complete packet or certify that any body exists.
PgpPacketPrefix PacketPrefix(std::uint8_t type, std::size_t body_bytes) noexcept;
PgpSize PacketEncodedSize(std::uint8_t type, std::size_t body_bytes) noexcept;
PgpPacketDescription InspectPacket(PgpInput, PgpCancellation = {});
// Encode one shortest definite-length packet. Decode one packet at the front
// of a stream; packet_bytes tells its owner where the following packet starts.
// Partial lengths are accepted only for data types8,9,11,18, with a first
// partial segment >=512 and a required final definite length (possibly zero).
// Legacy headers require a separate compatibility decoder; no implicit fallback.
// Output must be exact-sized, exclusive private staging, disjoint from input.
// Bad extents/shape leave output untouched. Once admitted, cancellation or
// exceptions erase it. Neither function releases plaintext for observation.
PgpCode EncodePacket(std::uint8_t type, PgpInput body, PgpOutput,
                     PgpCancellation = {});
PgpCode CopyPacketBody(PgpInput, PgpOutput, PgpCancellation = {});

struct PgpLiteralDescription {
  PgpCode code = PgpCode::invalid_packet;
  std::uint8_t format = 0;
  PgpInput data{};
};
// BODY only, after packet reassembly. Metadata is untrusted and ignored, never
// a filename to open, timestamp authority, charset override, or security flag.
// b/t/u are structurally recognized; no byte/newline/charset transformation is
// performed. The owning text adapter separately validates declared UTF-8.
PgpLiteralDescription InspectLiteralBody(PgpInput) noexcept;
PgpSize LiteralPacketEncodedSize(std::size_t data_bytes) noexcept;
// Native byte-preserving literal packet: format b, empty filename, zero time.
PgpCode EncodeLiteralPacket(PgpInput data, PgpOutput, PgpCancellation = {});
}  // namespace scratchbird::core::crypto
