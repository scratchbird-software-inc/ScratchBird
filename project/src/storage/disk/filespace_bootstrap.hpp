// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "runtime_platform.hpp"
#include <array>
#include <cstddef>
#include <optional>

namespace scratchbird::storage::disk {
class FileDevice;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;

// FILESPACE-BOOTSTRAP-PREAMBLE-EXACT-ADMISSION-V1.
// These are Core registry identities, not database-wide page-size policy.
struct CanonicalFilespacePageProfile {
  Uuid uuid;
  u32 page_size_bytes;
  u32 layout_generation;
  u32 alignment_bytes;
};
inline constexpr std::array<CanonicalFilespacePageProfile, 5> kCanonicalFilespacePageProfiles{{
    {{{0,0,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0x81,0x92}},8192,1,4096},
    {{{0,0,0,0,0,0,0x70,0,0x80,0,0,0,0,0x01,0x63,0x84}},16384,1,4096},
    {{{0,0,0,0,0,0,0x70,0,0x80,0,0,0,0,0x03,0x27,0x68}},32768,1,4096},
    {{{0,0,0,0,0,0,0x70,0,0x80,0,0,0,0,0x06,0x55,0x36}},65536,1,4096},
    {{{0,0,0,0,0,0,0x70,0,0x80,0,0,0,0,0x13,0x10,0x72}},131072,1,4096},
}};
inline constexpr Uuid kNativeBootstrapIntegrityProfile{{
    0x01,0xa0,0x8e,0x13,0x41,0x6b,0x73,0xf6,
    0xb3,0xc5,0xbd,0xde,0x88,0xb1,0xf2,0xd9}};
inline constexpr std::size_t kFilespaceBootstrapBytes = 4096;
using SerializedFilespaceBootstrap = std::array<byte, kFilespaceBootstrapBytes>;
namespace FilespaceBootstrapFlag {
inline constexpr u32 payload_encrypted = 1;
inline constexpr u32 cluster_authority_required = 2;
}

struct FilespaceBootstrap {
  Uuid database_uuid;
  Uuid filespace_uuid;
  Uuid page_size_profile_uuid;
  Uuid checksum_profile_uuid;
  Uuid encryption_profile_uuid;
  u32 page_size_bytes = 0;
  u32 durable_format_generation = 1;
  u32 flags = 0;
  // Exact registry wire codes; never implicit casts of lifecycle source enums.
  u16 filespace_role = 0;
  u16 lifecycle_state = 0;
};

struct FilespaceBootstrapBinding {
  Uuid database_uuid;
  Uuid filespace_uuid;
  Uuid page_size_profile_uuid;
};

enum class FilespaceBootstrapError {
  none, invalid_framing, reserved_nonzero, digest_mismatch, hash_provider_failure,
  invalid_identity, unsupported_page_profile, unsupported_format_generation,
  unknown_flags, unsupported_checksum_profile, invalid_role, invalid_state,
  invalid_encryption_profile, binding_mismatch, device_not_open, io_failure,
  resource_exhausted
};

struct FilespaceBootstrapDecodeResult {
  FilespaceBootstrapError error = FilespaceBootstrapError::invalid_framing;
  std::optional<FilespaceBootstrap> preamble;
  bool ok() const noexcept { return error == FilespaceBootstrapError::none && preamble.has_value(); }
};
struct FilespaceBootstrapEncodeResult {
  FilespaceBootstrapError error = FilespaceBootstrapError::invalid_framing;
  std::optional<SerializedFilespaceBootstrap> bytes;
  bool ok() const noexcept { return error == FilespaceBootstrapError::none && bytes.has_value(); }
};

const CanonicalFilespacePageProfile* FindCanonicalFilespacePageProfile(const Uuid&) noexcept;
const CanonicalFilespacePageProfile* FindCanonicalFilespacePageProfileForSize(u32) noexcept;
FilespaceBootstrapError ValidateFilespaceBootstrap(
    const FilespaceBootstrap&, const FilespaceBootstrapBinding* expected = nullptr) noexcept;
FilespaceBootstrapEncodeResult EncodeFilespaceBootstrap(const FilespaceBootstrap&) noexcept;
FilespaceBootstrapDecodeResult DecodeFilespaceBootstrap(
    const byte*, std::size_t, const FilespaceBootstrapBinding* expected = nullptr) noexcept;
// Structural probe only. Never reopens/closes/writes the caller's owned device.
// Success is NOT full page-zero validation, attachment/serving or MGA admission.
FilespaceBootstrapDecodeResult ReadFilespaceBootstrapFromOpenDevice(
    FileDevice&, const FilespaceBootstrapBinding* expected = nullptr) noexcept;
}  // namespace scratchbird::storage::disk
