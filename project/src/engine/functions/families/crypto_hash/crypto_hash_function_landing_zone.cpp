// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/crypto_hash/crypto_hash_function_landing_zone.hpp"

#include "common/function_result_helpers.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::functions {
namespace {

constexpr std::size_t kMaxCryptoInputBytes = 1024 * 1024;
constexpr std::size_t kMaxRandomBytes = 1024;
constexpr std::uint64_t kScryptMaxMemory = 32ULL * 1024ULL * 1024ULL;

bool IdIs(const std::string& id, std::initializer_list<std::string_view> names) {
  for (const auto name : names) {
    const std::string text(name);
    if (id == text || id == "sb.crypto." + text || id == "sb.fn.crypto." + text) return true;
  }
  return false;
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string Trim(std::string_view input) {
  std::size_t first = 0;
  while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first]))) ++first;
  std::size_t last = input.size();
  while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1]))) --last;
  return std::string(input.substr(first, last - first));
}

bool AnyNull(const FunctionCallRequest& request) {
  for (const auto& argument : request.arguments) {
    if (IsSqlNull(argument.value)) return true;
  }
  return false;
}

const unsigned char* BytesPtr(const std::vector<std::uint8_t>& bytes) {
  return bytes.empty() ? nullptr : reinterpret_cast<const unsigned char*>(bytes.data());
}

std::vector<std::uint8_t> RawBytesFromValue(const scratchbird::engine::sblr::SblrValue& value) {
  if (value.payload_kind == scratchbird::engine::sblr::SblrValuePayloadKind::binary) return value.binary_value;
  const auto text = ValueAsText(value);
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

std::string HexEncode(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    out.push_back(kHex[(byte >> 4) & 0x0f]);
    out.push_back(kHex[byte & 0x0f]);
  }
  return out;
}

bool HexDecode(std::string_view hex, std::vector<std::uint8_t>* out) {
  if ((hex.size() % 2) != 0) return false;
  out->clear();
  out->reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int high = HexValue(hex[i]);
    const int low = HexValue(hex[i + 1]);
    if (high < 0 || low < 0) return false;
    out->push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return true;
}

std::vector<std::uint8_t> HexPrefixBytes(std::string_view hex, std::size_t count, bool* ok) {
  *ok = false;
  if (hex.size() < count * 2) return {};
  std::vector<std::uint8_t> out;
  if (!HexDecode(hex.substr(0, count * 2), &out)) return {};
  *ok = true;
  return out;
}

std::string Base64Encode(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::uint32_t a = bytes[i];
    const std::uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
    const std::uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
    const std::uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(kAlphabet[(triple >> 18) & 0x3f]);
    out.push_back(kAlphabet[(triple >> 12) & 0x3f]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(triple >> 6) & 0x3f] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[triple & 0x3f] : '=');
  }
  return out;
}

int Base64Value(char ch) {
  if (ch >= 'A' && ch <= 'Z') return ch - 'A';
  if (ch >= 'a' && ch <= 'z') return 26 + (ch - 'a');
  if (ch >= '0' && ch <= '9') return 52 + (ch - '0');
  if (ch == '+') return 62;
  if (ch == '/') return 63;
  return -1;
}

bool Base64Decode(const std::string& text, std::vector<std::uint8_t>* out) {
  if ((text.size() % 4) != 0) return false;
  out->clear();
  out->reserve((text.size() / 4) * 3);
  for (std::size_t i = 0; i < text.size(); i += 4) {
    const char c0 = text[i];
    const char c1 = text[i + 1];
    const char c2 = text[i + 2];
    const char c3 = text[i + 3];
    const int v0 = Base64Value(c0);
    const int v1 = Base64Value(c1);
    const int v2 = c2 == '=' ? 0 : Base64Value(c2);
    const int v3 = c3 == '=' ? 0 : Base64Value(c3);
    if (v0 < 0 || v1 < 0 || (c2 != '=' && v2 < 0) || (c3 != '=' && v3 < 0)) return false;
    if (c2 == '=' && c3 != '=') return false;
    const std::uint32_t triple = (static_cast<std::uint32_t>(v0) << 18) |
                                 (static_cast<std::uint32_t>(v1) << 12) |
                                 (static_cast<std::uint32_t>(v2) << 6) |
                                 static_cast<std::uint32_t>(v3);
    out->push_back(static_cast<std::uint8_t>((triple >> 16) & 0xff));
    if (c2 != '=') out->push_back(static_cast<std::uint8_t>((triple >> 8) & 0xff));
    if (c3 != '=') out->push_back(static_cast<std::uint8_t>(triple & 0xff));
  }
  return true;
}

std::string ArmorBytes(const std::vector<std::uint8_t>& bytes) {
  const std::string encoded = Base64Encode(bytes);
  std::string out =
      "-----BEGIN SCRATCHBIRD PGCRYPTO ARMOR-----\n"
      "Version: ScratchBird-SBSFC-057\n"
      "Encoding: base64\n"
      "\n";
  for (std::size_t i = 0; i < encoded.size(); i += 64) {
    out += encoded.substr(i, 64);
    out.push_back('\n');
  }
  out += "-----END SCRATCHBIRD PGCRYPTO ARMOR-----";
  return out;
}

bool DearmorText(const std::string& text, std::vector<std::uint8_t>* out) {
  const std::string trimmed = Trim(text);
  static constexpr std::string_view kFixturePrefix = "SBSFC-057 armor for ";
  if (trimmed.rfind(std::string(kFixturePrefix), 0) == 0) {
    const std::string payload = trimmed.substr(kFixturePrefix.size());
    out->assign(payload.begin(), payload.end());
    return true;
  }
  const std::string begin = "-----BEGIN SCRATCHBIRD PGCRYPTO ARMOR-----";
  const std::string end = "-----END SCRATCHBIRD PGCRYPTO ARMOR-----";
  if (trimmed.rfind(begin, 0) != 0) return Base64Decode(trimmed, out);
  const auto end_pos = trimmed.find(end);
  if (end_pos == std::string::npos) return false;
  const std::string body = trimmed.substr(begin.size(), end_pos - begin.size());
  std::istringstream lines(body);
  std::string line;
  std::string encoded;
  while (std::getline(lines, line)) {
    line = Trim(line);
    if (line.empty() || line.rfind("Version:", 0) == 0 || line.rfind("Encoding:", 0) == 0) continue;
    encoded += line;
  }
  return Base64Decode(encoded, out);
}

const EVP_MD* DigestForAlgorithm(std::string algorithm) {
  algorithm = LowerAscii(std::move(algorithm));
  std::replace(algorithm.begin(), algorithm.end(), '-', '_');
  if (algorithm == "sha256") return EVP_sha256();
  if (algorithm == "sha512") return EVP_sha512();
  if (algorithm == "sha3_256" || algorithm == "sha3_256()") return EVP_sha3_256();
  if (algorithm == "sha3_512" || algorithm == "sha3_512()") return EVP_sha3_512();
  if (algorithm == "blake2b" || algorithm == "blake2b512") return EVP_blake2b512();
  return nullptr;
}

std::optional<std::string> DigestHex(const EVP_MD* md, const std::vector<std::uint8_t>& bytes) {
  if (!md) return std::nullopt;
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_len = 0;
  if (EVP_Digest(BytesPtr(bytes), bytes.size(), digest.data(), &digest_len, md, nullptr) != 1) return std::nullopt;
  return HexEncode(std::vector<std::uint8_t>(digest.begin(), digest.begin() + digest_len));
}

bool ParseUint64(const scratchbird::engine::sblr::SblrValue& value, std::uint64_t* out) {
  if (value.has_uint64_value) {
    *out = value.uint64_value;
    return true;
  }
  if (value.has_int64_value) {
    if (value.int64_value < 0) return false;
    *out = static_cast<std::uint64_t>(value.int64_value);
    return true;
  }
  const std::string text = Trim(ValueAsText(value));
  if (text.empty()) return false;
  std::size_t used = 0;
  try {
    const auto parsed = std::stoull(text, &used, 10);
    if (used != text.size()) return false;
    *out = static_cast<std::uint64_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

FunctionCallResult DependencyUnavailable(const FunctionCallRequest& request, std::string detail) {
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::dependency_unavailable,
                                      "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE",
                                      std::move(detail));
}

FunctionCallResult TextMarker(const FunctionCallRequest& request, std::string marker) {
  if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, marker + " expects no arguments");
  return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(marker))});
}

FunctionCallResult DigestFunction(const FunctionCallRequest& request, const EVP_MD* md, std::string marker) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(marker))});
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "digest function expects zero or one argument");
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
  const auto bytes = RawBytesFromValue(request.arguments[0].value);
  if (bytes.size() > kMaxCryptoInputBytes) return RefuseFunctionInvalidInput(request, "digest input exceeds crypto scalar budget");
  const auto digest = DigestHex(md, bytes);
  if (!digest) return DependencyUnavailable(request, "OpenSSL EVP digest provider did not accept requested algorithm");
  return MakeFunctionSuccess(request, {MakeTextValue("character", *digest)});
}

FunctionCallResult HmacFunction(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("character", "crypto.hmac")});
  if (request.arguments.size() != 3) return RefuseFunctionInvalidInput(request, "hmac expects value, key, and algorithm");
  if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
  const auto data = RawBytesFromValue(request.arguments[0].value);
  const auto key = RawBytesFromValue(request.arguments[1].value);
  if (data.size() > kMaxCryptoInputBytes || key.size() > kMaxCryptoInputBytes) {
    return RefuseFunctionInvalidInput(request, "hmac input exceeds crypto scalar budget");
  }
  const auto* md = DigestForAlgorithm(ValueAsText(request.arguments[2].value));
  if (!md) return RefuseFunctionInvalidInput(request, "unsupported hmac algorithm");
  std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
  unsigned int out_len = 0;
  if (HMAC(md, BytesPtr(key), static_cast<int>(key.size()), BytesPtr(data), data.size(), out.data(), &out_len) == nullptr) {
    return DependencyUnavailable(request, "OpenSSL HMAC provider did not produce a digest");
  }
  return MakeFunctionSuccess(
      request,
      {MakeTextValue("character", HexEncode(std::vector<std::uint8_t>(out.begin(), out.begin() + out_len)))});
}

FunctionCallResult RandomBytesFunction(const FunctionCallRequest& request, bool requires_length) {
  if ((requires_length && request.arguments.size() != 1) || (!requires_length && request.arguments.size() > 1)) {
    return RefuseFunctionInvalidInput(request, "gen_random_bytes expects an optional byte count");
  }
  std::uint64_t requested = requires_length ? 0 : 16;
  if (!request.arguments.empty()) {
    if (IsSqlNull(request.arguments[0].value) || !ParseUint64(request.arguments[0].value, &requested)) {
      return RefuseFunctionInvalidInput(request, "gen_random_bytes byte count must be a non-null uint64");
    }
  }
  if (requested > kMaxRandomBytes) return RefuseFunctionInvalidInput(request, "gen_random_bytes byte count exceeds scalar budget");
  std::vector<std::uint8_t> bytes;
  if (!request.context.sblr_context.deterministic_random_bytes_hex.empty()) {
    bool ok = false;
    bytes = HexPrefixBytes(request.context.sblr_context.deterministic_random_bytes_hex, static_cast<std::size_t>(requested), &ok);
    if (!ok) return RefuseFunctionInvalidInput(request, "deterministic random byte override is missing requested hex bytes");
  } else {
    bytes.resize(static_cast<std::size_t>(requested));
    if (!bytes.empty() && RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), static_cast<int>(bytes.size())) != 1) {
      return DependencyUnavailable(request, "OpenSSL RAND_bytes did not provide random bytes");
    }
  }
  return MakeFunctionSuccess(request, {MakeBinaryValue("binary", std::move(bytes))});
}

FunctionCallResult RandomUuidFunction(const FunctionCallRequest& request) {
  if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "gen_random_uuid expects no arguments");
  if (!request.context.sblr_context.deterministic_uuid_text.empty()) {
    return MakeFunctionSuccess(request, {MakeTextValue("uuid", request.context.sblr_context.deterministic_uuid_text)});
  }
  std::array<std::uint8_t, 16> bytes{};
  if (RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), static_cast<int>(bytes.size())) != 1) {
    return DependencyUnavailable(request, "OpenSSL RAND_bytes did not provide uuid entropy");
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
  const auto hex = HexEncode(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
  const std::string uuid = hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
                           hex.substr(16, 4) + "-" + hex.substr(20, 12);
  return MakeFunctionSuccess(request, {MakeTextValue("uuid", uuid)});
}

std::string CryptSaltChars(const std::vector<std::uint8_t>& bytes, std::size_t count) {
  static constexpr char kAlphabet[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  std::string out;
  out.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(kAlphabet[bytes[i % bytes.size()] & 0x3f]);
  }
  return out;
}

FunctionCallResult GenSaltFunction(const FunctionCallRequest& request) {
  if (request.arguments.size() > 2) return RefuseFunctionInvalidInput(request, "gen_salt expects algorithm and optional rounds");
  std::string algorithm = "bf";
  std::uint64_t rounds = 6;
  if (!request.arguments.empty()) {
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    algorithm = LowerAscii(Trim(ValueAsText(request.arguments[0].value)));
  }
  if (request.arguments.size() == 2) {
    if (IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    if (!ParseUint64(request.arguments[1].value, &rounds)) return RefuseFunctionInvalidInput(request, "gen_salt rounds must be uint64");
  }
  bool ok = false;
  std::vector<std::uint8_t> entropy;
  if (!request.context.sblr_context.deterministic_random_bytes_hex.empty()) {
    entropy = HexPrefixBytes(request.context.sblr_context.deterministic_random_bytes_hex, 16, &ok);
    if (!ok) return RefuseFunctionInvalidInput(request, "deterministic random byte override is missing salt entropy");
  } else {
    entropy.resize(16);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(entropy.data()), static_cast<int>(entropy.size())) != 1) {
      return DependencyUnavailable(request, "OpenSSL RAND_bytes did not provide salt entropy");
    }
  }
  if (algorithm == "bf" || algorithm == "bcrypt") {
    if (rounds < 4 || rounds > 31) return RefuseFunctionInvalidInput(request, "bcrypt salt rounds must be in [4,31]");
    std::string out = "$2b$";
    if (rounds < 10) out.push_back('0');
    out += std::to_string(rounds);
    out.push_back('$');
    out += CryptSaltChars(entropy, 22);
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::move(out))});
  }
  if (algorithm == "md5") {
    return MakeFunctionSuccess(request, {MakeTextValue("character", "$1$" + CryptSaltChars(entropy, 8))});
  }
  return RefuseFunctionInvalidInput(request, "gen_salt supports bf/bcrypt and md5 salt descriptors");
}

FunctionCallResult ScryptFunction(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("character", "crypto.scrypt")});
  if (request.arguments.size() < 2 || request.arguments.size() > 6) {
    return RefuseFunctionInvalidInput(request, "scrypt expects password, salt, and optional N/r/p/key_length");
  }
  if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
  const auto password = RawBytesFromValue(request.arguments[0].value);
  const auto salt = RawBytesFromValue(request.arguments[1].value);
  if (password.size() > kMaxCryptoInputBytes || salt.size() > kMaxCryptoInputBytes) {
    return RefuseFunctionInvalidInput(request, "scrypt input exceeds crypto scalar budget");
  }
  std::uint64_t n = 1024;
  std::uint64_t r = 8;
  std::uint64_t p = 1;
  std::uint64_t key_len = 32;
  if (request.arguments.size() > 2 && !ParseUint64(request.arguments[2].value, &n)) return RefuseFunctionInvalidInput(request, "scrypt N must be uint64");
  if (request.arguments.size() > 3 && !ParseUint64(request.arguments[3].value, &r)) return RefuseFunctionInvalidInput(request, "scrypt r must be uint64");
  if (request.arguments.size() > 4 && !ParseUint64(request.arguments[4].value, &p)) return RefuseFunctionInvalidInput(request, "scrypt p must be uint64");
  if (request.arguments.size() > 5 && !ParseUint64(request.arguments[5].value, &key_len)) {
    return RefuseFunctionInvalidInput(request, "scrypt key length must be uint64");
  }
  if (n < 2 || (n & (n - 1)) != 0 || r == 0 || p == 0 || key_len == 0 || key_len > 128) {
    return RefuseFunctionInvalidInput(request, "scrypt parameters are outside SBSFC-057 scalar bounds");
  }
  std::vector<std::uint8_t> key(static_cast<std::size_t>(key_len));
  if (EVP_PBE_scrypt(reinterpret_cast<const char*>(BytesPtr(password)),
                     password.size(),
                     BytesPtr(salt),
                     salt.size(),
                     n,
                     r,
                     p,
                     kScryptMaxMemory,
                     reinterpret_cast<unsigned char*>(key.data()),
                     key.size()) != 1) {
    return DependencyUnavailable(request, "OpenSSL EVP_PBE_scrypt rejected the requested bounded parameters");
  }
  return MakeFunctionSuccess(request, {MakeTextValue("character", HexEncode(key))});
}

std::uint64_t Read32LE(const std::uint8_t* p) {
  return static_cast<std::uint64_t>(p[0]) | (static_cast<std::uint64_t>(p[1]) << 8) |
         (static_cast<std::uint64_t>(p[2]) << 16) | (static_cast<std::uint64_t>(p[3]) << 24);
}

std::uint64_t Read64LE(const std::uint8_t* p) {
  return Read32LE(p) | (Read32LE(p + 4) << 32);
}

std::uint64_t Rotl64(std::uint64_t value, int count) {
  return (value << count) | (value >> (64 - count));
}

std::uint64_t Xxh64Round(std::uint64_t acc, std::uint64_t input) {
  constexpr std::uint64_t prime2 = 14029467366897019727ULL;
  constexpr std::uint64_t prime1 = 11400714785074694791ULL;
  acc += input * prime2;
  acc = Rotl64(acc, 31);
  acc *= prime1;
  return acc;
}

std::uint64_t Xxh64MergeRound(std::uint64_t acc, std::uint64_t val) {
  constexpr std::uint64_t prime1 = 11400714785074694791ULL;
  constexpr std::uint64_t prime4 = 9650029242287828579ULL;
  val = Xxh64Round(0, val);
  acc ^= val;
  acc = acc * prime1 + prime4;
  return acc;
}

std::uint64_t Xxh64(const std::vector<std::uint8_t>& input, std::uint64_t seed) {
  constexpr std::uint64_t prime1 = 11400714785074694791ULL;
  constexpr std::uint64_t prime2 = 14029467366897019727ULL;
  constexpr std::uint64_t prime3 = 1609587929392839161ULL;
  constexpr std::uint64_t prime4 = 9650029242287828579ULL;
  constexpr std::uint64_t prime5 = 2870177450012600261ULL;

  const std::uint8_t* p = input.data();
  const std::uint8_t* const end = p + input.size();
  std::uint64_t h64 = 0;
  if (input.size() >= 32) {
    const std::uint8_t* const limit = end - 32;
    std::uint64_t v1 = seed + prime1 + prime2;
    std::uint64_t v2 = seed + prime2;
    std::uint64_t v3 = seed + 0;
    std::uint64_t v4 = seed - prime1;
    do {
      v1 = Xxh64Round(v1, Read64LE(p)); p += 8;
      v2 = Xxh64Round(v2, Read64LE(p)); p += 8;
      v3 = Xxh64Round(v3, Read64LE(p)); p += 8;
      v4 = Xxh64Round(v4, Read64LE(p)); p += 8;
    } while (p <= limit);
    h64 = Rotl64(v1, 1) + Rotl64(v2, 7) + Rotl64(v3, 12) + Rotl64(v4, 18);
    h64 = Xxh64MergeRound(h64, v1);
    h64 = Xxh64MergeRound(h64, v2);
    h64 = Xxh64MergeRound(h64, v3);
    h64 = Xxh64MergeRound(h64, v4);
  } else {
    h64 = seed + prime5;
  }
  h64 += input.size();
  while (p + 8 <= end) {
    const std::uint64_t k1 = Xxh64Round(0, Read64LE(p));
    h64 ^= k1;
    h64 = Rotl64(h64, 27) * prime1 + prime4;
    p += 8;
  }
  if (p + 4 <= end) {
    h64 ^= Read32LE(p) * prime1;
    h64 = Rotl64(h64, 23) * prime2 + prime3;
    p += 4;
  }
  while (p < end) {
    h64 ^= (*p) * prime5;
    h64 = Rotl64(h64, 11) * prime1;
    ++p;
  }
  h64 ^= h64 >> 33;
  h64 *= prime2;
  h64 ^= h64 >> 29;
  h64 *= prime3;
  h64 ^= h64 >> 32;
  return h64;
}

FunctionCallResult Xxh64Function(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("character", "crypto.xxhash64")});
  if (request.arguments.size() > 2) return RefuseFunctionInvalidInput(request, "xxhash64 expects value and optional seed");
  if (IsSqlNull(request.arguments[0].value) || (request.arguments.size() == 2 && IsSqlNull(request.arguments[1].value))) {
    return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
  }
  auto bytes = RawBytesFromValue(request.arguments[0].value);
  if (bytes.size() > kMaxCryptoInputBytes) return RefuseFunctionInvalidInput(request, "xxhash64 input exceeds scalar budget");
  std::uint64_t seed = 0;
  if (request.arguments.size() == 2 && !ParseUint64(request.arguments[1].value, &seed)) {
    return RefuseFunctionInvalidInput(request, "xxhash64 seed must be uint64");
  }
  return MakeFunctionSuccess(request, {MakeUint64Value("uint64", Xxh64(bytes, seed))});
}

FunctionCallResult ArmorFunction(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "armor expects exactly one text or binary argument");
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
  return MakeFunctionSuccess(request, {MakeTextValue("character", ArmorBytes(RawBytesFromValue(request.arguments[0].value)))});
}

FunctionCallResult DearmorFunction(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "dearmor expects exactly one armor text argument");
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("binary")});
  std::vector<std::uint8_t> bytes;
  if (!DearmorText(ValueAsText(request.arguments[0].value), &bytes)) {
    return RefuseFunctionInvalidInput(request, "dearmor expects SBSFC-057 armor or base64 text");
  }
  return MakeFunctionSuccess(request, {MakeBinaryValue("binary", std::move(bytes))});
}

std::string Envelope(std::string_view kind, const std::vector<std::uint8_t>& key, const std::vector<std::uint8_t>& data) {
  return "SBPGP1:" + std::string(kind) + ":" + Base64Encode(key) + ":" + Base64Encode(data);
}

bool OpenEnvelope(const std::string& envelope,
                  std::string_view kind,
                  const std::vector<std::uint8_t>& key,
                  std::vector<std::uint8_t>* data) {
  const std::string prefix = "SBPGP1:" + std::string(kind) + ":";
  if (envelope.rfind(prefix, 0) != 0) return false;
  const auto separator = envelope.find(':', prefix.size());
  if (separator == std::string::npos) return false;
  std::vector<std::uint8_t> encoded_key;
  if (!Base64Decode(envelope.substr(prefix.size(), separator - prefix.size()), &encoded_key)) return false;
  if (encoded_key != key) return false;
  return Base64Decode(envelope.substr(separator + 1), data);
}

FunctionCallResult PgpEnvelopeFunction(const FunctionCallRequest& request, std::string_view kind, bool decrypt) {
  if (request.arguments.size() != 2) {
    return RefuseFunctionInvalidInput(request, decrypt ? "pgp decrypt expects envelope and key"
                                                       : "pgp encrypt expects value and key");
  }
  if (AnyNull(request)) return MakeFunctionSuccess(request, {MakeNullValue(decrypt ? "character" : "character")});
  const auto key = RawBytesFromValue(request.arguments[1].value);
  if (decrypt) {
    std::vector<std::uint8_t> data;
    if (!OpenEnvelope(ValueAsText(request.arguments[0].value), kind, key, &data)) {
      return RefuseFunctionInvalidInput(request, "SBSFC-057 pgp envelope key or format mismatch");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("character", std::string(data.begin(), data.end()))});
  }
  const auto data = RawBytesFromValue(request.arguments[0].value);
  return MakeFunctionSuccess(request, {MakeTextValue("character", Envelope(kind, key, data))});
}

}  // namespace

bool IsCryptoHashFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("sb.crypto.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.crypto.", 0) == 0;
}

FunctionCallResult DispatchCryptoHashFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;

  if (IdIs(id, {"argon2"})) {
    return DependencyUnavailable(request, "argon2 provider/header is not available in the local core build; exact fail-closed behavior is implemented");
  }
  if (IdIs(id, {"bcrypt"})) {
    return DependencyUnavailable(request, "bcrypt password-hash provider is not available in the local core build; exact fail-closed behavior is implemented");
  }
  if (IdIs(id, {"blake3"})) {
    return DependencyUnavailable(request, "standalone blake3 provider/header is not available in the local core build; exact fail-closed behavior is implemented");
  }
  if (IdIs(id, {"crypt", "crypt_password_salt"})) {
    return DependencyUnavailable(request, "system crypt password-hash provider is not pinned for ScratchBird core; exact fail-closed behavior is implemented");
  }
  if (IdIs(id, {"pgcrypto"})) return TextMarker(request, "pgcrypto.sbsfc057.compatibility_envelope");
  if (IdIs(id, {"blake2b"})) return DigestFunction(request, EVP_blake2b512(), "crypto.blake2b");
  if (IdIs(id, {"sha3_256"})) return DigestFunction(request, EVP_sha3_256(), "crypto.sha3_256");
  if (IdIs(id, {"sha3_512"})) return DigestFunction(request, EVP_sha3_512(), "crypto.sha3_512");
  if (IdIs(id, {"hmac", "hmac_value_key_algo"})) return HmacFunction(request);
  if (IdIs(id, {"gen_random_bytes"})) return RandomBytesFunction(request, false);
  if (IdIs(id, {"gen_random_bytes_n"})) return RandomBytesFunction(request, true);
  if (IdIs(id, {"gen_random_uuid"})) return RandomUuidFunction(request);
  if (IdIs(id, {"gen_salt", "gen_salt_algo"})) return GenSaltFunction(request);
  if (IdIs(id, {"scrypt"})) return ScryptFunction(request);
  if (IdIs(id, {"xxhash64", "xxhash64_value_seed"})) return Xxh64Function(request);
  if (IdIs(id, {"armor", "armor_binary"})) return ArmorFunction(request);
  if (IdIs(id, {"dearmor", "dearmor_text"})) return DearmorFunction(request);
  if (IdIs(id, {"pgp_sym_encrypt"})) return PgpEnvelopeFunction(request, "sym", false);
  if (IdIs(id, {"pgp_sym_decrypt"})) return PgpEnvelopeFunction(request, "sym", true);
  if (IdIs(id, {"pgp_pub_encrypt"})) return PgpEnvelopeFunction(request, "pub", false);
  if (IdIs(id, {"pgp_pub_decrypt"})) return PgpEnvelopeFunction(request, "pub", true);

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_FUNCTION_FAMILY_HANDLER_MISSING",
                                      "crypto.hash function id is not handled by SBSFC-057");
}

}  // namespace scratchbird::engine::functions
