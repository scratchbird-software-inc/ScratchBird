// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "graph_adjacency_physical_provider.hpp"
#include "runtime_platform.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "graph_adjacency_physical_provider_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

void RequireNoRuntimeLeak(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    Require(item.find("docs" "/execution-plans") == std::string::npos &&
                item.find("public_release_evidence") == std::string::npos &&
                item.find("docs/reference") == std::string::npos &&
                item.find("IRC-") == std::string::npos &&
                item.find("IRC_") == std::string::npos &&
                item.find("execution_plan") == std::string::npos,
            "runtime evidence leaked planning/spec/reference artifact");
  }
}

std::string UuidWithSuffix(std::string prefix, std::uint64_t suffix) {
  std::ostringstream out;
  out << prefix << std::setw(12) << std::setfill('0') << suffix;
  return out.str();
}

idx::TextInvertedRowLocator Locator(std::uint64_t row) {
  idx::TextInvertedRowLocator locator;
  locator.row_ordinal = row;
  locator.row_uuid = UuidWithSuffix("14141414-1414-7414-8414-", row);
  locator.version_uuid = UuidWithSuffix("15151515-1515-7515-8515-", row);
  return locator;
}

idx::GraphPropertyValue Prop(std::string key,
                             std::string type,
                             std::string value) {
  return {std::move(key), std::move(type), std::move(value)};
}

idx::GraphRecheckProof Proof() {
  idx::GraphRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_recheck_required = true;
  proof.exact_source_available = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "graph_exact_source_mga_security_recheck_contract";
  return proof;
}

idx::GraphDescriptor Descriptor() {
  idx::GraphDescriptor descriptor;
  descriptor.descriptor_epoch = 41;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  return descriptor;
}

idx::GraphVertexInput Vertex(std::string id,
                             std::uint64_t row,
                             std::vector<std::string> labels,
                             std::vector<idx::GraphPropertyValue> props) {
  return {std::move(id),
          Locator(row),
          std::move(labels),
          std::move(props),
          "vertex_src_" + std::to_string(row)};
}

idx::GraphEdgeInput Edge(std::string id,
                         std::string source,
                         std::string target,
                         std::string label,
                         std::uint64_t row,
                         std::vector<idx::GraphPropertyValue> props) {
  return {std::move(id),
          std::move(source),
          std::move(target),
          std::move(label),
          Locator(row),
          std::move(props),
          "edge_src_" + std::to_string(row)};
}

std::vector<idx::GraphVertexInput> BaseVertices() {
  return {Vertex("A", 10, {"Person"}, {Prop("color", "string", "blue")}),
          Vertex("B", 20, {"Person"}, {Prop("color", "string", "red")}),
          Vertex("C", 30, {"Company"}, {Prop("tier", "int64", "2")})};
}

std::vector<idx::GraphEdgeInput> BaseEdges() {
  return {Edge("e1", "A", "B", "KNOWS", 100,
               {Prop("since", "int64", "2020")}),
          Edge("e2", "B", "C", "LIKES", 110,
               {Prop("weight", "int64", "7")}),
          Edge("e3", "A", "C", "KNOWS", 120,
               {Prop("since", "int64", "2022")}),
          Edge("e4", "C", "A", "BLOCKS", 130,
               {Prop("reason", "string", "policy")})};
}

idx::GraphBuildRequest BuildRequest() {
  idx::GraphBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.descriptor = Descriptor();
  request.recheck_proof = Proof();
  request.vertices = BaseVertices();
  request.edges = BaseEdges();
  return request;
}

std::vector<std::string> EntityIds(
    const std::vector<idx::GraphCandidate>& candidates) {
  std::vector<std::string> ids;
  for (const auto& candidate : candidates) {
    ids.push_back(candidate.entity_id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<std::string> EdgeIds(
    const std::vector<idx::GraphCandidate>& candidates) {
  std::vector<std::string> ids;
  for (const auto& candidate : candidates) {
    ids.push_back(candidate.edge_id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

void RequireCandidateOnly(const idx::GraphQueryResult& result) {
  Require(result.ok() &&
              result.candidate_rows_only &&
              !result.final_rows_authorized &&
              result.exact_source_recheck_required &&
              result.mga_recheck_required &&
              result.security_recheck_required,
          "query omitted candidate-only recheck evidence");
  RequireNoRuntimeLeak(result.evidence);
  for (const auto& candidate : result.candidates) {
    Require(candidate.from_physical_index &&
                candidate.exact_source_recheck_required &&
                candidate.mga_recheck_required &&
                candidate.security_recheck_required &&
                !candidate.final_row_admitted &&
                !candidate.source_recheck_evidence_ref.empty(),
            "candidate leaked finality or omitted recheck proof");
  }
}

void WriteFile(const std::filesystem::path& path,
               const std::vector<platform::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.flush();
  Require(static_cast<bool>(out), "could not write graph provider artifact");
}

std::vector<platform::byte> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "could not read graph provider artifact");
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>()};
}

idx::GraphVertexLookupRequest VertexLookup(
    const idx::GraphAdjacencyPhysicalProvider& provider,
    std::string id) {
  idx::GraphVertexLookupRequest request;
  request.provider = provider;
  request.recheck_proof = Proof();
  request.vertex_id = std::move(id);
  return request;
}

idx::GraphAdjacencyLookupRequest Adjacent(
    const idx::GraphAdjacencyPhysicalProvider& provider,
    std::string vertex,
    idx::GraphAdjacencyDirection direction =
        idx::GraphAdjacencyDirection::outgoing) {
  idx::GraphAdjacencyLookupRequest request;
  request.provider = provider;
  request.recheck_proof = Proof();
  request.vertex_id = std::move(vertex);
  request.direction = direction;
  return request;
}

void VerifyIndexesPersistenceAndCandidateEvidence() {
  const auto built = idx::BuildGraphAdjacencyPhysicalProvider(BuildRequest());
  Require(built.ok(), "graph provider build failed");
  Require(built.provider.vertex_id_index_present &&
              built.provider.edge_source_adjacency_present &&
              built.provider.edge_target_adjacency_present &&
              built.provider.label_index_present &&
              built.provider.property_index_present &&
              built.provider.typed_edge_label_adjacency_present &&
              built.provider.frontier_batch_expansion_present &&
              built.provider.visited_compressed_bitmap_present,
          "graph physical index surfaces missing");
  Require(HasEvidence(built.provider.evidence,
                      idx::kGraphAdjacencyPhysicalProviderSearchKey),
          "provider evidence missing graph search key");
  RequireNoRuntimeLeak(built.provider.evidence);

  const auto vertex = idx::QueryGraphVertexIdIndex(
      VertexLookup(built.provider, "A"));
  RequireCandidateOnly(vertex);
  Require(EntityIds(vertex.candidates) == std::vector<std::string>({"A"}) &&
              vertex.vertex_id_index_used &&
              vertex.index_entries_examined == 1,
          "vertex id index lookup changed");

  auto outgoing = idx::QueryGraphAdjacencyIndex(Adjacent(built.provider, "A"));
  RequireCandidateOnly(outgoing);
  Require(outgoing.edge_source_adjacency_used &&
              outgoing.index_entries_examined == 2 &&
              EdgeIds(outgoing.candidates) ==
                  std::vector<std::string>({"e1", "e3"}),
          "source adjacency lookup changed");

  auto incoming = idx::QueryGraphAdjacencyIndex(
      Adjacent(built.provider, "C", idx::GraphAdjacencyDirection::incoming));
  RequireCandidateOnly(incoming);
  Require(incoming.edge_target_adjacency_used &&
              incoming.index_entries_examined == 2 &&
              EdgeIds(incoming.candidates) ==
                  std::vector<std::string>({"e2", "e3"}),
          "target adjacency lookup changed");

  auto typed = Adjacent(built.provider, "A");
  typed.label_filter_present = true;
  typed.edge_label = "KNOWS";
  const auto typed_result = idx::QueryGraphAdjacencyIndex(typed);
  RequireCandidateOnly(typed_result);
  Require(typed_result.typed_edge_label_adjacency_used &&
              typed_result.index_entries_examined == 2 &&
              EdgeIds(typed_result.candidates) ==
                  std::vector<std::string>({"e1", "e3"}),
          "typed edge-label adjacency lookup changed");

  idx::GraphLabelLookupRequest label;
  label.provider = built.provider;
  label.recheck_proof = Proof();
  label.label = "Person";
  label.include_edges = false;
  const auto person = idx::QueryGraphLabelIndex(label);
  RequireCandidateOnly(person);
  Require(person.label_index_used &&
              person.index_entries_examined == 2 &&
              EntityIds(person.candidates) ==
                  std::vector<std::string>({"A", "B"}),
          "vertex label index lookup changed");

  label.label = "KNOWS";
  label.include_vertices = false;
  label.include_edges = true;
  const auto knows = idx::QueryGraphLabelIndex(label);
  RequireCandidateOnly(knows);
  Require(EdgeIds(knows.candidates) ==
              std::vector<std::string>({"e1", "e3"}) &&
              knows.index_entries_examined == 2,
          "edge label index lookup changed");

  idx::GraphPropertyLookupRequest prop;
  prop.provider = built.provider;
  prop.recheck_proof = Proof();
  prop.key = "color";
  prop.type_tag = "string";
  prop.encoded_value = "blue";
  prop.include_edges = false;
  const auto color = idx::QueryGraphPropertyIndex(prop);
  RequireCandidateOnly(color);
  Require(color.property_index_used &&
              color.index_entries_examined == 1 &&
              EntityIds(color.candidates) == std::vector<std::string>({"A"}),
          "property index vertex lookup changed");

  prop.key = "since";
  prop.type_tag = "int64";
  prop.encoded_value = "2022";
  prop.include_vertices = false;
  prop.include_edges = true;
  const auto since = idx::QueryGraphPropertyIndex(prop);
  RequireCandidateOnly(since);
  Require(EdgeIds(since.candidates) == std::vector<std::string>({"e3"}) &&
              since.index_entries_examined == 1,
          "property index edge lookup changed");

  idx::GraphFrontierExpandRequest frontier;
  frontier.provider = built.provider;
  frontier.recheck_proof = Proof();
  frontier.frontier_vertex_ids = {"A"};
  frontier.visited_vertex_ids = {"A"};
  frontier.direction = idx::GraphAdjacencyDirection::outgoing;
  const auto expanded = idx::ExpandGraphFrontierBatch(frontier);
  RequireCandidateOnly(expanded);
  Require(expanded.compressed_bitmap_visited_set_used &&
              expanded.visited_candidate_set.encoding ==
                  idx::CandidateSetEncoding::compressed_bitmap &&
              expanded.visited_cardinality == 3 &&
              EdgeIds(expanded.candidates) ==
                  std::vector<std::string>({"e1", "e3"}),
          "frontier expansion or visited compressed bitmap changed");

  const auto serialized =
      idx::SerializeGraphAdjacencyPhysicalProvider(built.provider);
  Require(serialized.ok(), "graph provider serialization failed");
  const auto serialized_again =
      idx::SerializeGraphAdjacencyPhysicalProvider(built.provider);
  Require(serialized_again.ok() && serialized_again.bytes == serialized.bytes,
          "graph provider serialization is not deterministic");
  const auto path =
      std::filesystem::temp_directory_path() /
      ("scratchbird_graph_adjacency_physical_provider_gate_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()) +
       ".sbgra");
  WriteFile(path, serialized.bytes);
  const auto persisted = ReadFile(path);
  std::filesystem::remove(path);
  Require(persisted == serialized.bytes, "persisted graph artifact changed");

  idx::GraphOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = built.provider.relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = built.provider.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = built.provider.provider_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = built.provider.base_generation;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = built.provider.provider_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
  open.recheck_proof = Proof();
  const auto opened = idx::OpenGraphAdjacencyPhysicalProvider(open);
  Require(opened.ok(), "clean graph reopen failed");
  Require(idx::SerializeGraphAdjacencyPhysicalProvider(opened.provider).bytes ==
              serialized.bytes,
          "graph reopen serialization equivalence failed");

  auto corrupt = serialized.bytes;
  corrupt.back() ^= 0x51;
  open.bytes = corrupt;
  const auto corrupt_result = idx::OpenGraphAdjacencyPhysicalProvider(open);
  Require(!corrupt_result.ok() &&
              corrupt_result.open_class ==
                  idx::GraphAdjacencyOpenClass::bad_checksum,
          "corrupt graph artifact did not fail closed");
}

void VerifyMutationMaintenanceAndCompaction() {
  auto built = idx::BuildGraphAdjacencyPhysicalProvider(BuildRequest());
  Require(built.ok(), "mutation fixture build failed");

  idx::GraphMutation insert_vertex;
  insert_vertex.kind = idx::GraphMutationKind::insert_vertex;
  insert_vertex.expected_provider_generation_present = true;
  insert_vertex.expected_provider_generation = built.provider.provider_generation;
  insert_vertex.expected_descriptor_epoch_present = true;
  insert_vertex.expected_descriptor_epoch =
      built.provider.descriptor.descriptor_epoch;
  insert_vertex.after_vertex_present = true;
  insert_vertex.after_vertex =
      Vertex("D", 40, {"Person"}, {Prop("color", "string", "green")});
  insert_vertex.recheck_proof = Proof();
  auto inserted =
      idx::ApplyGraphAdjacencyPhysicalMutation(built.provider, insert_vertex);
  Require(inserted.ok() &&
              inserted.provider.provider_generation ==
                  built.provider.provider_generation + 1,
          "vertex insert mutation failed");

  idx::GraphMutation insert_edge;
  insert_edge.kind = idx::GraphMutationKind::insert_edge;
  insert_edge.expected_provider_generation_present = true;
  insert_edge.expected_provider_generation =
      inserted.provider.provider_generation;
  insert_edge.expected_descriptor_epoch_present = true;
  insert_edge.expected_descriptor_epoch =
      inserted.provider.descriptor.descriptor_epoch;
  insert_edge.after_edge_present = true;
  insert_edge.after_edge =
      Edge("e5", "A", "D", "KNOWS", 140, {Prop("since", "int64", "2024")});
  insert_edge.recheck_proof = Proof();
  auto edge_inserted = idx::ApplyGraphAdjacencyPhysicalMutation(
      inserted.provider, insert_edge);
  Require(edge_inserted.ok(), "edge insert mutation failed");
  auto a_out = idx::QueryGraphAdjacencyIndex(Adjacent(edge_inserted.provider, "A"));
  RequireCandidateOnly(a_out);
  Require(EdgeIds(a_out.candidates) ==
              std::vector<std::string>({"e1", "e3", "e5"}),
          "inserted edge was not maintained in source adjacency");

  idx::GraphMutation stale = insert_edge;
  stale.expected_provider_generation = inserted.provider.provider_generation;
  stale.after_edge = Edge("e6", "D", "A", "KNOWS", 150, {});
  Require(!idx::ApplyGraphAdjacencyPhysicalMutation(edge_inserted.provider,
                                                    stale)
               .ok(),
          "stale generation mutation was accepted");

  idx::GraphMutation update_edge;
  update_edge.kind = idx::GraphMutationKind::update_edge;
  update_edge.expected_provider_generation_present = true;
  update_edge.expected_provider_generation =
      edge_inserted.provider.provider_generation;
  update_edge.expected_descriptor_epoch_present = true;
  update_edge.expected_descriptor_epoch =
      edge_inserted.provider.descriptor.descriptor_epoch;
  update_edge.before_edge_present = true;
  update_edge.before_edge = BaseEdges()[0];
  update_edge.after_edge_present = true;
  update_edge.after_edge =
      Edge("e1", "A", "B", "FRIEND", 100, {Prop("since", "int64", "2020")});
  update_edge.recheck_proof = Proof();
  auto edge_updated = idx::ApplyGraphAdjacencyPhysicalMutation(
      edge_inserted.provider, update_edge);
  Require(edge_updated.ok(), "edge update mutation failed");
  auto friend_lookup = Adjacent(edge_updated.provider, "A");
  friend_lookup.label_filter_present = true;
  friend_lookup.edge_label = "FRIEND";
  const auto friends = idx::QueryGraphAdjacencyIndex(friend_lookup);
  RequireCandidateOnly(friends);
  Require(EdgeIds(friends.candidates) == std::vector<std::string>({"e1"}),
          "typed label update did not maintain adjacency");
  auto knows_lookup = Adjacent(edge_updated.provider, "A");
  knows_lookup.label_filter_present = true;
  knows_lookup.edge_label = "KNOWS";
  const auto knows = idx::QueryGraphAdjacencyIndex(knows_lookup);
  RequireCandidateOnly(knows);
  Require(EdgeIds(knows.candidates) == std::vector<std::string>({"e3", "e5"}),
          "old typed edge-label candidate survived update");

  idx::GraphMutation delete_edge;
  delete_edge.kind = idx::GraphMutationKind::delete_edge;
  delete_edge.expected_provider_generation_present = true;
  delete_edge.expected_provider_generation =
      edge_updated.provider.provider_generation;
  delete_edge.expected_descriptor_epoch_present = true;
  delete_edge.expected_descriptor_epoch =
      edge_updated.provider.descriptor.descriptor_epoch;
  delete_edge.before_edge_present = true;
  delete_edge.before_edge = BaseEdges()[2];
  delete_edge.recheck_proof = Proof();
  auto edge_deleted = idx::ApplyGraphAdjacencyPhysicalMutation(
      edge_updated.provider, delete_edge);
  Require(edge_deleted.ok() && edge_deleted.tombstone_written,
          "edge delete mutation did not tombstone");
  a_out = idx::QueryGraphAdjacencyIndex(Adjacent(edge_deleted.provider, "A"));
  RequireCandidateOnly(a_out);
  Require(EdgeIds(a_out.candidates) == std::vector<std::string>({"e1", "e5"}),
          "deleted edge survived as a candidate");

  idx::GraphMutation delete_vertex;
  delete_vertex.kind = idx::GraphMutationKind::delete_vertex;
  delete_vertex.expected_provider_generation_present = true;
  delete_vertex.expected_provider_generation =
      edge_deleted.provider.provider_generation;
  delete_vertex.expected_descriptor_epoch_present = true;
  delete_vertex.expected_descriptor_epoch =
      edge_deleted.provider.descriptor.descriptor_epoch;
  delete_vertex.before_vertex_present = true;
  delete_vertex.before_vertex = BaseVertices()[1];
  delete_vertex.recheck_proof = Proof();
  auto vertex_deleted = idx::ApplyGraphAdjacencyPhysicalMutation(
      edge_deleted.provider, delete_vertex);
  Require(vertex_deleted.ok() && vertex_deleted.tombstone_written,
          "vertex delete did not tombstone dependent adjacency");
  const auto b_lookup = idx::QueryGraphVertexIdIndex(
      VertexLookup(vertex_deleted.provider, "B"));
  RequireCandidateOnly(b_lookup);
  Require(b_lookup.candidates.empty(),
          "deleted vertex survived as candidate evidence");
  a_out = idx::QueryGraphAdjacencyIndex(Adjacent(vertex_deleted.provider, "A"));
  RequireCandidateOnly(a_out);
  Require(EdgeIds(a_out.candidates) == std::vector<std::string>({"e5"}),
          "edge incident to deleted vertex survived as candidate");

  const auto compacted =
      idx::CompactGraphAdjacencyPhysicalProvider(vertex_deleted.provider,
                                                 Proof());
  Require(compacted.ok() &&
              compacted.compaction_performed &&
              compacted.provider.provider_generation ==
                  vertex_deleted.provider.provider_generation + 1,
          "graph tombstone compaction failed");
  const auto serialized =
      idx::SerializeGraphAdjacencyPhysicalProvider(compacted.provider);
  Require(serialized.ok(), "compacted graph provider did not serialize");
}

void VerifyFailClosedDiagnostics() {
  auto missing = BuildRequest();
  missing.recheck_proof = {};
  const auto missing_result =
      idx::BuildGraphAdjacencyPhysicalProvider(missing);
  Require(!missing_result.ok() &&
              missing_result.diagnostic.diagnostic_code ==
                  "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.MISSING_RECHECK_PROOF",
          "missing recheck proof did not fail closed");

  auto authority = BuildRequest();
  authority.recheck_proof.provider_finality_authority_claimed = true;
  const auto authority_result =
      idx::BuildGraphAdjacencyPhysicalProvider(authority);
  Require(!authority_result.ok() &&
              authority_result.diagnostic.diagnostic_code ==
                  "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED",
          "authority claim was accepted");

  auto unsafe = BuildRequest();
  unsafe.descriptor.vertex_id_index = false;
  const auto unsafe_result =
      idx::BuildGraphAdjacencyPhysicalProvider(unsafe);
  Require(!unsafe_result.ok() &&
              unsafe_result.diagnostic.diagnostic_code ==
                  "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.UNSAFE_DESCRIPTOR",
          "unsafe descriptor was accepted");

  auto invalid_edge = BuildRequest();
  invalid_edge.edges[0].target_vertex_id = "missing";
  const auto invalid_edge_result =
      idx::BuildGraphAdjacencyPhysicalProvider(invalid_edge);
  Require(!invalid_edge_result.ok() &&
              invalid_edge_result.diagnostic.diagnostic_code ==
              "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.BUILD_CORRUPT",
          "edge targeting missing vertex was accepted");

  auto duplicate_locator = BuildRequest();
  duplicate_locator.edges[0].locator = duplicate_locator.vertices[0].locator;
  const auto duplicate_locator_result =
      idx::BuildGraphAdjacencyPhysicalProvider(duplicate_locator);
  Require(!duplicate_locator_result.ok() &&
              duplicate_locator_result.diagnostic.diagnostic_code ==
                  "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.BUILD_CORRUPT",
          "duplicate active graph row locator was accepted");

  const auto built = idx::BuildGraphAdjacencyPhysicalProvider(BuildRequest());
  Require(built.ok(), "fail-closed fixture build failed");
  auto query = VertexLookup(built.provider, "A");
  query.descriptor_store_scan = true;
  Require(!idx::QueryGraphVertexIdIndex(query).ok(),
          "descriptor-store scan fallback was accepted");
  query.descriptor_store_scan = false;
  query.behavior_store_scan = true;
  Require(!idx::QueryGraphVertexIdIndex(query).ok(),
          "behavior-store scan fallback was accepted");
  query.behavior_store_scan = false;
  query.contract_only_fallback = true;
  Require(!idx::QueryGraphVertexIdIndex(query).ok(),
          "contract-only fallback was accepted");
  query.contract_only_fallback = false;
  query.provider_only_fallback = true;
  Require(!idx::QueryGraphVertexIdIndex(query).ok(),
          "provider-only fallback was accepted");
  query.provider_only_fallback = false;
  query.descriptor_epoch_current = false;
  const auto stale_runtime = idx::QueryGraphVertexIdIndex(query);
  Require(!stale_runtime.ok() &&
              stale_runtime.diagnostic.diagnostic_code ==
                  "INDEX.GRAPH_ADJACENCY_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
          "stale descriptor runtime did not fail closed");

  auto stale_artifact = built.provider;
  stale_artifact.vertex_id_index.pop_back();
  Require(!idx::SerializeGraphAdjacencyPhysicalProvider(stale_artifact).ok(),
          "stale/corrupt index projection serialized");

  const auto serialized =
      idx::SerializeGraphAdjacencyPhysicalProvider(built.provider);
  Require(serialized.ok(), "serialization failed for open refusals");
  idx::GraphOpenRequest open;
  open.bytes = serialized.bytes;
  open.recheck_proof = Proof();
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = 999;
  const auto stale_open = idx::OpenGraphAdjacencyPhysicalProvider(open);
  Require(!stale_open.ok() &&
              stale_open.open_class ==
                  idx::GraphAdjacencyOpenClass::stale_generation,
          "stale generation open was accepted");
  open.expected_provider_generation_present = false;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = 999;
  const auto stale_descriptor = idx::OpenGraphAdjacencyPhysicalProvider(open);
  Require(!stale_descriptor.ok() &&
              stale_descriptor.open_class ==
                  idx::GraphAdjacencyOpenClass::stale_descriptor_epoch,
          "stale descriptor open was accepted");
  idx::GraphOpenRequest corrupt;
  corrupt.bytes = {0x00, 0x01, 0x02};
  corrupt.recheck_proof = Proof();
  const auto corrupt_result = idx::OpenGraphAdjacencyPhysicalProvider(corrupt);
  Require(!corrupt_result.ok() &&
              corrupt_result.open_class ==
                  idx::GraphAdjacencyOpenClass::corrupt_payload,
          "short corrupt graph artifact was accepted");
}

}  // namespace

int main() {
  VerifyIndexesPersistenceAndCandidateEvidence();
  VerifyMutationMaintenanceAndCompaction();
  VerifyFailClosedDiagnostics();
  std::cout << "graph_adjacency_physical_provider_gate=passed\n";
  return EXIT_SUCCESS;
}
