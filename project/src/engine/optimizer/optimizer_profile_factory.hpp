// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_catalog_backed_planning.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// Statement-bound evidence that an engine-owned executor capability can serve
// one admitted logical node.  It is deliberately smaller than an optimizer
// profile: callers cannot provide transformations, property bindings, model
// families, cardinalities, or cost-vector terms.
struct CanonicalOptimizerNodeCapabilityBinding {
  std::uint32_t logical_node_id{0};
  std::string capability_uuid;
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
    const CanonicalOptimizerExecutorAvailability& executor_availability,
    std::string identity_scope,
    std::string calibration_profile_uuid);

}  // namespace scratchbird::engine::optimizer
