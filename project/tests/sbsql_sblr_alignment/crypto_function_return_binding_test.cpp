// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Syntax/overload result selection only: no live descriptor, admission or E2E claim.
#include "lowering/crypto_function_return_binding.hpp"
#include "lowering/lowering.hpp"
#include "cst/cst.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace p = scratchbird::parser::sbsql;
namespace {
unsigned checks = 0;
void Check(bool value, const char* reason) {
  ++checks;
  if (!value) throw std::runtime_error(reason);
}
struct Case {
  std::string_view id, result;
  std::vector<std::string_view> arguments;
};
// Independent complete list of scalar contracts, not derived from the table
// under test. The package capability is deliberately not a scalar case.
const std::array<Case, 26> cases{{
    {"sb.crypto.argon2", "binary", {"text", "bytea", "uint32", "uint32", "uint16", "uint16"}},
    {"sb.crypto.armor", "character", {"bytea"}},
    {"sb.crypto.armor_binary", "binary", {"bytea"}},
    {"sb.crypto.bcrypt", "character", {"text", "text"}},
    {"sb.crypto.blake2b", "binary", {"bytea"}},
    {"sb.crypto.blake3", "binary", {"bytea"}},
    {"sb.crypto.crypt", "character", {"text", "text"}},
    {"sb.crypto.crypt_password_salt", "character", {"text", "text"}},
    {"sb.crypto.dearmor", "binary", {"text"}},
    {"sb.crypto.dearmor_text", "character", {"text"}},
    {"sb.crypto.gen_random_bytes", "binary", {"uint32"}},
    {"sb.crypto.gen_random_bytes_n", "binary", {"uint32"}},
    {"sb.crypto.gen_random_uuid", "uuid", {}},
    {"sb.crypto.gen_salt", "character", {"text"}},
    {"sb.crypto.gen_salt_algo", "character", {"text", "uint32"}},
    {"sb.crypto.hmac", "binary", {"text", "text", "text"}},
    {"sb.crypto.hmac_value_key_algo", "binary", {"bytea", "bytea", "text"}},
    {"sb.crypto.pgp_pub_decrypt", "binary", {"bytea", "bytea"}},
    {"sb.crypto.pgp_pub_encrypt", "binary", {"bytea", "bytea"}},
    {"sb.crypto.pgp_sym_decrypt", "character", {"bytea", "text"}},
    {"sb.crypto.pgp_sym_encrypt", "binary", {"text", "text"}},
    {"sb.crypto.scrypt", "binary", {"text", "bytea", "uint64", "uint32", "uint32", "uint16"}},
    {"sb.crypto.sha3_256", "binary", {"bytea"}},
    {"sb.crypto.sha3_512", "binary", {"bytea"}},
    {"sb.crypto.xxhash64", "uint64", {"text"}},
    {"sb.crypto.xxhash64_value_seed", "uint64", {"bytea", "uint64"}},
}};

void CompleteDeclarations() {
  Check(p::kCryptoFunctionReturnDeclarations.size() == cases.size() + 1,
        "declaration list must cover exactly every scalar plus package capability");
  for (const auto& test : cases) {
    const auto* declared = p::FindCryptoFunctionReturnDeclaration(test.id);
    Check(declared && declared->result_type == test.result, "declared scalar return differs from Core contract");
    const auto selected = p::SelectCryptoFunctionReturnType(test.id, test.arguments);
    Check(selected && *selected == test.result, "exact declared overload did not select its result");
    Check(p::FindCryptoFunctionReturnDeclaration(declared->sql_name) == declared,
          "canonical SQL name did not resolve to same declaration");
    Check(p::SelectCryptoFunctionReturnType(declared->sql_name, test.arguments) == selected,
          "SQL spelling changed selected result");
    for (std::size_t i = 0; i < test.arguments.size(); ++i) {
      auto invalid = test.arguments;
      invalid[i] = "uuid";
      Check(!p::SelectCryptoFunctionReturnType(test.id, invalid), "unrelated argument type acquired a result");
      auto unresolved = test.arguments;
      unresolved[i] = "null";
      Check(p::SelectCryptoFunctionReturnType(test.id, unresolved) == selected,
            "literal NULL changed the unambiguous result family");
      auto internal_names = test.arguments;
      for (auto& type : internal_names) {
        if (type == "text") type = "character";
        if (type == "bytea") type = "binary";
      }
      Check(p::SelectCryptoFunctionReturnType(test.id, internal_names) == selected,
            "native parser family labels changed result selection");
    }
    auto excess = test.arguments;
    excess.resize(7, "null");
    Check(!p::SelectCryptoFunctionReturnType(test.id, excess), "unbounded arity admitted");
    for (std::size_t arity = 0; arity < declared->minimum_arguments; ++arity)
      Check(!p::SelectCryptoFunctionReturnType(test.id,
              std::span<const std::string_view>(test.arguments.data(), arity)), "missing mandatory operand admitted");
    const std::string collision = std::string(test.id) + ".other";
    Check(!p::SelectCryptoFunctionReturnType(collision, test.arguments), "namespace suffix acquired declared result");
  }
  for (std::string_view name : {"sb.crypto.pgcrypto", "pgcrypto"}) {
    const auto* capability = p::FindCryptoFunctionReturnDeclaration(name);
    Check(capability && capability->result_type.empty(), "package acquired scalar return declaration");
    Check(!p::SelectCryptoFunctionReturnType(name, {}), "package marker acquired scalar result");
  }
  for (std::string_view name : {"", "sb.crypto.not_registered", "sb.crypto.fake_random_bytes",
       "sb.crypto.dearmor_fake", "sb.crypto.pgp_pub_encrypt_extra", "sb.fn.crypto.armor", "crypto.armor"})
    Check(!p::FindCryptoFunctionReturnDeclaration(name), "undeclared crypto spelling admitted");
}

void AlternativesAndWidths() {
  const std::array<std::string_view, 2> salt{"text", "uint32"};
  Check(p::SelectCryptoFunctionReturnType("gen_salt", salt) == "character", "optional explicit salt cost result");
  for (std::string_view id : {"pgp_pub_decrypt", "pgp_pub_encrypt", "pgp_sym_decrypt", "pgp_sym_encrypt"}) {
    const auto* row = p::FindCryptoFunctionReturnDeclaration(id);
    std::array<std::string_view, 3> args{
        id == "pgp_sym_encrypt" ? "text" : "binary",
        id.starts_with("pgp_sym") ? "text" : "binary", "text"};
    Check(p::SelectCryptoFunctionReturnType(id, args) == row->result_type, "optional PGP options changed return");
    args[2] = "binary";
    Check(!p::SelectCryptoFunctionReturnType(id, args), "binary options accepted as text");
  }
  for (std::string_view id : {"hmac", "hmac_value_key_algo"})
    for (std::string_view a : {"text", "binary", "null"})
      for (std::string_view b : {"text", "binary", "null"}) {
        const std::array<std::string_view, 3> args{a, b, "text"};
        Check(p::SelectCryptoFunctionReturnType(id, args).has_value() ==
                  (a == b || a == "null" || b == "null"), "HMAC representation family matching violated");
      }
  for (std::string_view type : {"binary", "text"}) {
    const std::array<std::string_view, 1> plain{type};
    const std::array<std::string_view, 2> seeded{type, "uint64"};
    Check(p::SelectCryptoFunctionReturnType("xxhash64", plain) == "uint64", "XXH64 representation alternative");
    Check(p::SelectCryptoFunctionReturnType("xxhash64_value_seed", seeded) == "uint64", "XXH64 seeded alternative");
  }
  for (std::string_view wrong : {"uint16", "uint64", "int32", "int64", "text", "numeric"}) {
    const std::array<std::string_view, 1> args{wrong};
    Check(!p::SelectCryptoFunctionReturnType("gen_random_bytes", args), "random count implicitly narrowed or parsed");
  }
}

void ActualScalarSyntax() {
  // These exercise the real scalar analyzer, not just the declaration helper.
  for (std::string_view sql : {
       "SELECT sb.crypto.blake3(X'00ff');", "SELECT armor_binary(X'00ff');",
       "SELECT dearmor_text('packet');", "SELECT hmac('a','b','sha256');",
       "SELECT pgp_sym_encrypt('message','password');",
       "SELECT pgp_sym_decrypt(X'00','password','armor=false');",
       "SELECT xxhash64('a');", "SELECT gen_random_uuid();"})
    Check(p::HasScalarProjectionDemand(p::BuildCst(sql)), "declared crypto syntax did not select scalar route");
  for (std::string_view sql : {
       "SELECT sb.crypto.unknown(X'00');", "SELECT sb.crypto.fake_random_bytes();",
       "SELECT sb.crypto.blake3('not bytea');", "SELECT armor_binary('not bytea');",
       "SELECT hmac('a',X'00','sha256');", "SELECT hmac();",
       "SELECT pgp_sym_encrypt();", "SELECT gen_random_bytes();",
       "SELECT gen_random_bytes(7);", "SELECT pgcrypto();"})
    Check(!p::HasScalarProjectionDemand(p::BuildCst(sql)), "undeclared crypto shape acquired scalar result");
}

void PublicDecryptExplicitPassphraseOverload() {
  using F = p::CryptoArgumentFamily;
  for (const auto name : {"pgp_pub_decrypt", "sb.crypto.pgp_pub_decrypt"}) {
    const auto* declaration = p::FindCryptoFunctionReturnDeclaration(name);
    Check(declaration && declaration->minimum_arguments == 2 && declaration->maximum_arguments == 4 &&
        declaration->arguments[0] == F::binary && declaration->arguments[1] == F::binary &&
        declaration->arguments[2] == F::character && declaration->arguments[3] == F::character,
        "public decrypt exact2/3/4 argument contract missing");
    const std::array<std::string_view, 4> arguments{"bytea", "bytea", "text", "text"};
    for (std::size_t count = 0; count <= arguments.size(); ++count) {
      const auto selected = p::SelectCryptoFunctionReturnType(name,
          std::span<const std::string_view>(arguments.data(), count));
      Check(selected.has_value() == (count >= 2) && (!selected || *selected == "binary"),
          "public decrypt overload arity or return changed");
    }
    for (std::size_t count = 5; count != 9; ++count) {
      const std::vector<std::string_view> excess(count, "null");
      Check(!p::SelectCryptoFunctionReturnType(name, excess), "public decrypt excess NULL operands admitted");
    }
    for (std::size_t mask = 0; mask != 16; ++mask) {
      auto aliases = arguments;
      aliases[0] = mask & 1 ? "binary" : "bytea";
      aliases[1] = mask & 2 ? "binary" : "bytea";
      aliases[2] = mask & 4 ? "character" : "text";
      aliases[3] = mask & 8 ? "character" : "text";
      Check(p::SelectCryptoFunctionReturnType(name, aliases) == "binary",
          "public decrypt native family alias changed exact overload");
    }
    for (std::size_t position = 0; position != arguments.size(); ++position) {
      for (const auto wrong : {"uuid", "uint16", "uint32", "uint64", "int32", "int64",
                              "numeric", "boolean", "unknown", "", "blob"}) {
        auto invalid = arguments;
        invalid[position] = wrong;
        Check(!p::SelectCryptoFunctionReturnType(name, invalid),
            "public decrypt unrelated typed carrier acquired result");
      }
      auto crossed_family = arguments;
      crossed_family[position] = position < 2 ? "text" : "binary";
      Check(!p::SelectCryptoFunctionReturnType(name, crossed_family),
          "public decrypt implicitly crossed bytea/text boundary");
      // Result-family selection for an unresolved SQL NULL is not a typed
      // occurrence, carrier, receipt, key-use or strict-NULL execution grant.
      // A NULL already typed with the wrong family is rejected above.
      auto unresolved_null = arguments;
      unresolved_null[position] = "null";
      Check(p::SelectCryptoFunctionReturnType(name, unresolved_null) == "binary",
          "unresolved NULL changed sole declared result family");
    }
    const std::array<std::string_view, 4> all_null{"null", "null", "null", "null"};
    Check(p::SelectCryptoFunctionReturnType(name, all_null) == "binary",
          "sole result family became ambiguous for four unresolved NULLs");
  }
  for (const auto name : {"pgp_pub_encrypt", "pgp_sym_decrypt", "pgp_sym_encrypt"}) {
    const auto* declaration = p::FindCryptoFunctionReturnDeclaration(name);
    Check(declaration && declaration->minimum_arguments == 2 && declaration->maximum_arguments == 3,
          "public decrypt overload widened another PGP builtin");
    const std::array<std::string_view, 4> all_null{"null", "null", "null", "null"};
    Check(!p::SelectCryptoFunctionReturnType(name, all_null), "another PGP builtin acquired four-argument overload");
  }
  for (std::string_view sql : {
       "SELECT pgp_pub_decrypt(X'00',X'01');",
       "SELECT pgp_pub_decrypt(X'00',X'01','armor=false');",
       "SELECT pgp_pub_decrypt(X'00',X'01','passphrase','armor=false');",
       "SELECT sb.crypto.pgp_pub_decrypt(X'00',X'01','passphrase','armor=false');"})
    Check(p::HasScalarProjectionDemand(p::BuildCst(sql)),
        "actual public decrypt2/3/4 argument syntax did not select scalar route");
  for (std::string_view sql : {
       "SELECT pgp_pub_decrypt();", "SELECT pgp_pub_decrypt(X'00');",
       "SELECT pgp_pub_decrypt(X'00',X'01','passphrase','armor=false','extra');",
       "SELECT pgp_pub_decrypt('message',X'01','passphrase','armor=false');",
       "SELECT pgp_pub_decrypt(X'00','private-key','passphrase','armor=false');",
       "SELECT pgp_pub_decrypt(X'00',X'01',X'02','armor=false');",
       "SELECT pgp_pub_decrypt(X'00',X'01','passphrase',X'02');",
       "SELECT pgp_pub_decrypt(X'00',X'01',X'02');",
       "SELECT pgp_pub_encrypt(X'00',X'01','passphrase','armor=false');",
       "SELECT pgp_sym_encrypt('message','password','passphrase','armor=false');",
       "SELECT pgp_sym_decrypt(X'00','password','passphrase','armor=false');"})
    Check(!p::HasScalarProjectionDemand(p::BuildCst(sql)),
        "actual PGP parser accepted wrong overload, arity or bytea/text carrier");
}
}  // namespace

int main() {
  try {
    CompleteDeclarations();
    AlternativesAndWidths();
    ActualScalarSyntax();
    PublicDecryptExplicitPassphraseOverload();
    std::cout << "crypto_function_return_binding=passed checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "crypto_function_return_binding=failed checks=" << checks << " reason=" << error.what() << '\n';
    return 1;
  }
}
