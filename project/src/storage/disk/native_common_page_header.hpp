// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "filespace_bootstrap.hpp"

namespace scratchbird::storage::disk {
using scratchbird::core::platform::u64;

// NATIVE_COMMON_PAGE_HEADER_V2. Metadata only: successful decoding does not
// admit a family body, role, provider, encryption key or MGA publication.
inline constexpr std::size_t kNativeCommonPageHeaderBytes = 128;
using NativeCommonPageHeaderBytes = std::array<byte, kNativeCommonPageHeaderBytes>;
struct NativeCommonPageHeader {
  u32 page_size_bytes = 0;
  u32 page_type = 0; // Core uint16 identity, not the prototype PageType enum.
  Uuid database_uuid;
  Uuid filespace_uuid;
  Uuid page_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 flags = 0;
  Uuid page_size_profile_uuid;
};
struct NativeCommonPageHeaderBinding {
  FilespaceBootstrapBinding filespace;
  u64 page_number = 0;
  u64 page_generation = 0;
  u32 page_type = 0;
  // PageRefV2 carries a generation, not a page UUID. When independently known,
  // the page UUID is an additional exact constraint, never a replacement.
  std::optional<Uuid> page_uuid;
};
enum class NativeCommonPageHeaderError {
  none, invalid_framing, checksum_mismatch, invalid_extension, reserved_nonzero,
  unknown_flags, invalid_identity, unsupported_profile, unregistered_type,
  invalid_page_number, invalid_generation, invalid_binding, binding_mismatch,
  device_not_open, invalid_extent, io_failure, resource_exhausted
};
struct NativeCommonPageHeaderDecodeResult {
  NativeCommonPageHeaderError error = NativeCommonPageHeaderError::invalid_framing;
  std::optional<NativeCommonPageHeader> header;
  bool ok() const noexcept { return error == NativeCommonPageHeaderError::none && header.has_value(); }
};
struct NativeCommonPageHeaderEncodeResult {
  NativeCommonPageHeaderError error = NativeCommonPageHeaderError::invalid_framing;
  std::optional<NativeCommonPageHeaderBytes> bytes;
  bool ok() const noexcept { return error == NativeCommonPageHeaderError::none && bytes.has_value(); }
};
// Membership is not active-use permission. Reserved/forbidden entries remain
// identifiable for inspection; their owning role/status gates still apply.
bool IsRegisteredNativePageType(u32) noexcept;
u64 ComputeNativeCommonPageHeaderChecksum(const NativeCommonPageHeaderBytes&) noexcept;
NativeCommonPageHeaderEncodeResult EncodeNativeCommonPageHeader(const NativeCommonPageHeader&) noexcept;
NativeCommonPageHeaderDecodeResult DecodeNativeCommonPageHeader(
    const byte*, std::size_t, const NativeCommonPageHeaderBinding* expected = nullptr) noexcept;
// Borrow the already-owned device, never reopen, close or write it. Read only
// the bound common header, at4096 for page0 or page_number*filespace_profile.
// The owning operation must separately validate bootstrap, actual family body,
// role/lifecycle, providers, and recovery/finality before any ordinary serving.
NativeCommonPageHeaderDecodeResult ReadNativeCommonPageHeaderFromOpenDevice(
    FileDevice&, const NativeCommonPageHeaderBinding&) noexcept;
}  // namespace scratchbird::storage::disk
