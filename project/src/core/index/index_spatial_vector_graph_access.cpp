// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_spatial_vector_graph_access.hpp"

#include "policy_blocked_index_admission.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status RefuseStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

IndexSpatialAccessPlan RefuseSpatial(std::string code, std::string key, std::string detail = {}) {
  IndexSpatialAccessPlan plan;
  plan.status = RefuseStatus();
  plan.diagnostic = MakeIndexSpatialVectorGraphDiagnostic(plan.status, std::move(code), std::move(key), std::move(detail));
  return plan;
}

IndexVectorAdmissionDecision RefuseVector(std::string code, std::string key, bool policy_blocked) {
  IndexVectorAdmissionDecision decision;
  decision.status = RefuseStatus();
  decision.policy_blocked = policy_blocked;
  decision.diagnostic = MakeIndexSpatialVectorGraphDiagnostic(decision.status, std::move(code), std::move(key));
  return decision;
}

IndexGraphProfileDecision RefuseGraph(std::string code, std::string key) {
  IndexGraphProfileDecision decision;
  decision.status = RefuseStatus();
  decision.diagnostic = MakeIndexSpatialVectorGraphDiagnostic(decision.status, std::move(code), std::move(key));
  return decision;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
      return static_cast<char>(c - 'A' + 'a');
    }
    return static_cast<char>(c);
  });
  return value;
}

bool AdvancedVectorAlgorithm(IndexVectorAlgorithm algorithm) {
  return algorithm == IndexVectorAlgorithm::annoy || algorithm == IndexVectorAlgorithm::nsg ||
         algorithm == IndexVectorAlgorithm::diskann || algorithm == IndexVectorAlgorithm::scann ||
         algorithm == IndexVectorAlgorithm::gpu_cagra;
}
}  // namespace

IndexSpatialAccessPlan PlanSpatialIndexAccess(const IndexSpatialAccessRequest& request) {
  if (request.query_shape_token.empty()) {
    return RefuseSpatial("SB-INDEX-SPATIAL-QUERY-TOKEN-MISSING", "index.spatial.query_token_missing");
  }
  if (!request.resource.resource_uuid.valid() || request.resource.resource_epoch == 0 || !request.resource.deterministic) {
    return RefuseSpatial("SB-INDEX-SPATIAL-RESOURCE-INVALID", "index.spatial.resource_invalid");
  }
  if (request.resource.dimensions < 2 || request.resource.dimensions > 4) {
    return RefuseSpatial("SB-INDEX-SPATIAL-DIMENSIONS-UNSUPPORTED", "index.spatial.dimensions_unsupported",
                         std::to_string(request.resource.dimensions));
  }
  IndexSpatialAccessPlan plan;
  plan.status = OkStatus();
  plan.family = request.family == IndexFamily::rtree || request.family == IndexFamily::gist ||
                        request.family == IndexFamily::spgist
                    ? request.family
                    : IndexFamily::spatial;
  plan.admitted = true;
  plan.requires_exact_recheck = request.reference_requires_exact_recheck ||
                                request.predicate == IndexSpatialPredicate::distance_within ||
                                request.predicate == IndexSpatialPredicate::nearest;
  plan.can_order_by_distance = request.request_distance_order &&
                               (request.predicate == IndexSpatialPredicate::nearest ||
                                request.predicate == IndexSpatialPredicate::distance_within);
  plan.semantic_profile_id = "spatial:" + request.resource.crs_name + ":" + request.resource.coordinate_order;
  return plan;
}

IndexVectorAdmissionDecision AdmitVectorIndex(const IndexVectorAdmissionRequest& request) {
  if (request.descriptor.dimensions == 0 || request.descriptor.element_bytes == 0 ||
      !request.descriptor.metric_resource_uuid.valid() || !request.descriptor.deterministic_metric) {
    return RefuseVector("SB-INDEX-VECTOR-DESCRIPTOR-INVALID", "index.vector.descriptor_invalid", false);
  }
  if (AdvancedVectorAlgorithm(request.algorithm) && !request.policy_allows_advanced_alpha) {
    auto policy_request = MakePolicyBlockedIndexRouteRequest(
        IndexFamily::policy_blocked,
        std::string("vector:") + IndexVectorAlgorithmName(request.algorithm),
        "index.vector_admission",
        request.exact_fallback_allowed ? "vector_exact" : "",
        request.exact_fallback_allowed);
    policy_request.required_feature =
        std::string("advanced_vector_algorithm:") +
        IndexVectorAlgorithmName(request.algorithm);
    policy_request.required_policy =
        "SB_POLICY_INDEX_ADVANCED_VECTOR_NOT_ACCEPTED_ALPHA";
    const auto policy_blocked =
        EvaluatePolicyBlockedIndexAdmission(policy_request);
    IndexVectorAdmissionDecision decision;
    decision.status = policy_blocked.status;
    decision.family = IndexFamily::policy_blocked;
    decision.policy_blocked = true;
    decision.diagnostic = policy_blocked.diagnostic;
    if (request.exact_fallback_allowed) {
      decision.status = OkStatus();
      decision.family = IndexFamily::vector_exact;
      decision.admitted = true;
      decision.policy_blocked = false;
      decision.requested_algorithm_policy_blocked = true;
      decision.exact_fallback = true;
      decision.requires_rerank = false;
      decision.semantic_profile_id = "vector:exact:fallback";
      decision.diagnostic = MakePolicyBlockedIndexDiagnostic(
          decision.status,
          "INDEX.POLICY_BLOCKED.EXPLICIT_FALLBACK_SELECTED",
          "index.policy_blocked.explicit_fallback_selected",
          policy_request,
          PolicyBlockedIndexRefusalReason::policy_not_accepted);
    }
    return decision;
  }
  if (request.algorithm != IndexVectorAlgorithm::flat && request.algorithm != IndexVectorAlgorithm::binary_flat &&
      !request.policy_allows_approximate) {
    return RefuseVector("SB-INDEX-VECTOR-APPROX-POLICY-REFUSED", "index.vector.approx_policy_refused", true);
  }

  IndexVectorAdmissionDecision decision;
  decision.status = OkStatus();
  decision.admitted = true;
  decision.requires_rerank = request.algorithm != IndexVectorAlgorithm::flat && request.algorithm != IndexVectorAlgorithm::binary_flat;
  switch (request.algorithm) {
    case IndexVectorAlgorithm::flat:
      decision.family = IndexFamily::vector_exact;
      decision.semantic_profile_id = "vector:flat:" + std::to_string(request.descriptor.dimensions);
      break;
    case IndexVectorAlgorithm::binary_flat:
      decision.family = IndexFamily::vector_exact;
      decision.semantic_profile_id = "vector:binary_flat:" + std::to_string(request.descriptor.dimensions);
      break;
    case IndexVectorAlgorithm::hnsw:
    case IndexVectorAlgorithm::rhnsw_quantized:
      decision.family = IndexFamily::vector_hnsw;
      decision.semantic_profile_id = "vector:hnsw:" + std::to_string(request.requested_neighbors);
      break;
    case IndexVectorAlgorithm::ivf_flat:
    case IndexVectorAlgorithm::ivf_pq:
    case IndexVectorAlgorithm::ivf_sq8:
      if (request.training_row_count == 0 || request.requested_lists == 0) {
        return RefuseVector("SB-INDEX-VECTOR-IVF-TRAINING-MISSING", "index.vector.ivf_training_missing", false);
      }
      decision.family = IndexFamily::vector_ivf;
      decision.semantic_profile_id = "vector:ivf:" + std::to_string(request.requested_lists);
      break;
    case IndexVectorAlgorithm::annoy:
    case IndexVectorAlgorithm::nsg:
    case IndexVectorAlgorithm::diskann:
    case IndexVectorAlgorithm::scann:
    case IndexVectorAlgorithm::gpu_cagra: {
      auto policy_request = MakePolicyBlockedIndexRouteRequest(
          IndexFamily::policy_blocked,
          std::string("vector:") + IndexVectorAlgorithmName(request.algorithm),
          "index.vector_admission",
          request.exact_fallback_allowed ? "vector_exact" : "",
          request.exact_fallback_allowed);
      policy_request.required_feature =
          std::string("advanced_vector_algorithm:") +
          IndexVectorAlgorithmName(request.algorithm);
      policy_request.required_policy =
          "SB_POLICY_INDEX_ADVANCED_VECTOR_NOT_ACCEPTED_ALPHA";
      policy_request.policy_accepted = true;
      policy_request.required_feature_available = false;
      const auto policy_blocked =
          EvaluatePolicyBlockedIndexAdmission(policy_request);
      decision.family = IndexFamily::policy_blocked;
      decision.policy_blocked = true;
      decision.admitted = false;
      decision.status = policy_blocked.status;
      decision.diagnostic = policy_blocked.diagnostic;
      break;
    }
  }
  return decision;
}

IndexGraphProfileDecision PlanGraphOrReferenceStructureIndex(const IndexGraphProfileRequest& request) {
  if (!request.policy_allows_emulation) {
    return RefuseGraph("SB-INDEX-GRAPH-EMULATION-POLICY-REFUSED", "index.graph.emulation_policy_refused");
  }
  IndexGraphProfileDecision decision;
  decision.status = OkStatus();
  decision.admitted = true;
  decision.emulated = !request.reference_name.empty();
  decision.semantic_profile_id = Lower(request.reference_name.empty() ? "sb" : request.reference_name) + ":" + Lower(request.reference_surface);
  switch (request.profile) {
    case IndexGraphProfile::vertex_lookup:
    case IndexGraphProfile::edge_lookup:
    case IndexGraphProfile::label_property:
    case IndexGraphProfile::path_topology:
    case IndexGraphProfile::neo4j_lookup:
      decision.family = IndexFamily::graph;
      break;
    case IndexGraphProfile::redis_structure:
      decision.family = request.requires_range_order ? IndexFamily::btree : IndexFamily::hash;
      decision.requires_recheck = false;
      break;
    case IndexGraphProfile::cassandra_sai:
      decision.family = request.requires_text_analysis ? IndexFamily::inverted : IndexFamily::brin_zone;
      break;
    case IndexGraphProfile::clickhouse_sparse:
      decision.family = IndexFamily::brin_zone;
      break;
    case IndexGraphProfile::mongodb_wildcard:
      decision.family = request.requires_vector_similarity ? IndexFamily::vector_hnsw : IndexFamily::document_path;
      break;
  }
  return decision;
}

const char* IndexVectorAlgorithmName(IndexVectorAlgorithm algorithm) {
  switch (algorithm) {
    case IndexVectorAlgorithm::flat: return "flat";
    case IndexVectorAlgorithm::binary_flat: return "binary_flat";
    case IndexVectorAlgorithm::hnsw: return "hnsw";
    case IndexVectorAlgorithm::ivf_flat: return "ivf_flat";
    case IndexVectorAlgorithm::ivf_pq: return "ivf_pq";
    case IndexVectorAlgorithm::ivf_sq8: return "ivf_sq8";
    case IndexVectorAlgorithm::rhnsw_quantized: return "rhnsw_quantized";
    case IndexVectorAlgorithm::annoy: return "annoy";
    case IndexVectorAlgorithm::nsg: return "nsg";
    case IndexVectorAlgorithm::diskann: return "diskann";
    case IndexVectorAlgorithm::scann: return "scann";
    case IndexVectorAlgorithm::gpu_cagra: return "gpu_cagra";
  }
  return "unknown";
}

DiagnosticRecord MakeIndexSpatialVectorGraphDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.spatial_vector_graph");
}

}  // namespace scratchbird::core::index
