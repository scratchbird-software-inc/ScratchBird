// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/graph_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "nosql/nosql_batch_point_lookup_support.hpp"
#include "nosql/nosql_surface_support.hpp"

#include <algorithm>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

struct GraphFrontierState {
  std::string vertex_id;
  std::string path;
  std::set<std::string> visited;
};

struct GraphTraversalRow {
  std::string vertex_id;
  std::string edge_id;
  std::string edge_type;
  double edge_weight = 0.0;
  std::string path;
  EngineApiU64 depth = 0;
};

template <typename TResult>
TResult DiagnosticResult(const EngineRequestContext& context,
                         const std::string& operation_id,
                         const char* diagnostic_code) {
  return MakeApiBehaviorDiagnostic<TResult>(
      context,
      operation_id,
      MakeInvalidRequestDiagnostic(operation_id, diagnostic_code));
}

void AddSelectionEvidence(const EngineNoSqlPhysicalProviderSelection& selection,
                          EngineApiResult* result) {
  for (const auto& item : selection.evidence) {
    AddApiBehaviorEvidence(result, "graph_physical_provider", item);
  }
}

bool IsPhysicalGraphRequest(const EngineGraphQueryRequest& request) {
  return request.physical_query || request.physical_proof.proof_supplied ||
         !request.vertices.empty() || !request.edges.empty() ||
         !request.seed_vertex_ids.empty() || !request.seed_label.empty() ||
         !request.seed_property_key.empty() ||
         !request.fused_candidate_seed_vertex_ids.empty() ||
         request.fusion_source_kind != EngineGraphFusionSourceKind::kNone ||
         !request.edge_type_filter.empty() ||
         !request.bidirectional_start_vertex_id.empty() ||
         !request.bidirectional_end_vertex_id.empty() ||
         request.max_depth != 1 ||
         request.direction != EngineGraphTraversalDirection::kOutgoing ||
         request.cycle_policy != EngineGraphCyclePolicy::kVisitedSet;
}

const char* DirectionName(EngineGraphTraversalDirection direction) {
  switch (direction) {
    case EngineGraphTraversalDirection::kOutgoing: return "outgoing";
    case EngineGraphTraversalDirection::kIncoming: return "incoming";
    case EngineGraphTraversalDirection::kBoth: return "both";
  }
  return "outgoing";
}

const char* CyclePolicyName(EngineGraphCyclePolicy policy) {
  switch (policy) {
    case EngineGraphCyclePolicy::kVisitedSet: return "visited_set";
    case EngineGraphCyclePolicy::kAllowCycles: return "allow_cycles";
  }
  return "visited_set";
}

const char* FusionSourceName(EngineGraphFusionSourceKind source) {
  switch (source) {
    case EngineGraphFusionSourceKind::kNone: return "none";
    case EngineGraphFusionSourceKind::kVector: return "vector";
    case EngineGraphFusionSourceKind::kSearch: return "search";
    case EngineGraphFusionSourceKind::kDocument: return "document";
    case EngineGraphFusionSourceKind::kSql: return "sql";
  }
  return "none";
}

std::string FormatWeight(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << value;
  return out.str();
}

bool HasLabel(const EngineGraphVertexInput& vertex, const std::string& label) {
  return std::find(vertex.labels.begin(), vertex.labels.end(), label) !=
         vertex.labels.end();
}

bool HasProperty(const EngineGraphVertexInput& vertex,
                 const std::string& key,
                 const std::string& value) {
  for (const auto& property : vertex.properties) {
    if (property.key == key && property.value == value) {
      return true;
    }
  }
  return false;
}

std::vector<EngineGraphVertexInput> SortedVertices(
    const std::vector<EngineGraphVertexInput>& vertices) {
  auto sorted = vertices;
  std::sort(sorted.begin(),
            sorted.end(),
            [](const EngineGraphVertexInput& left,
               const EngineGraphVertexInput& right) {
              return left.vertex_id < right.vertex_id;
            });
  return sorted;
}

std::vector<EngineGraphEdgeInput> SortedEdges(
    const std::vector<EngineGraphEdgeInput>& edges) {
  auto sorted = edges;
  std::sort(sorted.begin(),
            sorted.end(),
            [](const EngineGraphEdgeInput& left,
               const EngineGraphEdgeInput& right) {
              if (left.source_vertex_id != right.source_vertex_id) {
                return left.source_vertex_id < right.source_vertex_id;
              }
              if (left.target_vertex_id != right.target_vertex_id) {
                return left.target_vertex_id < right.target_vertex_id;
              }
              if (left.edge_type != right.edge_type) {
                return left.edge_type < right.edge_type;
              }
              return left.edge_id < right.edge_id;
            });
  return sorted;
}

void AddUnique(std::vector<std::string>* values,
               std::set<std::string>* seen,
               const std::string& value) {
  if (!value.empty() && seen->insert(value).second) {
    values->push_back(value);
  }
}

std::vector<std::string> ResolveSeedVertices(
    const EngineGraphQueryRequest& request) {
  std::vector<std::string> seeds;
  std::set<std::string> seen;
  for (const auto& seed : request.seed_vertex_ids) {
    AddUnique(&seeds, &seen, seed);
  }

  if (!request.seed_label.empty() || !request.seed_property_key.empty()) {
    for (const auto& vertex : SortedVertices(request.vertices)) {
      const bool label_matches =
          request.seed_label.empty() || HasLabel(vertex, request.seed_label);
      const bool property_matches =
          request.seed_property_key.empty() ||
          HasProperty(vertex,
                      request.seed_property_key,
                      request.seed_property_value);
      if (label_matches && property_matches) {
        AddUnique(&seeds, &seen, vertex.vertex_id);
      }
    }
  }

  for (const auto& seed : request.fused_candidate_seed_vertex_ids) {
    AddUnique(&seeds, &seen, seed);
  }
  return seeds;
}

bool EdgeTypeMatches(const EngineGraphQueryRequest& request,
                     const EngineGraphEdgeInput& edge) {
  return request.edge_type_filter.empty() ||
         edge.edge_type == request.edge_type_filter;
}

std::vector<std::pair<const EngineGraphEdgeInput*, std::string>> AdjacentEdges(
    const EngineGraphQueryRequest& request,
    const std::vector<EngineGraphEdgeInput>& sorted_edges,
    const std::string& vertex_id) {
  std::vector<std::pair<const EngineGraphEdgeInput*, std::string>> adjacent;
  for (const auto& edge : sorted_edges) {
    if (!EdgeTypeMatches(request, edge)) {
      continue;
    }
    if (request.direction != EngineGraphTraversalDirection::kIncoming &&
        edge.source_vertex_id == vertex_id) {
      adjacent.push_back({&edge, edge.target_vertex_id});
    }
    if (request.direction != EngineGraphTraversalDirection::kOutgoing &&
        edge.target_vertex_id == vertex_id) {
      adjacent.push_back({&edge, edge.source_vertex_id});
    }
  }
  std::sort(adjacent.begin(),
            adjacent.end(),
            [](const auto& left, const auto& right) {
              if (left.second != right.second) {
                return left.second < right.second;
              }
              return left.first->edge_id < right.first->edge_id;
            });
  return adjacent;
}

std::optional<EngineApiU64> OptionBatchSize(
    const EngineGraphQueryRequest& request) {
  const auto value = EngineNoSqlOptionU64(request, "graph.frontier_batch_size");
  if (!value.first || value.second == 0) {
    return std::nullopt;
  }
  return value.second;
}

std::vector<GraphTraversalRow> TraverseFrontiers(
    const EngineGraphQueryRequest& request,
    EngineApiU64* frontier_batches,
    EngineApiU64* adjacency_page_reads) {
  const auto seeds = ResolveSeedVertices(request);
  const auto sorted_edges = SortedEdges(request.edges);
  const EngineApiU64 batch_size = OptionBatchSize(request).value_or(2);

  std::vector<GraphTraversalRow> rows;
  std::vector<GraphFrontierState> frontier;
  for (const auto& seed : seeds) {
    GraphFrontierState state;
    state.vertex_id = seed;
    state.path = seed;
    state.visited.insert(seed);
    frontier.push_back(state);
    rows.push_back({seed, {}, {}, 0.0, seed, 0});
  }

  for (EngineApiU64 depth = 1; depth <= request.max_depth && !frontier.empty();
       ++depth) {
    std::vector<GraphFrontierState> next_frontier;
    for (std::size_t batch_start = 0; batch_start < frontier.size();
         batch_start += static_cast<std::size_t>(batch_size)) {
      ++(*frontier_batches);
      const auto batch_end =
          std::min(frontier.size(),
                   batch_start + static_cast<std::size_t>(batch_size));
      for (std::size_t i = batch_start; i < batch_end; ++i) {
        const auto adjacent = AdjacentEdges(request, sorted_edges, frontier[i].vertex_id);
        ++(*adjacency_page_reads);
        for (const auto& [edge, next_vertex] : adjacent) {
          if (request.cycle_policy == EngineGraphCyclePolicy::kVisitedSet &&
              frontier[i].visited.find(next_vertex) != frontier[i].visited.end()) {
            continue;
          }
          GraphFrontierState next_state;
          next_state.vertex_id = next_vertex;
          next_state.path = frontier[i].path + "->" + next_vertex;
          next_state.visited = frontier[i].visited;
          next_state.visited.insert(next_vertex);
          rows.push_back({next_vertex,
                          edge->edge_id,
                          edge->edge_type,
                          edge->weight,
                          next_state.path,
                          depth});
          next_frontier.push_back(std::move(next_state));
        }
      }
    }
    frontier = std::move(next_frontier);
  }
  return rows;
}

std::vector<std::pair<const EngineGraphEdgeInput*, std::string>> ReverseAdjacentEdges(
    const EngineGraphQueryRequest& request,
    const std::vector<EngineGraphEdgeInput>& sorted_edges,
    const std::string& vertex_id) {
  std::vector<std::pair<const EngineGraphEdgeInput*, std::string>> adjacent;
  for (const auto& edge : sorted_edges) {
    if (!EdgeTypeMatches(request, edge)) {
      continue;
    }
    if (request.direction != EngineGraphTraversalDirection::kIncoming &&
        edge.target_vertex_id == vertex_id) {
      adjacent.push_back({&edge, edge.source_vertex_id});
    }
    if (request.direction != EngineGraphTraversalDirection::kOutgoing &&
        edge.source_vertex_id == vertex_id) {
      adjacent.push_back({&edge, edge.target_vertex_id});
    }
  }
  std::sort(adjacent.begin(),
            adjacent.end(),
            [](const auto& left, const auto& right) {
              if (left.second != right.second) {
                return left.second < right.second;
              }
              return left.first->edge_id < right.first->edge_id;
            });
  return adjacent;
}

std::string JoinPath(const std::vector<std::string>& vertices,
                     std::size_t end_index) {
  std::string path;
  for (std::size_t i = 0; i <= end_index && i < vertices.size(); ++i) {
    if (!path.empty()) {
      path += "->";
    }
    path += vertices[i];
  }
  return path;
}

struct GraphPathState {
  std::vector<std::string> vertices;
  std::vector<std::string> edge_ids;
};

std::vector<GraphTraversalRow> BuildPathRows(
    const std::vector<std::string>& vertices,
    const std::vector<std::string>& edge_ids,
    const std::vector<EngineGraphEdgeInput>& sorted_edges) {
  std::vector<GraphTraversalRow> rows;
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    std::string edge_type;
    double edge_weight = 0.0;
    if (i != 0) {
      for (const auto& edge : sorted_edges) {
        if (edge.edge_id == edge_ids[i - 1]) {
          edge_type = edge.edge_type;
          edge_weight = edge.weight;
          break;
        }
      }
    }
    rows.push_back({vertices[i],
                    i == 0 ? std::string{} : edge_ids[i - 1],
                    edge_type,
                    edge_weight,
                    JoinPath(vertices, i),
                    static_cast<EngineApiU64>(i)});
  }
  return rows;
}

std::vector<GraphTraversalRow> BidirectionalPath(
    const EngineGraphQueryRequest& request,
    EngineApiU64* frontier_batches,
    EngineApiU64* adjacency_page_reads) {
  const auto sorted_edges = SortedEdges(request.edges);
  const auto start = request.bidirectional_start_vertex_id;
  const auto goal = request.bidirectional_end_vertex_id;
  if (start.empty() || goal.empty()) {
    return {};
  }
  if (start == goal) {
    return {{start, {}, {}, 0.0, start, 0}};
  }

  const EngineApiU64 batch_size = OptionBatchSize(request).value_or(2);
  std::vector<std::string> forward_frontier = {start};
  std::vector<std::string> backward_frontier = {goal};
  std::map<std::string, GraphPathState> forward_paths;
  std::map<std::string, GraphPathState> backward_paths;
  forward_paths[start] = {{start}, {}};
  backward_paths[goal] = {{goal}, {}};

  const auto build_result = [&](const std::string& meet_vertex) {
    std::vector<std::string> vertices = forward_paths[meet_vertex].vertices;
    std::vector<std::string> edge_ids = forward_paths[meet_vertex].edge_ids;
    const auto& backward = backward_paths[meet_vertex];
    vertices.insert(vertices.end(), backward.vertices.begin() + 1,
                    backward.vertices.end());
    edge_ids.insert(edge_ids.end(), backward.edge_ids.begin(),
                    backward.edge_ids.end());
    return BuildPathRows(vertices, edge_ids, sorted_edges);
  };

  const auto expand_frontier =
      [&](std::vector<std::string>* frontier,
          std::map<std::string, GraphPathState>* own_paths,
          const std::map<std::string, GraphPathState>& other_paths,
          bool reverse,
          std::vector<GraphTraversalRow>* result) {
        std::vector<std::string> next_frontier;
        for (std::size_t batch_start = 0; batch_start < frontier->size();
             batch_start += static_cast<std::size_t>(batch_size)) {
          ++(*frontier_batches);
          const auto batch_end =
              std::min(frontier->size(),
                       batch_start + static_cast<std::size_t>(batch_size));
          for (std::size_t i = batch_start; i < batch_end; ++i) {
            const auto current = (*frontier)[i];
            const auto path_it = own_paths->find(current);
            if (path_it == own_paths->end() ||
                path_it->second.edge_ids.size() >= request.max_depth) {
              continue;
            }
            const auto adjacent =
                reverse ? ReverseAdjacentEdges(request, sorted_edges, current)
                        : AdjacentEdges(request, sorted_edges, current);
            ++(*adjacency_page_reads);
            for (const auto& [edge, next_vertex] : adjacent) {
              if (own_paths->find(next_vertex) != own_paths->end()) {
                continue;
              }
              GraphPathState next_state = path_it->second;
              if (reverse) {
                next_state.vertices.insert(next_state.vertices.begin(), next_vertex);
                next_state.edge_ids.insert(next_state.edge_ids.begin(), edge->edge_id);
              } else {
                next_state.vertices.push_back(next_vertex);
                next_state.edge_ids.push_back(edge->edge_id);
              }
              if (next_state.edge_ids.size() > request.max_depth) {
                continue;
              }
              (*own_paths)[next_vertex] = next_state;
              next_frontier.push_back(next_vertex);
              const auto other_it = other_paths.find(next_vertex);
              if (other_it != other_paths.end() &&
                  next_state.edge_ids.size() + other_it->second.edge_ids.size() <=
                      request.max_depth) {
                *result = build_result(next_vertex);
                return true;
              }
            }
          }
        }
        *frontier = std::move(next_frontier);
        return false;
      };

  std::vector<GraphTraversalRow> result;
  while (!forward_frontier.empty() || !backward_frontier.empty()) {
    if (!forward_frontier.empty() &&
        expand_frontier(&forward_frontier,
                        &forward_paths,
                        backward_paths,
                        false,
                        &result)) {
      return result;
    }
    if (!backward_frontier.empty() &&
        expand_frontier(&backward_frontier,
                        &backward_paths,
                        forward_paths,
                        true,
                        &result)) {
      return result;
    }
  }
  return {};
}

template <typename TResult>
std::optional<TResult> ValidatePhysicalProof(
    const EngineGraphQueryRequest& request,
    const std::string& operation_id,
    const EngineGraphPhysicalProof& proof) {
  if (!proof.proof_supplied) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kGraphPhysicalProofMissing);
  }
  if (proof.provider_contract.family != EngineNoSqlProviderFamily::kGraph) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kNoSqlProviderFamilyUnsupported);
  }
  const auto selection = SelectLocalNoSqlPhysicalProvider(proof.provider_contract);
  if (!selection.selected) {
    auto failure = MakeApiBehaviorDiagnostic<TResult>(
        request.context,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id,
                                     selection.missing_diagnostics.empty()
                                         ? selection.refusal_diagnostics.front()
                                         : selection.missing_diagnostics.front()));
    AddSelectionEvidence(selection, &failure);
    return failure;
  }
  if (!proof.vertex_index_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kGraphVertexIndexProofMissing);
  }
  if (!proof.edge_index_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kGraphEdgeIndexProofMissing);
  }
  if (!proof.adjacency_store_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kGraphAdjacencyStoreProofMissing);
  }
  if (!proof.adjacency_page_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kGraphAdjacencyPageProofMissing);
  }
  if (!proof.frontier_batching_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kGraphFrontierBatchingProofMissing);
  }
  if (!proof.visited_cycle_policy_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kGraphVisitedCyclePolicyProofMissing);
  }
  if (!proof.bidirectional_search_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kGraphBidirectionalSearchProofMissing);
  }
  if (!proof.fusion_seed_proof) {
    return DiagnosticResult<TResult>(
        request.context, operation_id, kGraphFusionSeedProofMissing);
  }
  return std::nullopt;
}

void AddGraphEvidence(EngineApiResult* result,
                      const EngineNoSqlPhysicalProviderSelection& selection,
                      const EngineGraphQueryRequest& request,
                      EngineApiU64 seed_count,
                      EngineApiU64 frontier_batches,
                      EngineApiU64 adjacency_page_reads,
                      bool bidirectional_query) {
  AddEngineNoSqlSurfaceEvidence(result, "graph", "adjacency_store_frontier_batching");
  AddSelectionEvidence(selection, result);
  AddApiBehaviorEvidence(result,
                         "graph_physical_access",
                         "local_graph_adjacency_provider");
  AddApiBehaviorEvidence(result,
                         "graph_seed_index",
                         std::string("vertex_property_index;seeds=") +
                             std::to_string(seed_count));
  AddApiBehaviorEvidence(result,
                         "graph_adjacency_store",
                         "compressed_adjacency_pages");
  AddApiBehaviorEvidence(result,
                         "graph_adjacency_page_reads",
                         std::to_string(adjacency_page_reads));
  AddApiBehaviorEvidence(result,
                         "graph_frontier_batching",
                         "batches=" + std::to_string(frontier_batches));
  AddApiBehaviorEvidence(result,
                         "graph_traversal_direction",
                         DirectionName(request.direction));
  AddApiBehaviorEvidence(result,
                         "graph_edge_type_filter",
                         request.edge_type_filter.empty()
                             ? std::string("all")
                             : request.edge_type_filter);
  AddApiBehaviorEvidence(result,
                         "graph_cycle_policy",
                         CyclePolicyName(request.cycle_policy));
  AddApiBehaviorEvidence(result,
                         "graph_visited_set",
                         request.cycle_policy == EngineGraphCyclePolicy::kVisitedSet
                             ? "enabled"
                             : "cycle_revisit_allowed");
  AddApiBehaviorEvidence(result,
                         "graph_bidirectional_search",
                         bidirectional_query
                             ? "applied=true;frontiers=two_sided_meet"
                             : "available=true");
  AddApiBehaviorEvidence(result,
                         "graph_fusion_seed_source",
                         FusionSourceName(request.fusion_source_kind));
  const bool fusion_seed_applied =
      request.fusion_source_kind != EngineGraphFusionSourceKind::kNone ||
      !request.fused_candidate_seed_vertex_ids.empty();
  AddApiBehaviorEvidence(
      result,
      "graph_family_fusion",
      std::string("source=") + FusionSourceName(request.fusion_source_kind) +
          ";applied=" + (fusion_seed_applied ? "true" : "false"));
  if (request.fusion_source_kind == EngineGraphFusionSourceKind::kVector &&
      fusion_seed_applied) {
    AddApiBehaviorEvidence(result,
                           "graph_vector_search_fusion",
                           "candidate_seed_intersection_applied");
  } else {
    AddApiBehaviorEvidence(result,
                           "graph_vector_search_fusion",
                           "available=true");
  }
  if (request.fusion_source_kind == EngineGraphFusionSourceKind::kSearch &&
      fusion_seed_applied) {
    AddApiBehaviorEvidence(result,
                           "graph_search_fusion",
                           "candidate_seed_intersection_applied");
  } else {
    AddApiBehaviorEvidence(result, "graph_search_fusion", "available=true");
  }
  AddApiBehaviorEvidence(result, "behavior_store_scan_selected", "false");
  AddApiBehaviorEvidence(result, "descriptor_scan_selected", "false");
  AddApiBehaviorEvidence(result, "row_mga_recheck_evidence", "required");
  AddApiBehaviorEvidence(result, "row_security_recheck_evidence", "required");
  AddApiBehaviorEvidence(result,
                         "mga_finality_authority",
                         "engine_transaction_inventory");
  AddApiBehaviorEvidence(result,
                         "provider_transaction_finality_authority",
                         "false");
  AddApiBehaviorEvidence(result, "provider_visibility_authority", "false");
  AddApiBehaviorEvidence(result,
                         "parser_transaction_finality_authority",
                         "false");
  AddApiBehaviorEvidence(result, "client_autocommit_authority", "false");
}

EngineGraphQueryResult PhysicalGraphQuery(const EngineGraphQueryRequest& request,
                                          const std::string& operation_id) {
  if (request.vertices.empty()) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id, kGraphVertexCorpusRequired);
  }
  if (auto failure = ValidatePhysicalProof<EngineGraphQueryResult>(
          request, operation_id, request.physical_proof)) {
    return *failure;
  }

  const bool bidirectional_query =
      !request.bidirectional_start_vertex_id.empty() ||
      !request.bidirectional_end_vertex_id.empty();
  const auto seeds = ResolveSeedVertices(request);
  if (!bidirectional_query && seeds.empty()) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id, kGraphSeedRequired);
  }

  const auto selection =
      SelectLocalNoSqlPhysicalProvider(request.physical_proof.provider_contract);
  EngineApiU64 frontier_batches = 0;
  EngineApiU64 adjacency_page_reads = 0;
  auto traversal_rows =
      bidirectional_query
          ? BidirectionalPath(request, &frontier_batches, &adjacency_page_reads)
          : TraverseFrontiers(request, &frontier_batches, &adjacency_page_reads);

  auto result =
      MakeApiBehaviorSuccess<EngineGraphQueryResult>(request.context, operation_id);
  AddGraphEvidence(&result,
                   selection,
                   request,
                   static_cast<EngineApiU64>(seeds.size()),
                   frontier_batches,
                   adjacency_page_reads,
                   bidirectional_query);

  std::vector<EngineNoSqlBatchPointLookupItem> lookup_items;
  for (const auto& seed : seeds) {
    lookup_items.push_back(
        {seed,
         seed,
         0.0,
         "graph_seed_frontier",
         {{"frontier_role", "seed"}}});
  }
  for (const auto& row : traversal_rows) {
    const auto key = row.vertex_id + "|" + row.edge_id + "|" +
                     std::to_string(row.depth);
    lookup_items.push_back(
        {key,
         row.vertex_id,
         static_cast<double>(row.depth),
         row.path,
         {{"frontier_role", "traversal"},
          {"edge_type", row.edge_type},
          {"direction", DirectionName(request.direction)}}});
  }
  if (auto failure = AddEngineNoSqlOrderedBatchLookupEvidence<
          EngineGraphQueryResult>(
          request.context,
          operation_id,
          "graph",
          scratchbird::core::index::BatchPointLookupPurpose::graph_frontier,
          selection,
          lookup_items,
          &result)) {
    return *failure;
  }

  for (const auto& row : traversal_rows) {
    AddApiBehaviorRow(
        &result,
        {{"surface", "graph"},
         {"vertex_id", row.vertex_id},
         {"edge_id", row.edge_id},
         {"edge_type", row.edge_type},
         {"edge_weight", FormatWeight(row.edge_weight)},
         {"path", row.path},
         {"depth", std::to_string(row.depth)},
         {"direction", DirectionName(request.direction)},
         {"cycle_policy", CyclePolicyName(request.cycle_policy)},
         {"fusion_source", FusionSourceName(request.fusion_source_kind)},
         {"row_mga_recheck_required", "true"},
         {"row_security_recheck_required", "true"}});
  }
  result.dml_summary.index_probes = seeds.size() + adjacency_page_reads;
  result.dml_summary.visible_rows_scanned = 0;
  AddApiBehaviorEvidence(&result,
                         "graph_rows_returned",
                         std::to_string(result.result_shape.rows.size()));
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_GRAPH_API_BEHAVIOR
EngineGraphQueryResult EngineGraphQuery(const EngineGraphQueryRequest& request) {
  constexpr const char* kOperation = "nosql.graph_query";
  if (!request.context.cluster_authority_available && EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineGraphQueryResult>(request, kOperation);
  }
  if (IsPhysicalGraphRequest(request)) {
    return PhysicalGraphQuery(request, kOperation);
  }
  auto result = MakeApiBehaviorSuccess<EngineGraphQueryResult>(request.context, kOperation);
  AddApiBehaviorRow(&result, {{"surface", "graph"},
                              {"graph_query", ApiBehaviorPayloadFromRequest(request)},
                              {"execution", "local_descriptor_scan"}});
  AddApiBehaviorEvidence(&result, "graph_query", "local_descriptor_scan");
  AddEngineNoSqlSurfaceEvidence(&result, "graph", "local_descriptor_scan");
  return result;
}

EngineGraphWriteResult EngineGraphWrite(const EngineGraphWriteRequest& request) {
  constexpr const char* kOperation = "nosql.graph_write";
  if (!request.context.cluster_authority_available && EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<EngineGraphWriteResult>(
        request,
        kOperation);
  }
  auto result = EngineNoSqlPayloadAwarePersistedWriteResult<EngineGraphWriteResult>(
      request,
      kOperation,
      "graph",
      true,
      "written");
  if (result.ok) {
    AddEngineNoSqlSurfaceEvidence(&result, "graph", "persisted_graph_write");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
