// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "spatial_api.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>

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

bool CheckedAdd(const std::uint64_t left, const std::uint64_t right,
                std::uint64_t* out) {
  if (out == nullptr ||
      left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  *out = left + right;
  return true;
}

bool CheckedMultiply(const std::uint64_t left, const std::uint64_t right,
                     std::uint64_t* out) {
  if (out == nullptr ||
      (left != 0 &&
       right > std::numeric_limits<std::uint64_t>::max() / left)) {
    return false;
  }
  *out = left * right;
  return true;
}

bool AccountArray(const std::size_t count, const std::size_t element_bytes,
                  std::uint64_t* bytes) {
  std::uint64_t allocation = 0;
  return bytes != nullptr &&
         CheckedMultiply(count, element_bytes, &allocation) &&
         CheckedAdd(*bytes, allocation, bytes);
}

bool AccountString(const std::string& value, std::uint64_t* bytes) {
  return bytes != nullptr && CheckedAdd(*bytes, value.capacity(), bytes) &&
         CheckedAdd(*bytes, 1, bytes);
}

bool AccountBytes(const std::vector<std::uint8_t>& value,
                  std::uint64_t* bytes) {
  return bytes != nullptr &&
         CheckedAdd(*bytes, value.capacity(), bytes);
}

bool AccountMgaContext(const executor::PhysicalMgaStatementContext& context,
                       std::uint64_t* bytes) {
  return AccountString(context.statement_uuid, bytes) &&
         AccountString(context.owning_transaction_uuid, bytes) &&
         AccountString(context.statement_snapshot_uuid, bytes) &&
         AccountString(context.statement_metadata_snapshot_uuid, bytes) &&
         AccountArray(context.active_excluded_local_transaction_ids.capacity(),
                      sizeof(std::uint64_t), bytes) &&
         AccountArray(
             context.in_doubt_excluded_local_transaction_ids.capacity(),
             sizeof(std::uint64_t), bytes) &&
         AccountString(context.snapshot_kind, bytes) &&
         AccountString(context.statement_timestamp, bytes);
}

bool AccountSourceRows(const std::vector<SpatialSourceRowV1>& rows,
                       std::uint64_t* bytes) {
  if (!AccountArray(rows.capacity(), sizeof(SpatialSourceRowV1), bytes)) {
    return false;
  }
  for (const auto& row : rows) {
    if (!AccountString(row.row_uuid, bytes) ||
        !AccountBytes(row.encoded_point, bytes) ||
        !AccountString(row.crs_uuid, bytes) ||
        !AccountString(row.value_profile_id, bytes)) {
      return false;
    }
  }
  return true;
}

std::optional<std::uint64_t> RequestCarrierMemoryBytes(
    const SpatialExecutionRequestV2& request) {
  std::uint64_t bytes = sizeof(request);
  if (!AccountString(request.profile_id, &bytes) ||
      !AccountString(request.operation_id, &bytes) ||
      !AccountString(request.predicate_id, &bytes) ||
      !AccountString(request.object_uuid, &bytes) ||
      !AccountString(request.geometry_descriptor_uuid, &bytes) ||
      !AccountString(request.geometry_type_uuid, &bytes) ||
      !AccountString(request.crs_uuid, &bytes) ||
      !AccountString(request.query_crs_uuid, &bytes) ||
      !AccountMgaContext(request.statement_context, &bytes) ||
      !AccountMgaContext(request.current_statement_context, &bytes) ||
      !AccountSourceRows(request.source_rows, &bytes) ||
      !AccountBytes(request.encoded_query_point, &bytes) ||
      !AccountArray(request.candidate_proof.candidate_row_ordinals.capacity(),
                    sizeof(std::size_t), &bytes)) {
    return std::nullopt;
  }
  return bytes;
}

std::optional<std::uint64_t> ResultCarrierMemoryBytes(
    const SpatialExecutionResultV2& result) {
  std::uint64_t bytes = sizeof(result);
  if (!AccountArray(result.rows.capacity(), sizeof(SpatialResultRowV1),
                    &bytes) ||
      !AccountString(result.physical_operator_id, &bytes) ||
      !AccountString(result.diagnostic_id, &bytes) ||
      !AccountString(result.detail, &bytes)) {
    return std::nullopt;
  }
  for (const auto& row : result.rows) {
    if (!AccountString(row.row_uuid, &bytes) ||
        !AccountBytes(row.encoded_point, &bytes) ||
        !AccountString(row.crs_uuid, &bytes)) {
      return std::nullopt;
    }
  }
  return bytes;
}

std::optional<std::uint64_t> ProjectedExecutionPeakBytes(
    const SpatialExecutionRequestV2& request) {
  const auto request_memory = RequestCarrierMemoryBytes(request);
  if (!request_memory.has_value()) return std::nullopt;
  std::uint64_t result_base = sizeof(SpatialExecutionResultV2);
  constexpr std::uint64_t kResultStringBudget = 512;
  if (!CheckedAdd(result_base, kResultStringBudget, &result_base)) {
    return std::nullopt;
  }
  std::uint64_t decoded_points = 0;
  std::uint64_t ordinals = 0;
  std::uint64_t uuid_set = 0;
  std::uint64_t candidate_set = 0;
  std::uint64_t result_rows = 0;
  constexpr std::uint64_t kSetNodeOverhead = 6 * sizeof(void*);
  if (!CheckedMultiply(request.source_rows.size(), sizeof(SpatialPoint2dV1),
                       &decoded_points) ||
      !CheckedMultiply(request.source_rows.size(), sizeof(std::size_t),
                       &ordinals) ||
      !CheckedMultiply(request.source_rows.size(),
                       sizeof(std::string) + kSetNodeOverhead,
                       &uuid_set) ||
      !CheckedMultiply(request.source_rows.size(),
                       sizeof(std::size_t) + kSetNodeOverhead,
                       &candidate_set) ||
      !CheckedMultiply(request.source_rows.size(),
                       sizeof(SpatialResultRowV1), &result_rows)) {
    return std::nullopt;
  }
  for (const auto& row : request.source_rows) {
    if (!CheckedAdd(uuid_set, row.row_uuid.size() + 1, &uuid_set) ||
        !CheckedAdd(result_rows, row.row_uuid.size() + 1, &result_rows) ||
        !CheckedAdd(result_rows, row.encoded_point.size(), &result_rows) ||
        !CheckedAdd(result_rows, row.crs_uuid.size() + 1, &result_rows)) {
      return std::nullopt;
    }
  }
  std::uint64_t validation_peak = *request_memory;
  std::uint64_t candidate_peak = *request_memory;
  std::uint64_t result_peak = *request_memory;
  constexpr std::uint64_t kDecodeTransientBytes =
      sizeof(std::vector<std::uint8_t>) + 32;
  if (!CheckedAdd(validation_peak, result_base, &validation_peak) ||
      !CheckedAdd(validation_peak, decoded_points, &validation_peak) ||
      !CheckedAdd(validation_peak, uuid_set, &validation_peak) ||
      !CheckedAdd(validation_peak, kDecodeTransientBytes,
                  &validation_peak) ||
      !CheckedAdd(candidate_peak, result_base, &candidate_peak) ||
      !CheckedAdd(candidate_peak, decoded_points, &candidate_peak) ||
      !CheckedAdd(candidate_peak, ordinals, &candidate_peak) ||
      !CheckedAdd(candidate_peak, candidate_set, &candidate_peak) ||
      !CheckedAdd(result_peak, result_base, &result_peak) ||
      !CheckedAdd(result_peak, decoded_points, &result_peak) ||
      !CheckedAdd(result_peak, ordinals, &result_peak) ||
      !CheckedAdd(result_peak, result_rows, &result_peak)) {
    return std::nullopt;
  }
  return std::max({validation_peak, candidate_peak, result_peak});
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
  if ((decoded.x == 0.0 && std::signbit(decoded.x)) ||
      (decoded.y == 0.0 && std::signbit(decoded.y))) {
    return false;
  }
  if (decoded.x == 0.0) decoded.x = 0.0;
  if (decoded.y == 0.0) decoded.y = 0.0;
  *point = decoded;
  return true;
}

bool DecodeSpatialPoint2dV1(const std::string_view bytes,
                            SpatialPoint2dV1* point) {
  if (point == nullptr || bytes.size() != 24 || bytes[0] != 'S' ||
      bytes[1] != 'B' || bytes[2] != 'P' || bytes[3] != '1' ||
      static_cast<std::uint8_t>(bytes[4]) != 1 ||
      static_cast<std::uint8_t>(bytes[5]) != 2 || bytes[6] != 0 ||
      bytes[7] != 0) {
    return false;
  }
  const auto get_u64_be = [&](const std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
      value = (value << 8) |
              static_cast<std::uint8_t>(bytes[offset + index]);
    }
    return value;
  };
  SpatialPoint2dV1 decoded{
      std::bit_cast<double>(get_u64_be(8)),
      std::bit_cast<double>(get_u64_be(16))};
  if (!std::isfinite(decoded.x) || !std::isfinite(decoded.y) ||
      (decoded.x == 0.0 && std::signbit(decoded.x)) ||
      (decoded.y == 0.0 && std::signbit(decoded.y))) {
    return false;
  }
  if (decoded.x == 0.0) decoded.x = 0.0;
  if (decoded.y == 0.0) decoded.y = 0.0;
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

static SpatialExecutionResultV2 ExecuteSpatialNativeV2Impl(
    SpatialExecutionRequestV2 request) {
  SpatialExecutionResultV2 result;
  result.memory_grant_bytes = request.maximum_memory_bytes;
  const auto refuse = [&](std::string diagnostic, std::string detail) {
    result.rows.clear();
    result.diagnostic_id = std::move(diagnostic);
    result.detail = std::move(detail);
    return std::move(result);
  };
  const auto cancelled = [&]() noexcept {
    if (!request.cancellation_requested) return false;
    try {
      if (!request.cancellation_requested(request.cancellation_context)) {
        return false;
      }
      result.cancellation_observed = true;
      return true;
    } catch (...) {
      result.cancellation_probe_failed = true;
      return true;
    }
  };
  const auto cancellation_refusal = [&] {
    return refuse(result.cancellation_probe_failed
                      ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                      : "SB_MODEL_EXECUTION_CANCELLED_V1",
                  result.cancellation_probe_failed
                      ? "spatial cancellation probe failed"
                      : "spatial execution was cancelled");
  };
  try {
    if (cancelled()) return cancellation_refusal();
    const bool source = request.operation_id == "SPATIAL_SOURCE";
    const bool match = request.operation_id == "SPATIAL_MATCH";
    const bool nearest = request.operation_id == "SPATIAL_NEAREST";
    if (request.abi_version != 2 ||
        request.profile_id != kSpatialNativeCartesianPoint2dV1 ||
        (!source && !match && !nearest) ||
        !CanonicalUuid(request.object_uuid) ||
        !CanonicalUuid(request.geometry_descriptor_uuid) ||
        !CanonicalUuid(request.geometry_type_uuid) ||
        !CanonicalUuid(request.crs_uuid) || request.crs_generation == 0 ||
        request.source_generation == 0 || request.catalog_generation == 0 ||
        request.policy_generation == 0 || request.security_generation == 0 ||
        request.resource_generation == 0 || request.route_generation == 0 ||
        request.maximum_rows == 0 || request.maximum_memory_bytes == 0 ||
        !request.cancellation_requested || !request.security_admitted ||
        request.parser_execution_authority_claimed ||
        request.provider_visibility_authority_claimed ||
        request.provider_finality_authority_claimed) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "spatial request is incomplete or claims forbidden authority");
    }
    if (!executor::PhysicalMgaStatementContextValid(
            request.statement_context) ||
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
    if (request.source_rows.size() > request.maximum_rows) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "spatial source exceeds bounded row grant");
    }
    const auto projected_peak = ProjectedExecutionPeakBytes(request);
    if (!projected_peak.has_value() ||
        *projected_peak > request.maximum_memory_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "spatial execution exceeds its byte grant");
    }
    result.peak_live_memory_bytes = *projected_peak;
    SpatialPoint2dV1 query_point;
    if ((match || nearest) &&
        !DecodeSpatialPoint2dV1(request.encoded_query_point, &query_point)) {
      return refuse("SB_MODEL_SPATIAL_COORDINATE_INVALID_V1",
                    "query point is not canonical SBP1");
    }

    std::vector<SpatialPoint2dV1> decoded_points;
    decoded_points.reserve(request.source_rows.size());
    {
      std::set<std::string> row_uuids;
      for (const auto& row : request.source_rows) {
        if (cancelled()) return cancellation_refusal();
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
    }

    std::vector<std::size_t> ordinals;
    ordinals.reserve(request.source_rows.size());
    if (!source && CandidateProofUsable(request.candidate_proof)) {
      std::set<std::size_t> unique;
      for (const auto ordinal :
           request.candidate_proof.candidate_row_ordinals) {
        if (cancelled()) return cancellation_refusal();
        if (ordinal >= request.source_rows.size() ||
            !unique.insert(ordinal).second) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        "spatial candidate receipt is malformed");
        }
        ordinals.push_back(ordinal);
      }
      result.physical_operator_id =
          "PHYSICAL_SPATIAL_CANDIDATE_SCAN_V1";
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

    result.rows.reserve(request.source_rows.size());
    for (const auto ordinal : ordinals) {
      if (cancelled()) return cancellation_refusal();
      const auto& source_row = request.source_rows[ordinal];
      const auto& point = decoded_points[ordinal];
      SpatialResultRowV1 row{source_row.row_uuid, source_row.encoded_point,
                             source_row.crs_uuid, false, 0.0};
      if (match) {
        row.predicate_truth =
            point.x == query_point.x && point.y == query_point.y;
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
      if (cancelled()) return cancellation_refusal();
      std::sort(result.rows.begin(), result.rows.end(),
                [](const auto& left, const auto& right) {
                  return left.distance < right.distance ||
                         (left.distance == right.distance &&
                          left.row_uuid < right.row_uuid);
                });
      if (cancelled()) return cancellation_refusal();
      if (result.rows.size() > request.top_k) {
        result.rows.resize(request.top_k);
      }
    }
    result.diagnostic_id = "SB_EXECUTOR_OK";
    const auto retained_memory = ResultCarrierMemoryBytes(result);
    const auto live_request_memory = RequestCarrierMemoryBytes(request);
    std::uint64_t live_result_phase = 0;
    std::uint64_t decoded_point_bytes = 0;
    std::uint64_t ordinal_bytes = 0;
    if (!retained_memory.has_value() ||
        !live_request_memory.has_value() ||
        !CheckedMultiply(decoded_points.capacity(),
                         sizeof(SpatialPoint2dV1),
                         &decoded_point_bytes) ||
        !CheckedMultiply(ordinals.capacity(), sizeof(std::size_t),
                         &ordinal_bytes) ||
        !CheckedAdd(*live_request_memory, *retained_memory,
                    &live_result_phase) ||
        !CheckedAdd(live_result_phase, decoded_point_bytes,
                    &live_result_phase) ||
        !CheckedAdd(live_result_phase, ordinal_bytes,
                    &live_result_phase) ||
        live_result_phase > request.maximum_memory_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "spatial live result phase exceeds its byte grant");
    }
    result.current_live_memory_bytes = *retained_memory;
    result.peak_live_memory_bytes =
        std::max(result.peak_live_memory_bytes, live_result_phase);
    result.memory_receipt_complete =
        result.current_live_memory_bytes <= result.peak_live_memory_bytes &&
        result.peak_live_memory_bytes <= result.memory_grant_bytes;
    if (!result.memory_receipt_complete) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "spatial memory receipt is incomplete");
    }
    if (cancelled()) return cancellation_refusal();
    result.accepted = true;
    result.root_publishable = true;
    result.candidate_recheck_complete = true;
    result.mga_recheck_complete = true;
    return std::move(result);
  } catch (const std::bad_alloc&) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "spatial execution allocation was refused");
  } catch (const std::length_error&) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "spatial execution allocation length was refused");
  } catch (const std::exception& exception) {
    return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                  std::string("spatial execution threw: ") +
                      exception.what());
  } catch (...) {
    return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                  "spatial execution threw a non-standard exception");
  }
}

SpatialExecutionResultV2 ExecuteSpatialNativeV2(
    SpatialExecutionRequestV2&& request) {
  const auto memory_grant_bytes = request.maximum_memory_bytes;
  try {
    return ExecuteSpatialNativeV2Impl(std::move(request));
  } catch (const std::bad_alloc&) {
    SpatialExecutionResultV2 result;
    result.memory_grant_bytes = memory_grant_bytes;
    result.diagnostic_id = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
    result.detail = "spatial request move allocation was refused";
    return result;
  } catch (...) {
    SpatialExecutionResultV2 result;
    result.memory_grant_bytes = memory_grant_bytes;
    result.diagnostic_id = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
    result.detail = "spatial request move failed";
    return result;
  }
}

}  // namespace scratchbird::engine::internal_api::nosql
