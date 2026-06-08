// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_hash_page.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

namespace {

inline constexpr platform::u32 kOffsetPageKind = 12;
inline constexpr platform::u32 kOffsetBodyChecksum = 24;
inline constexpr platform::u32 kOffsetIndexUuid = 32;
inline constexpr platform::u32 kOffsetOwningBucketPageNumber = 80;
inline constexpr platform::u32 kHeaderBytes = page::kIndexHashPageBodyHeaderBytes;
inline constexpr platform::u32 kCollisionRootBytes = 24;
inline constexpr platform::u32 kEntryOffsetNextOrdinal = 2;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "index_hash_physical_page_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence, std::string_view needle) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const std::string& value) {
                       return value == needle || value.find(needle) != std::string::npos;
                     });
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 millis,
                                  platform::byte suffix) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(kind, generated.value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

std::vector<platform::byte> Key(std::string_view text) {
  return {text.begin(), text.end()};
}

void Rechecksum(std::vector<platform::byte>* bytes) {
  platform::StoreLittle64(bytes->data() + kOffsetBodyChecksum, 0);
  platform::StoreLittle64(bytes->data() + kOffsetBodyChecksum,
                          page::ComputeIndexHashPageChecksum(*bytes));
}

page::IndexHashPhysicalIndex NewIndex(
    platform::u32 bucket_count = 2,
    platform::u16 algorithm_version =
        page::kIndexHashProductionDefaultAlgorithmVersion,
    platform::u64 hash_seed = 0,
    bool test_fixture_seed_allowed = false,
    bool test_fixture_force_route_hash_collision = false,
    bool test_fixture_force_fingerprint_collision = false) {
  auto initialized = page::InitializeIndexHashPhysicalIndex(
      GeneratedUuid(platform::UuidKind::object, 1700000000000ull, 0x10),
      512,
      hash_seed,
      algorithm_version,
      bucket_count,
      test_fixture_seed_allowed,
      test_fixture_force_route_hash_collision,
      test_fixture_force_fingerprint_collision);
  Require(initialized.ok(), "hash index initialize failed");
  return initialized.index;
}

page::IndexHashPhysicalInsertResult Insert(page::IndexHashPhysicalIndex* index,
                                           const std::vector<platform::byte>& key,
                                           platform::byte row_suffix,
                                           platform::byte version_suffix) {
  auto route = page::LocateIndexHashBucket(*index, key);
  Require(route.ok(), "bucket route failed");
  auto latch = page::AcquireIndexHashBucketExclusiveLatch(index, route.bucket_page_number);
  Require(latch.active(), "exclusive latch acquisition failed");
  page::IndexHashPhysicalInsertRequest request;
  request.encoded_key = key;
  request.row_uuid = GeneratedUuid(platform::UuidKind::row,
                                   1700000100000ull + row_suffix,
                                   row_suffix);
  request.version_uuid = GeneratedUuid(platform::UuidKind::row,
                                       1700000200000ull + version_suffix,
                                       version_suffix);
  request.latch_evidence = latch.evidence();
  auto inserted = page::InsertIndexHashEntry(index, request);
  if (!inserted.ok()) {
    std::cerr << "insert diagnostic=" << inserted.diagnostic.diagnostic_code << '\n';
  }
  Require(inserted.ok() && inserted.inserted, "insert failed");
  return inserted;
}

page::IndexHashPhysicalProbeResult Probe(const page::IndexHashPhysicalIndex& index,
                                         const std::vector<platform::byte>& key) {
  auto route = page::LocateIndexHashBucket(index, key);
  Require(route.ok(), "probe route failed");
  auto latch = page::AcquireIndexHashBucketSharedLatch(index, route.bucket_page_number);
  Require(latch.active(), "shared latch acquisition failed");
  page::IndexHashPhysicalProbeRequest request;
  request.encoded_key = key;
  request.latch_evidence = latch.evidence();
  auto probed = page::ProbeIndexHashBucket(index, request);
  if (!probed.ok()) {
    std::cerr << "probe diagnostic=" << probed.diagnostic.diagnostic_code << '\n';
  }
  Require(probed.ok(), "probe failed");
  return probed;
}

void HashDomainUsesFull64Bits() {
  std::unordered_map<platform::u64, std::vector<platform::byte>> seen;
  bool high_bits_observed = false;
  for (int i = 0; i < 8192; ++i) {
    auto key = Key("full-width-hash-key-" + std::to_string(i));
    const auto hash =
        page::ComputeIndexHashKeyHash(0x123456789abcdef0ull,
                                      page::kIndexHashProductionDefaultAlgorithmVersion,
                                      key);
    high_bits_observed = high_bits_observed || ((hash >> 16u) != 0);
    const auto inserted = seen.emplace(hash, key);
    Require(inserted.second || inserted.first->second == key,
            "unexpected full-width hash collision in deterministic sample");
  }
  Require(high_bits_observed, "hash key domain was truncated to low 16 bits");
}

void AlgorithmVersionsAndFingerprints() {
  auto refused_seed = page::InitializeIndexHashPhysicalIndex(
      GeneratedUuid(platform::UuidKind::object, 1700000000200ull, 0x12),
      512,
      0x123456789abcdef0ull,
      page::kIndexHashProductionDefaultAlgorithmVersion,
      2);
  Require(!refused_seed.ok() &&
              refused_seed.diagnostic.diagnostic_code ==
                  "SB-INDEX-HASH-SEED-CLIENT-SUPPLIED",
          "deterministic seed was accepted without test-fixture mode");

  auto allowed_seed = page::InitializeIndexHashPhysicalIndex(
      GeneratedUuid(platform::UuidKind::object, 1700000000300ull, 0x13),
      512,
      0x123456789abcdef0ull,
      page::kIndexHashProductionDefaultAlgorithmVersion,
      2,
      true);
  Require(allowed_seed.ok(), "explicit test-fixture deterministic seed refused");
  Require(HasEvidence(allowed_seed.evidence, "hash_seed_test_fixture_mode=true"),
          "test-fixture seed evidence missing");
  Require(HasEvidence(allowed_seed.evidence, "hash_seed_engine_generated=false"),
          "test-fixture seed was reported as engine-generated");
  Require(HasEvidence(allowed_seed.evidence, "hash_seed_protected=false"),
          "test-fixture seed was reported as protected");
  Require(HasEvidence(allowed_seed.evidence,
                      "hash_seed_entropy_source=deterministic_test_fixture"),
          "test-fixture seed source evidence missing");

  const auto production_uuid =
      GeneratedUuid(platform::UuidKind::object, 1700000000400ull, 0x14);
  auto production_seed_a = page::InitializeIndexHashPhysicalIndex(
      production_uuid,
      512,
      0,
      page::kIndexHashProductionDefaultAlgorithmVersion,
      2);
  auto production_seed_b = page::InitializeIndexHashPhysicalIndex(
      production_uuid,
      512,
      0,
      page::kIndexHashProductionDefaultAlgorithmVersion,
      2);
  Require(production_seed_a.ok() && production_seed_b.ok(),
          "production protected seed generation failed");
  Require(production_seed_a.index.hash_seed != 0 &&
              production_seed_b.index.hash_seed != 0,
          "production seed was zero");
  Require(production_seed_a.index.hash_seed_high64 != 0 &&
              production_seed_b.index.hash_seed_high64 != 0,
          "production high seed was zero");
  Require(production_seed_a.index.hash_seed != production_seed_b.index.hash_seed,
          "production seed was deterministic for same index UUID");
  Require(production_seed_a.index.hash_seed_high64 !=
              production_seed_b.index.hash_seed_high64,
          "production high seed was deterministic for same index UUID");
  Require(HasEvidence(production_seed_a.evidence,
                      "hash_seed_engine_generated=true"),
          "production seed was not reported as engine-generated");
  Require(HasEvidence(production_seed_a.evidence, "hash_seed_protected=true"),
          "production seed was not reported as protected");
  Require(HasEvidence(production_seed_a.evidence, "hash_seed_entropy_source="),
          "production seed entropy source evidence missing");
  Require(HasEvidence(production_seed_a.evidence, "hash_seed_key_material_bits=128"),
          "production seed key material width evidence missing");

  auto legacy = NewIndex(2, page::kIndexHashAlgorithmVersion1LegacyFnv64);
  Insert(&legacy, Key("legacy-key"), 0x24, 0x25);
  auto legacy_report = page::BuildIndexHashPhysicalReport(legacy);
  Require(legacy_report.report.valid, "legacy report invalid");
  Require(legacy_report.report.legacy_compatible,
          "legacy v1 compatibility was not reported");
  Require(legacy_report.report.rebuild_recommended,
          "legacy v1 did not recommend rebuild");
  Require(HasEvidence(legacy_report.report.evidence,
                      "legacy_v1_rebuild_recommended=true"),
          "legacy rebuild evidence missing");

  auto keyed = NewIndex(2);
  Insert(&keyed, Key("keyed-hash-key"), 0x26, 0x27);
  auto keyed_report = page::BuildIndexHashPhysicalReport(keyed);
  Require(keyed_report.report.valid, "keyed report invalid");
  Require(keyed_report.report.hash_algorithm_version ==
              page::kIndexHashProductionDefaultAlgorithmVersion,
          "production default hash algorithm drifted");
  Require(!keyed_report.report.high_assurance_fingerprint_present,
          "v2 unexpectedly reported high-assurance fingerprints");
  Require(HasEvidence(keyed_report.report.evidence,
                      "hash_algorithm_version=v2_keyed_hash64"),
          "v2 hash algorithm evidence missing");

  auto high = NewIndex(2, page::kIndexHashHighAssuranceAlgorithmVersion);
  Insert(&high, Key("high-assurance-key"), 0x28, 0x29);
  auto high_probe = Probe(high, Key("high-assurance-key"));
  Require(high_probe.fingerprint_present,
          "v3 probe did not publish fingerprint metadata");
  Require(high_probe.encoded_key_compare_count == 1,
          "v3 probe did not keep full encoded-key comparison mandatory");
  auto high_report = page::BuildIndexHashPhysicalReport(high);
  Require(high_report.report.valid, "high-assurance report invalid");
  Require(high_report.report.high_assurance_fingerprint_present,
          "v3 report did not record fingerprint support");
  Require(HasEvidence(high_report.report.evidence,
                      "hash_algorithm_version=v3_keyed_hash128_fingerprint"),
          "v3 hash algorithm evidence missing");

  auto exported = page::ExportIndexHashPhysicalIndexImage(high);
  Require(exported.ok(), "high-assurance export failed");
  auto imported = page::ImportIndexHashPhysicalIndexImage(exported.image);
  Require(imported.ok(), "high-assurance import failed");
  auto imported_probe = Probe(imported.index, Key("high-assurance-key"));
  Require(imported_probe.fingerprint_present &&
              imported_probe.locators.size() == 1,
          "high-assurance fingerprint did not survive import/export");

  auto forced_route = NewIndex(2,
                               page::kIndexHashHighAssuranceAlgorithmVersion,
                               0,
                               true,
                               true,
                               false);
  Insert(&forced_route, Key("forced-route-a"), 0x2a, 0x2b);
  Insert(&forced_route, Key("forced-route-b"), 0x2c, 0x2d);
  auto forced_route_probe = Probe(forced_route, Key("forced-route-b"));
  Require(forced_route_probe.locators.size() == 1,
          "forced route collision returned wrong locator count");
  Require(forced_route_probe.collision_entries_traversed >= 2,
          "forced route collision chain was not traversed");
  Require(forced_route_probe.fingerprint_mismatch_count >= 1,
          "v3 fingerprint did not short-circuit forced route collision");
  Require(forced_route_probe.encoded_key_compare_count == 1,
          "v3 fingerprint did not reduce encoded-key compares");

  auto forced_fingerprint = NewIndex(2,
                                     page::kIndexHashHighAssuranceAlgorithmVersion,
                                     0,
                                     true,
                                     true,
                                     true);
  Insert(&forced_fingerprint, Key("forced-fingerprint-a"), 0x2e, 0x2f);
  Insert(&forced_fingerprint, Key("forced-fingerprint-b"), 0x34, 0x35);
  auto forced_fingerprint_probe =
      Probe(forced_fingerprint, Key("forced-fingerprint-b"));
  Require(forced_fingerprint_probe.locators.size() == 1,
          "forced fingerprint collision returned wrong locator count");
  Require(forced_fingerprint_probe.encoded_key_compare_count >= 2,
          "fingerprint collision did not fall through to full encoded-key compare");
}

void DirectoryBucketRoundTripAndSeedVersion() {
  auto index = NewIndex();
  Insert(&index, Key("round-trip-a"), 0x20, 0x21);
  Insert(&index, Key("round-trip-b"), 0x22, 0x23);

  auto exported = page::ExportIndexHashPhysicalIndexImage(index);
  Require(exported.ok(), "export failed");
  auto imported = page::ImportIndexHashPhysicalIndexImage(exported.image);
  Require(imported.ok(), "import failed");
  auto report = page::BuildIndexHashPhysicalReport(imported.index);
  Require(report.ok() && report.report.valid, "round-trip report invalid");
  Require(report.report.bucket_page_count == 2, "bucket count not preserved");
  Require(report.report.live_entry_count == 2, "live entries not preserved");
  Require(imported.index.hash_seed_high64 == index.hash_seed_high64,
          "high seed was not preserved by export/import");
  Require(report.report.hash_seed_high64 == index.hash_seed_high64,
          "high seed was not reported after import");

  auto mismatched_seed = imported.index;
  mismatched_seed.hash_seed ^= 0x55u;
  auto refused_seed = page::FetchIndexHashPhysicalPage(mismatched_seed,
                                                       mismatched_seed.directory_page_number);
  Require(!refused_seed.ok() &&
              refused_seed.diagnostic.diagnostic_code == "SB-INDEX-HASH-SEED-MISMATCH",
          "seed mismatch was not refused");

  auto mismatched_seed_high = imported.index;
  mismatched_seed_high.hash_seed_high64 ^= 0x55u;
  auto refused_seed_high =
      page::FetchIndexHashPhysicalPage(mismatched_seed_high,
                                       mismatched_seed_high.directory_page_number);
  Require(!refused_seed_high.ok() &&
              refused_seed_high.diagnostic.diagnostic_code ==
                  "SB-INDEX-HASH-SEED-MISMATCH",
          "high seed mismatch was not refused");

  auto mismatched_version = exported.image;
  mismatched_version.hash_algorithm_version = 99;
  auto refused_version = page::ImportIndexHashPhysicalIndexImage(mismatched_version);
  Require(!refused_version.ok(), "version mismatch import was not refused");

  auto mismatched_uuid = exported.image;
  mismatched_uuid.index_uuid =
      GeneratedUuid(platform::UuidKind::object, 1700000001000ull, 0x11);
  auto refused_uuid = page::ImportIndexHashPhysicalIndexImage(mismatched_uuid);
  Require(!refused_uuid.ok(), "index UUID mismatch import was not refused");
}

void CollisionOverflowTombstoneAndEvidence() {
  auto index = NewIndex(1);
  const auto collision_key = Key("collision-shared-key");
  auto first = Insert(&index, collision_key, 0x30, 0x31);
  auto second = Insert(&index, collision_key, 0x32, 0x33);
  Require(first.key_hash == second.key_hash, "test collision did not collide");

  auto collision_probe = Probe(index, collision_key);
  Require(collision_probe.locators.size() == 2,
          "same-key collision chain probe mismatch");
  Require(collision_probe.collision_entries_traversed >= 2,
          "collision chain was not traversed");

  const auto overflow_key = Key("overflow-fixed-key");
  bool overflow_created = false;
  for (int i = 0; i < 12; ++i) {
    auto inserted = Insert(&index, overflow_key,
                           static_cast<platform::byte>(0x40 + i),
                           static_cast<platform::byte>(0x60 + i));
    overflow_created = overflow_created || inserted.overflow_page_created;
  }
  Require(overflow_created, "overflow page was not created");
  auto overflow_probe = Probe(index, overflow_key);
  Require(overflow_probe.locators.size() >= 12, "overflow probe failed");
  Require(overflow_probe.pages_traversed > 1, "overflow chain was not traversed");

  const auto delete_key = Key("delete-me");
  Insert(&index, delete_key, 0x90, 0x91);
  auto delete_probe = Probe(index, delete_key);
  Require(delete_probe.locators.size() == 1, "delete setup probe failed");
  auto delete_route = page::LocateIndexHashBucket(index, delete_key);
  auto delete_latch =
      page::AcquireIndexHashBucketExclusiveLatch(&index, delete_route.bucket_page_number);
  page::IndexHashPhysicalDeleteRequest delete_request;
  delete_request.encoded_key = delete_key;
  delete_request.row_uuid = delete_probe.locators.front().row_uuid;
  delete_request.version_uuid = delete_probe.locators.front().version_uuid;
  delete_request.latch_evidence = delete_latch.evidence();
  auto deleted = page::DeleteIndexHashEntry(&index, delete_request);
  Require(deleted.ok() && deleted.tombstone_marked, "delete tombstone failed");
  delete_latch = {};

  auto after_delete = Probe(index, delete_key);
  Require(after_delete.locators.empty(), "deleted locator was not excluded");
  auto tombstone_report = page::BuildIndexHashPhysicalReport(index);
  Require(tombstone_report.report.valid, "tombstone report invalid");
  Require(tombstone_report.report.tombstone_entry_count >= 1,
          "tombstone not validation visible");

  Require(HasEvidence(after_delete.evidence, "candidate_set_only=true"),
          "candidate evidence missing");
  Require(HasEvidence(after_delete.evidence, "materialized_final_rows=false"),
          "materialized final rows evidence missing");
  Require(HasEvidence(after_delete.evidence, "parser_finality_authority=false"),
          "parser authority evidence missing");
  Require(HasEvidence(after_delete.evidence, "provider_finality_authority=false"),
          "provider authority evidence missing");
  Require(HasEvidence(after_delete.evidence, "wal_finality_authority=false"),
          "wal authority evidence missing");
  Require(HasEvidence(after_delete.evidence, "mga_recheck_required=true"),
          "mga recheck evidence missing");
  Require(HasEvidence(after_delete.evidence, "security_recheck_required=true"),
          "security recheck evidence missing");
}

void MissingLatchProofFailsClosed() {
  auto index = NewIndex();
  const auto key = Key("missing-latch");
  auto route = page::LocateIndexHashBucket(index, key);
  Require(route.ok(), "route failed");

  page::IndexHashPhysicalInsertRequest insert_request;
  insert_request.encoded_key = key;
  insert_request.row_uuid = GeneratedUuid(platform::UuidKind::row, 1700000300000ull, 0x70);
  insert_request.version_uuid = GeneratedUuid(platform::UuidKind::row, 1700000300100ull, 0x71);
  auto insert_refused = page::InsertIndexHashEntry(&index, insert_request);
  Require(!insert_refused.ok() &&
              insert_refused.diagnostic.diagnostic_code == "SB-INDEX-HASH-LATCH-PROOF-MISSING",
          "insert without latch did not fail closed");

  page::IndexHashPhysicalProbeRequest probe_request;
  probe_request.encoded_key = key;
  auto probe_refused = page::ProbeIndexHashBucket(index, probe_request);
  Require(!probe_refused.ok() &&
              probe_refused.diagnostic.diagnostic_code == "SB-INDEX-HASH-LATCH-PROOF-MISSING",
          "probe without latch did not fail closed");

  page::IndexHashPhysicalDeleteRequest delete_request;
  delete_request.encoded_key = key;
  delete_request.row_uuid = insert_request.row_uuid;
  delete_request.version_uuid = insert_request.version_uuid;
  auto delete_refused = page::DeleteIndexHashEntry(&index, delete_request);
  Require(!delete_refused.ok() &&
              delete_refused.diagnostic.diagnostic_code == "SB-INDEX-HASH-LATCH-PROOF-MISSING",
          "delete without latch did not fail closed");
}

void ValidationCatchesCorruption() {
  auto index = NewIndex(1);
  const auto collision_key = Key("validation-collision-shared-key");
  Insert(&index, collision_key, 0x80, 0x81);
  Insert(&index, collision_key, 0x82, 0x83);

  auto checksum_corrupt = index;
  checksum_corrupt.pages.front().serialized.back() ^= 0x7u;
  auto checksum_report = page::BuildIndexHashPhysicalReport(checksum_corrupt);
  Require(!checksum_report.report.valid, "checksum corruption not caught");
  Require(checksum_report.report.corruption_class ==
              page::IndexHashPhysicalCorruptionClass::checksum,
          "checksum corruption class mismatch");

  auto kind_corrupt = index;
  platform::StoreLittle16(kind_corrupt.pages.front().serialized.data() + kOffsetPageKind,
                          static_cast<platform::u16>(0x7777u));
  Rechecksum(&kind_corrupt.pages.front().serialized);
  auto kind_report = page::BuildIndexHashPhysicalReport(kind_corrupt);
  Require(!kind_report.report.valid, "page kind corruption not caught");
  Require(kind_report.report.corruption_class ==
              page::IndexHashPhysicalCorruptionClass::page_kind ||
              kind_report.report.corruption_class ==
                  page::IndexHashPhysicalCorruptionClass::directory,
          "page kind corruption class mismatch");

  auto chain_corrupt = index;
  auto route = page::LocateIndexHashBucket(chain_corrupt, collision_key);
  const auto page_index = std::find_if(
      chain_corrupt.pages.begin(),
      chain_corrupt.pages.end(),
      [&](const page::IndexHashPhysicalPageImage& image) {
        return image.page_number == route.bucket_page_number;
      });
  Require(page_index != chain_corrupt.pages.end(), "bucket image missing");
  const platform::u32 first_entry_offset = kHeaderBytes + kCollisionRootBytes;
  platform::StoreLittle16(page_index->serialized.data() +
                              first_entry_offset +
                              kEntryOffsetNextOrdinal,
                          0x1234u);
  Rechecksum(&page_index->serialized);
  auto chain_report = page::BuildIndexHashPhysicalReport(chain_corrupt);
  Require(!chain_report.report.valid, "collision chain corruption not caught");
  Require(chain_report.report.corruption_class ==
              page::IndexHashPhysicalCorruptionClass::collision_chain,
          "collision chain corruption class mismatch");

  auto directory_corrupt = NewIndex(2);
  platform::StoreLittle64(directory_corrupt.pages.front().serialized.data() +
                              kHeaderBytes + sizeof(platform::u64),
                          platform::LoadLittle64(
                              directory_corrupt.pages.front().serialized.data() +
                              kHeaderBytes));
  Rechecksum(&directory_corrupt.pages.front().serialized);
  auto directory_report = page::BuildIndexHashPhysicalReport(directory_corrupt);
  Require(!directory_report.report.valid, "duplicate directory bucket not caught");
  Require(directory_report.report.corruption_class ==
              page::IndexHashPhysicalCorruptionClass::directory,
          "duplicate directory bucket corruption class mismatch");

  auto overflow_owner_corrupt = NewIndex(1);
  bool overflow_created = false;
  for (int i = 0; i < 12; ++i) {
    auto inserted = Insert(&overflow_owner_corrupt, Key("owner-overflow-key"),
                           static_cast<platform::byte>(0xa0 + i),
                           static_cast<platform::byte>(0xb0 + i));
    overflow_created = overflow_created || inserted.overflow_page_created;
  }
  Require(overflow_created, "overflow owner corruption setup failed");
  auto overflow_page = std::find_if(
      overflow_owner_corrupt.pages.begin(),
      overflow_owner_corrupt.pages.end(),
      [](const page::IndexHashPhysicalPageImage& image) {
        return platform::LoadLittle16(image.serialized.data() + kOffsetPageKind) ==
               static_cast<platform::u16>(page::IndexHashPageKind::overflow);
      });
  Require(overflow_page != overflow_owner_corrupt.pages.end(),
          "overflow page image missing");
  platform::StoreLittle64(overflow_page->serialized.data() +
                              kOffsetOwningBucketPageNumber,
                          9999);
  Rechecksum(&overflow_page->serialized);
  auto overflow_report =
      page::BuildIndexHashPhysicalReport(overflow_owner_corrupt);
  Require(!overflow_report.report.valid, "overflow owner corruption not caught");
  Require(overflow_report.report.corruption_class ==
              page::IndexHashPhysicalCorruptionClass::overflow_chain,
          "overflow owner corruption class mismatch");
}

}  // namespace

int main() {
  HashDomainUsesFull64Bits();
  AlgorithmVersionsAndFingerprints();
  DirectoryBucketRoundTripAndSeedVersion();
  CollisionOverflowTombstoneAndEvidence();
  MissingLatchProofFailsClosed();
  ValidationCatchesCorruption();
  return EXIT_SUCCESS;
}
