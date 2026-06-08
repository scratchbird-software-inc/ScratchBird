// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_platform.hpp"
#include "vector_ivf_pq_physical_provider.hpp"

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
  std::cerr << "vector_ivf_pq_physical_provider_gate: " << message << '\n';
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
    const std::vector<idx::VectorIvfPqCandidate>& candidates) {
  std::vector<std::uint64_t> rows;
  for (const auto& candidate : candidates) {
    rows.push_back(candidate.locator.row_ordinal);
  }
  return rows;
}

std::string RowsString(const std::vector<std::uint64_t>& rows) {
  std::ostringstream out;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i != 0) out << ',';
    out << rows[i];
  }
  return out.str();
}

idx::VectorIvfPqRecheckProof Proof() {
  idx::VectorIvfPqRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_vector_available = true;
  proof.exact_rerank_proof_supplied = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "vector_ivf_pq_source_mga_security_rerank_contract";
  return proof;
}

idx::VectorIvfPqDescriptor Descriptor() {
  idx::VectorIvfPqDescriptor descriptor;
  descriptor.dimensions = 4;
  descriptor.element_profile = idx::VectorExactElementProfile::fp32;
  descriptor.descriptor_epoch = 51;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  return descriptor;
}

idx::VectorIvfPqMetricResource Metric() {
  idx::VectorIvfPqMetricResource metric;
  metric.metric_resource_uuid = "99999999-9999-7999-8999-999999999999";
  metric.metric_resource_epoch = 53;
  metric.metric_kind = idx::VectorExactMetricKind::l2;
  metric.deterministic = true;
  metric.safe = true;
  return metric;
}

idx::VectorIvfPqBuildProfile Profile(idx::VectorIvfPqCompression compression) {
  idx::VectorIvfPqBuildProfile profile;
  profile.compression = compression;
  profile.centroid_count = 3;
  profile.nprobe = 3;
  profile.training_iterations = 5;
  profile.max_training_rows = 32;
  profile.pq_subspaces = 2;
  profile.pq_codewords = 4;
  profile.retrain_imbalance_ratio = 1.20;
  profile.rebuild_tombstone_ratio = 0.10;
  return profile;
}

std::vector<idx::VectorIvfPqSourceRow> RowsFixture() {
  return {{Locator(10), {0.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(20), {1.0F, 1.0F, 1.0F, 1.0F}},
          {Locator(30), {2.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(40), {1.0F, 1.0F, 1.0F, 1.0F}},
          {Locator(50), {-1.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(70), {3.0F, 3.0F, 3.0F, 3.0F}},
          {Locator(80), {0.0F, 2.0F, 0.0F, 0.0F}}};
}

idx::VectorIvfPqBuildRequest BuildRequest(
    idx::VectorIvfPqCompression compression) {
  idx::VectorIvfPqBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.training_generation = 13;
  request.descriptor = Descriptor();
  request.metric = Metric();
  request.profile = Profile(compression);
  request.recheck_proof = Proof();
  request.rows = RowsFixture();
  return request;
}

idx::VectorIvfPqQueryRequest QueryRequest(
    const idx::VectorIvfPqPhysicalProvider& provider) {
  idx::VectorIvfPqQueryRequest request;
  request.provider = provider;
  request.recheck_proof = Proof();
  idx::VectorIvfPqQuery first;
  first.vector = {1.0F, 1.0F, 1.0F, 1.0F};
  first.top_k = 3;
  first.nprobe = 3;
  idx::VectorIvfPqQuery second;
  second.vector = {0.0F, 0.0F, 0.0F, 0.0F};
  second.top_k = 2;
  second.nprobe = 3;
  second.candidate_set = {Locator(10), Locator(30), Locator(50)};
  second.metadata_prefilter = [](const idx::TextInvertedRowLocator& locator) {
    return locator.row_ordinal != 50;
  };
  request.queries = {first, second};
  return request;
}

idx::VectorIvfPqOpenRequest OpenRequest(
    const std::vector<platform::byte>& bytes) {
  idx::VectorIvfPqOpenRequest open;
  open.bytes = bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = BuildRequest(idx::VectorIvfPqCompression::ivf_flat).relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = BuildRequest(idx::VectorIvfPqCompression::ivf_flat).index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = BuildRequest(idx::VectorIvfPqCompression::ivf_flat).provider_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = 7;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = 11;
  open.expected_training_generation_present = true;
  open.expected_training_generation = 13;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = 51;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = 53;
  open.expected_dimensions_present = true;
  open.expected_dimensions = 4;
  open.recheck_proof = Proof();
  return open;
}

void VerifyProviderSurfaces(
    const idx::VectorIvfPqPhysicalProvider& provider,
    idx::VectorIvfPqCompression compression) {
  Require(provider.centroid_training_present &&
              provider.list_assignment_present &&
              provider.nprobe_planner_present &&
              provider.ivf_list_storage_present &&
              provider.vector_payload_storage_present &&
              provider.compressed_code_storage_present &&
              provider.exact_rerank_present &&
              provider.metadata_prefilter_present &&
              provider.candidate_set_input_present &&
              provider.tombstones_present &&
              provider.generation_evidence_present &&
              provider.telemetry_present,
          "IVF/PQ physical provider surfaces missing");
  Require(HasEvidence(provider.evidence,
                      idx::kVectorIvfPqPhysicalProviderSearchKey),
          "neutral runtime evidence missing");
  RequireNoRuntimeLeak(provider.evidence);
  Require(provider.centroids.size() == provider.lists.size(),
          "centroid/list cardinality mismatch");
  Require(provider.live_vector_count == RowsFixture().size(),
          "live vector count changed");
  Require(provider.residual_error_mean >= 0.0 &&
              provider.list_imbalance_ratio >= 0.0,
          "residual/list imbalance telemetry missing");
  for (const auto& list : provider.lists) {
    Require(list.list_id == list.centroid_id,
            "list assignment is not centroid-aligned");
    Require(std::is_sorted(list.entries.begin(), list.entries.end(),
                           [](const auto& left, const auto& right) {
                             return left.locator.row_ordinal <
                                    right.locator.row_ordinal;
                           }),
            "list entries are not deterministic sorted");
    for (const auto& entry : list.entries) {
      Require(entry.list_id == list.list_id &&
                  !entry.exact_payload.empty() &&
                  !entry.compressed_code.empty() &&
                  entry.insert_generation == provider.provider_generation,
              "stored vector payload/code/generation missing");
      if (compression == idx::VectorIvfPqCompression::sq8) {
        Require(entry.compressed_code.size() == provider.descriptor.dimensions,
                "SQ8 code did not store one byte per dimension");
      }
      if (compression == idx::VectorIvfPqCompression::pq) {
        Require(entry.compressed_code.size() == provider.profile.pq_subspaces,
                "PQ code did not store one code per subspace");
      }
    }
  }
  if (compression == idx::VectorIvfPqCompression::sq8) {
    Require(provider.sq8_axes.size() == provider.descriptor.dimensions,
            "SQ8 scale information was not persisted");
  }
  if (compression == idx::VectorIvfPqCompression::pq) {
    Require(provider.pq_codebooks.size() == provider.profile.pq_subspaces,
            "PQ codebooks were not persisted");
    for (const auto& codebook : provider.pq_codebooks) {
      Require(!codebook.centroids.empty() && codebook.width > 0,
              "PQ subspace codebook is empty");
    }
  }
}

void VerifyQueryPath(const idx::VectorIvfPqPhysicalProvider& provider) {
  const auto result = idx::QueryVectorIvfPqPhysicalProvider(QueryRequest(provider));
  Require(result.ok(), "IVF/PQ query failed");
  Require(result.nprobe_planner_used &&
              result.compressed_code_search_used &&
              result.metadata_prefilter_consumed &&
              result.candidate_set_consumed &&
              result.scalar_kernel_consumed &&
              result.exact_rerank_performed &&
              result.candidate_rows_only &&
              !result.final_rows_authorized &&
              result.batch_results.size() == 2,
          "query did not prove nprobe/compressed/exact-rerank candidate path");
  const auto first_rows = Rows(result.batch_results[0].candidates);
  if (first_rows != std::vector<std::uint64_t>({20, 40, 10})) {
    Fail("exact rerank top-k or deterministic tie order changed: " +
         RowsString(first_rows));
  }
  const auto second_rows = Rows(result.batch_results[1].candidates);
  if (second_rows != std::vector<std::uint64_t>({10, 30})) {
    Fail("candidate-set or metadata prefilter behavior changed: " +
         RowsString(second_rows));
  }
  for (const auto& batch : result.batch_results) {
    Require(batch.nprobe_planner_used &&
                batch.exact_rerank_performed &&
                !batch.selected_list_ids.empty() &&
                batch.compressed_decode_count > 0 &&
                batch.exact_rerank_count > 0,
            "single query omitted planner/code/rerank telemetry");
    for (const auto& candidate : batch.candidates) {
      Require(candidate.compressed_code_scored &&
                  candidate.decoded_from_physical_payload &&
                  candidate.exact_payload_reranked &&
                  candidate.exact_rerank_proof_verified &&
                  candidate.exact_source_recheck_required &&
                  candidate.mga_recheck_required &&
                  candidate.security_recheck_required &&
                  !candidate.final_row_admitted &&
                  !candidate.source_recheck_evidence_ref.empty(),
              "candidate omitted candidate-only recheck proof");
    }
  }
  Require(result.provider_after_telemetry.last_query_selected_lists > 0 &&
              result.provider_after_telemetry.last_query_candidate_count > 0 &&
              result.provider_after_telemetry.last_query_exact_rerank_count > 0 &&
              result.provider_after_telemetry.last_query_latency_units > 0,
          "query telemetry was not captured");
  RequireNoRuntimeLeak(result.evidence);
}

void VerifyBuildQueryReopenDeterminism() {
  const auto built = idx::BuildVectorIvfPqPhysicalProvider(
      BuildRequest(idx::VectorIvfPqCompression::ivf_flat));
  Require(built.ok(), "IVF_FLAT provider build failed");
  VerifyProviderSurfaces(built.provider, idx::VectorIvfPqCompression::ivf_flat);
  VerifyQueryPath(built.provider);

  const auto serialized = idx::SerializeVectorIvfPqPhysicalProvider(built.provider);
  const auto serialized_again =
      idx::SerializeVectorIvfPqPhysicalProvider(built.provider);
  Require(serialized.ok() && serialized_again.ok(),
          "IVF_FLAT serialization failed");
  Require(serialized.bytes == serialized_again.bytes &&
              serialized.checksum == serialized_again.checksum,
          "IVF_FLAT serialization is not deterministic");

  auto open = OpenRequest(serialized.bytes);
  const auto reopened = idx::OpenVectorIvfPqPhysicalProvider(open);
  if (!reopened.ok()) {
    Fail(std::string("IVF_FLAT reopen failed: ") +
         idx::VectorIvfPqOpenClassName(reopened.open_class));
  }
  RequireNoRuntimeLeak(reopened.provider.evidence);
  const auto reserialized =
      idx::SerializeVectorIvfPqPhysicalProvider(reopened.provider);
  Require(reserialized.ok() && reserialized.bytes == serialized.bytes,
          "IVF_FLAT reopen changed persisted bytes");

  auto corrupt = serialized.bytes;
  corrupt.back() ^= static_cast<platform::byte>(0x40);
  open.bytes = corrupt;
  const auto bad_checksum = idx::OpenVectorIvfPqPhysicalProvider(open);
  Require(!bad_checksum.ok() &&
              bad_checksum.open_class == idx::VectorIvfPqOpenClass::bad_checksum,
          "corrupt IVF/PQ bytes did not fail closed on checksum");

  open.bytes = {0x00, 0x01, 0x02};
  const auto corrupt_payload = idx::OpenVectorIvfPqPhysicalProvider(open);
  Require(!corrupt_payload.ok() &&
              corrupt_payload.open_class ==
                  idx::VectorIvfPqOpenClass::corrupt_payload,
          "corrupt payload did not fail closed");

  open = OpenRequest(serialized.bytes);
  open.expected_training_generation = 14;
  const auto stale_training = idx::OpenVectorIvfPqPhysicalProvider(open);
  Require(!stale_training.ok() &&
              stale_training.open_class ==
                  idx::VectorIvfPqOpenClass::stale_generation,
          "stale training generation reopen did not fail closed");

  open = {};
  open.bytes = serialized.bytes;
  const auto missing_proof = idx::OpenVectorIvfPqPhysicalProvider(open);
  Require(!missing_proof.ok() &&
              missing_proof.open_class ==
                  idx::VectorIvfPqOpenClass::missing_exact_recheck_proof,
          "missing reopen proof did not fail closed");
}

void VerifyCompressedCodecs() {
  const auto sq8 = idx::BuildVectorIvfPqPhysicalProvider(
      BuildRequest(idx::VectorIvfPqCompression::sq8));
  Require(sq8.ok(), "SQ8 provider build failed");
  VerifyProviderSurfaces(sq8.provider, idx::VectorIvfPqCompression::sq8);
  VerifyQueryPath(sq8.provider);
  Require(sq8.provider.compression_error_mean >= 0.0,
          "SQ8 compression error telemetry missing");
  const auto sq8_serialized =
      idx::SerializeVectorIvfPqPhysicalProvider(sq8.provider);
  Require(sq8_serialized.ok() &&
              idx::OpenVectorIvfPqPhysicalProvider(
                  OpenRequest(sq8_serialized.bytes)).ok(),
          "SQ8 provider did not persist/reopen");

  const auto pq = idx::BuildVectorIvfPqPhysicalProvider(
      BuildRequest(idx::VectorIvfPqCompression::pq));
  Require(pq.ok(), "PQ provider build failed");
  VerifyProviderSurfaces(pq.provider, idx::VectorIvfPqCompression::pq);
  VerifyQueryPath(pq.provider);
  Require(pq.provider.compression_error_mean >= 0.0,
          "PQ compression error telemetry missing");
  const auto pq_serialized =
      idx::SerializeVectorIvfPqPhysicalProvider(pq.provider);
  Require(pq_serialized.ok() &&
              idx::OpenVectorIvfPqPhysicalProvider(
                  OpenRequest(pq_serialized.bytes)).ok(),
          "PQ provider did not persist/reopen");
}

void VerifyMaintenanceLifecycle() {
  auto provider = idx::BuildVectorIvfPqPhysicalProvider(
      BuildRequest(idx::VectorIvfPqCompression::pq)).provider;

  idx::VectorIvfPqMutation duplicate;
  duplicate.kind = idx::VectorIvfPqMutationKind::insert_row;
  duplicate.expected_provider_generation_present = true;
  duplicate.expected_provider_generation = provider.provider_generation;
  duplicate.expected_training_generation_present = true;
  duplicate.expected_training_generation = provider.training_generation;
  duplicate.expected_descriptor_epoch_present = true;
  duplicate.expected_descriptor_epoch = provider.descriptor.descriptor_epoch;
  duplicate.expected_metric_resource_epoch_present = true;
  duplicate.expected_metric_resource_epoch = provider.metric.metric_resource_epoch;
  duplicate.after_row_present = true;
  duplicate.after_row = {Locator(20), {9.0F, 9.0F, 9.0F, 9.0F}};
  duplicate.recheck_proof = Proof();
  Require(!idx::ApplyVectorIvfPqPhysicalMutation(provider, duplicate).ok(),
          "duplicate row locator was accepted");

  idx::VectorIvfPqMutation insert = duplicate;
  insert.after_row = {Locator(60), {0.9F, 0.9F, 0.9F, 0.9F}};
  const auto inserted =
      idx::ApplyVectorIvfPqPhysicalMutation(provider, insert);
  Require(inserted.ok() &&
              inserted.list_assignment_recomputed &&
              inserted.provider.provider_generation ==
                  provider.provider_generation + 1,
          "insert maintenance failed or did not advance generation");
  auto inserted_query = QueryRequest(inserted.provider);
  inserted_query.queries.resize(1);
  const auto inserted_rows =
      Rows(idx::QueryVectorIvfPqPhysicalProvider(inserted_query)
               .batch_results[0]
               .candidates);
  Require(std::find(inserted_rows.begin(), inserted_rows.end(), 60) !=
              inserted_rows.end(),
          "inserted vector was not queryable as candidate evidence");

  idx::VectorIvfPqMutation stale = insert;
  stale.expected_provider_generation = provider.provider_generation;
  Require(!idx::ApplyVectorIvfPqPhysicalMutation(inserted.provider, stale).ok(),
          "stale provider generation mutation was accepted");

  idx::VectorIvfPqMutation update;
  update.kind = idx::VectorIvfPqMutationKind::update_row;
  update.expected_provider_generation_present = true;
  update.expected_provider_generation = inserted.provider.provider_generation;
  update.expected_training_generation_present = true;
  update.expected_training_generation = inserted.provider.training_generation;
  update.before_row_present = true;
  update.before_row = {Locator(60), {0.9F, 0.9F, 0.9F, 0.9F}};
  update.after_row_present = true;
  update.after_row = {Locator(60), {9.0F, 9.0F, 9.0F, 9.0F}};
  update.recheck_proof = Proof();
  const auto updated =
      idx::ApplyVectorIvfPqPhysicalMutation(inserted.provider, update);
  Require(updated.ok() && updated.list_assignment_recomputed,
          "update maintenance failed");
  auto query = QueryRequest(updated.provider);
  query.queries.resize(1);
  Require(Rows(idx::QueryVectorIvfPqPhysicalProvider(query)
                   .batch_results[0]
                   .candidates) ==
              std::vector<std::uint64_t>({20, 40, 10}),
          "update left stale vector candidate");

  idx::VectorIvfPqMutation erase;
  erase.kind = idx::VectorIvfPqMutationKind::delete_row;
  erase.expected_provider_generation_present = true;
  erase.expected_provider_generation = updated.provider.provider_generation;
  erase.expected_training_generation_present = true;
  erase.expected_training_generation = updated.provider.training_generation;
  erase.before_row_present = true;
  erase.before_row = {Locator(20), {1.0F, 1.0F, 1.0F, 1.0F}};
  erase.recheck_proof = Proof();
  const auto deleted =
      idx::ApplyVectorIvfPqPhysicalMutation(updated.provider, erase);
  Require(deleted.ok() &&
              deleted.tombstone_recorded &&
              deleted.provider.tombstone_count == 1 &&
              deleted.rebuild_recommended,
          "delete did not record tombstone/rebuild evidence");
  const auto after_delete =
      Rows(idx::QueryVectorIvfPqPhysicalProvider(QueryRequest(deleted.provider))
               .batch_results[0]
               .candidates);
  Require(std::find(after_delete.begin(), after_delete.end(), 20) ==
              after_delete.end(),
          "tombstoned row appeared in IVF/PQ candidates");
}

void VerifyFailClosedSurfaces() {
  auto request = BuildRequest(idx::VectorIvfPqCompression::ivf_flat);
  request.rows.push_back(request.rows.front());
  Require(!idx::BuildVectorIvfPqPhysicalProvider(request).ok(),
          "duplicate row locator build did not fail closed");

  request = BuildRequest(idx::VectorIvfPqCompression::ivf_flat);
  request.rows.front().vector.pop_back();
  Require(!idx::BuildVectorIvfPqPhysicalProvider(request).ok(),
          "dimension mismatch did not fail closed");

  request = BuildRequest(idx::VectorIvfPqCompression::ivf_flat);
  request.descriptor.element_profile = idx::VectorExactElementProfile::int8;
  Require(!idx::BuildVectorIvfPqPhysicalProvider(request).ok(),
          "unsupported descriptor profile did not fail closed");

  request = BuildRequest(idx::VectorIvfPqCompression::ivf_flat);
  request.metric.safe = false;
  Require(!idx::BuildVectorIvfPqPhysicalProvider(request).ok(),
          "unsafe metric did not fail closed");

  request = BuildRequest(idx::VectorIvfPqCompression::pq);
  request.profile.pq_subspaces = 0;
  Require(!idx::BuildVectorIvfPqPhysicalProvider(request).ok(),
          "invalid PQ profile did not fail closed");

  request = BuildRequest(idx::VectorIvfPqCompression::ivf_flat);
  request.recheck_proof = {};
  Require(!idx::BuildVectorIvfPqPhysicalProvider(request).ok(),
          "missing exact/MGA/security proof did not fail closed");

  request = BuildRequest(idx::VectorIvfPqCompression::ivf_flat);
  request.metric.index_finality_authority_claimed = true;
  Require(!idx::BuildVectorIvfPqPhysicalProvider(request).ok(),
          "unsafe authority claim did not fail closed");

  const auto provider = idx::BuildVectorIvfPqPhysicalProvider(
      BuildRequest(idx::VectorIvfPqCompression::pq)).provider;
  auto query = QueryRequest(provider);
  query.descriptor_store_scan = true;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(query).ok(),
          "descriptor scan fallback did not fail closed");
  query.descriptor_store_scan = false;
  query.behavior_store_scan = true;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(query).ok(),
          "behavior scan fallback did not fail closed");
  query.behavior_store_scan = false;
  query.contract_only_fallback = true;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(query).ok(),
          "contract fallback did not fail closed");
  query.contract_only_fallback = false;
  query.provider_only_fallback = true;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(query).ok(),
          "provider fallback did not fail closed");
  query.provider_only_fallback = false;
  query.training_generation_current = false;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(query).ok(),
          "stale training generation query did not fail closed");
  query.training_generation_current = true;
  query.metric_resource_epoch_current = false;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(query).ok(),
          "stale metric epoch query did not fail closed");
  query.metric_resource_epoch_current = true;
  query.descriptor_epoch_current = false;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(query).ok(),
          "stale descriptor epoch query did not fail closed");
  query = QueryRequest(provider);
  query.recheck_proof = {};
  Require(!idx::QueryVectorIvfPqPhysicalProvider(query).ok(),
          "missing query proof did not fail closed");
  query = QueryRequest(provider);
  query.queries[0].vector.pop_back();
  Require(!idx::QueryVectorIvfPqPhysicalProvider(query).ok(),
          "query dimension mismatch did not fail closed");
  query = QueryRequest(provider);
  query.queries[0].candidate_set = {idx::TextInvertedRowLocator{}};
  Require(!idx::QueryVectorIvfPqPhysicalProvider(query).ok(),
          "invalid candidate-set locator was accepted");

  auto invalid = provider;
  invalid.centroids.front().vector.clear();
  Require(!idx::QueryVectorIvfPqPhysicalProvider(QueryRequest(invalid)).ok(),
          "invalid centroid runtime did not fail closed");
  invalid = provider;
  invalid.lists.front().centroid_id = 99;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(QueryRequest(invalid)).ok(),
          "invalid list assignment runtime did not fail closed");
  invalid = provider;
  invalid.pq_codebooks.front().centroids.clear();
  Require(!idx::QueryVectorIvfPqPhysicalProvider(QueryRequest(invalid)).ok(),
          "invalid codebook runtime did not fail closed");
  invalid = provider;
  invalid.lists.front().entries.front().compressed_code.front() = 250;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(QueryRequest(invalid)).ok(),
          "invalid compressed code runtime did not fail closed");

  invalid = provider;
  invalid.live_vector_count = 0;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(QueryRequest(invalid)).ok(),
          "stale IVF/PQ provider counts did not fail closed");
  invalid = provider;
  invalid.centroids.front().assigned_count += 10;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(QueryRequest(invalid)).ok(),
          "stale centroid assignment telemetry did not fail closed");
  invalid = provider;
  invalid.training_generation_evidence = provider.training_generation + 1;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(QueryRequest(invalid)).ok(),
          "stale training generation evidence did not fail closed");
  invalid = provider;
  invalid.compression_error_mean = -1.0;
  Require(!idx::QueryVectorIvfPqPhysicalProvider(QueryRequest(invalid)).ok(),
          "invalid compression telemetry did not fail closed");
}

void VerifyEmptyProvider() {
  auto request = BuildRequest(idx::VectorIvfPqCompression::ivf_flat);
  request.rows.clear();
  const auto empty = idx::BuildVectorIvfPqPhysicalProvider(request);
  Require(empty.ok() &&
              empty.provider.centroids.empty() &&
              empty.provider.live_vector_count == 0,
          "empty IVF/PQ provider build failed");
  idx::VectorIvfPqQueryRequest query;
  query.provider = empty.provider;
  query.recheck_proof = Proof();
  idx::VectorIvfPqQuery q;
  q.vector = {0.0F, 0.0F, 0.0F, 0.0F};
  q.top_k = 3;
  q.nprobe = 1;
  query.queries = {q};
  const auto result = idx::QueryVectorIvfPqPhysicalProvider(query);
  Require(result.ok() &&
              result.batch_results.size() == 1 &&
              result.batch_results[0].candidates.empty(),
          "empty IVF/PQ provider did not query as empty candidate set");
  const auto serialized =
      idx::SerializeVectorIvfPqPhysicalProvider(empty.provider);
  Require(serialized.ok(), "empty provider serialization failed");
  idx::VectorIvfPqOpenRequest open;
  open.bytes = serialized.bytes;
  open.recheck_proof = Proof();
  const auto reopened = idx::OpenVectorIvfPqPhysicalProvider(open);
  Require(reopened.ok() && reopened.provider.centroids.empty(),
          "empty provider reopen failed");
}

}  // namespace

int main() {
  VerifyBuildQueryReopenDeterminism();
  VerifyCompressedCodecs();
  VerifyMaintenanceLifecycle();
  VerifyFailClosedSurfaces();
  VerifyEmptyProvider();
  std::cout << "vector_ivf_pq_physical_provider_gate IRC-122 passed\n";
  return EXIT_SUCCESS;
}
