// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_model_family_composition_support.hpp"
#include "canonical_query_scalar_support.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_MODEL_FAMILY_PLANNING_AUTHORITY
// Builds model capability/cost snapshots and delegates planning to the
// engine optimizer. These are not MGA visibility snapshots. Owns no storage,
// selected execution, snapshot construction, or transaction finality.

namespace {

opt::ModelFamilyCapabilitySnapshotV1 MakeModelFamilyCapabilitySnapshotV1(
    const opt::ModelFamilyCoordinatorRequestV1& planning,
    const opt::ModelFamilyAlternativeRouteClassV1 route_class,
    core::platform::Uuid provider_uuid,
    core::platform::Uuid capability_uuid,
    const std::uint64_t provider_generation,
    const bool available,
    const std::uint64_t work_units,
    const std::uint64_t sequential_pages,
    const std::uint64_t memory_bytes_required) {
  opt::ModelFamilyCapabilitySnapshotV1 snapshot;
  snapshot.route_class = route_class;
  snapshot.provider_uuid = std::move(provider_uuid);
  snapshot.capability_uuid = std::move(capability_uuid);
  snapshot.provider_generation = provider_generation;
  snapshot.available = available;
  snapshot.metrics.statistics_snapshot_uuid =
      planning.statistics_snapshot_uuid;
  const auto property_identity = core::uuid::IssueRuntimeIdentityV7();
  const auto calibration_identity = core::uuid::IssueRuntimeIdentityV7();
  if (!property_identity || !calibration_identity) {
    // An incomplete snapshot is refused by the profile factory before use.
    return {};
  }
  snapshot.metrics.property_snapshot_uuid = *property_identity;
  snapshot.metrics.calibration_profile_uuid = *calibration_identity;
  snapshot.metrics.statistics_generation = planning.statistics_generation;
  snapshot.metrics.confidence_basis_points = 9000;
  snapshot.metrics.startup_events = 1;
  snapshot.metrics.estimated_rows = std::max<std::uint64_t>(1, work_units);
  snapshot.metrics.sequential_pages = sequential_pages;
  snapshot.metrics.working_set_bytes =
      std::max<std::uint64_t>(1, memory_bytes_required);
  snapshot.metrics.memory_grant_units =
      std::max<std::uint64_t>(1, memory_bytes_required);
  snapshot.metrics.predicate_evaluations =
      planning.operation_id.find("FILTER") != std::string::npos ? work_units : 0;
  if (planning.family_id == "vector") {
    snapshot.metrics.vector_distance_evaluations =
        std::max<std::uint64_t>(1, work_units);
  } else if (planning.family_id == "search") {
    snapshot.metrics.text_score_evaluations =
        std::max<std::uint64_t>(1, work_units);
  } else if (planning.family_id == "spatial") {
    snapshot.metrics.spatial_evaluations =
        std::max<std::uint64_t>(1, work_units);
  }
  snapshot.metrics.mga_rechecks =
      std::max<std::uint64_t>(1, work_units);
  return snapshot;
}

opt::ModelFamilyCoordinatorResultV1 PlanCanonicalModelFamilySourceV1(
    opt::ModelFamilyCoordinatorRequestV1 planning,
    std::vector<opt::ModelFamilyCapabilitySnapshotV1> snapshots) {
  opt::ModelFamilyProfileFactoryRequestV1 request;
  request.logical_request = std::move(planning);
  request.capability_snapshots = std::move(snapshots);
  return opt::PlanOptimizerOwnedModelFamilySourceV1(request);
}

}  // namespace

opt::ModelFamilyCapabilitySnapshotV1
MakeModelFamilyCapabilitySnapshotForCompositionV1(
    const opt::ModelFamilyCoordinatorRequestV1& planning,
    const opt::ModelFamilyAlternativeRouteClassV1 route_class,
    core::platform::Uuid provider_uuid,
    core::platform::Uuid capability_uuid,
    const std::uint64_t provider_generation,
    const bool available,
    const std::uint64_t work_units,
    const std::uint64_t sequential_pages,
    const std::uint64_t memory_bytes_required) {
  return MakeModelFamilyCapabilitySnapshotV1(
      planning, route_class, std::move(provider_uuid),
      std::move(capability_uuid), provider_generation, available, work_units,
      sequential_pages, memory_bytes_required);
}

opt::ModelFamilyCoordinatorResultV1
PlanCanonicalModelFamilySourceForCompositionV1(
    opt::ModelFamilyCoordinatorRequestV1 planning,
    std::vector<opt::ModelFamilyCapabilitySnapshotV1> snapshots) {
  return PlanCanonicalModelFamilySourceV1(
      std::move(planning), std::move(snapshots));
}

}  // namespace scratchbird::engine::sblr
