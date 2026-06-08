// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "vector_hnsw_physical_provider.hpp"

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

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'V', 'H',
                                               'N', 'S', 'W', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u32 kMaxDimensions = 16384;
inline constexpr u32 kMaxM = 128;
inline constexpr u32 kMaxEf = 4096;
inline constexpr u32 kMaxLevel = 32;
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

bool DescriptorAuthorityClean(const VectorHnswDescriptor& descriptor) {
  return !descriptor.parser_finality_authority_claimed &&
         !descriptor.donor_finality_authority_claimed &&
         !descriptor.provider_finality_authority_claimed &&
         !descriptor.index_finality_authority_claimed &&
         !descriptor.write_ahead_log_finality_authority_claimed;
}

bool MetricAuthorityClean(const VectorHnswMetricResource& metric) {
  return !metric.parser_finality_authority_claimed &&
         !metric.donor_finality_authority_claimed &&
         !metric.provider_finality_authority_claimed &&
         !metric.index_finality_authority_claimed &&
         !metric.write_ahead_log_finality_authority_claimed;
}

bool RecheckProofAuthorityClean(const VectorHnswRecheckProof& proof) {
  return !proof.parser_finality_authority_claimed &&
         !proof.donor_finality_authority_claimed &&
         !proof.provider_finality_authority_claimed &&
         !proof.index_finality_authority_claimed &&
         !proof.write_ahead_log_finality_authority_claimed &&
         !proof.visibility_authority_claimed &&
         !proof.security_authority_claimed &&
         !proof.transaction_finality_authority_claimed;
}

bool DescriptorSafe(const VectorHnswDescriptor& descriptor) {
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

bool MetricSafe(const VectorHnswMetricResource& metric) {
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

bool RecheckProofValid(const VectorHnswRecheckProof& proof) {
  return proof.proof_supplied &&
         proof.exact_source_vector_available &&
         proof.exact_rerank_proof_supplied &&
         proof.mga_recheck_required &&
         proof.security_recheck_required &&
         !proof.evidence_ref.empty() &&
         RecheckProofAuthorityClean(proof);
}

bool ProfileSafe(const VectorHnswBuildProfile& profile) {
  return profile.m > 0 &&
         profile.m <= kMaxM &&
         profile.ef_construction >= profile.m &&
         profile.ef_construction <= kMaxEf &&
         profile.ef_search > 0 &&
         profile.ef_search <= kMaxEf &&
         profile.max_level <= kMaxLevel &&
         profile.compaction_tombstone_ratio > 0.0 &&
         profile.compaction_tombstone_ratio < 1.0 &&
         profile.deterministic_level_assignment &&
         profile.scalar_kernel_present;
}

bool ProviderAuthorityClean(const VectorHnswPhysicalProvider& provider) {
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

u32 EncodedPayloadBytes(const VectorHnswDescriptor& descriptor) {
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

bool CandidateLess(const VectorHnswCandidate& left,
                   const VectorHnswCandidate& right) {
  return ScoreBetter(left.exact_score, left.locator, right.exact_score,
                     right.locator, left.lower_score_better);
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

std::vector<float> DecodeNodeVector(const VectorHnswGraphNode& node,
                                    const VectorHnswDescriptor& descriptor) {
  return DecodeVectorExactPayload(node.encoded_payload, descriptor);
}

bool SourceRowValid(const VectorHnswSourceRow& row,
                    const VectorHnswDescriptor& descriptor) {
  return LocatorValid(row.locator) &&
         row.vector.size() == descriptor.dimensions &&
         FiniteVector(row.vector);
}

bool NodePayloadValid(const VectorHnswGraphNode& node,
                      const VectorHnswDescriptor& descriptor) {
  return LocatorValid(node.locator) &&
         node.encoded_payload.size() == EncodedPayloadBytes(descriptor);
}

u64 HashBytes(u64 hash, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<u64>(bytes[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

u64 HashString(u64 hash, const std::string& value) {
  return HashBytes(hash, value.data(), value.size());
}

u64 HashLocator(const TextInvertedRowLocator& locator,
                const std::string& provider_uuid,
                u64 training_generation) {
  u64 hash = kFnvOffset;
  hash = HashBytes(hash, &locator.row_ordinal, sizeof(locator.row_ordinal));
  hash = HashString(hash, locator.row_uuid);
  hash = HashString(hash, locator.version_uuid);
  hash = HashString(hash, provider_uuid);
  hash = HashBytes(hash, &training_generation, sizeof(training_generation));
  return hash == 0 ? 1 : hash;
}

u32 DeterministicLevel(const TextInvertedRowLocator& locator,
                       const std::string& provider_uuid,
                       u64 training_generation,
                       u32 max_level) {
  u64 hash = HashLocator(locator, provider_uuid, training_generation);
  u32 level = 0;
  while (level < max_level && (hash & 0x3ull) == 0) {
    ++level;
    hash >>= 2;
  }
  return level;
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

void AppendU64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u64));
  StoreLittle64(out->data() + offset, value);
}

void AppendI32(std::vector<byte>* out, i32 value) {
  AppendU32(out, static_cast<u32>(value));
}

void AppendF64(std::vector<byte>* out, double value) {
  static_assert(sizeof(double) == sizeof(u64),
                "ScratchBird HNSW serialization requires 64-bit double");
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

VectorHnswBuildResult BuildFailure(std::string code,
                                   std::string key,
                                   std::string detail = {}) {
  VectorHnswBuildResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeVectorHnswPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

VectorHnswOpenResult OpenFailure(VectorHnswOpenClass open_class,
                                 std::string code,
                                 std::string key,
                                 std::string detail = {}) {
  VectorHnswOpenResult result;
  result.status = open_class == VectorHnswOpenClass::stale_format ||
                          open_class == VectorHnswOpenClass::stale_generation ||
                          open_class ==
                              VectorHnswOpenClass::stale_descriptor_epoch ||
                          open_class == VectorHnswOpenClass::stale_metric_epoch
                      ? WarnStatus()
                      : ErrorStatus();
  result.open_class = open_class;
  result.fail_closed = true;
  result.restricted_repair_required =
      open_class == VectorHnswOpenClass::bad_checksum ||
      open_class == VectorHnswOpenClass::corrupt_payload ||
      open_class == VectorHnswOpenClass::invalid_graph ||
      open_class == VectorHnswOpenClass::duplicate_row_locator;
  result.diagnostic = MakeVectorHnswPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_vector_hnsw_physical_provider");
  if (result.restricted_repair_required) {
    result.actions.push_back(
        "repair_requires_authoritative_source_vectors_and_exact_recheck");
  }
  return result;
}

VectorHnswQueryResult QueryFailure(std::string code,
                                   std::string key,
                                   std::string detail = {}) {
  VectorHnswQueryResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeVectorHnswPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.evidence.push_back(kVectorHnswPhysicalProviderSearchKey);
  result.evidence.push_back(
      "vector_hnsw_physical_provider.candidates_refused=true");
  result.evidence.push_back(
      "vector_hnsw_physical_provider.exact_source_recheck_required=true");
  result.evidence.push_back(
      "vector_hnsw_physical_provider.mga_recheck_required=true");
  result.evidence.push_back(
      "vector_hnsw_physical_provider.security_recheck_required=true");
  return result;
}

VectorHnswMutationResult MutationFailure(VectorHnswPhysicalProvider provider,
                                         std::string code,
                                         std::string key,
                                         std::string detail = {}) {
  VectorHnswMutationResult result;
  result.status = ErrorStatus();
  result.provider = std::move(provider);
  result.fail_closed = true;
  result.diagnostic = MakeVectorHnswPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_vector_hnsw_maintenance_mutation");
  return result;
}

VectorHnswCompactionResult CompactionFailure(VectorHnswPhysicalProvider provider,
                                             std::string code,
                                             std::string key,
                                             std::string detail = {}) {
  VectorHnswCompactionResult result;
  result.status = ErrorStatus();
  result.provider = std::move(provider);
  result.fail_closed = true;
  result.diagnostic = MakeVectorHnswPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("refuse_vector_hnsw_compaction_rebuild");
  return result;
}

const VectorHnswGraphNode* FindNodeById(
    const VectorHnswPhysicalProvider& provider,
    u32 node_id) {
  for (const auto& node : provider.nodes) {
    if (node.node_id == node_id) {
      return &node;
    }
  }
  return nullptr;
}

VectorHnswGraphNode* FindNodeById(VectorHnswPhysicalProvider* provider,
                                  u32 node_id) {
  for (auto& node : provider->nodes) {
    if (node.node_id == node_id) {
      return &node;
    }
  }
  return nullptr;
}

const VectorHnswGraphNode* FindNodeByLocator(
    const VectorHnswPhysicalProvider& provider,
    const TextInvertedRowLocator& locator) {
  auto iter = std::lower_bound(
      provider.nodes.begin(), provider.nodes.end(), locator,
      [](const VectorHnswGraphNode& node, const TextInvertedRowLocator& value) {
        return LocatorLess(node.locator, value);
      });
  if (iter == provider.nodes.end() || !LocatorEqual(iter->locator, locator)) {
    return nullptr;
  }
  return &*iter;
}

VectorHnswGraphNode* FindNodeByLocator(VectorHnswPhysicalProvider* provider,
                                       const TextInvertedRowLocator& locator) {
  auto iter = std::lower_bound(
      provider->nodes.begin(), provider->nodes.end(), locator,
      [](const VectorHnswGraphNode& node, const TextInvertedRowLocator& value) {
        return LocatorLess(node.locator, value);
      });
  if (iter == provider->nodes.end() || !LocatorEqual(iter->locator, locator)) {
    return nullptr;
  }
  return &*iter;
}

std::vector<const VectorHnswGraphNode*> NodesById(
    const VectorHnswPhysicalProvider& provider) {
  std::vector<const VectorHnswGraphNode*> by_id(provider.nodes.size(), nullptr);
  for (const auto& node : provider.nodes) {
    if (node.node_id < by_id.size()) {
      by_id[node.node_id] = &node;
    }
  }
  return by_id;
}

std::vector<VectorHnswGraphNode*> MutableNodesById(
    VectorHnswPhysicalProvider* provider) {
  std::vector<VectorHnswGraphNode*> by_id(provider->nodes.size(), nullptr);
  for (auto& node : provider->nodes) {
    if (node.node_id < by_id.size()) {
      by_id[node.node_id] = &node;
    }
  }
  return by_id;
}

struct NodeScore {
  u32 node_id = 0;
  double score = 0.0;
  TextInvertedRowLocator locator;
};

bool NodeScoreBetter(const NodeScore& left,
                     const NodeScore& right,
                     bool lower_score_better) {
  return ScoreBetter(left.score, left.locator, right.score, right.locator,
                     lower_score_better);
}

std::vector<NodeScore> SortAndTrim(std::vector<NodeScore> scores,
                                   u32 limit,
                                   bool lower_score_better) {
  std::sort(scores.begin(), scores.end(), [&](const auto& left,
                                              const auto& right) {
    return NodeScoreBetter(left, right, lower_score_better);
  });
  scores.erase(std::unique(scores.begin(), scores.end(),
                           [](const auto& left, const auto& right) {
                             return left.node_id == right.node_id;
                           }),
               scores.end());
  if (scores.size() > limit) {
    scores.resize(limit);
  }
  return scores;
}

NodeScore ScoreNode(const VectorHnswPhysicalProvider& provider,
                    const VectorHnswGraphNode& node,
                    const std::vector<float>& query,
                    u64* scalar_ops) {
  const auto decoded = DecodeNodeVector(node, provider.descriptor);
  NodeScore scored;
  scored.node_id = node.node_id;
  scored.locator = node.locator;
  scored.score = decoded.size() == query.size()
                     ? ScoreVectorPair(query, decoded,
                                       provider.metric.metric_kind, scalar_ops)
                     : std::numeric_limits<double>::infinity();
  return scored;
}

u32 GreedySearchLayer(const VectorHnswPhysicalProvider& provider,
                      u32 entry,
                      const std::vector<float>& query,
                      u32 layer,
                      u64* scalar_ops) {
  const auto by_id = NodesById(provider);
  const bool lower_score_better =
      LowerScoreBetter(provider.metric.metric_kind);
  auto* current = entry < by_id.size() ? by_id[entry] : nullptr;
  if (current == nullptr) {
    return entry;
  }
  NodeScore current_score = ScoreNode(provider, *current, query, scalar_ops);
  bool changed = true;
  while (changed) {
    changed = false;
    if (layer >= current->layer_neighbors.size()) {
      break;
    }
    for (u32 neighbor_id : current->layer_neighbors[layer]) {
      auto* neighbor = neighbor_id < by_id.size() ? by_id[neighbor_id] : nullptr;
      if (neighbor == nullptr || neighbor->tombstoned) {
        continue;
      }
      NodeScore score = ScoreNode(provider, *neighbor, query, scalar_ops);
      if (NodeScoreBetter(score, current_score, lower_score_better)) {
        current = neighbor;
        current_score = score;
        changed = true;
      }
    }
  }
  return current->node_id;
}

std::vector<NodeScore> SearchLayer(const VectorHnswPhysicalProvider& provider,
                                   u32 entry,
                                   const std::vector<float>& query,
                                   u32 layer,
                                   u32 ef,
                                   u64* edges_considered,
                                   u64* nodes_visited,
                                   u64* scalar_ops) {
  const auto by_id = NodesById(provider);
  const bool lower_score_better =
      LowerScoreBetter(provider.metric.metric_kind);
  if (entry >= by_id.size() || by_id[entry] == nullptr) {
    return {};
  }
  std::vector<NodeScore> visited;
  std::vector<NodeScore> frontier;
  std::set<u32> seen;
  const NodeScore entry_score = ScoreNode(provider, *by_id[entry], query,
                                          scalar_ops);
  visited.push_back(entry_score);
  frontier.push_back(entry_score);
  seen.insert(entry);
  ++*nodes_visited;
  while (!frontier.empty()) {
    std::sort(frontier.begin(), frontier.end(), [&](const auto& left,
                                                    const auto& right) {
      return NodeScoreBetter(left, right, lower_score_better);
    });
    const NodeScore current = frontier.front();
    frontier.erase(frontier.begin());
    const auto* node = current.node_id < by_id.size() ? by_id[current.node_id]
                                                      : nullptr;
    if (node == nullptr || layer >= node->layer_neighbors.size()) {
      continue;
    }
    for (u32 neighbor_id : node->layer_neighbors[layer]) {
      ++*edges_considered;
      if (seen.count(neighbor_id) != 0) {
        continue;
      }
      const auto* neighbor =
          neighbor_id < by_id.size() ? by_id[neighbor_id] : nullptr;
      if (neighbor == nullptr || neighbor->tombstoned) {
        continue;
      }
      seen.insert(neighbor_id);
      ++*nodes_visited;
      NodeScore score = ScoreNode(provider, *neighbor, query, scalar_ops);
      visited.push_back(score);
      frontier.push_back(score);
      visited = SortAndTrim(std::move(visited), std::max<u32>(ef, 1),
                            lower_score_better);
      frontier = SortAndTrim(std::move(frontier), std::max<u32>(ef, 1),
                             lower_score_better);
    }
  }
  return SortAndTrim(std::move(visited), std::max<u32>(ef, 1),
                     lower_score_better);
}

void PruneNeighbors(VectorHnswPhysicalProvider* provider,
                    u32 node_id,
                    u32 layer) {
  auto by_id = MutableNodesById(provider);
  auto* node = node_id < by_id.size() ? by_id[node_id] : nullptr;
  if (node == nullptr || layer >= node->layer_neighbors.size()) {
    return;
  }
  std::vector<u32>& neighbors = node->layer_neighbors[layer];
  std::sort(neighbors.begin(), neighbors.end());
  neighbors.erase(std::unique(neighbors.begin(), neighbors.end()),
                  neighbors.end());
  std::vector<NodeScore> scored;
  const auto decoded = DecodeNodeVector(*node, provider->descriptor);
  if (decoded.empty()) {
    return;
  }
  u64 scalar_ops = 0;
  for (u32 neighbor_id : neighbors) {
    auto* neighbor = neighbor_id < by_id.size() ? by_id[neighbor_id] : nullptr;
    if (neighbor == nullptr || neighbor->tombstoned) {
      continue;
    }
    scored.push_back(ScoreNode(*provider, *neighbor, decoded, &scalar_ops));
  }
  const bool lower_score_better =
      LowerScoreBetter(provider->metric.metric_kind);
  scored = SortAndTrim(std::move(scored), provider->profile.m,
                       lower_score_better);
  neighbors.clear();
  for (const auto& score : scored) {
    neighbors.push_back(score.node_id);
  }
  std::sort(neighbors.begin(), neighbors.end());
}

void ConnectLayer(VectorHnswPhysicalProvider* provider,
                  u32 node_id,
                  const std::vector<NodeScore>& selected,
                  u32 layer) {
  auto by_id = MutableNodesById(provider);
  auto* node = node_id < by_id.size() ? by_id[node_id] : nullptr;
  if (node == nullptr || layer >= node->layer_neighbors.size()) {
    return;
  }
  for (const auto& score : selected) {
    auto* neighbor =
        score.node_id < by_id.size() ? by_id[score.node_id] : nullptr;
    if (neighbor == nullptr || score.node_id == node_id ||
        layer >= neighbor->layer_neighbors.size()) {
      continue;
    }
    node->layer_neighbors[layer].push_back(score.node_id);
    neighbor->layer_neighbors[layer].push_back(node_id);
    PruneNeighbors(provider, neighbor->node_id, layer);
  }
  PruneNeighbors(provider, node_id, layer);
}

void RefreshProviderCounts(VectorHnswPhysicalProvider* provider,
                           bool repair_entry = true) {
  provider->live_node_count = 0;
  provider->tombstone_count = 0;
  provider->empty_graph = true;
  provider->max_observed_level = 0;
  for (const auto& node : provider->nodes) {
    if (node.tombstoned) {
      ++provider->tombstone_count;
    } else {
      ++provider->live_node_count;
      provider->empty_graph = false;
      provider->max_observed_level = std::max(provider->max_observed_level,
                                              node.level);
    }
  }
  const u64 total = provider->live_node_count + provider->tombstone_count;
  provider->tombstone_ratio =
      total == 0 ? 0.0 : static_cast<double>(provider->tombstone_count) /
                            static_cast<double>(total);
  provider->compaction_rebuild_required =
      provider->tombstone_ratio >= provider->profile.compaction_tombstone_ratio;
  if (provider->empty_graph) {
    provider->entry_point_node_id = 0;
    provider->entry_point_present = false;
  } else {
    provider->entry_point_present = true;
    const auto* current = FindNodeById(*provider, provider->entry_point_node_id);
    if (repair_entry && (current == nullptr || current->tombstoned)) {
      for (const auto& node : provider->nodes) {
        if (!node.tombstoned &&
            (current == nullptr ||
             node.level > current->level ||
             (node.level == current->level &&
              LocatorLess(node.locator, current->locator)))) {
          current = &node;
        }
      }
      provider->entry_point_node_id = current == nullptr ? 0 : current->node_id;
    }
  }
}

void SetProviderEvidence(VectorHnswPhysicalProvider* provider) {
  provider->evidence = {
      kVectorHnswPhysicalProviderSearchKey,
      "vector_hnsw_physical_provider.artifact_identity_deterministic=true",
      "vector_hnsw_physical_provider.checksumed_serialization=true",
      "vector_hnsw_physical_provider.graph_layers_present=true",
      "vector_hnsw_physical_provider.entry_point_present=true",
      "vector_hnsw_physical_provider.neighbor_lists_present=true",
      "vector_hnsw_physical_provider.ef_construction_graph_build=true",
      "vector_hnsw_physical_provider.ef_search_traversal=true",
      "vector_hnsw_physical_provider.exact_rerank=true",
      "vector_hnsw_physical_provider.tombstones_present=true",
      "vector_hnsw_physical_provider.compaction_rebuild_evidence=true",
      "vector_hnsw_physical_provider.candidate_evidence_only=true",
      "vector_hnsw_physical_provider.index_finality_authority=false",
      "vector_hnsw_physical_provider.write_ahead_log_authority=false"};
}

bool GraphValid(const VectorHnswPhysicalProvider& provider) {
  if (provider.nodes.size() > kMaxRows) {
    return false;
  }
  std::vector<bool> seen_ids(provider.nodes.size(), false);
  std::set<std::tuple<u64, std::string, std::string>> locators;
  u64 live_count = 0;
  u64 tombstone_count = 0;
  u32 max_live_level = 0;
  for (std::size_t i = 0; i < provider.nodes.size(); ++i) {
    const auto& node = provider.nodes[i];
    if (node.node_id >= provider.nodes.size() || seen_ids[node.node_id] ||
        !NodePayloadValid(node, provider.descriptor) ||
        node.level > provider.profile.max_level ||
        node.layer_neighbors.size() != node.level + 1 ||
        node.insert_generation == 0 ||
        (!node.tombstoned && node.delete_generation != 0) ||
        (node.tombstoned && node.delete_generation == 0)) {
      return false;
    }
    seen_ids[node.node_id] = true;
    const auto locator_key =
        std::make_tuple(node.locator.row_ordinal, node.locator.row_uuid,
                        node.locator.version_uuid);
    if (!locators.insert(locator_key).second) {
      return false;
    }
    if (i > 0 && !LocatorLess(provider.nodes[i - 1].locator, node.locator)) {
      return false;
    }
    if (node.tombstoned) {
      ++tombstone_count;
    } else {
      ++live_count;
      max_live_level = std::max(max_live_level, node.level);
    }
    for (u32 layer = 0; layer < node.layer_neighbors.size(); ++layer) {
      const auto& neighbors = node.layer_neighbors[layer];
      if (neighbors.size() > provider.profile.m ||
          !std::is_sorted(neighbors.begin(), neighbors.end())) {
        return false;
      }
      for (std::size_t n = 0; n < neighbors.size(); ++n) {
        const u32 neighbor_id = neighbors[n];
        if (neighbor_id >= provider.nodes.size() ||
            neighbor_id == node.node_id ||
            (n > 0 && neighbors[n - 1] == neighbor_id)) {
          return false;
        }
        const auto* neighbor = FindNodeById(provider, neighbor_id);
        if (neighbor == nullptr ||
            neighbor->layer_neighbors.size() <= layer) {
          return false;
        }
      }
    }
  }
  const u64 total = live_count + tombstone_count;
  const bool empty = live_count == 0;
  const double expected_ratio =
      total == 0 ? 0.0 : static_cast<double>(tombstone_count) /
                            static_cast<double>(total);
  const bool expected_compaction =
      expected_ratio >= provider.profile.compaction_tombstone_ratio;
  if (provider.live_node_count != live_count ||
      provider.tombstone_count != tombstone_count ||
      provider.empty_graph != empty ||
      provider.max_observed_level != (empty ? 0 : max_live_level) ||
      std::fabs(provider.tombstone_ratio - expected_ratio) > 1e-12 ||
      provider.compaction_rebuild_required != expected_compaction) {
    return false;
  }
  if (empty) {
    if (provider.entry_point_present || provider.entry_point_node_id != 0) {
      return false;
    }
  } else {
    const auto* entry = FindNodeById(provider, provider.entry_point_node_id);
    if (entry == nullptr || entry->tombstoned ||
        entry->level != provider.max_observed_level) {
      return false;
    }
  }
  return true;
}

bool ProviderValid(const VectorHnswPhysicalProvider& provider) {
  if (provider.artifact_kind != kVectorHnswPhysicalProviderArtifactKind ||
      !SameFormatVersion(provider.format_version,
                         {kVectorHnswPhysicalProviderCurrentMajor,
                          kVectorHnswPhysicalProviderCurrentMinor}) ||
      !PageExtentSummaryUuidTextValid(provider.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.index_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.provider_uuid) ||
      provider.base_generation == 0 ||
      provider.provider_generation == 0 ||
      provider.training_generation == 0 ||
      !DescriptorSafe(provider.descriptor) ||
      !MetricSafe(provider.metric) ||
      !ProfileSafe(provider.profile) ||
      !provider.graph_storage_present ||
      !provider.layers_present ||
      !provider.neighbor_lists_present ||
      !provider.row_locators_present ||
      !provider.encoded_payloads_present ||
      !provider.tombstones_present ||
      !provider.generation_evidence_present ||
      !provider.deterministic_ordering_present ||
      !provider.ef_construction_graph_build_present ||
      !provider.ef_search_traversal_present ||
      !provider.exact_rerank_present ||
      !provider.metadata_prefilter_present ||
      !provider.candidate_set_input_present ||
      !provider.scalar_kernel_present ||
      !ProviderAuthorityClean(provider) ||
      !GraphValid(provider)) {
    return false;
  }
  const bool empty = provider.live_node_count == 0;
  if (empty != provider.empty_graph ||
      (!empty && !provider.entry_point_present) ||
      (empty && provider.entry_point_present)) {
    return false;
  }
  return true;
}

bool BuildRequestValid(const VectorHnswBuildRequest& request) {
  return PageExtentSummaryUuidTextValid(request.relation_uuid) &&
         PageExtentSummaryUuidTextValid(request.index_uuid) &&
         PageExtentSummaryUuidTextValid(request.provider_uuid) &&
         request.base_generation > 0 &&
         request.provider_generation > 0 &&
         request.training_generation > 0 &&
         DescriptorSafe(request.descriptor) &&
         MetricSafe(request.metric) &&
         ProfileSafe(request.profile) &&
         RecheckProofValid(request.recheck_proof) &&
         std::all_of(request.rows.begin(), request.rows.end(),
                     [&](const auto& row) {
                       return SourceRowValid(row, request.descriptor);
                     });
}

VectorHnswGraphNode NodeFromSource(const VectorHnswSourceRow& source,
                                   const VectorHnswBuildRequest& request,
                                   u32 node_id) {
  VectorHnswGraphNode node;
  node.node_id = node_id;
  node.locator = source.locator;
  node.encoded_payload =
      EncodeVectorExactPayload(source.vector, request.descriptor);
  node.level = DeterministicLevel(source.locator, request.provider_uuid,
                                  request.training_generation,
                                  request.profile.max_level);
  node.insert_generation = request.provider_generation;
  node.layer_neighbors.resize(node.level + 1);
  return node;
}

void InsertPreparedNode(VectorHnswPhysicalProvider* provider,
                        VectorHnswGraphNode node) {
  const std::vector<float> vector = DecodeNodeVector(node, provider->descriptor);
  if (provider->empty_graph) {
    provider->entry_point_node_id = node.node_id;
    provider->max_observed_level = node.level;
    provider->nodes.push_back(std::move(node));
    std::sort(provider->nodes.begin(), provider->nodes.end(),
              [](const auto& left, const auto& right) {
                return LocatorLess(left.locator, right.locator);
              });
    RefreshProviderCounts(provider);
    return;
  }

  u32 entry = provider->entry_point_node_id;
  u64 scalar_ops = 0;
  if (node.level < provider->max_observed_level) {
    for (u32 layer = provider->max_observed_level; layer > node.level; --layer) {
      entry = GreedySearchLayer(*provider, entry, vector, layer, &scalar_ops);
    }
  }

  const u32 inserted_id = node.node_id;
  provider->nodes.push_back(std::move(node));
  std::sort(provider->nodes.begin(), provider->nodes.end(),
            [](const auto& left, const auto& right) {
              return LocatorLess(left.locator, right.locator);
            });

  for (u32 layer = std::min(provider->max_observed_level,
                            FindNodeById(*provider, inserted_id)->level);
       layer != static_cast<u32>(-1); --layer) {
    u64 edges = 0;
    u64 visited = 0;
    auto candidates = SearchLayer(*provider, entry, vector, layer,
                                  provider->profile.ef_construction,
                                  &edges, &visited, &scalar_ops);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](const auto& score) {
                                      return score.node_id == inserted_id;
                                    }),
                     candidates.end());
    candidates = SortAndTrim(std::move(candidates), provider->profile.m,
                             LowerScoreBetter(provider->metric.metric_kind));
    ConnectLayer(provider, inserted_id, candidates, layer);
    if (layer == 0) {
      break;
    }
  }

  const auto* inserted = FindNodeById(*provider, inserted_id);
  if (inserted != nullptr &&
      inserted->level > provider->max_observed_level) {
    provider->entry_point_node_id = inserted->node_id;
  }
  RefreshProviderCounts(provider);
}

VectorHnswPhysicalProvider BuildGraphFromRows(
    const VectorHnswBuildRequest& request,
    const std::vector<VectorHnswSourceRow>& rows) {
  VectorHnswPhysicalProvider provider;
  provider.relation_uuid = request.relation_uuid;
  provider.index_uuid = request.index_uuid;
  provider.provider_uuid = request.provider_uuid;
  provider.base_generation = request.base_generation;
  provider.provider_generation = request.provider_generation;
  provider.training_generation = request.training_generation;
  provider.descriptor = request.descriptor;
  provider.metric = request.metric;
  provider.profile = request.profile;
  provider.mutation_generation_evidence = request.provider_generation;
  provider.graph_rebuild_generation_evidence = request.provider_generation;
  SetProviderEvidence(&provider);

  u32 node_id = 0;
  for (const auto& source : rows) {
    InsertPreparedNode(&provider, NodeFromSource(source, request, node_id++));
  }
  RefreshProviderCounts(&provider);
  SetProviderEvidence(&provider);
  return provider;
}

bool CandidateSetContains(const std::vector<TextInvertedRowLocator>& locators,
                          const TextInvertedRowLocator& locator) {
  return std::binary_search(locators.begin(), locators.end(), locator,
                            LocatorLess);
}

bool CandidateSetValid(const std::vector<TextInvertedRowLocator>& locators) {
  return std::all_of(locators.begin(), locators.end(), LocatorValid);
}

bool RuntimeRequestSafe(const VectorHnswQueryRequest& request) {
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

bool SourceMatchesNodePayload(const VectorHnswSourceRow& source,
                              const VectorHnswGraphNode& node,
                              const VectorHnswDescriptor& descriptor) {
  return LocatorEqual(source.locator, node.locator) &&
         EncodeVectorExactPayload(source.vector, descriptor) ==
             node.encoded_payload;
}

std::vector<VectorHnswSourceRow> LiveRowsFromProvider(
    const VectorHnswPhysicalProvider& provider) {
  std::vector<VectorHnswSourceRow> rows;
  for (const auto& node : provider.nodes) {
    if (node.tombstoned) {
      continue;
    }
    VectorHnswSourceRow row;
    row.locator = node.locator;
    row.vector = DecodeNodeVector(node, provider.descriptor);
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(), [](const auto& left,
                                         const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
  return rows;
}

VectorHnswBuildRequest RebuildRequestFromProvider(
    const VectorHnswPhysicalProvider& provider,
    u64 next_generation,
    const std::vector<VectorHnswSourceRow>& rows,
    const VectorHnswRecheckProof& proof) {
  VectorHnswBuildRequest request;
  request.relation_uuid = provider.relation_uuid;
  request.index_uuid = provider.index_uuid;
  request.provider_uuid = provider.provider_uuid;
  request.base_generation = provider.base_generation;
  request.provider_generation = next_generation;
  request.training_generation = provider.training_generation;
  request.descriptor = provider.descriptor;
  request.metric = provider.metric;
  request.profile = provider.profile;
  request.recheck_proof = proof;
  request.rows = rows;
  return request;
}

}  // namespace

VectorHnswBuildResult BuildVectorHnswPhysicalProvider(
    const VectorHnswBuildRequest& request) {
  if (!RecheckProofAuthorityClean(request.recheck_proof) ||
      !DescriptorAuthorityClean(request.descriptor) ||
      !MetricAuthorityClean(request.metric)) {
    return BuildFailure(
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED",
        "index.vector_hnsw_physical_provider.authority_claim_refused",
        "vector HNSW provider is candidate evidence only and cannot claim transaction, visibility, security, index, provider, parser, donor, or write-ahead authority");
  }
  if (!RecheckProofValid(request.recheck_proof)) {
    return BuildFailure(
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_hnsw_physical_provider.missing_exact_recheck",
        "vector HNSW build requires exact source vector, exact rerank, MGA, and security proof");
  }
  if (!SupportedProfile(request.descriptor.element_profile)) {
    return BuildFailure(
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.UNSUPPORTED_ELEMENT_PROFILE",
        "index.vector_hnsw_physical_provider.unsupported_element_profile");
  }
  if (!DescriptorSafe(request.descriptor)) {
    return BuildFailure(
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.STALE_DESCRIPTOR",
        "index.vector_hnsw_physical_provider.stale_descriptor");
  }
  if (!MetricSafe(request.metric)) {
    return BuildFailure(
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.UNSAFE_METRIC_RESOURCE",
        "index.vector_hnsw_physical_provider.unsafe_metric_resource");
  }
  if (!ProfileSafe(request.profile)) {
    return BuildFailure(
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.UNSUPPORTED_PROFILE",
        "index.vector_hnsw_physical_provider.unsupported_profile");
  }
  for (const auto& row : request.rows) {
    if (row.vector.size() != request.descriptor.dimensions ||
        !FiniteVector(row.vector)) {
      return BuildFailure(
          "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.DIMENSION_MISMATCH",
          "index.vector_hnsw_physical_provider.dimension_mismatch");
    }
  }
  if (!BuildRequestValid(request)) {
    return BuildFailure(
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.BUILD_REFUSED",
        "index.vector_hnsw_physical_provider.build_refused");
  }

  std::vector<VectorHnswSourceRow> rows = request.rows;
  std::sort(rows.begin(), rows.end(), [](const auto& left,
                                         const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
  for (std::size_t i = 1; i < rows.size(); ++i) {
    if (LocatorEqual(rows[i - 1].locator, rows[i].locator)) {
      return BuildFailure(
          "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.DUPLICATE_ROW_LOCATOR",
          "index.vector_hnsw_physical_provider.duplicate_row_locator");
    }
  }

  auto provider = BuildGraphFromRows(request, rows);
  if (!ProviderValid(provider)) {
    return BuildFailure("INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.INVALID_GRAPH",
                        "index.vector_hnsw_physical_provider.invalid_graph");
  }
  VectorHnswBuildResult result;
  result.status = OkStatus();
  result.provider = std::move(provider);
  result.built = true;
  result.fail_closed = false;
  return result;
}

VectorHnswSerializeResult SerializeVectorHnswPhysicalProvider(
    const VectorHnswPhysicalProvider& provider) {
  VectorHnswSerializeResult result;
  if (!ProviderValid(provider)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeVectorHnswPhysicalProviderDiagnostic(
        result.status,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.SERIALIZE_REFUSED",
        "index.vector_hnsw_physical_provider.serialize_refused");
    return result;
  }
  std::vector<byte> bytes;
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  AppendU32(&bytes, kVectorHnswPhysicalProviderCurrentMajor);
  AppendU32(&bytes, kVectorHnswPhysicalProviderCurrentMinor);
  AppendU64(&bytes, 0);
  AppendString(&bytes, provider.relation_uuid);
  AppendString(&bytes, provider.index_uuid);
  AppendString(&bytes, provider.provider_uuid);
  AppendU64(&bytes, provider.base_generation);
  AppendU64(&bytes, provider.provider_generation);
  AppendU64(&bytes, provider.training_generation);
  AppendU32(&bytes, provider.descriptor.dimensions);
  AppendU32(&bytes, static_cast<u32>(provider.descriptor.element_profile));
  AppendU64(&bytes, provider.descriptor.descriptor_epoch);
  AppendF64(&bytes, provider.descriptor.int8_scale);
  AppendI32(&bytes, provider.descriptor.int8_zero_point);
  AppendString(&bytes, provider.metric.metric_resource_uuid);
  AppendU64(&bytes, provider.metric.metric_resource_epoch);
  AppendU32(&bytes, static_cast<u32>(provider.metric.metric_kind));
  AppendU32(&bytes, provider.profile.m);
  AppendU32(&bytes, provider.profile.ef_construction);
  AppendU32(&bytes, provider.profile.ef_search);
  AppendU32(&bytes, provider.profile.max_level);
  AppendF64(&bytes, provider.profile.compaction_tombstone_ratio);
  AppendBool(&bytes, provider.profile.deterministic_level_assignment);
  AppendBool(&bytes, provider.profile.scalar_kernel_present);
  AppendBool(&bytes, provider.profile.simd_kernel_present);
  AppendU32(&bytes, provider.entry_point_node_id);
  AppendU32(&bytes, provider.max_observed_level);
  AppendU64(&bytes, provider.mutation_generation_evidence);
  AppendU64(&bytes, provider.graph_rebuild_generation_evidence);
  AppendU64(&bytes, static_cast<u64>(provider.nodes.size()));
  for (const auto& node : provider.nodes) {
    AppendU32(&bytes, node.node_id);
    AppendLocator(&bytes, node.locator);
    AppendBytes(&bytes, node.encoded_payload);
    AppendU32(&bytes, node.level);
    AppendBool(&bytes, node.tombstoned);
    AppendU64(&bytes, node.insert_generation);
    AppendU64(&bytes, node.delete_generation);
    AppendU32(&bytes, static_cast<u32>(node.layer_neighbors.size()));
    for (const auto& layer : node.layer_neighbors) {
      AppendU32(&bytes, static_cast<u32>(layer.size()));
      for (u32 neighbor : layer) {
        AppendU32(&bytes, neighbor);
      }
    }
  }
  result.checksum = ComputeChecksum(bytes);
  StoreLittle64(bytes.data() + 16, result.checksum);
  result.status = OkStatus();
  result.bytes = std::move(bytes);
  return result;
}

VectorHnswOpenResult OpenVectorHnswPhysicalProvider(
    const VectorHnswOpenRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return OpenFailure(
        VectorHnswOpenClass::missing_exact_recheck_proof,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_hnsw_physical_provider.missing_exact_recheck");
  }
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(VectorHnswOpenClass::corrupt_payload,
                       "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_hnsw_physical_provider.corrupt_payload");
  }
  const u64 stored_checksum = LoadLittle64(request.bytes.data() + 16);
  if (stored_checksum == 0 ||
      stored_checksum != ComputeChecksum(request.bytes)) {
    return OpenFailure(VectorHnswOpenClass::bad_checksum,
                       "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.BAD_CHECKSUM",
                       "index.vector_hnsw_physical_provider.bad_checksum");
  }

  Reader reader(request.bytes);
  reader.SetOffset(8);
  VectorHnswPhysicalProvider provider;
  u32 major = 0;
  u32 minor = 0;
  u64 checksum = 0;
  u32 profile = 0;
  u32 metric_kind = 0;
  u64 node_count = 0;
  if (!reader.ReadU32(&major) ||
      !reader.ReadU32(&minor) ||
      !reader.ReadU64(&checksum)) {
    return OpenFailure(VectorHnswOpenClass::corrupt_payload,
                       "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_hnsw_physical_provider.corrupt_payload");
  }
  provider.format_version = {major, minor};
  if (!SameFormatVersion(provider.format_version,
                         {kVectorHnswPhysicalProviderCurrentMajor,
                          kVectorHnswPhysicalProviderCurrentMinor})) {
    return OpenFailure(VectorHnswOpenClass::stale_format,
                       "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.STALE_FORMAT",
                       "index.vector_hnsw_physical_provider.stale_format");
  }
  if (!reader.ReadString(&provider.relation_uuid) ||
      !reader.ReadString(&provider.index_uuid) ||
      !reader.ReadString(&provider.provider_uuid) ||
      !reader.ReadU64(&provider.base_generation) ||
      !reader.ReadU64(&provider.provider_generation) ||
      !reader.ReadU64(&provider.training_generation) ||
      !reader.ReadU32(&provider.descriptor.dimensions) ||
      !reader.ReadU32(&profile) ||
      !reader.ReadU64(&provider.descriptor.descriptor_epoch) ||
      !reader.ReadF64(&provider.descriptor.int8_scale) ||
      !reader.ReadI32(&provider.descriptor.int8_zero_point) ||
      !reader.ReadString(&provider.metric.metric_resource_uuid) ||
      !reader.ReadU64(&provider.metric.metric_resource_epoch) ||
      !reader.ReadU32(&metric_kind) ||
      !reader.ReadU32(&provider.profile.m) ||
      !reader.ReadU32(&provider.profile.ef_construction) ||
      !reader.ReadU32(&provider.profile.ef_search) ||
      !reader.ReadU32(&provider.profile.max_level) ||
      !reader.ReadF64(&provider.profile.compaction_tombstone_ratio) ||
      !reader.ReadBool(&provider.profile.deterministic_level_assignment) ||
      !reader.ReadBool(&provider.profile.scalar_kernel_present) ||
      !reader.ReadBool(&provider.profile.simd_kernel_present) ||
      !reader.ReadU32(&provider.entry_point_node_id) ||
      !reader.ReadU32(&provider.max_observed_level) ||
      !reader.ReadU64(&provider.mutation_generation_evidence) ||
      !reader.ReadU64(&provider.graph_rebuild_generation_evidence) ||
      !reader.ReadU64(&node_count) ||
      node_count > kMaxRows) {
    return OpenFailure(VectorHnswOpenClass::corrupt_payload,
                       "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_hnsw_physical_provider.corrupt_payload");
  }
  provider.descriptor.element_profile =
      static_cast<VectorExactElementProfile>(profile);
  provider.metric.metric_kind = static_cast<VectorExactMetricKind>(metric_kind);
  provider.descriptor.deterministic = true;
  provider.descriptor.descriptor_safe = true;
  provider.metric.deterministic = true;
  provider.metric.safe = true;
  for (u64 i = 0; i < node_count; ++i) {
    VectorHnswGraphNode node;
    u32 layer_count = 0;
    if (!reader.ReadU32(&node.node_id) ||
        !reader.ReadLocator(&node.locator) ||
        !reader.ReadBytes(&node.encoded_payload) ||
        !reader.ReadU32(&node.level) ||
        !reader.ReadBool(&node.tombstoned) ||
        !reader.ReadU64(&node.insert_generation) ||
        !reader.ReadU64(&node.delete_generation) ||
        !reader.ReadU32(&layer_count) ||
        layer_count > kMaxLevel + 1) {
      return OpenFailure(
          VectorHnswOpenClass::corrupt_payload,
          "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
          "index.vector_hnsw_physical_provider.corrupt_payload");
    }
    node.layer_neighbors.resize(layer_count);
    for (u32 layer = 0; layer < layer_count; ++layer) {
      u32 neighbor_count = 0;
      if (!reader.ReadU32(&neighbor_count) || neighbor_count > kMaxM) {
        return OpenFailure(
            VectorHnswOpenClass::invalid_graph,
            "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.INVALID_GRAPH",
            "index.vector_hnsw_physical_provider.invalid_graph");
      }
      node.layer_neighbors[layer].resize(neighbor_count);
      for (u32 n = 0; n < neighbor_count; ++n) {
        if (!reader.ReadU32(&node.layer_neighbors[layer][n])) {
          return OpenFailure(
              VectorHnswOpenClass::corrupt_payload,
              "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
              "index.vector_hnsw_physical_provider.corrupt_payload");
        }
      }
    }
    provider.nodes.push_back(std::move(node));
  }
  if (!reader.Done()) {
    return OpenFailure(VectorHnswOpenClass::corrupt_payload,
                       "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_hnsw_physical_provider.corrupt_payload");
  }
  SetProviderEvidence(&provider);
  RefreshProviderCounts(&provider, false);
  if (!SupportedProfile(provider.descriptor.element_profile)) {
    return OpenFailure(
        VectorHnswOpenClass::unsupported_element_profile,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.UNSUPPORTED_ELEMENT_PROFILE",
        "index.vector_hnsw_physical_provider.unsupported_element_profile");
  }
  if (!MetricSafe(provider.metric)) {
    return OpenFailure(
        VectorHnswOpenClass::unsafe_metric_resource,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.UNSAFE_METRIC_RESOURCE",
        "index.vector_hnsw_physical_provider.unsafe_metric_resource");
  }
  if ((request.expected_relation_uuid_present &&
       request.expected_relation_uuid != provider.relation_uuid) ||
      (request.expected_index_uuid_present &&
       request.expected_index_uuid != provider.index_uuid) ||
      (request.expected_provider_uuid_present &&
       request.expected_provider_uuid != provider.provider_uuid)) {
    return OpenFailure(VectorHnswOpenClass::identity_mismatch,
                       "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.IDENTITY_MISMATCH",
                       "index.vector_hnsw_physical_provider.identity_mismatch");
  }
  if ((request.expected_base_generation_present &&
       request.expected_base_generation != provider.base_generation) ||
      (request.expected_provider_generation_present &&
       request.expected_provider_generation != provider.provider_generation) ||
      (request.expected_training_generation_present &&
       request.expected_training_generation != provider.training_generation)) {
    return OpenFailure(VectorHnswOpenClass::stale_generation,
                       "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.STALE_GENERATION",
                       "index.vector_hnsw_physical_provider.stale_generation");
  }
  if (request.expected_descriptor_epoch_present &&
      request.expected_descriptor_epoch != provider.descriptor.descriptor_epoch) {
    return OpenFailure(
        VectorHnswOpenClass::stale_descriptor_epoch,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.vector_hnsw_physical_provider.stale_descriptor_epoch");
  }
  if (request.expected_metric_resource_epoch_present &&
      request.expected_metric_resource_epoch !=
          provider.metric.metric_resource_epoch) {
    return OpenFailure(
        VectorHnswOpenClass::stale_metric_epoch,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.STALE_METRIC_EPOCH",
        "index.vector_hnsw_physical_provider.stale_metric_epoch");
  }
  if (request.expected_dimensions_present &&
      request.expected_dimensions != provider.descriptor.dimensions) {
    return OpenFailure(
        VectorHnswOpenClass::dimension_mismatch,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.DIMENSION_MISMATCH",
        "index.vector_hnsw_physical_provider.dimension_mismatch");
  }
  if (!GraphValid(provider)) {
    return OpenFailure(VectorHnswOpenClass::invalid_graph,
                       "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.INVALID_GRAPH",
                       "index.vector_hnsw_physical_provider.invalid_graph");
  }
  if (!ProviderValid(provider)) {
    return OpenFailure(VectorHnswOpenClass::corrupt_payload,
                       "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.vector_hnsw_physical_provider.corrupt_payload");
  }

  VectorHnswOpenResult result;
  result.status = OkStatus();
  result.open_class = VectorHnswOpenClass::current;
  result.provider = std::move(provider);
  result.fail_closed = false;
  result.actions.push_back("open_vector_hnsw_physical_provider");
  return result;
}

VectorHnswQueryResult QueryVectorHnswPhysicalProvider(
    const VectorHnswQueryRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return QueryFailure(
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_hnsw_physical_provider.missing_exact_recheck");
  }
  if (!request.descriptor_epoch_current) {
    return QueryFailure(
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.vector_hnsw_physical_provider.stale_descriptor_epoch");
  }
  if (!request.metric_resource_epoch_current) {
    return QueryFailure(
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.STALE_METRIC_EPOCH",
        "index.vector_hnsw_physical_provider.stale_metric_epoch");
  }
  if (!RuntimeRequestSafe(request)) {
    return QueryFailure(
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
        "index.vector_hnsw_physical_provider.runtime_refused",
        "runtime query requires current descriptor and metric epochs, no scan/fallback mode, and exact source/MGA/security proof");
  }

  VectorHnswQueryResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.ef_search_traversal = true;
  result.descriptor_store_scan = request.descriptor_store_scan;
  result.behavior_store_scan = request.behavior_store_scan;
  result.contract_only_fallback = request.contract_only_fallback;
  result.provider_only_fallback = request.provider_only_fallback;
  result.provider_after_telemetry = request.provider;
  result.evidence = {
      kVectorHnswPhysicalProviderSearchKey,
      "vector_hnsw_physical_provider.ef_search_traversal=true",
      "vector_hnsw_physical_provider.approximate_candidates=true",
      "vector_hnsw_physical_provider.exact_rerank_performed=true",
      "vector_hnsw_physical_provider.scalar_kernel_consumed=true",
      "vector_hnsw_physical_provider.candidate_rows_only=true",
      "vector_hnsw_physical_provider.final_rows_authorized=false"};

  for (const auto& query : request.queries) {
    if (query.vector.size() != request.provider.descriptor.dimensions ||
        query.top_k == 0 ||
        !FiniteVector(query.vector)) {
      return QueryFailure(
          "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.DIMENSION_MISMATCH",
          "index.vector_hnsw_physical_provider.dimension_mismatch");
    }
    std::vector<TextInvertedRowLocator> candidate_set = query.candidate_set;
    if (!CandidateSetValid(candidate_set)) {
      return QueryFailure(
          "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.CANDIDATE_SET_REFUSED",
          "index.vector_hnsw_physical_provider.candidate_set_refused");
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

    VectorHnswSingleQueryResult single;
    single.ok = true;
    single.ef_search_used = true;
    single.exact_rerank_performed = true;
    if (request.provider.empty_graph || request.provider.live_node_count == 0) {
      single.approximate_candidate_count = 0;
      result.provider_after_telemetry.last_query_visited_nodes = 0;
      result.provider_after_telemetry.last_query_candidate_count = 0;
      result.provider_after_telemetry.last_query_exact_rerank_count = 0;
      result.provider_after_telemetry.last_query_latency_units = 0;
      result.provider_after_telemetry.last_query_recall_floor = 1.0;
      result.batch_results.push_back(std::move(single));
      continue;
    }
    const bool lower_score_better =
        LowerScoreBetter(request.provider.metric.metric_kind);
    const u32 ef = std::max({query.ef_search, request.provider.profile.ef_search,
                             query.top_k});
    u64 scalar_ops = 0;
    u32 entry = request.provider.entry_point_node_id;
    for (u32 layer = request.provider.max_observed_level; layer > 0; --layer) {
      entry = GreedySearchLayer(request.provider, entry, query.vector, layer,
                                &scalar_ops);
    }
    auto approximate = SearchLayer(request.provider, entry, query.vector, 0, ef,
                                   &single.graph_edges_considered,
                                   &single.graph_nodes_visited, &scalar_ops);
    for (const auto& locator : candidate_set) {
      const auto* node = FindNodeByLocator(request.provider, locator);
      if (node != nullptr && !node->tombstoned) {
        approximate.push_back(ScoreNode(request.provider, *node, query.vector,
                                        &scalar_ops));
      }
    }
    approximate = SortAndTrim(std::move(approximate),
                              std::max<u32>(ef, query.top_k),
                              lower_score_better);
    single.approximate_candidate_count = approximate.size();
    std::vector<VectorHnswCandidate> reranked;
    for (const auto& score : approximate) {
      const auto* node = FindNodeById(request.provider, score.node_id);
      if (node == nullptr || node->tombstoned) {
        continue;
      }
      if (!candidate_set.empty() &&
          !CandidateSetContains(candidate_set, node->locator)) {
        continue;
      }
      if (query.metadata_prefilter &&
          !query.metadata_prefilter(node->locator)) {
        continue;
      }
      const auto decoded = DecodeNodeVector(*node, request.provider.descriptor);
      if (decoded.size() != request.provider.descriptor.dimensions) {
        return QueryFailure(
            "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
            "index.vector_hnsw_physical_provider.corrupt_payload");
      }
      u64 exact_ops = 0;
      VectorHnswCandidate candidate;
      candidate.locator = node->locator;
      candidate.node_id = node->node_id;
      candidate.approximate_score = score.score;
      candidate.exact_score =
          ScoreVectorPair(query.vector, decoded,
                          request.provider.metric.metric_kind, &exact_ops);
      candidate.lower_score_better = lower_score_better;
      candidate.reached_by_ef_search = true;
      candidate.decoded_from_physical_payload = true;
      candidate.metadata_prefilter_passed = true;
      candidate.candidate_set_member = true;
      candidate.exact_rerank_proof_verified = true;
      candidate.source_recheck_evidence_ref = request.recheck_proof.evidence_ref;
      single.scalar_kernel_consumed_count += exact_ops;
      ++single.exact_rerank_count;
      reranked.push_back(std::move(candidate));
    }
    std::sort(reranked.begin(), reranked.end(), CandidateLess);
    if (reranked.size() > query.top_k) {
      reranked.resize(query.top_k);
    }
    single.candidates = std::move(reranked);
    result.scalar_kernel_consumed =
        result.scalar_kernel_consumed ||
        single.scalar_kernel_consumed_count > 0 ||
        scalar_ops > 0;
    result.exact_rerank_performed =
        result.exact_rerank_performed || single.exact_rerank_count > 0;
    result.provider_after_telemetry.last_query_visited_nodes =
        single.graph_nodes_visited;
    result.provider_after_telemetry.last_query_candidate_count =
        single.approximate_candidate_count;
    result.provider_after_telemetry.last_query_exact_rerank_count =
        single.exact_rerank_count;
    result.provider_after_telemetry.last_query_latency_units =
        single.graph_edges_considered + single.scalar_kernel_consumed_count;
    result.provider_after_telemetry.last_query_recall_floor =
        single.candidates.empty() ? 0.0 : 1.0;
    result.batch_results.push_back(std::move(single));
  }
  return result;
}

VectorHnswMutationResult ApplyVectorHnswPhysicalMutation(
    const VectorHnswPhysicalProvider& provider,
    const VectorHnswMutation& mutation) {
  if (!ProviderValid(provider)) {
    return MutationFailure(
        provider,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.MUTATION_REFUSED",
        "index.vector_hnsw_physical_provider.mutation_refused");
  }
  if (!RecheckProofValid(mutation.recheck_proof)) {
    return MutationFailure(
        provider,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_hnsw_physical_provider.missing_exact_recheck");
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
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.STALE_GENERATION",
        "index.vector_hnsw_physical_provider.stale_generation");
  }

  VectorHnswPhysicalProvider next = provider;
  const u64 next_generation = provider.provider_generation + 1;
  switch (mutation.kind) {
    case VectorHnswMutationKind::insert_row: {
      if (!mutation.after_row_present ||
          !SourceRowValid(mutation.after_row, provider.descriptor) ||
          FindNodeByLocator(next, mutation.after_row.locator) != nullptr) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.DUPLICATE_ROW_LOCATOR",
            "index.vector_hnsw_physical_provider.duplicate_row_locator");
      }
      VectorHnswGraphNode node;
      node.node_id = static_cast<u32>(next.nodes.size());
      node.locator = mutation.after_row.locator;
      node.encoded_payload =
          EncodeVectorExactPayload(mutation.after_row.vector, provider.descriptor);
      node.level = DeterministicLevel(node.locator, provider.provider_uuid,
                                      provider.training_generation,
                                      provider.profile.max_level);
      node.insert_generation = next_generation;
      node.layer_neighbors.resize(node.level + 1);
      next.provider_generation = next_generation;
      InsertPreparedNode(&next, std::move(node));
      next.graph_rebuild_generation_evidence = provider.graph_rebuild_generation_evidence;
      break;
    }
    case VectorHnswMutationKind::delete_row: {
      if (!mutation.before_row_present ||
          !SourceRowValid(mutation.before_row, provider.descriptor)) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_hnsw_physical_provider.mutation_refused");
      }
      auto* node = FindNodeByLocator(&next, mutation.before_row.locator);
      if (node == nullptr || node->tombstoned ||
          !SourceMatchesNodePayload(mutation.before_row, *node,
                                    provider.descriptor)) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_hnsw_physical_provider.mutation_refused");
      }
      node->tombstoned = true;
      node->delete_generation = next_generation;
      next.provider_generation = next_generation;
      RefreshProviderCounts(&next);
      break;
    }
    case VectorHnswMutationKind::update_row: {
      if (!mutation.before_row_present ||
          !mutation.after_row_present ||
          !SourceRowValid(mutation.before_row, provider.descriptor) ||
          !SourceRowValid(mutation.after_row, provider.descriptor) ||
          !LocatorEqual(mutation.before_row.locator, mutation.after_row.locator)) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_hnsw_physical_provider.mutation_refused");
      }
      const auto* existing = FindNodeByLocator(provider,
                                               mutation.before_row.locator);
      if (existing == nullptr || existing->tombstoned ||
          !SourceMatchesNodePayload(mutation.before_row, *existing,
                                    provider.descriptor)) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_hnsw_physical_provider.mutation_refused");
      }
      auto rows = LiveRowsFromProvider(provider);
      bool replaced = false;
      for (auto& row : rows) {
        if (LocatorEqual(row.locator, mutation.before_row.locator)) {
          row.vector = mutation.after_row.vector;
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        return MutationFailure(
            provider,
            "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.MUTATION_REFUSED",
            "index.vector_hnsw_physical_provider.mutation_refused");
      }
      auto rebuild = BuildGraphFromRows(
          RebuildRequestFromProvider(provider, next_generation, rows,
                                     mutation.recheck_proof),
          rows);
      rebuild.graph_rebuild_generation_evidence = next_generation;
      next = std::move(rebuild);
      break;
    }
  }
  next.provider_generation = next_generation;
  next.mutation_generation_evidence = next_generation;
  SetProviderEvidence(&next);
  RefreshProviderCounts(&next);
  if (!ProviderValid(next)) {
    return MutationFailure(
        provider,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.INVALID_GRAPH",
        "index.vector_hnsw_physical_provider.invalid_graph");
  }
  VectorHnswMutationResult result;
  result.status = OkStatus();
  result.provider = std::move(next);
  result.applied = true;
  result.fail_closed = false;
  result.graph_repaired =
      mutation.kind == VectorHnswMutationKind::insert_row ||
      mutation.kind == VectorHnswMutationKind::update_row;
  result.tombstone_recorded =
      mutation.kind == VectorHnswMutationKind::delete_row;
  result.compaction_rebuild_required =
      result.provider.compaction_rebuild_required;
  result.actions.push_back("apply_vector_hnsw_maintenance_mutation");
  result.actions.push_back("provider_generation_incremented");
  if (result.graph_repaired) {
    result.actions.push_back("graph_neighbors_repaired");
  }
  if (result.tombstone_recorded) {
    result.actions.push_back("delete_tombstone_recorded");
  }
  return result;
}

VectorHnswCompactionResult CompactVectorHnswPhysicalProvider(
    const VectorHnswPhysicalProvider& provider,
    const VectorHnswRecheckProof& proof) {
  if (!ProviderValid(provider)) {
    return CompactionFailure(
        provider,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.COMPACTION_REFUSED",
        "index.vector_hnsw_physical_provider.compaction_refused");
  }
  if (!RecheckProofValid(proof)) {
    return CompactionFailure(
        provider,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.vector_hnsw_physical_provider.missing_exact_recheck");
  }
  const u64 removed = provider.tombstone_count;
  const auto rows = LiveRowsFromProvider(provider);
  const u64 next_generation = provider.provider_generation + 1;
  auto rebuilt = BuildGraphFromRows(
      RebuildRequestFromProvider(provider, next_generation, rows, proof), rows);
  rebuilt.graph_rebuild_generation_evidence = next_generation;
  rebuilt.mutation_generation_evidence = next_generation;
  RefreshProviderCounts(&rebuilt);
  SetProviderEvidence(&rebuilt);
  if (!ProviderValid(rebuilt)) {
    return CompactionFailure(
        provider,
        "INDEX.VECTOR_HNSW_PHYSICAL_PROVIDER.INVALID_GRAPH",
        "index.vector_hnsw_physical_provider.invalid_graph");
  }
  VectorHnswCompactionResult result;
  result.status = OkStatus();
  result.provider = std::move(rebuilt);
  result.compacted = true;
  result.fail_closed = false;
  result.removed_tombstones = removed;
  result.actions.push_back("compact_vector_hnsw_graph_rebuild");
  result.actions.push_back("tombstones_removed");
  return result;
}

const char* VectorHnswOpenClassName(VectorHnswOpenClass open_class) {
  switch (open_class) {
    case VectorHnswOpenClass::current:
      return "current";
    case VectorHnswOpenClass::stale_format:
      return "stale_format";
    case VectorHnswOpenClass::stale_generation:
      return "stale_generation";
    case VectorHnswOpenClass::bad_checksum:
      return "bad_checksum";
    case VectorHnswOpenClass::corrupt_payload:
      return "corrupt_payload";
    case VectorHnswOpenClass::identity_mismatch:
      return "identity_mismatch";
    case VectorHnswOpenClass::stale_descriptor_epoch:
      return "stale_descriptor_epoch";
    case VectorHnswOpenClass::stale_metric_epoch:
      return "stale_metric_epoch";
    case VectorHnswOpenClass::dimension_mismatch:
      return "dimension_mismatch";
    case VectorHnswOpenClass::unsupported_element_profile:
      return "unsupported_element_profile";
    case VectorHnswOpenClass::unsafe_metric_resource:
      return "unsafe_metric_resource";
    case VectorHnswOpenClass::missing_exact_recheck_proof:
      return "missing_exact_recheck_proof";
    case VectorHnswOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case VectorHnswOpenClass::invalid_graph:
      return "invalid_graph";
    case VectorHnswOpenClass::duplicate_row_locator:
      return "duplicate_row_locator";
    case VectorHnswOpenClass::refused:
      return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeVectorHnswPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(diagnostic_code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.source_component = kVectorHnswPhysicalProviderSearchKey;
  diagnostic.remediation_hint =
      "rebuild vector HNSW provider from authoritative source vectors with exact source, MGA, security, and metric descriptor proof";
  if (!detail.empty()) {
    diagnostic.arguments.push_back({"detail", std::move(detail)});
  }
  return diagnostic;
}

}  // namespace scratchbird::core::index
