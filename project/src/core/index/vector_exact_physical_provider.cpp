// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "vector_exact_physical_provider.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'V', 'E',
                                               'X', 'P', '0', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u32 kMaxDimensions = 16384;
inline constexpr u64 kMaxRows = 1000000;
inline constexpr u64 kFnvOffset = 14695981039346656037ull;
inline constexpr u64 kFnvPrime = 1099511628211ull;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status WarnStatus() {
  return {StatusCode::ok, Severity::warning, Subsystem::engine};
}
Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

bool SameFormatVersion(PageExtentSummaryFormatVersion left,
                       PageExtentSummaryFormatVersion right) {
  return left.major == right.major && left.minor == right.minor;
}

bool LocatorValid(const TextInvertedRowLocator& locator) {
  return locator.row_ordinal > 0 &&
         PageExtentSummaryUuidTextValid(locator.row_uuid) &&
         PageExtentSummaryUuidTextValid(locator.version_uuid);
}

bool LocatorLess(const TextInvertedRowLocator& left,
                 const TextInvertedRowLocator& right) {
  return std::tie(left.row_ordinal, left.row_uuid, left.version_uuid) <
         std::tie(right.row_ordinal, right.row_uuid, right.version_uuid);
}

bool LocatorEqual(const TextInvertedRowLocator& left,
                  const TextInvertedRowLocator& right) {
  return !LocatorLess(left, right) && !LocatorLess(right, left);
}

bool FiniteVector(const std::vector<float>& values) {
  return std::all_of(values.begin(), values.end(),
                     [](float value) { return std::isfinite(value); });
}

bool SupportedProfile(VectorExactElementProfile profile) {
  return profile == VectorExactElementProfile::fp32 ||
         profile == VectorExactElementProfile::fp16 ||
         profile == VectorExactElementProfile::int8;
}

bool SupportedMetric(VectorExactMetricKind metric) {
  return metric == VectorExactMetricKind::l2 ||
         metric == VectorExactMetricKind::cosine ||
         metric == VectorExactMetricKind::inner_product;
}

bool DescriptorAuthorityClean(const VectorExactDescriptor& descriptor) {
  return !descriptor.parser_finality_authority_claimed &&
         !descriptor.donor_finality_authority_claimed &&
         !descriptor.provider_finality_authority_claimed &&
         !descriptor.index_finality_authority_claimed &&
         !descriptor.write_ahead_log_finality_authority_claimed;
}

bool MetricAuthorityClean(const VectorExactMetricResource& metric) {
  return !metric.parser_finality_authority_claimed &&
         !metric.donor_finality_authority_claimed &&
         !metric.provider_finality_authority_claimed &&
         !metric.index_finality_authority_claimed &&
         !metric.write_ahead_log_finality_authority_claimed;
}

bool RecheckProofAuthorityClean(const VectorExactRecheckProof& proof) {
  return !proof.parser_finality_authority_claimed &&
         !proof.donor_finality_authority_claimed &&
         !proof.provider_finality_authority_claimed &&
         !proof.index_finality_authority_claimed &&
         !proof.write_ahead_log_finality_authority_claimed &&
         !proof.visibility_authority_claimed &&
         !proof.security_authority_claimed &&
         !proof.transaction_finality_authority_claimed;
}

bool DescriptorSafe(const VectorExactDescriptor& descriptor) {
  return descriptor.dimensions > 0 &&
         descriptor.dimensions <= kMaxDimensions &&
         descriptor.descriptor_epoch > 0 &&
         SupportedProfile(descriptor.element_profile) &&
         descriptor.deterministic &&
         descriptor.descriptor_safe &&
         !descriptor.descriptor_store_scan &&
         !descriptor.behavior_store_scan &&
         DescriptorAuthorityClean(descriptor) &&
         (descriptor.element_profile != VectorExactElementProfile::int8 ||
          (std::isfinite(descriptor.int8_scale) &&
           descriptor.int8_scale > 0.0 &&
           descriptor.int8_zero_point >= -128 &&
           descriptor.int8_zero_point <= 127));
}

bool MetricSafe(const VectorExactMetricResource& metric) {
  return PageExtentSummaryUuidTextValid(metric.metric_resource_uuid) &&
         metric.metric_resource_epoch > 0 &&
         SupportedMetric(metric.metric_kind) &&
         metric.deterministic &&
         metric.safe &&
         !metric.descriptor_store_scan &&
         !metric.behavior_store_scan &&
         !metric.contract_only_fallback &&
         !metric.provider_only_fallback &&
         MetricAuthorityClean(metric);
}

bool RecheckProofValid(const VectorExactRecheckProof& proof) {
  return proof.proof_supplied &&
         proof.exact_source_vector_available &&
         proof.exact_rerank_proof_supplied &&
         proof.mga_recheck_required &&
         proof.security_recheck_required &&
         !proof.evidence_ref.empty() &&
         RecheckProofAuthorityClean(proof);
}

bool ProviderAuthorityClean(const VectorExactPhysicalProvider& provider) {
  return provider.candidate_evidence_only &&
         provider.exact_source_recheck_required &&
         provider.exact_rerank_proof_required &&
         provider.mga_recheck_required &&
         provider.security_recheck_required &&
         !provider.descriptor_store_scan &&
         !provider.behavior_store_scan &&
         !provider.contract_only_fallback &&
         !provider.provider_only_fallback &&
         !provider.visibility_authority_claimed &&
         !provider.security_authority_claimed &&
         !provider.transaction_finality_authority_claimed &&
         !provider.parser_finality_authority_claimed &&
         !provider.donor_finality_authority_claimed &&
         !provider.provider_finality_authority_claimed &&
         !provider.index_finality_authority_claimed &&
         !provider.write_ahead_log_finality_authority_claimed;
}

u32 EncodedPayloadBytes(const VectorExactDescriptor& descriptor) {
  switch (descriptor.element_profile) {
    case VectorExactElementProfile::fp32:
      return descriptor.dimensions * static_cast<u32>(sizeof(float));
    case VectorExactElementProfile::fp16:
      return descriptor.dimensions * static_cast<u32>(sizeof(u16));
    case VectorExactElementProfile::int8:
      return descriptor.dimensions;
  }
  return 0;
}

bool StoredRowValid(const VectorExactStoredRow& row,
                    const VectorExactDescriptor& descriptor) {
  return LocatorValid(row.locator) &&
         row.encoded_payload.size() == EncodedPayloadBytes(descriptor);
}

bool ProviderValid(const VectorExactPhysicalProvider& provider) {
  if (provider.artifact_kind != kVectorExactPhysicalProviderArtifactKind ||
      !SameFormatVersion(provider.format_version,
                         {kVectorExactPhysicalProviderCurrentMajor,
                          kVectorExactPhysicalProviderCurrentMinor}) ||
      !PageExtentSummaryUuidTextValid(provider.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.index_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.provider_uuid) ||
      provider.base_generation == 0 ||
      provider.provider_generation == 0 ||
      !DescriptorSafe(provider.descriptor) ||
      !MetricSafe(provider.metric) ||
      !provider.encoded_payloads_present ||
      !provider.fp32_payloads_supported ||
      !provider.fp16_payloads_supported ||
      !provider.int8_payloads_supported ||
      !provider.exact_decode_scoring_present ||
      !provider.batched_query_present ||
      !provider.metadata_prefilter_present ||
      !provider.candidate_set_input_present ||
      !provider.top_k_heap_present ||
      !provider.exact_rerank_present ||
      !provider.scalar_kernel_present ||
      !ProviderAuthorityClean(provider)) {
    return false;
  }
  for (std::size_t i = 0; i < provider.rows.size(); ++i) {
    if (!StoredRowValid(provider.rows[i], provider.descriptor) ||
        (i > 0 &&
         !LocatorLess(provider.rows[i - 1].locator, provider.rows[i].locator))) {
      return false;
    }
  }
  return true;
}

void AppendU8(std::vector<byte>* out, byte value) { out->push_back(value); }

void AppendBool(std::vector<byte>* out, bool value) {
  AppendU8(out, static_cast<byte>(value ? 1 : 0));
}

void AppendU32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u32));
  StoreLittle32(out->data() + offset, value);
}

void AppendI32(std::vector<byte>* out, i32 value) {
  AppendU32(out, static_cast<u32>(value));
}

void AppendU64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u64));
  StoreLittle64(out->data() + offset, value);
}

void AppendF64(std::vector<byte>* out, double value) {
  static_assert(sizeof(double) == sizeof(u64),
                "ScratchBird vector serialization requires 64-bit double");
  u64 bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU64(out, bits);
}

void AppendString(std::vector<byte>* out, const std::string& value) {
  AppendU32(out, static_cast<u32>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void AppendBytes(std::vector<byte>* out, const std::vector<byte>& value) {
  AppendU32(out, static_cast<u32>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void AppendLocator(std::vector<byte>* out,
                   const TextInvertedRowLocator& locator) {
  AppendU64(out, locator.row_ordinal);
  AppendString(out, locator.row_uuid);
  AppendString(out, locator.version_uuid);
}

u64 ComputeChecksum(std::vector<byte> bytes) {
  if (bytes.size() >= kHeaderBytes) {
    StoreLittle64(bytes.data() + 16, 0);
  }
  u64 hash = kFnvOffset;
  for (byte value : bytes) {
    hash ^= static_cast<u64>(value);
    hash *= kFnvPrime;
  }
  return hash == 0 ? 1 : hash;
}

class Reader {
 public:
  explicit Reader(const std::vector<byte>& bytes) : bytes_(bytes) {}
  void SetOffset(std::size_t offset) { offset_ = offset; }
  bool ReadU8(byte* out) {
    if (offset_ + 1 > bytes_.size()) return false;
    *out = bytes_[offset_++];
    return true;
  }
  bool ReadBool(bool* out) {
    byte value = 0;
    if (!ReadU8(&value) || value > 1) return false;
    *out = value != 0;
    return true;
  }
  bool ReadU32(u32* out) {
    if (offset_ + sizeof(u32) > bytes_.size()) return false;
    *out = LoadLittle32(bytes_.data() + offset_);
    offset_ += sizeof(u32);
    return true;
  }
  bool ReadI32(i32* out) {
    u32 value = 0;
    if (!ReadU32(&value)) return false;
    *out = static_cast<i32>(value);
    return true;
  }
  bool ReadU64(u64* out) {
    if (offset_ + sizeof(u64) > bytes_.size()) return false;
    *out = LoadLittle64(bytes_.data() + offset_);
    offset_ += sizeof(u64);
    return true;
  }
  bool ReadF64(double* out) {
    u64 bits = 0;
    if (!ReadU64(&bits)) return false;
    std::memcpy(out, &bits, sizeof(bits));
    return std::isfinite(*out);
  }
  bool ReadString(std::string* out) {
    u32 size = 0;
    if (!ReadU32(&size) || offset_ + size > bytes_.size()) return false;
    out->assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }
  bool ReadBytes(std::vector<byte>* out) {
    u32 size = 0;
    if (!ReadU32(&size) || offset_ + size > bytes_.size()) return false;
    out->assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += size;
    return true;
  }
  bool ReadLocator(TextInvertedRowLocator* out) {
    return ReadU64(&out->row_ordinal) &&
           ReadString(&out->row_uuid) &&
           ReadString(&out->version_uuid);
  }
  bool Done() const { return offset_ == bytes_.size(); }

 private:
  const std::vector<byte>& bytes_;
  std::size_t offset_ = 0;
};

VectorExactBuildResult BuildFailure(std::string code,
                                    std::string key,
                                    std::string detail = {}) {
  VectorExactBuildResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeVectorExactPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

VectorExactOpenResult OpenFailure(VectorExactOpenClass open_class,
                                  std::string code,
                                  std::string key,
                                  std::string detail = {}) {
  VectorExactOpenResult result;
  result.status = open_class == VectorExactOpenClass::stale_format ||
                          open_class == VectorExactOpenClass::stale_generation ||
                          open_class ==
                              VectorExactOpenClass::stale_descriptor_epoch ||
                          open_class ==
                              VectorExactOpenClass::stale_metric_epoch
                      ? WarnStatus()
                      : ErrorStatus();
  result.open_class = open_class;
  result.fail_closed = true;
  result.restricted_repair_required =
      open_class == VectorExactOpenClass::bad_checksum ||
      open_class == VectorExactOpenClass::corrupt_payload;
  result.diagnostic = MakeVectorExactPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_vector_exact_physical_provider");
  if (result.restricted_repair_required) {
    result.actions.push_back(
        "repair_requires_authoritative_source_vectors_and_exact_recheck");
  }
  return result;
}

VectorExactQueryResult QueryFailure(std::string code,
                                    std::string key,
                                    std::string detail = {}) {
  VectorExactQueryResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeVectorExactPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.evidence.push_back(kVectorExactPhysicalProviderSearchKey);
  result.evidence.push_back(
      "vector_exact_physical_provider.candidates_refused=true");
  result.evidence.push_back(
      "vector_exact_physical_provider.exact_source_recheck_required=true");
  result.evidence.push_back(
      "vector_exact_physical_provider.mga_recheck_required=true");
  result.evidence.push_back(
      "vector_exact_physical_provider.security_recheck_required=true");
  return result;
}

VectorExactMutationResult MutationFailure(VectorExactPhysicalProvider provider,
                                          std::string code,
                                          std::string key,
                                          std::string detail = {}) {
  VectorExactMutationResult result;
  result.status = ErrorStatus();
  result.provider = std::move(provider);
  result.fail_closed = true;
  result.diagnostic = MakeVectorExactPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_vector_exact_maintenance_mutation");
  return result;
}

bool LowerScoreBetter(VectorExactMetricKind metric) {
  return metric != VectorExactMetricKind::inner_product;
}

bool ScoreBetter(double left_score,
                 const TextInvertedRowLocator& left_locator,
                 double right_score,
                 const TextInvertedRowLocator& right_locator,
                 bool lower_score_better) {
  constexpr double kEpsilon = 1e-12;
  if (std::fabs(left_score - right_score) > kEpsilon) {
    return lower_score_better ? left_score < right_score
                              : left_score > right_score;
  }
  return LocatorLess(left_locator, right_locator);
}

bool WorseForHeap(const VectorExactCandidate& left,
                  const VectorExactCandidate& right) {
  return ScoreBetter(left.score, left.locator, right.score, right.locator,
                     left.lower_score_better);
}

float ReadF32(const byte* data) {
  u32 bits = LoadLittle32(data);
  float value = 0.0F;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void AppendF32(std::vector<byte>* out, float value) {
  u32 bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU32(out, bits);
}

double ScoreVectorPair(const std::vector<float>& left,
                       const std::vector<float>& right,
                       VectorExactMetricKind metric,
                       u64* scalar_ops) {
  double dot = 0.0;
  double left_norm = 0.0;
  double right_norm = 0.0;
  double l2 = 0.0;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const double l = left[i];
    const double r = right[i];
    dot += l * r;
    left_norm += l * l;
    right_norm += r * r;
    const double delta = l - r;
    l2 += delta * delta;
    ++*scalar_ops;
  }
  switch (metric) {
    case VectorExactMetricKind::l2:
      return l2;
    case VectorExactMetricKind::cosine:
      if (left_norm == 0.0 || right_norm == 0.0) {
        return std::numeric_limits<double>::infinity();
      }
      return 1.0 - (dot / (std::sqrt(left_norm) * std::sqrt(right_norm)));
    case VectorExactMetricKind::inner_product:
      return dot;
  }
  return std::numeric_limits<double>::infinity();
}

void SetProviderEvidence(VectorExactPhysicalProvider* provider) {
  provider->evidence = {
      kVectorExactPhysicalProviderSearchKey,
      "vector_exact_physical_provider.artifact_identity_deterministic=true",
      "vector_exact_physical_provider.checksumed_serialization=true",
      "vector_exact_physical_provider.fp32_payloads_supported=true",
      "vector_exact_physical_provider.fp16_payloads_supported=true",
      "vector_exact_physical_provider.int8_payloads_supported=true",
      "vector_exact_physical_provider.scalar_kernel_available=true",
      "vector_exact_physical_provider.batched_query_evaluation=true",
      "vector_exact_physical_provider.metadata_prefilter_input=true",
      "vector_exact_physical_provider.candidate_set_input=true",
      "vector_exact_physical_provider.top_k_heap=true",
      "vector_exact_physical_provider.exact_rerank=true",
      "vector_exact_physical_provider.candidate_evidence_only=true",
      "vector_exact_physical_provider.index_finality_authority=false",
      "vector_exact_physical_provider.write_ahead_log_authority=false"};
}

bool SourceRowValid(const VectorExactSourceRow& row,
                    const VectorExactDescriptor& descriptor) {
  return LocatorValid(row.locator) &&
         row.vector.size() == descriptor.dimensions &&
         FiniteVector(row.vector);
}

bool BuildRequestValid(const VectorExactBuildRequest& request) {
  return PageExtentSummaryUuidTextValid(request.relation_uuid) &&
         PageExtentSummaryUuidTextValid(request.index_uuid) &&
         PageExtentSummaryUuidTextValid(request.provider_uuid) &&
         request.base_generation > 0 &&
         request.provider_generation > 0 &&
         DescriptorSafe(request.descriptor) &&
         MetricSafe(request.metric) &&
         RecheckProofValid(request.recheck_proof) &&
         std::all_of(request.rows.begin(), request.rows.end(),
                     [&](const auto& row) {
                       return SourceRowValid(row, request.descriptor);
                     });
}

const VectorExactStoredRow* FindStoredRow(
    const VectorExactPhysicalProvider& provider,
    const TextInvertedRowLocator& locator) {
  auto iter = std::lower_bound(
      provider.rows.begin(), provider.rows.end(), locator,
      [](const VectorExactStoredRow& row, const TextInvertedRowLocator& value) {
        return LocatorLess(row.locator, value);
      });
  if (iter == provider.rows.end() || !LocatorEqual(iter->locator, locator)) {
    return nullptr;
  }
  return &*iter;
}

bool CandidateSetContains(const std::vector<TextInvertedRowLocator>& locators,
                          const TextInvertedRowLocator& locator) {
  return std::binary_search(locators.begin(), locators.end(), locator,
                            LocatorLess);
}

bool CandidateSetValid(const std::vector<TextInvertedRowLocator>& locators) {
  return std::all_of(locators.begin(), locators.end(), LocatorValid);
}

bool SourceMatchesStoredPayload(const VectorExactSourceRow& source,
                                const VectorExactStoredRow& stored,
                                const VectorExactDescriptor& descriptor) {
  return LocatorEqual(source.locator, stored.locator) &&
         EncodeVectorExactPayload(source.vector, descriptor) ==
             stored.encoded_payload;
}

bool RuntimeRequestSafe(const VectorExactQueryRequest& request) {
  return ProviderValid(request.provider) &&
         RecheckProofValid(request.recheck_proof) &&
         request.descriptor_epoch_current &&
         request.metric_resource_epoch_current &&
         !request.descriptor_store_scan &&
         !request.behavior_store_scan &&
         !request.contract_only_fallback &&
         !request.provider_only_fallback &&
         !request.queries.empty();
}

VectorExactStoredRow StoredRowFromSource(const VectorExactSourceRow& source,
                                         const VectorExactDescriptor& desc) {
  VectorExactStoredRow row;
  row.locator = source.locator;
  row.encoded_payload = EncodeVectorExactPayload(source.vector, desc);
  return row;
}

}  // namespace

u16 VectorExactEncodeFloat16(float value) {
  if (std::isnan(value)) {
    return 0x7e00u;
  }
  if (std::isinf(value)) {
    return value < 0.0F ? 0xfc00u : 0x7c00u;
  }
  u32 bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const u32 sign = (bits >> 16) & 0x8000u;
  i32 exponent = static_cast<i32>((bits >> 23) & 0xffu) - 127 + 15;
  u32 mantissa = bits & 0x7fffffu;
  if (exponent <= 0) {
    if (exponent < -10) {
      return static_cast<u16>(sign);
    }
    mantissa |= 0x800000u;
    const u32 shift = static_cast<u32>(14 - exponent);
    u32 rounded = mantissa >> shift;
    if ((mantissa >> (shift - 1)) & 1u) {
      ++rounded;
    }
    return static_cast<u16>(sign | rounded);
  }
  if (exponent >= 31) {
    return static_cast<u16>(sign | 0x7c00u);
  }
  mantissa += 0x1000u;
  if (mantissa & 0x800000u) {
    mantissa = 0;
    ++exponent;
    if (exponent >= 31) {
      return static_cast<u16>(sign | 0x7c00u);
    }
  }
  return static_cast<u16>(sign | (static_cast<u32>(exponent) << 10) |
                          (mantissa >> 13));
}

float VectorExactDecodeFloat16(u16 value) {
  const u32 sign = (static_cast<u32>(value) & 0x8000u) << 16;
  u32 exponent = (static_cast<u32>(value) >> 10) & 0x1fu;
  u32 mantissa = static_cast<u32>(value) & 0x03ffu;
  u32 bits = 0;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 1;
      while ((mantissa & 0x0400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x03ffu;
      bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  float decoded = 0.0F;
  std::memcpy(&decoded, &bits, sizeof(decoded));
  return decoded;
}

std::vector<byte> EncodeVectorExactPayload(
    const std::vector<float>& values,
    const VectorExactDescriptor& descriptor) {
  if (values.size() != descriptor.dimensions || !DescriptorSafe(descriptor) ||
      !FiniteVector(values)) {
    return {};
  }
  std::vector<byte> encoded;
  encoded.reserve(EncodedPayloadBytes(descriptor));
  switch (descriptor.element_profile) {
    case VectorExactElementProfile::fp32:
      for (float value : values) {
        AppendF32(&encoded, value);
      }
      break;
    case VectorExactElementProfile::fp16:
      for (float value : values) {
        const std::size_t offset = encoded.size();
        encoded.resize(offset + sizeof(u16));
        StoreLittle16(encoded.data() + offset, VectorExactEncodeFloat16(value));
      }
      break;
    case VectorExactElementProfile::int8:
      for (float value : values) {
        const double quantized =
            std::round(static_cast<double>(value) / descriptor.int8_scale) +
            descriptor.int8_zero_point;
        const int clamped = std::max(-128, std::min(127,
                                                    static_cast<int>(quantized)));
        encoded.push_back(static_cast<byte>(static_cast<signed char>(clamped)));
      }
      break;
  }
  return encoded;
}

std::vector<float> DecodeVectorExactPayload(
    const std::vector<byte>& encoded_payload,
    const VectorExactDescriptor& descriptor) {
  if (!DescriptorSafe(descriptor) ||
      encoded_payload.size() != EncodedPayloadBytes(descriptor)) {
    return {};
  }
  std::vector<float> values;
  values.reserve(descriptor.dimensions);
  switch (descriptor.element_profile) {
    case VectorExactElementProfile::fp32:
      for (u32 i = 0; i < descriptor.dimensions; ++i) {
        values.push_back(ReadF32(encoded_payload.data() + i * sizeof(float)));
      }
      break;
    case VectorExactElementProfile::fp16:
      for (u32 i = 0; i < descriptor.dimensions; ++i) {
        const u16 half =
            LoadLittle16(encoded_payload.data() + i * sizeof(u16));
        values.push_back(VectorExactDecodeFloat16(half));
      }
      break;
    case VectorExactElementProfile::int8:
      for (byte value : encoded_payload) {
        const int quantized =
            value <= 127 ? static_cast<int>(value)
                         : static_cast<int>(value) - 256;
        values.push_back(static_cast<float>(
            (quantized - descriptor.int8_zero_point) *
            descriptor.int8_scale));
      }
      break;
  }
  return FiniteVector(values) ? values : std::vector<float>{};
}

VectorExactBuildResult BuildVectorExactPhysicalProvider(
    const VectorExactBuildRequest& request) {
  if (!RecheckProofAuthorityClean(request.recheck_proof) ||
      !DescriptorAuthorityClean(request.descriptor) ||
      !MetricAuthorityClean(request.metric)) {
    return BuildFailure(
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED",
        "index.vector_exact_physical_provider.authority_claim_refused",
        "vector exact provider is candidate evidence only and cannot claim transaction, visibility, security, index, provider, parser, donor, or write-ahead authority");
  }
  if (!RecheckProofValid(request.recheck_proof)) {
    return BuildFailure(
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_exact_physical_provider.missing_exact_recheck",
        "vector exact provider build requires exact source vector, exact rerank, MGA, and security proof");
  }
  if (!SupportedProfile(request.descriptor.element_profile)) {
    return BuildFailure(
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.UNSUPPORTED_ELEMENT_PROFILE",
        "index.vector_exact_physical_provider.unsupported_element_profile");
  }
  if (!DescriptorSafe(request.descriptor)) {
    return BuildFailure(
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.STALE_DESCRIPTOR",
        "index.vector_exact_physical_provider.stale_descriptor",
        "vector descriptor must be current deterministic safe and dimensioned with no authority claims");
  }
  if (!MetricSafe(request.metric)) {
    return BuildFailure(
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.UNSAFE_METRIC_RESOURCE",
        "index.vector_exact_physical_provider.unsafe_metric_resource",
        "metric resource requires current safe deterministic UUID and epoch with no fallback or authority claims");
  }
  for (const auto& row : request.rows) {
    if (row.vector.size() != request.descriptor.dimensions ||
        !FiniteVector(row.vector)) {
      return BuildFailure(
          "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.DIMENSION_MISMATCH",
          "index.vector_exact_physical_provider.dimension_mismatch");
    }
  }
  if (!BuildRequestValid(request)) {
    return BuildFailure(
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.BUILD_REFUSED",
        "index.vector_exact_physical_provider.build_refused",
        "build requires valid identities, generations, descriptor, metric, rows, and recheck proof");
  }

  std::vector<VectorExactStoredRow> rows;
  rows.reserve(request.rows.size());
  for (const auto& source : request.rows) {
    auto stored = StoredRowFromSource(source, request.descriptor);
    if (!StoredRowValid(stored, request.descriptor)) {
      return BuildFailure(
          "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.DIMENSION_MISMATCH",
          "index.vector_exact_physical_provider.dimension_mismatch");
    }
    rows.push_back(std::move(stored));
  }
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
  for (std::size_t i = 1; i < rows.size(); ++i) {
    if (LocatorEqual(rows[i - 1].locator, rows[i].locator)) {
      return BuildFailure(
          "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.DUPLICATE_ROW_LOCATOR",
          "index.vector_exact_physical_provider.duplicate_row_locator");
    }
  }

  VectorExactPhysicalProvider provider;
  provider.relation_uuid = request.relation_uuid;
  provider.index_uuid = request.index_uuid;
  provider.provider_uuid = request.provider_uuid;
  provider.base_generation = request.base_generation;
  provider.provider_generation = request.provider_generation;
  provider.mutation_generation_evidence = request.provider_generation;
  provider.descriptor = request.descriptor;
  provider.metric = request.metric;
  provider.rows = std::move(rows);
  SetProviderEvidence(&provider);
  if (!ProviderValid(provider)) {
    return BuildFailure("INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.BUILD_CORRUPT",
                        "index.vector_exact_physical_provider.build_corrupt");
  }

  VectorExactBuildResult result;
  result.status = OkStatus();
  result.provider = std::move(provider);
  result.built = true;
  result.fail_closed = false;
  return result;
}

VectorExactSerializeResult SerializeVectorExactPhysicalProvider(
    const VectorExactPhysicalProvider& provider) {
  VectorExactSerializeResult result;
  if (!ProviderValid(provider)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeVectorExactPhysicalProviderDiagnostic(
        result.status,
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.SERIALIZE_REFUSED",
        "index.vector_exact_physical_provider.serialize_refused");
    return result;
  }
  std::vector<byte> bytes;
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  AppendU32(&bytes, kVectorExactPhysicalProviderCurrentMajor);
  AppendU32(&bytes, kVectorExactPhysicalProviderCurrentMinor);
  AppendU64(&bytes, 0);
  AppendString(&bytes, provider.relation_uuid);
  AppendString(&bytes, provider.index_uuid);
  AppendString(&bytes, provider.provider_uuid);
  AppendU64(&bytes, provider.base_generation);
  AppendU64(&bytes, provider.provider_generation);
  AppendU32(&bytes, provider.descriptor.dimensions);
  AppendU32(&bytes, static_cast<u32>(provider.descriptor.element_profile));
  AppendU64(&bytes, provider.descriptor.descriptor_epoch);
  AppendF64(&bytes, provider.descriptor.int8_scale);
  AppendI32(&bytes, provider.descriptor.int8_zero_point);
  AppendString(&bytes, provider.metric.metric_resource_uuid);
  AppendU64(&bytes, provider.metric.metric_resource_epoch);
  AppendU32(&bytes, static_cast<u32>(provider.metric.metric_kind));
  AppendU64(&bytes, provider.mutation_generation_evidence);
  AppendBool(&bytes, provider.descriptor.deterministic);
  AppendBool(&bytes, provider.descriptor.descriptor_safe);
  AppendBool(&bytes, provider.metric.deterministic);
  AppendBool(&bytes, provider.metric.safe);
  AppendU64(&bytes, static_cast<u64>(provider.rows.size()));
  for (const auto& row : provider.rows) {
    AppendLocator(&bytes, row.locator);
    AppendBytes(&bytes, row.encoded_payload);
  }
  result.checksum = ComputeChecksum(bytes);
  StoreLittle64(bytes.data() + 16, result.checksum);
  result.status = OkStatus();
  result.bytes = std::move(bytes);
  return result;
}

VectorExactOpenResult OpenVectorExactPhysicalProvider(
    const VectorExactOpenRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return OpenFailure(
        VectorExactOpenClass::missing_exact_recheck_proof,
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_exact_physical_provider.missing_exact_recheck");
  }
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(VectorExactOpenClass::corrupt_payload,
                       "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_exact_physical_provider.corrupt_payload");
  }
  const u64 stored_checksum = LoadLittle64(request.bytes.data() + 16);
  if (stored_checksum == 0 ||
      stored_checksum != ComputeChecksum(request.bytes)) {
    return OpenFailure(VectorExactOpenClass::bad_checksum,
                       "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.BAD_CHECKSUM",
                       "index.vector_exact_physical_provider.bad_checksum");
  }

  Reader reader(request.bytes);
  reader.SetOffset(8);
  VectorExactPhysicalProvider provider;
  u32 major = 0;
  u32 minor = 0;
  u64 checksum = 0;
  u32 profile = 0;
  u32 metric_kind = 0;
  u64 row_count = 0;
  if (!reader.ReadU32(&major) ||
      !reader.ReadU32(&minor) ||
      !reader.ReadU64(&checksum)) {
    return OpenFailure(VectorExactOpenClass::corrupt_payload,
                       "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_exact_physical_provider.corrupt_payload");
  }
  provider.format_version = {major, minor};
  if (!SameFormatVersion(provider.format_version,
                         {kVectorExactPhysicalProviderCurrentMajor,
                          kVectorExactPhysicalProviderCurrentMinor})) {
    return OpenFailure(VectorExactOpenClass::stale_format,
                       "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.STALE_FORMAT",
                       "index.vector_exact_physical_provider.stale_format");
  }
  if (!reader.ReadString(&provider.relation_uuid) ||
      !reader.ReadString(&provider.index_uuid) ||
      !reader.ReadString(&provider.provider_uuid) ||
      !reader.ReadU64(&provider.base_generation) ||
      !reader.ReadU64(&provider.provider_generation) ||
      !reader.ReadU32(&provider.descriptor.dimensions) ||
      !reader.ReadU32(&profile) ||
      !reader.ReadU64(&provider.descriptor.descriptor_epoch) ||
      !reader.ReadF64(&provider.descriptor.int8_scale) ||
      !reader.ReadI32(&provider.descriptor.int8_zero_point) ||
      !reader.ReadString(&provider.metric.metric_resource_uuid) ||
      !reader.ReadU64(&provider.metric.metric_resource_epoch) ||
      !reader.ReadU32(&metric_kind) ||
      !reader.ReadU64(&provider.mutation_generation_evidence) ||
      !reader.ReadBool(&provider.descriptor.deterministic) ||
      !reader.ReadBool(&provider.descriptor.descriptor_safe) ||
      !reader.ReadBool(&provider.metric.deterministic) ||
      !reader.ReadBool(&provider.metric.safe) ||
      !reader.ReadU64(&row_count) ||
      row_count > kMaxRows) {
    return OpenFailure(VectorExactOpenClass::corrupt_payload,
                       "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_exact_physical_provider.corrupt_payload");
  }
  provider.descriptor.element_profile =
      static_cast<VectorExactElementProfile>(profile);
  provider.metric.metric_kind = static_cast<VectorExactMetricKind>(metric_kind);
  for (u64 i = 0; i < row_count; ++i) {
    VectorExactStoredRow row;
    if (!reader.ReadLocator(&row.locator) ||
        !reader.ReadBytes(&row.encoded_payload)) {
      return OpenFailure(
          VectorExactOpenClass::corrupt_payload,
          "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
          "index.vector_exact_physical_provider.corrupt_payload");
    }
    provider.rows.push_back(std::move(row));
  }
  if (!reader.Done()) {
    return OpenFailure(VectorExactOpenClass::corrupt_payload,
                       "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_exact_physical_provider.corrupt_payload");
  }
  SetProviderEvidence(&provider);
  if (!SupportedProfile(provider.descriptor.element_profile)) {
    return OpenFailure(
        VectorExactOpenClass::unsupported_element_profile,
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.UNSUPPORTED_ELEMENT_PROFILE",
        "index.vector_exact_physical_provider.unsupported_element_profile");
  }
  if (!MetricSafe(provider.metric)) {
    return OpenFailure(
        VectorExactOpenClass::unsafe_metric_resource,
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.UNSAFE_METRIC_RESOURCE",
        "index.vector_exact_physical_provider.unsafe_metric_resource");
  }
  if ((request.expected_relation_uuid_present &&
       request.expected_relation_uuid != provider.relation_uuid) ||
      (request.expected_index_uuid_present &&
       request.expected_index_uuid != provider.index_uuid) ||
      (request.expected_provider_uuid_present &&
       request.expected_provider_uuid != provider.provider_uuid)) {
    return OpenFailure(VectorExactOpenClass::identity_mismatch,
                       "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.IDENTITY_MISMATCH",
                       "index.vector_exact_physical_provider.identity_mismatch");
  }
  if ((request.expected_base_generation_present &&
       request.expected_base_generation != provider.base_generation) ||
      (request.expected_provider_generation_present &&
       request.expected_provider_generation != provider.provider_generation)) {
    return OpenFailure(VectorExactOpenClass::stale_generation,
                       "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.STALE_GENERATION",
                       "index.vector_exact_physical_provider.stale_generation");
  }
  if (request.expected_descriptor_epoch_present &&
      request.expected_descriptor_epoch != provider.descriptor.descriptor_epoch) {
    return OpenFailure(
        VectorExactOpenClass::stale_descriptor_epoch,
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.vector_exact_physical_provider.stale_descriptor_epoch");
  }
  if (request.expected_metric_resource_epoch_present &&
      request.expected_metric_resource_epoch !=
          provider.metric.metric_resource_epoch) {
    return OpenFailure(
        VectorExactOpenClass::stale_metric_epoch,
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.STALE_METRIC_EPOCH",
        "index.vector_exact_physical_provider.stale_metric_epoch");
  }
  if (request.expected_dimensions_present &&
      request.expected_dimensions != provider.descriptor.dimensions) {
    return OpenFailure(
        VectorExactOpenClass::dimension_mismatch,
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.DIMENSION_MISMATCH",
        "index.vector_exact_physical_provider.dimension_mismatch");
  }
  if (!ProviderValid(provider)) {
    return OpenFailure(VectorExactOpenClass::corrupt_payload,
                       "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_exact_physical_provider.corrupt_payload");
  }

  VectorExactOpenResult result;
  result.status = OkStatus();
  result.open_class = VectorExactOpenClass::current;
  result.provider = std::move(provider);
  result.fail_closed = false;
  result.actions.push_back("open_vector_exact_physical_provider");
  return result;
}

VectorExactQueryResult QueryVectorExactPhysicalProvider(
    const VectorExactQueryRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return QueryFailure(
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_exact_physical_provider.missing_exact_recheck");
  }
  if (!request.descriptor_epoch_current) {
    return QueryFailure(
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.vector_exact_physical_provider.stale_descriptor_epoch");
  }
  if (!request.metric_resource_epoch_current) {
    return QueryFailure(
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.STALE_METRIC_EPOCH",
        "index.vector_exact_physical_provider.stale_metric_epoch");
  }
  if (!RuntimeRequestSafe(request)) {
    return QueryFailure(
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
        "index.vector_exact_physical_provider.runtime_refused",
        "runtime query requires current descriptor and metric epochs, no scan/fallback mode, and exact source/MGA/security proof");
  }

  VectorExactQueryResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.batched_query_evaluation = request.queries.size() > 1;
  result.descriptor_store_scan = request.descriptor_store_scan;
  result.behavior_store_scan = request.behavior_store_scan;
  result.contract_only_fallback = request.contract_only_fallback;
  result.provider_only_fallback = request.provider_only_fallback;
  result.evidence = {
      kVectorExactPhysicalProviderSearchKey,
      "vector_exact_physical_provider.batched_query_evaluation=true",
      "vector_exact_physical_provider.top_k_heap_used=true",
      "vector_exact_physical_provider.exact_rerank_performed=true",
      "vector_exact_physical_provider.scalar_kernel_consumed=true",
      "vector_exact_physical_provider.candidate_rows_only=true",
      "vector_exact_physical_provider.final_rows_authorized=false"};

  for (const auto& query : request.queries) {
    if (query.vector.size() != request.provider.descriptor.dimensions ||
        query.top_k == 0 ||
        !FiniteVector(query.vector)) {
      return QueryFailure(
          "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.DIMENSION_MISMATCH",
          "index.vector_exact_physical_provider.dimension_mismatch");
    }

    std::vector<TextInvertedRowLocator> candidate_set = query.candidate_set;
    if (!CandidateSetValid(candidate_set)) {
      return QueryFailure(
          "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.CANDIDATE_SET_REFUSED",
          "index.vector_exact_physical_provider.candidate_set_refused");
    }
    std::sort(candidate_set.begin(), candidate_set.end(), LocatorLess);
    candidate_set.erase(
        std::unique(candidate_set.begin(), candidate_set.end(), LocatorEqual),
        candidate_set.end());
    result.candidate_set_consumed =
        result.candidate_set_consumed || !candidate_set.empty();
    result.metadata_prefilter_consumed =
        result.metadata_prefilter_consumed ||
        static_cast<bool>(query.metadata_prefilter);

    VectorExactSingleQueryResult single;
    single.ok = true;
    single.top_k_heap_used = true;
    single.exact_rerank_performed = true;
    const bool lower_score_better =
        LowerScoreBetter(request.provider.metric.metric_kind);
    std::vector<VectorExactCandidate> heap;
    heap.reserve(query.top_k);
    for (const auto& row : request.provider.rows) {
      ++single.candidates_considered;
      if (!candidate_set.empty() &&
          !CandidateSetContains(candidate_set, row.locator)) {
        ++single.candidates_filtered;
        continue;
      }
      if (query.metadata_prefilter &&
          !query.metadata_prefilter(row.locator)) {
        ++single.candidates_filtered;
        continue;
      }
      const auto decoded =
          DecodeVectorExactPayload(row.encoded_payload, request.provider.descriptor);
      if (decoded.size() != request.provider.descriptor.dimensions) {
        return QueryFailure(
            "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
            "index.vector_exact_physical_provider.corrupt_payload");
      }
      ++single.decoded_vector_count;
      u64 scalar_ops = 0;
      const double score =
          ScoreVectorPair(query.vector, decoded, request.provider.metric.metric_kind,
                          &scalar_ops);
      single.scalar_kernel_consumed_count += scalar_ops;
      VectorExactCandidate candidate;
      candidate.locator = row.locator;
      candidate.score = score;
      candidate.lower_score_better = lower_score_better;
      candidate.decoded_from_physical_payload = true;
      candidate.metadata_prefilter_passed = true;
      candidate.candidate_set_member = true;
      candidate.exact_rerank_proof_verified = true;
      candidate.source_recheck_evidence_ref = request.recheck_proof.evidence_ref;
      if (heap.size() < query.top_k) {
        heap.push_back(std::move(candidate));
        std::push_heap(heap.begin(), heap.end(), WorseForHeap);
      } else if (ScoreBetter(candidate.score, candidate.locator, heap.front().score,
                             heap.front().locator, lower_score_better)) {
        std::pop_heap(heap.begin(), heap.end(), WorseForHeap);
        heap.back() = std::move(candidate);
        std::push_heap(heap.begin(), heap.end(), WorseForHeap);
      }
    }
    std::sort(heap.begin(), heap.end(), [&](const auto& left,
                                            const auto& right) {
      return ScoreBetter(left.score, left.locator, right.score, right.locator,
                         lower_score_better);
    });
    single.candidates = std::move(heap);
    result.scalar_kernel_consumed =
        result.scalar_kernel_consumed || single.scalar_kernel_consumed_count > 0;
    result.batch_results.push_back(std::move(single));
  }
  return result;
}

VectorExactMutationResult ApplyVectorExactPhysicalMutation(
    const VectorExactPhysicalProvider& provider,
    const VectorExactMutation& mutation) {
  if (!ProviderValid(provider)) {
    return MutationFailure(
        provider,
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.MUTATION_REFUSED",
        "index.vector_exact_physical_provider.mutation_refused");
  }
  if (!RecheckProofValid(mutation.recheck_proof)) {
    return MutationFailure(
        provider,
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_exact_physical_provider.missing_exact_recheck");
  }
  if ((mutation.expected_provider_generation_present &&
       mutation.expected_provider_generation != provider.provider_generation) ||
      (mutation.expected_descriptor_epoch_present &&
       mutation.expected_descriptor_epoch != provider.descriptor.descriptor_epoch) ||
      (mutation.expected_metric_resource_epoch_present &&
       mutation.expected_metric_resource_epoch !=
           provider.metric.metric_resource_epoch)) {
    return MutationFailure(
        provider,
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.STALE_DESCRIPTOR",
        "index.vector_exact_physical_provider.stale_descriptor");
  }

  VectorExactPhysicalProvider next = provider;
  switch (mutation.kind) {
    case VectorExactMutationKind::insert_row: {
      if (!mutation.after_row_present ||
          !SourceRowValid(mutation.after_row, provider.descriptor) ||
          FindStoredRow(next, mutation.after_row.locator) != nullptr) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.DUPLICATE_ROW_LOCATOR",
            "index.vector_exact_physical_provider.duplicate_row_locator");
      }
      next.rows.push_back(StoredRowFromSource(mutation.after_row,
                                              provider.descriptor));
      break;
    }
    case VectorExactMutationKind::delete_row: {
      if (!mutation.before_row_present ||
          !SourceRowValid(mutation.before_row, provider.descriptor)) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_exact_physical_provider.mutation_refused");
      }
      const auto iter = std::lower_bound(
          next.rows.begin(), next.rows.end(), mutation.before_row.locator,
          [](const VectorExactStoredRow& row,
             const TextInvertedRowLocator& locator) {
            return LocatorLess(row.locator, locator);
          });
      if (iter == next.rows.end() ||
          !SourceMatchesStoredPayload(mutation.before_row, *iter,
                                      provider.descriptor)) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_exact_physical_provider.mutation_refused");
      }
      next.rows.erase(iter);
      break;
    }
    case VectorExactMutationKind::update_row: {
      if (!mutation.before_row_present ||
          !mutation.after_row_present ||
          !SourceRowValid(mutation.before_row, provider.descriptor) ||
          !SourceRowValid(mutation.after_row, provider.descriptor) ||
          !LocatorEqual(mutation.before_row.locator, mutation.after_row.locator)) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_exact_physical_provider.mutation_refused");
      }
      auto iter = std::lower_bound(
          next.rows.begin(), next.rows.end(), mutation.before_row.locator,
          [](const VectorExactStoredRow& row,
             const TextInvertedRowLocator& locator) {
            return LocatorLess(row.locator, locator);
          });
      if (iter == next.rows.end() ||
          !SourceMatchesStoredPayload(mutation.before_row, *iter,
                                      provider.descriptor)) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_exact_physical_provider.mutation_refused");
      }
      *iter = StoredRowFromSource(mutation.after_row, provider.descriptor);
      break;
    }
  }
  std::sort(next.rows.begin(), next.rows.end(), [](const auto& left,
                                                   const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
  if (!ProviderValid(next)) {
    return MutationFailure(
        provider,
        "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.MUTATION_REFUSED",
        "index.vector_exact_physical_provider.mutation_refused");
  }
  next.provider_generation += 1;
  next.mutation_generation_evidence = next.provider_generation;
  SetProviderEvidence(&next);

  VectorExactMutationResult result;
  result.status = OkStatus();
  result.provider = std::move(next);
  result.applied = true;
  result.fail_closed = false;
  result.actions.push_back("apply_vector_exact_maintenance_mutation");
  result.actions.push_back("provider_generation_incremented");
  return result;
}

const char* VectorExactOpenClassName(VectorExactOpenClass open_class) {
  switch (open_class) {
    case VectorExactOpenClass::current:
      return "current";
    case VectorExactOpenClass::stale_format:
      return "stale_format";
    case VectorExactOpenClass::stale_generation:
      return "stale_generation";
    case VectorExactOpenClass::bad_checksum:
      return "bad_checksum";
    case VectorExactOpenClass::corrupt_payload:
      return "corrupt_payload";
    case VectorExactOpenClass::identity_mismatch:
      return "identity_mismatch";
    case VectorExactOpenClass::stale_descriptor_epoch:
      return "stale_descriptor_epoch";
    case VectorExactOpenClass::stale_metric_epoch:
      return "stale_metric_epoch";
    case VectorExactOpenClass::dimension_mismatch:
      return "dimension_mismatch";
    case VectorExactOpenClass::unsupported_element_profile:
      return "unsupported_element_profile";
    case VectorExactOpenClass::unsafe_metric_resource:
      return "unsafe_metric_resource";
    case VectorExactOpenClass::missing_exact_recheck_proof:
      return "missing_exact_recheck_proof";
    case VectorExactOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case VectorExactOpenClass::refused:
      return "refused";
  }
  return "refused";
}

const char* VectorExactElementProfileName(VectorExactElementProfile profile) {
  switch (profile) {
    case VectorExactElementProfile::fp32:
      return "fp32";
    case VectorExactElementProfile::fp16:
      return "fp16";
    case VectorExactElementProfile::int8:
      return "int8";
  }
  return "unsupported";
}

const char* VectorExactMetricKindName(VectorExactMetricKind metric) {
  switch (metric) {
    case VectorExactMetricKind::l2:
      return "l2";
    case VectorExactMetricKind::cosine:
      return "cosine";
    case VectorExactMetricKind::inner_product:
      return "inner_product";
  }
  return "unsupported";
}

DiagnosticRecord MakeVectorExactPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(diagnostic_code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.source_component = kVectorExactPhysicalProviderSearchKey;
  diagnostic.remediation_hint =
      "rebuild vector exact provider from authoritative source vectors with exact source, MGA, security, and metric descriptor proof";
  if (!detail.empty()) {
    diagnostic.arguments.push_back({"detail", std::move(detail)});
  }
  return diagnostic;
}

}  // namespace scratchbird::core::index
