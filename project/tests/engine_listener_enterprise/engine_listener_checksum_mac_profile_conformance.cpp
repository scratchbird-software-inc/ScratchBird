// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_binary.hpp"
#include "hash_digest.hpp"
#include "page_body_integrity.hpp"
#include "row_data_page.hpp"
#include "transaction_inventory.hpp"
#include "transaction_inventory_page.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace dt = scratchbird::core::datatypes;
namespace hash = scratchbird::core::hash;
namespace page = scratchbird::storage::page;
namespace txn = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;

inline constexpr u64 kBaseMillis = 1770300000000ull;
inline constexpr scratchbird::core::platform::u32 kPageSize = 8192;
inline constexpr std::size_t kTxnChecksumDigestOffset = 64;
inline constexpr std::size_t kTxnChainDigestOffset = 112;

bool Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

std::vector<byte> Bytes(std::string_view value) {
  return {value.begin(), value.end()};
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

page::RowDataPageBody RowBody() {
  page::RowDataPageBody body;
  body.relation_uuid = MakeUuid(UuidKind::object, 10);
  body.segment_id = 1;
  body.segment_generation = 1;
  body.page_number = 11;
  body.page_generation = 3;

  page::RowDataRecord row;
  row.row_uuid = MakeUuid(UuidKind::row, 11);
  row.transaction_uuid = MakeUuid(UuidKind::transaction, 12);
  row.local_transaction_id = 100;
  row.row_version = 1;
  row.stable_slot_id = 101;
  body.rows.push_back(row);
  return body;
}

dt::DatatypeDescriptorRecord Record(std::string name, std::string value) {
  dt::DatatypeDescriptorRecord record;
  record.field_name = std::move(name);
  record.payload = Bytes(value);
  return record;
}

dt::DatatypeDescriptorEnvelope Envelope(
    dt::DatatypeDescriptorIntegrityProfile profile) {
  dt::DatatypeDescriptorEnvelope envelope;
  envelope.kind = dt::DatatypeDescriptorEnvelopeKind::domain_policy;
  envelope.integrity_profile = profile;
  envelope.records.push_back(Record("domain_uuid",
                                    "019dffff-0000-7000-8000-000000000017"));
  envelope.records.push_back(Record("base_type", "integer"));
  envelope.records.push_back(Record("policy_envelope",
                                    "sblr_predicate:value_between:1:100"));
  return envelope;
}

txn::LocalTransactionInventory Inventory(bool* ok) {
  auto inventory = txn::MakeEmptyLocalTransactionInventory();
  const auto first =
      txn::BeginLocalTransaction(inventory,
                                 MakeUuid(UuidKind::transaction, 21),
                                 kBaseMillis + 21);
  *ok = Require(first.ok(), "first transaction begin failed") && *ok;
  const auto committed =
      txn::CommitLocalTransaction(first.inventory,
                                  first.entry.identity.local_id,
                                  kBaseMillis + 22);
  *ok = Require(committed.ok(), "first transaction commit failed") && *ok;
  const auto second =
      txn::BeginLocalTransaction(committed.inventory,
                                 MakeUuid(UuidKind::transaction, 23),
                                 kBaseMillis + 23);
  *ok = Require(second.ok(), "second transaction begin failed") && *ok;
  const auto rolled_back =
      txn::RollbackLocalTransaction(second.inventory,
                                    second.entry.identity.local_id,
                                    kBaseMillis + 24);
  *ok = Require(rolled_back.ok(), "second transaction rollback failed") && *ok;
  return rolled_back.inventory;
}

void RewriteTransactionChecksum(std::vector<byte>* serialized) {
  const auto digest = page::ComputeTransactionInventoryPageChecksumDigest(*serialized);
  std::copy(digest.begin(),
            digest.end(),
            serialized->begin() +
                static_cast<std::ptrdiff_t>(kTxnChecksumDigestOffset));
}

bool TestCoreHashVectors() {
  bool ok = true;
  const auto sha = hash::ComputeSha256Digest(Bytes("abc"));
  ok = Require(sha.ok(), "SHA-256 digest did not compute") && ok;
  ok = Require(hash::HexLower(sha.digest) ==
                   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "SHA-256 known answer mismatch") &&
       ok;

  const auto hmac = hash::ComputeHmacSha256Digest(
      Bytes("key"), Bytes("The quick brown fox jumps over the lazy dog"));
  ok = Require(hmac.ok(), "HMAC-SHA-256 digest did not compute") && ok;
  ok = Require(hash::HexLower(hmac.digest) ==
                   "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
               "HMAC-SHA-256 known answer mismatch") &&
       ok;
  return ok;
}

bool TestPageBodyProfiles() {
  bool ok = true;
  const auto built = page::BuildRowDataPageBody(RowBody(), kPageSize);
  ok = Require(built.ok(), "row body did not build") && ok;
  if (!ok) {
    return false;
  }

  const auto strong = page::ComputePageBodyChecksumDigest(
      page::PageBodyChecksumProfile::strong,
      built.serialized);
  const auto expected_sha = hash::ComputeSha256Digest(built.serialized);
  ok = Require(strong.ok(), "strong page-body digest did not compute") && ok;
  ok = Require(expected_sha.ok(), "expected SHA-256 digest did not compute") && ok;
  ok = Require(strong.digest.digest_algorithm == "sha256",
               "strong page-body profile did not use SHA-256") &&
       ok;
  ok = Require(strong.digest.digest_bytes == hash::kSha256DigestBytes,
               "strong page-body profile did not use full digest bytes") &&
       ok;
  ok = Require(strong.digest.digest_material ==
                   hash::DigestVector(expected_sha.digest),
               "strong page-body digest material mismatch") &&
       ok;
  ok = Require(page::VerifyPageBodyChecksumDigestMaterial(
                   page::PageBodyChecksumProfile::strong,
                   built.serialized,
                   strong.digest.digest_material)
                   .matched,
               "strong page-body full digest did not verify") &&
       ok;

  auto tampered_material = strong.digest.digest_material;
  tampered_material[0] ^= 0x01u;
  ok = Require(!page::VerifyPageBodyChecksumDigestMaterial(
                    page::PageBodyChecksumProfile::strong,
                    built.serialized,
                    tampered_material)
                    .ok(),
               "strong page-body accepted tampered full digest material") &&
       ok;

  const auto missing_key = page::ComputePageBodyChecksumDigest(
      page::PageBodyChecksumProfile::protected_keyed,
      built.serialized);
  ok = Require(!missing_key.ok(),
               "protected page-body profile accepted missing key material") &&
       ok;

  const auto key = Bytes("eler-017-page-key");
  const auto protected_digest = page::ComputePageBodyChecksumDigest(
      page::PageBodyChecksumProfile::protected_keyed,
      built.serialized,
      key);
  const auto expected_hmac = hash::ComputeHmacSha256Digest(key, built.serialized);
  ok = Require(protected_digest.ok(),
               "protected page-body digest did not compute") &&
       ok;
  ok = Require(expected_hmac.ok(), "expected HMAC digest did not compute") && ok;
  ok = Require(protected_digest.digest.digest_algorithm == "hmac-sha256",
               "protected page-body profile did not use HMAC-SHA-256") &&
       ok;
  ok = Require(protected_digest.digest.digest_material ==
                   hash::DigestVector(expected_hmac.digest),
               "protected page-body digest material mismatch") &&
       ok;
  ok = Require(!page::VerifyPageBodyChecksumDigestMaterial(
                    page::PageBodyChecksumProfile::protected_keyed,
                    built.serialized,
                    protected_digest.digest.digest_material,
                    Bytes("wrong-page-key"))
                    .ok(),
               "protected page-body verified with wrong key material") &&
       ok;
  return ok;
}

bool TestDatatypeDescriptorProfiles() {
  bool ok = true;
  const auto strong =
      dt::EncodeDatatypeDescriptorEnvelope(
          Envelope(dt::DatatypeDescriptorIntegrityProfile::strong));
  ok = Require(strong.ok(), "strong descriptor envelope did not encode") && ok;
  if (!strong.ok()) {
    return false;
  }
  ok = Require(strong.digest_algorithm == "sha256",
               "strong descriptor envelope did not use SHA-256") &&
       ok;
  ok = Require(strong.digest_bytes == hash::kSha256DigestBytes,
               "strong descriptor envelope did not use full digest bytes") &&
       ok;
  ok = Require(strong.digest_material.size() == hash::kSha256DigestBytes,
               "strong descriptor envelope did not expose full digest material") &&
       ok;
  ok = Require(dt::DecodeDatatypeDescriptorEnvelope(strong.encoded).ok(),
               "strong descriptor envelope did not decode") &&
       ok;

  auto corrupted_digest = strong.encoded;
  corrupted_digest[40] ^= 0x7fu;
  ok = Require(!dt::DecodeDatatypeDescriptorEnvelope(corrupted_digest).ok(),
               "descriptor envelope accepted tampered digest material") &&
       ok;

  const auto key = Bytes("eler-017-descriptor-key");
  const auto protected_envelope =
      dt::EncodeDatatypeDescriptorEnvelope(
          Envelope(dt::DatatypeDescriptorIntegrityProfile::protected_keyed),
          key);
  ok = Require(protected_envelope.ok(),
               "protected descriptor envelope did not encode") &&
       ok;
  if (!protected_envelope.ok()) {
    return false;
  }
  ok = Require(protected_envelope.digest_algorithm == "hmac-sha256",
               "protected descriptor envelope did not use HMAC-SHA-256") &&
       ok;
  ok = Require(!dt::DecodeDatatypeDescriptorEnvelope(
                    protected_envelope.encoded)
                    .ok(),
               "protected descriptor envelope decoded without key material") &&
       ok;
  ok = Require(dt::DecodeDatatypeDescriptorEnvelope(
                   protected_envelope.encoded,
                   key)
                   .ok(),
               "protected descriptor envelope did not decode with key material") &&
       ok;
  ok = Require(!dt::DecodeDatatypeDescriptorEnvelope(
                    protected_envelope.encoded,
                    Bytes("wrong-descriptor-key"))
                    .ok(),
               "protected descriptor envelope decoded with wrong key material") &&
       ok;
  return ok;
}

bool TestTransactionInventoryPageDigests() {
  bool ok = true;
  page::TransactionInventoryPageBody body;
  body.page_number = 17;
  body.previous_page_number = 16;
  body.next_page_number = 18;
  body.inventory_generation = 3;
  body.inventory = Inventory(&ok);
  if (!ok) {
    return false;
  }

  const auto built = page::BuildTransactionInventoryPageBody(body, kPageSize);
  ok = Require(built.ok(), "transaction inventory page did not build") && ok;
  ok = Require(page::TransactionInventoryPageDigestPresent(
                   built.body.chain_digest),
               "transaction inventory chain digest was empty") &&
       ok;
  ok = Require(built.body.chain_digest.size() ==
                   page::kTransactionInventoryPageDigestBytes,
               "transaction inventory chain digest was not full length") &&
       ok;
  const auto checksum_digest =
      page::ComputeTransactionInventoryPageChecksumDigest(built.serialized);
  ok = Require(page::TransactionInventoryPageDigestPresent(checksum_digest),
               "transaction inventory checksum digest was empty") &&
       ok;

  const auto parsed =
      page::ParseTransactionInventoryPageBody(built.serialized, body.page_number);
  ok = Require(parsed.ok(), "transaction inventory page did not parse") && ok;
  ok = Require(parsed.body.chain_digest == built.body.chain_digest,
               "transaction inventory chain digest did not round trip") &&
       ok;

  auto chain_corrupt = built.serialized;
  chain_corrupt[kTxnChainDigestOffset] ^= 0x11u;
  RewriteTransactionChecksum(&chain_corrupt);
  const auto chain_parse =
      page::ParseTransactionInventoryPageBody(chain_corrupt, body.page_number);
  ok = Require(!chain_parse.ok(),
               "transaction inventory accepted corrupted chain digest") &&
       ok;
  ok = Require(chain_parse.diagnostic.diagnostic_code ==
                   "SB-TXN-INVENTORY-PAGE-CHAIN-DIGEST-MISMATCH",
               "transaction inventory chain digest diagnostic changed") &&
       ok;

  auto weak_v1 = built.serialized;
  weak_v1[7] = static_cast<byte>('1');
  const auto weak_parse =
      page::ParseTransactionInventoryPageBody(weak_v1, body.page_number);
  ok = Require(!weak_parse.ok(),
               "transaction inventory accepted v1 weak digest page") &&
       ok;
  ok = Require(weak_parse.diagnostic.diagnostic_code ==
                   "SB-TXN-INVENTORY-PAGE-WEAK-DIGEST-REFUSED",
               "transaction inventory weak digest diagnostic changed") &&
       ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = TestCoreHashVectors() && ok;
  ok = TestPageBodyProfiles() && ok;
  ok = TestDatatypeDescriptorProfiles() && ok;
  ok = TestTransactionInventoryPageDigests() && ok;
  if (!ok) {
    return EXIT_FAILURE;
  }
  std::cout << "engine_listener_checksum_mac_profile_conformance=passed\n";
  return EXIT_SUCCESS;
}
