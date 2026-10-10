// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <cstdint>

namespace scratchbird::core::crypto {
struct Argon2idParameters {
  std::uint32_t memory_kib = 65536;
  std::uint32_t passes = 3;
  std::uint32_t lanes = 4;
};
struct Argon2Input { const std::uint8_t* data = nullptr; std::size_t size = 0; };
struct Argon2Output { std::uint8_t* data = nullptr; std::size_t size = 0; };
struct Argon2Cancellation { bool (*requested)(void*) = nullptr; void* context = nullptr; };
enum class Argon2Code { ok, invalid_parameters, invalid_extent, size_overflow, cancelled, provider_failure };
struct Argon2Workspace { Argon2Code code; std::uint64_t bytes; };
Argon2Workspace Argon2idWorkspaceBytes(Argon2idParameters) noexcept;
// Conservative fixed secret-scratch payload bound, not an ABI stack-frame size.
// Thread-stack and callback resources remain separately owned by the runtime.
std::size_t Argon2idFixedScratchBytes() noexcept;

// RFC9106 Argon2id version0x13. Mechanism only: no resource/security admission,
// entropy acquisition, heap allocation, hidden threads or I/O. Four lanes do
// not require four threads; this implementation processes lanes synchronously.
// Caller supplies exclusive, 8-byte-aligned workspace of the queried size and
// exclusive private output (4..65535 bytes). Inputs may overlap each other,
// but no mutable extent may overlap an input or another mutable extent.
// Password/salt/secret/associated-data lengths must fit uint32. Empty input is
// a byte-sequence value, not SQL NULL; OpenPGP separately requires16-byte salt.
// Invalid extents are untouched. After admission, workspace and fixed scratch
// are erased on every exit; output is erased on failure/cancellation/exception.
// Cancellation polls between bounded input slices, lanes and64-block batches,
// and after workspace erasure before success. Probes may throw; no exception
// crosses the C reference provider and all cleanup still occurs.
Argon2Code DeriveArgon2idKey(Argon2idParameters, Argon2Input password, Argon2Input salt,
    Argon2Output workspace, Argon2Output output, Argon2Cancellation = {},
    Argon2Input secret = {}, Argon2Input associated_data = {});
} // namespace scratchbird::core::crypto
