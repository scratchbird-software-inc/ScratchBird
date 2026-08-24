// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "model_family_coordinator.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// Raw, family-local observations. These are catalog/statistics/property facts,
// not optimizer alternatives: the factory owns alternative identities,
// implementation identities, route classification, cost vectors, and ranking
// eligibility.
struct ModelFamilyMetricSnapshotV1 {
  std::string statistics_snapshot_uuid;
  std::string property_snapshot_uuid;
  std::string calibration_profile_uuid;
  std::uint64_t statistics_generation{0};
  std::uint32_t confidence_basis_points{0};
  std::uint64_t startup_events{0};
  std::uint64_t estimated_rows{0};
  std::uint64_t sequential_pages{0};
  std::uint64_t random_page_lookups{0};
  std::uint64_t page_writes{0};
  std::uint64_t cache_operations{0};
  std::uint64_t working_set_bytes{0};
  std::uint64_t memory_grant_units{0};
  std::uint64_t spill_bytes{0};
  std::uint64_t network_bytes{0};
  std::uint64_t compressed_bytes{0};
  std::uint64_t encrypted_bytes{0};
  std::uint64_t predicate_evaluations{0};
  std::uint64_t vector_distance_evaluations{0};
  std::uint64_t text_score_evaluations{0};
  std::uint64_t spatial_evaluations{0};
  std::uint64_t udr_invocations{0};
  std::uint64_t mga_rechecks{0};
  std::uint64_t index_maintenance_operations{0};
  std::uint64_t uncertainty_events{0};
  std::uint64_t risk_events{0};
};

// Engine-owned availability evidence for one optimizer-defined route class.
// No implementation, alternative, transformation, or cost-vector identity is
// accepted from the caller.
struct ModelFamilyCapabilitySnapshotV1 {
  ModelFamilyAlternativeRouteClassV1 route_class{
      ModelFamilyAlternativeRouteClassV1::kNative};
  std::string provider_uuid;
  std::string capability_uuid;
  std::uint64_t provider_generation{0};
  bool available{false};
  bool exact{true};
  bool residual_recheck_required{true};
  bool base_row_mga_recheck_required{true};
  bool security_recheck_required{true};
  bool engine_owned{true};
  bool local_scope{true};
  bool parser_planning_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  ModelFamilyMetricSnapshotV1 metrics;
};

struct ModelFamilyProfileFactoryRequestV1 {
  std::uint16_t abi_version{1};
  std::string identity_scope;
  ModelFamilyCoordinatorRequestV1 logical_request;
  std::vector<ModelFamilyCapabilitySnapshotV1> capability_snapshots;
  bool engine_owned{true};
  bool parser_profile_authority_claimed{false};
};

struct ModelFamilyProfileFactoryResultV1 {
  bool accepted{false};
  bool optimizer_owned_enumeration{false};
  bool deterministic{false};
  bool data_access_allowed{false};
  std::uint32_t native_alternative_count{0};
  std::uint32_t exact_fallback_alternative_count{0};
  std::string candidate_inventory_receipt_uuid;
  std::vector<ModelFamilyCandidateV1> candidates;
  std::string diagnostic_id;
  std::string detail;
};

ModelFamilyProfileFactoryResultV1 BuildModelFamilyAlternativeProfilesV1(
    const ModelFamilyProfileFactoryRequestV1& request);

// Production entry point: candidate records must be absent from the logical
// request. The factory enumerates the finite family-local domain before the
// existing coordinator validates, ranks, and publishes the selected DAG.
ModelFamilyCoordinatorResultV1 PlanOptimizerOwnedModelFamilySourceV1(
    const ModelFamilyProfileFactoryRequestV1& request);

const char* ModelFamilyAlternativeRouteClassNameV1(
    ModelFamilyAlternativeRouteClassV1 route_class);

}  // namespace scratchbird::engine::optimizer
