// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "relational_window_definition_record.hpp"

namespace scratchbird::engine::internal_api {

enum class RelationalPropertyKind : std::uint8_t {
  kOrdering = 1,
  kGrouping,
  kPartitioning,
  kWindow,
  kExpressionEquivalence,
  kDistribution,
  kUniqueness,
  kMaterialization,
  kRewindability,
  kVectorOrdering,
  kTextScoreOrdering,
  kTimeOrdering,
  kLocality,
  kSecurityVisibility,
};

enum class RelationalPropertyDistributionKind : std::uint8_t {
  kNone = 0,
  kSingle,
  kReplicated,
  kHashPartitioned,
  kRangePartitioned,
  kRoundRobin,
  kOwnerRouted,
};

enum class RelationalPropertyMaterializationKind : std::uint8_t {
  kNone = 0,
  kStreaming,
  kMaterialized,
  kSpillBacked,
};

enum class RelationalPropertyRewindabilityKind : std::uint8_t {
  kNone = 0,
  kForwardOnly,
  kRewindable,
  kMarkRestore,
};

enum class RelationalPropertyLocalityKind : std::uint8_t {
  kNone = 0,
  kLocalProcess,
  kLocalNode,
  kFilespace,
  kShardOwner,
  kRemoteAllowed,
};

struct RelationalPropertyRecord {
  // Canonical kind218 preserves all generic and specialized state as binary.
  EngineUuid property_uuid;
  RelationalPropertyKind property_kind{RelationalPropertyKind::kOrdering};
  std::uint32_t origin_node_id{0};
  std::vector<std::uint32_t> expression_ids;
  std::vector<RelationalPropertyOrderingTerm> ordering_terms;
  std::vector<EngineUuid> dependency_property_uuids;
  EngineUuid window_frame_descriptor_uuid;
  RelationalPropertyDistributionKind distribution_kind{
      RelationalPropertyDistributionKind::kNone};
  RelationalPropertyMaterializationKind materialization_kind{
      RelationalPropertyMaterializationKind::kNone};
  RelationalPropertyRewindabilityKind rewindability_kind{
      RelationalPropertyRewindabilityKind::kNone};
  RelationalPropertyLocalityKind locality_kind{
      RelationalPropertyLocalityKind::kNone};
  EngineUuid locality_uuid;
  EngineUuid security_visibility_context_uuid;
  std::uint64_t security_visibility_generation{0};
};

}  // namespace scratchbird::engine::internal_api
