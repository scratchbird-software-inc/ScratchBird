// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/transaction/named_lock_key.hpp"
#include "lexer/lexer.hpp"
#include "transaction_lock.hpp"

#include <atomic>
#include <barrier>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <openssl/evp.h>
#include <openssl/sha.h>

namespace {
namespace mga = scratchbird::transaction::mga;
namespace parser = scratchbird::parser::sbsql;
using scratchbird::engine::internal_api::MakeNamedLockBinaryKey;
using scratchbird::engine::internal_api::NamedLockKeyFingerprint;
using Uuid = scratchbird::core::platform::Uuid;
unsigned checks = 0;
constexpr Uuid database{{0x01, 0x9d, 0, 0, 0, 0, 0x70, 0,
                         0x80, 0, 0, 0, 0, 0, 0x15, 0x01}};
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
void Exact(const Uuid& id, const std::string& data) {
  const auto key = MakeNamedLockBinaryKey(id, data);
  Check(key.size() == 24 + data.size(), "wrong binary key extent");
  Check(key.substr(0, 4) == "NLK1", "wrong binary namespace");
  for (unsigned i = 0; i < 16; ++i)
    Check(static_cast<unsigned char>(key[4 + i]) == id.bytes[i], "database UUID changed");
  std::uint32_t length = 0;
  for (unsigned i = 0; i < 4; ++i)
    length |= static_cast<std::uint32_t>(static_cast<unsigned char>(key[20 + i])) << (8 * i);
  Check(length == data.size(), "wrong little endian length");
  Check(key.substr(24) == data, "semantic name decoded or normalized in engine");
}
std::string Literal(std::string_view sql) {
  auto lexed = parser::Lex(sql);
  Check(lexed.messages.diagnostics.empty(), "actual lexer refused valid SQL literal");
  for (const auto& token : lexed.tokens) {
    if (token.kind == parser::TokenKind::kStringLiteral) return token.text;
  }
  throw std::runtime_error("actual lexer returned no string literal");
}
mga::TransactionLockResult Acquire(mga::LocalTransactionLockTable& table,
                                   unsigned owner, const std::string& key) {
  mga::TransactionLockRequest request;
  request.requester = mga::MakeLocalTransactionId(owner);
  request.resource_key = key;
  return table.Acquire(std::move(request));
}
void Cases() {
  const auto plain = Literal("LOCK NAMED 'gate';");
  const auto quoted_data = Literal("LOCK NAMED '''gate''';");
  const auto dollars = Literal("LOCK NAMED $key$'gate'$key$;");
  Check(plain == "gate" && quoted_data == "'gate'" && dollars == quoted_data,
        "parser literal semantic values differ");
  const auto plain_key = MakeNamedLockBinaryKey(database, plain);
  const auto quoted_key = MakeNamedLockBinaryKey(database, quoted_data);
  unsigned char digest[SHA256_DIGEST_LENGTH];
  Check(SHA256(reinterpret_cast<const unsigned char*>(plain_key.data()), plain_key.size(), digest),
        "independent digest failed");
  std::string expected_fingerprint = "sha256:";
  for (auto byte : digest) {
    expected_fingerprint += "0123456789abcdef"[byte >> 4];
    expected_fingerprint += "0123456789abcdef"[byte & 15];
  }
  Check(NamedLockKeyFingerprint(plain_key) == expected_fingerprint, "wrong observable fingerprint");
  {
    struct RestoreProvider {
      ~RestoreProvider() { EVP_set_default_properties(nullptr, ""); }
    } restore;
    Check(EVP_set_default_properties(nullptr, "provider=sb_missing_test_provider") == 1,
          "hash provider fault not installed");
    Check(NamedLockKeyFingerprint(plain_key).empty(), "hash failure fabricated fingerprint");
  }
  Check(NamedLockKeyFingerprint(plain_key) == expected_fingerprint, "hash provider retry failed");
  Check(plain_key != quoted_key, "double unquoting aliases distinct named locks");
  Check(quoted_key == MakeNamedLockBinaryKey(database, dollars), "equivalent syntax changed key");
  for (const std::string data : {plain, quoted_data, std::string("a\0b", 3),
                                std::string(" a "), std::string("A"),
                                std::string("a:b:c"), std::string("é"),
                                std::string("019d0000-0000-4000-8000-000000001501")})
    Exact(database, data);
  Exact(database, std::string(65536, 'x'));
  Check(MakeNamedLockBinaryKey(database, {}).empty(), "empty key accepted");
  Check(MakeNamedLockBinaryKey(database, std::string(65537, 'x')).empty(), "oversized key accepted");
  Check(MakeNamedLockBinaryKey({}, plain).empty(), "unknown database fallback");
  for (unsigned version = 0; version < 16; ++version) {
    auto id = database;
    id.bytes[6] = static_cast<std::uint8_t>(version << 4);
    if (version != 7) Check(MakeNamedLockBinaryKey(id, plain).empty(), "non-v7 database accepted");
  }
  for (unsigned variant : {0x00u, 0x40u, 0xc0u}) {
    auto id = database;
    id.bytes[8] = static_cast<std::uint8_t>(variant);
    Check(MakeNamedLockBinaryKey(id, plain).empty(), "invalid UUID variant accepted");
  }
  for (unsigned byte = 0; byte < 16; ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      if ((byte == 6 && bit >= 4) || (byte == 8 && bit >= 6)) continue;
      auto id = database;
      id.bytes[byte] ^= (1u << bit);
      Exact(id, plain);
      Check(MakeNamedLockBinaryKey(id, plain) != plain_key, "database identity bit lost");
    }
  }

  mga::LocalTransactionLockTable table;
  Check(Acquire(table, 1, plain_key).ok(), "plain lock acquisition failed");
  Check(Acquire(table, 2, quoted_key).ok(), "quoted-data lock aliases plain key");
  Check(!Acquire(table, 3, plain_key).ok(), "contention allowed second owner");
  Check(!table.Release(mga::MakeLocalTransactionId(2), plain_key).ok(), "foreign owner released lock");
  Check(table.held_lock_count() == 2, "wrong held lock count");
  Check(table.Release(mga::MakeLocalTransactionId(1), plain_key).ok(), "owner release failed");
  Check(Acquire(table, 3, plain_key).ok(), "released key unavailable");
  Check(table.ReleaseAll(mga::MakeLocalTransactionId(2)) == 1, "quoted owner retirement failed");
  Check(table.ReleaseAll(mga::MakeLocalTransactionId(3)) == 1, "plain owner retirement failed");
  Check(table.held_lock_count() == 0, "lock ownership leaked");

  constexpr unsigned count = 16;
  std::barrier start(count);
  std::barrier acquired(count);
  std::atomic<unsigned> winners{0}, release_failures{0};
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < count; ++i) {
    threads.emplace_back([&, i] {
      start.arrive_and_wait();
      const bool won = Acquire(table, i + 1, plain_key).ok();
      if (won) ++winners;
      acquired.arrive_and_wait();
      if (won && !table.Release(mga::MakeLocalTransactionId(i + 1), plain_key).ok())
        ++release_failures;
    });
  }
  for (auto& thread : threads) thread.join();
  Check(winners == 1 && release_failures == 0, "concurrent ownership not exclusive");
  Check(table.held_lock_count() == 0, "concurrent owner leaked");
  std::cout << "PASS checks=" << checks << " concurrent_contenders=" << count << '\n';
}
}  // namespace
int main() {
  try { Cases(); return 0; }
  catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
