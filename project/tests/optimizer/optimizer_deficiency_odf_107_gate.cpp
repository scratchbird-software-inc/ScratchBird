// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-107 cross-platform and CPU-feature compatibility gate.

#include "async_page_io.hpp"
#include "direct_binary_result_frame.hpp"
#include "gpu_acceleration.hpp"
#include "local_parser_shared_memory_transport.hpp"
#include "native_sblr_specialization.hpp"
#include "resource_governance_admission.hpp"
#include "runtime_capabilities.hpp"
#include "scoring_kernel_acceleration.hpp"
#include "shared_memory_ipc_ring.hpp"
#include "uuid_v7_index_encoding.hpp"
#include "vectorized_result_batch.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace exec = scratchbird::engine::executor;
namespace gpu = scratchbird::engine::gpu_acceleration;
namespace idx = scratchbird::core::index;
namespace ipc = scratchbird::ipc;
namespace server = scratchbird::server;
namespace native = scratchbird::engine::native_compile;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace wire = scratchbird::wire;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "parser_or_reference_authority=true",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true",
          "provider_security_policy_authority=true",
          "provider_redaction_policy_authority=true",
          "provider_recovery_authority=true",
          "provider_page_or_catalog_authority=true",
          "wal_authority=true",
          "write_ahead_log_finality_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-107 evidence leaked forbidden authority or document token");
    }
  }
}

platform::RuntimeEndian OppositeEndian() {
  return platform::CurrentRuntimeEndian() == platform::RuntimeEndian::little
             ? platform::RuntimeEndian::big
             : platform::RuntimeEndian::little;
}

platform::RuntimeCompatibilityDescriptor RuntimeDescriptor(
    std::string route_id) {
  auto descriptor =
      platform::CurrentRuntimeCompatibilityDescriptor(std::move(route_id));
  descriptor.deterministic_scalar_fallback_available = true;
  return descriptor;
}

agents::ResourceGovernanceAdmissionRequest Governance(
    agents::ResourceGovernanceFamily family) {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id =
      std::string("odf107.") + agents::ResourceGovernanceFamilyName(family);
  request.descriptor.descriptor_id =
      std::string("odf107.runtime.") +
      agents::ResourceGovernanceFamilyName(family);
  request.descriptor.family = family;
  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label =
      std::string("runtime.policy.odf107.") +
      agents::ResourceGovernanceFamilyName(family);
  request.descriptor.descriptor_generation = 107;
  request.descriptor.expected_generation = 107;
  request.descriptor.over_limit_action =
      agents::ResourceGovernanceAction::kExactScalarFallback;
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.descriptor.limits = {
      65536, 65536, 4096, 65536, 16, 2, 16, 128, 8, 128, 8, 2, 1000000};
  request.requested = {4096, 4096, 256, 4096, 2, 1, 2, 4, 1, 4, 1, 1, 1000};
  request.require_exact_scalar_fallback_available = true;
  request.exact_scalar_fallback_available = true;
  return request;
}

platform::Status OkStatus() {
  return {platform::StatusCode::ok, platform::Severity::info,
          platform::Subsystem::storage_page};
}

std::vector<std::uint8_t> RepeatedBytes(std::size_t count,
                                        std::uint8_t seed = 1) {
  std::vector<std::uint8_t> bytes(count);
  for (std::size_t i = 0; i < count; ++i) {
    bytes[i] = static_cast<std::uint8_t>(seed + (i % 17U));
  }
  return bytes;
}

exec::VectorizedResultBatch SimpleBatch() {
  exec::VectorizedResultBatchBuilder builder(2);
  builder.AddColumn(exec::MakeFixedWidthResultBatchColumn(
      "id", 2, 4, RepeatedBytes(8),
      exec::MakeResultBatchValidityBitmap(2)));
  auto finalized = builder.Finalize();
  Require(finalized.ok(), "ODF-107 vectorized fixture did not finalize");
  return finalized.batch;
}

void CoreNegotiationRouteAndDiagnostics() {
  auto descriptor = RuntimeDescriptor("odf107.core.compatible");
  descriptor.required_cpu_features = {"scalar_exact"};
  auto admitted = platform::NegotiateRuntimeCompatibility(descriptor);
  Require(admitted.ok &&
              admitted.action ==
                  platform::RuntimeCompatibilityAction::admit,
          "ODF-107 compatible runtime descriptor was not admitted");
  Require(EvidenceHas(admitted.evidence, "runtime_compatibility.route=odf107"),
          "ODF-107 core route evidence missing");
  RequireEvidenceHygiene(admitted.evidence);

  descriptor = RuntimeDescriptor("odf107.core.endian_mismatch");
  descriptor.provider_endian = OppositeEndian();
  auto fallback = platform::NegotiateRuntimeCompatibility(descriptor);
  Require(!fallback.ok && fallback.fallback_required &&
              fallback.action ==
                  platform::RuntimeCompatibilityAction::exact_scalar_fallback,
          "ODF-107 endian mismatch did not select exact scalar fallback");
  Require(fallback.diagnostic_code ==
              "SB_RUNTIME_COMPATIBILITY.ENDIAN_MISMATCH",
          "ODF-107 endian mismatch diagnostic changed");

  descriptor = RuntimeDescriptor("odf107.core.scalar_missing");
  descriptor.provider_endian = OppositeEndian();
  descriptor.deterministic_scalar_fallback_available = false;
  auto refused = platform::NegotiateRuntimeCompatibility(descriptor);
  Require(!refused.ok && refused.fail_closed,
          "ODF-107 missing scalar fallback did not fail closed");
  Require(refused.diagnostic_code ==
              "SB_RUNTIME_COMPATIBILITY.ENDIAN_MISMATCH",
          "ODF-107 fail-closed diagnostic changed");
}

void DirectBinaryFramesNegotiateCompatibility() {
  auto frame = wire::BuildDirectBinaryResultFrame(SimpleBatch());
  if (!frame.ok()) {
    Fail("ODF-107 direct binary frame build failed: " +
         frame.diagnostic.diagnostic_code + ":" +
         frame.diagnostic.remediation_hint);
  }
  Require(EvidenceHas(frame.evidence, "runtime_compatibility.route=odf107"),
          "ODF-107 direct binary build compatibility evidence missing");
  RequireEvidenceHygiene(frame.evidence);

  auto parsed = wire::ParseDirectBinaryResultFrame(frame.frame.bytes);
  Require(parsed.ok(), "ODF-107 direct binary frame parse failed");
  Require(EvidenceHas(parsed.evidence, "runtime_compatibility.route=odf107"),
          "ODF-107 direct binary parse compatibility evidence missing");

  auto bad = RuntimeDescriptor("odf107.direct_binary.bad_endian");
  bad.provider_endian = platform::RuntimeEndian::big;
  auto refused = wire::ValidateDirectBinaryResultFrameRuntimeCompatibility(bad);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-107 direct binary accepted wrong endian");
  Require(EvidenceHas(refused.evidence,
                      "SB_RUNTIME_COMPATIBILITY.ENDIAN_MISMATCH"),
          "ODF-107 direct binary endian diagnostic evidence missing");
}

void SharedMemoryRingNegotiatesCompatibility() {
  ipc::SharedMemoryRingOptions options;
  options.slot_count = 2;
  options.payload_capacity = 4096;
  options.max_inline_payload_bytes = 64;
  options.max_payload_bytes = 4096;
  options.runtime_compatibility = RuntimeDescriptor("odf107.ipc.compatible");
  ipc::SharedMemoryIpcRing ring;
  auto created = ipc::SharedMemoryIpcRing::Create(options, &ring);
  Require(created.ok, "ODF-107 shared-memory ring creation failed");
  Require(EvidenceHas(created.evidence, "runtime_compatibility.route=odf107"),
          "ODF-107 shared-memory ring compatibility evidence missing");
  Require(ring.header().runtime_generation ==
              platform::CurrentRuntimeCompatibilityGeneration(),
          "ODF-107 shared-memory ring runtime generation changed");

  options.runtime_compatibility = RuntimeDescriptor("odf107.ipc.bad_alignment");
  options.runtime_compatibility.required_alignment = 128;
  options.runtime_compatibility.provider_alignment = 8;
  auto refused = ipc::SharedMemoryIpcRing::Create(options, &ring);
  Require(!refused.ok &&
              refused.diagnostic.code ==
                  "IPC.RING.RUNTIME_COMPATIBILITY_MISMATCH",
          "ODF-107 shared-memory ring accepted incompatible alignment");
  Require(EvidenceHas(refused.evidence,
                      "SB_RUNTIME_COMPATIBILITY.ALIGNMENT_MISMATCH"),
          "ODF-107 shared-memory alignment diagnostic evidence missing");

  options.runtime_compatibility = RuntimeDescriptor("odf107.ipc.transport");
  server::LocalParserSharedMemoryTransport transport;
  auto transport_created =
      server::LocalParserSharedMemoryTransport::Create(options, options,
                                                       &transport);
  Require(transport_created.ok,
          "ODF-107 local parser shared-memory transport creation failed");
  Require(EvidenceHas(transport_created.evidence,
                      "runtime_compatibility.route=odf107"),
          "ODF-107 local parser transport compatibility evidence missing");
}

platform::TypedUuid MakeV7(std::uint8_t suffix) {
  platform::Uuid raw;
  raw.bytes = {0x01, 0x9d, 0x10, 0x20, 0x30, 0x40,
               0x70, 0x00, 0x80, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, suffix};
  return {platform::UuidKind::row, raw};
}

void UuidV7ComparatorAndDictionaryNegotiatesCompatibility() {
  idx::UuidV7IndexEncodeRequest request;
  request.expected_kind = platform::UuidKind::row;
  request.dictionary_generation = 107;
  for (std::uint8_t i = 1; i <= 24; ++i) {
    request.uuids.push_back(MakeV7(i));
  }
  request.runtime_compatibility =
      RuntimeDescriptor("odf107.uuid_v7.compatible");
  request.runtime_compatibility.required_accelerator_capabilities = {
      "uuid_v7_time_prefix_comparator"};
  request.runtime_compatibility.provider_accelerator_capabilities = {
      "uuid_v7_time_prefix_comparator",
      "uuid_v7_prefix_dictionary",
      "uncompressed_uuid_fallback"};
  auto encoded = idx::BuildUuidV7IndexPageEncoding(request);
  Require(encoded.ok && encoded.compressed,
          "ODF-107 UUID v7 index encoding did not use specialized route");
  Require(EvidenceHas(encoded.evidence,
                      "runtime_compatibility.route=odf107"),
          "ODF-107 UUID v7 runtime compatibility evidence missing");

  request.runtime_compatibility =
      RuntimeDescriptor("odf107.uuid_v7.missing_capability");
  request.runtime_compatibility.required_accelerator_capabilities = {
      "missing_uuid_v7_vector_compare"};
  request.runtime_compatibility.provider_accelerator_capabilities = {
      "uuid_v7_time_prefix_comparator"};
  auto fallback = idx::BuildUuidV7IndexPageEncoding(request);
  Require(!fallback.ok && fallback.fallback_to_uncompressed_uuid,
          "ODF-107 UUID v7 missing capability did not fall back");
  Require(fallback.refusal_reason.find(
              "SB_RUNTIME_COMPATIBILITY.ACCELERATOR_CAPABILITY_MISMATCH") !=
              std::string::npos,
          "ODF-107 UUID v7 capability diagnostic changed");
}

page::AsyncPageIoRequest AsyncRequest() {
  page::AsyncPageIoRequest request;
  request.route_generation = 107;
  request.capabilities.async_read_supported = true;
  request.capabilities.async_write_supported = true;
  request.capabilities.async_fsync_supported = true;
  request.capabilities.write_combining_supported = true;
  request.capabilities.publication_marker_supported = true;
  request.capabilities.durable_sync_fence_supported = true;
  request.capabilities.max_batch_operations = 8;
  request.capabilities.max_batch_bytes = 65536;
  request.capabilities.max_combined_writes = 8;
  request.capabilities.runtime_compatibility =
      RuntimeDescriptor("odf107.async.compatible");
  request.capabilities.runtime_compatibility.required_accelerator_capabilities =
      {"async_read", "write_combining", "durable_sync_fence"};
  request.policy.estimated_sync_micros = 1000;
  request.policy.estimated_async_micros = 500;
  page::AsyncPageIoOperation read;
  read.kind = page::AsyncPageIoOperationKind::kReadPage;
  read.operation_id = "odf107.read";
  read.page_number = 1;
  read.page_generation = 1;
  read.descriptor_generation = 107;
  read.byte_count = 4096;
  read.publication_marker = "m";
  read.expected_publication_marker = "m";
  page::AsyncPageIoOperation write = read;
  write.kind = page::AsyncPageIoOperationKind::kWritePage;
  write.operation_id = "odf107.write";
  write.page_number = 2;
  write.payload = RepeatedBytes(4096, 7);
  page::AsyncPageIoOperation fsync = read;
  fsync.kind = page::AsyncPageIoOperationKind::kFsync;
  fsync.operation_id = "odf107.fsync";
  request.operations = {read, write, fsync};
  request.resource_governance =
      Governance(agents::ResourceGovernanceFamily::kAsyncPageIo);
  return request;
}

page::AsyncPageIoRouteBackend AsyncBackend() {
  page::AsyncPageIoRouteBackend backend;
  backend.read_page = [](const page::AsyncPageIoOperation& operation) {
    page::AsyncPageIoBackendResult result;
    result.status = OkStatus();
    result.read_payload.assign(static_cast<std::size_t>(operation.byte_count),
                               3);
    return result;
  };
  backend.write_page = [](const page::AsyncPageIoOperation&) {
    page::AsyncPageIoBackendResult result;
    result.status = OkStatus();
    return result;
  };
  backend.fsync = [] {
    page::AsyncPageIoBackendResult result;
    result.status = OkStatus();
    return result;
  };
  return backend;
}

void AsyncIoAndWriteCombiningNegotiatesCompatibility() {
  auto request = AsyncRequest();
  auto result = page::ExecuteAsyncPageIoBatch(request, AsyncBackend());
  Require(result.ok() && result.selected,
          "ODF-107 async I/O compatible route did not execute");
  Require(EvidenceHas(result.evidence, "runtime_compatibility.route=odf107"),
          "ODF-107 async I/O compatibility evidence missing");
  RequireEvidenceHygiene(result.evidence);

  request = AsyncRequest();
  request.capabilities.runtime_compatibility.provider_endian =
      OppositeEndian();
  result = page::ExecuteAsyncPageIoBatch(request, AsyncBackend());
  Require(result.ok() && result.fallback_used,
          "ODF-107 async I/O endian mismatch did not use sync fallback");
  Require(result.diagnostic.diagnostic_code ==
              "async_page_io_odf107_runtime_fallback",
          "ODF-107 async I/O fallback diagnostic changed");
}

native::NativeSblrValueBatch NativeScalar(
    const native::NativeSblrInputBatch& input) {
  native::NativeSblrValueBatch values;
  values.values.reserve(input.values.size());
  for (const auto value : input.values) {
    values.values.push_back(value * 2);
  }
  return values;
}

native::NativeSblrSpecializationRequest NativeRequest() {
  native::NativeSblrSpecializationRequest request;
  request.kind = native::NativeSblrSpecializationKind::kProjection;
  request.identity.stable_template_id = "odf107.native.template";
  request.identity.sblr_digest = "odf107.native.digest";
  request.identity.plan_node_id = "odf107.plan";
  request.identity.template_generation = 107;
  request.identity.expected_template_generation = 107;
  request.epochs.security_epoch = 7;
  request.epochs.expected_security_epoch = 7;
  request.epochs.redaction_epoch = 8;
  request.epochs.expected_redaction_epoch = 8;
  request.hotness.observed_invocations = 10;
  request.hotness.minimum_invocations = 1;
  request.input.values = {1, 2, 3};
  request.input.row_count = request.input.values.size();
  request.scalar_reference = NativeScalar;
  request.provider.manifest.provider_id = "odf107.native.provider";
  request.provider.manifest.engine_abi_id = "sb_engine_abi_v3";
  request.provider.manifest.architecture = platform::CurrentRuntimeArchitecture();
  request.provider.manifest.cpu_capabilities = {"scalar_exact", "sse2"};
  request.provider.manifest.supported_kinds = {
      native::NativeSblrSpecializationKind::kProjection};
  request.provider.compile = [](const native::NativeSblrSpecializationRequest&) {
    native::NativeSblrCompileOutcome outcome;
    outcome.ok = true;
    outcome.kernel_id = "odf107.native.kernel";
    return outcome;
  };
  request.provider.run =
      [](const native::NativeSblrSpecializationRequest& req,
         const native::NativeSblrCompileOutcome& compile) {
        native::NativeSblrKernelOutcome outcome;
        outcome.ok = true;
        outcome.kernel_id = compile.kernel_id;
        outcome.template_generation = req.identity.template_generation;
        outcome.security_epoch = req.epochs.security_epoch;
        outcome.redaction_epoch = req.epochs.redaction_epoch;
        outcome.values = NativeScalar(req.input);
        return outcome;
      };
  request.capabilities.required_engine_abi_id = "sb_engine_abi_v3";
  request.capabilities.required_architecture = platform::CurrentRuntimeArchitecture();
  request.capabilities.required_cpu_capabilities = {"scalar_exact"};
  request.capabilities.runtime_compatibility =
      RuntimeDescriptor("odf107.native.compatible");
  request.resource_governance =
      Governance(agents::ResourceGovernanceFamily::kPreparedNativeSpecialization);
  return request;
}

void NativeSpecializationNegotiatesCompatibility() {
  auto request = NativeRequest();
  auto result = native::ExecuteNativeSblrSpecialization(request);
  Require(result.ok && result.native_used,
          "ODF-107 native specialization compatible route did not execute");
  Require(EvidenceHas(result.evidence, "runtime_compatibility.route=odf107"),
          "ODF-107 native specialization compatibility evidence missing");

  request = NativeRequest();
  request.capabilities.required_cpu_capabilities = {"missing_odf107_cpu"};
  request.capabilities.runtime_compatibility =
      RuntimeDescriptor("odf107.native.missing_cpu");
  result = native::ExecuteNativeSblrSpecialization(request);
  Require(result.ok && result.scalar_fallback_used,
          "ODF-107 native CPU mismatch did not use scalar fallback");
  Require(result.diagnostic_code ==
              "SB_NATIVE_SBLR.ODF107_RUNTIME_COMPATIBILITY_FALLBACK",
          "ODF-107 native compatibility diagnostic changed");

  request = NativeRequest();
  request.capabilities.required_cpu_capabilities =
      {"missing_odf107_cpu_without_explicit_descriptor"};
  request.capabilities.runtime_compatibility = {};
  result = native::ExecuteNativeSblrSpecialization(request);
  Require(result.ok && result.scalar_fallback_used,
          "ODF-107 native implicit CPU negotiation did not use scalar fallback");
  Require(result.diagnostic_code ==
              "SB_NATIVE_SBLR.ABI_CPU_CAPABILITY_MISMATCH_FALLBACK",
          "ODF-107 native implicit compatibility did not preserve legacy diagnostic");
  Require(EvidenceHas(result.evidence,
                      "SB_RUNTIME_COMPATIBILITY.CPU_FEATURE_MISMATCH"),
          "ODF-107 native implicit CPU negotiation evidence missing");
}

gpu::ScoringKernelValueBatch ScoringScalar(
    const gpu::ScoringKernelInputBatch& input) {
  gpu::ScoringKernelValueBatch values;
  double sum = 0.0;
  for (std::size_t i = 0; i < input.lhs_values.size(); ++i) {
    sum += input.lhs_values[i] * input.rhs_values[i];
  }
  values.double_values = {sum};
  return values;
}

gpu::ScoringKernelRequest ScoringRequest() {
  gpu::ScoringKernelRequest request;
  request.descriptor.kind = gpu::ScoringKernelKind::kVectorDistance;
  request.descriptor.descriptor_id = "odf107.scoring.descriptor";
  request.descriptor.kernel_digest = "odf107.scoring.digest";
  request.descriptor.descriptor_generation = 107;
  request.descriptor.expected_descriptor_generation = 107;
  request.input.batch_id = "odf107.scoring.batch";
  request.input.row_count = 3;
  request.input.materialized_memory_bytes = 4096;
  request.input.lhs_values = {1.0, 2.0, 3.0};
  request.input.rhs_values = {4.0, 5.0, 6.0};
  request.scalar_reference = ScoringScalar;
  request.provider.manifest.provider_id = "odf107.scoring.provider";
  request.provider.manifest.engine_abi_id = "sb_engine_abi_v3";
  request.provider.manifest.runtime_identity_id = "odf107.runtime.identity";
  request.provider.manifest.architecture = platform::CurrentRuntimeArchitecture();
  request.provider.manifest.cpu_capabilities = {"scalar_exact", "sse2"};
  request.provider.manifest.gpu_capabilities = {"deterministic_gpu_scoring"};
  request.provider.manifest.supported_kinds = {
      gpu::ScoringKernelKind::kVectorDistance};
  request.provider.run = [](const gpu::ScoringKernelRequest& req) {
    gpu::ScoringKernelProviderOutcome outcome;
    outcome.ok = true;
    outcome.kernel_id = "odf107.scoring.kernel";
    outcome.descriptor_generation = req.descriptor.descriptor_generation;
    outcome.runtime_identity_id = req.provider.manifest.runtime_identity_id;
    outcome.values = ScoringScalar(req.input);
    return outcome;
  };
  request.capabilities.required_engine_abi_id = "sb_engine_abi_v3";
  request.capabilities.required_runtime_identity_id = "odf107.runtime.identity";
  request.capabilities.required_architecture = platform::CurrentRuntimeArchitecture();
  request.capabilities.required_cpu_capabilities = {"scalar_exact"};
  request.capabilities.required_gpu_capabilities = {
      "deterministic_gpu_scoring"};
  request.capabilities.runtime_compatibility =
      RuntimeDescriptor("odf107.scoring.compatible");
  request.resource_governance =
      Governance(agents::ResourceGovernanceFamily::kScoringKernelAccelerator);
  return request;
}

void ScoringKernelNegotiatesCompatibility() {
  auto request = ScoringRequest();
  auto result = gpu::ExecuteScoringKernelAcceleration(request);
  Require(result.ok && result.accelerator_used,
          "ODF-107 scoring kernel compatible route did not execute");
  Require(EvidenceHas(result.evidence, "runtime_compatibility.route=odf107"),
          "ODF-107 scoring compatibility evidence missing");

  request = ScoringRequest();
  request.capabilities.required_gpu_capabilities = {"missing_odf107_gpu"};
  request.capabilities.runtime_compatibility =
      RuntimeDescriptor("odf107.scoring.missing_gpu");
  result = gpu::ExecuteScoringKernelAcceleration(request);
  Require(result.ok && result.scalar_fallback_used,
          "ODF-107 scoring GPU mismatch did not use scalar fallback");
  Require(result.diagnostic_code ==
              "SB_SCORING_KERNEL.ODF107_RUNTIME_COMPATIBILITY_FALLBACK",
          "ODF-107 scoring compatibility fallback diagnostic changed");

  request = ScoringRequest();
  request.capabilities.required_gpu_capabilities =
      {"missing_odf107_gpu_without_explicit_descriptor"};
  request.capabilities.runtime_compatibility = {};
  result = gpu::ExecuteScoringKernelAcceleration(request);
  Require(result.ok && result.scalar_fallback_used,
          "ODF-107 scoring implicit GPU negotiation did not use scalar fallback");
  Require(result.diagnostic_code ==
              "SB_SCORING_KERNEL.CAPABILITY_MISMATCH_FALLBACK",
          "ODF-107 scoring implicit compatibility did not preserve legacy diagnostic");
  Require(EvidenceHas(result.evidence,
                      "SB_RUNTIME_COMPATIBILITY.ACCELERATOR_CAPABILITY_MISMATCH"),
          "ODF-107 scoring implicit GPU negotiation evidence missing");
}

void GpuApiRuntimeRouteNegotiatesCompatibility() {
  gpu::GpuAccelerationRequest request;
  request.execution_requested = true;
  request.provider_available = true;
  request.security_context_present = true;
  request.workload = "vector";
  request.provider = "deterministic_gpu_scoring";
  request.backend = "deterministic_gpu_scoring";
  request.policy_profile = gpu::GpuPolicyProfile::optional_batch;
  request.batch_operation = gpu::GpuBatchOperation::vector_dot;
  request.values = {1.0, 2.0, 3.0};
  request.rhs_values = {4.0, 5.0, 6.0};
  request.runtime_compatibility =
      RuntimeDescriptor("odf107.gpu_api.compatible");
  request.runtime_compatibility.required_accelerator_capabilities = {
      "deterministic_gpu_batch"};
  auto result = gpu::EvaluateGpuAcceleration(request);
  Require(result.ok && !result.fallback_used,
          "ODF-107 GPU API compatible route did not admit provider");

  request.runtime_compatibility =
      RuntimeDescriptor("odf107.gpu_api.missing_capability");
  request.runtime_compatibility.required_accelerator_capabilities = {
      "missing_gpu_api_capability"};
  result = gpu::EvaluateGpuAcceleration(request);
  Require(result.ok && result.fallback_used &&
              result.diagnostic_code == "GPU.RUNTIME_COMPATIBILITY_FALLBACK",
          "ODF-107 GPU API capability mismatch did not use CPU fallback");
}

}  // namespace

int main() {
  CoreNegotiationRouteAndDiagnostics();
  DirectBinaryFramesNegotiateCompatibility();
  SharedMemoryRingNegotiatesCompatibility();
  UuidV7ComparatorAndDictionaryNegotiatesCompatibility();
  AsyncIoAndWriteCombiningNegotiatesCompatibility();
  NativeSpecializationNegotiatesCompatibility();
  ScoringKernelNegotiatesCompatibility();
  GpuApiRuntimeRouteNegotiatesCompatibility();
  std::cout << "optimizer_deficiency_odf_107_gate passed\n";
  return EXIT_SUCCESS;
}
