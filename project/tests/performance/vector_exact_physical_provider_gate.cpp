// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_platform.hpp"
#include "vector_exact_physical_provider.hpp"

#include <algorithm>
#include <cmath>
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
  std::cerr << "vector_exact_physical_provider_gate: " << message << '\n';
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
  locator.row_uuid = UuidWithSuffix("77777777-7777-7777-8777-", row);
  locator.version_uuid = UuidWithSuffix("88888888-8888-7888-8888-", row);
  return locator;
}

std::vector<std::uint64_t> Rows(
    const std::vector<idx::VectorExactCandidate>& candidates) {
  std::vector<std::uint64_t> rows;
  for (const auto& candidate : candidates) {
    rows.push_back(candidate.locator.row_ordinal);
  }
  return rows;
}

idx::VectorExactRecheckProof Proof() {
  idx::VectorExactRecheckProof proof;
  proof.proof_supplied = true;
  proof.exact_source_vector_available = true;
  proof.exact_rerank_proof_supplied = true;
  proof.mga_recheck_required = true;
  proof.security_recheck_required = true;
  proof.evidence_ref = "vector_exact_source_mga_security_rerank_contract";
  return proof;
}

idx::VectorExactDescriptor Descriptor(
    idx::VectorExactElementProfile profile) {
  idx::VectorExactDescriptor descriptor;
  descriptor.dimensions = 4;
  descriptor.element_profile = profile;
  descriptor.descriptor_epoch = 31;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  descriptor.int8_scale = 0.25;
  descriptor.int8_zero_point = 0;
  return descriptor;
}

idx::VectorExactMetricResource Metric(idx::VectorExactMetricKind kind) {
  idx::VectorExactMetricResource metric;
  metric.metric_resource_uuid = "99999999-9999-7999-8999-999999999999";
  metric.metric_resource_epoch = 37;
  metric.metric_kind = kind;
  metric.deterministic = true;
  metric.safe = true;
  return metric;
}

std::vector<idx::VectorExactSourceRow> RowsFixture() {
  return {{Locator(10), {0.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(20), {1.0F, 1.0F, 1.0F, 1.0F}},
          {Locator(30), {2.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(40), {1.0F, 1.0F, 1.0F, 1.0F}},
          {Locator(50), {-1.0F, 0.0F, 0.0F, 0.0F}}};
}

idx::VectorExactBuildRequest BuildRequest(
    idx::VectorExactElementProfile profile,
    idx::VectorExactMetricKind metric_kind) {
  idx::VectorExactBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.descriptor = Descriptor(profile);
  request.metric = Metric(metric_kind);
  request.recheck_proof = Proof();
  request.rows = RowsFixture();
  return request;
}

void WriteFile(const std::filesystem::path& path,
               const std::vector<platform::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.flush();
  Require(static_cast<bool>(out), "could not write persistence fixture");
}

std::vector<platform::byte> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "could not read persistence fixture");
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>()};
}

void VerifyFp32BuildQueryMaintenanceAndReopen() {
  const auto built = idx::BuildVectorExactPhysicalProvider(
      BuildRequest(idx::VectorExactElementProfile::fp32,
                   idx::VectorExactMetricKind::l2));
  Require(built.ok(), "fp32 vector exact provider build failed");
  Require(built.provider.encoded_payloads_present &&
              built.provider.fp32_payloads_supported &&
              built.provider.fp16_payloads_supported &&
              built.provider.int8_payloads_supported &&
              built.provider.exact_decode_scoring_present &&
              built.provider.batched_query_present &&
              built.provider.metadata_prefilter_present &&
              built.provider.candidate_set_input_present &&
              built.provider.top_k_heap_present &&
              built.provider.exact_rerank_present,
          "vector exact physical surfaces missing");
  Require(HasEvidence(built.provider.evidence,
                      idx::kVectorExactPhysicalProviderSearchKey),
          "neutral runtime evidence missing");
  RequireNoRuntimeLeak(built.provider.evidence);

  idx::VectorExactQueryRequest query;
  query.provider = built.provider;
  query.recheck_proof = Proof();
  idx::VectorExactQuery first;
  first.vector = {1.0F, 1.0F, 1.0F, 1.0F};
  first.top_k = 3;
  idx::VectorExactQuery second;
  second.vector = {0.0F, 0.0F, 0.0F, 0.0F};
  second.top_k = 2;
  second.candidate_set = {Locator(10), Locator(30), Locator(50)};
  second.metadata_prefilter = [](const idx::TextInvertedRowLocator& locator) {
    return locator.row_ordinal != 50;
  };
  query.queries = {first, second};
  const auto result = idx::QueryVectorExactPhysicalProvider(query);
  Require(result.ok(), "batched vector exact query failed");
  Require(result.batched_query_evaluation &&
              result.metadata_prefilter_consumed &&
              result.candidate_set_consumed &&
              result.scalar_kernel_consumed &&
              result.candidate_rows_only &&
              !result.final_rows_authorized &&
              result.batch_results.size() == 2,
          "query evidence did not prove batch/filter/scalar/candidate-only path");
  Require(Rows(result.batch_results[0].candidates) ==
              std::vector<std::uint64_t>({20, 40, 10}),
          "top-k heap or deterministic tie order changed");
  Require(Rows(result.batch_results[1].candidates) ==
              std::vector<std::uint64_t>({10, 30}),
          "candidate-set or metadata prefilter result changed");
  for (const auto& batch : result.batch_results) {
    Require(batch.top_k_heap_used &&
                batch.exact_rerank_performed &&
                batch.decoded_vector_count > 0 &&
                batch.scalar_kernel_consumed_count > 0,
            "single query omitted exact rerank/scalar evidence");
    for (const auto& candidate : batch.candidates) {
      Require(candidate.decoded_from_physical_payload &&
                  candidate.exact_rerank_proof_verified &&
                  candidate.exact_source_recheck_required &&
                  candidate.mga_recheck_required &&
                  candidate.security_recheck_required &&
                  !candidate.final_row_admitted &&
                  !candidate.source_recheck_evidence_ref.empty(),
              "candidate omitted candidate-only exact recheck proof");
    }
  }
  RequireNoRuntimeLeak(result.evidence);

  idx::VectorExactMutation duplicate;
  duplicate.kind = idx::VectorExactMutationKind::insert_row;
  duplicate.expected_provider_generation_present = true;
  duplicate.expected_provider_generation = built.provider.provider_generation;
  duplicate.expected_descriptor_epoch_present = true;
  duplicate.expected_descriptor_epoch = built.provider.descriptor.descriptor_epoch;
  duplicate.expected_metric_resource_epoch_present = true;
  duplicate.expected_metric_resource_epoch =
      built.provider.metric.metric_resource_epoch;
  duplicate.after_row_present = true;
  duplicate.after_row = {Locator(20), {9.0F, 9.0F, 9.0F, 9.0F}};
  duplicate.recheck_proof = Proof();
  Require(!idx::ApplyVectorExactPhysicalMutation(built.provider, duplicate).ok(),
          "duplicate row locator was accepted");

  idx::VectorExactMutation insert = duplicate;
  insert.after_row = {Locator(60), {1.0F, 1.0F, 1.0F, 1.0F}};
  const auto inserted =
      idx::ApplyVectorExactPhysicalMutation(built.provider, insert);
  Require(inserted.ok() &&
              inserted.provider.provider_generation ==
                  built.provider.provider_generation + 1,
          "insert maintenance failed or did not advance generation");
  query.provider = inserted.provider;
  query.queries = {first};
  Require(Rows(idx::QueryVectorExactPhysicalProvider(query)
                   .batch_results[0]
                   .candidates) ==
              std::vector<std::uint64_t>({20, 40, 60}),
          "inserted vector was not queryable as candidate evidence");

  idx::VectorExactMutation stale = insert;
  stale.expected_provider_generation = built.provider.provider_generation;
  Require(!idx::ApplyVectorExactPhysicalMutation(inserted.provider, stale).ok(),
          "stale descriptor/generation mutation was accepted");

  idx::VectorExactMutation update;
  update.kind = idx::VectorExactMutationKind::update_row;
  update.expected_provider_generation_present = true;
  update.expected_provider_generation = inserted.provider.provider_generation;
  update.expected_descriptor_epoch_present = true;
  update.expected_descriptor_epoch =
      inserted.provider.descriptor.descriptor_epoch;
  update.expected_metric_resource_epoch_present = true;
  update.expected_metric_resource_epoch =
      inserted.provider.metric.metric_resource_epoch;
  update.before_row_present = true;
  update.before_row = {Locator(60), {1.0F, 1.0F, 1.0F, 1.0F}};
  update.after_row_present = true;
  update.after_row = {Locator(60), {9.0F, 9.0F, 9.0F, 9.0F}};
  update.recheck_proof = Proof();
  const auto updated =
      idx::ApplyVectorExactPhysicalMutation(inserted.provider, update);
  Require(updated.ok(), "update maintenance failed");
  query.provider = updated.provider;
  Require(Rows(idx::QueryVectorExactPhysicalProvider(query)
                   .batch_results[0]
                   .candidates) ==
              std::vector<std::uint64_t>({20, 40, 10}),
          "update maintenance left stale vector candidate");

  idx::VectorExactMutation erase;
  erase.kind = idx::VectorExactMutationKind::delete_row;
  erase.expected_provider_generation_present = true;
  erase.expected_provider_generation = updated.provider.provider_generation;
  erase.expected_descriptor_epoch_present = true;
  erase.expected_descriptor_epoch =
      updated.provider.descriptor.descriptor_epoch;
  erase.expected_metric_resource_epoch_present = true;
  erase.expected_metric_resource_epoch =
      updated.provider.metric.metric_resource_epoch;
  erase.before_row_present = true;
  erase.before_row = {Locator(60), {9.0F, 9.0F, 9.0F, 9.0F}};
  erase.recheck_proof = Proof();
  const auto erased =
      idx::ApplyVectorExactPhysicalMutation(updated.provider, erase);
  Require(erased.ok(), "delete maintenance failed");

  const auto serialized = idx::SerializeVectorExactPhysicalProvider(erased.provider);
  Require(serialized.ok(), "serialization failed");
  const auto serialized_again =
      idx::SerializeVectorExactPhysicalProvider(erased.provider);
  Require(serialized_again.ok() && serialized_again.bytes == serialized.bytes,
          "serialization is not deterministic");
  const auto path =
      std::filesystem::temp_directory_path() /
      "scratchbird_vector_exact_physical_provider_gate.sbvex";
  WriteFile(path, serialized.bytes);
  const auto persisted = ReadFile(path);
  std::filesystem::remove(path);
  Require(persisted == serialized.bytes, "persisted bytes changed");

  idx::VectorExactOpenRequest open;
  open.bytes = serialized.bytes;
  open.expected_relation_uuid_present = true;
  open.expected_relation_uuid = erased.provider.relation_uuid;
  open.expected_index_uuid_present = true;
  open.expected_index_uuid = erased.provider.index_uuid;
  open.expected_provider_uuid_present = true;
  open.expected_provider_uuid = erased.provider.provider_uuid;
  open.expected_base_generation_present = true;
  open.expected_base_generation = erased.provider.base_generation;
  open.expected_provider_generation_present = true;
  open.expected_provider_generation = erased.provider.provider_generation;
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = erased.provider.descriptor.descriptor_epoch;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch =
      erased.provider.metric.metric_resource_epoch;
  open.expected_dimensions_present = true;
  open.expected_dimensions = erased.provider.descriptor.dimensions;
  open.recheck_proof = Proof();
  const auto opened = idx::OpenVectorExactPhysicalProvider(open);
  Require(opened.ok(), "clean reopen failed");
  const auto reserialized =
      idx::SerializeVectorExactPhysicalProvider(opened.provider);
  Require(reserialized.ok() && reserialized.bytes == serialized.bytes,
          "reopen serialization equivalence failed");

  auto corrupt = serialized.bytes;
  corrupt.back() ^= 0x7f;
  open.bytes = corrupt;
  const auto corrupt_result = idx::OpenVectorExactPhysicalProvider(open);
  Require(!corrupt_result.ok() &&
              corrupt_result.open_class == idx::VectorExactOpenClass::bad_checksum,
          "bad checksum did not fail closed");
}

void VerifyFp16AndInt8Scoring() {
  const auto fp16 = idx::BuildVectorExactPhysicalProvider(
      BuildRequest(idx::VectorExactElementProfile::fp16,
                   idx::VectorExactMetricKind::cosine));
  Require(fp16.ok(), "fp16 provider build failed");
  idx::VectorExactQueryRequest cosine;
  cosine.provider = fp16.provider;
  cosine.recheck_proof = Proof();
  idx::VectorExactQuery q;
  q.vector = {1.0F, 1.0F, 1.0F, 1.0F};
  q.top_k = 2;
  cosine.queries = {q};
  const auto cosine_result = idx::QueryVectorExactPhysicalProvider(cosine);
  Require(cosine_result.ok() &&
              Rows(cosine_result.batch_results[0].candidates) ==
                  std::vector<std::uint64_t>({20, 40}),
          "fp16 cosine decode/scoring changed");

  const auto int8 = idx::BuildVectorExactPhysicalProvider(
      BuildRequest(idx::VectorExactElementProfile::int8,
                   idx::VectorExactMetricKind::inner_product));
  Require(int8.ok(), "int8 provider build failed");
  idx::VectorExactQueryRequest ip;
  ip.provider = int8.provider;
  ip.recheck_proof = Proof();
  idx::VectorExactQuery ipq;
  ipq.vector = {1.0F, 1.0F, 1.0F, 1.0F};
  ipq.top_k = 3;
  ip.queries = {ipq};
  const auto ip_result = idx::QueryVectorExactPhysicalProvider(ip);
  Require(ip_result.ok() &&
              Rows(ip_result.batch_results[0].candidates) ==
                  std::vector<std::uint64_t>({20, 40, 30}),
          "int8 inner-product decode/scoring changed");

  const auto encoded = idx::EncodeVectorExactPayload(
      {0.25F, -0.25F, 1.0F, -1.0F},
      Descriptor(idx::VectorExactElementProfile::int8));
  const auto decoded = idx::DecodeVectorExactPayload(
      encoded, Descriptor(idx::VectorExactElementProfile::int8));
  Require(decoded.size() == 4 &&
              std::fabs(decoded[0] - 0.25F) < 0.001F &&
              std::fabs(decoded[1] + 0.25F) < 0.001F,
          "int8 encode/decode path is not real");
}

void VerifyEmptyProviderAndStrictMaintenance() {
  auto empty_request = BuildRequest(idx::VectorExactElementProfile::fp32,
                                    idx::VectorExactMetricKind::l2);
  empty_request.rows.clear();
  const auto empty = idx::BuildVectorExactPhysicalProvider(empty_request);
  Require(empty.ok(), "empty vector exact provider build failed");

  idx::VectorExactQueryRequest empty_query;
  empty_query.provider = empty.provider;
  empty_query.recheck_proof = Proof();
  idx::VectorExactQuery empty_q;
  empty_q.vector = {0.0F, 0.0F, 0.0F, 0.0F};
  empty_q.top_k = 5;
  empty_query.queries = {empty_q};
  const auto empty_result = idx::QueryVectorExactPhysicalProvider(empty_query);
  Require(empty_result.ok() &&
              empty_result.batch_results.size() == 1 &&
              empty_result.batch_results[0].candidates.empty(),
          "empty vector exact provider did not query as empty candidate set");

  const auto empty_serialized =
      idx::SerializeVectorExactPhysicalProvider(empty.provider);
  Require(empty_serialized.ok(), "empty provider serialization failed");
  idx::VectorExactOpenRequest empty_open;
  empty_open.bytes = empty_serialized.bytes;
  empty_open.recheck_proof = Proof();
  const auto empty_reopened = idx::OpenVectorExactPhysicalProvider(empty_open);
  Require(empty_reopened.ok() && empty_reopened.provider.rows.empty(),
          "empty provider reopen failed");

  auto one_row_request = BuildRequest(idx::VectorExactElementProfile::fp32,
                                      idx::VectorExactMetricKind::l2);
  one_row_request.rows = {{Locator(10), {0.0F, 0.0F, 0.0F, 0.0F}}};
  const auto one_row = idx::BuildVectorExactPhysicalProvider(one_row_request);
  Require(one_row.ok(), "one-row vector exact provider build failed");

  idx::VectorExactMutation wrong_delete;
  wrong_delete.kind = idx::VectorExactMutationKind::delete_row;
  wrong_delete.expected_provider_generation_present = true;
  wrong_delete.expected_provider_generation = one_row.provider.provider_generation;
  wrong_delete.expected_descriptor_epoch_present = true;
  wrong_delete.expected_descriptor_epoch =
      one_row.provider.descriptor.descriptor_epoch;
  wrong_delete.expected_metric_resource_epoch_present = true;
  wrong_delete.expected_metric_resource_epoch =
      one_row.provider.metric.metric_resource_epoch;
  wrong_delete.before_row_present = true;
  wrong_delete.before_row = {Locator(10), {9.0F, 9.0F, 9.0F, 9.0F}};
  wrong_delete.recheck_proof = Proof();
  Require(!idx::ApplyVectorExactPhysicalMutation(one_row.provider,
                                                 wrong_delete)
               .ok(),
          "delete accepted stale/mismatched source vector");

  idx::VectorExactMutation delete_last = wrong_delete;
  delete_last.before_row = {Locator(10), {0.0F, 0.0F, 0.0F, 0.0F}};
  const auto deleted_last =
      idx::ApplyVectorExactPhysicalMutation(one_row.provider, delete_last);
  Require(deleted_last.ok() && deleted_last.provider.rows.empty(),
          "delete of final vector row failed");

  empty_query.provider = deleted_last.provider;
  const auto after_delete = idx::QueryVectorExactPhysicalProvider(empty_query);
  Require(after_delete.ok() &&
              after_delete.batch_results[0].candidates.empty(),
          "deleted final row remained visible as vector candidate evidence");

  idx::VectorExactQueryRequest invalid_candidate = empty_query;
  invalid_candidate.provider = one_row.provider;
  invalid_candidate.queries[0].candidate_set = {idx::TextInvertedRowLocator{}};
  Require(!idx::QueryVectorExactPhysicalProvider(invalid_candidate).ok(),
          "invalid candidate-set locator was accepted");
}

void VerifyFailClosedDiagnostics() {
  auto missing = BuildRequest(idx::VectorExactElementProfile::fp32,
                              idx::VectorExactMetricKind::l2);
  missing.recheck_proof = {};
  const auto missing_result = idx::BuildVectorExactPhysicalProvider(missing);
  Require(!missing_result.ok() &&
              missing_result.diagnostic.diagnostic_code ==
                  "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.MISSING_EXACT_RECHECK",
          "missing exact/MGA/security proof did not fail closed");

  auto mismatch = BuildRequest(idx::VectorExactElementProfile::fp32,
                               idx::VectorExactMetricKind::l2);
  mismatch.rows[0].vector.pop_back();
  const auto mismatch_result =
      idx::BuildVectorExactPhysicalProvider(mismatch);
  Require(!mismatch_result.ok() &&
              mismatch_result.diagnostic.diagnostic_code ==
                  "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.DIMENSION_MISMATCH",
          "dimension mismatch did not fail closed");

  auto unsupported = BuildRequest(idx::VectorExactElementProfile::fp32,
                                  idx::VectorExactMetricKind::l2);
  unsupported.descriptor.element_profile =
      static_cast<idx::VectorExactElementProfile>(999);
  const auto unsupported_result =
      idx::BuildVectorExactPhysicalProvider(unsupported);
  Require(!unsupported_result.ok() &&
              unsupported_result.diagnostic.diagnostic_code ==
                  "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.UNSUPPORTED_ELEMENT_PROFILE",
          "unsupported element profile did not fail closed");

  auto unsafe_metric = BuildRequest(idx::VectorExactElementProfile::fp32,
                                    idx::VectorExactMetricKind::l2);
  unsafe_metric.metric.safe = false;
  Require(!idx::BuildVectorExactPhysicalProvider(unsafe_metric).ok(),
          "unsafe metric resource was accepted");

  auto authority = BuildRequest(idx::VectorExactElementProfile::fp32,
                                idx::VectorExactMetricKind::l2);
  authority.metric.index_finality_authority_claimed = true;
  const auto authority_result =
      idx::BuildVectorExactPhysicalProvider(authority);
  Require(!authority_result.ok() &&
              authority_result.diagnostic.diagnostic_code ==
                  "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.AUTHORITY_CLAIM_REFUSED",
          "authority claim was accepted");

  const auto built = idx::BuildVectorExactPhysicalProvider(
      BuildRequest(idx::VectorExactElementProfile::fp32,
                   idx::VectorExactMetricKind::l2));
  Require(built.ok(), "fixture build failed for runtime refusals");

  idx::VectorExactQueryRequest query;
  query.provider = built.provider;
  query.recheck_proof = Proof();
  idx::VectorExactQuery q;
  q.vector = {1.0F, 1.0F, 1.0F, 1.0F};
  q.top_k = 1;
  query.queries = {q};
  query.descriptor_store_scan = true;
  Require(!idx::QueryVectorExactPhysicalProvider(query).ok(),
          "descriptor scan fallback did not fail closed");
  query.descriptor_store_scan = false;
  query.behavior_store_scan = true;
  Require(!idx::QueryVectorExactPhysicalProvider(query).ok(),
          "behavior scan fallback did not fail closed");
  query.behavior_store_scan = false;
  query.contract_only_fallback = true;
  Require(!idx::QueryVectorExactPhysicalProvider(query).ok(),
          "contract-only fallback did not fail closed");
  query.contract_only_fallback = false;
  query.provider_only_fallback = true;
  Require(!idx::QueryVectorExactPhysicalProvider(query).ok(),
          "provider-only fallback did not fail closed");
  query.provider_only_fallback = false;
  query.metric_resource_epoch_current = false;
  const auto stale_metric_query =
      idx::QueryVectorExactPhysicalProvider(query);
  Require(!stale_metric_query.ok() &&
              stale_metric_query.diagnostic.diagnostic_code ==
                  "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.STALE_METRIC_EPOCH",
          "stale metric epoch query did not fail closed");
  query.metric_resource_epoch_current = true;
  query.descriptor_epoch_current = false;
  const auto stale_descriptor_query =
      idx::QueryVectorExactPhysicalProvider(query);
  Require(!stale_descriptor_query.ok() &&
              stale_descriptor_query.diagnostic.diagnostic_code ==
                  "INDEX.VECTOR_EXACT_PHYSICAL_PROVIDER.STALE_DESCRIPTOR_EPOCH",
          "stale descriptor query did not fail closed");

  const auto serialized = idx::SerializeVectorExactPhysicalProvider(built.provider);
  Require(serialized.ok(), "serialization failed for reopen refusals");
  idx::VectorExactOpenRequest open;
  open.bytes = serialized.bytes;
  open.recheck_proof = Proof();
  open.expected_descriptor_epoch_present = true;
  open.expected_descriptor_epoch = 999;
  Require(!idx::OpenVectorExactPhysicalProvider(open).ok(),
          "stale descriptor epoch reopen did not fail closed");
  open.expected_descriptor_epoch_present = false;
  open.expected_metric_resource_epoch_present = true;
  open.expected_metric_resource_epoch = 999;
  const auto stale_metric = idx::OpenVectorExactPhysicalProvider(open);
  Require(!stale_metric.ok() &&
              stale_metric.open_class ==
                  idx::VectorExactOpenClass::stale_metric_epoch,
          "stale metric epoch reopen did not fail closed");
  idx::VectorExactOpenRequest corrupt;
  corrupt.bytes = {0x00, 0x01, 0x02};
  corrupt.recheck_proof = Proof();
  const auto corrupt_result = idx::OpenVectorExactPhysicalProvider(corrupt);
  Require(!corrupt_result.ok() &&
              corrupt_result.open_class ==
                  idx::VectorExactOpenClass::corrupt_payload,
          "corrupt payload reopen did not fail closed");
}

}  // namespace

int main() {
  VerifyFp32BuildQueryMaintenanceAndReopen();
  VerifyFp16AndInt8Scoring();
  VerifyEmptyProviderAndStrictMaintenance();
  VerifyFailClosedDiagnostics();
  std::cout << "vector_exact_physical_provider_gate=passed\n";
  return EXIT_SUCCESS;
}
