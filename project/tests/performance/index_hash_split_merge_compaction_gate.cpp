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
#include <vector>

namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

namespace {

inline constexpr platform::u32 kOffsetPageKind = 12;
inline constexpr platform::u32 kOffsetBodyChecksum = 24;
inline constexpr platform::u32 kOffsetPageNumber = 48;
inline constexpr platform::u32 kOffsetOwningBucketPageNumber = 80;
inline constexpr platform::u32 kHeaderBytes = 96;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "index_hash_split_merge_compaction_gate: " << message << '\n';
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

page::IndexHashPhysicalIndex NewIndex(platform::u32 bucket_count,
                                      platform::u32 page_size = 1024) {
  auto initialized = page::InitializeIndexHashPhysicalIndex(
      GeneratedUuid(platform::UuidKind::object, 1700001000000ull, 0x10),
      page_size,
      0,
      page::kIndexHashProductionDefaultAlgorithmVersion,
      bucket_count);
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
  Require(latch.active(), "exclusive insert latch acquisition failed");
  page::IndexHashPhysicalInsertRequest request;
  request.encoded_key = key;
  request.row_uuid = GeneratedUuid(platform::UuidKind::row,
                                   1700001100000ull + row_suffix,
                                   row_suffix);
  request.version_uuid = GeneratedUuid(platform::UuidKind::row,
                                       1700001200000ull + version_suffix,
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
  Require(latch.active(), "shared probe latch acquisition failed");
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

std::vector<page::IndexHashBucketLatchGuard> AcquireAllExclusive(
    page::IndexHashPhysicalIndex* index,
    std::vector<page::IndexHashBucketLatchEvidence>* evidence) {
  auto directory = page::FetchIndexHashPhysicalPage(*index, index->directory_page_number);
  Require(directory.ok(), "directory fetch failed");
  std::vector<page::IndexHashBucketLatchGuard> guards;
  for (platform::u64 bucket_page_number :
       directory.body.directory_bucket_page_numbers) {
    auto guard = page::AcquireIndexHashBucketExclusiveLatch(index, bucket_page_number);
    Require(guard.active(), "exclusive maintenance latch acquisition failed");
    evidence->push_back(guard.evidence());
    guards.push_back(std::move(guard));
  }
  return guards;
}

page::IndexHashPhysicalMaintenanceResult Maintain(page::IndexHashPhysicalIndex* index) {
  std::vector<page::IndexHashBucketLatchEvidence> evidence;
  auto guards = AcquireAllExclusive(index, &evidence);
  page::IndexHashPhysicalMaintenanceRequest request;
  request.exclusive_bucket_latches = evidence;
  auto maintained = page::MaintainIndexHashPhysicalStructure(index, request);
  if (!maintained.ok()) {
    std::cerr << "maintenance diagnostic="
              << maintained.diagnostic.diagnostic_code << '\n';
  }
  Require(maintained.ok(), "maintenance failed");
  return maintained;
}

void DeleteLocator(page::IndexHashPhysicalIndex* index,
                  const std::vector<platform::byte>& key,
                  const page::IndexHashPhysicalRowLocator& locator) {
  auto route = page::LocateIndexHashBucket(*index, key);
  Require(route.ok(), "delete route failed");
  auto latch = page::AcquireIndexHashBucketExclusiveLatch(index, route.bucket_page_number);
  Require(latch.active(), "exclusive delete latch acquisition failed");
  page::IndexHashPhysicalDeleteRequest request;
  request.encoded_key = key;
  request.row_uuid = locator.row_uuid;
  request.version_uuid = locator.version_uuid;
  request.latch_evidence = latch.evidence();
  auto deleted = page::DeleteIndexHashEntry(index, request);
  Require(deleted.ok() && deleted.tombstone_marked, "delete failed");
}

void RequireReopenValid(const page::IndexHashPhysicalIndex& index,
                        platform::u32 expected_bucket_count,
                        platform::u64 expected_live_entries) {
  auto exported = page::ExportIndexHashPhysicalIndexImage(index);
  Require(exported.ok(), "export failed");
  auto imported = page::ImportIndexHashPhysicalIndexImage(exported.image);
  Require(imported.ok(), "import failed");
  auto report = page::BuildIndexHashPhysicalReport(imported.index);
  Require(report.ok() && report.report.valid, "reopened report invalid");
  Require(report.report.bucket_count == expected_bucket_count,
          "reopened bucket count mismatch");
  Require(report.report.live_entry_count == expected_live_entries,
          "reopened live entry count mismatch");
}

void RequireEntriesRoutedByDirectory(const page::IndexHashPhysicalIndex& index) {
  auto directory = page::FetchIndexHashPhysicalPage(index, index.directory_page_number);
  Require(directory.ok(), "route assertion directory fetch failed");
  bool saw_last_bucket_entry = false;
  for (platform::u32 bucket_index = 0;
       bucket_index < directory.body.directory_bucket_page_numbers.size();
       ++bucket_index) {
    platform::u64 current = directory.body.directory_bucket_page_numbers[bucket_index];
    while (current != 0) {
      auto fetched = page::FetchIndexHashPhysicalPage(index, current);
      Require(fetched.ok(), "route assertion page fetch failed");
      for (const auto& entry : fetched.body.entries) {
        Require(entry.key_hash % directory.body.bucket_count == bucket_index,
                "entry remained in wrong physical bucket after mutation");
        if (!entry.deleted && bucket_index + 1 == directory.body.bucket_count) {
          saw_last_bucket_entry = true;
        }
      }
      current = fetched.body.overflow_page_number;
    }
  }
  Require(saw_last_bucket_entry, "split did not physically populate new bucket");
}

void SplitRedistributesAndReopens() {
  auto index = NewIndex(2, 2048);
  std::vector<std::vector<platform::byte>> keys;
  bool has_new_bucket_residue = false;
  for (int i = 0; keys.size() < 10 || !has_new_bucket_residue; ++i) {
    auto key = Key("split-key-" + std::to_string(i));
    const auto hash = page::ComputeIndexHashKeyHashWithSeed(
        index.hash_seed,
        index.hash_seed_high64,
        index.hash_algorithm_version,
        key);
    if (keys.size() >= 10 && hash % 3 != 2) {
      continue;
    }
    has_new_bucket_residue = has_new_bucket_residue || hash % 3 == 2;
    keys.push_back(key);
    Insert(&index,
           key,
           static_cast<platform::byte>(0x20 + keys.size()),
           static_cast<platform::byte>(0x40 + keys.size()));
  }

  auto maintained = Maintain(&index);
  Require(maintained.hash_split_applied, "split not applied");
  Require(!maintained.hash_merge_applied, "split unexpectedly merged");
  Require(maintained.old_bucket_count == 2 && maintained.new_bucket_count == 3,
          "split bucket count mismatch");
  Require(HasEvidence(maintained.evidence, "hash_split_applied=true"),
          "split evidence missing");
  Require(HasEvidence(maintained.evidence,
                      "hash_structural_mutation_method=linear_modulo_rehash"),
          "linear split method evidence missing");
  Require(HasEvidence(maintained.evidence, "benchmark_clean_capability=false"),
          "benchmark-clean refusal evidence missing");
  RequireEntriesRoutedByDirectory(index);
  for (const auto& key : keys) {
    Require(Probe(index, key).locators.size() == 1, "split lost locator");
  }
  RequireReopenValid(index, 3, keys.size());
}

void MergeCompactsOverflowAndReopens() {
  auto index = NewIndex(3, 512);
  const auto hot_key = Key("merge-hot-overflow-key");
  for (int i = 0; i < 12; ++i) {
    Insert(&index,
           hot_key,
           static_cast<platform::byte>(0x70 + i),
           static_cast<platform::byte>(0x90 + i));
  }
  auto before = page::BuildIndexHashPhysicalReport(index);
  Require(before.report.valid && before.report.overflow_page_count > 0,
          "overflow setup failed");

  auto locators = Probe(index, hot_key).locators;
  Require(locators.size() == 12, "delete setup locator count mismatch");
  for (std::size_t i = 0; i < 10; ++i) {
    DeleteLocator(&index, hot_key, locators[i]);
  }

  auto maintained = Maintain(&index);
  Require(maintained.hash_merge_applied, "merge not applied");
  Require(maintained.hash_overflow_compaction_applied, "compaction not applied");
  Require(maintained.new_bucket_count == 2, "merge bucket count mismatch");
  Require(maintained.overflow_pages_reclaimed > 0, "overflow pages not reclaimed");
  Require(HasEvidence(maintained.evidence, "hash_merge_applied=true"),
          "merge evidence missing");
  Require(HasEvidence(maintained.evidence,
                      "hash_overflow_compaction_applied=true"),
          "compaction evidence missing");

  auto after = page::BuildIndexHashPhysicalReport(index);
  Require(after.report.valid, "post merge report invalid");
  Require(after.report.tombstone_entry_count == 0, "tombstones were not compacted");
  Require(after.report.live_entry_count == 2, "live locators not preserved");
  Require(Probe(index, hot_key).locators.size() == 2, "merge lost live locators");
  RequireReopenValid(index, 2, 2);
}

void MissingLatchProofFailsClosed() {
  auto index = NewIndex(2);
  page::IndexHashPhysicalMaintenanceRequest request;
  auto refused = page::MaintainIndexHashPhysicalStructure(&index, request);
  Require(!refused.ok(), "maintenance without latches succeeded");
  Require(refused.diagnostic.diagnostic_code == "SB-INDEX-HASH-LATCH-PROOF-MISSING",
          "missing latch diagnostic mismatch");
  Require(HasEvidence(refused.evidence, "hash_structural_mutation_refused=true"),
          "missing latch refusal evidence absent");
  Require(HasEvidence(refused.evidence, "unsafe_repair_refused=true"),
          "unsafe repair refusal evidence absent");
}

void CorruptSplitMergeAndOverflowImagesRefused() {
  auto split_index = NewIndex(2, 2048);
  for (int i = 0; i < 10; ++i) {
    Insert(&split_index,
           Key("corrupt-split-" + std::to_string(i)),
           static_cast<platform::byte>(0xa0 + i),
           static_cast<platform::byte>(0xb0 + i));
  }
  Require(Maintain(&split_index).hash_split_applied, "split corruption setup failed");
  auto split_image = page::ExportIndexHashPhysicalIndexImage(split_index).image;
  split_image.pages.front().serialized.back() ^= 0x5u;
  auto refused_split = page::ImportIndexHashPhysicalIndexImage(split_image);
  Require(!refused_split.ok(), "corrupt split image reopened");
  auto corrupt_split_index = split_index;
  corrupt_split_index.pages.front().serialized.back() ^= 0x5u;
  page::IndexHashPhysicalMaintenanceRequest repair_request;
  auto repair_refused =
      page::MaintainIndexHashPhysicalStructure(&corrupt_split_index, repair_request);
  Require(!repair_refused.ok(), "corrupt split image maintenance succeeded");
  Require(HasEvidence(repair_refused.evidence, "unsafe_repair_refused=true"),
          "unsafe repair refusal evidence missing on corrupt split image");
  Require(HasEvidence(repair_refused.evidence, "page_image_validation_failed=true"),
          "page image validation refusal evidence missing");

  auto merge_index = NewIndex(3);
  Insert(&merge_index, Key("merge-survivor"), 0xc1, 0xc2);
  Require(Maintain(&merge_index).hash_merge_applied, "merge corruption setup failed");
  auto merge_image = page::ExportIndexHashPhysicalIndexImage(merge_index).image;
  platform::StoreLittle16(merge_image.pages.front().serialized.data() + kOffsetPageKind,
                          static_cast<platform::u16>(0x7777u));
  Rechecksum(&merge_image.pages.front().serialized);
  auto refused_merge = page::ImportIndexHashPhysicalIndexImage(merge_image);
  Require(!refused_merge.ok(), "corrupt merge image reopened");

  auto overflow_index = NewIndex(1, 512);
  for (int i = 0; i < 12; ++i) {
    Insert(&overflow_index,
           Key("corrupt-overflow"),
           static_cast<platform::byte>(0xd0 + i),
           static_cast<platform::byte>(0xe0 + i));
  }
  auto overflow_image = page::ExportIndexHashPhysicalIndexImage(overflow_index).image;
  auto overflow_page = std::find_if(
      overflow_image.pages.begin(),
      overflow_image.pages.end(),
      [](const page::IndexHashPhysicalPageImage& image) {
        return platform::LoadLittle16(image.serialized.data() + kOffsetPageKind) ==
               static_cast<platform::u16>(page::IndexHashPageKind::overflow);
      });
  Require(overflow_page != overflow_image.pages.end(), "overflow image missing");
  platform::StoreLittle64(overflow_page->serialized.data() +
                              kOffsetOwningBucketPageNumber,
                          9999);
  Rechecksum(&overflow_page->serialized);
  auto refused_overflow = page::ImportIndexHashPhysicalIndexImage(overflow_image);
  Require(!refused_overflow.ok(), "corrupt overflow image reopened");

  auto orphan_overflow = overflow_index;
  auto compacted = Maintain(&orphan_overflow);
  Require(compacted.hash_overflow_compaction_applied,
          "orphan overflow setup did not compact");
  auto orphan_image = *overflow_page;
  orphan_image.page_number = 99999;
  platform::StoreLittle64(orphan_image.serialized.data() + kOffsetPageNumber,
                          orphan_image.page_number);
  Rechecksum(&orphan_image.serialized);
  orphan_overflow.pages.push_back(std::move(orphan_image));
  auto orphan_report = page::BuildIndexHashPhysicalReport(orphan_overflow);
  Require(!orphan_report.report.valid, "orphan overflow page was accepted");
  Require(orphan_report.report.corruption_class ==
              page::IndexHashPhysicalCorruptionClass::overflow_chain,
          "orphan overflow corruption class mismatch");
}

}  // namespace

int main() {
  SplitRedistributesAndReopens();
  MergeCompactsOverflowAndReopens();
  MissingLatchProofFailsClosed();
  CorruptSplitMergeAndOverflowImagesRefused();
  return EXIT_SUCCESS;
}
