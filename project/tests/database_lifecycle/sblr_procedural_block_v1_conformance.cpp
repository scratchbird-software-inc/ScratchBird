// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_procedural_block_runtime.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

bool HasDiagnostic(const api::EngineApiResult& result,
                   std::string_view diagnostic_code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == diagnostic_code) return true;
  }
  return false;
}

bool HasDiagnosticDetail(const api::EngineApiResult& result,
                         std::string_view detail_fragment) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.find(detail_fragment) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasRuntimeDiagnostic(const sblr::SblrResult& result,
                          std::string_view diagnostic_code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_id == diagnostic_code) return true;
  }
  return false;
}

bool HasEnvelopeDiagnostic(const sblr::SblrDispatchResult& result,
                           std::string_view diagnostic_code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == diagnostic_code) return true;
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == value) {
      return true;
    }
  }
  return false;
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string NewUuidText(UuidKind kind, std::uint64_t timestamp) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, timestamp);
  Require(generated.ok(), "procedural block test UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

struct DatabaseFixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;

  DatabaseFixture() = default;
  DatabaseFixture(const DatabaseFixture&) = delete;
  DatabaseFixture& operator=(const DatabaseFixture&) = delete;
  DatabaseFixture(DatabaseFixture&& other) noexcept
      : directory(std::move(other.directory)),
        database_path(std::move(other.database_path)),
        database_uuid(std::move(other.database_uuid)) {
    other.directory.clear();
  }
  DatabaseFixture& operator=(DatabaseFixture&&) = delete;

  ~DatabaseFixture() {
    if (directory.empty()) return;
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};

DatabaseFixture CreateDatabaseFixture() {
  const auto now = NowMillis();
  DatabaseFixture fixture;
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("sb_procedural_block_v1_" + std::to_string(now) + "_" +
                       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "fixture.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, now + 1).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 2).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = now + 3;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "procedural block test database creation failed");
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  return fixture;
}

api::EngineRequestContext BeginTransaction(const DatabaseFixture& fixture) {
  const auto now = NowMillis();
  api::EngineBeginTransactionRequest begin;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id = "sblr-procedural-block-v1";
  begin.context.database_path = fixture.database_path.string();
  begin.context.database_uuid.canonical = fixture.database_uuid;
  begin.context.principal_uuid.canonical =
      NewUuidText(UuidKind::object, now + 10);
  begin.context.session_uuid.canonical =
      NewUuidText(UuidKind::object, now + 11);
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch = 1;
  begin.context.name_resolution_epoch = 1;
  begin.isolation_level = "read_committed";

  const auto begun = api::EngineBeginTransaction(begin);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "procedural block test transaction begin failed");
  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  context.statement_timestamp = "2026-07-15T12:34:56.123456Z";
  context.transaction_timestamp = context.statement_timestamp;
  context.current_timestamp = context.statement_timestamp;
  return context;
}

void RollbackTransaction(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  const auto result = api::EngineRollbackTransaction(rollback);
  Require(result.ok,
          "procedural block execution changed or finalized the MGA transaction");
}

void CommitTransaction(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  const auto result = api::EngineCommitTransaction(commit);
  Require(result.ok,
          "procedural block finality-admission test commit failed");
}

std::vector<std::string> CommonOptions(std::uint32_t local_count,
                                       std::uint32_t output_count,
                                       std::uint32_t slot_count,
                                       std::uint32_t instruction_count) {
  return {
      "procedural_ir_contract:sblr.procedural.block.v1",
      "procedural_block_kind:anonymous",
      "procedural_input_count:0",
      "procedural_local_count:" + std::to_string(local_count),
      "procedural_output_count:" + std::to_string(output_count),
      "procedural_slot_count:" + std::to_string(slot_count),
      "procedural_instruction_count:" + std::to_string(instruction_count),
      "procedural_yield_count:0",
  };
}

std::vector<std::string> EmptyResultOptions(std::string type,
                                            bool nullable,
                                            std::uint32_t character_length = 0) {
  auto options = CommonOptions(0, 1, 1, 0);
  options.push_back("procedural_slot_0_id:result.0");
  options.push_back("procedural_slot_0_kind:result");
  options.push_back("procedural_slot_0_type:" + std::move(type));
  options.push_back(std::string("procedural_slot_0_nullable:") +
                    (nullable ? "true" : "false"));
  if (character_length != 0) {
    options.push_back("procedural_slot_0_character_length:" +
                      std::to_string(character_length));
  }
  return options;
}

std::vector<std::string> TimestampAssignmentOptions() {
  auto options = CommonOptions(1, 0, 1, 1);
  options.push_back("procedural_slot_0_id:local.0");
  options.push_back("procedural_slot_0_kind:local");
  options.push_back("procedural_slot_0_type:character");
  options.push_back("procedural_slot_0_character_length:100");
  options.push_back("procedural_slot_0_nullable:true");
  options.push_back("procedural_instruction_0_kind:assign");
  options.push_back("procedural_instruction_0_target_slot:local.0");
  options.push_back("procedural_instruction_0_expression_kind:substring");
  options.push_back(
      "procedural_instruction_0_source_kind:context_variable");
  options.push_back(
      "procedural_instruction_0_source_id:ctx_current_timestamp");
  options.push_back(
      "procedural_instruction_0_source_cast_type:character");
  options.push_back("procedural_instruction_0_start_kind:literal_int64");
  options.push_back("procedural_instruction_0_start_value:1");
  options.push_back("procedural_instruction_0_length_kind:to_end");
  return options;
}

sblr::SblrDispatchResult DispatchBlock(
    const api::EngineRequestContext& context,
    const std::vector<std::string>& options,
    std::string_view operand_type = "text") {
  auto envelope = sblr::MakeSblrEnvelope(
      "transaction.execute_block",
      "SBLR_TRANSACTION_EXECUTE_BLOCK",
      "SBLR_PROCEDURAL_BLOCK_V1_CONFORMANCE");
  // This implementation-only tuple deliberately has no canonical numeric
  // opcode.  Supply otherwise valid producer/registry identities so the
  // dispatch assertion below isolates that missing canonical identity rather
  // than failing first on an unrelated header omission.
  envelope.parser_package_uuid = context.session_uuid.canonical;
  envelope.registry_snapshot_uuid = context.database_uuid.canonical;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.contains_sql_text = false;
  for (const auto& option : options) {
    const auto separator = option.find(':');
    Require(separator != std::string::npos,
            "procedural block test option is malformed");
    envelope.operands.push_back(
        {std::string(operand_type), option.substr(0, separator),
         option.substr(separator + 1)});
  }
  api::EngineApiRequest api_request;
  api_request.context = context;
  api_request.operation_id = "transaction.execute_block";
  sblr::SblrDispatchRequest request{context, std::move(envelope),
                                    std::move(api_request)};
  return sblr::DispatchSblrOperation(std::move(request));
}

void TestStrictDecoder() {
  const auto empty_character =
      sblr::DecodeSblrProceduralBlockV1(
          EmptyResultOptions("character", true, 10));
  Require(empty_character.present && empty_character.valid,
          "valid empty character-result block was rejected");
  Require(empty_character.block.output_count == 1 &&
              empty_character.block.yield_count == 0,
          "empty result block decoded the wrong counts");

  const auto assignment = sblr::DecodeSblrProceduralBlockV1(
      TimestampAssignmentOptions());
  Require(assignment.present && assignment.valid &&
              assignment.block.instructions.size() == 1,
          "valid timestamp assignment block was rejected");

  auto duplicate = TimestampAssignmentOptions();
  duplicate.push_back("procedural_ir_contract:sblr.procedural.block.v1");
  const auto duplicate_result =
      sblr::DecodeSblrProceduralBlockV1(duplicate);
  Require(!duplicate_result.valid &&
              duplicate_result.diagnostic_code ==
                  "SB_SBLR_PROCEDURAL_IR_DUPLICATE_FIELD",
          "duplicate procedural field did not fail closed");

  auto nonzero_yield = EmptyResultOptions("int32", false);
  for (auto& option : nonzero_yield) {
    if (option == "procedural_yield_count:0") {
      option = "procedural_yield_count:1";
    }
  }
  Require(!sblr::DecodeSblrProceduralBlockV1(nonzero_yield).valid,
          "non-zero-yield v1 block was accepted");

  auto overflowing_yield = EmptyResultOptions("int32", false);
  for (auto& option : overflowing_yield) {
    if (option == "procedural_yield_count:0") {
      option = "procedural_yield_count:18446744073709551616";
    }
  }
  const auto overflowing_yield_result =
      sblr::DecodeSblrProceduralBlockV1(overflowing_yield);
  Require(!overflowing_yield_result.valid &&
              overflowing_yield_result.diagnostic_code ==
                  "SB_SBLR_PROCEDURAL_IR_FIELD_INVALID",
          "overflowing unsigned procedural count wrapped into the v1 range");

  auto unknown = EmptyResultOptions("int32", false);
  unknown.push_back("procedural_future_opcode:accept_me");
  const auto unknown_result = sblr::DecodeSblrProceduralBlockV1(unknown);
  Require(!unknown_result.valid &&
              unknown_result.diagnostic_code ==
                  "SB_SBLR_PROCEDURAL_IR_UNKNOWN_FIELD",
          "unknown procedural field did not fail closed");

  auto source_payload = TimestampAssignmentOptions();
  source_payload.push_back("source_sql:execute block as begin end");
  const auto source_payload_result =
      sblr::DecodeSblrProceduralBlockV1(source_payload);
  Require(!source_payload_result.valid &&
              source_payload_result.diagnostic_code ==
                  "SB_SBLR_PROCEDURAL_SOURCE_PAYLOAD_FORBIDDEN",
          "source SQL payload was accepted by procedural IR");

  const auto source_only_result = sblr::DecodeSblrProceduralBlockV1(
      {"source_sql:execute block as begin end"});
  Require(source_only_result.present && !source_only_result.valid &&
              source_only_result.diagnostic_code ==
                  "SB_SBLR_PROCEDURAL_SOURCE_PAYLOAD_FORBIDDEN",
          "source-only SQL payload fell through as a contract-absent block");
}

void TestRuntimeSemantics() {
  const auto not_null = sblr::DecodeSblrProceduralBlockV1(
      EmptyResultOptions("int32", false));
  Require(not_null.valid, "NOT NULL empty result block failed to decode");
  sblr::SblrExecutionContext context;
  context.security_context_present = true;
  context.transaction_context_present = true;
  const auto empty_execution =
      sblr::ExecuteSblrProceduralBlockV1(not_null.block, context);
  Require(empty_execution.result.ok() && empty_execution.result.rows.empty(),
          "unassigned NOT NULL result was incorrectly validated without yield");
  Require(empty_execution.assignment_frame.slots.size() == 1 &&
              !empty_execution.assignment_frame.slots.front().allow_null &&
              !empty_execution.assignment_frame.slots.front().assigned,
          "NOT NULL result slot did not remain unassigned and non-nullable");

  const auto assignment = sblr::DecodeSblrProceduralBlockV1(
      TimestampAssignmentOptions());
  Require(assignment.valid, "timestamp assignment block failed to decode");
  context.current_timestamp = "2026-07-15T12:34:56.123456Z";
  const auto assigned =
      sblr::ExecuteSblrProceduralBlockV1(assignment.block, context);
  Require(assigned.result.ok() && assigned.result.rows.empty() &&
              assigned.instructions_executed == 1,
          "timestamp substring assignment was not executed exactly once");
  Require(assigned.assignment_frame.slots.size() == 1 &&
              assigned.assignment_frame.slots.front().assigned &&
              assigned.assignment_frame.slots.front().value.text_value ==
                  context.current_timestamp,
          "timestamp substring assignment produced the wrong local value");

  context.current_timestamp.clear();
  const auto missing_timestamp =
      sblr::ExecuteSblrProceduralBlockV1(assignment.block, context);
  Require(!missing_timestamp.result.ok() &&
              missing_timestamp.instructions_executed == 0 &&
              HasRuntimeDiagnostic(missing_timestamp.result,
                                   "SB_DIAG_CONTEXT_VARIABLE_UNAVAILABLE"),
          "missing engine timestamp did not prove real expression execution");

  auto decoder_bypass = not_null.block;
  decoder_bypass.yield_count = 1;
  const auto invalid_runtime =
      sblr::ExecuteSblrProceduralBlockV1(decoder_bypass, context);
  Require(!invalid_runtime.result.ok() &&
              HasRuntimeDiagnostic(
                  invalid_runtime.result,
                  "SB_SBLR_PROCEDURAL_IR_RUNTIME_CONTRACT_INVALID"),
          "typed procedural POD bypassed the runtime contract guard");
}

void TestDispatchRefusalAndMgaNonMutation() {
  const auto fixture = CreateDatabaseFixture();
  const auto context = BeginTransaction(fixture);

  const auto assigned = DispatchBlock(context, TimestampAssignmentOptions());
  Require(!assigned.envelope_validated && !assigned.accepted &&
              !assigned.dispatched_to_api && !assigned.api_result.ok &&
              HasEnvelopeDiagnostic(
                  assigned, "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH"),
          "unallocated procedural block SBOP did not fail closed before API dispatch");
  Require(assigned.api_result.result_shape.rows.empty() &&
              assigned.api_result.evidence.empty(),
          "refused procedural block published rows or execution evidence");

  auto invalid_options = EmptyResultOptions("int32", false);
  invalid_options.push_back("procedural_unknown_contract_field:no");
  const auto invalid = DispatchBlock(context, invalid_options);
  Require(!invalid.envelope_validated && !invalid.dispatched_to_api &&
              HasEnvelopeDiagnostic(
                  invalid, "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH"),
          "invalid procedural IR bypassed the unallocated-opcode refusal");

  const auto wrong_operand_type = DispatchBlock(
      context, EmptyResultOptions("int32", false), "assignment");
  Require(!wrong_operand_type.envelope_validated &&
              !wrong_operand_type.dispatched_to_api &&
              HasEnvelopeDiagnostic(
                  wrong_operand_type,
                  "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH"),
          "non-text procedural operand bypassed the unallocated-opcode refusal");

  const auto legacy = DispatchBlock(context, {});
  Require(!legacy.envelope_validated && !legacy.dispatched_to_api &&
              HasEnvelopeDiagnostic(
                  legacy, "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH"),
          "contract-absent procedural SBOP reached a legacy execution route");

  const auto source_only = DispatchBlock(
      context, {"source_sql:execute block as begin end"});
  Require(!source_only.envelope_validated &&
              !source_only.dispatched_to_api &&
              source_only.api_result.result_shape.rows.empty(),
          "source-only procedural payload reached a legacy execution route");

  // Every refusal above occurs before MGA dispatch.  A successful rollback is
  // the independent postcondition that the active transaction was neither
  // finalized nor replaced by the rejected envelopes.
  RollbackTransaction(context);

  const auto committed_context = BeginTransaction(fixture);
  const auto before_commit =
      DispatchBlock(committed_context, TimestampAssignmentOptions());
  Require(!before_commit.dispatched_to_api && !before_commit.api_result.ok,
          "procedural refusal unexpectedly mutated the commit candidate");
  CommitTransaction(committed_context);
}

}  // namespace

int main() {
  TestStrictDecoder();
  TestRuntimeSemantics();
  TestDispatchRefusalAndMgaNonMutation();
  std::cout << "sblr procedural block v1 conformance: ok\n";
  return EXIT_SUCCESS;
}
