// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_type_codec_identity_v3.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace scratchbird::core::datatypes {

using platform::byte;
using platform::u8;
using platform::u32;
using platform::u64;

inline constexpr u64 kBlobMaximumLogicalBytesV3 =
    static_cast<u64>(std::numeric_limits<std::int64_t>::max());
inline constexpr u32 kBlobProfileFingerprintPreimageBytesV3 = 3423;

inline constexpr platform::Uuid kBlobV11ReceiptUuid{{
    0x01,0xa1,0x09,0x5f,0xf2,0x05,0x72,0xd3,
    0xab,0xea,0x15,0xc6,0xd4,0x1a,0x4a,0xd4}};
inline constexpr platform::Uuid kBlobV3ProfileUuid{{
    0x01,0xa1,0x09,0x5f,0xf2,0x05,0x73,0x9c,
    0x81,0x66,0x22,0x1d,0xfe,0x6b,0x81,0x8d}};
inline constexpr std::array<byte, 32> kBlobV3ProfileFingerprint{{
    0x5f,0x6d,0xe3,0x00,0x9a,0xce,0x30,0xc9,
    0x70,0x93,0x0b,0x0b,0x26,0x95,0xf4,0x20,
    0x1e,0x78,0x69,0x91,0xfb,0x7b,0x9e,0xf5,
    0xb4,0x95,0x5b,0x04,0x01,0x43,0x5b,0xc8}};

struct BlobAuthorityReceiptV3 {
  platform::Uuid receipt_uuid{};
  platform::Uuid catalog_snapshot_uuid{};
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
};

enum class BlobPolicyKindV3 : u8 {
  descriptor = 0,
  canonicalization,
  bounds,
  state,
  memory,
  resource,
  lifetime,
  component_codec,
  row_state,
  hash,
  comparison,
  operation,
  cast,
  index,
  statistics,
  backup,
  protection,
  wire,
  diagnostic,
  metric,
  v1_v2,
  count,
};

inline constexpr std::size_t kBlobPolicyBindingCountV3 =
    static_cast<std::size_t>(BlobPolicyKindV3::count);

struct BlobValidatedProfileHandleV3 {
  BlobAuthorityReceiptV3 receipt;
  DatatypeTypeCodecIdentityRowV3 identity;
  platform::Uuid profile_uuid{};
  u64 profile_generation = 0;
  std::array<DatatypePolicyIdentityV3, kBlobPolicyBindingCountV3>
      policy_bindings{};
  std::array<byte, 32> profile_fingerprint{};
  u32 profile_fingerprint_preimage_bytes = 0;
};

enum class BlobValueStateV3 : u8 { value = 0, sql_null = 1 };

// This view deliberately carries no opaque lifetime pointer and no caller-set
// ownership claim. Current Core requires authenticated retain/release and
// snapshot authority for borrowed bytes, but its exact ABI is not published.
// Therefore the first bounded tranche admits only states that expose no
// content: empty VALUE and clean SQL_NULL. The lifetime gate in the validator
// is the single point to extend after exact token/authority publication.
struct BlobMaterializedValueViewV3 {
  const BlobValidatedProfileHandleV3* profile = nullptr;
  BlobValueStateV3 state = BlobValueStateV3::value;
  u64 logical_length = 0;
  std::span<const byte> bytes;
};

// Budgets default to the Core-defined zero allowance. Profile and empty-state
// validation do no allocation, byte read, or callback work, so zero budgets do
// not prevent those structural checks from succeeding.
struct BlobExecutionControlV3 {
  u64 maximum_allocation_bytes = 0;
  u64 maximum_content_bytes_read = 0;
  u64 maximum_content_bytes_written = 0;
  u64 maximum_reader_calls = 0;
  std::span<byte> scratch;
  bool (*cancelled)(void*) noexcept = nullptr;
  void* cancellation_context = nullptr;
};

struct BlobDiagnosticFactV3 {
  platform::Status status;
  std::string_view diagnostic_code;
  std::string_view detail;
};

struct BlobValidationResultV3 {
  platform::Status status;
  BlobDiagnosticFactV3 diagnostic;
  bool ok() const noexcept { return status.ok(); }
};

struct BlobProfileResultV3 {
  platform::Status status;
  BlobDiagnosticFactV3 diagnostic;
  BlobValidatedProfileHandleV3 profile;
  bool ok() const noexcept { return status.ok(); }
};

struct BlobMaterializedViewResultV3 {
  platform::Status status;
  BlobDiagnosticFactV3 diagnostic;
  BlobMaterializedValueViewV3 value;
  bool ok() const noexcept { return status.ok(); }
};

BlobProfileResultV3 BuildBlobValidatedProfileHandleV3(
    const BlobAuthorityReceiptV3& receipt,
    const DatatypeTypeCodecIdentityRowV3& identity) noexcept;

BlobProfileResultV3 BuildCurrentBlobValidatedProfileHandleV3(
    const platform::Uuid& receipt_uuid) noexcept;

BlobValidationResultV3 ValidateBlobProfileHandleV3(
    const BlobValidatedProfileHandleV3& profile,
    const BlobExecutionControlV3& control = {}) noexcept;

BlobMaterializedViewResultV3 ValidateBlobMaterializedValueViewNoAllocV3(
    const BlobMaterializedValueViewV3& value,
    bool null_allowed = true,
    const BlobExecutionControlV3& control = {}) noexcept;

}  // namespace scratchbird::core::datatypes
