// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "security/security_principal_lifecycle.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifndef SCRATCHBIRD_PROJECT_SOURCE_DIR
#define SCRATCHBIRD_PROJECT_SOURCE_DIR "."
#endif

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

struct B001Row {
  std::string_view audit_id;
  std::string_view sql;
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view family;
  std::string_view result_shape;
  std::string_view target_ref;
  std::string_view target_ref_kind;
  std::string_view count;
};

constexpr std::array<B001Row, 50> kRows{{
    {"AUDIT-0523", "ALTER GPU ARTIFACT gpu_artifact_1 QUARANTINE", "op.gpu.artifact_quarantine", "SBLR_OP_GPU_ARTIFACT_QUARANTINE", "sblr.acceleration.gpu.v3", "rs.acceleration.control.v1", "gpu_artifact_1", "artifact", ""},
    {"AUDIT-0524", "ALTER GPU CACHE CLEAR", "op.gpu.cache_clear", "SBLR_OP_GPU_CACHE_CLEAR", "sblr.acceleration.gpu.v3", "rs.acceleration.control.v1", "", "", ""},
    {"AUDIT-0521", "ALTER GPU DEVICE device0 QUARANTINE", "op.gpu.device_quarantine", "SBLR_OP_GPU_DEVICE_QUARANTINE", "sblr.acceleration.gpu.v3", "rs.acceleration.control.v1", "device0", "device", ""},
    {"AUDIT-0522", "ALTER GPU KERNEL kernel_main QUARANTINE", "op.gpu.kernel_quarantine", "SBLR_OP_GPU_KERNEL_QUARANTINE", "sblr.acceleration.gpu.v3", "rs.acceleration.control.v1", "kernel_main", "kernel", ""},
    {"AUDIT-0520", "ALTER GPU PROFILE profile_fast DISABLE", "op.gpu.profile_disable", "SBLR_OP_GPU_PROFILE_DISABLE", "sblr.acceleration.gpu.v3", "rs.acceleration.control.v1", "profile_fast", "profile", ""},
    {"AUDIT-0519", "ALTER GPU PROFILE profile_fast ENABLE", "op.gpu.profile_enable", "SBLR_OP_GPU_PROFILE_ENABLE", "sblr.acceleration.gpu.v3", "rs.acceleration.control.v1", "profile_fast", "profile", ""},
    {"AUDIT-0469", "ALTER MANAGEMENT LISTENER native_listener DRAIN", "op.management.listener.drain", "SBLR_OP_MANAGEMENT_LISTENER_DRAIN", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "native_listener", "listener", ""},
    {"AUDIT-0470", "ALTER MANAGEMENT LISTENER native_listener UNDRAIN", "op.management.listener.undrain", "SBLR_OP_MANAGEMENT_LISTENER_UNDRAIN", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "native_listener", "listener", ""},
    {"AUDIT-0468", "ALTER MANAGEMENT MANAGER RESTART", "op.management.manager.restart", "SBLR_OP_MANAGEMENT_MANAGER_RESTART", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "", "", ""},
    {"AUDIT-0466", "ALTER MANAGEMENT MANAGER START", "op.management.manager.start", "SBLR_OP_MANAGEMENT_MANAGER_START", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "", "", ""},
    {"AUDIT-0467", "ALTER MANAGEMENT MANAGER STOP", "op.management.manager.stop", "SBLR_OP_MANAGEMENT_MANAGER_STOP", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "", "", ""},
    {"AUDIT-0471", "ALTER MANAGEMENT PARSER POOL parser_pool_1 RESIZE 8", "op.management.parser_pool.resize", "SBLR_OP_MANAGEMENT_PARSER_POOL_RESIZE", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "parser_pool_1", "pool", "8"},
    {"AUDIT-0518", "ALTER NATIVE COMPILE AOT REBUILD unit_main", "op.native_compile.aot_rebuild", "SBLR_OP_NATIVE_COMPILE_AOT_REBUILD", "sblr.acceleration.llvm.v3", "rs.acceleration.control.v1", "unit_main", "unit", ""},
    {"AUDIT-0517", "ALTER NATIVE COMPILE ARTIFACT native_artifact_1 QUARANTINE", "op.native_compile.artifact_quarantine", "SBLR_OP_NATIVE_COMPILE_ARTIFACT_QUARANTINE", "sblr.acceleration.llvm.v3", "rs.acceleration.control.v1", "native_artifact_1", "artifact", ""},
    {"AUDIT-0516", "ALTER NATIVE COMPILE CACHE INVALIDATE", "op.native_compile.cache_invalidate", "SBLR_OP_NATIVE_COMPILE_CACHE_INVALIDATE", "sblr.acceleration.llvm.v3", "rs.acceleration.control.v1", "", "", ""},
    {"AUDIT-0515", "ALTER NATIVE COMPILE PROFILE native_profile DISABLE", "op.native_compile.profile_disable", "SBLR_OP_NATIVE_COMPILE_PROFILE_DISABLE", "sblr.acceleration.llvm.v3", "rs.acceleration.control.v1", "native_profile", "profile", ""},
    {"AUDIT-0514", "ALTER NATIVE COMPILE PROFILE native_profile ENABLE", "op.native_compile.profile_enable", "SBLR_OP_NATIVE_COMPILE_PROFILE_ENABLE", "sblr.acceleration.llvm.v3", "rs.acceleration.control.v1", "native_profile", "profile", ""},
    {"AUDIT-0472", "CONFIG RELOAD", "op.management.config.reload", "SBLR_OP_MANAGEMENT_CONFIG_RELOAD", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "", "", ""},
    {"AUDIT-0476", "ALTER MANAGEMENT INSTRUCTION instr_42 ACK", "op.management.instruction.ack", "SBLR_OP_MANAGEMENT_INSTRUCTION_ACK", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "instr_42", "instruction", ""},
    {"AUDIT-0473", "ALTER MANAGEMENT INSTRUCTION instr_42 APPLY", "op.management.instruction.apply", "SBLR_OP_MANAGEMENT_INSTRUCTION_APPLY", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "instr_42", "instruction", ""},
    {"AUDIT-0474", "ALTER MANAGEMENT INSTRUCTION instr_42 CANCEL", "op.management.instruction.cancel", "SBLR_OP_MANAGEMENT_INSTRUCTION_CANCEL", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "instr_42", "instruction", ""},
    {"AUDIT-0475", "ALTER MANAGEMENT INSTRUCTION instr_42 QUARANTINE", "op.management.instruction.quarantine", "SBLR_OP_MANAGEMENT_INSTRUCTION_QUARANTINE", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "instr_42", "instruction", ""},
    {"AUDIT-0477", "SUPPORT BUNDLE CREATE", "op.management.support_bundle.create", "SBLR_OP_MANAGEMENT_SUPPORT_BUNDLE_CREATE", "sblr.management.runtime_operation.v3", "rs.acceleration.control.v1", "", "", ""},
    {"AUDIT-0448", "SHOW AOT ARTIFACTS", "op.show.aot_artifacts", "SBLR_OP_SHOW_AOT_ARTIFACTS", "sblr.acceleration.llvm.v3", "rs.show.native_compile.v1", "", "", ""},
    {"AUDIT-0489", "SHOW AUDIT", "op.show.audit", "SBLR_OP_SHOW_AUDIT", "sblr.catalog.introspect.v3", "rs.security.policy.v1", "", "", ""},
    {"AUDIT-0440", "SHOW BUFFER POOL", "op.show.buffer_pool", "SBLR_OP_SHOW_BUFFER_POOL", "sblr.observability.inspect.v3", "rs.show.buffer_pool.v1", "", "", ""},
    {"AUDIT-0439", "SHOW CACHE", "op.show.cache", "SBLR_OP_SHOW_CACHE", "sblr.observability.inspect.v3", "rs.show.cache.v1", "", "", ""},
    {"AUDIT-0424", "SHOW CAPABILITIES", "op.show.capabilities", "SBLR_OP_SHOW_CAPABILITIES", "sblr.observability.inspect.v3", "rs.show.capabilities.v1", "", "", ""},
    {"AUDIT-0428", "SHOW CONTEXT", "op.show.context", "SBLR_OP_SHOW_CONTEXT", "sblr.observability.inspect.v3", "rs.show.context.v1", "", "", ""},
    {"AUDIT-0427", "SHOW DIALECT", "op.show.dialect", "SBLR_OP_SHOW_DIALECT", "sblr.observability.inspect.v3", "rs.show.context.v1", "", "", ""},
    {"AUDIT-0484", "SHOW DISCOVERY RIGHTS FOR principal_admin", "op.show.discovery_rights", "SBLR_OP_SHOW_DISCOVERY_RIGHTS", "sblr.catalog.introspect.v3", "rs.security.grant.v1", "principal_admin", "principal", ""},
    {"AUDIT-0449", "SHOW GPU", "op.show.gpu", "SBLR_OP_SHOW_GPU", "sblr.acceleration.gpu.v3", "rs.show.gpu.v1", "", "", ""},
    {"AUDIT-0453", "SHOW GPU ARTIFACTS", "op.show.gpu_artifacts", "SBLR_OP_SHOW_GPU_ARTIFACTS", "sblr.acceleration.gpu.v3", "rs.show.native_compile.v1", "", "", ""},
    {"AUDIT-0450", "SHOW GPU CAPABILITY", "op.show.gpu_capability", "SBLR_OP_SHOW_GPU_CAPABILITY", "sblr.acceleration.gpu.v3", "rs.show.gpu.v1", "", "", ""},
    {"AUDIT-0451", "SHOW GPU DEVICES", "op.show.gpu_devices", "SBLR_OP_SHOW_GPU_DEVICES", "sblr.acceleration.gpu.v3", "rs.show.gpu.v1", "", "", ""},
    {"AUDIT-0454", "SHOW GPU KERNELS", "op.show.gpu_kernels", "SBLR_OP_SHOW_GPU_KERNELS", "sblr.acceleration.gpu.v3", "rs.show.native_compile.v1", "", "", ""},
    {"AUDIT-0452", "SHOW GPU MEMORY", "op.show.gpu_memory", "SBLR_OP_SHOW_GPU_MEMORY", "sblr.acceleration.gpu.v3", "rs.show.gpu.v1", "", "", ""},
    {"AUDIT-0482", "SHOW GRANTS FOR principal_admin", "op.show.grants", "SBLR_OP_SHOW_GRANTS", "sblr.catalog.introspect.v3", "rs.security.grant.v1", "principal_admin", "principal", ""},
    {"AUDIT-0480", "SHOW GROUPS", "op.show.groups", "SBLR_OP_SHOW_GROUPS", "sblr.catalog.introspect.v3", "rs.security.principal.v1", "", "", ""},
    {"AUDIT-0481", "SHOW IDENTITY PROVIDERS", "op.show.identity_providers", "SBLR_OP_SHOW_IDENTITY_PROVIDERS", "sblr.catalog.introspect.v3", "rs.security.principal.v1", "", "", ""},
    {"AUDIT-0442", "SHOW INDEX HEALTH FOR idx_order_date", "op.show.index_health", "SBLR_OP_SHOW_INDEX_HEALTH", "sblr.observability.inspect.v3", "rs.show.index_health.v1", "idx_order_date", "index", ""},
    {"AUDIT-0441", "SHOW IO", "op.show.io", "SBLR_OP_SHOW_IO", "sblr.observability.inspect.v3", "rs.show.io.v1", "", "", ""},
    {"AUDIT-0432", "SHOW JOB job_daily", "op.show.job", "SBLR_OP_SHOW_JOB", "sblr.observability.inspect.v3", "rs.show.jobs.v1", "job_daily", "job", ""},
    {"AUDIT-0434", "SHOW JOB DEPENDENCIES FOR job_daily", "op.show.job_dependencies", "SBLR_OP_SHOW_JOB_DEPENDENCIES", "sblr.observability.inspect.v3", "rs.show.jobs.v1", "job_daily", "job", ""},
    {"AUDIT-0433", "SHOW JOB RUNS FOR job_daily", "op.show.job_runs", "SBLR_OP_SHOW_JOB_RUNS", "sblr.observability.inspect.v3", "rs.show.job_runs.v1", "job_daily", "job", ""},
    {"AUDIT-0443", "SHOW LLVM", "op.show.llvm", "SBLR_OP_SHOW_LLVM", "sblr.acceleration.llvm.v3", "rs.show.llvm.v1", "", "", ""},
    {"AUDIT-0444", "SHOW LLVM PROVENANCE", "op.show.llvm_provenance", "SBLR_OP_SHOW_LLVM_PROVENANCE", "sblr.acceleration.llvm.v3", "rs.show.llvm.v1", "", "", ""},
    {"AUDIT-0445", "SHOW LLVM TARGETS", "op.show.llvm_targets", "SBLR_OP_SHOW_LLVM_TARGETS", "sblr.acceleration.llvm.v3", "rs.show.llvm.v1", "", "", ""},
    {"AUDIT-0460", "SHOW MANAGEMENT CONFIG", "op.show.management.config", "SBLR_OP_SHOW_MANAGEMENT_CONFIG", "sblr.management.runtime_operation.v3", "rs.management.config.v1", "", "", ""},
    {"AUDIT-0462", "SHOW MANAGEMENT DRIFT", "op.show.management.drift", "SBLR_OP_SHOW_MANAGEMENT_DRIFT", "sblr.management.runtime_operation.v3", "rs.management.drift.v1", "", "", ""},
}};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasDiagnosticCode(const MessageVectorSet& messages, std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool ApiResultHasEvidence(const api::EngineApiResult& result,
                          std::string_view kind,
                          std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

std::string EvidenceMessage(const B001Row& row,
                            std::string_view phase,
                            std::string_view message) {
  std::string rendered(row.audit_id);
  rendered += ' ';
  rendered += row.operation_id;
  rendered += ' ';
  rendered += phase;
  rendered += ": ";
  rendered += message;
  return rendered;
}

std::string_view ExpectedEngineApiFunction(const B001Row& row) {
  if (StartsWith(row.operation_id, "op.gpu.")) return "EngineControlGpuAcceleration";
  if (StartsWith(row.operation_id, "op.native_compile.")) return "EngineControlNativeCompile";
  if (StartsWith(row.operation_id, "op.management.")) return "EngineControlManagementRuntime";
  if (StartsWith(row.operation_id, "op.show.management.")) return "EngineInspectManagementRuntime";
  if (row.operation_id == "op.show.audit" ||
      row.operation_id == "op.show.discovery_rights" ||
      row.operation_id == "op.show.grants" ||
      row.operation_id == "op.show.groups" ||
      row.operation_id == "op.show.identity_providers") {
    return "EngineSecurityInspectOperation";
  }
  if (row.operation_id == "op.show.aot_artifacts" ||
      row.operation_id == "op.show.llvm" ||
      row.operation_id == "op.show.llvm_provenance" ||
      row.operation_id == "op.show.llvm_targets") {
    return "EngineInspectNativeCompile";
  }
  if (row.operation_id == "op.show.gpu" ||
      row.operation_id == "op.show.gpu_artifacts" ||
      row.operation_id == "op.show.gpu_capability" ||
      row.operation_id == "op.show.gpu_devices" ||
      row.operation_id == "op.show.gpu_kernels" ||
      row.operation_id == "op.show.gpu_memory") {
    return "EngineInspectGpuAcceleration";
  }
  return "EngineInspectShowOperation";
}

std::string_view ExpectedAuthorityStep(const B001Row& row) {
  if (ExpectedEngineApiFunction(row) == "EngineSecurityInspectOperation") {
    return "authority.engine.security_inspection_api_required";
  }
  if (ExpectedEngineApiFunction(row) == "EngineControlManagementRuntime" ||
      ExpectedEngineApiFunction(row) == "EngineInspectManagementRuntime") {
    return "authority.engine.management_runtime_api_required";
  }
  if (ExpectedEngineApiFunction(row) == "EngineInspectShowOperation") {
    return "authority.engine.observability_api_required";
  }
  return "authority.engine.acceleration_api_required";
}

std::string_view ExpectedAdmissionFamily(const B001Row& row) {
  if (row.operation_id == "op.show.audit" ||
      row.operation_id == "op.show.discovery_rights" ||
      row.operation_id == "op.show.grants" ||
      row.operation_id == "op.show.groups" ||
      row.operation_id == "op.show.identity_providers") {
    return "sblr.catalog.introspect.v3";
  }
  if (StartsWith(row.operation_id, "op.management.")) {
    return "sblr.management.control.v3";
  }
  if (StartsWith(row.operation_id, "op.show.management.")) {
    return "sblr.management.report.v3";
  }
  if (row.operation_id == "op.show.index_health") {
    return "sblr.index.maintenance.v3";
  }
  if (StartsWith(row.operation_id, "op.show.gpu")) {
    return "sblr.acceleration.gpu.v3";
  }
  if (row.operation_id == "op.show.aot_artifacts" ||
      StartsWith(row.operation_id, "op.show.llvm") ||
      StartsWith(row.operation_id, "op.show.native_compile")) {
    return "sblr.acceleration.llvm.v3";
  }
  if (row.operation_id == "op.show.metrics" ||
      row.operation_id == "op.show.metrics_family" ||
      row.operation_id == "op.show.performance") {
    return "sblr.metrics.read.v3";
  }
  if (StartsWith(row.operation_id, "op.show.")) {
    return "sblr.management.report.v3";
  }
  return row.family;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-00000000c701";
  session.connection_uuid = "019f0000-0000-7000-8000-00000000c702";
  session.database_uuid = "019f0000-0000-7000-8000-00000000c703";
  session.catalog_epoch = 53;
  session.security_policy_epoch = 59;
  session.descriptor_epoch = 61;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "019f0000-0000-7000-8000-00000000c704";
  config.bundle_contract_id = "sbp_sbsql@sbsql-sblr-final-cleanup-b001";
  config.build_id = "sbsql-sblr-final-cleanup-b001";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

api::EngineRequestContext EngineContext(bool security_context_present = true) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-sblr-final-cleanup-b001";
  context.security_context_present = security_context_present;
  context.database_path = "/tmp/sbsql_sblr_final_cleanup_b001.sbdb";
  context.database_uuid.canonical = "019f0000-0000-7000-8000-00000000c801";
  context.session_uuid.canonical = "019f0000-0000-7000-8000-00000000c802";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-00000000c803";
  context.node_uuid.canonical = "019f0000-0000-7000-8000-00000000c804";
  context.statement_uuid.canonical = "019f0000-0000-7000-8000-00000000c805";
  context.current_diagnostic_uuid.canonical = "019f0000-0000-7000-8000-00000000c806";
  context.catalog_generation_id = 53;
  context.security_epoch = 59;
  context.resource_epoch = 61;
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelope(const B001Row& row) {
  auto envelope = sblr::MakeSblrEnvelope(std::string(row.operation_id),
                                         std::string(row.opcode),
                                         std::string("trace.b001.") +
                                             std::string(row.audit_id));
  envelope.result_shape = row.result_shape;
  envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = false;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  return envelope;
}

api::EngineApiRequest ApiRequestForRow(const B001Row& row) {
  api::EngineApiRequest request;
  request.option_envelopes.push_back(std::string("result_shape_contract:") +
                                     std::string(row.result_shape));
  if (!row.target_ref.empty()) {
    request.option_envelopes.push_back(std::string("target_ref:") +
                                       std::string(row.target_ref));
    request.option_envelopes.push_back(std::string("target_ref_kind:") +
                                       std::string(row.target_ref_kind));
  }
  if (!row.count.empty()) {
    request.option_envelopes.push_back(std::string("resize_count:") +
                                       std::string(row.count));
  }
  return request;
}

void RequireExactLowering(const B001Row& row) {
  const auto artifacts = RunPipeline(row.sql);
  Require(artifacts.bound.bound, EvidenceMessage(row, "parser_bind_lower", "row did not bind"));
  Require(!artifacts.bound.requires_name_resolution,
          EvidenceMessage(row, "parser_bind_lower", "row required parser-side name resolution"));
  Require(artifacts.verifier.admitted,
          EvidenceMessage(row, "parser_bind_lower", "SBLR verifier rejected row"));
  Require(artifacts.envelope.operation_id == row.operation_id,
          EvidenceMessage(row, "parser_bind_lower", "operation id mismatch"));
  Require(artifacts.envelope.sblr_opcode == row.opcode,
          EvidenceMessage(row, "parser_bind_lower", "opcode mismatch"));
  Require(artifacts.envelope.operation_family == row.family,
          EvidenceMessage(row, "parser_bind_lower", "operation family mismatch"));
  Require(artifacts.envelope.sblr_operation_key == row.family,
          EvidenceMessage(row, "parser_bind_lower", "SBLR operation key mismatch"));
  Require(artifacts.envelope.result_shape_key == row.result_shape,
          EvidenceMessage(row, "parser_bind_lower", "result shape mismatch"));
  Require(artifacts.envelope.engine_api_function == ExpectedEngineApiFunction(row),
          EvidenceMessage(row, "parser_bind_lower", "engine API function missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   ExpectedAuthorityStep(row)),
          EvidenceMessage(row, "parser_bind_lower", "engine API authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          EvidenceMessage(row, "parser_bind_lower", "no-SQL authority missing"));
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.cluster.provider_dispatch_not_required"),
          EvidenceMessage(row, "parser_bind_lower", "cluster boundary authority missing"));
  Require(Contains(artifacts.envelope.payload, "\"public_sbsql_exact_command\":true"),
          EvidenceMessage(row, "parser_bind_lower", "public exact command payload flag missing"));
  Require(Contains(artifacts.envelope.payload, "\"engine_api_function\":\"") &&
              Contains(artifacts.envelope.payload, ExpectedEngineApiFunction(row)),
          EvidenceMessage(row, "parser_bind_lower", "domain API function missing from payload"));
  Require(!Contains(artifacts.envelope.payload, row.audit_id),
          EvidenceMessage(row, "parser_bind_lower", "audit id leaked into production payload"));
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          EvidenceMessage(row, "parser_bind_lower", "parser execution evidence missing"));
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          EvidenceMessage(row, "parser_bind_lower", "source text key was embedded"));
  Require(!Contains(artifacts.envelope.payload, "\"sql_text\":"),
          EvidenceMessage(row, "parser_bind_lower", "SQL text key was embedded"));

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted,
          EvidenceMessage(row, "server_admission", "server admission rejected row"));
  Require(admission.requires_public_abi_dispatch,
          EvidenceMessage(row, "server_admission", "public ABI dispatch not required"));
  Require(admission.operation_id == row.operation_id,
          EvidenceMessage(row, "server_admission", "operation id mismatch"));
  Require(admission.operation_family == ExpectedAdmissionFamily(row),
          EvidenceMessage(row, "server_admission", "operation family mismatch"));
}

void RequireSblrRegistryAndDispatch(const B001Row& row) {
  const auto* entry = sblr::LookupSblrOperation(row.operation_id);
  Require(entry != nullptr, EvidenceMessage(row, "sblr_registry", "operation missing"));
  Require(entry->opcode == row.opcode, EvidenceMessage(row, "sblr_registry", "opcode mismatch"));
  const auto envelope = EngineEnvelope(row);
  const auto registry_validation = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  Require(registry_validation.ok,
          EvidenceMessage(row, "sblr_registry", "registry validation rejected row"));

  const sblr::SblrDispatchRequest request{EngineContext(), envelope, ApiRequestForRow(row)};
  const auto result = sblr::DispatchSblrOperation(request);
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
  Require(result.envelope_validated,
          EvidenceMessage(row, "engine_dispatch", "engine envelope was not valid"));
  Require(result.accepted,
          EvidenceMessage(row, "engine_dispatch", "dispatch did not accept row"));
  Require(result.dispatched_to_api,
          EvidenceMessage(row, "engine_dispatch", "dispatch did not route to API"));
  Require(result.api_result.ok,
          EvidenceMessage(row, "engine_dispatch", "internal API returned failure"));
  Require(result.api_result.operation_id == row.operation_id,
          EvidenceMessage(row, "engine_dispatch", "API operation id mismatch"));
  Require(result.api_result.result_shape.result_kind == row.result_shape,
          EvidenceMessage(row, "engine_dispatch", "API result shape mismatch"));
  Require(ApiResultHasEvidence(result.api_result,
                               "public_sbsql_operation",
                               row.operation_id),
          EvidenceMessage(row, "engine_dispatch", "public operation API evidence missing"));
  Require(ApiResultHasEvidence(result.api_result,
                               "engine_api_function",
                               ExpectedEngineApiFunction(row)),
          EvidenceMessage(row, "engine_dispatch", "domain API evidence missing"));
  Require(ApiResultHasEvidence(result.api_result,
                               "result_shape_contract",
                               row.result_shape),
          EvidenceMessage(row, "engine_dispatch", "result shape evidence missing"));
}

void RequireInvalidSyntaxDiagnostics() {
  const auto artifacts = RunPipeline("ALTER GPU DEVICE QUARANTINE");
  Require(artifacts.envelope.messages.has_errors(),
          "invalid exact command shape did not produce a message-vector error");
  Require(HasDiagnosticCode(artifacts.envelope.messages,
                            "SBSQL.EXACT_COMMAND.INVALID_SHAPE"),
          "invalid exact command shape did not produce the expected diagnostic code");
  const std::string rendered = RenderMessageVectorSet(artifacts.envelope.messages);
  Require(Contains(rendered, "SBSQL.EXACT_COMMAND.INVALID_SHAPE"),
          "rendered exact command message vector omitted diagnostic code");
  Require(Contains(rendered, "op.gpu.device_quarantine") &&
              Contains(rendered, "alter.gpu.device_quarantine"),
          "rendered exact command message vector omitted operation or surface fields");
}

void RequireSecurityRefusalRedaction() {
  api::EngineSecurityInspectOperationRequest request;
  request.context = EngineContext(false);
  request.operation_id = "op.show.grants";
  request.option_envelopes.push_back("target_ref:principal_secret");
  const auto result = api::EngineSecurityInspectOperation(request);
  Require(!result.ok, "security refusal unexpectedly succeeded");
  Require(!result.diagnostics.empty(), "security refusal omitted diagnostics");
  for (const auto& diagnostic : result.diagnostics) {
    Require(diagnostic.detail.find("principal_secret") == std::string::npos,
            "security refusal leaked target reference in diagnostic detail");
  }
}

void RequireShowJobsRoutePreserved() {
  const auto artifacts = RunPipeline("SHOW JOBS");
  Require(artifacts.bound.bound, "SHOW JOBS did not bind");
  Require(artifacts.verifier.admitted, "SHOW JOBS verifier rejected existing route");
  Require(artifacts.envelope.operation_id == "observability.show_jobs",
          "SHOW JOBS was hijacked away from the plural jobs exact route");
  Require(artifacts.envelope.sblr_opcode == "SBLR_OBSERVABILITY_SHOW_JOBS",
          "SHOW JOBS opcode changed unexpectedly");
  Require(Contains(artifacts.envelope.payload, "\"public_sbsql_exact_command\":true"),
          "SHOW JOBS was not marked as a public exact command route");
  Require(!Contains(artifacts.envelope.payload, "\"surface_key\":\"show.job\""),
          "SHOW JOBS was marked as the singular SHOW JOB exact-command route");
}

void RequireProductionSourceIntegrity() {
  static constexpr std::array<std::string_view, 17> kForbidden = {
      "sbsql_sblr_final_cleanup",
      "final_cleanup",
      "B001Exact",
      "IsB001",
      "b001_",
      "_b001",
      "EngineRunSbsqlSblrFinalCleanup",
      "AUDIT-0",
      "AUDIT-1",
      "AUDIT-2",
      "AUDIT-3",
      "AUDIT-4",
      "AUDIT-5",
      "AUDIT-6",
      "AUDIT-7",
      "AUDIT-8",
      "AUDIT-9",
  };
  const std::filesystem::path source_root =
      std::filesystem::path(SCRATCHBIRD_PROJECT_SOURCE_DIR) / "src";
  for (const auto& entry : std::filesystem::recursive_directory_iterator(source_root)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) continue;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    for (const auto token : kForbidden) {
      Require(!Contains(text, token),
              std::string("production source contains forbidden batch token ") +
                  std::string(token) + " in " + entry.path().string());
    }
  }
}

}  // namespace

int main() {
  RequireProductionSourceIntegrity();
  for (const auto& row : kRows) {
    RequireExactLowering(row);
    RequireSblrRegistryAndDispatch(row);
  }
  RequireInvalidSyntaxDiagnostics();
  RequireSecurityRefusalRedaction();
  RequireShowJobsRoutePreserved();
  std::cout << "sbsql_sblr_final_cleanup_b001_exact_route_conformance=passed\n";
  return EXIT_SUCCESS;
}
