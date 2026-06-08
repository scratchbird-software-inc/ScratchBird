// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CORE-HASH-DIGEST-ANCHOR
#include "runtime_platform.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::hash {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;

inline constexpr u16 kSha256DigestBytes = 32;
using Digest256 = std::array<byte, kSha256DigestBytes>;

struct HashDigestResult {
  Status status;
  DiagnosticRecord diagnostic;
  Digest256 digest{};
  u16 digest_bytes = 0;

  bool ok() const {
    return status.ok();
  }
};

HashDigestResult ComputeSha256Digest(const std::vector<byte>& payload);
HashDigestResult ComputeSha256Digest(const byte* payload, std::size_t payload_size);
HashDigestResult ComputeHmacSha256Digest(const std::vector<byte>& key,
                                         const std::vector<byte>& payload);
HashDigestResult ComputeHmacSha256Digest(const byte* key,
                                         std::size_t key_size,
                                         const byte* payload,
                                         std::size_t payload_size);
std::string HexLower(const Digest256& digest);
std::vector<byte> DigestVector(const Digest256& digest);
u64 DigestLow64(const Digest256& digest);
u64 DigestHigh64(const Digest256& digest);
bool ConstantTimeEqual(std::string_view lhs, std::string_view rhs);
bool ConstantTimeEqual(const std::vector<byte>& lhs, const std::vector<byte>& rhs);
DiagnosticRecord MakeHashDigestDiagnostic(Status status,
                                          std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail = {});

}  // namespace scratchbird::core::hash
