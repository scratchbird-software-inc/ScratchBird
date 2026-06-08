// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cloud/cloud_deployment_profile.hpp"
#include "cloud/cloud_identity_kms.hpp"
#include "cloud/cloud_provider_capability.hpp"
#include "cloud/edge_cache_cdn.hpp"
#include "extensibility/gpu_api.hpp"
#include "extensibility/llvm_api.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasApiDiagnostic(const api::EngineApiResult& result,
                      std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

template <typename TResult>
bool HasCloudDiagnostic(const TResult& result, std::string_view code) {
  for (const auto& diagnostic : result.cloud_diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDiagnostic(const api::EngineEdgeCacheCdnResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool HasEvidenceKind(const api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) return true;
  }
  return false;
}

void PrintApiDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

std::string FieldValue(const api::EngineRowValue& row, std::string_view field_name) {
  for (const auto& [name, value] : row.fields) {
    if (name == field_name) return value.encoded_value;
  }
  return {};
}

bool ResultHasFieldValue(const api::EngineApiResult& result,
                         std::string_view field_name,
                         std::string_view expected_value) {
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, field_name) == expected_value) return true;
  }
  return false;
}

std::string ProjectionFieldValue(const api::CloudProviderCapabilityProjectionRow& row,
                                 std::string_view field_name) {
  for (const auto& [name, value] : row.fields) {
    if (name == field_name) return value;
  }
  return {};
}

api::EngineRequestContext SecurityContext() {
  api::EngineRequestContext context;
  context.request_id = "sbsql-non-core-optional-provider-classification";
  context.database_path = "/tmp/sbsql_non_core_optional_provider_classification.sbdb";
  context.security_context_present = true;
  context.database_uuid.canonical = "019f0000-0000-7000-8000-000000120001";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000120002";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000120003";
  context.transaction_uuid.canonical = "019f0000-0000-7000-8000-000000120004";
  context.local_transaction_id = 120;
  context.security_epoch = 12;
  context.resource_epoch = 13;
  context.trace_tags.push_back("right:OPTIONAL_PROVIDER_INSPECT");
  return context;
}

api::EngineExternalEffectCommitEvidence CommitEvidence() {
  api::EngineExternalEffectCommitEvidence evidence;
  evidence.transaction_uuid = "019f0000-0000-7000-8000-000000120101";
  evidence.local_transaction_id = 1201;
  evidence.transaction_inventory_generation = 7;
  evidence.commit_evidence_hash = "hash:v1:commit-evidence";
  evidence.finality_mode = "local_final";
  evidence.mga_commit_visible = true;
  evidence.durable_commit_evidence = true;
  return evidence;
}

void RequireCloudProviderCapabilityGates() {
  auto registry = api::MakeCloudProviderCapabilityRegistryWithLocalEmulator();

  api::CloudProviderCapabilityProjectionContext projection_context;
  projection_context.build_scope = api::CloudBuildScope::public_single_node;
  projection_context.include_private_cluster_fields = true;
  const auto refused_projection =
      api::BuildCloudProviderCapabilitySysInformationProjection(registry,
                                                                projection_context);
  Require(!refused_projection.ok,
          "public sys.information projection exposed private cluster cloud fields");
  Require(HasCloudDiagnostic(refused_projection,
                             api::kCloudProviderDiagnosticClusterFieldRefused),
          "cloud projection cluster-field refusal diagnostic drifted");

  projection_context.include_private_cluster_fields = false;
  const auto projection =
      api::BuildCloudProviderCapabilitySysInformationProjection(registry,
                                                                projection_context);
  Require(projection.ok, "public cloud provider projection refused local emulator");
  Require(!projection.rows.empty(), "cloud provider projection omitted local emulator row");
  const auto route_modes = ProjectionFieldValue(projection.rows.front(), "route_modes");
  Require(!Contains(route_modes, "cluster_route_refused"),
          "public cloud projection leaked private cluster route mode");
  const auto diagnostic_policy =
      ProjectionFieldValue(projection.rows.front(), "diagnostic_policy");
  Require(Contains(diagnostic_policy, api::kCloudProviderDiagnosticClusterFieldRefused),
          "cloud projection did not expose cluster-field refusal policy");

  auto authority_claim = api::MakeLocalEmulatorCloudProviderCapabilityProfile();
  authority_claim.provider_profile_uuid = "019f0000-0000-7000-8000-000000120102";
  authority_claim.provider_state_finality_authority = true;
  authority_claim.provider_recovery_authority = true;
  const auto authority_result =
      api::ValidateCloudProviderCapabilityProfile(authority_claim,
                                                  api::CloudBuildScope::public_single_node);
  Require(!authority_result.ok, "cloud provider authority claim was accepted");
  Require(HasCloudDiagnostic(authority_result,
                             api::kCloudProviderDiagnosticCapabilityUnsupported),
          "cloud provider authority-boundary diagnostic drifted");
  Require(!authority_result.side_effects_performed,
          "cloud provider validation performed side effects");
}

void RequireCloudDeploymentGates() {
  auto registry = api::MakeCloudProviderCapabilityRegistryWithLocalEmulator();
  const auto provider_uuid =
      registry.profiles.front().provider_profile_uuid;

  const auto accepted =
      api::ValidateCloudDeploymentProfile(
          registry,
          api::MakeLocalEmulatorCloudDeploymentProfile(provider_uuid));
  Require(accepted.ok, "local emulator deployment profile was refused");
  Require(accepted.mga_authority_preserved,
          "cloud deployment validation did not preserve MGA authority");
  Require(!accepted.provider_state_finality_authority &&
              !accepted.provider_recovery_authority &&
              !accepted.parser_finality_authority,
          "cloud deployment validation granted provider or parser finality authority");

  auto refused = api::MakeLocalEmulatorCloudDeploymentProfile(provider_uuid);
  refused.deployment_kind =
      api::CloudDeploymentKind::managed_scratchbird_cloud_serverless_edge;
  refused.private_cluster_only_fields.push_back("cluster.fence_token");
  refused.requested_route_modes.push_back(api::CloudRouteMode::cluster_route_refused);
  refused.requested_kms_modes.push_back(api::CloudKmsMode::managed_hsm);
  const auto refused_result =
      api::ValidateCloudDeploymentProfile(registry, refused);
  Require(!refused_result.ok,
          "cloud deployment accepted non-core cluster/serverless/provider route");
  Require(HasCloudDiagnostic(refused_result,
                             api::kCloudProviderDiagnosticClusterFieldRefused),
          "cloud deployment cluster-field diagnostic drifted");
  Require(HasCloudDiagnostic(refused_result,
                             api::kCloudProviderDiagnosticServerlessProfileRefused),
          "cloud deployment serverless refusal diagnostic drifted");
  Require(HasCloudDiagnostic(refused_result,
                             api::kCloudProviderDiagnosticKmsModeUnsupported),
          "cloud deployment KMS optional-provider diagnostic drifted");
  Require(!refused_result.side_effects_performed,
          "cloud deployment refusal performed side effects");
}

void RequireCloudIdentityKmsGates() {
  api::EngineApiRequest plaintext;
  plaintext.operation_id = "cloud.identity_kms.validate_policy";
  plaintext.option_envelopes = {
      "plaintext:forbidden-secret",
      "identity_mode:local_emulator_identity",
      "kms_mode:local_kms_emulator",
  };
  const auto plaintext_result = api::ValidateCloudIdentityKmsPolicyApi(plaintext);
  Require(!plaintext_result.ok, "cloud identity/KMS accepted plaintext material");
  Require(HasApiDiagnostic(plaintext_result,
                           "SB_DIAG_CLOUD_PLAINTEXT_MATERIAL_FORBIDDEN"),
          "cloud identity/KMS plaintext diagnostic drifted");
  Require(HasEvidenceKind(plaintext_result, "cloud_identity_kms_denial_audit"),
          "cloud identity/KMS refusal omitted denial audit evidence");

  api::EngineApiRequest local;
  local.operation_id = "cloud.identity_kms.validate_policy";
  local.option_envelopes = {
      "identity_mode:local_emulator_identity",
      "local_emulator_fixture:true",
      "identity_emulator_evidence:verified",
      "kms_mode:local_kms_emulator",
      "kms_emulator_evidence:verified",
      "kms_profile_uuid:019f0000-0000-7000-8000-000000120201",
      "rotation_policy_uuid:019f0000-0000-7000-8000-000000120202",
      "audit_policy_uuid:019f0000-0000-7000-8000-000000120203",
      "emulator_key_ref:local-emulator-key-v1",
      "protected_material_uuid:019f0000-0000-7000-8000-000000120204",
      "protected_material_version_uuid:019f0000-0000-7000-8000-000000120205",
  };
  const auto local_result = api::ValidateCloudIdentityKmsPolicyApi(local);
  Require(local_result.ok, "cloud identity/KMS refused local emulator policy");
  Require(HasEvidenceKind(local_result, "cloud_identity_kms_policy_validated"),
          "cloud identity/KMS success omitted policy validation evidence");
  Require(HasEvidenceKind(local_result, "cloud_local_emulator_fixture"),
          "cloud identity/KMS success omitted local emulator fixture evidence");
  Require(ResultHasFieldValue(local_result, "plaintext_material_persisted", "false") &&
              ResultHasFieldValue(local_result, "plaintext_material_returned", "false"),
          "cloud identity/KMS did not prove plaintext material is not persisted or returned");
  Require(ResultHasFieldValue(local_result,
                              "transaction_finality_authority",
                              "scratchbird_mga_not_kms"),
          "cloud identity/KMS result claimed finality authority outside MGA");
}

void RequireEdgeCacheCdnGates() {
  api::ResetEdgeCacheCdnStateForTests();

  api::EngineEdgeProviderProfile unsupported;
  unsupported.provider_profile_uuid = "019f0000-0000-7000-8000-000000120301";
  unsupported.provider_family = "managed_edge";
  unsupported.signature_key_ref = "edge-signing-key";
  const auto unsupported_provider = api::RegisterEdgeProviderProfile(unsupported);
  Require(!unsupported_provider.ok,
          "edge/CDN accepted unsupported provider family");
  Require(HasDiagnostic(unsupported_provider, "SB-EDGE-PROVIDER-UNSUPPORTED"),
          "edge/CDN unsupported-provider diagnostic drifted");

  api::EngineEdgeProviderProfile provider;
  provider.provider_profile_uuid = "019f0000-0000-7000-8000-000000120302";
  provider.provider_family = "local_signed_stream";
  provider.provider_name = "local_signed_stream";
  provider.signature_key_ref = "edge-signing-key-ref";
  const auto registered = api::RegisterEdgeProviderProfile(provider);
  Require(registered.ok, "edge/CDN refused local signed-stream provider");

  api::EngineEdgeCacheTagRegistration tag;
  tag.cache_tag_descriptor_uuid = "019f0000-0000-7000-8000-000000120303";
  tag.cache_tag_id = "tenant.page.alpha";
  tag.tag_class = "database_derived_page";
  tag.dependency_scope = "table";
  tag.internal_dependency_ref = "table:orders";
  tag.redaction_policy_uuid = "redaction-policy-safe";
  tag.finality_mode = "local_final";
  tag.external_provider_profile_uuid = provider.provider_profile_uuid;
  const auto precommit_tag = api::RegisterEdgeCacheTag(tag);
  Require(!precommit_tag.ok,
          "edge/CDN accepted cache tag without MGA commit evidence");
  Require(HasDiagnostic(precommit_tag, "SB-EDGE-PRECOMMIT-INVALIDATION-REFUSED"),
          "edge/CDN precommit tag diagnostic drifted");

  tag.creation_commit_evidence = CommitEvidence();
  const auto registered_tag = api::RegisterEdgeCacheTag(tag);
  Require(registered_tag.ok && registered_tag.tag_registered,
          "edge/CDN refused committed cache tag registration");
  Require(registered_tag.tag.emitted_only_after_commit_evidence,
          "edge/CDN tag did not retain after-commit evidence");
  Require(!Contains(registered_tag.tag.redacted_dependency_ref, "orders"),
          "edge/CDN tag exposed unredacted internal dependency text");

  api::EngineEdgeInvalidationRequest invalidation;
  invalidation.provider_profile_uuid = provider.provider_profile_uuid;
  invalidation.cache_tag_ids.push_back(tag.cache_tag_id);
  invalidation.finality_mode = "local_final";
  invalidation.redaction_policy_uuid = tag.redaction_policy_uuid;
  invalidation.source_object_ref = "table:orders";
  invalidation.now_epoch_ms = 1779811203000;
  const auto precommit_invalidation =
      api::QueueEdgeCacheInvalidationAfterCommit(invalidation);
  Require(!precommit_invalidation.ok,
          "edge/CDN accepted invalidation without MGA commit evidence");
  Require(HasDiagnostic(precommit_invalidation,
                        "SB-EDGE-PRECOMMIT-INVALIDATION-REFUSED"),
          "edge/CDN precommit invalidation diagnostic drifted");

  invalidation.commit_evidence = CommitEvidence();
  const auto queued = api::QueueEdgeCacheInvalidationAfterCommit(invalidation);
  Require(queued.ok && queued.invalidation_queued,
          "edge/CDN refused committed invalidation queue entry");
  Require(queued.invalidation.emitted_after_commit_evidence,
          "edge/CDN invalidation did not retain after-commit evidence");
  Require(queued.invalidation.payload_redacted,
          "edge/CDN invalidation payload was not marked redacted");

  api::ResetEdgeCacheCdnStateForTests();
}

void RequireGpuOptionalProviderGates() {
  api::EngineInspectGpuCapabilityRequest missing_security;
  missing_security.option_envelopes.push_back("enable_gpu_execution");
  const auto missing_security_result =
      api::EngineInspectGpuCapability(missing_security);
  Require(!missing_security_result.ok,
          "GPU optional provider accepted execution without security context");
  Require(HasApiDiagnostic(missing_security_result,
                           "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "GPU security-context diagnostic drifted");

  api::EngineInspectGpuCapabilityRequest inspect;
  inspect.context = SecurityContext();
  inspect.option_envelopes = {
      "simulate_gpu_provider:gpu.simulated",
      "workload:inspect",
      "gpu_profile:inspect_only",
  };
  const auto inspected = api::EngineInspectGpuCapability(inspect);
  Require(inspected.ok, "GPU optional provider inspect route refused simulated provider");
  Require(HasEvidence(inspected, "gpu_capability", "inspected"),
          "GPU inspect route omitted capability evidence");
  Require(HasEvidence(inspected,
                      "execution_boundary",
                      "gpu_never_transaction_security_visibility_authority"),
          "GPU inspect route did not preserve authority boundary evidence");

  api::EngineInspectGpuCapabilityRequest bypass;
  bypass.context = SecurityContext();
  bypass.option_envelopes = {
      "simulate_gpu_provider:gpu.simulated",
      "enable_gpu_execution",
      "gpu_authority",
      "workload:vector_dot",
  };
  const auto bypass_result = api::EngineInspectGpuCapability(bypass);
  Require(!bypass_result.ok, "GPU optional provider accepted authority bypass");
  Require(HasApiDiagnostic(bypass_result,
                           "SB_ENGINE_API_GPU_AUTHORITY_BYPASS_REFUSED"),
          "GPU authority-bypass diagnostic drifted");
}

void RequireLlvmOptionalProviderGates() {
  std::remove("/tmp/sbsql_non_core_optional_provider_classification.sbdb.sb.api_events");

  api::EngineCompileLlvmModuleRequest raw_sql;
  raw_sql.context = SecurityContext();
  raw_sql.option_envelopes = {
      "compile:jit",
      "module:SELECT 1",
      "sblr_fragment",
      "cache_key:cbq012-raw-sql",
  };
  const auto raw_sql_result = api::EngineCompileLlvmModule(raw_sql);
  Require(!raw_sql_result.ok, "LLVM optional provider accepted raw SQL text");
  Require(HasApiDiagnostic(raw_sql_result, "SB_ENGINE_API_LLVM_RAW_SQL_REFUSED"),
          "LLVM raw-SQL diagnostic drifted");

  api::EngineCompileLlvmModuleRequest bypass;
  bypass.context = SecurityContext();
  bypass.option_envelopes = {
      "compile:jit",
      "module:sblr_projection_unit",
      "sblr_fragment",
      "cache_key:cbq012-bypass",
      "bypass_mga",
  };
  const auto bypass_result = api::EngineCompileLlvmModule(bypass);
  Require(!bypass_result.ok, "LLVM optional provider accepted authority bypass");
  Require(HasApiDiagnostic(bypass_result,
                           "SB_ENGINE_API_LLVM_AUTHORITY_BYPASS_REFUSED"),
          "LLVM authority-bypass diagnostic drifted");

  api::EngineCompileLlvmModuleRequest fallback;
  fallback.context = SecurityContext();
  fallback.context.local_transaction_id = 0;
  fallback.context.transaction_uuid.canonical.clear();
  fallback.option_envelopes = {
      "compile:jit",
      "module:sblr_projection_unit",
      "sblr_fragment",
      "cache_key:cbq012-llvm-fallback",
      "allow_interpreter_fallback",
      "simulate_llvm_unavailable",
      "llvm_test_fixture",
  };
  const auto fallback_result = api::EngineCompileLlvmModule(fallback);
  if (!fallback_result.ok) PrintApiDiagnostics(fallback_result);
  Require(fallback_result.ok, "LLVM optional provider refused interpreter fallback route");
  Require(HasEvidence(fallback_result, "llvm_compile_runtime", "interpreter"),
          "LLVM fallback route did not expose interpreter runtime evidence");
  Require(HasEvidence(fallback_result, "execution_boundary", "sblr_only_engine_authority"),
          "LLVM fallback route did not preserve SBLR-only authority evidence");

  std::remove("/tmp/sbsql_non_core_optional_provider_classification.sbdb.sb.api_events");
}

}  // namespace

int main() {
  RequireCloudProviderCapabilityGates();
  RequireCloudDeploymentGates();
  RequireCloudIdentityKmsGates();
  RequireEdgeCacheCdnGates();
  RequireGpuOptionalProviderGates();
  RequireLlvmOptionalProviderGates();
  std::cout << "sbsql_non_core_optional_provider_classification_conformance=passed\n";
  return EXIT_SUCCESS;
}
