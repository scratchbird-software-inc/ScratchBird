// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_platform.hpp"
#include "vector_hnsw_physical_provider.hpp"

#include <algorithm>
#include <cstdlib>
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
  std::cerr << "vector_hnsw_physical_provider_gate: " << message << '\n';
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
                item.find("execution_plan") == std::string::npos &&
                item.find("SB_ORH") == std::string::npos,
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
  locator.row_uuid = UuidWithSuffix("77777777-7777-7777-8777-", row);
  locator.version_uuid = UuidWithSuffix("88888888-8888-7888-8888-", row);
  return locator;
}

std::vector<std::uint64_t> Rows(
    const std::vector<idx::VectorHnswCandidate>& candidates) {
  std::vector<std::uint64_t> rows;
  for (const auto& candidate : candidates) {
    rows.push_back(candidate.locator.row_ordinal);
  }
  return rows;
}

idx::VectorHnswRecheckProof Proof() {
  idx::VectorHnswRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_vector_available = true;
  proof.exact_rerank_proof_supplied = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "vector_hnsw_source_mga_security_rerank_contract";
  return proof;
}

idx::VectorHnswDescriptor Descriptor() {
  idx::VectorHnswDescriptor descriptor;
  descriptor.dimensions = 4;
  descriptor.element_profile = idx::VectorExactElementProfile::fp32;
  descriptor.descriptor_epoch = 41;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  descriptor.int8_scale = 0.25;
  descriptor.int8_zero_point = 0;
  return descriptor;
}

idx::VectorHnswMetricResource Metric() {
  idx::VectorHnswMetricResource metric;
  metric.metric_resource_uuid = "99999999-9999-7999-8999-999999999999";
  metric.metric_resource_epoch = 43;
  metric.metric_kind = idx::VectorExactMetricKind::l2;
  metric.deterministic = true;
  metric.safe = true;
  return metric;
}

idx::VectorHnswBuildProfile Profile() {
  idx::VectorHnswBuildProfile profile;
  profile.m = 4;
  profile.ef_construction = 16;
  profile.ef_search = 16;
  profile.max_level = 5;
  profile.compaction_tombstone_ratio = 0.10;
  return profile;
}

std::vector<idx::VectorHnswSourceRow> RowsFixture() {
  return {{Locator(10), {0.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(20), {1.0F, 1.0F, 1.0F, 1.0F}},
          {Locator(30), {2.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(40), {1.0F, 1.0F, 1.0F, 1.0F}},
          {Locator(50), {-1.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(70), {3.0F, 3.0F, 3.0F, 3.0F}},
          {Locator(80), {0.0F, 2.0F, 0.0F, 0.0F}}};
}

idx::VectorHnswBuildRequest BuildRequest() {
  idx::VectorHnswBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.training_generation = 13;
  request.descriptor = Descriptor();
  request.metric = Metric();
  request.profile = Profile();
  request.recheck_proof = Proof();
  request.rows = RowsFixture();
  return request;
}

std::uint32_t ReadU32(const std::vector<platform::byte>& bytes,
                      std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t ReadU64(const std::vector<platform::byte>& bytes,
                      std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8);
  }
  return value;
}

void WriteU32(std::vector<platform::byte>* bytes,
              std::size_t offset,
              std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    (*bytes)[offset + i] =
        static_cast<platform::byte>((value >> (i * 8)) & 0xffu);
  }
}

void WriteU64(std::vector<platform::byte>* bytes,
              std::size_t offset,
              std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    (*bytes)[offset + i] =
        static_cast<platform::byte>((value >> (i * 8)) & 0xffu);
  }
}

void SkipString(const std::vector<platform::byte>& bytes,
                std::size_t* offset) {
  const std::uint32_t size = ReadU32(bytes, *offset);
  *offset += 4 + size;
}

std::uint64_t Checksum(std::vector<platform::byte> bytes) {
  constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
  constexpr std::uint64_t kFnvPrime = 1099511628211ull;
  WriteU64(&bytes, 16, 0);
  std::uint64_t hash = kFnvOffset;
  for (platform::byte value : bytes) {
    hash ^= static_cast<std::uint64_t>(value);
    hash *= kFnvPrime;
  }
  return hash == 0 ? 1 : hash;
}

std::size_t EntryPointOffset(const std::vector<platform::byte>& bytes) {
  std::size_t offset = 24;
  SkipString(bytes, &offset);
  SkipString(bytes, &offset);
  SkipString(bytes, &offset);
  offset += 8 + 8 + 8;
  offset += 4 + 4 + 8 + 8 + 4;
  SkipString(bytes, &offset);
  offset += 8 + 4;
  offset += 4 + 4 + 4 + 4 + 8 + 1 + 1 + 1;
  return offset;
}

void VerifyGraphInvariants(const idx::VectorHnswPhysicalProvider& provider) {
  Require(provider.graph_storage_present && provider.layers_present &&
              provider.entry_point_present && provider.neighbor_lists_present &&
              provider.row_locators_present && provider.encoded_payloads_present,
          "HNSW physical graph storage surfaces missing");
  Require(provider.live_node_count == RowsFixture().size(),
          "live node count changed");
  const auto* entry = &provider.nodes.front();
  for (const auto& node : provider.nodes) {
    if (node.node_id == provider.entry_point_node_id) {
      entry = &node;
    }
    Require(node.layer_neighbors.size() == node.level + 1,
            "node layer count does not match level");
    for (std::uint32_t layer = 0; layer < node.layer_neighbors.size(); ++layer) {
      Require(std::is_sorted(node.layer_neighbors[layer].begin(),
                             node.layer_neighbors[layer].end()),
              "neighbor list is not deterministic sorted");
      Require(node.layer_neighbors[layer].size() <= provider.profile.m,
              "neighbor list exceeds configured M");
      for (std::uint32_t neighbor : node.layer_neighbors[layer]) {
        Require(neighbor < provider.nodes.size(),
                "neighbor id outside graph");
        Require(neighbor != node.node_id,
                "node linked to itself");
      }
    }
  }
  Require(entry->level == provider.max_observed_level,
          "entry point does not sit on highest live level");
}

void VerifyBuildReopenDeterminismAndRefusals() {
  const auto built = idx::BuildVectorHnswPhysicalProvider(BuildRequest());
  Require(built.ok(), "HNSW provider build failed");
  Require(HasEvidence(built.provider.evidence,
                      idx::kVectorHnswPhysicalProviderSearchKey),
          "neutral runtime evidence missing");
  RequireNoRuntimeLeak(built.provider.evidence);
  VerifyGraphInvariants(built.provider);

  const auto serialized = idx::SerializeVectorHnswPhysicalProvider(built.provider);
  const auto serialized_again =
      idx::SerializeVectorHnswPhysicalProvider(built.provider);
  Require(serialized.ok() && serialized_again.ok(),
          "HNSW provider serialization failed");
  Require(serialized.bytes == serialized_again.bytes &&
              serialized.checksum == serialized_again.checksum,
          "HNSW serialization is not deterministic");

  idx::VectorHnswOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = BuildRequest().relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = BuildRequest().index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = BuildRequest().provider_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = 7;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = 11;
  open.expected_training_generation_present = true;
  open.expected_training_generation = 13;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = 41;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = 43;
  open.expected_dimensions_present = true;
  open.expected_dimensions = 4;
  open.recheck_proof = Proof();
  const auto reopened = idx::OpenVectorHnswPhysicalProvider(open);
  Require(reopened.ok(), "HNSW reopen failed");
  VerifyGraphInvariants(reopened.provider);
  RequireNoRuntimeLeak(reopened.provider.evidence);

  auto corrupt = serialized.bytes;
  corrupt.back() ^= static_cast<platform::byte>(0x40);
  open.bytes = corrupt;
  const auto bad_checksum = idx::OpenVectorHnswPhysicalProvider(open);
  Require(!bad_checksum.ok() &&
              bad_checksum.open_class == idx::VectorHnswOpenClass::bad_checksum,
          "corrupt HNSW bytes did not fail closed on checksum");

  auto invalid_entry = serialized.bytes;
  WriteU32(&invalid_entry, EntryPointOffset(invalid_entry), 999);
  WriteU64(&invalid_entry, 16, Checksum(invalid_entry));
  open.bytes = invalid_entry;
  const auto invalid_graph = idx::OpenVectorHnswPhysicalProvider(open);
  Require(!invalid_graph.ok() &&
              invalid_graph.open_class == idx::VectorHnswOpenClass::invalid_graph,
          "invalid HNSW entry point did not fail closed");

  open.bytes = serialized.bytes;
  open.expected_provider_generation = 12;
  const auto stale = idx::OpenVectorHnswPhysicalProvider(open);
  Require(!stale.ok() &&
              stale.open_class == idx::VectorHnswOpenClass::stale_generation,
          "stale generation did not fail closed");

  open = {};
  open.bytes = serialized.bytes;
  const auto missing_proof = idx::OpenVectorHnswPhysicalProvider(open);
  Require(!missing_proof.ok() &&
              missing_proof.open_class ==
                  idx::VectorHnswOpenClass::missing_exact_recheck_proof,
          "missing reopen proof did not fail closed");

  auto empty_request = BuildRequest();
  empty_request.rows.clear();
  const auto empty = idx::BuildVectorHnswPhysicalProvider(empty_request);
  Require(empty.ok() && empty.provider.empty_graph &&
              !empty.provider.entry_point_present &&
              empty.provider.live_node_count == 0,
          "empty HNSW provider build failed");
  idx::VectorHnswQueryRequest empty_query;
  empty_query.provider = empty.provider;
  empty_query.recheck_proof = Proof();
  idx::VectorHnswQuery empty_q;
  empty_q.vector = {0.0F, 0.0F, 0.0F, 0.0F};
  empty_q.top_k = 3;
  empty_q.ef_search = 8;
  empty_query.queries = {empty_q};
  const auto empty_result = idx::QueryVectorHnswPhysicalProvider(empty_query);
  Require(empty_result.ok() &&
              empty_result.batch_results.size() == 1 &&
              empty_result.batch_results[0].candidates.empty(),
          "empty HNSW provider did not query as empty candidate set");
  const auto empty_serialized =
      idx::SerializeVectorHnswPhysicalProvider(empty.provider);
  Require(empty_serialized.ok(), "empty HNSW serialization failed");
  idx::VectorHnswOpenRequest empty_open;
  empty_open.bytes = empty_serialized.bytes;
  empty_open.recheck_proof = Proof();
  const auto empty_reopened = idx::OpenVectorHnswPhysicalProvider(empty_open);
  Require(empty_reopened.ok() && empty_reopened.provider.empty_graph,
          "empty HNSW reopen failed");
}

void VerifyEfSearchExactRerankAndMutationLifecycle() {
  auto provider = idx::BuildVectorHnswPhysicalProvider(BuildRequest()).provider;
  idx::VectorHnswQueryRequest query;
  query.provider = provider;
  query.recheck_proof = Proof();
  idx::VectorHnswQuery first;
  first.vector = {1.0F, 1.0F, 1.0F, 1.0F};
  first.top_k = 3;
  first.ef_search = 32;
  idx::VectorHnswQuery second;
  second.vector = {0.0F, 0.0F, 0.0F, 0.0F};
  second.top_k = 2;
  second.ef_search = 32;
  second.candidate_set = {Locator(10), Locator(30), Locator(50)};
  second.metadata_prefilter = [](const idx::TextInvertedRowLocator& locator) {
    return locator.row_ordinal != 50;
  };
  query.queries = {first, second};
  const auto result = idx::QueryVectorHnswPhysicalProvider(query);
  Require(result.ok(), "HNSW query failed");
  Require(result.ef_search_traversal && result.exact_rerank_performed &&
              result.metadata_prefilter_consumed &&
              result.candidate_set_consumed &&
              result.scalar_kernel_consumed &&
              result.candidate_rows_only && !result.final_rows_authorized,
          "query did not prove efSearch exact rerank candidate-only path");
  Require(Rows(result.batch_results[0].candidates) ==
              std::vector<std::uint64_t>({20, 40, 10}),
          "HNSW exact rerank top-k changed");
  Require(Rows(result.batch_results[1].candidates) ==
              std::vector<std::uint64_t>({10, 30}),
          "HNSW candidate-set/prefilter top-k changed");
  for (const auto& batch : result.batch_results) {
    Require(batch.ef_search_used && batch.exact_rerank_performed &&
                batch.graph_nodes_visited > 0 &&
                batch.exact_rerank_count > 0,
            "single query omitted efSearch/rerank telemetry");
    for (const auto& candidate : batch.candidates) {
      Require(candidate.reached_by_ef_search &&
                  candidate.decoded_from_physical_payload &&
                  candidate.exact_rerank_proof_verified &&
                  candidate.exact_source_recheck_required &&
                  candidate.mga_recheck_required &&
                  candidate.security_recheck_required &&
                  !candidate.final_row_admitted &&
                  !candidate.source_recheck_evidence_ref.empty(),
              "candidate omitted candidate-only proof");
    }
  }
  RequireNoRuntimeLeak(result.evidence);

  idx::VectorHnswMutation insert;
  insert.kind = idx::VectorHnswMutationKind::insert_row;
  insert.expected_provider_generation_present = true;
  insert.expected_provider_generation = provider.provider_generation;
  insert.expected_descriptor_epoch_present = true;
  insert.expected_descriptor_epoch = 41;
  insert.expected_metric_resource_epoch_present = true;
  insert.expected_metric_resource_epoch = 43;
  insert.after_row_present = true;
  insert.after_row = {Locator(60), {0.8F, 0.8F, 0.8F, 0.8F}};
  insert.recheck_proof = Proof();
  auto inserted = idx::ApplyVectorHnswPhysicalMutation(provider, insert);
  Require(inserted.ok() && inserted.graph_repaired,
          "HNSW insert repair failed");

  idx::VectorHnswMutation update;
  update.kind = idx::VectorHnswMutationKind::update_row;
  update.expected_provider_generation_present = true;
  update.expected_provider_generation = inserted.provider.provider_generation;
  update.before_row_present = true;
  update.before_row = insert.after_row;
  update.after_row_present = true;
  update.after_row = {Locator(60), {4.0F, 4.0F, 4.0F, 4.0F}};
  update.recheck_proof = Proof();
  auto updated = idx::ApplyVectorHnswPhysicalMutation(inserted.provider, update);
  Require(updated.ok() && updated.graph_repaired,
          "HNSW update replacement rebuild failed");

  idx::VectorHnswMutation del;
  del.kind = idx::VectorHnswMutationKind::delete_row;
  del.expected_provider_generation_present = true;
  del.expected_provider_generation = updated.provider.provider_generation;
  del.before_row_present = true;
  del.before_row = {Locator(20), {1.0F, 1.0F, 1.0F, 1.0F}};
  del.recheck_proof = Proof();
  auto deleted = idx::ApplyVectorHnswPhysicalMutation(updated.provider, del);
  Require(deleted.ok() && deleted.tombstone_recorded &&
              deleted.provider.tombstone_count == 1 &&
              deleted.compaction_rebuild_required,
          "HNSW delete tombstone did not record compaction evidence");

  idx::VectorHnswQueryRequest deleted_query;
  deleted_query.provider = deleted.provider;
  deleted_query.recheck_proof = Proof();
  idx::VectorHnswQuery near_deleted;
  near_deleted.vector = {1.0F, 1.0F, 1.0F, 1.0F};
  near_deleted.top_k = 3;
  near_deleted.ef_search = 32;
  deleted_query.queries = {near_deleted};
  const auto after_delete = idx::QueryVectorHnswPhysicalProvider(deleted_query);
  Require(after_delete.ok(), "query after delete failed");
  const auto rows_after_delete = Rows(after_delete.batch_results[0].candidates);
  Require(std::find(rows_after_delete.begin(), rows_after_delete.end(), 20) ==
              rows_after_delete.end(),
          "tombstoned row appeared in HNSW candidates");

  const auto compacted =
      idx::CompactVectorHnswPhysicalProvider(deleted.provider, Proof());
  Require(compacted.ok() && compacted.removed_tombstones == 1 &&
              compacted.provider.tombstone_count == 0 &&
              !compacted.provider.compaction_rebuild_required,
          "HNSW compaction rebuild failed");
  VerifyGraphInvariants(compacted.provider);

  auto fp16_request = BuildRequest();
  fp16_request.descriptor.element_profile = idx::VectorExactElementProfile::fp16;
  fp16_request.rows = {{Locator(110), {0.3333F, 0.2F, 0.1F, 0.0F}},
                       {Locator(120), {1.0F, 0.0F, 0.0F, 0.0F}}};
  const auto fp16 = idx::BuildVectorHnswPhysicalProvider(fp16_request);
  Require(fp16.ok(), "fp16 HNSW provider build failed");
  idx::VectorHnswMutation fp16_update;
  fp16_update.kind = idx::VectorHnswMutationKind::update_row;
  fp16_update.expected_provider_generation_present = true;
  fp16_update.expected_provider_generation = fp16.provider.provider_generation;
  fp16_update.before_row_present = true;
  fp16_update.before_row = fp16_request.rows.front();
  fp16_update.after_row_present = true;
  fp16_update.after_row = {Locator(110), {0.75F, 0.25F, 0.0F, 0.0F}};
  fp16_update.recheck_proof = Proof();
  const auto fp16_updated =
      idx::ApplyVectorHnswPhysicalMutation(fp16.provider, fp16_update);
  Require(fp16_updated.ok(),
          "fp16 HNSW update did not match encoded physical payload");
}

void VerifyFailClosedSurfaces() {
  auto request = BuildRequest();
  request.rows.push_back(request.rows.front());
  Require(!idx::BuildVectorHnswPhysicalProvider(request).ok(),
          "duplicate row locator build did not fail closed");

  request = BuildRequest();
  request.rows.front().vector.push_back(5.0F);
  Require(!idx::BuildVectorHnswPhysicalProvider(request).ok(),
          "dimension mismatch did not fail closed");

  request = BuildRequest();
  request.descriptor.element_profile =
      static_cast<idx::VectorExactElementProfile>(999);
  Require(!idx::BuildVectorHnswPhysicalProvider(request).ok(),
          "unsupported profile did not fail closed");

  request = BuildRequest();
  request.metric.safe = false;
  Require(!idx::BuildVectorHnswPhysicalProvider(request).ok(),
          "unsafe metric did not fail closed");

  request = BuildRequest();
  request.profile.m = 0;
  Require(!idx::BuildVectorHnswPhysicalProvider(request).ok(),
          "unsupported HNSW profile did not fail closed");

  request = BuildRequest();
  request.recheck_proof = {};
  Require(!idx::BuildVectorHnswPhysicalProvider(request).ok(),
          "missing build proof did not fail closed");

  request = BuildRequest();
  request.recheck_proof.provider_finality_authority_claimed = true;
  Require(!idx::BuildVectorHnswPhysicalProvider(request).ok(),
          "unsafe authority proof did not fail closed");

  const auto provider = idx::BuildVectorHnswPhysicalProvider(BuildRequest()).provider;
  idx::VectorHnswQueryRequest query;
  query.provider = provider;
  query.recheck_proof = Proof();
  idx::VectorHnswQuery q;
  q.vector = {1.0F, 1.0F, 1.0F, 1.0F};
  q.top_k = 1;
  q.ef_search = 8;
  query.queries = {q};
  query.descriptor_store_scan = true;
  Require(!idx::QueryVectorHnswPhysicalProvider(query).ok(),
          "descriptor scan fallback did not fail closed");
  query.descriptor_store_scan = false;
  query.behavior_store_scan = true;
  Require(!idx::QueryVectorHnswPhysicalProvider(query).ok(),
          "behavior scan fallback did not fail closed");
  query.behavior_store_scan = false;
  query.contract_only_fallback = true;
  Require(!idx::QueryVectorHnswPhysicalProvider(query).ok(),
          "contract fallback did not fail closed");
  query.contract_only_fallback = false;
  query.provider_only_fallback = true;
  Require(!idx::QueryVectorHnswPhysicalProvider(query).ok(),
          "provider fallback did not fail closed");
  query = {};
  query.provider = provider;
  query.recheck_proof = Proof();
  q.vector = {1.0F, 1.0F};
  query.queries = {q};
  Require(!idx::QueryVectorHnswPhysicalProvider(query).ok(),
          "query dimension mismatch did not fail closed");

  idx::VectorHnswMutation mutation;
  mutation.kind = idx::VectorHnswMutationKind::insert_row;
  mutation.expected_provider_generation_present = true;
  mutation.expected_provider_generation = provider.provider_generation + 1;
  mutation.after_row_present = true;
  mutation.after_row = {Locator(90), {1.0F, 0.0F, 0.0F, 0.0F}};
  mutation.recheck_proof = Proof();
  Require(!idx::ApplyVectorHnswPhysicalMutation(provider, mutation).ok(),
          "unsafe generation mutation did not fail closed");

  auto invalid = provider;
  invalid.nodes.front().layer_neighbors.front().push_back(999);
  Require(!idx::QueryVectorHnswPhysicalProvider(
               idx::VectorHnswQueryRequest{invalid, {q}, Proof()})
               .ok(),
          "invalid graph runtime did not fail closed");

  invalid = provider;
  invalid.live_node_count = 0;
  invalid.empty_graph = true;
  Require(!idx::QueryVectorHnswPhysicalProvider(
               idx::VectorHnswQueryRequest{invalid, {q}, Proof()})
               .ok(),
          "stale HNSW provider counts did not fail closed");
}

}  // namespace

int main() {
  VerifyBuildReopenDeterminismAndRefusals();
  VerifyEfSearchExactRerankAndMutationLifecycle();
  VerifyFailClosedSurfaces();
  std::cout << "vector_hnsw_physical_provider_gate IRC-121 passed\n";
  return EXIT_SUCCESS;
}
