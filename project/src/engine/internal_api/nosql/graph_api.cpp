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
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/nosql_batch_point_lookup_support.hpp"
#include "nosql/nosql_surface_support.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
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
  std::string path_identity;
  std::set<std::string> visited;
};

struct GraphTraversalRow {
  std::string vertex_id;
  std::string edge_id;
  std::string_view edge_type;
  double edge_weight = 0.0;
  std::string path;
  std::string path_identity;
  EngineApiU64 depth = 0;
};

struct GraphTraversalResult {
  bool resource_exhausted = false;
  std::vector<GraphTraversalRow> rows;
  bool cancelled = false;
  bool coordinator_failed = false;
};

enum class GraphCancellationProbe {
  kContinue,
  kCancelled,
  kCoordinatorFailed,
};

GraphCancellationProbe PollGraphCancellation(
    const EngineGraphQueryRequest& request) {
  if (!request.context.query_cancellation_requested) {
    return GraphCancellationProbe::kContinue;
  }
  try {
    return request.context.query_cancellation_requested()
               ? GraphCancellationProbe::kCancelled
               : GraphCancellationProbe::kContinue;
  } catch (...) {
    return GraphCancellationProbe::kCoordinatorFailed;
  }
}

const char* GraphCancellationDiagnostic(
    const EngineGraphQueryRequest& request) {
  switch (PollGraphCancellation(request)) {
    case GraphCancellationProbe::kContinue:
      return nullptr;
    case GraphCancellationProbe::kCancelled:
      return "SB_MODEL_EXECUTION_CANCELLED_V1";
    case GraphCancellationProbe::kCoordinatorFailed:
      return "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
  }
  return "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
}

bool CanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

bool ExactGraphValueDescriptor(const EngineDescriptor& descriptor,
                               const std::string_view expected_type,
                               const std::string_view expected_type_uuid,
                               const scratchbird::core::datatypes::
                                   DatatypeTypeCodecIdentityRowV1*
                                       expected_registry_identity,
                               const std::string_view expected_column_uuid,
                               const bool expected_nullable) {
  if (!QowCanonicalDescriptorIdentityV1(descriptor) ||
      descriptor.descriptor_kind != "canonical_type_descriptor" ||
      descriptor.canonical_type_name != expected_type) {
    return false;
  }
  std::map<std::string_view, std::string_view> fields;
  const auto encoded = std::string_view(descriptor.encoded_descriptor);
  std::size_t offset = 0;
  while (offset <= encoded.size()) {
    const auto end = encoded.find(';', offset);
    const auto field = encoded.substr(
        offset, end == std::string_view::npos ? std::string_view::npos
                                              : end - offset);
    const auto equal = field.find('=');
    if (field.empty() || equal == std::string_view::npos || equal == 0 ||
        equal + 1 == field.size() ||
        !fields.emplace(field.substr(0, equal), field.substr(equal + 1)).second)
      return false;
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  const bool contextual_text = expected_type == "text";
  if (fields.size() != (contextual_text ? 12U : 3U) ||
      !fields.contains("canonical") ||
      !fields.contains("type_uuid") || !fields.contains("nullable") ||
      fields.at("canonical") != expected_type ||
      !CanonicalUuid(fields.at("type_uuid")) ||
      fields.at("type_uuid") != expected_type_uuid ||
      fields.at("nullable") != (expected_nullable ? "true" : "false") ||
      (contextual_text &&
       (expected_registry_identity == nullptr ||
        !fields.contains("column_uuid") ||
        fields.at("column_uuid") != expected_column_uuid ||
        !fields.contains("datatype_descriptor_uuid") ||
        fields.at("datatype_descriptor_uuid") !=
            expected_registry_identity->descriptor_uuid ||
        descriptor.descriptor_uuid.canonical !=
            expected_column_uuid ||
        !fields.contains("datatype_descriptor_generation") ||
        fields.at("datatype_descriptor_generation") !=
            std::to_string(
                expected_registry_identity->descriptor_generation) ||
        !fields.contains("type_generation") ||
        fields.at("type_generation") !=
            std::to_string(expected_registry_identity->type_generation) ||
        !fields.contains("codec_uuid") ||
        fields.at("codec_uuid") != expected_registry_identity->codec_uuid ||
        !fields.contains("codec_id") ||
        fields.at("codec_id") != expected_registry_identity->codec_id ||
        !fields.contains("codec_version") ||
        fields.at("codec_version") !=
            std::to_string(expected_registry_identity->codec_version) ||
        !fields.contains("codec_generation") ||
        fields.at("codec_generation") !=
            std::to_string(expected_registry_identity->codec_generation) ||
        !fields.contains("null_encoding") ||
        fields.at("null_encoding") !=
            std::to_string(expected_registry_identity->null_encoding_code))) ||
      (!contextual_text && fields.contains("column_uuid"))) {
    return false;
  }
  return true;
}

bool ExactGraphDescriptorCohort(
    const MgaRelationStorageDescriptor& descriptor) {
  static constexpr std::array<std::string_view, 9> kNames{
      "vertex_uuid",       "edge_uuid",       "path_uuid",
      "vertex_labels",     "vertex_properties", "edge_properties",
      "direction",         "depth",           "cycle_policy"};
  static constexpr std::array<std::string_view, 9> kTypes{
      "uuid", "uuid", "uuid", "text", "text", "text", "text",
      "uint64", "text"};
  static constexpr std::array<bool, 9> kNullable{
      false, true, false, false, false, false, false, false, false};
  if (descriptor.columns.size() != kNames.size()) return false;
  const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return false;
  for (std::size_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    const auto& column = descriptor.columns[ordinal];
    for (std::size_t prior = 0; prior < ordinal; ++prior) {
      if (descriptor.columns[prior].column_uuid.canonical ==
          column.column_uuid.canonical) {
        return false;
      }
    }
    const auto type_id =
        scratchbird::core::datatypes::CanonicalTypeIdFromStableName(
            std::string(kTypes[ordinal]));
    const auto type_row =
        scratchbird::core::datatypes::LookupDatatypeCatalogRow(
            manifest.manifest, type_id);
    if (!type_row.ok() || type_row.manifest.descriptor_rows.size() != 1 ||
        !type_row.manifest.descriptor_rows.front().descriptor_uuid.valid()) {
      return false;
    }
    const auto& descriptor_row =
        type_row.manifest.descriptor_rows.front();
    const auto descriptor_uuid = scratchbird::core::uuid::UuidToString(
        descriptor_row.descriptor_uuid.value);
    const auto codec_identity =
        scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
            "019d0000-0000-7000-8000-00000000d701",
            manifest.manifest.catalog_epoch, 1, descriptor_uuid,
            descriptor_row.descriptor_epoch);
    const auto expected_type_uuid =
        codec_identity.ok ? codec_identity.row.type_uuid : descriptor_uuid;
    const auto* expected_registry_identity =
        codec_identity.ok ? &codec_identity.row : nullptr;
    if (kTypes[ordinal] == "text" && expected_registry_identity == nullptr) {
      return false;
    }
    if (column.ordinal != ordinal ||
        column.canonical_name_key != kNames[ordinal] ||
        column.nullable != kNullable[ordinal] || column.generated ||
        column.identity_column || !column.charset_uuid.empty() ||
        !column.collation_uuid.empty() || column.character_length != 0 ||
        column.storage_class != "inline_row_value" ||
        column.max_inline_bytes != 4096 ||
        column.overflow_policy != "mga_large_value_locator" ||
        !CanonicalUuid(column.column_uuid.canonical) ||
        !ExactGraphValueDescriptor(column.value_descriptor, kTypes[ordinal],
                                   expected_type_uuid,
                                   expected_registry_identity,
                                   column.column_uuid.canonical,
                                   kNullable[ordinal])) {
      return false;
    }
  }
  return true;
}

std::string DerivedUuid(const std::string_view seed) {
  std::uint64_t high = 1469598103934665603ULL;
  std::uint64_t low = 1099511628211ULL;
  for (const auto ch : seed) {
    high = (high ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
    low = (low + static_cast<unsigned char>(ch)) * 1469598103934665603ULL;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string raw(32, '0');
  for (std::size_t index = 0; index < 16; ++index) {
    raw[index] = kHex[(high >> ((15 - index) * 4)) & 0xf];
    raw[16 + index] = kHex[(low >> ((15 - index) * 4)) & 0xf];
  }
  raw[12] = '7';
  raw[16] = kHex[(static_cast<unsigned>(raw[16] <= '9'
                                            ? raw[16] - '0'
                                            : raw[16] - 'a' + 10) &
                  0x3) |
                 0x8];
  return raw.substr(0, 8) + "-" + raw.substr(8, 4) + "-" +
         raw.substr(12, 4) + "-" + raw.substr(16, 4) + "-" +
         raw.substr(20, 12);
}

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
  return request.physical_query || request.persistent_graph_source ||
         request.physical_proof.proof_supplied ||
         !request.vertices.empty() || !request.edges.empty() ||
         !request.seed_vertex_ids.empty() || !request.seed_label.empty() ||
         !request.seed_property_key.empty() ||
         !request.fused_candidate_seed_vertex_ids.empty() ||
         request.fusion_source_kind != EngineGraphFusionSourceKind::kNone ||
         !request.edge_type_filter.empty() ||
         !request.bidirectional_start_vertex_id.empty() ||
         !request.bidirectional_end_vertex_id.empty() ||
         request.min_depth != 0 || request.max_depth != 1 ||
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

std::vector<const EngineGraphVertexInput*> SortedVertices(
    const std::vector<EngineGraphVertexInput>& vertices) {
  std::vector<const EngineGraphVertexInput*> sorted;
  sorted.reserve(vertices.size());
  for (const auto& vertex : vertices) sorted.push_back(&vertex);
  std::sort(sorted.begin(),
            sorted.end(),
            [](const EngineGraphVertexInput* left,
               const EngineGraphVertexInput* right) {
              return left->vertex_id < right->vertex_id;
            });
  return sorted;
}

std::vector<const EngineGraphEdgeInput*> SortedEdges(
    const std::vector<EngineGraphEdgeInput>& edges) {
  std::vector<const EngineGraphEdgeInput*> sorted;
  sorted.reserve(edges.size());
  for (const auto& edge : edges) sorted.push_back(&edge);
  std::sort(sorted.begin(),
            sorted.end(),
            [](const EngineGraphEdgeInput* left,
               const EngineGraphEdgeInput* right) {
              if (left->source_vertex_id != right->source_vertex_id) {
                return left->source_vertex_id < right->source_vertex_id;
              }
              if (left->target_vertex_id != right->target_vertex_id) {
                return left->target_vertex_id < right->target_vertex_id;
              }
              if (left->edge_type != right->edge_type) {
                return left->edge_type < right->edge_type;
              }
              return left->edge_id < right->edge_id;
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
    const EngineGraphQueryRequest& request,
    const std::size_t maximum_seed_count =
        std::numeric_limits<std::size_t>::max()) {
  std::vector<std::string> seeds;
  std::set<std::string> seen;
  const auto add_seed = [&](const std::string& value) {
    if (seeds.size() <= maximum_seed_count) {
      AddUnique(&seeds, &seen, value);
    }
  };
  for (const auto& seed : request.seed_vertex_ids) {
    add_seed(seed);
  }

  if (!request.seed_label.empty() || !request.seed_property_key.empty()) {
    for (const auto* vertex : SortedVertices(request.vertices)) {
      const bool label_matches =
          request.seed_label.empty() || HasLabel(*vertex, request.seed_label);
      const bool property_matches =
          request.seed_property_key.empty() ||
          HasProperty(*vertex,
                      request.seed_property_key,
                      request.seed_property_value);
      if (label_matches && property_matches) {
        add_seed(vertex->vertex_id);
      }
    }
  }

  for (const auto& seed : request.fused_candidate_seed_vertex_ids) {
    add_seed(seed);
  }
  if (request.persistent_graph_source && seeds.empty() &&
      request.seed_label.empty() && request.seed_property_key.empty()) {
    for (const auto* vertex : SortedVertices(request.vertices)) {
      add_seed(vertex->vertex_id);
    }
  }
  return seeds;
}

struct GraphCorpusValidation {
  bool ok = false;
  const char* diagnostic_id = "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1";
};

GraphCorpusValidation ValidateDirectGraphCorpus(
    const EngineGraphQueryRequest& request) {
  const auto invalid = [] {
    return GraphCorpusValidation{
        false, "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"};
  };
  if (request.cycle_policy != EngineGraphCyclePolicy::kVisitedSet) {
    return {false, kGraphUnboundedExpansionRefused};
  }
  if ((!request.graph_object_uuid.empty() &&
       !CanonicalUuid(request.graph_object_uuid)) ||
      (request.direction != EngineGraphTraversalDirection::kOutgoing &&
       request.direction != EngineGraphTraversalDirection::kIncoming &&
       request.direction != EngineGraphTraversalDirection::kBoth) ||
      (request.fusion_source_kind != EngineGraphFusionSourceKind::kNone &&
       request.fusion_source_kind != EngineGraphFusionSourceKind::kVector &&
       request.fusion_source_kind != EngineGraphFusionSourceKind::kSearch &&
       request.fusion_source_kind != EngineGraphFusionSourceKind::kDocument &&
       request.fusion_source_kind != EngineGraphFusionSourceKind::kSql) ||
      (request.seed_property_key.empty() &&
       !request.seed_property_value.empty()) ||
      (request.fused_candidate_seed_vertex_ids.empty() !=
       (request.fusion_source_kind == EngineGraphFusionSourceKind::kNone)) ||
      (request.bidirectional_start_vertex_id.empty() !=
       request.bidirectional_end_vertex_id.empty())) {
    return invalid();
  }

  for (std::size_t vertex_ordinal = 0;
       vertex_ordinal < request.vertices.size(); ++vertex_ordinal) {
    if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
      return {false, diagnostic};
    }
    const auto& vertex = request.vertices[vertex_ordinal];
    if (!CanonicalUuid(vertex.vertex_id) ||
        std::ranges::any_of(
            request.vertices.begin(),
            request.vertices.begin() + vertex_ordinal,
            [&](const auto& prior) {
              return prior.vertex_id == vertex.vertex_id;
            })) {
      return invalid();
    }
    for (std::size_t label_ordinal = 0;
         label_ordinal < vertex.labels.size(); ++label_ordinal) {
      if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
        return {false, diagnostic};
      }
      if (vertex.labels[label_ordinal].empty() ||
          std::ranges::find(vertex.labels.begin(),
                            vertex.labels.begin() + label_ordinal,
                            vertex.labels[label_ordinal]) !=
              vertex.labels.begin() + label_ordinal) {
        return invalid();
      }
    }
    for (std::size_t property_ordinal = 0;
         property_ordinal < vertex.properties.size(); ++property_ordinal) {
      if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
        return {false, diagnostic};
      }
      const auto& property = vertex.properties[property_ordinal];
      if (property.key.empty() ||
          std::ranges::any_of(
              vertex.properties.begin(),
              vertex.properties.begin() + property_ordinal,
              [&](const auto& prior) { return prior.key == property.key; })) {
        return invalid();
      }
    }
  }
  for (std::size_t edge_ordinal = 0; edge_ordinal < request.edges.size();
       ++edge_ordinal) {
    if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
      return {false, diagnostic};
    }
    const auto& edge = request.edges[edge_ordinal];
    const auto vertex_exists = [&](const std::string& vertex_id) {
      return std::ranges::any_of(request.vertices, [&](const auto& vertex) {
        return vertex.vertex_id == vertex_id;
      });
    };
    if (!CanonicalUuid(edge.edge_id) ||
        !CanonicalUuid(edge.source_vertex_id) ||
        !CanonicalUuid(edge.target_vertex_id) || edge.edge_type.empty() ||
        !std::isfinite(edge.weight) || vertex_exists(edge.edge_id) ||
        std::ranges::any_of(
            request.edges.begin(), request.edges.begin() + edge_ordinal,
            [&](const auto& prior) { return prior.edge_id == edge.edge_id; }) ||
        !vertex_exists(edge.source_vertex_id) ||
        !vertex_exists(edge.target_vertex_id)) {
      return invalid();
    }
    for (std::size_t property_ordinal = 0;
         property_ordinal < edge.properties.size(); ++property_ordinal) {
      if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
        return {false, diagnostic};
      }
      const auto& property = edge.properties[property_ordinal];
      if (property.key.empty() ||
          std::ranges::any_of(
              edge.properties.begin(),
              edge.properties.begin() + property_ordinal,
              [&](const auto& prior) { return prior.key == property.key; })) {
        return invalid();
      }
    }
  }
  const auto exact_vertex_reference = [&](const std::string& vertex_id) {
    return CanonicalUuid(vertex_id) &&
           std::ranges::any_of(request.vertices, [&](const auto& vertex) {
             return vertex.vertex_id == vertex_id;
           });
  };
  for (const auto& seed : request.seed_vertex_ids) {
    if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
      return {false, diagnostic};
    }
    if (!exact_vertex_reference(seed)) return invalid();
  }
  for (const auto& seed : request.fused_candidate_seed_vertex_ids) {
    if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
      return {false, diagnostic};
    }
    if (!exact_vertex_reference(seed)) return invalid();
  }
  if (!request.bidirectional_start_vertex_id.empty() &&
      (!exact_vertex_reference(request.bidirectional_start_vertex_id) ||
       !exact_vertex_reference(request.bidirectional_end_vertex_id))) {
    return invalid();
  }
  return {true, nullptr};
}

bool EdgeTypeMatches(const EngineGraphQueryRequest& request,
                     const EngineGraphEdgeInput& edge) {
  return request.edge_type_filter.empty() ||
         edge.edge_type == request.edge_type_filter;
}

std::vector<std::pair<const EngineGraphEdgeInput*, std::string_view>>
AdjacentEdges(
    const EngineGraphQueryRequest& request,
    const std::vector<const EngineGraphEdgeInput*>& sorted_edges,
    const std::string_view vertex_id) {
  std::vector<std::pair<const EngineGraphEdgeInput*, std::string_view>>
      adjacent;
  for (const auto* edge : sorted_edges) {
    if (!EdgeTypeMatches(request, *edge)) {
      continue;
    }
    if (request.direction != EngineGraphTraversalDirection::kIncoming &&
        edge->source_vertex_id == vertex_id) {
      adjacent.push_back({edge, edge->target_vertex_id});
    }
    if (request.direction != EngineGraphTraversalDirection::kOutgoing &&
        edge->target_vertex_id == vertex_id) {
      adjacent.push_back({edge, edge->source_vertex_id});
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

std::size_t TraversalStateByteLimit(
    const EngineGraphQueryRequest& request,
    const std::uint64_t memory_budget_bytes) {
  // A state owns two path strings plus a visited-set copy. Reserve a
  // deliberately conservative 256 bytes for every possible path depth in
  // addition to the fixed frontier containers and the traversal/result-row
  // identity materialization. This bounds allocation before any state or row
  // is appended; exact exchange accounting remains a second, narrower
  // publication boundary.
  constexpr std::uint64_t kFixedStateBytes = 2048;
  constexpr std::uint64_t kPerDepthStateBytes = 256;
  constexpr std::uint64_t kViewFixedBytes = 1024;
  constexpr std::uint64_t kVertexViewBytes = 2 * sizeof(void*);
  constexpr std::uint64_t kEdgeViewBytes =
      2 * (sizeof(void*) + sizeof(std::string_view) + 64);
  const auto vertex_count = static_cast<std::uint64_t>(request.vertices.size());
  const auto edge_count = static_cast<std::uint64_t>(request.edges.size());
  if (vertex_count >
          (std::numeric_limits<std::uint64_t>::max() - kViewFixedBytes) /
              kVertexViewBytes ||
      edge_count >
          (std::numeric_limits<std::uint64_t>::max() - kViewFixedBytes -
           vertex_count * kVertexViewBytes) /
              kEdgeViewBytes) {
    return 0;
  }
  const auto view_bytes =
      kViewFixedBytes + vertex_count * kVertexViewBytes +
      edge_count * kEdgeViewBytes;
  if (memory_budget_bytes <= view_bytes) return 0;
  if (request.max_depth == std::numeric_limits<std::uint64_t>::max()) {
    return 0;
  }
  const auto depth_count = request.max_depth + 1;
  if (depth_count >
      (std::numeric_limits<std::uint64_t>::max() - kFixedStateBytes) /
          kPerDepthStateBytes) {
    return 0;
  }
  const auto bytes_per_state =
      kFixedStateBytes + depth_count * kPerDepthStateBytes;
  return static_cast<std::size_t>(
      (memory_budget_bytes - view_bytes) / bytes_per_state);
}

GraphTraversalResult TraverseFrontiers(
    const EngineGraphQueryRequest& request,
    const std::uint64_t memory_budget_bytes,
    EngineApiU64* frontier_batches,
    EngineApiU64* adjacency_page_reads) {
  const auto state_byte_limit =
      TraversalStateByteLimit(request, memory_budget_bytes);
  if (state_byte_limit == 0) return {true, {}};
  const auto row_limit = std::min<std::size_t>(
      static_cast<std::size_t>(request.maximum_output_rows),
      state_byte_limit);
  const auto seeds = ResolveSeedVertices(request, row_limit);
  const auto sorted_edges = SortedEdges(request.edges);
  const EngineApiU64 batch_size = OptionBatchSize(request).value_or(2);

  GraphTraversalResult result;
  auto& rows = result.rows;
  std::vector<GraphFrontierState> frontier;
  if (row_limit == 0 || seeds.size() > row_limit) {
    result.resource_exhausted = true;
    return result;
  }
  for (const auto& seed : seeds) {
    const auto cancellation = PollGraphCancellation(request);
    if (cancellation != GraphCancellationProbe::kContinue) {
      result.cancelled = cancellation == GraphCancellationProbe::kCancelled;
      result.coordinator_failed =
          cancellation == GraphCancellationProbe::kCoordinatorFailed;
      result.rows.clear();
      return result;
    }
    GraphFrontierState state;
    state.vertex_id = seed;
    state.path = seed;
    state.path_identity = seed;
    state.visited.insert(seed);
    frontier.push_back(state);
    rows.push_back({seed, {}, {}, 0.0, seed, seed, 0});
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
        const auto cancellation = PollGraphCancellation(request);
        if (cancellation != GraphCancellationProbe::kContinue) {
          result.cancelled =
              cancellation == GraphCancellationProbe::kCancelled;
          result.coordinator_failed =
              cancellation == GraphCancellationProbe::kCoordinatorFailed;
          result.rows.clear();
          return result;
        }
        const auto adjacent = AdjacentEdges(request, sorted_edges, frontier[i].vertex_id);
        ++(*adjacency_page_reads);
        for (const auto& [edge, next_vertex] : adjacent) {
          const auto cancellation = PollGraphCancellation(request);
          if (cancellation != GraphCancellationProbe::kContinue) {
            result.cancelled =
                cancellation == GraphCancellationProbe::kCancelled;
            result.coordinator_failed =
                cancellation == GraphCancellationProbe::kCoordinatorFailed;
            result.rows.clear();
            return result;
          }
          const std::string next_vertex_id(next_vertex);
          if (request.cycle_policy == EngineGraphCyclePolicy::kVisitedSet &&
              frontier[i].visited.find(next_vertex_id) !=
                  frontier[i].visited.end()) {
            continue;
          }
          GraphFrontierState next_state;
          next_state.vertex_id = next_vertex_id;
          next_state.path = frontier[i].path + "->" + next_state.vertex_id;
          next_state.path_identity =
              frontier[i].path_identity + "-[" + edge->edge_id + "]->" +
              next_state.vertex_id;
          next_state.visited = frontier[i].visited;
          next_state.visited.insert(next_state.vertex_id);
          if (rows.size() >= row_limit) {
            result.resource_exhausted = true;
            return result;
          }
          rows.push_back({next_state.vertex_id,
                          edge->edge_id,
                          edge->edge_type,
                          edge->weight,
                          next_state.path,
                          next_state.path_identity,
                          depth});
          next_frontier.push_back(std::move(next_state));
        }
      }
    }
    frontier = std::move(next_frontier);
  }
  return result;
}

std::vector<std::pair<const EngineGraphEdgeInput*, std::string_view>>
ReverseAdjacentEdges(
    const EngineGraphQueryRequest& request,
    const std::vector<const EngineGraphEdgeInput*>& sorted_edges,
    const std::string_view vertex_id) {
  std::vector<std::pair<const EngineGraphEdgeInput*, std::string_view>>
      adjacent;
  for (const auto* edge : sorted_edges) {
    if (!EdgeTypeMatches(request, *edge)) {
      continue;
    }
    if (request.direction != EngineGraphTraversalDirection::kIncoming &&
        edge->target_vertex_id == vertex_id) {
      adjacent.push_back({edge, edge->source_vertex_id});
    }
    if (request.direction != EngineGraphTraversalDirection::kOutgoing &&
        edge->source_vertex_id == vertex_id) {
      adjacent.push_back({edge, edge->target_vertex_id});
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
    const std::vector<const EngineGraphEdgeInput*>& sorted_edges) {
  std::vector<GraphTraversalRow> rows;
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    std::string_view edge_type;
    double edge_weight = 0.0;
    if (i != 0) {
      for (const auto* edge : sorted_edges) {
        if (edge->edge_id == edge_ids[i - 1]) {
          edge_type = edge->edge_type;
          edge_weight = edge->weight;
          break;
        }
      }
    }
    rows.push_back({vertices[i],
                    i == 0 ? std::string{} : edge_ids[i - 1],
                    edge_type,
                    edge_weight,
                    JoinPath(vertices, i),
                    i == 0
                        ? vertices.front()
                        : rows.back().path_identity + "-[" +
                              edge_ids[i - 1] + "]->" + vertices[i],
                    static_cast<EngineApiU64>(i)});
  }
  return rows;
}

GraphTraversalResult BidirectionalPath(
    const EngineGraphQueryRequest& request,
    const std::uint64_t memory_budget_bytes,
    EngineApiU64* frontier_batches,
    EngineApiU64* adjacency_page_reads) {
  const auto start = request.bidirectional_start_vertex_id;
  const auto goal = request.bidirectional_end_vertex_id;
  const auto state_byte_limit =
      TraversalStateByteLimit(request, memory_budget_bytes);
  if (state_byte_limit == 0) return {true, {}};
  const auto sorted_edges = SortedEdges(request.edges);
  if (start.empty() || goal.empty()) {
    return {};
  }
  if (start == goal) {
    if (request.maximum_output_rows < 1 || state_byte_limit < 1) {
      return {true, {}};
    }
    return {false, {{start, {}, {}, 0.0, start, start, 0}}};
  }
  if (request.maximum_output_rows < 2 || state_byte_limit < 2) {
    return {true, {}};
  }

  const EngineApiU64 batch_size = OptionBatchSize(request).value_or(2);
  std::vector<std::string> forward_frontier = {start};
  std::vector<std::string> backward_frontier = {goal};
  std::map<std::string, GraphPathState> forward_paths;
  std::map<std::string, GraphPathState> backward_paths;
  forward_paths[start] = {{start}, {}};
  backward_paths[goal] = {{goal}, {}};
  bool resource_exhausted = false;
  bool cancelled = false;
  bool coordinator_failed = false;

  const auto build_result = [&](const std::string& meet_vertex) {
    std::vector<std::string> vertices = forward_paths[meet_vertex].vertices;
    std::vector<std::string> edge_ids = forward_paths[meet_vertex].edge_ids;
    const auto& backward = backward_paths[meet_vertex];
    vertices.insert(vertices.end(), backward.vertices.begin() + 1,
                    backward.vertices.end());
    edge_ids.insert(edge_ids.end(), backward.edge_ids.begin(),
                    backward.edge_ids.end());
    auto rows = BuildPathRows(vertices, edge_ids, sorted_edges);
    if (rows.size() > request.maximum_output_rows) {
      resource_exhausted = true;
      rows.clear();
    }
    return rows;
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
          const auto cancellation = PollGraphCancellation(request);
          if (cancellation != GraphCancellationProbe::kContinue) {
            cancelled = cancellation == GraphCancellationProbe::kCancelled;
            coordinator_failed =
                cancellation == GraphCancellationProbe::kCoordinatorFailed;
            return true;
          }
          ++(*frontier_batches);
          const auto batch_end =
              std::min(frontier->size(),
                       batch_start + static_cast<std::size_t>(batch_size));
          for (std::size_t i = batch_start; i < batch_end; ++i) {
            const auto cancellation = PollGraphCancellation(request);
            if (cancellation != GraphCancellationProbe::kContinue) {
              cancelled =
                  cancellation == GraphCancellationProbe::kCancelled;
              coordinator_failed =
                  cancellation == GraphCancellationProbe::kCoordinatorFailed;
              return true;
            }
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
              const auto cancellation = PollGraphCancellation(request);
              if (cancellation != GraphCancellationProbe::kContinue) {
                cancelled =
                    cancellation == GraphCancellationProbe::kCancelled;
                coordinator_failed =
                    cancellation == GraphCancellationProbe::kCoordinatorFailed;
                return true;
              }
              const std::string next_vertex_id(next_vertex);
              if (own_paths->find(next_vertex_id) != own_paths->end()) {
                continue;
              }
              GraphPathState next_state = path_it->second;
              if (reverse) {
                next_state.vertices.insert(next_state.vertices.begin(),
                                           next_vertex_id);
                next_state.edge_ids.insert(next_state.edge_ids.begin(), edge->edge_id);
              } else {
                next_state.vertices.push_back(next_vertex_id);
                next_state.edge_ids.push_back(edge->edge_id);
              }
              if (next_state.edge_ids.size() > request.max_depth) {
                continue;
              }
              const auto other_it = other_paths.find(next_vertex_id);
              if (other_it != other_paths.end() &&
                  next_state.edge_ids.size() + other_it->second.edge_ids.size() <=
                      request.max_depth) {
                (*own_paths)[next_vertex_id] = next_state;
                *result = build_result(next_vertex_id);
                return true;
              }
              if (own_paths->size() + other_paths.size() >=
                  state_byte_limit) {
                resource_exhausted = true;
                return true;
              }
              (*own_paths)[next_vertex_id] = next_state;
              next_frontier.push_back(next_vertex_id);
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
      return {resource_exhausted, std::move(result), cancelled,
              coordinator_failed};
    }
    if (!backward_frontier.empty() &&
        expand_frontier(&backward_frontier,
                        &backward_paths,
                        forward_paths,
                        true,
                        &result)) {
      return {resource_exhausted, std::move(result), cancelled,
              coordinator_failed};
    }
  }
  return {resource_exhausted, std::move(result), cancelled,
          coordinator_failed};
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

struct GraphResultMaterializationPreflight {
  bool ok = false;
  bool resource_exhausted = false;
  bool cancelled = false;
  bool coordinator_failed = false;
  std::string detail;
};

bool ReserveGraphResultBytes(const std::uint64_t bytes,
                             const std::uint64_t limit,
                             std::uint64_t* used) {
  if (bytes > std::numeric_limits<std::uint64_t>::max() - *used ||
      *used + bytes > limit) {
    return false;
  }
  *used += bytes;
  return true;
}

bool ReserveScaledGraphString(const std::string_view value,
                              const std::uint64_t multiplier,
                              const std::uint64_t limit,
                              std::uint64_t* used) {
  const auto size = static_cast<std::uint64_t>(value.size());
  if (multiplier != 0 &&
      size > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    return false;
  }
  return ReserveGraphResultBytes(size * multiplier, limit, used);
}

GraphResultMaterializationPreflight PreflightGraphResultMaterialization(
    const EngineGraphQueryRequest& request,
    const std::vector<std::string>& seeds,
    const std::vector<GraphTraversalRow>& rows,
    const std::uint64_t memory_budget_bytes) {
  GraphResultMaterializationPreflight preflight;
  const auto polling_refused = [&] {
    const auto cancellation = PollGraphCancellation(request);
    if (cancellation == GraphCancellationProbe::kContinue) return false;
    preflight.cancelled = cancellation == GraphCancellationProbe::kCancelled;
    preflight.coordinator_failed =
        cancellation == GraphCancellationProbe::kCoordinatorFailed;
    preflight.detail = preflight.cancelled
                           ? "graph result materialization was cancelled"
                           : "graph cancellation coordinator probe failed";
    return true;
  };
  std::uint64_t used = 0;
  // Ordered lookup evidence and API behavior rows both copy dynamic values.
  // Reserve each dynamic input for every intermediate/final copy plus fixed
  // vector nodes, typed values, descriptors, UUIDs, field names, evidence,
  // and allocator slack. This completes before either output layer receives
  // its first item, so refusal cannot expose a partial result.
  constexpr std::uint64_t kResultFixedBytes = 32 * 1024;
  constexpr std::uint64_t kSeedFixedBytes = 8 * 1024;
  constexpr std::uint64_t kRowFixedBytes = 16 * 1024;
  constexpr std::uint64_t kDynamicCopyMultiplier = 16;
  if (!ReserveGraphResultBytes(kResultFixedBytes, memory_budget_bytes,
                               &used)) {
    preflight.resource_exhausted = true;
    preflight.detail = "graph result fixed materialization exceeded budget";
    return preflight;
  }
  for (const auto& seed : seeds) {
    if (polling_refused()) return preflight;
    if (!ReserveGraphResultBytes(kSeedFixedBytes, memory_budget_bytes, &used) ||
        !ReserveScaledGraphString(seed, kDynamicCopyMultiplier,
                                  memory_budget_bytes, &used)) {
      preflight.resource_exhausted = true;
      preflight.detail = "graph seed evidence materialization exceeded budget";
      return preflight;
    }
  }
  for (const auto& row : rows) {
    if (polling_refused()) return preflight;
    const auto vertex = std::ranges::find_if(
        request.vertices, [&](const auto& candidate) {
          return candidate.vertex_id == row.vertex_id;
        });
    const auto edge = std::ranges::find_if(
        request.edges, [&](const auto& candidate) {
          return candidate.edge_id == row.edge_id;
        });
    if (vertex == request.vertices.end() || row.vertex_id.empty() ||
        row.path.empty() || row.path_identity.empty() ||
        (row.depth == 0 && !row.edge_id.empty()) ||
        (row.depth > 0 && edge == request.edges.end()) ||
        !std::isfinite(row.edge_weight)) {
      preflight.detail =
          "graph traversal row does not resolve exact vertex/edge identity";
      return preflight;
    }
    if (!ReserveGraphResultBytes(kRowFixedBytes, memory_budget_bytes, &used) ||
        !ReserveScaledGraphString(row.vertex_id, kDynamicCopyMultiplier,
                                  memory_budget_bytes, &used) ||
        !ReserveScaledGraphString(row.edge_id, kDynamicCopyMultiplier,
                                  memory_budget_bytes, &used) ||
        !ReserveScaledGraphString(row.edge_type, kDynamicCopyMultiplier,
                                  memory_budget_bytes, &used) ||
        !ReserveScaledGraphString(row.path, kDynamicCopyMultiplier,
                                  memory_budget_bytes, &used) ||
        !ReserveScaledGraphString(row.path_identity,
                                  kDynamicCopyMultiplier,
                                  memory_budget_bytes, &used) ||
        !ReserveScaledGraphString(request.graph_object_uuid,
                                  kDynamicCopyMultiplier,
                                  memory_budget_bytes, &used)) {
      preflight.resource_exhausted = true;
      preflight.detail = "graph result row materialization exceeded budget";
      return preflight;
    }
    for (const auto& label : vertex->labels) {
      if (polling_refused()) return preflight;
      if (!ReserveScaledGraphString(label, kDynamicCopyMultiplier,
                                    memory_budget_bytes, &used) ||
          !ReserveGraphResultBytes(kDynamicCopyMultiplier,
                                   memory_budget_bytes, &used)) {
        preflight.resource_exhausted = true;
        preflight.detail =
            "graph vertex-label materialization exceeded budget";
        return preflight;
      }
    }
    for (const auto& property : vertex->properties) {
      if (polling_refused()) return preflight;
      if (!ReserveScaledGraphString(property.key, kDynamicCopyMultiplier,
                                    memory_budget_bytes, &used) ||
          !ReserveScaledGraphString(property.value, kDynamicCopyMultiplier,
                                    memory_budget_bytes, &used) ||
          !ReserveGraphResultBytes(2 * kDynamicCopyMultiplier,
                                   memory_budget_bytes, &used)) {
        preflight.resource_exhausted = true;
        preflight.detail =
            "graph vertex-property materialization exceeded budget";
        return preflight;
      }
    }
    if (edge != request.edges.end()) {
      for (const auto& property : edge->properties) {
        if (polling_refused()) return preflight;
        if (!ReserveScaledGraphString(property.key, kDynamicCopyMultiplier,
                                      memory_budget_bytes, &used) ||
            !ReserveScaledGraphString(property.value,
                                      kDynamicCopyMultiplier,
                                      memory_budget_bytes, &used) ||
            !ReserveGraphResultBytes(2 * kDynamicCopyMultiplier,
                                     memory_budget_bytes, &used)) {
          preflight.resource_exhausted = true;
          preflight.detail =
              "graph edge-property materialization exceeded budget";
          return preflight;
        }
      }
    }
  }
  preflight.ok = true;
  return preflight;
}

struct PersistentGraphCorpus {
  bool ok = false;
  std::vector<EngineGraphVertexInput> vertices;
  std::vector<EngineGraphEdgeInput> edges;
  EngineApiU64 scanned_row_versions = 0;
  EngineApiU64 visibility_rechecks = 0;
  EngineApiU64 decoded_bytes = 0;
  EngineApiU64 retained_memory_bytes = 0;
  std::string diagnostic_id;
  std::string detail;
};

PersistentGraphCorpus LoadPersistentGraphCorpus(
    const EngineGraphQueryRequest& request) {
  PersistentGraphCorpus corpus;
  const auto invalid = [&](const char* diagnostic, std::string detail) {
    corpus.diagnostic_id = diagnostic;
    corpus.detail = std::move(detail);
    return corpus;
  };
  if (!CanonicalUuid(request.graph_object_uuid) ||
      request.bound_object_identity.object_uuid.canonical !=
          request.graph_object_uuid ||
      request.bound_object_identity.resolved_object_type != "graph" ||
      !CanonicalUuid(
          request.bound_object_identity.resolved_schema_uuid.canonical) ||
      request.bound_object_identity.catalog_generation_id == 0 ||
      request.bound_object_identity.security_epoch == 0 ||
      request.bound_object_identity.resource_epoch == 0 ||
      request.bound_object_identity.catalog_generation_id !=
          request.context.catalog_generation_id ||
      request.bound_object_identity.security_epoch !=
          request.context.security_epoch ||
      request.bound_object_identity.resource_epoch !=
          request.context.resource_epoch ||
      request.provider_generation == 0 ||
      request.maximum_scanned_row_versions == 0 ||
      request.maximum_decoded_bytes == 0 || request.maximum_output_rows == 0) {
    return invalid("SB_MODEL_BINDING_INCOMPLETE_V1",
                   "persistent graph object or engine authority is incomplete");
  }
  const auto authorization = EvaluateMaterializedAuthorization(
      request.context, request.context.authorization_context, "SELECT",
      request.graph_object_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return invalid("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                   "persistent graph SELECT authorization was refused");
  }
  const auto loaded =
      LoadMgaRelationStorageDescriptor(request.context,
                                       request.graph_object_uuid);
  if (!loaded.ok) {
    return invalid(kGraphExactFallbackUnavailable,
                   "persistent graph relation descriptor is unavailable");
  }
  const auto& relation = loaded.descriptor;
  if (relation.relation_uuid.canonical != request.graph_object_uuid ||
      relation.database_uuid.canonical !=
          request.context.database_uuid.canonical ||
      relation.schema_uuid.canonical !=
          request.bound_object_identity.resolved_schema_uuid.canonical ||
      relation.relation_kind != "table" ||
      relation.storage_profile != "local_mga_rowstore_v1" ||
      relation.descriptor_uuid.canonical.empty() ||
      relation.descriptor_generation != request.provider_generation ||
      !ExactGraphDescriptorCohort(relation)) {
    return invalid(kNoSqlProviderGenerationStale,
                   "persistent graph relation identity or generation drifted");
  }
  MgaVisibleHeapRelationReadRequest read_request;
  read_request.relation_uuid = request.graph_object_uuid;
  read_request.maximum_scanned_row_versions =
      request.maximum_scanned_row_versions;
  // Leave a deterministic half-budget for the normalized graph corpus while
  // decoded MGA rows are simultaneously live during validation.
  read_request.maximum_decoded_bytes = request.maximum_decoded_bytes / 2;
  if (read_request.maximum_decoded_bytes == 0) {
    return invalid("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                   "persistent graph decode budget is too small");
  }
  read_request.maximum_output_rows = request.maximum_scanned_row_versions;
  bool cancellation_probe_failed = false;
  read_request.cancellation_requested = [&] {
    const auto probe = PollGraphCancellation(request);
    cancellation_probe_failed =
        cancellation_probe_failed ||
        probe == GraphCancellationProbe::kCoordinatorFailed;
    return probe != GraphCancellationProbe::kContinue;
  };
  const auto read = ReadVisibleMgaHeapRelation(request.context, read_request);
  corpus.scanned_row_versions = read.scanned_row_version_count;
  corpus.visibility_rechecks = read.visibility_recheck_count;
  corpus.decoded_bytes = read.decoded_byte_count;
  if (cancellation_probe_failed) {
    return invalid("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                   "persistent graph cancellation coordinator probe failed");
  }
  if (!read.ok) {
    return invalid(read.diagnostic.code.empty()
                       ? kGraphExactFallbackUnavailable
                       : read.diagnostic.code.c_str(),
                   read.diagnostic.detail.empty()
                       ? "persistent graph relation is unavailable"
                       : read.diagnostic.detail);
  }

  std::uint64_t retained_bytes = sizeof(PersistentGraphCorpus) + 1024;
  const auto reserve_retained = [&](const std::uint64_t bytes) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - retained_bytes ||
        retained_bytes + bytes > request.maximum_decoded_bytes ||
        read.decoded_byte_count >
            request.maximum_decoded_bytes - retained_bytes - bytes) {
      return false;
    }
    retained_bytes += bytes;
    return true;
  };
  if (retained_bytes > request.maximum_decoded_bytes ||
      read.decoded_byte_count >
          request.maximum_decoded_bytes - retained_bytes) {
    return invalid("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                   "persistent graph decode and corpus budget was exceeded");
  }
  const auto string_reservation = [](const std::string& value) {
    constexpr std::uint64_t kAllocatorSlack = 32;
    return static_cast<std::uint64_t>(2 * sizeof(std::string) +
                                      value.size() + 1 + kAllocatorSlack);
  };
  for (const auto& row : read.visible_rows) {
    if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
      return invalid(diagnostic,
                     "persistent graph normalization was cancelled");
    }
    for (std::size_t left = 0; left < row.values.size(); ++left) {
      if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
        return invalid(diagnostic,
                       "persistent graph normalization was cancelled");
      }
      for (std::size_t right = left + 1; right < row.values.size(); ++right) {
        if (row.values[left].first == row.values[right].first) {
          return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                         "persistent graph row has duplicate field keys");
        }
      }
    }
    const auto value_for = [&](const std::string_view key)
        -> const std::string* {
      const auto value = std::ranges::find_if(
          row.values, [&](const auto& item) { return item.first == key; });
      return value == row.values.end() ? nullptr : &value->second;
    };
    const auto* kind = value_for("record_kind");
    if (kind == nullptr) {
      return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                     "persistent graph row has no record kind");
    }
    if (*kind == "vertex") {
      const auto* id = value_for("vertex_uuid");
      if (id == nullptr || !CanonicalUuid(*id) ||
          std::ranges::any_of(corpus.vertices, [&](const auto& vertex) {
            return vertex.vertex_id == *id;
          }) ||
          row.row_uuid != *id) {
        return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                       "persistent graph vertex identity is invalid");
      }
      if (!reserve_retained(512) ||
          !reserve_retained(string_reservation(*id))) {
        return invalid("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                       "persistent graph normalized corpus exceeded budget");
      }
      EngineGraphVertexInput vertex;
      vertex.vertex_id = *id;
      std::vector<std::pair<std::size_t, std::string>> ordered_labels;
      for (const auto& [key, value] : row.values) {
        if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
          return invalid(diagnostic,
                         "persistent graph normalization was cancelled");
        }
        if (key == "record_kind" || key == "vertex_uuid") {
          continue;
        } else if (key.starts_with("label.")) {
          const auto ordinal_text = std::string_view(key).substr(6);
          std::size_t ordinal = 0;
          const auto parsed = std::from_chars(
              ordinal_text.data(), ordinal_text.data() + ordinal_text.size(),
              ordinal);
          if (ordinal_text.empty() || value.empty() ||
              parsed.ec != std::errc{} ||
              parsed.ptr != ordinal_text.data() + ordinal_text.size() ||
              std::to_string(ordinal) != ordinal_text) {
            return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                           "persistent graph vertex label is malformed");
          }
          if (!reserve_retained(2 * sizeof(std::string)) ||
              !reserve_retained(string_reservation(value))) {
            return invalid(
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                "persistent graph normalized corpus exceeded budget");
          }
          ordered_labels.push_back({ordinal, value});
        } else if (key.starts_with("property.")) {
          if (key.size() == 9) {
            return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                           "persistent graph vertex property is malformed");
          }
          if (!reserve_retained(2 * sizeof(EngineGraphProperty)) ||
              !reserve_retained(string_reservation(key)) ||
              !reserve_retained(string_reservation(value))) {
            return invalid(
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                "persistent graph normalized corpus exceeded budget");
          }
          vertex.properties.push_back({key.substr(9), value});
        } else {
          return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                         "persistent graph vertex field is unsupported");
        }
      }
      std::ranges::sort(ordered_labels, [](const auto& left, const auto& right) {
        return left.first < right.first;
      });
      for (std::size_t ordinal = 0; ordinal < ordered_labels.size(); ++ordinal) {
        if (ordered_labels[ordinal].first != ordinal) {
          return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                         "persistent graph vertex label ordinals are not contiguous");
        }
        if (std::ranges::any_of(
                vertex.labels, [&](const auto& label) {
                  return label == ordered_labels[ordinal].second;
                })) {
          return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                         "persistent graph vertex labels are not unique");
        }
        vertex.labels.push_back(std::move(ordered_labels[ordinal].second));
      }
      std::ranges::sort(vertex.properties, {}, &EngineGraphProperty::key);
      corpus.vertices.push_back(std::move(vertex));
    } else if (*kind == "edge") {
      const auto* id = value_for("edge_uuid");
      const auto* source = value_for("source_vertex_uuid");
      const auto* target = value_for("target_vertex_uuid");
      const auto* type = value_for("edge_type");
      const auto* weight = value_for("edge_weight");
      if (id == nullptr || source == nullptr || target == nullptr ||
          type == nullptr || weight == nullptr || !CanonicalUuid(*id) ||
          !CanonicalUuid(*source) || !CanonicalUuid(*target) ||
          type->empty() ||
          std::ranges::any_of(corpus.edges, [&](const auto& edge) {
            return edge.edge_id == *id;
          }) ||
          row.row_uuid != *id) {
        return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                       "persistent graph edge identity is invalid");
      }
      if (!reserve_retained(768) ||
          !reserve_retained(string_reservation(*id)) ||
          !reserve_retained(string_reservation(*source)) ||
          !reserve_retained(string_reservation(*target)) ||
          !reserve_retained(string_reservation(*type))) {
        return invalid("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                       "persistent graph normalized corpus exceeded budget");
      }
      EngineGraphEdgeInput edge;
      edge.edge_id = *id;
      edge.source_vertex_id = *source;
      edge.target_vertex_id = *target;
      edge.edge_type = *type;
      try {
        std::size_t consumed = 0;
        edge.weight = std::stod(*weight, &consumed);
        if (consumed != weight->size() || !std::isfinite(edge.weight) ||
            *weight != FormatWeight(edge.weight)) {
          return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                         "persistent graph edge weight is malformed");
        }
      } catch (...) {
        return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                       "persistent graph edge weight is malformed");
      }
      for (const auto& [key, value] : row.values) {
        if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
          return invalid(diagnostic,
                         "persistent graph normalization was cancelled");
        }
        if (key == "record_kind" || key == "edge_uuid" ||
            key == "source_vertex_uuid" || key == "target_vertex_uuid" ||
            key == "edge_type" || key == "edge_weight") {
          continue;
        } else if (key.starts_with("property.") && key.size() > 9) {
          if (!reserve_retained(2 * sizeof(EngineGraphProperty)) ||
              !reserve_retained(string_reservation(key)) ||
              !reserve_retained(string_reservation(value))) {
            return invalid(
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                "persistent graph normalized corpus exceeded budget");
          }
          edge.properties.push_back({key.substr(9), value});
        } else {
          return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                         "persistent graph edge field is unsupported");
        }
      }
      std::ranges::sort(edge.properties, {}, &EngineGraphProperty::key);
      corpus.edges.push_back(std::move(edge));
    } else {
      return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                     "persistent graph row kind is unsupported");
    }
  }
  corpus.retained_memory_bytes = retained_bytes;
  if (corpus.vertices.empty()) {
    return invalid(kGraphExactFallbackUnavailable,
                   "persistent graph has no visible vertex rows");
  }
  for (const auto& edge : corpus.edges) {
    if (const auto* diagnostic = GraphCancellationDiagnostic(request)) {
      return invalid(diagnostic,
                     "persistent graph normalization was cancelled");
    }
    const auto vertex_exists = [&](const std::string& vertex_id) {
      return std::ranges::any_of(corpus.vertices, [&](const auto& vertex) {
        return vertex.vertex_id == vertex_id;
      });
    };
    if (!vertex_exists(edge.source_vertex_id) ||
        !vertex_exists(edge.target_vertex_id)) {
      return invalid("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                     "persistent graph edge endpoint is not visible");
    }
  }
  corpus.ok = true;
  return corpus;
}

EngineGraphQueryResult PhysicalGraphQuery(const EngineGraphQueryRequest& request,
                                          const std::string& operation_id) {
  if (request.min_depth > request.max_depth) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id, kGraphUnboundedExpansionRefused);
  }
  if (auto failure = ValidatePhysicalProof<EngineGraphQueryResult>(
          request, operation_id, request.physical_proof)) {
    return *failure;
  }
  if (request.persistent_graph_source &&
      (request.physical_proof.provider_contract.provider_generation
               .required_generation != request.provider_generation ||
       request.physical_proof.provider_contract.provider_generation
               .available_generation != request.provider_generation)) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id, kNoSqlProviderGenerationStale);
  }
  std::optional<EngineGraphQueryRequest> persistent_executable;
  PersistentGraphCorpus persistent;
  if (request.persistent_graph_source) {
    if (!request.vertices.empty() || !request.edges.empty() ||
        request.cycle_policy != EngineGraphCyclePolicy::kVisitedSet) {
      return DiagnosticResult<EngineGraphQueryResult>(
          request.context, operation_id, kGraphUnboundedExpansionRefused);
    }
    persistent = LoadPersistentGraphCorpus(request);
    if (!persistent.ok) {
      return DiagnosticResult<EngineGraphQueryResult>(
          request.context, operation_id,
          persistent.diagnostic_id.empty()
              ? kGraphExactFallbackUnavailable
              : persistent.diagnostic_id.c_str());
    }
    if (persistent.retained_memory_bytes >= request.maximum_decoded_bytes) {
      return DiagnosticResult<EngineGraphQueryResult>(
          request.context, operation_id,
          "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1");
    }
    persistent_executable.emplace(request);
    auto& working = *persistent_executable;
    working.maximum_decoded_bytes =
        request.maximum_decoded_bytes - persistent.retained_memory_bytes;
    working.vertices = std::move(persistent.vertices);
    working.edges = std::move(persistent.edges);
  }
  const auto& executable = persistent_executable.has_value()
                               ? *persistent_executable
                               : request;
  if (executable.vertices.empty()) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id, kGraphVertexCorpusRequired);
  }
  const bool bidirectional_query =
      !executable.bidirectional_start_vertex_id.empty() ||
      !executable.bidirectional_end_vertex_id.empty();
  if (executable.maximum_decoded_bytes < 2) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id,
        "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1");
  }
  const auto traversal_memory_budget = executable.maximum_decoded_bytes / 2;
  const auto output_memory_budget =
      executable.maximum_decoded_bytes - traversal_memory_budget;
  const auto seed_limit = std::min<std::size_t>(
      static_cast<std::size_t>(executable.maximum_output_rows),
      TraversalStateByteLimit(executable, traversal_memory_budget));
  if (seed_limit == 0) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id,
        "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1");
  }
  const auto corpus_validation = ValidateDirectGraphCorpus(executable);
  if (!corpus_validation.ok) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id,
        corpus_validation.diagnostic_id);
  }
  const auto seeds = ResolveSeedVertices(executable, seed_limit);
  if (!bidirectional_query && seeds.size() > seed_limit) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id,
        "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1");
  }
  if (!bidirectional_query && seeds.empty()) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id, kGraphSeedRequired);
  }

  const auto selection =
      SelectLocalNoSqlPhysicalProvider(request.physical_proof.provider_contract);
  EngineApiU64 frontier_batches = 0;
  EngineApiU64 adjacency_page_reads = 0;
  auto traversal =
      bidirectional_query
          ? BidirectionalPath(executable, traversal_memory_budget,
                              &frontier_batches,
                              &adjacency_page_reads)
          : TraverseFrontiers(executable, traversal_memory_budget,
                              &frontier_batches,
                              &adjacency_page_reads);
  if (traversal.resource_exhausted) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id,
        "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1");
  }
  if (traversal.cancelled) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id, "SB_MODEL_EXECUTION_CANCELLED_V1");
  }
  if (traversal.coordinator_failed) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id,
        "SB_MODEL_COORDINATOR_LEG_FAILED_V1");
  }
  auto& traversal_rows = traversal.rows;
  std::erase_if(traversal_rows, [&](const auto& row) {
    return row.depth < request.min_depth;
  });

  const auto materialization = PreflightGraphResultMaterialization(
      executable, seeds, traversal_rows, output_memory_budget);
  if (!materialization.ok) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id,
        materialization.coordinator_failed
            ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
            : materialization.cancelled
            ? "SB_MODEL_EXECUTION_CANCELLED_V1"
            : materialization.resource_exhausted
            ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
            : "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1");
  }

  auto result =
      MakeApiBehaviorSuccess<EngineGraphQueryResult>(request.context, operation_id);
  AddGraphEvidence(&result,
                   selection,
                   executable,
                   static_cast<EngineApiU64>(seeds.size()),
                   frontier_batches,
                   adjacency_page_reads,
                   bidirectional_query);
  if (request.persistent_graph_source) {
    AddApiBehaviorEvidence(&result, "graph_source_authority",
                           "persistent_mga_relation_v1");
    AddApiBehaviorEvidence(&result, "graph_object_uuid",
                           request.graph_object_uuid);
    AddApiBehaviorEvidence(&result, "graph_provider_generation",
                           std::to_string(request.provider_generation));
    AddApiBehaviorEvidence(&result, "graph_base_row_mga_rechecks",
                           std::to_string(persistent.visibility_rechecks));
    AddApiBehaviorEvidence(&result, "graph_scanned_row_versions",
                           std::to_string(persistent.scanned_row_versions));
    AddApiBehaviorEvidence(&result, "graph_source_decoded_bytes",
                           std::to_string(persistent.decoded_bytes));
    AddApiBehaviorEvidence(&result, "graph_corpus_retained_memory_bytes",
                           std::to_string(persistent.retained_memory_bytes));
  }

  std::vector<EngineNoSqlBatchPointLookupItem> lookup_items;
  for (const auto& seed : seeds) {
    if (const auto* diagnostic = GraphCancellationDiagnostic(executable)) {
      return DiagnosticResult<EngineGraphQueryResult>(
          request.context, operation_id, diagnostic);
    }
    lookup_items.push_back(
        {seed,
         seed,
         0.0,
         "graph_seed_frontier",
         {{"frontier_role", "seed"}}});
  }
  for (const auto& row : traversal_rows) {
    if (const auto* diagnostic = GraphCancellationDiagnostic(executable)) {
      return DiagnosticResult<EngineGraphQueryResult>(
          request.context, operation_id, diagnostic);
    }
    const auto key = row.vertex_id + "|" + row.edge_id + "|" +
                     std::to_string(row.depth);
    lookup_items.push_back(
        {key,
         row.vertex_id,
         static_cast<double>(row.depth),
         row.path,
         {{"frontier_role", "traversal"},
         {"edge_type", std::string(row.edge_type)},
          {"direction", DirectionName(executable.direction)}}});
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
    if (const auto* diagnostic = GraphCancellationDiagnostic(executable)) {
      return DiagnosticResult<EngineGraphQueryResult>(
          request.context, operation_id, diagnostic);
    }
    const auto vertex = std::ranges::find_if(
        executable.vertices, [&](const auto& candidate) {
          return candidate.vertex_id == row.vertex_id;
        });
    const auto edge = std::ranges::find_if(
        executable.edges, [&](const auto& candidate) {
          return candidate.edge_id == row.edge_id;
        });
    const auto encode_labels = [](const auto& labels) {
      std::string encoded;
      for (const auto& label : labels) {
        if (!encoded.empty()) encoded += ',';
        encoded += label;
      }
      return encoded;
    };
    const auto encode_properties = [](const auto& properties) {
      std::string encoded;
      for (const auto& property : properties) {
        if (!encoded.empty()) encoded += ';';
        encoded += property.key + "=" + property.value;
      }
      return encoded;
    };
    AddApiBehaviorRow(
        &result,
        {{"surface", "graph"},
         {"vertex_id", row.vertex_id},
         {"vertex_uuid", row.vertex_id},
         {"edge_id", row.edge_id},
         {"edge_uuid", row.edge_id},
         {"edge_type", std::string(row.edge_type)},
         {"edge_weight", FormatWeight(row.edge_weight)},
         {"vertex_labels",
          vertex == executable.vertices.end()
              ? std::string{}
              : encode_labels(vertex->labels)},
         {"vertex_properties",
          vertex == executable.vertices.end()
              ? std::string{}
              : encode_properties(vertex->properties)},
         {"edge_properties",
          edge == executable.edges.end()
              ? std::string{}
              : encode_properties(edge->properties)},
         {"path", row.path},
         {"path_uuid",
          DerivedUuid(executable.graph_object_uuid + "|" +
                      row.path_identity)},
         {"depth", std::to_string(row.depth)},
         {"direction", DirectionName(executable.direction)},
         {"cycle_policy", CyclePolicyName(executable.cycle_policy)},
         {"fusion_source", FusionSourceName(executable.fusion_source_kind)},
         {"row_mga_recheck_required", "true"},
         {"row_security_recheck_required", "true"}});
  }
  result.dml_summary.index_probes = seeds.size() + adjacency_page_reads;
  result.dml_summary.visible_rows_scanned = persistent.visibility_rechecks;
  AddApiBehaviorEvidence(&result,
                         "graph_rows_returned",
                         std::to_string(result.result_shape.rows.size()));
  if (const auto* diagnostic = GraphCancellationDiagnostic(executable)) {
    return DiagnosticResult<EngineGraphQueryResult>(
        request.context, operation_id, diagnostic);
  }
  return result;
}

EngineGraphWriteResult StructuredGraphWrite(
    const EngineGraphWriteRequest& request,
    const std::string& operation_id) {
  if (!CanonicalUuid(request.graph_object_uuid) ||
      request.bound_object_identity.object_uuid.canonical !=
          request.graph_object_uuid ||
      request.bound_object_identity.resolved_object_type != "graph" ||
      !CanonicalUuid(
          request.bound_object_identity.resolved_schema_uuid.canonical) ||
      request.bound_object_identity.catalog_generation_id !=
          request.context.catalog_generation_id ||
      request.bound_object_identity.security_epoch !=
          request.context.security_epoch ||
      request.bound_object_identity.resource_epoch !=
          request.context.resource_epoch ||
      request.context.local_transaction_id == 0 ||
      request.provider_generation == 0 ||
      (request.vertices.empty() && request.edges.empty())) {
    return DiagnosticResult<EngineGraphWriteResult>(
        request.context, operation_id, "SB_MODEL_BINDING_INCOMPLETE_V1");
  }
  const auto authorization = EvaluateMaterializedAuthorization(
      request.context, request.context.authorization_context, "INSERT",
      request.graph_object_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return DiagnosticResult<EngineGraphWriteResult>(
        request.context, operation_id,
        "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1");
  }
  const auto loaded =
      LoadMgaRelationStorageDescriptor(request.context,
                                       request.graph_object_uuid);
  if (!loaded.ok ||
      loaded.descriptor.relation_uuid.canonical !=
          request.graph_object_uuid ||
      loaded.descriptor.database_uuid.canonical !=
          request.context.database_uuid.canonical ||
      loaded.descriptor.schema_uuid.canonical !=
          request.bound_object_identity.resolved_schema_uuid.canonical ||
      loaded.descriptor.relation_kind != "table" ||
      loaded.descriptor.storage_profile != "local_mga_rowstore_v1" ||
      loaded.descriptor.descriptor_uuid.canonical.empty() ||
      loaded.descriptor.descriptor_generation != request.provider_generation ||
      !ExactGraphDescriptorCohort(loaded.descriptor)) {
    return DiagnosticResult<EngineGraphWriteResult>(
        request.context, operation_id, kNoSqlProviderGenerationStale);
  }
  std::set<std::string> entity_ids;
  std::set<std::string> vertex_ids;
  std::vector<CrudRowVersionRecord> rows;
  const auto append_properties = [](
      const std::vector<EngineGraphProperty>& properties,
      std::vector<std::pair<std::string, std::string>>* values) {
    std::set<std::string> keys;
    for (const auto& property : properties) {
      if (property.key.empty() || !keys.insert(property.key).second) {
        return false;
      }
      values->push_back({"property." + property.key, property.value});
    }
    return true;
  };
  for (const auto& vertex : request.vertices) {
    std::set<std::string> labels;
    if (!CanonicalUuid(vertex.vertex_id) ||
        !entity_ids.insert(vertex.vertex_id).second ||
        std::ranges::any_of(vertex.labels, [&](const auto& label) {
          return label.empty() || !labels.insert(label).second;
        })) {
      return DiagnosticResult<EngineGraphWriteResult>(
          request.context, operation_id,
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1");
    }
    vertex_ids.insert(vertex.vertex_id);
    CrudRowVersionRecord row;
    row.creator_tx = request.context.local_transaction_id;
    row.table_uuid = request.graph_object_uuid;
    row.row_uuid = vertex.vertex_id;
    row.version_uuid = DerivedUuid(
        request.graph_object_uuid + "|vertex|" + vertex.vertex_id + "|" +
        std::to_string(request.context.local_transaction_id) + "|" +
        std::to_string(request.provider_generation));
    row.values = {{"record_kind", "vertex"},
                  {"vertex_uuid", vertex.vertex_id}};
    std::size_t ordinal = 0;
    for (const auto& label : vertex.labels) {
      row.values.push_back(
          {"label." + std::to_string(ordinal++), label});
    }
    if (!append_properties(vertex.properties, &row.values)) {
      return DiagnosticResult<EngineGraphWriteResult>(
          request.context, operation_id,
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1");
    }
    rows.push_back(std::move(row));
  }
  for (const auto& edge : request.edges) {
    if (!CanonicalUuid(edge.edge_id) ||
        !CanonicalUuid(edge.source_vertex_id) ||
        !CanonicalUuid(edge.target_vertex_id) || edge.edge_type.empty() ||
        !std::isfinite(edge.weight) || !entity_ids.insert(edge.edge_id).second ||
        !vertex_ids.contains(edge.source_vertex_id) ||
        !vertex_ids.contains(edge.target_vertex_id)) {
      return DiagnosticResult<EngineGraphWriteResult>(
          request.context, operation_id,
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1");
    }
    CrudRowVersionRecord row;
    row.creator_tx = request.context.local_transaction_id;
    row.table_uuid = request.graph_object_uuid;
    row.row_uuid = edge.edge_id;
    row.version_uuid = DerivedUuid(
        request.graph_object_uuid + "|edge|" + edge.edge_id + "|" +
        std::to_string(request.context.local_transaction_id) + "|" +
        std::to_string(request.provider_generation));
    row.values = {{"record_kind", "edge"},
                  {"edge_uuid", edge.edge_id},
                  {"source_vertex_uuid", edge.source_vertex_id},
                  {"target_vertex_uuid", edge.target_vertex_id},
                  {"edge_type", edge.edge_type},
                  {"edge_weight", FormatWeight(edge.weight)}};
    if (!append_properties(edge.properties, &row.values)) {
      return DiagnosticResult<EngineGraphWriteResult>(
          request.context, operation_id,
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1");
    }
    rows.push_back(std::move(row));
  }
  std::vector<std::uint64_t> event_sequences;
  const auto persisted =
      AppendMgaRowVersions(request.context, &rows, &event_sequences);
  if (persisted.error || event_sequences.size() != rows.size()) {
    return DiagnosticResult<EngineGraphWriteResult>(
        request.context, operation_id,
        persisted.code.empty() ? kGraphExactFallbackUnavailable
                               : persisted.code.c_str());
  }
  auto result =
      MakeApiBehaviorSuccess<EngineGraphWriteResult>(request.context,
                                                     operation_id);
  result.primary_object.uuid.canonical = request.graph_object_uuid;
  result.primary_object.object_kind = "graph";
  result.dml_summary.rows_changed = rows.size();
  result.dml_summary.append_calls = 1;
  AddEngineNoSqlSurfaceEvidence(&result, "graph",
                                "structured_mga_graph_write");
  AddApiBehaviorEvidence(&result, "graph_source_authority",
                         "persistent_mga_relation_v1");
  AddApiBehaviorEvidence(&result, "provider_transaction_finality_authority",
                         "false");
  AddApiBehaviorEvidence(&result, "provider_visibility_authority", "false");
  return result;
}

}  // namespace

bool EngineGraphDescriptorCohortExact(
    const MgaRelationStorageDescriptor& descriptor) {
  return ExactGraphDescriptorCohort(descriptor);
}

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
  if (request.structured_graph_persist) {
    return StructuredGraphWrite(request, kOperation);
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
