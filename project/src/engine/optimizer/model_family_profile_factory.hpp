// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "model_family_coordinator.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace scratchbird::engine::optimizer {

// Raw, family-local observations. These are catalog/statistics/property facts,
// not optimizer alternatives: the factory owns alternative identities,
// implementation identities, route classification, cost vectors, and ranking
// eligibility.
struct ModelFamilyMetricSnapshotV1 {
  scratchbird::core::platform::Uuid statistics_snapshot_uuid;
  scratchbird::core::platform::Uuid property_snapshot_uuid;
  scratchbird::core::platform::Uuid calibration_profile_uuid;
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
  scratchbird::core::platform::Uuid provider_uuid;
  scratchbird::core::platform::Uuid capability_uuid;
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

// Immutable runtime ownership, independent of capability/data-access authority.
// Identity reuse requires the exact full binary planning-scope content binding.
class ModelFamilyProfileIdentityOwnerV1 {
 public:
  using Uuid = scratchbird::core::platform::Uuid;
  using Key = std::tuple<ModelFamilyAlternativeRouteClassV1, Uuid, Uuid>;
  struct Identities {
    Key key;
    Uuid alternative_uuid;
    Uuid cost_vector_uuid;
  };
  static std::shared_ptr<const ModelFamilyProfileIdentityOwnerV1> Create(
      std::string binding, std::vector<Key> keys,
      std::uint64_t maximum_binding_bytes) noexcept;
  bool Matches(std::string_view binding) const noexcept { return binding_ == binding; }
  const Uuid& InventoryUuid() const noexcept { return inventory_uuid_; }
  const Identities* Find(const Key& key) const noexcept;
  std::size_t Size() const noexcept { return identities_.size(); }
 private:
  ModelFamilyProfileIdentityOwnerV1() = default;
  ModelFamilyProfileIdentityOwnerV1(const ModelFamilyProfileIdentityOwnerV1&) = delete;
  ModelFamilyProfileIdentityOwnerV1& operator=(const ModelFamilyProfileIdentityOwnerV1&) = delete;
  Uuid inventory_uuid_;
  std::string binding_;
  std::vector<Identities> identities_;
};

struct ModelFamilyProfileFactoryRequestV1 {
  std::uint16_t abi_version{1};
  std::shared_ptr<const ModelFamilyProfileIdentityOwnerV1> identity_owner;
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
  scratchbird::core::platform::Uuid candidate_inventory_receipt_uuid;
  std::shared_ptr<const ModelFamilyProfileIdentityOwnerV1> identity_owner;
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
