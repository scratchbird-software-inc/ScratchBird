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
#include "transaction/savepoint_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
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
using sblr::SblrValue;
using sblr::SblrValuePayloadKind;

constexpr std::string_view kSchemaUuid = "019f0000-0000-7000-8000-000000011001";
constexpr std::string_view kStatementUuid = "019f0000-0000-7000-8000-000000011002";

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
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
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
  policy.policy_name = "sbsql_missing_functionality_gate_011";
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
      MemoryPolicy(), "sbsql_missing_functionality_lock_mga_policy_conformance");
  Require(configured.ok(), "Gate 011 memory fixture configuration failed");
  Require(configured.fixture_mode, "Gate 011 memory fixture mode was not active");
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000011101";
  session.connection_uuid = "019f0000-0000-7000-8000-000000011102";
  session.database_uuid = "019f0000-0000-7000-8000-000000011103";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 111;
  session.security_policy_epoch = 112;
  session.descriptor_epoch = 113;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsql_missing_gate_011";
  config.parser_uuid = "019f0000-0000-7000-8000-000000011104";
  config.bundle_contract_id = "sbp_sbsql@sbsql-missing-gate-011";
  config.build_id = "sbsql-missing-functionality-gate-011";
  return config;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

void PrintMessages(const PipelineArtifacts& artifacts) {
  if (artifacts.cst.messages.has_errors()) {
    std::cerr << RenderMessageVectorSet(artifacts.cst.messages);
  }
  if (artifacts.ast.messages.has_errors()) {
    std::cerr << RenderMessageVectorSet(artifacts.ast.messages);
  }
  if (!artifacts.bound.bound) {
    std::cerr << RenderMessageVectorSet(artifacts.bound.messages);
  }
  if (!artifacts.verifier.admitted) {
    std::cerr << RenderMessageVectorSet(artifacts.verifier.messages);
  }
}

PipelineArtifacts RunPipeline(std::string_view sql,
                              std::vector<std::string> resolved = {}) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast,
                            artifacts.cst,
                            ParserConfigForTest(),
                            session,
                            std::move(resolved));
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

sblr::SblrOperationEnvelope EngineEnvelopeFromParser(const SblrEnvelope& parser_envelope) {
  auto engine_envelope = sblr::MakeSblrEnvelope(
      parser_envelope.engine_api_operation_id.empty() ? parser_envelope.operation_id
                                                      : parser_envelope.engine_api_operation_id,
      parser_envelope.sblr_opcode,
      parser_envelope.trace_key);
  engine_envelope.result_shape = parser_envelope.result_shape_key;
  engine_envelope.diagnostic_shape = "diagnostic.canonical_message_vector";
  engine_envelope.requires_security_context = true;
  engine_envelope.requires_transaction_context = true;
  engine_envelope.requires_cluster_authority = false;
  engine_envelope.contains_sql_text = false;
  engine_envelope.parser_resolved_names_to_uuids = true;
  for (const auto& operand : parser_envelope.operands) {
    engine_envelope.operands.push_back({operand.type, operand.name, operand.value});
  }
  return engine_envelope;
}

void PrintDispatchDiagnostics(const sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << "dispatch " << diagnostic.code << ':' << diagnostic.message << '\n';
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    std::cerr << "api " << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

std::filesystem::path TestDatabasePath() {
  return std::filesystem::temp_directory_path() /
         ("sbsql_missing_gate_011_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void RemoveDatabaseArtifacts(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  for (const auto suffix : {".sb.api_events", ".sb.crud_events", ".sb.name_events",
                            ".sb.transaction_inventory", ".dirty.manifest",
                            ".recovery.evidence", ".sb.owner.lock",
                            ".sb.txn_publish"}) {
    std::filesystem::remove(path.string() + suffix, ignored);
  }
}

std::string CreateMinimalDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810110000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810110001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810110002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "Gate 011 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext EngineContext(const std::filesystem::path& path,
                                        const std::string& database_uuid,
                                        std::string request_id,
                                        std::string session_uuid) {
  api::EngineRequestContext context;
  context.request_id = std::move(request_id);
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.session_uuid.canonical = std::move(session_uuid);
  context.principal_uuid.canonical = "019f0000-0000-7000-8000-000000011202";
  context.current_schema_uuid.canonical = std::string(kSchemaUuid);
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.catalog_generation_id = 1;
  context.security_epoch = 2;
  context.resource_epoch = 3;
  context.name_resolution_epoch = 4;
  context.trace_tags.push_back("right:TRANSACTION_CONTROL");
  return context;
}

api::EngineRequestContext BeginEngineTransaction(const std::filesystem::path& path,
                                                 const std::string& database_uuid,
                                                 std::string request_id,
                                                 std::string session_uuid) {
  auto context = EngineContext(path, database_uuid, std::move(request_id),
                               std::move(session_uuid));
  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  const auto result = api::EngineBeginTransaction(begin);
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, "Gate 011 transaction begin failed");
  context.local_transaction_id = result.local_transaction_id;
  context.transaction_uuid = result.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      result.snapshot_visible_through_local_transaction_id;
  return context;
}

api::EngineCommitTransactionResult CommitEngineTransaction(
    const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  const auto result = api::EngineCommitTransaction(commit);
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, "Gate 011 transaction commit failed");
  return result;
}

api::EngineRollbackTransactionResult RollbackEngineTransaction(
    const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  const auto result = api::EngineRollbackTransaction(rollback);
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, "Gate 011 transaction rollback failed");
  return result;
}

void RequireGeneratedRegistryRows() {
  const auto* lock_table = FindGeneratedSurfaceRegistryRowById("SBSQL-35D014F65562");
  Require(lock_table != nullptr, "Gate 011 lock_table registry row missing");
  Require(lock_table->family == "transaction", "Gate 011 lock_table family drifted");
  Require(lock_table->sblr_operation_family == "sblr.transaction.control.v3",
          "Gate 011 lock_table SBLR family drifted");

  const auto* lock_table_stmt = FindGeneratedSurfaceRegistryRowById("SBSQL-4F6D2D4E3F22");
  Require(lock_table_stmt != nullptr, "Gate 011 lock_table_stmt registry row missing");
  Require(lock_table_stmt->family == "transaction",
          "Gate 011 lock_table_stmt family drifted");
  Require(lock_table_stmt->sblr_operation_family == "sblr.transaction.control.v3",
          "Gate 011 lock_table_stmt SBLR family drifted");

  const auto* get_lock = FindGeneratedSurfaceRegistryRowById("SBSQL-6E2D0E0B0110");
  Require(get_lock != nullptr && get_lock->canonical_name == "get_lock(name,timeout)",
          "Gate 011 get_lock registry row missing");
  Require(get_lock->family == "expression_runtime",
          "Gate 011 get_lock family drifted");
  Require(get_lock->sblr_operation_family == "sblr.expression.runtime.v3",
          "Gate 011 get_lock SBLR family drifted");

  const auto* release_lock = FindGeneratedSurfaceRegistryRowById("SBSQL-6E2D0E0B0111");
  Require(release_lock != nullptr && release_lock->canonical_name == "release_lock(name)",
          "Gate 011 release_lock registry row missing");
  Require(release_lock->family == "expression_runtime",
          "Gate 011 release_lock family drifted");
  Require(release_lock->sblr_operation_family == "sblr.expression.runtime.v3",
          "Gate 011 release_lock SBLR family drifted");

  const auto* select_for_update = FindGeneratedSurfaceRegistryRowById("SBSQL-728CB259DD81");
  Require(select_for_update != nullptr &&
              select_for_update->canonical_name == "lock_row_for_update",
          "Gate 011 SELECT FOR UPDATE compatibility registry row missing");
  Require(select_for_update->family == "dml",
          "Gate 011 SELECT FOR UPDATE must remain DML/MGA-owned");
  Require(select_for_update->sblr_operation_family == "sblr.dml.operation.v3",
          "Gate 011 SELECT FOR UPDATE SBLR family drifted");
}

PipelineArtifacts RequireLockRoute(std::string_view sql,
                                   std::string_view operation_id,
                                   std::string_view opcode,
                                   std::string_view engine_api_function) {
  auto artifacts = RunPipeline(sql);
  PrintMessages(artifacts);
  Require(!artifacts.cst.messages.has_errors(), "Gate 011 CST failed");
  Require(!artifacts.ast.messages.has_errors(), "Gate 011 AST failed");
  Require(artifacts.bound.bound, "Gate 011 bind failed");
  Require(artifacts.verifier.admitted, "Gate 011 verifier rejected lock route");
  Require(artifacts.envelope.operation_family == "sblr.transaction.control.v3",
          "Gate 011 lock route family mismatch");
  Require(artifacts.envelope.sblr_operation_key == "sblr.transaction.control.v3",
          "Gate 011 lock route key mismatch");
  Require(artifacts.envelope.operation_id == operation_id,
          "Gate 011 lock route operation mismatch");
  Require(artifacts.envelope.sblr_opcode == opcode,
          "Gate 011 lock route opcode mismatch");
  Require(artifacts.envelope.engine_api_function == engine_api_function,
          "Gate 011 lock route engine API function mismatch");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.mga_transaction_control_required"),
          "Gate 011 lock route MGA authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.engine.transaction_lock_policy_required"),
          "Gate 011 lock route policy authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "Gate 011 lock route parser no-SQL authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "Gate 011 lock route parser no-finality authority missing");
  Require(HasValue(artifacts.envelope.policy_refs, "transaction_lock_mga_policy"),
          "Gate 011 transaction lock policy ref missing");
  Require(Contains(artifacts.envelope.payload, "\"transaction_lock_route\":true"),
          "Gate 011 lock payload missing route marker");
  Require(Contains(artifacts.envelope.payload, "\"mga_visibility_impact\":false"),
          "Gate 011 lock payload missing no-visibility proof");
  Require(Contains(artifacts.envelope.payload, "\"transaction_finality_impact\":false"),
          "Gate 011 lock payload missing no-finality proof");
  Require(Contains(artifacts.envelope.payload, "\"cleanup_horizon_pinned\":false"),
          "Gate 011 lock payload missing cleanup proof");
  Require(Contains(artifacts.envelope.payload, "\"parser_executes_sql\":false"),
          "Gate 011 lock payload allowed parser SQL execution");
  Require(!Contains(artifacts.envelope.payload, sql),
          "Gate 011 lock payload embedded source SQL text");
  Require(!Contains(artifacts.envelope.payload, "WAL") &&
              !Contains(artifacts.envelope.payload, "wal"),
          "Gate 011 lock payload carried WAL authority");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "Gate 011 server admission rejected lock route");
  Require(admission.requires_public_abi_dispatch,
          "Gate 011 server admission did not require public ABI dispatch");
  Require(admission.operation_id == operation_id,
          "Gate 011 server admission operation mismatch");

  const auto* registry_row = sblr::LookupSblrOperation(operation_id);
  Require(registry_row != nullptr, "Gate 011 opcode registry row missing");
  Require(registry_row->opcode == opcode, "Gate 011 opcode registry drifted");
  Require(registry_row->requires_security_context,
          "Gate 011 opcode registry must require security context");
  Require(registry_row->requires_transaction_context,
          "Gate 011 opcode registry must require transaction context");
  return artifacts;
}

sblr::SblrDispatchResult DispatchLockRoute(
    const api::EngineRequestContext& context,
    const PipelineArtifacts& artifacts) {
  auto result = sblr::DispatchSblrOperation(
      {context, EngineEnvelopeFromParser(artifacts.envelope), api::EngineApiRequest{}});
  PrintDispatchDiagnostics(result);
  Require(result.envelope_validated, "Gate 011 engine envelope rejected");
  Require(result.accepted, "Gate 011 dispatch refused public lock route");
  Require(result.dispatched_to_api, "Gate 011 dispatch did not call engine API");
  return result;
}

api::EngineLockNamedResult LockNamedDirect(api::EngineRequestContext context,
                                           std::string_view name,
                                           std::string_view scope = "local") {
  api::EngineLockNamedRequest request;
  request.context = std::move(context);
  request.option_envelopes.push_back("lock_surface:LOCK NAMED");
  request.option_envelopes.push_back("lock_descriptor:" + std::string(name));
  request.option_envelopes.push_back("nowait:true");
  request.option_envelopes.push_back("lock_scope:" + std::string(scope));
  request.option_envelopes.push_back("cluster_scope_requested:" +
                                     std::string(scope == "cluster" ? "true" : "false"));
  return api::EngineLockNamed(request);
}

api::EngineUnlockNamedResult UnlockNamedDirect(api::EngineRequestContext context,
                                               std::string_view name) {
  api::EngineUnlockNamedRequest request;
  request.context = std::move(context);
  request.option_envelopes.push_back("lock_surface:UNLOCK NAMED");
  request.option_envelopes.push_back("lock_descriptor:" + std::string(name));
  return api::EngineUnlockNamed(request);
}

api::EngineLockTableResult LockTableDirect(api::EngineRequestContext context,
                                           std::string_view target,
                                           std::string_view mode,
                                           bool engine_owned_fence) {
  api::EngineLockTableRequest request;
  request.context = std::move(context);
  request.option_envelopes.push_back("lock_surface:LOCK TABLE");
  request.option_envelopes.push_back("lock_descriptor:" + std::string(target));
  request.option_envelopes.push_back("lock_mode:" + std::string(mode));
  request.option_envelopes.push_back("nowait:true");
  request.option_envelopes.push_back(
      std::string("engine_owned_admission_fence:") +
      (engine_owned_fence ? "true" : "false"));
  return api::EngineLockTable(request);
}

api::EngineCreateSavepointResult CreateSavepointDirect(
    api::EngineRequestContext context,
    std::string_view name) {
  api::EngineCreateSavepointRequest request;
  request.context = std::move(context);
  request.option_envelopes.push_back("savepoint_name:" + std::string(name));
  return api::EngineCreateSavepoint(request);
}

api::EngineRollbackToSavepointResult RollbackToSavepointDirect(
    api::EngineRequestContext context,
    std::string_view name) {
  api::EngineRollbackToSavepointRequest request;
  request.context = std::move(context);
  request.option_envelopes.push_back("savepoint_name:" + std::string(name));
  return api::EngineRollbackToSavepoint(request);
}

void RequireTableLockRoutes(const std::filesystem::path& path,
                            const std::string& database_uuid) {
  auto context = BeginEngineTransaction(
      path,
      database_uuid,
      "sbsql-missing-gate-011-table",
      "019f0000-0000-7000-8000-000000011301");

  const auto share = RequireLockRoute(
      "LOCK TABLE ONLY accounts, public.orders IN SHARE MODE WAIT 5;",
      "transaction.lock_table",
      "SBLR_TXN_LOCK_TABLE",
      "EngineLockTable");
  Require(Contains(share.envelope.payload, "\"lock_timeout_millis\":\"5\""),
          "Gate 011 LOCK TABLE WAIT operand missing");
  auto share_result = DispatchLockRoute(context, share);
  Require(share_result.api_result.ok, "Gate 011 share table lock no-op failed");
  Require(HasEvidence(share_result.api_result, "lock_policy", "compatibility_noop"),
          "Gate 011 share table lock policy evidence missing");
  Require(HasEvidence(share_result.api_result, "lock_decision", "granted_noop"),
          "Gate 011 share table lock no-op decision missing");
  Require(HasEvidence(share_result.api_result, "mga_visibility_impact", "false"),
          "Gate 011 share table lock changed MGA visibility");
  Require(HasEvidence(share_result.api_result, "transaction_finality_impact", "false"),
          "Gate 011 share table lock changed finality");
  Require(HasEvidence(share_result.api_result, "cleanup_horizon_pinned", "false"),
          "Gate 011 share table lock pinned cleanup horizon");

  const auto skip = RequireLockRoute("LOCK accounts SKIP LOCKED;",
                                     "transaction.lock_table",
                                     "SBLR_TXN_LOCK_TABLE",
                                     "EngineLockTable");
  Require(Contains(skip.envelope.payload, "\"skip_locked\":true"),
          "Gate 011 LOCK optional TABLE/SKIP LOCKED payload missing");
  auto skip_result = DispatchLockRoute(context, skip);
  Require(skip_result.api_result.ok,
          "Gate 011 SKIP LOCKED compatibility no-op failed");
  Require(HasEvidence(skip_result.api_result, "lock_policy", "compatibility_noop"),
          "Gate 011 SKIP LOCKED compatibility policy evidence missing");

  const auto exclusive = RequireLockRoute(
      "LOCK TABLE accounts IN ACCESS EXCLUSIVE MODE NOWAIT;",
      "transaction.lock_table",
      "SBLR_TXN_LOCK_TABLE",
      "EngineLockTable");
  auto exclusive_result = DispatchLockRoute(context, exclusive);
  Require(!exclusive_result.api_result.ok,
          "Gate 011 exclusive table lock was not refused by default");
  Require(HasDiagnostic(exclusive_result.api_result, "TCL.LOCK_NOT_AVAILABLE"),
          "Gate 011 exclusive table lock refusal diagnostic missing");
  Require(HasEvidence(exclusive_result.api_result, "mga_visibility_impact", "false"),
          "Gate 011 refused table lock changed MGA visibility");
  Require(HasEvidence(exclusive_result.api_result, "transaction_finality_impact", "false"),
          "Gate 011 refused table lock changed finality");

  auto fence_context = context;
  fence_context.trace_tags.push_back("right:CATALOG_MUTATE");
  fence_context.trace_tags.push_back("engine_owned_ddl_admission_fence_authorized");
  const auto fence_result =
      LockTableDirect(fence_context, "accounts", "write_or_exclusive", true);
  Require(fence_result.ok, "Gate 011 authorized engine-owned fence was refused");
  Require(fence_result.admission_fence,
          "Gate 011 authorized engine-owned fence flag missing");
  Require(HasEvidence(fence_result, "lock_policy", "engine_owned_admission_fence"),
          "Gate 011 authorized engine-owned fence policy evidence missing");
  Require(HasEvidence(fence_result, "lock_decision", "admitted_fence"),
          "Gate 011 authorized engine-owned fence decision evidence missing");
  Require(HasEvidence(fence_result, "ordinary_reader_blocking", "false"),
          "Gate 011 authorized engine-owned fence blocks ordinary readers");
  Require(HasEvidence(fence_result, "mga_visibility_impact", "false"),
          "Gate 011 authorized engine-owned fence changed MGA visibility");
  Require(HasEvidence(fence_result, "transaction_finality_impact", "false"),
          "Gate 011 authorized engine-owned fence changed finality");
  Require(HasEvidence(fence_result, "mga_cleanup_horizon_impact", "false"),
          "Gate 011 authorized engine-owned fence pinned cleanup horizon");

  const auto unlock = RequireLockRoute("UNLOCK TABLE ONLY accounts;",
                                       "transaction.unlock_table",
                                       "SBLR_TXN_UNLOCK_TABLE",
                                       "EngineUnlockTable");
  auto unlock_result = DispatchLockRoute(context, unlock);
  Require(unlock_result.api_result.ok, "Gate 011 unlock table no-op failed");
  Require(HasEvidence(unlock_result.api_result, "release_outcome",
                      "noop_no_table_lock_held"),
          "Gate 011 unlock table no-op evidence missing");

  const auto commit = CommitEngineTransaction(context);
  Require(HasEvidence(commit, "transaction_advisory_locks_released", "0"),
          "Gate 011 table compatibility no-op retained advisory locks");
}

void RequireNamedLockRoutesAndPolicy(const std::filesystem::path& path,
                                     const std::string& database_uuid) {
  auto parser_context = BeginEngineTransaction(
      path,
      database_uuid,
      "sbsql-missing-gate-011-parser-named",
      "019f0000-0000-7000-8000-000000011401");

  const auto parser_lock = RequireLockRoute(
      "LOCK NAMED 'gate011_parser_named' IN EXCLUSIVE MODE NOWAIT;",
      "transaction.lock_named",
      "SBLR_TXN_LOCK_NAMED",
      "EngineLockNamed");
  auto parser_lock_result = DispatchLockRoute(parser_context, parser_lock);
  Require(parser_lock_result.api_result.ok,
          "Gate 011 parser named lock dispatch failed");
  Require(HasEvidence(parser_lock_result.api_result, "lock_policy", "advisory_lock"),
          "Gate 011 parser named lock policy evidence missing");
  Require(HasEvidence(parser_lock_result.api_result, "mga_visibility_impact", "false"),
          "Gate 011 parser named lock changed MGA visibility");

  const auto parser_unlock = RequireLockRoute(
      "UNLOCK NAMED 'gate011_parser_named';",
      "transaction.unlock_named",
      "SBLR_TXN_UNLOCK_NAMED",
      "EngineUnlockNamed");
  auto parser_unlock_result = DispatchLockRoute(parser_context, parser_unlock);
  Require(parser_unlock_result.api_result.ok,
          "Gate 011 parser named unlock dispatch failed");
  Require(HasEvidence(parser_unlock_result.api_result, "release_outcome", "released"),
          "Gate 011 parser named unlock release evidence missing");
  CommitEngineTransaction(parser_context);

  auto savepoint_owner = BeginEngineTransaction(
      path,
      database_uuid,
      "sbsql-missing-gate-011-savepoint-owner",
      "019f0000-0000-7000-8000-000000011407");
  auto savepoint_contender = BeginEngineTransaction(
      path,
      database_uuid,
      "sbsql-missing-gate-011-savepoint-contender",
      "019f0000-0000-7000-8000-000000011408");
  constexpr std::string_view savepoint_name = "gate011_sp";
  constexpr std::string_view savepoint_lock_name = "gate011_savepoint_retained_lock";
  const auto savepoint_create = CreateSavepointDirect(savepoint_owner, savepoint_name);
  Require(savepoint_create.ok,
          "Gate 011 savepoint create failed before advisory lock retention proof");
  Require(HasEvidence(savepoint_create, "mga_savepoint", "savepoint_create"),
          "Gate 011 savepoint create evidence missing");
  const auto savepoint_lock = LockNamedDirect(savepoint_owner, savepoint_lock_name);
  Require(savepoint_lock.ok && savepoint_lock.acquired,
          "Gate 011 savepoint advisory lock acquire failed");
  const auto savepoint_rollback =
      RollbackToSavepointDirect(savepoint_owner, savepoint_name);
  Require(savepoint_rollback.ok,
          "Gate 011 rollback-to-savepoint failed during lock retention proof");
  Require(HasEvidence(savepoint_rollback, "mga_savepoint", "savepoint_rollback"),
          "Gate 011 rollback-to-savepoint evidence missing");
  const auto savepoint_retained_contention =
      LockNamedDirect(savepoint_contender, savepoint_lock_name);
  Require(!savepoint_retained_contention.ok,
          "Gate 011 savepoint rollback released advisory lock unexpectedly");
  Require(HasDiagnostic(savepoint_retained_contention, "TCL.LOCK_TIMEOUT"),
          "Gate 011 savepoint-retained lock timeout diagnostic missing");
  const auto savepoint_release =
      UnlockNamedDirect(savepoint_owner, savepoint_lock_name);
  Require(savepoint_release.ok && savepoint_release.released,
          "Gate 011 savepoint-retained lock explicit release failed");
  const auto savepoint_after_release =
      LockNamedDirect(savepoint_contender, savepoint_lock_name);
  Require(savepoint_after_release.ok && savepoint_after_release.acquired,
          "Gate 011 savepoint-retained lock unavailable after explicit release");
  RollbackEngineTransaction(savepoint_contender);
  RollbackEngineTransaction(savepoint_owner);

  auto first = BeginEngineTransaction(
      path,
      database_uuid,
      "sbsql-missing-gate-011-contention-1",
      "019f0000-0000-7000-8000-000000011402");
  auto second = BeginEngineTransaction(
      path,
      database_uuid,
      "sbsql-missing-gate-011-contention-2",
      "019f0000-0000-7000-8000-000000011403");
  constexpr std::string_view contention_name = "gate011_contention_named";
  const auto first_lock = LockNamedDirect(first, contention_name);
  Require(first_lock.ok && first_lock.acquired,
          "Gate 011 direct named lock acquire failed");
  Require(HasEvidence(first_lock, "lock_policy", "advisory_lock"),
          "Gate 011 direct named lock policy evidence missing");

  const auto second_lock = LockNamedDirect(second, contention_name);
  Require(!second_lock.ok && !second_lock.acquired,
          "Gate 011 contended named lock was not refused");
  Require(HasDiagnostic(second_lock, "TCL.LOCK_TIMEOUT"),
          "Gate 011 contended named lock timeout diagnostic missing");
  Require(HasEvidence(second_lock, "mga_visibility_impact", "false"),
          "Gate 011 contended named lock changed MGA visibility");

  const auto second_unlock = UnlockNamedDirect(second, contention_name);
  Require(second_unlock.ok && !second_unlock.released,
          "Gate 011 non-owner unlock should be no-op");
  Require(HasEvidence(second_unlock, "release_outcome", "noop_not_owned"),
          "Gate 011 non-owner unlock evidence missing");

  const auto first_unlock = UnlockNamedDirect(first, contention_name);
  Require(first_unlock.ok && first_unlock.released,
          "Gate 011 owner unlock failed");
  Require(HasEvidence(first_unlock, "release_outcome", "released"),
          "Gate 011 owner unlock evidence missing");

  const auto second_after_release = LockNamedDirect(second, contention_name);
  Require(second_after_release.ok && second_after_release.acquired,
          "Gate 011 named lock was not acquirable after release");
  const auto rollback_second = RollbackEngineTransaction(second);
  Require(HasEvidence(rollback_second, "transaction_advisory_locks_released", "1"),
          "Gate 011 rollback did not release named advisory lock");

  auto cleanup = BeginEngineTransaction(
      path,
      database_uuid,
      "sbsql-missing-gate-011-cleanup",
      "019f0000-0000-7000-8000-000000011404");
  constexpr std::string_view cleanup_name = "gate011_transaction_end_cleanup";
  const auto cleanup_lock = LockNamedDirect(cleanup, cleanup_name);
  Require(cleanup_lock.ok && cleanup_lock.acquired,
          "Gate 011 cleanup named lock acquire failed");
  const auto cleanup_commit = CommitEngineTransaction(cleanup);
  Require(HasEvidence(cleanup_commit, "transaction_advisory_locks_released", "1"),
          "Gate 011 commit did not release named advisory lock");

  auto post_cleanup = BeginEngineTransaction(
      path,
      database_uuid,
      "sbsql-missing-gate-011-post-cleanup",
      "019f0000-0000-7000-8000-000000011405");
  const auto post_cleanup_lock = LockNamedDirect(post_cleanup, cleanup_name);
  Require(post_cleanup_lock.ok && post_cleanup_lock.acquired,
          "Gate 011 named lock was not released at transaction end");
  RollbackEngineTransaction(post_cleanup);

  auto cluster_context = BeginEngineTransaction(
      path,
      database_uuid,
      "sbsql-missing-gate-011-cluster",
      "019f0000-0000-7000-8000-000000011406");
  const auto cluster_lock = LockNamedDirect(cluster_context,
                                           "gate011_cluster_scope",
                                           "cluster");
  Require(!cluster_lock.ok, "Gate 011 cluster named lock was admitted without authority");
  Require(HasDiagnostic(cluster_lock, "CLUSTER.AUTHORITY_REQUIRED"),
          "Gate 011 cluster authority refusal diagnostic missing");
  Require(cluster_lock.cluster_authority_required,
          "Gate 011 cluster authority flag missing");
  RollbackEngineTransaction(cluster_context);

  RollbackEngineTransaction(first);
}

SblrValue TextValue(std::string descriptor, std::string input) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::text;
  value.is_null = false;
  value.encoded_value = std::move(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue Int64Value(std::int64_t input) {
  SblrValue value;
  value.descriptor_id = "int64";
  value.payload_kind = SblrValuePayloadKind::signed_integer;
  value.is_null = false;
  value.has_int64_value = true;
  value.int64_value = input;
  value.encoded_value = std::to_string(input);
  value.text_value = value.encoded_value;
  return value;
}

SblrValue NullValue(std::string descriptor) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor);
  value.payload_kind = SblrValuePayloadKind::none;
  value.is_null = true;
  return value;
}

sblr::SblrExecutionContext FunctionContext(std::string session_uuid) {
  sblr::SblrExecutionContext context;
  context.database_uuid = "019f0000-0000-7000-8000-000000011501";
  context.session_uuid = std::move(session_uuid);
  context.user_uuid = "019f0000-0000-7000-8000-000000011502";
  context.transaction_uuid = "019f0000-0000-7000-8000-000000011503";
  context.local_transaction_id = 11503;
  context.transaction_context_present = true;
  context.security_context_present = true;
  context.statement_uuid = std::string(kStatementUuid);
  context.application_name = "sbsql-missing-gate-011";
  return context;
}

sblr::SblrResult RunFunction(const fn::FunctionRegistry& registry,
                             const sblr::SblrExecutionContext& context,
                             std::string function_id,
                             std::vector<SblrValue> values) {
  fn::FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.sblr_context = context;
  for (std::size_t index = 0; index < values.size(); ++index) {
    request.arguments.push_back(fn::FunctionArgument{"arg" + std::to_string(index),
                                                     std::move(values[index])});
  }
  return fn::DispatchFunctionCall(registry, std::move(request)).result;
}

void RequireInt64Result(std::string_view label,
                        const sblr::SblrResult& result,
                        std::int64_t expected) {
  if (!result.ok()) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.diagnostic_id << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok(), "Gate 011 function result failed");
  Require(result.scalar_values.size() == 1, "Gate 011 function result arity drifted");
  const auto& value = result.scalar_values.front();
  if (value.is_null || value.descriptor_id != "int64" || !value.has_int64_value ||
      value.int64_value != expected) {
    std::cerr << label << " expected int64 " << expected << ", got "
              << value.descriptor_id << ' ' << value.encoded_value << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void RequireNullInt64(std::string_view label, const sblr::SblrResult& result) {
  Require(result.ok(), "Gate 011 null function result failed");
  Require(result.scalar_values.size() == 1, "Gate 011 null function arity drifted");
  const auto& value = result.scalar_values.front();
  if (!value.is_null || value.descriptor_id != "int64") {
    std::cerr << label << " expected null int64, got "
              << value.descriptor_id << ' ' << value.encoded_value << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool FunctionEvidenceContains(const sblr::SblrExecutionContext& context,
                              std::string_view fragment) {
  if (!context.session_runtime_state) return false;
  return std::any_of(
      context.session_runtime_state->advisory_lock_evidence.begin(),
      context.session_runtime_state->advisory_lock_evidence.end(),
      [fragment](const std::string& evidence) {
        return evidence.find(fragment) != std::string::npos;
      });
}

void RequireLockFunctionRuntimeAndLowering() {
  const auto package = fn::BuildStandardFunctionSeedPackage();
  Require(package.registry.Lookup("sb.scalar.get_lock") != nullptr,
          "Gate 011 get_lock function registry row missing");
  Require(package.registry.Lookup("sb.scalar.release_lock") != nullptr,
          "Gate 011 release_lock function registry row missing");

  auto context = FunctionContext("019f0000-0000-7000-8000-000000011601");
  RequireInt64Result("Gate 011 GET_LOCK acquire",
                     RunFunction(package.registry,
                                 context,
                                 "sb.scalar.get_lock",
                                 {TextValue("character", "gate011_function_lock"),
                                  Int64Value(0)}),
                     1);
  Require(FunctionEvidenceContains(context, "get_lock.acquired:"),
          "Gate 011 get_lock acquisition evidence missing");
  RequireInt64Result("Gate 011 GET_LOCK reentrant",
                     RunFunction(package.registry,
                                 context,
                                 "sb.scalar.get_lock",
                                 {TextValue("character", "gate011_function_lock"),
                                  Int64Value(0)}),
                     1);
  Require(FunctionEvidenceContains(context, "get_lock.reentrant:"),
          "Gate 011 get_lock reentrant evidence missing");

  auto other = context;
  other.session_uuid = "019f0000-0000-7000-8000-000000011602";
  RequireInt64Result("Gate 011 GET_LOCK contended",
                     RunFunction(package.registry,
                                 other,
                                 "sb.scalar.get_lock",
                                 {TextValue("character", "gate011_function_lock"),
                                  Int64Value(0)}),
                     0);
  Require(FunctionEvidenceContains(context, "get_lock.timeout:"),
          "Gate 011 get_lock timeout evidence missing");

  RequireInt64Result("Gate 011 RELEASE_LOCK decrement",
                     RunFunction(package.registry,
                                 context,
                                 "sb.scalar.release_lock",
                                 {TextValue("character", "gate011_function_lock")}),
                     1);
  RequireInt64Result("Gate 011 RELEASE_LOCK final",
                     RunFunction(package.registry,
                                 context,
                                 "sb.scalar.release_lock",
                                 {TextValue("character", "gate011_function_lock")}),
                     1);
  RequireInt64Result("Gate 011 RELEASE_LOCK not found",
                     RunFunction(package.registry,
                                 context,
                                 "sb.scalar.release_lock",
                                 {TextValue("character", "gate011_function_lock")}),
                     0);
  RequireNullInt64("Gate 011 GET_LOCK null name",
                   RunFunction(package.registry,
                               context,
                               "sb.scalar.get_lock",
                               {NullValue("character"), Int64Value(0)}));
  RequireNullInt64("Gate 011 RELEASE_LOCK null name",
                   RunFunction(package.registry,
                               context,
                               "sb.scalar.release_lock",
                               {NullValue("character")}));

  const auto get_lock = RunPipeline("SELECT GET_LOCK('gate011_projection', 0) AS got_lock");
  PrintMessages(get_lock);
  Require(!get_lock.cst.messages.has_errors(), "Gate 011 GET_LOCK CST failed");
  Require(!get_lock.ast.messages.has_errors(), "Gate 011 GET_LOCK AST failed");
  Require(get_lock.bound.bound, "Gate 011 GET_LOCK bind failed");
  Require(get_lock.verifier.admitted, "Gate 011 GET_LOCK verifier rejected route");
  Require(get_lock.envelope.operation_id == "query.evaluate_projection",
          "Gate 011 GET_LOCK projection operation drifted");
  Require(Contains(get_lock.envelope.payload,
                   "\"projection_0_function_id\":\"sb.scalar.get_lock\""),
          "Gate 011 GET_LOCK projection function id missing");
  Require(Contains(get_lock.envelope.payload,
                   "\"projection_0_function_arg_count\":\"2\""),
          "Gate 011 GET_LOCK projection arity missing");
  Require(!get_lock.envelope.parser_executes_sql,
          "Gate 011 GET_LOCK projection allowed parser SQL execution");

  const auto release_lock =
      RunPipeline("SELECT RELEASE_LOCK('gate011_projection') AS released_lock");
  PrintMessages(release_lock);
  Require(!release_lock.cst.messages.has_errors(), "Gate 011 RELEASE_LOCK CST failed");
  Require(!release_lock.ast.messages.has_errors(), "Gate 011 RELEASE_LOCK AST failed");
  Require(release_lock.bound.bound, "Gate 011 RELEASE_LOCK bind failed");
  Require(release_lock.verifier.admitted,
          "Gate 011 RELEASE_LOCK verifier rejected route");
  Require(release_lock.envelope.operation_id == "query.evaluate_projection",
          "Gate 011 RELEASE_LOCK projection operation drifted");
  Require(Contains(release_lock.envelope.payload,
                   "\"projection_0_function_id\":\"sb.scalar.release_lock\""),
          "Gate 011 RELEASE_LOCK projection function id missing");
  Require(Contains(release_lock.envelope.payload,
                   "\"projection_0_function_arg_count\":\"1\""),
          "Gate 011 RELEASE_LOCK projection arity missing");
}

void RequireSelectForUpdateCompatibilityEvidence() {
  const auto artifacts =
      RunPipeline("SELECT id FROM customer FOR UPDATE;",
                  {"019f0000-0000-7000-8000-000000011701"});
  PrintMessages(artifacts);
  Require(!artifacts.cst.messages.has_errors(), "Gate 011 FOR UPDATE CST failed");
  Require(!artifacts.ast.messages.has_errors(), "Gate 011 FOR UPDATE AST failed");
  Require(artifacts.bound.bound, "Gate 011 FOR UPDATE bind failed");
  Require(artifacts.verifier.admitted, "Gate 011 FOR UPDATE verifier rejected route");
  Require(artifacts.envelope.operation_family == "sblr.dml.operation.v3",
          "Gate 011 FOR UPDATE must use DML operation family");
  Require(artifacts.envelope.operation_id == "dml.update_rows",
          "Gate 011 FOR UPDATE must lower to MGA update authority");
  Require(artifacts.envelope.sblr_opcode == "SBLR_DML_UPDATE_ROWS",
          "Gate 011 FOR UPDATE opcode drifted");
  Require(Contains(artifacts.envelope.payload, "SBSQL-728CB259DD81"),
          "Gate 011 FOR UPDATE row evidence missing");
  Require(Contains(artifacts.envelope.payload, "select_for_update"),
          "Gate 011 FOR UPDATE compatibility variant missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          "Gate 011 FOR UPDATE parser no-SQL authority missing");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          "Gate 011 FOR UPDATE parser no-finality authority missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "Gate 011 FOR UPDATE allowed parser SQL execution");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  RequireGeneratedRegistryRows();

  const auto path = TestDatabasePath();
  RemoveDatabaseArtifacts(path);
  const auto database_uuid = CreateMinimalDatabase(path);

  RequireTableLockRoutes(path, database_uuid);
  RequireNamedLockRoutesAndPolicy(path, database_uuid);
  RequireLockFunctionRuntimeAndLowering();
  RequireSelectForUpdateCompatibilityEvidence();

  RemoveDatabaseArtifacts(path);
  std::cout << "sbsql_missing_functionality_lock_mga_policy_conformance=passed\n";
  return EXIT_SUCCESS;
}
