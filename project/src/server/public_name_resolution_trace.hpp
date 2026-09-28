// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../core/hash/hash_digest.hpp"
#include "../wire/public_result_packet.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::server::name_resolution_trace {
namespace packet = wire::public_result;
inline constexpr std::string_view kMagic = "SBNRT003";
inline constexpr std::size_t kMaximumPayloadBytes = 64u * 1024u * 1024u;
inline packet::Field Text(std::string name, std::string_view value) {
  return {std::move(name), packet::Kind::text, std::string(value)};
}
inline packet::Field Number(std::string name, std::uint64_t value) {
  return {std::move(name), packet::Kind::unsigned_integer, packet::Unsigned(value)};
}
inline packet::Field Identity(std::string name, const core::platform::Uuid& value) {
  return {std::move(name), packet::Kind::uuid,
      {reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()}};
}

// Private diagnostic evidence, never executable metadata or cache authority.
// Each appended record is independently length-delimited and SHA-256 checked.
inline std::string Encode(const std::vector<packet::Field>& fields) {
  std::string payload;
  if (!packet::Encode(fields, &payload) || payload.size() > kMaximumPayloadBytes) return {};
  std::string frame(kMagic);
  frame += packet::Unsigned(payload.size());
  frame += payload;
  const auto digest = core::hash::ComputeSha256Digest(
      reinterpret_cast<const core::platform::byte*>(frame.data()), frame.size());
  if (!digest.ok() || digest.digest_bytes != core::hash::kSha256DigestBytes) return {};
  frame.append(reinterpret_cast<const char*>(digest.digest.data()), digest.digest.size());
  return frame;
}
inline bool Decode(std::string_view frame, std::vector<packet::Field>* output) {
  if (!output || frame.size() < 60 || frame.size() > kMaximumPayloadBytes + 48 ||
      !frame.starts_with(kMagic)) return false;
  const auto size = core::platform::LoadLittle64(frame.data() + 8);
  if (size != frame.size() - 48) return false;
  const auto digest = core::hash::ComputeSha256Digest(
      reinterpret_cast<const core::platform::byte*>(frame.data()), frame.size() - 32);
  if (!digest.ok() || digest.digest_bytes != core::hash::kSha256DigestBytes ||
      !std::equal(digest.digest.begin(), digest.digest.end(),
          reinterpret_cast<const core::platform::byte*>(frame.data() + frame.size() - 32))) return false;
  return packet::Decode(frame.substr(16, static_cast<std::size_t>(size)), output);
}
inline bool Append(const std::vector<packet::Field>& fields) {
  const char* path = std::getenv("SCRATCHBIRD_PUBLIC_NAME_RESOLUTION_TRACE_FILE");
  if (path == nullptr || *path == '\0') return false;
  const auto frame = Encode(fields);
  if (frame.empty()) return false;
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  std::ofstream out(path, std::ios::app | std::ios::binary);
  if (!out) return false;
  out.write(frame.data(), static_cast<std::streamsize>(frame.size()));
  out.flush();
  return static_cast<bool>(out);
}
} // namespace scratchbird::server::name_resolution_trace
