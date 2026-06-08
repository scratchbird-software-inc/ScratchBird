// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by data_scalar_functions.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
  out->clear();
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const int a = Base64Value(text[i]);
    const int b = Base64Value(text[i + 1]);
    const int c = text[i + 2] == '=' ? 0 : Base64Value(text[i + 2]);
    const int d = text[i + 3] == '=' ? 0 : Base64Value(text[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return false;
    const std::uint32_t triple = (static_cast<std::uint32_t>(a) << 18) |
                                 (static_cast<std::uint32_t>(b) << 12) |
                                 (static_cast<std::uint32_t>(c) << 6) |
                                 static_cast<std::uint32_t>(d);
    out->push_back(static_cast<std::uint8_t>((triple >> 16) & 0xff));
    if (text[i + 2] != '=') out->push_back(static_cast<std::uint8_t>((triple >> 8) & 0xff));
    if (text[i + 3] != '=') out->push_back(static_cast<std::uint8_t>(triple & 0xff));
  }
  return true;
}

std::uint64_t Fnva64(const std::vector<std::uint8_t>& bytes) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto byte : bytes) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::vector<std::uint8_t> RandomBytes(std::size_t byte_count) {
  std::vector<std::uint8_t> out(byte_count);
  std::random_device random;
  for (std::size_t i = 0; i < byte_count; ++i) {
    out[i] = static_cast<std::uint8_t>(random() & 0xffu);
  }
  return out;
}

std::string UuidBytesToCanonicalText(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
    out.push_back(kHex[(bytes[i] >> 4) & 0x0f]);
    out.push_back(kHex[bytes[i] & 0x0f]);
  }
  return out;
}

std::string RandomUuidV4Text() {
  auto bytes = RandomBytes(16);
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fu) | 0x40u);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fu) | 0x80u);
  return UuidBytesToCanonicalText(bytes);
}

std::string RandomUuidV1Text() {
  auto bytes = RandomBytes(16);
  const std::uint64_t unix_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const std::uint64_t uuid_ts_100ns = (unix_ns / 100ull) + 122192928000000000ull;
  bytes[0] = static_cast<std::uint8_t>((uuid_ts_100ns >> 24) & 0xff);
  bytes[1] = static_cast<std::uint8_t>((uuid_ts_100ns >> 16) & 0xff);
  bytes[2] = static_cast<std::uint8_t>((uuid_ts_100ns >> 8) & 0xff);
  bytes[3] = static_cast<std::uint8_t>(uuid_ts_100ns & 0xff);
  bytes[4] = static_cast<std::uint8_t>((uuid_ts_100ns >> 40) & 0xff);
  bytes[5] = static_cast<std::uint8_t>((uuid_ts_100ns >> 32) & 0xff);
  bytes[6] = static_cast<std::uint8_t>(((uuid_ts_100ns >> 56) & 0x0fu) | 0x10u);
  bytes[7] = static_cast<std::uint8_t>((uuid_ts_100ns >> 48) & 0xff);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fu) | 0x80u);
  bytes[10] = static_cast<std::uint8_t>(bytes[10] | 0x01u);
  return UuidBytesToCanonicalText(bytes);
}

std::string RandomUuidV7Text() {
  auto bytes = RandomBytes(16);
  const std::uint64_t unix_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  bytes[0] = static_cast<std::uint8_t>((unix_ms >> 40) & 0xff);
  bytes[1] = static_cast<std::uint8_t>((unix_ms >> 32) & 0xff);
  bytes[2] = static_cast<std::uint8_t>((unix_ms >> 24) & 0xff);
  bytes[3] = static_cast<std::uint8_t>((unix_ms >> 16) & 0xff);
  bytes[4] = static_cast<std::uint8_t>((unix_ms >> 8) & 0xff);
  bytes[5] = static_cast<std::uint8_t>(unix_ms & 0xff);
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fu) | 0x70u);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fu) | 0x80u);
  return UuidBytesToCanonicalText(bytes);
}

bool CanonicalUuidBytes(const std::string& text, std::array<std::uint8_t, 16>* bytes) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(text);
  if (!parsed.ok()) return false;
  *bytes = parsed.value.bytes;
  return true;
}

std::string FormatUnixMillisUtcTimestamp(std::uint64_t unix_epoch_millis) {
  const std::time_t seconds = static_cast<std::time_t>(unix_epoch_millis / 1000ull);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::optional<std::string> UuidEmbeddedTimestampText(const std::array<std::uint8_t, 16>& bytes) {
  constexpr std::uint64_t kUuidUnixEpochOffset100ns = 122192928000000000ull;
  const int version = (bytes[6] >> 4) & 0x0f;
  if (version == 1) {
    const std::uint64_t time_low = (static_cast<std::uint64_t>(bytes[0]) << 24) |
                                   (static_cast<std::uint64_t>(bytes[1]) << 16) |
                                   (static_cast<std::uint64_t>(bytes[2]) << 8) |
                                   static_cast<std::uint64_t>(bytes[3]);
    const std::uint64_t time_mid = (static_cast<std::uint64_t>(bytes[4]) << 8) |
                                   static_cast<std::uint64_t>(bytes[5]);
    const std::uint64_t time_hi = ((static_cast<std::uint64_t>(bytes[6]) & 0x0full) << 8) |
                                  static_cast<std::uint64_t>(bytes[7]);
    const std::uint64_t uuid_100ns = (time_hi << 48) | (time_mid << 32) | time_low;
    if (uuid_100ns < kUuidUnixEpochOffset100ns) return std::nullopt;
    return FormatUnixMillisUtcTimestamp((uuid_100ns - kUuidUnixEpochOffset100ns) / 10000ull);
  }
  if (version == 7) {
    const std::uint64_t unix_ms = (static_cast<std::uint64_t>(bytes[0]) << 40) |
                                  (static_cast<std::uint64_t>(bytes[1]) << 32) |
                                  (static_cast<std::uint64_t>(bytes[2]) << 24) |
                                  (static_cast<std::uint64_t>(bytes[3]) << 16) |
                                  (static_cast<std::uint64_t>(bytes[4]) << 8) |
                                  static_cast<std::uint64_t>(bytes[5]);
    return FormatUnixMillisUtcTimestamp(unix_ms);
  }
  return std::nullopt;
}

FunctionCallResult UnaryText(const FunctionCallRequest& request, std::string (*fn)(std::string)) {
  if (request.arguments.size() != 1) {
    return RefuseFunctionInvalidInput(request, "expected exactly one argument");
  }
  if (IsSqlNull(request.arguments[0].value)) {
    return MakeFunctionSuccess(request, {MakeNullValue(request.arguments[0].value.descriptor_id)});
  }
  return MakeFunctionSuccess(request, {MakeTextValue(request.arguments[0].value.descriptor_id,
                                                    fn(ValueAsText(request.arguments[0].value)))});
}

bool ValueAsReal64(const scratchbird::engine::sblr::SblrValue& value, double* out) {
  if (value.has_real64_value) {
    *out = value.real64_value;
    return true;
  }
  if (value.has_int64_value) {
    *out = static_cast<double>(value.int64_value);
    return true;
  }
  if (value.has_uint64_value) {
    *out = static_cast<double>(value.uint64_value);
    return true;
  }
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stod(ValueAsText(value), &consumed);
    if (consumed != ValueAsText(value).size()) return false;
    *out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

bool ValueAsInt64Strict(const scratchbird::engine::sblr::SblrValue& value, std::int64_t* out) {
  if (value.has_int64_value) {
    *out = value.int64_value;
    return true;
  }
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoll(ValueAsText(value), &consumed, 10);
    if (consumed != ValueAsText(value).size()) return false;
    *out = static_cast<std::int64_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool ValueAsUint64Strict(const scratchbird::engine::sblr::SblrValue& value, std::uint64_t* out) {
  if (value.has_uint64_value) {
    *out = value.uint64_value;
    return true;
  }
  if (value.has_int64_value && value.int64_value >= 0) {
    *out = static_cast<std::uint64_t>(value.int64_value);
    return true;
  }
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(ValueAsText(value), &consumed, 10);
    if (consumed != ValueAsText(value).size()) return false;
    *out = static_cast<std::uint64_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

std::uint64_t Int64Magnitude(std::int64_t value) {
  if (value == std::numeric_limits<std::int64_t>::min()) {
    return static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ull;
  }
  return static_cast<std::uint64_t>(value < 0 ? -value : value);
}

std::uint64_t GcdMagnitude(std::uint64_t left, std::uint64_t right) {
  while (right != 0) {
    const std::uint64_t next = left % right;
    left = right;
    right = next;
  }
  return left;
}

bool Uint64FitsInt64(std::uint64_t value) {
  return value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

bool ValueTruthy(const scratchbird::engine::sblr::SblrValue& value) {
  if (value.is_null) return false;
  if (value.has_int64_value) return value.int64_value != 0;
  if (value.has_uint64_value) return value.uint64_value != 0;
  if (value.has_real64_value) return value.real64_value != 0.0;
  const auto text = LowerAscii(ValueAsText(value));
  return text == "true" || text == "t" || text == "yes" || text == "y" || text == "1";
}

bool ParseBooleanText(const scratchbird::engine::sblr::SblrValue& value, bool* out) {
  if (value.is_null || out == nullptr) return false;
  if (value.payload_kind == scratchbird::engine::sblr::SblrValuePayloadKind::boolean) {
    *out = value.has_uint64_value ? value.uint64_value != 0 : value.int64_value != 0;
    return true;
  }
  const auto text = LowerAscii(ValueAsText(value));
  if (text == "true" || text == "t" || text == "yes" || text == "y" || text == "1") {
    *out = true;
    return true;
  }
  if (text == "false" || text == "f" || text == "no" || text == "n" || text == "0") {
    *out = false;
    return true;
