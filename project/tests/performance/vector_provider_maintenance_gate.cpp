// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_platform.hpp"
#include "vector_provider_maintenance.hpp"

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
  std::cerr << "vector_provider_maintenance_gate: " << message << '\n';
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

std::vector<idx::VectorExactSourceRow> RowsFixture() {
  return {{Locator(10), {0.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(20), {1.0F, 1.0F, 1.0F, 1.0F}},
          {Locator(30), {2.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(40), {1.0F, 1.0F, 1.0F, 1.0F}},
          {Locator(50), {-1.0F, 0.0F, 0.0F, 0.0F}},
          {Locator(70), {3.0F, 3.0F, 3.0F, 3.0F}},
          {Locator(80), {0.0F, 2.0F, 0.0F, 0.0F}}};
}

idx::VectorProviderMaintenanceProof MaintenanceProof() {
  idx::VectorProviderMaintenanceProof proof;
  proof.proof_supplied = true;
  proof.exact_source_available = true;
  proof.exact_recheck_proof_supplied = true;
  proof.mga_recheck_proof_supplied = true;
  proof.security_recheck_proof_supplied = true;
  proof.candidate_only_non_authority = true;
  proof.evidence_ref = "vector_provider_source_mga_security_recheck";
  return proof;
}

idx::VectorExactRecheckProof ProviderProof() {
  return idx::ToVectorExactRecheckProof(MaintenanceProof());
}

idx::VectorExactDescriptor Descriptor(std::uint64_t epoch) {
  idx::VectorExactDescriptor descriptor;
  descriptor.dimensions = 4;
  descriptor.element_profile = idx::VectorExactElementProfile::fp32;
  descriptor.descriptor_epoch = epoch;
  descriptor.deterministic = true;
  descriptor.descriptor_safe = true;
  return descriptor;
}

idx::VectorExactMetricResource Metric(std::uint64_t epoch) {
  idx::VectorExactMetricResource metric;
  metric.metric_resource_uuid = "99999999-9999-7999-8999-999999999999";
  metric.metric_resource_epoch = epoch;
  metric.metric_kind = idx::VectorExactMetricKind::l2;
  metric.deterministic = true;
  metric.safe = true;
  return metric;
}

idx::VectorExactBuildRequest ExactBuildRequest() {
  idx::VectorExactBuildRequest request;
  request.relation_uuid = "11111111-1111-7111-8111-111111111111";
  request.index_uuid = "22222222-2222-7222-8222-222222222222";
  request.provider_uuid = "33333333-3333-7333-8333-333333333333";
  request.base_generation = 7;
  request.provider_generation = 11;
  request.descriptor = Descriptor(31);
  request.metric = Metric(37);
  request.recheck_proof = ProviderProof();
  request.rows = RowsFixture();
  return request;
}

idx::VectorHnswBuildRequest HnswBuildRequest() {
  idx::VectorHnswBuildRequest request;
  request.relation_uuid = ExactBuildRequest().relation_uuid;
  request.index_uuid = ExactBuildRequest().index_uuid;
  request.provider_uuid = ExactBuildRequest().provider_uuid;
  request.base_generation = 7;
  request.provider_generation = 11;
  request.training_generation = 13;
  request.descriptor = Descriptor(41);
  request.metric = Metric(43);
  request.profile.m = 4;
  request.profile.ef_construction = 16;
  request.profile.ef_search = 16;
  request.profile.max_level = 5;
  request.profile.compaction_tombstone_ratio = 0.10;
  request.recheck_proof = ProviderProof();
  request.rows = RowsFixture();
  return request;
}

idx::VectorIvfPqBuildRequest IvfPqBuildRequest(
    idx::VectorIvfPqCompression compression) {
  idx::VectorIvfPqBuildRequest request;
  request.relation_uuid = ExactBuildRequest().relation_uuid;
  request.index_uuid = ExactBuildRequest().index_uuid;
  request.provider_uuid = ExactBuildRequest().provider_uuid;
  request.base_generation = 7;
  request.provider_generation = 11;
  request.training_generation = 13;
  request.descriptor = Descriptor(51);
  request.metric = Metric(53);
  request.profile.compression = compression;
  request.profile.centroid_count = 3;
  request.profile.nprobe = 3;
  request.profile.training_iterations = 5;
  request.profile.max_training_rows = 32;
  request.profile.pq_subspaces = 2;
  request.profile.pq_codewords = 4;
  request.profile.retrain_imbalance_ratio = 1.20;
  request.profile.rebuild_tombstone_ratio = 0.10;
  request.recheck_proof = ProviderProof();
  request.rows = RowsFixture();
  return request;
}

idx::VectorProviderMaintenanceContext ContextFor(
    const idx::VectorExactPhysicalProvider& provider) {
  idx::VectorProviderMaintenanceContext context;
  context.collection_uuid = provider.relation_uuid;
  context.index_uuid = provider.index_uuid;
  context.provider_uuid = provider.provider_uuid;
  context.expected_provider_generation = provider.provider_generation;
  context.expected_descriptor_epoch = provider.descriptor.descriptor_epoch;
  context.expected_metric_resource_epoch = provider.metric.metric_resource_epoch;
  context.proof = MaintenanceProof();
  context.policy.max_latency_units = 50;
  return context;
}

idx::VectorProviderMaintenanceContext ContextFor(
    const idx::VectorHnswPhysicalProvider& provider) {
  idx::VectorProviderMaintenanceContext context;
  context.collection_uuid = provider.relation_uuid;
  context.index_uuid = provider.index_uuid;
  context.provider_uuid = provider.provider_uuid;
  context.expected_provider_generation = provider.provider_generation;
  context.expected_training_generation = provider.training_generation;
  context.expected_descriptor_epoch = provider.descriptor.descriptor_epoch;
  context.expected_metric_resource_epoch = provider.metric.metric_resource_epoch;
  context.proof = MaintenanceProof();
  context.policy.max_latency_units = 50;
  context.policy.max_tombstone_ratio = 0.10;
  return context;
}

idx::VectorProviderMaintenanceContext ContextFor(
    const idx::VectorIvfPqPhysicalProvider& provider) {
  idx::VectorProviderMaintenanceContext context;
  context.collection_uuid = provider.relation_uuid;
  context.index_uuid = provider.index_uuid;
  context.provider_uuid = provider.provider_uuid;
  context.expected_provider_generation = provider.provider_generation;
  context.expected_training_generation = provider.training_generation;
  context.expected_descriptor_epoch = provider.descriptor.descriptor_epoch;
  context.expected_metric_resource_epoch = provider.metric.metric_resource_epoch;
  context.proof = MaintenanceProof();
  context.policy.max_tombstone_ratio = 0.10;
  context.policy.max_list_imbalance_ratio = 1.10;
  context.policy.max_residual_error_mean = 0.01;
  context.policy.max_compression_error_mean = 0.01;
  context.policy.max_latency_units = 2;
  return context;
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
  const auto size = ReadU32(bytes, *offset);
  *offset += 4 + size;
}

void SkipBytes(const std::vector<platform::byte>& bytes,
               std::size_t* offset) {
  const auto size = ReadU32(bytes, *offset);
  *offset += 4 + static_cast<std::size_t>(size);
}

void SkipVectorF32(const std::vector<platform::byte>& bytes,
                   std::size_t* offset) {
  const auto size = ReadU32(bytes, *offset);
  *offset += 4 + static_cast<std::size_t>(size) * 4;
}

void SkipLocator(const std::vector<platform::byte>& bytes,
                 std::size_t* offset) {
  *offset += 8;
  SkipString(bytes, offset);
  SkipString(bytes, offset);
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

void RefreshChecksum(std::vector<platform::byte>* bytes) {
  WriteU64(bytes, 16, Checksum(*bytes));
}

std::size_t HnswEntryPointOffset(const std::vector<platform::byte>& bytes) {
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

std::size_t IvfCentroidCountOffset(const std::vector<platform::byte>& bytes) {
  std::size_t offset = 24;
  SkipString(bytes, &offset);
  SkipString(bytes, &offset);
  SkipString(bytes, &offset);
  offset += 8 + 8 + 8 + 4 + 8;
  SkipString(bytes, &offset);
  offset += 8 + 4;
  offset += 4 + 4 + 4 + 4 + 4 + 4 + 4 + 8 + 8;
  offset += 8 + 8 + 8 + 8 + 8 + 8 + 8 + 1 + 1;
  offset += 8 + 8 + 8 + 8 + 8;
  return offset;
}

std::size_t IvfFirstCentroidVectorSizeOffset(
    const std::vector<platform::byte>& bytes) {
  return IvfCentroidCountOffset(bytes) + 8 + 4;
}

std::size_t IvfAfterCentroidsOffset(const std::vector<platform::byte>& bytes) {
  std::size_t offset = IvfCentroidCountOffset(bytes);
  const auto centroid_count = ReadU64(bytes, offset);
  offset += 8;
  for (std::uint64_t i = 0; i < centroid_count; ++i) {
    offset += 4;
    SkipVectorF32(bytes, &offset);
    offset += 8 + 8;
  }
  return offset;
}

std::size_t IvfFirstListIdOffset(const std::vector<platform::byte>& bytes) {
  std::size_t offset = IvfAfterCentroidsOffset(bytes);
  const auto sq8_count = ReadU64(bytes, offset);
  offset += 8 + static_cast<std::size_t>(sq8_count) * 8;
  const auto codebook_count = ReadU64(bytes, offset);
  offset += 8;
  for (std::uint64_t i = 0; i < codebook_count; ++i) {
    offset += 4 + 4 + 4;
    const auto centroid_vectors = ReadU32(bytes, offset);
    offset += 4;
    for (std::uint32_t j = 0; j < centroid_vectors; ++j) {
      SkipVectorF32(bytes, &offset);
    }
  }
  offset += 8;
  return offset;
}

std::size_t IvfFirstListCentroidIdOffset(
    const std::vector<platform::byte>& bytes) {
  return IvfFirstListIdOffset(bytes) + 4;
}

std::size_t IvfFirstCodebookCentroidCountOffset(
    const std::vector<platform::byte>& bytes) {
  std::size_t offset = IvfAfterCentroidsOffset(bytes);
  const auto sq8_count = ReadU64(bytes, offset);
  offset += 8 + static_cast<std::size_t>(sq8_count) * 8;
  offset += 8;
  offset += 4 + 4 + 4;
  return offset;
}

std::size_t IvfFirstCompressedCodePayloadOffset(
    const std::vector<platform::byte>& bytes) {
  std::size_t offset = IvfFirstListIdOffset(bytes);
  offset += 4 + 4 + 8 + 8 + 8;
  offset += 8;
  SkipLocator(bytes, &offset);
  SkipBytes(bytes, &offset);
  offset += 4;
  return offset;
}

std::size_t IvfFirstCompressedCodeSizeOffset(
    const std::vector<platform::byte>& bytes) {
  return IvfFirstCompressedCodePayloadOffset(bytes) - 4;
}

void VerifyRealProviderValidation() {
  const auto exact =
      idx::BuildVectorExactPhysicalProvider(ExactBuildRequest()).provider;
  const auto hnsw =
      idx::BuildVectorHnswPhysicalProvider(HnswBuildRequest()).provider;
  const auto ivf = idx::BuildVectorIvfPqPhysicalProvider(
                       IvfPqBuildRequest(idx::VectorIvfPqCompression::pq))
                       .provider;
  const auto exact_valid =
      idx::ValidateVectorExactProviderForMaintenance(exact, ContextFor(exact));
  const auto hnsw_valid =
      idx::ValidateVectorHnswProviderForMaintenance(hnsw, ContextFor(hnsw));
  const auto ivf_valid =
      idx::ValidateVectorIvfPqProviderForMaintenance(ivf, ContextFor(ivf));
  Require(exact_valid.ok() && exact_valid.serialize_open_path_consumed &&
              exact_valid.query_sanity_consumed,
          "exact provider was not validated through open/query paths");
  Require(hnsw_valid.ok() && hnsw_valid.serialize_open_path_consumed &&
              hnsw_valid.query_sanity_consumed,
          "HNSW provider was not validated through open/query paths");
  Require(ivf_valid.ok() && ivf_valid.serialize_open_path_consumed &&
              ivf_valid.query_sanity_consumed,
          "IVF/PQ provider was not validated through open/query paths");
  RequireNoRuntimeLeak(exact_valid.evidence);
  RequireNoRuntimeLeak(hnsw_valid.evidence);
  RequireNoRuntimeLeak(ivf_valid.evidence);
  Require(HasEvidence(ivf_valid.evidence, "compression_error_mean"),
          "IVF/PQ maintenance did not surface compression telemetry");
}

void VerifyEmptyProviderValidation() {
  auto exact_request = ExactBuildRequest();
  exact_request.rows.clear();
  auto hnsw_request = HnswBuildRequest();
  hnsw_request.rows.clear();
  auto ivf_request = IvfPqBuildRequest(idx::VectorIvfPqCompression::ivf_flat);
  ivf_request.rows.clear();

  const auto exact_build = idx::BuildVectorExactPhysicalProvider(exact_request);
  const auto hnsw_build = idx::BuildVectorHnswPhysicalProvider(hnsw_request);
  const auto ivf_build = idx::BuildVectorIvfPqPhysicalProvider(ivf_request);
  Require(exact_build.ok() && hnsw_build.ok() && ivf_build.ok(),
          "empty vector provider build did not produce valid providers");
  const auto exact = exact_build.provider;
  const auto hnsw = hnsw_build.provider;
  const auto ivf = ivf_build.provider;

  const auto exact_valid =
      idx::ValidateVectorExactProviderForMaintenance(exact, ContextFor(exact));
  const auto hnsw_valid =
      idx::ValidateVectorHnswProviderForMaintenance(hnsw, ContextFor(hnsw));
  const auto ivf_valid =
      idx::ValidateVectorIvfPqProviderForMaintenance(ivf, ContextFor(ivf));

  Require(exact_valid.ok() && exact_valid.query_sanity_consumed &&
              HasEvidence(exact_valid.evidence,
                          "empty_provider_query_result_empty=true"),
          "empty exact provider was not validated through empty query path");
  Require(hnsw_valid.ok() && hnsw_valid.query_sanity_consumed &&
              HasEvidence(hnsw_valid.evidence,
                          "empty_provider_query_result_empty=true"),
          "empty HNSW provider was not validated through empty query path");
  Require(ivf_valid.ok() && ivf_valid.query_sanity_consumed &&
              HasEvidence(ivf_valid.evidence,
                          "empty_provider_query_result_empty=true"),
          "empty IVF/PQ provider was not validated through empty query path");
  RequireNoRuntimeLeak(exact_valid.evidence);
  RequireNoRuntimeLeak(hnsw_valid.evidence);
  RequireNoRuntimeLeak(ivf_valid.evidence);
}

void VerifyHnswCompactionPublishGate() {
  auto provider =
      idx::BuildVectorHnswPhysicalProvider(HnswBuildRequest()).provider;
  idx::VectorHnswMutation del;
  del.kind = idx::VectorHnswMutationKind::delete_row;
  del.expected_provider_generation_present = true;
  del.expected_provider_generation = provider.provider_generation;
  del.before_row_present = true;
  del.before_row = {Locator(20), {1.0F, 1.0F, 1.0F, 1.0F}};
  del.recheck_proof = ProviderProof();
  provider = idx::ApplyVectorHnswPhysicalMutation(provider, del).provider;
  auto context = ContextFor(provider);
  const auto scheduled =
      idx::ScheduleVectorHnswProviderMaintenance(provider, context);
  Require(scheduled.ok() &&
              scheduled.job.job_kind ==
                  idx::VectorProviderMaintenanceJobKind::compact &&
              scheduled.job.hnsw_candidate.has_value() &&
              scheduled.job.state ==
                  idx::VectorProviderMaintenanceJobState::publish_ready,
          "HNSW tombstone compaction did not create publish-ready candidate");

  auto publish_context = context;
  publish_context.proof.validation_successful = true;
  publish_context.proof.generation_advanced = true;
  const auto published = idx::PublishVectorProviderMaintenanceCandidate(
      scheduled.job, publish_context, provider.provider_generation);
  Require(published.ok() && published.hnsw_provider.has_value() &&
              published.hnsw_provider->provider_generation >
                  provider.provider_generation &&
              published.hnsw_provider->tombstone_count == 0,
          "publish gate did not advance to compacted HNSW generation");
  RequireNoRuntimeLeak(scheduled.job.evidence);
  RequireNoRuntimeLeak(published.evidence);
}

void VerifyIvfPqPolicySchedulesRetrainAndRebuild() {
  auto provider = idx::BuildVectorIvfPqPhysicalProvider(
                      IvfPqBuildRequest(idx::VectorIvfPqCompression::pq))
                      .provider;
  auto retrain_context = ContextFor(provider);
  auto drifted = provider;
  drifted.list_imbalance_ratio = 3.0;
  drifted.residual_error_mean = 2.0;
  drifted.compression_error_mean = 2.0;
  drifted.last_query_latency_units = 100;
  const auto retrain = idx::ScheduleVectorIvfPqProviderMaintenance(
      drifted, retrain_context, RowsFixture());
  Require(retrain.ok() &&
              retrain.job.job_kind ==
                  idx::VectorProviderMaintenanceJobKind::retrain &&
              retrain.job.ivf_pq_candidate.has_value() &&
              retrain.job.new_training_generation >
                  provider.training_generation &&
              HasEvidence(retrain.job.evidence, "list_imbalance") &&
              HasEvidence(retrain.job.evidence, "residual_error") &&
              HasEvidence(retrain.job.evidence, "compression_error") &&
              HasEvidence(retrain.job.evidence, "latency_policy"),
          "IVF/PQ drift policy did not schedule retrain with telemetry reasons");

  idx::VectorIvfPqMutation del;
  del.kind = idx::VectorIvfPqMutationKind::delete_row;
  del.expected_provider_generation_present = true;
  del.expected_provider_generation = provider.provider_generation;
  del.expected_training_generation_present = true;
  del.expected_training_generation = provider.training_generation;
  del.before_row_present = true;
  del.before_row = {Locator(20), {1.0F, 1.0F, 1.0F, 1.0F}};
  del.recheck_proof = ProviderProof();
  auto tombstoned =
      idx::ApplyVectorIvfPqPhysicalMutation(provider, del).provider;
  auto rebuild_context = ContextFor(tombstoned);
  rebuild_context.policy.max_list_imbalance_ratio = 100.0;
  rebuild_context.policy.max_residual_error_mean = 100.0;
  rebuild_context.policy.max_compression_error_mean = 100.0;
  rebuild_context.policy.max_latency_units = 0;
  const auto rebuild = idx::ScheduleVectorIvfPqProviderMaintenance(
      tombstoned, rebuild_context, RowsFixture());
  Require(rebuild.ok() &&
              rebuild.job.job_kind ==
                  idx::VectorProviderMaintenanceJobKind::rebuild &&
              rebuild.job.ivf_pq_candidate.has_value() &&
              rebuild.job.new_provider_generation >
                  tombstoned.provider_generation,
          "IVF/PQ tombstone policy did not schedule rebuild");
  RequireNoRuntimeLeak(retrain.job.evidence);
  RequireNoRuntimeLeak(rebuild.job.evidence);
}

void VerifyJobLifecycleAndPublishRefusals() {
  const auto provider =
      idx::BuildVectorHnswPhysicalProvider(HnswBuildRequest()).provider;
  auto context = ContextFor(provider);
  auto job = idx::ScheduleVectorHnswProviderMaintenance(provider, context).job;
  const auto progressed =
      idx::RecordVectorProviderMaintenanceProgress(job, 1, 4, "validate");
  Require(progressed.ok() &&
              progressed.job.state ==
                  idx::VectorProviderMaintenanceJobState::running,
          "progress update did not enter running state");
  const auto cancelled =
      idx::CancelVectorProviderMaintenanceJob(progressed.job, "operator");
  Require(cancelled.ok() &&
              cancelled.job.state ==
                  idx::VectorProviderMaintenanceJobState::cancelled,
          "cancel did not move job to cancelled state");
  const auto resumed = idx::ResumeVectorProviderMaintenanceJob(cancelled.job);
  Require(resumed.ok() &&
              resumed.job.state ==
                  idx::VectorProviderMaintenanceJobState::scheduled,
          "resume did not move cancelled job to scheduled state");
  const auto retry = idx::ClassifyVectorProviderMaintenanceFailure(
      resumed.job,
      idx::VectorProviderMaintenanceFailureClass::
          transient_provider_unavailable,
      true);
  Require(retry.ok() && retry.job.retry_attempts == 1 &&
              retry.job.state ==
                  idx::VectorProviderMaintenanceJobState::scheduled,
          "retryable failure did not schedule retry");
  const auto permanent = idx::ClassifyVectorProviderMaintenanceFailure(
      retry.job,
      idx::VectorProviderMaintenanceFailureClass::
          permanent_validation_failed,
      false);
  Require(!permanent.ok() &&
              permanent.job.state ==
                  idx::VectorProviderMaintenanceJobState::failed &&
              permanent.job.failure_class ==
                  idx::VectorProviderMaintenanceFailureClass::
                      permanent_validation_failed &&
              !permanent.job.support_bundle_rows.empty(),
          "permanent failure was not classified with support bundle output");

  const auto refused_cancel =
      idx::CancelVectorProviderMaintenanceJob(cancelled.job, "again");
  Require(!refused_cancel.ok(), "second cancel did not fail closed");
  const auto refused_resume =
      idx::ResumeVectorProviderMaintenanceJob(provider.provider_uuid.empty()
                                                  ? cancelled.job
                                                  : job);
  Require(!refused_resume.ok(),
          "resume from non-cancelled/non-failed state did not fail closed");

  auto missing_proof = context;
  const auto publish_without_candidate =
      idx::PublishVectorProviderMaintenanceCandidate(job, missing_proof,
                                                     provider.provider_generation);
  Require(!publish_without_candidate.ok(),
          "publish without validation/proof/candidate did not fail closed");

  auto compacted = idx::CompactVectorHnswPhysicalProvider(provider, ProviderProof());
  auto unsafe_job = job;
  unsafe_job.state = idx::VectorProviderMaintenanceJobState::publish_ready;
  unsafe_job.validation_successful = true;
  unsafe_job.publish_candidate_available = true;
  unsafe_job.candidate_only_non_authority = true;
  unsafe_job.old_provider_generation = provider.provider_generation;
  unsafe_job.new_provider_generation = provider.provider_generation;
  unsafe_job.hnsw_candidate = compacted.provider;
  auto publish_context = context;
  publish_context.proof.validation_successful = true;
  publish_context.proof.generation_advanced = true;
  Require(!idx::PublishVectorProviderMaintenanceCandidate(
               unsafe_job, publish_context, provider.provider_generation)
               .ok(),
          "publish without generation advancement did not fail closed");
  unsafe_job.new_provider_generation = compacted.provider.provider_generation;
  unsafe_job.validation_successful = false;
  Require(!idx::PublishVectorProviderMaintenanceCandidate(
               unsafe_job, publish_context, provider.provider_generation)
               .ok(),
          "publish without validation did not fail closed");
  unsafe_job.validation_successful = true;
  unsafe_job.candidate_only_non_authority = false;
  Require(!idx::PublishVectorProviderMaintenanceCandidate(
               unsafe_job, publish_context, provider.provider_generation)
               .ok(),
          "publish without non-authority evidence did not fail closed");
  unsafe_job.candidate_only_non_authority = true;
  Require(!idx::PublishVectorProviderMaintenanceCandidate(
               unsafe_job, publish_context, provider.provider_generation + 99)
               .ok(),
          "publish against stale active generation did not fail closed");

  auto tampered_job = unsafe_job;
  tampered_job.hnsw_candidate = compacted.provider;
  tampered_job.hnsw_candidate->live_node_count = 0;
  Require(!idx::PublishVectorProviderMaintenanceCandidate(
               tampered_job, publish_context, provider.provider_generation)
               .ok(),
          "publish did not revalidate tampered HNSW candidate");
}

void VerifyRepairClassification() {
  const auto exact =
      idx::BuildVectorExactPhysicalProvider(ExactBuildRequest()).provider;
  const auto exact_bytes =
      idx::SerializeVectorExactPhysicalProvider(exact).bytes;
  auto exact_bad_checksum = exact_bytes;
  exact_bad_checksum.back() ^= static_cast<platform::byte>(0x40);
  const auto exact_repair = idx::DiagnoseVectorExactProviderRepair(
      exact_bad_checksum, ContextFor(exact));
  Require(!exact_repair.ok() &&
              exact_repair.repair_class ==
                  idx::VectorProviderRepairClass::bad_checksum &&
              !exact_repair.support_bundle_rows.empty(),
          "bad exact checksum was not classified as restricted repair");
  const auto exact_corrupt = idx::DiagnoseVectorExactProviderRepair(
      {0x00, 0x01, 0x02}, ContextFor(exact));
  Require(!exact_corrupt.ok() &&
              exact_corrupt.repair_class ==
                  idx::VectorProviderRepairClass::corrupt_payload,
          "corrupt exact payload was not classified");

  const auto hnsw =
      idx::BuildVectorHnswPhysicalProvider(HnswBuildRequest()).provider;
  auto hnsw_invalid =
      idx::SerializeVectorHnswPhysicalProvider(hnsw).bytes;
  WriteU32(&hnsw_invalid, HnswEntryPointOffset(hnsw_invalid), 999);
  RefreshChecksum(&hnsw_invalid);
  const auto hnsw_repair =
      idx::DiagnoseVectorHnswProviderRepair(hnsw_invalid, ContextFor(hnsw));
  Require(!hnsw_repair.ok() &&
              hnsw_repair.repair_class ==
                  idx::VectorProviderRepairClass::invalid_graph,
          "invalid HNSW graph was not classified");

  const auto ivf = idx::BuildVectorIvfPqPhysicalProvider(
                       IvfPqBuildRequest(idx::VectorIvfPqCompression::pq))
                       .provider;
  const auto ivf_bytes =
      idx::SerializeVectorIvfPqPhysicalProvider(ivf).bytes;
  auto invalid_centroid = ivf_bytes;
  WriteU32(&invalid_centroid,
           IvfFirstCentroidVectorSizeOffset(invalid_centroid), 0);
  RefreshChecksum(&invalid_centroid);
  Require(idx::DiagnoseVectorIvfPqProviderRepair(invalid_centroid,
                                                 ContextFor(ivf))
              .repair_class ==
              idx::VectorProviderRepairClass::invalid_centroid,
          "invalid IVF/PQ centroid was not classified");
  auto invalid_list = ivf_bytes;
  WriteU32(&invalid_list, IvfFirstListCentroidIdOffset(invalid_list), 999);
  RefreshChecksum(&invalid_list);
  Require(idx::DiagnoseVectorIvfPqProviderRepair(invalid_list, ContextFor(ivf))
              .repair_class == idx::VectorProviderRepairClass::invalid_list,
          "invalid IVF/PQ list was not classified");
  auto invalid_codebook = ivf_bytes;
  WriteU32(&invalid_codebook,
           IvfFirstCodebookCentroidCountOffset(invalid_codebook), 0);
  RefreshChecksum(&invalid_codebook);
  Require(idx::DiagnoseVectorIvfPqProviderRepair(invalid_codebook,
                                                 ContextFor(ivf))
              .repair_class ==
              idx::VectorProviderRepairClass::invalid_codebook,
          "invalid IVF/PQ codebook was not classified");
  auto invalid_code = ivf_bytes;
  WriteU32(&invalid_code, IvfFirstCompressedCodeSizeOffset(invalid_code),
           1000000);
  RefreshChecksum(&invalid_code);
  Require(idx::DiagnoseVectorIvfPqProviderRepair(invalid_code, ContextFor(ivf))
              .repair_class == idx::VectorProviderRepairClass::invalid_code,
          "invalid IVF/PQ compressed code was not classified");
}

void VerifyStaleAndAuthorityFailClosed() {
  const auto provider =
      idx::BuildVectorHnswPhysicalProvider(HnswBuildRequest()).provider;
  auto stale_context = ContextFor(provider);
  stale_context.expected_provider_generation = provider.provider_generation + 1;
  Require(!idx::ValidateVectorHnswProviderForMaintenance(provider,
                                                         stale_context)
               .ok(),
          "stale provider generation validation did not fail closed");
  stale_context = ContextFor(provider);
  stale_context.expected_training_generation = provider.training_generation + 1;
  Require(!idx::ValidateVectorHnswProviderForMaintenance(provider,
                                                         stale_context)
               .ok(),
          "stale training generation validation did not fail closed");
  stale_context = ContextFor(provider);
  stale_context.expected_descriptor_epoch = provider.descriptor.descriptor_epoch + 1;
  Require(!idx::ValidateVectorHnswProviderForMaintenance(provider,
                                                         stale_context)
               .ok(),
          "stale descriptor epoch validation did not fail closed");
  stale_context = ContextFor(provider);
  stale_context.expected_metric_resource_epoch =
      provider.metric.metric_resource_epoch + 1;
  Require(!idx::ValidateVectorHnswProviderForMaintenance(provider,
                                                         stale_context)
               .ok(),
          "stale metric epoch validation did not fail closed");

  auto authority_context = ContextFor(provider);
  authority_context.proof.provider_finality_authority_claimed = true;
  Require(!idx::ValidateVectorHnswProviderForMaintenance(provider,
                                                         authority_context)
               .ok(),
          "unsafe proof authority claim did not fail closed");
  auto unsafe_provider = provider;
  unsafe_provider.index_finality_authority_claimed = true;
  Require(!idx::ValidateVectorHnswProviderForMaintenance(unsafe_provider,
                                                         ContextFor(provider))
               .ok(),
          "unsafe provider authority claim did not fail closed");
}

}  // namespace

int main() {
  VerifyRealProviderValidation();
  VerifyEmptyProviderValidation();
  VerifyHnswCompactionPublishGate();
  VerifyIvfPqPolicySchedulesRetrainAndRebuild();
  VerifyJobLifecycleAndPublishRefusals();
  VerifyRepairClassification();
  VerifyStaleAndAuthorityFailClosed();
  std::cout << "vector_provider_maintenance_gate: passed\n";
  return EXIT_SUCCESS;
}
