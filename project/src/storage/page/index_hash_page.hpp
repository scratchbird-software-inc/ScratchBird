// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-HASH-PAGE-ANCHOR
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <memory>
#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u16 kIndexHashAlgorithmVersion1LegacyFnv64 = 1;
inline constexpr u16 kIndexHashAlgorithmVersion2KeyedHash64 = 2;
inline constexpr u16 kIndexHashAlgorithmVersion3KeyedHash128Fingerprint = 3;
inline constexpr u16 kIndexHashAlgorithmVersion1 =
    kIndexHashAlgorithmVersion1LegacyFnv64;
inline constexpr u16 kIndexHashProductionDefaultAlgorithmVersion =
    kIndexHashAlgorithmVersion2KeyedHash64;
inline constexpr u16 kIndexHashHighAssuranceAlgorithmVersion =
    kIndexHashAlgorithmVersion3KeyedHash128Fingerprint;
inline constexpr u16 kIndexHashNoEntryOrdinal = 0xffffu;
inline constexpr u32 kIndexHashPageBodyHeaderBytes = 104;

enum class IndexHashPageKind : u16 {
  directory = 1,
  bucket = 2,
  overflow = 3,
  unknown = 0xffffu
};

enum class IndexHashBucketLatchMode : u16 {
  none = 0,
  shared = 1,
  optimistic = 2,
  exclusive = 3
};

enum class IndexHashPhysicalCorruptionClass : u16 {
  none = 0,
  checksum = 1,
  page_kind = 2,
  seed_version = 3,
  directory = 4,
  overflow_chain = 5,
  collision_chain = 6,
  page = 7,
  unknown = 0xffffu
};

struct IndexHashRowVersionLocator {
  TypedUuid row_uuid;
  TypedUuid version_uuid;
};

struct IndexHashEntry {
  u64 key_hash = 0;
  u64 key_fingerprint_low64 = 0;
  u64 key_fingerprint_high64 = 0;
  u16 fingerprint_algorithm_version = 0;
  bool fingerprint_present = false;
  std::vector<byte> encoded_key;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  bool deleted = false;
  u64 next_collision_page_number = 0;
  u16 next_collision_ordinal = kIndexHashNoEntryOrdinal;
};

struct IndexHashCollisionRoot {
  u64 key_hash = 0;
  u64 page_number = 0;
  u16 entry_ordinal = kIndexHashNoEntryOrdinal;
};

struct IndexHashPageBody {
  TypedUuid index_uuid;
  u64 page_number = 0;
  IndexHashPageKind page_kind = IndexHashPageKind::unknown;
  u64 hash_seed = 0;
  u64 hash_seed_high64 = 0;
  u16 hash_algorithm_version = kIndexHashProductionDefaultAlgorithmVersion;
  bool test_fixture_force_route_hash_collision = false;
  bool test_fixture_force_fingerprint_collision = false;
  u32 bucket_index = 0;
  u32 bucket_count = 0;
  u64 overflow_page_number = 0;
  u64 owning_bucket_page_number = 0;
  u32 free_space_bytes = 0;
  std::vector<u64> directory_bucket_page_numbers;
  std::vector<IndexHashCollisionRoot> collision_roots;
  std::vector<IndexHashEntry> entries;
};

struct IndexHashPageBodyResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexHashPageBody body;
  std::vector<byte> serialized;

  bool ok() const {
    return status.ok();
  }
};

struct IndexHashPhysicalPageImage {
  u64 page_number = 0;
  std::vector<byte> serialized;
};

struct IndexHashPhysicalIndex {
  u32 page_size = 0;
  TypedUuid index_uuid;
  u64 hash_seed = 0;
  u64 hash_seed_high64 = 0;
  u16 hash_algorithm_version = kIndexHashProductionDefaultAlgorithmVersion;
  bool hash_seed_engine_generated = true;
  bool hash_seed_test_fixture_mode = false;
  bool hash_seed_protected = true;
  std::string hash_seed_entropy_source = "engine_os_entropy";
  bool test_fixture_force_route_hash_collision = false;
  bool test_fixture_force_fingerprint_collision = false;
  u64 directory_page_number = 1;
  u64 next_page_number = 1;
  std::vector<IndexHashPhysicalPageImage> pages;
};

struct IndexHashPhysicalIndexImage {
  u32 page_size = 0;
  TypedUuid index_uuid;
  u64 hash_seed = 0;
  u64 hash_seed_high64 = 0;
  u16 hash_algorithm_version = kIndexHashProductionDefaultAlgorithmVersion;
  bool hash_seed_engine_generated = true;
  bool hash_seed_test_fixture_mode = false;
  bool hash_seed_protected = true;
  std::string hash_seed_entropy_source = "engine_os_entropy";
  bool test_fixture_force_route_hash_collision = false;
  bool test_fixture_force_fingerprint_collision = false;
  u64 directory_page_number = 1;
  u64 next_page_number = 1;
  std::vector<IndexHashPhysicalPageImage> pages;
  std::vector<std::string> evidence;
};

struct IndexHashPhysicalIndexResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexHashPhysicalIndex index;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexHashPhysicalIndexImageResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexHashPhysicalIndexImage image;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexHashBucketLatchEvidence {
  u64 bucket_page_number = 0;
  u32 bucket_index = 0;
  IndexHashBucketLatchMode mode = IndexHashBucketLatchMode::none;
  u64 token = 0;
  bool active = false;
};

struct IndexHashBucketLatchGuardState;

class IndexHashBucketLatchGuard {
 public:
  IndexHashBucketLatchGuard();
  IndexHashBucketLatchGuard(IndexHashBucketLatchGuard&& other) noexcept;
  IndexHashBucketLatchGuard& operator=(IndexHashBucketLatchGuard&& other) noexcept;
  ~IndexHashBucketLatchGuard();

  IndexHashBucketLatchGuard(const IndexHashBucketLatchGuard&) = delete;
  IndexHashBucketLatchGuard& operator=(const IndexHashBucketLatchGuard&) = delete;

  const IndexHashBucketLatchEvidence& evidence() const {
    return evidence_;
  }

  bool active() const {
    return state_ != nullptr && evidence_.active;
  }

 private:
  friend IndexHashBucketLatchGuard AcquireIndexHashBucketSharedLatch(
      const IndexHashPhysicalIndex& index,
      u64 bucket_page_number);
  friend IndexHashBucketLatchGuard AcquireIndexHashBucketOptimisticLatch(
      const IndexHashPhysicalIndex& index,
      u64 bucket_page_number);
  friend IndexHashBucketLatchGuard AcquireIndexHashBucketExclusiveLatch(
      IndexHashPhysicalIndex* index,
      u64 bucket_page_number);

  IndexHashBucketLatchGuard(std::shared_ptr<IndexHashBucketLatchGuardState> state,
                            IndexHashBucketLatchEvidence evidence);

  std::shared_ptr<IndexHashBucketLatchGuardState> state_;
  IndexHashBucketLatchEvidence evidence_;
};

struct IndexHashBucketRouteResult {
  Status status;
  DiagnosticRecord diagnostic;
  u64 key_hash = 0;
  u32 bucket_index = 0;
  u64 bucket_page_number = 0;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexHashPhysicalInsertRequest {
  std::vector<byte> encoded_key;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  IndexHashBucketLatchEvidence latch_evidence;
};

struct IndexHashPhysicalInsertResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool inserted = false;
  bool overflow_page_created = false;
  u64 key_hash = 0;
  u32 bucket_index = 0;
  u64 bucket_page_number = 0;
  u64 inserted_page_number = 0;
  u16 inserted_entry_ordinal = kIndexHashNoEntryOrdinal;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexHashPhysicalDeleteRequest {
  std::vector<byte> encoded_key;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  IndexHashBucketLatchEvidence latch_evidence;
};

struct IndexHashPhysicalDeleteResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool deleted = false;
  bool tombstone_marked = false;
  u64 key_hash = 0;
  u32 bucket_index = 0;
  u64 bucket_page_number = 0;
  u64 deleted_page_number = 0;
  u16 deleted_entry_ordinal = kIndexHashNoEntryOrdinal;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexHashPhysicalMaintenancePolicy {
  u32 split_load_factor_per_mille = 4000;
  u32 merge_load_factor_per_mille = 1000;
  u32 tombstone_ratio_per_mille = 250;
  u32 overflow_depth_threshold = 1;
  u32 collision_chain_warn_threshold = 16;
  u32 collision_chain_rebuild_threshold = 64;
  u32 min_bucket_count = 1;
  u32 max_bucket_count = 256;
};

struct IndexHashHardeningPolicy {
  bool allow_legacy_fnv64_for_tests = false;
  u16 production_default_algorithm =
      kIndexHashProductionDefaultAlgorithmVersion;
  bool high_assurance_mode_enabled = false;
  u32 collision_chain_warn_threshold = 16;
  u32 collision_chain_rebuild_threshold = 64;
  u32 overflow_depth_warn_threshold = 1;
  bool require_keyed_hash_for_user_supplied_uuid = true;
  bool require_high_assurance_for_security_catalogs = true;
  bool require_high_assurance_for_cluster_catalogs = true;
};

struct IndexHashPhysicalMaintenanceRequest {
  IndexHashPhysicalMaintenancePolicy policy;
  std::vector<IndexHashBucketLatchEvidence> exclusive_bucket_latches;
};

struct IndexHashPhysicalMaintenanceResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool hash_split_applied = false;
  bool hash_merge_applied = false;
  bool hash_overflow_compaction_applied = false;
  bool benchmark_clean_capability = false;
  u32 old_bucket_count = 0;
  u32 new_bucket_count = 0;
  u64 live_entry_count = 0;
  u64 tombstone_entry_count = 0;
  u32 load_factor_per_mille = 0;
  u32 tombstone_ratio_per_mille = 0;
  u32 max_overflow_depth = 0;
  u64 overflow_pages_reclaimed = 0;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexHashPhysicalRowLocator {
  std::vector<byte> encoded_key;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  u64 key_hash = 0;
  u32 bucket_index = 0;
  u64 bucket_page_number = 0;
  u64 page_number = 0;
  u16 entry_ordinal = kIndexHashNoEntryOrdinal;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool tombstone_excluded = true;
};

struct IndexHashPhysicalProbeRequest {
  std::vector<byte> encoded_key;
  IndexHashBucketLatchEvidence latch_evidence;
};

struct IndexHashPhysicalProbeResult {
  Status status;
  DiagnosticRecord diagnostic;
  u64 key_hash = 0;
  u64 key_fingerprint_low64 = 0;
  u64 key_fingerprint_high64 = 0;
  bool fingerprint_present = false;
  u32 bucket_index = 0;
  u64 bucket_page_number = 0;
  u64 pages_traversed = 0;
  u64 collision_entries_traversed = 0;
  u64 fingerprint_mismatch_count = 0;
  u64 encoded_key_compare_count = 0;
  std::vector<IndexHashPhysicalRowLocator> locators;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexHashPhysicalReport {
  Status status;
  DiagnosticRecord diagnostic;
  bool valid = false;
  u64 directory_page_number = 0;
  u32 bucket_count = 0;
  u64 page_count = 0;
  u64 bucket_page_count = 0;
  u64 overflow_page_count = 0;
  u64 live_entry_count = 0;
  u64 tombstone_entry_count = 0;
  u64 collision_root_count = 0;
  u32 max_collision_chain_length = 0;
  u64 total_collision_chain_length = 0;
  u32 average_collision_chain_length = 0;
  u32 max_overflow_depth = 0;
  u32 bucket_load_factor_per_mille = 0;
  u64 collision_count = 0;
  u64 fingerprint_mismatch_count = 0;
  u64 encoded_key_compare_count = 0;
  u32 page_size = 0;
  u64 hash_seed = 0;
  u64 hash_seed_high64 = 0;
  u16 hash_algorithm_version = 0;
  bool hash_seed_engine_generated = false;
  bool hash_seed_client_supplied = false;
  bool hash_seed_test_fixture_mode = false;
  bool hash_seed_protected = false;
  std::string hash_seed_entropy_source;
  bool high_assurance_fingerprint_present = false;
  bool rebuild_recommended = false;
  bool reseed_recommended = false;
  bool legacy_compatible = false;
  IndexHashPhysicalCorruptionClass corruption_class =
      IndexHashPhysicalCorruptionClass::none;
  std::string exact_diagnostic_code;
  std::string exact_diagnostic_message_key;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  std::vector<std::string> evidence;
  std::vector<std::string> support_bundle_rows;

  bool ok() const {
    return status.ok();
  }
};

struct IndexHashPhysicalReportResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexHashPhysicalReport report;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

const char* IndexHashPageKindName(IndexHashPageKind kind);
const char* IndexHashBucketLatchModeName(IndexHashBucketLatchMode mode);
const char* IndexHashAlgorithmVersionName(u16 version);
const char* IndexHashPhysicalCorruptionClassName(
    IndexHashPhysicalCorruptionClass corruption_class);

u64 ComputeIndexHashPageChecksum(const std::vector<byte>& body);
u64 ComputeIndexHashKeyHash(u64 hash_seed,
                            u16 hash_algorithm_version,
                            const std::vector<byte>& encoded_key,
                            bool test_fixture_force_route_hash_collision = false);
u64 ComputeIndexHashKeyHashWithSeed(u64 hash_seed_low64,
                                    u64 hash_seed_high64,
                                    u16 hash_algorithm_version,
                                    const std::vector<byte>& encoded_key,
                                    bool test_fixture_force_route_hash_collision = false);
IndexHashEntry ComputeIndexHashEntryFingerprints(
    IndexHashEntry entry,
    u64 hash_seed,
    u16 hash_algorithm_version,
    bool test_fixture_force_route_hash_collision = false,
    bool test_fixture_force_fingerprint_collision = false);
IndexHashHardeningPolicy DefaultIndexHashHardeningPolicy();

DiagnosticRecord MakeIndexHashPageDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

IndexHashPageBodyResult BuildIndexHashPageBody(const IndexHashPageBody& body,
                                               u32 page_size);
IndexHashPageBodyResult ParseIndexHashPageBody(const std::vector<byte>& serialized,
                                               u64 page_number,
                                               u64 expected_hash_seed = 0,
                                               u16 expected_hash_algorithm_version = 0,
                                               bool test_fixture_force_route_hash_collision = false,
                                               bool test_fixture_force_fingerprint_collision = false,
                                               u64 expected_hash_seed_high64 = 0);

IndexHashPhysicalIndexResult InitializeIndexHashPhysicalIndex(
    TypedUuid index_uuid,
    u32 page_size,
    u64 hash_seed,
    u16 hash_algorithm_version,
    u32 bucket_count,
    bool test_fixture_seed_allowed = false,
    bool test_fixture_force_route_hash_collision = false,
    bool test_fixture_force_fingerprint_collision = false);

IndexHashPhysicalIndexImageResult ExportIndexHashPhysicalIndexImage(
    const IndexHashPhysicalIndex& index);
IndexHashPhysicalIndexResult ImportIndexHashPhysicalIndexImage(
    const IndexHashPhysicalIndexImage& image);

IndexHashPageBodyResult FetchIndexHashPhysicalPage(const IndexHashPhysicalIndex& index,
                                                   u64 page_number);
IndexHashBucketRouteResult LocateIndexHashBucket(
    const IndexHashPhysicalIndex& index,
    const std::vector<byte>& encoded_key);

IndexHashBucketLatchGuard AcquireIndexHashBucketSharedLatch(
    const IndexHashPhysicalIndex& index,
    u64 bucket_page_number);
IndexHashBucketLatchGuard AcquireIndexHashBucketOptimisticLatch(
    const IndexHashPhysicalIndex& index,
    u64 bucket_page_number);
IndexHashBucketLatchGuard AcquireIndexHashBucketExclusiveLatch(
    IndexHashPhysicalIndex* index,
    u64 bucket_page_number);

IndexHashPhysicalInsertResult InsertIndexHashEntry(
    IndexHashPhysicalIndex* index,
    const IndexHashPhysicalInsertRequest& request);
IndexHashPhysicalProbeResult ProbeIndexHashBucket(
    const IndexHashPhysicalIndex& index,
    const IndexHashPhysicalProbeRequest& request);
IndexHashPhysicalDeleteResult DeleteIndexHashEntry(
    IndexHashPhysicalIndex* index,
    const IndexHashPhysicalDeleteRequest& request);
IndexHashPhysicalMaintenanceResult MaintainIndexHashPhysicalStructure(
    IndexHashPhysicalIndex* index,
    const IndexHashPhysicalMaintenanceRequest& request);

IndexHashPhysicalReportResult BuildIndexHashPhysicalReport(
    const IndexHashPhysicalIndex& index);

}  // namespace scratchbird::storage::page
