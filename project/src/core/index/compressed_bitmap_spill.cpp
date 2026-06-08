// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compressed_bitmap_spill.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

#if defined(__AVX512F__) && defined(__AVX512VPOPCNTDQ__)
#include <immintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::u8;

constexpr std::array<byte, 8> kSpillMagic = {'S', 'B', 'C', 'B',
                                             'M', 'S', '7', '2'};
constexpr u16 kSpillFormatVersion = 1;
constexpr u16 kSpillHeaderBytes = 48;
constexpr u32 kContainerOrdinalSpan = 65536;
constexpr u32 kDenseBitmapWordCount = 1024;
constexpr u32 kFlagRequiresExactRecheck = 1u << 0u;
constexpr u32 kFlagRequiresMgaRecheck = 1u << 1u;
constexpr u32 kFlagRequiresSecurityRecheck = 1u << 2u;
constexpr u32 kFlagNonAuthorityEvidence = 1u << 3u;
constexpr u32 kRequiredFlags = kFlagRequiresExactRecheck |
                               kFlagRequiresMgaRecheck |
                               kFlagRequiresSecurityRecheck |
                               kFlagNonAuthorityEvidence;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefusalStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::engine};
}

DiagnosticRecord MakeDiagnostic(Status status,
                                std::string diagnostic_code,
                                std::string message_key,
                                std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::move(diagnostic_code);
  record.message_key = std::move(message_key);
  if (!detail.empty()) {
    record.arguments.push_back(DiagnosticArgument{"detail", std::move(detail)});
  }
  record.source_component = "core.index.compressed_bitmap_spill";
  return record;
}

std::vector<std::string> BaseEvidence() {
  return {"compressed_bitmap_spill.format_magic=SBCBMS72",
          "compressed_bitmap_spill.format_version=1",
          "compressed_bitmap_spill.materialized_row_expansion=false",
          "candidate_stream_only=true",
          "candidate_set_finality_authority=false",
          "parser_or_donor_authority=false",
          "provider_finality_authority=false",
          "wal_recovery_or_finality_authority=false",
          "exact_recheck.required=true",
          "mga_visibility_recheck.required=true",
          "security_authorization_recheck.required=true"};
}

void AppendEvidence(std::vector<std::string>* out,
                    const std::vector<std::string>& evidence) {
  out->insert(out->end(), evidence.begin(), evidence.end());
}

CompressedBitmapSpillResult RefuseSpill(
    CompressedBitmapSpillClassification classification,
    const std::string& diagnostic_code,
    const std::string& message_key,
    const std::string& reason) {
  CompressedBitmapSpillResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.classification = classification;
  result.diagnostic =
      MakeDiagnostic(result.status, diagnostic_code, message_key, reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseEvidence();
  result.evidence.push_back(
      "compressed_bitmap_spill.classification=" +
      std::string(CompressedBitmapSpillClassificationName(classification)));
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  return result;
}

CompressedBitmapPopcountResult RefusePopcount(const std::string& diagnostic_code,
                                              const std::string& message_key,
                                              const std::string& reason) {
  CompressedBitmapPopcountResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.diagnostic =
      MakeDiagnostic(result.status, diagnostic_code, message_key, reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseEvidence();
  result.evidence.push_back("popcount.fail_closed=true");
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  return result;
}

u64 Fnv1a64(const byte* data, std::size_t size) {
  u64 hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

void Append16(std::vector<byte>* out, u16 value) {
  const auto stored = scratchbird::core::platform::HostToLittle16(value);
  const auto* bytes = reinterpret_cast<const byte*>(&stored);
  out->insert(out->end(), bytes, bytes + sizeof(stored));
}

void Append32(std::vector<byte>* out, u32 value) {
  const auto stored = scratchbird::core::platform::HostToLittle32(value);
  const auto* bytes = reinterpret_cast<const byte*>(&stored);
  out->insert(out->end(), bytes, bytes + sizeof(stored));
}

void Append64(std::vector<byte>* out, u64 value) {
  const auto stored = scratchbird::core::platform::HostToLittle64(value);
  const auto* bytes = reinterpret_cast<const byte*>(&stored);
  out->insert(out->end(), bytes, bytes + sizeof(stored));
}

bool AddOverflows(u64 left, u64 right) {
  return right > std::numeric_limits<u64>::max() - left;
}

bool HasContainerPayloadForType(
    const CandidateSetCompressedBitmapContainer& container) {
  switch (container.type) {
    case CandidateSetCompressedBitmapContainerType::array_sparse:
      return container.array_offsets.size() == container.cardinality &&
             container.runs.empty() && container.bitmap_words.empty();
    case CandidateSetCompressedBitmapContainerType::run:
      return !container.runs.empty() && container.array_offsets.empty() &&
             container.bitmap_words.empty();
    case CandidateSetCompressedBitmapContainerType::dense_bitmap:
      return container.bitmap_words.size() == kDenseBitmapWordCount &&
             container.array_offsets.empty() && container.runs.empty();
  }
  return false;
}

std::vector<u64> DenseWordsFromContainer(
    const CandidateSetCompressedBitmapContainer& container) {
  std::vector<u64> words(kDenseBitmapWordCount, 0);
  switch (container.type) {
    case CandidateSetCompressedBitmapContainerType::array_sparse:
      for (const auto offset : container.array_offsets) {
        words[offset / 64] |= 1ull << (offset % 64);
      }
      break;
    case CandidateSetCompressedBitmapContainerType::run:
      for (const auto& run : container.runs) {
        for (u32 delta = 0; delta < run.run_length; ++delta) {
          const u32 offset = static_cast<u32>(run.start_offset) + delta;
          words[offset / 64] |= 1ull << (offset % 64);
        }
      }
      break;
    case CandidateSetCompressedBitmapContainerType::dense_bitmap:
      words = container.bitmap_words;
      break;
  }
  return words;
}

u64 ScalarPopcountWords(const std::vector<u64>& words) {
  u64 count = 0;
  for (const auto word : words) {
    count += static_cast<u64>(__builtin_popcountll(word));
  }
  return count;
}

u32 FirstContainerOffset(const CandidateSetCompressedBitmapContainer& container) {
  switch (container.type) {
    case CandidateSetCompressedBitmapContainerType::array_sparse:
      return container.array_offsets.front();
    case CandidateSetCompressedBitmapContainerType::run:
      return container.runs.front().start_offset;
    case CandidateSetCompressedBitmapContainerType::dense_bitmap:
      for (u32 word_index = 0; word_index < container.bitmap_words.size();
           ++word_index) {
        const u64 word = container.bitmap_words[word_index];
        if (word != 0) {
          return word_index * 64u +
                 static_cast<u32>(__builtin_ctzll(word));
        }
      }
      break;
  }
  return 0;
}

u32 LastContainerOffset(const CandidateSetCompressedBitmapContainer& container) {
  switch (container.type) {
    case CandidateSetCompressedBitmapContainerType::array_sparse:
      return container.array_offsets.back();
    case CandidateSetCompressedBitmapContainerType::run: {
      const auto& run = container.runs.back();
      return static_cast<u32>(run.start_offset) + run.run_length - 1u;
    }
    case CandidateSetCompressedBitmapContainerType::dense_bitmap:
      for (u32 reverse = static_cast<u32>(container.bitmap_words.size());
           reverse > 0; --reverse) {
        const u64 word = container.bitmap_words[reverse - 1u];
        if (word != 0) {
          return (reverse - 1u) * 64u + 63u -
                 static_cast<u32>(__builtin_clzll(word));
        }
      }
      break;
  }
  return 0;
}

bool ValidateCompressedBitmapSpillCandidateSet(const CandidateSet& set) {
  if (set.encoding != CandidateSetEncoding::compressed_bitmap ||
      !set.compressed || !set.approximate ||
      !set.rows.empty() || !set.compressed_ranges.empty() ||
      !set.non_authority_evidence_present ||
      set.candidate_set_finality_authority ||
      set.parser_or_donor_finality_or_visibility_authority ||
      set.provider_finality_or_visibility_authority ||
      set.wal_recovery_or_finality_authority ||
      set.final_rows_authorized ||
      !set.requires_exact_recheck ||
      !set.requires_mga_visibility_recheck ||
      !set.requires_security_authorization_recheck) {
    return false;
  }
  if (set.compressed_bitmap_containers.empty()) {
    return set.compressed_bitmap_cardinality == 0 &&
           !set.compressed_bitmap_row_ordinal_range_present &&
           set.compressed_bitmap_min_row_ordinal == 0 &&
           set.compressed_bitmap_max_row_ordinal == 0;
  }
  if (!set.compressed_bitmap_row_ordinal_range_present ||
      set.compressed_bitmap_min_row_ordinal >
          set.compressed_bitmap_max_row_ordinal) {
    return false;
  }

  bool first_container = true;
  u64 previous_base = 0;
  u64 cardinality = 0;
  u64 min_row_ordinal = 0;
  u64 max_row_ordinal = 0;
  for (const auto& container : set.compressed_bitmap_containers) {
    if (container.base_row_ordinal % kContainerOrdinalSpan != 0 ||
        container.ordinal_span == 0 ||
        container.ordinal_span > kContainerOrdinalSpan ||
        container.cardinality == 0 ||
        container.cardinality > container.ordinal_span ||
        AddOverflows(container.base_row_ordinal, container.ordinal_span - 1) ||
        !HasContainerPayloadForType(container)) {
      return false;
    }
    if (!first_container && container.base_row_ordinal <= previous_base) {
      return false;
    }

    u64 payload_cardinality = 0;
    switch (container.type) {
      case CandidateSetCompressedBitmapContainerType::array_sparse:
        payload_cardinality =
            static_cast<u64>(container.array_offsets.size());
        for (std::size_t i = 0; i < container.array_offsets.size(); ++i) {
          const auto offset = container.array_offsets[i];
          if (offset >= container.ordinal_span ||
              (i != 0 && offset <= container.array_offsets[i - 1])) {
            return false;
          }
        }
        break;
      case CandidateSetCompressedBitmapContainerType::run: {
        u64 previous_end = 0;
        bool first_run = true;
        for (const auto& run : container.runs) {
          const u64 run_start = run.start_offset;
          const u64 run_end = run_start + run.run_length;
          if (run.run_length == 0 || run_start >= container.ordinal_span ||
              run_end > container.ordinal_span ||
              (!first_run && run_start <= previous_end) ||
              AddOverflows(payload_cardinality, run.run_length)) {
            return false;
          }
          payload_cardinality += run.run_length;
          previous_end = run_end;
          first_run = false;
        }
        break;
      }
      case CandidateSetCompressedBitmapContainerType::dense_bitmap:
        for (u32 word_index = 0;
             word_index < static_cast<u32>(container.bitmap_words.size());
             ++word_index) {
          u64 word = container.bitmap_words[word_index];
          while (word != 0) {
            const u32 bit = static_cast<u32>(__builtin_ctzll(word));
            const u32 offset = word_index * 64u + bit;
            if (offset >= container.ordinal_span) {
              return false;
            }
            ++payload_cardinality;
            word &= word - 1ull;
          }
        }
        break;
    }
    if (payload_cardinality != container.cardinality ||
        AddOverflows(cardinality, container.cardinality)) {
      return false;
    }

    const u64 container_min =
        container.base_row_ordinal + FirstContainerOffset(container);
    const u64 container_max =
        container.base_row_ordinal + LastContainerOffset(container);
    if (cardinality == 0) {
      min_row_ordinal = container_min;
    }
    max_row_ordinal = container_max;
    cardinality += container.cardinality;
    previous_base = container.base_row_ordinal;
    first_container = false;
  }
  return cardinality == set.compressed_bitmap_cardinality &&
         min_row_ordinal == set.compressed_bitmap_min_row_ordinal &&
         max_row_ordinal == set.compressed_bitmap_max_row_ordinal;
}

#if defined(__AVX512F__) && defined(__AVX512VPOPCNTDQ__)
u64 Avx512PopcountWords(const std::vector<u64>& words) {
  u64 count = 0;
  std::size_t i = 0;
  alignas(64) u64 lanes[8] = {};
  for (; i + 8 <= words.size(); i += 8) {
    const auto input =
        _mm512_loadu_si512(reinterpret_cast<const void*>(words.data() + i));
    const auto popcounts = _mm512_popcnt_epi64(input);
    _mm512_store_si512(reinterpret_cast<void*>(lanes), popcounts);
    for (const auto lane : lanes) {
      count += lane;
    }
  }
  for (; i < words.size(); ++i) {
    count += static_cast<u64>(__builtin_popcountll(words[i]));
  }
  return count;
}
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
u64 NeonPopcountWords(const std::vector<u64>& words) {
  u64 count = 0;
  std::size_t i = 0;
  alignas(16) u8 lanes[16] = {};
  for (; i + 2 <= words.size(); i += 2) {
    const auto values = vld1q_u64(words.data() + i);
    const auto bytes = vreinterpretq_u8_u64(values);
    const auto popcounts = vcntq_u8(bytes);
    vst1q_u8(lanes, popcounts);
    for (const auto lane : lanes) {
      count += lane;
    }
  }
  for (; i < words.size(); ++i) {
    count += static_cast<u64>(__builtin_popcountll(words[i]));
  }
  return count;
}
#endif

CompressedBitmapPopcountBackendKind SelectedBackend() {
#if defined(__AVX512F__) && defined(__AVX512VPOPCNTDQ__)
  return CompressedBitmapPopcountBackendKind::avx512_vpopcntdq;
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  return CompressedBitmapPopcountBackendKind::arm_neon_vcnt;
#else
  return CompressedBitmapPopcountBackendKind::scalar;
#endif
}

u64 CountWordsWithSelectedBackend(const std::vector<u64>& words) {
#if defined(__AVX512F__) && defined(__AVX512VPOPCNTDQ__)
  return Avx512PopcountWords(words);
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  return NeonPopcountWords(words);
#else
  return ScalarPopcountWords(words);
#endif
}

bool BackendIsSimd(CompressedBitmapPopcountBackendKind backend) {
  return backend == CompressedBitmapPopcountBackendKind::avx512_vpopcntdq ||
         backend == CompressedBitmapPopcountBackendKind::arm_neon_vcnt;
}

bool ReadHeader(const std::vector<byte>& artifact,
                u16* version,
                u16* header_bytes,
                u32* flags,
                u64* spill_epoch,
                u64* source_generation,
                u64* payload_bytes,
                u64* payload_checksum) {
  if (artifact.size() < kSpillHeaderBytes + sizeof(u64)) {
    return false;
  }
  *version = scratchbird::core::platform::LoadLittle16(artifact.data() + 8);
  *header_bytes =
      scratchbird::core::platform::LoadLittle16(artifact.data() + 10);
  *flags = scratchbird::core::platform::LoadLittle32(artifact.data() + 12);
  *spill_epoch =
      scratchbird::core::platform::LoadLittle64(artifact.data() + 16);
  *source_generation =
      scratchbird::core::platform::LoadLittle64(artifact.data() + 24);
  *payload_bytes =
      scratchbird::core::platform::LoadLittle64(artifact.data() + 32);
  *payload_checksum =
      scratchbird::core::platform::LoadLittle64(artifact.data() + 40);
  return true;
}

CompressedBitmapSpillResult FinishSpill(
    CompressedBitmapSpillClassification classification,
    CandidateSet set,
    std::vector<byte> artifact,
    std::vector<std::string> evidence) {
  CompressedBitmapSpillResult result;
  result.status = OkStatus();
  result.classification = classification;
  result.output = std::move(set);
  result.artifact = std::move(artifact);
  result.evidence = std::move(evidence);
  result.diagnostic =
      MakeDiagnostic(result.status, "SB_COMPRESSED_BITMAP_SPILL.OK",
                     "compressed_bitmap_spill.ok",
                     CompressedBitmapSpillClassificationName(classification));
  return result;
}

}  // namespace

const char* CompressedBitmapSpillClassificationName(
    CompressedBitmapSpillClassification classification) {
  switch (classification) {
    case CompressedBitmapSpillClassification::clean_reopen:
      return "clean_reopen";
    case CompressedBitmapSpillClassification::repaired_reopen:
      return "repaired_reopen";
    case CompressedBitmapSpillClassification::truncated:
      return "truncated";
    case CompressedBitmapSpillClassification::corrupt:
      return "corrupt";
    case CompressedBitmapSpillClassification::stale:
      return "stale";
    case CompressedBitmapSpillClassification::repair_refused:
      return "repair_refused";
  }
  return "unknown";
}

const char* CompressedBitmapPopcountBackendName(
    CompressedBitmapPopcountBackendKind backend) {
  switch (backend) {
    case CompressedBitmapPopcountBackendKind::scalar:
      return "scalar";
    case CompressedBitmapPopcountBackendKind::avx512_vpopcntdq:
      return "avx512_vpopcntdq";
    case CompressedBitmapPopcountBackendKind::arm_neon_vcnt:
      return "arm_neon_vcnt";
  }
  return "unknown";
}

CompressedBitmapPopcountResult CountCompressedBitmapWithSelectedBackend(
    const CandidateSet& set) {
  if (!ValidateCompressedBitmapSpillCandidateSet(set)) {
    return RefusePopcount("SB_COMPRESSED_BITMAP_SPILL.INVALID_CANDIDATE_SET",
                          "compressed_bitmap_spill.invalid_candidate_set",
                          "compressed_bitmap_popcount_invalid_candidate_set");
  }

  CompressedBitmapPopcountResult result;
  result.status = OkStatus();
  result.backend = SelectedBackend();
  result.simd_available = BackendIsSimd(result.backend);

  u64 cardinality = 0;
  for (const auto& container : set.compressed_bitmap_containers) {
    const auto words = DenseWordsFromContainer(container);
    const auto container_count = CountWordsWithSelectedBackend(words);
    if (container_count != container.cardinality ||
        AddOverflows(cardinality, container_count)) {
      return RefusePopcount("SB_COMPRESSED_BITMAP_SPILL.POPCOUNT_MISMATCH",
                            "compressed_bitmap_spill.popcount_mismatch",
                            "compressed_bitmap_container_cardinality_mismatch");
    }
    cardinality += container_count;
  }
  if (cardinality != set.compressed_bitmap_cardinality) {
    return RefusePopcount("SB_COMPRESSED_BITMAP_SPILL.POPCOUNT_MISMATCH",
                          "compressed_bitmap_spill.popcount_mismatch",
                          "compressed_bitmap_set_cardinality_mismatch");
  }

  result.cardinality = cardinality;
  result.evidence = BaseEvidence();
  result.evidence.push_back("compressed_bitmap.popcount.backend=" +
                            std::string(CompressedBitmapPopcountBackendName(
                                result.backend)));
  result.evidence.push_back("compressed_bitmap.popcount.simd_available=" +
                            std::string(result.simd_available ? "true"
                                                              : "false"));
  if (!result.simd_available) {
    result.evidence.push_back("compressed_bitmap.popcount.fallback_backend=scalar");
  }
  result.evidence.push_back("compressed_bitmap.popcount.container_level=true");
  result.evidence.push_back("compressed_bitmap.popcount.materialized_rows=false");
  result.evidence.push_back("compressed_bitmap.popcount.cardinality=" +
                            std::to_string(cardinality));
  result.diagnostic =
      MakeDiagnostic(result.status, "SB_COMPRESSED_BITMAP_SPILL.POPCOUNT_OK",
                     "compressed_bitmap_spill.popcount_ok",
                     CompressedBitmapPopcountBackendName(result.backend));
  return result;
}

CompressedBitmapSpillResult SerializeCompressedBitmapSpill(
    const CandidateSet& set,
    CompressedBitmapSpillDescriptor descriptor) {
  const auto popcount = CountCompressedBitmapWithSelectedBackend(set);
  if (!popcount.ok()) {
    return RefuseSpill(CompressedBitmapSpillClassification::corrupt,
                       popcount.diagnostic.diagnostic_code,
                       popcount.diagnostic.message_key,
                       popcount.refusal_reasons.empty()
                           ? "compressed_bitmap_spill_popcount_refused"
                           : popcount.refusal_reasons.front());
  }

  auto payload = SerializeCompressedBitmapCandidateSet(set);
  if (!payload.ok()) {
    return RefuseSpill(CompressedBitmapSpillClassification::corrupt,
                       payload.diagnostic.diagnostic_code,
                       payload.diagnostic.message_key,
                       payload.refusal_reasons.empty()
                           ? "compressed_bitmap_spill_payload_refused"
                           : payload.refusal_reasons.front());
  }

  std::vector<byte> artifact(kSpillMagic.begin(), kSpillMagic.end());
  Append16(&artifact, kSpillFormatVersion);
  Append16(&artifact, kSpillHeaderBytes);
  Append32(&artifact, kRequiredFlags);
  Append64(&artifact, descriptor.spill_epoch);
  Append64(&artifact, descriptor.source_generation);
  Append64(&artifact, static_cast<u64>(payload.serialized.size()));
  Append64(&artifact, Fnv1a64(payload.serialized.data(),
                              payload.serialized.size()));
  artifact.insert(artifact.end(), payload.serialized.begin(),
                  payload.serialized.end());
  Append64(&artifact, Fnv1a64(artifact.data(), artifact.size()));

  auto evidence = BaseEvidence();
  evidence.push_back("compressed_bitmap_spill.serialization=deterministic");
  evidence.push_back("compressed_bitmap_spill.spill_epoch=" +
                     std::to_string(descriptor.spill_epoch));
  evidence.push_back("compressed_bitmap_spill.source_generation=" +
                     std::to_string(descriptor.source_generation));
  evidence.push_back("compressed_bitmap_spill.payload_bytes=" +
                     std::to_string(payload.serialized.size()));
  evidence.push_back("compressed_bitmap_spill.cardinality=" +
                     std::to_string(set.compressed_bitmap_cardinality));
  evidence.push_back("compressed_bitmap_spill.container_count=" +
                     std::to_string(set.compressed_bitmap_containers.size()));
  AppendEvidence(&evidence, popcount.evidence);
  return FinishSpill(CompressedBitmapSpillClassification::clean_reopen, set,
                     std::move(artifact), std::move(evidence));
}

CompressedBitmapSpillResult OpenCompressedBitmapSpill(
    const std::vector<byte>& artifact,
    CompressedBitmapSpillDescriptor expected_descriptor,
    const CandidateSetAuthorityContext& authority) {
  if (artifact.size() < kSpillHeaderBytes + sizeof(u64)) {
    return RefuseSpill(CompressedBitmapSpillClassification::truncated,
                       "SB_COMPRESSED_BITMAP_SPILL.TRUNCATED",
                       "compressed_bitmap_spill.truncated",
                       "compressed_bitmap_spill_truncated_header");
  }
  if (!std::equal(kSpillMagic.begin(), kSpillMagic.end(), artifact.begin())) {
    return RefuseSpill(CompressedBitmapSpillClassification::corrupt,
                       "SB_COMPRESSED_BITMAP_SPILL.BAD_MAGIC",
                       "compressed_bitmap_spill.bad_magic",
                       "compressed_bitmap_spill_bad_magic");
  }

  u16 version = 0;
  u16 header_bytes = 0;
  u32 flags = 0;
  u64 spill_epoch = 0;
  u64 source_generation = 0;
  u64 payload_bytes = 0;
  u64 payload_checksum = 0;
  if (!ReadHeader(artifact, &version, &header_bytes, &flags, &spill_epoch,
                  &source_generation, &payload_bytes, &payload_checksum)) {
    return RefuseSpill(CompressedBitmapSpillClassification::truncated,
                       "SB_COMPRESSED_BITMAP_SPILL.TRUNCATED",
                       "compressed_bitmap_spill.truncated",
                       "compressed_bitmap_spill_truncated_header");
  }
  if (version != kSpillFormatVersion || header_bytes != kSpillHeaderBytes) {
    return RefuseSpill(CompressedBitmapSpillClassification::stale,
                       "SB_COMPRESSED_BITMAP_SPILL.STALE_FORMAT",
                       "compressed_bitmap_spill.stale_format",
                       "compressed_bitmap_spill_incompatible_version");
  }
  if ((flags & kRequiredFlags) != kRequiredFlags) {
    return RefuseSpill(CompressedBitmapSpillClassification::corrupt,
                       "SB_COMPRESSED_BITMAP_SPILL.RECHECK_CONTRACT_REQUIRED",
                       "compressed_bitmap_spill.recheck_contract_required",
                       "compressed_bitmap_spill_missing_recheck_contract");
  }
  if (spill_epoch != expected_descriptor.spill_epoch ||
      source_generation != expected_descriptor.source_generation) {
    return RefuseSpill(CompressedBitmapSpillClassification::stale,
                       "SB_COMPRESSED_BITMAP_SPILL.STALE_EPOCH",
                       "compressed_bitmap_spill.stale_epoch",
                       "compressed_bitmap_spill_stale_epoch_or_generation");
  }
  if (payload_bytes == 0 ||
      payload_bytes >
          std::numeric_limits<u64>::max() - kSpillHeaderBytes - sizeof(u64) ||
      artifact.size() !=
          kSpillHeaderBytes + static_cast<std::size_t>(payload_bytes) +
              sizeof(u64)) {
    return RefuseSpill(CompressedBitmapSpillClassification::truncated,
                       "SB_COMPRESSED_BITMAP_SPILL.TRUNCATED",
                       "compressed_bitmap_spill.truncated",
                       "compressed_bitmap_spill_truncated_payload");
  }
  const u64 stored_artifact_checksum =
      scratchbird::core::platform::LoadLittle64(artifact.data() +
                                                artifact.size() -
                                                    sizeof(u64));
  const u64 computed_artifact_checksum =
      Fnv1a64(artifact.data(), artifact.size() - sizeof(u64));
  if (stored_artifact_checksum != computed_artifact_checksum) {
    return RefuseSpill(CompressedBitmapSpillClassification::corrupt,
                       "SB_COMPRESSED_BITMAP_SPILL.CHECKSUM_MISMATCH",
                       "compressed_bitmap_spill.checksum_mismatch",
                       "compressed_bitmap_spill_artifact_checksum_mismatch");
  }
  const auto* payload_begin = artifact.data() + kSpillHeaderBytes;
  if (Fnv1a64(payload_begin, static_cast<std::size_t>(payload_bytes)) !=
      payload_checksum) {
    return RefuseSpill(CompressedBitmapSpillClassification::corrupt,
                       "SB_COMPRESSED_BITMAP_SPILL.PAYLOAD_CHECKSUM_MISMATCH",
                       "compressed_bitmap_spill.payload_checksum_mismatch",
                       "compressed_bitmap_spill_payload_checksum_mismatch");
  }

  std::vector<byte> payload(payload_begin,
                            payload_begin + static_cast<std::size_t>(
                                                payload_bytes));
  auto parsed = DeserializeCompressedBitmapCandidateSet(payload, authority);
  if (!parsed.ok()) {
    return RefuseSpill(CompressedBitmapSpillClassification::corrupt,
                       "SB_COMPRESSED_BITMAP_SPILL.PAYLOAD_INVALID",
                       "compressed_bitmap_spill.payload_invalid",
                       parsed.refusal_reasons.empty()
                           ? "compressed_bitmap_spill_payload_invalid"
                           : parsed.refusal_reasons.front());
  }
  const auto popcount = CountCompressedBitmapWithSelectedBackend(parsed.output);
  if (!popcount.ok()) {
    return RefuseSpill(CompressedBitmapSpillClassification::corrupt,
                       popcount.diagnostic.diagnostic_code,
                       popcount.diagnostic.message_key,
                       popcount.refusal_reasons.empty()
                           ? "compressed_bitmap_spill_popcount_refused"
                           : popcount.refusal_reasons.front());
  }

  auto evidence = BaseEvidence();
  evidence.push_back("compressed_bitmap_spill.reopen=clean");
  evidence.push_back("compressed_bitmap_spill.spill_epoch=" +
                     std::to_string(spill_epoch));
  evidence.push_back("compressed_bitmap_spill.source_generation=" +
                     std::to_string(source_generation));
  evidence.push_back("compressed_bitmap_spill.payload_checksum=validated");
  evidence.push_back("compressed_bitmap_spill.artifact_checksum=validated");
  evidence.push_back("compressed_bitmap_spill.cardinality=" +
                     std::to_string(parsed.output.compressed_bitmap_cardinality));
  evidence.push_back("compressed_bitmap_spill.row_ordinal_min=" +
                     std::to_string(
                         parsed.output.compressed_bitmap_min_row_ordinal));
  evidence.push_back("compressed_bitmap_spill.row_ordinal_max=" +
                     std::to_string(
                         parsed.output.compressed_bitmap_max_row_ordinal));
  evidence.push_back("compressed_bitmap_spill.container_count=" +
                     std::to_string(
                         parsed.output.compressed_bitmap_containers.size()));
  AppendEvidence(&evidence, popcount.evidence);
  return FinishSpill(CompressedBitmapSpillClassification::clean_reopen,
                     std::move(parsed.output), artifact, std::move(evidence));
}

CompressedBitmapSpillResult RepairOrOpenCompressedBitmapSpill(
    const std::vector<byte>& artifact,
    CompressedBitmapSpillDescriptor expected_descriptor,
    const CandidateSetAuthorityContext& authority,
    const CandidateSet* authoritative_rebuild_input,
    const CompressedBitmapRepairAdmission& admission) {
  auto opened = OpenCompressedBitmapSpill(artifact, expected_descriptor, authority);
  if (opened.ok()) {
    return opened;
  }
  const auto original_classification = opened.classification;
  const auto original_reason = opened.refusal_reasons.empty()
                                   ? std::string("unknown")
                                   : opened.refusal_reasons.front();

  if (!admission.repair_admitted || !admission.descriptor_match_proven ||
      !admission.authoritative_rebuild_input_proven ||
      admission.admitted_spill_epoch != expected_descriptor.spill_epoch ||
      admission.admitted_source_generation !=
          expected_descriptor.source_generation) {
    auto refused = RefuseSpill(
        CompressedBitmapSpillClassification::repair_refused,
        "SB_COMPRESSED_BITMAP_SPILL.REPAIR_NOT_ADMITTED",
        "compressed_bitmap_spill.repair_not_admitted",
        "compressed_bitmap_spill_repair_admission_not_proven");
    refused.evidence.push_back("compressed_bitmap_spill.original_classification=" +
                               std::string(
                                   CompressedBitmapSpillClassificationName(
                                       original_classification)));
    refused.evidence.push_back("compressed_bitmap_spill.original_reason=" +
                               original_reason);
    return refused;
  }
  if (authoritative_rebuild_input == nullptr) {
    auto refused = RefuseSpill(
        CompressedBitmapSpillClassification::repair_refused,
        "SB_COMPRESSED_BITMAP_SPILL.REPAIR_INPUT_MISSING",
        "compressed_bitmap_spill.repair_input_missing",
        "compressed_bitmap_spill_missing_authoritative_rebuild_input");
    refused.evidence.push_back("compressed_bitmap_spill.original_classification=" +
                               std::string(
                                   CompressedBitmapSpillClassificationName(
                                       original_classification)));
    return refused;
  }

  auto rebuilt =
      SerializeCompressedBitmapSpill(*authoritative_rebuild_input,
                                     expected_descriptor);
  if (!rebuilt.ok()) {
    auto refused = RefuseSpill(
        CompressedBitmapSpillClassification::repair_refused,
        "SB_COMPRESSED_BITMAP_SPILL.REPAIR_INPUT_INVALID",
        "compressed_bitmap_spill.repair_input_invalid",
        rebuilt.refusal_reasons.empty()
            ? "compressed_bitmap_spill_authoritative_rebuild_input_invalid"
            : rebuilt.refusal_reasons.front());
    refused.evidence.push_back("compressed_bitmap_spill.original_classification=" +
                               std::string(
                                   CompressedBitmapSpillClassificationName(
                                       original_classification)));
    return refused;
  }

  auto reopened =
      OpenCompressedBitmapSpill(rebuilt.artifact, expected_descriptor, authority);
  if (!reopened.ok()) {
    auto refused = RefuseSpill(
        CompressedBitmapSpillClassification::repair_refused,
        "SB_COMPRESSED_BITMAP_SPILL.REPAIRED_REOPEN_FAILED",
        "compressed_bitmap_spill.repaired_reopen_failed",
        reopened.refusal_reasons.empty()
            ? "compressed_bitmap_spill_repaired_reopen_failed"
            : reopened.refusal_reasons.front());
    refused.evidence.push_back("compressed_bitmap_spill.original_classification=" +
                               std::string(
                                   CompressedBitmapSpillClassificationName(
                                       original_classification)));
    return refused;
  }

  reopened.classification = CompressedBitmapSpillClassification::repaired_reopen;
  reopened.diagnostic =
      MakeDiagnostic(reopened.status, "SB_COMPRESSED_BITMAP_SPILL.REPAIRED",
                     "compressed_bitmap_spill.repaired",
                     "compressed_bitmap_spill_repaired_reopen");
  reopened.evidence.push_back("compressed_bitmap_spill.reopen=repaired");
  reopened.evidence.push_back("compressed_bitmap_spill.original_classification=" +
                              std::string(
                                  CompressedBitmapSpillClassificationName(
                                      original_classification)));
  reopened.evidence.push_back("compressed_bitmap_spill.original_reason=" +
                              original_reason);
  reopened.evidence.push_back("compressed_bitmap_spill.repair.admitted=true");
  reopened.evidence.push_back(
      "compressed_bitmap_spill.repair.authoritative_rebuild_input=true");
  reopened.evidence.push_back("compressed_bitmap_spill.repair.non_authoritative=true");
  if (!admission.proof_detail.empty()) {
    reopened.evidence.push_back("compressed_bitmap_spill.repair.proof_detail=" +
                                admission.proof_detail);
  }
  return reopened;
}

}  // namespace scratchbird::core::index
