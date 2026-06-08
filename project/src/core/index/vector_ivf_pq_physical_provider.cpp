// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "vector_ivf_pq_physical_provider.hpp"

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

using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'V', 'I',
                                               'V', 'F', 'P', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u32 kMaxDimensions = 16384;
inline constexpr u32 kMaxCentroids = 65536;
inline constexpr u32 kMaxPqSubspaces = 256;
inline constexpr u32 kMaxPqCodewords = 256;
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

bool SupportedMetric(VectorExactMetricKind metric) {
  return metric == VectorExactMetricKind::l2 ||
         metric == VectorExactMetricKind::cosine ||
         metric == VectorExactMetricKind::inner_product;
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

bool DescriptorAuthorityClean(const VectorIvfPqDescriptor& descriptor) {
  return !descriptor.parser_finality_authority_claimed &&
         !descriptor.donor_finality_authority_claimed &&
         !descriptor.provider_finality_authority_claimed &&
         !descriptor.index_finality_authority_claimed &&
         !descriptor.write_ahead_log_finality_authority_claimed;
}

bool MetricAuthorityClean(const VectorIvfPqMetricResource& metric) {
  return !metric.parser_finality_authority_claimed &&
         !metric.donor_finality_authority_claimed &&
         !metric.provider_finality_authority_claimed &&
         !metric.index_finality_authority_claimed &&
         !metric.write_ahead_log_finality_authority_claimed;
}

bool RecheckProofAuthorityClean(const VectorIvfPqRecheckProof& proof) {
  return !proof.parser_finality_authority_claimed &&
         !proof.donor_finality_authority_claimed &&
         !proof.provider_finality_authority_claimed &&
         !proof.index_finality_authority_claimed &&
         !proof.write_ahead_log_finality_authority_claimed &&
         !proof.visibility_authority_claimed &&
         !proof.security_authority_claimed &&
         !proof.transaction_finality_authority_claimed;
}

bool DescriptorSafe(const VectorIvfPqDescriptor& descriptor) {
  return descriptor.dimensions > 0 &&
         descriptor.dimensions <= kMaxDimensions &&
         descriptor.descriptor_epoch > 0 &&
         descriptor.element_profile == VectorExactElementProfile::fp32 &&
         descriptor.deterministic &&
         descriptor.descriptor_safe &&
         !descriptor.descriptor_store_scan &&
         !descriptor.behavior_store_scan &&
         DescriptorAuthorityClean(descriptor);
}

bool MetricSafe(const VectorIvfPqMetricResource& metric) {
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

bool RecheckProofValid(const VectorIvfPqRecheckProof& proof) {
  return proof.proof_supplied &&
         proof.exact_source_vector_available &&
         proof.exact_rerank_proof_supplied &&
         proof.mga_recheck_required &&
         proof.security_recheck_required &&
         !proof.evidence_ref.empty() &&
         RecheckProofAuthorityClean(proof);
}

bool ProfileSafe(const VectorIvfPqBuildProfile& profile,
                 const VectorIvfPqDescriptor& descriptor) {
  const bool compression_ok =
      profile.compression == VectorIvfPqCompression::ivf_flat ||
      profile.compression == VectorIvfPqCompression::sq8 ||
      profile.compression == VectorIvfPqCompression::pq;
  const bool pq_ok =
      profile.compression != VectorIvfPqCompression::pq ||
      (profile.pq_subspaces > 0 &&
       profile.pq_subspaces <= std::min(descriptor.dimensions, kMaxPqSubspaces) &&
       profile.pq_codewords > 1 &&
       profile.pq_codewords <= kMaxPqCodewords);
  return compression_ok &&
         profile.centroid_count > 0 &&
         profile.centroid_count <= kMaxCentroids &&
         profile.nprobe > 0 &&
         profile.nprobe <= profile.centroid_count &&
         profile.training_iterations > 0 &&
         profile.training_iterations <= 64 &&
         profile.max_training_rows > 0 &&
         profile.retrain_imbalance_ratio > 1.0 &&
         profile.rebuild_tombstone_ratio > 0.0 &&
         profile.rebuild_tombstone_ratio < 1.0 &&
         profile.deterministic_training &&
         profile.scalar_kernel_present &&
         pq_ok;
}

bool ProviderAuthorityClean(const VectorIvfPqPhysicalProvider& provider) {
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
void AppendF32(std::vector<byte>* out, float value) {
  u32 bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU32(out, bits);
}
void AppendF64(std::vector<byte>* out, double value) {
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
void AppendVector(std::vector<byte>* out, const std::vector<float>& values) {
  AppendU32(out, static_cast<u32>(values.size()));
  for (float value : values) AppendF32(out, value);
}

u64 ComputeChecksum(std::vector<byte> bytes) {
  if (bytes.size() >= kHeaderBytes) StoreLittle64(bytes.data() + 16, 0);
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
  bool ReadF32(float* out) {
    u32 bits = 0;
    if (!ReadU32(&bits)) return false;
    std::memcpy(out, &bits, sizeof(bits));
    return std::isfinite(*out);
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
  bool ReadVector(std::vector<float>* out, u32 max_size) {
    u32 size = 0;
    if (!ReadU32(&size) || size > max_size) return false;
    out->resize(size);
    for (float& value : *out) {
      if (!ReadF32(&value)) return false;
    }
    return true;
  }
  bool Done() const { return offset_ == bytes_.size(); }

 private:
  const std::vector<byte>& bytes_;
  std::size_t offset_ = 0;
};

VectorIvfPqBuildResult BuildFailure(std::string code,
                                    std::string key,
                                    std::string detail = {}) {
  VectorIvfPqBuildResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeVectorIvfPqPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

VectorIvfPqOpenResult OpenFailure(VectorIvfPqOpenClass open_class,
                                  std::string code,
                                  std::string key,
                                  std::string detail = {}) {
  VectorIvfPqOpenResult result;
  result.status = open_class == VectorIvfPqOpenClass::stale_format ||
                          open_class == VectorIvfPqOpenClass::stale_generation ||
                          open_class ==
                              VectorIvfPqOpenClass::stale_descriptor_epoch ||
                          open_class == VectorIvfPqOpenClass::stale_metric_epoch
                      ? WarnStatus()
                      : ErrorStatus();
  result.open_class = open_class;
  result.fail_closed = true;
  result.restricted_repair_required =
      open_class == VectorIvfPqOpenClass::bad_checksum ||
      open_class == VectorIvfPqOpenClass::corrupt_payload ||
      open_class == VectorIvfPqOpenClass::invalid_centroid ||
      open_class == VectorIvfPqOpenClass::invalid_list ||
      open_class == VectorIvfPqOpenClass::invalid_codebook ||
      open_class == VectorIvfPqOpenClass::invalid_code ||
      open_class == VectorIvfPqOpenClass::duplicate_row_locator;
  result.diagnostic = MakeVectorIvfPqPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_vector_ivf_pq_physical_provider");
  if (result.restricted_repair_required) {
    result.actions.push_back(
        "repair_requires_authoritative_source_vectors_and_exact_recheck");
  }
  return result;
}

VectorIvfPqQueryResult QueryFailure(std::string code,
                                    std::string key,
                                    std::string detail = {}) {
  VectorIvfPqQueryResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeVectorIvfPqPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.evidence.push_back(kVectorIvfPqPhysicalProviderSearchKey);
  result.evidence.push_back(
      "vector_ivf_pq_physical_provider.candidates_refused=true");
  result.evidence.push_back(
      "vector_ivf_pq_physical_provider.mga_recheck_required=true");
  result.evidence.push_back(
      "vector_ivf_pq_physical_provider.security_recheck_required=true");
  return result;
}

VectorIvfPqMutationResult MutationFailure(VectorIvfPqPhysicalProvider provider,
                                          std::string code,
                                          std::string key,
                                          std::string detail = {}) {
  VectorIvfPqMutationResult result;
  result.status = ErrorStatus();
  result.provider = std::move(provider);
  result.fail_closed = true;
  result.diagnostic = MakeVectorIvfPqPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_vector_ivf_pq_maintenance_mutation");
  return result;
}

bool SourceRowValid(const VectorIvfPqSourceRow& row,
                    const VectorIvfPqDescriptor& descriptor) {
  return LocatorValid(row.locator) &&
         row.vector.size() == descriptor.dimensions &&
         FiniteVector(row.vector);
}

bool CandidateSetValid(const std::vector<TextInvertedRowLocator>& locators) {
  return std::all_of(locators.begin(), locators.end(), LocatorValid);
}

bool CandidateSetContains(const std::vector<TextInvertedRowLocator>& locators,
                          const TextInvertedRowLocator& locator) {
  return std::binary_search(locators.begin(), locators.end(), locator,
                            LocatorLess);
}

std::vector<float> SubVector(const std::vector<float>& values,
                             u32 offset,
                             u32 width) {
  return {values.begin() + offset, values.begin() + offset + width};
}

std::vector<float> AddVectors(const std::vector<float>& left,
                              const std::vector<float>& right) {
  std::vector<float> out(left.size(), 0.0F);
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = left[i] + right[i];
  return out;
}

std::vector<float> SubtractVectors(const std::vector<float>& left,
                                   const std::vector<float>& right) {
  std::vector<float> out(left.size(), 0.0F);
  for (std::size_t i = 0; i < out.size(); ++i) out[i] = left[i] - right[i];
  return out;
}

u32 NearestVector(const std::vector<float>& query,
                 const std::vector<std::vector<float>>& vectors,
                 VectorExactMetricKind metric,
                 u64* scalar_ops) {
  u32 best = 0;
  double best_score = std::numeric_limits<double>::infinity();
  const bool lower = LowerScoreBetter(metric);
  for (u32 i = 0; i < vectors.size(); ++i) {
    const double score = ScoreVectorPair(query, vectors[i], metric, scalar_ops);
    if (i == 0 || (lower ? score < best_score : score > best_score)) {
      best = i;
      best_score = score;
    }
  }
  return best;
}

u32 AssignList(const VectorIvfPqPhysicalProvider& provider,
               const std::vector<float>& vector,
               double* residual_error) {
  std::vector<std::vector<float>> centroids;
  centroids.reserve(provider.centroids.size());
  for (const auto& centroid : provider.centroids) centroids.push_back(centroid.vector);
  u64 ops = 0;
  const u32 id = NearestVector(vector, centroids, provider.metric.metric_kind, &ops);
  *residual_error =
      ScoreVectorPair(vector, provider.centroids[id].vector,
                      provider.metric.metric_kind, &ops);
  return id;
}

std::vector<VectorIvfPqSourceRow> DeterministicTrainingSet(
    std::vector<VectorIvfPqSourceRow> rows,
    u32 max_training_rows) {
  std::sort(rows.begin(), rows.end(), [](const auto& left,
                                         const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
  if (rows.size() <= max_training_rows) return rows;
  std::vector<VectorIvfPqSourceRow> selected;
  selected.reserve(max_training_rows);
  const std::size_t stride = rows.size();
  for (u32 i = 0; i < max_training_rows; ++i) {
    selected.push_back(rows[(static_cast<std::size_t>(i) * stride) /
                            max_training_rows]);
  }
  return selected;
}

std::vector<VectorIvfPqCentroid> TrainCentroids(
    const std::vector<VectorIvfPqSourceRow>& rows,
    const VectorIvfPqBuildRequest& request) {
  const u32 k = std::min<u32>(request.profile.centroid_count, rows.size());
  std::vector<VectorIvfPqCentroid> centroids(k);
  for (u32 i = 0; i < k; ++i) {
    centroids[i].centroid_id = i;
    centroids[i].vector = rows[i].vector;
  }
  for (u32 iteration = 0; iteration < request.profile.training_iterations; ++iteration) {
    std::vector<std::vector<double>> sums(k,
        std::vector<double>(request.descriptor.dimensions, 0.0));
    std::vector<u64> counts(k, 0);
    for (const auto& row : rows) {
      std::vector<std::vector<float>> vectors;
      for (const auto& centroid : centroids) vectors.push_back(centroid.vector);
      u64 ops = 0;
      const u32 chosen = NearestVector(row.vector, vectors,
                                       request.metric.metric_kind, &ops);
      ++counts[chosen];
      for (u32 d = 0; d < request.descriptor.dimensions; ++d) {
        sums[chosen][d] += row.vector[d];
      }
    }
    for (u32 c = 0; c < k; ++c) {
      if (counts[c] == 0) continue;
      for (u32 d = 0; d < request.descriptor.dimensions; ++d) {
        centroids[c].vector[d] = static_cast<float>(sums[c][d] / counts[c]);
      }
    }
  }
  return centroids;
}

std::vector<VectorIvfPqSq8Axis> TrainSq8(
    const std::vector<VectorIvfPqSourceRow>& rows,
    const std::vector<VectorIvfPqCentroid>& centroids,
    const VectorIvfPqDescriptor& descriptor,
    const VectorIvfPqMetricResource& metric) {
  std::vector<VectorIvfPqSq8Axis> axes(descriptor.dimensions);
  std::vector<float> min_values(descriptor.dimensions,
                                std::numeric_limits<float>::infinity());
  std::vector<float> max_values(descriptor.dimensions,
                                -std::numeric_limits<float>::infinity());
  std::vector<std::vector<float>> centroid_vectors;
  for (const auto& centroid : centroids) centroid_vectors.push_back(centroid.vector);
  for (const auto& row : rows) {
    u64 ops = 0;
    const u32 list = NearestVector(row.vector, centroid_vectors,
                                   metric.metric_kind, &ops);
    const auto residual = SubtractVectors(row.vector, centroids[list].vector);
    for (u32 d = 0; d < descriptor.dimensions; ++d) {
      min_values[d] = std::min(min_values[d], residual[d]);
      max_values[d] = std::max(max_values[d], residual[d]);
    }
  }
  for (u32 d = 0; d < descriptor.dimensions; ++d) {
    if (!std::isfinite(min_values[d])) {
      min_values[d] = 0.0F;
      max_values[d] = 0.0F;
    }
    const float range = max_values[d] - min_values[d];
    axes[d].min_value = min_values[d];
    axes[d].scale = range <= 0.0F ? 1.0F : range / 255.0F;
  }
  return axes;
}

std::vector<byte> EncodeSq8(const std::vector<float>& vector,
                            const std::vector<float>& centroid,
                            const std::vector<VectorIvfPqSq8Axis>& axes) {
  std::vector<byte> code(axes.size(), 0);
  for (std::size_t d = 0; d < axes.size(); ++d) {
    const float residual = vector[d] - centroid[d];
    const float quantized =
        std::round((residual - axes[d].min_value) / axes[d].scale);
    const auto clamped = std::max(0, std::min(255, static_cast<int>(quantized)));
    code[d] = static_cast<byte>(clamped);
  }
  return code;
}

std::vector<float> DecodeSq8(const std::vector<byte>& code,
                             const std::vector<float>& centroid,
                             const std::vector<VectorIvfPqSq8Axis>& axes) {
  std::vector<float> out(centroid.size(), 0.0F);
  for (std::size_t d = 0; d < centroid.size(); ++d) {
    out[d] = centroid[d] + axes[d].min_value +
             static_cast<float>(code[d]) * axes[d].scale;
  }
  return out;
}

std::vector<VectorIvfPqCodebook> TrainPq(
    const std::vector<VectorIvfPqSourceRow>& rows,
    const std::vector<VectorIvfPqCentroid>& centroids,
    const VectorIvfPqBuildRequest& request) {
  std::vector<VectorIvfPqCodebook> codebooks;
  std::vector<std::vector<float>> centroid_vectors;
  for (const auto& centroid : centroids) centroid_vectors.push_back(centroid.vector);
  for (u32 s = 0; s < request.profile.pq_subspaces; ++s) {
    const u32 offset =
        (request.descriptor.dimensions * s) / request.profile.pq_subspaces;
    const u32 end =
        (request.descriptor.dimensions * (s + 1)) / request.profile.pq_subspaces;
    VectorIvfPqCodebook codebook;
    codebook.subspace_id = s;
    codebook.offset = offset;
    codebook.width = end - offset;
    std::vector<std::vector<float>> residuals;
    for (const auto& row : rows) {
      u64 ops = 0;
      const u32 list = NearestVector(row.vector, centroid_vectors,
                                     request.metric.metric_kind, &ops);
      const auto residual = SubtractVectors(row.vector, centroids[list].vector);
      residuals.push_back(SubVector(residual, offset, codebook.width));
    }
    const u32 k = std::min<u32>(request.profile.pq_codewords,
                                std::max<std::size_t>(residuals.size(), 1));
    codebook.centroids.assign(k, std::vector<float>(codebook.width, 0.0F));
    for (u32 c = 0; c < k; ++c) {
      codebook.centroids[c] = residuals.empty()
                                  ? std::vector<float>(codebook.width, 0.0F)
                                  : residuals[c % residuals.size()];
    }
    for (u32 iteration = 0; iteration < request.profile.training_iterations; ++iteration) {
      std::vector<std::vector<double>> sums(k,
          std::vector<double>(codebook.width, 0.0));
      std::vector<u64> counts(k, 0);
      for (const auto& residual : residuals) {
        u64 ops = 0;
        const u32 chosen = NearestVector(residual, codebook.centroids,
                                         VectorExactMetricKind::l2, &ops);
        ++counts[chosen];
        for (u32 d = 0; d < codebook.width; ++d) sums[chosen][d] += residual[d];
      }
      for (u32 c = 0; c < k; ++c) {
        if (counts[c] == 0) continue;
        for (u32 d = 0; d < codebook.width; ++d) {
          codebook.centroids[c][d] = static_cast<float>(sums[c][d] / counts[c]);
        }
      }
    }
    codebooks.push_back(std::move(codebook));
  }
  return codebooks;
}

std::vector<byte> EncodePq(const std::vector<float>& vector,
                           const std::vector<float>& centroid,
                           const std::vector<VectorIvfPqCodebook>& codebooks) {
  const auto residual = SubtractVectors(vector, centroid);
  std::vector<byte> code;
  code.reserve(codebooks.size());
  for (const auto& codebook : codebooks) {
    const auto sub = SubVector(residual, codebook.offset, codebook.width);
    u64 ops = 0;
    code.push_back(static_cast<byte>(
        NearestVector(sub, codebook.centroids, VectorExactMetricKind::l2, &ops)));
  }
  return code;
}

std::vector<float> DecodePq(const std::vector<byte>& code,
                            const std::vector<float>& centroid,
                            const std::vector<VectorIvfPqCodebook>& codebooks) {
  std::vector<float> residual(centroid.size(), 0.0F);
  for (std::size_t i = 0; i < codebooks.size(); ++i) {
    const auto& codebook = codebooks[i];
    const auto& selected = codebook.centroids[static_cast<u32>(code[i])];
    for (u32 d = 0; d < codebook.width; ++d) {
      residual[codebook.offset + d] = selected[d];
    }
  }
  return AddVectors(centroid, residual);
}

std::vector<float> DecodeApproximate(
    const VectorIvfPqPhysicalProvider& provider,
    const VectorIvfPqStoredVector& entry) {
  const auto& centroid = provider.centroids[entry.list_id].vector;
  switch (provider.profile.compression) {
    case VectorIvfPqCompression::ivf_flat:
      return DecodeVectorExactPayload(entry.compressed_code, provider.descriptor);
    case VectorIvfPqCompression::sq8:
      return DecodeSq8(entry.compressed_code, centroid, provider.sq8_axes);
    case VectorIvfPqCompression::pq:
      return DecodePq(entry.compressed_code, centroid, provider.pq_codebooks);
  }
  return {};
}

std::vector<byte> EncodeApproximate(const VectorIvfPqPhysicalProvider& provider,
                                    const std::vector<float>& vector,
                                    u32 list_id) {
  const auto& centroid = provider.centroids[list_id].vector;
  switch (provider.profile.compression) {
    case VectorIvfPqCompression::ivf_flat:
      return EncodeVectorExactPayload(vector, provider.descriptor);
    case VectorIvfPqCompression::sq8:
      return EncodeSq8(vector, centroid, provider.sq8_axes);
    case VectorIvfPqCompression::pq:
      return EncodePq(vector, centroid, provider.pq_codebooks);
  }
  return {};
}

bool CodeValid(const VectorIvfPqPhysicalProvider& provider,
               const VectorIvfPqStoredVector& entry) {
  if (entry.list_id >= provider.centroids.size() ||
      entry.exact_payload.size() != provider.descriptor.dimensions * sizeof(float)) {
    return false;
  }
  switch (provider.profile.compression) {
    case VectorIvfPqCompression::ivf_flat:
      return entry.compressed_code.size() == entry.exact_payload.size();
    case VectorIvfPqCompression::sq8:
      return entry.compressed_code.size() == provider.descriptor.dimensions &&
             provider.sq8_axes.size() == provider.descriptor.dimensions;
    case VectorIvfPqCompression::pq:
      if (entry.compressed_code.size() != provider.pq_codebooks.size()) return false;
      for (std::size_t i = 0; i < entry.compressed_code.size(); ++i) {
        if (static_cast<u32>(entry.compressed_code[i]) >=
            provider.pq_codebooks[i].centroids.size()) {
          return false;
        }
      }
      return true;
  }
  return false;
}

void SetProviderEvidence(VectorIvfPqPhysicalProvider* provider) {
  provider->evidence = {
      kVectorIvfPqPhysicalProviderSearchKey,
      "vector_ivf_pq_physical_provider.artifact_identity_deterministic=true",
      "vector_ivf_pq_physical_provider.checksumed_serialization=true",
      "vector_ivf_pq_physical_provider.centroid_training=true",
      "vector_ivf_pq_physical_provider.ivf_list_assignment=true",
      "vector_ivf_pq_physical_provider.nprobe_planner=true",
      "vector_ivf_pq_physical_provider.compressed_code_storage=true",
      "vector_ivf_pq_physical_provider.exact_rerank=true",
      "vector_ivf_pq_physical_provider.candidate_evidence_only=true",
      "vector_ivf_pq_physical_provider.index_finality_authority=false",
      "vector_ivf_pq_physical_provider.write_ahead_log_authority=false"};
}

void RefreshTelemetry(VectorIvfPqPhysicalProvider* provider) {
  provider->live_vector_count = 0;
  provider->tombstone_count = 0;
  provider->residual_error_mean = 0.0;
  u64 max_live = 0;
  u64 non_empty_lists = 0;
  double residual_sum = 0.0;
  double compression_sum = 0.0;
  for (auto& centroid : provider->centroids) {
    centroid.assigned_count = 0;
    centroid.residual_error_sum = 0.0;
  }
  for (auto& list : provider->lists) {
    list.live_count = 0;
    list.tombstone_count = 0;
    list.residual_error_sum = 0.0;
    for (const auto& entry : list.entries) {
      if (entry.tombstoned) {
        ++list.tombstone_count;
        ++provider->tombstone_count;
        continue;
      }
      const auto exact = DecodeVectorExactPayload(entry.exact_payload,
                                                  provider->descriptor);
      if (exact.size() != provider->descriptor.dimensions) continue;
      u64 ops = 0;
      const double residual = ScoreVectorPair(exact,
          provider->centroids[list.centroid_id].vector,
          provider->metric.metric_kind, &ops);
      const auto decoded = DecodeApproximate(*provider, entry);
      const double compression =
          decoded.size() == exact.size()
              ? ScoreVectorPair(exact, decoded, VectorExactMetricKind::l2, &ops)
              : std::numeric_limits<double>::infinity();
      ++list.live_count;
      ++provider->live_vector_count;
      list.residual_error_sum += residual;
      residual_sum += residual;
      compression_sum += compression;
      provider->centroids[list.centroid_id].assigned_count += 1;
      provider->centroids[list.centroid_id].residual_error_sum += residual;
    }
    if (list.live_count > 0) ++non_empty_lists;
    max_live = std::max(max_live, list.live_count);
  }
  if (provider->live_vector_count > 0) {
    provider->residual_error_mean =
        residual_sum / static_cast<double>(provider->live_vector_count);
    provider->compression_error_mean =
        compression_sum / static_cast<double>(provider->live_vector_count);
  } else {
    provider->compression_error_mean = 0.0;
  }
  const double avg = provider->lists.empty()
                         ? 0.0
                         : static_cast<double>(provider->live_vector_count) /
                               static_cast<double>(provider->lists.size());
  provider->list_imbalance_ratio = avg == 0.0 ? 0.0 : max_live / avg;
  const u64 total = provider->live_vector_count + provider->tombstone_count;
  const double tombstone_ratio =
      total == 0 ? 0.0 : static_cast<double>(provider->tombstone_count) /
                            static_cast<double>(total);
  provider->retrain_recommended =
      non_empty_lists > 0 &&
      provider->list_imbalance_ratio >= provider->profile.retrain_imbalance_ratio;
  provider->rebuild_recommended =
      tombstone_ratio >= provider->profile.rebuild_tombstone_ratio;
}

bool CodebooksValid(const VectorIvfPqPhysicalProvider& provider) {
  if (provider.profile.compression == VectorIvfPqCompression::sq8) {
    if (provider.sq8_axes.size() != provider.descriptor.dimensions) return false;
    return std::all_of(provider.sq8_axes.begin(), provider.sq8_axes.end(),
                       [](const auto& axis) {
                         return std::isfinite(axis.min_value) &&
                                std::isfinite(axis.scale) && axis.scale > 0.0F;
                       });
  }
  if (provider.profile.compression == VectorIvfPqCompression::pq) {
    if (provider.pq_codebooks.size() != provider.profile.pq_subspaces) return false;
    u32 last_end = 0;
    for (u32 i = 0; i < provider.pq_codebooks.size(); ++i) {
      const auto& codebook = provider.pq_codebooks[i];
      if (codebook.subspace_id != i ||
          codebook.offset != last_end ||
          codebook.width == 0 ||
          codebook.offset + codebook.width > provider.descriptor.dimensions ||
          codebook.centroids.empty() ||
          codebook.centroids.size() > kMaxPqCodewords) {
        return false;
      }
      last_end = codebook.offset + codebook.width;
      for (const auto& centroid : codebook.centroids) {
        if (centroid.size() != codebook.width || !FiniteVector(centroid)) {
          return false;
        }
      }
    }
    return last_end == provider.descriptor.dimensions;
  }
  return provider.sq8_axes.empty() && provider.pq_codebooks.empty();
}

bool ListsValid(const VectorIvfPqPhysicalProvider& provider) {
  if (provider.centroids.size() != provider.lists.size() ||
      provider.centroids.size() > kMaxCentroids) {
    return false;
  }
  std::set<std::tuple<u64, std::string, std::string>> locators;
  u64 live = 0;
  u64 tombstones = 0;
  for (u32 i = 0; i < provider.centroids.size(); ++i) {
    const auto& centroid = provider.centroids[i];
    const auto& list = provider.lists[i];
    if (centroid.centroid_id != i ||
        centroid.vector.size() != provider.descriptor.dimensions ||
        !FiniteVector(centroid.vector) ||
        list.list_id != i ||
        list.centroid_id != i) {
      return false;
    }
    u64 list_live = 0;
    u64 list_tombstones = 0;
    for (std::size_t e = 0; e < list.entries.size(); ++e) {
      const auto& entry = list.entries[e];
      if (!LocatorValid(entry.locator) ||
          entry.list_id != i ||
          entry.insert_generation == 0 ||
          (!entry.tombstoned && entry.delete_generation != 0) ||
          (entry.tombstoned && entry.delete_generation == 0) ||
          !CodeValid(provider, entry)) {
        return false;
      }
      if (e > 0 && !LocatorLess(list.entries[e - 1].locator, entry.locator)) {
        return false;
      }
      const auto key = std::make_tuple(entry.locator.row_ordinal,
                                       entry.locator.row_uuid,
                                       entry.locator.version_uuid);
      if (!locators.insert(key).second) return false;
      if (entry.tombstoned) {
        ++list_tombstones;
      } else {
        ++list_live;
      }
    }
    live += list_live;
    tombstones += list_tombstones;
    if (list.live_count != list_live || list.tombstone_count != list_tombstones) {
      return false;
    }
  }
  return provider.live_vector_count == live &&
         provider.tombstone_count == tombstones;
}

bool NearlyEqual(double left, double right) {
  if (!std::isfinite(left) || !std::isfinite(right)) {
    return false;
  }
  const double scale = std::max({1.0, std::fabs(left), std::fabs(right)});
  return std::fabs(left - right) <= 1e-9 * scale;
}

bool ProviderStatisticsValid(const VectorIvfPqPhysicalProvider& provider) {
  if (provider.training_generation_evidence != provider.training_generation ||
      provider.mutation_generation_evidence != provider.provider_generation ||
      provider.live_vector_count + provider.tombstone_count > kMaxRows ||
      provider.last_query_recall_floor < 0.0 ||
      provider.last_query_recall_floor > 1.0 ||
      !std::isfinite(provider.last_query_recall_floor) ||
      !std::isfinite(provider.list_imbalance_ratio) ||
      !std::isfinite(provider.residual_error_mean) ||
      !std::isfinite(provider.compression_error_mean)) {
    return false;
  }
  auto expected = provider;
  RefreshTelemetry(&expected);
  if (expected.live_vector_count != provider.live_vector_count ||
      expected.tombstone_count != provider.tombstone_count ||
      expected.retrain_recommended != provider.retrain_recommended ||
      expected.rebuild_recommended != provider.rebuild_recommended ||
      !NearlyEqual(expected.list_imbalance_ratio,
                   provider.list_imbalance_ratio) ||
      !NearlyEqual(expected.residual_error_mean,
                   provider.residual_error_mean) ||
      !NearlyEqual(expected.compression_error_mean,
                   provider.compression_error_mean)) {
    return false;
  }
  if (expected.centroids.size() != provider.centroids.size() ||
      expected.lists.size() != provider.lists.size()) {
    return false;
  }
  for (std::size_t i = 0; i < provider.centroids.size(); ++i) {
    if (expected.centroids[i].assigned_count !=
            provider.centroids[i].assigned_count ||
        !NearlyEqual(expected.centroids[i].residual_error_sum,
                     provider.centroids[i].residual_error_sum)) {
      return false;
    }
  }
  for (std::size_t i = 0; i < provider.lists.size(); ++i) {
    if (expected.lists[i].live_count != provider.lists[i].live_count ||
        expected.lists[i].tombstone_count != provider.lists[i].tombstone_count ||
        !NearlyEqual(expected.lists[i].residual_error_sum,
                     provider.lists[i].residual_error_sum)) {
      return false;
    }
  }
  return true;
}

bool ProviderValid(const VectorIvfPqPhysicalProvider& provider) {
  return provider.artifact_kind == kVectorIvfPqPhysicalProviderArtifactKind &&
         SameFormatVersion(provider.format_version,
                           {kVectorIvfPqPhysicalProviderCurrentMajor,
                            kVectorIvfPqPhysicalProviderCurrentMinor}) &&
         PageExtentSummaryUuidTextValid(provider.relation_uuid) &&
         PageExtentSummaryUuidTextValid(provider.index_uuid) &&
         PageExtentSummaryUuidTextValid(provider.provider_uuid) &&
         provider.base_generation > 0 &&
         provider.provider_generation > 0 &&
         provider.training_generation > 0 &&
         DescriptorSafe(provider.descriptor) &&
         MetricSafe(provider.metric) &&
         ProfileSafe(provider.profile, provider.descriptor) &&
         provider.centroid_training_present &&
         provider.list_assignment_present &&
         provider.nprobe_planner_present &&
         provider.ivf_list_storage_present &&
         provider.vector_payload_storage_present &&
         provider.compressed_code_storage_present &&
         provider.sq8_codec_present &&
         provider.pq_codec_present &&
         provider.exact_rerank_present &&
         provider.metadata_prefilter_present &&
         provider.candidate_set_input_present &&
         provider.tombstones_present &&
         provider.generation_evidence_present &&
         provider.telemetry_present &&
         ProviderAuthorityClean(provider) &&
         CodebooksValid(provider) &&
         ListsValid(provider) &&
         ProviderStatisticsValid(provider);
}

bool BuildRequestValid(const VectorIvfPqBuildRequest& request) {
  return PageExtentSummaryUuidTextValid(request.relation_uuid) &&
         PageExtentSummaryUuidTextValid(request.index_uuid) &&
         PageExtentSummaryUuidTextValid(request.provider_uuid) &&
         request.base_generation > 0 &&
         request.provider_generation > 0 &&
         request.training_generation > 0 &&
         DescriptorSafe(request.descriptor) &&
         MetricSafe(request.metric) &&
         ProfileSafe(request.profile, request.descriptor) &&
         RecheckProofValid(request.recheck_proof) &&
         std::all_of(request.rows.begin(), request.rows.end(),
                     [&](const auto& row) {
                       return SourceRowValid(row, request.descriptor);
                     });
}

VectorIvfPqStoredVector EntryFromSource(
    const VectorIvfPqPhysicalProvider& provider,
    const VectorIvfPqSourceRow& source,
    u64 generation) {
  double residual = 0.0;
  VectorIvfPqStoredVector entry;
  entry.locator = source.locator;
  entry.list_id = AssignList(provider, source.vector, &residual);
  entry.exact_payload = EncodeVectorExactPayload(source.vector, provider.descriptor);
  entry.compressed_code = EncodeApproximate(provider, source.vector, entry.list_id);
  entry.insert_generation = generation;
  return entry;
}

VectorIvfPqPhysicalProvider BuildProviderFromRows(
    const VectorIvfPqBuildRequest& request,
    std::vector<VectorIvfPqSourceRow> rows) {
  VectorIvfPqPhysicalProvider provider;
  provider.relation_uuid = request.relation_uuid;
  provider.index_uuid = request.index_uuid;
  provider.provider_uuid = request.provider_uuid;
  provider.base_generation = request.base_generation;
  provider.provider_generation = request.provider_generation;
  provider.training_generation = request.training_generation;
  provider.training_generation_evidence = request.training_generation;
  provider.mutation_generation_evidence = request.provider_generation;
  provider.descriptor = request.descriptor;
  provider.metric = request.metric;
  provider.profile = request.profile;
  SetProviderEvidence(&provider);
  if (rows.empty()) {
    return provider;
  }
  const auto training =
      DeterministicTrainingSet(rows, request.profile.max_training_rows);
  provider.centroids = TrainCentroids(training, request);
  provider.lists.resize(provider.centroids.size());
  for (u32 i = 0; i < provider.lists.size(); ++i) {
    provider.lists[i].list_id = i;
    provider.lists[i].centroid_id = i;
  }
  if (provider.profile.compression == VectorIvfPqCompression::sq8) {
    provider.sq8_axes = TrainSq8(training, provider.centroids,
                                 provider.descriptor, provider.metric);
  } else if (provider.profile.compression == VectorIvfPqCompression::pq) {
    provider.pq_codebooks = TrainPq(training, provider.centroids, request);
  }
  for (const auto& row : rows) {
    auto entry = EntryFromSource(provider, row, provider.provider_generation);
    provider.lists[entry.list_id].entries.push_back(std::move(entry));
  }
  for (auto& list : provider.lists) {
    std::sort(list.entries.begin(), list.entries.end(),
              [](const auto& left, const auto& right) {
                return LocatorLess(left.locator, right.locator);
              });
  }
  RefreshTelemetry(&provider);
  SetProviderEvidence(&provider);
  return provider;
}

const VectorIvfPqStoredVector* FindEntry(
    const VectorIvfPqPhysicalProvider& provider,
    const TextInvertedRowLocator& locator,
    u32* list_id_out = nullptr,
    std::size_t* entry_index_out = nullptr) {
  for (const auto& list : provider.lists) {
    auto iter = std::lower_bound(
        list.entries.begin(), list.entries.end(), locator,
        [](const VectorIvfPqStoredVector& entry,
           const TextInvertedRowLocator& value) {
          return LocatorLess(entry.locator, value);
        });
    if (iter != list.entries.end() && LocatorEqual(iter->locator, locator)) {
      if (list_id_out != nullptr) *list_id_out = list.list_id;
      if (entry_index_out != nullptr) {
        *entry_index_out =
            static_cast<std::size_t>(std::distance(list.entries.begin(), iter));
      }
      return &*iter;
    }
  }
  return nullptr;
}

bool SourceMatchesEntry(const VectorIvfPqSourceRow& source,
                        const VectorIvfPqStoredVector& entry,
                        const VectorIvfPqDescriptor& descriptor) {
  return LocatorEqual(source.locator, entry.locator) &&
         EncodeVectorExactPayload(source.vector, descriptor) == entry.exact_payload;
}

std::vector<u32> PlanNprobe(const VectorIvfPqPhysicalProvider& provider,
                            const std::vector<float>& query,
                            u32 requested_nprobe) {
  struct ScoredList {
    u32 list_id = 0;
    double score = 0.0;
  };
  std::vector<ScoredList> scored;
  const u32 nprobe =
      std::min<u32>(std::max(requested_nprobe, provider.profile.nprobe),
                    provider.centroids.size());
  for (const auto& centroid : provider.centroids) {
    u64 ops = 0;
    scored.push_back({centroid.centroid_id,
                      ScoreVectorPair(query, centroid.vector,
                                      provider.metric.metric_kind, &ops)});
  }
  const bool lower = LowerScoreBetter(provider.metric.metric_kind);
  std::sort(scored.begin(), scored.end(), [&](const auto& left,
                                              const auto& right) {
    if (std::fabs(left.score - right.score) > 1e-12) {
      return lower ? left.score < right.score : left.score > right.score;
    }
    return left.list_id < right.list_id;
  });
  std::vector<u32> selected;
  for (u32 i = 0; i < nprobe && i < scored.size(); ++i) {
    selected.push_back(scored[i].list_id);
  }
  std::sort(selected.begin(), selected.end());
  return selected;
}

bool RuntimeRequestSafe(const VectorIvfPqQueryRequest& request) {
  return ProviderValid(request.provider) &&
         RecheckProofValid(request.recheck_proof) &&
         request.descriptor_epoch_current &&
         request.metric_resource_epoch_current &&
         request.training_generation_current &&
         !request.descriptor_store_scan &&
         !request.behavior_store_scan &&
         !request.contract_only_fallback &&
         !request.provider_only_fallback &&
         !request.queries.empty();
}

}  // namespace

VectorIvfPqBuildResult BuildVectorIvfPqPhysicalProvider(
    const VectorIvfPqBuildRequest& request) {
  if (!RecheckProofAuthorityClean(request.recheck_proof) ||
      !DescriptorAuthorityClean(request.descriptor) ||
      !MetricAuthorityClean(request.metric)) {
    return BuildFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED",
        "index.vector_ivf_pq_physical_provider.authority_claim_refused",
        "vector IVF/PQ provider is candidate evidence only and cannot claim transaction, visibility, security, index, provider, parser, donor, or write-ahead authority");
  }
  if (!RecheckProofValid(request.recheck_proof)) {
    return BuildFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_ivf_pq_physical_provider.missing_exact_recheck");
  }
  if (request.descriptor.element_profile != VectorExactElementProfile::fp32) {
    return BuildFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.UNSUPPORTED_PROFILE",
        "index.vector_ivf_pq_physical_provider.unsupported_profile");
  }
  if (!DescriptorSafe(request.descriptor)) {
    return BuildFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.STALE_DESCRIPTOR",
        "index.vector_ivf_pq_physical_provider.stale_descriptor");
  }
  if (!MetricSafe(request.metric)) {
    return BuildFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.UNSAFE_METRIC_RESOURCE",
        "index.vector_ivf_pq_physical_provider.unsafe_metric_resource");
  }
  if (!ProfileSafe(request.profile, request.descriptor)) {
    return BuildFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.UNSUPPORTED_PROFILE",
        "index.vector_ivf_pq_physical_provider.unsupported_profile");
  }
  for (const auto& row : request.rows) {
    if (row.vector.size() != request.descriptor.dimensions ||
        !FiniteVector(row.vector)) {
      return BuildFailure(
          "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.DIMENSION_MISMATCH",
          "index.vector_ivf_pq_physical_provider.dimension_mismatch");
    }
  }
  if (!BuildRequestValid(request)) {
    return BuildFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.BUILD_REFUSED",
        "index.vector_ivf_pq_physical_provider.build_refused");
  }
  std::vector<VectorIvfPqSourceRow> rows = request.rows;
  std::sort(rows.begin(), rows.end(), [](const auto& left,
                                         const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
  for (std::size_t i = 1; i < rows.size(); ++i) {
    if (LocatorEqual(rows[i - 1].locator, rows[i].locator)) {
      return BuildFailure(
          "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.DUPLICATE_ROW_LOCATOR",
          "index.vector_ivf_pq_physical_provider.duplicate_row_locator");
    }
  }
  auto provider = BuildProviderFromRows(request, rows);
  if (!ProviderValid(provider)) {
    return BuildFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.INVALID_LIST",
        "index.vector_ivf_pq_physical_provider.invalid_list");
  }
  VectorIvfPqBuildResult result;
  result.status = OkStatus();
  result.provider = std::move(provider);
  result.built = true;
  result.fail_closed = false;
  return result;
}

VectorIvfPqSerializeResult SerializeVectorIvfPqPhysicalProvider(
    const VectorIvfPqPhysicalProvider& provider) {
  VectorIvfPqSerializeResult result;
  if (!ProviderValid(provider)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeVectorIvfPqPhysicalProviderDiagnostic(
        result.status,
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.SERIALIZE_REFUSED",
        "index.vector_ivf_pq_physical_provider.serialize_refused");
    return result;
  }
  std::vector<byte> bytes;
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  AppendU32(&bytes, kVectorIvfPqPhysicalProviderCurrentMajor);
  AppendU32(&bytes, kVectorIvfPqPhysicalProviderCurrentMinor);
  AppendU64(&bytes, 0);
  AppendString(&bytes, provider.relation_uuid);
  AppendString(&bytes, provider.index_uuid);
  AppendString(&bytes, provider.provider_uuid);
  AppendU64(&bytes, provider.base_generation);
  AppendU64(&bytes, provider.provider_generation);
  AppendU64(&bytes, provider.training_generation);
  AppendU32(&bytes, provider.descriptor.dimensions);
  AppendU64(&bytes, provider.descriptor.descriptor_epoch);
  AppendString(&bytes, provider.metric.metric_resource_uuid);
  AppendU64(&bytes, provider.metric.metric_resource_epoch);
  AppendU32(&bytes, static_cast<u32>(provider.metric.metric_kind));
  AppendU32(&bytes, static_cast<u32>(provider.profile.compression));
  AppendU32(&bytes, provider.profile.centroid_count);
  AppendU32(&bytes, provider.profile.nprobe);
  AppendU32(&bytes, provider.profile.training_iterations);
  AppendU32(&bytes, provider.profile.max_training_rows);
  AppendU32(&bytes, provider.profile.pq_subspaces);
  AppendU32(&bytes, provider.profile.pq_codewords);
  AppendF64(&bytes, provider.profile.retrain_imbalance_ratio);
  AppendF64(&bytes, provider.profile.rebuild_tombstone_ratio);
  AppendU64(&bytes, provider.mutation_generation_evidence);
  AppendU64(&bytes, provider.training_generation_evidence);
  AppendU64(&bytes, provider.live_vector_count);
  AppendU64(&bytes, provider.tombstone_count);
  AppendF64(&bytes, provider.list_imbalance_ratio);
  AppendF64(&bytes, provider.residual_error_mean);
  AppendF64(&bytes, provider.compression_error_mean);
  AppendBool(&bytes, provider.retrain_recommended);
  AppendBool(&bytes, provider.rebuild_recommended);
  AppendU64(&bytes, provider.last_query_selected_lists);
  AppendU64(&bytes, provider.last_query_candidate_count);
  AppendU64(&bytes, provider.last_query_exact_rerank_count);
  AppendU64(&bytes, provider.last_query_latency_units);
  AppendF64(&bytes, provider.last_query_recall_floor);
  AppendU64(&bytes, static_cast<u64>(provider.centroids.size()));
  for (const auto& centroid : provider.centroids) {
    AppendU32(&bytes, centroid.centroid_id);
    AppendVector(&bytes, centroid.vector);
    AppendU64(&bytes, centroid.assigned_count);
    AppendF64(&bytes, centroid.residual_error_sum);
  }
  AppendU64(&bytes, static_cast<u64>(provider.sq8_axes.size()));
  for (const auto& axis : provider.sq8_axes) {
    AppendF32(&bytes, axis.min_value);
    AppendF32(&bytes, axis.scale);
  }
  AppendU64(&bytes, static_cast<u64>(provider.pq_codebooks.size()));
  for (const auto& codebook : provider.pq_codebooks) {
    AppendU32(&bytes, codebook.subspace_id);
    AppendU32(&bytes, codebook.offset);
    AppendU32(&bytes, codebook.width);
    AppendU32(&bytes, static_cast<u32>(codebook.centroids.size()));
    for (const auto& centroid : codebook.centroids) AppendVector(&bytes, centroid);
  }
  AppendU64(&bytes, static_cast<u64>(provider.lists.size()));
  for (const auto& list : provider.lists) {
    AppendU32(&bytes, list.list_id);
    AppendU32(&bytes, list.centroid_id);
    AppendU64(&bytes, list.live_count);
    AppendU64(&bytes, list.tombstone_count);
    AppendF64(&bytes, list.residual_error_sum);
    AppendU64(&bytes, static_cast<u64>(list.entries.size()));
    for (const auto& entry : list.entries) {
      AppendLocator(&bytes, entry.locator);
      AppendU32(&bytes, entry.list_id);
      AppendBytes(&bytes, entry.exact_payload);
      AppendBytes(&bytes, entry.compressed_code);
      AppendBool(&bytes, entry.tombstoned);
      AppendU64(&bytes, entry.insert_generation);
      AppendU64(&bytes, entry.delete_generation);
    }
  }
  result.checksum = ComputeChecksum(bytes);
  StoreLittle64(bytes.data() + 16, result.checksum);
  result.status = OkStatus();
  result.bytes = std::move(bytes);
  return result;
}

VectorIvfPqOpenResult OpenVectorIvfPqPhysicalProvider(
    const VectorIvfPqOpenRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return OpenFailure(
        VectorIvfPqOpenClass::missing_exact_recheck_proof,
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_ivf_pq_physical_provider.missing_exact_recheck");
  }
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(VectorIvfPqOpenClass::corrupt_payload,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_ivf_pq_physical_provider.corrupt_payload");
  }
  const u64 stored_checksum = LoadLittle64(request.bytes.data() + 16);
  if (stored_checksum == 0 ||
      stored_checksum != ComputeChecksum(request.bytes)) {
    return OpenFailure(VectorIvfPqOpenClass::bad_checksum,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.BAD_CHECKSUM",
                       "index.vector_ivf_pq_physical_provider.bad_checksum");
  }
  Reader reader(request.bytes);
  reader.SetOffset(8);
  VectorIvfPqPhysicalProvider provider;
  u32 major = 0;
  u32 minor = 0;
  u64 checksum = 0;
  u32 metric_kind = 0;
  u32 compression = 0;
  u64 count = 0;
  if (!reader.ReadU32(&major) ||
      !reader.ReadU32(&minor) ||
      !reader.ReadU64(&checksum)) {
    return OpenFailure(VectorIvfPqOpenClass::corrupt_payload,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_ivf_pq_physical_provider.corrupt_payload");
  }
  provider.format_version = {major, minor};
  if (!SameFormatVersion(provider.format_version,
                         {kVectorIvfPqPhysicalProviderCurrentMajor,
                          kVectorIvfPqPhysicalProviderCurrentMinor})) {
    return OpenFailure(VectorIvfPqOpenClass::stale_format,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.STALE_FORMAT",
                       "index.vector_ivf_pq_physical_provider.stale_format");
  }
  if (!reader.ReadString(&provider.relation_uuid) ||
      !reader.ReadString(&provider.index_uuid) ||
      !reader.ReadString(&provider.provider_uuid) ||
      !reader.ReadU64(&provider.base_generation) ||
      !reader.ReadU64(&provider.provider_generation) ||
      !reader.ReadU64(&provider.training_generation) ||
      !reader.ReadU32(&provider.descriptor.dimensions) ||
      !reader.ReadU64(&provider.descriptor.descriptor_epoch) ||
      !reader.ReadString(&provider.metric.metric_resource_uuid) ||
      !reader.ReadU64(&provider.metric.metric_resource_epoch) ||
      !reader.ReadU32(&metric_kind) ||
      !reader.ReadU32(&compression) ||
      !reader.ReadU32(&provider.profile.centroid_count) ||
      !reader.ReadU32(&provider.profile.nprobe) ||
      !reader.ReadU32(&provider.profile.training_iterations) ||
      !reader.ReadU32(&provider.profile.max_training_rows) ||
      !reader.ReadU32(&provider.profile.pq_subspaces) ||
      !reader.ReadU32(&provider.profile.pq_codewords) ||
      !reader.ReadF64(&provider.profile.retrain_imbalance_ratio) ||
      !reader.ReadF64(&provider.profile.rebuild_tombstone_ratio) ||
      !reader.ReadU64(&provider.mutation_generation_evidence) ||
      !reader.ReadU64(&provider.training_generation_evidence) ||
      !reader.ReadU64(&provider.live_vector_count) ||
      !reader.ReadU64(&provider.tombstone_count) ||
      !reader.ReadF64(&provider.list_imbalance_ratio) ||
      !reader.ReadF64(&provider.residual_error_mean) ||
      !reader.ReadF64(&provider.compression_error_mean) ||
      !reader.ReadBool(&provider.retrain_recommended) ||
      !reader.ReadBool(&provider.rebuild_recommended) ||
      !reader.ReadU64(&provider.last_query_selected_lists) ||
      !reader.ReadU64(&provider.last_query_candidate_count) ||
      !reader.ReadU64(&provider.last_query_exact_rerank_count) ||
      !reader.ReadU64(&provider.last_query_latency_units) ||
      !reader.ReadF64(&provider.last_query_recall_floor) ||
      !reader.ReadU64(&count) ||
      count > kMaxCentroids) {
    return OpenFailure(VectorIvfPqOpenClass::corrupt_payload,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_ivf_pq_physical_provider.corrupt_payload");
  }
  provider.descriptor.element_profile = VectorExactElementProfile::fp32;
  provider.descriptor.deterministic = true;
  provider.descriptor.descriptor_safe = true;
  provider.metric.metric_kind = static_cast<VectorExactMetricKind>(metric_kind);
  provider.metric.deterministic = true;
  provider.metric.safe = true;
  provider.profile.compression = static_cast<VectorIvfPqCompression>(compression);
  for (u64 i = 0; i < count; ++i) {
    VectorIvfPqCentroid centroid;
    if (!reader.ReadU32(&centroid.centroid_id) ||
        !reader.ReadVector(&centroid.vector, kMaxDimensions) ||
        !reader.ReadU64(&centroid.assigned_count) ||
        !reader.ReadF64(&centroid.residual_error_sum)) {
      return OpenFailure(
          VectorIvfPqOpenClass::invalid_centroid,
          "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.INVALID_CENTROID",
          "index.vector_ivf_pq_physical_provider.invalid_centroid");
    }
    provider.centroids.push_back(std::move(centroid));
  }
  if (!reader.ReadU64(&count) || count > kMaxDimensions) {
    return OpenFailure(VectorIvfPqOpenClass::corrupt_payload,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_ivf_pq_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < count; ++i) {
    VectorIvfPqSq8Axis axis;
    if (!reader.ReadF32(&axis.min_value) || !reader.ReadF32(&axis.scale)) {
      return OpenFailure(
          VectorIvfPqOpenClass::invalid_codebook,
          "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.INVALID_CODEBOOK",
          "index.vector_ivf_pq_physical_provider.invalid_codebook");
    }
    provider.sq8_axes.push_back(axis);
  }
  if (!reader.ReadU64(&count) || count > kMaxPqSubspaces) {
    return OpenFailure(VectorIvfPqOpenClass::corrupt_payload,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_ivf_pq_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < count; ++i) {
    VectorIvfPqCodebook codebook;
    u32 codeword_count = 0;
    if (!reader.ReadU32(&codebook.subspace_id) ||
        !reader.ReadU32(&codebook.offset) ||
        !reader.ReadU32(&codebook.width) ||
        !reader.ReadU32(&codeword_count) ||
        codeword_count > kMaxPqCodewords) {
      return OpenFailure(
          VectorIvfPqOpenClass::invalid_codebook,
          "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.INVALID_CODEBOOK",
          "index.vector_ivf_pq_physical_provider.invalid_codebook");
    }
    for (u32 c = 0; c < codeword_count; ++c) {
      std::vector<float> centroid;
      if (!reader.ReadVector(&centroid, kMaxDimensions)) {
        return OpenFailure(
            VectorIvfPqOpenClass::invalid_codebook,
            "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.INVALID_CODEBOOK",
            "index.vector_ivf_pq_physical_provider.invalid_codebook");
      }
      codebook.centroids.push_back(std::move(centroid));
    }
    provider.pq_codebooks.push_back(std::move(codebook));
  }
  if (!reader.ReadU64(&count) || count > kMaxCentroids) {
    return OpenFailure(VectorIvfPqOpenClass::corrupt_payload,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_ivf_pq_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < count; ++i) {
    VectorIvfPqList list;
    u64 entry_count = 0;
    if (!reader.ReadU32(&list.list_id) ||
        !reader.ReadU32(&list.centroid_id) ||
        !reader.ReadU64(&list.live_count) ||
        !reader.ReadU64(&list.tombstone_count) ||
        !reader.ReadF64(&list.residual_error_sum) ||
        !reader.ReadU64(&entry_count) ||
        entry_count > kMaxRows) {
      return OpenFailure(VectorIvfPqOpenClass::invalid_list,
                         "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.INVALID_LIST",
                         "index.vector_ivf_pq_physical_provider.invalid_list");
    }
    for (u64 e = 0; e < entry_count; ++e) {
      VectorIvfPqStoredVector entry;
      if (!reader.ReadLocator(&entry.locator) ||
          !reader.ReadU32(&entry.list_id) ||
          !reader.ReadBytes(&entry.exact_payload) ||
          !reader.ReadBytes(&entry.compressed_code) ||
          !reader.ReadBool(&entry.tombstoned) ||
          !reader.ReadU64(&entry.insert_generation) ||
          !reader.ReadU64(&entry.delete_generation)) {
        return OpenFailure(VectorIvfPqOpenClass::invalid_code,
                           "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.INVALID_CODE",
                           "index.vector_ivf_pq_physical_provider.invalid_code");
      }
      list.entries.push_back(std::move(entry));
    }
    provider.lists.push_back(std::move(list));
  }
  if (!reader.Done()) {
    return OpenFailure(VectorIvfPqOpenClass::corrupt_payload,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_ivf_pq_physical_provider.corrupt_payload");
  }
  provider.profile.deterministic_training = true;
  provider.profile.scalar_kernel_present = true;
  SetProviderEvidence(&provider);
  if (!MetricSafe(provider.metric)) {
    return OpenFailure(
        VectorIvfPqOpenClass::unsafe_metric_resource,
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.UNSAFE_METRIC_RESOURCE",
        "index.vector_ivf_pq_physical_provider.unsafe_metric_resource");
  }
  if ((request.expected_relation_uuid_present &&
       request.expected_relation_uuid != provider.relation_uuid) ||
      (request.expected_index_uuid_present &&
       request.expected_index_uuid != provider.index_uuid) ||
      (request.expected_provider_uuid_present &&
       request.expected_provider_uuid != provider.provider_uuid)) {
    return OpenFailure(VectorIvfPqOpenClass::identity_mismatch,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.IDENTITY_MISMATCH",
                       "index.vector_ivf_pq_physical_provider.identity_mismatch");
  }
  if ((request.expected_base_generation_present &&
       request.expected_base_generation != provider.base_generation) ||
      (request.expected_provider_generation_present &&
       request.expected_provider_generation != provider.provider_generation) ||
      (request.expected_training_generation_present &&
       request.expected_training_generation != provider.training_generation)) {
    return OpenFailure(VectorIvfPqOpenClass::stale_generation,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.STALE_GENERATION",
                       "index.vector_ivf_pq_physical_provider.stale_generation");
  }
  if (request.expected_descriptor_epoch_present &&
      request.expected_descriptor_epoch != provider.descriptor.descriptor_epoch) {
    return OpenFailure(
        VectorIvfPqOpenClass::stale_descriptor_epoch,
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.vector_ivf_pq_physical_provider.stale_descriptor_epoch");
  }
  if (request.expected_metric_resource_epoch_present &&
      request.expected_metric_resource_epoch !=
          provider.metric.metric_resource_epoch) {
    return OpenFailure(
        VectorIvfPqOpenClass::stale_metric_epoch,
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.STALE_METRIC_EPOCH",
        "index.vector_ivf_pq_physical_provider.stale_metric_epoch");
  }
  if (request.expected_dimensions_present &&
      request.expected_dimensions != provider.descriptor.dimensions) {
    return OpenFailure(
        VectorIvfPqOpenClass::dimension_mismatch,
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.DIMENSION_MISMATCH",
        "index.vector_ivf_pq_physical_provider.dimension_mismatch");
  }
  if (!ProviderValid(provider)) {
    return OpenFailure(VectorIvfPqOpenClass::invalid_list,
                       "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.INVALID_LIST",
                       "index.vector_ivf_pq_physical_provider.invalid_list");
  }
  VectorIvfPqOpenResult result;
  result.status = OkStatus();
  result.open_class = VectorIvfPqOpenClass::current;
  result.provider = std::move(provider);
  result.fail_closed = false;
  result.actions.push_back("open_vector_ivf_pq_physical_provider");
  return result;
}

VectorIvfPqQueryResult QueryVectorIvfPqPhysicalProvider(
    const VectorIvfPqQueryRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return QueryFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_ivf_pq_physical_provider.missing_exact_recheck");
  }
  if (!request.descriptor_epoch_current) {
    return QueryFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.vector_ivf_pq_physical_provider.stale_descriptor_epoch");
  }
  if (!request.metric_resource_epoch_current) {
    return QueryFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.STALE_METRIC_EPOCH",
        "index.vector_ivf_pq_physical_provider.stale_metric_epoch");
  }
  if (!request.training_generation_current) {
    return QueryFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.STALE_TRAINING_GENERATION",
        "index.vector_ivf_pq_physical_provider.stale_training_generation");
  }
  if (!RuntimeRequestSafe(request)) {
    return QueryFailure(
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
        "index.vector_ivf_pq_physical_provider.runtime_refused");
  }
  VectorIvfPqQueryResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.nprobe_planner_used = true;
  result.compressed_code_search_used = true;
  result.provider_after_telemetry = request.provider;
  result.evidence = {
      kVectorIvfPqPhysicalProviderSearchKey,
      "vector_ivf_pq_physical_provider.nprobe_planner=true",
      "vector_ivf_pq_physical_provider.compressed_code_search=true",
      "vector_ivf_pq_physical_provider.exact_rerank_performed=true",
      "vector_ivf_pq_physical_provider.candidate_rows_only=true",
      "vector_ivf_pq_physical_provider.final_rows_authorized=false"};
  for (const auto& query : request.queries) {
    if (query.vector.size() != request.provider.descriptor.dimensions ||
        query.top_k == 0 ||
        !FiniteVector(query.vector)) {
      return QueryFailure(
          "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.DIMENSION_MISMATCH",
          "index.vector_ivf_pq_physical_provider.dimension_mismatch");
    }
    std::vector<TextInvertedRowLocator> candidate_set = query.candidate_set;
    if (!CandidateSetValid(candidate_set)) {
      return QueryFailure(
          "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.CANDIDATE_SET_REFUSED",
          "index.vector_ivf_pq_physical_provider.candidate_set_refused");
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

    VectorIvfPqSingleQueryResult single;
    single.ok = true;
    single.nprobe_planner_used = true;
    single.exact_rerank_performed = true;
    if (request.provider.centroids.empty()) {
      result.batch_results.push_back(std::move(single));
      continue;
    }
    single.selected_list_ids =
        PlanNprobe(request.provider, query.vector, query.nprobe);
    std::set<std::tuple<u64, std::string, std::string>> seen;
    const bool lower = LowerScoreBetter(request.provider.metric.metric_kind);
    std::vector<VectorIvfPqCandidate> approximate;
    auto consider = [&](const VectorIvfPqStoredVector& entry,
                        bool reached_by_nprobe) -> bool {
      const auto key = std::make_tuple(entry.locator.row_ordinal,
                                       entry.locator.row_uuid,
                                       entry.locator.version_uuid);
      if (!seen.insert(key).second || entry.tombstoned) return true;
      ++single.candidates_considered;
      if (!candidate_set.empty() &&
          !CandidateSetContains(candidate_set, entry.locator)) {
        ++single.candidates_filtered;
        return true;
      }
      if (query.metadata_prefilter && !query.metadata_prefilter(entry.locator)) {
        ++single.candidates_filtered;
        return true;
      }
      const auto approx = DecodeApproximate(request.provider, entry);
      if (approx.size() != request.provider.descriptor.dimensions) return false;
      ++single.compressed_decode_count;
      u64 ops = 0;
      VectorIvfPqCandidate candidate;
      candidate.locator = entry.locator;
      candidate.list_id = entry.list_id;
      candidate.approximate_score = ScoreVectorPair(
          query.vector, approx, request.provider.metric.metric_kind, &ops);
      candidate.lower_score_better = lower;
      candidate.reached_by_nprobe = reached_by_nprobe;
      candidate.compressed_code_scored = true;
      candidate.metadata_prefilter_passed = true;
      candidate.candidate_set_member = true;
      single.scalar_kernel_consumed_count += ops;
      approximate.push_back(std::move(candidate));
      return true;
    };
    for (u32 list_id : single.selected_list_ids) {
      for (const auto& entry : request.provider.lists[list_id].entries) {
        if (!consider(entry, true)) {
          return QueryFailure(
              "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.INVALID_CODE",
              "index.vector_ivf_pq_physical_provider.invalid_code");
        }
      }
    }
    for (const auto& locator : candidate_set) {
      u32 list_id = 0;
      std::size_t entry_index = 0;
      const auto* entry = FindEntry(request.provider, locator, &list_id,
                                    &entry_index);
      if (entry != nullptr &&
          std::find(single.selected_list_ids.begin(), single.selected_list_ids.end(),
                    list_id) == single.selected_list_ids.end()) {
        if (!consider(*entry, false)) {
          return QueryFailure(
              "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.INVALID_CODE",
              "index.vector_ivf_pq_physical_provider.invalid_code");
        }
      }
    }
    std::sort(approximate.begin(), approximate.end(), [&](const auto& left,
                                                          const auto& right) {
      return ScoreBetter(left.approximate_score, left.locator,
                         right.approximate_score, right.locator, lower);
    });
    std::vector<VectorIvfPqCandidate> reranked;
    for (auto candidate : approximate) {
      const auto* entry = FindEntry(request.provider, candidate.locator);
      if (entry == nullptr) continue;
      const auto exact = DecodeVectorExactPayload(entry->exact_payload,
                                                  request.provider.descriptor);
      if (exact.size() != request.provider.descriptor.dimensions) {
        return QueryFailure(
            "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
            "index.vector_ivf_pq_physical_provider.corrupt_payload");
      }
      u64 ops = 0;
      candidate.exact_score = ScoreVectorPair(
          query.vector, exact, request.provider.metric.metric_kind, &ops);
      candidate.decoded_from_physical_payload = true;
      candidate.exact_payload_reranked = true;
      candidate.exact_rerank_proof_verified = true;
      candidate.source_recheck_evidence_ref = request.recheck_proof.evidence_ref;
      single.scalar_kernel_consumed_count += ops;
      ++single.exact_rerank_count;
      reranked.push_back(std::move(candidate));
    }
    std::sort(reranked.begin(), reranked.end(), [&](const auto& left,
                                                    const auto& right) {
      return ScoreBetter(left.exact_score, left.locator, right.exact_score,
                         right.locator, lower);
    });
    if (reranked.size() > query.top_k) reranked.resize(query.top_k);
    single.candidates = std::move(reranked);
    result.scalar_kernel_consumed =
        result.scalar_kernel_consumed || single.scalar_kernel_consumed_count > 0;
    result.exact_rerank_performed =
        result.exact_rerank_performed || single.exact_rerank_count > 0;
    result.provider_after_telemetry.last_query_selected_lists =
        single.selected_list_ids.size();
    result.provider_after_telemetry.last_query_candidate_count =
        single.candidates_considered - single.candidates_filtered;
    result.provider_after_telemetry.last_query_exact_rerank_count =
        single.exact_rerank_count;
    result.provider_after_telemetry.last_query_latency_units =
        single.candidates_considered + single.scalar_kernel_consumed_count;
    result.provider_after_telemetry.last_query_recall_floor =
        single.candidates.empty() ? 0.0 : 1.0;
    result.batch_results.push_back(std::move(single));
  }
  return result;
}

VectorIvfPqMutationResult ApplyVectorIvfPqPhysicalMutation(
    const VectorIvfPqPhysicalProvider& provider,
    const VectorIvfPqMutation& mutation) {
  if (!ProviderValid(provider)) {
    return MutationFailure(
        provider,
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.MUTATION_REFUSED",
        "index.vector_ivf_pq_physical_provider.mutation_refused");
  }
  if (!RecheckProofValid(mutation.recheck_proof)) {
    return MutationFailure(
        provider,
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_ivf_pq_physical_provider.missing_exact_recheck");
  }
  if ((mutation.expected_provider_generation_present &&
       mutation.expected_provider_generation != provider.provider_generation) ||
      (mutation.expected_training_generation_present &&
       mutation.expected_training_generation != provider.training_generation) ||
      (mutation.expected_descriptor_epoch_present &&
       mutation.expected_descriptor_epoch != provider.descriptor.descriptor_epoch) ||
      (mutation.expected_metric_resource_epoch_present &&
       mutation.expected_metric_resource_epoch !=
           provider.metric.metric_resource_epoch)) {
    return MutationFailure(
        provider,
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.STALE_GENERATION",
        "index.vector_ivf_pq_physical_provider.stale_generation");
  }
  VectorIvfPqPhysicalProvider next = provider;
  const u64 next_generation = provider.provider_generation + 1;
  switch (mutation.kind) {
    case VectorIvfPqMutationKind::insert_row: {
      if (!mutation.after_row_present ||
          !SourceRowValid(mutation.after_row, provider.descriptor) ||
          provider.centroids.empty() ||
          FindEntry(provider, mutation.after_row.locator) != nullptr) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.DUPLICATE_ROW_LOCATOR",
            "index.vector_ivf_pq_physical_provider.duplicate_row_locator");
      }
      next.provider_generation = next_generation;
      auto entry = EntryFromSource(next, mutation.after_row, next_generation);
      next.lists[entry.list_id].entries.push_back(std::move(entry));
      break;
    }
    case VectorIvfPqMutationKind::delete_row: {
      if (!mutation.before_row_present ||
          !SourceRowValid(mutation.before_row, provider.descriptor)) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_ivf_pq_physical_provider.mutation_refused");
      }
      u32 list_id = 0;
      std::size_t entry_index = 0;
      const auto* current = FindEntry(next, mutation.before_row.locator,
                                      &list_id, &entry_index);
      if (current == nullptr || current->tombstoned ||
          !SourceMatchesEntry(mutation.before_row, *current,
                              provider.descriptor)) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_ivf_pq_physical_provider.mutation_refused");
      }
      auto& entry = next.lists[list_id].entries[entry_index];
      entry.tombstoned = true;
      entry.delete_generation = next_generation;
      next.provider_generation = next_generation;
      break;
    }
    case VectorIvfPqMutationKind::update_row: {
      if (!mutation.before_row_present ||
          !mutation.after_row_present ||
          !SourceRowValid(mutation.before_row, provider.descriptor) ||
          !SourceRowValid(mutation.after_row, provider.descriptor) ||
          !LocatorEqual(mutation.before_row.locator,
                        mutation.after_row.locator) ||
          provider.centroids.empty()) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_ivf_pq_physical_provider.mutation_refused");
      }
      u32 list_id = 0;
      std::size_t entry_index = 0;
      const auto* current = FindEntry(next, mutation.before_row.locator,
                                      &list_id, &entry_index);
      if (current == nullptr || current->tombstoned ||
          !SourceMatchesEntry(mutation.before_row, *current,
                              provider.descriptor)) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_ivf_pq_physical_provider.mutation_refused");
      }
      next.lists[list_id].entries.erase(
          next.lists[list_id].entries.begin() +
          static_cast<std::ptrdiff_t>(entry_index));
      next.provider_generation = next_generation;
      auto entry = EntryFromSource(next, mutation.after_row, next_generation);
      next.lists[entry.list_id].entries.push_back(std::move(entry));
      break;
    }
  }
  for (auto& list : next.lists) {
    std::sort(list.entries.begin(), list.entries.end(),
              [](const auto& left, const auto& right) {
                return LocatorLess(left.locator, right.locator);
              });
  }
  next.provider_generation = next_generation;
  next.mutation_generation_evidence = next_generation;
  RefreshTelemetry(&next);
  SetProviderEvidence(&next);
  if (!ProviderValid(next)) {
    return MutationFailure(
        provider,
        "INDEX.VECTOR_IVF_PQ_PHYSICAL_PROVIDER.INVALID_LIST",
        "index.vector_ivf_pq_physical_provider.invalid_list");
  }
  VectorIvfPqMutationResult result;
  result.status = OkStatus();
  result.provider = std::move(next);
  result.applied = true;
  result.fail_closed = false;
  result.tombstone_recorded =
      mutation.kind == VectorIvfPqMutationKind::delete_row;
  result.list_assignment_recomputed =
      mutation.kind != VectorIvfPqMutationKind::delete_row;
  result.retrain_recommended = result.provider.retrain_recommended;
  result.rebuild_recommended = result.provider.rebuild_recommended;
  result.actions.push_back("apply_vector_ivf_pq_maintenance_mutation");
  result.actions.push_back("provider_generation_incremented");
  if (result.tombstone_recorded) result.actions.push_back("delete_tombstone_recorded");
  if (result.list_assignment_recomputed) {
    result.actions.push_back("ivf_list_assignment_recomputed");
  }
  return result;
}

const char* VectorIvfPqOpenClassName(VectorIvfPqOpenClass open_class) {
  switch (open_class) {
    case VectorIvfPqOpenClass::current:
      return "current";
    case VectorIvfPqOpenClass::stale_format:
      return "stale_format";
    case VectorIvfPqOpenClass::stale_generation:
      return "stale_generation";
    case VectorIvfPqOpenClass::bad_checksum:
      return "bad_checksum";
    case VectorIvfPqOpenClass::corrupt_payload:
      return "corrupt_payload";
    case VectorIvfPqOpenClass::identity_mismatch:
      return "identity_mismatch";
    case VectorIvfPqOpenClass::stale_descriptor_epoch:
      return "stale_descriptor_epoch";
    case VectorIvfPqOpenClass::stale_metric_epoch:
      return "stale_metric_epoch";
    case VectorIvfPqOpenClass::dimension_mismatch:
      return "dimension_mismatch";
    case VectorIvfPqOpenClass::unsupported_profile:
      return "unsupported_profile";
    case VectorIvfPqOpenClass::unsafe_metric_resource:
      return "unsafe_metric_resource";
    case VectorIvfPqOpenClass::missing_exact_recheck_proof:
      return "missing_exact_recheck_proof";
    case VectorIvfPqOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case VectorIvfPqOpenClass::invalid_centroid:
      return "invalid_centroid";
    case VectorIvfPqOpenClass::invalid_list:
      return "invalid_list";
    case VectorIvfPqOpenClass::invalid_codebook:
      return "invalid_codebook";
    case VectorIvfPqOpenClass::invalid_code:
      return "invalid_code";
    case VectorIvfPqOpenClass::duplicate_row_locator:
      return "duplicate_row_locator";
    case VectorIvfPqOpenClass::refused:
      return "refused";
  }
  return "refused";
}

const char* VectorIvfPqCompressionName(VectorIvfPqCompression compression) {
  switch (compression) {
    case VectorIvfPqCompression::ivf_flat:
      return "ivf_flat";
    case VectorIvfPqCompression::sq8:
      return "sq8";
    case VectorIvfPqCompression::pq:
      return "pq";
  }
  return "unsupported";
}

DiagnosticRecord MakeVectorIvfPqPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(diagnostic_code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.source_component = kVectorIvfPqPhysicalProviderSearchKey;
  diagnostic.remediation_hint =
      "rebuild vector IVF/PQ provider from authoritative source vectors with exact source, MGA, security, metric descriptor, and training proof";
  if (!detail.empty()) {
    diagnostic.arguments.push_back({"detail", std::move(detail)});
  }
  return diagnostic;
}

}  // namespace scratchbird::core::index
