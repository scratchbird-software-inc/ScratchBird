// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "graph_adjacency_physical_provider.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'G', 'R',
                                               'A', 'D', '0', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u64 kMaxVertices = 1000000;
inline constexpr u64 kMaxEdges = 2000000;
inline constexpr u64 kMaxIndexEntries = 8000000;
inline constexpr u64 kFnvOffset = 14695981039346656037ull;
inline constexpr u64 kFnvPrime = 1099511628211ull;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

bool SameFormatVersion(PageExtentSummaryFormatVersion left,
                       PageExtentSummaryFormatVersion right) {
  return left.major == right.major && left.minor == right.minor;
}

bool LocatorValid(const TextInvertedRowLocator& locator) {
  return locator.row_ordinal > 0 &&
         PageExtentSummaryUuidTextValid(locator.row_uuid) &&
         PageExtentSummaryUuidTextValid(locator.version_uuid);
}

bool LocatorLess(const TextInvertedRowLocator& left,
                 const TextInvertedRowLocator& right) {
  return std::tie(left.row_ordinal, left.row_uuid, left.version_uuid) <
         std::tie(right.row_ordinal, right.row_uuid, right.version_uuid);
}

bool LocatorEqual(const TextInvertedRowLocator& left,
                  const TextInvertedRowLocator& right) {
  return !LocatorLess(left, right) && !LocatorLess(right, left);
}

bool StringVectorCleanAndSort(std::vector<std::string>* values) {
  for (const auto& value : *values) {
    if (value.empty()) return false;
  }
  std::sort(values->begin(), values->end());
  values->erase(std::unique(values->begin(), values->end()), values->end());
  return true;
}

bool PropertyLess(const GraphPropertyValue& left,
                  const GraphPropertyValue& right) {
  return std::tie(left.key, left.type_tag, left.encoded_value) <
         std::tie(right.key, right.type_tag, right.encoded_value);
}

bool PropertyEqual(const GraphPropertyValue& left,
                   const GraphPropertyValue& right) {
  return !PropertyLess(left, right) && !PropertyLess(right, left);
}

bool PropertyVectorEqual(const std::vector<GraphPropertyValue>& left,
                         const std::vector<GraphPropertyValue>& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), PropertyEqual);
}

bool CanonicalizeProperties(std::vector<GraphPropertyValue>* properties) {
  for (const auto& property : *properties) {
    if (property.key.empty() || property.type_tag.empty() ||
        property.encoded_value.empty()) {
      return false;
    }
  }
  std::sort(properties->begin(), properties->end(), PropertyLess);
  properties->erase(std::unique(properties->begin(), properties->end(),
                                PropertyEqual),
                    properties->end());
  return true;
}

bool RecheckAuthorityClean(const GraphRecheckProof& proof) {
  return !proof.parser_finality_authority_claimed &&
         !proof.reference_finality_authority_claimed &&
         !proof.provider_finality_authority_claimed &&
         !proof.index_finality_authority_claimed &&
         !proof.write_ahead_log_finality_authority_claimed &&
         !proof.visibility_authority_claimed &&
         !proof.security_authority_claimed &&
         !proof.transaction_finality_authority_claimed;
}

bool RecheckProofValid(const GraphRecheckProof& proof) {
  return proof.proof_supplied &&
         proof.exact_source_recheck_required &&
         proof.exact_source_available &&
         proof.mga_recheck_required &&
         proof.security_recheck_required &&
         !proof.evidence_ref.empty() &&
         RecheckAuthorityClean(proof);
}

bool DescriptorAuthorityClean(const GraphDescriptor& descriptor) {
  return !descriptor.parser_finality_authority_claimed &&
         !descriptor.reference_finality_authority_claimed &&
         !descriptor.provider_finality_authority_claimed &&
         !descriptor.index_finality_authority_claimed &&
         !descriptor.write_ahead_log_finality_authority_claimed;
}

bool DescriptorSafe(const GraphDescriptor& descriptor) {
  return descriptor.descriptor_epoch > 0 &&
         descriptor.deterministic &&
         descriptor.descriptor_safe &&
         descriptor.vertex_id_index &&
         descriptor.edge_source_adjacency &&
         descriptor.edge_target_adjacency &&
         descriptor.label_index &&
         descriptor.property_index &&
         descriptor.typed_edge_label_adjacency &&
         descriptor.frontier_batch_expansion &&
         descriptor.visited_compressed_bitmap &&
         !descriptor.descriptor_store_scan &&
         !descriptor.behavior_store_scan &&
         !descriptor.contract_only_fallback &&
         !descriptor.provider_only_fallback &&
         DescriptorAuthorityClean(descriptor);
}

bool ProviderAuthorityClean(const GraphAdjacencyPhysicalProvider& provider) {
  return !provider.visibility_authority_claimed &&
         !provider.security_authority_claimed &&
         !provider.transaction_finality_authority_claimed &&
         !provider.parser_finality_authority_claimed &&
         !provider.reference_finality_authority_claimed &&
         !provider.provider_finality_authority_claimed &&
         !provider.index_finality_authority_claimed &&
         !provider.write_ahead_log_finality_authority_claimed;
}

void AppendU8(std::vector<byte>* out, byte value) { out->push_back(value); }
void AppendBool(std::vector<byte>* out, bool value) {
  AppendU8(out, static_cast<byte>(value ? 1 : 0));
}
void AppendU32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u32));
  StoreLittle32(out->data() + offset, value);
}
void AppendU64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u64));
  StoreLittle64(out->data() + offset, value);
}
void AppendString(std::vector<byte>* out, const std::string& value) {
  AppendU32(out, static_cast<u32>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}
void AppendLocator(std::vector<byte>* out,
                   const TextInvertedRowLocator& locator) {
  AppendU64(out, locator.row_ordinal);
  AppendString(out, locator.row_uuid);
  AppendString(out, locator.version_uuid);
}
void AppendProperty(std::vector<byte>* out, const GraphPropertyValue& value) {
  AppendString(out, value.key);
  AppendString(out, value.type_tag);
  AppendString(out, value.encoded_value);
}
void AppendEntityKind(std::vector<byte>* out, GraphEntityKind kind) {
  AppendU32(out, static_cast<u32>(kind));
}

u64 ComputeChecksum(std::vector<byte> bytes) {
  if (bytes.size() >= kHeaderBytes) {
    std::fill(bytes.begin() + 16, bytes.begin() + 24, 0);
  }
  u64 hash = kFnvOffset;
  for (byte value : bytes) {
    hash ^= value;
    hash *= kFnvPrime;
  }
  return hash == 0 ? 1 : hash;
}

class Reader {
 public:
  explicit Reader(const std::vector<byte>& bytes) : bytes_(bytes) {}

  void SetOffset(std::size_t offset) { offset_ = offset; }
  bool Done() const { return offset_ == bytes_.size(); }

  bool ReadU8(byte* out) {
    if (offset_ + 1 > bytes_.size()) return false;
    *out = bytes_[offset_++];
    return true;
  }
  bool ReadBool(bool* out) {
    byte value = 0;
    if (!ReadU8(&value) || value > 1) return false;
    *out = value != 0;
    return true;
  }
  bool ReadU32(u32* out) {
    if (offset_ + sizeof(u32) > bytes_.size()) return false;
    *out = LoadLittle32(bytes_.data() + offset_);
    offset_ += sizeof(u32);
    return true;
  }
  bool ReadU64(u64* out) {
    if (offset_ + sizeof(u64) > bytes_.size()) return false;
    *out = LoadLittle64(bytes_.data() + offset_);
    offset_ += sizeof(u64);
    return true;
  }
  bool ReadString(std::string* out) {
    u32 size = 0;
    if (!ReadU32(&size) || offset_ + size > bytes_.size()) return false;
    out->assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }
  bool ReadLocator(TextInvertedRowLocator* locator) {
    return ReadU64(&locator->row_ordinal) &&
           ReadString(&locator->row_uuid) &&
           ReadString(&locator->version_uuid);
  }
  bool ReadProperty(GraphPropertyValue* value) {
    return ReadString(&value->key) &&
           ReadString(&value->type_tag) &&
           ReadString(&value->encoded_value);
  }
  bool ReadEntityKind(GraphEntityKind* kind) {
    u32 raw = 0;
    if (!ReadU32(&raw)) return false;
    if (raw == static_cast<u32>(GraphEntityKind::vertex)) {
      *kind = GraphEntityKind::vertex;
      return true;
    }
    if (raw == static_cast<u32>(GraphEntityKind::edge)) {
      *kind = GraphEntityKind::edge;
      return true;
    }
    return false;
  }

 private:
  const std::vector<byte>& bytes_;
  std::size_t offset_ = 0;
};

GraphBuildResult BuildFailure(std::string code,
                              std::string key,
                              std::string detail = {}) {
  GraphBuildResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeGraphAdjacencyPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

GraphOpenResult OpenFailure(GraphAdjacencyOpenClass open_class,
                            std::string code,
                            std::string key,
                            std::string detail = {}) {
  GraphOpenResult result;
  result.status = ErrorStatus();
  result.open_class = open_class;
  result.fail_closed = true;
  result.restricted_repair_required =
      open_class == GraphAdjacencyOpenClass::bad_checksum ||
      open_class == GraphAdjacencyOpenClass::corrupt_payload;
  result.diagnostic = MakeGraphAdjacencyPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

GraphQueryResult QueryFailure(std::string code,
                              std::string key,
                              std::string detail = {}) {
  GraphQueryResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeGraphAdjacencyPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

GraphFrontierExpandResult FrontierFailure(std::string code,
                                          std::string key,
                                          std::string detail = {}) {
  GraphFrontierExpandResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeGraphAdjacencyPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

GraphMutationResult MutationFailure(std::string code,
                                    std::string key,
                                    std::string detail = {}) {
  GraphMutationResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakeGraphAdjacencyPhysicalProviderDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

bool VertexInputValid(GraphVertexInput* vertex) {
  return !vertex->vertex_id.empty() &&
         LocatorValid(vertex->locator) &&
         !vertex->exact_source_recheck_evidence_ref.empty() &&
         StringVectorCleanAndSort(&vertex->labels) &&
         CanonicalizeProperties(&vertex->properties);
}

bool EdgeInputValid(GraphEdgeInput* edge) {
  return !edge->edge_id.empty() &&
         !edge->source_vertex_id.empty() &&
         !edge->target_vertex_id.empty() &&
         !edge->label.empty() &&
         LocatorValid(edge->locator) &&
         !edge->exact_source_recheck_evidence_ref.empty() &&
         CanonicalizeProperties(&edge->properties);
}

bool VertexRecordLess(const GraphVertexRecord& left,
                      const GraphVertexRecord& right) {
  return left.row.vertex_id < right.row.vertex_id;
}

bool EdgeRecordLess(const GraphEdgeRecord& left,
                    const GraphEdgeRecord& right) {
  return left.row.edge_id < right.row.edge_id;
}

bool VertexSame(const GraphVertexInput& left, const GraphVertexInput& right) {
  return left.vertex_id == right.vertex_id &&
         LocatorEqual(left.locator, right.locator) &&
         left.labels == right.labels &&
         PropertyVectorEqual(left.properties, right.properties) &&
         left.exact_source_recheck_evidence_ref ==
             right.exact_source_recheck_evidence_ref;
}

bool EdgeSame(const GraphEdgeInput& left, const GraphEdgeInput& right) {
  return left.edge_id == right.edge_id &&
         left.source_vertex_id == right.source_vertex_id &&
         left.target_vertex_id == right.target_vertex_id &&
         left.label == right.label &&
         LocatorEqual(left.locator, right.locator) &&
         PropertyVectorEqual(left.properties, right.properties) &&
         left.exact_source_recheck_evidence_ref ==
             right.exact_source_recheck_evidence_ref;
}

const GraphVertexRecord* FindVertex(const GraphAdjacencyPhysicalProvider& p,
                                    const std::string& vertex_id) {
  auto iter = std::lower_bound(
      p.vertices.begin(), p.vertices.end(), vertex_id,
      [](const GraphVertexRecord& row, const std::string& value) {
        return row.row.vertex_id < value;
      });
  if (iter == p.vertices.end() || iter->row.vertex_id != vertex_id) {
    return nullptr;
  }
  return &*iter;
}

const GraphEdgeRecord* FindEdge(const GraphAdjacencyPhysicalProvider& p,
                                const std::string& edge_id) {
  auto iter = std::lower_bound(
      p.edges.begin(), p.edges.end(), edge_id,
      [](const GraphEdgeRecord& row, const std::string& value) {
        return row.row.edge_id < value;
      });
  if (iter == p.edges.end() || iter->row.edge_id != edge_id) {
    return nullptr;
  }
  return &*iter;
}

bool VertexActive(const GraphAdjacencyPhysicalProvider& p,
                  const std::string& vertex_id) {
  const auto* vertex = FindVertex(p, vertex_id);
  return vertex != nullptr && !vertex->tombstoned;
}

bool VertexIdEntryLess(const GraphVertexIdIndexEntry& left,
                       const GraphVertexIdIndexEntry& right) {
  return std::tie(left.vertex_id, left.locator.row_ordinal, left.locator.row_uuid,
                  left.locator.version_uuid) <
         std::tie(right.vertex_id, right.locator.row_ordinal, right.locator.row_uuid,
                  right.locator.version_uuid);
}

bool AdjacencyEntryLess(const GraphAdjacencyIndexEntry& left,
                        const GraphAdjacencyIndexEntry& right) {
  return std::tie(left.vertex_id, left.edge_label, left.edge_id,
                  left.other_vertex_id, left.edge_locator.row_ordinal,
                  left.edge_locator.row_uuid, left.edge_locator.version_uuid) <
         std::tie(right.vertex_id, right.edge_label, right.edge_id,
                  right.other_vertex_id, right.edge_locator.row_ordinal,
                  right.edge_locator.row_uuid, right.edge_locator.version_uuid);
}

bool LabelEntryLess(const GraphLabelIndexEntry& left,
                    const GraphLabelIndexEntry& right) {
  return std::tie(left.label, left.entity_kind, left.entity_id,
                  left.locator.row_ordinal, left.locator.row_uuid,
                  left.locator.version_uuid) <
         std::tie(right.label, right.entity_kind, right.entity_id,
                  right.locator.row_ordinal, right.locator.row_uuid,
                  right.locator.version_uuid);
}

bool PropertyEntryLess(const GraphPropertyIndexEntry& left,
                       const GraphPropertyIndexEntry& right) {
  return std::tie(left.key, left.type_tag, left.encoded_value,
                  left.entity_kind, left.entity_id, left.locator.row_ordinal,
                  left.locator.row_uuid, left.locator.version_uuid) <
         std::tie(right.key, right.type_tag, right.encoded_value,
                  right.entity_kind, right.entity_id, right.locator.row_ordinal,
                  right.locator.row_uuid, right.locator.version_uuid);
}

bool IndexesEqual(const GraphAdjacencyPhysicalProvider& left,
                  const GraphAdjacencyPhysicalProvider& right) {
  return left.vertex_id_index.size() == right.vertex_id_index.size() &&
         left.edge_source_adjacency.size() == right.edge_source_adjacency.size() &&
         left.edge_target_adjacency.size() == right.edge_target_adjacency.size() &&
         left.label_index.size() == right.label_index.size() &&
         left.property_index.size() == right.property_index.size() &&
         left.typed_edge_label_adjacency.size() ==
             right.typed_edge_label_adjacency.size() &&
         std::equal(left.vertex_id_index.begin(), left.vertex_id_index.end(),
                    right.vertex_id_index.begin(),
                    [](const auto& a, const auto& b) {
                      return a.vertex_id == b.vertex_id &&
                             LocatorEqual(a.locator, b.locator);
                    }) &&
         std::equal(left.edge_source_adjacency.begin(),
                    left.edge_source_adjacency.end(),
                    right.edge_source_adjacency.begin(),
                    [](const auto& a, const auto& b) {
                      return a.vertex_id == b.vertex_id &&
                             a.edge_label == b.edge_label &&
                             a.edge_id == b.edge_id &&
                             a.other_vertex_id == b.other_vertex_id &&
                             LocatorEqual(a.edge_locator, b.edge_locator);
                    }) &&
         std::equal(left.edge_target_adjacency.begin(),
                    left.edge_target_adjacency.end(),
                    right.edge_target_adjacency.begin(),
                    [](const auto& a, const auto& b) {
                      return a.vertex_id == b.vertex_id &&
                             a.edge_label == b.edge_label &&
                             a.edge_id == b.edge_id &&
                             a.other_vertex_id == b.other_vertex_id &&
                             LocatorEqual(a.edge_locator, b.edge_locator);
                    }) &&
         std::equal(left.label_index.begin(), left.label_index.end(),
                    right.label_index.begin(),
                    [](const auto& a, const auto& b) {
                      return a.label == b.label &&
                             a.entity_kind == b.entity_kind &&
                             a.entity_id == b.entity_id &&
                             LocatorEqual(a.locator, b.locator);
                    }) &&
         std::equal(left.property_index.begin(), left.property_index.end(),
                    right.property_index.begin(),
                    [](const auto& a, const auto& b) {
                      return a.key == b.key &&
                             a.type_tag == b.type_tag &&
                             a.encoded_value == b.encoded_value &&
                             a.entity_kind == b.entity_kind &&
                             a.entity_id == b.entity_id &&
                             LocatorEqual(a.locator, b.locator);
                    }) &&
         std::equal(left.typed_edge_label_adjacency.begin(),
                    left.typed_edge_label_adjacency.end(),
                    right.typed_edge_label_adjacency.begin(),
                    [](const auto& a, const auto& b) {
                      return a.vertex_id == b.vertex_id &&
                             a.edge_label == b.edge_label &&
                             a.edge_id == b.edge_id &&
                             a.other_vertex_id == b.other_vertex_id &&
                             LocatorEqual(a.edge_locator, b.edge_locator);
                    });
}

void SetProviderEvidence(GraphAdjacencyPhysicalProvider* provider) {
  provider->evidence = {
      kGraphAdjacencyPhysicalProviderSearchKey,
      "graph_adjacency.vertex_id_index=physical",
      "graph_adjacency.edge_source_adjacency=physical",
      "graph_adjacency.edge_target_adjacency=physical",
      "graph_adjacency.label_index=physical",
      "graph_adjacency.property_index=physical",
      "graph_adjacency.typed_edge_label_adjacency=physical",
      "graph_adjacency.frontier_batch_expansion=physical",
      "graph_adjacency.visited_set=compressed_bitmap_candidate_set",
      "candidate_evidence_only=true",
      "exact_source_recheck_required=true",
      "mga_recheck_required=true",
      "security_recheck_required=true",
      "visibility_authority=false",
      "authorization_authority=false",
      "transaction_finality_authority=false",
      "recovery_finality_authority=false"};
}

void RebuildIndexes(GraphAdjacencyPhysicalProvider* provider) {
  provider->vertex_id_index.clear();
  provider->edge_source_adjacency.clear();
  provider->edge_target_adjacency.clear();
  provider->label_index.clear();
  provider->property_index.clear();
  provider->typed_edge_label_adjacency.clear();

  for (const auto& vertex : provider->vertices) {
    if (vertex.tombstoned) continue;
    provider->vertex_id_index.push_back(
        {vertex.row.vertex_id, vertex.row.locator});
    for (const auto& label : vertex.row.labels) {
      provider->label_index.push_back(
          {label, GraphEntityKind::vertex, vertex.row.vertex_id,
           vertex.row.locator});
    }
    for (const auto& property : vertex.row.properties) {
      provider->property_index.push_back(
          {property.key, property.type_tag, property.encoded_value,
           GraphEntityKind::vertex, vertex.row.vertex_id, vertex.row.locator});
    }
  }

  for (const auto& edge : provider->edges) {
    if (edge.tombstoned ||
        !VertexActive(*provider, edge.row.source_vertex_id) ||
        !VertexActive(*provider, edge.row.target_vertex_id)) {
      continue;
    }
    GraphAdjacencyIndexEntry source;
    source.vertex_id = edge.row.source_vertex_id;
    source.edge_label = edge.row.label;
    source.edge_id = edge.row.edge_id;
    source.other_vertex_id = edge.row.target_vertex_id;
    source.edge_locator = edge.row.locator;
    provider->edge_source_adjacency.push_back(source);
    provider->typed_edge_label_adjacency.push_back(source);

    GraphAdjacencyIndexEntry target = source;
    target.vertex_id = edge.row.target_vertex_id;
    target.other_vertex_id = edge.row.source_vertex_id;
    provider->edge_target_adjacency.push_back(target);

    provider->label_index.push_back(
        {edge.row.label, GraphEntityKind::edge, edge.row.edge_id,
         edge.row.locator});
    for (const auto& property : edge.row.properties) {
      provider->property_index.push_back(
          {property.key, property.type_tag, property.encoded_value,
           GraphEntityKind::edge, edge.row.edge_id, edge.row.locator});
    }
  }

  std::sort(provider->vertex_id_index.begin(), provider->vertex_id_index.end(),
            VertexIdEntryLess);
  std::sort(provider->edge_source_adjacency.begin(),
            provider->edge_source_adjacency.end(), AdjacencyEntryLess);
  std::sort(provider->edge_target_adjacency.begin(),
            provider->edge_target_adjacency.end(), AdjacencyEntryLess);
  std::sort(provider->typed_edge_label_adjacency.begin(),
            provider->typed_edge_label_adjacency.end(), AdjacencyEntryLess);
  std::sort(provider->label_index.begin(), provider->label_index.end(),
            LabelEntryLess);
  std::sort(provider->property_index.begin(), provider->property_index.end(),
            PropertyEntryLess);
}

bool ProviderFlagsValid(const GraphAdjacencyPhysicalProvider& p) {
  return p.artifact_kind == kGraphAdjacencyPhysicalProviderArtifactKind &&
         SameFormatVersion(p.format_version,
                           {kGraphAdjacencyPhysicalProviderCurrentMajor,
                            kGraphAdjacencyPhysicalProviderCurrentMinor}) &&
         PageExtentSummaryUuidTextValid(p.relation_uuid) &&
         PageExtentSummaryUuidTextValid(p.index_uuid) &&
         PageExtentSummaryUuidTextValid(p.provider_uuid) &&
         p.base_generation > 0 &&
         p.provider_generation > 0 &&
         DescriptorSafe(p.descriptor) &&
         p.vertex_id_index_present &&
         p.edge_source_adjacency_present &&
         p.edge_target_adjacency_present &&
         p.label_index_present &&
         p.property_index_present &&
         p.typed_edge_label_adjacency_present &&
         p.frontier_batch_expansion_present &&
         p.visited_compressed_bitmap_present &&
         p.candidate_evidence_only &&
         p.exact_source_recheck_required &&
         p.mga_recheck_required &&
         p.security_recheck_required &&
         !p.descriptor_store_scan &&
         !p.behavior_store_scan &&
         !p.contract_only_fallback &&
         !p.provider_only_fallback &&
         ProviderAuthorityClean(p);
}

bool ProviderRowsValid(const GraphAdjacencyPhysicalProvider& p) {
  std::set<std::string> active_vertices;
  std::set<std::tuple<u64, std::string, std::string>> active_row_locators;
  std::set<std::string> edge_ids;
  for (const auto& vertex : p.vertices) {
    auto copy = vertex.row;
    if (!VertexInputValid(&copy) ||
        copy.labels != vertex.row.labels ||
        !PropertyVectorEqual(copy.properties, vertex.row.properties)) {
      return false;
    }
    if (!vertex.tombstoned) {
      if (!active_vertices.insert(vertex.row.vertex_id).second) return false;
      if (!active_row_locators
               .insert({vertex.row.locator.row_ordinal,
                        vertex.row.locator.row_uuid,
                        vertex.row.locator.version_uuid})
               .second) {
        return false;
      }
    }
  }
  for (const auto& edge : p.edges) {
    auto copy = edge.row;
    if (!EdgeInputValid(&copy) ||
        !PropertyVectorEqual(copy.properties, edge.row.properties) ||
        !edge_ids.insert(edge.row.edge_id).second) {
      return false;
    }
    if (!edge.tombstoned &&
        (active_vertices.count(edge.row.source_vertex_id) == 0 ||
         active_vertices.count(edge.row.target_vertex_id) == 0)) {
      return false;
    }
    if (!edge.tombstoned &&
        !active_row_locators
             .insert({edge.row.locator.row_ordinal,
                      edge.row.locator.row_uuid,
                      edge.row.locator.version_uuid})
             .second) {
      return false;
    }
  }
  return std::is_sorted(p.vertices.begin(), p.vertices.end(), VertexRecordLess) &&
         std::is_sorted(p.edges.begin(), p.edges.end(), EdgeRecordLess);
}

bool ProviderIndexesValid(const GraphAdjacencyPhysicalProvider& p) {
  GraphAdjacencyPhysicalProvider rebuilt = p;
  RebuildIndexes(&rebuilt);
  return IndexesEqual(p, rebuilt);
}

bool ProviderValid(const GraphAdjacencyPhysicalProvider& p) {
  return ProviderFlagsValid(p) && ProviderRowsValid(p) && ProviderIndexesValid(p);
}

bool RuntimeSafe(const GraphQueryRequest& request) {
  return ProviderValid(request.provider) &&
         RecheckProofValid(request.recheck_proof) &&
         request.descriptor_epoch_current &&
         !request.descriptor_store_scan &&
         !request.behavior_store_scan &&
         !request.contract_only_fallback &&
         !request.provider_only_fallback;
}

GraphCandidate VertexCandidate(const GraphAdjacencyPhysicalProvider& provider,
                               const GraphVertexIdIndexEntry& entry) {
  GraphCandidate candidate;
  candidate.entity_kind = GraphEntityKind::vertex;
  candidate.entity_id = entry.vertex_id;
  candidate.vertex_id = entry.vertex_id;
  candidate.locator = entry.locator;
  const auto* vertex = FindVertex(provider, entry.vertex_id);
  if (vertex != nullptr) {
    candidate.source_recheck_evidence_ref =
        vertex->row.exact_source_recheck_evidence_ref;
  }
  return candidate;
}

GraphCandidate EdgeCandidate(const GraphAdjacencyPhysicalProvider& provider,
                             const GraphAdjacencyIndexEntry& entry,
                             bool outgoing) {
  GraphCandidate candidate;
  candidate.entity_kind = GraphEntityKind::edge;
  candidate.entity_id = entry.edge_id;
  candidate.edge_id = entry.edge_id;
  candidate.edge_label = entry.edge_label;
  candidate.locator = entry.edge_locator;
  if (outgoing) {
    candidate.source_vertex_id = entry.vertex_id;
    candidate.target_vertex_id = entry.other_vertex_id;
  } else {
    candidate.source_vertex_id = entry.other_vertex_id;
    candidate.target_vertex_id = entry.vertex_id;
  }
  const auto* edge = FindEdge(provider, entry.edge_id);
  if (edge != nullptr) {
    candidate.source_recheck_evidence_ref =
        edge->row.exact_source_recheck_evidence_ref;
  }
  return candidate;
}

GraphQueryResult QueryOkBase(const GraphAdjacencyPhysicalProvider& provider) {
  GraphQueryResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.evidence = provider.evidence;
  return result;
}

GraphFrontierExpandResult FrontierOkBase(
    const GraphAdjacencyPhysicalProvider& provider) {
  GraphFrontierExpandResult result;
  result.status = OkStatus();
  result.fail_closed = false;
  result.evidence = provider.evidence;
  return result;
}

CandidateSetAuthorityContext GraphCandidateSetAuthority() {
  CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

std::vector<u64> VertexOrdinals(const GraphAdjacencyPhysicalProvider& provider,
                                const std::set<std::string>& vertex_ids) {
  std::vector<u64> ordinals;
  for (const auto& id : vertex_ids) {
    const auto* vertex = FindVertex(provider, id);
    if (vertex != nullptr && !vertex->tombstoned) {
      ordinals.push_back(vertex->row.locator.row_ordinal);
    }
  }
  std::sort(ordinals.begin(), ordinals.end());
  ordinals.erase(std::unique(ordinals.begin(), ordinals.end()), ordinals.end());
  return ordinals;
}

void SortAndDeduplicateCandidates(std::vector<GraphCandidate>* candidates) {
  std::sort(candidates->begin(), candidates->end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.entity_kind, left.entity_id,
                              left.locator.row_ordinal, left.locator.row_uuid,
                              left.locator.version_uuid) <
                     std::tie(right.entity_kind, right.entity_id,
                              right.locator.row_ordinal, right.locator.row_uuid,
                              right.locator.version_uuid);
            });
  candidates->erase(
      std::unique(candidates->begin(), candidates->end(),
                  [](const auto& left, const auto& right) {
                    return left.entity_kind == right.entity_kind &&
                           left.entity_id == right.entity_id &&
                           LocatorEqual(left.locator, right.locator);
                  }),
      candidates->end());
}

void AppendDescriptor(std::vector<byte>* out, const GraphDescriptor& d) {
  AppendU64(out, d.descriptor_epoch);
  AppendBool(out, d.deterministic);
  AppendBool(out, d.descriptor_safe);
  AppendBool(out, d.vertex_id_index);
  AppendBool(out, d.edge_source_adjacency);
  AppendBool(out, d.edge_target_adjacency);
  AppendBool(out, d.label_index);
  AppendBool(out, d.property_index);
  AppendBool(out, d.typed_edge_label_adjacency);
  AppendBool(out, d.frontier_batch_expansion);
  AppendBool(out, d.visited_compressed_bitmap);
}

bool ReadDescriptor(Reader* reader, GraphDescriptor* d) {
  return reader->ReadU64(&d->descriptor_epoch) &&
         reader->ReadBool(&d->deterministic) &&
         reader->ReadBool(&d->descriptor_safe) &&
         reader->ReadBool(&d->vertex_id_index) &&
         reader->ReadBool(&d->edge_source_adjacency) &&
         reader->ReadBool(&d->edge_target_adjacency) &&
         reader->ReadBool(&d->label_index) &&
         reader->ReadBool(&d->property_index) &&
         reader->ReadBool(&d->typed_edge_label_adjacency) &&
         reader->ReadBool(&d->frontier_batch_expansion) &&
         reader->ReadBool(&d->visited_compressed_bitmap);
}

void AppendVertexInput(std::vector<byte>* out, const GraphVertexInput& row) {
  AppendString(out, row.vertex_id);
  AppendLocator(out, row.locator);
  AppendU64(out, static_cast<u64>(row.labels.size()));
  for (const auto& label : row.labels) AppendString(out, label);
  AppendU64(out, static_cast<u64>(row.properties.size()));
  for (const auto& property : row.properties) AppendProperty(out, property);
  AppendString(out, row.exact_source_recheck_evidence_ref);
}

bool ReadVertexInput(Reader* reader, GraphVertexInput* row) {
  u64 label_count = 0;
  u64 property_count = 0;
  if (!reader->ReadString(&row->vertex_id) ||
      !reader->ReadLocator(&row->locator) ||
      !reader->ReadU64(&label_count) ||
      label_count > kMaxIndexEntries) {
    return false;
  }
  for (u64 i = 0; i < label_count; ++i) {
    std::string label;
    if (!reader->ReadString(&label)) return false;
    row->labels.push_back(std::move(label));
  }
  if (!reader->ReadU64(&property_count) ||
      property_count > kMaxIndexEntries) {
    return false;
  }
  for (u64 i = 0; i < property_count; ++i) {
    GraphPropertyValue property;
    if (!reader->ReadProperty(&property)) return false;
    row->properties.push_back(std::move(property));
  }
  return reader->ReadString(&row->exact_source_recheck_evidence_ref);
}

void AppendEdgeInput(std::vector<byte>* out, const GraphEdgeInput& row) {
  AppendString(out, row.edge_id);
  AppendString(out, row.source_vertex_id);
  AppendString(out, row.target_vertex_id);
  AppendString(out, row.label);
  AppendLocator(out, row.locator);
  AppendU64(out, static_cast<u64>(row.properties.size()));
  for (const auto& property : row.properties) AppendProperty(out, property);
  AppendString(out, row.exact_source_recheck_evidence_ref);
}

bool ReadEdgeInput(Reader* reader, GraphEdgeInput* row) {
  u64 property_count = 0;
  if (!reader->ReadString(&row->edge_id) ||
      !reader->ReadString(&row->source_vertex_id) ||
      !reader->ReadString(&row->target_vertex_id) ||
      !reader->ReadString(&row->label) ||
      !reader->ReadLocator(&row->locator) ||
      !reader->ReadU64(&property_count) ||
      property_count > kMaxIndexEntries) {
    return false;
  }
  for (u64 i = 0; i < property_count; ++i) {
    GraphPropertyValue property;
    if (!reader->ReadProperty(&property)) return false;
    row->properties.push_back(std::move(property));
  }
  return reader->ReadString(&row->exact_source_recheck_evidence_ref);
}

void AppendVertexIdIndexEntry(std::vector<byte>* out,
                              const GraphVertexIdIndexEntry& entry) {
  AppendString(out, entry.vertex_id);
  AppendLocator(out, entry.locator);
}

bool ReadVertexIdIndexEntry(Reader* reader, GraphVertexIdIndexEntry* entry) {
  return reader->ReadString(&entry->vertex_id) &&
         reader->ReadLocator(&entry->locator);
}

void AppendAdjacencyIndexEntry(std::vector<byte>* out,
                               const GraphAdjacencyIndexEntry& entry) {
  AppendString(out, entry.vertex_id);
  AppendString(out, entry.edge_label);
  AppendString(out, entry.edge_id);
  AppendString(out, entry.other_vertex_id);
  AppendLocator(out, entry.edge_locator);
}

bool ReadAdjacencyIndexEntry(Reader* reader, GraphAdjacencyIndexEntry* entry) {
  return reader->ReadString(&entry->vertex_id) &&
         reader->ReadString(&entry->edge_label) &&
         reader->ReadString(&entry->edge_id) &&
         reader->ReadString(&entry->other_vertex_id) &&
         reader->ReadLocator(&entry->edge_locator);
}

void AppendLabelIndexEntry(std::vector<byte>* out,
                           const GraphLabelIndexEntry& entry) {
  AppendString(out, entry.label);
  AppendEntityKind(out, entry.entity_kind);
  AppendString(out, entry.entity_id);
  AppendLocator(out, entry.locator);
}

bool ReadLabelIndexEntry(Reader* reader, GraphLabelIndexEntry* entry) {
  return reader->ReadString(&entry->label) &&
         reader->ReadEntityKind(&entry->entity_kind) &&
         reader->ReadString(&entry->entity_id) &&
         reader->ReadLocator(&entry->locator);
}

void AppendPropertyIndexEntry(std::vector<byte>* out,
                              const GraphPropertyIndexEntry& entry) {
  AppendString(out, entry.key);
  AppendString(out, entry.type_tag);
  AppendString(out, entry.encoded_value);
  AppendEntityKind(out, entry.entity_kind);
  AppendString(out, entry.entity_id);
  AppendLocator(out, entry.locator);
}

bool ReadPropertyIndexEntry(Reader* reader, GraphPropertyIndexEntry* entry) {
  return reader->ReadString(&entry->key) &&
         reader->ReadString(&entry->type_tag) &&
         reader->ReadString(&entry->encoded_value) &&
         reader->ReadEntityKind(&entry->entity_kind) &&
         reader->ReadString(&entry->entity_id) &&
         reader->ReadLocator(&entry->locator);
}

}  // namespace

GraphBuildResult BuildGraphAdjacencyPhysicalProvider(
    const GraphBuildRequest& request) {
  if (!RecheckAuthorityClean(request.recheck_proof) ||
      !DescriptorAuthorityClean(request.descriptor)) {
    return BuildFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED",
        "index.graph_adjacency_physical_provider.authority_claim_refused");
  }
  if (!RecheckProofValid(request.recheck_proof)) {
    return BuildFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
        "index.graph_adjacency_physical_provider.missing_recheck_proof");
  }
  if (!DescriptorSafe(request.descriptor)) {
    return BuildFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.UNSAFE_DESCRIPTOR",
        "index.graph_adjacency_physical_provider.unsafe_descriptor");
  }
  if (request.base_generation == 0 ||
      request.provider_generation == 0 ||
      request.vertices.size() > kMaxVertices ||
      request.edges.size() > kMaxEdges ||
      !PageExtentSummaryUuidTextValid(request.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(request.index_uuid) ||
      !PageExtentSummaryUuidTextValid(request.provider_uuid)) {
    return BuildFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.BUILD_REFUSED",
        "index.graph_adjacency_physical_provider.build_refused");
  }

  GraphAdjacencyPhysicalProvider provider;
  provider.relation_uuid = request.relation_uuid;
  provider.index_uuid = request.index_uuid;
  provider.provider_uuid = request.provider_uuid;
  provider.base_generation = request.base_generation;
  provider.provider_generation = request.provider_generation;
  provider.descriptor = request.descriptor;
  for (auto vertex : request.vertices) {
    if (!VertexInputValid(&vertex)) {
      return BuildFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.INVALID_VERTEX",
          "index.graph_adjacency_physical_provider.invalid_vertex");
    }
    provider.vertices.push_back({std::move(vertex), false});
  }
  for (auto edge : request.edges) {
    if (!EdgeInputValid(&edge)) {
      return BuildFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.INVALID_EDGE",
          "index.graph_adjacency_physical_provider.invalid_edge");
    }
    provider.edges.push_back({std::move(edge), false});
  }
  std::sort(provider.vertices.begin(), provider.vertices.end(),
            VertexRecordLess);
  std::sort(provider.edges.begin(), provider.edges.end(), EdgeRecordLess);
  for (std::size_t i = 1; i < provider.vertices.size(); ++i) {
    if (provider.vertices[i - 1].row.vertex_id ==
        provider.vertices[i].row.vertex_id) {
      return BuildFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.DUPLICATE_VERTEX",
          "index.graph_adjacency_physical_provider.duplicate_vertex");
    }
  }
  for (std::size_t i = 1; i < provider.edges.size(); ++i) {
    if (provider.edges[i - 1].row.edge_id == provider.edges[i].row.edge_id) {
      return BuildFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.DUPLICATE_EDGE",
          "index.graph_adjacency_physical_provider.duplicate_edge");
    }
  }
  SetProviderEvidence(&provider);
  RebuildIndexes(&provider);
  if (!ProviderValid(provider)) {
    return BuildFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.BUILD_CORRUPT",
        "index.graph_adjacency_physical_provider.build_corrupt");
  }

  GraphBuildResult result;
  result.status = OkStatus();
  result.provider = std::move(provider);
  result.built = true;
  result.fail_closed = false;
  return result;
}

GraphSerializeResult SerializeGraphAdjacencyPhysicalProvider(
    const GraphAdjacencyPhysicalProvider& provider) {
  GraphSerializeResult result;
  if (!ProviderValid(provider)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeGraphAdjacencyPhysicalProviderDiagnostic(
        result.status,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.SERIALIZE_REFUSED",
        "index.graph_adjacency_physical_provider.serialize_refused");
    return result;
  }
  std::vector<byte> bytes;
  bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
  AppendU32(&bytes, kGraphAdjacencyPhysicalProviderCurrentMajor);
  AppendU32(&bytes, kGraphAdjacencyPhysicalProviderCurrentMinor);
  AppendU64(&bytes, 0);
  AppendString(&bytes, provider.relation_uuid);
  AppendString(&bytes, provider.index_uuid);
  AppendString(&bytes, provider.provider_uuid);
  AppendU64(&bytes, provider.base_generation);
  AppendU64(&bytes, provider.provider_generation);
  AppendDescriptor(&bytes, provider.descriptor);
  AppendU64(&bytes, static_cast<u64>(provider.vertices.size()));
  for (const auto& vertex : provider.vertices) {
    AppendVertexInput(&bytes, vertex.row);
    AppendBool(&bytes, vertex.tombstoned);
  }
  AppendU64(&bytes, static_cast<u64>(provider.edges.size()));
  for (const auto& edge : provider.edges) {
    AppendEdgeInput(&bytes, edge.row);
    AppendBool(&bytes, edge.tombstoned);
  }
  AppendU64(&bytes, static_cast<u64>(provider.vertex_id_index.size()));
  for (const auto& entry : provider.vertex_id_index) {
    AppendVertexIdIndexEntry(&bytes, entry);
  }
  AppendU64(&bytes, static_cast<u64>(provider.edge_source_adjacency.size()));
  for (const auto& entry : provider.edge_source_adjacency) {
    AppendAdjacencyIndexEntry(&bytes, entry);
  }
  AppendU64(&bytes, static_cast<u64>(provider.edge_target_adjacency.size()));
  for (const auto& entry : provider.edge_target_adjacency) {
    AppendAdjacencyIndexEntry(&bytes, entry);
  }
  AppendU64(&bytes, static_cast<u64>(provider.label_index.size()));
  for (const auto& entry : provider.label_index) {
    AppendLabelIndexEntry(&bytes, entry);
  }
  AppendU64(&bytes, static_cast<u64>(provider.property_index.size()));
  for (const auto& entry : provider.property_index) {
    AppendPropertyIndexEntry(&bytes, entry);
  }
  AppendU64(&bytes,
            static_cast<u64>(provider.typed_edge_label_adjacency.size()));
  for (const auto& entry : provider.typed_edge_label_adjacency) {
    AppendAdjacencyIndexEntry(&bytes, entry);
  }
  result.checksum = ComputeChecksum(bytes);
  StoreLittle64(bytes.data() + 16, result.checksum);
  result.status = OkStatus();
  result.bytes = std::move(bytes);
  return result;
}

GraphOpenResult OpenGraphAdjacencyPhysicalProvider(
    const GraphOpenRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return OpenFailure(
        GraphAdjacencyOpenClass::missing_recheck_proof,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
        "index.graph_adjacency_physical_provider.missing_recheck_proof");
  }
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(
        GraphAdjacencyOpenClass::corrupt_payload,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
        "index.graph_adjacency_physical_provider.corrupt_payload");
  }
  const u64 stored_checksum = LoadLittle64(request.bytes.data() + 16);
  if (stored_checksum == 0 ||
      stored_checksum != ComputeChecksum(request.bytes)) {
    return OpenFailure(
        GraphAdjacencyOpenClass::bad_checksum,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.BAD_CHECKSUM",
        "index.graph_adjacency_physical_provider.bad_checksum");
  }

  Reader reader(request.bytes);
  reader.SetOffset(8);
  GraphAdjacencyPhysicalProvider provider;
  u32 major = 0;
  u32 minor = 0;
  u64 checksum = 0;
  u64 count = 0;
  if (!reader.ReadU32(&major) ||
      !reader.ReadU32(&minor) ||
      !reader.ReadU64(&checksum)) {
    return OpenFailure(
        GraphAdjacencyOpenClass::corrupt_payload,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
        "index.graph_adjacency_physical_provider.corrupt_payload");
  }
  provider.format_version = {major, minor};
  if (!SameFormatVersion(provider.format_version,
                         {kGraphAdjacencyPhysicalProviderCurrentMajor,
                          kGraphAdjacencyPhysicalProviderCurrentMinor})) {
    return OpenFailure(
        GraphAdjacencyOpenClass::stale_format,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.STALE_FORMAT",
        "index.graph_adjacency_physical_provider.stale_format");
  }
  if (!reader.ReadString(&provider.relation_uuid) ||
      !reader.ReadString(&provider.index_uuid) ||
      !reader.ReadString(&provider.provider_uuid) ||
      !reader.ReadU64(&provider.base_generation) ||
      !reader.ReadU64(&provider.provider_generation) ||
      !ReadDescriptor(&reader, &provider.descriptor) ||
      !reader.ReadU64(&count) ||
      count > kMaxVertices) {
    return OpenFailure(
        GraphAdjacencyOpenClass::corrupt_payload,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
        "index.graph_adjacency_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < count; ++i) {
    GraphVertexRecord vertex;
    if (!ReadVertexInput(&reader, &vertex.row) ||
        !reader.ReadBool(&vertex.tombstoned)) {
      return OpenFailure(
          GraphAdjacencyOpenClass::corrupt_payload,
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
          "index.graph_adjacency_physical_provider.corrupt_payload");
    }
    provider.vertices.push_back(std::move(vertex));
  }
  if (!reader.ReadU64(&count) || count > kMaxEdges) {
    return OpenFailure(
        GraphAdjacencyOpenClass::corrupt_payload,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
        "index.graph_adjacency_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < count; ++i) {
    GraphEdgeRecord edge;
    if (!ReadEdgeInput(&reader, &edge.row) ||
        !reader.ReadBool(&edge.tombstoned)) {
      return OpenFailure(
          GraphAdjacencyOpenClass::corrupt_payload,
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
          "index.graph_adjacency_physical_provider.corrupt_payload");
    }
    provider.edges.push_back(std::move(edge));
  }
  if (!reader.ReadU64(&count) || count > kMaxIndexEntries) {
    return OpenFailure(GraphAdjacencyOpenClass::corrupt_payload,
                       "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.graph_adjacency_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < count; ++i) {
    GraphVertexIdIndexEntry entry;
    if (!ReadVertexIdIndexEntry(&reader, &entry)) {
      return OpenFailure(GraphAdjacencyOpenClass::corrupt_payload,
                         "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                         "index.graph_adjacency_physical_provider.corrupt_payload");
    }
    provider.vertex_id_index.push_back(std::move(entry));
  }
  auto read_adjacency = [&](std::vector<GraphAdjacencyIndexEntry>* entries) {
    u64 n = 0;
    if (!reader.ReadU64(&n) || n > kMaxIndexEntries) return false;
    for (u64 i = 0; i < n; ++i) {
      GraphAdjacencyIndexEntry entry;
      if (!ReadAdjacencyIndexEntry(&reader, &entry)) return false;
      entries->push_back(std::move(entry));
    }
    return true;
  };
  if (!read_adjacency(&provider.edge_source_adjacency) ||
      !read_adjacency(&provider.edge_target_adjacency)) {
    return OpenFailure(GraphAdjacencyOpenClass::corrupt_payload,
                       "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.graph_adjacency_physical_provider.corrupt_payload");
  }
  if (!reader.ReadU64(&count) || count > kMaxIndexEntries) {
    return OpenFailure(GraphAdjacencyOpenClass::corrupt_payload,
                       "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.graph_adjacency_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < count; ++i) {
    GraphLabelIndexEntry entry;
    if (!ReadLabelIndexEntry(&reader, &entry)) {
      return OpenFailure(GraphAdjacencyOpenClass::corrupt_payload,
                         "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                         "index.graph_adjacency_physical_provider.corrupt_payload");
    }
    provider.label_index.push_back(std::move(entry));
  }
  if (!reader.ReadU64(&count) || count > kMaxIndexEntries) {
    return OpenFailure(GraphAdjacencyOpenClass::corrupt_payload,
                       "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.graph_adjacency_physical_provider.corrupt_payload");
  }
  for (u64 i = 0; i < count; ++i) {
    GraphPropertyIndexEntry entry;
    if (!ReadPropertyIndexEntry(&reader, &entry)) {
      return OpenFailure(GraphAdjacencyOpenClass::corrupt_payload,
                         "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                         "index.graph_adjacency_physical_provider.corrupt_payload");
    }
    provider.property_index.push_back(std::move(entry));
  }
  if (!read_adjacency(&provider.typed_edge_label_adjacency) ||
      !reader.Done()) {
    return OpenFailure(GraphAdjacencyOpenClass::corrupt_payload,
                       "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
                       "index.graph_adjacency_physical_provider.corrupt_payload");
  }
  SetProviderEvidence(&provider);
  if (!DescriptorSafe(provider.descriptor)) {
    return OpenFailure(
        GraphAdjacencyOpenClass::stale_descriptor_epoch,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.UNSAFE_DESCRIPTOR",
        "index.graph_adjacency_physical_provider.unsafe_descriptor");
  }
  if ((request.expected_relation_uuid_present &&
       request.expected_relation_uuid != provider.relation_uuid) ||
      (request.expected_index_uuid_present &&
       request.expected_index_uuid != provider.index_uuid) ||
      (request.expected_provider_uuid_present &&
       request.expected_provider_uuid != provider.provider_uuid)) {
    return OpenFailure(
        GraphAdjacencyOpenClass::identity_mismatch,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.IDENTITY_MISMATCH",
        "index.graph_adjacency_physical_provider.identity_mismatch");
  }
  if ((request.expected_base_generation_present &&
       request.expected_base_generation != provider.base_generation) ||
      (request.expected_provider_generation_present &&
       request.expected_provider_generation != provider.provider_generation)) {
    return OpenFailure(
        GraphAdjacencyOpenClass::stale_generation,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.STALE_GENERATION",
        "index.graph_adjacency_physical_provider.stale_generation");
  }
  if (request.expected_descriptor_epoch_present &&
      request.expected_descriptor_epoch != provider.descriptor.descriptor_epoch) {
    return OpenFailure(
        GraphAdjacencyOpenClass::stale_descriptor_epoch,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.graph_adjacency_physical_provider.stale_descriptor_epoch");
  }
  if (!ProviderValid(provider)) {
    return OpenFailure(
        GraphAdjacencyOpenClass::corrupt_payload,
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.CORRUPT_PAYLOAD",
        "index.graph_adjacency_physical_provider.corrupt_payload");
  }

  GraphOpenResult result;
  result.status = OkStatus();
  result.open_class = GraphAdjacencyOpenClass::current;
  result.provider = std::move(provider);
  result.fail_closed = false;
  result.actions.push_back("open_graph_adjacency_physical_provider");
  return result;
}

GraphQueryResult QueryGraphVertexIdIndex(
    const GraphVertexLookupRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
        "index.graph_adjacency_physical_provider.missing_recheck_proof");
  }
  if (!request.descriptor_epoch_current) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.graph_adjacency_physical_provider.stale_descriptor_epoch");
  }
  if (!RuntimeSafe(request) || request.vertex_id.empty()) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
        "index.graph_adjacency_physical_provider.runtime_refused");
  }
  auto result = QueryOkBase(request.provider);
  result.vertex_id_index_used = true;
  auto iter = std::lower_bound(
      request.provider.vertex_id_index.begin(),
      request.provider.vertex_id_index.end(),
      request.vertex_id,
      [](const GraphVertexIdIndexEntry& entry, const std::string& value) {
        return entry.vertex_id < value;
      });
  for (; iter != request.provider.vertex_id_index.end() &&
         iter->vertex_id == request.vertex_id;
       ++iter) {
    ++result.index_entries_examined;
    result.candidates.push_back(VertexCandidate(request.provider, *iter));
  }
  return result;
}

GraphQueryResult QueryGraphAdjacencyIndex(
    const GraphAdjacencyLookupRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
        "index.graph_adjacency_physical_provider.missing_recheck_proof");
  }
  if (!request.descriptor_epoch_current) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.graph_adjacency_physical_provider.stale_descriptor_epoch");
  }
  if (!RuntimeSafe(request) || request.vertex_id.empty()) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
        "index.graph_adjacency_physical_provider.runtime_refused");
  }
  auto result = QueryOkBase(request.provider);
  auto scan = [&](const std::vector<GraphAdjacencyIndexEntry>& entries,
                  bool outgoing) {
    auto iter = request.label_filter_present
                    ? std::lower_bound(
                          entries.begin(),
                          entries.end(),
                          std::tie(request.vertex_id, request.edge_label),
                          [](const GraphAdjacencyIndexEntry& entry,
                             const auto& key) {
                            return std::tie(entry.vertex_id, entry.edge_label) <
                                   key;
                          })
                    : std::lower_bound(
                          entries.begin(),
                          entries.end(),
                          request.vertex_id,
                          [](const GraphAdjacencyIndexEntry& entry,
                             const std::string& value) {
                            return entry.vertex_id < value;
                          });
    for (; iter != entries.end() && iter->vertex_id == request.vertex_id;
         ++iter) {
      if (request.label_filter_present &&
          iter->edge_label != request.edge_label) {
        break;
      }
      ++result.index_entries_examined;
      result.candidates.push_back(
          EdgeCandidate(request.provider, *iter, outgoing));
    }
  };
  if (request.direction == GraphAdjacencyDirection::outgoing ||
      request.direction == GraphAdjacencyDirection::both) {
    result.edge_source_adjacency_used = true;
    result.typed_edge_label_adjacency_used = request.label_filter_present;
    const auto& entries = request.label_filter_present
                              ? request.provider.typed_edge_label_adjacency
                              : request.provider.edge_source_adjacency;
    scan(entries, true);
  }
  if (request.direction == GraphAdjacencyDirection::incoming ||
      request.direction == GraphAdjacencyDirection::both) {
    result.edge_target_adjacency_used = true;
    scan(request.provider.edge_target_adjacency, false);
  }
  SortAndDeduplicateCandidates(&result.candidates);
  return result;
}

GraphQueryResult QueryGraphLabelIndex(const GraphLabelLookupRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
        "index.graph_adjacency_physical_provider.missing_recheck_proof");
  }
  if (!request.descriptor_epoch_current) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.graph_adjacency_physical_provider.stale_descriptor_epoch");
  }
  if (!RuntimeSafe(request) || request.label.empty() ||
      (!request.include_vertices && !request.include_edges)) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
        "index.graph_adjacency_physical_provider.runtime_refused");
  }
  auto result = QueryOkBase(request.provider);
  result.label_index_used = true;
  auto iter = std::lower_bound(
      request.provider.label_index.begin(),
      request.provider.label_index.end(),
      request.label,
      [](const GraphLabelIndexEntry& entry, const std::string& value) {
        return entry.label < value;
      });
  for (; iter != request.provider.label_index.end() &&
         iter->label == request.label;
       ++iter) {
    ++result.index_entries_examined;
    if (iter->entity_kind == GraphEntityKind::vertex &&
        !request.include_vertices) {
      continue;
    }
    if (iter->entity_kind == GraphEntityKind::edge && !request.include_edges) {
      continue;
    }
    GraphCandidate candidate;
    candidate.entity_kind = iter->entity_kind;
    candidate.entity_id = iter->entity_id;
    candidate.locator = iter->locator;
    if (iter->entity_kind == GraphEntityKind::vertex) {
      candidate.vertex_id = iter->entity_id;
      const auto* vertex = FindVertex(request.provider, iter->entity_id);
      if (vertex != nullptr) {
        candidate.source_recheck_evidence_ref =
            vertex->row.exact_source_recheck_evidence_ref;
      }
    } else {
      candidate.edge_id = iter->entity_id;
      candidate.edge_label = iter->label;
      const auto* edge = FindEdge(request.provider, iter->entity_id);
      if (edge != nullptr) {
        candidate.source_vertex_id = edge->row.source_vertex_id;
        candidate.target_vertex_id = edge->row.target_vertex_id;
        candidate.source_recheck_evidence_ref =
            edge->row.exact_source_recheck_evidence_ref;
      }
    }
    result.candidates.push_back(std::move(candidate));
  }
  SortAndDeduplicateCandidates(&result.candidates);
  return result;
}

GraphQueryResult QueryGraphPropertyIndex(
    const GraphPropertyLookupRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
        "index.graph_adjacency_physical_provider.missing_recheck_proof");
  }
  if (!request.descriptor_epoch_current) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.graph_adjacency_physical_provider.stale_descriptor_epoch");
  }
  if (!RuntimeSafe(request) || request.key.empty() ||
      request.type_tag.empty() || request.encoded_value.empty() ||
      (!request.include_vertices && !request.include_edges)) {
    return QueryFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
        "index.graph_adjacency_physical_provider.runtime_refused");
  }
  auto result = QueryOkBase(request.provider);
  result.property_index_used = true;
  const auto key =
      std::tie(request.key, request.type_tag, request.encoded_value);
  auto iter = std::lower_bound(
      request.provider.property_index.begin(),
      request.provider.property_index.end(),
      key,
      [](const GraphPropertyIndexEntry& entry, const auto& value) {
        return std::tie(entry.key, entry.type_tag, entry.encoded_value) <
               value;
      });
  for (; iter != request.provider.property_index.end() &&
         iter->key == request.key &&
         iter->type_tag == request.type_tag &&
         iter->encoded_value == request.encoded_value;
       ++iter) {
    ++result.index_entries_examined;
    if (iter->entity_kind == GraphEntityKind::vertex &&
        !request.include_vertices) {
      continue;
    }
    if (iter->entity_kind == GraphEntityKind::edge && !request.include_edges) {
      continue;
    }
    GraphCandidate candidate;
    candidate.entity_kind = iter->entity_kind;
    candidate.entity_id = iter->entity_id;
    candidate.locator = iter->locator;
    if (iter->entity_kind == GraphEntityKind::vertex) {
      candidate.vertex_id = iter->entity_id;
      const auto* vertex = FindVertex(request.provider, iter->entity_id);
      if (vertex != nullptr) {
        candidate.source_recheck_evidence_ref =
            vertex->row.exact_source_recheck_evidence_ref;
      }
    } else {
      candidate.edge_id = iter->entity_id;
      const auto* edge = FindEdge(request.provider, iter->entity_id);
      if (edge != nullptr) {
        candidate.edge_label = edge->row.label;
        candidate.source_vertex_id = edge->row.source_vertex_id;
        candidate.target_vertex_id = edge->row.target_vertex_id;
        candidate.source_recheck_evidence_ref =
            edge->row.exact_source_recheck_evidence_ref;
      }
    }
    result.candidates.push_back(std::move(candidate));
  }
  SortAndDeduplicateCandidates(&result.candidates);
  return result;
}

GraphFrontierExpandResult ExpandGraphFrontierBatch(
    const GraphFrontierExpandRequest& request) {
  if (!RecheckProofValid(request.recheck_proof)) {
    return FrontierFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
        "index.graph_adjacency_physical_provider.missing_recheck_proof");
  }
  if (!request.descriptor_epoch_current) {
    return FrontierFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
        "index.graph_adjacency_physical_provider.stale_descriptor_epoch");
  }
  if (!RuntimeSafe(request) || request.frontier_vertex_ids.empty()) {
    return FrontierFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.RUNTIME_REFUSED",
        "index.graph_adjacency_physical_provider.runtime_refused");
  }
  auto result = FrontierOkBase(request.provider);
  std::set<std::string> visited(request.visited_vertex_ids.begin(),
                                request.visited_vertex_ids.end());
  std::set<std::string> next_vertices;
  for (const auto& vertex_id : request.frontier_vertex_ids) {
    GraphAdjacencyLookupRequest lookup;
    static_cast<GraphQueryRequest&>(lookup) = request;
    lookup.vertex_id = vertex_id;
    lookup.label_filter_present = request.label_filter_present;
    lookup.edge_label = request.edge_label;
    lookup.direction = request.direction;
    const auto expanded = QueryGraphAdjacencyIndex(lookup);
    if (!expanded.ok()) {
      return FrontierFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.FRONTIER_REFUSED",
          "index.graph_adjacency_physical_provider.frontier_refused");
    }
    result.edge_source_adjacency_used =
        result.edge_source_adjacency_used || expanded.edge_source_adjacency_used;
    result.edge_target_adjacency_used =
        result.edge_target_adjacency_used || expanded.edge_target_adjacency_used;
    result.typed_edge_label_adjacency_used =
        result.typed_edge_label_adjacency_used ||
        expanded.typed_edge_label_adjacency_used;
    result.index_entries_examined += expanded.index_entries_examined;
    for (const auto& candidate : expanded.candidates) {
      const std::string target =
          candidate.source_vertex_id == vertex_id
              ? candidate.target_vertex_id
              : candidate.source_vertex_id;
      if (target.empty() || visited.count(target) != 0 ||
          next_vertices.count(target) != 0) {
        continue;
      }
      result.candidates.push_back(candidate);
      next_vertices.insert(target);
      if (request.max_output_vertices > 0 &&
          next_vertices.size() >= request.max_output_vertices) {
        break;
      }
    }
    if (request.max_output_vertices > 0 &&
        next_vertices.size() >= request.max_output_vertices) {
      break;
    }
  }
  visited.insert(next_vertices.begin(), next_vertices.end());
  const auto visited_ordinals = VertexOrdinals(request.provider, visited);
  const auto compressed = MakeCompressedBitmapCandidateSetFromRowOrdinals(
      visited_ordinals, GraphCandidateSetAuthority());
  if (!compressed.ok()) {
    return FrontierFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.VISITED_SET_REFUSED",
        "index.graph_adjacency_physical_provider.visited_set_refused");
  }
  result.visited_candidate_set = compressed.output;
  result.compressed_bitmap_visited_set_used = true;
  result.visited_cardinality = result.visited_candidate_set
                                   .compressed_bitmap_cardinality;
  return result;
}

GraphMutationResult ApplyGraphAdjacencyPhysicalMutation(
    const GraphAdjacencyPhysicalProvider& provider,
    const GraphMutation& mutation) {
  if (!ProviderValid(provider)) {
    return MutationFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MUTATION_PROVIDER_INVALID",
        "index.graph_adjacency_physical_provider.mutation_provider_invalid");
  }
  if (!RecheckProofValid(mutation.recheck_proof)) {
    return MutationFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
        "index.graph_adjacency_physical_provider.missing_recheck_proof");
  }
  if ((mutation.expected_provider_generation_present &&
       mutation.expected_provider_generation != provider.provider_generation) ||
      (mutation.expected_descriptor_epoch_present &&
       mutation.expected_descriptor_epoch != provider.descriptor.descriptor_epoch)) {
    return MutationFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.STALE_EPOCH",
        "index.graph_adjacency_physical_provider.stale_epoch");
  }

  GraphAdjacencyPhysicalProvider next = provider;
  bool tombstone = false;
  if (mutation.kind == GraphMutationKind::insert_vertex) {
    auto after = mutation.after_vertex;
    if (!mutation.after_vertex_present ||
        !VertexInputValid(&after) ||
        FindVertex(next, after.vertex_id) != nullptr) {
      return MutationFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.INSERT_VERTEX_REFUSED",
          "index.graph_adjacency_physical_provider.insert_vertex_refused");
    }
    next.vertices.push_back({std::move(after), false});
  } else if (mutation.kind == GraphMutationKind::update_vertex) {
    auto before = mutation.before_vertex;
    auto after = mutation.after_vertex;
    if (!mutation.before_vertex_present ||
        !mutation.after_vertex_present ||
        !VertexInputValid(&before) ||
        !VertexInputValid(&after) ||
        before.vertex_id != after.vertex_id) {
      return MutationFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.UPDATE_VERTEX_REFUSED",
          "index.graph_adjacency_physical_provider.update_vertex_refused");
    }
    auto* vertex = const_cast<GraphVertexRecord*>(FindVertex(next, before.vertex_id));
    if (vertex == nullptr || vertex->tombstoned ||
        !VertexSame(vertex->row, before)) {
      return MutationFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.UPDATE_VERTEX_REFUSED",
          "index.graph_adjacency_physical_provider.update_vertex_refused");
    }
    vertex->row = std::move(after);
  } else if (mutation.kind == GraphMutationKind::delete_vertex) {
    auto before = mutation.before_vertex;
    if (!mutation.before_vertex_present ||
        !VertexInputValid(&before)) {
      return MutationFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.DELETE_VERTEX_REFUSED",
          "index.graph_adjacency_physical_provider.delete_vertex_refused");
    }
    auto* vertex = const_cast<GraphVertexRecord*>(FindVertex(next, before.vertex_id));
    if (vertex == nullptr || vertex->tombstoned ||
        !VertexSame(vertex->row, before)) {
      return MutationFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.DELETE_VERTEX_REFUSED",
          "index.graph_adjacency_physical_provider.delete_vertex_refused");
    }
    vertex->tombstoned = true;
    for (auto& edge : next.edges) {
      if (!edge.tombstoned &&
          (edge.row.source_vertex_id == before.vertex_id ||
           edge.row.target_vertex_id == before.vertex_id)) {
        edge.tombstoned = true;
      }
    }
    tombstone = true;
  } else if (mutation.kind == GraphMutationKind::insert_edge) {
    auto after = mutation.after_edge;
    if (!mutation.after_edge_present ||
        !EdgeInputValid(&after) ||
        FindEdge(next, after.edge_id) != nullptr ||
        !VertexActive(next, after.source_vertex_id) ||
        !VertexActive(next, after.target_vertex_id)) {
      return MutationFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.INSERT_EDGE_REFUSED",
          "index.graph_adjacency_physical_provider.insert_edge_refused");
    }
    next.edges.push_back({std::move(after), false});
  } else if (mutation.kind == GraphMutationKind::update_edge) {
    auto before = mutation.before_edge;
    auto after = mutation.after_edge;
    if (!mutation.before_edge_present ||
        !mutation.after_edge_present ||
        !EdgeInputValid(&before) ||
        !EdgeInputValid(&after) ||
        before.edge_id != after.edge_id ||
        !VertexActive(next, after.source_vertex_id) ||
        !VertexActive(next, after.target_vertex_id)) {
      return MutationFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.UPDATE_EDGE_REFUSED",
          "index.graph_adjacency_physical_provider.update_edge_refused");
    }
    auto* edge = const_cast<GraphEdgeRecord*>(FindEdge(next, before.edge_id));
    if (edge == nullptr || edge->tombstoned || !EdgeSame(edge->row, before)) {
      return MutationFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.UPDATE_EDGE_REFUSED",
          "index.graph_adjacency_physical_provider.update_edge_refused");
    }
    edge->row = std::move(after);
  } else {
    auto before = mutation.before_edge;
    if (!mutation.before_edge_present || !EdgeInputValid(&before)) {
      return MutationFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.DELETE_EDGE_REFUSED",
          "index.graph_adjacency_physical_provider.delete_edge_refused");
    }
    auto* edge = const_cast<GraphEdgeRecord*>(FindEdge(next, before.edge_id));
    if (edge == nullptr || edge->tombstoned || !EdgeSame(edge->row, before)) {
      return MutationFailure(
          "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.DELETE_EDGE_REFUSED",
          "index.graph_adjacency_physical_provider.delete_edge_refused");
    }
    edge->tombstoned = true;
    tombstone = true;
  }

  std::sort(next.vertices.begin(), next.vertices.end(), VertexRecordLess);
  std::sort(next.edges.begin(), next.edges.end(), EdgeRecordLess);
  ++next.provider_generation;
  RebuildIndexes(&next);
  SetProviderEvidence(&next);
  if (!ProviderValid(next)) {
    return MutationFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MUTATION_CORRUPT",
        "index.graph_adjacency_physical_provider.mutation_corrupt");
  }
  GraphMutationResult result;
  result.status = OkStatus();
  result.provider = std::move(next);
  result.applied = true;
  result.fail_closed = false;
  result.tombstone_written = tombstone;
  result.actions.push_back("apply_graph_adjacency_physical_mutation");
  return result;
}

GraphMutationResult CompactGraphAdjacencyPhysicalProvider(
    const GraphAdjacencyPhysicalProvider& provider,
    const GraphRecheckProof& recheck_proof) {
  if (!ProviderValid(provider)) {
    return MutationFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.COMPACTION_PROVIDER_INVALID",
        "index.graph_adjacency_physical_provider.compaction_provider_invalid");
  }
  if (!RecheckProofValid(recheck_proof)) {
    return MutationFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
        "index.graph_adjacency_physical_provider.missing_recheck_proof");
  }
  GraphAdjacencyPhysicalProvider next = provider;
  next.vertices.erase(
      std::remove_if(next.vertices.begin(), next.vertices.end(),
                     [](const auto& row) { return row.tombstoned; }),
      next.vertices.end());
  next.edges.erase(std::remove_if(next.edges.begin(), next.edges.end(),
                                  [](const auto& row) {
                                    return row.tombstoned;
                                  }),
                   next.edges.end());
  ++next.provider_generation;
  RebuildIndexes(&next);
  SetProviderEvidence(&next);
  if (!ProviderValid(next)) {
    return MutationFailure(
        "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.COMPACTION_CORRUPT",
        "index.graph_adjacency_physical_provider.compaction_corrupt");
  }
  GraphMutationResult result;
  result.status = OkStatus();
  result.provider = std::move(next);
  result.applied = true;
  result.fail_closed = false;
  result.compaction_performed = true;
  result.actions.push_back("compact_graph_adjacency_physical_provider");
  return result;
}

const char* GraphAdjacencyOpenClassName(GraphAdjacencyOpenClass open_class) {
  switch (open_class) {
    case GraphAdjacencyOpenClass::current: return "current";
    case GraphAdjacencyOpenClass::stale_format: return "stale_format";
    case GraphAdjacencyOpenClass::stale_generation: return "stale_generation";
    case GraphAdjacencyOpenClass::bad_checksum: return "bad_checksum";
    case GraphAdjacencyOpenClass::corrupt_payload: return "corrupt_payload";
    case GraphAdjacencyOpenClass::identity_mismatch: return "identity_mismatch";
    case GraphAdjacencyOpenClass::stale_descriptor_epoch:
      return "stale_descriptor_epoch";
    case GraphAdjacencyOpenClass::missing_recheck_proof:
      return "missing_recheck_proof";
    case GraphAdjacencyOpenClass::authority_claim_refused:
      return "authority_claim_refused";
    case GraphAdjacencyOpenClass::stale_runtime_epoch:
      return "stale_runtime_epoch";
    case GraphAdjacencyOpenClass::refused: return "refused";
  }
  return "unknown";
}

DiagnosticRecord MakeGraphAdjacencyPhysicalProviderDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.graph_adjacency_physical_provider");
}

}  // namespace scratchbird::core::index
