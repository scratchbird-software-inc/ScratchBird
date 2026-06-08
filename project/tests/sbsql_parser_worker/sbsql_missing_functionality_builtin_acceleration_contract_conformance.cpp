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
#include "database_lifecycle.hpp"
#include "dispatch/function_dispatch.hpp"
#include "lowering/lowering.hpp"
#include "memory.hpp"
#include "registry/function_seed_registry.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "rendering/rendering.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace fn = scratchbird::engine::functions;
namespace memory = scratchbird::core::memory;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kDatabaseUuidSeed = "019f0000-0000-7000-8000-000000012012";
constexpr std::string_view kStatementUuid = "019f0000-0000-7000-8000-000000012013";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  for (const auto& value : values) {
    if (value == expected) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == id) return true;
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "sbsql_missing_functionality_gate_012";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_missing_functionality_builtin_acceleration_contract_conformance");
  Require(configured.ok(), "Gate 012 memory fixture configuration failed");
  Require(configured.fixture_mode, "Gate 012 memory fixture mode was not active");
}

sblr::SblrValue TextValue(std::string descriptor, std::string input) {
  sblr::SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = sblr::SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
  value.text_value = value.encoded_value;
  return value;
}

sblr::SblrValue NullValue(std::string descriptor) {
  sblr::SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.is_null = true;
  return value;
}

sblr::SblrResult RunFunction(const fn::FunctionRegistry& registry,
                             std::string function_id,
                             std::vector<sblr::SblrValue> values = {}) {
  fn::FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context.statement_timestamp = "2026-06-05T00:00:00Z";
  request.context.sblr_context.current_timestamp = "2026-06-05T00:00:01Z";
  request.context.sblr_context.transaction_context_present = true;
  for (std::size_t index = 0; index < values.size(); ++index) {
    request.arguments.push_back(fn::FunctionArgument{"arg" + std::to_string(index),
                                                     std::move(values[index])});
  }
  return fn::DispatchFunctionCall(registry, std::move(request)).result;
}

void RequireLastDayRuntime() {
  const auto package = fn::BuildStandardFunctionSeedPackage();
  const auto* entry = package.registry.Lookup("sb.temporal.last_day");
  Require(entry != nullptr, "Gate 012 last_day function registry row missing");
  Require(entry->implementation_state == fn::FunctionImplementationState::implemented_behavior,
          "Gate 012 last_day is not implemented_behavior");
  Require(entry->package_state == fn::FunctionPackageState::core,
          "Gate 012 last_day is not a core package function");

  const auto normal = RunFunction(package.registry,
                                  "sb.temporal.last_day",
                                  {TextValue("date", "2026-05-11")});
  Require(normal.ok(), "Gate 012 last_day normal date failed");
  Require(normal.scalar_values.size() == 1, "Gate 012 last_day normal arity drifted");
  Require(!normal.scalar_values.front().is_null, "Gate 012 last_day normal returned null");
  Require(normal.scalar_values.front().descriptor_id == "date",
          "Gate 012 last_day normal descriptor drifted");
  Require(normal.scalar_values.front().text_value == "2026-05-31",
          "Gate 012 last_day normal value drifted");

  const auto leap = RunFunction(package.registry,
                                "sb.temporal.last_day",
                                {TextValue("date", "2024-02-15")});
  Require(leap.ok(), "Gate 012 last_day leap date failed");
  Require(leap.scalar_values.size() == 1, "Gate 012 last_day leap arity drifted");
  Require(leap.scalar_values.front().text_value == "2024-02-29",
          "Gate 012 last_day leap value drifted");

  const auto null_value = RunFunction(package.registry,
                                      "sb.temporal.last_day",
                                      {NullValue("date")});
  Require(null_value.ok(), "Gate 012 last_day null input failed");
  Require(null_value.scalar_values.size() == 1, "Gate 012 last_day null arity drifted");
  Require(null_value.scalar_values.front().is_null,
          "Gate 012 last_day null input did not return null");

  const auto* bare_row = FindGeneratedSurfaceRegistryRowById("SBSQL-24D3CBDD3F18");
  Require(bare_row != nullptr && bare_row->canonical_name == "last_day",
          "Gate 012 last_day bare SBsql registry row missing");
  const auto* typed_row = FindGeneratedSurfaceRegistryRowById("SBSQL-559A925B6DA8");
  Require(typed_row != nullptr && typed_row->canonical_name == "last_day(date)",
          "Gate 012 last_day(date) SBsql registry row missing");
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000012101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000012102";
  session.database_uuid = "019f0000-0000-7000-8000-000000012103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 120;
  session.security_policy_epoch = 121;
  session.descriptor_epoch = 122;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsql_missing_gate_012";
  config.parser_uuid = "019f0000-0000-7000-8000-000000012104";
  config.bundle_contract_id = "sbp_sbsql@sbsql-missing-gate-012";
  config.build_id = "sbsql-missing-functionality-gate-012";
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
                            {std::string(kStatementUuid)});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_missing_gate_012_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events", ".sb.crud_events", ".sb.name_events",
                            ".sb.transaction_inventory", ".dirty.manifest",
                            ".sb.owner.lock", ".sb.txn_publish"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  const auto database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810120000);
  const auto filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810120001);
  Require(database_uuid.ok(), "Gate 012 database UUID generation failed");
  Require(filespace_uuid.ok(), "Gate 012 filespace UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810120002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "Gate 012 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.request_id = "sbsql-missing-functionality-gate-012";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid.empty() ? std::string(kDatabaseUuidSeed)
                                                          : database_uuid;
  context.session_uuid.canonical = "019f0000-0000-7000-8000-000000012201";
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000012202";
  context.current_schema_uuid.canonical = "019f0000-0000-7000-8000-000000012203";
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.catalog_generation_id = 1;
  context.security_epoch = 2;
  context.resource_epoch = 3;
  context.name_resolution_epoch = 4;
  context.trace_tags.push_back("security.bootstrap");
  context.trace_tags.push_back("right:ACCELERATION_CONTROL");
  return context;
}

sblr::SblrOperationEnvelope EngineEnvelopeFromParser(const SblrEnvelope& parser_envelope) {
  auto engine_envelope = sblr::MakeSblrEnvelope(parser_envelope.operation_id,
                                                parser_envelope.sblr_opcode,
                                                "trace.sbsql.missing.gate012");
  engine_envelope.result_shape = parser_envelope.result_shape_key;
  engine_envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  engine_envelope.requires_security_context = true;
  engine_envelope.requires_transaction_context = false;
  engine_envelope.requires_cluster_authority = false;
  engine_envelope.contains_sql_text = false;
  engine_envelope.parser_resolved_names_to_uuids = true;
  for (const auto& operand : parser_envelope.operands) {
    engine_envelope.operands.push_back({operand.type, operand.name, operand.value});
  }
  return engine_envelope;
}

api::EngineApiRequest ApiRequestWithResultShape(std::string_view result_shape) {
  api::EngineApiRequest request;
  if (!result_shape.empty()) {
    request.option_envelopes.push_back("result_shape_contract:" + std::string(result_shape));
  }
  return request;
}

void RequireCompileStatementPipelineAndDispatch(const api::EngineRequestContext& context) {
  const auto* llvm_row = FindGeneratedSurfaceRegistryRowById("SBSQL-1DFEDF33C807");
  Require(llvm_row != nullptr && llvm_row->canonical_name == "llvm_stmt",
          "Gate 012 LLVM registry row missing");
  Require(llvm_row->sblr_operation_family == "sblr.acceleration.llvm.v3",
          "Gate 012 LLVM registry family drifted");

  const auto* opcode = sblr::LookupSblrOperation("extensibility.compile_llvm_module");
  Require(opcode != nullptr, "Gate 012 LLVM compile opcode row missing");
  Require(opcode->opcode == "SBLR_EXTENSIBILITY_COMPILE_LLVM_MODULE",
          "Gate 012 LLVM compile opcode drifted");

  const auto artifacts =
      RunPipeline("COMPILE STATEMENT monthly_close AS SBLR sblr_projection_unit MODE JIT;");
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "Gate 012 compile statement CST failed");
  Require(!artifacts.ast.messages.has_errors(), "Gate 012 compile statement AST failed");
  Require(artifacts.bound.bound, "Gate 012 compile statement bind failed");
  Require(artifacts.verifier.admitted, "Gate 012 compile statement verifier rejected route");
  Require(artifacts.envelope.operation_family == "sblr.acceleration.llvm.v3",
          "Gate 012 compile statement family mismatch");
  Require(artifacts.envelope.operation_id == "extensibility.compile_llvm_module",
          "Gate 012 compile statement operation mismatch");
  Require(artifacts.envelope.sblr_opcode == "SBLR_EXTENSIBILITY_COMPILE_LLVM_MODULE",
          "Gate 012 compile statement opcode mismatch");
  Require(artifacts.envelope.engine_api_function == "EngineCompileLlvmModule",
          "Gate 012 compile statement API mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "Gate 012 compile statement no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "Gate 012 compile statement no-finality authority missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "Gate 012 compile statement allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"compile_statement_route\":true"),
          "Gate 012 compile statement payload did not mark compile route");
  Require(Contains(artifacts.envelope.payload, "\"compile_module_symbol\":\"sblr_projection_unit\""),
          "Gate 012 compile statement payload missing module symbol");
  Require(!Contains(artifacts.envelope.payload, "COMPILE STATEMENT monthly_close"),
          "Gate 012 compile statement embedded source SQL text");
  Require(artifacts.envelope.operands.size() == 8,
          "Gate 012 compile statement operand count drifted");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  if (!admission.admitted) {
    for (const auto& diagnostic : admission.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
                << diagnostic.safe_message << '\n';
    }
  }
  Require(admission.admitted, "Gate 012 server admission rejected compile statement");
  Require(admission.requires_public_abi_dispatch,
          "Gate 012 compile statement did not require public ABI dispatch");

  const auto result = sblr::DispatchSblrOperation(
      {context, EngineEnvelopeFromParser(artifacts.envelope), api::EngineApiRequest{}});
  Require(result.envelope_validated, "Gate 012 compile statement engine envelope rejected");
  Require(result.accepted, "Gate 012 compile statement dispatch rejected");
  Require(result.dispatched_to_api, "Gate 012 compile statement not dispatched to API");
  Require(result.api_result.ok, "Gate 012 compile statement API failed");
  Require(result.api_result.operation_id == "extensibility.compile_llvm_module",
          "Gate 012 compile statement runtime operation drifted");
  Require(HasEvidence(result.api_result, "llvm_compile_contract", "validated_request_shape"),
          "Gate 012 compile statement contract evidence missing");
  Require(HasEvidence(result.api_result, "llvm_compile_runtime", "interpreter"),
          "Gate 012 compile statement fallback evidence missing");
  Require(HasEvidence(result.api_result, "llvm_cache", "miss"),
          "Gate 012 compile statement cache evidence missing");
  Require(HasEvidence(result.api_result, "execution_boundary", "sblr_only_engine_authority"),
          "Gate 012 compile statement engine authority evidence missing");
}

void RequireCacheControlRoute(const api::EngineRequestContext& context,
                              std::string_view sql,
                              std::string_view operation_id,
                              std::string_view opcode,
                              std::string_view engine_api_function,
                              std::string_view operation_family) {
  const auto artifacts = RunPipeline(sql);
  if (artifacts.cst.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  if (artifacts.ast.messages.has_errors()) std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  if (!artifacts.bound.bound) std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  if (!artifacts.verifier.admitted) std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "Gate 012 cache route CST failed");
  Require(!artifacts.ast.messages.has_errors(), "Gate 012 cache route AST failed");
  Require(artifacts.bound.bound, "Gate 012 cache route bind failed");
  Require(!artifacts.bound.requires_name_resolution,
          "Gate 012 cache route required parser-side name resolution");
  Require(artifacts.verifier.admitted, "Gate 012 cache route verifier rejected route");
  Require(artifacts.envelope.operation_family == operation_family,
          "Gate 012 cache route family mismatch");
  Require(artifacts.envelope.operation_id == operation_id,
          "Gate 012 cache route operation mismatch");
  Require(artifacts.envelope.sblr_opcode == opcode,
          "Gate 012 cache route opcode mismatch");
  Require(artifacts.envelope.engine_api_function == engine_api_function,
          "Gate 012 cache route API mismatch");
  Require(artifacts.envelope.result_shape_key == "rs.acceleration.control.v1",
          "Gate 012 cache route result shape mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.acceleration_api_required"),
          "Gate 012 cache route acceleration authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "Gate 012 cache route no-SQL-execution authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "Gate 012 cache route no-finality authority missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "Gate 012 cache route allowed parser SQL execution");
  Require(Contains(artifacts.envelope.payload, "\"public_sbsql_exact_command\":true"),
          "Gate 012 cache route payload did not mark exact command");
  Require(Contains(artifacts.envelope.payload,
                   "\"engine_result_shape_contract\":\"rs.acceleration.control.v1\""),
          "Gate 012 cache route payload missing result-shape contract");
  Require(!Contains(artifacts.envelope.payload, "\"source_text\""),
          "Gate 012 cache route embedded source text key");
  Require(!Contains(artifacts.envelope.payload, "\"sql_text\":"),
          "Gate 012 cache route embedded SQL text key");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "Gate 012 cache route server admission rejected route");
  Require(admission.requires_public_abi_dispatch,
          "Gate 012 cache route did not require public ABI dispatch");
  Require(admission.operation_id == operation_id,
          "Gate 012 cache route admission operation mismatch");
  Require(admission.operation_family == operation_family,
          "Gate 012 cache route admission family mismatch");

  const auto result = sblr::DispatchSblrOperation(
      {context,
       EngineEnvelopeFromParser(artifacts.envelope),
       ApiRequestWithResultShape(artifacts.envelope.result_shape_key)});
  Require(result.envelope_validated, "Gate 012 cache route engine envelope rejected");
  Require(result.accepted, "Gate 012 cache route dispatch rejected");
  Require(result.dispatched_to_api, "Gate 012 cache route not dispatched to API");
  Require(result.api_result.ok, "Gate 012 cache route API failed");
  Require(result.api_result.operation_id == operation_id,
          "Gate 012 cache route runtime operation drifted");
  Require(result.api_result.result_shape.result_kind == "rs.acceleration.control.v1",
          "Gate 012 cache route runtime result shape drifted");
  Require(HasEvidence(result.api_result, "public_sbsql_operation", operation_id),
          "Gate 012 cache route public operation evidence missing");
  Require(HasEvidence(result.api_result, "engine_api_function", engine_api_function),
          "Gate 012 cache route engine API evidence missing");
  Require(HasEvidence(result.api_result,
                      "result_shape_contract",
                      "rs.acceleration.control.v1"),
          "Gate 012 cache route result-shape evidence missing");
  Require(HasEvidence(result.api_result, "parser_executes_sql", "false"),
          "Gate 012 cache route parser-execution evidence missing");
}

void RequireCacheInvalidationRoutes(const api::EngineRequestContext& context) {
  RequireCacheControlRoute(context,
                           "ALTER NATIVE COMPILE CACHE INVALIDATE",
                           "op.native_compile.cache_invalidate",
                           "SBLR_OP_NATIVE_COMPILE_CACHE_INVALIDATE",
                           "EngineControlNativeCompile",
                           "sblr.acceleration.llvm.v3");
  RequireCacheControlRoute(context,
                           "ALTER GPU CACHE CLEAR",
                           "op.gpu.cache_clear",
                           "SBLR_OP_GPU_CACHE_CLEAR",
                           "EngineControlGpuAcceleration",
                           "sblr.acceleration.gpu.v3");
}

void RequireAccelerationRefusals(const api::EngineRequestContext& context) {
  auto raw_sql = sblr::MakeSblrEnvelope("extensibility.compile_llvm_module",
                                       "SBLR_EXTENSIBILITY_COMPILE_LLVM_MODULE",
                                       "trace.sbsql.missing.gate012.raw_sql");
  raw_sql.requires_security_context = true;
  raw_sql.contains_sql_text = false;
  raw_sql.parser_resolved_names_to_uuids = true;
  raw_sql.operands.push_back({"text", "compile", "jit"});
  raw_sql.operands.push_back({"text", "module", "sql:SELECT 1"});
  raw_sql.operands.push_back({"text", "raw_sql", "true"});
  const auto raw_sql_result =
      sblr::DispatchSblrOperation({context, raw_sql, api::EngineApiRequest{}});
  Require(raw_sql_result.envelope_validated,
          "Gate 012 raw SQL refusal envelope was rejected before API");
  Require(raw_sql_result.accepted, "Gate 012 raw SQL refusal route was not accepted");
  Require(!raw_sql_result.api_result.ok,
          "Gate 012 LLVM raw SQL compile was not refused");
  Require(HasDiagnostic(raw_sql_result.api_result, "SB_ENGINE_API_LLVM_RAW_SQL_REFUSED"),
          "Gate 012 LLVM raw SQL refusal diagnostic missing");

  auto gpu = sblr::MakeSblrEnvelope("extensibility.inspect_gpu_capability",
                                    "SBLR_EXTENSIBILITY_INSPECT_GPU_CAPABILITY",
                                    "trace.sbsql.missing.gate012.gpu");
  gpu.requires_security_context = true;
  gpu.contains_sql_text = false;
  gpu.parser_resolved_names_to_uuids = true;
  gpu.operands.push_back({"text", "simulate_gpu_provider", "gpu.simulated"});
  gpu.operands.push_back({"text", "workload", "inspect"});
  gpu.operands.push_back({"text", "gpu_profile", "inspect_only"});
  const auto gpu_result =
      sblr::DispatchSblrOperation({context, gpu, api::EngineApiRequest{}});
  Require(gpu_result.envelope_validated, "Gate 012 GPU envelope rejected");
  Require(gpu_result.accepted, "Gate 012 GPU route rejected");
  Require(gpu_result.api_result.ok, "Gate 012 GPU inspect failed");
  Require(HasEvidence(gpu_result.api_result, "gpu_capability", "inspected"),
          "Gate 012 GPU capability evidence missing");
  Require(HasEvidence(gpu_result.api_result,
                      "execution_boundary",
                      "gpu_never_transaction_security_visibility_authority"),
          "Gate 012 GPU authority boundary evidence missing");

  api::EngineRequestContext no_security_context = context;
  no_security_context.security_context_present = false;
  auto gpu_control = sblr::MakeSblrEnvelope("extensibility.inspect_gpu_capability",
                                            "SBLR_EXTENSIBILITY_INSPECT_GPU_CAPABILITY",
                                            "trace.sbsql.missing.gate012.gpu_refusal");
  gpu_control.requires_security_context = false;
  gpu_control.contains_sql_text = false;
  gpu_control.parser_resolved_names_to_uuids = true;
  gpu_control.operands.push_back({"text", "enable_gpu_execution", "true"});
  const auto gpu_refusal =
      sblr::DispatchSblrOperation({no_security_context, gpu_control, api::EngineApiRequest{}});
  Require(gpu_refusal.envelope_validated, "Gate 012 GPU refusal envelope rejected");
  Require(gpu_refusal.accepted, "Gate 012 GPU refusal route rejected before API");
  Require(!gpu_refusal.api_result.ok,
          "Gate 012 GPU control without security context was not refused");
  Require(HasDiagnostic(gpu_refusal.api_result,
                        "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "Gate 012 GPU security refusal diagnostic missing");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  RequireLastDayRuntime();

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);
  const auto context = EngineContext(path, database_uuid);

  RequireCompileStatementPipelineAndDispatch(context);
  RequireCacheInvalidationRoutes(context);
  RequireAccelerationRefusals(context);

  RemoveDatabaseArtifacts(path);
  std::cout << "sbsql_missing_functionality_builtin_acceleration_contract_conformance=passed\n";
  return EXIT_SUCCESS;
}
