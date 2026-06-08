// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_btree_page.hpp"
#include "index_hash_page.hpp"
#include "index_specialized_pages.hpp"
#include "page_body_integrity.hpp"
#include "page_header.hpp"
#include "row_data_page.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

using platform::TypedUuid;
using platform::UuidKind;
using platform::byte;
using platform::u64;

inline constexpr u64 kBaseMillis = 1770000000000ull;
inline constexpr platform::u32 kPageSize = 8192;
inline constexpr platform::u32 kBodyCapacity =
    kPageSize - disk::kPageHeaderSerializedBytes;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

disk::SerializedPageHeader HeaderFor(disk::PageType page_type,
                                     u64 page_number,
                                     u64 uuid_seed) {
  disk::PageHeader header;
  header.page_size = kPageSize;
  header.page_type = page_type;
  header.database_uuid = MakeUuid(UuidKind::database, 1).value;
  header.filespace_uuid = MakeUuid(UuidKind::filespace, 2).value;
  header.page_uuid = MakeUuid(UuidKind::page, uuid_seed).value;
  header.page_number = page_number;
  header.page_generation = 7;
  const auto serialized = disk::SerializePageHeader(header);
  return serialized.ok() ? serialized.serialized : disk::SerializedPageHeader{};
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

page::IndexBtreePageBody BtreeBody() {
  page::IndexBtreePageBody body;
  body.index_uuid = MakeUuid(UuidKind::object, 20);
  body.page_number = 21;
  body.page_kind = page::IndexBtreePageKind::root;
  body.tree_level = 0;
  return body;
}

page::IndexHashPageBody HashBody() {
  page::IndexHashPageBody body;
  body.index_uuid = MakeUuid(UuidKind::object, 30);
  body.page_number = 31;
  body.page_kind = page::IndexHashPageKind::directory;
  body.hash_seed = 0x1234567812345678ull;
  body.hash_seed_high64 = 0x8765432187654321ull;
  body.hash_algorithm_version = page::kIndexHashProductionDefaultAlgorithmVersion;
  body.bucket_count = 1;
  body.directory_bucket_page_numbers.push_back(32);
  return body;
}

page::IndexSpecializedPageBody SpecializedBody(disk::PageType page_type,
                                               page::IndexPageFamilyKind family) {
  page::IndexSpecializedPageBody body;
  body.header.index_object_uuid = MakeUuid(UuidKind::object, 40);
  body.header.family_uuid = MakeUuid(UuidKind::object, 41);
  body.header.resource_epoch = 2;
  body.header.mutation_epoch = 3;
  body.header.logical_page_number = 41;
  body.header.layout_version = 1;
  body.header.family = family;
  body.header.page_type = page_type;
  return body;
}

page::PageBodyAgreementResult Agreement(disk::PageType page_type,
                                        u64 page_number,
                                        u64 uuid_seed,
                                        const std::vector<byte>& body,
                                        page::PageBodyChecksumProfile profile =
                                            page::PageBodyChecksumProfile::fast,
                                        std::vector<byte> key = {}) {
  page::PageBodyAgreementRequest request;
  request.header = HeaderFor(page_type, page_number, uuid_seed);
  request.body = body;
  request.checksum_profile = profile;
  request.protected_key_material = std::move(key);
  return page::ValidatePageBodyAgreement(request);
}

bool ChecksumProfiles() {
  bool ok = true;
  const auto built = page::BuildRowDataPageBody(RowBody(), kPageSize);
  ok = Expect(built.ok(), "row body should build for checksum profiles") && ok;
  if (!built.ok()) {
    return false;
  }

  const auto fast = page::ComputePageBodyChecksumDigest(
      page::PageBodyChecksumProfile::fast,
      built.serialized);
  ok = Expect(fast.ok(), "fast checksum profile should compute") && ok;
  ok = Expect(fast.digest.digest_bytes == 8,
              "fast checksum profile should be 64-bit") && ok;
  ok = Expect(page::VerifyPageBodyChecksumDigest(
                  page::PageBodyChecksumProfile::fast,
                  built.serialized,
                  fast.digest.low64,
                  fast.digest.high64).matched,
              "fast checksum profile should verify exact digest") && ok;
  ok = Expect(!page::VerifyPageBodyChecksumDigest(
                   page::PageBodyChecksumProfile::fast,
                   built.serialized,
                   fast.digest.low64 ^ 1u,
                   fast.digest.high64).ok(),
              "fast checksum mismatch should fail closed") && ok;

  const auto strong = page::ComputePageBodyChecksumDigest(
      page::PageBodyChecksumProfile::strong,
      built.serialized);
  ok = Expect(strong.ok(), "strong checksum profile should compute") && ok;
  ok = Expect(strong.digest.digest_algorithm == "sha256",
              "strong checksum profile should use SHA-256") && ok;
  ok = Expect(strong.digest.digest_bytes == 32,
              "strong checksum profile should be 256-bit evidence") && ok;
  ok = Expect(strong.digest.digest_material.size() == 32,
              "strong checksum profile should expose full digest material") && ok;
  ok = Expect(strong.digest.low64 != 0 && strong.digest.high64 != 0,
              "strong checksum profile should populate both lanes") && ok;
  ok = Expect(page::VerifyPageBodyChecksumDigestMaterial(
                  page::PageBodyChecksumProfile::strong,
                  built.serialized,
                  strong.digest.digest_material).matched,
              "strong checksum profile should verify full digest material") && ok;

  const auto protected_missing = page::ComputePageBodyChecksumDigest(
      page::PageBodyChecksumProfile::protected_keyed,
      built.serialized);
  ok = Expect(!protected_missing.ok(),
              "protected checksum profile should require key material") && ok;
  ok = Expect(protected_missing.diagnostic.diagnostic_code ==
                  "SB-PAGE-CHECKSUM-PROTECTED-MATERIAL-REQUIRED",
              "protected checksum missing-key diagnostic should be stable") && ok;

  const std::vector<byte> key = {'p', 'c', 'r', '0', '3', '4'};
  const auto protected_digest = page::ComputePageBodyChecksumDigest(
      page::PageBodyChecksumProfile::protected_keyed,
      built.serialized,
      key);
  ok = Expect(protected_digest.ok(),
              "protected checksum profile should compute with key material") && ok;
  ok = Expect(protected_digest.digest.protected_material_required,
              "protected checksum profile should mark key requirement") && ok;
  ok = Expect(protected_digest.digest.protected_material_supplied,
              "protected checksum profile should mark supplied key material") && ok;
  ok = Expect(protected_digest.digest.digest_algorithm == "hmac-sha256",
              "protected checksum profile should use HMAC-SHA-256") && ok;
  ok = Expect(protected_digest.digest.digest_bytes == 32,
              "protected checksum profile should produce 256-bit evidence") && ok;
  ok = Expect(protected_digest.digest.digest_material.size() == 32,
              "protected checksum profile should expose full digest material") && ok;
  return ok;
}

bool BodyAgreementProfiles() {
  bool ok = true;

  const auto row = page::BuildRowDataPageBody(RowBody(), kPageSize);
  ok = Expect(row.ok(), "row body should build") && ok;
  if (!row.ok()) {
    return false;
  }
  auto row_agreement = Agreement(disk::PageType::row_data,
                                 RowBody().page_number,
                                 100,
                                 row.serialized,
                                 page::PageBodyChecksumProfile::strong);
  ok = Expect(row_agreement.ok(), "row body should agree with row_data header") && ok;
  ok = Expect(row_agreement.body_kind == page::PageBodyKind::row_data,
              "row agreement should identify row body kind") && ok;
  ok = Expect(row_agreement.production_admitted,
              "row body should remain production-admitted") && ok;

  const auto btree = page::BuildIndexBtreePageBody(BtreeBody(), kPageSize);
  ok = Expect(btree.ok(), "B-tree body should build") && ok;
  if (!btree.ok()) {
    return false;
  }
  auto btree_agreement = Agreement(disk::PageType::index_btree,
                                   BtreeBody().page_number,
                                   200,
                                   btree.serialized);
  ok = Expect(btree_agreement.ok(),
              "B-tree body should agree with generic B-tree header") && ok;
  ok = Expect(btree_agreement.index_family == page::IndexPageFamilyKind::btree,
              "B-tree agreement should record B-tree family") && ok;
  ok = Expect(btree_agreement.production_admitted,
              "generic B-tree body should remain production-admitted") && ok;

  auto btree_kind_mismatch = Agreement(disk::PageType::index_btree_leaf,
                                       BtreeBody().page_number,
                                       201,
                                       btree.serialized);
  ok = Expect(!btree_kind_mismatch.ok(),
              "B-tree root body should not agree with leaf header") && ok;
  ok = Expect(btree_kind_mismatch.kind ==
                  page::PageBodyAgreementKind::page_kind_mismatch,
              "B-tree root/leaf mismatch should be page-kind mismatch") && ok;

  auto family_mismatch = Agreement(disk::PageType::index_hash,
                                   BtreeBody().page_number,
                                   202,
                                   btree.serialized);
  ok = Expect(!family_mismatch.ok(),
              "B-tree body should not agree with hash header") && ok;
  ok = Expect(family_mismatch.kind ==
                  page::PageBodyAgreementKind::body_family_mismatch,
              "B-tree/hash mismatch should be family mismatch") && ok;

  const auto hash = page::BuildIndexHashPageBody(HashBody(), kPageSize);
  ok = Expect(hash.ok(), "hash body should build") && ok;
  if (!hash.ok()) {
    return false;
  }
  auto hash_agreement = Agreement(disk::PageType::index_hash,
                                  HashBody().page_number,
                                  300,
                                  hash.serialized);
  ok = Expect(hash_agreement.ok(),
              "hash body should agree with hash header") && ok;
  ok = Expect(hash_agreement.index_family == page::IndexPageFamilyKind::hash,
              "hash agreement should record hash family") && ok;
  ok = Expect(hash_agreement.production_admitted,
              "hash agreement should remain production-admitted") && ok;

  const auto specialized = page::BuildIndexSpecializedPageBody(
      SpecializedBody(disk::PageType::index_vector,
                      page::IndexPageFamilyKind::vector),
      kBodyCapacity);
  ok = Expect(specialized.ok(), "specialized index body should build") && ok;
  if (!specialized.ok()) {
    return false;
  }
  auto specialized_agreement = Agreement(disk::PageType::index_vector,
                                         41,
                                         400,
                                         specialized.serialized);
  ok = Expect(specialized_agreement.ok(),
              "specialized vector body should agree with vector header") && ok;
  ok = Expect(specialized_agreement.body_kind ==
                  page::PageBodyKind::index_specialized,
              "specialized agreement should identify specialized body kind") && ok;
  ok = Expect(specialized_agreement.index_family ==
                  page::IndexPageFamilyKind::vector,
              "specialized agreement should record vector family") && ok;
  ok = Expect(specialized_agreement.production_admitted,
              "specialized agreement should remain production-admitted") && ok;

  const auto specialized_wrong = page::BuildIndexSpecializedPageBody(
      SpecializedBody(disk::PageType::index_bitmap,
                      page::IndexPageFamilyKind::vector),
      kBodyCapacity);
  ok = Expect(specialized_wrong.ok(),
              "mismatched specialized fixture should still serialize") && ok;
  if (!specialized_wrong.ok()) {
    return false;
  }
  auto specialized_refused = Agreement(disk::PageType::index_bitmap,
                                       41,
                                       401,
                                       specialized_wrong.serialized);
  ok = Expect(!specialized_refused.ok(),
              "specialized body family mismatch should fail closed") && ok;
  ok = Expect(specialized_refused.kind ==
                  page::PageBodyAgreementKind::body_family_mismatch,
              "specialized mismatch should be family mismatch") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = ChecksumProfiles() && ok;
  ok = BodyAgreementProfiles() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
