// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "spatial_api.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>

namespace scratchbird::engine::internal_api::nosql {
namespace {

bool CanonicalUuid(std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) continue;
    if (!((value[i] >= '0' && value[i] <= '9') ||
          (value[i] >= 'a' && value[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

void PutU64Be(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

std::uint64_t GetU64Be(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value = (value << 8) | bytes[offset + i];
  }
  return value;
}

bool CandidateProofUsable(const SpatialCandidateProofV1& proof) {
  return proof.present && proof.fresh && proof.exact_bounds &&
         proof.source_generation_matches &&
         proof.catalog_generation_matches && proof.snapshot_safe;
}

}  // namespace

std::vector<std::uint8_t> EncodeSpatialPoint2dV1(SpatialPoint2dV1 point) {
  if (!std::isfinite(point.x) || !std::isfinite(point.y)) return {};
  if (point.x == 0.0) point.x = 0.0;
  if (point.y == 0.0) point.y = 0.0;
  std::vector<std::uint8_t> out = {'S', 'B', 'P', '1', 1, 2, 0, 0};
  PutU64Be(&out, std::bit_cast<std::uint64_t>(point.x));
  PutU64Be(&out, std::bit_cast<std::uint64_t>(point.y));
  return out;
}

bool DecodeSpatialPoint2dV1(const std::vector<std::uint8_t>& bytes,
                            SpatialPoint2dV1* point) {
  if (point == nullptr || bytes.size() != 24 || bytes[0] != 'S' ||
      bytes[1] != 'B' || bytes[2] != 'P' || bytes[3] != '1' ||
      bytes[4] != 1 || bytes[5] != 2 || bytes[6] != 0 || bytes[7] != 0) {
    return false;
  }
  SpatialPoint2dV1 decoded{
      std::bit_cast<double>(GetU64Be(bytes, 8)),
      std::bit_cast<double>(GetU64Be(bytes, 16))};
  if (!std::isfinite(decoded.x) || !std::isfinite(decoded.y)) return false;
  if (decoded.x == 0.0) decoded.x = 0.0;
  if (decoded.y == 0.0) decoded.y = 0.0;
  if (bytes != EncodeSpatialPoint2dV1(decoded)) return false;
  *point = decoded;
  return true;
}

SpatialExecutionResultV1 ExecuteSpatialNativeV1(
    const SpatialExecutionRequestV1& request) {
  SpatialExecutionResultV1 result;
  const auto refuse = [&](std::string diagnostic, std::string detail) {
    result.rows.clear();
    result.diagnostic_id = std::move(diagnostic);
    result.detail = std::move(detail);
    return result;
  };
  const bool source = request.operation_id == "SPATIAL_SOURCE";
  const bool match = request.operation_id == "SPATIAL_MATCH";
  const bool nearest = request.operation_id == "SPATIAL_NEAREST";
  if (request.abi_version != 1 ||
      request.profile_id != kSpatialNativeCartesianPoint2dV1 ||
      (!source && !match && !nearest) || !CanonicalUuid(request.object_uuid) ||
      !CanonicalUuid(request.geometry_descriptor_uuid) ||
      !CanonicalUuid(request.geometry_type_uuid) ||
      !CanonicalUuid(request.crs_uuid) || request.crs_generation == 0 ||
      request.source_generation == 0 || request.catalog_generation == 0 ||
      request.policy_generation == 0 || request.security_generation == 0 ||
      request.resource_generation == 0 || request.route_generation == 0 ||
      request.maximum_rows == 0 || !request.security_admitted ||
      request.parser_execution_authority_claimed ||
      request.provider_visibility_authority_claimed ||
      request.provider_finality_authority_claimed) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "spatial request is incomplete or claims forbidden authority");
  }
  if (!executor::PhysicalMgaStatementContextValid(request.statement_context) ||
      !executor::PhysicalMgaStatementContextValid(
          request.current_statement_context) ||
      !executor::PhysicalMgaStatementContextEqual(
          request.statement_context, request.current_statement_context)) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "spatial execution statement context changed");
  }
  if (match && request.predicate_id != "INTERSECTS" &&
      request.predicate_id != "CONTAINS") {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "only point INTERSECTS and CONTAINS are admitted");
  }
  if (nearest && (request.top_k == 0 || request.top_k > 4096)) {
    return refuse("SB_MODEL_SPATIAL_TOP_K_REFUSED_V1",
                  "spatial nearest top_k is outside 1..4096");
  }
  if (match || nearest) {
    if (!CanonicalUuid(request.query_crs_uuid)) {
      return refuse("SB_MODEL_SPATIAL_CRS_BINDING_REQUIRED_V1",
                    "spatial query CRS must be bound explicitly");
    }
    if (request.query_crs_uuid != request.crs_uuid) {
      return refuse("SB_MODEL_JOIN_SPATIAL_CRS_REFUSED_V1",
                    "spatial query and source CRS identities differ");
    }
  }
  SpatialPoint2dV1 query_point;
  if ((match || nearest) &&
      !DecodeSpatialPoint2dV1(request.encoded_query_point, &query_point)) {
    return refuse("SB_MODEL_SPATIAL_COORDINATE_INVALID_V1",
                  "query point is not canonical SBP1");
  }
  if (request.source_rows.size() > request.maximum_rows) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "spatial source exceeds bounded row grant");
  }

  std::set<std::string> row_uuids;
  std::vector<SpatialPoint2dV1> decoded_points;
  decoded_points.reserve(request.source_rows.size());
  for (const auto& row : request.source_rows) {
    SpatialPoint2dV1 point;
    if (row.value_profile_id != kSpatialNativeCartesianPoint2dV1) {
      return refuse("SB_MODEL_SPATIAL_PROFILE_UNSUPPORTED_V1",
                    "only the native Cartesian POINT profile is admitted");
    }
    if (!CanonicalUuid(row.row_uuid) ||
        !row_uuids.insert(row.row_uuid).second ||
        row.crs_uuid != request.crs_uuid ||
        !DecodeSpatialPoint2dV1(row.encoded_point, &point)) {
      return refuse(row.crs_uuid != request.crs_uuid
                        ? "SB_MODEL_SPATIAL_CRS_MISMATCH_V1"
                        : "SB_MODEL_SPATIAL_COORDINATE_INVALID_V1",
                    "spatial source row identity, CRS, or point is invalid");
    }
    decoded_points.push_back(point);
  }

  std::vector<std::size_t> ordinals;
  if (!source && CandidateProofUsable(request.candidate_proof)) {
    std::set<std::size_t> unique;
    for (const auto ordinal : request.candidate_proof.candidate_row_ordinals) {
      if (ordinal >= request.source_rows.size() || !unique.insert(ordinal).second) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "spatial candidate receipt is malformed");
      }
      ordinals.push_back(ordinal);
    }
    result.physical_operator_id = "PHYSICAL_SPATIAL_CANDIDATE_SCAN_V1";
  } else {
    if (!request.exact_scan_fallback_available) {
      return refuse("SB_MODEL_SPATIAL_EXACT_FALLBACK_UNAVAILABLE_V1",
                    "exact geometry scan fallback is unavailable");
    }
    ordinals.resize(request.source_rows.size());
    std::iota(ordinals.begin(), ordinals.end(), 0);
    result.exact_fallback_selected = true;
    result.physical_operator_id = "SPATIAL_EXACT_GEOMETRY_SCAN_V1";
  }

  for (const auto ordinal : ordinals) {
    const auto& source_row = request.source_rows[ordinal];
    const auto& point = decoded_points[ordinal];
    SpatialResultRowV1 row{source_row.row_uuid, source_row.encoded_point,
                           source_row.crs_uuid, false, 0.0};
    if (match) {
      row.predicate_truth = point.x == query_point.x && point.y == query_point.y;
      if (!row.predicate_truth) continue;
    } else if (nearest) {
      row.distance = std::hypot(point.x - query_point.x,
                                point.y - query_point.y);
      if (!std::isfinite(row.distance)) {
        return refuse("SB_MODEL_SPATIAL_COORDINATE_INVALID_V1",
                      "spatial distance overflowed");
      }
    }
    result.rows.push_back(std::move(row));
  }
  if (nearest) {
    std::sort(result.rows.begin(), result.rows.end(),
              [](const auto& left, const auto& right) {
                return left.distance < right.distance ||
                       (left.distance == right.distance &&
                        left.row_uuid < right.row_uuid);
              });
    if (result.rows.size() > request.top_k) result.rows.resize(request.top_k);
  }
  result.accepted = true;
  result.root_publishable = true;
  result.candidate_recheck_complete = true;
  result.mga_recheck_complete = true;
  result.diagnostic_id = "SB_EXECUTOR_OK";
  return result;
}

}  // namespace scratchbird::engine::internal_api::nosql
