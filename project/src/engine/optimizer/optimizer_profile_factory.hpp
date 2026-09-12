// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_catalog_backed_planning.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::optimizer {

// Statement-bound evidence that an engine-owned executor capability can serve
// one admitted logical node.  It is deliberately smaller than an optimizer
// profile: callers cannot provide transformations, property bindings, model
// families, cardinalities, or cost-vector terms.
struct CanonicalOptimizerNodeCapabilityBinding {
  std::uint32_t logical_node_id{0};
  planner::CanonicalPlannerUuid capability_uuid;
  std::uint64_t memory_bytes_required{0};
  bool available{true};
  std::string refusal_diagnostic_id;
};

// Complete engine-owned executor inventory plus the exact statement-bound
// availability bindings.  The optimizer treats this only as executable
// capability evidence and owns all alternative/profile enumeration.
struct CanonicalOptimizerExecutorAvailability {
  CanonicalExecutorCapabilityCatalog capability_catalog;
  std::vector<CanonicalOptimizerNodeCapabilityBinding> node_bindings;
  bool engine_owned{false};
  bool parser_profile_authority_claimed{false};
};

struct CanonicalOptimizerProfileFactoryIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string implementation_id;
  std::string field_id;
};

struct CanonicalOptimizerProfileIdentities {
  std::uint32_t logical_node_id{0};
  planner::CanonicalPlannerUuid capability_uuid;
  planner::CanonicalPlannerUuid alternative_uuid;
  planner::CanonicalPlannerUuid transformation_uuid;
  planner::CanonicalPlannerUuid cost_vector_uuid;
};

// Immutable per-planning-scope ownership. No process-global identity cache and
// no label/hash-derived UUIDs. Creation publishes either every key or nothing.
class CanonicalOptimizerProfileIdentityOwner {
 public:
  using Key = std::pair<std::uint32_t, planner::CanonicalPlannerUuid>;
  static std::shared_ptr<const CanonicalOptimizerProfileIdentityOwner> Create(
      std::string binding, std::vector<Key> keys,
      std::uint64_t maximum_count, std::uint64_t maximum_binding_bytes) noexcept;
  bool Matches(std::string_view binding) const noexcept { return binding_ == binding; }
  const planner::CanonicalPlannerUuid& ScopeUuid() const noexcept { return scope_uuid_; }
  const CanonicalOptimizerProfileIdentities* Find(
      std::uint32_t node, const planner::CanonicalPlannerUuid& capability) const noexcept;
  std::size_t Size() const noexcept { return identities_.size(); }

 private:
  CanonicalOptimizerProfileIdentityOwner() = default;
  CanonicalOptimizerProfileIdentityOwner(const CanonicalOptimizerProfileIdentityOwner&) = delete;
  CanonicalOptimizerProfileIdentityOwner& operator=(const CanonicalOptimizerProfileIdentityOwner&) = delete;
  planner::CanonicalPlannerUuid scope_uuid_;
  std::string binding_;
  std::vector<CanonicalOptimizerProfileIdentities> identities_;
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
  std::shared_ptr<const CanonicalOptimizerProfileIdentityOwner> identity_owner;
  std::vector<CanonicalOptimizerProfileFactoryIssue> issues;
};

// Builds the complete finite domain, validates it, and creates costed search
// candidates.  Callers publish capabilities and runtime implementations only;
// they cannot author alternative, transformation, or cost-vector identities.
CanonicalOptimizerProfileFactoryResult
BuildCanonicalOptimizerAlternativeProfiles(
    const CanonicalOptimizerAdmissionRequest& admission_request,
    const CanonicalOptimizerAdmissionResult& admission,
    const CanonicalOptimizerExecutorAvailability& executor_availability,
    planner::CanonicalPlannerUuid calibration_profile_uuid,
    std::shared_ptr<const CanonicalOptimizerProfileIdentityOwner> identity_owner = {});

}  // namespace scratchbird::engine::optimizer
