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
#include "engine/sblr/sblr_name_resolve_runtime.hpp"
#include "engine/sblr/sblr_optimizer_stats_drop_runtime.hpp"
#include "engine/sblr/sblr_optimizer_stats_read_runtime.hpp"
#include "engine/sblr/sblr_parse_text_runtime.hpp"
#include "engine/sblr/sblr_catalog_epoch_check_runtime.hpp"
#include "engine/sblr/sblr_database_attach_runtime.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "engine/sblr/sblr_source_artifact_runtime.hpp"
#include "engine/sblr/sblr_to_sbsql.hpp"
#include "ast/ast.hpp"
#include "cst/cst.hpp"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "wire/sbsql_test_wire.hpp"

#include <algorithm>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 7) {
    std::cerr << "usage: descriptor-client ENDPOINT DATABASE USER PASSWORD OPERATION APPLICATION\n";
    return 2;
  }
  scratchbird::parser::sbsql::ParserConfig config;
  config.server_endpoint = argv[1];
  config.database_token = argv[2];
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
  const std::string operation = argv[5];
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
    scratchbird::parser::sbsql::SbsqlCanonicalExecutionObservation
        observation;
    auto executed = external
                        ? session.RunSourceArtifactExternalReferenceForWire(
                              &observation)
                        : session.RunSourceArtifactContainerForWire(
                              &observation);
    if (!executed.accepted || executed.messages.has_errors() ||
        !observation.captured ||
        observation.operation_id != "engine.op.txn_begin" ||
        observation.canonical_container_bytes.empty() ||
        observation.external_source_artifact != external ||
        (external &&
         (observation.canonical_execution_envelope_bytes.empty() ||
          observation.external_source_artifact_bytes.empty()))) {
      for (const auto& diagnostic : executed.messages.diagnostics) {
        std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
      }
      std::cerr << "source_artifact_execution_observation_failed\n";
      return 4;
    }
    const auto decoded_container = container::DecodeSblrContainerBytes(
        observation.canonical_container_bytes.data(),
        observation.canonical_container_bytes.size());
    if (decoded_container.status != container::SblrCodecStatus::ok ||
        container::SblrReadU16(
            decoded_container.container.canonical_anchor.data() + 100) != 1 ||
        (external ? !decoded_container.container.source_map.empty()
                  : decoded_container.container.source_map.empty())) {
      std::cerr << "source_artifact_container_decode_failed\n";
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
        std::cerr << "source_artifact_external_reference_failed\n";
        return 4;
      }
    }
    const std::string_view opcode_bytes(
        reinterpret_cast<const char*>(
            decoded_container.container.operation_payload.data()),
        decoded_container.container.operation_payload.size());
    const auto decoded_stream = sblr::DecodeSblrOpcodeStream(opcode_bytes);
    if (!decoded_stream.ok || decoded_stream.stream.operations.size() != 3 ||
        decoded_stream.stream.operations[1].operation_id !=
            "engine.op.txn_begin" ||
        decoded_stream.stream.operations[1].opcode != "SBLR_TXN_BEGIN") {
      std::cerr << "source_artifact_opcode_stream_profile_failed\n";
      return 4;
    }
    const auto& artifact_bytes =
        external ? observation.external_source_artifact_bytes
                 : decoded_container.container.source_map;
    const auto decoded_artifact = sblr::DecodeSblrSourceArtifactMapV1(
        artifact_bytes.data(), artifact_bytes.size());
    if (decoded_artifact.status !=
            sblr::SblrSourceArtifactDecodeStatusV1::ok ||
        !decoded_artifact.artifact.symbols.empty() ||
        decoded_artifact.artifact.source_spans.size() != 1 ||
        decoded_artifact.artifact.render_hints.size() != 1 ||
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
      std::cerr << "source_artifact_binding_profile_failed\n";
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
        rendered.sbsql_text != "BEGIN TRANSACTION;") {
      std::cerr << "source_artifact_source_preserving_render_failed\n";
      return 4;
    }
    const auto reparsed_cst =
        scratchbird::parser::sbsql::BuildCst(rendered.sbsql_text);
    const auto reparsed_ast =
        scratchbird::parser::sbsql::BuildAst(reparsed_cst);
    if (reparsed_cst.messages.has_errors() ||
        reparsed_ast.messages.has_errors() ||
        reparsed_ast.family !=
            scratchbird::parser::sbsql::StatementFamily::kTransaction ||
        (reparsed_ast.statement_surface_name != "begin_transaction" &&
         reparsed_ast.statement_surface_name != "begin_stmt")) {
      std::cerr << "source_artifact_reparse_failed\n";
      return 4;
    }
    const auto rolled_back =
        session.RunPipeline("ROLLBACK TRANSACTION", true);
    if (!rolled_back.accepted || rolled_back.messages.has_errors()) {
      std::cerr << "source_artifact_cleanup_rollback_failed\n";
      return 4;
    }
    if (external) {
      std::cout << "CSC-TEST-005774 SOURCE_ARTIFACT_EXTERNAL_REFERENCE "
                   "accepted server_admission=true receipt_resolution=true "
                   "source_preserving_render=true reparse=true\n";
    } else {
      std::cout << "CSC-TEST-005770 SOURCE_ARTIFACT_CONTAINER accepted "
                   "server_admission=true source_preserving_render=true "
                   "reparse=true\n";
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
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true);return begun.accepted?session.RunProcedureInvokeForWire():begun; }()
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
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateTriggerForWire():begun; }()
                    : operation == "ddl-alter-trigger"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlAlterTriggerForWire():begun; }()
                    : operation == "ddl-drop-trigger"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlDropTriggerForWire():begun; }()
                    : operation == "ddl-create-procedure"
                          ? [&session] { auto begun=session.RunPipeline("BEGIN TRANSACTION",true); return begun.accepted?session.RunDdlCreateProcedureForWire():begun; }()
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
