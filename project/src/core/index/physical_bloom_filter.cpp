// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "physical_bloom_filter.hpp"

#include "index_key_encoding.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'P', 'B', 'L', 'M', '0', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u64 kFnvOffset = 14695981039346656037ull;
inline constexpr u64 kFnvPrime = 1099511628211ull;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status WarnStatus() { return {StatusCode::ok, Severity::warning, Subsystem::engine}; }
Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

PhysicalBloomFilterBuildResult BuildFailure(std::string code,
                                            std::string key,
                                            std::string detail = {}) {
  PhysicalBloomFilterBuildResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakePhysicalBloomFilterDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

PhysicalBloomFilterMutationResult MutationFailure(PhysicalBloomFilterPage page,
                                                  std::string code,
                                                  std::string key,
                                                  std::string detail = {}) {
  PhysicalBloomFilterMutationResult result;
  result.status = WarnStatus();
  result.page = std::move(page);
  result.scan_required = true;
  result.filter_invalidated = true;
  result.diagnostic = MakePhysicalBloomFilterDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

bool SameFormatVersion(PageExtentSummaryFormatVersion left,
                       PageExtentSummaryFormatVersion right) {
  return left.major == right.major && left.minor == right.minor;
}

bool HashCountValid(u32 hash_count) {
  return hash_count >= kPhysicalBloomFilterMinHashCount &&
         hash_count <= kPhysicalBloomFilterMaxHashCount;
}

bool BitCountValid(u64 bit_count) {
  return bit_count >= kPhysicalBloomFilterMinBitCount &&
         bit_count <= kPhysicalBloomFilterMaxBitCount;
}

u64 BitsetByteCount(u64 bit_count) {
  return (bit_count + 7) / 8;
}

bool FprValid(double value) {
  return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool FprTargetValid(double value) {
  return std::isfinite(value) && value > 0.0 && value < 1.0;
}

bool LayoutValid(const PhysicalBloomBlockedLayoutMetadata& layout, u64 bit_count) {
  if (!layout.packed_bitset ||
      !layout.blocked_layout ||
      layout.bits_per_word != 64 ||
      layout.block_bit_count < 64 ||
      layout.block_bit_count % 64 != 0 ||
      layout.bitset_byte_count != BitsetByteCount(bit_count)) {
    return false;
  }
  const u64 expected_blocks =
      (bit_count + layout.block_bit_count - 1) / layout.block_bit_count;
  return layout.block_count == expected_blocks && layout.block_count > 0;
}

bool TailBitsZero(const std::vector<byte>& bitset, u64 bit_count) {
  const u32 tail_bits = static_cast<u32>(bit_count % 8);
  if (tail_bits == 0 || bitset.empty()) {
    return true;
  }
  const byte allowed = static_cast<byte>((1u << tail_bits) - 1u);
  return (bitset.back() & static_cast<byte>(~allowed)) == 0;
}

bool PageValid(const PhysicalBloomFilterPage& page) {
  return PageExtentSummaryUuidTextValid(page.relation_uuid) &&
         PageExtentSummaryUuidTextValid(page.index_uuid) &&
         PageExtentSummaryUuidTextValid(page.segment_uuid) &&
         SameFormatVersion(page.format_version,
                           {kPhysicalBloomFilterCurrentMajor,
                            kPhysicalBloomFilterCurrentMinor}) &&
         page.base_generation > 0 &&
         page.filter_generation > 0 &&
         page.seed != 0 &&
         page.seed_version > 0 &&
         HashCountValid(page.hash_count) &&
         BitCountValid(page.bit_count) &&
         page.bitset.size() == BitsetByteCount(page.bit_count) &&
         TailBitsZero(page.bitset, page.bit_count) &&
         FprTargetValid(page.fpr_target) &&
         FprValid(page.estimated_fpr) &&
         FprValid(page.observed_fpr) &&
         page.observed_false_positive_count <= page.observed_absent_probe_count &&
         LayoutValid(page.layout, page.bit_count) &&
         page.mga_recheck_required &&
         page.security_recheck_required &&
         page.exact_recheck_required_for_maybe_present &&
         !page.visibility_finality_authority &&
         !page.parser_finality_authority_claimed &&
         !page.donor_finality_authority_claimed &&
         !page.provider_finality_authority_claimed &&
         !page.write_ahead_log_finality_authority_claimed;
}

bool AuthorityClean(const PhysicalBloomEncodedKeyEvidence& key) {
  return key.authoritative_encoded_key_evidence &&
         key.engine_mga_visible &&
         !key.parser_finality_authority_claimed &&
         !key.donor_finality_authority_claimed &&
         !key.provider_finality_authority_claimed &&
         !key.write_ahead_log_finality_authority_claimed;
}

bool EncodedKeyValid(std::string_view encoded_key) {
  return !encoded_key.empty() &&
         IsOrderPreservingIndexKeyEncoding(encoded_key) &&
         !IsUnsafeLegacyIndexKeyEncoding(encoded_key);
}

bool KeyEvidenceValid(const PhysicalBloomEncodedKeyEvidence& key) {
  return PageExtentSummaryUuidTextValid(key.row_uuid) &&
         PageExtentSummaryUuidTextValid(key.version_uuid) &&
         EncodedKeyValid(key.encoded_key) &&
         AuthorityClean(key);
}

bool AbsentProbeEvidenceValid(const PhysicalBloomAbsentProbeEvidence& probe) {
  return probe.authoritative_absent_probe_evidence &&
         EncodedKeyValid(probe.encoded_key);
}

bool BuildRequestValid(const PhysicalBloomFilterBuildRequest& request) {
  if (!PageExtentSummaryUuidTextValid(request.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(request.index_uuid) ||
      !PageExtentSummaryUuidTextValid(request.segment_uuid) ||
      request.base_generation == 0 ||
      request.filter_generation == 0 ||
      request.seed == 0 ||
      request.seed_version == 0 ||
      !HashCountValid(request.hash_count) ||
      !BitCountValid(request.bit_count) ||
      !FprTargetValid(request.fpr_target)) {
    return false;
  }
  return std::all_of(request.authoritative_keys.begin(),
                     request.authoritative_keys.end(),
                     KeyEvidenceValid) &&
         std::all_of(request.absent_probe_sample.begin(),
                     request.absent_probe_sample.end(),
                     AbsentProbeEvidenceValid);
}

void SortPage(PhysicalBloomFilterPage* page) {
  std::sort(page->evidence.begin(), page->evidence.end());
}

u64 SplitMix64(u64 value) {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

u64 HashBytes(std::string_view bytes, u64 seed, u64 stream) {
  u64 hash = kFnvOffset ^ SplitMix64(seed + stream);
  for (unsigned char value : bytes) {
    hash ^= static_cast<u64>(value);
    hash *= kFnvPrime;
  }
  return SplitMix64(hash ^ (static_cast<u64>(bytes.size()) << 32) ^ stream);
}

std::pair<u64, u64> DoubleHash(std::string_view encoded_key, u64 seed) {
  u64 first = HashBytes(encoded_key, seed, 0x626c6f6f6d5f6831ull);
  u64 second = HashBytes(encoded_key, seed, 0x626c6f6f6d5f6832ull);
  second |= 1ull;
  if (second == 0) {
    second = 0x9e3779b97f4a7c15ull;
  }
  return {first, second};
}

bool GetBit(const std::vector<byte>& bitset, u64 bit_index) {
  const u64 byte_index = bit_index / 8;
  const u32 bit_in_byte = static_cast<u32>(bit_index % 8);
  return (bitset[byte_index] & static_cast<byte>(1u << bit_in_byte)) != 0;
}

void SetBit(std::vector<byte>* bitset, u64 bit_index) {
  const u64 byte_index = bit_index / 8;
  const u32 bit_in_byte = static_cast<u32>(bit_index % 8);
  (*bitset)[byte_index] =
      static_cast<byte>((*bitset)[byte_index] | static_cast<byte>(1u << bit_in_byte));
}

void AddKeyToBitset(const PhysicalBloomFilterPage& page,
                    std::string_view encoded_key,
                    std::vector<byte>* bitset) {
  const auto [first, second] = DoubleHash(encoded_key, page.seed);
  const u64 block_count = std::max<u64>(page.layout.block_count, 1);
  const u64 block_index = first % block_count;
  const u64 block_start = block_index * page.layout.block_bit_count;
  const u64 block_bits =
      std::min<u64>(page.layout.block_bit_count, page.bit_count - block_start);
  const u64 step = SplitMix64(first ^ second ^ page.seed) | 1ull;
  for (u32 i = 0; i < page.hash_count; ++i) {
    const u64 bit_index =
        block_start + ((second + static_cast<u64>(i) * step) % block_bits);
    SetBit(bitset, bit_index);
  }
}

bool MaybeContains(const PhysicalBloomFilterPage& page, std::string_view encoded_key) {
  const auto [first, second] = DoubleHash(encoded_key, page.seed);
  const u64 block_count = std::max<u64>(page.layout.block_count, 1);
  const u64 block_index = first % block_count;
  const u64 block_start = block_index * page.layout.block_bit_count;
  const u64 block_bits =
      std::min<u64>(page.layout.block_bit_count, page.bit_count - block_start);
  const u64 step = SplitMix64(first ^ second ^ page.seed) | 1ull;
  for (u32 i = 0; i < page.hash_count; ++i) {
    const u64 bit_index =
        block_start + ((second + static_cast<u64>(i) * step) % block_bits);
    if (!GetBit(page.bitset, bit_index)) {
      return false;
    }
  }
  return true;
}

double EstimateFpr(u64 bit_count, u32 hash_count, u64 inserted_key_count) {
  if (bit_count == 0 || hash_count == 0 || inserted_key_count == 0) {
    return 0.0;
  }
  const double m = static_cast<double>(bit_count);
  const double k = static_cast<double>(hash_count);
  const double n = static_cast<double>(inserted_key_count);
  return std::pow(1.0 - std::exp(-(k * n) / m), k);
}

void MeasureAbsentSample(const PhysicalBloomFilterPage& page,
                         const std::vector<PhysicalBloomAbsentProbeEvidence>& sample,
                         u64* false_positive_count,
                         double* observed_fpr) {
  *false_positive_count = 0;
  for (const auto& probe : sample) {
    if (MaybeContains(page, probe.encoded_key)) {
      ++(*false_positive_count);
    }
  }
  *observed_fpr = sample.empty()
                      ? 0.0
                      : static_cast<double>(*false_positive_count) /
                            static_cast<double>(sample.size());
}

PhysicalBloomFilterBuildRequest RebuildRequestForPage(
    const PhysicalBloomFilterPage& page,
    std::vector<PhysicalBloomEncodedKeyEvidence> keys,
    std::vector<PhysicalBloomAbsentProbeEvidence> absent_probe_sample) {
  PhysicalBloomFilterBuildRequest request;
  request.relation_uuid = page.relation_uuid;
  request.index_uuid = page.index_uuid;
  request.segment_uuid = page.segment_uuid;
  request.base_generation = page.base_generation;
  request.filter_generation = page.filter_generation + 1;
  request.seed = page.seed;
  request.seed_version = page.seed_version;
  request.hash_count = page.hash_count;
  request.bit_count = page.bit_count;
  request.fpr_target = page.fpr_target;
  request.authoritative_keys = std::move(keys);
  request.absent_probe_sample = std::move(absent_probe_sample);
  return request;
}

void AppendU8(std::vector<byte>* out, byte value) {
  out->push_back(value);
}

void AppendU32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u32));
  scratchbird::core::platform::StoreLittle32(out->data() + offset, value);
}

void AppendU64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u64));
  StoreLittle64(out->data() + offset, value);
}

void AppendDouble(std::vector<byte>* out, double value) {
  u64 bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "double serialization requires IEEE width");
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU64(out, bits);
}

void AppendString(std::vector<byte>* out, std::string_view value) {
  AppendU32(out, static_cast<u32>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void AppendBytes(std::vector<byte>* out, const std::vector<byte>& bytes) {
  AppendU64(out, static_cast<u64>(bytes.size()));
  out->insert(out->end(), bytes.begin(), bytes.end());
}

void AppendLayout(std::vector<byte>* out,
                  const PhysicalBloomBlockedLayoutMetadata& layout) {
  AppendU32(out, layout.block_bit_count);
  AppendU32(out, layout.bits_per_word);
  AppendU64(out, layout.block_count);
  AppendU64(out, layout.bitset_byte_count);
  AppendU8(out, layout.packed_bitset ? 1 : 0);
  AppendU8(out, layout.blocked_layout ? 1 : 0);
}

u64 ComputeChecksum(std::vector<byte> bytes) {
  if (bytes.size() >= kHeaderBytes) {
    StoreLittle64(bytes.data() + 16, 0);
  }
  u64 hash = kFnvOffset;
  for (byte value : bytes) {
    hash ^= value;
    hash *= kFnvPrime;
  }
  return hash == 0 ? 1 : hash;
}

class Reader {
 public:
  explicit Reader(const std::vector<byte>& bytes) : bytes_(bytes) {}

  bool ReadU8(byte* out) {
    if (offset_ + 1 > bytes_.size()) {
      return false;
    }
    *out = bytes_[offset_++];
    return true;
  }

  bool ReadU32(u32* out) {
    if (offset_ + sizeof(u32) > bytes_.size()) {
      return false;
    }
    *out = scratchbird::core::platform::LoadLittle32(bytes_.data() + offset_);
    offset_ += sizeof(u32);
    return true;
  }

  bool ReadU64(u64* out) {
    if (offset_ + sizeof(u64) > bytes_.size()) {
      return false;
    }
    *out = LoadLittle64(bytes_.data() + offset_);
    offset_ += sizeof(u64);
    return true;
  }

  bool ReadDouble(double* out) {
    u64 bits = 0;
    if (!ReadU64(&bits)) {
      return false;
    }
    std::memcpy(out, &bits, sizeof(bits));
    return true;
  }

  bool ReadString(std::string* out) {
    u32 size = 0;
    if (!ReadU32(&size) || offset_ + size > bytes_.size()) {
      return false;
    }
    out->assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }

  bool ReadBytes(std::vector<byte>* out) {
    u64 size = 0;
    if (!ReadU64(&size) ||
        size > static_cast<u64>(std::numeric_limits<u32>::max()) ||
        offset_ + static_cast<std::size_t>(size) > bytes_.size()) {
      return false;
    }
    out->assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += static_cast<std::size_t>(size);
    return true;
  }

  bool Done() const { return offset_ == bytes_.size(); }
  void SetOffset(std::size_t offset) { offset_ = offset; }

 private:
  const std::vector<byte>& bytes_;
  std::size_t offset_ = 0;
};

bool ReadLayout(Reader* reader, PhysicalBloomBlockedLayoutMetadata* layout) {
  byte packed = 0;
  byte blocked = 0;
  if (!reader->ReadU32(&layout->block_bit_count) ||
      !reader->ReadU32(&layout->bits_per_word) ||
      !reader->ReadU64(&layout->block_count) ||
      !reader->ReadU64(&layout->bitset_byte_count) ||
      !reader->ReadU8(&packed) ||
      !reader->ReadU8(&blocked)) {
    return false;
  }
  layout->packed_bitset = packed != 0;
  layout->blocked_layout = blocked != 0;
  return true;
}

PhysicalBloomFilterOpenResult OpenFailure(PhysicalBloomFilterOpenClass open_class,
                                          std::string code,
                                          std::string key,
                                          std::string detail = {}) {
  PhysicalBloomFilterOpenResult result;
  result.status = open_class == PhysicalBloomFilterOpenClass::stale_generation ||
                          open_class == PhysicalBloomFilterOpenClass::stale_format
                      ? WarnStatus()
                      : ErrorStatus();
  result.open_class = open_class;
  result.scan_required = true;
  result.rebuild_required = true;
  result.restricted_repair_required =
      open_class == PhysicalBloomFilterOpenClass::bad_checksum ||
      open_class == PhysicalBloomFilterOpenClass::corrupt_payload ||
      open_class == PhysicalBloomFilterOpenClass::malformed_bitset_length;
  result.diagnostic = MakePhysicalBloomFilterDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("physical_bloom_filter_scan_fallback");
  if (result.restricted_repair_required) {
    result.actions.push_back("repair_requires_explicit_admission_and_authoritative_keys");
  }
  return result;
}

}  // namespace

PhysicalBloomFilterBuildResult BuildPhysicalBloomFilterFromEncodedKeyEvidence(
    const PhysicalBloomFilterBuildRequest& request) {
  if (!BuildRequestValid(request)) {
    return BuildFailure("INDEX.PHYSICAL_BLOOM_FILTER.BUILD_REFUSED",
                        "index.physical_bloom_filter.build_refused");
  }

  PhysicalBloomFilterPage page;
  page.relation_uuid = request.relation_uuid;
  page.index_uuid = request.index_uuid;
  page.segment_uuid = request.segment_uuid;
  page.base_generation = request.base_generation;
  page.filter_generation = request.filter_generation;
  page.seed = request.seed;
  page.seed_version = request.seed_version;
  page.hash_count = request.hash_count;
  page.bit_count = request.bit_count;
  page.inserted_key_count = static_cast<u64>(request.authoritative_keys.size());
  page.fpr_target = request.fpr_target;
  page.estimated_fpr =
      EstimateFpr(page.bit_count, page.hash_count, page.inserted_key_count);
  page.layout.block_count =
      (page.bit_count + page.layout.block_bit_count - 1) /
      page.layout.block_bit_count;
  page.layout.bitset_byte_count = BitsetByteCount(page.bit_count);
  page.bitset.assign(static_cast<std::size_t>(page.layout.bitset_byte_count), 0);

  for (const auto& key : request.authoritative_keys) {
    AddKeyToBitset(page, key.encoded_key, &page.bitset);
  }
  u64 false_positive_count = 0;
  MeasureAbsentSample(page,
                      request.absent_probe_sample,
                      &false_positive_count,
                      &page.observed_fpr);
  page.observed_absent_probe_count =
      static_cast<u64>(request.absent_probe_sample.size());
  page.observed_false_positive_count = false_positive_count;
  page.evidence.push_back("physical_bloom_filter_packed_bitset=true");
  page.evidence.push_back("bloom_negative_pruning_only=true");
  page.evidence.push_back("maybe_present_requires_exact_recheck=true");
  page.evidence.push_back("mga_recheck_required_for_positive=true");
  page.evidence.push_back("security_recheck_required_for_positive=true");
  page.evidence.push_back("visibility_finality_authority=false");
  page.evidence.push_back("parser_donor_provider_finality_authority=false");
  page.evidence.push_back("write_ahead_log_finality_authority=false");
  SortPage(&page);

  if (!PageValid(page)) {
    return BuildFailure("INDEX.PHYSICAL_BLOOM_FILTER.BUILD_RESULT_INVALID",
                        "index.physical_bloom_filter.build_result_invalid");
  }

  PhysicalBloomFilterBuildResult result;
  result.status = OkStatus();
  result.page = std::move(page);
  result.built = true;
  result.benchmark_clean_admissible =
      (!request.absent_probe_sample_required_for_benchmark_clean ||
       !request.absent_probe_sample.empty()) &&
      result.page.estimated_fpr <= result.page.fpr_target &&
      result.page.observed_fpr <= result.page.fpr_target;
  result.evidence = result.page.evidence;
  result.diagnostic = MakePhysicalBloomFilterDiagnostic(
      result.status,
      "INDEX.PHYSICAL_BLOOM_FILTER.BUILT",
      "index.physical_bloom_filter.built");
  return result;
}

PhysicalBloomFilterSerializeResult SerializePhysicalBloomFilterPage(
    const PhysicalBloomFilterPage& input_page) {
  PhysicalBloomFilterSerializeResult result;
  auto page = input_page;
  SortPage(&page);
  if (!PageValid(page)) {
    result.status = ErrorStatus();
    result.diagnostic = MakePhysicalBloomFilterDiagnostic(
        result.status,
        "INDEX.PHYSICAL_BLOOM_FILTER.SERIALIZE_REFUSED",
        "index.physical_bloom_filter.serialize_refused");
    return result;
  }

  auto& out = result.bytes;
  out.insert(out.end(), kMagic.begin(), kMagic.end());
  AppendU32(&out, page.format_version.major);
  AppendU32(&out, page.format_version.minor);
  AppendU64(&out, 0);
  AppendString(&out, page.relation_uuid);
  AppendString(&out, page.index_uuid);
  AppendString(&out, page.segment_uuid);
  AppendU64(&out, page.base_generation);
  AppendU64(&out, page.filter_generation);
  AppendU64(&out, page.seed);
  AppendU32(&out, page.seed_version);
  AppendU32(&out, page.hash_count);
  AppendU64(&out, page.bit_count);
  AppendU64(&out, page.inserted_key_count);
  AppendDouble(&out, page.fpr_target);
  AppendDouble(&out, page.estimated_fpr);
  AppendDouble(&out, page.observed_fpr);
  AppendU64(&out, page.observed_absent_probe_count);
  AppendU64(&out, page.observed_false_positive_count);
  AppendLayout(&out, page.layout);
  AppendU8(&out, page.mga_recheck_required ? 1 : 0);
  AppendU8(&out, page.security_recheck_required ? 1 : 0);
  AppendU8(&out, page.exact_recheck_required_for_maybe_present ? 1 : 0);
  AppendU8(&out, page.visibility_finality_authority ? 1 : 0);
  AppendU8(&out, page.parser_finality_authority_claimed ? 1 : 0);
  AppendU8(&out, page.donor_finality_authority_claimed ? 1 : 0);
  AppendU8(&out, page.provider_finality_authority_claimed ? 1 : 0);
  AppendU8(&out, page.write_ahead_log_finality_authority_claimed ? 1 : 0);
  AppendBytes(&out, page.bitset);
  AppendU32(&out, static_cast<u32>(page.evidence.size()));
  for (const auto& evidence : page.evidence) {
    AppendString(&out, evidence);
  }

  result.checksum = ComputeChecksum(out);
  StoreLittle64(out.data() + 16, result.checksum);
  result.status = OkStatus();
  result.diagnostic = MakePhysicalBloomFilterDiagnostic(
      result.status,
      "INDEX.PHYSICAL_BLOOM_FILTER.SERIALIZED",
      "index.physical_bloom_filter.serialized");
  return result;
}

PhysicalBloomFilterOpenResult OpenPhysicalBloomFilterPage(
    const PhysicalBloomFilterOpenRequest& request) {
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(PhysicalBloomFilterOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_BLOOM_FILTER.BAD_MAGIC",
                       "index.physical_bloom_filter.bad_magic");
  }

  Reader reader(request.bytes);
  reader.SetOffset(8);
  u32 major = 0;
  u32 minor = 0;
  u64 stored_checksum = 0;
  if (!reader.ReadU32(&major) ||
      !reader.ReadU32(&minor) ||
      !reader.ReadU64(&stored_checksum)) {
    return OpenFailure(PhysicalBloomFilterOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_BLOOM_FILTER.TRUNCATED_HEADER",
                       "index.physical_bloom_filter.truncated_header");
  }
  if (major != kPhysicalBloomFilterCurrentMajor ||
      minor != kPhysicalBloomFilterCurrentMinor) {
    return OpenFailure(PhysicalBloomFilterOpenClass::stale_format,
                       "INDEX.PHYSICAL_BLOOM_FILTER.STALE_FORMAT",
                       "index.physical_bloom_filter.stale_format");
  }
  if (stored_checksum == 0 || ComputeChecksum(request.bytes) != stored_checksum) {
    return OpenFailure(PhysicalBloomFilterOpenClass::bad_checksum,
                       "INDEX.PHYSICAL_BLOOM_FILTER.BAD_CHECKSUM",
                       "index.physical_bloom_filter.bad_checksum");
  }

  PhysicalBloomFilterPage page;
  page.format_version = {major, minor};
  if (!reader.ReadString(&page.relation_uuid) ||
      !reader.ReadString(&page.index_uuid) ||
      !reader.ReadString(&page.segment_uuid) ||
      !reader.ReadU64(&page.base_generation) ||
      !reader.ReadU64(&page.filter_generation) ||
      !reader.ReadU64(&page.seed) ||
      !reader.ReadU32(&page.seed_version) ||
      !reader.ReadU32(&page.hash_count) ||
      !reader.ReadU64(&page.bit_count) ||
      !reader.ReadU64(&page.inserted_key_count) ||
      !reader.ReadDouble(&page.fpr_target) ||
      !reader.ReadDouble(&page.estimated_fpr) ||
      !reader.ReadDouble(&page.observed_fpr) ||
      !reader.ReadU64(&page.observed_absent_probe_count) ||
      !reader.ReadU64(&page.observed_false_positive_count) ||
      !ReadLayout(&reader, &page.layout)) {
    return OpenFailure(PhysicalBloomFilterOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_BLOOM_FILTER.TRUNCATED_PAYLOAD",
                       "index.physical_bloom_filter.truncated_payload");
  }

  byte mga_recheck = 0;
  byte security_recheck = 0;
  byte exact_recheck = 0;
  byte visibility_authority = 0;
  byte parser_authority = 0;
  byte donor_authority = 0;
  byte provider_authority = 0;
  byte log_authority = 0;
  if (!reader.ReadU8(&mga_recheck) ||
      !reader.ReadU8(&security_recheck) ||
      !reader.ReadU8(&exact_recheck) ||
      !reader.ReadU8(&visibility_authority) ||
      !reader.ReadU8(&parser_authority) ||
      !reader.ReadU8(&donor_authority) ||
      !reader.ReadU8(&provider_authority) ||
      !reader.ReadU8(&log_authority) ||
      !reader.ReadBytes(&page.bitset)) {
    return OpenFailure(PhysicalBloomFilterOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_BLOOM_FILTER.TRUNCATED_FLAGS_OR_BITSET",
                       "index.physical_bloom_filter.truncated_flags_or_bitset");
  }
  page.mga_recheck_required = mga_recheck != 0;
  page.security_recheck_required = security_recheck != 0;
  page.exact_recheck_required_for_maybe_present = exact_recheck != 0;
  page.visibility_finality_authority = visibility_authority != 0;
  page.parser_finality_authority_claimed = parser_authority != 0;
  page.donor_finality_authority_claimed = donor_authority != 0;
  page.provider_finality_authority_claimed = provider_authority != 0;
  page.write_ahead_log_finality_authority_claimed = log_authority != 0;

  u32 evidence_count = 0;
  if (!reader.ReadU32(&evidence_count)) {
    return OpenFailure(PhysicalBloomFilterOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_BLOOM_FILTER.TRUNCATED_EVIDENCE",
                       "index.physical_bloom_filter.truncated_evidence");
  }
  for (u32 index = 0; index < evidence_count; ++index) {
    std::string evidence;
    if (!reader.ReadString(&evidence)) {
      return OpenFailure(PhysicalBloomFilterOpenClass::corrupt_payload,
                         "INDEX.PHYSICAL_BLOOM_FILTER.TRUNCATED_EVIDENCE_ENTRY",
                         "index.physical_bloom_filter.truncated_evidence_entry");
    }
    page.evidence.push_back(std::move(evidence));
  }
  if (!reader.Done()) {
    return OpenFailure(PhysicalBloomFilterOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_BLOOM_FILTER.TRAILING_BYTES",
                       "index.physical_bloom_filter.trailing_bytes");
  }
  if (!HashCountValid(page.hash_count)) {
    return OpenFailure(PhysicalBloomFilterOpenClass::invalid_hash_count,
                       "INDEX.PHYSICAL_BLOOM_FILTER.INVALID_HASH_COUNT",
                       "index.physical_bloom_filter.invalid_hash_count");
  }
  if (!BitCountValid(page.bit_count) ||
      page.bitset.size() != BitsetByteCount(page.bit_count) ||
      page.layout.bitset_byte_count != BitsetByteCount(page.bit_count)) {
    return OpenFailure(PhysicalBloomFilterOpenClass::malformed_bitset_length,
                       "INDEX.PHYSICAL_BLOOM_FILTER.MALFORMED_BITSET_LENGTH",
                       "index.physical_bloom_filter.malformed_bitset_length");
  }
  if (!PageValid(page)) {
    return OpenFailure(PhysicalBloomFilterOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_BLOOM_FILTER.INVALID_PAYLOAD",
                       "index.physical_bloom_filter.invalid_payload");
  }
  if ((request.expected_relation_uuid_present &&
       page.relation_uuid != request.expected_relation_uuid) ||
      (request.expected_index_uuid_present &&
       page.index_uuid != request.expected_index_uuid) ||
      (request.expected_segment_uuid_present &&
       page.segment_uuid != request.expected_segment_uuid)) {
    auto result = OpenFailure(PhysicalBloomFilterOpenClass::identity_mismatch,
                              "INDEX.PHYSICAL_BLOOM_FILTER.IDENTITY_MISMATCH",
                              "index.physical_bloom_filter.identity_mismatch");
    result.page = std::move(page);
    return result;
  }
  if (request.expected_base_generation_present &&
      page.base_generation != request.expected_base_generation) {
    auto result = OpenFailure(PhysicalBloomFilterOpenClass::stale_generation,
                              "INDEX.PHYSICAL_BLOOM_FILTER.STALE_GENERATION",
                              "index.physical_bloom_filter.stale_generation");
    result.page = std::move(page);
    result.actions.push_back("rebuild_bloom_filter_from_authoritative_keys");
    return result;
  }

  PhysicalBloomFilterOpenResult result;
  result.status = OkStatus();
  result.open_class = PhysicalBloomFilterOpenClass::current;
  result.page = std::move(page);
  result.scan_required = false;
  result.rebuild_required = false;
  result.actions.push_back("physical_bloom_filter_opened_clean");
  result.actions.push_back("maybe_present_requires_exact_mga_security_recheck");
  result.diagnostic = MakePhysicalBloomFilterDiagnostic(
      result.status,
      "INDEX.PHYSICAL_BLOOM_FILTER.OPENED",
      "index.physical_bloom_filter.opened");
  return result;
}

PhysicalBloomProbeResult ProbePhysicalBloomFilter(
    const PhysicalBloomProbeRequest& request) {
  PhysicalBloomProbeResult result;
  if (!PageValid(request.page) || !EncodedKeyValid(request.encoded_key)) {
    result.status = WarnStatus();
    result.scan_required = true;
    result.diagnostic = MakePhysicalBloomFilterDiagnostic(
        result.status,
        "INDEX.PHYSICAL_BLOOM_FILTER.PROBE_SCAN_REQUIRED",
        "index.physical_bloom_filter.probe_scan_required");
    result.evidence.push_back("invalid_bloom_or_key_scan_required");
    return result;
  }

  result.status = OkStatus();
  result.diagnostic = MakePhysicalBloomFilterDiagnostic(
      result.status,
      "INDEX.PHYSICAL_BLOOM_FILTER.PROBED",
      "index.physical_bloom_filter.probed");
  if (!MaybeContains(request.page, request.encoded_key)) {
    result.decision = PhysicalBloomProbeDecision::definitely_absent;
    result.can_prune = true;
    result.scan_required = false;
    result.exact_recheck_required = false;
    result.false_positive_possible = false;
    result.evidence.push_back("bloom_definitely_absent_negative_prune");
    result.evidence.push_back("absence_is_not_visibility_or_finality_authority");
    return result;
  }

  result.decision = PhysicalBloomProbeDecision::maybe_present;
  result.can_prune = false;
  result.scan_required = true;
  result.exact_recheck_required = true;
  result.false_positive_possible = true;
  result.mga_recheck_required = true;
  result.security_recheck_required = true;
  result.evidence.push_back("bloom_maybe_present_false_positive_possible");
  result.evidence.push_back("maybe_present_requires_exact_recheck=true");
  result.evidence.push_back("mga_recheck_required=true");
  result.evidence.push_back("security_recheck_required=true");
  result.evidence.push_back("visibility_finality_authority=false");
  return result;
}

PhysicalBloomFprMeasurementResult MeasurePhysicalBloomFilterFpr(
    const PhysicalBloomFprMeasurementRequest& request) {
  PhysicalBloomFprMeasurementResult result;
  if (!PageValid(request.page) ||
      (request.sample_required_for_benchmark_clean &&
       request.absent_probe_sample.empty()) ||
      !std::all_of(request.absent_probe_sample.begin(),
                   request.absent_probe_sample.end(),
                   AbsentProbeEvidenceValid)) {
    result.status = WarnStatus();
    result.diagnostic = MakePhysicalBloomFilterDiagnostic(
        result.status,
        "INDEX.PHYSICAL_BLOOM_FILTER.FPR_MEASUREMENT_REFUSED",
        "index.physical_bloom_filter.fpr_measurement_refused");
    result.evidence.push_back("benchmark_clean_refused_missing_or_invalid_absent_sample");
    return result;
  }
  u64 false_positive_count = 0;
  double observed_fpr = 0.0;
  MeasureAbsentSample(request.page,
                      request.absent_probe_sample,
                      &false_positive_count,
                      &observed_fpr);
  result.status = OkStatus();
  result.measured = true;
  result.estimated_fpr = request.page.estimated_fpr;
  result.observed_fpr = observed_fpr;
  result.absent_probe_count =
      static_cast<u64>(request.absent_probe_sample.size());
  result.false_positive_count = false_positive_count;
  result.benchmark_clean_admissible =
      !request.absent_probe_sample.empty() &&
      result.estimated_fpr <= request.page.fpr_target &&
      result.observed_fpr <= request.page.fpr_target;
  result.evidence.push_back(result.benchmark_clean_admissible
                                ? "benchmark_clean_fpr_evidence_admitted"
                                : "benchmark_clean_refused_fpr_target_exceeded");
  result.evidence.push_back("bloom_fpr_sample_is_absent_probe_evidence_only");
  result.diagnostic = MakePhysicalBloomFilterDiagnostic(
      result.status,
      "INDEX.PHYSICAL_BLOOM_FILTER.FPR_MEASURED",
      "index.physical_bloom_filter.fpr_measured");
  return result;
}

PhysicalBloomFilterMutationResult ApplyPhysicalBloomFilterMutation(
    const PhysicalBloomFilterPage& page,
    const PhysicalBloomFilterMutation& mutation) {
  if (!PageValid(page)) {
    return MutationFailure(page,
                           "INDEX.PHYSICAL_BLOOM_FILTER.MUTATION_INVALID_PAGE",
                           "index.physical_bloom_filter.mutation_invalid_page");
  }
  if ((mutation.before_key_present && !KeyEvidenceValid(mutation.before_key)) ||
      (mutation.after_key_present && !KeyEvidenceValid(mutation.after_key))) {
    return MutationFailure(page,
                           "INDEX.PHYSICAL_BLOOM_FILTER.MUTATION_INVALID_EVIDENCE",
                           "index.physical_bloom_filter.mutation_invalid_evidence");
  }

  auto rebuild_if_admitted = [&]() -> PhysicalBloomFilterMutationResult {
    if (!mutation.rebuild_admitted) {
      auto refused = MutationFailure(
          page,
          "INDEX.PHYSICAL_BLOOM_FILTER.REBUILD_REQUIRED",
          "index.physical_bloom_filter.rebuild_required");
      refused.actions.push_back("delete_or_update_requires_authoritative_rebuild");
      return refused;
    }
    return RepairPhysicalBloomFilterFromEncodedKeyEvidence(
        page, mutation.authoritative_source_keys, true, mutation.absent_probe_sample);
  };

  switch (mutation.kind) {
    case PhysicalBloomMutationKind::append_key: {
      if (!mutation.after_key_present || !mutation.after_key.engine_mga_visible) {
        return MutationFailure(page,
                               "INDEX.PHYSICAL_BLOOM_FILTER.APPEND_EVIDENCE_MISSING",
                               "index.physical_bloom_filter.append_evidence_missing");
      }
      auto working = page;
      AddKeyToBitset(working, mutation.after_key.encoded_key, &working.bitset);
      ++working.inserted_key_count;
      ++working.filter_generation;
      working.estimated_fpr =
          EstimateFpr(working.bit_count, working.hash_count, working.inserted_key_count);
      if (!mutation.absent_probe_sample.empty() &&
          std::all_of(mutation.absent_probe_sample.begin(),
                      mutation.absent_probe_sample.end(),
                      AbsentProbeEvidenceValid)) {
        MeasureAbsentSample(working,
                            mutation.absent_probe_sample,
                            &working.observed_false_positive_count,
                            &working.observed_fpr);
        working.observed_absent_probe_count =
            static_cast<u64>(mutation.absent_probe_sample.size());
      } else {
        working.observed_absent_probe_count = 0;
        working.observed_false_positive_count = 0;
        working.observed_fpr = 0.0;
      }
      if (!PageValid(working)) {
        return MutationFailure(working,
                               "INDEX.PHYSICAL_BLOOM_FILTER.MUTATION_RESULT_INVALID",
                               "index.physical_bloom_filter.mutation_result_invalid");
      }
      PhysicalBloomFilterMutationResult result;
      result.status = OkStatus();
      result.page = std::move(working);
      result.applied = true;
      result.scan_required = false;
      result.actions.push_back("physical_bloom_filter_append_bits_set");
      result.actions.push_back("maybe_present_requires_exact_mga_security_recheck");
      result.diagnostic = MakePhysicalBloomFilterDiagnostic(
          result.status,
          "INDEX.PHYSICAL_BLOOM_FILTER.MUTATION_APPLIED",
          "index.physical_bloom_filter.mutation_applied");
      return result;
    }
    case PhysicalBloomMutationKind::delete_key:
    case PhysicalBloomMutationKind::update_key:
      return rebuild_if_admitted();
  }
  return MutationFailure(page,
                         "INDEX.PHYSICAL_BLOOM_FILTER.UNKNOWN_MUTATION",
                         "index.physical_bloom_filter.unknown_mutation");
}

PhysicalBloomFilterMutationResult RepairPhysicalBloomFilterFromEncodedKeyEvidence(
    const PhysicalBloomFilterPage& stale_or_corrupt_page,
    const std::vector<PhysicalBloomEncodedKeyEvidence>& authoritative_source_keys,
    bool repair_admitted,
    const std::vector<PhysicalBloomAbsentProbeEvidence>& absent_probe_sample) {
  if (!repair_admitted) {
    auto refused = MutationFailure(
        stale_or_corrupt_page,
        "INDEX.PHYSICAL_BLOOM_FILTER.REPAIR_REFUSED",
        "index.physical_bloom_filter.repair_refused");
    refused.actions.push_back("repair_requires_explicit_admission_and_authoritative_keys");
    return refused;
  }
  auto request = RebuildRequestForPage(
      stale_or_corrupt_page, authoritative_source_keys, absent_probe_sample);
  auto rebuilt = BuildPhysicalBloomFilterFromEncodedKeyEvidence(request);
  PhysicalBloomFilterMutationResult result;
  result.status = rebuilt.status;
  result.diagnostic = rebuilt.diagnostic;
  result.page = std::move(rebuilt.page);
  result.applied = rebuilt.ok();
  result.rebuild_performed = rebuilt.ok();
  result.scan_required = !rebuilt.ok();
  result.actions.push_back(rebuilt.ok()
                               ? "physical_bloom_filter_rebuilt_from_authoritative_keys"
                               : "physical_bloom_filter_rebuild_refused");
  return result;
}

const char* PhysicalBloomFilterOpenClassName(
    PhysicalBloomFilterOpenClass open_class) {
  switch (open_class) {
    case PhysicalBloomFilterOpenClass::current: return "current";
    case PhysicalBloomFilterOpenClass::stale_format: return "stale_format";
    case PhysicalBloomFilterOpenClass::stale_generation: return "stale_generation";
    case PhysicalBloomFilterOpenClass::bad_checksum: return "bad_checksum";
    case PhysicalBloomFilterOpenClass::corrupt_payload: return "corrupt_payload";
    case PhysicalBloomFilterOpenClass::malformed_bitset_length:
      return "malformed_bitset_length";
    case PhysicalBloomFilterOpenClass::invalid_hash_count: return "invalid_hash_count";
    case PhysicalBloomFilterOpenClass::identity_mismatch: return "identity_mismatch";
    case PhysicalBloomFilterOpenClass::refused: return "refused";
  }
  return "unknown";
}

const char* PhysicalBloomProbeDecisionName(PhysicalBloomProbeDecision decision) {
  switch (decision) {
    case PhysicalBloomProbeDecision::definitely_absent: return "definitely_absent";
    case PhysicalBloomProbeDecision::maybe_present: return "maybe_present";
    case PhysicalBloomProbeDecision::scan_or_rebuild_required:
      return "scan_or_rebuild_required";
  }
  return "unknown";
}

DiagnosticRecord MakePhysicalBloomFilterDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.physical_bloom_filter");
}

}  // namespace scratchbird::core::index
