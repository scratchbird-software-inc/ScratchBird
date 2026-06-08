// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "uuid_v7_index_encoding.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace scratchbird::core::index {
namespace {
namespace platform = scratchbird::core::platform;

using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::uuid::CompareUuidV7ForIndex;
using scratchbird::core::uuid::ExtractUuidV7TimePrefix;

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'V', '7', 'I', 'D', 'X', '1'};
inline constexpr u16 kFormatVersion = 1;
inline constexpr u16 kMaxPrefixBytes = 15;

u64 Fnv1a64(const byte* data, std::size_t size) {
  u64 hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<u64>(data[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

void Append16(std::vector<byte>* out, u16 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle16(out->data() + offset, value);
}

void Append32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle32(out->data() + offset, value);
}

void Append64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle64(out->data() + offset, value);
}

u16 CommonPrefixBytes(const std::vector<TypedUuid>& uuids) {
  if (uuids.empty()) {
    return 0;
  }
  u16 prefix = 16;
  for (std::size_t i = 1; i < uuids.size(); ++i) {
    u16 current = 0;
    while (current < prefix &&
           uuids.front().value.bytes[current] == uuids[i].value.bytes[current]) {
      ++current;
    }
    prefix = current;
  }
  return std::min(prefix, kMaxPrefixBytes);
}

UuidV7IndexEncodedPage Refuse(std::string reason) {
  UuidV7IndexEncodedPage result;
  result.refusal_reason = std::move(reason);
  result.fallback_to_uncompressed_uuid = true;
  result.evidence.push_back("fallback_to_uncompressed_uuid=true");
  result.evidence.push_back("uuid_ordering_finality_authority=false");
  result.evidence.push_back("finality_authority=false");
  result.evidence.push_back("visibility_authority=false");
  return result;
}

void AppendCompatibilityEvidence(
    UuidV7IndexEncodedPage* result,
    const platform::RuntimeCompatibilityResult& compatibility) {
  result->evidence.insert(result->evidence.end(),
                          compatibility.evidence.begin(),
                          compatibility.evidence.end());
  result->dictionary.evidence.insert(result->dictionary.evidence.end(),
                                     compatibility.evidence.begin(),
                                     compatibility.evidence.end());
}

platform::RuntimeCompatibilityDescriptor UuidV7Compatibility(
    const UuidV7IndexEncodeRequest& request) {
  const bool explicit_descriptor =
      !request.runtime_compatibility.route_id.empty();
  auto descriptor = request.runtime_compatibility;
  if (descriptor.route_id.empty()) {
    descriptor = platform::CurrentRuntimeCompatibilityDescriptor(
        "core.index.uuid_v7_index_encoding");
  }
  descriptor.route_id = descriptor.route_id.empty()
                            ? "core.index.uuid_v7_index_encoding"
                            : descriptor.route_id;
  descriptor.source_component = "core.index.uuid_v7_index_encoding";
  descriptor.required_endian = platform::RuntimeEndian::little;
  if (!explicit_descriptor ||
      descriptor.provider_endian == platform::RuntimeEndian::unknown) {
    descriptor.provider_endian = platform::RuntimeEndian::little;
  }
  if (descriptor.required_alignment == 0) {
    descriptor.required_alignment = 1;
  }
  descriptor.accelerator_requested = true;
  descriptor.deterministic_scalar_fallback_available = true;
  if (descriptor.provider_accelerator_capabilities.empty()) {
    descriptor.provider_accelerator_capabilities = {
        "uuid_v7_time_prefix_comparator",
        "uuid_v7_prefix_dictionary",
        "uncompressed_uuid_fallback"};
  }
  return descriptor;
}

bool ReadBytes(const std::vector<byte>& bytes,
               std::size_t* offset,
               std::size_t count,
               const byte** out) {
  if (*offset + count > bytes.size()) {
    return false;
  }
  *out = bytes.data() + *offset;
  *offset += count;
  return true;
}

bool Read16(const std::vector<byte>& bytes, std::size_t* offset, u16* out) {
  const byte* ptr = nullptr;
  if (!ReadBytes(bytes, offset, sizeof(*out), &ptr)) {
    return false;
  }
  *out = LoadLittle16(ptr);
  return true;
}

bool Read32(const std::vector<byte>& bytes, std::size_t* offset, u32* out) {
  const byte* ptr = nullptr;
  if (!ReadBytes(bytes, offset, sizeof(*out), &ptr)) {
    return false;
  }
  *out = LoadLittle32(ptr);
  return true;
}

bool Read64(const std::vector<byte>& bytes, std::size_t* offset, u64* out) {
  const byte* ptr = nullptr;
  if (!ReadBytes(bytes, offset, sizeof(*out), &ptr)) {
    return false;
  }
  *out = LoadLittle64(ptr);
  return true;
}

bool PredicateBelowPage(const UuidV7TimeRangePredicate& predicate, u64 page_min) {
  return predicate.upper_present &&
         (predicate.upper_unix_epoch_millis < page_min ||
          (predicate.upper_unix_epoch_millis == page_min && !predicate.upper_inclusive));
}

bool PredicateAbovePage(const UuidV7TimeRangePredicate& predicate, u64 page_max) {
  return predicate.lower_present &&
         (predicate.lower_unix_epoch_millis > page_max ||
          (predicate.lower_unix_epoch_millis == page_max && !predicate.lower_inclusive));
}

}  // namespace

const char* UuidV7TimePruneDecisionKindName(UuidV7TimePruneDecisionKind decision) {
  switch (decision) {
    case UuidV7TimePruneDecisionKind::scan: return "scan";
    case UuidV7TimePruneDecisionKind::prune: return "prune";
    case UuidV7TimePruneDecisionKind::fallback_uncompressed: return "fallback_uncompressed";
  }
  return "unknown";
}

UuidV7IndexEncodedPage BuildUuidV7IndexPageEncoding(
    const UuidV7IndexEncodeRequest& request) {
  const auto compatibility = platform::NegotiateRuntimeCompatibility(
      UuidV7Compatibility(request));
  if (compatibility.action ==
      platform::RuntimeCompatibilityAction::exact_scalar_fallback) {
    auto result =
        Refuse("runtime_compatibility:" + compatibility.diagnostic_code);
    AppendCompatibilityEvidence(&result, compatibility);
    return result;
  }
  if (compatibility.action ==
      platform::RuntimeCompatibilityAction::fail_closed) {
    auto result =
        Refuse("runtime_compatibility:" + compatibility.diagnostic_code);
    result.evidence.push_back("fail_closed=true");
    AppendCompatibilityEvidence(&result, compatibility);
    return result;
  }
  if (request.expected_kind == UuidKind::unknown) {
    return Refuse("unsupported_kind");
  }
  if (request.uuids.empty()) {
    return Refuse("empty_page");
  }
  if (request.dictionary_generation == 0) {
    return Refuse("dictionary_generation_required");
  }

  std::vector<TypedUuid> sorted = request.uuids;
  for (const auto& uuid : sorted) {
    if (uuid.kind != request.expected_kind) {
      return Refuse("kind_mismatch");
    }
    const auto time = ExtractUuidV7TimePrefix(uuid.value);
    if (!time.ok) {
      return Refuse(time.refusal_reason);
    }
  }
  std::sort(sorted.begin(), sorted.end(), [&](const TypedUuid& left, const TypedUuid& right) {
    return CompareUuidV7ForIndex(left, right, request.expected_kind).comparison < 0;
  });

  UuidV7IndexEncodedPage result;
  result.ok = true;
  result.compressed = true;
  result.fallback_to_uncompressed_uuid = false;
  result.decoded_round_trip = sorted;
  result.uncompressed_bytes = static_cast<u64>(sorted.size() * 16);
  result.dictionary.format_version = kFormatVersion;
  result.dictionary.uuid_kind = request.expected_kind;
  result.dictionary.dictionary_generation = request.dictionary_generation;
  result.dictionary.prefix_bytes = CommonPrefixBytes(sorted);
  result.dictionary.prefix.assign(sorted.front().value.bytes.begin(),
                                  sorted.front().value.bytes.begin() + result.dictionary.prefix_bytes);
  result.dictionary.base_unix_epoch_millis =
      ExtractUuidV7TimePrefix(sorted.front().value).unix_epoch_millis;
  result.dictionary.max_unix_epoch_millis =
      ExtractUuidV7TimePrefix(sorted.back().value).unix_epoch_millis;
  result.dictionary.evidence = {
      "page_dictionary_present=true",
      "uuidv7_prefix_compression=true",
      "time_prefix_delta_encoding=true",
      "external_identity_remains_uuid=true",
      "uuid_ordering_finality_authority=false",
      "finality_authority=false",
      "visibility_authority=false",
  };
  result.evidence = result.dictionary.evidence;
  AppendCompatibilityEvidence(&result, compatibility);

  std::vector<byte> serialized(kMagic.begin(), kMagic.end());
  Append16(&serialized, kFormatVersion);
  Append16(&serialized, static_cast<u16>(request.expected_kind));
  Append64(&serialized, request.dictionary_generation);
  Append64(&serialized, result.dictionary.base_unix_epoch_millis);
  Append64(&serialized, result.dictionary.max_unix_epoch_millis);
  Append16(&serialized, result.dictionary.prefix_bytes);
  Append32(&serialized, static_cast<u32>(sorted.size()));
  serialized.insert(serialized.end(), result.dictionary.prefix.begin(), result.dictionary.prefix.end());

  for (const auto& uuid : sorted) {
    const u64 time = ExtractUuidV7TimePrefix(uuid.value).unix_epoch_millis;
    Append64(&serialized, time - result.dictionary.base_unix_epoch_millis);
    const u16 suffix_bytes = static_cast<u16>(16u - result.dictionary.prefix_bytes);
    Append16(&serialized, suffix_bytes);
    serialized.insert(serialized.end(),
                      uuid.value.bytes.begin() + result.dictionary.prefix_bytes,
                      uuid.value.bytes.end());
  }

  result.dictionary.dictionary_checksum = Fnv1a64(serialized.data(), serialized.size());
  Append64(&serialized, result.dictionary.dictionary_checksum);
  result.serialized = std::move(serialized);
  result.compressed_bytes = static_cast<u64>(result.serialized.size());
  result.bytes_saved = result.uncompressed_bytes > result.compressed_bytes
                           ? result.uncompressed_bytes - result.compressed_bytes
                           : 0;
  if (result.bytes_saved == 0) {
    return Refuse("compressed_page_not_smaller");
  }
  return result;
}

UuidV7IndexEncodedPage DecodeUuidV7IndexPageEncoding(
    const std::vector<byte>& serialized,
    UuidKind expected_kind,
    u64 expected_dictionary_generation) {
  if (serialized.size() < kMagic.size() + 2 + 2 + 8 + 8 + 8 + 2 + 4 + 8) {
    return Refuse("truncated_dictionary");
  }
  const u64 stored_checksum = LoadLittle64(serialized.data() + serialized.size() - sizeof(u64));
  const u64 computed_checksum = Fnv1a64(serialized.data(), serialized.size() - sizeof(u64));
  if (stored_checksum != computed_checksum) {
    return Refuse("dictionary_checksum_mismatch");
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), serialized.begin())) {
    return Refuse("bad_dictionary_magic");
  }

  std::size_t offset = kMagic.size();
  u16 format = 0;
  u16 stored_kind = 0;
  u64 generation = 0;
  u64 base_time = 0;
  u64 max_time = 0;
  u16 prefix_bytes = 0;
  u32 entry_count = 0;
  if (!Read16(serialized, &offset, &format) || !Read16(serialized, &offset, &stored_kind) ||
      !Read64(serialized, &offset, &generation) || !Read64(serialized, &offset, &base_time) ||
      !Read64(serialized, &offset, &max_time) || !Read16(serialized, &offset, &prefix_bytes) ||
      !Read32(serialized, &offset, &entry_count)) {
    return Refuse("truncated_dictionary");
  }
  if (format != kFormatVersion) {
    return Refuse("incompatible_dictionary_format");
  }
  if (expected_kind == UuidKind::unknown || stored_kind != static_cast<u16>(expected_kind)) {
    return Refuse("incompatible_dictionary_kind");
  }
  if (generation != expected_dictionary_generation) {
    return Refuse("stale_dictionary_generation");
  }
  if (max_time < base_time) {
    return Refuse("corrupt_time_range");
  }
  if (prefix_bytes > kMaxPrefixBytes) {
    return Refuse("corrupt_prefix_length");
  }

  UuidV7IndexEncodedPage result;
  result.dictionary.format_version = format;
  result.dictionary.uuid_kind = expected_kind;
  result.dictionary.dictionary_generation = generation;
  result.dictionary.base_unix_epoch_millis = base_time;
  result.dictionary.max_unix_epoch_millis = max_time;
  result.dictionary.prefix_bytes = prefix_bytes;
  result.dictionary.dictionary_checksum = stored_checksum;
  result.dictionary.evidence = {
      "page_dictionary_present=true",
      "dictionary_checksum_valid=true",
      "dictionary_generation_current=true",
      "finality_authority=false",
  };
  const byte* prefix = nullptr;
  if (!ReadBytes(serialized, &offset, prefix_bytes, &prefix)) {
    return Refuse("truncated_prefix");
  }
  result.dictionary.prefix.assign(prefix, prefix + prefix_bytes);

  for (u32 i = 0; i < entry_count; ++i) {
    u64 delta = 0;
    u16 suffix_bytes = 0;
    if (!Read64(serialized, &offset, &delta) || !Read16(serialized, &offset, &suffix_bytes)) {
      return Refuse("truncated_entry");
    }
    if (delta > max_time - base_time) {
      return Refuse("corrupt_time_delta");
    }
    if (suffix_bytes != 16u - prefix_bytes) {
      return Refuse("corrupt_suffix_length");
    }
    const byte* suffix = nullptr;
    if (!ReadBytes(serialized, &offset, suffix_bytes, &suffix)) {
      return Refuse("truncated_suffix");
    }
    TypedUuid uuid;
    uuid.kind = expected_kind;
    std::copy(result.dictionary.prefix.begin(), result.dictionary.prefix.end(), uuid.value.bytes.begin());
    std::copy(suffix, suffix + suffix_bytes, uuid.value.bytes.begin() + prefix_bytes);
    const auto time = ExtractUuidV7TimePrefix(uuid.value);
    if (!time.ok || time.unix_epoch_millis != base_time + delta ||
        time.unix_epoch_millis > max_time) {
      return Refuse("decoded_uuid_v7_time_delta_mismatch");
    }
    result.decoded_round_trip.push_back(uuid);
  }
  if (offset != serialized.size() - sizeof(u64)) {
    return Refuse("trailing_dictionary_bytes");
  }

  result.ok = true;
  result.compressed = true;
  result.fallback_to_uncompressed_uuid = false;
  result.serialized = serialized;
  result.uncompressed_bytes = static_cast<u64>(result.decoded_round_trip.size() * 16);
  result.compressed_bytes = static_cast<u64>(serialized.size());
  result.evidence = result.dictionary.evidence;
  result.evidence.push_back("round_trip_decode_valid=true");
  return result;
}

UuidV7TimePruneDecision PlanUuidV7TimePrefixPrune(
    const UuidV7IndexPageDictionary& dictionary,
    const UuidV7TimeRangePredicate& predicate,
    bool base_row_mga_recheck_required) {
  UuidV7TimePruneDecision decision;
  decision.finality_authority = false;
  decision.visibility_authority = false;
  decision.evidence.push_back("uuid_ordering_finality_authority=false");
  if (!base_row_mga_recheck_required) {
    decision.refusal_reason = "mga_recheck_required";
    decision.evidence.push_back("fallback_to_uncompressed_scan=true");
    return decision;
  }
  if (dictionary.format_version != kFormatVersion || dictionary.uuid_kind == UuidKind::unknown ||
      dictionary.prefix.empty() || dictionary.dictionary_checksum == 0 ||
      dictionary.max_unix_epoch_millis < dictionary.base_unix_epoch_millis) {
    decision.refusal_reason = "dictionary_not_usable";
    decision.evidence.push_back("fallback_to_uncompressed_scan=true");
    return decision;
  }

  const u64 page_min = dictionary.base_unix_epoch_millis;
  const u64 page_max = dictionary.max_unix_epoch_millis;
  decision.ok = true;
  decision.candidate_lower_unix_epoch_millis = page_min;
  decision.candidate_upper_unix_epoch_millis = page_max;
  if (PredicateBelowPage(predicate, page_min) || PredicateAbovePage(predicate, page_max)) {
    decision.decision = UuidV7TimePruneDecisionKind::prune;
    decision.candidate_range_selected = false;
    decision.fallback_to_uncompressed_scan = false;
    decision.evidence.push_back("time_prefix_prune_selected=true");
    return decision;
  }
  decision.decision = UuidV7TimePruneDecisionKind::scan;
  decision.candidate_range_selected = true;
  decision.fallback_to_uncompressed_scan = false;
  decision.evidence.push_back("time_prefix_candidate_range_selected=true");
  decision.evidence.push_back("mga_visibility_recheck=required");
  return decision;
}

}  // namespace scratchbird::core::index
