// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../executor/model_family_executor.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

struct ModelFamilyCostVectorV1 {
  std::string cost_vector_uuid;
  std::uint64_t cpu_units{0};
  std::uint64_t sequential_read_units{0};
  std::uint64_t random_read_units{0};
  std::uint64_t memory_bytes_required{0};
  std::uint64_t uncertainty_penalty{0};
  std::uint64_t risk_penalty{0};
};

struct ModelFamilyCandidateV1 {
  std::string alternative_uuid;
  std::string provider_uuid;
  std::string capability_uuid;
  std::string implementation_id{"physical_document_path_scan_v1"};
  std::uint64_t provider_generation{0};
  bool available{false};
  bool exact{false};
  bool exact_collection_fallback{false};
  bool residual_recheck_required{true};
  bool base_row_mga_recheck_required{true};
  bool security_recheck_required{true};
  bool engine_owned{true};
  bool local_scope{true};
  bool parser_planning_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  ModelFamilyCostVectorV1 cost;
};

struct ModelFamilyCoordinatorRequestV1 {
  std::uint16_t abi_version{1};
  std::string family_id;
  std::string operation_id;
  std::string logical_operator_id;
  std::uint32_t logical_node_id{0};
  std::string object_uuid;
  std::vector<std::uint32_t> output_descriptor_ids;
  scratchbird::engine::executor::PhysicalMgaStatementContext
      mga_statement_context;
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string capability_snapshot_uuid;
  std::string resource_snapshot_uuid;
  std::string statistics_snapshot_uuid;
  std::string route_snapshot_uuid;
  std::uint64_t catalog_generation{0};
  std::uint64_t current_catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t policy_epoch{0};
  std::uint64_t resource_epoch{0};
  std::uint64_t statistics_generation{0};
  std::uint64_t route_epoch{0};
  std::uint64_t route_generation{0};
  std::uint64_t memory_budget_bytes{0};
  bool security_admitted{true};
  bool parser_planning_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  std::vector<ModelFamilyCandidateV1> candidates;
};

struct ModelFamilyCoordinatorResultV1 {
  bool accepted{false};
  bool selected{false};
  bool data_access_allowed{false};
  bool deterministic{false};
  bool exact_fallback_selected{false};
  ModelFamilyCandidateV1 selected_candidate;
  scratchbird::engine::executor::TypedPhysicalNodeDag physical_dag;
  std::string logical_operator_id;
  std::string physical_operator_id;
  std::string diagnostic_id;
  std::string detail;
};

ModelFamilyCoordinatorResultV1 CoordinateDocumentFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request);

ModelFamilyCoordinatorResultV1 CoordinateKeyValueFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request);

ModelFamilyCoordinatorResultV1 CoordinateModelFamilySourceV1(
    const ModelFamilyCoordinatorRequestV1& request);

}  // namespace scratchbird::engine::optimizer
