// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hash_digest.hpp"
#include "hash_digest_parts.hpp"

#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <iomanip>
#include <istream>
#include <limits>
#include <memory>
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

HashDigestResult ComputeSha256DigestParts(const HashDigestSegment* segments,
                                        std::size_t segment_count) {
  if (segment_count != 0 && segments == nullptr)
    return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "segments_missing");
  // SHA256 encodes the input length in a 64-bit bit count. Check every borrowed
  // extent before reading any payload or allocating the digest context.
  constexpr u64 maximum_bytes = std::numeric_limits<u64>::max() / 8;
  u64 total = 0;
  for (std::size_t i = 0; i < segment_count; ++i) {
    if ((segments[i].size != 0 && segments[i].data == nullptr) ||
        segments[i].size > maximum_bytes - total)
      return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "segment_extent_invalid");
    total += segments[i].size;
  }
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed");
  for (std::size_t i = 0; i < segment_count; ++i)
    if (segments[i].size != 0 &&
        EVP_DigestUpdate(context.get(), segments[i].data, segments[i].size) != 1)
      return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed");
  HashDigestResult result;
  result.status = HashOkStatus();
  unsigned int digest_len = 0;
  if (EVP_DigestFinal_ex(context.get(), result.digest.data(), &digest_len) != 1 ||
      digest_len != result.digest.size())
    return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed");
  result.digest_bytes = static_cast<u16>(digest_len);
  return result;
}

HashDigestResult ComputeSha256Digest(const byte* payload, std::size_t payload_size) {
  if ((payload_size != 0 && payload == nullptr) ||
      payload_size > std::numeric_limits<u64>::max() / 8)
    return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "payload_extent_invalid");
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

HashDigestResult ComputeSha256Stream(std::istream& input, u64 expected_bytes) {
  constexpr u64 maximum_bytes = std::numeric_limits<u64>::max() / 8;
  if (expected_bytes > maximum_bytes || !input.good())
    return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "stream_extent_invalid");
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "stream_backend_initialize");
  std::array<char, 65536> buffer;
  u64 remaining = expected_bytes;
  while (remaining != 0) {
    const auto count = static_cast<std::streamsize>(std::min<u64>(remaining, buffer.size()));
    try {
      input.read(buffer.data(), count);
    } catch (...) {
      return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "stream_read_failed");
    }
    if (input.gcount() != count || !input.good())
      return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "stream_short_read");
    if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1)
      return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "stream_backend_update");
    remaining -= static_cast<u64>(count);
  }
  try {
    if (input.peek() != std::char_traits<char>::eof())
      return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "stream_trailing_bytes");
  } catch (const std::ios_base::failure&) {
    // EOF may be enabled in the caller's exception mask; clean EOF is still
    // the required boundary. A streambuf failure also sets badbit and refuses.
    if (!input.eof() || input.bad())
      return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "stream_read_failed");
  } catch (...) {
    return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "stream_read_failed");
  }
  if (!input.eof() || input.bad())
    return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "stream_end_invalid");
  HashDigestResult result;
  result.status = HashOkStatus();
  unsigned int digest_len = 0;
  if (EVP_DigestFinal_ex(context.get(), result.digest.data(), &digest_len) != 1 || digest_len != result.digest.size())
    return HashError("SB-CORE-HASH-SHA256-FAILED", "core.hash.sha256_failed", "stream_backend_finalize");
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
  constexpr u64 maximum_sha256_bytes = std::numeric_limits<u64>::max() / 8;
  constexpr std::size_t hmac_block_bytes = 64;
  if (key_size > maximum_sha256_bytes ||
      (payload_size != 0 && payload == nullptr) ||
      payload_size > maximum_sha256_bytes - hmac_block_bytes)
    return HashError("SB-CORE-HASH-HMAC-SHA256-FAILED", "core.hash.hmac_sha256_failed", "input_extent_invalid");
  // HMAC hashes keys longer than its block size. Prehash here so the backend's
  // int key-length ABI cannot narrow a caller's valid size_t key extent.
  HashDigestResult reduced_key;
  struct CleanseKey {
    Digest256& bytes;
    ~CleanseKey() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
  } cleanse_key{reduced_key.digest};
  if (key_size > hmac_block_bytes) {
    reduced_key = ComputeSha256Digest(key, key_size);
    if (!reduced_key.ok())
      return HashError("SB-CORE-HASH-HMAC-SHA256-FAILED", "core.hash.hmac_sha256_failed", "key_hash_failed");
    key = reduced_key.digest.data();
    key_size = reduced_key.digest.size();
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
  std::size_t diff = lhs.size() ^ rhs.size();
  for (std::size_t i = 0; i < max_size; ++i) {
    const byte a = i < lhs.size() ? static_cast<byte>(lhs[i]) : 0;
    const byte b = i < rhs.size() ? static_cast<byte>(rhs[i]) : 0;
    diff |= static_cast<std::size_t>(a ^ b);
  }
  return diff == 0;
}

bool ConstantTimeEqual(const std::vector<byte>& lhs, const std::vector<byte>& rhs) {
  const std::size_t max_size = std::max(lhs.size(), rhs.size());
  std::size_t diff = lhs.size() ^ rhs.size();
  for (std::size_t i = 0; i < max_size; ++i) {
    const byte a = i < lhs.size() ? lhs[i] : 0;
    const byte b = i < rhs.size() ? rhs[i] : 0;
    diff |= static_cast<std::size_t>(a ^ b);
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
