// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "descriptor_value_runtime.hpp"
#include "indexed_physical_operator.hpp"
#include "index_key_encoding.hpp"

namespace scratchbird::engine::executor {

// One shared typed interface for production and component consumers. UUID
// values never cross this boundary as formatted strings.
struct CanonicalIndexStorageResolvedRowV1 {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalScanCandidateEvidence candidate;
  internal_api::EngineUuid version_uuid;
  bool engine_mga_visibility_rechecked = false;
  bool engine_security_rechecked = false;
  bool engine_residual_rechecked = false;
};

struct CanonicalSelectedIndexStorageRequestV1 {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  internal_api::EngineUuid selected_alternative_uuid;
  internal_api::EngineUuid selected_index_uuid;
  std::string available_implementation_id;
  internal_api::EngineUuid relation_uuid;
  CanonicalExecutionMgaAuthority mga_authority;
  std::uint64_t selected_descriptor_generation = 0;
  std::uint64_t current_descriptor_generation = 0;
  std::vector<internal_api::EngineUuid> selected_key_descriptor_uuids;
  std::string selected_key_profile_id;
  std::vector<scratchbird::core::index::IndexKeyEncodingComponent>
      point_key_components;
  scratchbird::core::index::IndexKeySemanticProfile key_profile;
  const scratchbird::storage::page::IndexBtreePhysicalTree* physical_tree =
      nullptr;
  std::size_t maximum_candidate_count = 0;
  std::function<bool()> cancellation_requested;
  std::function<CanonicalIndexStorageResolvedRowV1(
      const IndexedPhysicalOperatorLocator&)>
      resolve_engine_row_version;
  internal_api::EngineUuid heap_fallback_alternative_uuid;
  bool physical_tree_engine_owned = false;
  bool resolver_engine_owned = false;
  bool selected_index_is_approximate = false;
  bool exact_fallback_recheck_authorized = false;
};

struct CanonicalSelectedIndexStorageResultV1 {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalScanAccessResult scan_result;
  std::vector<scratchbird::core::platform::byte> encoded_point_key;
  internal_api::EngineUuid selected_alternative_uuid;
  internal_api::EngineUuid selected_index_uuid;
  std::size_t physical_locator_count = 0;
  std::size_t resolved_row_version_count = 0;
  bool exact_key_encoded = false;
  bool exact_selected_index_bound = false;
  bool data_access_observation_known = false;
  bool data_access_observed = false;
  bool exact_fallback_recheck_applied = false;
  bool governed_heap_replan_required = false;
  internal_api::EngineUuid governed_heap_fallback_alternative_uuid;
};

CanonicalSelectedIndexStorageResultV1
ExecuteCanonicalSelectedIndexStorageAccessV1(
    const CanonicalSelectedIndexStorageRequestV1& request);

}  // namespace scratchbird::engine::executor
