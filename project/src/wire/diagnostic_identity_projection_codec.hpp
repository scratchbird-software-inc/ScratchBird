// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace scratchbird::wire {
using DiagnosticRegistryUuidV1 = std::array<std::uint8_t,16>;
using DiagnosticRegistrySha256V1 = std::array<std::uint8_t,32>;
using DiagnosticIdentityProjectionBytesV1 = std::array<std::uint8_t,72>;
struct DiagnosticIdentityProjectionV1 {
  DiagnosticRegistryUuidV1 diagnostic_uuid{};
  std::uint64_t diagnostic_generation=0;
  std::uint32_t precedence_ordinal=0;
  std::uint8_t severity_code=0;
  std::uint8_t redaction_class=0;
  std::uint32_t maximum_safe_field_count=0;
  DiagnosticRegistrySha256V1 row_identity_sha256{};
};
// Exact DIAGNOSTIC_IDENTITY_DURABLE_NORMALIZATION_V1 byte/hash contract.
// Encoding replaces the input hash only after complete success. A private
// normalization may include internal severity; parser endpoints must not.
// Structural validation is not caller authorization or registry membership.
bool EncodeDiagnosticIdentityProjectionV1(DiagnosticIdentityProjectionV1* row,
    DiagnosticIdentityProjectionBytesV1* bytes,bool allow_internal=false) noexcept;
bool DecodeDiagnosticIdentityProjectionV1(const std::uint8_t* bytes,std::size_t size,
    DiagnosticIdentityProjectionV1* row,bool allow_internal=false) noexcept;
bool ValidateDiagnosticIdentityCohortV1(const std::vector<DiagnosticIdentityProjectionV1>& rows,
    bool allow_internal=false) noexcept;
} // namespace scratchbird::wire
