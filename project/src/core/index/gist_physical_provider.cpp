// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "gist_physical_provider.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
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

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'G', 'I',
                                               'S', 'T', '0', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u64 kFnvOffset = 14695981039346656037ull;
inline constexpr u64 kFnvPrime = 1099511628211ull;
inline constexpr u64 kMaxEmbeddedSpatialBytes = 128ull * 1024ull * 1024ull;

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

bool LocatorLess(const TextInvertedRowLocator& left,
                 const TextInvertedRowLocator& right) {
  return std::tie(left.row_ordinal, left.row_uuid, left.version_uuid) <
         std::tie(right.row_ordinal, right.row_uuid, right.version_uuid);
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

bool RecheckAuthorityClean(const GistExactRecheckProof& proof) {
  return !proof.parser_finality_authority_claimed &&
         !proof.reference_finality_authority_claimed &&
         !proof.provider_finality_authority_claimed &&
         !proof.index_finality_authority_claimed &&
         !proof.write_ahead_log_finality_authority_claimed &&
         !proof.visibility_authority_claimed &&
         !proof.security_authority_claimed &&
         !proof.transaction_finality_authority_claimed;
}

bool RecheckProofValid(const GistExactRecheckProof& proof) {
  return proof.proof_supplied &&
         proof.exact_source_geometry_available &&
         proof.exact_predicate_recheck_required &&
         proof.mga_recheck_required &&
         proof.security_recheck_required &&
         !proof.evidence_ref.empty() &&
         RecheckAuthorityClean(proof);
}

bool OpclassAuthorityClean(const GistOpclassDescriptor& opclass) {
  return !opclass.parser_finality_authority_claimed &&
         !opclass.reference_finality_authority_claimed &&
         !opclass.provider_finality_authority_claimed &&
         !opclass.index_finality_authority_claimed &&
         !opclass.write_ahead_log_finality_authority_claimed;
}

bool OpclassDescriptorSafe(const GistOpclassDescriptor& opclass) {
  return opclass.opclass_name == kGistSpatialMbrOpclassName &&
         opclass.opclass_epoch > 0 &&
         opclass.resource_epoch > 0 &&
         opclass.registered &&
         opclass.deterministic &&
         opclass.immutable &&
         opclass.safe &&
         opclass.supports_nearest &&
         opclass.dimensions >= 2 &&
         opclass.dimensions <= 4 &&
         opclass.srid > 0 &&
         !opclass.descriptor_store_scan &&
         !opclass.behavior_store_scan &&
         !opclass.contract_only_fallback &&
         !opclass.provider_only_fallback &&
         OpclassAuthorityClean(opclass);
}

bool MethodSetComplete(const GistOpclassRuntime& opclass) {
  return static_cast<bool>(opclass.methods.consistent) &&
         static_cast<bool>(opclass.methods.union_key) &&
         static_cast<bool>(opclass.methods.compress) &&
         static_cast<bool>(opclass.methods.decompress) &&
         static_cast<bool>(opclass.methods.penalty) &&
         static_cast<bool>(opclass.methods.picksplit) &&
         static_cast<bool>(opclass.methods.same) &&
         static_cast<bool>(opclass.methods.distance);
}

bool OpclassRuntimeSafe(const GistOpclassRuntime& opclass) {
  return OpclassDescriptorSafe(opclass.descriptor) &&
         MethodSetComplete(opclass);
}

bool SameOpclassDescriptor(const GistOpclassDescriptor& left,
                           const GistOpclassDescriptor& right) {
  return left.opclass_name == right.opclass_name &&
         left.opclass_epoch == right.opclass_epoch &&
         left.resource_epoch == right.resource_epoch &&
         left.dimensions == right.dimensions &&
         left.srid == right.srid &&
         left.registered == right.registered &&
         left.deterministic == right.deterministic &&
         left.immutable == right.immutable &&
         left.safe == right.safe &&
         left.supports_nearest == right.supports_nearest;
}

bool ProviderAuthorityClean(const GistPhysicalProvider& provider) {
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
         !provider.reference_finality_authority_claimed &&
         !provider.provider_finality_authority_claimed &&
         !provider.index_finality_authority_claimed &&
         !provider.write_ahead_log_finality_authority_claimed;
}

bool ProviderValid(const GistPhysicalProvider& provider) {
  if (provider.artifact_kind != kGistPhysicalProviderArtifactKind ||
      !SameFormatVersion(provider.format_version,
                         {kGistPhysicalProviderCurrentMajor,
                          kGistPhysicalProviderCurrentMinor}) ||
      provider.relation_uuid != provider.physical_tree.relation_uuid ||
      provider.index_uuid != provider.physical_tree.index_uuid ||
      provider.provider_uuid != provider.physical_tree.provider_uuid ||
      provider.base_generation != provider.physical_tree.base_generation ||
      provider.provider_generation !=
          provider.physical_tree.provider_generation ||
      provider.opclass.dimensions != provider.physical_tree.descriptor.dimensions ||
      provider.opclass.srid != provider.physical_tree.srid_resource.srid ||
      !OpclassDescriptorSafe(provider.opclass) ||
      !provider.opclass_required_for_runtime ||
      !provider.spatial_rtree_provider_consumed ||
      !provider.consistent_present ||
      !provider.union_present ||
      !provider.compress_present ||
      !provider.decompress_present ||
      !provider.penalty_present ||
      !provider.picksplit_present ||
      !provider.same_present ||
      !provider.distance_present ||
      !ProviderAuthorityClean(provider)) {
    return false;
  }
  return SerializeSpatialRTreePhysicalProvider(provider.physical_tree).ok();
}

SpatialRTreeRecheckProof ToSpatialProof(const GistExactRecheckProof& proof) {
  SpatialRTreeRecheckProof out;
  out.proof_supplied = proof.proof_supplied;
  out.exact_source_geometry_available = proof.exact_source_geometry_available;
  out.exact_predicate_recheck_required =
      proof.exact_predicate_recheck_required;
  out.mga_recheck_required = proof.mga_recheck_required;
  out.security_recheck_required = proof.security_recheck_required;
  out.evidence_ref = proof.evidence_ref;
  out.parser_finality_authority_claimed =
      proof.parser_finality_authority_claimed;
  out.reference_finality_authority_claimed =
      proof.reference_finality_authority_claimed;
  out.provider_finality_authority_claimed =
      proof.provider_finality_authority_claimed;
  out.index_finality_authority_claimed =
      proof.index_finality_authority_claimed;
  out.write_ahead_log_finality_authority_claimed =
      proof.write_ahead_log_finality_authority_claimed;
  out.visibility_authority_claimed = proof.visibility_authority_claimed;
  out.security_authority_claimed = proof.security_authority_claimed;
  out.transaction_finality_authority_claimed =
      proof.transaction_finality_authority_claimed;
  return out;
}

void SetProviderEvidence(GistPhysicalProvider* provider) {
  provider->evidence = {
      kGistPhysicalProviderSearchKey,
      "gist_registered_deterministic_opclass_required",
      "gist_consistent_union_compress_decompress_penalty_picksplit_same_distance",
      "gist_spatial_mbr_opclass",
      "gist_spatial_rtree_physical_provider_consumed",
      "gist_candidate_locator_evidence"};
}

GistCompressedKey CompressChecked(const GistOpclassRuntime& opclass,
                                  const SpatialRTreeMbr& key) {
  if (!OpclassRuntimeSafe(opclass)) {
    return {};
  }
  auto compressed = opclass.methods.compress(key);
  if (!GistCompressedKeyValid(compressed, opclass.descriptor)) {
    return {};
  }
  const auto decompressed = opclass.methods.decompress(compressed);
  if (!MbrSame(decompressed, key)) {
    return {};
  }
  return compressed;
}

GistBuildResult BuildFailure(std::string code,
                             std::string key,
                             std::string detail = {}) {
  GistBuildResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeGistPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

GistOpenResult OpenFailure(GistOpenClass open_class,
                           std::string code,
                           std::string key,
                           std::string detail = {}) {
  GistOpenResult result;
  result.status = ErrorStatus();
  result.open_class = open_class;
  result.fail_closed = true;
  result.restricted_repair_required =
      open_class == GistOpenClass::bad_checksum ||
      open_class == GistOpenClass::corrupt_payload;
  result.diagnostic = MakeGistPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

GistOpenClass OpenClassFromSpatial(SpatialRTreeOpenClass open_class) {
  switch (open_class) {
    case SpatialRTreeOpenClass::current:
      return GistOpenClass::current;
    case SpatialRTreeOpenClass::stale_format:
      return GistOpenClass::stale_format;
    case SpatialRTreeOpenClass::stale_generation:
      return GistOpenClass::stale_generation;
    case SpatialRTreeOpenClass::bad_checksum:
      return GistOpenClass::bad_checksum;
    case SpatialRTreeOpenClass::corrupt_payload:
      return GistOpenClass::corrupt_payload;
    case SpatialRTreeOpenClass::identity_mismatch:
      return GistOpenClass::identity_mismatch;
    case SpatialRTreeOpenClass::stale_descriptor_epoch:
      return GistOpenClass::stale_descriptor_epoch;
    case SpatialRTreeOpenClass::stale_srid_resource_epoch:
      return GistOpenClass::stale_srid_resource_epoch;
    case SpatialRTreeOpenClass::invalid_mbr:
      return GistOpenClass::invalid_compressed_key;
    case SpatialRTreeOpenClass::unsupported_geometry_profile:
      return GistOpenClass::incompatible_spatial_profile;
    case SpatialRTreeOpenClass::authority_claim_refused:
      return GistOpenClass::authority_claim_refused;
    case SpatialRTreeOpenClass::missing_exact_recheck_proof:
      return GistOpenClass::missing_recheck_proof;
    case SpatialRTreeOpenClass::refused:
      return GistOpenClass::refused;
  }
  return GistOpenClass::refused;
}

GistQueryResult QueryFailure(std::string code,
                             std::string key,
                             std::string detail = {}) {
  GistQueryResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeGistPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

GistMutationResult MutationFailure(std::string code,
                                   std::string key,
                                   std::string detail = {}) {
  GistMutationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeGistPhysicalProviderDiagnostic(
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
void AppendString(std::vector<byte>* out, const std::string& value) {
  AppendU32(out, static_cast<u32>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}
void AppendBytes(std::vector<byte>* out, const std::vector<byte>& bytes) {
  AppendU64(out, static_cast<u64>(bytes.size()));
  out->insert(out->end(), bytes.begin(), bytes.end());
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
  bool ReadString(std::string* out) {
    u32 size = 0;
    if (!ReadU32(&size) || offset_ + size > bytes_.size()) return false;
    out->assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }
  bool ReadBytes(std::vector<byte>* out) {
    u64 size = 0;
    if (!ReadU64(&size) || size > kMaxEmbeddedSpatialBytes ||
        offset_ + size > bytes_.size()) {
      return false;
    }
    out->assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += static_cast<std::size_t>(size);
    return true;
  }

 private:
  const std::vector<byte>& bytes_;
  std::size_t offset_ = 0;
};

}  // namespace

GistOpclassRuntime MakeSpatialMbrGistOpclass(u64 opclass_epoch,
                                             u64 resource_epoch,
                                             u32 dimensions,
                                             u32 srid) {
  GistOpclassRuntime runtime;
  runtime.descriptor.opclass_name = kGistSpatialMbrOpclassName;
  runtime.descriptor.opclass_epoch = opclass_epoch;
  runtime.descriptor.resource_epoch = resource_epoch;
  runtime.descriptor.registered = true;
  runtime.descriptor.deterministic = true;
  runtime.descriptor.immutable = true;
  runtime.descriptor.safe = true;
  runtime.descriptor.supports_nearest = true;
  runtime.descriptor.dimensions = dimensions;
  runtime.descriptor.srid = srid;

  runtime.methods.compress =
      [dimensions, srid](const SpatialRTreeMbr& mbr) {
        GistCompressedKey key;
        key.dimensions = dimensions;
        key.srid = srid;
        if (mbr.dimensions != dimensions || mbr.srid != srid) {
          return key;
        }
        key.bytes = EncodeSpatialRTreeMbr(mbr);
        key.deterministic = true;
        key.valid = !key.bytes.empty();
        return key;
      };
  runtime.methods.decompress =
      [dimensions, srid](const GistCompressedKey& key) {
        if (key.dimensions != dimensions || key.srid != srid ||
            !key.deterministic || !key.valid) {
          return SpatialRTreeMbr{};
        }
        return DecodeSpatialRTreeMbr(key.bytes, dimensions, srid);
      };
  runtime.methods.same =
      [dimensions, srid](const GistCompressedKey& left,
                         const GistCompressedKey& right) {
        if (left.dimensions != dimensions || right.dimensions != dimensions ||
            left.srid != srid || right.srid != srid) {
          return false;
        }
        return left.bytes == right.bytes && left.valid && right.valid;
      };
  runtime.methods.union_key =
      [runtime, dimensions, srid](const std::vector<GistCompressedKey>& keys) {
        GistCompressedKey out;
        out.dimensions = dimensions;
        out.srid = srid;
        if (keys.empty()) {
          return out;
        }
        auto cover = runtime.methods.decompress(keys.front());
        if (cover.dimensions != dimensions || cover.srid != srid) {
          return out;
        }
        for (std::size_t i = 1; i < keys.size(); ++i) {
          const auto next = runtime.methods.decompress(keys[i]);
          if (next.dimensions != dimensions || next.srid != srid) {
            return out;
          }
          cover = UnionMbr(cover, next);
        }
        return runtime.methods.compress(cover);
      };
  runtime.methods.penalty =
      [runtime](const GistCompressedKey& current,
                const GistCompressedKey& added) {
        const auto left = runtime.methods.decompress(current);
        const auto right = runtime.methods.decompress(added);
        if (left.dimensions == 0 || right.dimensions == 0 ||
            left.dimensions != right.dimensions || left.srid != right.srid) {
          return std::numeric_limits<double>::infinity();
        }
        return Area(UnionMbr(left, right)) - Area(left);
      };
  runtime.methods.picksplit =
      [runtime, dimensions, srid](const std::vector<GistCompressedKey>& keys,
                                  u32 min_entries) {
        GistPickSplitResult result;
        if (keys.size() < 2 || min_entries == 0 ||
            min_entries * 2 > keys.size() + 1) {
          return result;
        }
        std::vector<std::pair<SpatialRTreeMbr, GistCompressedKey>> decoded;
        decoded.reserve(keys.size());
        for (const auto& key : keys) {
          const auto mbr = runtime.methods.decompress(key);
          if (mbr.dimensions != dimensions || mbr.srid != srid) {
            return result;
          }
          decoded.push_back({mbr, key});
        }
        std::sort(decoded.begin(), decoded.end(), [](const auto& left,
                                                     const auto& right) {
          const double left_center = (left.first.min[0] + left.first.max[0]) * 0.5;
          const double right_center = (right.first.min[0] + right.first.max[0]) * 0.5;
          if (left_center != right_center) return left_center < right_center;
          return Area(left.first) < Area(right.first);
        });
        const std::size_t split_at =
            std::max<std::size_t>(min_entries, decoded.size() / 2);
        for (std::size_t i = 0; i < decoded.size(); ++i) {
          (i < split_at ? result.left : result.right).push_back(decoded[i].second);
        }
        result.deterministic = true;
        result.valid = !result.left.empty() && !result.right.empty();
        return result;
      };
  runtime.methods.consistent =
      [runtime](const GistCompressedKey& key,
                const SpatialRTreeMbr& query,
                GistPredicateStrategy strategy) {
        const auto mbr = runtime.methods.decompress(key);
        if (mbr.dimensions == 0 || query.dimensions != mbr.dimensions ||
            query.srid != mbr.srid) {
          return false;
        }
        switch (strategy) {
          case GistPredicateStrategy::point:
          case GistPredicateStrategy::contains:
            return Contains(mbr, query);
          case GistPredicateStrategy::within:
            return Contains(query, mbr);
          case GistPredicateStrategy::intersects:
          case GistPredicateStrategy::range:
            return Intersects(mbr, query);
          case GistPredicateStrategy::nearest:
            return true;
        }
        return false;
      };
  runtime.methods.distance =
      [runtime](const GistCompressedKey& key,
                const std::vector<double>& point) {
        const auto mbr = runtime.methods.decompress(key);
        if (mbr.dimensions == 0 || point.size() != mbr.dimensions ||
            !std::all_of(point.begin(), point.end(), [](double value) {
              return std::isfinite(value);
            })) {
          return std::numeric_limits<double>::infinity();
        }
        return MinDistanceToPoint(mbr, point);
      };
  return runtime;
}

bool GistCompressedKeyValid(const GistCompressedKey& key,
                            const GistOpclassDescriptor& opclass) {
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

SpatialRTreeQueryKind GistStrategyToSpatialRTreeQueryKind(
    GistPredicateStrategy strategy) {
  switch (strategy) {
    case GistPredicateStrategy::point: return SpatialRTreeQueryKind::point;
    case GistPredicateStrategy::intersects:
      return SpatialRTreeQueryKind::intersects;
    case GistPredicateStrategy::contains: return SpatialRTreeQueryKind::contains;
    case GistPredicateStrategy::within: return SpatialRTreeQueryKind::within;
    case GistPredicateStrategy::range: return SpatialRTreeQueryKind::range;
    case GistPredicateStrategy::nearest: return SpatialRTreeQueryKind::nearest;
  }
  return SpatialRTreeQueryKind::intersects;
}

GistBuildResult BuildGistPhysicalProvider(const GistBuildRequest& request) {
  if (!RecheckAuthorityClean(request.recheck_proof) ||
      !OpclassAuthorityClean(request.opclass.descriptor)) {
    return BuildFailure("INDEX.GIST_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED",
                        "index.gist_physical_provider.authority_claim_refused");
  }
  if (!RecheckProofValid(request.recheck_proof)) {
    return BuildFailure("INDEX.GIST_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
                        "index.gist_physical_provider.missing_exact_recheck");
  }
  if (!OpclassRuntimeSafe(request.opclass)) {
    return BuildFailure("INDEX.GIST_PHYSICAL_PROVIDER.UNSAFE_OPCLASS",
                        "index.gist_physical_provider.unsafe_opclass");
  }
  if (request.opclass.descriptor.dimensions !=
          request.spatial_descriptor.dimensions ||
      request.opclass.descriptor.srid != request.srid_resource.srid) {
    return BuildFailure(
        "INDEX.GIST_PHYSICAL_PROVIDER.INCOMPATIBLE_SPATIAL_PROFILE",
        "index.gist_physical_provider.incompatible_spatial_profile");
  }

  std::vector<SpatialRTreeSourceRow> spatial_rows;
  std::vector<GistCompressedKey> compressed;
  spatial_rows.reserve(request.rows.size());
  compressed.reserve(request.rows.size());
  for (const auto& row : request.rows) {
    const auto key = CompressChecked(request.opclass, row.key);
    if (!GistCompressedKeyValid(key, request.opclass.descriptor)) {
      return BuildFailure("INDEX.GIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
                          "index.gist_physical_provider.invalid_compressed_key");
    }
    compressed.push_back(key);
    spatial_rows.push_back(
        {row.locator, request.opclass.methods.decompress(key),
         row.exact_source_recheck_evidence_ref});
  }

  u64 consistent_calls = 0;
  u64 union_calls = 0;
  u64 compress_calls = static_cast<u64>(request.rows.size());
  u64 decompress_calls = static_cast<u64>(request.rows.size());
  u64 penalty_calls = 0;
  u64 picksplit_calls = 0;
  u64 same_calls = 0;
  u64 distance_calls = 0;
  if (!compressed.empty()) {
    const auto union_key = request.opclass.methods.union_key(compressed);
    ++union_calls;
    if (!GistCompressedKeyValid(union_key, request.opclass.descriptor)) {
      return BuildFailure("INDEX.GIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
                          "index.gist_physical_provider.invalid_union_key");
    }
    const auto first_mbr = request.opclass.methods.decompress(compressed.front());
    ++decompress_calls;
    if (!request.opclass.methods.consistent(compressed.front(),
                                            first_mbr,
                                            GistPredicateStrategy::intersects)) {
      return BuildFailure("INDEX.GIST_PHYSICAL_PROVIDER.OPCLASS_INCONSISTENT",
                          "index.gist_physical_provider.opclass_inconsistent");
    }
    ++consistent_calls;
    if (!request.opclass.methods.same(compressed.front(), compressed.front())) {
      return BuildFailure("INDEX.GIST_PHYSICAL_PROVIDER.OPCLASS_INCONSISTENT",
                          "index.gist_physical_provider.same_inconsistent");
    }
    ++same_calls;
    const auto distance =
        request.opclass.methods.distance(compressed.front(), first_mbr.min);
    if (!std::isfinite(distance)) {
      return BuildFailure("INDEX.GIST_PHYSICAL_PROVIDER.OPCLASS_INCONSISTENT",
                          "index.gist_physical_provider.distance_inconsistent");
    }
    ++distance_calls;
  }
  if (compressed.size() >= 2) {
    const auto penalty =
        request.opclass.methods.penalty(compressed.front(), compressed.back());
    if (!std::isfinite(penalty) || penalty < 0.0) {
      return BuildFailure("INDEX.GIST_PHYSICAL_PROVIDER.OPCLASS_INCONSISTENT",
                          "index.gist_physical_provider.penalty_inconsistent");
    }
    ++penalty_calls;
    const auto split = request.opclass.methods.picksplit(
        compressed, std::max<u32>(1, request.min_entries));
    if (!split.valid || !split.deterministic || split.left.empty() ||
        split.right.empty()) {
      return BuildFailure("INDEX.GIST_PHYSICAL_PROVIDER.OPCLASS_INCONSISTENT",
                          "index.gist_physical_provider.picksplit_inconsistent");
    }
    ++picksplit_calls;
  }

  SpatialRTreeBuildRequest spatial;
  spatial.relation_uuid = request.relation_uuid;
  spatial.index_uuid = request.index_uuid;
  spatial.provider_uuid = request.provider_uuid;
  spatial.base_generation = request.base_generation;
  spatial.provider_generation = request.provider_generation;
  spatial.descriptor = request.spatial_descriptor;
  spatial.srid_resource = request.srid_resource;
  spatial.recheck_proof = ToSpatialProof(request.recheck_proof);
  spatial.build_mode = request.build_mode;
  spatial.max_entries = request.max_entries;
  spatial.min_entries = request.min_entries;
  spatial.rows = std::move(spatial_rows);
  const auto built_spatial = BuildSpatialRTreePhysicalProvider(spatial);
  if (!built_spatial.ok()) {
    GistBuildResult result = BuildFailure(
        "INDEX.GIST_PHYSICAL_PROVIDER.SPATIAL_RTREE_REFUSED",
        "index.gist_physical_provider.spatial_rtree_refused");
    result.diagnostic = built_spatial.diagnostic;
    return result;
  }

  GistPhysicalProvider provider;
  provider.relation_uuid = request.relation_uuid;
  provider.index_uuid = request.index_uuid;
  provider.provider_uuid = request.provider_uuid;
  provider.base_generation = request.base_generation;
  provider.provider_generation = request.provider_generation;
  provider.opclass = request.opclass.descriptor;
  provider.physical_tree = built_spatial.provider;
  provider.consistent_call_count = consistent_calls;
  provider.union_call_count = union_calls;
  provider.compress_call_count = compress_calls;
  provider.decompress_call_count = decompress_calls;
  provider.penalty_call_count = penalty_calls;
  provider.picksplit_call_count = picksplit_calls;
  provider.same_call_count = same_calls;
  provider.distance_call_count = distance_calls;
  SetProviderEvidence(&provider);
  if (!ProviderValid(provider)) {
    return BuildFailure("INDEX.GIST_PHYSICAL_PROVIDER.BUILD_CORRUPT",
                        "index.gist_physical_provider.build_corrupt");
  }

  GistBuildResult result;
  result.status = OkStatus();
  result.provider = std::move(provider);
  result.built = true;
  result.fail_closed = false;
  result.used_spatial_rtree_provider = true;
  result.all_methods_exercised =
      consistent_calls > 0 && union_calls > 0 && compress_calls > 0 &&
      decompress_calls > 0 && penalty_calls > 0 && picksplit_calls > 0 &&
      same_calls > 0 && distance_calls > 0;
  return result;
}

GistSerializeResult SerializeGistPhysicalProvider(
    const GistPhysicalProvider& provider) {
  GistSerializeResult result;
  if (!ProviderValid(provider)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeGistPhysicalProviderDiagnostic(
        result.status,
        "INDEX.GIST_PHYSICAL_PROVIDER.SERIALIZE_REFUSED",
        "index.gist_physical_provider.serialize_refused");
    return result;
  }
  const auto spatial = SerializeSpatialRTreePhysicalProvider(provider.physical_tree);
  if (!spatial.ok()) {
    result.status = ErrorStatus();
    result.diagnostic = spatial.diagnostic;
    return result;
  }
  std::vector<byte> bytes;
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  AppendU32(&bytes, kGistPhysicalProviderCurrentMajor);
  AppendU32(&bytes, kGistPhysicalProviderCurrentMinor);
  AppendU64(&bytes, 0);
  AppendString(&bytes, provider.relation_uuid);
  AppendString(&bytes, provider.index_uuid);
  AppendString(&bytes, provider.provider_uuid);
  AppendU64(&bytes, provider.base_generation);
  AppendU64(&bytes, provider.provider_generation);
  AppendString(&bytes, provider.opclass.opclass_name);
  AppendU64(&bytes, provider.opclass.opclass_epoch);
  AppendU64(&bytes, provider.opclass.resource_epoch);
  AppendBool(&bytes, provider.opclass.registered);
  AppendBool(&bytes, provider.opclass.deterministic);
  AppendBool(&bytes, provider.opclass.immutable);
  AppendBool(&bytes, provider.opclass.safe);
  AppendBool(&bytes, provider.opclass.supports_nearest);
  AppendU32(&bytes, provider.opclass.dimensions);
  AppendU32(&bytes, provider.opclass.srid);
  AppendU64(&bytes, provider.consistent_call_count);
  AppendU64(&bytes, provider.union_call_count);
  AppendU64(&bytes, provider.compress_call_count);
  AppendU64(&bytes, provider.decompress_call_count);
  AppendU64(&bytes, provider.penalty_call_count);
  AppendU64(&bytes, provider.picksplit_call_count);
  AppendU64(&bytes, provider.same_call_count);
  AppendU64(&bytes, provider.distance_call_count);
  AppendBytes(&bytes, spatial.bytes);
  result.checksum = ComputeChecksum(bytes);
  StoreLittle64(bytes.data() + 16, result.checksum);
  result.status = OkStatus();
  result.bytes = std::move(bytes);
  return result;
}

GistOpenResult OpenGistPhysicalProvider(const GistOpenRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return OpenFailure(GistOpenClass::missing_recheck_proof,
                       "INDEX.GIST_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
                       "index.gist_physical_provider.missing_exact_recheck");
  }
  if (!OpclassRuntimeSafe(request.opclass)) {
    return OpenFailure(GistOpenClass::unsafe_opclass,
                       "INDEX.GIST_PHYSICAL_PROVIDER.UNSAFE_OPCLASS",
                       "index.gist_physical_provider.unsafe_opclass");
  }
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(GistOpenClass::corrupt_payload,
                       "INDEX.GIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.gist_physical_provider.corrupt_payload");
  }
  const u64 stored_checksum = LoadLittle64(request.bytes.data() + 16);
  if (stored_checksum == 0 ||
      stored_checksum != ComputeChecksum(request.bytes)) {
    return OpenFailure(GistOpenClass::bad_checksum,
                       "INDEX.GIST_PHYSICAL_PROVIDER.BAD_CHECKSUM",
                       "index.gist_physical_provider.bad_checksum");
  }

  Reader reader(request.bytes);
  reader.SetOffset(8);
  GistPhysicalProvider provider;
  u32 major = 0;
  u32 minor = 0;
  u64 checksum = 0;
  std::vector<byte> spatial_bytes;
  if (!reader.ReadU32(&major) ||
      !reader.ReadU32(&minor) ||
      !reader.ReadU64(&checksum)) {
    return OpenFailure(GistOpenClass::corrupt_payload,
                       "INDEX.GIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.gist_physical_provider.corrupt_payload");
  }
  provider.format_version = {major, minor};
  if (!SameFormatVersion(provider.format_version,
                         {kGistPhysicalProviderCurrentMajor,
                          kGistPhysicalProviderCurrentMinor})) {
    return OpenFailure(GistOpenClass::stale_format,
                       "INDEX.GIST_PHYSICAL_PROVIDER.STALE_FORMAT",
                       "index.gist_physical_provider.stale_format");
  }
  if (!reader.ReadString(&provider.relation_uuid) ||
      !reader.ReadString(&provider.index_uuid) ||
      !reader.ReadString(&provider.provider_uuid) ||
      !reader.ReadU64(&provider.base_generation) ||
      !reader.ReadU64(&provider.provider_generation) ||
      !reader.ReadString(&provider.opclass.opclass_name) ||
      !reader.ReadU64(&provider.opclass.opclass_epoch) ||
      !reader.ReadU64(&provider.opclass.resource_epoch) ||
      !reader.ReadBool(&provider.opclass.registered) ||
      !reader.ReadBool(&provider.opclass.deterministic) ||
      !reader.ReadBool(&provider.opclass.immutable) ||
      !reader.ReadBool(&provider.opclass.safe) ||
      !reader.ReadBool(&provider.opclass.supports_nearest) ||
      !reader.ReadU32(&provider.opclass.dimensions) ||
      !reader.ReadU32(&provider.opclass.srid) ||
      !reader.ReadU64(&provider.consistent_call_count) ||
      !reader.ReadU64(&provider.union_call_count) ||
      !reader.ReadU64(&provider.compress_call_count) ||
      !reader.ReadU64(&provider.decompress_call_count) ||
      !reader.ReadU64(&provider.penalty_call_count) ||
      !reader.ReadU64(&provider.picksplit_call_count) ||
      !reader.ReadU64(&provider.same_call_count) ||
      !reader.ReadU64(&provider.distance_call_count) ||
      !reader.ReadBytes(&spatial_bytes) ||
      !reader.Done()) {
    return OpenFailure(GistOpenClass::corrupt_payload,
                       "INDEX.GIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.gist_physical_provider.corrupt_payload");
  }
  SetProviderEvidence(&provider);
  if (!SameOpclassDescriptor(provider.opclass, request.opclass.descriptor)) {
    return OpenFailure(GistOpenClass::unsafe_opclass,
                       "INDEX.GIST_PHYSICAL_PROVIDER.UNSAFE_OPCLASS",
                       "index.gist_physical_provider.unsafe_opclass");
  }

  SpatialRTreeOpenRequest spatial_open;
  spatial_open.bytes = std::move(spatial_bytes);
  spatial_open.expected_relation_uuid_present = true;
  spatial_open.expected_relation_uuid = provider.relation_uuid;
  spatial_open.expected_index_uuid_present = true;
  spatial_open.expected_index_uuid = provider.index_uuid;
  spatial_open.expected_provider_uuid_present = true;
  spatial_open.expected_provider_uuid = provider.provider_uuid;
  spatial_open.expected_base_generation_present = true;
  spatial_open.expected_base_generation = provider.base_generation;
  spatial_open.expected_provider_generation_present = true;
  spatial_open.expected_provider_generation = provider.provider_generation;
  spatial_open.expected_descriptor_epoch_present =
      request.expected_descriptor_epoch_present;
  spatial_open.expected_descriptor_epoch = request.expected_descriptor_epoch;
  spatial_open.expected_srid_resource_epoch_present =
      request.expected_srid_resource_epoch_present;
  spatial_open.expected_srid_resource_epoch =
      request.expected_srid_resource_epoch;
  spatial_open.expected_srid_present = true;
  spatial_open.expected_srid = provider.opclass.srid;
  spatial_open.recheck_proof = ToSpatialProof(request.recheck_proof);
  const auto opened_spatial =
      OpenSpatialRTreePhysicalProvider(spatial_open);
  if (!opened_spatial.ok()) {
    return OpenFailure(OpenClassFromSpatial(opened_spatial.open_class),
                       opened_spatial.diagnostic.diagnostic_code,
                       opened_spatial.diagnostic.message_key);
  }
  provider.physical_tree = opened_spatial.provider;

  if ((request.expected_relation_uuid_present &&
       request.expected_relation_uuid != provider.relation_uuid) ||
      (request.expected_index_uuid_present &&
       request.expected_index_uuid != provider.index_uuid) ||
      (request.expected_provider_uuid_present &&
       request.expected_provider_uuid != provider.provider_uuid)) {
    return OpenFailure(GistOpenClass::identity_mismatch,
                       "INDEX.GIST_PHYSICAL_PROVIDER.IDENTITY_MISMATCH",
                       "index.gist_physical_provider.identity_mismatch");
  }
  if ((request.expected_base_generation_present &&
       request.expected_base_generation != provider.base_generation) ||
      (request.expected_provider_generation_present &&
       request.expected_provider_generation != provider.provider_generation)) {
    return OpenFailure(GistOpenClass::stale_generation,
                       "INDEX.GIST_PHYSICAL_PROVIDER.STALE_GENERATION",
                       "index.gist_physical_provider.stale_generation");
  }
  if (request.expected_opclass_epoch_present &&
      request.expected_opclass_epoch != provider.opclass.opclass_epoch) {
    return OpenFailure(GistOpenClass::stale_opclass_epoch,
                       "INDEX.GIST_PHYSICAL_PROVIDER.STALE_OPCLASS_EPOCH",
                       "index.gist_physical_provider.stale_opclass_epoch");
  }
  if (request.expected_resource_epoch_present &&
      request.expected_resource_epoch != provider.opclass.resource_epoch) {
    return OpenFailure(GistOpenClass::stale_resource_epoch,
                       "INDEX.GIST_PHYSICAL_PROVIDER.STALE_RESOURCE_EPOCH",
                       "index.gist_physical_provider.stale_resource_epoch");
  }
  if (request.expected_srid_present &&
      request.expected_srid != provider.opclass.srid) {
    return OpenFailure(GistOpenClass::stale_srid_resource_epoch,
                       "INDEX.GIST_PHYSICAL_PROVIDER.STALE_SRID",
                       "index.gist_physical_provider.stale_srid");
  }
  if (!ProviderValid(provider)) {
    return OpenFailure(GistOpenClass::corrupt_payload,
                       "INDEX.GIST_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.gist_physical_provider.corrupt_payload");
  }

  GistOpenResult result;
  result.status = OkStatus();
  result.open_class = GistOpenClass::current;
  result.provider = std::move(provider);
  result.fail_closed = false;
  result.actions.push_back("open_gist_physical_provider");
  return result;
}

GistQueryResult QueryGistPhysicalProvider(const GistQueryRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return QueryFailure("INDEX.GIST_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
                        "index.gist_physical_provider.missing_exact_recheck");
  }
  if (!request.opclass_epoch_current) {
    return QueryFailure("INDEX.GIST_PHYSICAL_PROVIDER.STALE_OPCLASS_EPOCH",
                        "index.gist_physical_provider.stale_opclass_epoch");
  }
  if (!request.resource_epoch_current) {
    return QueryFailure("INDEX.GIST_PHYSICAL_PROVIDER.STALE_RESOURCE_EPOCH",
                        "index.gist_physical_provider.stale_resource_epoch");
  }
  if (!request.descriptor_epoch_current) {
    return QueryFailure("INDEX.GIST_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
                        "index.gist_physical_provider.stale_descriptor_epoch");
  }
  if (!request.srid_resource_epoch_current) {
    return QueryFailure(
        "INDEX.GIST_PHYSICAL_PROVIDER.STALE_SRID_RESOURCE_EPOCH",
        "index.gist_physical_provider.stale_srid_resource_epoch");
  }
  if (!ProviderValid(request.provider) ||
      !OpclassRuntimeSafe(request.opclass) ||
      !SameOpclassDescriptor(request.provider.opclass,
                             request.opclass.descriptor) ||
      request.descriptor_store_scan ||
      request.behavior_store_scan ||
      request.contract_only_fallback ||
      request.provider_only_fallback) {
    return QueryFailure("INDEX.GIST_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
                        "index.gist_physical_provider.runtime_refused");
  }

  SpatialRTreeQueryRequest spatial;
  spatial.provider = request.provider.physical_tree;
  spatial.kind = GistStrategyToSpatialRTreeQueryKind(request.strategy);
  spatial.query_mbr = request.query_mbr;
  spatial.query_point = request.query_point;
  spatial.top_k =
      request.strategy == GistPredicateStrategy::nearest ? 0 : request.top_k;
  spatial.recheck_proof = ToSpatialProof(request.recheck_proof);
  spatial.descriptor_epoch_current = request.descriptor_epoch_current;
  spatial.srid_resource_epoch_current = request.srid_resource_epoch_current;
  const auto spatial_result = QuerySpatialRTreePhysicalProvider(spatial);
  if (!spatial_result.ok()) {
    GistQueryResult result = QueryFailure(
        "INDEX.GIST_PHYSICAL_PROVIDER.SPATIAL_RTREE_REFUSED",
        "index.gist_physical_provider.spatial_rtree_refused");
    result.diagnostic = spatial_result.diagnostic;
    return result;
  }

  GistQueryResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.nodes_visited = spatial_result.nodes_visited;
  result.entries_examined = spatial_result.entries_examined;
  result.priority_queue_used = spatial_result.priority_queue_used;
  result.spatial_rtree_provider_used = true;
  result.evidence = request.provider.evidence;
  result.evidence.insert(result.evidence.end(),
                         spatial_result.evidence.begin(),
                         spatial_result.evidence.end());
  for (const auto& spatial_candidate : spatial_result.candidates) {
    const auto compressed =
        CompressChecked(request.opclass, spatial_candidate.mbr);
    if (!GistCompressedKeyValid(compressed, request.opclass.descriptor)) {
      return QueryFailure("INDEX.GIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
                          "index.gist_physical_provider.invalid_compressed_key");
    }
    const bool consistent =
        request.strategy == GistPredicateStrategy::nearest ||
        request.opclass.methods.consistent(compressed,
                                           request.query_mbr,
                                           request.strategy);
    result.opclass_consistent_used =
        result.opclass_consistent_used ||
        request.strategy != GistPredicateStrategy::nearest;
    if (!consistent) {
      continue;
    }
    GistCandidate candidate;
    candidate.locator = spatial_candidate.locator;
    candidate.key = spatial_candidate.mbr;
    candidate.distance = spatial_candidate.distance;
    if (request.strategy == GistPredicateStrategy::nearest) {
      candidate.distance =
          request.opclass.methods.distance(compressed, request.query_point);
      result.opclass_distance_used = true;
      if (!std::isfinite(candidate.distance)) {
        return QueryFailure(
            "INDEX.GIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
            "index.gist_physical_provider.invalid_distance_key");
      }
    }
    candidate.opclass_consistent = true;
    candidate.source_recheck_evidence_ref =
        spatial_candidate.source_recheck_evidence_ref;
    result.candidates.push_back(std::move(candidate));
  }
  if (request.strategy == GistPredicateStrategy::nearest) {
    std::sort(result.candidates.begin(), result.candidates.end(),
              [](const auto& left, const auto& right) {
      if (left.distance != right.distance) return left.distance < right.distance;
      return LocatorLess(left.locator, right.locator);
    });
    if (request.top_k != 0 && result.candidates.size() > request.top_k) {
      result.candidates.resize(request.top_k);
    }
  } else {
    std::sort(result.candidates.begin(), result.candidates.end(),
              [](const auto& left, const auto& right) {
      return LocatorLess(left.locator, right.locator);
    });
  }
  return result;
}

GistMutationResult ApplyGistPhysicalMutation(
    const GistPhysicalProvider& provider,
    const GistOpclassRuntime& opclass,
    const GistMutation& mutation) {
  if (!ProviderValid(provider) ||
      !OpclassRuntimeSafe(opclass) ||
      !SameOpclassDescriptor(provider.opclass, opclass.descriptor)) {
    return MutationFailure("INDEX.GIST_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
                           "index.gist_physical_provider.runtime_refused");
  }
  if (!RecheckProofValid(mutation.recheck_proof)) {
    return MutationFailure("INDEX.GIST_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
                           "index.gist_physical_provider.missing_exact_recheck");
  }
  if ((mutation.expected_provider_generation_present &&
       mutation.expected_provider_generation != provider.provider_generation) ||
      (mutation.expected_opclass_epoch_present &&
       mutation.expected_opclass_epoch != provider.opclass.opclass_epoch) ||
      (mutation.expected_resource_epoch_present &&
       mutation.expected_resource_epoch != provider.opclass.resource_epoch) ||
      (mutation.expected_descriptor_epoch_present &&
       mutation.expected_descriptor_epoch !=
           provider.physical_tree.descriptor.descriptor_epoch) ||
      (mutation.expected_srid_resource_epoch_present &&
       mutation.expected_srid_resource_epoch !=
           provider.physical_tree.srid_resource.resource_epoch)) {
    return MutationFailure("INDEX.GIST_PHYSICAL_PROVIDER.STALE_EPOCH",
                           "index.gist_physical_provider.stale_epoch");
  }

  SpatialRTreeMutation spatial;
  spatial.kind = mutation.kind == GistMutationKind::insert_row
                     ? SpatialRTreeMutationKind::insert_row
                     : mutation.kind == GistMutationKind::delete_row
                           ? SpatialRTreeMutationKind::delete_row
                           : SpatialRTreeMutationKind::update_row;
  spatial.expected_provider_generation_present = true;
  spatial.expected_provider_generation = provider.provider_generation;
  spatial.expected_descriptor_epoch_present = true;
  spatial.expected_descriptor_epoch =
      provider.physical_tree.descriptor.descriptor_epoch;
  spatial.expected_srid_resource_epoch_present = true;
  spatial.expected_srid_resource_epoch =
      provider.physical_tree.srid_resource.resource_epoch;
  spatial.recheck_proof = ToSpatialProof(mutation.recheck_proof);
  if (mutation.before_row_present) {
    const auto key = CompressChecked(opclass, mutation.before_row.key);
    if (!GistCompressedKeyValid(key, opclass.descriptor)) {
      return MutationFailure(
          "INDEX.GIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
          "index.gist_physical_provider.invalid_before_key");
    }
    spatial.before_row_present = true;
    spatial.before_row = {
        mutation.before_row.locator,
        opclass.methods.decompress(key),
        mutation.before_row.exact_source_recheck_evidence_ref};
  }
  if (mutation.after_row_present) {
    const auto key = CompressChecked(opclass, mutation.after_row.key);
    if (!GistCompressedKeyValid(key, opclass.descriptor)) {
      return MutationFailure(
          "INDEX.GIST_PHYSICAL_PROVIDER.INVALID_COMPRESSED_KEY",
          "index.gist_physical_provider.invalid_after_key");
    }
    spatial.after_row_present = true;
    spatial.after_row = {
        mutation.after_row.locator,
        opclass.methods.decompress(key),
        mutation.after_row.exact_source_recheck_evidence_ref};
  }

  const auto applied =
      ApplySpatialRTreePhysicalMutation(provider.physical_tree, spatial);
  if (!applied.ok()) {
    GistMutationResult result = MutationFailure(
        "INDEX.GIST_PHYSICAL_PROVIDER.SPATIAL_RTREE_REFUSED",
        "index.gist_physical_provider.spatial_rtree_refused");
    result.diagnostic = applied.diagnostic;
    return result;
  }
  GistPhysicalProvider next = provider;
  next.physical_tree = applied.provider;
  next.provider_generation = applied.provider.provider_generation;
  next.compress_call_count +=
      static_cast<u64>(mutation.before_row_present) +
      static_cast<u64>(mutation.after_row_present);
  next.decompress_call_count +=
      static_cast<u64>(mutation.before_row_present) +
      static_cast<u64>(mutation.after_row_present);
  SetProviderEvidence(&next);
  if (!ProviderValid(next)) {
    return MutationFailure("INDEX.GIST_PHYSICAL_PROVIDER.MUTATION_CORRUPT",
                           "index.gist_physical_provider.mutation_corrupt");
  }
  GistMutationResult result;
  result.status = OkStatus();
  result.provider = std::move(next);
  result.applied = true;
  result.fail_closed = false;
  result.spatial_rtree_provider_used = true;
  result.split_performed = applied.split_performed;
  result.merge_performed = applied.merge_performed;
  result.tombstone_written = applied.tombstone_written;
  result.actions.push_back("apply_gist_physical_mutation");
  return result;
}

const char* GistOpenClassName(GistOpenClass open_class) {
  switch (open_class) {
    case GistOpenClass::current: return "current";
    case GistOpenClass::stale_format: return "stale_format";
    case GistOpenClass::stale_generation: return "stale_generation";
    case GistOpenClass::bad_checksum: return "bad_checksum";
    case GistOpenClass::corrupt_payload: return "corrupt_payload";
    case GistOpenClass::identity_mismatch: return "identity_mismatch";
    case GistOpenClass::stale_opclass_epoch: return "stale_opclass_epoch";
    case GistOpenClass::stale_resource_epoch: return "stale_resource_epoch";
    case GistOpenClass::stale_descriptor_epoch:
      return "stale_descriptor_epoch";
    case GistOpenClass::stale_srid_resource_epoch:
      return "stale_srid_resource_epoch";
    case GistOpenClass::unsafe_opclass: return "unsafe_opclass";
    case GistOpenClass::invalid_compressed_key:
      return "invalid_compressed_key";
    case GistOpenClass::incompatible_spatial_profile:
      return "incompatible_spatial_profile";
    case GistOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case GistOpenClass::missing_recheck_proof:
      return "missing_recheck_proof";
    case GistOpenClass::refused: return "refused";
  }
  return "unknown";
}

DiagnosticRecord MakeGistPhysicalProviderDiagnostic(Status status,
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
                        "core.index.gist_physical_provider");
}

}  // namespace scratchbird::core::index
