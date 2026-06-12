// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-106 resource governance and admission quota gate.

#include "async_page_io.hpp"
#include "parallel_physical_pipeline.hpp"
#include "resource_governance_admission.hpp"
#include "scoring_kernel_executor.hpp"
#include "server_resource_governance.hpp"
#include "sblr_native_specialization.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace exec = scratchbird::engine::executor;
namespace gpu = scratchbird::engine::gpu_acceleration;
namespace native = scratchbird::engine::native_compile;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace server = scratchbird::server;

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
          "parser_or_reference_finality_or_visibility_authority=true",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true",
          "provider_recovery_authority=true",
          "wal_recovery_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-106 evidence leaked forbidden authority or execution_plan token");
    }
  }
}

agents::ResourceGovernanceQuotaVector Limits() {
  return {65536, 32768, 4096, 65536, 64, 4, 64, 1024, 16, 1024, 64, 8,
          1000000};
}

agents::ResourceGovernanceQuotaVector SmallRequest() {
  return {4096, 1024, 256, 4096, 4, 1, 4, 32, 1, 32, 4, 1, 1000};
}

agents::ResourceGovernanceAdmissionRequest Governance(
    agents::ResourceGovernanceFamily family,
    agents::ResourceGovernanceAction over_limit_action) {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id =
      std::string("odf106.") + agents::ResourceGovernanceFamilyName(family);
  request.descriptor.descriptor_id =
      std::string("odf106.runtime.") +
      agents::ResourceGovernanceFamilyName(family);
  request.descriptor.family = family;
  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label =
      std::string("runtime.policy.odf106.") +
      agents::ResourceGovernanceFamilyName(family);
  request.descriptor.descriptor_generation = 106;
  request.descriptor.expected_generation = 106;
  request.descriptor.limits = Limits();
  request.descriptor.over_limit_action = over_limit_action;
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.requested = SmallRequest();
  request.require_exact_scalar_fallback_available =
      over_limit_action == agents::ResourceGovernanceAction::kExactScalarFallback;
  request.exact_scalar_fallback_available =
      request.require_exact_scalar_fallback_available;
  return request;
}

void RequireAdmissionAction(
    const agents::ResourceGovernanceAdmissionResult& result,
    agents::ResourceGovernanceAction action,
    std::string_view diagnostic) {
  Require(result.action == action, "ODF-106 admission action changed");
  Require(result.diagnostic_code == diagnostic,
          "ODF-106 admission diagnostic changed");
  Require(EvidenceHas(result.evidence,
                      "resource_governance.hidden_unlimited_default=false"),
          "ODF-106 hidden-unlimited evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

void SuccessfulAdmissionForEveryFamily() {
  const std::vector<agents::ResourceGovernanceFamily> families = {
      agents::ResourceGovernanceFamily::kQueryMemoryArena,
      agents::ResourceGovernanceFamily::kAdaptiveTuningKnob,
      agents::ResourceGovernanceFamily::kAsyncPageIo,
      agents::ResourceGovernanceFamily::kParallelPhysicalPipeline,
      agents::ResourceGovernanceFamily::kPreparedNativeSpecialization,
      agents::ResourceGovernanceFamily::kScoringKernelAccelerator,
      agents::ResourceGovernanceFamily::kAcceleratorProviderCache,
      agents::ResourceGovernanceFamily::kBulkCopyLane};
  for (const auto family : families) {
    const auto result = agents::AdmitResourceGovernance(
        Governance(family, agents::ResourceGovernanceAction::kFailClosed));
    Require(result.ok && result.reservation_created,
            "ODF-106 healthy family admission failed");
    RequireAdmissionAction(result, agents::ResourceGovernanceAction::kAdmit,
                           "SB_RESOURCE_GOVERNANCE.ADMITTED");
  }
}

void DescriptorRefusalsAreExact() {
  auto request = Governance(agents::ResourceGovernanceFamily::kAsyncPageIo,
                            agents::ResourceGovernanceAction::kSlowdownDegrade);
  request.descriptor.expected_generation = 107;
  RequireAdmissionAction(agents::AdmitResourceGovernance(request),
                         agents::ResourceGovernanceAction::kFailClosed,
                         "SB_RESOURCE_GOVERNANCE.STALE_DESCRIPTOR_REFUSED");

  request = Governance(agents::ResourceGovernanceFamily::kBulkCopyLane,
                       agents::ResourceGovernanceAction::kFailClosed);
  request.descriptor.limits.io_ops = -1;
  RequireAdmissionAction(agents::AdmitResourceGovernance(request),
                         agents::ResourceGovernanceAction::kFailClosed,
                         "SB_RESOURCE_GOVERNANCE.NEGATIVE_LIMIT_REFUSED");

  request = Governance(agents::ResourceGovernanceFamily::kAcceleratorProviderCache,
                       agents::ResourceGovernanceAction::kFailClosed);
  request.descriptor.corrupt = true;
  RequireAdmissionAction(agents::AdmitResourceGovernance(request),
                         agents::ResourceGovernanceAction::kFailClosed,
                         "SB_RESOURCE_GOVERNANCE.CORRUPT_DESCRIPTOR_REFUSED");

  request = Governance(agents::ResourceGovernanceFamily::kScoringKernelAccelerator,
                       agents::ResourceGovernanceAction::kExactScalarFallback);
  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kExecution_PlanEvidence;
  request.descriptor.source_path_or_label =
      "docs" "/execution-plans/optimizer-deficiency-full-implementation-closure/TRACKER.csv";
  RequireAdmissionAction(agents::AdmitResourceGovernance(request),
                         agents::ResourceGovernanceAction::kFailClosed,
                         "SB_RESOURCE_GOVERNANCE.EXECUTION_PLAN_DESCRIPTOR_REFUSED");

  request = Governance(agents::ResourceGovernanceFamily::kBulkCopyLane,
                       agents::ResourceGovernanceAction::kFailClosed);
  request.requested.io_ops = request.descriptor.limits.io_ops + 1;
  const auto quota_refused = agents::AdmitResourceGovernance(request);
  RequireAdmissionAction(quota_refused,
                         agents::ResourceGovernanceAction::kFailClosed,
                         "SB_RESOURCE_GOVERNANCE.QUOTA_REFUSED");
  Require(quota_refused.fail_closed,
          "ODF-106 fail-closed quota action did not set fail_closed");

  request = Governance(agents::ResourceGovernanceFamily::kScoringKernelAccelerator,
                       agents::ResourceGovernanceAction::kExactScalarFallback);
  request.expected_family = agents::ResourceGovernanceFamily::kAsyncPageIo;
  RequireAdmissionAction(agents::AdmitResourceGovernance(request),
                         agents::ResourceGovernanceAction::kFailClosed,
                         "SB_RESOURCE_GOVERNANCE.FAMILY_MISMATCH_REFUSED");
}

void NoQuotaDimensionHasUnlimitedDefault() {
  for (int ordinal = 0; ordinal < 13; ++ordinal) {
    auto request = Governance(agents::ResourceGovernanceFamily::kQueryMemoryArena,
                              agents::ResourceGovernanceAction::kFailClosed);
    auto* fields = &request.descriptor.limits;
    switch (ordinal) {
      case 0: fields->memory_bytes = 0; break;
      case 1: fields->device_memory_bytes = 0; break;
      case 2: fields->pinned_memory_bytes = 0; break;
      case 3: fields->io_bytes = 0; break;
      case 4: fields->io_ops = 0; break;
      case 5: fields->worker_threads = 0; break;
      case 6: fields->backlog_items = 0; break;
      case 7: fields->candidate_rows = 0; break;
      case 8: fields->cache_entries = 0; break;
      case 9: fields->batch_rows = 0; break;
      case 10: fields->fragments = 0; break;
      case 11: fields->lanes = 0; break;
      case 12: fields->time_budget_microseconds = 0; break;
    }
    RequireAdmissionAction(agents::AdmitResourceGovernance(request),
                           agents::ResourceGovernanceAction::kFailClosed,
                           "SB_RESOURCE_GOVERNANCE.UNBOUNDED_LIMIT_REFUSED");
  }
}

platform::Status OkStatus() {
  return {platform::StatusCode::ok, platform::Severity::info,
          platform::Subsystem::storage_page};
}

page::AsyncPageIoRouteBackend AsyncBackend() {
  page::AsyncPageIoRouteBackend backend;
  backend.read_page = [](const page::AsyncPageIoOperation& op) {
    page::AsyncPageIoBackendResult result;
    result.status = OkStatus();
    result.read_payload.assign(static_cast<std::size_t>(op.byte_count), 1);
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

page::AsyncPageIoRequest AsyncRequest(bool over_quota) {
  page::AsyncPageIoRequest request;
  request.route_generation = 106;
  request.capabilities.async_read_supported = true;
  request.capabilities.async_write_supported = true;
  request.capabilities.async_fsync_supported = true;
  request.capabilities.write_combining_supported = true;
  request.capabilities.publication_marker_supported = true;
  request.capabilities.durable_sync_fence_supported = true;
  request.capabilities.max_batch_operations = 8;
  request.capabilities.max_batch_bytes = 65536;
  request.capabilities.max_combined_writes = 8;
  request.policy.estimated_sync_micros = 1000;
  request.policy.estimated_async_micros = 500;
  page::AsyncPageIoOperation op;
  op.kind = page::AsyncPageIoOperationKind::kReadPage;
  op.operation_id = "odf106.read";
  op.page_number = 1;
  op.page_generation = 1;
  op.descriptor_generation = 106;
  op.byte_count = 4096;
  op.publication_marker = "m";
  op.expected_publication_marker = "m";
  request.operations = {op};
  request.resource_governance =
      Governance(agents::ResourceGovernanceFamily::kAsyncPageIo,
                 agents::ResourceGovernanceAction::kSlowdownDegrade);
  if (over_quota) {
    request.resource_governance.requested.io_bytes =
        request.resource_governance.descriptor.limits.io_bytes + 1;
  }
  return request;
}

void AsyncPageIoUsesAdmissionResult() {
  auto admitted = page::ExecuteAsyncPageIoBatch(AsyncRequest(false), AsyncBackend());
  Require(admitted.ok() && admitted.selected,
          "ODF-106 admitted async route did not execute");
  Require(EvidenceHas(admitted.evidence, "resource_governance.route=odf106"),
          "ODF-106 async route did not carry governance evidence");

  auto degraded = page::ExecuteAsyncPageIoBatch(AsyncRequest(true), AsyncBackend());
  Require(degraded.ok() && degraded.fallback_used,
          "ODF-106 async quota pressure did not degrade to sync fallback");
  Require(degraded.diagnostic.diagnostic_code ==
              "async_page_io_odf106_quota_degrade",
          "ODF-106 async degrade diagnostic changed");

  auto mismatch = AsyncRequest(false);
  mismatch.resource_governance.descriptor.family =
      agents::ResourceGovernanceFamily::kScoringKernelAccelerator;
  auto refused = page::ExecuteAsyncPageIoBatch(mismatch, AsyncBackend());
  Require(!refused.ok() && refused.fail_closed,
          "ODF-106 async route accepted mismatched quota family");
  Require(refused.diagnostic.diagnostic_code ==
              "async_page_io_odf106_quota_refused",
          "ODF-106 async family-mismatch diagnostic changed");
  Require(EvidenceHas(refused.evidence,
                      "SB_RESOURCE_GOVERNANCE.FAMILY_MISMATCH_REFUSED"),
          "ODF-106 async family-mismatch evidence missing");
}

void ParallelPipelineUsesAdmissionResult() {
  exec::ParallelPhysicalPipelineRequest request;
  request.family = exec::ParallelPhysicalPipelineFamily::kPageScan;
  request.snapshot.token_id = "odf106.snapshot";
  request.snapshot.snapshot_generation = 106;
  request.resource_governance =
      Governance(agents::ResourceGovernanceFamily::kParallelPhysicalPipeline,
                 agents::ResourceGovernanceAction::kSlowdownDegrade);
  request.resource_governance.requested.worker_threads =
      request.resource_governance.descriptor.limits.worker_threads + 1;
  const auto result = exec::ExecuteParallelPhysicalPipeline(request);
  Require(result.fallback_used,
          "ODF-106 parallel route did not consume slowdown admission result");
  Require(result.diagnostic.diagnostic_code ==
              "parallel_pipeline_odf106_quota_degrade",
          "ODF-106 parallel degrade diagnostic changed");
}

gpu::ScoringKernelValueBatch ScalarScoring(
    const gpu::ScoringKernelInputBatch& input) {
  gpu::ScoringKernelValueBatch values;
  values.double_values = {input.lhs_values.front() - input.rhs_values.front()};
  return values;
}

exec::ScoringKernelExecutionRequest ScoringRequest(bool over_quota) {
  exec::ScoringKernelExecutionRequest request;
  request.descriptor.kind = gpu::ScoringKernelKind::kVectorDistance;
  request.descriptor.descriptor_id = "odf106.scoring";
  request.descriptor.kernel_digest = "digest";
  request.descriptor.descriptor_generation = 1;
  request.descriptor.expected_descriptor_generation = 1;
  request.input.batch_id = "odf106.batch";
  request.input.row_count = 1;
  request.input.materialized_memory_bytes = 16;
  request.input.lhs_values = {7.0};
  request.input.rhs_values = {3.0};
  request.scalar_reference = ScalarScoring;
  request.provider.manifest.provider_id = "odf106.provider";
  request.provider.manifest.engine_abi_id = "sb_engine_abi_v3";
  request.provider.manifest.supported_kinds = {gpu::ScoringKernelKind::kVectorDistance};
  request.provider.run = [](const exec::ScoringKernelExecutionRequest& req) {
    gpu::ScoringKernelProviderOutcome outcome;
    outcome.ok = true;
    outcome.kernel_id = "odf106.kernel";
    outcome.descriptor_generation = req.descriptor.descriptor_generation;
    outcome.runtime_identity_id = req.provider.manifest.runtime_identity_id;
    outcome.values = ScalarScoring(req.input);
    return outcome;
  };
  request.resource_governance =
      Governance(agents::ResourceGovernanceFamily::kScoringKernelAccelerator,
                 agents::ResourceGovernanceAction::kExactScalarFallback);
  if (over_quota) {
    request.resource_governance.requested.device_memory_bytes =
        request.resource_governance.descriptor.limits.device_memory_bytes + 1;
  }
  return request;
}

void ScoringKernelUsesAdmissionResult() {
  const auto accelerated = exec::ExecuteOptionalScoringKernel(ScoringRequest(false));
  Require(accelerated.ok && accelerated.accelerator_used,
          "ODF-106 admitted scoring kernel did not execute accelerator");
  Require(EvidenceHas(accelerated.evidence,
                      "scoring_kernel.resource_governance_action=admit"),
          "ODF-106 scoring route did not consume admission");

  const auto fallback = exec::ExecuteOptionalScoringKernel(ScoringRequest(true));
  Require(fallback.ok && fallback.scalar_fallback_used,
          "ODF-106 scoring over-quota did not use exact scalar fallback");
  Require(fallback.diagnostic_code ==
              "SB_SCORING_KERNEL.ODF106_EXACT_SCALAR_FALLBACK",
          "ODF-106 scoring fallback diagnostic changed");
}

native::NativeSblrValueBatch ScalarNative(
    const native::NativeSblrInputBatch& input) {
  native::NativeSblrValueBatch values;
  values.values = input.values;
  return values;
}

native::NativeSblrSpecializationRequest NativeRequest(bool over_quota) {
  native::NativeSblrSpecializationRequest request;
  request.kind = native::NativeSblrSpecializationKind::kProjection;
  request.identity.stable_template_id = "odf106.template";
  request.identity.sblr_digest = "odf106.digest";
  request.identity.template_generation = 1;
  request.identity.expected_template_generation = 1;
  request.epochs.security_epoch = 1;
  request.epochs.expected_security_epoch = 1;
  request.epochs.redaction_epoch = 1;
  request.epochs.expected_redaction_epoch = 1;
  request.hotness.observed_invocations = 10;
  request.hotness.minimum_invocations = 1;
  request.input.values = {1, 2};
  request.input.row_count = 2;
  request.scalar_reference = ScalarNative;
  request.provider.manifest.provider_id = "odf106.native.provider";
  request.provider.manifest.engine_abi_id = "sb_engine_abi_v3";
  request.provider.manifest.supported_kinds = {
      native::NativeSblrSpecializationKind::kProjection};
  request.provider.compile = [](const native::NativeSblrSpecializationRequest&) {
    native::NativeSblrCompileOutcome outcome;
    outcome.ok = true;
    outcome.kernel_id = "odf106.native.kernel";
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
        outcome.values = ScalarNative(req.input);
        return outcome;
      };
  request.resource_governance =
      Governance(agents::ResourceGovernanceFamily::kPreparedNativeSpecialization,
                 agents::ResourceGovernanceAction::kExactScalarFallback);
  if (over_quota) {
    request.resource_governance.requested.pinned_memory_bytes =
        request.resource_governance.descriptor.limits.pinned_memory_bytes + 1;
  }
  return request;
}

void NativeSpecializationUsesAdmissionResult() {
  const auto native_result =
      sblr::ExecuteSblrNativeSpecialization(NativeRequest(false));
  Require(native_result.ok && native_result.native_used,
          "ODF-106 admitted native route did not execute");
  Require(EvidenceHas(native_result.evidence,
                      "native_sblr.resource_governance_action=admit"),
          "ODF-106 native route did not consume admission");

  const auto fallback =
      sblr::ExecuteSblrNativeSpecialization(NativeRequest(true));
  Require(fallback.ok && fallback.scalar_fallback_used,
          "ODF-106 native over-quota did not use exact scalar fallback");
  Require(fallback.diagnostic_code ==
              "SB_NATIVE_SBLR.ODF106_EXACT_SCALAR_FALLBACK",
          "ODF-106 native fallback diagnostic changed");
}

void CancellationAndServerRoute() {
  auto cancelled = Governance(agents::ResourceGovernanceFamily::kBulkCopyLane,
                              agents::ResourceGovernanceAction::kFailClosed);
  cancelled.cancellation_requested = true;
  RequireAdmissionAction(agents::AdmitResourceGovernance(cancelled),
                         agents::ResourceGovernanceAction::kCancel,
                         "SB_RESOURCE_GOVERNANCE.CANCELLED");

  server::ServerResourceGovernanceContext context;
  context.engine_runtime_bound = true;
  context.security_context_present = true;
  auto server_request =
      Governance(agents::ResourceGovernanceFamily::kAcceleratorProviderCache,
                 agents::ResourceGovernanceAction::kFailClosed);
  const auto admitted =
      server::AdmitServerResourceGovernance(context, server_request);
  Require(admitted.ok, "ODF-106 server admission route refused healthy request");
  Require(EvidenceHas(admitted.evidence, "server.resource_governance.route=odf106"),
          "ODF-106 server route evidence missing");

  context.parser_or_client_authority = true;
  const auto refused =
      server::AdmitServerResourceGovernance(context, server_request);
  Require(!refused.ok && refused.fail_closed &&
              refused.diagnostic_code ==
                  "SB_SERVER_RESOURCE_GOVERNANCE.CONTEXT_REFUSED",
          "ODF-106 server parser/client authority was accepted");
}

}  // namespace

int main() {
  SuccessfulAdmissionForEveryFamily();
  DescriptorRefusalsAreExact();
  NoQuotaDimensionHasUnlimitedDefault();
  AsyncPageIoUsesAdmissionResult();
  ParallelPipelineUsesAdmissionResult();
  ScoringKernelUsesAdmissionResult();
  NativeSpecializationUsesAdmissionResult();
  CancellationAndServerRoute();
  std::cout << "optimizer_deficiency_odf_106_gate passed\n";
  return EXIT_SUCCESS;
}
