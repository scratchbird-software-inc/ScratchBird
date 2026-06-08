// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hash_digest.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace scratchbird::core::hash {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status HashOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::platform};
}

Status HashErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::platform};
}

HashDigestResult HashError(std::string diagnostic_code,
                           std::string message_key,
                           std::string detail = {}) {
  HashDigestResult result;
  result.status = HashErrorStatus();
  result.diagnostic = MakeHashDigestDiagnostic(result.status,
                                               std::move(diagnostic_code),
                                               std::move(message_key),
                                               std::move(detail));
  return result;
}

}  // namespace

HashDigestResult ComputeSha256Digest(const std::vector<byte>& payload) {
  return ComputeSha256Digest(payload.empty() ? nullptr : payload.data(),
                             payload.size());
}

HashDigestResult ComputeSha256Digest(const byte* payload, std::size_t payload_size) {
  HashDigestResult result;
  result.status = HashOkStatus();
  unsigned int digest_len = 0;
  if (EVP_Digest(payload,
                 payload_size,
                 result.digest.data(),
                 &digest_len,
                 EVP_sha256(),
                 nullptr) != 1 ||
      digest_len != result.digest.size()) {
    return HashError("SB-CORE-HASH-SHA256-FAILED",
                     "core.hash.sha256_failed");
  }
  result.digest_bytes = static_cast<u16>(digest_len);
  return result;
}

HashDigestResult ComputeHmacSha256Digest(const std::vector<byte>& key,
                                         const std::vector<byte>& payload) {
  return ComputeHmacSha256Digest(key.empty() ? nullptr : key.data(),
                                 key.size(),
                                 payload.empty() ? nullptr : payload.data(),
                                 payload.size());
}

HashDigestResult ComputeHmacSha256Digest(const byte* key,
                                         std::size_t key_size,
                                         const byte* payload,
                                         std::size_t payload_size) {
  if (key == nullptr || key_size == 0) {
    return HashError("SB-CORE-HASH-HMAC-KEY-REQUIRED",
                     "core.hash.hmac_key_required");
  }
  HashDigestResult result;
  result.status = HashOkStatus();
  unsigned int digest_len = 0;
  if (HMAC(EVP_sha256(),
           key,
           static_cast<int>(key_size),
           payload,
           payload_size,
           result.digest.data(),
           &digest_len) == nullptr ||
      digest_len != result.digest.size()) {
    return HashError("SB-CORE-HASH-HMAC-SHA256-FAILED",
                     "core.hash.hmac_sha256_failed");
  }
  result.digest_bytes = static_cast<u16>(digest_len);
  return result;
}

std::string HexLower(const Digest256& digest) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const byte value : digest) {
    out << std::setw(2) << static_cast<unsigned int>(value);
  }
  return out.str();
}

std::vector<byte> DigestVector(const Digest256& digest) {
  return {digest.begin(), digest.end()};
}

u64 DigestLow64(const Digest256& digest) {
  return LoadLittle64(digest.data());
}

u64 DigestHigh64(const Digest256& digest) {
  return LoadLittle64(digest.data() + sizeof(u64));
}

bool ConstantTimeEqual(std::string_view lhs, std::string_view rhs) {
  const std::size_t max_size = std::max(lhs.size(), rhs.size());
  byte diff = static_cast<byte>(lhs.size() ^ rhs.size());
  for (std::size_t i = 0; i < max_size; ++i) {
    const byte a = i < lhs.size() ? static_cast<byte>(lhs[i]) : 0;
    const byte b = i < rhs.size() ? static_cast<byte>(rhs[i]) : 0;
    diff = static_cast<byte>(diff | (a ^ b));
  }
  return diff == 0;
}

bool ConstantTimeEqual(const std::vector<byte>& lhs, const std::vector<byte>& rhs) {
  const std::size_t max_size = std::max(lhs.size(), rhs.size());
  byte diff = static_cast<byte>(lhs.size() ^ rhs.size());
  for (std::size_t i = 0; i < max_size; ++i) {
    const byte a = i < lhs.size() ? lhs[i] : 0;
    const byte b = i < rhs.size() ? rhs[i] : 0;
    diff = static_cast<byte>(diff | (a ^ b));
  }
  return diff == 0;
}

DiagnosticRecord MakeHashDigestDiagnostic(Status status,
                                          std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.hash.digest");
}

}  // namespace scratchbird::core::hash
