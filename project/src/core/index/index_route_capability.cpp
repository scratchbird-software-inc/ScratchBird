// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_route_capability.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {

bool OrderedWriteFamily(IndexFamily family) {
  return family == IndexFamily::btree ||
         family == IndexFamily::unique_btree ||
         family == IndexFamily::expression ||
         family == IndexFamily::partial ||
         family == IndexFamily::covering;
}

bool TokenSearchFamily(IndexFamily family) {
  return family == IndexFamily::full_text ||
         family == IndexFamily::gin ||
         family == IndexFamily::inverted ||
         family == IndexFamily::ngram ||
         family == IndexFamily::sparse_wand;
}

bool VectorFamily(IndexFamily family) {
  return family == IndexFamily::vector_exact ||
         family == IndexFamily::vector_hnsw ||
         family == IndexFamily::vector_ivf;
}

bool ApproximateVectorFamily(IndexFamily family) {
  return family == IndexFamily::vector_hnsw ||
         family == IndexFamily::vector_ivf;
}

bool SpatialFamily(IndexFamily family) {
  return family == IndexFamily::spatial ||
         family == IndexFamily::rtree ||
         family == IndexFamily::gist ||
         family == IndexFamily::spgist;
}

bool CandidateFamily(IndexFamily family) {
  return family == IndexFamily::bitmap ||
         family == IndexFamily::document_path ||
         family == IndexFamily::graph ||
         TokenSearchFamily(family) ||
         VectorFamily(family) ||
         SpatialFamily(family);
}

bool SummarySegmentPruneFamily(IndexFamily family) {
  return family == IndexFamily::brin_zone ||
         family == IndexFamily::columnar_zone;
}

bool PruningFamily(IndexFamily family) {
  return family == IndexFamily::bloom || SummarySegmentPruneFamily(family);
}

bool RankingOrSeedFamily(IndexFamily family) {
  return family == IndexFamily::sparse_wand ||
         family == IndexFamily::document_path ||
         family == IndexFamily::graph ||
         VectorFamily(family) ||
         SpatialFamily(family);
}

bool EqualityLookupFamily(IndexFamily family) {
  return OrderedWriteFamily(family) ||
         family == IndexFamily::hash ||
         family == IndexFamily::document_path ||
         family == IndexFamily::graph;
}

bool OrderedRangeFamily(IndexFamily family) {
  return OrderedWriteFamily(family);
}

bool FamilyComplete(IndexFamily family) {
  const auto* state = FindBuiltinIndexFamilyPhysicalCapabilityState(family);
  return state != nullptr && state->runtime_available && state->benchmark_clean &&
         state->blocker == IndexFamilyPhysicalCapabilityBlocker::none &&
         state->physically_complete();
}

bool RouteSupportsFamily(IndexRouteKind route, IndexFamily family) {
  if (!FamilyComplete(family)) {
    return false;
  }
  switch (route) {
    case IndexRouteKind::dml_insert:
    case IndexRouteKind::dml_update:
    case IndexRouteKind::dml_delete:
      return family != IndexFamily::reference_emulated &&
             family != IndexFamily::policy_blocked &&
             family != IndexFamily::unknown;
    case IndexRouteKind::sql_select:
      return family != IndexFamily::reference_emulated &&
             family != IndexFamily::policy_blocked &&
             family != IndexFamily::unknown;
    case IndexRouteKind::bulk_build:
      return family != IndexFamily::reference_emulated &&
             family != IndexFamily::policy_blocked &&
             family != IndexFamily::unknown;
    case IndexRouteKind::nosql_document:
      return family == IndexFamily::document_path;
    case IndexRouteKind::nosql_graph:
      return family == IndexFamily::graph;
    case IndexRouteKind::nosql_vector:
      return VectorFamily(family);
    case IndexRouteKind::nosql_search:
      return TokenSearchFamily(family);
    case IndexRouteKind::maintenance:
    case IndexRouteKind::validate_repair:
      return family != IndexFamily::reference_emulated &&
             family != IndexFamily::policy_blocked &&
             family != IndexFamily::unknown;
    case IndexRouteKind::unknown:
      return false;
  }
  return false;
}

std::string UnsupportedDetail(IndexRouteKind route, IndexFamily family) {
  std::string detail = "route=";
  detail += IndexRouteKindName(route);
  detail += ";family=";
  detail += IndexFamilyName(family);
  detail += ";physical_family_complete=";
  detail += FamilyComplete(family) ? "true" : "false";
  detail += ";route_complete=false";
  return detail;
}

IndexRouteCapabilityState BuildState(IndexRouteKind route, IndexFamily family) {
  IndexRouteCapabilityState state;
  state.route = route;
  state.family = family;
  state.family_physical_complete = FamilyComplete(family);
  state.route_declared = route != IndexRouteKind::unknown &&
                         family != IndexFamily::unknown;
  state.route_supported = RouteSupportsFamily(route, family);
  state.benchmark_clean = state.family_physical_complete &&
                          state.route_supported;

  state.supports_read = state.route_supported &&
                        (route == IndexRouteKind::sql_select ||
                         route == IndexRouteKind::nosql_document ||
                         route == IndexRouteKind::nosql_graph ||
                         route == IndexRouteKind::nosql_vector ||
                         route == IndexRouteKind::nosql_search ||
                         route == IndexRouteKind::maintenance ||
                         route == IndexRouteKind::validate_repair);
  state.supports_write = state.route_supported &&
                         (route == IndexRouteKind::dml_insert ||
                          route == IndexRouteKind::dml_update ||
                          route == IndexRouteKind::dml_delete);
  state.supports_mutation = state.route_supported &&
                            (state.supports_write ||
                             route == IndexRouteKind::maintenance);
  state.supports_bulk_build = state.route_supported &&
                              route == IndexRouteKind::bulk_build;
  state.supports_reopen = state.route_supported &&
                          (route == IndexRouteKind::maintenance ||
                           route == IndexRouteKind::validate_repair ||
                           route == IndexRouteKind::bulk_build);
  state.supports_ordered_range = state.route_supported &&
                                 OrderedRangeFamily(family);
  state.supports_equality_lookup = state.route_supported &&
                                   EqualityLookupFamily(family);
  state.supports_negative_prune = state.route_supported &&
                                  family == IndexFamily::bloom;
  state.supports_summary_segment_prune =
      state.route_supported && SummarySegmentPruneFamily(family);
  state.produces_candidate_set = state.route_supported &&
                                 CandidateFamily(family);
  state.produces_ranking_or_seed = state.route_supported &&
                                   RankingOrSeedFamily(family);
  state.approximate_candidate_source = state.route_supported &&
                                      ApproximateVectorFamily(family);
  state.requires_exact_rerank = state.route_supported &&
                                (ApproximateVectorFamily(family) ||
                                 family == IndexFamily::sparse_wand);
  state.requires_exact_recheck = state.route_supported &&
                                 (CandidateFamily(family) ||
                                  PruningFamily(family) ||
                                  family == IndexFamily::hash);
  state.requires_mga_recheck = true;
  state.requires_security_recheck = true;
  state.hash_requires_keyed_algorithm =
      family == IndexFamily::hash &&
      (route == IndexRouteKind::sql_select ||
       route == IndexRouteKind::maintenance ||
       route == IndexRouteKind::validate_repair);
  state.hash_legacy_benchmark_clean_requires_policy =
      family == IndexFamily::hash;
  state.hash_high_assurance_required_by_policy =
      family == IndexFamily::hash &&
      (route == IndexRouteKind::maintenance ||
       route == IndexRouteKind::validate_repair);

  if (state.route_complete()) {
    state.route_diagnostic_code = "INDEX.ROUTE_CAPABILITY.OK";
    state.route_message_key = "index.route_capability.ok";
    state.route_detail = "route and family are benchmark-clean for this public path";
  } else if (!state.family_physical_complete) {
    const auto* family_state =
        FindBuiltinIndexFamilyPhysicalCapabilityState(family);
    state.route_diagnostic_code =
        family_state != nullptr && !family_state->blocker_diagnostic_code.empty()
            ? family_state->blocker_diagnostic_code
            : "INDEX.ROUTE_CAPABILITY.PHYSICAL_FAMILY_BLOCKED";
    state.route_message_key =
        family_state != nullptr && !family_state->blocker_message_key.empty()
            ? family_state->blocker_message_key
            : "index.route_capability.physical_family_blocked";
    state.route_detail = UnsupportedDetail(route, family);
  } else {
    state.route_diagnostic_code =
        "INDEX.ROUTE_CAPABILITY.UNSUPPORTED_ROUTE_FAMILY";
    state.route_message_key =
        "index.route_capability.unsupported_route_family";
    state.route_detail = UnsupportedDetail(route, family);
  }

  state.evidence.push_back(std::string("route_kind=") +
                           IndexRouteKindName(route));
  state.evidence.push_back(std::string("index_family=") +
                           IndexFamilyName(family));
  state.evidence.push_back(
      std::string("family_physical_complete=") +
      (state.family_physical_complete ? "true" : "false"));
  state.evidence.push_back(std::string("route_supported=") +
                           (state.route_supported ? "true" : "false"));
  state.evidence.push_back(std::string("route_benchmark_clean=") +
                           (state.benchmark_clean ? "true" : "false"));
  state.evidence.push_back(std::string("supports_read=") +
                           (state.supports_read ? "true" : "false"));
  state.evidence.push_back(std::string("supports_write=") +
                           (state.supports_write ? "true" : "false"));
  state.evidence.push_back(
      std::string("requires_exact_recheck=") +
      (state.requires_exact_recheck ? "true" : "false"));
  state.evidence.push_back(
      std::string("supports_summary_segment_prune=") +
      (state.supports_summary_segment_prune ? "true" : "false"));
  state.evidence.push_back(
      std::string("produces_ranking_or_seed=") +
      (state.produces_ranking_or_seed ? "true" : "false"));
  state.evidence.push_back(
      std::string("requires_mga_recheck=") +
      (state.requires_mga_recheck ? "true" : "false"));
  state.evidence.push_back(
      std::string("requires_security_recheck=") +
      (state.requires_security_recheck ? "true" : "false"));
  state.evidence.push_back(
      std::string("requires_exact_rerank=") +
      (state.requires_exact_rerank ? "true" : "false"));
  if (family == IndexFamily::hash) {
    state.evidence.push_back("hash_equality_lookup_supported=true");
    state.evidence.push_back("hash_ordered_range_supported=false");
    state.evidence.push_back("hash_negative_prune_supported=false");
    state.evidence.push_back("hash_dml_write_requires_explicit_hash_route=true");
    state.evidence.push_back(
        std::string("hash_route_requires_keyed_algorithm=") +
        (state.hash_requires_keyed_algorithm ? "true" : "false"));
    state.evidence.push_back(
        std::string("hash_legacy_benchmark_clean_requires_policy=") +
        (state.hash_legacy_benchmark_clean_requires_policy ? "true" : "false"));
    state.evidence.push_back(
        std::string("hash_high_assurance_required_by_policy=") +
        (state.hash_high_assurance_required_by_policy ? "true" : "false"));
  }
  return state;
}

std::vector<IndexRouteCapabilityState> BuildStates() {
  const IndexRouteKind routes[] = {
      IndexRouteKind::dml_insert,
      IndexRouteKind::dml_update,
      IndexRouteKind::dml_delete,
      IndexRouteKind::sql_select,
      IndexRouteKind::bulk_build,
      IndexRouteKind::nosql_document,
      IndexRouteKind::nosql_graph,
      IndexRouteKind::nosql_vector,
      IndexRouteKind::nosql_search,
      IndexRouteKind::maintenance,
      IndexRouteKind::validate_repair};
  std::vector<IndexRouteCapabilityState> states;
  states.reserve(BuiltinIndexFamilyDescriptors().size() *
                 (sizeof(routes) / sizeof(routes[0])));
  for (const auto& descriptor : BuiltinIndexFamilyDescriptors()) {
    for (const auto route : routes) {
      states.push_back(BuildState(route, descriptor.family));
    }
  }
  return states;
}

}  // namespace

const char* IndexRouteKindName(IndexRouteKind route) {
  switch (route) {
    case IndexRouteKind::dml_insert: return "dml_insert";
    case IndexRouteKind::dml_update: return "dml_update";
    case IndexRouteKind::dml_delete: return "dml_delete";
    case IndexRouteKind::sql_select: return "sql_select";
    case IndexRouteKind::bulk_build: return "bulk_build";
    case IndexRouteKind::nosql_document: return "nosql_document";
    case IndexRouteKind::nosql_graph: return "nosql_graph";
    case IndexRouteKind::nosql_vector: return "nosql_vector";
    case IndexRouteKind::nosql_search: return "nosql_search";
    case IndexRouteKind::maintenance: return "maintenance";
    case IndexRouteKind::validate_repair: return "validate_repair";
    case IndexRouteKind::unknown: return "unknown";
  }
  return "unknown";
}

const std::vector<IndexRouteCapabilityState>&
BuiltinIndexRouteCapabilityStates() {
  static const std::vector<IndexRouteCapabilityState> states = BuildStates();
  return states;
}

const IndexRouteCapabilityState* FindBuiltinIndexRouteCapabilityState(
    IndexRouteKind route,
    IndexFamily family) {
  for (const auto& state : BuiltinIndexRouteCapabilityStates()) {
    if (state.route == route && state.family == family) {
      return &state;
    }
  }
  return nullptr;
}

DiagnosticRecord MakeIndexRouteCapabilityDiagnostic(
    Status status,
    const IndexRouteCapabilityState& state) {
  const auto* family_state =
      FindBuiltinIndexFamilyPhysicalCapabilityState(state.family);
  auto record = MakeIndexFamilyCapabilityDiagnostic(
      status,
      state.route_diagnostic_code,
      state.route_message_key,
      IndexFamilyName(state.family),
      state.route_detail,
      state.family_physical_complete
          ? IndexFamilyPhysicalCapabilityBlocker::none
          : family_state != nullptr
                ? family_state->blocker
                : IndexFamilyPhysicalCapabilityBlocker::unknown_family);
  record.arguments.push_back({"route", IndexRouteKindName(state.route)});
  record.arguments.push_back(
      {"route_supported", state.route_supported ? "true" : "false"});
  record.arguments.push_back(
      {"route_benchmark_clean", state.benchmark_clean ? "true" : "false"});
  record.source_component = "sb_core_index.route_capability";
  return record;
}

}  // namespace scratchbird::core::index
