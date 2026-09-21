// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../api_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class RelationalWindowFrameUnit : std::uint8_t {
  kRows = 1,
  kRange,
  kGroups,
};

enum class RelationalWindowFrameBoundKind : std::uint8_t {
  kUnboundedPreceding = 1,
  kPreceding,
  kCurrentRow,
  kFollowing,
  kUnboundedFollowing,
};

enum class RelationalWindowFrameExclusion : std::uint8_t {
  kNoOthers = 1,
  kCurrentRow,
  kGroup,
  kTies,
};

struct RelationalWindowFrameBoundRecord {
  RelationalWindowFrameBoundKind bound_kind{
      RelationalWindowFrameBoundKind::kCurrentRow};
  std::optional<std::uint32_t> offset_expression_id;
};

enum class RelationalPropertySortDirection : std::uint8_t {
  kAscending = 1,
  kDescending,
};

enum class RelationalPropertyNullPlacement : std::uint8_t {
  kNullsFirst = 1,
  kNullsLast,
};

struct RelationalPropertyOrderingTerm {
  std::uint32_t expression_id{0};
  RelationalPropertySortDirection direction{
      RelationalPropertySortDirection::kAscending};
  RelationalPropertyNullPlacement null_placement{
      RelationalPropertyNullPlacement::kNullsLast};
  EngineUuid collation_uuid;
};

struct RelationalWindowDefinitionRecord {
  std::uint32_t window_id{0};
  std::uint32_t relation_node_id{0};
  std::optional<std::string> canonical_name_key;
  std::optional<std::uint32_t> inherited_window_id;
  std::vector<std::uint32_t> partition_expression_ids;
  std::vector<RelationalPropertyOrderingTerm> ordering_terms;
  std::optional<RelationalWindowFrameUnit> frame_unit;
  std::optional<RelationalWindowFrameBoundRecord> frame_start;
  std::optional<RelationalWindowFrameBoundRecord> frame_end;
  RelationalWindowFrameExclusion exclusion{
      RelationalWindowFrameExclusion::kNoOthers};
};

}  // namespace scratchbird::engine::internal_api
