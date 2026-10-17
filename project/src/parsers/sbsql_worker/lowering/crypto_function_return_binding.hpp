// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace scratchbird::parser::sbsql {

// Closed syntax/return declarations from Core's manifest-listed
// chapters/functions/appendix-crypto-function-library.md and
// registries/builtin-expression-registry.yaml. These labels are not executable
// identities, descriptor occurrences, policy/profile selection or admission.
// Executable UUIDs still come exclusively from the engine-issued cohort.
enum class CryptoArgumentFamily {
  absent, character, binary, character_or_binary, uint16, uint32, uint64,
  untyped_null
};

struct CryptoFunctionReturnDeclaration {
  std::string_view builtin_id;
  std::string_view sql_name;
  // Empty only for the package capability spelling: it has NO scalar result.
  std::string_view result_type;
  std::size_t minimum_arguments;
  std::size_t maximum_arguments;
  std::array<CryptoArgumentFamily, 6> arguments{};
  bool matching_first_two_families = false;
};

inline constexpr auto kCryptoFunctionReturnDeclarations = [] {
  using F = CryptoArgumentFamily;
  return std::array<CryptoFunctionReturnDeclaration, 27>{{
      {"sb.crypto.argon2", "argon2", "binary", 6, 6,
       {F::character, F::binary, F::uint32, F::uint32, F::uint16, F::uint16}},
      {"sb.crypto.armor", "armor", "character", 1, 1, {F::binary}},
      {"sb.crypto.armor_binary", "armor_binary", "binary", 1, 1, {F::binary}},
      {"sb.crypto.bcrypt", "bcrypt", "character", 2, 2, {F::character, F::character}},
      {"sb.crypto.blake2b", "blake2b", "binary", 1, 1, {F::binary}},
      {"sb.crypto.blake3", "blake3", "binary", 1, 1, {F::binary}},
      {"sb.crypto.crypt", "crypt", "character", 2, 2, {F::character, F::character}},
      {"sb.crypto.crypt_password_salt", "crypt_password_salt", "character", 2, 2,
       {F::character, F::character}},
      {"sb.crypto.dearmor", "dearmor", "binary", 1, 1, {F::character}},
      {"sb.crypto.dearmor_text", "dearmor_text", "character", 1, 1, {F::character}},
      {"sb.crypto.gen_random_bytes", "gen_random_bytes", "binary", 1, 1, {F::uint32}},
      {"sb.crypto.gen_random_bytes_n", "gen_random_bytes_n", "binary", 1, 1, {F::uint32}},
      {"sb.crypto.gen_random_uuid", "gen_random_uuid", "uuid", 0, 0, {}},
      {"sb.crypto.gen_salt", "gen_salt", "character", 1, 2, {F::character, F::uint32}},
      {"sb.crypto.gen_salt_algo", "gen_salt_algo", "character", 2, 2,
       {F::character, F::uint32}},
      {"sb.crypto.hmac", "hmac", "binary", 3, 3,
       {F::character_or_binary, F::character_or_binary, F::character}, true},
      {"sb.crypto.hmac_value_key_algo", "hmac_value_key_algo", "binary", 3, 3,
       {F::character_or_binary, F::character_or_binary, F::character}, true},
      {"sb.crypto.pgcrypto", "pgcrypto", {}, 0, 0, {}},
      // With three arguments the third remains options. Only the exact
      // four-argument shape adds an explicit passphrase before options;
      // this syntax declaration never admits or accesses either secret.
      {"sb.crypto.pgp_pub_decrypt", "pgp_pub_decrypt", "binary", 2, 4,
       {F::binary, F::binary, F::character, F::character}},
      {"sb.crypto.pgp_pub_encrypt", "pgp_pub_encrypt", "binary", 2, 3,
       {F::binary, F::binary, F::character}},
      {"sb.crypto.pgp_sym_decrypt", "pgp_sym_decrypt", "character", 2, 3,
       {F::binary, F::character, F::character}},
      {"sb.crypto.pgp_sym_encrypt", "pgp_sym_encrypt", "binary", 2, 3,
       {F::character, F::character, F::character}},
      {"sb.crypto.scrypt", "scrypt", "binary", 6, 6,
       {F::character, F::binary, F::uint64, F::uint32, F::uint32, F::uint16}},
      {"sb.crypto.sha3_256", "sha3_256", "binary", 1, 1, {F::binary}},
      {"sb.crypto.sha3_512", "sha3_512", "binary", 1, 1, {F::binary}},
      {"sb.crypto.xxhash64", "xxhash64", "uint64", 1, 1, {F::character_or_binary}},
      {"sb.crypto.xxhash64_value_seed", "xxhash64_value_seed", "uint64", 2, 2,
       {F::character_or_binary, F::uint64}},
  }};
}();

static_assert([] {
  constexpr std::string_view prefix = "sb.crypto.";
  for (std::size_t i = 0; i < kCryptoFunctionReturnDeclarations.size(); ++i) {
    const auto& d = kCryptoFunctionReturnDeclarations[i];
    if (!d.builtin_id.starts_with(prefix) || d.builtin_id.substr(prefix.size()) != d.sql_name ||
        d.minimum_arguments > d.maximum_arguments || d.maximum_arguments > d.arguments.size()) return false;
    for (std::size_t j = 0; j < d.arguments.size(); ++j)
      if ((d.arguments[j] == CryptoArgumentFamily::absent) != (j >= d.maximum_arguments)) return false;
    for (std::size_t j = 0; j < i; ++j)
      if (d.builtin_id == kCryptoFunctionReturnDeclarations[j].builtin_id ||
          d.sql_name == kCryptoFunctionReturnDeclarations[j].sql_name) return false;
  }
  return true;
}());

// Input has already undergone the parser's normal SQL identifier folding.
// Exact names only: no family-prefix admission, suffixes or donor-name guessing.
inline constexpr const CryptoFunctionReturnDeclaration* FindCryptoFunctionReturnDeclaration(
    std::string_view name) noexcept {
  for (const auto& declaration : kCryptoFunctionReturnDeclarations)
    if (name == declaration.builtin_id || name == declaration.sql_name) return &declaration;
  return nullptr;
}

inline constexpr CryptoArgumentFamily CryptoArgumentFamilyForType(
    std::string_view type) noexcept {
  using F = CryptoArgumentFamily;
  if (type == "character" || type == "text") return F::character;
  if (type == "binary" || type == "bytea") return F::binary;
  if (type == "uint16") return F::uint16;
  if (type == "uint32") return F::uint32;
  if (type == "uint64") return F::uint64;
  if (type == "null") return F::untyped_null;
  return F::absent;
}

// Select only a declared result after matching the exact Core overload shape.
// A literal NULL can leave a formal argument unresolved because every declared
// overload of each listed identity has the SAME result family. This does NOT
// bind that NULL's descriptor or create a contextual occurrence/default profile.
// Those remain the owning typed binder's responsibility before transport.
inline constexpr std::optional<std::string_view> SelectCryptoFunctionReturnType(
    std::string_view name, std::span<const std::string_view> arguments) noexcept {
  const auto* declaration = FindCryptoFunctionReturnDeclaration(name);
  if (!declaration || declaration->result_type.empty() ||
      arguments.size() < declaration->minimum_arguments ||
      arguments.size() > declaration->maximum_arguments) return std::nullopt;
  using F = CryptoArgumentFamily;
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const auto family = CryptoArgumentFamilyForType(arguments[i]);
    const auto expected = declaration->arguments[i];
    if (family != F::untyped_null && family != expected &&
        !(expected == F::character_or_binary && (family == F::character || family == F::binary)))
      return std::nullopt;
  }
  if (declaration->matching_first_two_families) {
    const auto first = CryptoArgumentFamilyForType(arguments[0]);
    const auto second = CryptoArgumentFamilyForType(arguments[1]);
    if (first != F::untyped_null && second != F::untyped_null && first != second) return std::nullopt;
  }
  return declaration->result_type;
}

}  // namespace scratchbird::parser::sbsql
