// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "query/plan_api.hpp"
#include "logical_plan.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct CanonicalRelationalPlanningScope {
  EngineUuid catalog_epoch_uuid;
  EngineUuid security_context_uuid;
  EngineUuid statement_uuid;
  std::string statement_timestamp;
  EngineUuid owning_transaction_uuid;
  EngineUuid statement_snapshot_uuid;
  EngineUuid statement_metadata_snapshot_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  bool metadata_snapshot_engine_owned{false};
  bool authorization_context_engine_owned{false};
};

struct CanonicalRelationalBridgeIssue {
  std::string diagnostic_id;
  std::uint32_t logical_node_id{0};
  std::string field_id;
};

struct CanonicalRelationalBridgeResult {
  bool accepted{false};
  bool data_access_allowed{false};
  EngineUuid catalog_epoch_uuid;
  EngineUuid statement_uuid;
  EngineUuid owning_transaction_uuid;
  EngineUuid statement_snapshot_uuid;
  EngineUuid statement_metadata_snapshot_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  scratchbird::engine::planner::CanonicalLogicalRelationalGraph logical_graph;
  scratchbird::engine::planner::CanonicalLogicalPropertyCatalog
      property_catalog;
  std::vector<CanonicalRelationalBridgeIssue> issues;
};

// QOW-ROUTE-STAGE-302-PROPERTY-BRIDGE-V1
CanonicalRelationalBridgeResult
PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
    const TypedRelationalDag& dag,
    const CanonicalRelationalPlanningScope& engine_scope);

// Shared internal access-candidate contract. Tests consume these exact types;
// they must not redeclare an independent ABI or turn UUID authority into text.
struct CanonicalAccessIndexMetadataV1 {
  EngineUuid index_uuid;
  EngineUuid relation_uuid;
  EngineUuid alternative_uuid;
  EngineUuid capability_uuid;
  std::string implementation_id;
  std::vector<std::uint32_t> key_expression_ids;
  std::uint64_t catalog_generation{0};
  std::uint64_t relation_descriptor_generation{0};
  std::uint64_t index_generation{0};
  std::uint64_t statistics_generation{0};
  std::uint64_t statistics_index_generation{0};
  std::uint64_t statistics_catalog_generation{0};
  std::uint64_t visible_generation{0};
  bool catalog_record_current{false};
  bool lifecycle_ready{false};
  bool build_validation_complete{false};
  bool profile_authoritative{false};
  bool profile_supports_mga_visibility{false};
  bool profile_supports_generation_visibility{false};
  bool supports_exact_lookup{false};
  bool supports_range_scan{false};
  bool statistics_present{false};
  bool statistics_current{false};
  bool statistics_stale{false};
  bool statistics_profile_coupled{false};
  bool statistics_mga_visible{false};
  bool visibility_evidence_engine_owned{false};
  bool visible_to_statement_snapshot{false};
  bool approximate{false};
  bool exact_fallback{false};
  bool residual_recheck_required{false};
  bool data_access_observed{false};
};

struct CanonicalAccessCandidateReceiptV1 {
  EngineUuid alternative_uuid;
  EngineUuid index_uuid;
  std::string implementation_id;
  EngineUuid capability_uuid;
  std::uint32_t logical_node_id{0};
  std::uint64_t catalog_generation{0};
  std::uint64_t relation_descriptor_generation{0};
  std::uint64_t index_generation{0};
  std::uint64_t statistics_generation{0};
  std::uint64_t visible_generation{0};
  bool available{false};
  bool heap_fallback{false};
  bool capability_validated{false};
  bool generation_validated{false};
  bool statistics_validated{false};
  bool visibility_validated{false};
  bool residual_recheck_required{false};
  std::string refusal_diagnostic_id;
};

struct CanonicalAccessCandidatePlanningRequestV1 {
  scratchbird::engine::planner::CanonicalLogicalRelationalGraph logical_graph;
  std::uint32_t logical_node_id{0};
  EngineUuid relation_uuid;
  std::vector<std::uint32_t> predicate_expression_ids;
  std::string predicate_kind;
  EngineUuid heap_alternative_uuid;
  EngineUuid heap_capability_uuid;
  EngineUuid statistics_snapshot_uuid;
  std::uint64_t current_catalog_generation{0};
  std::uint64_t current_relation_descriptor_generation{0};
  std::uint64_t current_statistics_generation{0};
  std::size_t maximum_candidate_count{0};
  bool metadata_snapshot_engine_owned{false};
  bool storage_descriptor_engine_owned{false};
  bool statistics_snapshot_engine_owned{false};
  bool data_access_observed{false};
  bool parser_planning_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  std::vector<CanonicalAccessIndexMetadataV1> indexes;
};

struct CanonicalAccessCandidatePlanningResultV1 {
  bool accepted{false};
  bool planning_complete_before_access{false};
  bool data_access_allowed{false};
  scratchbird::engine::planner::CanonicalPhysicalAlternativeCatalog catalog;
  std::vector<CanonicalAccessCandidateReceiptV1> receipts;
  std::string diagnostic_id;
  std::string field_id;
};

CanonicalAccessCandidatePlanningResultV1
QowGenerateCanonicalAccessCandidatesV1(
    const CanonicalAccessCandidatePlanningRequestV1& request);

bool QowValidateCanonicalSelectedAccessExecutionV1(
    const CanonicalAccessCandidatePlanningResultV1& planning,
    const EngineUuid& selected_alternative_uuid,
    const scratchbird::engine::executor::TypedPhysicalNodeDag& physical_dag,
    const CanonicalOptimizerSelectedExecutionResult& execution,
    std::string* diagnostic_id,
    std::string* field_id);

}  // namespace scratchbird::engine::internal_api
