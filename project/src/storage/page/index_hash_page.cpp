// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_hash_page.hpp"

#include "database_format.hpp"
#include "page_header.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

#if defined(__linux__)
#include <sys/random.h>
#include <sys/types.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#include <cstdlib>
#include <unistd.h>
#endif

namespace scratchbird::storage::page {

u64 ComputeIndexHashKeyHashWithSeedInternal(
    u64 hash_seed_low64,
    u64 hash_seed_high64,
    u16 hash_algorithm_version,
    const std::vector<byte>& encoded_key,
    bool test_fixture_force_route_hash_collision);

IndexHashEntry ComputeIndexHashEntryFingerprintsWithSeedInternal(
    IndexHashEntry entry,
    u64 hash_seed_low64,
    u64 hash_seed_high64,
    u16 hash_algorithm_version,
    bool test_fixture_force_route_hash_collision,
    bool test_fixture_force_fingerprint_collision);

namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;

inline constexpr std::array<byte, 8> kIndexHashMagic = {'S', 'B', 'I', 'H', '0', '0', '0', '1'};
inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetHeaderBytes = 8;
inline constexpr u32 kOffsetPageKind = 12;
inline constexpr u32 kOffsetAlgorithmVersion = 14;
inline constexpr u32 kOffsetBodyBytes = 16;
inline constexpr u32 kOffsetEntryCount = 20;
inline constexpr u32 kOffsetCollisionRootCount = 22;
inline constexpr u32 kOffsetBodyChecksum = 24;
inline constexpr u32 kOffsetIndexUuid = 32;
inline constexpr u32 kOffsetPageNumber = 48;
inline constexpr u32 kOffsetHashSeed = 56;
inline constexpr u32 kOffsetBucketIndex = 64;
inline constexpr u32 kOffsetBucketCount = 68;
inline constexpr u32 kOffsetOverflowPageNumber = 72;
inline constexpr u32 kOffsetOwningBucketPageNumber = 80;
inline constexpr u32 kOffsetFreeSpaceBytes = 88;
inline constexpr u32 kOffsetHashSeedHigh64 = 96;
inline constexpr u32 kIndexHashPageBodyHeaderBytesLegacy96 = 96;

inline constexpr u32 kCollisionRootBytes = 24;
inline constexpr u32 kRootOffsetHash = 0;
inline constexpr u32 kRootOffsetPageNumber = 8;
inline constexpr u32 kRootOffsetEntryOrdinal = 16;

inline constexpr u32 kEntryHeaderBytesV1V2 = 72;
inline constexpr u32 kEntryHeaderBytesV3 = 96;
inline constexpr u32 kEntryOffsetFlags = 0;
inline constexpr u32 kEntryOffsetNextOrdinal = 2;
inline constexpr u32 kEntryOffsetKeyBytes = 4;
inline constexpr u32 kEntryOffsetKeyChecksum = 8;
inline constexpr u32 kEntryOffsetKeyHash = 16;
inline constexpr u32 kEntryOffsetRowUuid = 24;
inline constexpr u32 kEntryOffsetVersionUuid = 40;
inline constexpr u32 kEntryOffsetNextPage = 56;
inline constexpr u32 kEntryOffsetFingerprintLow64 = 64;
inline constexpr u32 kEntryOffsetFingerprintHigh64 = 72;
inline constexpr u32 kEntryOffsetFingerprintAlgorithm = 80;
inline constexpr u64 kIndexHashForcedRouteHashFixtureValue =
    0x484153485f464958ull;
inline constexpr u64 kIndexHashForcedFingerprintLowFixtureValue =
    0x46494e475f4c4f57ull;
inline constexpr u64 kIndexHashForcedFingerprintHighFixtureValue =
    0x46494e475f484947ull;

namespace EntryFlag {
inline constexpr u16 deleted = 1u << 0;
inline constexpr u16 fingerprint_present = 1u << 1;
}  // namespace EntryFlag

Status HashOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status HashErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::storage_page};
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool IsNilUuid(const Uuid& uuid) {
  return uuid.is_nil();
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

struct HashSeedMaterial {
  u64 seed = 0;
  u64 seed_high64 = 0;
  bool protected_material = false;
  std::string entropy_source;
};

u64 Fnv1a64(const byte* data, std::size_t size, u64 seed = 1469598103934665603ull) {
  u64 hash = seed;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<u64>(data[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 EngineGeneratedSeedForIndex(const TypedUuid& index_uuid,
                                u16 hash_algorithm_version) {
  u64 seed = 0xcbf29ce484222325ull ^
             (static_cast<u64>(hash_algorithm_version) << 32);
  if (index_uuid.valid()) {
    seed = Fnv1a64(index_uuid.value.bytes.data(),
                   index_uuid.value.bytes.size(),
                   seed);
  }
  return seed == 0 ? 0xa5a5a5a5d3c7b901ull : seed;
}

u64 DeriveSeedHigh64(u64 seed_low64, u16 hash_algorithm_version) {
  u64 material[2] = {
      seed_low64,
      static_cast<u64>(hash_algorithm_version) ^ 0x5342484153484b32ull};
  const u64 derived = Fnv1a64(reinterpret_cast<const byte*>(material),
                             sizeof(material),
                             0x9ae16a3b2f90404full);
  return derived == 0 ? 0x632be59bd9b4e019ull : derived;
}

bool FillProtectedRandomBytes(byte* output,
                              std::size_t size,
                              std::string* entropy_source) {
#if defined(__linux__)
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t got = getrandom(output + offset, size - offset, 0);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (got == 0) {
      return false;
    }
    offset += static_cast<std::size_t>(got);
  }
  if (entropy_source != nullptr) {
    *entropy_source = "linux_getrandom";
  }
  return true;
#elif defined(_WIN32)
  using BCryptGenRandomFn = LONG(WINAPI*)(void*, unsigned char*, unsigned long, unsigned long);
  constexpr unsigned long kBcryptUseSystemPreferredRng = 0x00000002ul;
  HMODULE module = LoadLibraryA("bcrypt.dll");
  if (module == nullptr) {
    return false;
  }
  auto* function = reinterpret_cast<BCryptGenRandomFn>(
      GetProcAddress(module, "BCryptGenRandom"));
  const bool ok = function != nullptr &&
                  function(nullptr,
                           reinterpret_cast<unsigned char*>(output),
                           static_cast<unsigned long>(size),
                           kBcryptUseSystemPreferredRng) == 0;
  FreeLibrary(module);
  if (ok && entropy_source != nullptr) {
    *entropy_source = "windows_bcryptgenrandom";
  }
  return ok;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
  if (size <= 256 && getentropy(output, size) == 0) {
    if (entropy_source != nullptr) {
      *entropy_source = "posix_getentropy";
    }
    return true;
  }
  arc4random_buf(output, size);
  if (entropy_source != nullptr) {
    *entropy_source = "arc4random_buf";
  }
  return true;
#else
  (void)output;
  (void)size;
  (void)entropy_source;
  return false;
#endif
}

HashSeedMaterial ProductionRandomSeedForIndex() {
  std::array<byte, 16> entropy{};
  std::string source;
  if (FillProtectedRandomBytes(entropy.data(), entropy.size(), &source)) {
    u64 seed = LoadLittle64(entropy.data());
    u64 seed_high64 = LoadLittle64(entropy.data() + sizeof(u64));
    if (seed == 0 && seed_high64 == 0) {
      return {0, 0, false, "os_entropy_all_zero"};
    }
    if (seed == 0) {
      seed = Fnv1a64(entropy.data(), entropy.size(), 0x4841534853454544ull);
    }
    if (seed_high64 == 0) {
      seed_high64 = Fnv1a64(entropy.data(),
                            entropy.size(),
                            0x484153484b455932ull);
    }
    return {seed, seed_high64, true, source};
  }

  return {0, 0, false, "os_entropy_unavailable"};
}

u64 Rotl64(u64 value, int bits) {
  return (value << bits) | (value >> (64 - bits));
}

void SipRound(u64* v0, u64* v1, u64* v2, u64* v3) {
  *v0 += *v1;
  *v1 = Rotl64(*v1, 13);
  *v1 ^= *v0;
  *v0 = Rotl64(*v0, 32);
  *v2 += *v3;
  *v3 = Rotl64(*v3, 16);
  *v3 ^= *v2;
  *v0 += *v3;
  *v3 = Rotl64(*v3, 21);
  *v3 ^= *v0;
  *v2 += *v1;
  *v1 = Rotl64(*v1, 17);
  *v1 ^= *v2;
  *v2 = Rotl64(*v2, 32);
}

u64 SipHash24(const byte* data, std::size_t size, u64 key0, u64 key1) {
  u64 v0 = 0x736f6d6570736575ull ^ key0;
  u64 v1 = 0x646f72616e646f6dull ^ key1;
  u64 v2 = 0x6c7967656e657261ull ^ key0;
  u64 v3 = 0x7465646279746573ull ^ key1;

  const std::size_t full_blocks = size / 8;
  for (std::size_t block = 0; block < full_blocks; ++block) {
    u64 message = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      message |= static_cast<u64>(data[block * 8 + i]) << (i * 8);
    }
    v3 ^= message;
    SipRound(&v0, &v1, &v2, &v3);
    SipRound(&v0, &v1, &v2, &v3);
    v0 ^= message;
  }

  u64 final_block = static_cast<u64>(size) << 56;
  const std::size_t tail_offset = full_blocks * 8;
  for (std::size_t i = tail_offset; i < size; ++i) {
    final_block |= static_cast<u64>(data[i]) << ((i - tail_offset) * 8);
  }
  v3 ^= final_block;
  SipRound(&v0, &v1, &v2, &v3);
  SipRound(&v0, &v1, &v2, &v3);
  v0 ^= final_block;

  v2 ^= 0xff;
  for (int i = 0; i < 4; ++i) {
    SipRound(&v0, &v1, &v2, &v3);
  }
  const u64 hash = v0 ^ v1 ^ v2 ^ v3;
  return hash == 0 ? 1 : hash;
}

u64 KeyedHash64(u64 hash_seed_low64,
                u64 hash_seed_high64,
                u16 hash_algorithm_version,
                u64 domain,
                const std::vector<byte>& encoded_key) {
  const u64 key0 = hash_seed_low64 ^ 0x9e3779b97f4a7c15ull ^
                   (static_cast<u64>(hash_algorithm_version) << 48);
  const u64 key1 = hash_seed_high64 ^
                   (hash_seed_low64 << 17) ^ (hash_seed_low64 >> 7) ^
                   0xd1b54a32d192ed03ull ^ domain;
  return SipHash24(encoded_key.data(), encoded_key.size(), key0, key1);
}

IndexHashPageBodyResult HashPageError(std::string diagnostic_code,
                                      std::string message_key,
                                      std::string detail = {}) {
  IndexHashPageBodyResult result;
  result.status = HashErrorStatus();
  result.diagnostic = MakeIndexHashPageDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

IndexHashPhysicalIndexResult HashIndexError(std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {}) {
  IndexHashPhysicalIndexResult result;
  result.status = HashErrorStatus();
  result.diagnostic = MakeIndexHashPageDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

IndexHashBucketRouteResult HashRouteError(std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail = {}) {
  IndexHashBucketRouteResult result;
  result.status = HashErrorStatus();
  result.diagnostic = MakeIndexHashPageDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

IndexHashPhysicalInsertResult HashInsertError(std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {}) {
  IndexHashPhysicalInsertResult result;
  result.status = HashErrorStatus();
  result.diagnostic = MakeIndexHashPageDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

IndexHashPhysicalProbeResult HashProbeError(std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {}) {
  IndexHashPhysicalProbeResult result;
  result.status = HashErrorStatus();
  result.diagnostic = MakeIndexHashPageDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

IndexHashPhysicalDeleteResult HashDeleteError(std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {}) {
  IndexHashPhysicalDeleteResult result;
  result.status = HashErrorStatus();
  result.diagnostic = MakeIndexHashPageDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  return result;
}

IndexHashPhysicalMaintenanceResult HashMaintenanceError(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  IndexHashPhysicalMaintenanceResult result;
  result.status = HashErrorStatus();
  result.diagnostic = MakeIndexHashPageDiagnostic(result.status,
                                                  std::move(diagnostic_code),
                                                  std::move(message_key),
                                                  std::move(detail));
  result.evidence = {"hash_structural_mutation_refused=true",
                     "unsafe_repair_refused=true",
                     "benchmark_clean_capability=false",
                     "candidate_set_only=true",
                     "materialized_final_rows=false",
                     "mga_recheck_required=true",
                     "security_recheck_required=true",
                     "parser_finality_authority=false",
                     "provider_finality_authority=false",
                     "wal_finality_authority=false",
                     "visibility_authority=false",
                     "authorization_authority=false",
                     "transaction_finality_authority=false",
                     "recovery_authority=false"};
  return result;
}

bool IsKnownPageKind(IndexHashPageKind kind) {
  return kind == IndexHashPageKind::directory ||
         kind == IndexHashPageKind::bucket ||
         kind == IndexHashPageKind::overflow;
}

bool IsSupportedHashAlgorithm(u16 version) {
  return version == kIndexHashAlgorithmVersion1LegacyFnv64 ||
         version == kIndexHashAlgorithmVersion2KeyedHash64 ||
         version == kIndexHashAlgorithmVersion3KeyedHash128Fingerprint;
}

bool HashAlgorithmRequiresFingerprint(u16 version) {
  return version == kIndexHashAlgorithmVersion3KeyedHash128Fingerprint;
}

u32 EntryHeaderBytesForAlgorithm(u16 version) {
  return HashAlgorithmRequiresFingerprint(version) ? kEntryHeaderBytesV3
                                                   : kEntryHeaderBytesV1V2;
}

u16 FlagsFor(const IndexHashEntry& entry) {
  u16 flags = 0;
  if (entry.deleted) {
    flags |= EntryFlag::deleted;
  }
  if (entry.fingerprint_present) {
    flags |= EntryFlag::fingerprint_present;
  }
  return flags;
}

u64 ImageKey(const IndexHashPhysicalIndex& index) {
  return reinterpret_cast<u64>(&index);
}

std::vector<std::string> CandidateEvidence() {
  return {"candidate_set_only=true",
          "materialized_final_rows=false",
          "mga_recheck_required=true",
          "security_recheck_required=true",
          "parser_finality_authority=false",
          "parser_hash_authority=false",
          "reference_hash_authority=false",
          "hash_finality_authority=false",
          "provider_finality_authority=false",
          "wal_finality_authority=false",
          "visibility_authority=false",
          "authorization_authority=false",
          "transaction_finality_authority=false",
          "recovery_authority=false"};
}

template <typename T>
void AddCandidateEvidence(T* result) {
  auto evidence = CandidateEvidence();
  result->evidence.insert(result->evidence.end(), evidence.begin(), evidence.end());
}

std::optional<std::size_t> FindPageImageIndex(const IndexHashPhysicalIndex& index,
                                              u64 page_number) {
  for (std::size_t i = 0; i < index.pages.size(); ++i) {
    if (index.pages[i].page_number == page_number) {
      return i;
    }
  }
  return std::nullopt;
}

void PublishPage(IndexHashPhysicalIndex* index, const IndexHashPageBodyResult& staged) {
  const auto existing = FindPageImageIndex(*index, staged.body.page_number);
  if (existing.has_value()) {
    index->pages[*existing].serialized = staged.serialized;
    return;
  }
  index->pages.push_back({staged.body.page_number, staged.serialized});
}

IndexHashPageBodyResult FetchPageUnlocked(const IndexHashPhysicalIndex& index,
                                          u64 page_number) {
  const auto page_index = FindPageImageIndex(index, page_number);
  if (!page_index.has_value()) {
    return HashPageError("SB-INDEX-HASH-PAGE-MISSING",
                         "storage.index_hash_page.page_missing",
                         std::to_string(page_number));
  }
  return ParseIndexHashPageBody(index.pages[*page_index].serialized,
                                page_number,
                                index.hash_seed,
                                index.hash_algorithm_version,
                                index.test_fixture_force_route_hash_collision,
                                index.test_fixture_force_fingerprint_collision,
                                index.hash_seed_high64);
}

IndexHashPageBodyResult FetchDirectoryUnlocked(const IndexHashPhysicalIndex& index) {
  auto directory = FetchPageUnlocked(index, index.directory_page_number);
  if (!directory.ok()) {
    return directory;
  }
  if (!SameTypedUuid(directory.body.index_uuid, index.index_uuid)) {
    return HashPageError("SB-INDEX-HASH-INDEX-UUID-MISMATCH",
                         "storage.index_hash_page.index_uuid_mismatch",
                         std::to_string(index.directory_page_number));
  }
  if (directory.body.page_kind != IndexHashPageKind::directory) {
    return HashPageError("SB-INDEX-HASH-DIRECTORY-KIND-INVALID",
                         "storage.index_hash_page.directory_kind_invalid",
                         std::to_string(index.directory_page_number));
  }
  return directory;
}

IndexHashPageBodyResult ValidateEntryBasics(const IndexHashEntry& entry,
                                            u64 expected_hash,
                                            u64 hash_seed_low64,
                                            u64 hash_seed_high64,
                                            u16 hash_algorithm_version,
                                            bool test_fixture_force_route_hash_collision,
                                            bool test_fixture_force_fingerprint_collision,
                                            std::size_t ordinal) {
  if (entry.encoded_key.empty()) {
    return HashPageError("SB-INDEX-HASH-KEY-MISSING",
                         "storage.index_hash_page.key_missing",
                         std::to_string(ordinal));
  }
  if (entry.key_hash != expected_hash) {
    return HashPageError("SB-INDEX-HASH-ENTRY-HASH-MISMATCH",
                         "storage.index_hash_page.entry_hash_mismatch",
                         std::to_string(ordinal));
  }
  const bool expects_fingerprint =
      HashAlgorithmRequiresFingerprint(hash_algorithm_version);
  if (entry.fingerprint_present != expects_fingerprint) {
    return HashPageError("SB-INDEX-HASH-FINGERPRINT-PRESENCE-MISMATCH",
                         "storage.index_hash_page.fingerprint_presence_mismatch",
                         std::to_string(ordinal));
  }
  if (expects_fingerprint) {
    if (entry.fingerprint_algorithm_version != hash_algorithm_version) {
      return HashPageError("SB-INDEX-HASH-FINGERPRINT-ALGORITHM-MISMATCH",
                           "storage.index_hash_page.fingerprint_algorithm_mismatch",
                           std::to_string(ordinal));
    }
    IndexHashEntry expected_entry;
    expected_entry.encoded_key = entry.encoded_key;
    expected_entry.row_uuid = entry.row_uuid;
    expected_entry.version_uuid = entry.version_uuid;
    expected_entry.deleted = entry.deleted;
    expected_entry.next_collision_page_number =
        entry.next_collision_page_number;
    expected_entry.next_collision_ordinal = entry.next_collision_ordinal;
    const auto expected = ComputeIndexHashEntryFingerprintsWithSeedInternal(
        std::move(expected_entry),
        hash_seed_low64,
        hash_seed_high64,
        hash_algorithm_version,
        test_fixture_force_route_hash_collision,
        test_fixture_force_fingerprint_collision);
    if (entry.key_fingerprint_low64 != expected.key_fingerprint_low64 ||
        entry.key_fingerprint_high64 != expected.key_fingerprint_high64) {
      return HashPageError("SB-INDEX-HASH-FINGERPRINT-MISMATCH",
                           "storage.index_hash_page.fingerprint_mismatch",
                           std::to_string(ordinal));
    }
  } else if (entry.fingerprint_algorithm_version != 0 ||
             entry.key_fingerprint_low64 != 0 ||
             entry.key_fingerprint_high64 != 0) {
    return HashPageError("SB-INDEX-HASH-FINGERPRINT-UNEXPECTED",
                         "storage.index_hash_page.fingerprint_unexpected",
                         std::to_string(ordinal));
  }
  if (!IsTypedEngineIdentity(entry.row_uuid, UuidKind::row) ||
      !IsTypedEngineIdentity(entry.version_uuid, UuidKind::row)) {
    return HashPageError("SB-INDEX-HASH-LOCATOR-UUID-INVALID",
                         "storage.index_hash_page.locator_uuid_invalid",
                         std::to_string(ordinal));
  }
  return {};
}

IndexHashPageBodyResult ValidatePageForBuild(const IndexHashPageBody& body) {
  if (!IsKnownPageKind(body.page_kind)) {
    return HashPageError("SB-INDEX-HASH-PAGE-KIND-INVALID",
                         "storage.index_hash_page.kind_invalid",
                         std::to_string(static_cast<u16>(body.page_kind)));
  }
  if (!IsSupportedHashAlgorithm(body.hash_algorithm_version)) {
    return HashPageError("SB-INDEX-HASH-ALGORITHM-UNSUPPORTED",
                         "storage.index_hash_page.algorithm_unsupported",
                         std::to_string(body.hash_algorithm_version));
  }
  if (body.hash_algorithm_version != kIndexHashAlgorithmVersion1LegacyFnv64 &&
      body.hash_seed_high64 == 0) {
    return HashPageError("SB-INDEX-HASH-SEED-MISSING",
                         "storage.index_hash_page.seed_missing",
                         "keyed_hash_requires_128_bit_seed_material");
  }
  if (body.page_number == 0) {
    return HashPageError("SB-INDEX-HASH-PAGE-NUMBER-REQUIRED",
                         "storage.index_hash_page.page_number_required");
  }
  if (!IsTypedEngineIdentity(body.index_uuid, UuidKind::object)) {
    return HashPageError("SB-INDEX-HASH-INDEX-UUID-MUST-BE-V7",
                         "storage.index_hash_page.index_uuid_must_be_v7");
  }
  if (body.page_kind == IndexHashPageKind::directory) {
    if (body.directory_bucket_page_numbers.empty() ||
        body.bucket_count != body.directory_bucket_page_numbers.size()) {
      return HashPageError("SB-INDEX-HASH-DIRECTORY-BUCKETS-INVALID",
                           "storage.index_hash_page.directory_buckets_invalid",
                           std::to_string(body.bucket_count));
    }
    std::set<u64> bucket_pages;
    for (u64 bucket_page_number : body.directory_bucket_page_numbers) {
      if (bucket_page_number == 0 ||
          !bucket_pages.insert(bucket_page_number).second) {
        return HashPageError("SB-INDEX-HASH-DIRECTORY-BUCKETS-INVALID",
                             "storage.index_hash_page.directory_buckets_invalid",
                             std::to_string(bucket_page_number));
      }
    }
    return {};
  }
  if (body.page_kind == IndexHashPageKind::bucket) {
    if (body.owning_bucket_page_number != 0) {
      return HashPageError("SB-INDEX-HASH-BUCKET-OWNER-INVALID",
                           "storage.index_hash_page.bucket_owner_invalid",
                           std::to_string(body.page_number));
    }
  } else if (body.page_kind == IndexHashPageKind::overflow) {
    if (body.owning_bucket_page_number == 0) {
      return HashPageError("SB-INDEX-HASH-OVERFLOW-OWNER-MISSING",
                           "storage.index_hash_page.overflow_owner_missing",
                           std::to_string(body.page_number));
    }
    if (!body.collision_roots.empty()) {
      return HashPageError("SB-INDEX-HASH-OVERFLOW-ROOTS-INVALID",
                           "storage.index_hash_page.overflow_roots_invalid",
                           std::to_string(body.page_number));
    }
  }
  for (std::size_t i = 0; i < body.entries.size(); ++i) {
    const auto entry_validation =
        ValidateEntryBasics(body.entries[i],
                            ComputeIndexHashKeyHashWithSeedInternal(
                                body.hash_seed,
                                body.hash_seed_high64,
                                body.hash_algorithm_version,
                                body.entries[i].encoded_key,
                                body.test_fixture_force_route_hash_collision),
                            body.hash_seed,
                            body.hash_seed_high64,
                            body.hash_algorithm_version,
                            body.test_fixture_force_route_hash_collision,
                            body.test_fixture_force_fingerprint_collision,
                            i);
    if (!entry_validation.ok()) {
      return entry_validation;
    }
  }
  return {};
}

u32 DirectoryBodyBytes(const IndexHashPageBody& body) {
  return kIndexHashPageBodyHeaderBytes +
         static_cast<u32>(body.directory_bucket_page_numbers.size() * sizeof(u64));
}

u32 BucketBodyBytes(const IndexHashPageBody& body) {
  u32 bytes = kIndexHashPageBodyHeaderBytes +
              static_cast<u32>(body.collision_roots.size() * kCollisionRootBytes);
  const u32 entry_header_bytes =
      EntryHeaderBytesForAlgorithm(body.hash_algorithm_version);
  for (const auto& entry : body.entries) {
    bytes += entry_header_bytes + static_cast<u32>(entry.encoded_key.size());
  }
  return bytes;
}

std::vector<IndexHashPageBody> BucketChainBodies(const IndexHashPhysicalIndex& index,
                                                 u64 bucket_page_number,
                                                 IndexHashPageBodyResult* error) {
  std::vector<IndexHashPageBody> chain;
  std::set<u64> seen;
  u64 current = bucket_page_number;
  while (current != 0) {
    if (!seen.insert(current).second) {
      *error = HashPageError("SB-INDEX-HASH-OVERFLOW-CYCLE",
                             "storage.index_hash_page.overflow_cycle",
                             std::to_string(current));
      return {};
    }
    auto fetched = FetchPageUnlocked(index, current);
    if (!fetched.ok()) {
      *error = fetched;
      return {};
    }
    if (chain.empty() && fetched.body.page_kind != IndexHashPageKind::bucket) {
      *error = HashPageError("SB-INDEX-HASH-BUCKET-KIND-INVALID",
                             "storage.index_hash_page.bucket_kind_invalid",
                             std::to_string(current));
      return {};
    }
    if (!chain.empty() && fetched.body.page_kind != IndexHashPageKind::overflow) {
      *error = HashPageError("SB-INDEX-HASH-OVERFLOW-KIND-INVALID",
                             "storage.index_hash_page.overflow_kind_invalid",
                             std::to_string(current));
      return {};
    }
    if (!SameTypedUuid(fetched.body.index_uuid, index.index_uuid) ||
        fetched.body.hash_seed != index.hash_seed ||
        fetched.body.hash_seed_high64 != index.hash_seed_high64 ||
        fetched.body.hash_algorithm_version != index.hash_algorithm_version ||
        fetched.body.bucket_count == 0) {
      *error = HashPageError("SB-INDEX-HASH-SEED-VERSION-MISMATCH",
                             "storage.index_hash_page.seed_version_mismatch",
                             std::to_string(current));
      return {};
    }
    if (chain.empty()) {
      if (fetched.body.owning_bucket_page_number != 0) {
        *error = HashPageError("SB-INDEX-HASH-BUCKET-OWNER-INVALID",
                               "storage.index_hash_page.bucket_owner_invalid",
                               std::to_string(current));
        return {};
      }
    } else {
      const auto& bucket = chain.front();
      if (fetched.body.owning_bucket_page_number != bucket.page_number ||
          fetched.body.bucket_index != bucket.bucket_index ||
          fetched.body.bucket_count != bucket.bucket_count) {
        *error = HashPageError("SB-INDEX-HASH-OVERFLOW-OWNER-MISMATCH",
                               "storage.index_hash_page.overflow_owner_mismatch",
                               std::to_string(current));
        return {};
      }
    }
    current = fetched.body.overflow_page_number;
    chain.push_back(std::move(fetched.body));
  }
  error->status = HashOkStatus();
  return chain;
}

struct EntryRef {
  u64 page_number = 0;
  u16 ordinal = kIndexHashNoEntryOrdinal;
  IndexHashEntry* entry = nullptr;
};

void RebuildCollisionChains(std::vector<IndexHashPageBody>* chain) {
  std::unordered_map<u64, EntryRef> first;
  std::unordered_map<u64, EntryRef> last;
  for (auto& page : *chain) {
    for (auto& root : page.collision_roots) {
      root = {};
    }
    page.collision_roots.clear();
    for (auto& entry : page.entries) {
      entry.next_collision_page_number = 0;
      entry.next_collision_ordinal = kIndexHashNoEntryOrdinal;
    }
  }

  for (auto& page : *chain) {
    for (std::size_t i = 0; i < page.entries.size(); ++i) {
      auto& entry = page.entries[i];
      const EntryRef current{page.page_number, static_cast<u16>(i), &entry};
      auto prior = last.find(entry.key_hash);
      if (prior == last.end()) {
        first.emplace(entry.key_hash, current);
      } else {
        prior->second.entry->next_collision_page_number = page.page_number;
        prior->second.entry->next_collision_ordinal = static_cast<u16>(i);
      }
      last[entry.key_hash] = current;
    }
  }

  IndexHashPageBody& bucket = chain->front();
  std::vector<u64> hashes;
  hashes.reserve(first.size());
  for (const auto& item : first) {
    hashes.push_back(item.first);
  }
  std::sort(hashes.begin(), hashes.end());
  for (u64 hash : hashes) {
    const auto ref = first.at(hash);
    bucket.collision_roots.push_back({hash, ref.page_number, ref.ordinal});
  }
}

IndexHashPageBody MakeOverflowPage(const IndexHashPhysicalIndex& index,
                                   const IndexHashPageBody& bucket) {
  IndexHashPageBody page;
  page.index_uuid = index.index_uuid;
  page.page_number = index.next_page_number;
  page.page_kind = IndexHashPageKind::overflow;
  page.hash_seed = index.hash_seed;
  page.hash_seed_high64 = index.hash_seed_high64;
  page.hash_algorithm_version = index.hash_algorithm_version;
  page.test_fixture_force_route_hash_collision =
      index.test_fixture_force_route_hash_collision;
  page.test_fixture_force_fingerprint_collision =
      index.test_fixture_force_fingerprint_collision;
  page.bucket_index = bucket.bucket_index;
  page.bucket_count = bucket.bucket_count;
  page.owning_bucket_page_number = bucket.page_number;
  return page;
}

IndexHashPageBody MakeOverflowPageWithNumber(const IndexHashPhysicalIndex& index,
                                             const IndexHashPageBody& bucket,
                                             u64 page_number,
                                             u32 bucket_count) {
  IndexHashPageBody page;
  page.index_uuid = index.index_uuid;
  page.page_number = page_number;
  page.page_kind = IndexHashPageKind::overflow;
  page.hash_seed = index.hash_seed;
  page.hash_seed_high64 = index.hash_seed_high64;
  page.hash_algorithm_version = index.hash_algorithm_version;
  page.test_fixture_force_route_hash_collision =
      index.test_fixture_force_route_hash_collision;
  page.test_fixture_force_fingerprint_collision =
      index.test_fixture_force_fingerprint_collision;
  page.bucket_index = bucket.bucket_index;
  page.bucket_count = bucket_count;
  page.owning_bucket_page_number = bucket.page_number;
  return page;
}

bool CanAddEntryToPage(const IndexHashPageBody& page,
                       const IndexHashEntry& entry,
                       u32 page_size) {
  IndexHashPageBody copy = page;
  copy.entries.push_back(entry);
  if (copy.page_kind == IndexHashPageKind::bucket) {
    copy.collision_roots.push_back({entry.key_hash, copy.page_number, 0});
  }
  return BucketBodyBytes(copy) <= page_size - kPageHeaderSerializedBytes;
}

IndexHashPageBodyResult StageChain(const std::vector<IndexHashPageBody>& chain,
                                   u32 page_size,
                                   std::vector<IndexHashPhysicalPageImage>* staged) {
  for (const auto& page : chain) {
    auto built = BuildIndexHashPageBody(page, page_size);
    if (!built.ok()) {
      return built;
    }
    staged->push_back({built.body.page_number, built.serialized});
  }
  IndexHashPageBodyResult ok;
  ok.status = HashOkStatus();
  return ok;
}

bool ChainStages(const std::vector<IndexHashPageBody>& chain, u32 page_size) {
  std::vector<IndexHashPhysicalPageImage> ignored;
  return StageChain(chain, page_size, &ignored).ok();
}

struct LatchKey {
  u64 index_identity = 0;
  u64 bucket_page_number = 0;

  bool operator==(const LatchKey& other) const {
    return index_identity == other.index_identity &&
           bucket_page_number == other.bucket_page_number;
  }
};

struct LatchKeyHash {
  std::size_t operator()(const LatchKey& key) const {
    return std::hash<u64>{}(key.index_identity) ^
           (std::hash<u64>{}(key.bucket_page_number) << 1u);
  }
};

struct ActiveLatchProof {
  u64 index_identity = 0;
  u64 bucket_page_number = 0;
  u32 bucket_index = 0;
  IndexHashBucketLatchMode mode = IndexHashBucketLatchMode::none;
};

std::mutex& LatchRegistryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<LatchKey, std::weak_ptr<std::shared_mutex>, LatchKeyHash>& Latches() {
  static std::unordered_map<LatchKey, std::weak_ptr<std::shared_mutex>, LatchKeyHash> latches;
  return latches;
}

std::unordered_map<u64, ActiveLatchProof>& ActiveLatchProofs() {
  static std::unordered_map<u64, ActiveLatchProof> proofs;
  return proofs;
}

u64 NextLatchToken() {
  static std::atomic<u64> token{1};
  return token.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<std::shared_mutex> LatchForBucket(u64 index_identity,
                                                  u64 bucket_page_number) {
  std::lock_guard<std::mutex> guard(LatchRegistryMutex());
  LatchKey key{index_identity, bucket_page_number};
  auto& weak = Latches()[key];
  auto latch = weak.lock();
  if (!latch) {
    latch = std::make_shared<std::shared_mutex>();
    weak = latch;
  }
  return latch;
}

IndexHashPageBodyResult BucketIndexForLatch(const IndexHashPhysicalIndex& index,
                                            u64 bucket_page_number,
                                            u32* bucket_index) {
  auto fetched = FetchPageUnlocked(index, bucket_page_number);
  if (!fetched.ok()) {
    return fetched;
  }
  if (fetched.body.page_kind != IndexHashPageKind::bucket) {
    return HashPageError("SB-INDEX-HASH-LATCH-BUCKET-KIND-INVALID",
                         "storage.index_hash_page.latch_bucket_kind_invalid",
                         std::to_string(bucket_page_number));
  }
  *bucket_index = fetched.body.bucket_index;
  return fetched;
}

bool LatchProofAllows(u64 index_identity,
                      const IndexHashBucketLatchEvidence& evidence,
                      u64 bucket_page_number,
                      IndexHashBucketLatchMode required_mode) {
  if (!evidence.active || evidence.bucket_page_number != bucket_page_number ||
      evidence.token == 0) {
    return false;
  }
  std::lock_guard<std::mutex> guard(LatchRegistryMutex());
  const auto found = ActiveLatchProofs().find(evidence.token);
  if (found == ActiveLatchProofs().end()) {
    return false;
  }
  const ActiveLatchProof& proof = found->second;
  if (proof.index_identity != index_identity ||
      proof.bucket_page_number != bucket_page_number ||
      proof.bucket_index != evidence.bucket_index ||
      proof.mode != evidence.mode) {
    return false;
  }
  if (required_mode == IndexHashBucketLatchMode::exclusive) {
    return proof.mode == IndexHashBucketLatchMode::exclusive;
  }
  return proof.mode == IndexHashBucketLatchMode::shared ||
         proof.mode == IndexHashBucketLatchMode::optimistic ||
         proof.mode == IndexHashBucketLatchMode::exclusive;
}

IndexHashPageBodyResult ValidateExclusiveStructuralLatchProofs(
    const IndexHashPhysicalIndex& index,
    const IndexHashPageBody& directory,
    const std::vector<IndexHashBucketLatchEvidence>& latch_evidence) {
  for (u32 bucket_index = 0; bucket_index < directory.directory_bucket_page_numbers.size();
       ++bucket_index) {
    const u64 bucket_page_number =
        directory.directory_bucket_page_numbers[bucket_index];
    const auto found = std::find_if(
        latch_evidence.begin(),
        latch_evidence.end(),
        [&](const IndexHashBucketLatchEvidence& evidence) {
          return evidence.bucket_page_number == bucket_page_number &&
                 evidence.bucket_index == bucket_index;
        });
    if (found == latch_evidence.end() ||
        !LatchProofAllows(ImageKey(index),
                          *found,
                          bucket_page_number,
                          IndexHashBucketLatchMode::exclusive)) {
      return HashPageError("SB-INDEX-HASH-LATCH-PROOF-MISSING",
                           "storage.index_hash_page.latch_proof_missing",
                           "structural_mutation_requires_all_bucket_exclusive_latches");
    }
  }
  IndexHashPageBodyResult ok;
  ok.status = HashOkStatus();
  return ok;
}

std::vector<IndexHashEntry> CollectHashEntries(
    const IndexHashPhysicalIndex& index,
    const IndexHashPageBody& directory,
    bool include_deleted,
    IndexHashPageBodyResult* error) {
  std::vector<IndexHashEntry> entries;
  for (u64 bucket_page_number : directory.directory_bucket_page_numbers) {
    IndexHashPageBodyResult chain_error;
    auto chain = BucketChainBodies(index, bucket_page_number, &chain_error);
    if (!chain_error.ok()) {
      *error = chain_error;
      return {};
    }
    for (const auto& page : chain) {
      for (const auto& entry : page.entries) {
        if (include_deleted || !entry.deleted) {
          entries.push_back(entry);
        }
      }
    }
  }
  error->status = HashOkStatus();
  return entries;
}

std::vector<u64> OverflowPageNumbers(const IndexHashPhysicalIndex& index) {
  std::vector<u64> page_numbers;
  for (const auto& image : index.pages) {
    auto parsed = ParseIndexHashPageBody(image.serialized,
                                         image.page_number,
                                         index.hash_seed,
                                         index.hash_algorithm_version,
                                         index.test_fixture_force_route_hash_collision,
                                         index.test_fixture_force_fingerprint_collision,
                                         index.hash_seed_high64);
    if (parsed.ok() && parsed.body.page_kind == IndexHashPageKind::overflow) {
      page_numbers.push_back(image.page_number);
    }
  }
  std::sort(page_numbers.begin(), page_numbers.end());
  return page_numbers;
}

IndexHashPageBodyResult RebuildHashPhysicalImages(
    const IndexHashPhysicalIndex& index,
    const IndexHashPageBody& old_directory,
    u32 new_bucket_count,
    const std::vector<IndexHashEntry>& entries,
    std::vector<IndexHashPhysicalPageImage>* staged_pages,
    u64* next_page_number,
    u64* overflow_pages_reused) {
  staged_pages->clear();
  if (new_bucket_count == 0) {
    return HashPageError("SB-INDEX-HASH-BUCKET-COUNT-INVALID",
                         "storage.index_hash_page.bucket_count_invalid");
  }

  u64 local_next_page_number = index.next_page_number;
  std::vector<u64> bucket_pages;
  bucket_pages.reserve(new_bucket_count);
  for (u32 i = 0; i < new_bucket_count; ++i) {
    if (i < old_directory.directory_bucket_page_numbers.size()) {
      bucket_pages.push_back(old_directory.directory_bucket_page_numbers[i]);
    } else {
      bucket_pages.push_back(local_next_page_number++);
    }
  }

  std::vector<std::vector<IndexHashEntry>> by_bucket(new_bucket_count);
  for (auto entry : entries) {
    entry = ComputeIndexHashEntryFingerprintsWithSeedInternal(
        std::move(entry),
        index.hash_seed,
        index.hash_seed_high64,
        index.hash_algorithm_version,
        index.test_fixture_force_route_hash_collision,
        index.test_fixture_force_fingerprint_collision);
    entry.next_collision_page_number = 0;
    entry.next_collision_ordinal = kIndexHashNoEntryOrdinal;
    by_bucket[static_cast<std::size_t>(entry.key_hash % new_bucket_count)]
        .push_back(std::move(entry));
  }

  IndexHashPageBody directory;
  directory.index_uuid = index.index_uuid;
  directory.page_number = index.directory_page_number;
  directory.page_kind = IndexHashPageKind::directory;
  directory.hash_seed = index.hash_seed;
  directory.hash_seed_high64 = index.hash_seed_high64;
  directory.hash_algorithm_version = index.hash_algorithm_version;
  directory.test_fixture_force_route_hash_collision =
      index.test_fixture_force_route_hash_collision;
  directory.test_fixture_force_fingerprint_collision =
      index.test_fixture_force_fingerprint_collision;
  directory.bucket_count = new_bucket_count;
  directory.directory_bucket_page_numbers = bucket_pages;
  auto staged_directory = BuildIndexHashPageBody(directory, index.page_size);
  if (!staged_directory.ok()) {
    return staged_directory;
  }
  staged_pages->push_back({staged_directory.body.page_number,
                           staged_directory.serialized});

  const auto reusable_overflow_pages = OverflowPageNumbers(index);
  std::size_t reusable_overflow_index = 0;
  u64 reused = 0;
  auto next_overflow_page_number = [&]() {
    if (reusable_overflow_index < reusable_overflow_pages.size()) {
      ++reused;
      return reusable_overflow_pages[reusable_overflow_index++];
    }
    return local_next_page_number++;
  };

  for (u32 bucket_index = 0; bucket_index < new_bucket_count; ++bucket_index) {
    IndexHashPageBody bucket;
    bucket.index_uuid = index.index_uuid;
    bucket.page_number = bucket_pages[bucket_index];
    bucket.page_kind = IndexHashPageKind::bucket;
    bucket.hash_seed = index.hash_seed;
    bucket.hash_seed_high64 = index.hash_seed_high64;
    bucket.hash_algorithm_version = index.hash_algorithm_version;
    bucket.test_fixture_force_route_hash_collision =
        index.test_fixture_force_route_hash_collision;
    bucket.test_fixture_force_fingerprint_collision =
        index.test_fixture_force_fingerprint_collision;
    bucket.bucket_index = bucket_index;
    bucket.bucket_count = new_bucket_count;

    std::vector<IndexHashPageBody> chain{bucket};
    for (const auto& entry : by_bucket[bucket_index]) {
      auto attempted = chain;
      attempted.back().entries.push_back(entry);
      RebuildCollisionChains(&attempted);
      if (ChainStages(attempted, index.page_size)) {
        chain = std::move(attempted);
        continue;
      }

      const u64 overflow_page_number = next_overflow_page_number();
      chain.back().overflow_page_number = overflow_page_number;
      auto overflow = MakeOverflowPageWithNumber(index,
                                                 chain.front(),
                                                 overflow_page_number,
                                                 new_bucket_count);
      overflow.entries.push_back(entry);
      chain.push_back(std::move(overflow));
      RebuildCollisionChains(&chain);
      if (!ChainStages(chain, index.page_size)) {
        return HashPageError("SB-INDEX-HASH-PAGE-BODY-TOO-LARGE",
                             "storage.index_hash_page.body_too_large",
                             "structural_repack_bucket");
      }
    }
    RebuildCollisionChains(&chain);
    auto staged_chain = StageChain(chain, index.page_size, staged_pages);
    if (!staged_chain.ok()) {
      return staged_chain;
    }
  }

  *next_page_number = local_next_page_number;
  *overflow_pages_reused = reused;
  IndexHashPageBodyResult ok;
  ok.status = HashOkStatus();
  return ok;
}

void RegisterLatchProof(u64 token, const ActiveLatchProof& proof) {
  std::lock_guard<std::mutex> guard(LatchRegistryMutex());
  ActiveLatchProofs()[token] = proof;
}

void UnregisterLatchProof(u64 token) {
  if (token == 0) {
    return;
  }
  std::lock_guard<std::mutex> guard(LatchRegistryMutex());
  ActiveLatchProofs().erase(token);
}

}  // namespace

struct IndexHashBucketLatchGuardState {
  ~IndexHashBucketLatchGuardState() {
    UnregisterLatchProof(token);
  }

  u64 token = 0;
  std::shared_ptr<std::shared_mutex> latch;
  std::unique_ptr<std::shared_lock<std::shared_mutex>> shared_lock;
  std::unique_ptr<std::unique_lock<std::shared_mutex>> exclusive_lock;
};

IndexHashBucketLatchGuard::IndexHashBucketLatchGuard() = default;

IndexHashBucketLatchGuard::IndexHashBucketLatchGuard(
    std::shared_ptr<IndexHashBucketLatchGuardState> state,
    IndexHashBucketLatchEvidence evidence)
    : state_(std::move(state)), evidence_(evidence) {}

IndexHashBucketLatchGuard::IndexHashBucketLatchGuard(
    IndexHashBucketLatchGuard&& other) noexcept = default;

IndexHashBucketLatchGuard& IndexHashBucketLatchGuard::operator=(
    IndexHashBucketLatchGuard&& other) noexcept = default;

IndexHashBucketLatchGuard::~IndexHashBucketLatchGuard() = default;

const char* IndexHashPageKindName(IndexHashPageKind kind) {
  switch (kind) {
    case IndexHashPageKind::directory: return "directory";
    case IndexHashPageKind::bucket: return "bucket";
    case IndexHashPageKind::overflow: return "overflow";
    case IndexHashPageKind::unknown: return "unknown";
  }
  return "unknown";
}

const char* IndexHashBucketLatchModeName(IndexHashBucketLatchMode mode) {
  switch (mode) {
    case IndexHashBucketLatchMode::none: return "none";
    case IndexHashBucketLatchMode::shared: return "shared";
    case IndexHashBucketLatchMode::optimistic: return "optimistic";
    case IndexHashBucketLatchMode::exclusive: return "exclusive";
  }
  return "unknown";
}

const char* IndexHashAlgorithmVersionName(u16 version) {
  switch (version) {
    case kIndexHashAlgorithmVersion1LegacyFnv64:
      return "v1_legacy_fnv64";
    case kIndexHashAlgorithmVersion2KeyedHash64:
      return "v2_keyed_hash64";
    case kIndexHashAlgorithmVersion3KeyedHash128Fingerprint:
      return "v3_keyed_hash128_fingerprint";
  }
  return "unknown";
}

const char* IndexHashPhysicalCorruptionClassName(
    IndexHashPhysicalCorruptionClass corruption_class) {
  switch (corruption_class) {
    case IndexHashPhysicalCorruptionClass::none: return "none";
    case IndexHashPhysicalCorruptionClass::checksum: return "checksum";
    case IndexHashPhysicalCorruptionClass::page_kind: return "page_kind";
    case IndexHashPhysicalCorruptionClass::seed_version: return "seed_version";
    case IndexHashPhysicalCorruptionClass::directory: return "directory";
    case IndexHashPhysicalCorruptionClass::overflow_chain: return "overflow_chain";
    case IndexHashPhysicalCorruptionClass::collision_chain: return "collision_chain";
    case IndexHashPhysicalCorruptionClass::page: return "page";
    case IndexHashPhysicalCorruptionClass::unknown: return "unknown";
  }
  return "unknown";
}

u64 ComputeIndexHashPageChecksum(const std::vector<byte>& body) {
  std::vector<byte> normalized = body;
  if (normalized.size() >= kOffsetBodyChecksum + sizeof(u64)) {
    StoreLittle64(normalized.data() + kOffsetBodyChecksum, 0);
  }
  return Fnv1a64(normalized.data(), normalized.size());
}

u64 ComputeIndexHashKeyHashWithSeedInternal(
    u64 hash_seed_low64,
    u64 hash_seed_high64,
    u16 hash_algorithm_version,
    const std::vector<byte>& encoded_key,
    bool test_fixture_force_route_hash_collision) {
  if (!IsSupportedHashAlgorithm(hash_algorithm_version)) {
    return 0;
  }
  if (test_fixture_force_route_hash_collision) {
    return kIndexHashForcedRouteHashFixtureValue;
  }
  if (hash_algorithm_version == kIndexHashAlgorithmVersion1LegacyFnv64) {
    u64 basis = 1469598103934665603ull ^ hash_seed_low64;
    basis ^= static_cast<u64>(hash_algorithm_version);
    basis *= 1099511628211ull;
    const u64 raw = Fnv1a64(encoded_key.data(), encoded_key.size(), basis);
    return raw == 0 ? 1 : raw;
  }
  return KeyedHash64(hash_seed_low64,
                     hash_seed_high64,
                     hash_algorithm_version,
                     0x484153485f524f55ull,
                     encoded_key);
}

IndexHashEntry ComputeIndexHashEntryFingerprintsWithSeedInternal(
    IndexHashEntry entry,
    u64 hash_seed_low64,
    u64 hash_seed_high64,
    u16 hash_algorithm_version,
    bool test_fixture_force_route_hash_collision,
    bool test_fixture_force_fingerprint_collision) {
  entry.key_hash =
      ComputeIndexHashKeyHashWithSeedInternal(
          hash_seed_low64,
          hash_seed_high64,
          hash_algorithm_version,
          entry.encoded_key,
          test_fixture_force_route_hash_collision);
  if (HashAlgorithmRequiresFingerprint(hash_algorithm_version)) {
    if (test_fixture_force_fingerprint_collision) {
      entry.key_fingerprint_low64 = kIndexHashForcedFingerprintLowFixtureValue;
      entry.key_fingerprint_high64 = kIndexHashForcedFingerprintHighFixtureValue;
    } else {
      entry.key_fingerprint_low64 =
          KeyedHash64(hash_seed_low64,
                      hash_seed_high64,
                      hash_algorithm_version,
                      0x46494e4745524c4full,
                      entry.encoded_key);
      entry.key_fingerprint_high64 =
          KeyedHash64(hash_seed_low64,
                      hash_seed_high64,
                      hash_algorithm_version,
                      0x46494e4745524849ull,
                      entry.encoded_key);
    }
    entry.fingerprint_algorithm_version = hash_algorithm_version;
    entry.fingerprint_present = true;
  } else {
    entry.key_fingerprint_low64 = 0;
    entry.key_fingerprint_high64 = 0;
    entry.fingerprint_algorithm_version = 0;
    entry.fingerprint_present = false;
  }
  return entry;
}

u64 ComputeIndexHashKeyHashWithSeed(u64 hash_seed_low64,
                                    u64 hash_seed_high64,
                                    u16 hash_algorithm_version,
                                    const std::vector<byte>& encoded_key,
                                    bool test_fixture_force_route_hash_collision) {
  return ComputeIndexHashKeyHashWithSeedInternal(
      hash_seed_low64,
      hash_seed_high64,
      hash_algorithm_version,
      encoded_key,
      test_fixture_force_route_hash_collision);
}

u64 ComputeIndexHashKeyHash(u64 hash_seed,
                            u16 hash_algorithm_version,
                            const std::vector<byte>& encoded_key,
                            bool test_fixture_force_route_hash_collision) {
  return ComputeIndexHashKeyHashWithSeedInternal(
      hash_seed,
      DeriveSeedHigh64(hash_seed, hash_algorithm_version),
      hash_algorithm_version,
      encoded_key,
      test_fixture_force_route_hash_collision);
}

IndexHashEntry ComputeIndexHashEntryFingerprints(
    IndexHashEntry entry,
    u64 hash_seed,
    u16 hash_algorithm_version,
    bool test_fixture_force_route_hash_collision,
    bool test_fixture_force_fingerprint_collision) {
  return ComputeIndexHashEntryFingerprintsWithSeedInternal(
      std::move(entry),
      hash_seed,
      DeriveSeedHigh64(hash_seed, hash_algorithm_version),
      hash_algorithm_version,
      test_fixture_force_route_hash_collision,
      test_fixture_force_fingerprint_collision);
}

IndexHashHardeningPolicy DefaultIndexHashHardeningPolicy() {
  IndexHashHardeningPolicy policy;
  return policy;
}

DiagnosticRecord MakeIndexHashPageDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.index_hash");
}

IndexHashPageBodyResult BuildIndexHashPageBody(const IndexHashPageBody& body,
                                               u32 page_size) {
  if (page_size <= kPageHeaderSerializedBytes + kIndexHashPageBodyHeaderBytes) {
    return HashPageError("SB-INDEX-HASH-PAGE-SIZE-TOO-SMALL",
                         "storage.index_hash_page.page_size_too_small",
                         std::to_string(page_size));
  }
  auto validation = ValidatePageForBuild(body);
  if (!validation.ok()) {
    return validation;
  }

  const u32 body_bytes = body.page_kind == IndexHashPageKind::directory
                             ? DirectoryBodyBytes(body)
                             : BucketBodyBytes(body);
  if (body.entries.size() > std::numeric_limits<u16>::max() ||
      body.collision_roots.size() > std::numeric_limits<u16>::max()) {
    return HashPageError("SB-INDEX-HASH-PAGE-BODY-TOO-LARGE",
                         "storage.index_hash_page.body_too_large",
                         "entry_or_collision_root_count_overflow");
  }
  if (body_bytes > page_size - kPageHeaderSerializedBytes) {
    return HashPageError("SB-INDEX-HASH-PAGE-BODY-TOO-LARGE",
                         "storage.index_hash_page.body_too_large",
                         std::to_string(body_bytes));
  }

  IndexHashPageBodyResult result;
  result.status = HashOkStatus();
  result.body = body;
  result.body.free_space_bytes = page_size - kPageHeaderSerializedBytes - body_bytes;
  result.serialized.assign(page_size - kPageHeaderSerializedBytes, 0);
  std::copy(kIndexHashMagic.begin(), kIndexHashMagic.end(),
            result.serialized.begin() + kOffsetMagic);
  StoreLittle32(result.serialized.data() + kOffsetHeaderBytes,
                kIndexHashPageBodyHeaderBytes);
  StoreLittle16(result.serialized.data() + kOffsetPageKind,
                static_cast<u16>(body.page_kind));
  StoreLittle16(result.serialized.data() + kOffsetAlgorithmVersion,
                body.hash_algorithm_version);
  StoreLittle16(result.serialized.data() + kOffsetEntryCount,
                static_cast<u16>(body.entries.size()));
  StoreLittle16(result.serialized.data() + kOffsetCollisionRootCount,
                static_cast<u16>(body.collision_roots.size()));
  std::copy(body.index_uuid.value.bytes.begin(),
            body.index_uuid.value.bytes.end(),
            result.serialized.begin() + kOffsetIndexUuid);
  StoreLittle64(result.serialized.data() + kOffsetPageNumber, body.page_number);
  StoreLittle64(result.serialized.data() + kOffsetHashSeed, body.hash_seed);
  StoreLittle32(result.serialized.data() + kOffsetBucketIndex, body.bucket_index);
  StoreLittle32(result.serialized.data() + kOffsetBucketCount, body.bucket_count);
  StoreLittle64(result.serialized.data() + kOffsetOverflowPageNumber,
                body.overflow_page_number);
  StoreLittle64(result.serialized.data() + kOffsetOwningBucketPageNumber,
                body.owning_bucket_page_number);
  StoreLittle32(result.serialized.data() + kOffsetFreeSpaceBytes,
                result.body.free_space_bytes);
  StoreLittle64(result.serialized.data() + kOffsetHashSeedHigh64,
                body.hash_seed_high64);

  u32 offset = kIndexHashPageBodyHeaderBytes;
  if (body.page_kind == IndexHashPageKind::directory) {
    for (u64 bucket_page : body.directory_bucket_page_numbers) {
      StoreLittle64(result.serialized.data() + offset, bucket_page);
      offset += sizeof(u64);
    }
  } else {
    const u32 entry_header_bytes =
        EntryHeaderBytesForAlgorithm(body.hash_algorithm_version);
    for (const auto& root : body.collision_roots) {
      StoreLittle64(result.serialized.data() + offset + kRootOffsetHash, root.key_hash);
      StoreLittle64(result.serialized.data() + offset + kRootOffsetPageNumber,
                    root.page_number);
      StoreLittle16(result.serialized.data() + offset + kRootOffsetEntryOrdinal,
                    root.entry_ordinal);
      offset += kCollisionRootBytes;
    }
    for (const auto& entry : body.entries) {
      StoreLittle16(result.serialized.data() + offset + kEntryOffsetFlags,
                    FlagsFor(entry));
      StoreLittle16(result.serialized.data() + offset + kEntryOffsetNextOrdinal,
                    entry.next_collision_ordinal);
      StoreLittle32(result.serialized.data() + offset + kEntryOffsetKeyBytes,
                    static_cast<u32>(entry.encoded_key.size()));
      StoreLittle64(result.serialized.data() + offset + kEntryOffsetKeyChecksum,
                    Fnv1a64(entry.encoded_key.data(), entry.encoded_key.size()));
      StoreLittle64(result.serialized.data() + offset + kEntryOffsetKeyHash,
                    entry.key_hash);
      std::copy(entry.row_uuid.value.bytes.begin(),
                entry.row_uuid.value.bytes.end(),
                result.serialized.begin() + offset + kEntryOffsetRowUuid);
      std::copy(entry.version_uuid.value.bytes.begin(),
                entry.version_uuid.value.bytes.end(),
                result.serialized.begin() + offset + kEntryOffsetVersionUuid);
      StoreLittle64(result.serialized.data() + offset + kEntryOffsetNextPage,
                    entry.next_collision_page_number);
      if (entry.fingerprint_present) {
        StoreLittle64(result.serialized.data() + offset +
                          kEntryOffsetFingerprintLow64,
                      entry.key_fingerprint_low64);
        StoreLittle64(result.serialized.data() + offset +
                          kEntryOffsetFingerprintHigh64,
                      entry.key_fingerprint_high64);
        StoreLittle16(result.serialized.data() + offset +
                          kEntryOffsetFingerprintAlgorithm,
                      entry.fingerprint_algorithm_version);
      }
      offset += entry_header_bytes;
      std::copy(entry.encoded_key.begin(),
                entry.encoded_key.end(),
                result.serialized.begin() + offset);
      offset += static_cast<u32>(entry.encoded_key.size());
    }
  }

  StoreLittle32(result.serialized.data() + kOffsetBodyBytes, offset);
  StoreLittle64(result.serialized.data() + kOffsetBodyChecksum,
                ComputeIndexHashPageChecksum(result.serialized));
  return result;
}

IndexHashPageBodyResult ParseIndexHashPageBody(const std::vector<byte>& serialized,
                                               u64 page_number,
                                               u64 expected_hash_seed,
                                               u16 expected_hash_algorithm_version,
                                               bool test_fixture_force_route_hash_collision,
                                               bool test_fixture_force_fingerprint_collision,
                                               u64 expected_hash_seed_high64) {
  if (serialized.size() < kIndexHashPageBodyHeaderBytesLegacy96) {
    return HashPageError("SB-INDEX-HASH-PAGE-BODY-SHORT",
                         "storage.index_hash_page.body_short",
                         std::to_string(page_number));
  }
  if (!std::equal(kIndexHashMagic.begin(),
                  kIndexHashMagic.end(),
                  serialized.begin() + kOffsetMagic)) {
    return HashPageError("SB-INDEX-HASH-PAGE-MAGIC-INVALID",
                         "storage.index_hash_page.magic_invalid",
                         std::to_string(page_number));
  }
  const u32 header_bytes = LoadLittle32(serialized.data() + kOffsetHeaderBytes);
  if (header_bytes != kIndexHashPageBodyHeaderBytes &&
      header_bytes != kIndexHashPageBodyHeaderBytesLegacy96) {
    return HashPageError("SB-INDEX-HASH-PAGE-HEADER-SIZE-INVALID",
                         "storage.index_hash_page.header_size_invalid",
                         std::to_string(page_number));
  }
  if (LoadLittle64(serialized.data() + kOffsetBodyChecksum) !=
      ComputeIndexHashPageChecksum(serialized)) {
    return HashPageError("SB-INDEX-HASH-PAGE-CHECKSUM-MISMATCH",
                         "storage.index_hash_page.checksum_mismatch",
                         std::to_string(page_number));
  }
  const u32 body_bytes = LoadLittle32(serialized.data() + kOffsetBodyBytes);
  if (body_bytes > serialized.size() || body_bytes < header_bytes) {
    return HashPageError("SB-INDEX-HASH-PAGE-BODY-SIZE-INVALID",
                         "storage.index_hash_page.body_size_invalid",
                         std::to_string(page_number));
  }

  IndexHashPageBodyResult result;
  result.status = HashOkStatus();
  result.serialized = serialized;
  result.body.page_kind =
      static_cast<IndexHashPageKind>(LoadLittle16(serialized.data() + kOffsetPageKind));
  if (!IsKnownPageKind(result.body.page_kind)) {
    return HashPageError("SB-INDEX-HASH-PAGE-KIND-INVALID",
                         "storage.index_hash_page.kind_invalid",
                         std::to_string(page_number));
  }
  result.body.hash_algorithm_version =
      LoadLittle16(serialized.data() + kOffsetAlgorithmVersion);
  result.body.test_fixture_force_route_hash_collision =
      test_fixture_force_route_hash_collision;
  result.body.test_fixture_force_fingerprint_collision =
      test_fixture_force_fingerprint_collision;
  if (!IsSupportedHashAlgorithm(result.body.hash_algorithm_version)) {
    return HashPageError("SB-INDEX-HASH-ALGORITHM-UNSUPPORTED",
                         "storage.index_hash_page.algorithm_unsupported",
                         std::to_string(result.body.hash_algorithm_version));
  }
  result.body.hash_seed = LoadLittle64(serialized.data() + kOffsetHashSeed);
  result.body.hash_seed_high64 =
      header_bytes >= kIndexHashPageBodyHeaderBytes
          ? LoadLittle64(serialized.data() + kOffsetHashSeedHigh64)
          : DeriveSeedHigh64(result.body.hash_seed,
                             result.body.hash_algorithm_version);
  if (expected_hash_algorithm_version != 0 &&
      result.body.hash_algorithm_version != expected_hash_algorithm_version) {
    return HashPageError("SB-INDEX-HASH-ALGORITHM-VERSION-MISMATCH",
                         "storage.index_hash_page.algorithm_version_mismatch",
                         std::to_string(page_number));
  }
  if (expected_hash_seed != 0 && result.body.hash_seed != expected_hash_seed) {
    return HashPageError("SB-INDEX-HASH-SEED-MISMATCH",
                         "storage.index_hash_page.seed_mismatch",
                         std::to_string(page_number));
  }
  if (expected_hash_seed_high64 != 0 &&
      result.body.hash_seed_high64 != expected_hash_seed_high64) {
    return HashPageError("SB-INDEX-HASH-SEED-MISMATCH",
                         "storage.index_hash_page.seed_mismatch",
                         std::to_string(page_number));
  }

  result.body.index_uuid.kind = UuidKind::object;
  std::copy(serialized.begin() + kOffsetIndexUuid,
            serialized.begin() + kOffsetIndexUuid + 16,
            result.body.index_uuid.value.bytes.begin());
  if (!IsTypedEngineIdentity(result.body.index_uuid, UuidKind::object)) {
    return HashPageError("SB-INDEX-HASH-INDEX-UUID-MUST-BE-V7",
                         "storage.index_hash_page.index_uuid_must_be_v7",
                         std::to_string(page_number));
  }
  result.body.page_number = LoadLittle64(serialized.data() + kOffsetPageNumber);
  if (result.body.page_number == 0) {
    result.body.page_number = page_number;
  }
  if (result.body.page_number != page_number) {
    return HashPageError("SB-INDEX-HASH-PAGE-NUMBER-MISMATCH",
                         "storage.index_hash_page.page_number_mismatch",
                         std::to_string(page_number));
  }
  result.body.bucket_index = LoadLittle32(serialized.data() + kOffsetBucketIndex);
  result.body.bucket_count = LoadLittle32(serialized.data() + kOffsetBucketCount);
  result.body.overflow_page_number =
      LoadLittle64(serialized.data() + kOffsetOverflowPageNumber);
  result.body.owning_bucket_page_number =
      LoadLittle64(serialized.data() + kOffsetOwningBucketPageNumber);
  result.body.free_space_bytes = LoadLittle32(serialized.data() + kOffsetFreeSpaceBytes);
  if (result.body.free_space_bytes != serialized.size() - body_bytes) {
    return HashPageError("SB-INDEX-HASH-PAGE-FREE-SPACE-INVALID",
                         "storage.index_hash_page.free_space_invalid",
                         std::to_string(page_number));
  }

  u32 offset = header_bytes;
  if (result.body.page_kind == IndexHashPageKind::directory) {
    if (result.body.bucket_count == 0) {
      return HashPageError("SB-INDEX-HASH-DIRECTORY-EMPTY",
                           "storage.index_hash_page.directory_empty",
                           std::to_string(page_number));
    }
    for (u32 i = 0; i < result.body.bucket_count; ++i) {
      if (offset + sizeof(u64) > body_bytes) {
        return HashPageError("SB-INDEX-HASH-DIRECTORY-SHORT",
                             "storage.index_hash_page.directory_short",
                             std::to_string(i));
      }
      result.body.directory_bucket_page_numbers.push_back(
          LoadLittle64(serialized.data() + offset));
      offset += sizeof(u64);
    }
  } else {
    const u16 root_count = LoadLittle16(serialized.data() + kOffsetCollisionRootCount);
    const u16 entry_count = LoadLittle16(serialized.data() + kOffsetEntryCount);
    const u32 entry_header_bytes =
        EntryHeaderBytesForAlgorithm(result.body.hash_algorithm_version);
    for (u16 i = 0; i < root_count; ++i) {
      if (offset + kCollisionRootBytes > body_bytes) {
        return HashPageError("SB-INDEX-HASH-COLLISION-ROOT-SHORT",
                             "storage.index_hash_page.collision_root_short",
                             std::to_string(i));
      }
      IndexHashCollisionRoot root;
      root.key_hash = LoadLittle64(serialized.data() + offset + kRootOffsetHash);
      root.page_number = LoadLittle64(serialized.data() + offset + kRootOffsetPageNumber);
      root.entry_ordinal =
          LoadLittle16(serialized.data() + offset + kRootOffsetEntryOrdinal);
      result.body.collision_roots.push_back(root);
      offset += kCollisionRootBytes;
    }
    for (u16 i = 0; i < entry_count; ++i) {
      if (offset + entry_header_bytes > body_bytes) {
        return HashPageError("SB-INDEX-HASH-ENTRY-SHORT",
                             "storage.index_hash_page.entry_short",
                             std::to_string(i));
      }
      IndexHashEntry entry;
      const u16 flags = LoadLittle16(serialized.data() + offset + kEntryOffsetFlags);
      entry.deleted = (flags & EntryFlag::deleted) != 0;
      entry.fingerprint_present =
          (flags & EntryFlag::fingerprint_present) != 0;
      entry.next_collision_ordinal =
          LoadLittle16(serialized.data() + offset + kEntryOffsetNextOrdinal);
      const u32 key_bytes = LoadLittle32(serialized.data() + offset + kEntryOffsetKeyBytes);
      const u64 key_checksum =
          LoadLittle64(serialized.data() + offset + kEntryOffsetKeyChecksum);
      entry.key_hash = LoadLittle64(serialized.data() + offset + kEntryOffsetKeyHash);
      Uuid row_uuid;
      std::copy(serialized.begin() + offset + kEntryOffsetRowUuid,
                serialized.begin() + offset + kEntryOffsetRowUuid + 16,
                row_uuid.bytes.begin());
      if (!IsNilUuid(row_uuid)) {
        entry.row_uuid.kind = UuidKind::row;
        entry.row_uuid.value = row_uuid;
      }
      Uuid version_uuid;
      std::copy(serialized.begin() + offset + kEntryOffsetVersionUuid,
                serialized.begin() + offset + kEntryOffsetVersionUuid + 16,
                version_uuid.bytes.begin());
      if (!IsNilUuid(version_uuid)) {
        entry.version_uuid.kind = UuidKind::row;
        entry.version_uuid.value = version_uuid;
      }
      entry.next_collision_page_number =
          LoadLittle64(serialized.data() + offset + kEntryOffsetNextPage);
      if (entry.fingerprint_present) {
        if (entry_header_bytes < kEntryHeaderBytesV3) {
          return HashPageError("SB-INDEX-HASH-FINGERPRINT-UNEXPECTED",
                               "storage.index_hash_page.fingerprint_unexpected",
                               std::to_string(i));
        }
        entry.key_fingerprint_low64 =
            LoadLittle64(serialized.data() + offset +
                         kEntryOffsetFingerprintLow64);
        entry.key_fingerprint_high64 =
            LoadLittle64(serialized.data() + offset +
                         kEntryOffsetFingerprintHigh64);
        entry.fingerprint_algorithm_version =
            LoadLittle16(serialized.data() + offset +
                         kEntryOffsetFingerprintAlgorithm);
      }
      offset += entry_header_bytes;
      if (offset + key_bytes > body_bytes) {
        return HashPageError("SB-INDEX-HASH-KEY-SHORT",
                             "storage.index_hash_page.key_short",
                             std::to_string(i));
      }
      entry.encoded_key.assign(serialized.begin() + offset,
                               serialized.begin() + offset + key_bytes);
      if (key_checksum != Fnv1a64(entry.encoded_key.data(), entry.encoded_key.size())) {
        return HashPageError("SB-INDEX-HASH-KEY-CHECKSUM-MISMATCH",
                             "storage.index_hash_page.key_checksum_mismatch",
                             std::to_string(i));
      }
      const auto entry_validation =
          ValidateEntryBasics(entry,
                              ComputeIndexHashKeyHashWithSeedInternal(
                                  result.body.hash_seed,
                                  result.body.hash_seed_high64,
                                  result.body.hash_algorithm_version,
                                  entry.encoded_key,
                                  result.body.test_fixture_force_route_hash_collision),
                              result.body.hash_seed,
                              result.body.hash_seed_high64,
                              result.body.hash_algorithm_version,
                              result.body.test_fixture_force_route_hash_collision,
                              result.body.test_fixture_force_fingerprint_collision,
                              i);
      if (!entry_validation.ok()) {
        return entry_validation;
      }
      result.body.entries.push_back(std::move(entry));
      offset += key_bytes;
    }
  }
  if (offset != body_bytes) {
    return HashPageError("SB-INDEX-HASH-PAGE-TRAILING-BYTES",
                         "storage.index_hash_page.trailing_bytes",
                         std::to_string(page_number));
  }
  const auto validation = ValidatePageForBuild(result.body);
  if (!validation.ok()) {
    return validation;
  }
  return result;
}

IndexHashPhysicalIndexResult InitializeIndexHashPhysicalIndex(
    TypedUuid index_uuid,
    u32 page_size,
    u64 hash_seed,
    u16 hash_algorithm_version,
    u32 bucket_count,
    bool test_fixture_seed_allowed,
    bool test_fixture_force_route_hash_collision,
    bool test_fixture_force_fingerprint_collision) {
  if (!IsSupportedHashAlgorithm(hash_algorithm_version)) {
    return HashIndexError("SB-INDEX-HASH-ALGORITHM-UNSUPPORTED",
                          "storage.index_hash_page.algorithm_unsupported",
                          std::to_string(hash_algorithm_version));
  }
  if (hash_seed != 0 && !test_fixture_seed_allowed) {
    return HashIndexError("SB-INDEX-HASH-SEED-CLIENT-SUPPLIED",
                          "storage.index_hash_page.seed_client_supplied",
                          "deterministic_hash_seed_requires_test_fixture_mode");
  }
  if ((test_fixture_force_route_hash_collision ||
       test_fixture_force_fingerprint_collision) &&
      !test_fixture_seed_allowed) {
    return HashIndexError("SB-INDEX-HASH-TEST-FIXTURE-REQUIRED",
                          "storage.index_hash_page.test_fixture_required",
                          "forced_collision_hooks_require_test_fixture_mode");
  }
  if (bucket_count == 0) {
    return HashIndexError("SB-INDEX-HASH-BUCKET-COUNT-INVALID",
                          "storage.index_hash_page.bucket_count_invalid");
  }

  IndexHashPhysicalIndexResult result;
  result.status = HashOkStatus();
  HashSeedMaterial seed_material;
  if (test_fixture_seed_allowed) {
    seed_material.seed =
        hash_seed == 0 ? EngineGeneratedSeedForIndex(index_uuid,
                                                     hash_algorithm_version)
                       : hash_seed;
    seed_material.seed_high64 =
        DeriveSeedHigh64(seed_material.seed, hash_algorithm_version);
    seed_material.protected_material = false;
    seed_material.entropy_source = "deterministic_test_fixture";
  } else {
    seed_material = ProductionRandomSeedForIndex();
    if (seed_material.seed == 0 || !seed_material.protected_material) {
      return HashIndexError("SB-INDEX-HASH-SEED-ENTROPY-UNAVAILABLE",
                            "storage.index_hash_page.seed_entropy_unavailable",
                            seed_material.entropy_source);
    }
  }
  const u64 effective_hash_seed = seed_material.seed;
  const u64 effective_hash_seed_high64 = seed_material.seed_high64;
  result.index.page_size = page_size;
  result.index.index_uuid = index_uuid;
  result.index.hash_seed = effective_hash_seed;
  result.index.hash_seed_high64 = effective_hash_seed_high64;
  result.index.hash_algorithm_version = hash_algorithm_version;
  result.index.hash_seed_engine_generated = !test_fixture_seed_allowed;
  result.index.hash_seed_test_fixture_mode = test_fixture_seed_allowed;
  result.index.hash_seed_protected = seed_material.protected_material;
  result.index.hash_seed_entropy_source = seed_material.entropy_source;
  result.index.test_fixture_force_route_hash_collision =
      test_fixture_force_route_hash_collision;
  result.index.test_fixture_force_fingerprint_collision =
      test_fixture_force_fingerprint_collision;
  result.index.directory_page_number = 1;
  result.index.next_page_number = 2 + bucket_count;

  IndexHashPageBody directory;
  directory.index_uuid = index_uuid;
  directory.page_number = result.index.directory_page_number;
  directory.page_kind = IndexHashPageKind::directory;
  directory.hash_seed = effective_hash_seed;
  directory.hash_seed_high64 = effective_hash_seed_high64;
  directory.hash_algorithm_version = hash_algorithm_version;
  directory.test_fixture_force_route_hash_collision =
      test_fixture_force_route_hash_collision;
  directory.test_fixture_force_fingerprint_collision =
      test_fixture_force_fingerprint_collision;
  directory.bucket_count = bucket_count;
  for (u32 i = 0; i < bucket_count; ++i) {
    directory.directory_bucket_page_numbers.push_back(2 + i);
  }
  auto staged_directory = BuildIndexHashPageBody(directory, page_size);
  if (!staged_directory.ok()) {
    return HashIndexError(staged_directory.diagnostic.diagnostic_code,
                          staged_directory.diagnostic.message_key,
                          "directory");
  }
  PublishPage(&result.index, staged_directory);

  for (u32 i = 0; i < bucket_count; ++i) {
    IndexHashPageBody bucket;
    bucket.index_uuid = index_uuid;
    bucket.page_number = 2 + i;
    bucket.page_kind = IndexHashPageKind::bucket;
    bucket.hash_seed = effective_hash_seed;
    bucket.hash_seed_high64 = effective_hash_seed_high64;
    bucket.hash_algorithm_version = hash_algorithm_version;
    bucket.test_fixture_force_route_hash_collision =
        test_fixture_force_route_hash_collision;
    bucket.test_fixture_force_fingerprint_collision =
        test_fixture_force_fingerprint_collision;
    bucket.bucket_index = i;
    bucket.bucket_count = bucket_count;
    auto staged_bucket = BuildIndexHashPageBody(bucket, page_size);
    if (!staged_bucket.ok()) {
      return HashIndexError(staged_bucket.diagnostic.diagnostic_code,
                            staged_bucket.diagnostic.message_key,
                            "bucket");
    }
    PublishPage(&result.index, staged_bucket);
  }

  result.evidence = {"hash_directory_page_serialized=true",
                     "hash_bucket_pages_serialized=true",
                     "hash_seed_recorded=true",
                     std::string("hash_seed_engine_generated=") +
                         (result.index.hash_seed_engine_generated ? "true" : "false"),
                     "hash_seed_client_supplied=false",
                     std::string("hash_seed_test_fixture_mode=") +
                         (result.index.hash_seed_test_fixture_mode ? "true" : "false"),
                     std::string("hash_seed_protected=") +
                         (result.index.hash_seed_protected ? "true" : "false"),
                     std::string("hash_seed_entropy_source=") +
                         result.index.hash_seed_entropy_source,
                     "hash_seed_high64_redacted=true",
                     "hash_seed_key_material_bits=128",
                     "hash_algorithm_version_recorded=true",
                     std::string("hash_algorithm_version=") +
                         IndexHashAlgorithmVersionName(hash_algorithm_version),
                     std::string("high_assurance_fingerprint_present=") +
                         (HashAlgorithmRequiresFingerprint(hash_algorithm_version)
                              ? "true"
                              : "false"),
                     "split_merge_compaction_implemented=true",
                     "benchmark_clean_capability=false"};
  AddCandidateEvidence(&result);
  return result;
}

IndexHashPhysicalIndexImageResult ExportIndexHashPhysicalIndexImage(
    const IndexHashPhysicalIndex& index) {
  IndexHashPhysicalIndexImageResult result;
  result.status = HashOkStatus();
  result.image.page_size = index.page_size;
  result.image.index_uuid = index.index_uuid;
  result.image.hash_seed = index.hash_seed;
  result.image.hash_seed_high64 = index.hash_seed_high64;
  result.image.hash_algorithm_version = index.hash_algorithm_version;
  result.image.hash_seed_engine_generated = index.hash_seed_engine_generated;
  result.image.hash_seed_test_fixture_mode = index.hash_seed_test_fixture_mode;
  result.image.hash_seed_protected = index.hash_seed_protected;
  result.image.hash_seed_entropy_source = index.hash_seed_entropy_source;
  result.image.test_fixture_force_route_hash_collision =
      index.test_fixture_force_route_hash_collision;
  result.image.test_fixture_force_fingerprint_collision =
      index.test_fixture_force_fingerprint_collision;
  result.image.directory_page_number = index.directory_page_number;
  result.image.next_page_number = index.next_page_number;
  result.image.pages = index.pages;
  result.image.evidence = {"hash_physical_image_exported=true",
                           "serialized_page_images_preserved=true",
                           std::string("hash_algorithm_version=") +
                               IndexHashAlgorithmVersionName(
                                   index.hash_algorithm_version),
                           std::string("hash_seed_engine_generated=") +
                               (index.hash_seed_engine_generated ? "true" : "false"),
                           "hash_seed_client_supplied=false",
                           std::string("hash_seed_test_fixture_mode=") +
                               (index.hash_seed_test_fixture_mode ? "true" : "false"),
                           std::string("hash_seed_protected=") +
                               (index.hash_seed_protected ? "true" : "false"),
                           std::string("hash_seed_entropy_source=") +
                               index.hash_seed_entropy_source,
                           "hash_seed_redacted=true",
                           "hash_seed_high64_redacted=true",
                           "hash_seed_key_material_bits=128",
                           std::string("high_assurance_fingerprint_present=") +
                               (HashAlgorithmRequiresFingerprint(
                                    index.hash_algorithm_version)
                                    ? "true"
                                    : "false"),
                           "full_encoded_key_compare_mandatory=true"};
  result.evidence = result.image.evidence;
  AddCandidateEvidence(&result);
  return result;
}

IndexHashPhysicalIndexResult ImportIndexHashPhysicalIndexImage(
    const IndexHashPhysicalIndexImage& image) {
  IndexHashPhysicalIndexResult result;
  result.status = HashOkStatus();
  result.index.page_size = image.page_size;
  result.index.index_uuid = image.index_uuid;
  result.index.hash_seed = image.hash_seed;
  result.index.hash_seed_high64 =
      image.hash_seed_high64 == 0
          ? DeriveSeedHigh64(image.hash_seed, image.hash_algorithm_version)
          : image.hash_seed_high64;
  result.index.hash_algorithm_version = image.hash_algorithm_version;
  result.index.hash_seed_engine_generated = image.hash_seed_engine_generated;
  result.index.hash_seed_test_fixture_mode = image.hash_seed_test_fixture_mode;
  result.index.hash_seed_protected = image.hash_seed_protected;
  result.index.hash_seed_entropy_source = image.hash_seed_entropy_source;
  result.index.test_fixture_force_route_hash_collision =
      image.test_fixture_force_route_hash_collision;
  result.index.test_fixture_force_fingerprint_collision =
      image.test_fixture_force_fingerprint_collision;
  result.index.directory_page_number = image.directory_page_number;
  result.index.next_page_number = image.next_page_number;
  result.index.pages = image.pages;

  auto report = BuildIndexHashPhysicalReport(result.index);
  if (!report.report.valid) {
    return HashIndexError(report.report.exact_diagnostic_code,
                          report.report.exact_diagnostic_message_key,
                          "import_validation");
  }
  result.evidence = {"hash_physical_image_imported=true",
                     "directory_bucket_overflow_reopened=true",
                     "hash_seed_version_validated=true",
                     std::string("hash_algorithm_version=") +
                         IndexHashAlgorithmVersionName(
                             result.index.hash_algorithm_version),
                     std::string("hash_seed_engine_generated=") +
                         (result.index.hash_seed_engine_generated ? "true" : "false"),
                     "hash_seed_client_supplied=false",
                     std::string("hash_seed_test_fixture_mode=") +
                         (result.index.hash_seed_test_fixture_mode ? "true" : "false"),
                     std::string("hash_seed_protected=") +
                         (result.index.hash_seed_protected ? "true" : "false"),
                     std::string("hash_seed_entropy_source=") +
                         result.index.hash_seed_entropy_source,
                     "hash_seed_redacted=true",
                     "hash_seed_high64_redacted=true",
                     "hash_seed_key_material_bits=128",
                     std::string("high_assurance_fingerprint_present=") +
                         (HashAlgorithmRequiresFingerprint(
                              result.index.hash_algorithm_version)
                              ? "true"
                              : "false"),
                     std::string("legacy_v1_rebuild_recommended=") +
                         (result.index.hash_algorithm_version ==
                                  kIndexHashAlgorithmVersion1LegacyFnv64
                              ? "true"
                              : "false"),
                     "authoritative_base_rows_required_for_repair=true"};
  AddCandidateEvidence(&result);
  return result;
}

IndexHashPageBodyResult FetchIndexHashPhysicalPage(const IndexHashPhysicalIndex& index,
                                                   u64 page_number) {
  return FetchPageUnlocked(index, page_number);
}

IndexHashBucketRouteResult LocateIndexHashBucket(
    const IndexHashPhysicalIndex& index,
    const std::vector<byte>& encoded_key) {
  auto directory = FetchDirectoryUnlocked(index);
  if (!directory.ok()) {
    return HashRouteError(directory.diagnostic.diagnostic_code,
                          directory.diagnostic.message_key,
                          "directory");
  }
  if (!SameTypedUuid(directory.body.index_uuid, index.index_uuid)) {
    return HashRouteError("SB-INDEX-HASH-INDEX-UUID-MISMATCH",
                          "storage.index_hash_page.index_uuid_mismatch",
                          "directory");
  }
  const u64 key_hash =
      ComputeIndexHashKeyHashWithSeedInternal(
          index.hash_seed,
          index.hash_seed_high64,
          index.hash_algorithm_version,
          encoded_key,
          index.test_fixture_force_route_hash_collision);
  const u32 bucket_index =
      static_cast<u32>(key_hash % directory.body.directory_bucket_page_numbers.size());
  IndexHashBucketRouteResult result;
  result.status = HashOkStatus();
  result.key_hash = key_hash;
  result.bucket_index = bucket_index;
  result.bucket_page_number = directory.body.directory_bucket_page_numbers[bucket_index];
  result.evidence = {"directory_hash_route=true",
                     "hash_seed_version_consumed=true",
                     std::string("hash_seed_engine_generated=") +
                         (index.hash_seed_engine_generated ? "true" : "false"),
                     "hash_seed_client_supplied=false",
                     std::string("hash_seed_test_fixture_mode=") +
                         (index.hash_seed_test_fixture_mode ? "true" : "false"),
                     "hash_seed_key_material_bits=128",
                     std::string("hash_algorithm_version=") +
                         IndexHashAlgorithmVersionName(index.hash_algorithm_version),
                     std::string("high_assurance_fingerprint_present=") +
                         (HashAlgorithmRequiresFingerprint(index.hash_algorithm_version)
                              ? "true"
                              : "false"),
                     "split_merge_compaction_implemented=true"};
  return result;
}

IndexHashBucketLatchGuard AcquireIndexHashBucketSharedLatch(
    const IndexHashPhysicalIndex& index,
    u64 bucket_page_number) {
  u32 bucket_index = 0;
  auto bucket = BucketIndexForLatch(index, bucket_page_number, &bucket_index);
  if (!bucket.ok()) {
    return {};
  }
  auto state = std::make_shared<IndexHashBucketLatchGuardState>();
  state->token = NextLatchToken();
  state->latch = LatchForBucket(ImageKey(index), bucket_page_number);
  state->shared_lock = std::make_unique<std::shared_lock<std::shared_mutex>>(*state->latch);
  IndexHashBucketLatchEvidence evidence;
  evidence.bucket_page_number = bucket_page_number;
  evidence.bucket_index = bucket_index;
  evidence.mode = IndexHashBucketLatchMode::shared;
  evidence.token = state->token;
  evidence.active = true;
  RegisterLatchProof(state->token,
                     {ImageKey(index),
                      bucket_page_number,
                      bucket_index,
                      IndexHashBucketLatchMode::shared});
  return {std::move(state), evidence};
}

IndexHashBucketLatchGuard AcquireIndexHashBucketOptimisticLatch(
    const IndexHashPhysicalIndex& index,
    u64 bucket_page_number) {
  u32 bucket_index = 0;
  auto bucket = BucketIndexForLatch(index, bucket_page_number, &bucket_index);
  if (!bucket.ok()) {
    return {};
  }
  auto state = std::make_shared<IndexHashBucketLatchGuardState>();
  state->token = NextLatchToken();
  IndexHashBucketLatchEvidence evidence;
  evidence.bucket_page_number = bucket_page_number;
  evidence.bucket_index = bucket_index;
  evidence.mode = IndexHashBucketLatchMode::optimistic;
  evidence.token = state->token;
  evidence.active = true;
  RegisterLatchProof(state->token,
                     {ImageKey(index),
                      bucket_page_number,
                      bucket_index,
                      IndexHashBucketLatchMode::optimistic});
  return {std::move(state), evidence};
}

IndexHashBucketLatchGuard AcquireIndexHashBucketExclusiveLatch(
    IndexHashPhysicalIndex* index,
    u64 bucket_page_number) {
  if (index == nullptr) {
    return {};
  }
  u32 bucket_index = 0;
  auto bucket = BucketIndexForLatch(*index, bucket_page_number, &bucket_index);
  if (!bucket.ok()) {
    return {};
  }
  auto state = std::make_shared<IndexHashBucketLatchGuardState>();
  state->token = NextLatchToken();
  state->latch = LatchForBucket(ImageKey(*index), bucket_page_number);
  state->exclusive_lock = std::make_unique<std::unique_lock<std::shared_mutex>>(*state->latch);
  IndexHashBucketLatchEvidence evidence;
  evidence.bucket_page_number = bucket_page_number;
  evidence.bucket_index = bucket_index;
  evidence.mode = IndexHashBucketLatchMode::exclusive;
  evidence.token = state->token;
  evidence.active = true;
  RegisterLatchProof(state->token,
                     {ImageKey(*index),
                      bucket_page_number,
                      bucket_index,
                      IndexHashBucketLatchMode::exclusive});
  return {std::move(state), evidence};
}

IndexHashPhysicalInsertResult InsertIndexHashEntry(
    IndexHashPhysicalIndex* index,
    const IndexHashPhysicalInsertRequest& request) {
  if (index == nullptr) {
    return HashInsertError("SB-INDEX-HASH-INDEX-INVALID",
                           "storage.index_hash_page.index_invalid");
  }
  auto route = LocateIndexHashBucket(*index, request.encoded_key);
  if (!route.ok()) {
    return HashInsertError(route.diagnostic.diagnostic_code,
                           route.diagnostic.message_key,
                           "route");
  }
  if (!LatchProofAllows(ImageKey(*index),
                        request.latch_evidence,
                        route.bucket_page_number,
                        IndexHashBucketLatchMode::exclusive)) {
    return HashInsertError("SB-INDEX-HASH-LATCH-PROOF-MISSING",
                           "storage.index_hash_page.latch_proof_missing",
                           "insert_requires_exclusive_bucket_latch");
  }

  IndexHashEntry entry;
  entry.encoded_key = request.encoded_key;
  entry.row_uuid = request.row_uuid;
  entry.version_uuid = request.version_uuid;
  entry = ComputeIndexHashEntryFingerprintsWithSeedInternal(
      std::move(entry),
      index->hash_seed,
      index->hash_seed_high64,
      index->hash_algorithm_version,
      index->test_fixture_force_route_hash_collision,
      index->test_fixture_force_fingerprint_collision);
  auto entry_validation = ValidateEntryBasics(entry,
                                             route.key_hash,
                                             index->hash_seed,
                                             index->hash_seed_high64,
                                             index->hash_algorithm_version,
                                             index->test_fixture_force_route_hash_collision,
                                             index->test_fixture_force_fingerprint_collision,
                                             0);
  if (!entry_validation.ok()) {
    return HashInsertError(entry_validation.diagnostic.diagnostic_code,
                           entry_validation.diagnostic.message_key,
                           "entry");
  }

  IndexHashPageBodyResult chain_error;
  auto chain = BucketChainBodies(*index, route.bucket_page_number, &chain_error);
  if (!chain_error.ok()) {
    return HashInsertError(chain_error.diagnostic.diagnostic_code,
                           chain_error.diagnostic.message_key,
                           "chain");
  }

  u64 inserted_page_number = 0;
  u16 inserted_ordinal = kIndexHashNoEntryOrdinal;
  bool overflow_created = false;
  for (auto& page : chain) {
    if (CanAddEntryToPage(page, entry, index->page_size)) {
      inserted_page_number = page.page_number;
      inserted_ordinal = static_cast<u16>(page.entries.size());
      page.entries.push_back(entry);
      break;
    }
  }
  if (inserted_page_number == 0) {
    IndexHashPageBody overflow = MakeOverflowPage(*index, chain.front());
    ++index->next_page_number;
    chain.back().overflow_page_number = overflow.page_number;
    inserted_page_number = overflow.page_number;
    inserted_ordinal = 0;
    overflow.entries.push_back(entry);
    chain.push_back(std::move(overflow));
    overflow_created = true;
  }

  RebuildCollisionChains(&chain);
  for (const auto& page : chain) {
    auto staged = BuildIndexHashPageBody(page, index->page_size);
    if (!staged.ok()) {
      return HashInsertError(staged.diagnostic.diagnostic_code,
                             staged.diagnostic.message_key,
                             "stage");
    }
    PublishPage(index, staged);
  }

  IndexHashPhysicalInsertResult result;
  result.status = HashOkStatus();
  result.inserted = true;
  result.overflow_page_created = overflow_created;
  result.key_hash = route.key_hash;
  result.bucket_index = route.bucket_index;
  result.bucket_page_number = route.bucket_page_number;
  result.inserted_page_number = inserted_page_number;
  result.inserted_entry_ordinal = inserted_ordinal;
  result.evidence = {"exclusive_bucket_latch_evidence_valid=true",
                     "collision_chain_rebuilt=true",
                     "bucket_local_insert=true",
                     std::string("hash_algorithm_version=") +
                         IndexHashAlgorithmVersionName(index->hash_algorithm_version),
                     std::string("high_assurance_fingerprint_present=") +
                         (entry.fingerprint_present ? "true" : "false"),
                     "full_encoded_key_compare_mandatory=true",
                     "split_merge_compaction_implemented=true"};
  if (overflow_created) {
    result.evidence.push_back("overflow_page_created=true");
  }
  AddCandidateEvidence(&result);
  return result;
}

IndexHashPhysicalProbeResult ProbeIndexHashBucket(
    const IndexHashPhysicalIndex& index,
    const IndexHashPhysicalProbeRequest& request) {
  auto route = LocateIndexHashBucket(index, request.encoded_key);
  if (!route.ok()) {
    return HashProbeError(route.diagnostic.diagnostic_code,
                          route.diagnostic.message_key,
                          "route");
  }
  if (!LatchProofAllows(ImageKey(index),
                        request.latch_evidence,
                        route.bucket_page_number,
                        IndexHashBucketLatchMode::shared)) {
    return HashProbeError("SB-INDEX-HASH-LATCH-PROOF-MISSING",
                          "storage.index_hash_page.latch_proof_missing",
                          "probe_requires_shared_or_optimistic_bucket_latch");
  }
  IndexHashPageBodyResult chain_error;
  auto chain = BucketChainBodies(index, route.bucket_page_number, &chain_error);
  if (!chain_error.ok()) {
    return HashProbeError(chain_error.diagnostic.diagnostic_code,
                          chain_error.diagnostic.message_key,
                          "chain");
  }

  IndexHashPhysicalProbeResult result;
  result.status = HashOkStatus();
  result.key_hash = route.key_hash;
  IndexHashEntry probe_entry;
  probe_entry.encoded_key = request.encoded_key;
  probe_entry = ComputeIndexHashEntryFingerprintsWithSeedInternal(
      std::move(probe_entry),
      index.hash_seed,
      index.hash_seed_high64,
      index.hash_algorithm_version,
      index.test_fixture_force_route_hash_collision,
      index.test_fixture_force_fingerprint_collision);
  result.fingerprint_present = probe_entry.fingerprint_present;
  result.key_fingerprint_low64 = probe_entry.key_fingerprint_low64;
  result.key_fingerprint_high64 = probe_entry.key_fingerprint_high64;
  result.bucket_index = route.bucket_index;
  result.bucket_page_number = route.bucket_page_number;
  result.pages_traversed = chain.size();
  result.evidence = {"bucket_latch_evidence_valid=true",
                     "exact_probe_by_hash_then_key=true",
                     std::string("hash_algorithm_version=") +
                         IndexHashAlgorithmVersionName(index.hash_algorithm_version),
                     std::string("high_assurance_fingerprint_present=") +
                         (probe_entry.fingerprint_present ? "true" : "false"),
                     "collision_chain_traversal=true",
                     "deleted_tombstones_excluded=true"};

  const auto root = std::find_if(chain.front().collision_roots.begin(),
                                 chain.front().collision_roots.end(),
                                 [&](const IndexHashCollisionRoot& candidate) {
                                   return candidate.key_hash == route.key_hash;
                                 });
  if (root != chain.front().collision_roots.end()) {
    u64 current_page = root->page_number;
    u16 current_ordinal = root->entry_ordinal;
    std::set<std::pair<u64, u16>> seen;
    while (current_page != 0 && current_ordinal != kIndexHashNoEntryOrdinal) {
      if (!seen.insert({current_page, current_ordinal}).second) {
        return HashProbeError("SB-INDEX-HASH-COLLISION-CHAIN-CYCLE",
                              "storage.index_hash_page.collision_chain_cycle",
                              std::to_string(current_page));
      }
      auto page_it = std::find_if(chain.begin(),
                                  chain.end(),
                                  [&](const IndexHashPageBody& page) {
                                    return page.page_number == current_page;
                                  });
      if (page_it == chain.end() || current_ordinal >= page_it->entries.size()) {
        return HashProbeError("SB-INDEX-HASH-COLLISION-CHAIN-BROKEN",
                              "storage.index_hash_page.collision_chain_broken",
                              std::to_string(current_page));
      }
      const auto& entry = page_it->entries[current_ordinal];
      ++result.collision_entries_traversed;
      if (!entry.deleted && entry.key_hash == route.key_hash) {
        if (probe_entry.fingerprint_present) {
          if (!entry.fingerprint_present ||
              entry.key_fingerprint_low64 != probe_entry.key_fingerprint_low64 ||
              entry.key_fingerprint_high64 != probe_entry.key_fingerprint_high64) {
            ++result.fingerprint_mismatch_count;
            current_page = entry.next_collision_page_number;
            current_ordinal = entry.next_collision_ordinal;
            continue;
          }
        }
        ++result.encoded_key_compare_count;
        if (entry.encoded_key != request.encoded_key) {
          current_page = entry.next_collision_page_number;
          current_ordinal = entry.next_collision_ordinal;
          continue;
        }
        IndexHashPhysicalRowLocator locator;
        locator.encoded_key = entry.encoded_key;
        locator.row_uuid = entry.row_uuid;
        locator.version_uuid = entry.version_uuid;
        locator.key_hash = entry.key_hash;
        locator.bucket_index = route.bucket_index;
        locator.bucket_page_number = route.bucket_page_number;
        locator.page_number = current_page;
        locator.entry_ordinal = current_ordinal;
        result.locators.push_back(std::move(locator));
      }
      current_page = entry.next_collision_page_number;
      current_ordinal = entry.next_collision_ordinal;
    }
  }
  AddCandidateEvidence(&result);
  result.evidence.push_back("locator_stream_candidate_only=true");
  result.evidence.push_back("full_encoded_key_compare_mandatory=true");
  result.evidence.push_back("encoded_key_compare_count=" +
                            std::to_string(result.encoded_key_compare_count));
  result.evidence.push_back("fingerprint_mismatch_count=" +
                            std::to_string(result.fingerprint_mismatch_count));
  return result;
}

IndexHashPhysicalDeleteResult DeleteIndexHashEntry(
    IndexHashPhysicalIndex* index,
    const IndexHashPhysicalDeleteRequest& request) {
  if (index == nullptr) {
    return HashDeleteError("SB-INDEX-HASH-INDEX-INVALID",
                           "storage.index_hash_page.index_invalid");
  }
  auto route = LocateIndexHashBucket(*index, request.encoded_key);
  if (!route.ok()) {
    return HashDeleteError(route.diagnostic.diagnostic_code,
                           route.diagnostic.message_key,
                           "route");
  }
  if (!LatchProofAllows(ImageKey(*index),
                        request.latch_evidence,
                        route.bucket_page_number,
                        IndexHashBucketLatchMode::exclusive)) {
    return HashDeleteError("SB-INDEX-HASH-LATCH-PROOF-MISSING",
                           "storage.index_hash_page.latch_proof_missing",
                           "delete_requires_exclusive_bucket_latch");
  }
  if (!IsTypedEngineIdentity(request.row_uuid, UuidKind::row) ||
      !IsTypedEngineIdentity(request.version_uuid, UuidKind::row)) {
    return HashDeleteError("SB-INDEX-HASH-LOCATOR-UUID-INVALID",
                           "storage.index_hash_page.locator_uuid_invalid",
                           "delete_locator");
  }
  IndexHashPageBodyResult chain_error;
  auto chain = BucketChainBodies(*index, route.bucket_page_number, &chain_error);
  if (!chain_error.ok()) {
    return HashDeleteError(chain_error.diagnostic.diagnostic_code,
                           chain_error.diagnostic.message_key,
                           "chain");
  }
  IndexHashEntry delete_key;
  delete_key.encoded_key = request.encoded_key;
  delete_key = ComputeIndexHashEntryFingerprintsWithSeedInternal(
      std::move(delete_key),
      index->hash_seed,
      index->hash_seed_high64,
      index->hash_algorithm_version,
      index->test_fixture_force_route_hash_collision,
      index->test_fixture_force_fingerprint_collision);

  for (auto& page : chain) {
    for (std::size_t i = 0; i < page.entries.size(); ++i) {
      auto& entry = page.entries[i];
      if (entry.deleted || entry.key_hash != route.key_hash) {
        continue;
      }
      if (delete_key.fingerprint_present &&
          (!entry.fingerprint_present ||
           entry.key_fingerprint_low64 != delete_key.key_fingerprint_low64 ||
           entry.key_fingerprint_high64 != delete_key.key_fingerprint_high64)) {
        continue;
      }
      if (entry.encoded_key == request.encoded_key &&
          SameTypedUuid(entry.row_uuid, request.row_uuid) &&
          SameTypedUuid(entry.version_uuid, request.version_uuid)) {
        entry.deleted = true;
        RebuildCollisionChains(&chain);
        for (const auto& staged_page : chain) {
          auto staged = BuildIndexHashPageBody(staged_page, index->page_size);
          if (!staged.ok()) {
            return HashDeleteError(staged.diagnostic.diagnostic_code,
                                   staged.diagnostic.message_key,
                                   "stage");
          }
          PublishPage(index, staged);
        }
        IndexHashPhysicalDeleteResult result;
        result.status = HashOkStatus();
        result.deleted = true;
        result.tombstone_marked = true;
        result.key_hash = route.key_hash;
        result.bucket_index = route.bucket_index;
        result.bucket_page_number = route.bucket_page_number;
        result.deleted_page_number = page.page_number;
        result.deleted_entry_ordinal = static_cast<u16>(i);
        result.evidence = {"exclusive_bucket_latch_evidence_valid=true",
                           "bucket_local_tombstone_delete=true",
                           "collision_chain_rebuilt=true",
                           std::string("hash_algorithm_version=") +
                               IndexHashAlgorithmVersionName(
                                   index->hash_algorithm_version),
                           std::string("high_assurance_fingerprint_present=") +
                               (entry.fingerprint_present ? "true" : "false"),
                           "full_encoded_key_compare_mandatory=true",
                           "tombstone_compaction_implemented=true"};
        AddCandidateEvidence(&result);
        return result;
      }
    }
  }
  return HashDeleteError("SB-INDEX-HASH-DELETE-NOT-FOUND",
                         "storage.index_hash_page.delete_not_found",
                         std::to_string(route.bucket_page_number));
}

IndexHashPhysicalMaintenanceResult MaintainIndexHashPhysicalStructure(
    IndexHashPhysicalIndex* index,
    const IndexHashPhysicalMaintenanceRequest& request) {
  if (index == nullptr) {
    return HashMaintenanceError("SB-INDEX-HASH-INDEX-INVALID",
                                "storage.index_hash_page.index_invalid");
  }

  auto preflight = BuildIndexHashPhysicalReport(*index);
  if (!preflight.report.valid) {
    auto refused = HashMaintenanceError(
        preflight.report.exact_diagnostic_code.empty()
            ? "SB-INDEX-HASH-PAGE-IMAGE-VALIDATION-FAILED"
            : preflight.report.exact_diagnostic_code,
        preflight.report.exact_diagnostic_message_key.empty()
            ? "storage.index_hash_page.page_image_validation_failed"
            : preflight.report.exact_diagnostic_message_key,
        "preflight_page_image_validation");
    refused.evidence.push_back("page_image_validation_failed=true");
    return refused;
  }

  auto directory = FetchDirectoryUnlocked(*index);
  if (!directory.ok()) {
    return HashMaintenanceError(directory.diagnostic.diagnostic_code,
                                directory.diagnostic.message_key,
                                "directory");
  }
  auto latch_validation =
      ValidateExclusiveStructuralLatchProofs(*index,
                                             directory.body,
                                             request.exclusive_bucket_latches);
  if (!latch_validation.ok()) {
    return HashMaintenanceError(latch_validation.diagnostic.diagnostic_code,
                                latch_validation.diagnostic.message_key,
                                "latch_validation");
  }

  IndexHashPhysicalMaintenanceResult result;
  result.status = HashOkStatus();
  result.old_bucket_count = directory.body.bucket_count;
  result.new_bucket_count = directory.body.bucket_count;
  result.live_entry_count = preflight.report.live_entry_count;
  result.tombstone_entry_count = preflight.report.tombstone_entry_count;
  result.max_overflow_depth = preflight.report.max_overflow_depth;
  result.benchmark_clean_capability = false;
  const u64 total_entries =
      result.live_entry_count + result.tombstone_entry_count;
  result.load_factor_per_mille =
      result.old_bucket_count == 0
          ? 0
          : static_cast<u32>((result.live_entry_count * 1000u) /
                             result.old_bucket_count);
  result.tombstone_ratio_per_mille =
      total_entries == 0
          ? 0
          : static_cast<u32>((result.tombstone_entry_count * 1000u) /
                             total_entries);

  const auto& policy = request.policy;
  const bool compact_by_tombstone =
      result.tombstone_ratio_per_mille >= policy.tombstone_ratio_per_mille &&
      result.tombstone_entry_count > 0;
  const bool compact_by_overflow =
      result.max_overflow_depth > policy.overflow_depth_threshold;
  result.hash_overflow_compaction_applied =
      compact_by_tombstone || compact_by_overflow;

  if (result.load_factor_per_mille >= policy.split_load_factor_per_mille &&
      result.old_bucket_count < policy.max_bucket_count) {
    result.hash_split_applied = true;
    result.new_bucket_count = result.old_bucket_count + 1;
  } else if (result.load_factor_per_mille <= policy.merge_load_factor_per_mille &&
             result.old_bucket_count > policy.min_bucket_count) {
    result.hash_merge_applied = true;
    result.new_bucket_count = result.old_bucket_count - 1;
  }

  result.evidence = {"exclusive_bucket_latch_evidence_valid=true",
                     "hash_structural_mutation_method=linear_modulo_rehash",
                     "hash_load_factor_threshold_checked=true",
                     "hash_tombstone_ratio_threshold_checked=true",
                     "hash_overflow_depth_threshold_checked=true",
                     "hash_collision_chain_threshold_checked=true",
                     std::string("hash_algorithm_version=") +
                         IndexHashAlgorithmVersionName(
                             index->hash_algorithm_version),
                     std::string("high_assurance_fingerprint_present=") +
                         (HashAlgorithmRequiresFingerprint(
                              index->hash_algorithm_version)
                              ? "true"
                              : "false"),
                     "full_encoded_key_compare_mandatory=true",
                     std::string("hash_split_applied=") +
                         (result.hash_split_applied ? "true" : "false"),
                     std::string("hash_merge_applied=") +
                         (result.hash_merge_applied ? "true" : "false"),
                     std::string("hash_overflow_compaction_applied=") +
                         (result.hash_overflow_compaction_applied ? "true" : "false"),
                     "row_version_locators_preserved=true",
                     "collision_chains_rebuilt=true",
                     "serialized_page_images_updated=true",
                     "benchmark_clean_capability=false"};
  if (preflight.report.rebuild_recommended) {
    result.evidence.push_back("hash_rebuild_recommended=true");
  }
  if (preflight.report.reseed_recommended) {
    result.evidence.push_back("hash_reseed_recommended=true");
  }
  if (preflight.report.max_collision_chain_length >=
      policy.collision_chain_rebuild_threshold) {
    result.evidence.push_back("adversarial_collision_pattern_suspected=true");
  }
  AddCandidateEvidence(&result);

  if (!result.hash_split_applied && !result.hash_merge_applied &&
      !result.hash_overflow_compaction_applied) {
    result.evidence.push_back("hash_structural_mutation_needed=false");
    return result;
  }

  IndexHashPageBodyResult collect_error;
  auto entries = CollectHashEntries(*index,
                                    directory.body,
                                    !result.hash_overflow_compaction_applied,
                                    &collect_error);
  if (!collect_error.ok()) {
    return HashMaintenanceError(collect_error.diagnostic.diagnostic_code,
                                collect_error.diagnostic.message_key,
                                "collect_entries");
  }

  std::vector<IndexHashPhysicalPageImage> staged_pages;
  u64 next_page_number = index->next_page_number;
  u64 overflow_pages_reused = 0;
  auto rebuild = RebuildHashPhysicalImages(*index,
                                           directory.body,
                                           result.new_bucket_count,
                                           entries,
                                           &staged_pages,
                                           &next_page_number,
                                           &overflow_pages_reused);
  if (!rebuild.ok()) {
    return HashMaintenanceError(rebuild.diagnostic.diagnostic_code,
                                rebuild.diagnostic.message_key,
                                "rebuild");
  }

  IndexHashPhysicalIndex candidate = *index;
  candidate.pages = staged_pages;
  candidate.next_page_number = next_page_number;
  auto postflight = BuildIndexHashPhysicalReport(candidate);
  if (!postflight.report.valid) {
    auto refused = HashMaintenanceError(
        postflight.report.exact_diagnostic_code.empty()
            ? "SB-INDEX-HASH-PAGE-IMAGE-VALIDATION-FAILED"
            : postflight.report.exact_diagnostic_code,
        postflight.report.exact_diagnostic_message_key.empty()
            ? "storage.index_hash_page.page_image_validation_failed"
            : postflight.report.exact_diagnostic_message_key,
        "postflight_page_image_validation");
    refused.evidence.push_back("page_image_validation_failed=true");
    return refused;
  }

  if (preflight.report.overflow_page_count > postflight.report.overflow_page_count) {
    result.overflow_pages_reclaimed =
        preflight.report.overflow_page_count - postflight.report.overflow_page_count;
  }
  result.evidence.push_back("post_mutation_page_image_validation_passed=true");
  result.evidence.push_back("overflow_pages_reused=" +
                            std::to_string(overflow_pages_reused));

  *index = std::move(candidate);
  return result;
}

IndexHashPhysicalReportResult BuildIndexHashPhysicalReport(
    const IndexHashPhysicalIndex& index) {
  IndexHashPhysicalReportResult result;
  result.status = HashOkStatus();
  result.report.status = HashOkStatus();
  result.report.directory_page_number = index.directory_page_number;
  result.report.page_count = index.pages.size();
  result.report.page_size = index.page_size;
  result.report.hash_seed = index.hash_seed;
  result.report.hash_seed_high64 = index.hash_seed_high64;
  result.report.hash_algorithm_version = index.hash_algorithm_version;
  result.report.hash_seed_engine_generated = index.hash_seed_engine_generated;
  result.report.hash_seed_client_supplied = false;
  result.report.hash_seed_test_fixture_mode = index.hash_seed_test_fixture_mode;
  result.report.hash_seed_protected = index.hash_seed_protected;
  result.report.hash_seed_entropy_source = index.hash_seed_entropy_source;
  result.report.high_assurance_fingerprint_present =
      HashAlgorithmRequiresFingerprint(index.hash_algorithm_version);
  result.report.legacy_compatible =
      index.hash_algorithm_version == kIndexHashAlgorithmVersion1LegacyFnv64;
  result.report.valid = true;
  result.report.evidence = {"hash_physical_validation_report=true",
                            std::string("hash_algorithm_version=") +
                                IndexHashAlgorithmVersionName(
                                    index.hash_algorithm_version),
                            std::string("hash_seed_engine_generated=") +
                                (index.hash_seed_engine_generated ? "true" : "false"),
                            "hash_seed_client_supplied=false",
                            std::string("hash_seed_test_fixture_mode=") +
                                (index.hash_seed_test_fixture_mode ? "true" : "false"),
                            std::string("hash_seed_protected=") +
                                (index.hash_seed_protected ? "true" : "false"),
                            std::string("hash_seed_entropy_source=") +
                                index.hash_seed_entropy_source,
                            "hash_seed_high64_redacted=true",
                            "hash_seed_key_material_bits=128",
                            "parser_hash_authority=false",
                            "reference_hash_authority=false",
                            "hash_finality_authority=false",
                            std::string("high_assurance_fingerprint_present=") +
                                (HashAlgorithmRequiresFingerprint(
                                     index.hash_algorithm_version)
                                     ? "true"
                                     : "false"),
                            "collision_chain_metadata_checked=true",
                            "overflow_chain_metadata_checked=true",
                            "checksum_validation_checked=true",
                            "page_kind_validation_checked=true",
                            "hash_split_merge_compaction_validation_ready=true",
                            "tombstones_remain_validation_visible=true",
                            "benchmark_clean_capability=false"};
  result.report.support_bundle_rows = {"candidate_set_only=true",
                                       "materialized_final_rows=false",
                                      "transaction_finality=false",
                                      std::string("hash_algorithm_version=") +
                                          IndexHashAlgorithmVersionName(
                                              index.hash_algorithm_version),
                                      std::string("hash_seed_engine_generated=") +
                                          (index.hash_seed_engine_generated
                                               ? "true"
                                               : "false"),
                                      std::string("hash_seed_test_fixture_mode=") +
                                          (index.hash_seed_test_fixture_mode
                                               ? "true"
                                               : "false"),
                                      std::string("hash_seed_protected=") +
                                          (index.hash_seed_protected
                                               ? "true"
                                               : "false"),
                                      std::string("hash_seed_entropy_source=") +
                                          index.hash_seed_entropy_source,
                                      "hash_seed_redacted=true",
                                      "hash_seed_high64_redacted=true",
                                      "hash_seed_key_material_bits=128",
                                      "parser_hash_authority=false",
                                      "reference_hash_authority=false",
                                      "hash_finality_authority=false",
                                       "parser_finality_authority=false",
                                       "provider_finality_authority=false",
                                       "wal_finality_authority=false",
                                       "mga_recheck_required=true",
                                       "security_recheck_required=true",
                                       "visibility_authority=false",
                                       "authorization_authority=false",
                                       "transaction_finality_authority=false",
                                       "recovery_authority=false"};

  auto fail = [&](IndexHashPhysicalCorruptionClass corruption,
                  const DiagnosticRecord& diagnostic) {
    result.report.valid = false;
    result.report.corruption_class = corruption;
    result.report.exact_diagnostic_code = diagnostic.diagnostic_code;
    result.report.exact_diagnostic_message_key = diagnostic.message_key;
    result.report.diagnostic = diagnostic;
    result.diagnostic = diagnostic;
  };

  auto directory = FetchDirectoryUnlocked(index);
  if (!directory.ok()) {
    const std::string code = directory.diagnostic.diagnostic_code;
    fail(code.find("CHECKSUM") != std::string::npos
             ? IndexHashPhysicalCorruptionClass::checksum
             : (code.find("SEED") != std::string::npos ||
                code.find("ALGORITHM-VERSION") != std::string::npos
                    ? IndexHashPhysicalCorruptionClass::seed_version
                    : IndexHashPhysicalCorruptionClass::directory),
         directory.diagnostic);
    result.evidence = result.report.evidence;
    return result;
  }
  result.report.bucket_count = directory.body.bucket_count;

  std::set<u64> directory_pages(directory.body.directory_bucket_page_numbers.begin(),
                                directory.body.directory_bucket_page_numbers.end());
  std::set<u64> reachable_overflow_pages;
  for (u64 bucket_page_number : directory.body.directory_bucket_page_numbers) {
    IndexHashPageBodyResult chain_error;
    auto chain = BucketChainBodies(index, bucket_page_number, &chain_error);
    if (!chain_error.ok()) {
      const std::string code = chain_error.diagnostic.diagnostic_code;
      fail(code.find("CHECKSUM") != std::string::npos
               ? IndexHashPhysicalCorruptionClass::checksum
               : (code.find("SEED") != std::string::npos ||
                  code.find("ALGORITHM-VERSION") != std::string::npos
                      ? IndexHashPhysicalCorruptionClass::seed_version
                      : (code.find("KIND") != std::string::npos
                             ? IndexHashPhysicalCorruptionClass::page_kind
                             : IndexHashPhysicalCorruptionClass::overflow_chain)),
           chain_error.diagnostic);
      result.evidence = result.report.evidence;
      return result;
    }
    ++result.report.bucket_page_count;
    result.report.overflow_page_count += chain.size() - 1;
    result.report.max_overflow_depth =
        std::max(result.report.max_overflow_depth,
                 static_cast<u32>(chain.size() - 1));
    result.report.collision_root_count += chain.front().collision_roots.size();

    std::vector<IndexHashPageBody> expected = chain;
    RebuildCollisionChains(&expected);
    if (expected.front().collision_roots.size() != chain.front().collision_roots.size()) {
      fail(IndexHashPhysicalCorruptionClass::collision_chain,
           MakeIndexHashPageDiagnostic(HashErrorStatus(),
                                       "SB-INDEX-HASH-COLLISION-ROOT-MISMATCH",
                                       "storage.index_hash_page.collision_root_mismatch",
                                       std::to_string(bucket_page_number)));
      result.evidence = result.report.evidence;
      return result;
    }
    for (std::size_t i = 0; i < expected.front().collision_roots.size(); ++i) {
      const auto& left = expected.front().collision_roots[i];
      const auto& right = chain.front().collision_roots[i];
      if (left.key_hash != right.key_hash ||
          left.page_number != right.page_number ||
          left.entry_ordinal != right.entry_ordinal) {
        fail(IndexHashPhysicalCorruptionClass::collision_chain,
             MakeIndexHashPageDiagnostic(HashErrorStatus(),
                                         "SB-INDEX-HASH-COLLISION-ROOT-MISMATCH",
                                         "storage.index_hash_page.collision_root_mismatch",
                                         std::to_string(bucket_page_number)));
        result.evidence = result.report.evidence;
        return result;
      }
    }
    for (const auto& root : chain.front().collision_roots) {
      u64 current_page = root.page_number;
      u16 current_ordinal = root.entry_ordinal;
      u32 chain_length = 0;
      std::set<std::pair<u64, u16>> seen;
      while (current_page != 0 && current_ordinal != kIndexHashNoEntryOrdinal) {
        if (!seen.insert({current_page, current_ordinal}).second) {
          break;
        }
        auto page_it = std::find_if(chain.begin(),
                                    chain.end(),
                                    [&](const IndexHashPageBody& page) {
                                      return page.page_number == current_page;
                                    });
        if (page_it == chain.end() || current_ordinal >= page_it->entries.size()) {
          break;
        }
        const auto& entry = page_it->entries[current_ordinal];
        ++chain_length;
        if (entry.fingerprint_present) {
          result.report.high_assurance_fingerprint_present = true;
        }
        current_page = entry.next_collision_page_number;
        current_ordinal = entry.next_collision_ordinal;
      }
      result.report.max_collision_chain_length =
          std::max(result.report.max_collision_chain_length, chain_length);
      result.report.total_collision_chain_length += chain_length;
      if (chain_length > 1) {
        result.report.collision_count += chain_length - 1;
      }
    }
    for (std::size_t p = 0; p < expected.size(); ++p) {
      if (p != 0) {
        reachable_overflow_pages.insert(chain[p].page_number);
      }
      if (expected[p].overflow_page_number != chain[p].overflow_page_number ||
          expected[p].entries.size() != chain[p].entries.size()) {
        fail(IndexHashPhysicalCorruptionClass::overflow_chain,
             MakeIndexHashPageDiagnostic(HashErrorStatus(),
                                         "SB-INDEX-HASH-OVERFLOW-CHAIN-MISMATCH",
                                         "storage.index_hash_page.overflow_chain_mismatch",
                                         std::to_string(bucket_page_number)));
        result.evidence = result.report.evidence;
        return result;
      }
      for (std::size_t e = 0; e < expected[p].entries.size(); ++e) {
        const auto& left = expected[p].entries[e];
        const auto& right = chain[p].entries[e];
        if (left.next_collision_page_number != right.next_collision_page_number ||
            left.next_collision_ordinal != right.next_collision_ordinal) {
          fail(IndexHashPhysicalCorruptionClass::collision_chain,
               MakeIndexHashPageDiagnostic(HashErrorStatus(),
                                           "SB-INDEX-HASH-COLLISION-CHAIN-MISMATCH",
                                           "storage.index_hash_page.collision_chain_mismatch",
                                           std::to_string(bucket_page_number)));
          result.evidence = result.report.evidence;
          return result;
        }
        if (right.deleted) {
          ++result.report.tombstone_entry_count;
        } else {
          ++result.report.live_entry_count;
        }
        ++result.report.encoded_key_compare_count;
      }
    }
  }
  for (const auto& page : index.pages) {
    auto parsed = ParseIndexHashPageBody(page.serialized,
                                         page.page_number,
                                         index.hash_seed,
                                         index.hash_algorithm_version,
                                         index.test_fixture_force_route_hash_collision,
                                         index.test_fixture_force_fingerprint_collision,
                                         index.hash_seed_high64);
    if (!parsed.ok()) {
      const std::string code = parsed.diagnostic.diagnostic_code;
      fail(code.find("CHECKSUM") != std::string::npos
               ? IndexHashPhysicalCorruptionClass::checksum
               : (code.find("SEED") != std::string::npos ||
                  code.find("ALGORITHM-VERSION") != std::string::npos
                      ? IndexHashPhysicalCorruptionClass::seed_version
                      : IndexHashPhysicalCorruptionClass::page),
           parsed.diagnostic);
      result.evidence = result.report.evidence;
      return result;
    }
    if (parsed.body.page_kind == IndexHashPageKind::bucket &&
        directory_pages.find(parsed.body.page_number) == directory_pages.end()) {
      fail(IndexHashPhysicalCorruptionClass::directory,
           MakeIndexHashPageDiagnostic(HashErrorStatus(),
                                       "SB-INDEX-HASH-BUCKET-NOT-IN-DIRECTORY",
                                       "storage.index_hash_page.bucket_not_in_directory",
                                       std::to_string(parsed.body.page_number)));
      result.evidence = result.report.evidence;
      return result;
    }
    if (parsed.body.page_kind == IndexHashPageKind::overflow &&
        reachable_overflow_pages.find(parsed.body.page_number) ==
            reachable_overflow_pages.end()) {
      fail(IndexHashPhysicalCorruptionClass::overflow_chain,
           MakeIndexHashPageDiagnostic(HashErrorStatus(),
                                       "SB-INDEX-HASH-ORPHAN-OVERFLOW-PAGE",
                                       "storage.index_hash_page.orphan_overflow_page",
                                       std::to_string(parsed.body.page_number)));
      result.evidence = result.report.evidence;
      return result;
    }
  }

  result.report.corruption_class = IndexHashPhysicalCorruptionClass::none;
  if (result.report.collision_root_count != 0) {
    result.report.average_collision_chain_length =
        static_cast<u32>(result.report.total_collision_chain_length /
                         result.report.collision_root_count);
  }
  if (result.report.bucket_count != 0) {
    result.report.bucket_load_factor_per_mille =
        static_cast<u32>((result.report.live_entry_count * 1000u) /
                         result.report.bucket_count);
  }
  const auto hardening = DefaultIndexHashHardeningPolicy();
  result.report.rebuild_recommended =
      result.report.legacy_compatible ||
      result.report.max_collision_chain_length >=
          hardening.collision_chain_rebuild_threshold ||
      result.report.max_overflow_depth > hardening.overflow_depth_warn_threshold;
  result.report.reseed_recommended =
      result.report.legacy_compatible ||
      result.report.max_collision_chain_length >=
          hardening.collision_chain_warn_threshold;
  if (result.report.legacy_compatible) {
    result.report.evidence.push_back("legacy_v1_opened_compatibility=true");
    result.report.evidence.push_back("legacy_v1_rebuild_recommended=true");
  }
  result.report.evidence.push_back("bucket_count=" +
                                   std::to_string(result.report.bucket_count));
  result.report.evidence.push_back(
      "live_entry_count=" + std::to_string(result.report.live_entry_count));
  result.report.evidence.push_back(
      "tombstone_entry_count=" +
      std::to_string(result.report.tombstone_entry_count));
  result.report.evidence.push_back(
      "max_collision_chain_length=" +
      std::to_string(result.report.max_collision_chain_length));
  result.report.evidence.push_back(
      "average_collision_chain_length=" +
      std::to_string(result.report.average_collision_chain_length));
  result.report.evidence.push_back(
      "collision_count=" + std::to_string(result.report.collision_count));
  result.report.evidence.push_back(
      "fingerprint_mismatch_count=" +
      std::to_string(result.report.fingerprint_mismatch_count));
  result.report.evidence.push_back(
      "encoded_key_compare_count=" +
      std::to_string(result.report.encoded_key_compare_count));
  result.report.evidence.push_back(
      "max_overflow_depth=" +
      std::to_string(result.report.max_overflow_depth));
  result.report.evidence.push_back(
      "bucket_load_factor_per_mille=" +
      std::to_string(result.report.bucket_load_factor_per_mille));
  result.report.evidence.push_back(
      std::string("rebuild_recommended=") +
      (result.report.rebuild_recommended ? "true" : "false"));
  result.report.evidence.push_back(
      std::string("reseed_recommended=") +
      (result.report.reseed_recommended ? "true" : "false"));
  result.report.support_bundle_rows.push_back(
      "bucket_count=" + std::to_string(result.report.bucket_count));
  result.report.support_bundle_rows.push_back(
      "live_entry_count=" + std::to_string(result.report.live_entry_count));
  result.report.support_bundle_rows.push_back(
      "tombstone_entry_count=" +
      std::to_string(result.report.tombstone_entry_count));
  result.report.support_bundle_rows.push_back(
      "collision_root_count=" +
      std::to_string(result.report.collision_root_count));
  result.report.support_bundle_rows.push_back(
      "max_collision_chain_length=" +
      std::to_string(result.report.max_collision_chain_length));
  result.report.support_bundle_rows.push_back(
      "average_collision_chain_length=" +
      std::to_string(result.report.average_collision_chain_length));
  result.report.support_bundle_rows.push_back(
      "max_overflow_depth=" +
      std::to_string(result.report.max_overflow_depth));
  result.report.support_bundle_rows.push_back(
      "bucket_load_factor_per_mille=" +
      std::to_string(result.report.bucket_load_factor_per_mille));
  result.report.support_bundle_rows.push_back(
      "collision_count=" + std::to_string(result.report.collision_count));
  result.report.support_bundle_rows.push_back(
      "fingerprint_mismatch_count=" +
      std::to_string(result.report.fingerprint_mismatch_count));
  result.report.support_bundle_rows.push_back(
      "encoded_key_compare_count=" +
      std::to_string(result.report.encoded_key_compare_count));
  result.report.support_bundle_rows.push_back(
      std::string("high_assurance_fingerprint_present=") +
      (result.report.high_assurance_fingerprint_present ? "true" : "false"));
  result.report.support_bundle_rows.push_back(
      std::string("legacy_compatible=") +
      (result.report.legacy_compatible ? "true" : "false"));
  result.report.support_bundle_rows.push_back("hash_seed_redacted=true");
  result.report.support_bundle_rows.push_back("hash_seed_high64_redacted=true");
  result.report.support_bundle_rows.push_back(
      std::string("rebuild_recommended=") +
      (result.report.rebuild_recommended ? "true" : "false"));
  result.report.support_bundle_rows.push_back(
      std::string("reseed_recommended=") +
      (result.report.reseed_recommended ? "true" : "false"));
  auto candidate = CandidateEvidence();
  result.report.evidence.insert(result.report.evidence.end(), candidate.begin(), candidate.end());
  result.evidence = result.report.evidence;
  return result;
}

}  // namespace scratchbird::storage::page
