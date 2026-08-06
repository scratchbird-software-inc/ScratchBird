// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "logical_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

struct CanonicalRelationalCandidateLegalityIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string alternative_uuid;
  std::string field_id;
};

struct CanonicalRelationalCandidateLegalityRecord {
  std::string alternative_uuid;
  std::uint32_t logical_node_id{0};
  bool legal{false};
  bool property_enforcement_required{false};
  std::vector<std::string> enforced_property_uuids;
  std::vector<std::string> missing_property_uuids;
  std::string refusal_diagnostic_id;
};

struct CanonicalRelationalCandidateLegalityResult {
  bool accepted{false};
  bool data_access_allowed{false};
  bool complete_legal_coverage{false};
  std::size_t selectable_candidate_count{0};
  std::vector<CanonicalRelationalCandidateLegalityRecord> candidates;
  std::vector<CanonicalRelationalCandidateLegalityIssue> issues;
};

enum class CanonicalWindowPropertyEnforcementKind : std::uint8_t {
  kReuse = 1,
  kSort,
  kRepartition,
  kRepartitionAndSort,
};

struct CanonicalWindowPropertyScheduleStage {
  std::uint32_t logical_node_id{0};
  std::string window_property_uuid;
  CanonicalWindowPropertyEnforcementKind enforcement_kind{
      CanonicalWindowPropertyEnforcementKind::kReuse};
  std::vector<std::string> reused_property_uuids;
  std::vector<std::string> enforced_property_uuids;
  std::uint64_t estimated_cost_units{0};
};

struct CanonicalWindowPropertyScheduleResult {
  bool accepted{false};
  bool complete_legal_schedule{false};
  bool data_access_allowed{false};
  std::size_t reused_stage_count{0};
  std::size_t sort_stage_count{0};
  std::size_t repartition_stage_count{0};
  std::uint64_t estimated_cost_units{0};
  std::vector<CanonicalWindowPropertyScheduleStage> stages;
  std::vector<CanonicalRelationalCandidateLegalityIssue> issues;
};

CanonicalRelationalCandidateLegalityResult
EvaluateCanonicalRelationalCandidateLegality(
    const scratchbird::engine::planner::CanonicalLogicalRelationalGraph& graph,
    const scratchbird::engine::planner::CanonicalLogicalPropertyCatalog&
        properties,
    const scratchbird::engine::planner::CanonicalPhysicalAlternativeCatalog&
        alternatives);

CanonicalWindowPropertyScheduleResult PlanCanonicalWindowPropertySchedule(
    const scratchbird::engine::planner::CanonicalLogicalRelationalGraph& graph,
    const scratchbird::engine::planner::CanonicalLogicalPropertyCatalog&
        properties,
    std::uint64_t estimated_input_rows);

}  // namespace scratchbird::engine::optimizer
