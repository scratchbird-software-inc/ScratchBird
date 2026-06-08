// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "spatial_rtree_physical_provider.hpp"

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

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'R', 'T',
                                               'R', 'E', '0', '1'};
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

bool RecheckAuthorityClean(const SpatialRTreeRecheckProof& proof) {
  return !proof.parser_finality_authority_claimed &&
         !proof.donor_finality_authority_claimed &&
         !proof.provider_finality_authority_claimed &&
         !proof.index_finality_authority_claimed &&
         !proof.write_ahead_log_finality_authority_claimed &&
         !proof.visibility_authority_claimed &&
         !proof.security_authority_claimed &&
         !proof.transaction_finality_authority_claimed;
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

bool RecheckProofValid(const SpatialRTreeRecheckProof& proof) {
  return proof.proof_supplied &&
         proof.exact_source_geometry_available &&
         proof.exact_predicate_recheck_required &&
         proof.mga_recheck_required &&
         proof.security_recheck_required &&
         !proof.evidence_ref.empty() &&
         RecheckAuthorityClean(proof);
}

bool SupportedCoordinateOrder(const SpatialRTreeSridResource& resource,
                              const SpatialRTreeDescriptor& descriptor) {
  if (descriptor.dimensions == 2) {
    return resource.coordinate_order == "xy";
  }
  if (descriptor.dimensions == 3) {
    return resource.coordinate_order == "xyz" ||
           resource.coordinate_order == "xym";
  }
  if (descriptor.dimensions == 4) {
    return resource.coordinate_order == "xyzm";
  }
  return false;
}

bool DescriptorSafe(const SpatialRTreeDescriptor& descriptor) {
  return descriptor.dimensions >= 2 &&
         descriptor.dimensions <= 4 &&
         descriptor.descriptor_epoch > 0 &&
         descriptor.deterministic &&
         descriptor.descriptor_safe &&
         descriptor.supports_mbr &&
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
         SupportedCoordinateOrder(resource, descriptor) &&
         !resource.descriptor_store_scan &&
         !resource.behavior_store_scan &&
         !resource.contract_only_fallback &&
         !resource.provider_only_fallback &&
         SridAuthorityClean(resource);
}

bool ProviderAuthorityClean(const SpatialRTreePhysicalProvider& provider) {
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

bool NodeEntryIsRow(const SpatialRTreeNodeEntry& entry) {
  return !entry.child && LocatorValid(entry.locator);
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

double Enlargement(const SpatialRTreeMbr& current,
                   const SpatialRTreeMbr& added) {
  return Area(UnionMbr(current, added)) - Area(current);
}

double CenterAxis(const SpatialRTreeMbr& mbr, std::size_t axis) {
  return (mbr.min[axis] + mbr.max[axis]) * 0.5;
}

bool MbrEqual(const SpatialRTreeMbr& left, const SpatialRTreeMbr& right) {
  return left.dimensions == right.dimensions &&
         left.srid == right.srid &&
         left.min == right.min &&
         left.max == right.max;
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

SpatialRTreeMbr CoverEntries(const std::vector<SpatialRTreeNodeEntry>& entries,
                             u32 dimensions,
                             u32 srid) {
  SpatialRTreeMbr cover;
  cover.dimensions = dimensions;
  cover.srid = srid;
  cover.min.assign(dimensions, 0.0);
  cover.max.assign(dimensions, 0.0);
  if (entries.empty()) {
    return cover;
  }
  cover = entries.front().mbr;
  for (std::size_t i = 1; i < entries.size(); ++i) {
    cover = UnionMbr(cover, entries[i].mbr);
  }
  return cover;
}

void RefreshNodeCover(SpatialRTreeNode* node, u32 dimensions, u32 srid) {
  node->cover = CoverEntries(node->entries, dimensions, srid);
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
  static_assert(sizeof(double) == sizeof(u64),
                "ScratchBird spatial serialization requires 64-bit double");
  u64 bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU64(out, bits);
}
void AppendString(std::vector<byte>* out, const std::string& value) {
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
  for (double value : mbr.min) {
    AppendF64(out, value);
  }
  for (double value : mbr.max) {
    AppendF64(out, value);
  }
}

u64 ComputeChecksum(std::vector<byte> bytes) {
  if (bytes.size() >= kHeaderBytes) {
    std::fill(bytes.begin() + 16, bytes.begin() + 24, 0);
  }
  u64 hash = kFnvOffset;
  for (byte value : bytes) {
    hash ^= value;
    hash *= kFnvPrime;
  }
  return hash;
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
  bool ReadLocator(TextInvertedRowLocator* locator) {
    return ReadU64(&locator->row_ordinal) &&
           ReadString(&locator->row_uuid) &&
           ReadString(&locator->version_uuid);
  }
  bool ReadMbr(SpatialRTreeMbr* mbr) {
    if (!ReadU32(&mbr->dimensions) ||
        !ReadU32(&mbr->srid) ||
        mbr->dimensions < 2 ||
        mbr->dimensions > 4) {
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

 private:
  const std::vector<byte>& bytes_;
  std::size_t offset_ = 0;
};

SpatialRTreeBuildResult BuildFailure(std::string code,
                                     std::string key,
                                     std::string detail = {}) {
  SpatialRTreeBuildResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeSpatialRTreePhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

SpatialRTreeOpenResult OpenFailure(SpatialRTreeOpenClass open_class,
                                   std::string code,
                                   std::string key,
                                   std::string detail = {}) {
  SpatialRTreeOpenResult result;
  result.status = ErrorStatus();
  result.open_class = open_class;
  result.fail_closed = true;
  result.restricted_repair_required =
      open_class == SpatialRTreeOpenClass::bad_checksum ||
      open_class == SpatialRTreeOpenClass::corrupt_payload;
  result.diagnostic = MakeSpatialRTreePhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

SpatialRTreeQueryResult QueryFailure(std::string code,
                                     std::string key,
                                     std::string detail = {}) {
  SpatialRTreeQueryResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeSpatialRTreePhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

SpatialRTreeMutationResult MutationFailure(std::string code,
                                           std::string key,
                                           std::string detail = {}) {
  SpatialRTreeMutationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeSpatialRTreePhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

SpatialRTreeNodeEntry RowEntry(const SpatialRTreeStoredRow& row) {
  SpatialRTreeNodeEntry entry;
  entry.mbr = row.mbr;
  entry.locator = row.locator;
  entry.tombstoned = row.tombstoned;
  return entry;
}

SpatialRTreeNodeEntry ChildEntry(const SpatialRTreeNode& node) {
  SpatialRTreeNodeEntry entry;
  entry.mbr = node.cover;
  entry.child = true;
  entry.child_node = node.node_id;
  return entry;
}

struct TreeBuildState {
  u32 dimensions = 0;
  u32 srid = 0;
  u32 max_entries = kSpatialRTreeDefaultMaxEntries;
  u32 min_entries = kSpatialRTreeDefaultMinEntries;
  std::vector<SpatialRTreeNode> nodes;
  u32 root_node_id = 0;
  u64 split_count = 0;
};

u32 NewNode(TreeBuildState* state, bool leaf, u32 level) {
  SpatialRTreeNode node;
  node.node_id = static_cast<u32>(state->nodes.size());
  node.leaf = leaf;
  node.level = level;
  node.cover.dimensions = state->dimensions;
  node.cover.srid = state->srid;
  node.cover.min.assign(state->dimensions, 0.0);
  node.cover.max.assign(state->dimensions, 0.0);
  state->nodes.push_back(std::move(node));
  return static_cast<u32>(state->nodes.size() - 1);
}

std::pair<std::vector<SpatialRTreeNodeEntry>, std::vector<SpatialRTreeNodeEntry>>
SplitEntries(std::vector<SpatialRTreeNodeEntry> entries,
             u32 dimensions,
             u32 srid,
             u32 min_entries) {
  std::size_t seed_a = 0;
  std::size_t seed_b = entries.size() > 1 ? 1 : 0;
  double worst_waste = -1.0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    for (std::size_t j = i + 1; j < entries.size(); ++j) {
      const double waste = Area(UnionMbr(entries[i].mbr, entries[j].mbr)) -
                           Area(entries[i].mbr) - Area(entries[j].mbr);
      if (waste > worst_waste) {
        worst_waste = waste;
        seed_a = i;
        seed_b = j;
      }
    }
  }

  std::vector<SpatialRTreeNodeEntry> left;
  std::vector<SpatialRTreeNodeEntry> right;
  left.push_back(entries[seed_a]);
  right.push_back(entries[seed_b]);
  std::vector<bool> used(entries.size(), false);
  used[seed_a] = true;
  used[seed_b] = true;

  std::size_t remaining = entries.size() - 2;
  while (remaining > 0) {
    if (left.size() + remaining == min_entries) {
      for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!used[i]) {
          left.push_back(entries[i]);
          used[i] = true;
          --remaining;
        }
      }
      break;
    }
    if (right.size() + remaining == min_entries) {
      for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!used[i]) {
          right.push_back(entries[i]);
          used[i] = true;
          --remaining;
        }
      }
      break;
    }

    const auto left_cover = CoverEntries(left, dimensions, srid);
    const auto right_cover = CoverEntries(right, dimensions, srid);
    std::size_t best = entries.size();
    double best_preference = -1.0;
    bool choose_left = true;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      if (used[i]) continue;
      const double left_growth = Enlargement(left_cover, entries[i].mbr);
      const double right_growth = Enlargement(right_cover, entries[i].mbr);
      const double preference = std::fabs(left_growth - right_growth);
      if (preference > best_preference) {
        best_preference = preference;
        best = i;
        if (left_growth != right_growth) {
          choose_left = left_growth < right_growth;
        } else if (Area(left_cover) != Area(right_cover)) {
          choose_left = Area(left_cover) < Area(right_cover);
        } else {
          choose_left = left.size() <= right.size();
        }
      }
    }
    if (best == entries.size()) break;
    (choose_left ? left : right).push_back(entries[best]);
    used[best] = true;
    --remaining;
  }
  return {std::move(left), std::move(right)};
}

struct InsertResult {
  bool split = false;
  u32 new_node_id = 0;
};

InsertResult InsertIntoNode(TreeBuildState* state,
                            u32 node_id,
                            const SpatialRTreeNodeEntry& entry) {
  if (state->nodes[node_id].leaf) {
    state->nodes[node_id].entries.push_back(entry);
  } else {
    std::size_t best_child = 0;
    double best_growth = std::numeric_limits<double>::infinity();
    double best_area = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < state->nodes[node_id].entries.size(); ++i) {
      const double growth =
          Enlargement(state->nodes[node_id].entries[i].mbr, entry.mbr);
      const double area = Area(state->nodes[node_id].entries[i].mbr);
      if (growth < best_growth ||
          (growth == best_growth && area < best_area)) {
        best_growth = growth;
        best_area = area;
        best_child = i;
      }
    }
    const u32 child_id = state->nodes[node_id].entries[best_child].child_node;
    const auto inserted = InsertIntoNode(state, child_id, entry);
    state->nodes[node_id].entries[best_child].mbr =
        state->nodes[child_id].cover;
    if (inserted.split) {
      state->nodes[node_id].entries.push_back(
          ChildEntry(state->nodes[inserted.new_node_id]));
    }
  }

  RefreshNodeCover(&state->nodes[node_id], state->dimensions, state->srid);
  if (state->nodes[node_id].entries.size() <= state->max_entries) {
    return {};
  }

  const auto split = SplitEntries(state->nodes[node_id].entries,
                                  state->dimensions,
                                  state->srid,
                                  state->min_entries);
  state->nodes[node_id].entries = split.first;
  RefreshNodeCover(&state->nodes[node_id], state->dimensions, state->srid);
  const u32 new_node_id =
      NewNode(state, state->nodes[node_id].leaf, state->nodes[node_id].level);
  state->nodes[new_node_id].entries = split.second;
  RefreshNodeCover(&state->nodes[new_node_id], state->dimensions, state->srid);
  ++state->split_count;
  return {true, new_node_id};
}

TreeBuildState BuildByInsertion(std::vector<SpatialRTreeStoredRow> rows,
                                u32 dimensions,
                                u32 srid,
                                u32 max_entries,
                                u32 min_entries) {
  TreeBuildState state;
  state.dimensions = dimensions;
  state.srid = srid;
  state.max_entries = max_entries;
  state.min_entries = min_entries;
  const u32 root = NewNode(&state, true, 1);
  state.root_node_id = root;
  for (const auto& row : rows) {
    if (row.tombstoned) continue;
    const auto inserted = InsertIntoNode(&state, state.root_node_id, RowEntry(row));
    if (inserted.split) {
      const u32 old_root = state.root_node_id;
      const u32 new_root = NewNode(&state, false, state.nodes[old_root].level + 1);
      state.nodes[new_root].entries.push_back(ChildEntry(state.nodes[old_root]));
      state.nodes[new_root].entries.push_back(
          ChildEntry(state.nodes[inserted.new_node_id]));
      RefreshNodeCover(&state.nodes[new_root], dimensions, srid);
      state.root_node_id = new_root;
    }
  }
  if (state.nodes[root].entries.empty() && state.nodes.size() == 1) {
    state.nodes.clear();
    state.root_node_id = 0;
  }
  return state;
}

void SortSpatialEntries(std::vector<SpatialRTreeNodeEntry>* entries) {
  std::sort(entries->begin(), entries->end(), [](const auto& left,
                                                 const auto& right) {
    const double lx = CenterAxis(left.mbr, 0);
    const double rx = CenterAxis(right.mbr, 0);
    if (lx != rx) return lx < rx;
    const double ly = CenterAxis(left.mbr, 1);
    const double ry = CenterAxis(right.mbr, 1);
    if (ly != ry) return ly < ry;
    return LocatorLess(left.locator, right.locator);
  });
}

TreeBuildState BuildByStrBulk(std::vector<SpatialRTreeStoredRow> rows,
                              u32 dimensions,
                              u32 srid,
                              u32 max_entries,
                              u32 min_entries) {
  TreeBuildState state;
  state.dimensions = dimensions;
  state.srid = srid;
  state.max_entries = max_entries;
  state.min_entries = min_entries;
  std::vector<SpatialRTreeNodeEntry> entries;
  for (const auto& row : rows) {
    if (!row.tombstoned) {
      entries.push_back(RowEntry(row));
    }
  }
  if (entries.empty()) {
    return state;
  }
  SortSpatialEntries(&entries);

  std::vector<u32> current_level;
  for (std::size_t i = 0; i < entries.size(); i += max_entries) {
    const u32 node_id = NewNode(&state, true, 1);
    const auto end = std::min(entries.size(), i + max_entries);
    state.nodes[node_id].entries.assign(entries.begin() + i,
                                        entries.begin() + end);
    RefreshNodeCover(&state.nodes[node_id], dimensions, srid);
    current_level.push_back(node_id);
  }

  u32 level = 1;
  while (current_level.size() > 1) {
    std::vector<SpatialRTreeNodeEntry> child_entries;
    child_entries.reserve(current_level.size());
    for (u32 node_id : current_level) {
      child_entries.push_back(ChildEntry(state.nodes[node_id]));
    }
    SortSpatialEntries(&child_entries);
    std::vector<u32> next_level;
    ++level;
    for (std::size_t i = 0; i < child_entries.size(); i += max_entries) {
      const u32 node_id = NewNode(&state, false, level);
      const auto end = std::min(child_entries.size(), i + max_entries);
      state.nodes[node_id].entries.assign(child_entries.begin() + i,
                                          child_entries.begin() + end);
      RefreshNodeCover(&state.nodes[node_id], dimensions, srid);
      next_level.push_back(node_id);
    }
    current_level = std::move(next_level);
  }
  state.root_node_id = current_level.front();
  return state;
}

bool StoredRowValid(const SpatialRTreeStoredRow& row,
                    const SpatialRTreeDescriptor& descriptor,
                    const SpatialRTreeSridResource& resource) {
  return LocatorValid(row.locator) &&
         SpatialRTreeMbrValid(row.mbr, descriptor, resource) &&
         !row.exact_source_recheck_evidence_ref.empty();
}

const SpatialRTreeStoredRow* FindStoredRow(
    const SpatialRTreePhysicalProvider& provider,
    const TextInvertedRowLocator& locator);

bool NodeValid(const SpatialRTreePhysicalProvider& provider,
               const SpatialRTreeNode& node) {
  if (node.node_id >= provider.nodes.size() ||
      provider.nodes[node.node_id].node_id != node.node_id ||
      node.entries.size() > provider.max_entries ||
      (node.entries.empty() && !provider.rows.empty())) {
    return false;
  }
  for (const auto& entry : node.entries) {
    if (!SpatialRTreeMbrValid(entry.mbr,
                              provider.descriptor,
                              provider.srid_resource)) {
      return false;
    }
    if (node.leaf) {
      if (!NodeEntryIsRow(entry)) return false;
    } else if (!entry.child || entry.child_node >= provider.nodes.size()) {
      return false;
    }
  }
  return MbrEqual(node.cover,
                  CoverEntries(node.entries,
                               provider.descriptor.dimensions,
                               provider.srid_resource.srid));
}

bool TreeMatchesRows(const SpatialRTreePhysicalProvider& provider) {
  if (provider.nodes.empty() ||
      provider.root_node_id >= provider.nodes.size() ||
      provider.nodes[provider.root_node_id].level != provider.tree_height) {
    return false;
  }

  std::vector<bool> visited(provider.nodes.size(), false);
  std::vector<u32> stack{provider.root_node_id};
  std::vector<TextInvertedRowLocator> tree_rows;
  while (!stack.empty()) {
    const u32 node_id = stack.back();
    stack.pop_back();
    if (node_id >= provider.nodes.size() || visited[node_id]) {
      return false;
    }
    visited[node_id] = true;
    const auto& node = provider.nodes[node_id];
    if (node.leaf && node.level != 1) {
      return false;
    }
    for (const auto& entry : node.entries) {
      if (node.leaf) {
        if (entry.child || entry.tombstoned || !LocatorValid(entry.locator)) {
          return false;
        }
        const auto* row = FindStoredRow(provider, entry.locator);
        if (row == nullptr || row->tombstoned ||
            !MbrEqual(row->mbr, entry.mbr)) {
          return false;
        }
        tree_rows.push_back(entry.locator);
      } else {
        if (!entry.child || entry.child_node >= provider.nodes.size()) {
          return false;
        }
        const auto& child = provider.nodes[entry.child_node];
        if (child.level + 1 != node.level ||
            !MbrEqual(entry.mbr, child.cover)) {
          return false;
        }
        stack.push_back(entry.child_node);
      }
    }
  }

  if (std::any_of(visited.begin(), visited.end(), [](bool seen) {
        return !seen;
      })) {
    return false;
  }

  std::sort(tree_rows.begin(), tree_rows.end(), LocatorLess);
  for (std::size_t i = 1; i < tree_rows.size(); ++i) {
    if (LocatorEqual(tree_rows[i - 1], tree_rows[i])) {
      return false;
    }
  }
  std::vector<TextInvertedRowLocator> active_rows;
  for (const auto& row : provider.rows) {
    if (!row.tombstoned) {
      active_rows.push_back(row.locator);
    }
  }
  std::sort(active_rows.begin(), active_rows.end(), LocatorLess);
  if (tree_rows.size() != active_rows.size()) {
    return false;
  }
  for (std::size_t i = 0; i < tree_rows.size(); ++i) {
    if (!LocatorEqual(tree_rows[i], active_rows[i])) {
      return false;
    }
  }
  return true;
}

bool ProviderValid(const SpatialRTreePhysicalProvider& provider) {
  if (provider.artifact_kind != kSpatialRTreePhysicalProviderArtifactKind ||
      !SameFormatVersion(provider.format_version,
                         {kSpatialRTreePhysicalProviderCurrentMajor,
                          kSpatialRTreePhysicalProviderCurrentMinor}) ||
      !PageExtentSummaryUuidTextValid(provider.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.index_uuid) ||
      !PageExtentSummaryUuidTextValid(provider.provider_uuid) ||
      provider.base_generation == 0 ||
      provider.provider_generation == 0 ||
      provider.max_entries < 3 ||
      provider.min_entries == 0 ||
      provider.min_entries >= provider.max_entries ||
      !DescriptorSafe(provider.descriptor) ||
      !SridResourceSafe(provider.srid_resource, provider.descriptor) ||
      !provider.mbr_encoding_present ||
      !provider.insert_search_split_merge_present ||
      !provider.nearest_neighbor_priority_queue_present ||
      !provider.srid_epoch_cache_present ||
      !provider.str_bulk_build_present ||
      !ProviderAuthorityClean(provider)) {
    return false;
  }
  for (std::size_t i = 0; i < provider.rows.size(); ++i) {
    if (!StoredRowValid(provider.rows[i],
                        provider.descriptor,
                        provider.srid_resource) ||
        (i > 0 &&
         !LocatorLess(provider.rows[i - 1].locator, provider.rows[i].locator))) {
      return false;
    }
  }
  const bool has_active = std::any_of(provider.rows.begin(),
                                     provider.rows.end(),
                                     [](const auto& row) {
                                       return !row.tombstoned;
                                     });
  if (!has_active) {
    return provider.nodes.empty() && provider.tree_height == 0;
  }
  if (provider.nodes.empty() ||
      provider.root_node_id >= provider.nodes.size() ||
      provider.tree_height == 0) {
    return false;
  }
  for (const auto& node : provider.nodes) {
    if (!NodeValid(provider, node)) {
      return false;
    }
  }
  return TreeMatchesRows(provider);
}

void SetProviderEvidence(SpatialRTreePhysicalProvider* provider) {
  provider->evidence = {
      kSpatialRTreePhysicalProviderSearchKey,
      "spatial_rtree_mbr_encoding",
      "spatial_rtree_insert_search_split_merge",
      "spatial_rtree_nearest_priority_queue",
      "spatial_rtree_srid_epoch_cache",
      "spatial_rtree_str_bulk_build",
      "spatial_rtree_candidate_locator_evidence"};
}

bool SourceRowsValid(const SpatialRTreeBuildRequest& request) {
  for (const auto& row : request.rows) {
    if (!LocatorValid(row.locator) ||
        !SpatialRTreeMbrValid(row.mbr,
                              request.descriptor,
                              request.srid_resource) ||
        row.exact_source_recheck_evidence_ref.empty()) {
      return false;
    }
  }
  return true;
}

SpatialRTreeStoredRow StoredFromSource(const SpatialRTreeSourceRow& source) {
  SpatialRTreeStoredRow row;
  row.locator = source.locator;
  row.mbr = source.mbr;
  row.exact_source_recheck_evidence_ref =
      source.exact_source_recheck_evidence_ref;
  return row;
}

void InstallTree(SpatialRTreePhysicalProvider* provider, TreeBuildState state) {
  provider->nodes = std::move(state.nodes);
  provider->root_node_id = provider->nodes.empty() ? 0 : state.root_node_id;
  provider->tree_height =
      provider->nodes.empty() ? 0 : provider->nodes[provider->root_node_id].level;
  provider->split_count = state.split_count;
}

bool QueryMbrCompatible(const SpatialRTreeQueryRequest& request) {
  if (request.kind == SpatialRTreeQueryKind::nearest) {
    return request.query_point.size() == request.provider.descriptor.dimensions &&
           std::all_of(request.query_point.begin(),
                       request.query_point.end(),
                       [](double value) { return std::isfinite(value); });
  }
  return SpatialRTreeMbrValid(request.query_mbr,
                              request.provider.descriptor,
                              request.provider.srid_resource);
}

bool RuntimeRequestSafe(const SpatialRTreeQueryRequest& request) {
  return ProviderValid(request.provider) &&
         RecheckProofValid(request.recheck_proof) &&
         request.descriptor_epoch_current &&
         request.srid_resource_epoch_current &&
         !request.descriptor_store_scan &&
         !request.behavior_store_scan &&
         !request.contract_only_fallback &&
         !request.provider_only_fallback &&
         QueryMbrCompatible(request);
}

bool PredicateMatches(SpatialRTreeQueryKind kind,
                      const SpatialRTreeMbr& row_mbr,
                      const SpatialRTreeMbr& query_mbr) {
  switch (kind) {
    case SpatialRTreeQueryKind::point:
    case SpatialRTreeQueryKind::contains:
      return Contains(row_mbr, query_mbr);
    case SpatialRTreeQueryKind::within:
      return Contains(query_mbr, row_mbr);
    case SpatialRTreeQueryKind::intersects:
    case SpatialRTreeQueryKind::range:
      return Intersects(row_mbr, query_mbr);
    case SpatialRTreeQueryKind::nearest:
      return true;
  }
  return false;
}

const SpatialRTreeStoredRow* FindStoredRow(
    const SpatialRTreePhysicalProvider& provider,
    const TextInvertedRowLocator& locator) {
  auto iter = std::lower_bound(
      provider.rows.begin(), provider.rows.end(), locator,
      [](const SpatialRTreeStoredRow& row, const TextInvertedRowLocator& value) {
        return LocatorLess(row.locator, value);
      });
  if (iter == provider.rows.end() || !LocatorEqual(iter->locator, locator)) {
    return nullptr;
  }
  return &*iter;
}

bool SourceMatchesStored(const SpatialRTreeSourceRow& source,
                         const SpatialRTreeStoredRow& stored) {
  return LocatorEqual(source.locator, stored.locator) &&
         MbrEqual(source.mbr, stored.mbr) &&
         source.exact_source_recheck_evidence_ref ==
             stored.exact_source_recheck_evidence_ref;
}

SpatialRTreeCandidate MakeCandidate(const SpatialRTreeStoredRow& row,
                                    double distance) {
  SpatialRTreeCandidate candidate;
  candidate.locator = row.locator;
  candidate.mbr = row.mbr;
  candidate.distance = distance;
  candidate.source_recheck_evidence_ref =
      row.exact_source_recheck_evidence_ref;
  return candidate;
}

void SortCandidates(std::vector<SpatialRTreeCandidate>* candidates) {
  std::sort(candidates->begin(), candidates->end(), [](const auto& left,
                                                       const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
}

}  // namespace

bool SpatialRTreeMbrValid(const SpatialRTreeMbr& mbr,
                          const SpatialRTreeDescriptor& descriptor,
                          const SpatialRTreeSridResource& srid_resource) {
  if (!DescriptorSafe(descriptor) ||
      !SridResourceSafe(srid_resource, descriptor) ||
      mbr.dimensions != descriptor.dimensions ||
      mbr.srid != srid_resource.srid ||
      mbr.min.size() != descriptor.dimensions ||
      mbr.max.size() != descriptor.dimensions) {
    return false;
  }
  for (std::size_t i = 0; i < mbr.min.size(); ++i) {
    if (!std::isfinite(mbr.min[i]) ||
        !std::isfinite(mbr.max[i]) ||
        mbr.min[i] > mbr.max[i]) {
      return false;
    }
  }
  return true;
}

std::vector<byte> EncodeSpatialRTreeMbr(const SpatialRTreeMbr& mbr) {
  if (mbr.dimensions < 2 ||
      mbr.dimensions > 4 ||
      mbr.min.size() != mbr.dimensions ||
      mbr.max.size() != mbr.dimensions) {
    return {};
  }
  for (std::size_t i = 0; i < mbr.min.size(); ++i) {
    if (!std::isfinite(mbr.min[i]) ||
        !std::isfinite(mbr.max[i]) ||
        mbr.min[i] > mbr.max[i]) {
      return {};
    }
  }
  std::vector<byte> out;
  AppendMbr(&out, mbr);
  return out;
}

SpatialRTreeMbr DecodeSpatialRTreeMbr(const std::vector<byte>& encoded,
                                      u32 expected_dimensions,
                                      u32 expected_srid) {
  Reader reader(encoded);
  SpatialRTreeMbr mbr;
  if (!reader.ReadMbr(&mbr) ||
      !reader.Done() ||
      mbr.dimensions != expected_dimensions ||
      mbr.srid != expected_srid) {
    return {};
  }
  return mbr;
}

SpatialRTreeBuildResult BuildSpatialRTreePhysicalProvider(
    const SpatialRTreeBuildRequest& request) {
  if (!RecheckAuthorityClean(request.recheck_proof) ||
      !DescriptorAuthorityClean(request.descriptor) ||
      !SridAuthorityClean(request.srid_resource)) {
    return BuildFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED",
        "index.spatial_rtree_physical_provider.authority_claim_refused");
  }
  if (!RecheckProofValid(request.recheck_proof)) {
    return BuildFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.spatial_rtree_physical_provider.missing_exact_recheck");
  }
  if (!DescriptorSafe(request.descriptor)) {
    return BuildFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.UNSUPPORTED_GEOMETRY_PROFILE",
        "index.spatial_rtree_physical_provider.unsupported_geometry_profile");
  }
  if (!SridResourceSafe(request.srid_resource, request.descriptor)) {
    return BuildFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.UNSAFE_SRID_RESOURCE",
        "index.spatial_rtree_physical_provider.unsafe_srid_resource");
  }
  if (request.max_entries < 3 ||
      request.min_entries == 0 ||
      request.min_entries >= request.max_entries ||
      request.base_generation == 0 ||
      request.provider_generation == 0 ||
      !PageExtentSummaryUuidTextValid(request.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(request.index_uuid) ||
      !PageExtentSummaryUuidTextValid(request.provider_uuid)) {
    return BuildFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.BUILD_REFUSED",
        "index.spatial_rtree_physical_provider.build_refused");
  }
  if (!SourceRowsValid(request)) {
    return BuildFailure("INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.INVALID_MBR",
                        "index.spatial_rtree_physical_provider.invalid_mbr");
  }

  std::vector<SpatialRTreeStoredRow> rows;
  rows.reserve(request.rows.size());
  for (const auto& row : request.rows) {
    rows.push_back(StoredFromSource(row));
  }
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return LocatorLess(left.locator, right.locator);
  });
  for (std::size_t i = 1; i < rows.size(); ++i) {
    if (LocatorEqual(rows[i - 1].locator, rows[i].locator)) {
      return BuildFailure(
          "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.DUPLICATE_ROW_LOCATOR",
          "index.spatial_rtree_physical_provider.duplicate_row_locator");
    }
  }

  SpatialRTreePhysicalProvider provider;
  provider.relation_uuid = request.relation_uuid;
  provider.index_uuid = request.index_uuid;
  provider.provider_uuid = request.provider_uuid;
  provider.base_generation = request.base_generation;
  provider.provider_generation = request.provider_generation;
  provider.descriptor = request.descriptor;
  provider.srid_resource = request.srid_resource;
  provider.max_entries = request.max_entries;
  provider.min_entries = request.min_entries;
  provider.rows = rows;
  SetProviderEvidence(&provider);
  if (request.build_mode == SpatialRTreeBuildMode::str_bulk) {
    InstallTree(&provider,
                BuildByStrBulk(rows,
                               request.descriptor.dimensions,
                               request.srid_resource.srid,
                               request.max_entries,
                               request.min_entries));
  } else {
    InstallTree(&provider,
                BuildByInsertion(rows,
                                 request.descriptor.dimensions,
                                 request.srid_resource.srid,
                                 request.max_entries,
                                 request.min_entries));
  }
  if (!ProviderValid(provider)) {
    return BuildFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.BUILD_CORRUPT",
        "index.spatial_rtree_physical_provider.build_corrupt");
  }

  SpatialRTreeBuildResult result;
  result.status = OkStatus();
  result.provider = std::move(provider);
  result.built = true;
  result.fail_closed = false;
  result.used_str_bulk_build =
      request.build_mode == SpatialRTreeBuildMode::str_bulk;
  return result;
}

SpatialRTreeSerializeResult SerializeSpatialRTreePhysicalProvider(
    const SpatialRTreePhysicalProvider& provider) {
  SpatialRTreeSerializeResult result;
  if (!ProviderValid(provider)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeSpatialRTreePhysicalProviderDiagnostic(
        result.status,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.SERIALIZE_REFUSED",
        "index.spatial_rtree_physical_provider.serialize_refused");
    return result;
  }
  std::vector<byte> bytes;
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  AppendU32(&bytes, kSpatialRTreePhysicalProviderCurrentMajor);
  AppendU32(&bytes, kSpatialRTreePhysicalProviderCurrentMinor);
  AppendU64(&bytes, 0);
  AppendString(&bytes, provider.relation_uuid);
  AppendString(&bytes, provider.index_uuid);
  AppendString(&bytes, provider.provider_uuid);
  AppendU64(&bytes, provider.base_generation);
  AppendU64(&bytes, provider.provider_generation);
  AppendU32(&bytes, provider.descriptor.dimensions);
  AppendU64(&bytes, provider.descriptor.descriptor_epoch);
  AppendBool(&bytes, provider.descriptor.deterministic);
  AppendBool(&bytes, provider.descriptor.descriptor_safe);
  AppendBool(&bytes, provider.descriptor.supports_point);
  AppendBool(&bytes, provider.descriptor.supports_mbr);
  AppendBool(&bytes, provider.descriptor.supports_z);
  AppendBool(&bytes, provider.descriptor.supports_m);
  AppendString(&bytes, provider.srid_resource.resource_uuid);
  AppendU32(&bytes, provider.srid_resource.srid);
  AppendU64(&bytes, provider.srid_resource.resource_epoch);
  AppendString(&bytes, provider.srid_resource.coordinate_order);
  AppendBool(&bytes, provider.srid_resource.deterministic);
  AppendBool(&bytes, provider.srid_resource.safe);
  AppendBool(&bytes, provider.srid_resource.cache_present);
  AppendU32(&bytes, provider.max_entries);
  AppendU32(&bytes, provider.min_entries);
  AppendU32(&bytes, provider.root_node_id);
  AppendU32(&bytes, provider.tree_height);
  AppendU64(&bytes, provider.split_count);
  AppendU64(&bytes, provider.merge_count);
  AppendU64(&bytes, static_cast<u64>(provider.rows.size()));
  for (const auto& row : provider.rows) {
    AppendLocator(&bytes, row.locator);
    AppendMbr(&bytes, row.mbr);
    AppendString(&bytes, row.exact_source_recheck_evidence_ref);
    AppendBool(&bytes, row.tombstoned);
  }
  AppendU64(&bytes, static_cast<u64>(provider.nodes.size()));
  for (const auto& node : provider.nodes) {
    AppendU32(&bytes, node.node_id);
    AppendBool(&bytes, node.leaf);
    AppendU32(&bytes, node.level);
    AppendMbr(&bytes, node.cover);
    AppendU64(&bytes, static_cast<u64>(node.entries.size()));
    for (const auto& entry : node.entries) {
      AppendMbr(&bytes, entry.mbr);
      AppendBool(&bytes, entry.child);
      AppendU32(&bytes, entry.child_node);
      AppendLocator(&bytes, entry.locator);
      AppendBool(&bytes, entry.tombstoned);
    }
  }
  result.checksum = ComputeChecksum(bytes);
  StoreLittle64(bytes.data() + 16, result.checksum);
  result.status = OkStatus();
  result.bytes = std::move(bytes);
  return result;
}

SpatialRTreeOpenResult OpenSpatialRTreePhysicalProvider(
    const SpatialRTreeOpenRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return OpenFailure(
        SpatialRTreeOpenClass::missing_exact_recheck_proof,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.spatial_rtree_physical_provider.missing_exact_recheck");
  }
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(
        SpatialRTreeOpenClass::corrupt_payload,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
        "index.spatial_rtree_physical_provider.corrupt_payload");
  }
  const u64 stored_checksum = LoadLittle64(request.bytes.data() + 16);
  if (stored_checksum == 0 ||
      stored_checksum != ComputeChecksum(request.bytes)) {
    return OpenFailure(SpatialRTreeOpenClass::bad_checksum,
                       "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.BAD_CHECKSUM",
                       "index.spatial_rtree_physical_provider.bad_checksum");
  }

  Reader reader(request.bytes);
  reader.SetOffset(8);
  SpatialRTreePhysicalProvider provider;
  u32 major = 0;
  u32 minor = 0;
  u64 checksum = 0;
  u64 row_count = 0;
  u64 node_count = 0;
  if (!reader.ReadU32(&major) ||
      !reader.ReadU32(&minor) ||
      !reader.ReadU64(&checksum)) {
    return OpenFailure(
        SpatialRTreeOpenClass::corrupt_payload,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
        "index.spatial_rtree_physical_provider.corrupt_payload");
  }
  provider.format_version = {major, minor};
  if (!SameFormatVersion(provider.format_version,
                         {kSpatialRTreePhysicalProviderCurrentMajor,
                          kSpatialRTreePhysicalProviderCurrentMinor})) {
    return OpenFailure(SpatialRTreeOpenClass::stale_format,
                       "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.STALE_FORMAT",
                       "index.spatial_rtree_physical_provider.stale_format");
  }
  if (!reader.ReadString(&provider.relation_uuid) ||
      !reader.ReadString(&provider.index_uuid) ||
      !reader.ReadString(&provider.provider_uuid) ||
      !reader.ReadU64(&provider.base_generation) ||
      !reader.ReadU64(&provider.provider_generation) ||
      !reader.ReadU32(&provider.descriptor.dimensions) ||
      !reader.ReadU64(&provider.descriptor.descriptor_epoch) ||
      !reader.ReadBool(&provider.descriptor.deterministic) ||
      !reader.ReadBool(&provider.descriptor.descriptor_safe) ||
      !reader.ReadBool(&provider.descriptor.supports_point) ||
      !reader.ReadBool(&provider.descriptor.supports_mbr) ||
      !reader.ReadBool(&provider.descriptor.supports_z) ||
      !reader.ReadBool(&provider.descriptor.supports_m) ||
      !reader.ReadString(&provider.srid_resource.resource_uuid) ||
      !reader.ReadU32(&provider.srid_resource.srid) ||
      !reader.ReadU64(&provider.srid_resource.resource_epoch) ||
      !reader.ReadString(&provider.srid_resource.coordinate_order) ||
      !reader.ReadBool(&provider.srid_resource.deterministic) ||
      !reader.ReadBool(&provider.srid_resource.safe) ||
      !reader.ReadBool(&provider.srid_resource.cache_present) ||
      !reader.ReadU32(&provider.max_entries) ||
      !reader.ReadU32(&provider.min_entries) ||
      !reader.ReadU32(&provider.root_node_id) ||
      !reader.ReadU32(&provider.tree_height) ||
      !reader.ReadU64(&provider.split_count) ||
      !reader.ReadU64(&provider.merge_count) ||
      !reader.ReadU64(&row_count) ||
      row_count > kMaxRows) {
    return OpenFailure(
        SpatialRTreeOpenClass::corrupt_payload,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
        "index.spatial_rtree_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < row_count; ++i) {
    SpatialRTreeStoredRow row;
    if (!reader.ReadLocator(&row.locator) ||
        !reader.ReadMbr(&row.mbr) ||
        !reader.ReadString(&row.exact_source_recheck_evidence_ref) ||
        !reader.ReadBool(&row.tombstoned)) {
      return OpenFailure(
          SpatialRTreeOpenClass::corrupt_payload,
          "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
          "index.spatial_rtree_physical_provider.corrupt_payload");
    }
    provider.rows.push_back(std::move(row));
  }
  if (!reader.ReadU64(&node_count) || node_count > kMaxNodes) {
    return OpenFailure(
        SpatialRTreeOpenClass::corrupt_payload,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
        "index.spatial_rtree_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < node_count; ++i) {
    SpatialRTreeNode node;
    u64 entry_count = 0;
    if (!reader.ReadU32(&node.node_id) ||
        !reader.ReadBool(&node.leaf) ||
        !reader.ReadU32(&node.level) ||
        !reader.ReadMbr(&node.cover) ||
        !reader.ReadU64(&entry_count) ||
        entry_count > provider.max_entries) {
      return OpenFailure(
          SpatialRTreeOpenClass::corrupt_payload,
          "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
          "index.spatial_rtree_physical_provider.corrupt_payload");
    }
    for (u64 j = 0; j < entry_count; ++j) {
      SpatialRTreeNodeEntry entry;
      if (!reader.ReadMbr(&entry.mbr) ||
          !reader.ReadBool(&entry.child) ||
          !reader.ReadU32(&entry.child_node) ||
          !reader.ReadLocator(&entry.locator) ||
          !reader.ReadBool(&entry.tombstoned)) {
        return OpenFailure(
            SpatialRTreeOpenClass::corrupt_payload,
            "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
            "index.spatial_rtree_physical_provider.corrupt_payload");
      }
      node.entries.push_back(std::move(entry));
    }
    provider.nodes.push_back(std::move(node));
  }
  if (!reader.Done()) {
    return OpenFailure(
        SpatialRTreeOpenClass::corrupt_payload,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
        "index.spatial_rtree_physical_provider.corrupt_payload");
  }
  SetProviderEvidence(&provider);

  if (!DescriptorSafe(provider.descriptor)) {
    return OpenFailure(
        SpatialRTreeOpenClass::unsupported_geometry_profile,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.UNSUPPORTED_GEOMETRY_PROFILE",
        "index.spatial_rtree_physical_provider.unsupported_geometry_profile");
  }
  if (!SridResourceSafe(provider.srid_resource, provider.descriptor)) {
    return OpenFailure(
        SpatialRTreeOpenClass::stale_srid_resource_epoch,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.UNSAFE_SRID_RESOURCE",
        "index.spatial_rtree_physical_provider.unsafe_srid_resource");
  }
  if ((request.expected_relation_uuid_present &&
       request.expected_relation_uuid != provider.relation_uuid) ||
      (request.expected_index_uuid_present &&
       request.expected_index_uuid != provider.index_uuid) ||
      (request.expected_provider_uuid_present &&
       request.expected_provider_uuid != provider.provider_uuid)) {
    return OpenFailure(SpatialRTreeOpenClass::identity_mismatch,
                       "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.IDENTITY_MISMATCH",
                       "index.spatial_rtree_physical_provider.identity_mismatch");
  }
  if ((request.expected_base_generation_present &&
       request.expected_base_generation != provider.base_generation) ||
      (request.expected_provider_generation_present &&
       request.expected_provider_generation != provider.provider_generation)) {
    return OpenFailure(SpatialRTreeOpenClass::stale_generation,
                       "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.STALE_GENERATION",
                       "index.spatial_rtree_physical_provider.stale_generation");
  }
  if (request.expected_descriptor_epoch_present &&
      request.expected_descriptor_epoch != provider.descriptor.descriptor_epoch) {
    return OpenFailure(
        SpatialRTreeOpenClass::stale_descriptor_epoch,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.spatial_rtree_physical_provider.stale_descriptor_epoch");
  }
  if (request.expected_srid_resource_epoch_present &&
      request.expected_srid_resource_epoch !=
          provider.srid_resource.resource_epoch) {
    return OpenFailure(
        SpatialRTreeOpenClass::stale_srid_resource_epoch,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.STALE_SRID_RESOURCE_EPOCH",
        "index.spatial_rtree_physical_provider.stale_srid_resource_epoch");
  }
  if (request.expected_srid_present &&
      request.expected_srid != provider.srid_resource.srid) {
    return OpenFailure(
        SpatialRTreeOpenClass::stale_srid_resource_epoch,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.STALE_SRID",
        "index.spatial_rtree_physical_provider.stale_srid");
  }
  if (!ProviderValid(provider)) {
    return OpenFailure(
        SpatialRTreeOpenClass::corrupt_payload,
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
        "index.spatial_rtree_physical_provider.corrupt_payload");
  }

  SpatialRTreeOpenResult result;
  result.status = OkStatus();
  result.open_class = SpatialRTreeOpenClass::current;
  result.provider = std::move(provider);
  result.fail_closed = false;
  result.actions.push_back("open_spatial_rtree_physical_provider");
  return result;
}

SpatialRTreeQueryResult QuerySpatialRTreePhysicalProvider(
    const SpatialRTreeQueryRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return QueryFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.spatial_rtree_physical_provider.missing_exact_recheck");
  }
  if (!request.descriptor_epoch_current) {
    return QueryFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.spatial_rtree_physical_provider.stale_descriptor_epoch");
  }
  if (!request.srid_resource_epoch_current) {
    return QueryFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.STALE_SRID_RESOURCE_EPOCH",
        "index.spatial_rtree_physical_provider.stale_srid_resource_epoch");
  }
  if (!RuntimeRequestSafe(request)) {
    return QueryFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
        "index.spatial_rtree_physical_provider.runtime_refused");
  }

  SpatialRTreeQueryResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.evidence = request.provider.evidence;
  if (request.provider.nodes.empty()) {
    return result;
  }
  if (request.kind == SpatialRTreeQueryKind::nearest) {
    struct QueueItem {
      double distance = 0.0;
      bool row = false;
      u32 node_id = 0;
      TextInvertedRowLocator locator;
      bool operator<(const QueueItem& other) const {
        if (distance != other.distance) return distance > other.distance;
        return locator.row_ordinal > other.locator.row_ordinal;
      }
    };
    std::priority_queue<QueueItem> queue;
    queue.push({MinDistanceToPoint(
                    request.provider.nodes[request.provider.root_node_id].cover,
                    request.query_point),
                false,
                request.provider.root_node_id,
                {}});
    result.priority_queue_used = true;
    while (!queue.empty() &&
           (request.top_k == 0 || result.candidates.size() < request.top_k)) {
      const auto item = queue.top();
      queue.pop();
      if (item.row) {
        const auto* row = FindStoredRow(request.provider, item.locator);
        if (row != nullptr && !row->tombstoned) {
          result.candidates.push_back(MakeCandidate(*row, item.distance));
        }
        continue;
      }
      ++result.nodes_visited;
      const auto& node = request.provider.nodes[item.node_id];
      for (const auto& entry : node.entries) {
        ++result.entries_examined;
        const double distance = MinDistanceToPoint(entry.mbr,
                                                   request.query_point);
        if (entry.child) {
          queue.push({distance, false, entry.child_node, {}});
        } else {
          queue.push({distance, true, 0, entry.locator});
        }
      }
    }
    return result;
  }

  std::vector<u32> stack{request.provider.root_node_id};
  while (!stack.empty()) {
    const u32 node_id = stack.back();
    stack.pop_back();
    ++result.nodes_visited;
    const auto& node = request.provider.nodes[node_id];
    for (const auto& entry : node.entries) {
      ++result.entries_examined;
      if (!PredicateMatches(request.kind, entry.mbr, request.query_mbr)) {
        continue;
      }
      if (entry.child) {
        stack.push_back(entry.child_node);
      } else {
        const auto* row = FindStoredRow(request.provider, entry.locator);
        if (row != nullptr && !row->tombstoned &&
            PredicateMatches(request.kind, row->mbr, request.query_mbr)) {
          result.candidates.push_back(MakeCandidate(*row, 0.0));
        }
      }
    }
  }
  result.mbr_predicate_evaluated = true;
  SortCandidates(&result.candidates);
  return result;
}

SpatialRTreeMutationResult ApplySpatialRTreePhysicalMutation(
    const SpatialRTreePhysicalProvider& provider,
    const SpatialRTreeMutation& mutation) {
  if (!ProviderValid(provider)) {
    return MutationFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.MUTATION_PROVIDER_INVALID",
        "index.spatial_rtree_physical_provider.mutation_provider_invalid");
  }
  if (!RecheckProofValid(mutation.recheck_proof)) {
    return MutationFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
        "index.spatial_rtree_physical_provider.missing_exact_recheck");
  }
  if ((mutation.expected_provider_generation_present &&
       mutation.expected_provider_generation != provider.provider_generation) ||
      (mutation.expected_descriptor_epoch_present &&
       mutation.expected_descriptor_epoch != provider.descriptor.descriptor_epoch) ||
      (mutation.expected_srid_resource_epoch_present &&
       mutation.expected_srid_resource_epoch !=
           provider.srid_resource.resource_epoch)) {
    return MutationFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.STALE_EPOCH",
        "index.spatial_rtree_physical_provider.stale_epoch");
  }

  SpatialRTreePhysicalProvider next = provider;
  const u64 prior_split_count = next.split_count;
  if (mutation.kind == SpatialRTreeMutationKind::insert_row) {
    if (!mutation.after_row_present ||
        !StoredRowValid(StoredFromSource(mutation.after_row),
                        provider.descriptor,
                        provider.srid_resource) ||
        FindStoredRow(provider, mutation.after_row.locator) != nullptr) {
      return MutationFailure(
          "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.INSERT_REFUSED",
          "index.spatial_rtree_physical_provider.insert_refused");
    }
    next.rows.push_back(StoredFromSource(mutation.after_row));
    std::sort(next.rows.begin(), next.rows.end(), [](const auto& left,
                                                     const auto& right) {
      return LocatorLess(left.locator, right.locator);
    });
  } else if (mutation.kind == SpatialRTreeMutationKind::delete_row) {
    if (!mutation.before_row_present) {
      return MutationFailure(
          "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.DELETE_REFUSED",
          "index.spatial_rtree_physical_provider.delete_refused");
    }
    auto iter = std::lower_bound(
        next.rows.begin(), next.rows.end(), mutation.before_row.locator,
        [](const SpatialRTreeStoredRow& row,
           const TextInvertedRowLocator& locator) {
          return LocatorLess(row.locator, locator);
        });
    if (iter == next.rows.end() ||
        !LocatorEqual(iter->locator, mutation.before_row.locator) ||
        iter->tombstoned ||
        !SourceMatchesStored(mutation.before_row, *iter)) {
      return MutationFailure(
          "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.DELETE_REFUSED",
          "index.spatial_rtree_physical_provider.delete_refused");
    }
    iter->tombstoned = true;
    ++next.merge_count;
  } else {
    if (!mutation.before_row_present || !mutation.after_row_present) {
      return MutationFailure(
          "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.UPDATE_REFUSED",
          "index.spatial_rtree_physical_provider.update_refused");
    }
    auto iter = std::lower_bound(
        next.rows.begin(), next.rows.end(), mutation.before_row.locator,
        [](const SpatialRTreeStoredRow& row,
           const TextInvertedRowLocator& locator) {
          return LocatorLess(row.locator, locator);
        });
    if (iter == next.rows.end() ||
        !LocatorEqual(iter->locator, mutation.before_row.locator) ||
        iter->tombstoned ||
        !SourceMatchesStored(mutation.before_row, *iter) ||
        !LocatorEqual(mutation.before_row.locator,
                      mutation.after_row.locator) ||
        !SpatialRTreeMbrValid(mutation.after_row.mbr,
                              provider.descriptor,
                              provider.srid_resource)) {
      return MutationFailure(
          "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.UPDATE_REFUSED",
          "index.spatial_rtree_physical_provider.update_refused");
    }
    *iter = StoredFromSource(mutation.after_row);
  }

  auto state = BuildByInsertion(next.rows,
                                next.descriptor.dimensions,
                                next.srid_resource.srid,
                                next.max_entries,
                                next.min_entries);
  InstallTree(&next, std::move(state));
  ++next.provider_generation;
  SetProviderEvidence(&next);
  if (!ProviderValid(next)) {
    return MutationFailure(
        "INDEX.SPATIAL_RTREE_PHYSICAL_PROVIDER.MUTATION_CORRUPT",
        "index.spatial_rtree_physical_provider.mutation_corrupt");
  }

  SpatialRTreeMutationResult result;
  result.status = OkStatus();
  result.provider = std::move(next);
  result.applied = true;
  result.fail_closed = false;
  result.split_performed = result.provider.split_count > prior_split_count;
  result.merge_performed =
      mutation.kind == SpatialRTreeMutationKind::delete_row;
  result.tombstone_written =
      mutation.kind == SpatialRTreeMutationKind::delete_row;
  result.actions.push_back("apply_spatial_rtree_physical_mutation");
  return result;
}

const char* SpatialRTreeOpenClassName(SpatialRTreeOpenClass open_class) {
  switch (open_class) {
    case SpatialRTreeOpenClass::current: return "current";
    case SpatialRTreeOpenClass::stale_format: return "stale_format";
    case SpatialRTreeOpenClass::stale_generation: return "stale_generation";
    case SpatialRTreeOpenClass::bad_checksum: return "bad_checksum";
    case SpatialRTreeOpenClass::corrupt_payload: return "corrupt_payload";
    case SpatialRTreeOpenClass::identity_mismatch: return "identity_mismatch";
    case SpatialRTreeOpenClass::stale_descriptor_epoch:
      return "stale_descriptor_epoch";
    case SpatialRTreeOpenClass::stale_srid_resource_epoch:
      return "stale_srid_resource_epoch";
    case SpatialRTreeOpenClass::invalid_mbr: return "invalid_mbr";
    case SpatialRTreeOpenClass::unsupported_geometry_profile:
      return "unsupported_geometry_profile";
    case SpatialRTreeOpenClass::missing_exact_recheck_proof:
      return "missing_exact_recheck_proof";
    case SpatialRTreeOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case SpatialRTreeOpenClass::refused: return "refused";
  }
  return "unknown";
}

const char* SpatialRTreeQueryKindName(SpatialRTreeQueryKind kind) {
  switch (kind) {
    case SpatialRTreeQueryKind::point: return "point";
    case SpatialRTreeQueryKind::intersects: return "intersects";
    case SpatialRTreeQueryKind::contains: return "contains";
    case SpatialRTreeQueryKind::within: return "within";
    case SpatialRTreeQueryKind::range: return "range";
    case SpatialRTreeQueryKind::nearest: return "nearest";
  }
  return "unknown";
}

DiagnosticRecord MakeSpatialRTreePhysicalProviderDiagnostic(
    Status status,
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
                        "core.index.spatial_rtree_physical_provider");
}

}  // namespace scratchbird::core::index
