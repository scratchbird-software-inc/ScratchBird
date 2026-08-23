// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_catalog_backed_planning.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// Engine capability input to the optimizer-owned profile factory.  These
// records describe executable implementations; they are not alternatives,
// costs, transformations, or selected plans.  The factory derives those
// optimizer identities from the admitted graph and its snapshots.
struct CanonicalOptimizerImplementationProfile {
  std::uint32_t logical_node_id{0};
  std::string implementation_id;
  std::string capability_uuid;
  scratchbird::engine::planner::CanonicalLogicalRelationalNodeKind
      logical_node_kind{
          scratchbird::engine::planner::CanonicalLogicalRelationalNodeKind::
              kValues};
  scratchbird::engine::executor::PhysicalNodeKind physical_node_kind{
      scratchbird::engine::executor::PhysicalNodeKind::kValues};
  std::string transformation_rule_id;
  std::size_t minimum_input_count{0};
  std::size_t maximum_input_count{0};
  std::vector<std::string> required_property_uuids;
  std::vector<std::string> delivered_property_uuids;
  std::vector<scratchbird::engine::planner::CanonicalLogicalPropertyKind>
      supported_property_kinds;
  std::uint64_t estimated_rows_hint{0};
  std::uint64_t memory_bytes_required{0};
  std::uint64_t page_read_sequential_units{0};
  std::uint64_t page_read_random_units{0};
  std::uint64_t page_write_units{0};
  std::uint64_t cache_units{0};
  std::uint64_t memory_grant_units{0};
  std::uint64_t spill_units{0};
  std::uint64_t network_units{0};
  std::uint64_t compression_units{0};
  std::uint64_t encryption_units{0};
  std::uint64_t predicate_evaluation_units{0};
  std::uint64_t vector_distance_units{0};
  std::uint64_t text_scoring_units{0};
  std::uint64_t spatial_evaluation_units{0};
  std::uint64_t udr_invocation_units{0};
  std::uint64_t mga_units{0};
  std::uint64_t index_maintenance_units{0};
  std::uint64_t mga_visibility_checks_expected{0};
  bool storage_read_capable{false};
  bool mga_visibility_capable{false};
  bool spill_supported{false};
  bool parallel_safe{false};
  bool parallel_required{false};
  bool residual_predicate_required{false};
  bool storage_recheck_required{false};
  std::string compatibility_profile_id{"native.sblr.row.v1"};
  std::string model_family_id{"relational.local.v1"};
  bool available{true};
  std::string refusal_diagnostic_id;
};

struct CanonicalOptimizerProfileFactoryIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string implementation_id;
  std::string field_id;
};

struct CanonicalOptimizerProfileFactoryResult {
  bool accepted{false};
  bool optimizer_owned_enumeration{false};
  bool snapshot_derived{false};
  bool deterministic{false};
  bool data_access_allowed{false};
  CanonicalOptimizerAlternativeInventoryResult inventory;
  CanonicalExecutorCapabilityCatalog capability_catalog;
  std::vector<CanonicalOptimizerSearchCandidateInput> candidates;
  std::vector<CanonicalOptimizerProfileFactoryIssue> issues;
};

// Builds the complete finite domain, validates it, and creates costed search
// candidates.  Callers publish capabilities and runtime implementations only;
// they cannot author alternative, transformation, or cost-vector identities.
CanonicalOptimizerProfileFactoryResult
BuildCanonicalOptimizerAlternativeProfiles(
    const CanonicalOptimizerAdmissionRequest& admission_request,
    const CanonicalOptimizerAdmissionResult& admission,
    const std::vector<CanonicalOptimizerImplementationProfile>&
        implementations,
    std::string identity_scope,
    std::string calibration_profile_uuid);

}  // namespace scratchbird::engine::optimizer
