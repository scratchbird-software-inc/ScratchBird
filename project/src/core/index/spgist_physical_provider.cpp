// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "spgist_physical_provider.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <tuple>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'S', 'P',
                                               'G', 'S', '0', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u64 kMaxRows = 1000000;
inline constexpr u64 kMaxNodes = 1000000;
inline constexpr u64 kFnvOffset = 14695981039346656037ull;
inline constexpr u64 kFnvPrime = 1099511628211ull;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
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

bool RecheckAuthorityClean(const SpGistExactRecheckProof& proof) {
  return !proof.parser_finality_authority_claimed &&
         !proof.donor_finality_authority_claimed &&
         !proof.provider_finality_authority_claimed &&
         !proof.index_finality_authority_claimed &&
         !proof.write_ahead_log_finality_authority_claimed &&
         !proof.visibility_authority_claimed &&
         !proof.security_authority_claimed &&
         !proof.transaction_finality_authority_claimed;
}

bool RecheckProofValid(const SpGistExactRecheckProof& proof) {
  return proof.proof_supplied &&
         proof.exact_source_geometry_available &&
         proof.exact_predicate_recheck_required &&
         proof.mga_recheck_required &&
         proof.security_recheck_required &&
         !proof.evidence_ref.empty() &&
         RecheckAuthorityClean(proof);
}

bool DescriptorAuthorityClean(const SpatialRTreeDescriptor& descriptor) {
  return !descriptor.parser_finality_authority_claimed &&
         !descriptor.donor_finality_authority_claimed &&
         !descriptor.provider_finality_authority_claimed &&
         !descriptor.index_finality_authority_claimed &&
         !descriptor.write_ahead_log_finality_authority_claimed;
}

bool SridAuthorityClean(const SpatialRTreeSridResource& resource) {
  return !resource.parser_finality_authority_claimed &&
         !resource.donor_finality_authority_claimed &&
         !resource.provider_finality_authority_claimed &&
         !resource.index_finality_authority_claimed &&
         !resource.write_ahead_log_finality_authority_claimed;
}

bool OpclassAuthorityClean(const SpGistOpclassDescriptor& opclass) {
  return !opclass.parser_finality_authority_claimed &&
         !opclass.donor_finality_authority_claimed &&
         !opclass.provider_finality_authority_claimed &&
         !opclass.index_finality_authority_claimed &&
         !opclass.write_ahead_log_finality_authority_claimed;
}

bool DescriptorSafe(const SpatialRTreeDescriptor& descriptor) {
  return descriptor.dimensions == 2 &&
         descriptor.descriptor_epoch > 0 &&
         descriptor.deterministic &&
         descriptor.descriptor_safe &&
         descriptor.supports_mbr &&
         !descriptor.supports_z &&
         !descriptor.supports_m &&
         !descriptor.descriptor_store_scan &&
         !descriptor.behavior_store_scan &&
         !descriptor.contract_only_fallback &&
         !descriptor.provider_only_fallback &&
         DescriptorAuthorityClean(descriptor);
}

bool SridResourceSafe(const SpatialRTreeSridResource& resource,
                      const SpatialRTreeDescriptor& descriptor) {
  return PageExtentSummaryUuidTextValid(resource.resource_uuid) &&
         resource.resource_epoch > 0 &&
         resource.srid > 0 &&
         resource.deterministic &&
         resource.safe &&
         resource.cache_present &&
         resource.coordinate_order == "xy" &&
         !resource.descriptor_store_scan &&
         !resource.behavior_store_scan &&
         !resource.contract_only_fallback &&
         !resource.provider_only_fallback &&
         SridAuthorityClean(resource) &&
         descriptor.dimensions == 2;
}

bool OpclassDescriptorSafe(const SpGistOpclassDescriptor& opclass) {
  return opclass.opclass_name == kSpGistSpatialQuadMbrOpclassName &&
         opclass.opclass_epoch > 0 &&
         opclass.resource_epoch > 0 &&
         opclass.registered &&
         opclass.deterministic &&
         opclass.immutable &&
         opclass.safe &&
         opclass.dimensions == 2 &&
         opclass.srid > 0 &&
         !opclass.descriptor_store_scan &&
         !opclass.behavior_store_scan &&
         !opclass.contract_only_fallback &&
         !opclass.provider_only_fallback &&
         OpclassAuthorityClean(opclass);
}

bool MethodSetComplete(const SpGistOpclassRuntime& opclass) {
  return static_cast<bool>(opclass.methods.compress) &&
         static_cast<bool>(opclass.methods.decompress) &&
         static_cast<bool>(opclass.methods.choose) &&
         static_cast<bool>(opclass.methods.inner_consistent) &&
         static_cast<bool>(opclass.methods.leaf_consistent) &&
         static_cast<bool>(opclass.methods.distance);
}

bool OpclassRuntimeSafe(const SpGistOpclassRuntime& opclass) {
  return OpclassDescriptorSafe(opclass.descriptor) && MethodSetComplete(opclass);
}

bool SameOpclassDescriptor(const SpGistOpclassDescriptor& left,
                           const SpGistOpclassDescriptor& right) {
  return left.opclass_name == right.opclass_name &&
         left.opclass_epoch == right.opclass_epoch &&
         left.resource_epoch == right.resource_epoch &&
         left.registered == right.registered &&
         left.deterministic == right.deterministic &&
         left.immutable == right.immutable &&
         left.safe == right.safe &&
         left.dimensions == right.dimensions &&
         left.srid == right.srid;
}

bool MbrSame(const SpatialRTreeMbr& left, const SpatialRTreeMbr& right) {
  return left.dimensions == right.dimensions &&
         left.srid == right.srid &&
         left.min == right.min &&
         left.max == right.max;
}

double AxisWidth(const SpatialRTreeMbr& mbr, std::size_t axis) {
  return mbr.max[axis] - mbr.min[axis];
}

double Area(const SpatialRTreeMbr& mbr) {
  if (mbr.dimensions == 0 || mbr.min.size() != mbr.max.size()) {
    return 0.0;
  }
  double area = 1.0;
  for (std::size_t i = 0; i < mbr.min.size(); ++i) {
    area *= std::max(0.0, AxisWidth(mbr, i));
  }
  return area;
}

SpatialRTreeMbr UnionMbr(const SpatialRTreeMbr& left,
                         const SpatialRTreeMbr& right) {
  SpatialRTreeMbr out = left;
  for (std::size_t i = 0; i < out.min.size(); ++i) {
    out.min[i] = std::min(out.min[i], right.min[i]);
    out.max[i] = std::max(out.max[i], right.max[i]);
  }
  return out;
}

bool Intersects(const SpatialRTreeMbr& left, const SpatialRTreeMbr& right) {
  for (std::size_t i = 0; i < left.min.size(); ++i) {
    if (left.max[i] < right.min[i] || right.max[i] < left.min[i]) {
      return false;
    }
  }
  return true;
}

bool Contains(const SpatialRTreeMbr& outer, const SpatialRTreeMbr& inner) {
  for (std::size_t i = 0; i < outer.min.size(); ++i) {
    if (outer.min[i] > inner.min[i] || outer.max[i] < inner.max[i]) {
      return false;
    }
  }
  return true;
}

double CenterAxis(const SpatialRTreeMbr& mbr, std::size_t axis) {
  return (mbr.min[axis] + mbr.max[axis]) * 0.5;
}

double MinDistanceToPoint(const SpatialRTreeMbr& mbr,
                          const std::vector<double>& point) {
  double sum = 0.0;
  for (std::size_t i = 0; i < point.size(); ++i) {
    double delta = 0.0;
    if (point[i] < mbr.min[i]) {
      delta = mbr.min[i] - point[i];
    } else if (point[i] > mbr.max[i]) {
      delta = point[i] - mbr.max[i];
    }
    sum += delta * delta;
  }
  return std::sqrt(sum);
}

SpatialRTreeMbr QuadrantPrefix(const SpatialRTreeMbr& prefix, u32 quadrant) {
  SpatialRTreeMbr child = prefix;
  const double mid_x = CenterAxis(prefix, 0);
  const double mid_y = CenterAxis(prefix, 1);
  if ((quadrant & 1u) == 0) {
    child.max[0] = mid_x;
  } else {
    child.min[0] = mid_x;
  }
  if ((quadrant & 2u) == 0) {
    child.max[1] = mid_y;
  } else {
    child.min[1] = mid_y;
  }
  return child;
}

SpatialRTreeMbr CoverRows(const std::vector<SpGistStoredRow>& rows) {
  SpatialRTreeMbr cover;
  bool first = true;
  for (const auto& row : rows) {
    if (row.tombstoned) continue;
    if (first) {
      cover = row.key;
      first = false;
    } else {
      cover = UnionMbr(cover, row.key);
    }
  }
  if (first) return cover;
  const double width = std::max(1.0, AxisWidth(cover, 0));
  const double height = std::max(1.0, AxisWidth(cover, 1));
  const double side = std::max(width, height);
  const double center_x = CenterAxis(cover, 0);
  const double center_y = CenterAxis(cover, 1);
  cover.min[0] = center_x - side * 0.5;
  cover.max[0] = center_x + side * 0.5;
  cover.min[1] = center_y - side * 0.5;
  cover.max[1] = center_y + side * 0.5;
  return cover;
}

bool ProviderAuthorityClean(const SpGistPhysicalProvider& provider) {
  return provider.candidate_evidence_only &&
         provider.exact_source_recheck_required &&
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

bool MbrValid(const SpatialRTreeMbr& mbr,
              const SpatialRTreeDescriptor& descriptor,
              const SpatialRTreeSridResource& resource) {
  return SpatialRTreeMbrValid(mbr, descriptor, resource);
}

bool StoredRowValid(const SpGistStoredRow& row,
                    const SpGistOpclassDescriptor& opclass,
                    const SpatialRTreeDescriptor& descriptor,
                    const SpatialRTreeSridResource& resource) {
  return LocatorValid(row.locator) &&
         MbrValid(row.key, descriptor, resource) &&
         SpGistCompressedKeyValid(row.compressed, opclass) &&
         DecodeSpatialRTreeMbr(row.compressed.bytes,
                               opclass.dimensions,
                               opclass.srid)
             .dimensions == opclass.dimensions &&
         !row.exact_source_recheck_evidence_ref.empty();
}

bool NodePrefixValid(const SpGistPhysicalProvider& provider,
                     const SpatialRTreeMbr& prefix) {
  return MbrValid(prefix, provider.spatial_descriptor, provider.srid_resource);
}

bool ProviderValid(const SpGistPhysicalProvider& provider) {
  if (provider.artifact_kind != kSpGistPhysicalProviderArtifactKind ||
      !SameFormatVersion(provider.format_version,
                         {kSpGistPhysicalProviderCurrentMajor,
                          kSpGistPhysicalProviderCurrentMinor}) ||
      !PageExtentSummaryUuidTextValid(provider.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.index_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.provider_uuid) ||
      provider.base_generation == 0 ||
      provider.provider_generation == 0 ||
      provider.leaf_capacity == 0 ||
      provider.max_depth == 0 ||
      provider.max_depth > 64 ||
      !DescriptorSafe(provider.spatial_descriptor) ||
      !SridResourceSafe(provider.srid_resource, provider.spatial_descriptor) ||
      !OpclassDescriptorSafe(provider.opclass) ||
      provider.opclass.dimensions != provider.spatial_descriptor.dimensions ||
      provider.opclass.srid != provider.srid_resource.srid ||
      !provider.physical_inner_tuple_layout_present ||
      !provider.physical_leaf_tuple_layout_present ||
      !provider.partitioned_search_tree_present ||
      !provider.choose_present ||
      !provider.inner_consistent_present ||
      !provider.leaf_consistent_present ||
      !provider.compress_present ||
      !provider.decompress_present ||
      !provider.bulk_spatial_prefix_build_present ||
      !ProviderAuthorityClean(provider)) {
    return false;
  }
  for (std::size_t i = 0; i < provider.rows.size(); ++i) {
    if (!StoredRowValid(provider.rows[i],
                        provider.opclass,
                        provider.spatial_descriptor,
                        provider.srid_resource) ||
        (i > 0 &&
         !LocatorLess(provider.rows[i - 1].locator,
                      provider.rows[i].locator))) {
      return false;
    }
  }
  const bool has_active =
      std::any_of(provider.rows.begin(), provider.rows.end(), [](const auto& row) {
        return !row.tombstoned;
      });
  if (!has_active) {
    return provider.nodes.empty() && provider.tree_height == 0;
  }
  if (provider.nodes.empty() ||
      provider.nodes.size() > kMaxNodes ||
      provider.root_node_id >= provider.nodes.size() ||
      provider.tree_height == 0) {
    return false;
  }
  std::vector<bool> visited(provider.nodes.size(), false);
  std::vector<u32> stack{provider.root_node_id};
  std::vector<TextInvertedRowLocator> seen_rows;
  while (!stack.empty()) {
    const u32 node_id = stack.back();
    stack.pop_back();
    if (node_id >= provider.nodes.size() || visited[node_id]) {
      return false;
    }
    visited[node_id] = true;
    const auto& node = provider.nodes[node_id];
    if (node.node_id != node_id ||
        node.depth > provider.max_depth ||
        !NodePrefixValid(provider, node.prefix)) {
      return false;
    }
    if (node.leaf) {
      if (!node.inner_tuples.empty() || node.leaf_tuples.empty()) {
        return false;
      }
      for (const auto& leaf : node.leaf_tuples) {
        if (leaf.tombstoned ||
            !LocatorValid(leaf.locator) ||
            !MbrValid(leaf.key,
                      provider.spatial_descriptor,
                      provider.srid_resource) ||
            !Contains(node.prefix, leaf.key) ||
            !SpGistCompressedKeyValid(leaf.compressed, provider.opclass) ||
            leaf.exact_source_recheck_evidence_ref.empty()) {
          return false;
        }
        seen_rows.push_back(leaf.locator);
      }
    } else {
      if (!node.leaf_tuples.empty() || node.inner_tuples.empty()) {
        return false;
      }
      std::vector<bool> used_partition(4, false);
      for (const auto& inner : node.inner_tuples) {
        if (inner.child_node >= provider.nodes.size() ||
            !SpGistPartitionKeyValid(inner.partition, provider.max_depth) ||
            inner.partition.depth != node.depth + 1 ||
            inner.partition.quadrant >= used_partition.size() ||
            used_partition[inner.partition.quadrant] ||
            !NodePrefixValid(provider, inner.prefix) ||
            !MbrSame(inner.prefix,
                     QuadrantPrefix(node.prefix, inner.partition.quadrant))) {
          return false;
        }
        used_partition[inner.partition.quadrant] = true;
        const auto& child = provider.nodes[inner.child_node];
        if (child.depth != inner.partition.depth ||
            !MbrSame(child.prefix, inner.prefix)) {
          return false;
        }
        stack.push_back(inner.child_node);
      }
    }
  }
  if (std::any_of(visited.begin(), visited.end(), [](bool seen) {
        return !seen;
      })) {
    return false;
  }
  std::sort(seen_rows.begin(), seen_rows.end(), LocatorLess);
  std::vector<TextInvertedRowLocator> active_rows;
  for (const auto& row : provider.rows) {
    if (!row.tombstoned) active_rows.push_back(row.locator);
  }
  std::sort(active_rows.begin(), active_rows.end(), LocatorLess);
  if (seen_rows.size() != active_rows.size()) {
    return false;
  }
  for (std::size_t i = 0; i < seen_rows.size(); ++i) {
    if (!LocatorEqual(seen_rows[i], active_rows[i])) {
      return false;
    }
  }
  return true;
}

void SetProviderEvidence(SpGistPhysicalProvider* provider) {
  provider->evidence = {
      kSpGistPhysicalProviderSearchKey,
      "spgist_physical_inner_tuple_layout",
      "spgist_physical_leaf_tuple_layout",
      "spgist_partitioned_search_tree",
      "spgist_choose_inner_consistent_leaf_consistent_opclass",
      "spgist_spatial_quad_mbr_opclass",
      "spgist_bulk_spatial_prefix_build",
      "spgist_candidate_locator_evidence"};
}

SpGistBuildResult BuildFailure(std::string code,
                               std::string key,
                               std::string detail = {}) {
  SpGistBuildResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeSpGistPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

SpGistOpenResult OpenFailure(SpGistOpenClass open_class,
                             std::string code,
                             std::string key,
                             std::string detail = {}) {
  SpGistOpenResult result;
  result.status = ErrorStatus();
  result.open_class = open_class;
  result.fail_closed = true;
  result.restricted_repair_required =
      open_class == SpGistOpenClass::bad_checksum ||
      open_class == SpGistOpenClass::corrupt_payload;
  result.diagnostic = MakeSpGistPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

SpGistQueryResult QueryFailure(std::string code,
                               std::string key,
                               std::string detail = {}) {
  SpGistQueryResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeSpGistPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

SpGistMutationResult MutationFailure(std::string code,
                                     std::string key,
                                     std::string detail = {}) {
  SpGistMutationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeSpGistPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
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
void AppendMbr(std::vector<byte>* out, const SpatialRTreeMbr& mbr) {
  AppendU32(out, mbr.dimensions);
  AppendU32(out, mbr.srid);
  for (double value : mbr.min) AppendF64(out, value);
  for (double value : mbr.max) AppendF64(out, value);
}
void AppendPartition(std::vector<byte>* out, const SpGistPartitionKey& key) {
  AppendU32(out, key.depth);
  AppendU32(out, key.quadrant);
  AppendBool(out, key.valid);
}
void AppendCompressed(std::vector<byte>* out, const SpGistCompressedKey& key) {
  AppendBytes(out, key.bytes);
  AppendU32(out, key.dimensions);
  AppendU32(out, key.srid);
  AppendBool(out, key.deterministic);
  AppendBool(out, key.valid);
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
  bool Done() const { return offset_ == bytes_.size(); }
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
  bool ReadLocator(TextInvertedRowLocator* locator) {
    return ReadU64(&locator->row_ordinal) &&
           ReadString(&locator->row_uuid) &&
           ReadString(&locator->version_uuid);
  }
  bool ReadMbr(SpatialRTreeMbr* mbr) {
    if (!ReadU32(&mbr->dimensions) ||
        !ReadU32(&mbr->srid) ||
        mbr->dimensions != 2) {
      return false;
    }
    mbr->min.assign(mbr->dimensions, 0.0);
    mbr->max.assign(mbr->dimensions, 0.0);
    for (double& value : mbr->min) {
      if (!ReadF64(&value)) return false;
    }
    for (double& value : mbr->max) {
      if (!ReadF64(&value)) return false;
    }
    return true;
  }
  bool ReadPartition(SpGistPartitionKey* key) {
    return ReadU32(&key->depth) &&
           ReadU32(&key->quadrant) &&
           ReadBool(&key->valid);
  }
  bool ReadCompressed(SpGistCompressedKey* key) {
    return ReadBytes(&key->bytes) &&
           ReadU32(&key->dimensions) &&
           ReadU32(&key->srid) &&
           ReadBool(&key->deterministic) &&
           ReadBool(&key->valid);
  }

 private:
  const std::vector<byte>& bytes_;
  std::size_t offset_ = 0;
};

struct BuildTreeState {
  SpGistOpclassRuntime opclass;
  u32 leaf_capacity = kSpGistDefaultLeafCapacity;
  u32 max_depth = kSpGistDefaultMaxDepth;
  std::vector<SpGistNode> nodes;
  u64 split_count = 0;
  u64 choose_calls = 0;
  bool invalid_opclass = false;
};

enum class PartitionRowsResult {
  partitioned,
  leaf_required,
  invalid_opclass
};

u32 NewNode(BuildTreeState* state,
            bool leaf,
            u32 depth,
            const SpatialRTreeMbr& prefix) {
  SpGistNode node;
  node.node_id = static_cast<u32>(state->nodes.size());
  node.leaf = leaf;
  node.depth = depth;
  node.prefix = prefix;
  state->nodes.push_back(std::move(node));
  return static_cast<u32>(state->nodes.size() - 1);
}

bool PartitionRows(BuildTreeState* state,
                   const SpatialRTreeMbr& prefix,
                   u32 depth,
                   const std::vector<const SpGistStoredRow*>& input,
                   std::array<std::vector<const SpGistStoredRow*>, 4>* output) {
  for (const auto* row : input) {
    const auto chosen =
        state->opclass.methods.choose(prefix, row->compressed, depth + 1);
    ++state->choose_calls;
    if (!chosen.valid ||
        !chosen.deterministic ||
        !SpGistPartitionKeyValid(chosen.partition, state->max_depth) ||
        chosen.partition.depth != depth + 1 ||
        chosen.partition.quadrant >= output->size()) {
      state->invalid_opclass = true;
      return false;
    }
    const auto child_prefix =
        QuadrantPrefix(prefix, chosen.partition.quadrant);
    if (!Contains(child_prefix, row->key)) {
      return false;
    }
    (*output)[chosen.partition.quadrant].push_back(row);
  }
  return true;
}

PartitionRowsResult PartitionRowsChecked(
    BuildTreeState* state,
    const SpatialRTreeMbr& prefix,
    u32 depth,
    const std::vector<const SpGistStoredRow*>& input,
    std::array<std::vector<const SpGistStoredRow*>, 4>* output) {
  if (PartitionRows(state, prefix, depth, input, output)) {
    return PartitionRowsResult::partitioned;
  }
  if (state->invalid_opclass) {
    return PartitionRowsResult::invalid_opclass;
  }
  return PartitionRowsResult::leaf_required;
}

u32 BuildLeafNode(BuildTreeState* state,
                  const SpatialRTreeMbr& prefix,
                  u32 depth,
                  const std::vector<const SpGistStoredRow*>& rows) {
  const u32 node_id = NewNode(state, true, depth, prefix);
  auto& node = state->nodes[node_id];
  for (const auto* row : rows) {
    SpGistLeafTuple tuple;
    tuple.locator = row->locator;
    tuple.compressed = row->compressed;
    tuple.key = row->key;
    tuple.exact_source_recheck_evidence_ref =
        row->exact_source_recheck_evidence_ref;
    node.leaf_tuples.push_back(std::move(tuple));
  }
  std::sort(node.leaf_tuples.begin(), node.leaf_tuples.end(),
            [](const auto& left, const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
  return node_id;
}

u32 BuildPrefixNode(BuildTreeState* state,
                    const SpatialRTreeMbr& prefix,
                    u32 depth,
                    const std::vector<const SpGistStoredRow*>& rows) {
  if (rows.empty()) {
    return NewNode(state, true, depth, prefix);
  }
  if (rows.size() <= state->leaf_capacity || depth >= state->max_depth) {
    return BuildLeafNode(state, prefix, depth, rows);
  }

  std::array<std::vector<const SpGistStoredRow*>, 4> partitions;
  const auto partition_result =
      PartitionRowsChecked(state, prefix, depth, rows, &partitions);
  if (partition_result == PartitionRowsResult::invalid_opclass) {
    return NewNode(state, true, depth, prefix);
  }
  if (partition_result == PartitionRowsResult::leaf_required) {
    return BuildLeafNode(state, prefix, depth, rows);
  }
  u32 non_empty = 0;
  for (const auto& partition : partitions) {
    if (!partition.empty()) ++non_empty;
  }
  if (non_empty <= 1 && depth + 1 >= state->max_depth) {
    return BuildLeafNode(state, prefix, depth, rows);
  }

  const u32 node_id = NewNode(state, false, depth, prefix);
  ++state->split_count;
  for (u32 quadrant = 0; quadrant < partitions.size(); ++quadrant) {
    if (partitions[quadrant].empty()) continue;
    const auto child_prefix = QuadrantPrefix(prefix, quadrant);
    const u32 child_node =
        BuildPrefixNode(state, child_prefix, depth + 1, partitions[quadrant]);
    SpGistInnerTuple inner;
    inner.child_node = child_node;
    inner.partition.depth = depth + 1;
    inner.partition.quadrant = quadrant;
    inner.partition.valid = true;
    inner.prefix = child_prefix;
    state->nodes[node_id].inner_tuples.push_back(std::move(inner));
  }
  return node_id;
}

bool InstallTree(SpGistPhysicalProvider* provider,
                 const SpGistOpclassRuntime& opclass) {
  BuildTreeState state;
  state.opclass = opclass;
  state.leaf_capacity = provider->leaf_capacity;
  state.max_depth = provider->max_depth;
  std::vector<const SpGistStoredRow*> active;
  active.reserve(provider->rows.size());
  for (const auto& row : provider->rows) {
    if (!row.tombstoned) active.push_back(&row);
  }
  if (active.empty()) {
    provider->nodes.clear();
    provider->root_node_id = 0;
    provider->tree_height = 0;
    provider->split_partition_count = 0;
    return true;
  }
  const auto root_prefix = CoverRows(provider->rows);
  if (!MbrValid(root_prefix,
                provider->spatial_descriptor,
                provider->srid_resource)) {
    return false;
  }
  const u32 root = BuildPrefixNode(&state, root_prefix, 0, active);
  if (state.invalid_opclass) {
    return false;
  }
  provider->nodes = std::move(state.nodes);
  provider->root_node_id = root;
  provider->tree_height = 0;
  for (const auto& node : provider->nodes) {
    provider->tree_height = std::max(provider->tree_height, node.depth + 1);
  }
  provider->split_partition_count = state.split_count;
  provider->choose_call_count += state.choose_calls;
  return true;
}

SpGistCompressedKey CompressChecked(const SpGistOpclassRuntime& opclass,
                                    const SpatialRTreeMbr& key) {
  if (!OpclassRuntimeSafe(opclass)) return {};
  auto compressed = opclass.methods.compress(key);
  if (!SpGistCompressedKeyValid(compressed, opclass.descriptor)) return {};
  const auto decompressed = opclass.methods.decompress(compressed);
  if (!MbrSame(decompressed, key)) return {};
  return compressed;
}

const SpGistStoredRow* FindStoredRow(const SpGistPhysicalProvider& provider,
                                     const TextInvertedRowLocator& locator) {
  auto iter = std::lower_bound(
      provider.rows.begin(), provider.rows.end(), locator,
      [](const SpGistStoredRow& row, const TextInvertedRowLocator& value) {
        return LocatorLess(row.locator, value);
      });
  if (iter == provider.rows.end() || !LocatorEqual(iter->locator, locator)) {
    return nullptr;
  }
  return &*iter;
}

bool SourceMatchesStored(const SpGistSourceRow& source,
                         const SpGistStoredRow& stored) {
  return LocatorEqual(source.locator, stored.locator) &&
         MbrSame(source.key, stored.key) &&
         source.exact_source_recheck_evidence_ref ==
             stored.exact_source_recheck_evidence_ref;
}

bool SourceRowValid(const SpGistSourceRow& row,
                    const SpatialRTreeDescriptor& descriptor,
                    const SpatialRTreeSridResource& resource) {
  return LocatorValid(row.locator) &&
         MbrValid(row.key, descriptor, resource) &&
         !row.exact_source_recheck_evidence_ref.empty();
}

bool QueryMbrCompatible(const SpGistQueryRequest& request) {
  if (request.strategy == SpGistPredicateStrategy::nearest) {
    return request.query_point.size() == request.provider.opclass.dimensions &&
           std::all_of(request.query_point.begin(),
                       request.query_point.end(),
                       [](double value) { return std::isfinite(value); });
  }
  return MbrValid(request.query_mbr,
                  request.provider.spatial_descriptor,
                  request.provider.srid_resource);
}

bool RuntimeRequestSafe(const SpGistQueryRequest& request) {
  return ProviderValid(request.provider) &&
         OpclassRuntimeSafe(request.opclass) &&
         SameOpclassDescriptor(request.provider.opclass,
                               request.opclass.descriptor) &&
         RecheckProofValid(request.recheck_proof) &&
         request.opclass_epoch_current &&
         request.resource_epoch_current &&
         request.descriptor_epoch_current &&
         request.srid_resource_epoch_current &&
         !request.descriptor_store_scan &&
         !request.behavior_store_scan &&
         !request.contract_only_fallback &&
         !request.provider_only_fallback &&
         QueryMbrCompatible(request);
}

SpGistCandidate MakeCandidate(const SpGistLeafTuple& tuple, double distance) {
  SpGistCandidate candidate;
  candidate.locator = tuple.locator;
  candidate.key = tuple.key;
  candidate.distance = distance;
  candidate.opclass_leaf_consistent = true;
  candidate.source_recheck_evidence_ref =
      tuple.exact_source_recheck_evidence_ref;
  return candidate;
}

void SortCandidates(std::vector<SpGistCandidate>* candidates) {
  std::sort(candidates->begin(), candidates->end(), [](const auto& left,
                                                       const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
}

}  // namespace

SpGistOpclassRuntime MakeSpatialQuadMbrSpGistOpclass(u64 opclass_epoch,
                                                     u64 resource_epoch,
                                                     u32 srid) {
  SpGistOpclassRuntime runtime;
  runtime.descriptor.opclass_name = kSpGistSpatialQuadMbrOpclassName;
  runtime.descriptor.opclass_epoch = opclass_epoch;
  runtime.descriptor.resource_epoch = resource_epoch;
  runtime.descriptor.registered = true;
  runtime.descriptor.deterministic = true;
  runtime.descriptor.immutable = true;
  runtime.descriptor.safe = true;
  runtime.descriptor.dimensions = 2;
  runtime.descriptor.srid = srid;

  runtime.methods.compress = [srid](const SpatialRTreeMbr& mbr) {
    SpGistCompressedKey key;
    key.dimensions = 2;
    key.srid = srid;
    if (mbr.dimensions != 2 || mbr.srid != srid) {
      return key;
    }
    key.bytes = EncodeSpatialRTreeMbr(mbr);
    key.deterministic = true;
    key.valid = !key.bytes.empty();
    return key;
  };
  runtime.methods.decompress = [srid](const SpGistCompressedKey& key) {
    if (key.dimensions != 2 || key.srid != srid ||
        !key.deterministic || !key.valid) {
      return SpatialRTreeMbr{};
    }
    return DecodeSpatialRTreeMbr(key.bytes, 2, srid);
  };
  runtime.methods.choose =
      [runtime](const SpatialRTreeMbr& prefix,
                const SpGistCompressedKey& key,
                u32 child_depth) {
        SpGistChooseResult result;
        const auto mbr = runtime.methods.decompress(key);
        if (mbr.dimensions != 2 || prefix.dimensions != 2 ||
            mbr.srid != prefix.srid ||
            child_depth == 0) {
          return result;
        }
        const double mid_x = CenterAxis(prefix, 0);
        const double mid_y = CenterAxis(prefix, 1);
        const double center_x = CenterAxis(mbr, 0);
        const double center_y = CenterAxis(mbr, 1);
        result.partition.depth = child_depth;
        result.partition.quadrant =
            (center_x >= mid_x ? 1u : 0u) | (center_y >= mid_y ? 2u : 0u);
        result.partition.valid = true;
        result.valid = true;
        result.deterministic = true;
        return result;
      };
  runtime.methods.inner_consistent =
      [](const SpatialRTreeMbr& prefix,
         const SpatialRTreeMbr& query,
         SpGistPredicateStrategy strategy,
         u32 child_depth) {
        std::vector<SpGistPartitionKey> out;
        if (prefix.dimensions != 2 ||
            (strategy != SpGistPredicateStrategy::nearest &&
             (query.dimensions != 2 || query.srid != prefix.srid))) {
          return out;
        }
        for (u32 quadrant = 0; quadrant < 4; ++quadrant) {
          const auto child = QuadrantPrefix(prefix, quadrant);
          bool admit = false;
          switch (strategy) {
            case SpGistPredicateStrategy::point:
            case SpGistPredicateStrategy::contains:
            case SpGistPredicateStrategy::intersects:
            case SpGistPredicateStrategy::range:
              admit = Intersects(child, query);
              break;
            case SpGistPredicateStrategy::within:
              admit = Intersects(child, query) || Contains(query, child);
              break;
            case SpGistPredicateStrategy::nearest:
              admit = true;
              break;
          }
          if (admit) {
            out.push_back({child_depth, quadrant, true});
          }
        }
        return out;
      };
  runtime.methods.leaf_consistent =
      [runtime](const SpGistCompressedKey& key,
                const SpatialRTreeMbr& query,
                SpGistPredicateStrategy strategy) {
        const auto mbr = runtime.methods.decompress(key);
        if (mbr.dimensions != 2 ||
            (strategy != SpGistPredicateStrategy::nearest &&
             (query.dimensions != 2 || query.srid != mbr.srid))) {
          return false;
        }
        switch (strategy) {
          case SpGistPredicateStrategy::point:
          case SpGistPredicateStrategy::contains:
            return Contains(mbr, query);
          case SpGistPredicateStrategy::within:
            return Contains(query, mbr);
          case SpGistPredicateStrategy::intersects:
          case SpGistPredicateStrategy::range:
            return Intersects(mbr, query);
          case SpGistPredicateStrategy::nearest:
            return true;
        }
        return false;
      };
  runtime.methods.distance =
      [runtime](const SpGistCompressedKey& key,
                const std::vector<double>& point) {
        const auto mbr = runtime.methods.decompress(key);
        if (mbr.dimensions != 2 || point.size() != 2 ||
            !std::all_of(point.begin(), point.end(), [](double value) {
              return std::isfinite(value);
            })) {
          return std::numeric_limits<double>::infinity();
        }
        return MinDistanceToPoint(mbr, point);
      };
  return runtime;
}

bool SpGistCompressedKeyValid(const SpGistCompressedKey& key,
                              const SpGistOpclassDescriptor& opclass) {
  if (!OpclassDescriptorSafe(opclass) ||
      !key.valid ||
      !key.deterministic ||
      key.dimensions != opclass.dimensions ||
      key.srid != opclass.srid ||
      key.bytes.empty()) {
    return false;
  }
  const auto decoded =
      DecodeSpatialRTreeMbr(key.bytes, opclass.dimensions, opclass.srid);
  return decoded.dimensions == opclass.dimensions &&
         decoded.srid == opclass.srid &&
         EncodeSpatialRTreeMbr(decoded) == key.bytes;
}

bool SpGistPartitionKeyValid(const SpGistPartitionKey& key, u32 max_depth) {
  return key.valid && key.depth > 0 && key.depth <= max_depth &&
         key.quadrant < 4;
}

SpGistBuildResult BuildSpGistPhysicalProvider(
    const SpGistBuildRequest& request) {
  if (!RecheckAuthorityClean(request.recheck_proof) ||
      !DescriptorAuthorityClean(request.spatial_descriptor) ||
      !SridAuthorityClean(request.srid_resource) ||
      !OpclassAuthorityClean(request.opclass.descriptor)) {
    return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED",
                        "index.spgist_physical_provider.authority_claim_refused");
  }
  if (!RecheckProofValid(request.recheck_proof)) {
    return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
                        "index.spgist_physical_provider.missing_exact_recheck");
  }
  if (!DescriptorSafe(request.spatial_descriptor) ||
      !SridResourceSafe(request.srid_resource, request.spatial_descriptor) ||
      !OpclassRuntimeSafe(request.opclass)) {
    return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.UNSAFE_OPCLASS",
                        "index.spgist_physical_provider.unsafe_opclass");
  }
  if (request.opclass.descriptor.dimensions !=
          request.spatial_descriptor.dimensions ||
      request.opclass.descriptor.srid != request.srid_resource.srid) {
    return BuildFailure(
        "INDEX.SPGIST_PHYSICAL_PROVIDER.INCOMPATIBLE_SPATIAL_PROFILE",
        "index.spgist_physical_provider.incompatible_spatial_profile");
  }
  if (request.base_generation == 0 ||
      request.provider_generation == 0 ||
      request.leaf_capacity == 0 ||
      request.max_depth == 0 ||
      request.max_depth > 64 ||
      !PageExtentSummaryUuidTextValid(request.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(request.index_uuid) ||
      !PageExtentSummaryUuidTextValid(request.provider_uuid)) {
    return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.BUILD_REFUSED",
                        "index.spgist_physical_provider.build_refused");
  }

  SpGistPhysicalProvider provider;
  provider.relation_uuid = request.relation_uuid;
  provider.index_uuid = request.index_uuid;
  provider.provider_uuid = request.provider_uuid;
  provider.base_generation = request.base_generation;
  provider.provider_generation = request.provider_generation;
  provider.spatial_descriptor = request.spatial_descriptor;
  provider.srid_resource = request.srid_resource;
  provider.opclass = request.opclass.descriptor;
  provider.leaf_capacity = request.leaf_capacity;
  provider.max_depth = request.max_depth;
  provider.rows.reserve(request.rows.size());
  SetProviderEvidence(&provider);

  for (const auto& source : request.rows) {
    if (!SourceRowValid(source, request.spatial_descriptor, request.srid_resource)) {
      return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
                          "index.spgist_physical_provider.invalid_source_key");
    }
    auto compressed = CompressChecked(request.opclass, source.key);
    ++provider.compress_call_count;
    ++provider.decompress_call_count;
    if (!SpGistCompressedKeyValid(compressed, request.opclass.descriptor)) {
      return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
                          "index.spgist_physical_provider.invalid_compressed_key");
    }
    provider.rows.push_back({source.locator,
                             source.key,
                             std::move(compressed),
                             source.exact_source_recheck_evidence_ref,
                             false});
  }
  std::sort(provider.rows.begin(), provider.rows.end(),
            [](const auto& left, const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
  for (std::size_t i = 1; i < provider.rows.size(); ++i) {
    if (LocatorEqual(provider.rows[i - 1].locator, provider.rows[i].locator)) {
      return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.DUPLICATE_ROW_LOCATOR",
                          "index.spgist_physical_provider.duplicate_row_locator");
    }
  }

  if (!provider.rows.empty()) {
    const auto first_prefix = CoverRows(provider.rows);
    const auto chosen =
        request.opclass.methods.choose(first_prefix,
                                       provider.rows.front().compressed,
                                       1);
    ++provider.choose_call_count;
    if (!chosen.valid ||
        !chosen.deterministic ||
        !SpGistPartitionKeyValid(chosen.partition, provider.max_depth)) {
      return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.INVALID_PARTITION_KEY",
                          "index.spgist_physical_provider.invalid_partition_key");
    }
    const auto inner = request.opclass.methods.inner_consistent(
        first_prefix,
        provider.rows.front().key,
        SpGistPredicateStrategy::intersects,
        1);
    ++provider.inner_consistent_call_count;
    if (inner.empty()) {
      return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.OPCLASS_INCONSISTENT",
                          "index.spgist_physical_provider.inner_inconsistent");
    }
    if (!request.opclass.methods.leaf_consistent(provider.rows.front().compressed,
                                                provider.rows.front().key,
                                                SpGistPredicateStrategy::intersects)) {
      return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.OPCLASS_INCONSISTENT",
                          "index.spgist_physical_provider.leaf_inconsistent");
    }
    ++provider.leaf_consistent_call_count;
    const auto distance =
        request.opclass.methods.distance(provider.rows.front().compressed,
                                         provider.rows.front().key.min);
    if (!std::isfinite(distance)) {
      return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.OPCLASS_INCONSISTENT",
                          "index.spgist_physical_provider.distance_inconsistent");
    }
    ++provider.distance_call_count;
  }

  if (!InstallTree(&provider, request.opclass) || !ProviderValid(provider)) {
    return BuildFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.BUILD_CORRUPT",
                        "index.spgist_physical_provider.build_corrupt");
  }

  SpGistBuildResult result;
  result.status = OkStatus();
  result.provider = std::move(provider);
  result.built = true;
  result.fail_closed = false;
  result.used_bulk_spatial_prefix_build = true;
  result.all_methods_exercised =
      result.provider.choose_call_count > 0 &&
      result.provider.inner_consistent_call_count > 0 &&
      result.provider.leaf_consistent_call_count > 0 &&
      result.provider.compress_call_count > 0 &&
      result.provider.decompress_call_count > 0 &&
      result.provider.distance_call_count > 0;
  return result;
}

SpGistSerializeResult SerializeSpGistPhysicalProvider(
    const SpGistPhysicalProvider& provider) {
  SpGistSerializeResult result;
  if (!ProviderValid(provider)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeSpGistPhysicalProviderDiagnostic(
        result.status,
        "INDEX.SPGIST_PHYSICAL_PROVIDER.SERIALIZE_REFUSED",
        "index.spgist_physical_provider.serialize_refused");
    return result;
  }
  std::vector<byte> bytes;
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  AppendU32(&bytes, kSpGistPhysicalProviderCurrentMajor);
  AppendU32(&bytes, kSpGistPhysicalProviderCurrentMinor);
  AppendU64(&bytes, 0);
  AppendString(&bytes, provider.relation_uuid);
  AppendString(&bytes, provider.index_uuid);
  AppendString(&bytes, provider.provider_uuid);
  AppendU64(&bytes, provider.base_generation);
  AppendU64(&bytes, provider.provider_generation);
  AppendU32(&bytes, provider.spatial_descriptor.dimensions);
  AppendU64(&bytes, provider.spatial_descriptor.descriptor_epoch);
  AppendBool(&bytes, provider.spatial_descriptor.deterministic);
  AppendBool(&bytes, provider.spatial_descriptor.descriptor_safe);
  AppendBool(&bytes, provider.spatial_descriptor.supports_point);
  AppendBool(&bytes, provider.spatial_descriptor.supports_mbr);
  AppendBool(&bytes, provider.spatial_descriptor.supports_z);
  AppendBool(&bytes, provider.spatial_descriptor.supports_m);
  AppendString(&bytes, provider.srid_resource.resource_uuid);
  AppendU32(&bytes, provider.srid_resource.srid);
  AppendU64(&bytes, provider.srid_resource.resource_epoch);
  AppendString(&bytes, provider.srid_resource.coordinate_order);
  AppendBool(&bytes, provider.srid_resource.deterministic);
  AppendBool(&bytes, provider.srid_resource.safe);
  AppendBool(&bytes, provider.srid_resource.cache_present);
  AppendString(&bytes, provider.opclass.opclass_name);
  AppendU64(&bytes, provider.opclass.opclass_epoch);
  AppendU64(&bytes, provider.opclass.resource_epoch);
  AppendBool(&bytes, provider.opclass.registered);
  AppendBool(&bytes, provider.opclass.deterministic);
  AppendBool(&bytes, provider.opclass.immutable);
  AppendBool(&bytes, provider.opclass.safe);
  AppendU32(&bytes, provider.opclass.dimensions);
  AppendU32(&bytes, provider.opclass.srid);
  AppendU32(&bytes, provider.root_node_id);
  AppendU32(&bytes, provider.tree_height);
  AppendU32(&bytes, provider.leaf_capacity);
  AppendU32(&bytes, provider.max_depth);
  AppendU64(&bytes, provider.split_partition_count);
  AppendU64(&bytes, provider.merge_count);
  AppendU64(&bytes, provider.choose_call_count);
  AppendU64(&bytes, provider.inner_consistent_call_count);
  AppendU64(&bytes, provider.leaf_consistent_call_count);
  AppendU64(&bytes, provider.compress_call_count);
  AppendU64(&bytes, provider.decompress_call_count);
  AppendU64(&bytes, provider.distance_call_count);
  AppendU64(&bytes, static_cast<u64>(provider.rows.size()));
  for (const auto& row : provider.rows) {
    AppendLocator(&bytes, row.locator);
    AppendMbr(&bytes, row.key);
    AppendCompressed(&bytes, row.compressed);
    AppendString(&bytes, row.exact_source_recheck_evidence_ref);
    AppendBool(&bytes, row.tombstoned);
  }
  AppendU64(&bytes, static_cast<u64>(provider.nodes.size()));
  for (const auto& node : provider.nodes) {
    AppendU32(&bytes, node.node_id);
    AppendBool(&bytes, node.leaf);
    AppendU32(&bytes, node.depth);
    AppendMbr(&bytes, node.prefix);
    AppendU64(&bytes, static_cast<u64>(node.inner_tuples.size()));
    for (const auto& inner : node.inner_tuples) {
      AppendU32(&bytes, inner.child_node);
      AppendPartition(&bytes, inner.partition);
      AppendMbr(&bytes, inner.prefix);
    }
    AppendU64(&bytes, static_cast<u64>(node.leaf_tuples.size()));
    for (const auto& leaf : node.leaf_tuples) {
      AppendLocator(&bytes, leaf.locator);
      AppendCompressed(&bytes, leaf.compressed);
      AppendMbr(&bytes, leaf.key);
      AppendString(&bytes, leaf.exact_source_recheck_evidence_ref);
      AppendBool(&bytes, leaf.tombstoned);
    }
  }
  result.checksum = ComputeChecksum(bytes);
  StoreLittle64(bytes.data() + 16, result.checksum);
  result.status = OkStatus();
  result.bytes = std::move(bytes);
  return result;
}

SpGistOpenResult OpenSpGistPhysicalProvider(const SpGistOpenRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return OpenFailure(SpGistOpenClass::missing_recheck_proof,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
                       "index.spgist_physical_provider.missing_exact_recheck");
  }
  if (!OpclassRuntimeSafe(request.opclass)) {
    return OpenFailure(SpGistOpenClass::unsafe_opclass,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.UNSAFE_OPCLASS",
                       "index.spgist_physical_provider.unsafe_opclass");
  }
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(SpGistOpenClass::corrupt_payload,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.spgist_physical_provider.corrupt_payload");
  }
  const u64 stored_checksum = LoadLittle64(request.bytes.data() + 16);
  if (stored_checksum == 0 ||
      stored_checksum != ComputeChecksum(request.bytes)) {
    return OpenFailure(SpGistOpenClass::bad_checksum,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.BAD_CHECKSUM",
                       "index.spgist_physical_provider.bad_checksum");
  }

  Reader reader(request.bytes);
  reader.SetOffset(8);
  SpGistPhysicalProvider provider;
  u32 major = 0;
  u32 minor = 0;
  u64 checksum = 0;
  if (!reader.ReadU32(&major) ||
      !reader.ReadU32(&minor) ||
      !reader.ReadU64(&checksum)) {
    return OpenFailure(SpGistOpenClass::corrupt_payload,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.spgist_physical_provider.corrupt_payload");
  }
  provider.format_version = {major, minor};
  if (!SameFormatVersion(provider.format_version,
                         {kSpGistPhysicalProviderCurrentMajor,
                          kSpGistPhysicalProviderCurrentMinor})) {
    return OpenFailure(SpGistOpenClass::stale_format,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_FORMAT",
                       "index.spgist_physical_provider.stale_format");
  }
  if (!reader.ReadString(&provider.relation_uuid) ||
      !reader.ReadString(&provider.index_uuid) ||
      !reader.ReadString(&provider.provider_uuid) ||
      !reader.ReadU64(&provider.base_generation) ||
      !reader.ReadU64(&provider.provider_generation) ||
      !reader.ReadU32(&provider.spatial_descriptor.dimensions) ||
      !reader.ReadU64(&provider.spatial_descriptor.descriptor_epoch) ||
      !reader.ReadBool(&provider.spatial_descriptor.deterministic) ||
      !reader.ReadBool(&provider.spatial_descriptor.descriptor_safe) ||
      !reader.ReadBool(&provider.spatial_descriptor.supports_point) ||
      !reader.ReadBool(&provider.spatial_descriptor.supports_mbr) ||
      !reader.ReadBool(&provider.spatial_descriptor.supports_z) ||
      !reader.ReadBool(&provider.spatial_descriptor.supports_m) ||
      !reader.ReadString(&provider.srid_resource.resource_uuid) ||
      !reader.ReadU32(&provider.srid_resource.srid) ||
      !reader.ReadU64(&provider.srid_resource.resource_epoch) ||
      !reader.ReadString(&provider.srid_resource.coordinate_order) ||
      !reader.ReadBool(&provider.srid_resource.deterministic) ||
      !reader.ReadBool(&provider.srid_resource.safe) ||
      !reader.ReadBool(&provider.srid_resource.cache_present) ||
      !reader.ReadString(&provider.opclass.opclass_name) ||
      !reader.ReadU64(&provider.opclass.opclass_epoch) ||
      !reader.ReadU64(&provider.opclass.resource_epoch) ||
      !reader.ReadBool(&provider.opclass.registered) ||
      !reader.ReadBool(&provider.opclass.deterministic) ||
      !reader.ReadBool(&provider.opclass.immutable) ||
      !reader.ReadBool(&provider.opclass.safe) ||
      !reader.ReadU32(&provider.opclass.dimensions) ||
      !reader.ReadU32(&provider.opclass.srid) ||
      !reader.ReadU32(&provider.root_node_id) ||
      !reader.ReadU32(&provider.tree_height) ||
      !reader.ReadU32(&provider.leaf_capacity) ||
      !reader.ReadU32(&provider.max_depth) ||
      !reader.ReadU64(&provider.split_partition_count) ||
      !reader.ReadU64(&provider.merge_count) ||
      !reader.ReadU64(&provider.choose_call_count) ||
      !reader.ReadU64(&provider.inner_consistent_call_count) ||
      !reader.ReadU64(&provider.leaf_consistent_call_count) ||
      !reader.ReadU64(&provider.compress_call_count) ||
      !reader.ReadU64(&provider.decompress_call_count) ||
      !reader.ReadU64(&provider.distance_call_count)) {
    return OpenFailure(SpGistOpenClass::corrupt_payload,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.spgist_physical_provider.corrupt_payload");
  }
  u64 row_count = 0;
  if (!reader.ReadU64(&row_count) || row_count > kMaxRows) {
    return OpenFailure(SpGistOpenClass::corrupt_payload,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.spgist_physical_provider.corrupt_payload");
  }
  provider.rows.resize(static_cast<std::size_t>(row_count));
  for (auto& row : provider.rows) {
    if (!reader.ReadLocator(&row.locator) ||
        !reader.ReadMbr(&row.key) ||
        !reader.ReadCompressed(&row.compressed) ||
        !reader.ReadString(&row.exact_source_recheck_evidence_ref) ||
        !reader.ReadBool(&row.tombstoned)) {
      return OpenFailure(SpGistOpenClass::corrupt_payload,
                         "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                         "index.spgist_physical_provider.corrupt_payload");
    }
  }
  u64 node_count = 0;
  if (!reader.ReadU64(&node_count) || node_count > kMaxNodes) {
    return OpenFailure(SpGistOpenClass::corrupt_payload,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.spgist_physical_provider.corrupt_payload");
  }
  provider.nodes.resize(static_cast<std::size_t>(node_count));
  for (auto& node : provider.nodes) {
    u64 inner_count = 0;
    u64 leaf_count = 0;
    if (!reader.ReadU32(&node.node_id) ||
        !reader.ReadBool(&node.leaf) ||
        !reader.ReadU32(&node.depth) ||
        !reader.ReadMbr(&node.prefix) ||
        !reader.ReadU64(&inner_count) ||
        inner_count > 4) {
      return OpenFailure(SpGistOpenClass::corrupt_payload,
                         "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                         "index.spgist_physical_provider.corrupt_payload");
    }
    node.inner_tuples.resize(static_cast<std::size_t>(inner_count));
    for (auto& inner : node.inner_tuples) {
      if (!reader.ReadU32(&inner.child_node) ||
          !reader.ReadPartition(&inner.partition) ||
          !reader.ReadMbr(&inner.prefix)) {
        return OpenFailure(SpGistOpenClass::corrupt_payload,
                           "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                           "index.spgist_physical_provider.corrupt_payload");
      }
    }
    if (!reader.ReadU64(&leaf_count) || leaf_count > kMaxRows) {
      return OpenFailure(SpGistOpenClass::corrupt_payload,
                         "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                         "index.spgist_physical_provider.corrupt_payload");
    }
    node.leaf_tuples.resize(static_cast<std::size_t>(leaf_count));
    for (auto& leaf : node.leaf_tuples) {
      if (!reader.ReadLocator(&leaf.locator) ||
          !reader.ReadCompressed(&leaf.compressed) ||
          !reader.ReadMbr(&leaf.key) ||
          !reader.ReadString(&leaf.exact_source_recheck_evidence_ref) ||
          !reader.ReadBool(&leaf.tombstoned)) {
        return OpenFailure(SpGistOpenClass::corrupt_payload,
                           "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                           "index.spgist_physical_provider.corrupt_payload");
      }
    }
  }
  if (!reader.Done()) {
    return OpenFailure(SpGistOpenClass::corrupt_payload,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.spgist_physical_provider.corrupt_payload");
  }
  SetProviderEvidence(&provider);
  if (!SameOpclassDescriptor(provider.opclass, request.opclass.descriptor)) {
    return OpenFailure(SpGistOpenClass::unsafe_opclass,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.UNSAFE_OPCLASS",
                       "index.spgist_physical_provider.unsafe_opclass");
  }
  if ((request.expected_relation_uuid_present &&
       request.expected_relation_uuid != provider.relation_uuid) ||
      (request.expected_index_uuid_present &&
       request.expected_index_uuid != provider.index_uuid) ||
      (request.expected_provider_uuid_present &&
       request.expected_provider_uuid != provider.provider_uuid)) {
    return OpenFailure(SpGistOpenClass::identity_mismatch,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.IDENTITY_MISMATCH",
                       "index.spgist_physical_provider.identity_mismatch");
  }
  if ((request.expected_base_generation_present &&
       request.expected_base_generation != provider.base_generation) ||
      (request.expected_provider_generation_present &&
       request.expected_provider_generation != provider.provider_generation)) {
    return OpenFailure(SpGistOpenClass::stale_generation,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_GENERATION",
                       "index.spgist_physical_provider.stale_generation");
  }
  if (request.expected_opclass_epoch_present &&
      request.expected_opclass_epoch != provider.opclass.opclass_epoch) {
    return OpenFailure(SpGistOpenClass::stale_opclass_epoch,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_OPCLASS_EPOCH",
                       "index.spgist_physical_provider.stale_opclass_epoch");
  }
  if (request.expected_resource_epoch_present &&
      request.expected_resource_epoch != provider.opclass.resource_epoch) {
    return OpenFailure(SpGistOpenClass::stale_resource_epoch,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_RESOURCE_EPOCH",
                       "index.spgist_physical_provider.stale_resource_epoch");
  }
  if (request.expected_descriptor_epoch_present &&
      request.expected_descriptor_epoch !=
          provider.spatial_descriptor.descriptor_epoch) {
    return OpenFailure(SpGistOpenClass::stale_descriptor_epoch,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
                       "index.spgist_physical_provider.stale_descriptor_epoch");
  }
  if (request.expected_srid_resource_epoch_present &&
      request.expected_srid_resource_epoch !=
          provider.srid_resource.resource_epoch) {
    return OpenFailure(SpGistOpenClass::stale_srid_resource_epoch,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_SRID_RESOURCE_EPOCH",
                       "index.spgist_physical_provider.stale_srid_resource_epoch");
  }
  if (request.expected_srid_present &&
      request.expected_srid != provider.srid_resource.srid) {
    return OpenFailure(SpGistOpenClass::stale_srid_resource_epoch,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_SRID",
                       "index.spgist_physical_provider.stale_srid");
  }
  if (!ProviderValid(provider)) {
    return OpenFailure(SpGistOpenClass::corrupt_payload,
                       "INDEX.SPGIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.spgist_physical_provider.corrupt_payload");
  }

  SpGistOpenResult result;
  result.status = OkStatus();
  result.open_class = SpGistOpenClass::current;
  result.provider = std::move(provider);
  result.fail_closed = false;
  result.actions.push_back("open_spgist_physical_provider");
  return result;
}

SpGistQueryResult QuerySpGistPhysicalProvider(
    const SpGistQueryRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return QueryFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
                        "index.spgist_physical_provider.missing_exact_recheck");
  }
  if (!request.opclass_epoch_current) {
    return QueryFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_OPCLASS_EPOCH",
                        "index.spgist_physical_provider.stale_opclass_epoch");
  }
  if (!request.resource_epoch_current) {
    return QueryFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_RESOURCE_EPOCH",
                        "index.spgist_physical_provider.stale_resource_epoch");
  }
  if (!request.descriptor_epoch_current) {
    return QueryFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
                        "index.spgist_physical_provider.stale_descriptor_epoch");
  }
  if (!request.srid_resource_epoch_current) {
    return QueryFailure(
        "INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_SRID_RESOURCE_EPOCH",
        "index.spgist_physical_provider.stale_srid_resource_epoch");
  }
  if (!RuntimeRequestSafe(request)) {
    return QueryFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
                        "index.spgist_physical_provider.runtime_refused");
  }

  SpGistQueryResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.partitioned_search_tree_used = true;
  result.evidence = request.provider.evidence;
  if (request.provider.nodes.empty()) {
    return result;
  }
  std::vector<u32> stack{request.provider.root_node_id};
  while (!stack.empty()) {
    const u32 node_id = stack.back();
    stack.pop_back();
    if (node_id >= request.provider.nodes.size()) {
      return QueryFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
                          "index.spgist_physical_provider.invalid_node_ref");
    }
    const auto& node = request.provider.nodes[node_id];
    ++result.nodes_visited;
    if (node.leaf) {
      for (const auto& tuple : node.leaf_tuples) {
        ++result.leaf_tuples_examined;
        if (tuple.tombstoned) continue;
        const bool consistent =
            request.opclass.methods.leaf_consistent(tuple.compressed,
                                                    request.query_mbr,
                                                    request.strategy);
        result.leaf_consistent_used = true;
        if (!consistent) continue;
        double distance = 0.0;
        if (request.strategy == SpGistPredicateStrategy::nearest) {
          distance = request.opclass.methods.distance(tuple.compressed,
                                                      request.query_point);
          if (!std::isfinite(distance)) {
            return QueryFailure(
                "INDEX.SPGIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
                "index.spgist_physical_provider.invalid_distance_key");
          }
        }
        result.candidates.push_back(MakeCandidate(tuple, distance));
      }
    } else {
      const auto partitions = request.opclass.methods.inner_consistent(
          node.prefix,
          request.query_mbr,
          request.strategy,
          node.depth + 1);
      result.inner_consistent_used = true;
      ++result.inner_tuples_examined;
      for (const auto& partition : partitions) {
        if (!SpGistPartitionKeyValid(partition, request.provider.max_depth)) {
          return QueryFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.INVALID_PARTITION_KEY",
                              "index.spgist_physical_provider.invalid_partition_key");
        }
        for (const auto& inner : node.inner_tuples) {
          if (inner.partition.depth == partition.depth &&
              inner.partition.quadrant == partition.quadrant) {
            stack.push_back(inner.child_node);
          }
        }
      }
    }
  }
  if (request.strategy == SpGistPredicateStrategy::nearest) {
    result.priority_queue_used = true;
    std::sort(result.candidates.begin(), result.candidates.end(),
              [](const auto& left, const auto& right) {
      if (left.distance != right.distance) return left.distance < right.distance;
      return LocatorLess(left.locator, right.locator);
    });
    if (request.top_k != 0 && result.candidates.size() > request.top_k) {
      result.candidates.resize(request.top_k);
    }
  } else {
    SortCandidates(&result.candidates);
  }
  return result;
}

SpGistMutationResult ApplySpGistPhysicalMutation(
    const SpGistPhysicalProvider& provider,
    const SpGistOpclassRuntime& opclass,
    const SpGistMutation& mutation) {
  if (!ProviderValid(provider) ||
      !OpclassRuntimeSafe(opclass) ||
      !SameOpclassDescriptor(provider.opclass, opclass.descriptor) ||
      !RecheckProofValid(mutation.recheck_proof)) {
    return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
                           "index.spgist_physical_provider.runtime_refused");
  }
  if ((mutation.expected_provider_generation_present &&
       mutation.expected_provider_generation != provider.provider_generation) ||
      (mutation.expected_opclass_epoch_present &&
       mutation.expected_opclass_epoch != provider.opclass.opclass_epoch) ||
      (mutation.expected_resource_epoch_present &&
       mutation.expected_resource_epoch != provider.opclass.resource_epoch) ||
      (mutation.expected_descriptor_epoch_present &&
       mutation.expected_descriptor_epoch !=
           provider.spatial_descriptor.descriptor_epoch) ||
      (mutation.expected_srid_resource_epoch_present &&
       mutation.expected_srid_resource_epoch !=
           provider.srid_resource.resource_epoch)) {
    return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.STALE_EPOCH",
                           "index.spgist_physical_provider.stale_epoch");
  }

  SpGistPhysicalProvider next = provider;
  const u64 old_splits = next.split_partition_count;
  const std::size_t old_nodes = next.nodes.size();
  bool tombstone = false;
  if (mutation.kind == SpGistMutationKind::insert_row) {
    if (!mutation.after_row_present ||
        !SourceRowValid(mutation.after_row,
                        next.spatial_descriptor,
                        next.srid_resource) ||
        FindStoredRow(next, mutation.after_row.locator) != nullptr) {
      return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.MUTATION_REFUSED",
                             "index.spgist_physical_provider.insert_refused");
    }
    const auto compressed = CompressChecked(opclass, mutation.after_row.key);
    if (!SpGistCompressedKeyValid(compressed, opclass.descriptor)) {
      return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
                             "index.spgist_physical_provider.invalid_compressed_key");
    }
    next.rows.push_back({mutation.after_row.locator,
                         mutation.after_row.key,
                         compressed,
                         mutation.after_row.exact_source_recheck_evidence_ref,
                         false});
  } else if (mutation.kind == SpGistMutationKind::delete_row) {
    if (!mutation.before_row_present) {
      return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.MUTATION_REFUSED",
                             "index.spgist_physical_provider.delete_refused");
    }
    bool found = false;
    for (auto& row : next.rows) {
      if (LocatorEqual(row.locator, mutation.before_row.locator)) {
        if (row.tombstoned || !SourceMatchesStored(mutation.before_row, row)) {
          return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.MUTATION_REFUSED",
                                 "index.spgist_physical_provider.delete_refused");
        }
        row.tombstoned = true;
        found = true;
        tombstone = true;
        break;
      }
    }
    if (!found) {
      return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.MUTATION_REFUSED",
                             "index.spgist_physical_provider.delete_missing");
    }
  } else {
    if (!mutation.before_row_present || !mutation.after_row_present) {
      return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.MUTATION_REFUSED",
                             "index.spgist_physical_provider.update_refused");
    }
    bool found = false;
    for (auto& row : next.rows) {
      if (LocatorEqual(row.locator, mutation.before_row.locator)) {
        if (row.tombstoned || !SourceMatchesStored(mutation.before_row, row)) {
          return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.MUTATION_REFUSED",
                                 "index.spgist_physical_provider.update_refused");
        }
        row.tombstoned = true;
        found = true;
        tombstone = true;
        break;
      }
    }
    if (!found ||
        !SourceRowValid(mutation.after_row,
                        next.spatial_descriptor,
                        next.srid_resource) ||
        (!LocatorEqual(mutation.before_row.locator, mutation.after_row.locator) &&
         FindStoredRow(next, mutation.after_row.locator) != nullptr)) {
      return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.MUTATION_REFUSED",
                             "index.spgist_physical_provider.update_refused");
    }
    const auto compressed = CompressChecked(opclass, mutation.after_row.key);
    if (!SpGistCompressedKeyValid(compressed, opclass.descriptor)) {
      return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
                             "index.spgist_physical_provider.invalid_compressed_key");
    }
    next.rows.push_back({mutation.after_row.locator,
                         mutation.after_row.key,
                         compressed,
                         mutation.after_row.exact_source_recheck_evidence_ref,
                         false});
  }
  std::sort(next.rows.begin(), next.rows.end(), [](const auto& left,
                                                   const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
  if (!InstallTree(&next, opclass)) {
    return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.BUILD_CORRUPT",
                           "index.spgist_physical_provider.build_corrupt");
  }
  next.provider_generation += 1;
  SetProviderEvidence(&next);
  if (!ProviderValid(next)) {
    return MutationFailure("INDEX.SPGIST_PHYSICAL_PROVIDER.BUILD_CORRUPT",
                           "index.spgist_physical_provider.build_corrupt");
  }
  SpGistMutationResult result;
  result.status = OkStatus();
  result.provider = std::move(next);
  result.applied = true;
  result.fail_closed = false;
  result.split_partition_performed =
      result.provider.split_partition_count > old_splits;
  result.merge_performed = result.provider.nodes.size() < old_nodes;
  result.tombstone_written = tombstone;
  result.actions.push_back("apply_spgist_physical_mutation");
  return result;
}

const char* SpGistOpenClassName(SpGistOpenClass open_class) {
  switch (open_class) {
    case SpGistOpenClass::current: return "current";
    case SpGistOpenClass::stale_format: return "stale_format";
    case SpGistOpenClass::stale_generation: return "stale_generation";
    case SpGistOpenClass::bad_checksum: return "bad_checksum";
    case SpGistOpenClass::corrupt_payload: return "corrupt_payload";
    case SpGistOpenClass::identity_mismatch: return "identity_mismatch";
    case SpGistOpenClass::stale_opclass_epoch: return "stale_opclass_epoch";
    case SpGistOpenClass::stale_resource_epoch: return "stale_resource_epoch";
    case SpGistOpenClass::stale_descriptor_epoch:
      return "stale_descriptor_epoch";
    case SpGistOpenClass::stale_srid_resource_epoch:
      return "stale_srid_resource_epoch";
    case SpGistOpenClass::unsafe_opclass: return "unsafe_opclass";
    case SpGistOpenClass::invalid_compressed_key:
      return "invalid_compressed_key";
    case SpGistOpenClass::invalid_partition_key:
      return "invalid_partition_key";
    case SpGistOpenClass::incompatible_spatial_profile:
      return "incompatible_spatial_profile";
    case SpGistOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case SpGistOpenClass::missing_recheck_proof:
      return "missing_recheck_proof";
    case SpGistOpenClass::refused: return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeSpGistPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  arguments.push_back({"artifact_kind", kSpGistPhysicalProviderArtifactKind});
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
                        "core.index.spgist_physical_provider");
}

}  // namespace scratchbird::core::index
