// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../../executor/physical_node_abi.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api::nosql {

inline constexpr std::string_view kSpatialNativeCartesianPoint2dV1 =
    "SPATIAL_NATIVE_CARTESIAN_POINT_2D_V1";

struct SpatialPoint2dV1 {
  double x{0.0};
  double y{0.0};
};

struct SpatialSourceRowV1 {
  std::string row_uuid;
  std::vector<std::uint8_t> encoded_point;
  std::string crs_uuid;
  std::string value_profile_id{std::string(kSpatialNativeCartesianPoint2dV1)};
};

struct SpatialCandidateProofV1 {
  bool present{false};
  bool fresh{false};
  bool exact_bounds{false};
  bool source_generation_matches{false};
  bool catalog_generation_matches{false};
  bool snapshot_safe{false};
  std::vector<std::size_t> candidate_row_ordinals;
};

struct SpatialExecutionRequestV1 {
  std::uint16_t abi_version{1};
  std::string profile_id{std::string(kSpatialNativeCartesianPoint2dV1)};
  std::string operation_id;
  std::string predicate_id;
  std::string object_uuid;
  std::string geometry_descriptor_uuid;
  std::string geometry_type_uuid;
  std::string crs_uuid;
  std::string query_crs_uuid;
  std::uint64_t crs_generation{0};
  std::uint64_t source_generation{0};
  std::uint64_t catalog_generation{0};
  std::uint64_t policy_generation{0};
  std::uint64_t security_generation{0};
  std::uint64_t resource_generation{0};
  std::uint64_t route_generation{0};
  executor::PhysicalMgaStatementContext statement_context;
  executor::PhysicalMgaStatementContext current_statement_context;
  std::vector<SpatialSourceRowV1> source_rows;
  std::vector<std::uint8_t> encoded_query_point;
  std::uint32_t top_k{0};
  std::size_t maximum_rows{0};
  bool security_admitted{false};
  bool exact_scan_fallback_available{false};
  bool parser_execution_authority_claimed{false};
  bool provider_visibility_authority_claimed{false};
  bool provider_finality_authority_claimed{false};
  SpatialCandidateProofV1 candidate_proof;
};

struct SpatialResultRowV1 {
  std::string row_uuid;
  std::vector<std::uint8_t> encoded_point;
  std::string crs_uuid;
  bool predicate_truth{false};
  double distance{0.0};
};

struct SpatialExecutionResultV1 {
  bool accepted{false};
  bool root_publishable{false};
  bool exact_fallback_selected{false};
  bool candidate_recheck_complete{false};
  bool mga_recheck_complete{false};
  std::vector<SpatialResultRowV1> rows;
  std::string physical_operator_id;
  std::string diagnostic_id;
  std::string detail;
};

std::vector<std::uint8_t> EncodeSpatialPoint2dV1(SpatialPoint2dV1 point);

bool DecodeSpatialPoint2dV1(const std::vector<std::uint8_t>& bytes,
                            SpatialPoint2dV1* point);

SpatialExecutionResultV1 ExecuteSpatialNativeV1(
    const SpatialExecutionRequestV1& request);

}  // namespace scratchbird::engine::internal_api::nosql
