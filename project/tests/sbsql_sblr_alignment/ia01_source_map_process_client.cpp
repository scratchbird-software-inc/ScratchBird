// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// CSC-TEST-002337: explicit authenticated SOURCE_MAP process client.
#include "common/common.hpp"
#include "engine/sblr/sblr_stmt_execute_runtime.hpp"
#include "engine/sblr/sblr_stmt_execute_direct_runtime.hpp"
#include "engine/sblr/sblr_stmt_free_runtime.hpp"
#include "engine/sblr/sblr_stmt_cancel_runtime.hpp"
#include "engine/sblr/sblr_stmt_prepare_runtime.hpp"
#include "engine/sblr/sblr_parameter_bind_runtime.hpp"
#include "engine/sblr/sblr_result_page_runtime.hpp"
#include "engine/sblr/sblr_catalog_introspect_runtime.hpp"
#include "engine/sblr/sblr_name_resolve_runtime.hpp"
#include "engine/sblr/sblr_optimizer_stats_drop_runtime.hpp"
#include "engine/sblr/sblr_optimizer_stats_read_runtime.hpp"
#include "engine/sblr/sblr_parse_text_runtime.hpp"
#include "engine/sblr/sblr_catalog_epoch_check_runtime.hpp"
#include "engine/sblr/sblr_database_attach_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_schema_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_trigger_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_trigger_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_trigger_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_procedure_runtime.hpp"
#include "engine/sblr/sblr_procedure_invoke_runtime.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "engine/sblr/sblr_savepoint_runtime.hpp"
#include "engine/sblr/sblr_sec_alter_policy_runtime.hpp"
#include "engine/sblr/sblr_source_artifact_runtime.hpp"
#include "engine/sblr/sblr_to_sbsql.hpp"
#include "ast/ast.hpp"
#include "cst/cst.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "engine/internal_api/sblr_ddl_create_procedure_coordinator.hpp"
#include "engine/internal_api/sblr_procedure_invoke_coordinator.hpp"
#include "wire/sbsql_test_wire.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 7) {
    std::cerr << "usage: descriptor-client ENDPOINT DATABASE USER PASSWORD OPERATION APPLICATION\n";
    return 2;
  }
  const std::string operation = argv[5];
  scratchbird::parser::sbsql::ParserConfig config;
  config.server_endpoint = argv[1];
  config.database_token = argv[2];
  if (operation == "security-policy-show-budget") {
    config.resource_budget.max_statement_bytes = 24;
  }
  scratchbird::parser::sbsql::SbsqlTestWireSession session(config, nullptr, nullptr);
  scratchbird::parser::sbsql::AuthCredentialEnvelope credentials;
  credentials.provider_family = "local_password";
  credentials.principal = argv[3];
  credentials.requested_database = argv[2];
  credentials.application_name = argv[6];
  credentials.credential_evidence = argv[4];
  credentials.credential_evidence_present = true;
  scratchbird::parser::sbsql::MessageVectorSet messages;
  if (!session.AuthenticateCredentials(credentials, &messages)) {
    for (const auto& diagnostic : messages.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    return 3;
  }
  if (operation == "security-visibility-parent") {
    const auto dump_failure = [](std::string_view phase,
                                 const auto& failed) {
      std::cerr << "CSC-TEST-005826 SECURITY_VISIBILITY_PARENT " << phase
                << " accepted=" << failed.accepted
                << " operation=" << failed.server_operation_id
                << " row_count=" << failed.server_row_count
                << " result=" << failed.server_result_payload << '\n';
      for (const auto& diagnostic : failed.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
    };
    const auto require_projection = [&](std::string_view sql,
                                        std::string_view field,
                                        bool expected) {
      auto projected = session.RunPipeline(sql, true);
      const std::string expected_row =
          "row[0]=" + std::string(field) + "=" +
          (expected ? "1" : "0");
      const std::string expected_metadata =
          "row_meta[0]=" + std::string(field) + ":boolean:not_null";
      const bool exact =
          projected.accepted && !projected.outcome_unknown &&
          !projected.messages.has_errors() &&
          projected.server_operation_id == "query.evaluate_projection" &&
          projected.server_cursor_uuid.empty() &&
          projected.server_row_count == 1 &&
          projected.server_result_payload.find(
              "operation_id=query.evaluate_projection") !=
              std::string::npos &&
          projected.server_result_payload.find(expected_row) !=
              std::string::npos &&
          projected.server_result_payload.find(expected_metadata) !=
              std::string::npos &&
          projected.sblr_payload.find("security.evaluate_visibility") ==
              std::string::npos &&
          projected.sblr_payload.find(sql) == std::string::npos;
      if (!exact) dump_failure(field, projected);
      return exact;
    };

    auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
    if (!begun.accepted || begun.messages.has_errors()) {
      dump_failure("begin_failed", begun);
      return 4;
    }
    const bool table_allowed = require_projection(
        "SELECT has_table_privilege('app.customers', 'SELECT') "
        "AS table_allowed;",
        "table_allowed", true);
    const bool table_optional_user_allowed = require_projection(
        "SELECT has_table_privilege('current_user', 'app.customers', "
        "'SELECT') AS table_optional_user_allowed;",
        "table_optional_user_allowed", true);
    const bool column_allowed = require_projection(
        "SELECT has_column_privilege('app.customers', 'id', 'SELECT') "
        "AS column_allowed;",
        "column_allowed", true);
    const bool column_optional_user_allowed = require_projection(
        "SELECT has_column_privilege('current_user', 'app.customers', 'id', "
        "'SELECT') AS column_optional_user_allowed;",
        "column_optional_user_allowed", true);
    const bool function_allowed = require_projection(
        "SELECT has_function_privilege('has_table_privilege', 'EXECUTE') "
        "AS function_allowed;",
        "function_allowed", true);
    const bool function_optional_user_allowed = require_projection(
        "SELECT has_function_privilege('current_user', "
        "'has_table_privilege', 'EXECUTE') "
        "AS function_optional_user_allowed;",
        "function_optional_user_allowed", true);
    const bool schema_allowed = require_projection(
        "SELECT has_schema_privilege('app', 'USAGE') AS schema_allowed;",
        "schema_allowed", true);
    const bool schema_optional_user_allowed = require_projection(
        "SELECT has_schema_privilege('current_user', 'app', 'USAGE') "
        "AS schema_optional_user_allowed;",
        "schema_optional_user_allowed", true);
    const bool invalid_denied = require_projection(
        "SELECT has_table_privilege('app.customers', 'NOT_A_RIGHT') "
        "AS invalid_denied;",
        "invalid_denied", false);
    auto rolled_back = session.RunPipeline("ROLLBACK TRANSACTION", true);
    if (!rolled_back.accepted || rolled_back.messages.has_errors()) {
      dump_failure("rollback_failed", rolled_back);
      return 4;
    }
    if (!table_allowed || !table_optional_user_allowed || !column_allowed ||
        !column_optional_user_allowed || !function_allowed ||
        !function_optional_user_allowed || !schema_allowed ||
        !schema_optional_user_allowed || !invalid_denied) {
      return 4;
    }
    std::cout << "CSC-TEST-005826 SECURITY_VISIBILITY_PARENT accepted "
                 "canonical_sblr=true internal_evaluator_hidden=true "
                 "table=true column=true function=true schema=true "
                 "optional_user_signatures=true "
                 "invalid_right=false transaction_rolled_back=true\n";
    return 0;
  }
  if (operation == "security-alter-policy" ||
      operation == "security-alter-policy-observe" ||
      operation == "security-alter-policy-rollback" ||
      operation == "security-alter-policy-invalid" ||
      operation == "security-policy-show-budget") {
    namespace sblr = scratchbird::engine::sblr;
    constexpr std::string_view kPolicyUuid =
        "018f0a2b-0000-7000-9000-000000000701";
    const bool observer = operation == "security-alter-policy-observe";
    const bool rollback = operation == "security-alter-policy-rollback";
    const bool invalid = operation == "security-alter-policy-invalid";
    const bool budget = operation == "security-policy-show-budget";
    const char* artifact_path =
        std::getenv("SCRATCHBIRD_TEST_SECURITY_ALTER_POLICY_RESULT_ARTIFACT");
    const auto nonzero = [](const auto& value) {
      return std::ranges::any_of(
          value, [](const std::uint8_t byte) { return byte != 0; });
    };
    const auto dump_failure = [](std::string_view phase,
                                 const auto& failed) {
      std::cerr << "CSC-TEST-002989 SEC_ALTER_POLICY " << phase
                << " accepted=" << failed.accepted
                << " outcome_unknown=" << failed.outcome_unknown
                << " operation=" << failed.server_operation_id << '\n';
      for (const auto& diagnostic : failed.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
    };
    const auto read_artifact = [&]() {
      std::vector<std::uint8_t> bytes;
      if (artifact_path == nullptr || *artifact_path == '\0') return bytes;
      std::ifstream in(artifact_path, std::ios::binary);
      if (!in) return bytes;
      in.seekg(0, std::ios::end);
      const auto extent = in.tellg();
      if (extent <= 0) return bytes;
      bytes.resize(static_cast<std::size_t>(extent));
      in.seekg(0, std::ios::beg);
      in.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
      if (!in) bytes.clear();
      return bytes;
    };
    const auto write_artifact = [&](const std::string& bytes) {
      if (artifact_path == nullptr || *artifact_path == '\0' ||
          bytes.empty()) {
        return false;
      }
      std::ofstream out(artifact_path, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      return out.good();
    };

    sblr::SblrSecAlterPolicyResultV1 committed_terminal;
    std::string detail;
    const auto committed_bytes = read_artifact();
    const bool committed_terminal_available =
        sblr::DecodeSblrSecAlterPolicyResultV1(
            committed_bytes.data(), committed_bytes.size(),
            &committed_terminal, &detail);

    auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
    if (!begun.accepted || begun.messages.has_errors()) {
      dump_failure("begin_failed", begun);
      return 4;
    }

    if (budget) {
      auto refused = session.RunPipeline(
          "SHOW SECURITY POLICY app.app_policy;", true);
      const bool no_canonical_or_server_result =
          refused.sblr_payload.empty() &&
          refused.server_operation_id.empty() &&
          refused.server_cursor_uuid.empty() &&
          refused.server_row_count == 0 &&
          refused.server_affected_rows == 0 &&
          !refused.server_affected_rows_present &&
          refused.server_result_payload.empty();
      const bool exact_refusal =
          !refused.accepted && !refused.outcome_unknown &&
          no_canonical_or_server_result &&
          refused.messages.diagnostics.size() == 1 &&
          refused.messages.diagnostics.front().code ==
              "SBSQL.RESOURCE.STATEMENT_TOO_LARGE";
      auto ended = session.RunPipeline("ROLLBACK TRANSACTION", true);
      if (!exact_refusal || !ended.accepted || ended.messages.has_errors()) {
        dump_failure("resource_budget_refusal_failed", refused);
        return 4;
      }
      std::cout << "CSC-TEST-002164 SECURITY_POLICY_SHOW "
                   "budget_refused=true no_sblr=true "
                   "no_server_dispatch=true no_publication=true\n";
      return 0;
    }

    if (observer) {
      auto observed = session.RunPipeline(
          "SHOW SECURITY POLICY app.app_policy;", true);
      const std::string generation_marker =
          committed_terminal_available
              ? "policy_generation=" +
                    std::to_string(committed_terminal.generation)
              : std::string{};
      const bool exact_observer =
          committed_terminal_available && observed.accepted &&
          !observed.outcome_unknown && !observed.messages.has_errors() &&
          observed.server_operation_id == "security.policy.show" &&
          observed.server_row_count == 1 &&
          observed.server_result_payload.find(
              "policy_uuid=" + std::string(kPolicyUuid)) !=
              std::string::npos &&
          observed.server_result_payload.find("lifecycle_state=active") !=
              std::string::npos &&
          observed.server_result_payload.find(generation_marker) !=
              std::string::npos;
      auto ended = session.RunPipeline("ROLLBACK TRANSACTION", true);
      if (!exact_observer || !ended.accepted || ended.messages.has_errors()) {
        dump_failure(detail.empty() ? "observer_contract_failed" : detail,
                     observed);
        return 4;
      }
      std::cout << "CSC-TEST-002989 SEC_ALTER_POLICY "
                   "observer_active=true independent_session=true "
                   "exact_policy_identity=true exact_generation=true\n";
      return 0;
    }

    if (invalid) {
      auto refused = session.RunPipeline(
          "ACTIVATE POLICY app.missing_policy;", true);
      const bool exact_refusal =
          !refused.accepted && !refused.outcome_unknown &&
          refused.server_operation_id.empty() &&
          refused.server_result_payload.empty() &&
          refused.messages.diagnostics.size() == 1 &&
          refused.messages.diagnostics.front().code ==
              "CATALOG.NAME.NOT_FOUND_OR_NOT_VISIBLE";
      auto ended = session.RunPipeline("ROLLBACK TRANSACTION", true);
      if (!exact_refusal || !ended.accepted || ended.messages.has_errors()) {
        dump_failure("missing_policy_refusal_failed", refused);
        return 4;
      }
      std::cout << "CSC-TEST-002989 SEC_ALTER_POLICY "
                   "missing_policy_refused=true no_execution=true "
                   "no_publication=true\n";
      return 0;
    }

    auto altered = session.RunPipeline(
        "ACTIVATE POLICY app.app_policy;", true);
    sblr::SblrSecAlterPolicyResultV1 terminal;
    const auto canonical_container =
        scratchbird::engine::DecodeSblrContainerBytes(
            reinterpret_cast<const std::uint8_t*>(altered.sblr_payload.data()),
            altered.sblr_payload.size());
    const auto canonical_stream =
        canonical_container.status == scratchbird::engine::SblrCodecStatus::ok
            ? sblr::DecodeSblrOpcodeStream(std::string_view(
                  reinterpret_cast<const char*>(canonical_container.container
                                                    .operation_payload.data()),
                  canonical_container.container.operation_payload.size()))
            : sblr::SblrOpcodeStreamResult{};
    sblr::SblrSecAlterPolicyDescriptorV1 operand;
    const bool exact_submission =
        canonical_stream.ok &&
        scratchbird::engine::EncodeSblrContainer(
            canonical_container.container) ==
            std::vector<std::uint8_t>(altered.sblr_payload.begin(),
                                      altered.sblr_payload.end()) &&
        canonical_stream.stream.operations.size() == 3 &&
        canonical_stream.stream.operations[1].operation_id ==
            "engine.op.sec_alter_policy" &&
        canonical_stream.stream.operations[1].opcode ==
            "SBLR_SEC_ALTER_POLICY" &&
        canonical_stream.stream.operations[1].opcode_code == 1798 &&
        canonical_stream.stream.operations[1].operands.size() == 1 &&
        canonical_stream.stream.operations[1].operands[0].type ==
            "security_policy_descriptor" &&
        canonical_stream.stream.operations[1].operands[0].name == "policy" &&
        canonical_stream.stream.operations[1].operands[0].value_kind ==
            sblr::SblrValueKind::security_alter_policy_descriptor &&
        sblr::DecodeSblrSecAlterPolicyDescriptorV1(
            canonical_stream.stream.operations[1].operands[0]
                .value_body.data(),
            canonical_stream.stream.operations[1].operands[0]
                .value_body.size(),
            &operand, &detail, true);
    const bool exact_terminal =
        exact_submission && altered.accepted && !altered.outcome_unknown &&
        !altered.messages.has_errors() &&
        altered.server_operation_id == "engine.op.sec_alter_policy" &&
        altered.server_request_payload_bytes == 896 &&
        altered.server_row_count == 0 &&
        sblr::DecodeSblrSecAlterPolicyResultV1(
            reinterpret_cast<const std::uint8_t*>(
                altered.server_result_payload.data()),
            altered.server_result_payload.size(), &terminal, &detail) &&
        sblr::EncodeSblrSecAlterPolicyResultV1(terminal) ==
            std::vector<std::uint8_t>(altered.server_result_payload.begin(),
                                      altered.server_result_payload.end()) &&
        terminal.receipt == operand.receipt &&
        terminal.policy_uuid == operand.policy_uuid &&
        terminal.previous_generation == operand.expected_generation &&
        terminal.generation == terminal.previous_generation + 1 &&
        terminal.database_uuid == operand.database_uuid &&
        terminal.owning_transaction_uuid == operand.owning_transaction_uuid &&
        terminal.owning_local_transaction_id ==
            operand.owning_local_transaction_id &&
        terminal.statement_snapshot_uuid == operand.statement_snapshot_uuid &&
        terminal.cache_invalidation_generation == terminal.generation &&
        terminal.security_generation == terminal.generation &&
        terminal.action == 1 && terminal.status == 1 &&
        terminal.publication_barrier == 1 &&
        terminal.descriptor_evidence == operand.descriptor_evidence &&
        terminal.availability == operand.availability &&
        terminal.recovery_uuid == operand.recovery_uuid &&
        nonzero(terminal.mutation_uuid) && nonzero(terminal.effect_evidence) &&
        nonzero(terminal.result_evidence) &&
        nonzero(terminal.publication_barrier_uuid) &&
        terminal.mutation_uuid != terminal.policy_uuid &&
        terminal.publication_barrier_uuid != terminal.policy_uuid &&
        terminal.publication_barrier_uuid != terminal.mutation_uuid;
    if (!exact_terminal || !session.HasHeldSecurityAlterPolicyForWire()) {
      dump_failure(detail.empty() ? "terminal_contract_failed" : detail,
                   altered);
      return 4;
    }

    if (rollback) {
      if (!committed_terminal_available ||
          terminal.previous_generation != committed_terminal.generation) {
        dump_failure("rollback_predecessor_mismatch", altered);
        return 4;
      }
      auto ended = session.RunPipeline("ROLLBACK TRANSACTION", true);
      session.AcknowledgeSecurityAlterPolicyCompletionForWire();
      if (!ended.accepted || ended.messages.has_errors() ||
          session.HasHeldSecurityAlterPolicyForWire()) {
        dump_failure("rollback_failed", ended);
        return 4;
      }
      std::cout << "CSC-TEST-002989 SEC_ALTER_POLICY "
                   "rollback=true statement_publication=true "
                   "transaction_visibility_unchanged=true\n";
      return 0;
    }

    auto committed = session.RunPipeline("COMMIT TRANSACTION", true);
    if (!committed.accepted || committed.messages.has_errors() ||
        !write_artifact(altered.server_result_payload)) {
      dump_failure("commit_or_artifact_failed", committed);
      return 4;
    }
    session.AcknowledgeSecurityAlterPolicyCompletionForWire();
    if (session.HasHeldSecurityAlterPolicyForWire()) {
      std::cerr << "security_alter_policy_terminal_holder_not_released\n";
      return 4;
    }
    std::cout << "CSC-TEST-002989 SEC_ALTER_POLICY accepted "
                 "surface_id=SBSQL-5BC7985C2B11 canonical_sblr=true "
                 "policy_generation_incremented=true commit=true "
                 "publication_barrier=passed\n";
    return 0;
  }
  if (operation == "stmt-prepare-boundaries" ||
      operation == "stmt-execute-boundaries" ||
      operation == "stmt-free-boundaries") {
    namespace parser = scratchbird::parser::sbsql;
    namespace ipc = scratchbird::parser::ipc;
    namespace sblr = scratchbird::engine::sblr;

    const auto dump_result = [](std::string_view label,
                                const parser::PipelineResult& result) {
      std::cerr << label << " accepted=" << result.accepted
                << " outcome_unknown=" << result.outcome_unknown
                << " operation=" << result.server_operation_id << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
    };
    const auto no_canonical_result = [](const parser::PipelineResult& result) {
      return result.sblr_payload.empty() && result.server_operation_id.empty() &&
             result.server_cursor_uuid.empty() && result.server_row_count == 0 &&
             result.server_affected_rows == 0 &&
             !result.server_affected_rows_present &&
             result.server_result_payload.empty();
    };
    const auto exact_refusal = [&](const parser::PipelineResult& result,
                                   std::string_view code,
                                   std::string_view detail) {
      if (result.accepted || result.outcome_unknown ||
          result.messages.diagnostics.size() != 1 ||
          result.messages.diagnostics.front().code != code ||
          !no_canonical_result(result)) {
        return false;
      }
      const auto& fields = result.messages.diagnostics.front().fields;
      return std::ranges::any_of(fields, [&](const auto& field) {
        return field.name == "detail" && field.value == detail;
      });
    };
    const auto exact_budget_refusal = [&](const parser::PipelineResult& result) {
      return !result.accepted && !result.outcome_unknown &&
             result.messages.diagnostics.size() == 1 &&
             result.messages.diagnostics.front().code ==
                 "SBSQL.RESOURCE.STATEMENT_TOO_LARGE" &&
             no_canonical_result(result);
    };
    const auto exact_hidden_pair = [&](const parser::PipelineResult& hidden,
                                       const parser::PipelineResult& absent,
                                       std::string_view hidden_name,
                                       std::string_view absent_name) {
      if (hidden.accepted || absent.accepted || hidden.outcome_unknown ||
          absent.outcome_unknown || !no_canonical_result(hidden) ||
          !no_canonical_result(absent) ||
          hidden.messages.diagnostics.size() != 1 ||
          absent.messages.diagnostics.size() != 1 ||
          hidden.messages.diagnostics.front().code != "SECURITY.ACCESS_DENIED" ||
          absent.messages.diagnostics.front().code != "SECURITY.ACCESS_DENIED") {
        return false;
      }
      const auto hidden_json = ipc::MessageVectorToJson(hidden.messages);
      const auto absent_json = ipc::MessageVectorToJson(absent.messages);
      return hidden_json == absent_json &&
             hidden_json.find(hidden_name) == std::string::npos &&
             hidden_json.find(absent_name) == std::string::npos;
    };
    const auto exact_prepare = [](const parser::PipelineResult& result) {
      sblr::SblrStmtPrepareResultV1 decoded;
      std::string detail;
      return result.accepted && !result.messages.has_errors() &&
             result.server_operation_id == "engine.op.stmt_prepare" &&
             sblr::DecodeSblrStmtPrepareResultV1(
                 reinterpret_cast<const std::uint8_t*>(
                     result.server_result_payload.data()),
                 result.server_result_payload.size(), &decoded, &detail) &&
             decoded.status == 1 && decoded.publication_barrier == 1 &&
             decoded.prepared_generation != 0 &&
             decoded.executor_availability_generation != 0;
    };
    const auto exact_free = [](const parser::PipelineResult& result) {
      sblr::SblrStmtFreeResultV1 decoded;
      std::string detail;
      return result.accepted && !result.messages.has_errors() &&
             result.server_operation_id == "engine.op.stmt_free" &&
             sblr::DecodeSblrStmtFreeResultV1(
                 reinterpret_cast<const std::uint8_t*>(
                     result.server_result_payload.data()),
                 result.server_result_payload.size(), &decoded, &detail) &&
             decoded.terminal_state == 1 && decoded.publication_barrier == 1 &&
             decoded.terminal_prepared_generation != 0 &&
             decoded.executor_availability_generation != 0;
    };
    const auto begin_transaction = [&](parser::SbsqlTestWireSession& target) {
      const auto begun = target.RunPipeline("BEGIN TRANSACTION", true);
      if (!begun.accepted || begun.messages.has_errors()) {
        dump_result("statement_boundary_begin_failed", begun);
        return false;
      }
      return true;
    };
    const auto rollback_transaction = [&](parser::SbsqlTestWireSession& target) {
      const auto rolled_back = target.RunPipeline("ROLLBACK TRANSACTION", true);
      if (!rolled_back.accepted || rolled_back.messages.has_errors()) {
        dump_result("statement_boundary_rollback_failed", rolled_back);
        return false;
      }
      return true;
    };
    const auto execute_scalar = [&](parser::SbsqlTestWireSession& target,
                                    std::string_view statement_name,
                                    std::uint64_t expected_value) {
      const auto sql = "EXECUTE " + std::string(statement_name) + ";";
      const auto executed = target.RunPipeline(sql, true, true);
      sblr::SblrStmtExecuteResultV1 decoded;
      std::string detail;
      if (!executed.accepted || executed.messages.has_errors() ||
          executed.server_operation_id != "engine.op.stmt_execute" ||
          executed.server_cursor_uuid.empty() || executed.server_row_count != 1 ||
          !sblr::DecodeSblrStmtExecuteResultV1(
              reinterpret_cast<const std::uint8_t*>(
                  executed.server_result_payload.data()),
              executed.server_result_payload.size(), &decoded, &detail) ||
          decoded.status != 1 || decoded.publication_barrier != 1) {
        dump_result("statement_boundary_execute_failed", executed);
        return false;
      }
      const auto fetched =
          target.FetchCursorOnRoute(executed.server_cursor_uuid, 1);
      const auto expected_row =
          "row[0]=key_a=" + std::to_string(expected_value);
      if (!fetched.accepted || fetched.row_count != 1 ||
          !fetched.end_of_cursor ||
          fetched.row_packet.find(expected_row) == std::string::npos) {
        std::cerr << "statement_boundary_fetch_failed row_packet="
                  << fetched.row_packet << '\n';
        return false;
      }
      return true;
    };
    const auto make_peer = [&](parser::ParserConfig peer_config,
                               std::string_view application_suffix) {
      auto peer = std::make_unique<parser::SbsqlTestWireSession>(
          std::move(peer_config), nullptr, nullptr);
      auto peer_credentials = credentials;
      peer_credentials.application_name += application_suffix;
      parser::MessageVectorSet peer_messages;
      if (!peer->AuthenticateCredentials(peer_credentials, &peer_messages)) {
        for (const auto& diagnostic : peer_messages.diagnostics) {
          std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        }
        return std::unique_ptr<parser::SbsqlTestWireSession>{};
      }
      return peer;
    };

    parser::ParserConfig budget_config = config;
    budget_config.resource_budget.max_statement_bytes = 8;
    auto budget_session = make_peer(std::move(budget_config), "-budget");
    auto peer_session = make_peer(config, "-peer");
    if (budget_session == nullptr || peer_session == nullptr) return 4;

    if (operation == "stmt-prepare-boundaries") {
      const auto malformed = session.RunPipeline("PREPARE broken;", true);
      const auto budgeted = budget_session->RunPipeline(
          "PREPARE STATEMENT isolated_prepare AS SELECT 1;", true);
      if (!exact_refusal(malformed, "SBLR.OPERAND.INVALID",
                         "prepare_requires_statement_name_and_body") ||
          !exact_budget_refusal(budgeted)) {
        dump_result("stmt_prepare_malformed", malformed);
        dump_result("stmt_prepare_budget", budgeted);
        return 4;
      }
      const auto owner_prepared = session.RunPipeline(
          "PREPARE STATEMENT isolated_prepare AS SELECT 1;", true);
      const auto peer_prepared = peer_session->RunPipeline(
          "PREPARE STATEMENT isolated_prepare AS SELECT 2;", true);
      if (!exact_prepare(owner_prepared) || !exact_prepare(peer_prepared)) {
        dump_result("stmt_prepare_owner", owner_prepared);
        dump_result("stmt_prepare_peer", peer_prepared);
        return 4;
      }
      const auto collision = session.RunPipeline(
          "PREPARE STATEMENT isolated_prepare AS SELECT 2;", true);
      if (collision.accepted || collision.outcome_unknown ||
          collision.messages.diagnostics.size() != 1 ||
          collision.messages.diagnostics.front().code !=
              "MGA.TRANSACTION.STALE" ||
          !no_canonical_result(collision)) {
        dump_result("stmt_prepare_collision", collision);
        return 4;
      }
      if (!begin_transaction(session) ||
          !execute_scalar(session, "isolated_prepare", 1) ||
          !rollback_transaction(session) ||
          !begin_transaction(*peer_session) ||
          !execute_scalar(*peer_session, "isolated_prepare", 2) ||
          !rollback_transaction(*peer_session)) {
        return 4;
      }
      std::cout << "CSC-TEST-001470 CSC-TEST-001471 CSC-TEST-001472 "
                   "STMT_PREPARE_BOUNDARIES accepted malformed=true "
                   "budget=true session_isolation=true "
                   "collision_preserved_original=true cancellation_fault="
                   "CSC-TEST-003576\n";
      return 0;
    }

    if (operation == "stmt-execute-boundaries") {
      const auto malformed = session.RunPipeline("EXECUTE;", true);
      const auto budgeted = budget_session->RunPipeline(
          "EXECUTE isolated_execute;", true, true);
      if (!exact_refusal(malformed, "SBLR.OPERAND.INVALID",
                         "execute_requires_statement_and_one_name") ||
          !exact_budget_refusal(budgeted)) {
        dump_result("stmt_execute_malformed", malformed);
        dump_result("stmt_execute_budget", budgeted);
        return 4;
      }
      const auto owner_prepared = session.RunPipeline(
          "PREPARE STATEMENT isolated_execute AS SELECT 1;", true);
      if (!exact_prepare(owner_prepared)) {
        dump_result("stmt_execute_owner_prepare", owner_prepared);
        return 4;
      }
      const auto hidden = peer_session->RunPipeline(
          "EXECUTE isolated_execute;", true, true);
      const auto absent = peer_session->RunPipeline(
          "EXECUTE definitely_absent_execute;", true, true);
      if (!exact_hidden_pair(hidden, absent, "isolated_execute",
                             "definitely_absent_execute")) {
        dump_result("stmt_execute_hidden", hidden);
        dump_result("stmt_execute_absent", absent);
        return 4;
      }
      const auto peer_prepared = peer_session->RunPipeline(
          "PREPARE STATEMENT isolated_execute AS SELECT 2;", true);
      if (!exact_prepare(peer_prepared) || !begin_transaction(session) ||
          !execute_scalar(session, "isolated_execute", 1) ||
          !rollback_transaction(session) ||
          !begin_transaction(*peer_session) ||
          !execute_scalar(*peer_session, "isolated_execute", 2) ||
          !rollback_transaction(*peer_session)) {
        dump_result("stmt_execute_peer_prepare", peer_prepared);
        return 4;
      }
      std::cout << "CSC-TEST-001178 CSC-TEST-001179 CSC-TEST-001180 "
                   "STMT_EXECUTE_BOUNDARIES accepted malformed=true "
                   "budget=true cross_session_hidden=true "
                   "owner_execution_preserved=true cancellation_fault="
                   "CSC-TEST-003580\n";
      return 0;
    }

    const auto malformed =
        session.RunPipeline("DEALLOCATE STATEMENT;", true);
    const auto budgeted = budget_session->RunPipeline(
        "DEALLOCATE STATEMENT isolated_free;", true);
    if (!exact_refusal(
            malformed, "SBLR.OPERAND.INVALID",
            "deallocate_requires_statement_or_prepare_and_one_name") ||
        !exact_budget_refusal(budgeted)) {
      dump_result("stmt_free_malformed", malformed);
      dump_result("stmt_free_budget", budgeted);
      return 4;
    }
    const auto owner_prepared = session.RunPipeline(
        "PREPARE STATEMENT isolated_free AS SELECT 1;", true);
    if (!exact_prepare(owner_prepared)) {
      dump_result("stmt_free_owner_prepare", owner_prepared);
      return 4;
    }
    const auto hidden = peer_session->RunPipeline(
        "DEALLOCATE STATEMENT isolated_free;", true);
    const auto absent = peer_session->RunPipeline(
        "DEALLOCATE STATEMENT definitely_absent_free;", true);
    if (!exact_hidden_pair(hidden, absent, "isolated_free",
                           "definitely_absent_free")) {
      dump_result("stmt_free_hidden", hidden);
      dump_result("stmt_free_absent", absent);
      return 4;
    }
    if (!begin_transaction(session) ||
        !execute_scalar(session, "isolated_free", 1) ||
        !rollback_transaction(session)) {
      return 4;
    }
    const auto freed = session.RunPipeline(
        "DEALLOCATE STATEMENT isolated_free;", true);
    if (!exact_free(freed)) {
      dump_result("stmt_free_owner", freed);
      return 4;
    }
    const auto revoked = session.RunPipeline("EXECUTE isolated_free;", true,
                                             true);
    const auto revoked_absent = session.RunPipeline(
        "EXECUTE definitely_absent_free;", true, true);
    if (!exact_hidden_pair(revoked, revoked_absent, "isolated_free",
                           "definitely_absent_free")) {
      dump_result("stmt_free_revoked", revoked);
      dump_result("stmt_free_revoked_absent", revoked_absent);
      return 4;
    }
    std::cout << "CSC-TEST-000854 CSC-TEST-000855 CSC-TEST-000856 "
                 "STMT_FREE_BOUNDARIES accepted malformed=true budget=true "
                 "cross_session_hidden=true failed_free_no_effect=true "
                 "revocation_hidden=true cancellation_fault="
                 "CSC-TEST-003588\n";
    return 0;
  }
  if (operation == "source-artifact-container" ||
      operation == "source-artifact-external") {
    namespace container = scratchbird::engine;
    namespace sblr = scratchbird::engine::sblr;
    const bool external = operation == "source-artifact-external";
    struct TransactionArtifactCase {
      std::string_view sql;
      std::string_view operation_id;
      std::string_view opcode;
      std::string_view rendered_sql;
      bool setup_begin;
      bool cleanup_rollback;
      std::string_view source_label;
      bool source_label_quoted;
    };
    const TransactionArtifactCase cases[] = {
        {"BEGIN TRANSACTION", "engine.op.txn_begin", "SBLR_TXN_BEGIN",
         "BEGIN TRANSACTION;", false, true, "", false},
        {"COMMIT TRANSACTION", "engine.op.txn_commit", "SBLR_TXN_COMMIT",
         "COMMIT;", true, false, "", false},
        {"ROLLBACK TRANSACTION", "engine.op.txn_rollback",
         "SBLR_TXN_ROLLBACK", "ROLLBACK;", true, false, "", false},
        {"SAVEPOINT source_mark", "engine.op.txn_savepoint",
         "SBLR_TXN_SAVEPOINT", "SAVEPOINT source_mark;", true, false,
         "source_mark", false},
        {"ROLLBACK TO SAVEPOINT source_mark",
         "engine.op.txn_rollback_to_savepoint",
         "SBLR_TXN_ROLLBACK_TO_SAVEPOINT",
         "ROLLBACK TO SAVEPOINT source_mark;", false, false, "source_mark",
         false},
        {"RELEASE SAVEPOINT source_mark",
         "engine.op.txn_release_savepoint",
         "SBLR_TXN_RELEASE_SAVEPOINT", "RELEASE SAVEPOINT source_mark;",
         false, true, "source_mark", false},
        {"SAVEPOINT \"Case Mark\"", "engine.op.txn_savepoint",
         "SBLR_TXN_SAVEPOINT", "SAVEPOINT \"Case Mark\";", true, false,
         "Case Mark", true},
        {"ROLLBACK TO SAVEPOINT \"Case Mark\"",
         "engine.op.txn_rollback_to_savepoint",
         "SBLR_TXN_ROLLBACK_TO_SAVEPOINT",
         "ROLLBACK TO SAVEPOINT \"Case Mark\";", false, false, "Case Mark",
         true},
        {"RELEASE SAVEPOINT \"Case Mark\"",
         "engine.op.txn_release_savepoint",
         "SBLR_TXN_RELEASE_SAVEPOINT", "RELEASE SAVEPOINT \"Case Mark\";",
         false, true, "Case Mark", true},
    };
    for (const auto& artifact_case : cases) {
      if (artifact_case.setup_begin) {
        const auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
        if (!begun.accepted || begun.messages.has_errors()) {
          std::cerr << "source_artifact_setup_begin_failed:"
                    << artifact_case.operation_id << '\n';
          return 4;
        }
      }
      scratchbird::parser::sbsql::SbsqlCanonicalExecutionObservation
          observation;
      auto executed = external
                          ? session.RunSourceArtifactExternalReferenceForWire(
                                &observation, artifact_case.sql)
                          : session.RunSourceArtifactContainerForWire(
                                &observation, artifact_case.sql);
      if (!executed.accepted || executed.messages.has_errors() ||
          !observation.captured ||
          observation.operation_id != artifact_case.operation_id ||
          observation.canonical_container_bytes.empty() ||
          observation.external_source_artifact != external ||
          (external &&
           (observation.canonical_execution_envelope_bytes.empty() ||
            observation.external_source_artifact_bytes.empty()))) {
        for (const auto& diagnostic : executed.messages.diagnostics) {
          std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        }
        std::cerr << "source_artifact_execution_observation_failed:"
                  << artifact_case.operation_id << '\n';
        return 4;
      }
      const auto decoded_container = container::DecodeSblrContainerBytes(
          observation.canonical_container_bytes.data(),
          observation.canonical_container_bytes.size());
      if (decoded_container.status != container::SblrCodecStatus::ok ||
          container::SblrReadU16(
              decoded_container.container.canonical_anchor.data() + 100) !=
              1 ||
          (external ? !decoded_container.container.source_map.empty()
                    : decoded_container.container.source_map.empty())) {
        std::cerr << "source_artifact_container_decode_failed:"
                  << artifact_case.operation_id << '\n';
        return 4;
      }
      container::SblrExecutionEnvelopeSemanticView ingress_view;
      container::SblrDecodedExecutionEnvelopeV1 decoded_ingress;
      if (external) {
        decoded_ingress = container::DecodeSblrExecutionEnvelopeV1Bytes(
            observation.canonical_execution_envelope_bytes.data(),
            observation.canonical_execution_envelope_bytes.size());
        if (decoded_ingress.status != container::SblrCodecStatus::ok ||
            !container::SblrValidateExecutionEnvelopeFields(
                decoded_ingress.envelope, &ingress_view) ||
            !ingress_view.source_artifact_present ||
            ingress_view.source_artifact_ref_kind != 4 ||
            ingress_view.source_artifact_declared_size !=
                observation.external_source_artifact_bytes.size() ||
            ingress_view.source_artifact_crc32c != container::SblrCrc32c(
                observation.external_source_artifact_bytes.data(),
                observation.external_source_artifact_bytes.size()) ||
            ingress_view.source_artifact_checksum_kind != 2 ||
            ingress_view.source_artifact_checksum_sha256 !=
                sblr::HashSblrSourceArtifactBytesV1(
                    observation.external_source_artifact_bytes.data(),
                    observation.external_source_artifact_bytes.size())) {
          std::cerr << "source_artifact_external_reference_failed:"
                    << artifact_case.operation_id << '\n';
          return 4;
        }
      }
      const std::string_view opcode_bytes(
          reinterpret_cast<const char*>(
              decoded_container.container.operation_payload.data()),
          decoded_container.container.operation_payload.size());
      const auto decoded_stream = sblr::DecodeSblrOpcodeStream(opcode_bytes);
      if (!decoded_stream.ok ||
          decoded_stream.stream.operations.size() != 3 ||
          decoded_stream.stream.operations[1].operation_id !=
              artifact_case.operation_id ||
          decoded_stream.stream.operations[1].opcode != artifact_case.opcode) {
        std::cerr << "source_artifact_opcode_stream_profile_failed:"
                  << artifact_case.operation_id << '\n';
        return 4;
      }
      const auto& artifact_bytes =
          external ? observation.external_source_artifact_bytes
                   : decoded_container.container.source_map;
      const auto decoded_artifact = sblr::DecodeSblrSourceArtifactMapV1(
          artifact_bytes.data(), artifact_bytes.size());
      const bool has_source_label = !artifact_case.source_label.empty();
      const bool exact_label_profile = !has_source_label
          ? decoded_artifact.artifact.symbols.empty() &&
                decoded_artifact.artifact.source_spans.size() == 1
          : decoded_artifact.artifact.symbols.size() == 1 &&
                decoded_artifact.artifact.source_spans.size() == 2 &&
                decoded_artifact.artifact.symbols[0].symbol_id == 1 &&
                decoded_artifact.artifact.symbols[0].symbol_key ==
                    "savepoint.label.1" &&
                decoded_artifact.artifact.symbols[0].symbol_kind ==
                    sblr::SblrSourceArtifactSymbolKindV1::label &&
                decoded_artifact.artifact.symbols[0].declaration_node_id == 2 &&
                decoded_artifact.artifact.symbols[0].scope_node_id == 1 &&
                decoded_artifact.artifact.symbols[0].raw_name_utf8 ==
                    artifact_case.source_label &&
                decoded_artifact.artifact.symbols[0].normalized_lookup_key ==
                    artifact_case.source_label &&
                decoded_artifact.artifact.symbols[0].was_quoted ==
                    artifact_case.source_label_quoted &&
                decoded_artifact.artifact.symbols[0].quote_style ==
                    (artifact_case.source_label_quoted
                         ? sblr::SblrSourceArtifactQuoteStyleV1::double_quote
                         : sblr::SblrSourceArtifactQuoteStyleV1::none) &&
                decoded_artifact.artifact.symbols[0].source_span_id == 2 &&
                decoded_artifact.artifact.source_spans[1].node_id == 2 &&
                decoded_artifact.artifact.source_spans[1].span_kind ==
                    sblr::SblrSourceArtifactSpanKindV1::identifier;
      if (decoded_artifact.status !=
              sblr::SblrSourceArtifactDecodeStatusV1::ok ||
          !exact_label_profile ||
          decoded_artifact.artifact.render_hints.size() != 1 ||
          decoded_artifact.artifact.render_hints[0].symbol_id !=
              (has_source_label ? 1U : 0U) ||
          decoded_artifact.artifact.render_hints[0].delimiter_hint !=
              (artifact_case.source_label_quoted
                   ? sblr::SblrSourceArtifactQuoteStyleV1::double_quote
                   : sblr::SblrSourceArtifactQuoteStyleV1::none) ||
          decoded_artifact.artifact.source_text_ref.present ||
          (external
               ? (!std::all_of(
                      decoded_artifact.artifact.container_request_uuid.begin(),
                      decoded_artifact.artifact.container_request_uuid.end(),
                      [](std::uint8_t value) { return value == 0; }) ||
                  !std::equal(
                      decoded_artifact.artifact.sblr_envelope_uuid.begin(),
                      decoded_artifact.artifact.sblr_envelope_uuid.end(),
                      decoded_ingress.envelope.fields[0].begin()) ||
                  decoded_artifact.artifact.artifact_uuid !=
                      ingress_view.source_artifact_uuid)
               : !std::equal(
                     decoded_artifact.artifact.container_request_uuid.begin(),
                     decoded_artifact.artifact.container_request_uuid.end(),
                     decoded_container.container.canonical_anchor.begin() +
                         116)) ||
          !std::equal(decoded_artifact.artifact.dialect_family_uuid.begin(),
                      decoded_artifact.artifact.dialect_family_uuid.end(),
                      decoded_container.container.canonical_anchor.begin() +
                          16) ||
          !std::equal(decoded_artifact.artifact.parser_package_uuid.begin(),
                      decoded_artifact.artifact.parser_package_uuid.end(),
                      decoded_container.container.canonical_anchor.begin() +
                          32)) {
        std::cerr << "source_artifact_binding_profile_failed:"
                  << artifact_case.operation_id << '\n';
        return 4;
      }
      const auto rendered = external
          ? sblr::RenderSblrExternalSourceArtifactToSbsql(
                observation.canonical_container_bytes.data(),
                observation.canonical_container_bytes.size(),
                observation.canonical_execution_envelope_bytes.data(),
                observation.canonical_execution_envelope_bytes.size(),
                observation.external_source_artifact_bytes.data(),
                observation.external_source_artifact_bytes.size(),
                sblr::SblrToSbsqlOptions{.source_preserving = true})
          : sblr::RenderSblrContainerToSbsql(
                observation.canonical_container_bytes.data(),
                observation.canonical_container_bytes.size(),
                sblr::SblrToSbsqlOptions{.source_preserving = true});
      if (!rendered.ok || !rendered.diagnostics.empty() ||
          rendered.sbsql_text != artifact_case.rendered_sql) {
        std::cerr << "source_artifact_source_preserving_render_failed:"
                  << artifact_case.operation_id << '\n';
        return 4;
      }
      const auto reparsed_cst =
          scratchbird::parser::sbsql::BuildCst(rendered.sbsql_text);
      const auto reparsed_ast =
          scratchbird::parser::sbsql::BuildAst(reparsed_cst);
      const bool exact_surface =
          artifact_case.operation_id == "engine.op.txn_begin"
              ? (reparsed_ast.statement_surface_name == "begin_transaction" ||
                 reparsed_ast.statement_surface_name == "begin_stmt")
          : artifact_case.operation_id == "engine.op.txn_commit"
              ? (reparsed_ast.statement_surface_name == "commit" ||
                 reparsed_ast.statement_surface_name == "commit_stmt" ||
                 reparsed_ast.statement_surface_name == "commit_options")
          : artifact_case.operation_id == "engine.op.txn_rollback"
              ? (reparsed_ast.statement_surface_name == "rollback" ||
                 reparsed_ast.statement_surface_name == "rollback_stmt")
          : artifact_case.operation_id == "engine.op.txn_savepoint"
              ? (reparsed_ast.statement_surface_name == "savepoint" ||
                 reparsed_ast.statement_surface_name == "savepoint_stmt")
          : artifact_case.operation_id ==
                    "engine.op.txn_rollback_to_savepoint"
              ? reparsed_ast.statement_surface_name ==
                    "rollback_to_savepoint_stmt"
              : reparsed_ast.statement_surface_name ==
                    "release_savepoint_stmt";
      if (reparsed_cst.messages.has_errors() ||
          reparsed_ast.messages.has_errors() ||
          reparsed_ast.family !=
              scratchbird::parser::sbsql::StatementFamily::kTransaction ||
          !exact_surface) {
        std::cerr << "source_artifact_reparse_failed:"
                  << artifact_case.operation_id << '\n';
        return 4;
      }
      if (artifact_case.cleanup_rollback) {
        const auto rolled_back =
            session.RunPipeline("ROLLBACK TRANSACTION", true);
        if (!rolled_back.accepted || rolled_back.messages.has_errors()) {
          std::cerr << "source_artifact_cleanup_rollback_failed\n";
          return 4;
        }
      }
    }

    const auto schema_begin = session.RunPipeline("BEGIN TRANSACTION", true);
    if (!schema_begin.accepted || schema_begin.messages.has_errors()) {
      std::cerr << "source_artifact_create_schema_begin_failed\n";
      return 4;
    }
    constexpr std::string_view kCreateSchemaSql =
        "CREATE SCHEMA \"Source Schema\";";
    scratchbird::parser::sbsql::SbsqlCanonicalExecutionObservation
        schema_observation;
    auto schema_created =
        external
            ? session.RunSourceArtifactExternalReferenceForWire(
                  &schema_observation, kCreateSchemaSql)
            : session.RunSourceArtifactContainerForWire(&schema_observation,
                                                        kCreateSchemaSql);
    if (!schema_created.accepted || schema_created.outcome_unknown ||
        schema_created.messages.has_errors() || !schema_observation.captured ||
        schema_observation.operation_id != "engine.op.ddl_create_schema" ||
        schema_observation.external_source_artifact != external) {
      std::cerr << "source_artifact_create_schema_execution_failed:accepted="
                << schema_created.accepted
                << ":unknown=" << schema_created.outcome_unknown
                << ":captured=" << schema_observation.captured << '\n';
      for (const auto& diagnostic : schema_created.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message;
        for (const auto& field : diagnostic.fields) {
          std::cerr << ':' << field.name << '=' << field.value;
        }
        std::cerr << '\n';
      }
      return 4;
    }
    const auto schema_container = container::DecodeSblrContainerBytes(
        schema_observation.canonical_container_bytes.data(),
        schema_observation.canonical_container_bytes.size());
    container::SblrExecutionEnvelopeSemanticView schema_ingress_view;
    container::SblrDecodedExecutionEnvelopeV1 schema_ingress;
    if (schema_container.status != container::SblrCodecStatus::ok ||
        container::SblrReadU16(
            schema_container.container.canonical_anchor.data() + 100) != 1 ||
        (external ? !schema_container.container.source_map.empty()
                  : schema_container.container.source_map.empty())) {
      std::cerr << "source_artifact_create_schema_container_failed\n";
      return 4;
    }
    if (external) {
      schema_ingress = container::DecodeSblrExecutionEnvelopeV1Bytes(
          schema_observation.canonical_execution_envelope_bytes.data(),
          schema_observation.canonical_execution_envelope_bytes.size());
      if (schema_ingress.status != container::SblrCodecStatus::ok ||
          !container::SblrValidateExecutionEnvelopeFields(
              schema_ingress.envelope, &schema_ingress_view) ||
          !schema_ingress_view.source_artifact_present ||
          schema_ingress_view.source_artifact_ref_kind != 4) {
        std::cerr << "source_artifact_create_schema_external_ref_failed\n";
        return 4;
      }
    }
    const std::string_view schema_opcode_bytes(
        reinterpret_cast<const char*>(
            schema_container.container.operation_payload.data()),
        schema_container.container.operation_payload.size());
    const auto schema_stream =
        sblr::DecodeSblrOpcodeStream(schema_opcode_bytes);
    sblr::SblrDdlCreateSchemaDescriptorV1 schema_descriptor;
    std::string schema_detail;
    if (!schema_stream.ok || schema_stream.stream.operations.size() != 3 ||
        schema_stream.stream.operations[1].operation_id !=
            "engine.op.ddl_create_schema" ||
        schema_stream.stream.operations[1].opcode !=
            "SBLR_DDL_CREATE_SCHEMA" ||
        schema_stream.stream.operations[1].operands.size() != 1 ||
        schema_stream.stream.operations[1].operands[0].ordinal != 1 ||
        schema_stream.stream.operations[1].operands[0].type !=
            "create_schema_descriptor" ||
        schema_stream.stream.operations[1].operands[0].name != "schema" ||
        schema_stream.stream.operations[1].operands[0].value_kind !=
            sblr::SblrValueKind::create_schema_descriptor ||
        !sblr::DecodeSblrDdlCreateSchemaDescriptorV1(
            schema_stream.stream.operations[1].operands[0].value_body.data(),
            schema_stream.stream.operations[1].operands[0].value_body.size(),
            &schema_descriptor, &schema_detail, true)) {
      std::cerr << "source_artifact_create_schema_descriptor_failed:"
                << schema_detail << '\n';
      return 4;
    }
    sblr::SblrDdlCreateSchemaResultV1 schema_terminal;
    if (!sblr::DecodeSblrDdlCreateSchemaResultV1(
            reinterpret_cast<const std::uint8_t*>(
                schema_created.server_result_payload.data()),
            schema_created.server_result_payload.size(), &schema_terminal,
            &schema_detail) ||
        schema_terminal.schema_uuid != schema_descriptor.schema_uuid ||
        schema_terminal.schema_generation !=
            schema_descriptor.schema_generation) {
      std::cerr << "source_artifact_create_schema_terminal_failed:"
                << schema_detail << '\n';
      return 4;
    }
    const auto& schema_artifact_bytes =
        external ? schema_observation.external_source_artifact_bytes
                 : schema_container.container.source_map;
    const auto schema_artifact = sblr::DecodeSblrSourceArtifactMapV1(
        schema_artifact_bytes.data(), schema_artifact_bytes.size());
    const bool schema_binding =
        external
            ? std::all_of(
                  schema_artifact.artifact.container_request_uuid.begin(),
                  schema_artifact.artifact.container_request_uuid.end(),
                  [](std::uint8_t byte) { return byte == 0; }) &&
                  std::equal(
                      schema_artifact.artifact.sblr_envelope_uuid.begin(),
                      schema_artifact.artifact.sblr_envelope_uuid.end(),
                      schema_ingress.envelope.fields[0].begin()) &&
                  schema_artifact.artifact.artifact_uuid ==
                      schema_ingress_view.source_artifact_uuid
            : std::all_of(
                  schema_artifact.artifact.sblr_envelope_uuid.begin(),
                  schema_artifact.artifact.sblr_envelope_uuid.end(),
                  [](std::uint8_t byte) { return byte == 0; }) &&
                  std::equal(
                      schema_artifact.artifact.container_request_uuid.begin(),
                      schema_artifact.artifact.container_request_uuid.end(),
                      schema_container.container.canonical_anchor.begin() +
                          116);
    if (schema_artifact.status !=
            sblr::SblrSourceArtifactDecodeStatusV1::ok ||
        !schema_binding || schema_artifact.artifact.source_text_ref.present ||
        schema_artifact.artifact.symbols.size() != 1 ||
        schema_artifact.artifact.source_spans.size() != 2 ||
        schema_artifact.artifact.render_hints.size() != 1 ||
        schema_artifact.artifact.symbols[0].symbol_id != 1 ||
        schema_artifact.artifact.symbols[0].symbol_key !=
            "schema.path.atom.1" ||
        schema_artifact.artifact.symbols[0].symbol_kind !=
            sblr::SblrSourceArtifactSymbolKindV1::object_display_name ||
        schema_artifact.artifact.symbols[0].declaration_node_id != 2 ||
        schema_artifact.artifact.symbols[0].scope_node_id != 1 ||
        schema_artifact.artifact.symbols[0].related_object_uuid !=
            schema_descriptor.schema_uuid ||
        schema_artifact.artifact.symbols[0].raw_name_utf8 != "Source Schema" ||
        schema_artifact.artifact.symbols[0].normalized_lookup_key !=
            "Source Schema" ||
        !schema_artifact.artifact.symbols[0].was_quoted ||
        schema_artifact.artifact.symbols[0].quote_style !=
            sblr::SblrSourceArtifactQuoteStyleV1::double_quote ||
        schema_artifact.artifact.symbols[0].ordinal != 1 ||
        schema_artifact.artifact.symbols[0].source_span_id != 2 ||
        schema_artifact.artifact.source_spans[1].node_id != 2 ||
        schema_artifact.artifact.source_spans[1].span_kind !=
            sblr::SblrSourceArtifactSpanKindV1::identifier ||
        schema_artifact.artifact.render_hints[0].node_id != 2 ||
        schema_artifact.artifact.render_hints[0].symbol_id != 1 ||
        schema_artifact.artifact.render_hints[0].delimiter_hint !=
            sblr::SblrSourceArtifactQuoteStyleV1::double_quote ||
        schema_artifact.artifact.render_hints[0].format_group !=
            "source_preserving_ddl_create_schema_v1") {
      std::cerr << "source_artifact_create_schema_symbol_profile_failed\n";
      return 4;
    }
    const auto rendered_schema =
        external
            ? sblr::RenderSblrExternalSourceArtifactToSbsql(
                  schema_observation.canonical_container_bytes.data(),
                  schema_observation.canonical_container_bytes.size(),
                  schema_observation.canonical_execution_envelope_bytes.data(),
                  schema_observation.canonical_execution_envelope_bytes.size(),
                  schema_observation.external_source_artifact_bytes.data(),
                  schema_observation.external_source_artifact_bytes.size(),
                  sblr::SblrToSbsqlOptions{.source_preserving = true})
            : sblr::RenderSblrContainerToSbsql(
                  schema_observation.canonical_container_bytes.data(),
                  schema_observation.canonical_container_bytes.size(),
                  sblr::SblrToSbsqlOptions{.source_preserving = true});
    const auto schema_reparsed_cst =
        scratchbird::parser::sbsql::BuildCst(rendered_schema.sbsql_text);
    const auto schema_reparsed_ast =
        scratchbird::parser::sbsql::BuildAst(schema_reparsed_cst);
    if (!rendered_schema.ok || !rendered_schema.diagnostics.empty() ||
        rendered_schema.sbsql_text != kCreateSchemaSql ||
        schema_reparsed_cst.messages.has_errors() ||
        schema_reparsed_ast.messages.has_errors() ||
        schema_reparsed_ast.family !=
            scratchbird::parser::sbsql::StatementFamily::kCatalog ||
        schema_reparsed_ast.statement_surface_name != "create_object") {
      std::cerr << "source_artifact_create_schema_render_reparse_failed:ok="
                << rendered_schema.ok << ":text="
                << rendered_schema.sbsql_text << ":diagnostics=";
      for (const auto& diagnostic : rendered_schema.diagnostics) {
        std::cerr << diagnostic.code << '/' << diagnostic.message << ';';
      }
      std::cerr << ":cst_errors="
                << schema_reparsed_cst.messages.has_errors()
                << ":ast_errors="
                << schema_reparsed_ast.messages.has_errors()
                << ":family="
                << static_cast<int>(schema_reparsed_ast.family)
                << ":surface="
                << schema_reparsed_ast.statement_surface_name << '\n';
      return 4;
    }
    session.AcknowledgeDdlCreateSchemaCompletionForWire();
    const auto schema_rollback =
        session.RunPipeline("ROLLBACK TRANSACTION", true);
    if (!schema_rollback.accepted || schema_rollback.messages.has_errors()) {
      std::cerr << "source_artifact_create_schema_rollback_failed\n";
      return 4;
    }
    if (external) {
      std::cout << "CSC-TEST-005774 CSC-TEST-005777 CSC-TEST-005779 "
                   "CSC-TEST-005787 "
                   "SOURCE_ARTIFACT_EXTERNAL_REFERENCE "
                   "accepted server_admission=true receipt_resolution=true "
                   "source_preserving_render=true reparse=true "
                   "transaction_controls=begin,commit,rollback "
                   "savepoint_controls=create,rollback_to,release "
                   "savepoint_labels=unquoted,double_quoted "
                   "create_schema=quoted_descriptor_bound\n";
    } else {
      std::cout << "CSC-TEST-005770 CSC-TEST-005776 CSC-TEST-005778 "
                   "CSC-TEST-005786 "
                   "SOURCE_ARTIFACT_CONTAINER accepted "
                   "server_admission=true source_preserving_render=true "
                   "reparse=true "
                   "transaction_controls=begin,commit,rollback "
                   "savepoint_controls=create,rollback_to,release "
                   "savepoint_labels=unquoted,double_quoted "
                   "create_schema=quoted_descriptor_bound\n";
    }
    return 0;
  }
  if (operation == "ddl-create-trigger" ||
      operation == "ddl-create-trigger-observe") {
    namespace ddl = scratchbird::engine::sblr;
    const auto dump_failure = [](std::string_view phase,
                                 const auto& result) {
      std::cerr << "CSC-TEST-002621 DDL_CREATE_TRIGGER " << phase
                << " accepted=" << result.accepted
                << " outcome_unknown=" << result.outcome_unknown
                << " operation=" << result.server_operation_id << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
    };
    const auto nonzero = [](const auto& value) {
      return std::ranges::any_of(
          value, [](const std::uint8_t byte) { return byte != 0; });
    };
    const char* artifact_path =
        std::getenv("SCRATCHBIRD_TEST_DDL_CREATE_TRIGGER_RESULT_ARTIFACT");
    const auto write_artifact = [&](const std::string& bytes) {
      if (artifact_path == nullptr || *artifact_path == '\0' ||
          bytes.empty()) {
        return false;
      }
      std::ofstream out(artifact_path, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      return out.good();
    };
    const auto read_artifact = [&]() {
      std::vector<std::uint8_t> bytes;
      if (artifact_path == nullptr || *artifact_path == '\0') return bytes;
      std::ifstream in(artifact_path, std::ios::binary);
      if (!in) return bytes;
      in.seekg(0, std::ios::end);
      const auto extent = in.tellg();
      if (extent <= 0) return bytes;
      bytes.resize(static_cast<std::size_t>(extent));
      in.seekg(0, std::ios::beg);
      in.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
      if (!in) bytes.clear();
      return bytes;
    };

    auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
    if (!begun.accepted || begun.messages.has_errors()) {
      dump_failure("begin_failed", begun);
      return 4;
    }

    if (operation == "ddl-create-trigger") {
      constexpr std::string_view kCreateTriggerSql = R"SBSQL(CREATE TRIGGER app.trig_items_ai
AFTER INSERT
ON TABLE app.trig_items
FOR EACH ROW
AS
BEGIN
  INSERT INTO app.trig_audit
    (audit_id, event_kind, item_id, old_price, new_price, audit_note)
  VALUES
    (NEXT VALUE FOR app.trig_audit_seq,
     'INSERT',
     new.item_id,
     NULL,
     new.item_price,
     'item inserted');
END;)SBSQL";
      auto created = session.RunPipeline(kCreateTriggerSql, true);
      ddl::SblrDdlCreateTriggerResultV1 terminal;
      std::string detail;
      const auto canonical_container =
          scratchbird::engine::DecodeSblrContainerBytes(
              reinterpret_cast<const std::uint8_t*>(
                  created.sblr_payload.data()),
              created.sblr_payload.size());
      const auto canonical_stream =
          canonical_container.status == scratchbird::engine::SblrCodecStatus::ok
              ? ddl::DecodeSblrOpcodeStream(std::string_view(
                    reinterpret_cast<const char*>(canonical_container.container
                                                      .operation_payload.data()),
                    canonical_container.container.operation_payload.size()))
              : ddl::SblrOpcodeStreamResult{};
      const bool exact_submission =
          canonical_stream.ok &&
          scratchbird::engine::EncodeSblrContainer(
              canonical_container.container) ==
              std::vector<std::uint8_t>(created.sblr_payload.begin(),
                                        created.sblr_payload.end()) &&
          canonical_stream.stream.operations.size() == 3 &&
          canonical_stream.stream.operations[1].operation_id ==
              "engine.op.ddl_create_trigger" &&
          canonical_stream.stream.operations[1].opcode ==
              "SBLR_DDL_CREATE_TRIGGER" &&
          canonical_stream.stream.operations[1].opcode_code == 1551 &&
          canonical_stream.stream.operations[1].operands.size() == 1 &&
          canonical_stream.stream.operations[1].operands[0].type ==
              "create_trigger_descriptor" &&
          canonical_stream.stream.operations[1].operands[0].name ==
              "trigger" &&
          canonical_stream.stream.operations[1].operands[0].value_kind ==
              ddl::SblrValueKind::create_trigger_descriptor;
      const bool exact_terminal =
          exact_submission && created.accepted && !created.outcome_unknown &&
          !created.messages.has_errors() &&
          created.server_operation_id == "engine.op.ddl_create_trigger" &&
          !created.sblr_payload.empty() &&
          created.server_request_payload_bytes ==
              ddl::kSblrDdlCreateTriggerRequestV1Bytes &&
          ddl::DecodeSblrDdlCreateTriggerResultV1(
              reinterpret_cast<const std::uint8_t*>(
                  created.server_result_payload.data()),
              created.server_result_payload.size(), &terminal, &detail) &&
          ddl::EncodeSblrDdlCreateTriggerResultV1(terminal) ==
              std::vector<std::uint8_t>(
                  created.server_result_payload.begin(),
                  created.server_result_payload.end()) &&
          nonzero(terminal.receipt) && nonzero(terminal.trigger_uuid) &&
          terminal.trigger_generation != 0 &&
          nonzero(terminal.target_relation_uuid) &&
          terminal.target_relation_generation != 0 &&
          nonzero(terminal.schema_uuid) && terminal.schema_generation != 0 &&
          nonzero(terminal.owning_transaction_uuid) &&
          terminal.owning_local_transaction_id != 0 &&
          nonzero(terminal.statement_snapshot_uuid) &&
          nonzero(terminal.catalog_row_uuid) &&
          nonzero(terminal.mutation_uuid) && nonzero(terminal.body_sblr_uuid) &&
          terminal.body_sblr_generation != 0 &&
          terminal.catalog_generation != 0 && terminal.security_epoch != 0 &&
          terminal.resource_generation != 0 &&
          nonzero(terminal.descriptor_evidence_sha256) &&
          nonzero(terminal.evidence) && terminal.availability != 0 &&
          nonzero(terminal.publication_barrier) &&
          terminal.trigger_uuid != terminal.target_relation_uuid &&
          terminal.trigger_uuid != terminal.schema_uuid &&
          terminal.trigger_uuid != terminal.catalog_row_uuid &&
          terminal.trigger_uuid != terminal.mutation_uuid &&
          terminal.trigger_uuid != terminal.body_sblr_uuid &&
          terminal.trigger_uuid != terminal.publication_barrier;
      if (!exact_terminal || !write_artifact(created.server_result_payload)) {
        dump_failure(detail.empty() ? "terminal_contract_failed" : detail,
                     created);
        return 4;
      }
      auto committed = session.RunPipeline("COMMIT TRANSACTION", true);
      if (!committed.accepted || committed.messages.has_errors()) {
        dump_failure("commit_failed", committed);
        return 4;
      }
      session.AcknowledgeDdlCreateTriggerCompletionForWire();
      if (session.HasHeldDdlCreateTriggerForWire()) {
        std::cerr << "ddl_create_trigger_terminal_holder_not_released\n";
        return 4;
      }
      std::cout << "CSC-TEST-002621 DDL_CREATE_TRIGGER accepted "
                   "surface_id=SBSQL-5127560F8031 "
                   "canonical_sblr=true catalog_mutation=true commit=true "
                   "publication_barrier=passed\n";
      return 0;
    }

    const auto expected_bytes = read_artifact();
    ddl::SblrDdlCreateTriggerResultV1 expected;
    std::string detail;
    auto observed = session.RunPipeline(
        "RESOLVE NAME app.trig_items_ai AS TRIGGER;", true);
    ddl::SblrNameResolveResultV1 resolved;
    const bool exact_observer =
        !expected_bytes.empty() &&
        ddl::DecodeSblrDdlCreateTriggerResultV1(
            expected_bytes.data(), expected_bytes.size(), &expected, &detail) &&
        observed.accepted && !observed.outcome_unknown &&
        !observed.messages.has_errors() &&
        observed.server_operation_id == "engine.op.name_resolve" &&
        ddl::DecodeSblrNameResolveResultV1(
            reinterpret_cast<const std::uint8_t*>(
                observed.server_result_payload.data()),
            observed.server_result_payload.size(), &resolved, &detail) &&
        resolved.status == 1 && resolved.visibility == 1 &&
        resolved.object_class == 9 &&
        resolved.resolved_object_uuid == expected.trigger_uuid &&
        resolved.resolved_namespace_uuid == expected.schema_uuid &&
        resolved.object_descriptor_generation != 0 &&
        nonzero(resolved.publication_evidence_uuid) &&
        nonzero(resolved.resolution_material_sha256) &&
        nonzero(resolved.executor_evidence_sha256);
    auto rolled_back = session.RunPipeline("ROLLBACK TRANSACTION", true);
    if (!exact_observer || !rolled_back.accepted ||
        rolled_back.messages.has_errors()) {
      dump_failure(detail.empty() ? "observer_contract_failed" : detail,
                   observed);
      return 4;
    }
    std::cout << "CSC-TEST-002621 DDL_CREATE_TRIGGER observer_visible=true "
                 "surface_id=SBSQL-5127560F8031 "
                 "independent_session=true exact_trigger_identity=true\n";
    return 0;
  }
  if (operation == "ddl-alter-trigger" ||
      operation == "ddl-alter-trigger-observe" ||
      operation == "ddl-drop-trigger" ||
      operation == "ddl-drop-trigger-observe") {
    namespace ddl = scratchbird::engine::sblr;
    const bool alter_family =
        operation == "ddl-alter-trigger" ||
        operation == "ddl-alter-trigger-observe";
    const bool observer =
        operation == "ddl-alter-trigger-observe" ||
        operation == "ddl-drop-trigger-observe";
    const char* artifact_path = std::getenv(
        alter_family
            ? "SCRATCHBIRD_TEST_DDL_ALTER_TRIGGER_RESULT_ARTIFACT"
            : "SCRATCHBIRD_TEST_DDL_DROP_TRIGGER_RESULT_ARTIFACT");
    const auto nonzero = [](const auto& value) {
      return std::ranges::any_of(
          value, [](const std::uint8_t byte) { return byte != 0; });
    };
    const auto dump_failure = [&](std::string_view phase,
                                  const auto& result) {
      std::cerr << (alter_family ? "CSC-TEST-002625 DDL_ALTER_TRIGGER "
                                 : "CSC-TEST-002629 DDL_DROP_TRIGGER ")
                << phase << " accepted=" << result.accepted
                << " outcome_unknown=" << result.outcome_unknown
                << " operation=" << result.server_operation_id << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
    };
    const auto write_artifact = [&](const std::string& bytes) {
      if (artifact_path == nullptr || *artifact_path == '\0' ||
          bytes.empty()) {
        return false;
      }
      std::ofstream out(artifact_path, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      return out.good();
    };
    const auto read_artifact = [&]() {
      std::vector<std::uint8_t> bytes;
      if (artifact_path == nullptr || *artifact_path == '\0') return bytes;
      std::ifstream in(artifact_path, std::ios::binary);
      if (!in) return bytes;
      in.seekg(0, std::ios::end);
      const auto extent = in.tellg();
      if (extent <= 0) return bytes;
      bytes.resize(static_cast<std::size_t>(extent));
      in.seekg(0, std::ios::beg);
      in.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
      if (!in) bytes.clear();
      return bytes;
    };

    auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
    if (!begun.accepted || begun.messages.has_errors()) {
      dump_failure("begin_failed", begun);
      return 4;
    }

    if (observer) {
      const auto expected_bytes = read_artifact();
      auto observed = session.RunPipeline(
          "RESOLVE NAME app.trig_items_ai AS TRIGGER;", true);
      ddl::SblrNameResolveResultV1 resolved;
      std::string detail;
      bool exact_observer = false;
      if (alter_family) {
        ddl::SblrDdlAlterTriggerResultV1 expected;
        exact_observer =
            !expected_bytes.empty() &&
            ddl::DecodeSblrDdlAlterTriggerResultV1(
                expected_bytes.data(), expected_bytes.size(), &expected,
                &detail) &&
            observed.accepted && !observed.outcome_unknown &&
            !observed.messages.has_errors() &&
            observed.server_operation_id == "engine.op.name_resolve" &&
            ddl::DecodeSblrNameResolveResultV1(
                reinterpret_cast<const std::uint8_t*>(
                    observed.server_result_payload.data()),
                observed.server_result_payload.size(), &resolved, &detail) &&
            resolved.status == 1 && resolved.visibility == 1 &&
            resolved.object_class == 9 &&
            resolved.resolved_object_uuid == expected.trigger_uuid &&
            resolved.resolved_namespace_uuid == expected.schema_uuid &&
            resolved.object_descriptor_generation ==
                expected.trigger_generation &&
            nonzero(resolved.publication_evidence_uuid) &&
            nonzero(resolved.resolution_material_sha256) &&
            nonzero(resolved.executor_evidence_sha256);
      } else {
        ddl::SblrDdlDropTriggerResultV1 expected;
        exact_observer =
            !expected_bytes.empty() &&
            ddl::DecodeSblrDdlDropTriggerResultV1(
                expected_bytes.data(), expected_bytes.size(), &expected,
                &detail) &&
            observed.accepted && !observed.outcome_unknown &&
            !observed.messages.has_errors() &&
            observed.server_operation_id == "engine.op.name_resolve" &&
            ddl::DecodeSblrNameResolveResultV1(
                reinterpret_cast<const std::uint8_t*>(
                    observed.server_result_payload.data()),
                observed.server_result_payload.size(), &resolved, &detail) &&
            resolved.status == 2 && resolved.visibility == 2 &&
            resolved.object_class == 9 &&
            !nonzero(resolved.resolved_object_uuid) &&
            !nonzero(resolved.resolved_namespace_uuid);
      }
      auto rolled_back = session.RunPipeline("ROLLBACK TRANSACTION", true);
      if (!exact_observer || !rolled_back.accepted ||
          rolled_back.messages.has_errors()) {
        if (alter_family) {
          ddl::SblrDdlAlterTriggerResultV1 expected;
          const bool decoded_expected =
              !expected_bytes.empty() &&
              ddl::DecodeSblrDdlAlterTriggerResultV1(
                  expected_bytes.data(), expected_bytes.size(), &expected,
                  &detail);
          std::cerr << "observer_fields decoded_expected=" << decoded_expected
                    << " status=" << static_cast<unsigned>(resolved.status)
                    << " visibility="
                    << static_cast<unsigned>(resolved.visibility)
                    << " object_class=" << resolved.object_class
                    << " object_uuid_match="
                    << (decoded_expected &&
                        resolved.resolved_object_uuid == expected.trigger_uuid)
                    << " namespace_uuid_match="
                    << (decoded_expected &&
                        resolved.resolved_namespace_uuid == expected.schema_uuid)
                    << " descriptor_generation="
                    << resolved.object_descriptor_generation
                    << " expected_generation="
                    << (decoded_expected ? expected.trigger_generation : 0)
                    << " publication="
                    << nonzero(resolved.publication_evidence_uuid)
                    << " material="
                    << nonzero(resolved.resolution_material_sha256)
                    << " executor="
                    << nonzero(resolved.executor_evidence_sha256) << '\n';
        }
        dump_failure(detail.empty() ? "observer_contract_failed" : detail,
                     observed);
        return 4;
      }
      if (alter_family) {
        std::cout << "CSC-TEST-002625 DDL_ALTER_TRIGGER "
                     "surface_id=SBSQL-AA3896D3895F "
                     "observer_visible=true independent_session=true "
                     "exact_successor_generation=true\n";
      } else {
        std::cout << "CSC-TEST-002629 DDL_DROP_TRIGGER "
                     "surface_id=SBSQL-E64AF6FD5CD3 "
                     "observer_absent=true independent_session=true "
                     "exact_tombstone_visible=true\n";
      }
      return 0;
    }

    auto changed = session.RunPipeline(
        alter_family
            ? "ALTER TRIGGER app.trig_items_ai INACTIVE SET ORDER 7 SET "
              "SECURITY DEFINER COMPILE VALIDATE;"
            : "DROP TRIGGER app.trig_items_ai RESTRICT;",
        true);
    const auto canonical_container =
        scratchbird::engine::DecodeSblrContainerBytes(
            reinterpret_cast<const std::uint8_t*>(changed.sblr_payload.data()),
            changed.sblr_payload.size());
    const auto canonical_stream =
        canonical_container.status == scratchbird::engine::SblrCodecStatus::ok
            ? ddl::DecodeSblrOpcodeStream(std::string_view(
                  reinterpret_cast<const char*>(canonical_container.container
                                                    .operation_payload.data()),
                  canonical_container.container.operation_payload.size()))
            : ddl::SblrOpcodeStreamResult{};
    const auto& expected_operation =
        alter_family ? "engine.op.ddl_alter_trigger"
                     : "engine.op.ddl_drop_trigger";
    const auto& expected_opcode =
        alter_family ? "SBLR_DDL_ALTER_TRIGGER" : "SBLR_DDL_DROP_TRIGGER";
    const auto& expected_operand =
        alter_family ? "alter_trigger_descriptor" : "drop_trigger_descriptor";
    const std::uint16_t expected_code = alter_family ? 1552 : 1553;
    const bool exact_submission =
        canonical_stream.ok &&
        scratchbird::engine::EncodeSblrContainer(
            canonical_container.container) ==
            std::vector<std::uint8_t>(changed.sblr_payload.begin(),
                                      changed.sblr_payload.end()) &&
        canonical_stream.stream.operations.size() == 3 &&
        canonical_stream.stream.operations[1].operation_id ==
            expected_operation &&
        canonical_stream.stream.operations[1].opcode == expected_opcode &&
        canonical_stream.stream.operations[1].opcode_code == expected_code &&
        canonical_stream.stream.operations[1].operands.size() == 1 &&
        canonical_stream.stream.operations[1].operands[0].type ==
            expected_operand &&
        canonical_stream.stream.operations[1].operands[0].name == "trigger";
    std::string detail;
    bool exact_terminal = false;
    if (alter_family && exact_submission) {
      ddl::SblrDdlAlterTriggerDescriptorV1 descriptor;
      ddl::SblrDdlAlterTriggerResultV1 terminal;
      const auto& operand = canonical_stream.stream.operations[1].operands[0];
      exact_terminal =
          operand.value_kind == ddl::SblrValueKind::alter_trigger_descriptor &&
          ddl::DecodeSblrDdlAlterTriggerDescriptorV1(
              operand.value_body.data(), operand.value_body.size(),
              &descriptor, &detail, true) &&
          changed.accepted && !changed.outcome_unknown &&
          !changed.messages.has_errors() &&
          changed.server_operation_id == expected_operation &&
          changed.server_request_payload_bytes ==
              ddl::kSblrDdlAlterTriggerRequestV1Bytes &&
          ddl::DecodeSblrDdlAlterTriggerResultV1(
              reinterpret_cast<const std::uint8_t*>(
                  changed.server_result_payload.data()),
              changed.server_result_payload.size(), &terminal, &detail) &&
          ddl::EncodeSblrDdlAlterTriggerResultV1(terminal) ==
              std::vector<std::uint8_t>(
                  changed.server_result_payload.begin(),
                  changed.server_result_payload.end()) &&
          terminal.receipt == descriptor.receipt &&
          terminal.trigger_uuid == descriptor.trigger_uuid &&
          terminal.trigger_generation == descriptor.trigger_generation + 1 &&
          terminal.target_relation_uuid == descriptor.target_relation_uuid &&
          terminal.target_relation_generation ==
              descriptor.target_relation_generation &&
          terminal.schema_uuid == descriptor.schema_uuid &&
          terminal.schema_generation == descriptor.schema_generation &&
          terminal.owning_transaction_uuid ==
              descriptor.owning_transaction_uuid &&
          terminal.owning_local_transaction_id ==
              descriptor.owning_local_transaction_id &&
          terminal.statement_snapshot_uuid ==
              descriptor.statement_snapshot_uuid &&
          terminal.body_sblr_uuid == descriptor.body_sblr_uuid &&
          terminal.body_sblr_generation == descriptor.body_sblr_generation &&
          terminal.catalog_generation == descriptor.catalog_generation &&
          terminal.security_epoch == descriptor.security_epoch &&
          terminal.resource_generation == descriptor.resource_generation &&
          terminal.descriptor_evidence_sha256 == descriptor.evidence &&
          terminal.availability == descriptor.availability &&
          nonzero(terminal.catalog_row_uuid) &&
          nonzero(terminal.mutation_uuid) && nonzero(terminal.evidence) &&
          nonzero(terminal.publication_barrier) &&
          write_artifact(changed.server_result_payload);
    } else if (!alter_family && exact_submission) {
      ddl::SblrDdlDropTriggerDescriptorV1 descriptor;
      ddl::SblrDdlDropTriggerResultV1 terminal;
      const auto& operand = canonical_stream.stream.operations[1].operands[0];
      exact_terminal =
          operand.value_kind == ddl::SblrValueKind::drop_trigger_descriptor &&
          ddl::DecodeSblrDdlDropTriggerDescriptorV1(
              operand.value_body.data(), operand.value_body.size(),
              &descriptor, &detail, true) &&
          changed.accepted && !changed.outcome_unknown &&
          !changed.messages.has_errors() &&
          changed.server_operation_id == expected_operation &&
          changed.server_request_payload_bytes ==
              ddl::kSblrDdlDropTriggerRequestV1Bytes &&
          ddl::DecodeSblrDdlDropTriggerResultV1(
              reinterpret_cast<const std::uint8_t*>(
                  changed.server_result_payload.data()),
              changed.server_result_payload.size(), &terminal, &detail) &&
          ddl::EncodeSblrDdlDropTriggerResultV1(terminal) ==
              std::vector<std::uint8_t>(
                  changed.server_result_payload.begin(),
                  changed.server_result_payload.end()) &&
          terminal.receipt == descriptor.receipt &&
          terminal.trigger_uuid == descriptor.trigger_uuid &&
          terminal.trigger_generation == descriptor.trigger_generation + 1 &&
          terminal.target_relation_uuid == descriptor.target_relation_uuid &&
          terminal.target_relation_generation ==
              descriptor.target_relation_generation &&
          terminal.schema_uuid == descriptor.schema_uuid &&
          terminal.schema_generation == descriptor.schema_generation &&
          terminal.owning_transaction_uuid ==
              descriptor.owning_transaction_uuid &&
          terminal.owning_local_transaction_id ==
              descriptor.owning_local_transaction_id &&
          terminal.statement_snapshot_uuid ==
              descriptor.statement_snapshot_uuid &&
          terminal.body_sblr_uuid == descriptor.body_sblr_uuid &&
          terminal.body_sblr_generation == descriptor.body_sblr_generation &&
          terminal.catalog_generation == descriptor.catalog_generation &&
          terminal.security_epoch == descriptor.security_epoch &&
          terminal.resource_generation == descriptor.resource_generation &&
          terminal.descriptor_evidence_sha256 == descriptor.evidence &&
          terminal.availability == descriptor.availability &&
          nonzero(terminal.catalog_row_uuid) &&
          nonzero(terminal.mutation_uuid) && nonzero(terminal.evidence) &&
          nonzero(terminal.publication_barrier) &&
          write_artifact(changed.server_result_payload);
    }
    if (!exact_terminal) {
      dump_failure(detail.empty() ? "terminal_contract_failed" : detail,
                   changed);
      return 4;
    }

    auto committed = session.RunPipeline("COMMIT TRANSACTION", true);
    if (!committed.accepted || committed.messages.has_errors()) {
      dump_failure("commit_failed", committed);
      return 4;
    }
    if (alter_family) {
      session.AcknowledgeDdlAlterTriggerCompletionForWire();
      if (session.HasHeldDdlAlterTriggerForWire()) {
        std::cerr << "ddl_alter_trigger_terminal_holder_not_released\n";
        return 4;
      }
      std::cout << "CSC-TEST-002625 DDL_ALTER_TRIGGER accepted "
                   "surface_id=SBSQL-AA3896D3895F "
                   "canonical_sblr=true catalog_successor=true commit=true "
                   "publication_barrier=passed\n";
    } else {
      session.AcknowledgeDdlDropTriggerCompletionForWire();
      if (session.HasHeldDdlDropTriggerForWire()) {
        std::cerr << "ddl_drop_trigger_terminal_holder_not_released\n";
        return 4;
      }
      std::cout << "CSC-TEST-002629 DDL_DROP_TRIGGER accepted "
                   "surface_id=SBSQL-E64AF6FD5CD3 "
                   "canonical_sblr=true catalog_tombstone=true commit=true "
                   "publication_barrier=passed\n";
    }
    return 0;
  }
  if (operation == "ddl-create-procedure" ||
      operation == "ddl-create-procedure-observe" ||
      operation == "ddl-create-procedure-observe-absent" ||
      operation == "ddl-create-procedure-rollback" ||
      operation == "ddl-create-procedure-invalid" ||
      operation == "procedure-invoke" ||
      operation == "procedure-invoke-invalid") {
    namespace sblr = scratchbird::engine::sblr;
    namespace engine_api = scratchbird::engine::internal_api;
    const auto nonzero = [](const auto& value) {
      return std::ranges::any_of(
          value, [](const std::uint8_t byte) { return byte != 0; });
    };
    const auto read_u32 = [](const std::uint8_t* bytes) {
      std::uint32_t value = 0;
      for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (8U * index);
      }
      return value;
    };
    const auto read_u64 = [](const std::uint8_t* bytes) {
      std::uint64_t value = 0;
      for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (8U * index);
      }
      return value;
    };
    const auto dump_failure = [&](std::string_view phase,
                                  const auto& failed) {
      std::cerr << (operation == "procedure-invoke" ||
                            operation == "procedure-invoke-invalid"
                        ? "CSC-TEST-002501 PROCEDURE_INVOKE "
                        : "CSC-TEST-002633 DDL_CREATE_PROCEDURE ")
                << phase << " accepted=" << failed.accepted
                << " outcome_unknown=" << failed.outcome_unknown
                << " operation=" << failed.server_operation_id << '\n';
      for (const auto& diagnostic : failed.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
    };
    const char* create_artifact_path = std::getenv(
        "SCRATCHBIRD_TEST_DDL_CREATE_PROCEDURE_RESULT_ARTIFACT");
    const auto write_create_artifact = [&](const std::string& bytes) {
      if (create_artifact_path == nullptr || *create_artifact_path == '\0' ||
          bytes.empty()) {
        return false;
      }
      std::ofstream out(create_artifact_path,
                        std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      return out.good();
    };
    const auto read_create_artifact = [&]() {
      std::vector<std::uint8_t> bytes;
      if (create_artifact_path == nullptr || *create_artifact_path == '\0') {
        return bytes;
      }
      std::ifstream in(create_artifact_path, std::ios::binary);
      if (!in) return bytes;
      in.seekg(0, std::ios::end);
      const auto extent = in.tellg();
      if (extent <= 0) return bytes;
      bytes.resize(static_cast<std::size_t>(extent));
      in.seekg(0, std::ios::beg);
      in.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
      if (!in) bytes.clear();
      return bytes;
    };

    auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
    if (!begun.accepted || begun.messages.has_errors()) {
      dump_failure("begin_failed", begun);
      return 4;
    }

    const auto exact_pre_sblr_refusal = [&](const auto& refused,
                                             std::string_view code,
                                             std::string_view detail) {
      if (refused.accepted || refused.outcome_unknown ||
          !refused.sblr_payload.empty() ||
          !refused.server_operation_id.empty() ||
          !refused.server_result_payload.empty() ||
          !refused.server_cursor_uuid.empty() || refused.server_row_count != 0 ||
          refused.server_affected_rows != 0 ||
          refused.server_affected_rows_present ||
          refused.messages.diagnostics.size() != 1 ||
          refused.messages.diagnostics.front().code != code) {
        return false;
      }
      return std::ranges::any_of(
          refused.messages.diagnostics.front().fields,
          [&](const auto& field) {
            return field.name == "detail" && field.value == detail;
          });
    };

    if (operation == "ddl-create-procedure-invalid" ||
        operation == "procedure-invoke-invalid") {
      const bool invoke_invalid = operation == "procedure-invoke-invalid";
      auto refused_shape = session.RunPipeline(
          invoke_invalid
              ? "EXECUTE PROCEDURE app.alignment_null_procedure(1, 2);"
              : "CREATE PROCEDURE app.invalid_with_parameter(value BIGINT, "
                "other BIGINT) AS BEGIN NULL; END;",
          true);
      const bool exact_shape_refusal = exact_pre_sblr_refusal(
          refused_shape, "SBLR.OPERAND.INVALID",
          invoke_invalid ? "procedure_invoke_argument_shape_invalid"
                         : "ddl_create_procedure_parameter_shape_invalid");
      auto refused_authority = session.RunPipeline(
          invoke_invalid
              ? "EXECUTE PROCEDURE app.alignment_null_procedure("
                "9223372036854775808);"
              : "CREATE PROCEDURE app.invalid_parameter_type(value TEXT) "
                "AS BEGIN NULL; END;",
          true);
      const bool exact_authority_refusal = exact_pre_sblr_refusal(
          refused_authority,
          invoke_invalid ? "PROCEDURE.ARGUMENT_SHAPE_INVALID"
                         : "SBLR.OPERAND.INVALID",
          invoke_invalid ? "9223372036854775808"
                         : "ddl_create_procedure_parameter_type_invalid");
      const bool no_held_authority =
          invoke_invalid ? !session.HasHeldProcedureInvokeForWire()
                         : !session.HasHeldDdlCreateProcedureForWire();
      auto rolled_back = session.RunPipeline("ROLLBACK TRANSACTION", true);
      if (!exact_shape_refusal || !exact_authority_refusal ||
          !no_held_authority || !rolled_back.accepted ||
          rolled_back.messages.has_errors()) {
        dump_failure("parameter_refusal_contract_failed",
                     exact_shape_refusal ? refused_authority : refused_shape);
        return 4;
      }
      if (invoke_invalid) {
        std::cout << "CSC-TEST-005805 PROCEDURE_INVOKE "
                     "malformed_refusal=SBLR.OPERAND.INVALID "
                     "no_canonical_execution=true no_state_mutation=true\n";
      } else {
        std::cout << "CSC-TEST-005804 DDL_CREATE_PROCEDURE "
                     "malformed_refusal=SBLR.OPERAND.INVALID "
                     "no_canonical_execution=true no_catalog_mutation=true\n";
      }
      return 0;
    }

    if (operation == "ddl-create-procedure-observe" ||
        operation == "ddl-create-procedure-observe-absent") {
      const bool absent_case =
          operation == "ddl-create-procedure-observe-absent";
      const auto expected_bytes = absent_case
                                      ? std::vector<std::uint8_t>{}
                                      : read_create_artifact();
      sblr::SblrDdlCreateProcedureResultV1 expected;
      std::string detail;
      auto observed = session.RunPipeline(
          absent_case
              ? "RESOLVE NAME app.alignment_rolled_back_procedure AS PROCEDURE;"
              : "RESOLVE NAME app.alignment_null_procedure AS PROCEDURE;",
          true);
      sblr::SblrNameResolveResultV1 resolved;
      const bool decoded_observer =
          observed.accepted && !observed.outcome_unknown &&
          !observed.messages.has_errors() &&
          observed.server_operation_id == "engine.op.name_resolve" &&
          sblr::DecodeSblrNameResolveResultV1(
              reinterpret_cast<const std::uint8_t*>(
                  observed.server_result_payload.data()),
              observed.server_result_payload.size(), &resolved, &detail) &&
          resolved.object_class == 7 &&
          nonzero(resolved.publication_evidence_uuid) &&
          nonzero(resolved.resolution_material_sha256) &&
          nonzero(resolved.executor_evidence_sha256);
      const bool exact_observer =
          decoded_observer &&
          (absent_case
               ? resolved.status == 2 && resolved.visibility == 2 &&
                     !nonzero(resolved.resolved_object_uuid) &&
                     !nonzero(resolved.resolved_namespace_uuid)
               : !expected_bytes.empty() &&
                     sblr::DecodeSblrDdlCreateProcedureResultV1(
                         expected_bytes.data(), expected_bytes.size(),
                         &expected, &detail) &&
                     resolved.status == 1 && resolved.visibility == 1 &&
                     std::equal(resolved.resolved_object_uuid.begin(),
                                resolved.resolved_object_uuid.end(),
                                expected.body.begin()) &&
                     // Procedures created through the executable/name
                     // lifecycle are resolved from the durable name registry.
                     // Its descriptor fence is the exact catalog generation
                     // published in PCRS; body offset 16 instead identifies the
                     // executable-body generation revalidated by PIDO.
                     resolved.object_descriptor_generation ==
                         read_u64(expected.body.data() + 48) &&
                     resolved.catalog_generation ==
                         read_u64(expected.body.data() + 48) &&
                     nonzero(resolved.resolved_namespace_uuid));
      auto rolled_back = session.RunPipeline("ROLLBACK TRANSACTION", true);
      if (!exact_observer || !rolled_back.accepted ||
          rolled_back.messages.has_errors()) {
        dump_failure(detail.empty() ? "observer_contract_failed" : detail,
                     observed);
        return 4;
      }
      if (absent_case) {
        std::cout << "CSC-TEST-005803 DDL_CREATE_PROCEDURE "
                     "observer_absent=true independent_session=true\n";
      } else {
        std::cout << "CSC-TEST-005801 DDL_CREATE_PROCEDURE "
                     "observer_visible=true independent_session=true "
                     "exact_procedure_identity=true\n";
      }
      return 0;
    }

    if (operation == "ddl-create-procedure" ||
        operation == "ddl-create-procedure-rollback") {
      const bool rollback_case =
          operation == "ddl-create-procedure-rollback";
      const std::string_view kCreateProcedureSql =
          rollback_case
              ? "CREATE PROCEDURE app.alignment_rolled_back_procedure("
                "IN marker BIGINT) AS "
                "BEGIN NULL; END;"
              : "CREATE PROCEDURE app.alignment_null_procedure("
                "IN marker BIGINT) AS BEGIN NULL; END;";
      auto created = session.RunPipeline(kCreateProcedureSql, true);
      const auto container = scratchbird::engine::DecodeSblrContainerBytes(
          reinterpret_cast<const std::uint8_t*>(created.sblr_payload.data()),
          created.sblr_payload.size());
      const auto stream =
          container.status == scratchbird::engine::SblrCodecStatus::ok
              ? sblr::DecodeSblrOpcodeStream(std::string_view(
                    reinterpret_cast<const char*>(
                        container.container.operation_payload.data()),
                    container.container.operation_payload.size()))
              : sblr::SblrOpcodeStreamResult{};
      sblr::SblrDdlCreateProcedureDescriptorV1 descriptor;
      engine_api::SblrDdlCreateProcedureAuthorityInputV1 authority;
      sblr::SblrDdlCreateProcedureResultV1 terminal;
      std::string detail;
      const bool exact_stream =
          stream.ok && stream.stream.operations.size() == 3 &&
          stream.stream.operations[1].operation_id ==
              "engine.op.ddl_create_procedure" &&
          stream.stream.operations[1].opcode ==
              "SBLR_DDL_CREATE_PROCEDURE" &&
          stream.stream.operations[1].opcode_code == 1554 &&
          stream.stream.operations[1].operands.size() == 1 &&
          stream.stream.operations[1].operands[0].type ==
              "create_procedure_descriptor" &&
          stream.stream.operations[1].operands[0].name == "procedure" &&
          stream.stream.operations[1].operands[0].value_kind ==
              sblr::SblrValueKind::create_procedure_descriptor &&
          sblr::DecodeSblrDdlCreateProcedureDescriptorV1(
              stream.stream.operations[1].operands[0].value_body.data(),
              stream.stream.operations[1].operands[0].value_body.size(),
              &descriptor, &detail, true) &&
          engine_api::DecodeSblrDdlCreateProcedureAuthorityInputV1(
              descriptor, &authority, &detail) &&
          scratchbird::engine::EncodeSblrContainer(container.container) ==
              std::vector<std::uint8_t>(created.sblr_payload.begin(),
                                        created.sblr_payload.end());
      const auto exact_at = [&](std::size_t offset, const auto& expected) {
        return offset <= terminal.body.size() &&
               terminal.body.size() - offset >= expected.size() &&
               std::equal(expected.begin(), expected.end(),
                          terminal.body.begin() +
                              static_cast<std::ptrdiff_t>(offset));
      };
      const bool exact_terminal =
          exact_stream && created.accepted && !created.outcome_unknown &&
          !created.messages.has_errors() &&
          created.server_operation_id ==
              "engine.op.ddl_create_procedure" &&
          created.server_row_count == 0 &&
          sblr::DecodeSblrDdlCreateProcedureResultV1(
              reinterpret_cast<const std::uint8_t*>(
                  created.server_result_payload.data()),
              created.server_result_payload.size(), &terminal, &detail) &&
          sblr::EncodeSblrDdlCreateProcedureResultV1(terminal) ==
              std::vector<std::uint8_t>(
                  created.server_result_payload.begin(),
                  created.server_result_payload.end()) &&
          exact_at(0, authority.procedure_uuid) &&
          read_u64(terminal.body.data() + 16) ==
              authority.procedure_generation &&
          terminal.body[24] == 1 && exact_at(32, authority.receipt) &&
          read_u64(terminal.body.data() + 48) ==
              authority.catalog_generation &&
          read_u64(terminal.body.data() + 56) ==
              authority.schema_generation &&
          exact_at(64, authority.schema_uuid) &&
          exact_at(80, authority.owning_transaction_uuid) &&
          read_u64(terminal.body.data() + 96) ==
              authority.owning_local_transaction_id &&
          exact_at(104, authority.statement_snapshot_uuid) &&
          exact_at(152, authority.body_sblr_uuid) &&
          read_u64(terminal.body.data() + 168) ==
              authority.body_sblr_generation &&
          exact_at(176, authority.body_sblr_sha256) &&
          exact_at(208, descriptor.evidence) &&
          terminal.availability ==
              authority.executor_availability_generation &&
          nonzero(terminal.evidence) &&
          nonzero(terminal.publication_barrier) &&
          (rollback_case ||
           write_create_artifact(created.server_result_payload));
      if (!exact_terminal) {
        dump_failure(detail.empty() ? "terminal_contract_failed" : detail,
                     created);
        return 4;
      }
      auto finalized = session.RunPipeline(
          rollback_case ? "ROLLBACK TRANSACTION" : "COMMIT TRANSACTION",
          true);
      if (!finalized.accepted || finalized.messages.has_errors()) {
        dump_failure(rollback_case ? "rollback_failed" : "commit_failed",
                     finalized);
        return 4;
      }
      session.AcknowledgeDdlCreateProcedureCompletionForWire();
      if (session.HasHeldDdlCreateProcedureForWire()) {
        std::cerr << "ddl_create_procedure_terminal_holder_not_released\n";
        return 4;
      }
      if (rollback_case) {
        std::cout << "CSC-TEST-005803 DDL_CREATE_PROCEDURE "
                     "rollback=true no_visible_catalog_effect=true\n";
      } else {
        std::cout << "CSC-TEST-002633 CSC-TEST-005800 "
                     "DDL_CREATE_PROCEDURE accepted canonical_sblr=true "
                     "typed_null_body=true typed_parameter_abi=true "
                     "parameter_count=1 catalog_mutation=true commit=true "
                     "publication_barrier=passed\n";
      }
      return 0;
    }

    constexpr std::string_view kInvokeProcedureSql =
        "EXECUTE PROCEDURE app.alignment_null_procedure(-7);";
    auto invoked = session.RunPipeline(kInvokeProcedureSql, true);
    const auto container = scratchbird::engine::DecodeSblrContainerBytes(
        reinterpret_cast<const std::uint8_t*>(invoked.sblr_payload.data()),
        invoked.sblr_payload.size());
    const auto stream =
        container.status == scratchbird::engine::SblrCodecStatus::ok
            ? sblr::DecodeSblrOpcodeStream(std::string_view(
                  reinterpret_cast<const char*>(
                      container.container.operation_payload.data()),
                  container.container.operation_payload.size()))
            : sblr::SblrOpcodeStreamResult{};
    sblr::SblrProcedureInvokeDescriptorV1 descriptor;
    engine_api::SblrProcedureInvokeAuthorityInputV1 authority;
    sblr::SblrProcedureInvokeResultV1 terminal;
    std::string detail;
    const bool exact_stream =
        stream.ok && stream.stream.operations.size() == 3 &&
        stream.stream.operations[1].operation_id ==
            "engine.op.procedure_invoke" &&
        stream.stream.operations[1].opcode == "SBLR_PROCEDURE_INVOKE" &&
        stream.stream.operations[1].opcode_code == 1030 &&
        stream.stream.operations[1].operands.size() == 1 &&
        stream.stream.operations[1].operands[0].type ==
            "procedure_invoke_descriptor" &&
        stream.stream.operations[1].operands[0].name == "procedure" &&
        stream.stream.operations[1].operands[0].value_kind ==
            sblr::SblrValueKind::procedure_invoke_descriptor &&
        sblr::DecodeSblrProcedureInvokeDescriptorV1(
            stream.stream.operations[1].operands[0].value_body.data(),
            stream.stream.operations[1].operands[0].value_body.size(),
            &descriptor, &detail, true) &&
        engine_api::DecodeSblrProcedureInvokeAuthorityInputV1(
            descriptor, &authority, &detail) &&
        scratchbird::engine::EncodeSblrContainer(container.container) ==
            std::vector<std::uint8_t>(invoked.sblr_payload.begin(),
                                      invoked.sblr_payload.end());
    const auto exact_at = [&](std::size_t offset, const auto& expected) {
      return offset <= terminal.body.size() &&
             terminal.body.size() - offset >= expected.size() &&
             std::equal(expected.begin(), expected.end(),
                        terminal.body.begin() +
                            static_cast<std::ptrdiff_t>(offset));
    };
    const bool exact_terminal =
        exact_stream && invoked.accepted && !invoked.outcome_unknown &&
        !invoked.messages.has_errors() &&
        invoked.server_operation_id == "engine.op.procedure_invoke" &&
        invoked.server_row_count == 0 &&
        authority.argument_count == 1 &&
        nonzero(authority.argument_vector_uuid) &&
        nonzero(authority.argument_vector_sha256) &&
        sblr::DecodeSblrProcedureInvokeResultV1(
            reinterpret_cast<const std::uint8_t*>(
                invoked.server_result_payload.data()),
            invoked.server_result_payload.size(), &terminal, &detail) &&
        sblr::EncodeSblrProcedureInvokeResultV1(terminal) ==
            std::vector<std::uint8_t>(invoked.server_result_payload.begin(),
                                      invoked.server_result_payload.end()) &&
        exact_at(0, authority.invocation_uuid) &&
        read_u64(terminal.body.data() + 16) ==
            authority.invocation_generation &&
        terminal.body[24] == 1 && terminal.body[25] == 0 &&
        exact_at(32, authority.output_descriptor_vector_uuid) &&
        read_u64(terminal.body.data() + 48) ==
            authority.output_descriptor_vector_generation &&
        read_u32(terminal.body.data() + 56) == 0 &&
        read_u32(terminal.body.data() + 60) == 0 &&
        exact_at(208, authority.effect_set_sha256) &&
        terminal.availability ==
            authority.executor_availability_generation &&
        nonzero(terminal.evidence) && nonzero(terminal.barrier) &&
        terminal.barrier != authority.invocation_uuid &&
        terminal.barrier != authority.recovery_uuid;
    if (!exact_terminal) {
      dump_failure(detail.empty() ? "terminal_contract_failed" : detail,
                   invoked);
      return 4;
    }
    session.AcknowledgeProcedureInvokeCompletionForWire();
    if (session.HasHeldProcedureInvokeForWire()) {
      std::cerr << "procedure_invoke_execute_holder_not_released\n";
      return 4;
    }
    auto called = session.RunPipeline(
        "CALL app.alignment_null_procedure(+8);", true);
    sblr::SblrProcedureInvokeResultV1 call_terminal;
    if (!called.accepted || called.outcome_unknown ||
        called.messages.has_errors() ||
        called.server_operation_id != "engine.op.procedure_invoke" ||
        called.server_row_count != 0 ||
        !sblr::DecodeSblrProcedureInvokeResultV1(
            reinterpret_cast<const std::uint8_t*>(
                called.server_result_payload.data()),
            called.server_result_payload.size(), &call_terminal, &detail) ||
        !nonzero(call_terminal.evidence) || !nonzero(call_terminal.barrier)) {
      dump_failure(detail.empty() ? "call_route_failed" : detail, called);
      return 4;
    }
    auto committed = session.RunPipeline("COMMIT TRANSACTION", true);
    if (!committed.accepted || committed.messages.has_errors()) {
      dump_failure("commit_failed", committed);
      return 4;
    }
    session.AcknowledgeProcedureInvokeCompletionForWire();
    if (session.HasHeldProcedureInvokeForWire()) {
      std::cerr << "procedure_invoke_terminal_holder_not_released\n";
      return 4;
    }
    std::cout << "CSC-TEST-002501 CSC-TEST-005802 PROCEDURE_INVOKE "
                 "accepted canonical_sblr=true typed_null_body=true "
                 "argument_count=1 canonical_int64=true execute_and_call=true "
                 "output_count=0 commit=true publication_barrier=passed\n";
    return 0;
  }
  if (operation == "ddl-create-schema" ||
      operation == "ddl-create-schema-recover" ||
      operation == "ddl-create-schema-observe" ||
      operation == "ddl-create-schema-duplicate" ||
      operation == "ddl-create-schema-rollback" ||
      operation == "ddl-create-schema-observe-absent") {
    namespace ddl = scratchbird::engine::sblr;
    const auto dump_failure = [](std::string_view phase,
                                 const auto& result) {
      std::cerr << "CSC-TEST-005780 DDL_CREATE_SCHEMA " << phase
                << " accepted=" << result.accepted
                << " outcome_unknown=" << result.outcome_unknown
                << " operation=" << result.server_operation_id << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
    };
    const auto nonzero = [](const auto& value) {
      return std::ranges::any_of(
          value, [](const std::uint8_t byte) { return byte != 0; });
    };

    auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
    if (!begun.accepted || begun.messages.has_errors()) {
      dump_failure("begin_failed", begun);
      return 4;
    }
    const auto decode_name_result = [&](const auto& resolved_result,
                                        ddl::SblrNameResolveResultV1* out) {
      std::string detail;
      return resolved_result.accepted && !resolved_result.outcome_unknown &&
             !resolved_result.messages.has_errors() &&
             resolved_result.server_operation_id == "engine.op.name_resolve" &&
             !resolved_result.sblr_payload.empty() && out != nullptr &&
             ddl::DecodeSblrNameResolveResultV1(
                 reinterpret_cast<const std::uint8_t*>(
                     resolved_result.server_result_payload.data()),
                 resolved_result.server_result_payload.size(), out, &detail);
    };
    const auto exact_schema_terminal = [&](const auto& created,
                                           ddl::SblrDdlCreateSchemaResultV1* out) {
      std::string detail;
      return created.accepted && !created.outcome_unknown &&
             !created.messages.has_errors() &&
             created.server_operation_id == "engine.op.ddl_create_schema" &&
             !created.sblr_payload.empty() && out != nullptr &&
             ddl::DecodeSblrDdlCreateSchemaResultV1(
                 reinterpret_cast<const std::uint8_t*>(
                     created.server_result_payload.data()),
                 created.server_result_payload.size(), out, &detail) &&
             nonzero(out->receipt) && nonzero(out->schema_uuid) &&
             out->schema_generation != 0 &&
             nonzero(out->catalog_row_uuid) && nonzero(out->mutation_uuid) &&
             nonzero(out->evidence) && out->availability != 0 &&
             nonzero(out->publication_barrier) &&
             out->catalog_row_uuid != out->schema_uuid &&
             out->mutation_uuid != out->schema_uuid &&
             out->publication_barrier != out->schema_uuid;
    };
    const char* recovery_artifact_base =
        std::getenv("SCRATCHBIRD_TEST_DDL_CREATE_SCHEMA_RECOVERY_ARTIFACT");
    const auto write_recovery_artifact = [&](std::string_view suffix,
                                             const std::uint8_t* bytes,
                                             std::size_t size) {
      if (recovery_artifact_base == nullptr || bytes == nullptr || size == 0) {
        return false;
      }
      std::ofstream out(std::string(recovery_artifact_base) +
                            std::string(suffix),
                        std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(bytes),
                static_cast<std::streamsize>(size));
      return out.good();
    };
    const auto read_recovery_artifact = [&](std::string_view suffix) {
      std::vector<std::uint8_t> bytes;
      if (recovery_artifact_base == nullptr) return bytes;
      std::ifstream in(std::string(recovery_artifact_base) +
                           std::string(suffix),
                       std::ios::binary);
      if (!in) return bytes;
      in.seekg(0, std::ios::end);
      const auto extent = in.tellg();
      if (extent <= 0) return bytes;
      bytes.resize(static_cast<std::size_t>(extent));
      in.seekg(0, std::ios::beg);
      in.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
      if (!in) bytes.clear();
      return bytes;
    };

    if (operation == "ddl-create-schema" ||
        operation == "ddl-create-schema-rollback") {
      const bool rollback_case = operation == "ddl-create-schema-rollback";
      auto created =
          session.RunPipeline(rollback_case
                                  ? "CREATE SCHEMA qa_rolled_back_schema;"
                                  : "CREATE SCHEMA qa_schema;",
                              true);
      ddl::SblrDdlCreateSchemaResultV1 terminal;
      if (!exact_schema_terminal(created, &terminal)) {
        dump_failure("terminal_contract_failed", created);
        return 4;
      }
      if (!rollback_case) {
        const auto recovery_request =
            session.DdlCreateSchemaRecoveryRequestForWire();
        auto recovered =
            session.RecoverDdlCreateSchemaForWire(recovery_request);
        if (recovery_request.empty() || !recovered.accepted ||
            recovered.outcome_unknown || recovered.messages.has_errors() ||
            recovered.server_operation_id !=
                "engine.op.ddl_create_schema.recover" ||
            recovered.server_result_payload != created.server_result_payload ||
            !write_recovery_artifact(".csrq", recovery_request.data(),
                                     recovery_request.size()) ||
            !write_recovery_artifact(
                ".csrs",
                reinterpret_cast<const std::uint8_t*>(
                    created.server_result_payload.data()),
                created.server_result_payload.size())) {
          dump_failure("authenticated_recovery_failed", recovered);
          return 4;
        }
      }
      session.AcknowledgeDdlCreateSchemaCompletionForWire();
      auto finalized = session.RunPipeline(
          rollback_case ? "ROLLBACK TRANSACTION" : "COMMIT TRANSACTION",
          true);
      if (!finalized.accepted || finalized.messages.has_errors()) {
        dump_failure(rollback_case ? "rollback_failed" : "commit_failed",
                     finalized);
        return 4;
      }
      if (rollback_case) {
        std::cout << "CSC-TEST-005783 DDL_CREATE_SCHEMA "
                     "rollback=true no_visible_catalog_effect=true\n";
        return 0;
      }
      std::cout << "CSC-TEST-005780 DDL_CREATE_SCHEMA accepted "
                   "canonical_sblr=true catalog_mutation=true "
                   "commit=true publication_barrier=passed "
                   "authenticated_recovery=true exact_csrs_replay=true\n";
      return 0;
    }

    if (operation == "ddl-create-schema-recover") {
      const auto recovery_request = read_recovery_artifact(".csrq");
      const auto expected_result = read_recovery_artifact(".csrs");
      auto recovered =
          session.RecoverDdlCreateSchemaForWire(recovery_request);
      ddl::SblrDdlCreateSchemaResultV1 terminal;
      std::string detail;
      const bool exact_recovery =
          !recovery_request.empty() && !expected_result.empty() &&
          recovered.accepted && !recovered.outcome_unknown &&
          !recovered.messages.has_errors() &&
          recovered.server_operation_id ==
              "engine.op.ddl_create_schema.recover" &&
          recovered.sblr_payload.empty() &&
          recovered.server_result_payload == std::string(
              reinterpret_cast<const char*>(expected_result.data()),
              expected_result.size()) &&
          ddl::DecodeSblrDdlCreateSchemaResultV1(
              expected_result.data(), expected_result.size(), &terminal,
              &detail);
      auto observed = session.RunPipeline(
          "RESOLVE NAME qa_schema AS SCHEMA;", true);
      ddl::SblrNameResolveResultV1 resolved;
      const bool exact_observer = decode_name_result(observed, &resolved) &&
                                  resolved.status == 1 &&
                                  resolved.visibility == 1 &&
                                  resolved.object_class == 3 &&
                                  resolved.resolved_object_uuid ==
                                      terminal.schema_uuid;
      auto rolled_back = session.RunPipeline("ROLLBACK TRANSACTION", true);
      if (!exact_recovery || !exact_observer || !rolled_back.accepted ||
          rolled_back.messages.has_errors()) {
        dump_failure("restarted_recovery_failed", recovered);
        return 4;
      }
      std::cout << "CSC-TEST-005785 DDL_CREATE_SCHEMA_RECOVERY accepted "
                   "new_session=true restarted_server=true "
                   "byte_identical_csrs=true exact_schema_visible=true "
                   "no_rebind=true\n";
      return 0;
    }

    if (operation == "ddl-create-schema-duplicate") {
      auto before_result =
          session.RunPipeline("RESOLVE NAME qa_schema AS SCHEMA;", true);
      ddl::SblrNameResolveResultV1 before;
      if (!decode_name_result(before_result, &before) || before.status != 1 ||
          before.visibility != 1 || before.object_class != 3 ||
          !nonzero(before.resolved_object_uuid)) {
        dump_failure("duplicate_before_observer_failed", before_result);
        return 4;
      }
      auto duplicate =
          session.RunPipeline("CREATE SCHEMA qa_schema;", true);
      const bool exact_refusal =
          !duplicate.accepted && !duplicate.outcome_unknown &&
          duplicate.server_result_payload.empty() &&
          std::ranges::any_of(
              duplicate.messages.diagnostics, [](const auto& diagnostic) {
                return diagnostic.code == "CATALOG.NAME.AMBIGUOUS";
              });
      if (!exact_refusal) {
        dump_failure("duplicate_refusal_failed", duplicate);
        return 4;
      }
      auto after_result =
          session.RunPipeline("RESOLVE NAME qa_schema AS SCHEMA;", true);
      ddl::SblrNameResolveResultV1 after;
      if (!decode_name_result(after_result, &after) || after.status != 1 ||
          after.visibility != 1 ||
          after.resolved_object_uuid != before.resolved_object_uuid ||
          after.object_descriptor_generation !=
              before.object_descriptor_generation) {
        dump_failure("duplicate_after_observer_failed", after_result);
        return 4;
      }
      auto rolled_back = session.RunPipeline("ROLLBACK TRANSACTION", true);
      if (!rolled_back.accepted || rolled_back.messages.has_errors()) {
        dump_failure("duplicate_rollback_failed", rolled_back);
        return 4;
      }
      std::cout << "CSC-TEST-005781 DDL_CREATE_SCHEMA "
                   "duplicate_refusal=CATALOG.NAME.AMBIGUOUS "
                   "no_catalog_mutation=true\n";
      return 0;
    }

    const bool absent_case =
        operation == "ddl-create-schema-observe-absent";
    auto observed = session.RunPipeline(
        absent_case ? "RESOLVE NAME qa_rolled_back_schema AS SCHEMA;"
                    : "RESOLVE NAME qa_schema AS SCHEMA;",
        true);
    ddl::SblrNameResolveResultV1 resolved;
    const bool exact_observer = decode_name_result(observed, &resolved) &&
        resolved.object_class == 3 &&
        (absent_case
             ? resolved.status == 2 && resolved.visibility == 2 &&
                   !nonzero(resolved.resolved_object_uuid) &&
                   !nonzero(resolved.resolved_namespace_uuid)
             : resolved.status == 1 && resolved.visibility == 1 &&
                   nonzero(resolved.resolved_object_uuid) &&
                   nonzero(resolved.resolved_namespace_uuid) &&
                   resolved.object_descriptor_generation != 0);
    if (!exact_observer) {
      dump_failure("observer_contract_failed", observed);
      return 4;
    }
    auto rolled_back = session.RunPipeline("ROLLBACK TRANSACTION", true);
    if (!rolled_back.accepted || rolled_back.messages.has_errors()) {
      dump_failure("observer_rollback_failed", rolled_back);
      return 4;
    }
    if (absent_case) {
      std::cout << "CSC-TEST-005783 DDL_CREATE_SCHEMA "
                   "observer_absent=true independent_session=true\n";
    } else {
      std::cout << "CSC-TEST-005780 DDL_CREATE_SCHEMA observer_visible=true "
                   "independent_session=true exact_schema_identity=true\n";
    }
    return 0;
  }
  auto result = operation == "show-version"
                    ? session.RunShowVersionForWire()
                    : operation == "show-wait-events"
                    ? session.RunShowWaitEventsForWire()
                    : operation == "error-vector"
                    ? session.RunErrorVectorForWire()
                    : operation == "txn-commit"
                          ? [&session] {
                              auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
                              if (!begun.accepted) return begun;
                              auto committed = session.RunPipeline("COMMIT TRANSACTION", true);
                              if (!committed.accepted) return committed;
                              const auto replay =
                                  session.RunRetiredTransactionCommitReplayForWire();
                              const bool stale = !replay.accepted &&
                                  !replay.messages.diagnostics.empty() &&
                                  replay.messages.diagnostics.front().code ==
                                      "MGA.TRANSACTION.STALE";
                              if (!stale) {
                                committed.accepted = false;
                                committed.messages.diagnostics.push_back(
                                    {"MGA.TRANSACTION.REPLAY_NOT_REFUSED", "ERROR",
                                     "Retired transaction handle replay was not refused as stale.",
                                     "sbsql_sblr_alignment"});
                              }
                              return committed;
                            }()
                    : operation == "txn-rollback"
                          ? [&session] {
                              auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
                              if (!begun.accepted) return begun;
                              auto rolled_back =
                                  session.RunPipeline("ROLLBACK TRANSACTION", true);
                              if (!rolled_back.accepted) return rolled_back;
                              const auto replay =
                                  session.RunRetiredTransactionRollbackReplayForWire();
                              const bool stale = !replay.accepted &&
                                  !replay.messages.diagnostics.empty() &&
                                  replay.messages.diagnostics.front().code ==
                                      "MGA.TRANSACTION.STALE";
                              if (!stale) {
                                rolled_back.accepted = false;
                                rolled_back.messages.diagnostics.push_back(
                                    {"MGA.TRANSACTION.REPLAY_NOT_REFUSED", "ERROR",
                                     "Retired transaction handle replay was not refused as stale.",
                                     "sbsql_sblr_alignment"});
                              }
                              return rolled_back;
                            }()
                    : operation == "txn-savepoint"
                          ? [&session] {
                              auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
                              if (!begun.accepted) return begun;
                              auto created =
                                  session.RunPipeline("SAVEPOINT alignment_point", true);
                              if (!created.accepted) return created;
                              const auto replay =
                                  session.RunRetiredSavepointReplayForWire();
                              const bool stale = !replay.accepted &&
                                  !replay.messages.diagnostics.empty() &&
                                  (replay.messages.diagnostics.front().code ==
                                       "MGA.SAVEPOINT.STALE" ||
                                   replay.messages.diagnostics.front().code ==
                                       "MGA.TRANSACTION.STALE");
                              if (!stale) {
                                created.accepted = false;
                                created.messages.diagnostics.push_back(
                                    {"MGA.SAVEPOINT.REPLAY_NOT_REFUSED", "ERROR",
                                     "Consumed savepoint descriptor replay was not refused as stale.",
                                     "sbsql_sblr_alignment"});
                              }
                              return created;
                            }()
                    : operation == "txn-release-savepoint"
                          ? [&session] {
                              auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
                              if (!begun.accepted) return begun;
                              auto created = session.RunPipeline(
                                  "SAVEPOINT alignment_point", true);
                              if (!created.accepted) return created;
                              auto descendant = session.RunPipeline(
                                  "SAVEPOINT descendant_point", true);
                              if (!descendant.accepted) return descendant;
                              auto released =
                                  session.RunReleaseParentSavepointForWire();
                              if (!released.accepted) return released;
                              const auto replay =
                                  session.RunReleasedSavepointReplayForWire();
                              const bool stale = !replay.accepted &&
                                  !replay.messages.diagnostics.empty() &&
                                  (replay.messages.diagnostics.front().code ==
                                       "MGA.SAVEPOINT.STALE" ||
                                   replay.messages.diagnostics.front().code ==
                                       "MGA.TRANSACTION.STALE");
                              if (!stale) {
                                released.accepted = false;
                                released.messages.diagnostics.push_back(
                                    {"MGA.SAVEPOINT.REPLAY_NOT_REFUSED", "ERROR",
                                     "Released savepoint handle replay was not refused as stale.",
                                     "sbsql_sblr_alignment"});
                              }
                              return released;
                            }()
                    : operation == "txn-rollback-to-savepoint"
                          ? [&session] {
                              auto begun=session.RunPipeline("BEGIN TRANSACTION",true); if(!begun.accepted)return begun;
                              auto parent=session.RunPipeline("SAVEPOINT alignment_point",true); if(!parent.accepted)return parent;
                              auto child=session.RunPipeline("SAVEPOINT descendant_point",true); if(!child.accepted)return child;
                              auto rolled=session.RunRollbackParentSavepointForWire(); if(!rolled.accepted)return rolled;
                              const auto descendant_stale=session.RunRolledBackDescendantForWire();
                              const bool descendant_refused=!descendant_stale.accepted&&!descendant_stale.messages.diagnostics.empty()&&
                                  descendant_stale.messages.diagnostics.front().code=="MGA.SAVEPOINT.STALE";
                              if(!descendant_refused){rolled.accepted=false;rolled.messages.diagnostics.push_back({"MGA.SAVEPOINT.DESCENDANT_NOT_STALE","ERROR","Rolled-back descendant savepoint remained visible.","sbsql_sblr_alignment"});return rolled;}
                              const auto replay=session.RunRolledBackSavepointReplayForWire();
                              const bool stale=!replay.accepted&&!replay.messages.diagnostics.empty()&&
                                  (replay.messages.diagnostics.front().code=="MGA.SAVEPOINT.STALE"||replay.messages.diagnostics.front().code=="MGA.TRANSACTION.STALE");
                              if(!stale){rolled.accepted=false;rolled.messages.diagnostics.push_back({"MGA.SAVEPOINT.REPLAY_NOT_REFUSED","ERROR","Old rollback-to-savepoint authority was not stale.","sbsql_sblr_alignment"});}
                              return rolled;
                            }()
                    : operation == "psql-autonomous-frame"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunAutonomousFrameForWire():begun; }()
                    : operation == "transaction-reservation-release"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunReservationReleaseForWire():begun; }()
                    : operation == "temporary-instance-cleanup"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunTemporaryInstanceCleanupForWire():begun; }()
                    : operation == "cursor-open"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunCursorOpenForWire():begun; }()
                    : operation == "cursor-fetch"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);if(!begun.accepted)return begun;auto opened=session.RunCursorOpenForWire();return opened.accepted?session.RunCursorFetchForWire():opened; }()
                    : operation == "cursor-close"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);if(!begun.accepted)return begun;auto opened=session.RunCursorOpenForWire();return opened.accepted?session.RunCursorCloseForWire():opened; }()
                    : operation == "read-by-key"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReadByKeyForWire():begun; }()
                    : operation == "read-range"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReadRangeForWire():begun; }()
                    : operation == "read-stream"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReadStreamForWire():begun; }()
                    : operation == "result-set-pass"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunResultSetPassForWire():begun; }()
                    : operation == "access-cursor-open"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAccessCursorOpenForWire():begun; }()
                    : operation == "access-cursor-fetch"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);if(!begun.accepted)return begun;auto opened=session.RunAccessCursorOpenForWire();return opened.accepted?session.RunAccessCursorFetchForWire():opened; }()
                    : operation == "access-cursor-close"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);if(!begun.accepted)return begun;auto opened=session.RunAccessCursorOpenForWire();return opened.accepted?session.RunAccessCursorCloseForWire():opened; }()
                    : operation == "insert"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunInsertForWire():begun; }()
                    : operation == "update"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunUpdateForWire():begun; }()
                    : operation == "delete"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDeleteForWire():begun; }()
                    : operation == "merge"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunMergeForWire():begun; }()
                    : operation == "table-truncate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunTableTruncateForWire():begun; }()
                    : operation == "table-analyze"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunTableAnalyzeForWire():begun; }()
                    : operation == "bulk-import-stream"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunBulkImportStreamForWire():begun; }()
                    : operation == "bulk-export-stream"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunBulkExportStreamForWire():begun; }()
                    : operation == "statement-batch"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunStatementBatchForWire():begun; }()
                    : operation == "atomic-cas"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAtomicCasForWire():begun; }()
                    : operation == "atomic-rmw"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAtomicRmwForWire():begun; }()
                    : operation == "advisory-lock"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAdvisoryLockForWire():begun; }()
                    : operation == "advisory-lock-release"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAdvisoryLockReleaseForWire():begun; }()
                    : operation == "function-call"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunFunctionCallForWire():begun; }()
                    : operation == "operator-call"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunOperatorCallForWire():begun; }()
                    : operation == "cast"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunCastForWire():begun; }()
                    : operation == "compare"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunCompareForWire():begun; }()
                    : operation == "domain-operation"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDomainOperationForWire():begun; }()
                    : operation == "udr-invoke"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunUdrInvokeForWire():begun; }()
                    : operation == "procedure-invoke"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunProcedureInvokeForWire("EXECUTE PROCEDURE app.alignment_null_procedure;", false):begun; }()
                    : operation == "function-invoke"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunFunctionInvokeForWire():begun; }()
                    : operation == "aggregate-invoke"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAggregateInvokeForWire():begun; }()
                    : operation == "sequence-nextval"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunSequenceNextvalForWire():begun; }()
                    : operation == "sequence-currval"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunSequenceCurrvalForWire():begun; }()
                    : operation == "sequence-setval"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunSequenceSetvalForWire():begun; }()
                    : operation == "query-numeric"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunQueryNumericForWire():begun; }()
                    : operation == "advanced-datatype-family"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunQueryEvaluateAdvancedDatatypeFamilyForWire():begun; }()
                    : operation == "project"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunProjectForWire():begun; }()
                    : operation == "show-object-detail"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunShowObjectDetailForWire():begun; }()
                    : operation == "name-resolve"
                          ? [&session] {
                              auto begun = session.RunPipeline(
                                  "BEGIN TRANSACTION", true);
                              return begun.accepted
                                         ? session.RunPipeline(
                                               "RESOLVE NAME app.customers AS table;",
                                               true)
                                         : begun;
                            }()
                    : operation == "optimizer-stats-read"
                          ? [&session] {
                              auto begun = session.RunPipeline(
                                  "BEGIN TRANSACTION", true);
                              return begun.accepted
                                         ? session.RunPipeline(
                                               "OPTIMIZER STATS READ;", true)
                                         : begun;
                            }()
                    : operation == "optimizer-stats-drop"
                          ? [&session] {
                              auto begun = session.RunPipeline(
                                  "BEGIN TRANSACTION", true);
                              if (!begun.accepted) return begun;
                              auto dropped = session.RunPipeline(
                                  "OPTIMIZER STATS DROP;", true);
                              if (!dropped.accepted) return dropped;
                              auto committed = session.RunPipeline(
                                  "COMMIT", true);
                              return committed.accepted ? dropped : committed;
                            }()
                    : operation == "parse-text"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunParseTextForWire():begun; }()
                    : operation == "catalog-epoch-check"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunCatalogEpochCheckForWire():begun; }()
                    : operation == "database-attach"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDatabaseAttachForWire():begun; }()
                    : operation == "database-detach"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDatabaseDetachForWire():begun; }()
                    : operation == "database-checkpoint"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDatabaseCheckpointForWire():begun; }()
                    : operation == "database-vacuum"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDatabaseVacuumForWire():begun; }()
                    : operation == "database-alter"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunDatabaseAlterForWire():begun; }()
                    : operation == "lifecycle-create-database"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleCreateDatabaseForWire():begun; }()
                    : operation == "lifecycle-open-database"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleOpenDatabaseForWire():begun; }()
                    : operation == "lifecycle-attach-database"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleAttachDatabaseForWire():begun; }()
                    : operation == "lifecycle-detach-database"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleDetachDatabaseForWire():begun; }()
                    : operation == "lifecycle-enter-maintenance"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleEnterMaintenanceForWire():begun; }()
                    : operation == "lifecycle-exit-maintenance"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleExitMaintenanceForWire():begun; }()
                    : operation == "lifecycle-enter-restricted-open"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleEnterRestrictedOpenForWire():begun; }()
                    : operation == "lifecycle-exit-restricted-open"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleExitRestrictedOpenForWire():begun; }()
                    : operation == "lifecycle-inspect-database"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleInspectDatabaseForWire():begun; }()
                    : operation == "lifecycle-verify-database"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleVerifyDatabaseForWire():begun; }()
                    : operation == "lifecycle-repair-database"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleRepairDatabaseForWire():begun; }()
                    : operation == "lifecycle-shutdown-database"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleShutdownDatabaseForWire():begun; }()
                    : operation == "lifecycle-shutdown-force"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleShutdownForceForWire():begun; }()
                    : operation == "lifecycle-shutdown-acknowledge"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleShutdownAcknowledgeForWire():begun; }()
                    : operation == "lifecycle-drop-database"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLifecycleDropDatabaseForWire():begun; }()
                    : operation == "repl-consumer-subscribe"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReplConsumerSubscribeForWire():begun; }()
                    : operation == "repl-consumer-resume"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReplConsumerResumeForWire():begun; }()
                    : operation == "repl-consumer-pause"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReplConsumerPauseForWire():begun; }()
                    : operation == "repl-consumer-cancel"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReplConsumerCancelForWire():begun; }()
                    : operation == "repl-cdc-receive"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReplCdcReceiveForWire():begun; }()
                    : operation == "repl-cdc-ack"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunReplCdcAckForWire():begun; }()
                    : operation == "repl-2pc-prewrite"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunRepl2pcPrewriteForWire():begun; }()
                    : operation == "repl-2pc-commit"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunRepl2pcCommitForWire():begun; }()
                    : operation == "repl-2pc-cleanup"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunRepl2pcCleanupForWire():begun; }()
                    : operation == "repl-2pc-resolve-lock"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunRepl2pcResolveLockForWire():begun; }()
                    : operation == "repl-2pc-pessimistic-lock"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunRepl2pcPessimisticLockForWire():begun; }()
                    : operation == "repl-2pc-pessimistic-rollback"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunRepl2pcPessimisticRollbackForWire():begun; }()
                    : operation == "repl-2pc-heartbeat"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunRepl2pcHeartbeatForWire():begun; }()
                    : operation == "repl-2pc-check-status"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunRepl2pcCheckStatusForWire():begun; }()
                    : operation == "graph-traverse"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunGraphTraverseForWire():begun; }()
                    : operation == "graph-optional-match"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunGraphOptionalMatchForWire():begun; }()
                    : operation == "graph-create"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunGraphCreateForWire():begun; }()
                    : operation == "graph-merge"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunGraphMergeForWire():begun; }()
                    : operation == "graph-set"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunGraphSetForWire():begun; }()
                    : operation == "graph-remove"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunGraphRemoveForWire():begun; }()
                    : operation == "graph-delete"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunGraphDeleteForWire():begun; }()
                    : operation == "graph-detach-delete"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunGraphDetachDeleteForWire():begun; }()
                    : operation == "fulltext-score"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunFulltextScoreForWire():begun; }()
                    : operation == "fulltext-phrase-score"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunFulltextPhraseScoreForWire():begun; }()
                    : operation == "fulltext-multi-field-score"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunFulltextMultiFieldScoreForWire():begun; }()
                    : operation == "fulltext-regex-match"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunFulltextRegexMatchForWire():begun; }()
                    : operation == "fulltext-wildcard-match"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunFulltextWildcardMatchForWire():begun; }()
                    : operation == "fulltext-prefix-match"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunFulltextPrefixMatchForWire():begun; }()
                    : operation == "fulltext-analyzer-apply"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunFulltextAnalyzerApplyForWire():begun; }()
                    : operation == "aggregate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunAggregateForWire():begun; }()
                    : operation == "group"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunGroupForWire():begun; }()
                    : operation == "sort"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunSortForWire():begun; }()
                    : operation == "limit"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunLimitForWire():begun; }()
                    : operation == "window"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunWindowForWire():begun; }()
                    : operation == "return-result-set"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunReturnResultSetForWire():begun; }()
                    : operation == "kv-structured-read"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredReadForWire():begun; }()
                    : operation == "kv-structured-mutate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredMutateForWire():begun; }()
                    : operation == "kv-structured-scan"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredScanForWire():begun; }()
                    : operation == "kv-structured-stream-read"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredStreamReadForWire():begun; }()
                    : operation == "kv-structured-stream-append"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredStreamAppendForWire():begun; }()
                    : operation == "kv-structured-timeseries"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunKvStructuredTimeseriesForWire():begun; }()
                    : operation == "system-config-set"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSystemConfigSetForWire():begun; }()
                    : operation == "system-config-get"
                          ? session.RunSystemConfigGetForWire()
                    : operation == "system-config-reset"
                          ? session.RunSystemConfigResetForWire()
                    : operation == "ddl-create-rule"
                          ? session.RunDdlCreateRuleForWire()
                    : operation == "ddl-drop-rule"
                          ? session.RunDdlDropRuleForWire()
                    : operation == "ddl-create-publication"
                          ? session.RunDdlCreatePublicationForWire()
                    : operation == "ddl-alter-publication"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterPublicationForWire():begun; }()
                    : operation == "ddl-drop-publication"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropPublicationForWire():begun; }()
                    : operation == "ddl-create-subscription"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateSubscriptionForWire():begun; }()
                    : operation == "ddl-alter-subscription"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterSubscriptionForWire():begun; }()
                    : operation == "ddl-drop-subscription"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropSubscriptionForWire():begun; }()
                    : operation == "ddl-create-operator"
                          ? session.RunDdlCreateOperatorForWire()
                    : operation == "ddl-drop-operator"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropOperatorForWire():begun; }()
                    : operation == "ddl-create-operator-class"
                          ? session.RunDdlCreateOperatorClassForWire()
                    : operation == "ddl-drop-operator-class"
                          ? session.RunDdlDropOperatorClassForWire()
                    : operation == "ddl-create-operator-family"
                          ? session.RunDdlCreateOperatorFamilyForWire()
                    : operation == "ddl-alter-operator-family"
                          ? session.RunDdlAlterOperatorFamilyForWire()
                    : operation == "ddl-drop-operator-family"
                          ? session.RunDdlDropOperatorFamilyForWire()
                    : operation == "ddl-drop-cast"
                          ? session.RunDdlDropCastForWire()
                    : operation == "ddl-create-extension"
                          ? session.RunDdlCreateExtensionForWire()
                    : operation == "ddl-alter-extension"
                          ? session.RunDdlAlterExtensionForWire()
                    : operation == "ddl-drop-extension"
                          ? session.RunDdlDropExtensionForWire()
                    : operation == "cluster-create-placement-policy"
                          ? session.RunClusterCreatePlacementPolicyForWire()
                    : operation == "cluster-alter-placement-policy"
                          ? session.RunClusterAlterPlacementPolicyForWire()
                    : operation == "cluster-drop-placement-policy"
                          ? session.RunClusterDropPlacementPolicyForWire()
                    : operation == "versioned-branch-create"
                          ? session.RunVersionedBranchCreateForWire()
                    : operation == "versioned-branch-delete"
                          ? session.RunVersionedBranchDeleteForWire()
                    : operation == "versioned-diff"
                          ? session.RunVersionedDiffForWire()
                    : operation == "versioned-tag"
                          ? session.RunVersionedTagForWire()
                    : operation == "versioned-revert"
                          ? session.RunVersionedRevertForWire()
                    : operation == "versioned-reset"
                          ? session.RunVersionedResetForWire()
                    : operation == "bitemporal-as-of"
                          ? session.RunBitemporalAsOfForWire()
                    : operation == "verifiable-history-prove"
                          ? session.RunVerifiableHistoryProveForWire()
                    : operation == "verify-proof-descriptor"
                          ? session.RunVerifyProofDescriptorForWire()
                    : operation == "versioned-merge"
                          ? session.RunVersionedMergeForWire()
                    : operation == "versioned-hash-read"
                          ? session.RunVersionedHashReadForWire()
                    : operation == "versioned-status-read"
                          ? session.RunVersionedStatusReadForWire()
                    : operation == "accel-llvm-policy-set"
                          ? session.RunAccelLlvmPolicySetForWire()
                    : operation == "accel-llvm-compile"
                          ? session.RunAccelLlvmCompileForWire()
                    : operation == "accel-gpu-compile"
                          ? session.RunAccelGpuCompileForWire()
                    : operation == "accel-llvm-inspect"
                          ? session.RunAccelLlvmInspectForWire()
                    : operation == "accel-llvm-invalidate"
                          ? session.RunAccelLlvmInvalidateForWire()
                    : operation == "accel-gpu-policy-set"
                          ? session.RunAccelGpuPolicySetForWire()
                    : operation == "accel-gpu-inspect"
                          ? session.RunAccelGpuInspectForWire()
                    : operation == "accel-gpu-invalidate"
                          ? session.RunAccelGpuInvalidateForWire()
                    : operation == "bridge-describe-capabilities"
                          ? session.RunBridgeDescribeCapabilitiesForWire()
                    : operation == "bridge-open-channel"
                          ? session.RunBridgeOpenChannelForWire()
                    : operation == "bridge-authenticate"
                          ? session.RunBridgeAuthenticateForWire()
                    : operation == "bridge-open-session"
                          ? session.RunBridgeOpenSessionForWire()
                    : operation == "bridge-close-session"
                          ? session.RunBridgeCloseSessionForWire()
                    : operation == "bridge-health"
                          ? session.RunBridgeHealthForWire()
                    : operation == "bridge-begin-transaction"
                          ? session.RunBridgeBeginTransactionForWire()
                    : operation == "bridge-commit-transaction"
                          ? session.RunBridgeCommitTransactionForWire()
                    : operation == "bridge-rollback-transaction"
                          ? session.RunBridgeRollbackTransactionForWire()
                    : operation == "alter-gpu-profile-disable"
                          ? session.RunGpuProfileDisableRefusalForWire()
                    : operation == "filespace-create"
                          ? session.RunDiagnosticRefusalForWire()
                    : operation == "diagnostic-refusal"
                          ? session.RunDiagnosticRefusalForWire()
                    : operation == "diagnostic-reset"
                          ? session.RunDiagnosticResetForWire()
                    : operation == "descriptor-transform"
                          ? session.RunDescriptorTransformForWire()
                    : operation == "migration-begin-donor"
                          ? session.RunMigrationBeginDonorForWire()
                    : operation == "migration-alter"
                          ? session.RunMigrationAlterForWire()
                    : operation == "show-migration"
                          ? session.RunShowMigrationForWire()
                    : operation == "migration-cutover"
                          ? session.RunMigrationCutoverForWire()
                    : operation == "migration-rollback"
                          ? session.RunMigrationRollbackForWire()
                    : operation == "migration-retain-evidence"
                          ? session.RunMigrationRetainEvidenceForWire()
                    : operation == "internal-trigger-dispatch"
                          ? session.RunInternalTriggerDispatchForWire()
                    : operation == "internal-exception-raise"
                          ? session.RunInternalExceptionRaiseForWire()
                    : operation == "internal-exception-resignal"
                          ? session.RunInternalExceptionResignalForWire()
                    : operation == "ddl-create-domain"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateDomainForWire():begun; }()
                    : operation == "ddl-create-sequence"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateSequenceForWire():begun; }()
                    : operation == "ddl-create-materialized-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateMaterializedViewForWire():begun; }()
                    : operation == "ddl-create-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateViewForWire():begun; }()
                    : operation == "ddl-drop-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropViewForWire():begun; }()
                    : operation == "ddl-refresh-materialized-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlRefreshMaterializedViewForWire():begun; }()
                    : operation == "ddl-drop-synonym"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropSynonymForWire():begun; }()
                    : operation == "ddl-drop-foreign-table"
                          ? session.RunDdlDropForeignTableForWire()
                    : operation == "ddl-drop-package"
                          ? session.RunDdlDropPackageForWire()
                    : operation == "ddl-alter-package"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterPackageForWire():begun; }()
                    : operation == "ddl-alter-sequence"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterSequenceForWire():begun; }()
                    : operation == "ddl-drop-sequence"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropSequenceForWire():begun; }()
                    : operation == "ddl-drop-materialized-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropMaterializedViewForWire():begun; }()
                    : operation == "ddl-create-type"
                          ? session.RunPipeline(
                                "CREATE TYPE replay_type AS (value TEXT);",
                                true)
                    : operation == "ddl-create-table-as-query-with-data"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateTableAsQueryWithDataForWire():begun; }()
                    : operation == "ddl-create-table-as-query-with-no-data"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateTableAsQueryWithNoDataForWire():begun; }()
                    : operation == "ddl-alter-type"
                          ? session.RunPipeline(
                                "ALTER TYPE replay_type ADD ATTRIBUTE extra TEXT;",
                                true)
                    : operation == "ddl-drop-type"
                          ? session.RunPipeline("DROP TYPE replay_type;", true)
                    : operation == "ddl-drop-table"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropTableForWire():begun; }()
                    : operation == "ddl-create-trigger"
                          ? session.RunPipeline(
                                "CREATE TRIGGER app.trig_items_ai AFTER INSERT ON TABLE app.trig_items FOR EACH ROW AS BEGIN INSERT INTO app.trig_audit (audit_id, event_kind, item_id, old_price, new_price, audit_note) VALUES (NEXT VALUE FOR app.trig_audit_seq, 'INSERT', new.item_id, NULL, new.item_price, 'item inserted'); END;",
                                true)
                    : operation == "ddl-alter-trigger"
                          ? session.RunPipeline(
                                "ALTER TRIGGER app.trig_items_ai INACTIVE;",
                                true)
                    : operation == "ddl-drop-trigger"
                          ? session.RunPipeline(
                                "DROP TRIGGER app.trig_items_ai RESTRICT;",
                                true)
                    : operation == "ddl-create-procedure"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateProcedureForWire("CREATE PROCEDURE app.alignment_null_procedure AS BEGIN NULL; END;", false):begun; }()
                    : operation == "ddl-alter-procedure"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterProcedureForWire():begun; }()
                    : operation == "ddl-drop-procedure"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropProcedureForWire():begun; }()
                    : operation == "ddl-create-function"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateFunctionForWire():begun; }()
                    : operation == "ddl-alter-function"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterFunctionForWire():begun; }()
                    : operation == "ddl-create-package"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreatePackageForWire():begun; }()
                    : operation == "ddl-create-temporary-table"
                          ? session.RunDdlCreateTemporaryTableForWire()
                    : operation == "ddl-drop-temporary-table"
                          ? session.RunDdlDropTemporaryTableForWire()
                    : operation == "ddl-rename-object-vector"
                          ? session.RunDdlRenameObjectVectorForWire()
                    : operation == "ddl-rename-object"
                          ? session.RunDdlRenameObjectForWire()
                    : operation == "ddl-create-synonym"
                          ? session.RunDdlCreateSynonymForWire()
                    : operation == "ddl-create-foreign-table"
                                ? session.RunDdlCreateForeignTableForWire()
                    : operation == "ddl-create-fdw"
                                ? session.RunDdlCreateFdwForWire()
                    : operation == "ddl-drop-fdw"
                                ? session.RunDdlDropFdwForWire()
                    : operation == "ddl-create-or-replace-srs"
                          ? session.RunDdlCreateOrReplaceSrsForWire()
                    : operation == "ddl-drop-srs"
                          ? session.RunDdlDropSrsForWire()
                    : operation == "ddl-create-rewrite-rule"
                          ? session.RunDdlCreateRewriteRuleForWire()
                    : operation == "ddl-alter-rewrite-rule"
                          ? session.RunDdlAlterRewriteRuleForWire()
                    : operation == "ddl-drop-rewrite-rule"
                          ? session.RunDdlDropRewriteRuleForWire()
                    : operation == "ddl-validate-constraint"
                          ? session.RunDdlValidateConstraintForWire()
                    : operation == "security-create-privilege-template"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityCreatePrivilegeTemplateForWire():begun; }()
                    : operation == "security-create-user"
                          ? session.RunSecurityCreateUserForWire()
                    : operation == "security-alter-user"
                          ? session.RunSecurityAlterUserForWire()
                    : operation == "security-create-role"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityCreateRoleForWire():begun; }()
                    : operation == "security-create-policy"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityCreatePolicyForWire():begun; }()
                    : operation == "security-drop-policy"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityDropPolicyForWire():begun; }()
                    : operation == "security-alter-policy"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityAlterPolicyForWire():begun; }()
                    : operation == "security-drop-user"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityDropUserForWire():begun; }()
                    : operation == "security-authenticate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityAuthenticateForWire():begun; }()
                    : operation == "security-deauthenticate"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityDeauthenticateForWire():begun; }()
                    : operation == "session-role-switch"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionRoleSwitchForWire():begun; }()
                    : operation == "session-setting-set"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionSettingSetForWire():begun; }()
                    : operation == "session-setting-reset"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionSettingResetForWire():begun; }()
                    : operation == "session-setting-get"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionSettingGetForWire():begun; }()
                    : operation == "session-default-qualifier-set"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionDefaultQualifierSetForWire():begun; }()
                    : operation == "session-discard"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionDiscardForWire():begun; }()
                    : operation == "session-snapshot-handle"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSessionSnapshotHandleForWire():begun; }()
                    : operation == "context-set"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunContextSetForWire():begun; }()
                    : operation == "context-unset"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunContextUnsetForWire():begun; }()
                    : operation == "context-get"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunContextGetForWire():begun; }()
                    : operation == "stmt-prepare"
                          ? session.RunPipeline(
                                "PREPARE prep_one AS SELECT 1;",
                                true)
                    : operation == "stmt-execute"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunStmtExecuteForWire(true):begun; }()
                    : operation == "stmt-execute-direct"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunStmtExecuteDirectForWire(true):begun; }()
                    : operation == "stmt-free"
                          ? session.RunStmtFreeForWire()
                    : operation == "stmt-cancel"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunStmtCancelForWire():begun; }()
                    : operation == "parameter-bind"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunParameterBindForWire():begun; }()
                    : operation == "parameter-bind-multi-nullable"
        ? [&session] {
            auto begun = session.RunPipeline("BEGIN TRANSACTION", true);
            if (!begun.accepted || begun.messages.has_errors()) return begun;
            auto prepared = session.RunPipeline(
                "PREPARE prep_parameter_multi (BIGINT, BIGINT) AS VALUES (?, ?);",
                true);
            if (!prepared.accepted || prepared.messages.has_errors()) {
              return prepared;
            }
            return session.RunPipeline(
                "EXECUTE prep_parameter_multi (7, NULL);", true, true);
          }()
                    : operation == "result-page"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunResultPageForWire():begun; }()
                    : operation == "query-execute"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunQueryExecuteForWire():begun; }()
                    : operation == "query-explain"
        ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunQueryExplainForWire():begun; }()
                    : operation == "security-alter-role"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityAlterRoleForWire():begun; }()
                    : operation == "security-create-group-mapping"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityCreateGroupMappingForWire():begun; }()
                    : operation == "security-drop-group-mapping"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityDropGroupMappingForWire():begun; }()
                    : operation == "security-grant"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityGrantForWire():begun; }()
                    : operation == "security-revoke"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityRevokeForWire():begun; }()
                    : operation == "security-drop-role"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunSecurityDropRoleForWire():begun; }()
                    : operation == "security-alter-privilege-template"
                          ? session.RunSecurityAlterPrivilegeTemplateForWire()
                    : operation == "security-drop-privilege-template"
                          ? session.RunSecurityDropPrivilegeTemplateForWire()
                    : operation == "database-create-template-clone"
                          ? session.RunDatabaseCreateTemplateCloneForWire()
                    : operation == "ddl-create-aggregate"
                          ? session.RunDdlCreateAggregateForWire()
                    : operation == "ddl-create-macro"
                          ? session.RunDdlCreateMacroForWire()
                    : operation == "ddl-create-dictionary"
                          ? session.RunDdlCreateDictionaryForWire()
                    : operation == "ddl-drop-dictionary"
                          ? session.RunDdlDropDictionaryForWire()
                    : operation == "ddl-alter-dictionary"
                          ? session.RunDdlAlterDictionaryForWire()
                    : operation == "ddl-create-continuous-view"
                          ? session.RunDdlCreateContinuousViewForWire()
                    : operation == "ddl-alter-continuous-view"
                          ? session.RunDdlAlterContinuousViewForWire()
                    : operation == "ddl-drop-continuous-view"
                          ? session.RunDdlDropContinuousViewForWire()
                    : operation == "dml-async-insert-submit"
                          ? session.RunDmlAsyncInsertSubmitForWire()
                    : operation == "dml-async-insert-status"
                          ? session.RunDmlAsyncInsertStatusForWire()
                    : operation == "dml-counter-add"
                          ? session.RunDmlCounterAddForWire()
                          : operation == "dml-conditional-mutate"
                                ? session.RunDmlConditionalMutateForWire()
                    : operation == "dml-timeseries-schema-write"
                          ? session.RunDmlTimeseriesSchemaWriteForWire()
                    : operation == "ddl-timeseries-series-cardinality-policy"
                          ? session.RunDdlTimeseriesSeriesCardinalityPolicyForWire()
                    : operation == "ddl-create-timeseries-value-cache"
                          ? session.RunDdlCreateTimeseriesValueCacheForWire()
                          : operation == "ddl-alter-timeseries-value-cache"
                                ? session.RunDdlAlterTimeseriesValueCacheForWire()
                    : operation == "ddl-drop-timeseries-value-cache"
                          ? session.RunDdlDropTimeseriesValueCacheForWire()
                    : operation == "dml-async-insert-cancel"
                          ? session.RunDmlAsyncInsertCancelForWire()
                    : operation == "ddl-drop-macro"
                          ? session.RunDdlDropMacroForWire()
                    : operation == "admin-register-external-relation-resolver"
                          ? session.RunAdminRegisterExternalRelationResolverForWire()
                    : operation == "admin-unregister-external-relation-resolver"
                          ? session.RunAdminUnregisterExternalRelationResolverForWire()
                    : operation == "ddl-alter-aggregate"
                          ? session.RunDdlAlterAggregateForWire()
                    : operation == "ddl-drop-aggregate"
                          ? session.RunDdlDropAggregateForWire()
                    : operation == "ddl-purge-system-history"
                          ? session.RunDdlPurgeSystemHistoryForWire()
                    : operation == "ddl-set-index-optimizer-eligibility"
                          ? session.RunDdlSetIndexOptimizerEligibilityForWire()
                    : operation == "ddl-set-table-type-enforcement"
                          ? session.RunDdlSetTableTypeEnforcementForWire()
                    : operation == "database-serialize-logical-snapshot"
                          ? session.RunDatabaseSerializeLogicalSnapshotForWire()
                    : operation == "database-deserialize-logical-snapshot"
                          ? session.RunDatabaseDeserializeLogicalSnapshotForWire()
                    : operation == "ddl-drop-function"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropFunctionForWire():begun; }()
                    : operation == "ddl-alter-view"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterViewForWire():begun; }()
                    : operation == "ddl-alter-domain"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterDomainForWire():begun; }()
                    : operation == "ddl-create-schema"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateSchemaForWire():begun; }()
                    : operation == "ddl-create-table"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateTableForWire():begun; }()
                    : operation == "ddl-create-index"
                          ? session.RunPipeline(
                                "CREATE INDEX replay_target_id_idx ON "
                                "replay_target (id);",
                                true)
                    : operation == "ddl-drop-index"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropIndexForWire():begun; }()
                    : operation == "txn-begin"
                          ? session.RunPipeline("BEGIN TRANSACTION", true)
                          : session.RunSourceMapForWire();
  if (operation == "ddl-create-index") {
    const bool exact_refusal =
        !result.accepted && result.messages.diagnostics.size() == 1 &&
        result.messages.diagnostics.front().code == "SBSQL.IMPL.NOT_AVAILABLE";
    const bool no_canonical_result =
        result.sblr_payload.empty() && result.server_operation_id.empty() &&
        result.server_cursor_uuid.empty() && result.server_row_count == 0 &&
        result.server_affected_rows == 0 &&
        !result.server_affected_rows_present &&
        result.server_result_payload.empty();
    if (!exact_refusal || !no_canonical_result) {
      std::cerr << "CSC-TEST-002601 DDL_CREATE_INDEX fail_closed_contract_failed\n";
      return 4;
    }
    std::cout << "CSC-TEST-002601 DDL_CREATE_INDEX deterministic_refusal=SBSQL.IMPL.NOT_AVAILABLE no_canonical_execution=true\n";
    return 0;
  }
  if (operation == "ddl-create-type" || operation == "ddl-alter-type" ||
      operation == "ddl-drop-type") {
    const bool exact_refusal =
        !result.accepted && result.messages.diagnostics.size() == 1 &&
        result.messages.diagnostics.front().code == "SBSQL.IMPL.NOT_AVAILABLE";
    const bool no_canonical_result =
        result.sblr_payload.empty() && result.server_operation_id.empty() &&
        result.server_cursor_uuid.empty() && result.server_row_count == 0 &&
        result.server_affected_rows == 0 &&
        !result.server_affected_rows_present &&
        result.server_result_payload.empty();
    const char* test_id = operation == "ddl-create-type"
                              ? "CSC-TEST-002673"
                              : operation == "ddl-alter-type"
                                    ? "CSC-TEST-002675"
                                    : "CSC-TEST-002677";
    const char* operation_label = operation == "ddl-create-type"
                                      ? "DDL_CREATE_TYPE"
                                      : operation == "ddl-alter-type"
                                            ? "DDL_ALTER_TYPE"
                                            : "DDL_DROP_TYPE";
    if (!exact_refusal || !no_canonical_result) {
      std::cerr << test_id << ' ' << operation_label
                << " fail_closed_contract_failed\n";
      return 4;
    }
    std::cout << test_id << ' ' << operation_label
              << " deterministic_refusal=SBSQL.IMPL.NOT_AVAILABLE"
                 " no_canonical_execution=true\n";
    return 0;
  }
  if (operation == "stmt-prepare") {
    scratchbird::engine::sblr::SblrStmtPrepareResultV1 decoded;
    std::string detail;
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.stmt_prepare" &&
        scratchbird::engine::sblr::DecodeSblrStmtPrepareResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded, &detail) &&
        decoded.status == 1 && decoded.publication_barrier == 1 &&
        decoded.prepared_generation != 0 &&
        decoded.executor_availability_generation != 0;
    if (!exact_result) {
      std::cerr << "CSC-TEST-003573 STMT_PREPARE exact_result_failed"
                << " detail=" << detail << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
      return 4;
    }
    std::cout << "CSC-TEST-003573 STMT_PREPARE accepted "
                 "canonical_sblr=true publication_barrier=passed "
                 "surface_id=SBSQL-5535E9A48BE4 input=prepare_stmt\n";
    return 0;
  }
  if (operation == "stmt-execute") {
    scratchbird::engine::sblr::SblrStmtExecuteResultV1 decoded;
    std::string detail;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.stmt_execute" &&
        !result.server_cursor_uuid.empty() && result.server_row_count == 1 &&
        scratchbird::engine::sblr::DecodeSblrStmtExecuteResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded, &detail) &&
        decoded.status == 1 && decoded.publication_barrier == 1 &&
        decoded.executor_availability_generation != 0 &&
        nonzero(decoded.result_descriptor_uuid) &&
        nonzero(decoded.result_handle_uuid) &&
        nonzero(decoded.effect_evidence_sha256) &&
        nonzero(decoded.operation_evidence_uuid);
    if (!exact_result) {
      std::cerr << "CSC-TEST-003577 STMT_EXECUTE exact_result_failed"
                << " detail=" << detail
                << " payload_bytes=" << result.server_result_payload.size()
                << " payload_magic="
                << result.server_result_payload.substr(
                       0, std::min<std::size_t>(
                              4, result.server_result_payload.size()))
                << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
      return 4;
    }
    const auto fetched =
        session.FetchCursorOnRoute(result.server_cursor_uuid, 1);
    const bool exact_nested_result =
        fetched.accepted && fetched.row_count == 1 && fetched.end_of_cursor &&
        !fetched.row_packet.empty() &&
        fetched.row_packet.find("operation_id=engine.op.stmt_execute") !=
            std::string::npos &&
        fetched.row_packet.find("result_kind=stmt_execute_result") !=
            std::string::npos &&
        fetched.row_packet.find("row[0]=key_a=1") != std::string::npos &&
        fetched.row_packet.find("row_meta[0]=key_a:int64:not_null") !=
            std::string::npos;
    if (!exact_nested_result) {
      std::cerr << "CSC-TEST-003577 STMT_EXECUTE nested_result_failed"
                << " accepted=" << fetched.accepted
                << " row_count=" << fetched.row_count
                << " end_of_cursor=" << fetched.end_of_cursor
                << " row_packet=" << fetched.row_packet << '\n';
      for (const auto& diagnostic : fetched.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    std::cout << "CSC-TEST-003577 STMT_EXECUTE accepted "
                 "canonical_sblr=true publication_barrier=passed "
                 "result_handle=validated nested_row=key_a:1 "
                 "surface_id=SBSQL-414E9A624B34 "
                 "input=execute_prepared_stmt\n";
    return 0;
  }
  if (operation == "stmt-execute-direct") {
    scratchbird::engine::sblr::SblrStmtExecuteDirectResultV1 decoded;
    std::string detail;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.stmt_execute_direct" &&
        !result.server_cursor_uuid.empty() && result.server_row_count == 1 &&
        scratchbird::engine::sblr::DecodeSblrStmtExecuteDirectResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded, &detail) &&
        decoded.status == 1 && decoded.publication_barrier == 1 &&
        decoded.executor_availability_generation != 0 &&
        nonzero(decoded.result_descriptor_uuid) &&
        nonzero(decoded.result_handle_uuid) &&
        nonzero(decoded.effect_evidence_sha256) &&
        nonzero(decoded.operation_evidence_uuid);
    if (!exact_result) {
      std::cerr << "CSC-TEST-003581 STMT_EXECUTE_DIRECT exact_result_failed"
                << " detail=" << detail
                << " payload_bytes=" << result.server_result_payload.size()
                << " payload_magic="
                << result.server_result_payload.substr(
                       0, std::min<std::size_t>(4,
                                                result.server_result_payload.size()))
                << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
      return 4;
    }
    const auto fetched =
        session.FetchCursorOnRoute(result.server_cursor_uuid, 1);
    const bool exact_nested_result =
        fetched.accepted && fetched.row_count == 1 && fetched.end_of_cursor &&
        !fetched.row_packet.empty() &&
        fetched.row_packet.find(
            "operation_id=engine.op.stmt_execute_direct") !=
            std::string::npos &&
        fetched.row_packet.find("result_kind=stmt_execute_result") !=
            std::string::npos &&
        fetched.row_packet.find("row[0]=key_a=1") != std::string::npos &&
        fetched.row_packet.find("row_meta[0]=key_a:int64:not_null") !=
            std::string::npos;
    if (!exact_nested_result) {
      std::cerr << "CSC-TEST-003581 STMT_EXECUTE_DIRECT nested_result_failed"
                << " accepted=" << fetched.accepted
                << " row_count=" << fetched.row_count
                << " end_of_cursor=" << fetched.end_of_cursor
                << " row_packet=" << fetched.row_packet << '\n';
      for (const auto& diagnostic : fetched.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    std::cout << "CSC-TEST-003581 STMT_EXECUTE_DIRECT accepted "
                 "canonical_sblr=true publication_barrier=passed "
                 "result_handle=validated nested_row=key_a:1\n";
    return 0;
  }
  if (operation == "stmt-free") {
    scratchbird::engine::sblr::SblrStmtFreeResultV1 decoded;
    std::string detail;
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.stmt_free" &&
        scratchbird::engine::sblr::DecodeSblrStmtFreeResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded, &detail) &&
        decoded.terminal_state == 1 && decoded.publication_barrier == 1 &&
        decoded.terminal_prepared_generation != 0 &&
        decoded.executor_availability_generation != 0;
    if (!exact_result) {
      std::cerr << "CSC-TEST-003585 STMT_FREE exact_result_failed"
                << " detail=" << detail << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
      return 4;
    }
    std::cout << "CSC-TEST-003585 STMT_FREE accepted "
                 "canonical_sblr=true publication_barrier=passed "
                 "surface_id=SBSQL-FB03794952FB input=deallocate_stmt\n";
    return 0;
  }
  if (operation == "stmt-cancel") {
    scratchbird::engine::sblr::SblrStmtCancelResultV1 decoded;
    std::string detail;
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.stmt_cancel" &&
        scratchbird::engine::sblr::DecodeSblrStmtCancelResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded, &detail) &&
        decoded.state == 3 && decoded.finality <= 1 &&
        decoded.publication_barrier == 1 &&
        decoded.target_execution_generation != 0 &&
        decoded.executor_availability_generation != 0;
    if (!exact_result) {
      std::cerr << "CSC-TEST-003589 STMT_CANCEL exact_result_failed"
                << " detail=" << detail << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
      return 4;
    }
    std::cout << "CSC-TEST-003589 STMT_CANCEL accepted "
                 "canonical_sblr=true state=already_terminal "
                 "publication_barrier=passed\n";
    return 0;
  }
  if (operation == "parameter-bind") {
    scratchbird::engine::sblr::SblrStmtExecuteResultV1 decoded;
    std::string detail;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.stmt_execute" &&
        !result.server_cursor_uuid.empty() && result.server_row_count == 1 &&
        scratchbird::engine::sblr::DecodeSblrStmtExecuteResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded, &detail) &&
        decoded.status == 1 && decoded.publication_barrier == 1 &&
        decoded.executor_availability_generation != 0 &&
        nonzero(decoded.result_descriptor_uuid) &&
        nonzero(decoded.result_handle_uuid) &&
        nonzero(decoded.effect_evidence_sha256) &&
        nonzero(decoded.operation_evidence_uuid);
    if (!exact_result) {
      std::cerr << "CSC-TEST-003593 PARAMETER_BIND consume_failed"
                << " detail=" << detail
                << " payload_bytes=" << result.server_result_payload.size()
                << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
      return 4;
    }
    const auto fetched =
        session.FetchCursorOnRoute(result.server_cursor_uuid, 1);
    const bool exact_typed_result =
        fetched.accepted && fetched.row_count == 1 && fetched.end_of_cursor &&
        !fetched.row_packet.empty() &&
        fetched.row_packet.find("operation_id=engine.op.stmt_execute") !=
            std::string::npos &&
        fetched.row_packet.find("result_kind=stmt_execute_result") !=
            std::string::npos &&
        fetched.row_packet.find("row[0]=key_a=7") !=
            std::string::npos &&
        fetched.row_packet.find("row_meta[0]=key_a:int64:not_null") !=
            std::string::npos;
    if (!exact_typed_result) {
      std::cerr << "CSC-TEST-003593 PARAMETER_BIND typed_result_failed"
                << " accepted=" << fetched.accepted
                << " row_count=" << fetched.row_count
                << " end_of_cursor=" << fetched.end_of_cursor
                << " row_packet=" << fetched.row_packet << '\n';
      for (const auto& diagnostic : fetched.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    std::cout << "CSC-TEST-003593 PARAMETER_BIND accepted "
                 "canonical_sblr=true durable_bind_consumed=true "
                 "typed_value=7 publication_barrier=passed "
                 "public_name=prep_parameter declared_type=BIGINT "
                 "prepare_surface=SBSQL-5535E9A48BE4 "
                 "execute_surface=SBSQL-414E9A624B34\n";
    return 0;
  }
  if (operation == "parameter-bind-multi-nullable") {
    scratchbird::engine::sblr::SblrStmtExecuteResultV1 decoded;
    std::string detail;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.stmt_execute" &&
        !result.server_cursor_uuid.empty() && result.server_row_count == 1 &&
        scratchbird::engine::sblr::DecodeSblrStmtExecuteResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded, &detail) &&
        decoded.status == 1 && decoded.publication_barrier == 1 &&
        decoded.executor_availability_generation != 0 &&
        nonzero(decoded.result_descriptor_uuid) &&
        nonzero(decoded.result_handle_uuid) &&
        nonzero(decoded.effect_evidence_sha256) &&
        nonzero(decoded.operation_evidence_uuid);
    if (!exact_result) {
      std::cerr << "CSC-TEST-005775 PARAMETER_BIND_MULTI_NULLABLE "
                   "consume_failed detail="
                << detail
                << " payload_bytes=" << result.server_result_payload.size()
                << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
      return 4;
    }
    const auto fetched =
        session.FetchCursorOnRoute(result.server_cursor_uuid, 1);
    const bool exact_typed_result =
        fetched.accepted && fetched.row_count == 1 && fetched.end_of_cursor &&
        !fetched.row_packet.empty() &&
        fetched.row_packet.find("operation_id=engine.op.stmt_execute") !=
            std::string::npos &&
        fetched.row_packet.find("result_kind=stmt_execute_result") !=
            std::string::npos &&
        fetched.row_packet.find("row[0]=key_a=7;amount=") !=
            std::string::npos &&
        fetched.row_packet.find(
            "row_meta[0]=key_a:int64:not_null;amount:int64:null") !=
            std::string::npos &&
        fetched.row_packet.find("<NULL>") == std::string::npos;
    if (!exact_typed_result) {
      std::cerr << "CSC-TEST-005775 PARAMETER_BIND_MULTI_NULLABLE "
                   "typed_result_failed accepted="
                << fetched.accepted << " row_count=" << fetched.row_count
                << " end_of_cursor=" << fetched.end_of_cursor
                << " row_packet=" << fetched.row_packet << '\n';
      for (const auto& diagnostic : fetched.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    std::cout << "CSC-TEST-005775 PARAMETER_BIND_MULTI_NULLABLE accepted "
                 "canonical_sblr=true durable_bind_consumed=true "
                 "slot_count=2 first_value=7 second_value=null "
                 "publication_barrier=passed public_name=prep_parameter_multi "
                 "declared_types=BIGINT,BIGINT\n";
    return 0;
  }
  if (operation == "result-page") {
    scratchbird::engine::sblr::SblrResultPageResultV1 decoded;
    std::string detail;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.result_page" &&
        !result.server_cursor_uuid.empty() && result.server_row_count == 1 &&
        scratchbird::engine::sblr::DecodeSblrResultPageResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded, &detail) &&
        decoded.completion_state == 1 && decoded.terminal_state == 1 &&
        decoded.returned_row_count == 1 && decoded.next_row_offset == 1 &&
        decoded.executor_availability_generation != 0 &&
        nonzero(decoded.cursor_uuid) &&
        nonzero(decoded.result_set_handle_uuid) &&
        nonzero(decoded.row_descriptor_uuid) &&
        nonzero(decoded.redaction_profile_uuid) &&
        nonzero(decoded.result_material_sha256) &&
        nonzero(decoded.executor_evidence_sha256) &&
        nonzero(decoded.publication_barrier_uuid) &&
        nonzero(decoded.result_evidence_uuid) &&
        !nonzero(decoded.next_continuation_uuid) &&
        decoded.next_continuation_generation == 0;
    if (!exact_result) {
      std::cerr << "CSC-TEST-003597 RESULT_PAGE exact_result_failed"
                << " detail=" << detail
                << " payload_bytes=" << result.server_result_payload.size()
                << " row_count=" << result.server_row_count << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
      return 4;
    }
    std::cout << "CSC-TEST-003597 RESULT_PAGE accepted "
                 "canonical_sblr=true returned_rows=1 terminal=true "
                 "publication_barrier=passed\n";
    return 0;
  }
  if (operation == "show-object-detail") {
    scratchbird::engine::sblr::SblrCatalogIntrospectResultV1 decoded;
    std::string detail;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    const bool exact_terminal =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.catalog_introspect" &&
        !result.server_cursor_uuid.empty() && result.server_row_count > 1 &&
        result.server_result_payload.size() == 320 &&
        scratchbird::engine::sblr::DecodeSblrCatalogIntrospectResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded, &detail) &&
        decoded.row_count == result.server_row_count &&
        decoded.object_kind == scratchbird::engine::sblr::
                                   kSblrCatalogIntrospectObjectKindTableV1 &&
        decoded.profile == scratchbird::engine::sblr::
                               kSblrCatalogIntrospectProfileShowObjectDetailV1 &&
        decoded.flags == scratchbird::engine::sblr::
                             kSblrCatalogIntrospectDetailFlagV1 &&
        decoded.catalog_epoch != 0 && decoded.security_epoch != 0 &&
        decoded.availability != 0 && nonzero(decoded.request_uuid) &&
        nonzero(decoded.readable_projection_uuid) &&
        nonzero(decoded.row_descriptor_uuid) &&
        nonzero(decoded.result_set_uuid) && nonzero(decoded.object_uuid) &&
        nonzero(decoded.statement_snapshot_uuid) &&
        nonzero(decoded.row_material_sha256) &&
        nonzero(decoded.descriptor_evidence_sha256) &&
        nonzero(decoded.evidence) && nonzero(decoded.publication_barrier);
    scratchbird::parser::ipc::ServerFetchResult fetched;
    std::uint64_t fetched_rows = 0;
    std::string fetched_packets;
    bool fetched_to_end = false;
    if (exact_terminal) {
      while (fetched_rows < result.server_row_count && !fetched_to_end) {
        auto page = session.FetchCursorOnRoute(
            result.server_cursor_uuid,
            result.server_row_count - fetched_rows, 0, 0);
        if (!page.accepted || page.messages.has_errors() ||
            page.cursor_uuid != result.server_cursor_uuid ||
            page.row_count == 0) {
          fetched = std::move(page);
          break;
        }
        fetched_rows += page.row_count;
        fetched_packets.append(page.row_packet);
        fetched_to_end = page.end_of_cursor;
        fetched = std::move(page);
      }
    }
    const bool exact_rows =
        fetched.accepted && !fetched.messages.has_errors() &&
        fetched.cursor_uuid == result.server_cursor_uuid &&
        fetched_rows == result.server_row_count && fetched_to_end &&
        fetched_packets.find(
            "operation_id=engine.op.catalog_introspect\n") !=
            std::string::npos &&
        fetched_packets.find(
            "result_kind=rs.sbsql.show_object_detail.v1\n") !=
            std::string::npos &&
        fetched_packets.find(
            "row[0]=section=identity;section_ordinal=1;"
            "element_ordinal=1;element_kind=table;"
            "element_name=APP.CUSTOMERS;") != std::string::npos &&
        fetched_packets.find("section=columns;") != std::string::npos &&
        fetched_packets.find("element_kind=column;") !=
            std::string::npos &&
        fetched_packets.find("related_object_path=APP.CUSTOMERS;") !=
            std::string::npos &&
        fetched_packets.find("section=properties;") !=
            std::string::npos &&
        fetched_packets.find("row_meta[0]=section:text:not_null;") !=
            std::string::npos;
    auto closed = exact_rows
                      ? session.CloseCursorOnRoute(result.server_cursor_uuid)
                      : scratchbird::parser::ipc::ServerCloseCursorResult{};
    const bool exact_eos_cleanup =
        exact_rows && !closed.accepted && !closed.outcome_unknown &&
        !closed.caller_cleanup_required && !closed.route_fatal &&
        closed.messages.has_errors();
    auto rolled_back =
        exact_eos_cleanup
            ? session.RunPipeline("ROLLBACK TRANSACTION", true)
            : scratchbird::parser::sbsql::PipelineResult{};
    if (!exact_terminal || !exact_rows || !exact_eos_cleanup ||
        !rolled_back.accepted ||
        rolled_back.messages.has_errors()) {
      std::cerr << "CSC-TEST-003609 CATALOG_INTROSPECT_SHOW_TABLE "
                   "exact_public_route_failed detail="
                << detail << " terminal=" << exact_terminal
                << " rows=" << exact_rows
                << " eos_cleanup=" << exact_eos_cleanup
                << " rollback=" << rolled_back.accepted
                << " fetched_rows=" << fetched_rows
                << " row_packet=" << fetched_packets << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      for (const auto& diagnostic : fetched.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    std::cout << "CSC-TEST-003609 CATALOG_INTROSPECT_SHOW_TABLE accepted "
                 "canonical_sblr=true receipt_name_bound=true "
                 "typed_cirs=true cursor_rows=true identity=true "
                 "columns=true properties=true cursor_closed=true "
                 "transaction_rolled_back=true publication_barrier=passed\n";
    return 0;
  }
  if (operation == "name-resolve") {
    scratchbird::engine::sblr::SblrNameResolveResultV1 decoded;
    std::string detail;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.name_resolve" &&
        result.server_row_count == 0 && !result.server_result_payload.empty() &&
        result.sblr_payload.find("app.customers") == std::string::npos &&
        scratchbird::engine::sblr::DecodeSblrNameResolveResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded, &detail) &&
        decoded.status == 1 && decoded.visibility == 1 &&
        decoded.object_class == 2 &&
        decoded.object_descriptor_generation != 0 &&
        decoded.catalog_generation != 0 && decoded.security_epoch != 0 &&
        nonzero(decoded.resolution_uuid) &&
        nonzero(decoded.resolved_object_uuid) &&
        nonzero(decoded.resolved_namespace_uuid) &&
        nonzero(decoded.redaction_profile_uuid) &&
        nonzero(decoded.publication_evidence_uuid) &&
        nonzero(decoded.resolution_material_sha256) &&
        nonzero(decoded.executor_evidence_sha256);
    if (!exact_result) {
      std::cerr << "CSC-TEST-003613 NAME_RESOLVE exact_result_failed"
                << " detail=" << detail
                << " payload_bytes=" << result.server_result_payload.size()
                << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    std::cout << "CSC-TEST-003613 NAME_RESOLVE accepted "
                 "canonical_sblr=true visible_table=true "
                 "publication_barrier=passed "
                 "surface_id=SBSQL-5E6DC360F377 "
                 "input=resolve_name_public\n";
    return 0;
  }
  if (operation == "optimizer-stats-read") {
    scratchbird::engine::sblr::SblrOptimizerStatsReadResultV1 decoded;
    std::string detail;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.optimizer_stats_read" &&
        result.server_row_count == 0 && !result.server_result_payload.empty() &&
        result.sblr_payload.find("OPTIMIZER STATS READ") == std::string::npos &&
        scratchbird::engine::sblr::DecodeSblrOptimizerStatsReadResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded, &detail) &&
        decoded.flags == scratchbird::engine::sblr::
                             kSblrOptimizerStatsReadCatalogFlags &&
        decoded.catalog_generation != 0 && decoded.security_epoch != 0 &&
        decoded.resource_epoch != 0 && decoded.inventory_generation != 0 &&
        decoded.executor_availability_generation != 0 &&
        nonzero(decoded.statistics_snapshot_uuid) &&
        nonzero(decoded.statement_receipt_uuid) &&
        nonzero(decoded.statement_snapshot_uuid) &&
        nonzero(decoded.result_material_sha256) &&
        nonzero(decoded.executor_evidence_sha256);
    if (!exact_result) {
      std::cerr << "CSC-TEST-003617 OPTIMIZER_STATS_READ exact_result_failed"
                << " detail=" << detail
                << " payload_bytes=" << result.server_result_payload.size()
                << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    std::cout << "CSC-TEST-003617 OPTIMIZER_STATS_READ accepted "
                 "canonical_sblr=true immutable_catalog_snapshot=true "
                 "publication_barrier=passed\n";
    return 0;
  }
  if (operation == "optimizer-stats-drop") {
    scratchbird::engine::sblr::SblrOptimizerStatsDropResultV1 dropped;
    std::string detail;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    const bool exact_drop =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.optimizer_stats_drop" &&
        result.server_row_count == 0 && !result.server_result_payload.empty() &&
        result.sblr_payload.find("OPTIMIZER STATS DROP") == std::string::npos &&
        scratchbird::engine::sblr::DecodeSblrOptimizerStatsDropResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &dropped, &detail) &&
        dropped.flags == scratchbird::engine::sblr::
                             kSblrOptimizerStatsDropAllScopesFlag &&
        dropped.status == scratchbird::engine::sblr::
                              kSblrOptimizerStatsDropPublishedStatus &&
        dropped.statistics_epoch == dropped.prior_statistics_epoch + 1 &&
        dropped.cache_invalidation_generation == dropped.statistics_epoch &&
        dropped.publication_barrier_generation == dropped.effect_generation &&
        dropped.catalog_generation != 0 && dropped.security_epoch != 0 &&
        dropped.resource_epoch != 0 && dropped.inventory_generation != 0 &&
        dropped.executor_availability_generation != 0 &&
        nonzero(dropped.effect_uuid) && nonzero(dropped.statement_receipt_uuid) &&
        nonzero(dropped.durable_publication_uuid) &&
        nonzero(dropped.result_material_sha256) &&
        nonzero(dropped.executor_evidence_sha256);
    if (!exact_drop) {
      std::cerr << "CSC-TEST-003621 OPTIMIZER_STATS_DROP exact_result_failed"
                << " detail=" << detail
                << " payload_bytes=" << result.server_result_payload.size()
                << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }

    // Prove the committed epoch through a fresh authenticated parser/server
    // session. The observer receives only OSRR; it does not reuse the DROP
    // receipt, transaction, descriptor, or result bytes.
    scratchbird::parser::sbsql::SbsqlTestWireSession observer(
        config, nullptr, nullptr);
    scratchbird::parser::sbsql::MessageVectorSet observer_messages;
    if (!observer.AuthenticateCredentials(credentials, &observer_messages)) {
      for (const auto& diagnostic : observer_messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    auto observer_begin = observer.RunPipeline("BEGIN TRANSACTION", true);
    auto observed = observer_begin.accepted
                        ? observer.RunPipeline("OPTIMIZER STATS READ;", true)
                        : observer_begin;
    scratchbird::engine::sblr::SblrOptimizerStatsReadResultV1 read;
    const bool independent_visibility =
        observed.accepted && !observed.messages.has_errors() &&
        observed.server_operation_id == "engine.op.optimizer_stats_read" &&
        scratchbird::engine::sblr::DecodeSblrOptimizerStatsReadResultV1(
            reinterpret_cast<const std::uint8_t*>(
                observed.server_result_payload.data()),
            observed.server_result_payload.size(), &read, &detail) &&
        read.optimizer_statistics_epoch == dropped.statistics_epoch &&
        read.optimizer_statistics_epoch > 1 &&
        read.executor_availability_generation != 0;
    if (!independent_visibility) {
      std::cerr << "CSC-TEST-003621 OPTIMIZER_STATS_DROP "
                   "independent_epoch_observation_failed"
                << " detail=" << detail
                << " observed_payload_bytes="
                << observed.server_result_payload.size() << '\n';
      for (const auto& diagnostic : observed.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    std::cout << "CSC-TEST-003621 OPTIMIZER_STATS_DROP accepted "
                 "canonical_sblr=true durable_epoch_advanced=true "
                 "cache_invalidation=passed independent_session_read=passed "
                 "publication_barrier=passed\n";
    return 0;
  }
  if (operation == "parse-text") {
    namespace sblr = scratchbird::engine::sblr;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    const auto contains_text = [](const std::vector<std::uint8_t>& bytes,
                                  std::string_view text) {
      return !text.empty() &&
             std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) !=
                 bytes.end();
    };

    std::string detail;
    sblr::SblrParseTextResultV1 decoded_result;
    sblr::SblrParseTextDescriptorV1 decoded_descriptor;
    const auto outer_container = scratchbird::engine::DecodeSblrContainerBytes(
        reinterpret_cast<const std::uint8_t*>(result.sblr_payload.data()),
        result.sblr_payload.size());
    const auto outer_stream = outer_container.status ==
                                      scratchbird::engine::SblrCodecStatus::ok
                                  ? sblr::DecodeSblrOpcodeStream(std::string_view(
                                        reinterpret_cast<const char*>(
                                            outer_container.container
                                                .operation_payload.data()),
                                        outer_container.container
                                            .operation_payload.size()))
                                  : sblr::SblrOpcodeStreamResult{};
    const bool outer_root_shape =
        outer_stream.ok && outer_stream.stream.operations.size() == 3 &&
        outer_stream.stream.operations[1].operation_id ==
            "engine.op.parse_text" &&
        outer_stream.stream.operations[1].opcode == "SBLR_PARSE_TEXT" &&
        outer_stream.stream.operations[1].opcode_code ==
            sblr::kSblrParseTextOpcodeCode &&
        outer_stream.stream.operations[1].operands.size() == 1 &&
        outer_stream.stream.operations[1].operands[0].type ==
            "parse_text_descriptor" &&
        outer_stream.stream.operations[1].operands[0].name == "text" &&
        outer_stream.stream.operations[1].operands[0].value_kind ==
            sblr::SblrValueKind::parse_text_descriptor &&
        sblr::DecodeSblrParseTextDescriptorV1(
            outer_stream.stream.operations[1].operands[0].value_body.data(),
            outer_stream.stream.operations[1].operands[0].value_body.size(),
            &decoded_descriptor, &detail);
    const bool result_shape =
        sblr::DecodeSblrParseTextResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded_result, &detail);
    const auto nested_container =
        result_shape
            ? scratchbird::engine::DecodeSblrContainerBytes(
                  decoded_result.canonical_sblr_bytes.data(),
                  decoded_result.canonical_sblr_bytes.size())
            : scratchbird::engine::SblrDecodedContainer{};
    const auto nested_stream =
        nested_container.status == scratchbird::engine::SblrCodecStatus::ok
            ? sblr::DecodeSblrOpcodeStream(std::string_view(
                  reinterpret_cast<const char*>(
                      nested_container.container.operation_payload.data()),
                  nested_container.container.operation_payload.size()))
            : sblr::SblrOpcodeStreamResult{};
    const bool nested_root_shape =
        nested_stream.ok && nested_stream.stream.operations.size() == 3 &&
        nested_stream.stream.operations[1].operation_id == "query.execute" &&
        nested_stream.stream.operations[1].opcode == "SBLR_QUERY_EXECUTE" &&
        nested_stream.stream.operations[1].opcode_code == 4615;
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.parse_text" &&
        result.server_cursor_uuid.empty() && result.server_row_count == 0 &&
        result.server_affected_rows == 0 &&
        !result.server_affected_rows_present &&
        !result.server_result_payload.empty() && outer_root_shape &&
        result_shape && nested_root_shape && decoded_result.status == 1 &&
        decoded_result.publication_barrier == 1 &&
        decoded_result.parse_uuid == decoded_descriptor.parse_uuid &&
        decoded_result.statement_receipt_uuid ==
            decoded_descriptor.statement_receipt_uuid &&
        decoded_result.language_profile_uuid ==
            decoded_descriptor.language_profile_uuid &&
        decoded_result.language_profile_generation ==
            decoded_descriptor.language_profile_generation &&
        decoded_result.parser_package_uuid ==
            decoded_descriptor.parser_package_uuid &&
        decoded_result.catalog_generation ==
            decoded_descriptor.catalog_generation &&
        decoded_result.security_epoch == decoded_descriptor.security_epoch &&
        decoded_result.resource_epoch == decoded_descriptor.resource_epoch &&
        decoded_result.canonical_sblr_bytes ==
            decoded_descriptor.canonical_sblr_bytes &&
        decoded_result.executor_availability_generation ==
            decoded_descriptor.executor_availability_generation &&
        nonzero(decoded_result.parse_uuid) &&
        nonzero(decoded_result.statement_receipt_uuid) &&
        nonzero(decoded_result.language_profile_uuid) &&
        nonzero(decoded_result.parser_package_uuid) &&
        nonzero(decoded_result.parse_evidence_uuid) &&
        nonzero(decoded_result.result_evidence_sha256) &&
        nonzero(decoded_result.executor_evidence_sha256) &&
        !contains_text(decoded_result.canonical_sblr_bytes, "SELECT 1") &&
        result.sblr_payload.find("SELECT 1") == std::string::npos &&
        result.server_result_payload.find("SELECT 1") == std::string::npos;
    if (!exact_result) {
      std::cerr << "CSC-TEST-003625 PARSE_TEXT exact_result_failed"
                << " detail=" << detail
                << " outer_ok=" << outer_root_shape
                << " result_ok=" << result_shape
                << " nested_ok=" << nested_root_shape
                << " result_bytes=" << result.server_result_payload.size()
                << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    std::cout << "CSC-TEST-003625 PARSE_TEXT accepted "
                 "canonical_sblr=true nested_canonical_sblr=true "
                 "publication_barrier=passed\n";
    return 0;
  }
  if (operation == "catalog-epoch-check") {
    namespace sblr = scratchbird::engine::sblr;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    std::string detail;
    sblr::SblrCatalogEpochCheckResultV1 decoded_result;
    sblr::SblrCatalogEpochCheckDescriptorV1 decoded_descriptor;
    const auto outer_container = scratchbird::engine::DecodeSblrContainerBytes(
        reinterpret_cast<const std::uint8_t*>(result.sblr_payload.data()),
        result.sblr_payload.size());
    const auto outer_stream =
        outer_container.status == scratchbird::engine::SblrCodecStatus::ok
            ? sblr::DecodeSblrOpcodeStream(std::string_view(
                  reinterpret_cast<const char*>(
                      outer_container.container.operation_payload.data()),
                  outer_container.container.operation_payload.size()))
            : sblr::SblrOpcodeStreamResult{};
    const bool outer_root_shape =
        outer_stream.ok && outer_stream.stream.operations.size() == 3 &&
        outer_stream.stream.operations[1].operation_id ==
            "engine.op.catalog_epoch_check" &&
        outer_stream.stream.operations[1].opcode ==
            "SBLR_CATALOG_EPOCH_CHECK" &&
        outer_stream.stream.operations[1].opcode_code ==
            sblr::kSblrCatalogEpochCheckOpcodeCode &&
        outer_stream.stream.operations[1].operands.size() == 1 &&
        outer_stream.stream.operations[1].operands[0].type ==
            "catalog_epoch_check_descriptor" &&
        outer_stream.stream.operations[1].operands[0].name ==
            "catalog_epoch" &&
        outer_stream.stream.operations[1].operands[0].value_kind ==
            sblr::SblrValueKind::catalog_epoch_check_descriptor &&
        sblr::DecodeSblrCatalogEpochCheckDescriptorV1(
            outer_stream.stream.operations[1].operands[0].value_body.data(),
            outer_stream.stream.operations[1].operands[0].value_body.size(),
            &decoded_descriptor, &detail);
    const bool result_shape =
        sblr::DecodeSblrCatalogEpochCheckResultV1(
            reinterpret_cast<const std::uint8_t*>(
                result.server_result_payload.data()),
            result.server_result_payload.size(), &decoded_result, &detail);
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.catalog_epoch_check" &&
        result.server_cursor_uuid.empty() && result.server_row_count == 0 &&
        result.server_affected_rows == 0 &&
        !result.server_affected_rows_present && outer_root_shape &&
        result_shape && !decoded_descriptor.object_scoped &&
        !decoded_result.object_scoped && decoded_result.status == 1 &&
        decoded_result.visibility == 1 &&
        decoded_result.check_uuid == decoded_descriptor.check_uuid &&
        decoded_result.observed_catalog_epoch_uuid ==
            decoded_descriptor.requested_catalog_epoch_uuid &&
        decoded_result.observed_catalog_generation ==
            decoded_descriptor.requested_catalog_generation &&
        decoded_result.database_uuid == decoded_descriptor.database_uuid &&
        decoded_result.schema_tree_uuid ==
            decoded_descriptor.schema_tree_uuid &&
        decoded_result.schema_tree_generation ==
            decoded_descriptor.schema_tree_generation &&
        decoded_result.observed_security_epoch ==
            decoded_descriptor.security_epoch &&
        decoded_result.observed_resource_epoch ==
            decoded_descriptor.resource_epoch &&
        decoded_result.executor_availability_generation ==
            decoded_descriptor.executor_availability_generation &&
        nonzero(decoded_descriptor.statement_receipt_uuid) &&
        nonzero(decoded_descriptor.policy_snapshot_uuid) &&
        nonzero(decoded_descriptor.catalog_snapshot_uuid) &&
        nonzero(decoded_descriptor.descriptor_sha256) &&
        nonzero(decoded_descriptor.visibility_scope_sha256) &&
        nonzero(decoded_result.redaction_profile_uuid) &&
        nonzero(decoded_result.publication_evidence_uuid) &&
        nonzero(decoded_result.result_material_sha256) &&
        result.sblr_payload.find("CATALOG EPOCH CHECK") ==
            std::string::npos;
    if (!exact_result) {
      std::cerr << "CSC-TEST-003629 CATALOG_EPOCH_CHECK exact_result_failed"
                << " detail=" << detail
                << " outer_ok=" << outer_root_shape
                << " result_ok=" << result_shape
                << " result_bytes=" << result.server_result_payload.size()
                << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
      return 4;
    }
    std::cout << "CSC-TEST-003629 CATALOG_EPOCH_CHECK accepted "
                 "canonical_sblr=true current_epoch=true "
                 "redaction_bound=true publication_barrier=passed\n";
    return 0;
  }
  if (operation == "database-attach") {
    namespace sblr = scratchbird::engine::sblr;
    const auto nonzero = [](const auto& value) {
      return std::any_of(value.begin(), value.end(),
                         [](std::uint8_t byte) { return byte != 0; });
    };
    std::string detail;
    sblr::SblrDatabaseAttachResultV1 decoded_result;
    sblr::SblrDatabaseAttachDescriptorV1 decoded_descriptor;
    const auto outer_container = scratchbird::engine::DecodeSblrContainerBytes(
        reinterpret_cast<const std::uint8_t*>(result.sblr_payload.data()),
        result.sblr_payload.size());
    const auto outer_stream =
        outer_container.status == scratchbird::engine::SblrCodecStatus::ok
            ? sblr::DecodeSblrOpcodeStream(std::string_view(
                  reinterpret_cast<const char*>(
                      outer_container.container.operation_payload.data()),
                  outer_container.container.operation_payload.size()))
            : sblr::SblrOpcodeStreamResult{};
    const bool outer_root_shape =
        outer_stream.ok && outer_stream.stream.operations.size() == 3 &&
        outer_stream.stream.operations[1].operation_id ==
            "engine.op.database_attach" &&
        outer_stream.stream.operations[1].opcode ==
            "SBLR_DATABASE_ATTACH" &&
        outer_stream.stream.operations[1].opcode_code ==
            sblr::kSblrDatabaseAttachOpcodeCode &&
        outer_stream.stream.operations[1].operands.size() == 1 &&
        outer_stream.stream.operations[1].operands[0].ordinal == 1 &&
        outer_stream.stream.operations[1].operands[0].type ==
            "database_attach_descriptor" &&
        outer_stream.stream.operations[1].operands[0].name ==
            "attachment" &&
        outer_stream.stream.operations[1].operands[0].value_kind ==
            sblr::SblrValueKind::database_attach_descriptor &&
        sblr::DecodeSblrDatabaseAttachDescriptorV1(
            outer_stream.stream.operations[1].operands[0].value_body.data(),
            outer_stream.stream.operations[1].operands[0].value_body.size(),
            &decoded_descriptor, &detail);
    const bool result_shape = sblr::DecodeSblrDatabaseAttachResultV1(
        reinterpret_cast<const std::uint8_t*>(
            result.server_result_payload.data()),
        result.server_result_payload.size(), &decoded_result, &detail);
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "engine.op.database_attach" &&
        result.server_cursor_uuid.empty() && result.server_row_count == 0 &&
        result.server_affected_rows == 0 &&
        !result.server_affected_rows_present && outer_root_shape &&
        result_shape && decoded_descriptor.mode == 1 &&
        decoded_descriptor.alias_scope == 1 && decoded_result.status == 1 &&
        decoded_result.lifecycle_state == 1 &&
        decoded_result.publication_barrier == 1 &&
        decoded_result.attach_uuid == decoded_descriptor.attach_uuid &&
        decoded_result.database_uuid == decoded_descriptor.database_uuid &&
        decoded_result.alias_uuid == decoded_descriptor.alias_uuid &&
        decoded_result.database_generation != 0 &&
        decoded_result.catalog_generation ==
            decoded_descriptor.catalog_generation &&
        nonzero(decoded_descriptor.statement_receipt_uuid) &&
        nonzero(decoded_descriptor.storage_uuid) &&
        nonzero(decoded_descriptor.catalog_snapshot_uuid) &&
        nonzero(decoded_descriptor.security_context_uuid) &&
        nonzero(decoded_descriptor.policy_snapshot_uuid) &&
        nonzero(decoded_descriptor.transaction_uuid) &&
        nonzero(decoded_descriptor.descriptor_sha256) &&
        nonzero(decoded_descriptor.storage_alias_binding_sha256) &&
        nonzero(decoded_result.catalog_epoch_uuid) &&
        nonzero(decoded_result.attachment_evidence_uuid) &&
        nonzero(decoded_result.result_material_sha256) &&
        nonzero(decoded_result.executor_evidence_sha256) &&
        decoded_descriptor.executor_availability_generation != 0 &&
        result.sblr_payload.find("DATABASE ATTACH REGISTERED") ==
            std::string::npos &&
        result.sblr_payload.find("workplan_attachment") ==
            std::string::npos;
    if (!exact_result) {
      std::cerr << "CSC-TEST-003633 DATABASE_ATTACH exact_result_failed"
                << " detail=" << detail
                << " outer_ok=" << outer_root_shape
                << " result_ok=" << result_shape
                << " result_bytes=" << result.server_result_payload.size()
                << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        for (const auto& field : diagnostic.fields) {
          std::cerr << diagnostic.code << ':' << field.name << '='
                    << field.value << '\n';
        }
      }
      return 4;
    }
    std::cout << "CSC-TEST-003633 DATABASE_ATTACH accepted "
                 "canonical_sblr=true registered_storage=true "
                 "session_alias=true publication_barrier=passed\n";
    return 0;
  }
  if (operation == "query-execute") {
    const bool exact_result =
        result.accepted && !result.messages.has_errors() &&
        result.server_operation_id == "query.execute" &&
        !result.server_cursor_uuid.empty() && result.server_row_count == 1 &&
        result.server_result_payload.empty() &&
        result.sblr_payload.find("SELECT key_a") == std::string::npos;
    if (!exact_result) {
      std::cerr << "CSC-TEST-003601 QUERY_EXECUTE exact_result_failed"
                << " accepted=" << result.accepted
                << " operation_id=" << result.server_operation_id
                << " cursor_uuid=" << result.server_cursor_uuid
                << " row_count=" << result.server_row_count
                << " terminal_payload_bytes="
                << result.server_result_payload.size() << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    const auto fetched =
        session.FetchCursorOnRoute(result.server_cursor_uuid, 1);
    const bool exact_row =
        fetched.accepted && fetched.row_count == 1 && fetched.end_of_cursor &&
        fetched.row_packet.find("operation_id=query.execute") !=
            std::string::npos &&
        fetched.row_packet.find("row[0]=key_a=1") != std::string::npos &&
        fetched.row_packet.find("row_meta[0]=key_a:int64:not_null") !=
            std::string::npos;
    if (!exact_row) {
      std::cerr << "CSC-TEST-003601 QUERY_EXECUTE row_result_failed"
                << " accepted=" << fetched.accepted
                << " row_count=" << fetched.row_count
                << " end_of_cursor=" << fetched.end_of_cursor
                << " row_packet=" << fetched.row_packet << '\n';
      for (const auto& diagnostic : fetched.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      return 4;
    }
    std::cout << "CSC-TEST-003601 QUERY_EXECUTE accepted "
                 "canonical_sblr=true result_handle=validated "
                 "typed_row=key_a:1\n";
    return 0;
  }
  const char* static_refusal_test_id =
      operation == "diagnostic-refusal" ? "CSC-TEST-003849" :
      operation == "diagnostic-reset" ? "CSC-TEST-003853" :
      operation == "descriptor-transform" ? "CSC-TEST-003857" :
      operation == "show-wait-events" ? "CSC-TEST-002702" :
      operation == "ddl-create-temporary-table" ? "CSC-TEST-002661" :
      operation == "ddl-drop-temporary-table" ? "CSC-TEST-002665" :
      operation == "ddl-create-or-replace-srs" ? "CSC-TEST-002673" :
      operation == "ddl-drop-srs" ? "CSC-TEST-002677" :
      operation == "ddl-create-rewrite-rule" ? "CSC-TEST-002681" :
      operation == "ddl-alter-rewrite-rule" ? "CSC-TEST-002685" :
      operation == "ddl-drop-rewrite-rule" ? "CSC-TEST-002689" :
      operation == "ddl-validate-constraint" ? "CSC-TEST-002693" :
      operation == "security-alter-privilege-template" ? "CSC-TEST-002701" :
      operation == "security-drop-privilege-template" ? "CSC-TEST-002705" :
      operation == "database-create-template-clone" ? "CSC-TEST-002709" :
      operation == "ddl-create-aggregate" ? "CSC-TEST-002713" :
      operation == "ddl-alter-aggregate" ? "CSC-TEST-002717" :
      operation == "ddl-drop-aggregate" ? "CSC-TEST-002721" :
      operation == "ddl-purge-system-history" ? "CSC-TEST-002725" :
      operation == "ddl-set-index-optimizer-eligibility" ?
          "CSC-TEST-002729" :
      operation == "ddl-set-table-type-enforcement" ? "CSC-TEST-002733" :
      operation == "database-serialize-logical-snapshot" ?
          "CSC-TEST-002737" :
      operation == "database-deserialize-logical-snapshot" ?
          "CSC-TEST-002741" :
      operation == "ddl-create-macro" ? "CSC-TEST-002745" :
      operation == "security-create-user" ? "CSC-TEST-002965" :
      operation == "ddl-drop-macro" ? "CSC-TEST-002749" :
      operation == "ddl-drop-package" ? "CSC-TEST-002893" :
      operation == "admin-register-external-relation-resolver" ?
          "CSC-TEST-002753" :
      operation == "admin-unregister-external-relation-resolver" ?
          "CSC-TEST-002757" :
      operation == "ddl-create-dictionary" ? "CSC-TEST-002761" :
      operation == "ddl-drop-dictionary" ? "CSC-TEST-002769" :
      operation == "ddl-alter-dictionary" ? "CSC-TEST-002765" :
      operation == "ddl-create-continuous-view" ? "CSC-TEST-002773" :
      operation == "ddl-alter-continuous-view" ? "CSC-TEST-002777" :
      operation == "ddl-drop-continuous-view" ? "CSC-TEST-002781" :
      operation == "dml-async-insert-submit" ? "CSC-TEST-002785" :
      operation == "dml-async-insert-status" ? "CSC-TEST-002789" :
      operation == "dml-async-insert-cancel" ? "CSC-TEST-002793" :
      operation == "dml-conditional-mutate" ? "CSC-TEST-002797" :
      operation == "dml-timeseries-schema-write" ? "CSC-TEST-002805" :
      operation == "ddl-timeseries-series-cardinality-policy" ?
          "CSC-TEST-002809" :
      operation == "ddl-create-timeseries-value-cache" ?
          "CSC-TEST-002813" :
      operation == "ddl-alter-timeseries-value-cache" ?
          "CSC-TEST-002817" :
      operation == "ddl-create-synonym" ? "CSC-TEST-002941" :
      operation == "ddl-create-foreign-table" ? "CSC-TEST-002949" :
      operation == "ddl-create-fdw" ? "CSC-TEST-002957" :
      operation == "ddl-drop-fdw" ? "CSC-TEST-002961" :
      operation == "ddl-drop-foreign-table" ? "CSC-TEST-002953" : nullptr;
  const char* static_refusal_operation_label =
      operation == "diagnostic-refusal" ? "DIAGNOSTIC_REFUSAL" :
      operation == "diagnostic-reset" ? "DIAGNOSTIC_RESET" :
      operation == "descriptor-transform" ? "DESCRIPTOR_TRANSFORM" :
      operation == "show-wait-events" ? "READ_METRICS" :
      operation == "ddl-create-temporary-table" ? "DDL_CREATE_TEMPORARY_TABLE" :
      operation == "ddl-drop-temporary-table" ? "DDL_DROP_TEMPORARY_TABLE" :
      operation == "ddl-create-or-replace-srs" ? "DDL_CREATE_OR_REPLACE_SRS" :
      operation == "ddl-drop-srs" ? "DDL_DROP_SRS" :
      operation == "ddl-create-rewrite-rule" ? "DDL_CREATE_REWRITE_RULE" :
      operation == "ddl-alter-rewrite-rule" ? "DDL_ALTER_REWRITE_RULE" :
      operation == "ddl-drop-rewrite-rule" ? "DDL_DROP_REWRITE_RULE" :
      operation == "ddl-validate-constraint" ? "DDL_VALIDATE_CONSTRAINT" :
      operation == "security-alter-privilege-template" ?
          "SECURITY_ALTER_PRIVILEGE_TEMPLATE" :
      operation == "security-drop-privilege-template" ?
          "SECURITY_DROP_PRIVILEGE_TEMPLATE" :
      operation == "database-create-template-clone" ?
          "DATABASE_CREATE_TEMPLATE_CLONE" :
      operation == "ddl-create-aggregate" ? "DDL_CREATE_AGGREGATE" :
      operation == "ddl-alter-aggregate" ? "DDL_ALTER_AGGREGATE" :
      operation == "ddl-drop-aggregate" ? "DDL_DROP_AGGREGATE" :
      operation == "ddl-purge-system-history" ?
          "DDL_PURGE_SYSTEM_HISTORY" :
      operation == "ddl-set-index-optimizer-eligibility" ?
          "DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY" :
      operation == "ddl-set-table-type-enforcement" ?
          "DDL_SET_TABLE_TYPE_ENFORCEMENT" :
      operation == "database-serialize-logical-snapshot" ?
          "DATABASE_SERIALIZE_LOGICAL_SNAPSHOT" :
      operation == "database-deserialize-logical-snapshot" ?
          "DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT" :
      operation == "ddl-create-macro" ? "DDL_CREATE_MACRO" :
      operation == "security-create-user" ? "SECURITY_CREATE_USER" :
      operation == "ddl-drop-macro" ? "DDL_DROP_MACRO" :
      operation == "ddl-drop-package" ? "DDL_DROP_PACKAGE" :
      operation == "admin-register-external-relation-resolver" ?
          "ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER" :
      operation == "admin-unregister-external-relation-resolver" ?
          "ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER" :
      operation == "ddl-create-dictionary" ? "DDL_CREATE_DICTIONARY" :
      operation == "ddl-drop-dictionary" ? "DDL_DROP_DICTIONARY" :
      operation == "ddl-alter-dictionary" ? "DDL_ALTER_DICTIONARY" :
      operation == "ddl-create-continuous-view" ?
          "DDL_CREATE_CONTINUOUS_VIEW" :
      operation == "ddl-alter-continuous-view" ?
          "DDL_ALTER_CONTINUOUS_VIEW" :
      operation == "ddl-drop-continuous-view" ?
          "DDL_DROP_CONTINUOUS_VIEW" :
      operation == "dml-async-insert-submit" ?
          "DML_ASYNC_INSERT_SUBMIT" :
      operation == "dml-async-insert-status" ?
          "DML_ASYNC_INSERT_STATUS" :
      operation == "dml-async-insert-cancel" ?
          "DML_ASYNC_INSERT_CANCEL" :
      operation == "dml-conditional-mutate" ?
          "DML_CONDITIONAL_MUTATE" :
      operation == "dml-timeseries-schema-write" ?
          "DML_TIMESERIES_SCHEMA_WRITE" :
      operation == "ddl-timeseries-series-cardinality-policy" ?
          "DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY" :
      operation == "ddl-create-timeseries-value-cache" ?
          "DDL_CREATE_TIMESERIES_VALUE_CACHE" :
      operation == "ddl-alter-timeseries-value-cache" ?
          "DDL_ALTER_TIMESERIES_VALUE_CACHE" :
      operation == "ddl-create-synonym" ? "DDL_CREATE_SYNONYM" :
      operation == "ddl-create-foreign-table" ?
          "DDL_CREATE_FOREIGN_TABLE" :
      operation == "ddl-create-fdw" ? "DDL_CREATE_FDW" :
      operation == "ddl-drop-fdw" ? "DDL_DROP_FDW" :
      operation == "ddl-drop-foreign-table" ?
          "DDL_DROP_FOREIGN_TABLE" : nullptr;
  if (static_refusal_test_id != nullptr &&
      static_refusal_operation_label != nullptr) {
    const bool exact_refusal =
        !result.accepted && result.messages.diagnostics.size() == 1 &&
        result.messages.diagnostics.front().code ==
            "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING";
    const bool no_result_publication =
        result.server_operation_id.empty() && result.server_cursor_uuid.empty() &&
        result.server_row_count == 0 && result.server_affected_rows == 0 &&
        !result.server_affected_rows_present &&
        result.server_result_payload.empty();
    if (!exact_refusal || !no_result_publication) {
      std::cerr << static_refusal_test_id << ' '
                << static_refusal_operation_label
                << " fail_closed_contract_failed"
                << " accepted=" << result.accepted
                << " diagnostic_count="
                << result.messages.diagnostics.size()
                << " server_operation_id=" << result.server_operation_id
                << " server_cursor_uuid=" << result.server_cursor_uuid
                << " server_row_count=" << result.server_row_count
                << " server_affected_rows=" << result.server_affected_rows
                << " server_affected_rows_present="
                << result.server_affected_rows_present
                << " server_result_payload_bytes="
                << result.server_result_payload.size() << '\n';
      for (const auto& diagnostic : result.messages.diagnostics) {
        std::cerr << "diagnostic=" << diagnostic.code << ':'
                  << diagnostic.message << '\n';
      }
      return 4;
    }
    std::cout << static_refusal_test_id << ' '
              << static_refusal_operation_label
              << " deterministic_refusal="
                 "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"
                 " no_engine_dispatch=true no_result_publication=true\n";
    return 0;
  }
  if (operation == "ddl-create-operator-class" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") {
    std::cout << "CSC-TEST-004009 DDL_CREATE_OPERATOR_CLASS deterministic_cluster_refusal\n";
    return 0;
  }
  if (operation == "ddl-drop-operator-class" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") {
    std::cout << "CSC-TEST-004013 DDL_DROP_OPERATOR_CLASS deterministic_cluster_refusal\n";
    return 0;
  }
  if (operation == "ddl-create-operator-family" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") {
    std::cout << "CSC-TEST-004017 DDL_CREATE_OPERATOR_FAMILY deterministic_cluster_refusal\n";
    return 0;
  }
  if (operation == "ddl-alter-operator-family" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") {
    std::cout << "CSC-TEST-004021 DDL_ALTER_OPERATOR_FAMILY deterministic_cluster_refusal\n";
    return 0;
  }
  if (operation == "ddl-drop-operator-family" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") {
    std::cout << "CSC-TEST-004025 DDL_DROP_OPERATOR_FAMILY deterministic_cluster_refusal\n";
    return 0;
  }
  if (operation == "ddl-drop-cast" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") {
    std::cout << "CSC-TEST-004033 DDL_DROP_CAST deterministic_cluster_refusal\n";
    return 0;
  }
  if (operation == "ddl-create-extension" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") {
    std::cout << "CSC-TEST-004049 DDL_CREATE_EXTENSION deterministic_cluster_refusal\n";
    return 0;
  }
  if (operation == "ddl-alter-extension" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") {
    std::cout << "CSC-TEST-004053 DDL_ALTER_EXTENSION deterministic_cluster_refusal\n";
    return 0;
  }
  if (operation == "ddl-drop-extension" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") {
    std::cout << "CSC-TEST-004057 DDL_DROP_EXTENSION deterministic_cluster_refusal\n";
    return 0;
  }
  if (operation == "filespace-create" && !result.accepted && !result.messages.diagnostics.empty()) {
    std::cout << "CSC-TEST-003025 FILESPACE_CREATE deterministic_profile_refusal\n";
    return 0;
  }
  if (operation == "ddl-create-subscription" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") {
    std::cout << "CSC-TEST-003989 DDL_CREATE_SUBSCRIPTION deterministic_cluster_refusal\n";
    return 0;
  }
  if (operation == "ddl-alter-subscription" ||
      operation == "ddl-drop-subscription") {
    const bool exact_refusal =
        !result.accepted && result.messages.diagnostics.size() == 1 &&
        result.messages.diagnostics.front().code ==
            "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN";
    const bool no_canonical_result =
        result.sblr_payload.empty() && result.server_operation_id.empty() &&
        result.server_cursor_uuid.empty() && result.server_row_count == 0 &&
        result.server_affected_rows == 0 &&
        !result.server_affected_rows_present &&
        result.server_result_payload.empty();
    if (!exact_refusal || !no_canonical_result) {
      std::cerr << (operation == "ddl-alter-subscription"
                        ? "CSC-TEST-003993 DDL_ALTER_SUBSCRIPTION fail_closed_contract_failed\n"
                        : "CSC-TEST-003997 DDL_DROP_SUBSCRIPTION fail_closed_contract_failed\n");
      return 4;
    }
    std::cout << (operation == "ddl-alter-subscription"
                      ? "CSC-TEST-003993 DDL_ALTER_SUBSCRIPTION deterministic_cluster_refusal\n"
                      : "CSC-TEST-003997 DDL_DROP_SUBSCRIPTION deterministic_cluster_refusal\n");
    return 0;
  }
  if (operation == "cluster-create-placement-policy" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "SB_DIAG_CLUSTER_TXN_UNAVAILABLE") {
    std::cout << "CSC-TEST-004073 CLUSTER_CREATE_PLACEMENT_POLICY deterministic_cluster_refusal\n";
    return 0;
  }
  if ((operation == "cluster-alter-placement-policy" || operation == "cluster-drop-placement-policy") && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "SB_DIAG_CLUSTER_TXN_UNAVAILABLE") {
    std::cout << (operation == "cluster-alter-placement-policy" ? "CSC-TEST-004077 CLUSTER_ALTER_PLACEMENT_POLICY" : "CSC-TEST-004081 CLUSTER_DROP_PLACEMENT_POLICY") << " deterministic_cluster_refusal\n";
    return 0;
  }
  if (operation == "versioned-branch-create" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004117 VERSIONED_BRANCH_CREATE deterministic_cluster_refusal\n"; return 0; }
  if (operation == "versioned-branch-delete" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004121 VERSIONED_BRANCH_DELETE deterministic_cluster_refusal\n"; return 0; }
  if (operation == "versioned-diff" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004125 VERSIONED_DIFF deterministic_cluster_refusal\n"; return 0; }
  if (operation == "versioned-tag" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004129 VERSIONED_TAG deterministic_cluster_refusal\n"; return 0; }
  if (operation == "versioned-revert" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004133 VERSIONED_REVERT deterministic_cluster_refusal\n"; return 0; }
  if (operation == "versioned-reset" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004137 VERSIONED_RESET deterministic_cluster_refusal\n"; return 0; }
  if (operation == "bitemporal-as-of" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004285 BITEMPORAL_AS_OF deterministic_cluster_refusal\n"; return 0; }
  if (operation == "verifiable-history-prove" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004289 VERIFIABLE_HISTORY_PROVE deterministic_cluster_refusal\n"; return 0; }
  if (operation == "verify-proof-descriptor" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004293 VERIFY_PROOF_DESCRIPTOR deterministic_cluster_refusal\n"; return 0; }
  if (operation == "versioned-merge" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004297 VERSIONED_MERGE deterministic_cluster_refusal\n"; return 0; }
  if (operation == "versioned-hash-read" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004301 VERSIONED_HASH_READ deterministic_cluster_refusal\n"; return 0; }
  if (operation == "versioned-status-read" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004305 VERSIONED_STATUS_READ deterministic_cluster_refusal\n"; return 0; }
  if (operation == "accel-llvm-policy-set" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004141 ACCEL_LLVM_POLICY_SET deterministic_cluster_refusal\n"; return 0; }
  if (operation == "accel-llvm-compile" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004145 ACCEL_LLVM_COMPILE deterministic_cluster_refusal\n"; return 0; }
  if (operation == "accel-gpu-compile" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004161 ACCEL_GPU_COMPILE deterministic_cluster_refusal\n"; return 0; }
  if (operation == "accel-llvm-inspect" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004149 ACCEL_LLVM_INSPECT deterministic_cluster_refusal\n"; return 0; }
  if (operation == "accel-llvm-invalidate" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004153 ACCEL_LLVM_INVALIDATE deterministic_cluster_refusal\n"; return 0; }
  if (operation == "accel-gpu-policy-set" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004157 ACCEL_GPU_POLICY_SET deterministic_cluster_refusal\n"; return 0; }
  if (operation == "accel-gpu-inspect" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004165 ACCEL_GPU_INSPECT deterministic_cluster_refusal\n"; return 0; }
  if (operation == "accel-gpu-invalidate" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004169 ACCEL_GPU_INVALIDATE deterministic_cluster_refusal\n"; return 0; }
  if (operation == "bridge-describe-capabilities" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004173 BRIDGE_DESCRIBE_CAPABILITIES deterministic_cluster_refusal\n"; return 0; }
  if (operation == "bridge-open-channel" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004177 BRIDGE_OPEN_CHANNEL deterministic_cluster_refusal\n"; return 0; }
  if (operation == "bridge-authenticate" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004181 BRIDGE_AUTHENTICATE deterministic_cluster_refusal\n"; return 0; }
  if (operation == "bridge-open-session" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004185 BRIDGE_OPEN_SESSION deterministic_cluster_refusal\n"; return 0; }
  if (operation == "bridge-close-session" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004189 BRIDGE_CLOSE_SESSION deterministic_cluster_refusal\n"; return 0; }
  if (operation == "bridge-health" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004193 BRIDGE_HEALTH deterministic_cluster_refusal\n"; return 0; }
  if (operation == "bridge-begin-transaction" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004197 BRIDGE_BEGIN_TRANSACTION deterministic_cluster_refusal\n"; return 0; }
  if (operation == "bridge-commit-transaction" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004201 BRIDGE_COMMIT_TRANSACTION deterministic_cluster_refusal\n"; return 0; }
  if (operation == "bridge-rollback-transaction" && !result.accepted && !result.messages.diagnostics.empty() && result.messages.diagnostics.front().code == "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN") { std::cout << "CSC-TEST-004205 BRIDGE_ROLLBACK_TRANSACTION deterministic_cluster_refusal\n"; return 0; }
  if (!result.accepted) {
    if (result.messages.diagnostics.empty())
      std::cerr << "SBLR.DDL_CREATE_TYPE.EMPTY_FAILURE operation=" << operation
                << " payload_bytes=" << result.sblr_payload.size()
                << " result_bytes=" << result.server_result_payload.size() << '\n';
    for (const auto& diagnostic : result.messages.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      for (const auto& field : diagnostic.fields)
        std::cerr << diagnostic.code << ':' << field.name << '=' << field.value << '\n';
    }
    return 4;
  }
  std::cout << (operation == "error-vector"
                    ? "CSC-TEST-002341 ERROR_VECTOR accepted\n"
                    : operation == "txn-commit"
                          ? "CSC-TEST-002349 TXN_COMMIT accepted\n"
                    : operation == "txn-rollback"
                          ? "CSC-TEST-002353 TXN_ROLLBACK accepted\n"
                    : operation == "txn-savepoint"
                          ? "CSC-TEST-002357 TXN_SAVEPOINT accepted\n"
                    : operation == "txn-release-savepoint"
                          ? "CSC-TEST-002361 TXN_RELEASE_SAVEPOINT accepted\n"
                    : operation == "txn-rollback-to-savepoint"
                          ? "CSC-TEST-002365 TXN_ROLLBACK_TO_SAVEPOINT accepted\n"
                    : operation == "psql-autonomous-frame"
                          ? "CSC-TEST-002369 PSQL_AUTONOMOUS_FRAME accepted\n"
                    : operation == "transaction-reservation-release"
                          ? "CSC-TEST-002373 TRANSACTION_RESERVATION_RELEASE accepted\n"
                    : operation == "temporary-instance-cleanup"
                          ? "CSC-TEST-002377 TEMPORARY_INSTANCE_CLEANUP accepted\n"
                    : operation == "cursor-open"
                          ? "CSC-TEST-002381 CURSOR_OPEN accepted\n"
                    : operation == "cursor-fetch"
                          ? "CSC-TEST-002385 CURSOR_FETCH accepted\n"
                    : operation == "cursor-close"
                          ? "CSC-TEST-002389 CURSOR_CLOSE accepted\n"
                    : operation == "read-by-key"
                          ? "CSC-TEST-002393 READ_BY_KEY accepted\n"
                    : operation == "read-range"
                          ? "CSC-TEST-002397 READ_RANGE accepted\n"
                    : operation == "read-stream"
                          ? "CSC-TEST-002401 READ_STREAM accepted\n"
                    : operation == "result-set-pass"
                          ? "CSC-TEST-002405 RESULT_SET_PASS accepted\n"
                    : operation == "access-cursor-open"
                          ? "CSC-TEST-002409 ACCESS_CURSOR_OPEN accepted\n"
                    : operation == "access-cursor-fetch"
                          ? "CSC-TEST-002413 ACCESS_CURSOR_FETCH accepted\n"
                    : operation == "access-cursor-close"
                          ? "CSC-TEST-002417 ACCESS_CURSOR_CLOSE accepted\n"
                    : operation == "ddl-create-temporary-table"
                          ? "CSC-TEST-002661 DDL_CREATE_TEMPORARY_TABLE accepted\n"
                    : operation == "ddl-drop-temporary-table"
                          ? "CSC-TEST-002665 DDL_DROP_TEMPORARY_TABLE accepted\n"
                    : operation == "alter-gpu-profile-disable"
                          ? "CSC-TEST-002913 ALTER_GPU_PROFILE_DISABLE deterministic_refusal\n"
                    : operation == "insert"
                          ? "CSC-TEST-002421 INSERT accepted\n"
                    : operation == "update"
                          ? "CSC-TEST-002425 UPDATE accepted\n"
                    : operation == "delete"
                          ? "CSC-TEST-002429 DELETE accepted\n"
                    : operation == "merge"
                          ? "CSC-TEST-002433 MERGE accepted\n"
                    : operation == "txn-begin"
                          ? "CSC-TEST-002345 TXN_BEGIN accepted\n"
                    : "CSC-TEST-002337 SOURCE_MAP accepted\n");
  return 0;
}
