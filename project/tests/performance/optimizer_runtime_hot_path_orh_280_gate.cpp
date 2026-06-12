// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_hot_path_execution.hpp"

#include "resource_governance_admission.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace api = scratchbird::engine::internal_api;
namespace native = scratchbird::engine::native_compile;
namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-280 gate failure: " << message << '\n';
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

std::string HashValues(const std::vector<std::int64_t>& values) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto value : values) {
    const auto text = std::to_string(value);
    for (const unsigned char ch : text) {
      hash ^= ch;
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << "fnv1a64:" << std::hex << hash;
  return out.str();
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.request_id = "orh280-sblr-hot-path";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000280001";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-000000280002";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000280003";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000280004";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000280005";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-000000280006";
  context.local_transaction_id = 280;
  context.snapshot_visible_through_local_transaction_id = 279;
  context.transaction_isolation_level = "snapshot";
  context.catalog_generation_id = 2800;
  context.security_epoch = 2801;
  context.resource_epoch = 2802;
  context.name_resolution_epoch = 2803;
  context.security_context_present = true;
  context.trace_tags = {"ORH-280", "ORH-GATE-280",
                        "mga_transaction_regression"};
  return context;
}

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7000-8000-000000280100";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor = "type=int64";
  return descriptor;
}

sblr::SblrOperationEnvelope Envelope() {
  auto envelope = sblr::MakeSblrEnvelope("query.plan_operation",
                                         "SBLR_QUERY_PLAN_OPERATION",
                                         "trace.orh280.hot_path");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.source_artifact_map.policy_status =
      "non_authoritative_render_metadata";
  envelope.source_artifact_map.source_identity =
      "sblr-envelope:orh280:projection";
  envelope.source_artifact_map.source_hash = "sha256:orh280-sblr-envelope";
  envelope.source_artifact_map.render_metadata_only = true;
  envelope.source_artifact_map.contains_sql_text = false;
  envelope.source_artifact_map.raw_sql_text_authoritative = false;
  envelope.operands.push_back({"predicate_slot", "predicate:tenant", "tenant"});
  envelope.operands.push_back({"parameter_slot", "param:tenant", "tenant"});
  return envelope;
}

api::EngineApiRequest ApiRequest(const api::EngineRequestContext& context) {
  api::EngineApiRequest request;
  request.context = context;
  request.operation_id = "query.plan_operation";
  request.target_object.uuid.canonical =
      "019f0000-0000-7000-8000-000000280200";
  request.target_object.object_kind = "table";
  request.descriptors.push_back(Descriptor());
  api::EngineTypedValue parameter;
  parameter.descriptor = Descriptor();
  parameter.encoded_value = "7";
  request.predicate.predicate_kind = "eq";
  request.predicate.canonical_predicate_envelope = "tenant";
  request.predicate.bound_values.push_back(parameter);
  request.policy_profile.names = {"role_authorization", "tenant_visibility"};
  request.policy_profile.encoded_profiles = {"policy=orh280"};
  return request;
}

agents::ResourceGovernanceQuotaVector Quotas(std::int64_t value) {
  agents::ResourceGovernanceQuotaVector quotas;
  quotas.memory_bytes = value;
  quotas.device_memory_bytes = value;
  quotas.pinned_memory_bytes = value;
  quotas.io_bytes = value;
  quotas.io_ops = value;
  quotas.worker_threads = value;
  quotas.backlog_items = value;
  quotas.candidate_rows = value;
  quotas.cache_entries = value;
  quotas.batch_rows = value;
  quotas.fragments = value;
  quotas.lanes = value;
  quotas.time_budget_microseconds = value;
  return quotas;
}

agents::ResourceGovernanceAdmissionRequest Governance() {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id = "orh280.sblr.native_specialization";
  request.expected_family =
      agents::ResourceGovernanceFamily::kPreparedNativeSpecialization;
  request.descriptor.descriptor_id = "runtime.sblr.orh280.native";
  request.descriptor.family =
      agents::ResourceGovernanceFamily::kPreparedNativeSpecialization;
  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label = "runtime_policy:orh280";
  request.descriptor.descriptor_generation = 280;
  request.descriptor.expected_generation = 280;
  request.descriptor.limits = Quotas(1000000);
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.descriptor.over_limit_action =
      agents::ResourceGovernanceAction::kExactScalarFallback;
  request.requested = Quotas(2);
  request.requested.memory_bytes = 4096;
  request.requested.batch_rows = 4;
  request.requested.time_budget_microseconds = 100;
  request.require_exact_scalar_fallback_available = true;
  request.exact_scalar_fallback_available = true;
  return request;
}

native::NativeSblrProvider Provider(bool mismatch = false) {
  native::NativeSblrProvider provider;
  provider.manifest.provider_id = "orh280.native.provider";
  provider.manifest.engine_abi_id = "sb_engine_abi_v3";
  provider.manifest.supported_kinds = {
      native::NativeSblrSpecializationKind::kProjection};
  provider.manifest.safe_to_execute = true;
  provider.compile = [](const native::NativeSblrSpecializationRequest&) {
    native::NativeSblrCompileOutcome outcome;
    outcome.ok = true;
    outcome.kernel_id = "orh280.kernel.projection";
    return outcome;
  };
  provider.run = [mismatch](const native::NativeSblrSpecializationRequest& request,
                            const native::NativeSblrCompileOutcome& compile) {
    native::NativeSblrKernelOutcome outcome;
    outcome.ok = true;
    outcome.kernel_id = compile.kernel_id;
    outcome.template_generation = request.identity.template_generation;
    outcome.security_epoch = request.epochs.security_epoch;
    outcome.redaction_epoch = request.epochs.redaction_epoch;
    for (const auto value : request.input.values) {
      outcome.values.values.push_back(value * 2 + (mismatch ? 1 : 0));
    }
    return outcome;
  };
  return provider;
}

sblr::SblrNativeSpecializationRequest NativeRequest(bool mismatch = false) {
  sblr::SblrNativeSpecializationRequest request;
  request.kind = native::NativeSblrSpecializationKind::kProjection;
  request.identity.plan_node_id = "orh280.plan.projection";
  request.hotness.observed_invocations = 64;
  request.hotness.minimum_invocations = 2;
  request.hotness.observed_rows = 4;
  request.hotness.minimum_rows = 4;
  request.input.values = {1, 2, 3, 4};
  request.input.row_count = request.input.values.size();
  request.scalar_reference = [](const native::NativeSblrInputBatch& input) {
    native::NativeSblrValueBatch values;
    for (const auto value : input.values) {
      values.values.push_back(value * 2);
    }
    return values;
  };
  request.provider = Provider(mismatch);
  request.resource_governance = Governance();
  return request;
}

sblr::SblrHotPathExecutionRequest Request(
    scratchbird::engine::executor::PreparedTemplateCache* cache) {
  const auto context = Context();
  sblr::SblrHotPathExecutionRequest request;
  request.route_label = "orh280.sblr.hot_path.projection_batch";
  request.envelope = Envelope();
  request.context = context;
  request.api_request = ApiRequest(context);
  request.template_cache = cache;
  request.native_specialization = NativeRequest();
  request.superinstruction.fused_opcodes = {"SBLR_LOAD_SLOT",
                                            "SBLR_APPLY_PROJECTION",
                                            "SBLR_EMIT_ROW"};
  request.superinstruction.superinstruction_id =
      "super.orh280.load_project_emit";
  request.superinstruction.available = true;
  request.superinstruction.safe = true;
  request.superinstruction.exact_scalar_fallback_available = true;
  request.superinstruction.scalar_dispatches = 12;
  request.superinstruction.fused_dispatches = 3;
  request.batch.repeated_rows = 4;
  request.batch.scalar_dispatches_per_row = 3;
  request.batch.batched_dispatches_total = 4;
  request.batch.row_ordering_preserved = true;
  request.batch.result_contract_hash_matches = true;
  request.batch.expected_result_contract_hash = HashValues({2, 4, 6, 8});
  request.batch.observed_result_contract_hash =
      request.batch.expected_result_contract_hash;
  request.profiler.source_label = "engine_internal_sblr_counter";
  request.profiler.measured = true;
  request.profiler.sample_count = 7;
  request.profiler.baseline_dispatch_us = 840;
  request.profiler.optimized_dispatch_us = 210;
  request.authority.engine_mga_snapshot_bound = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_recheck_required = true;
  return request;
}

void RequireAccepted(const sblr::SblrHotPathExecutionResult& result) {
  Require(result.ok && result.benchmark_clean,
          "SBLR hot path was not benchmark-clean");
  Require(!result.fallback_used && !result.fail_closed,
          "SBLR hot path unexpectedly fell back/refused");
  Require(result.first_prepare.ok && result.reused_prepare.ok &&
              result.reused_prepare.reused_existing_template,
          "prepared template was not built and reused");
  Require(result.bind.ok, "prepared template did not bind");
  Require(result.specialization.ok && result.specialization.native_used,
          "opcode specialization did not use native route");
  Require(result.dispatch_us_saved > 0 && result.opcode_dispatches_saved > 0,
          "dispatch overhead was not reduced");
  Require(HasEvidence(result.evidence,
                      "sblr_hot_path.execution_authority=engine_sblr_internal_envelope"),
          "SBLR engine authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "sblr_hot_path.parser_execution_authority=false"),
          "parser non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "sblr_hot_path.mga_finality_authority=engine_transaction_inventory"),
          "MGA authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "sblr_hot_path.security_recheck_required=true"),
          "security recheck evidence missing");
  Require(HasEvidence(result.evidence,
                      "sblr_hot_path.prepared_template_reused=true"),
          "prepared template reuse evidence missing");
  Require(HasEvidence(result.evidence,
                      "sblr_hot_path.opcode_specialization=native"),
          "opcode specialization evidence missing");
  Require(HasEvidence(result.evidence,
                      "sblr_hot_path.superinstruction_id=super.orh280"),
          "superinstruction evidence missing");
  Require(HasEvidence(result.evidence,
                      "sblr_hot_path.batched_repeated_rows=4"),
          "batched repeated-row evidence missing");
  Require(HasEvidence(result.evidence,
                      "sblr_hot_path.profiler_source_label=engine_internal_sblr_counter"),
          "profiler evidence missing");
  Require(HasEvidence(result.evidence,
                      "native_sblr.scalar_equivalence=verified"),
          "native exact-equivalence evidence missing");
}

void RequireRejected(const sblr::SblrHotPathExecutionResult& result,
                     std::string_view diagnostic,
                     std::string_view label) {
  Require(!result.benchmark_clean,
          std::string(label) + " was marked benchmark-clean");
  Require(result.diagnostic_code.find(diagnostic) != std::string::npos,
          std::string(label) + " diagnostic mismatch: " +
              result.diagnostic_code);
}

void RequireFallback(const sblr::SblrHotPathExecutionResult& result,
                     std::string_view diagnostic,
                     std::string_view label) {
  Require(result.fallback_used && !result.benchmark_clean,
          std::string(label) + " did not take exact fallback");
  Require(result.diagnostic_code.find(diagnostic) != std::string::npos,
          std::string(label) + " fallback diagnostic mismatch: " +
              result.diagnostic_code);
  Require(HasEvidence(result.evidence,
                      "sblr_hot_path.exact_fallback_required=true"),
          std::string(label) + " missing exact fallback evidence");
}

void TestPositiveHotPath() {
  scratchbird::engine::executor::PreparedTemplateCache cache;
  RequireAccepted(sblr::ExecuteSblrHotPath(Request(&cache)));
}

void TestNegativeAuthorityAndEnvelopeCases() {
  scratchbird::engine::executor::PreparedTemplateCache cache;

  auto raw_sql = Request(&cache);
  raw_sql.envelope.contains_sql_text = true;
  RequireRejected(sblr::ExecuteSblrHotPath(raw_sql),
                  "ORH_SBLR_HOT_PATH_EXTERNAL_AUTHORITY_REFUSED",
                  "raw SQL parser authority");

  scratchbird::engine::executor::PreparedTemplateCache reference_cache;
  auto reference = Request(&reference_cache);
  reference.authority.reference_execution_authority = true;
  RequireRejected(sblr::ExecuteSblrHotPath(reference),
                  "ORH_SBLR_HOT_PATH_EXTERNAL_AUTHORITY_REFUSED",
                  "reference execution authority");

  scratchbird::engine::executor::PreparedTemplateCache client_cache;
  auto client = Request(&client_cache);
  client.authority.client_execution_authority = true;
  RequireRejected(sblr::ExecuteSblrHotPath(client),
                  "ORH_SBLR_HOT_PATH_EXTERNAL_AUTHORITY_REFUSED",
                  "client execution authority");

  scratchbird::engine::executor::PreparedTemplateCache missing_cache;
  auto missing = Request(&missing_cache);
  missing.envelope.operation_id.clear();
  RequireRejected(sblr::ExecuteSblrHotPath(missing),
                  "ORH_SBLR_HOT_PATH_ENVELOPE_PROOF_MISSING",
                  "missing SBLR envelope proof");
}

void TestNegativeFallbackAndSafetyCases() {
  scratchbird::engine::executor::PreparedTemplateCache stale_cache;
  auto stale = Request(&stale_cache);
  stale.native_specialization.identity.template_generation = 2800;
  stale.native_specialization.identity.expected_template_generation = 2801;
  RequireFallback(sblr::ExecuteSblrHotPath(stale),
                  "SB_NATIVE_SBLR.STALE_TEMPLATE_GENERATION_FALLBACK",
                  "stale prepared-template generation");

  scratchbird::engine::executor::PreparedTemplateCache unsupported_cache;
  auto unsupported = Request(&unsupported_cache);
  unsupported.native_specialization.provider.manifest.supported_kinds.clear();
  RequireFallback(sblr::ExecuteSblrHotPath(unsupported),
                  "SB_NATIVE_SBLR.UNSUPPORTED_KIND_FALLBACK",
                  "unsupported opcode specialization");

  scratchbird::engine::executor::PreparedTemplateCache mismatch_cache;
  auto mismatch = Request(&mismatch_cache);
  mismatch.native_specialization = NativeRequest(true);
  RequireRejected(sblr::ExecuteSblrHotPath(mismatch),
                  "SB_NATIVE_SBLR.RESULT_MISMATCH_REFUSED",
                  "opcode specialization result mismatch");

  scratchbird::engine::executor::PreparedTemplateCache batch_cache;
  auto batch = Request(&batch_cache);
  batch.batch.observed_result_contract_hash = "fnv1a64:wrong";
  batch.batch.result_contract_hash_matches = false;
  RequireRejected(sblr::ExecuteSblrHotPath(batch),
                  "ORH_SBLR_HOT_PATH_RESULT_CONTRACT_MISMATCH",
                  "batch ordering/result contract drift");

  scratchbird::engine::executor::PreparedTemplateCache super_cache;
  auto super = Request(&super_cache);
  super.superinstruction.safe = false;
  RequireFallback(sblr::ExecuteSblrHotPath(super),
                  "ORH_SBLR_HOT_PATH_UNSAFE_SUPERINSTRUCTION",
                  "unsafe superinstruction");

  scratchbird::engine::executor::PreparedTemplateCache fallback_cache;
  auto fallback = Request(&fallback_cache);
  fallback.superinstruction.exact_scalar_fallback_available = false;
  RequireRejected(sblr::ExecuteSblrHotPath(fallback),
                  "ORH_SBLR_HOT_PATH_EXACT_FALLBACK_UNPROVEN",
                  "fallback not exact");
}

void TestNegativeMgaSecurityProfilerCases() {
  scratchbird::engine::executor::PreparedTemplateCache mga_cache;
  auto mga = Request(&mga_cache);
  mga.authority.engine_mga_snapshot_bound = false;
  RequireRejected(sblr::ExecuteSblrHotPath(mga),
                  "ORH_SBLR_HOT_PATH_MGA_UNPROVEN",
                  "missing MGA evidence");

  scratchbird::engine::executor::PreparedTemplateCache security_cache;
  auto security = Request(&security_cache);
  security.authority.security_recheck_required = false;
  RequireRejected(sblr::ExecuteSblrHotPath(security),
                  "ORH_SBLR_HOT_PATH_SECURITY_UNPROVEN",
                  "missing security evidence");

  scratchbird::engine::executor::PreparedTemplateCache profiler_cache;
  auto profiler = Request(&profiler_cache);
  profiler.profiler.measured = false;
  profiler.profiler.source_label = "contract_only";
  RequireRejected(sblr::ExecuteSblrHotPath(profiler),
                  "ORH_SBLR_HOT_PATH_PROFILER_MISSING",
                  "profiler missing/contract-only evidence");
}

}  // namespace

int main() {
  TestPositiveHotPath();
  TestNegativeAuthorityAndEnvelopeCases();
  TestNegativeFallbackAndSafetyCases();
  TestNegativeMgaSecurityProfilerCases();
  std::cout << "ORH-280 SBLR hot path gate passed\n";
  return EXIT_SUCCESS;
}
