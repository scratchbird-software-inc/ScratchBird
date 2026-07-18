// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"
#include "firebird_execution_session.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

struct ParseCase {
  std::string_view sql;
  std::string_view statement_family;
  std::string_view operation_family;
  bool expect_non_file_diagnostic{false};
  bool expect_parser_support_udr_route{false};
};

bool ExpectParseCase(const ParseCase& test) {
  const auto result = scratchbird::parser::firebird::ParseStatement(test.sql);
  if (!Expect(result.ok, std::string("Firebird parse failed for: ") +
                           std::string(test.sql))) {
    return false;
  }
  if (!Expect(result.statement_family == test.statement_family,
              std::string("Firebird statement family mismatch for: ") +
                  std::string(test.sql))) {
    return false;
  }
  if (!Expect(result.operation_family == test.operation_family,
              std::string("Firebird operation family mismatch for: ") +
                  std::string(test.sql))) {
    return false;
  }
  const std::string expected_operation =
      "\"operation_family\":\"" + std::string(test.operation_family) + "\"";
  const std::string expected_statement =
      "\"statement_family\":\"" + std::string(test.statement_family) + "\"";
  if (!Expect(Contains(result.sblr_envelope, expected_statement),
              std::string("Firebird SBLR envelope missing statement family for: ") +
                  std::string(test.sql))) {
    return false;
  }
  if (!Expect(Contains(result.sblr_envelope, expected_operation),
              std::string("Firebird SBLR envelope missing operation for: ") +
                  std::string(test.sql))) {
    return false;
  }
  if (!Expect(Contains(result.sblr_envelope,
                       "\"descriptor_resolution\":\"uuid_required\"") &&
                  Contains(result.sblr_envelope,
                           "\"engine_authority\":\"scratchbird\"") &&
                  Contains(result.sblr_envelope, "\"finite_subset\":true") &&
                  Contains(result.sblr_envelope,
                           "\"full_declared_surface_assignment\":true"),
              std::string("Firebird SBLR envelope missing binder descriptor fields for: ") +
                  std::string(test.sql))) {
    return false;
  }
  if (!Expect(Contains(result.sblr_envelope, "\"parser_evidence\":{") &&
                  Contains(result.parser_evidence_json,
                           "\"cst_materialized\":true") &&
                  Contains(result.parser_evidence_json,
                           "\"ast_materialized\":true") &&
                  Contains(result.parser_evidence_json,
                           "\"bound_ast_materialized\":true") &&
                  Contains(result.parser_evidence_json,
                           "\"source_text_redacted\":true") &&
                  Contains(result.parser_evidence_json,
                           "\"descriptor_uuid_required\":true") &&
                  Contains(result.parser_evidence_json,
                           "\"parser_transaction_finality_authority\":false") &&
                  Contains(result.parser_evidence_json,
                           "\"parser_storage_authority\":false") &&
                  Contains(result.parser_evidence_json,
                           "\"firebird_connection_sandbox_evidence\":{") &&
                  Contains(result.parser_evidence_json,
                           "\"evidence_contract\":\"firebird_connection_sandbox_evidence.v1\"") &&
                  Contains(result.parser_evidence_json,
                           "\"connection_sandbox_contract\":\"compatibility_connection_schema_root_v1\"") &&
                  Contains(result.parser_evidence_json,
                           "\"user_object_resolution\":\"relative_to_connection_schema_root\"") &&
                  Contains(result.parser_evidence_json,
                           "\"direct_cross_root_access\":\"unsupported_denied\"") &&
                  Contains(result.parser_evidence_json,
                           "\"foreign_parser_tree_visibility_inherited\":false") &&
                  Contains(result.parser_evidence_json,
                           "\"catalog_projection_can_query_outside_sandbox\":true") &&
                  Contains(result.parser_evidence_json,
                           "\"parser_authorization_authority\":false") &&
                  Contains(result.parser_evidence_json,
                           "\"mga_transaction_authority\":\"scratchbird_engine\""),
              std::string("Firebird parser evidence contract mismatch for: ") +
                  std::string(test.sql))) {
    return false;
  }
  if (!Expect(Contains(result.sblr_envelope, "\"sql_text_included\":false"),
              std::string("Firebird SBLR envelope leaked SQL policy for: ") +
                  std::string(test.sql))) {
    return false;
  }
  const bool has_non_file_diagnostic =
      Contains(result.message_vector_json, "FIREBIRD.EMULATION.NON_FILE_SURFACE");
  if (!Expect(has_non_file_diagnostic == test.expect_non_file_diagnostic,
              std::string("Firebird non-file diagnostic expectation mismatch for: ") +
                  std::string(test.sql))) {
    return false;
  }
  if (!Expect(result.parser_support_udr_route ==
                  test.expect_parser_support_udr_route,
              std::string("Firebird parser-support UDR route expectation mismatch for: ") +
                  std::string(test.sql))) {
    return false;
  }
  const std::string expected_udr_route =
      "\"parser_support_udr_route\":" +
      std::string(test.expect_parser_support_udr_route ? "true" : "false");
  if (!Expect(Contains(result.sblr_envelope, expected_udr_route),
              std::string("Firebird SBLR envelope missing parser-support UDR route flag for: ") +
                  std::string(test.sql))) {
    return false;
  }
  return true;
}

bool ExpectRejected(std::string_view sql, std::string_view diagnostic_code) {
  const auto result = scratchbird::parser::firebird::ParseStatement(sql);
  if (!Expect(!result.ok, std::string("Firebird parse was accepted for: ") +
                            std::string(sql))) {
    std::cerr << result.sblr_envelope << '\n';
    return false;
  }
  if (!Expect(result.sblr_envelope.empty(),
              std::string("Rejected Firebird input produced SBLR for: ") +
                  std::string(sql))) {
    return false;
  }
  if (!Expect(Contains(result.message_vector_json, diagnostic_code),
              std::string("Rejected Firebird input missing diagnostic for: ") +
                  std::string(sql))) {
    std::cerr << result.message_vector_json << '\n';
    return false;
  }
  return true;
}

bool ExpectCompositeTableKeyEnvelope() {
  scratchbird::parser::ipc::ParserClientConfig config;
  scratchbird::parser::firebird::FirebirdExecutionSession session(config);
  const auto result = session.RunStatement(
      "CREATE TABLE T (A INTEGER NOT NULL, B INTEGER NOT NULL, "
      "C INTEGER NOT NULL, D INTEGER NOT NULL, "
      "CONSTRAINT PK_T PRIMARY KEY (A, B), "
      "CONSTRAINT UQ_T UNIQUE (C, D))",
      {}, false);
  const auto& envelope = result.sblr_payload;
  bool ok = true;
  ok = Expect(result.accepted,
              "Firebird composite table-key lowering was rejected") && ok;
  ok = Expect(Contains(envelope, "\"table_index_count\":2"),
              "Firebird composite table-key count missing") && ok;
  ok = Expect(Contains(envelope, "\"table_index_0_key_count\":2") &&
                  Contains(envelope, "\"table_index_0_key_0\":\"A\"") &&
                  Contains(envelope, "\"table_index_0_key_1\":\"B\"") &&
                  Contains(envelope,
                           "\"table_index_0_constraint_kind\":\"primary_key\"") &&
                  Contains(envelope,
                           "\"table_index_0_constraint_name\":\"PK_T\""),
              "Firebird ordered composite PRIMARY KEY fields missing") && ok;
  ok = Expect(Contains(envelope, "\"table_index_1_key_count\":2") &&
                  Contains(envelope, "\"table_index_1_key_0\":\"C\"") &&
                  Contains(envelope, "\"table_index_1_key_1\":\"D\"") &&
                  Contains(envelope,
                           "\"table_index_1_constraint_kind\":\"unique\"") &&
                  Contains(envelope,
                           "\"table_index_1_constraint_name\":\"UQ_T\""),
              "Firebird ordered composite UNIQUE fields missing") && ok;
  ok = Expect(envelope.find("\"table_index_0_key_0\":\"A\"") <
                      envelope.find("\"table_index_0_key_1\":\"B\"") &&
                  envelope.find("\"table_index_1_key_0\":\"C\"") <
                      envelope.find("\"table_index_1_key_1\":\"D\""),
              "Firebird composite key column order was not retained") && ok;
  ok = Expect(!Contains(envelope, ";primary_key=true") &&
                  !Contains(envelope, ";unique=true"),
              "Firebird table constraint leaked into per-column uniqueness") && ok;
  ok = Expect(!Contains(envelope, "\"table_object_uuid\"") &&
                  !Contains(envelope, "\"requested_index_uuid\"") &&
                  !Contains(envelope, "\"table_index_0_index_uuid\"") &&
                  !Contains(envelope, "\"table_index_0_constraint_uuid\"") &&
                  !Contains(envelope, "\"table_index_0_index_name\"") &&
                  !Contains(envelope, "\"table_index_1_index_name\""),
              "Firebird composite table-key lowering invented engine identity or names") && ok;

  const auto expect_lowering_refused = [&](std::string_view sql,
                                           std::string_view label) {
    const auto rejected = session.RunStatement(sql, {}, true);
    const std::string diagnostics =
        scratchbird::parser::ipc::MessageVectorToJson(rejected.messages);
    return Expect(!rejected.accepted,
                  std::string(label) + " was accepted") &&
           Expect(rejected.sblr_payload.empty(),
                  std::string(label) + " produced executable SBLR") &&
           Expect(Contains(diagnostics, "FIREBIRD.SBLR.LOWERING_UNAVAILABLE"),
                  std::string(label) + " did not fail closed in the lowerer") &&
           Expect(!Contains(diagnostics, "FIREBIRD.SERVER.UNAVAILABLE"),
                  std::string(label) + " reached the server route");
  };

  ok = expect_lowering_refused(
           "CREATE TABLE T_DUP (A INTEGER, PRIMARY KEY (A, A))",
           "duplicate composite key component") && ok;
  ok = expect_lowering_refused(
           "CREATE TABLE T_UNKNOWN (A INTEGER, UNIQUE (A, MISSING_COLUMN))",
           "unknown composite key component") && ok;
  ok = expect_lowering_refused(
           "CREATE TABLE T_MULTI (A INTEGER, B INTEGER, "
           "PRIMARY KEY (A), PRIMARY KEY (B))",
           "multiple table primary keys") && ok;

  std::string oversized_key = "CREATE TABLE T_KEY_BOUND (";
  for (std::size_t i = 0; i < 65; ++i) {
    if (i != 0) oversized_key += ", ";
    oversized_key += "C" + std::to_string(i) + " INTEGER";
  }
  oversized_key += ", UNIQUE (";
  for (std::size_t i = 0; i < 65; ++i) {
    if (i != 0) oversized_key += ", ";
    oversized_key += "C" + std::to_string(i);
  }
  oversized_key += "))";
  ok = expect_lowering_refused(oversized_key,
                               "oversized composite key") && ok;

  std::string oversized_constraint_count =
      "CREATE TABLE T_INDEX_BOUND (A INTEGER";
  for (std::size_t i = 0; i < 65; ++i) {
    oversized_constraint_count += ", UNIQUE (A)";
  }
  oversized_constraint_count += ")";
  ok = expect_lowering_refused(oversized_constraint_count,
                               "oversized table-key count") && ok;
  return ok;
}

bool ExpectTextResourceCreateEnvelope() {
  scratchbird::parser::ipc::ParserClientConfig config;
  scratchbird::parser::firebird::FirebirdExecutionSession session(config);
  const auto result = session.RunStatement(
      "CREATE TABLE T_TEXT (ID INTEGER, FNAME VARCHAR(20) CHARACTER SET GBK)",
      {}, false);
  const auto& envelope = result.sblr_payload;
  bool ok = true;
  ok = Expect(result.accepted,
              "Firebird CHARACTER SET table lowering was rejected") && ok;
  ok = Expect(Contains(envelope,
                       "type=VARCHAR(20);nullable=true;character_length=20"),
              "Firebird character length was not lowered into the column descriptor") &&
       ok;
  ok = Expect(!Contains(envelope, "GBK") &&
                  !Contains(envelope, "charset_uuid=") &&
                  !Contains(envelope, "collation_uuid="),
              "Firebird parse-only lowering leaked a resource name or invented an engine UUID") &&
       ok;
  return ok;
}

bool ExpectAsciiCharInsertEnvelope() {
  constexpr std::string_view kOctetFromInt64ScalarHex =
      "7363616c61722e6f637465745f66726f6d5f696e7436342e7631";
  scratchbird::parser::ipc::ParserClientConfig config;
  scratchbird::parser::firebird::FirebirdExecutionSession session(config);
  const auto lowered = session.RunStatement(
      "INSERT INTO TEST_NONE VALUES (ASCII_CHAR(1))", {}, false);
  bool ok = true;
  ok = Expect(lowered.accepted,
              "Firebird literal ASCII_CHAR insert lowering was rejected") &&
       ok;
  ok = Expect(
           Contains(lowered.sblr_payload,
                    "\"operation_id\":\"dml.insert_rows\"") &&
               Contains(lowered.sblr_payload,
                        "\"insert_values_compact_format\":"
                        "\"sblr.dml.insert.cells.hex.v1\"") &&
               Contains(lowered.sblr_payload,
                        "\"insert_values_compact_payload\":"
                        "\"6330|" +
                            std::string(kOctetFromInt64ScalarHex) +
                            "|31|0\"") &&
               Contains(lowered.sblr_payload,
                        "\"target_object_uuid\":\"\"") &&
               !Contains(lowered.sblr_payload, "ASCII_CHAR") &&
               !Contains(lowered.sblr_payload,
                         "sbsql.insert_values.cells.v1"),
           "Firebird ASCII_CHAR literal did not bind to the neutral engine scalar packet") &&
       ok;

  const auto byte_255 = session.RunStatement(
      "INSERT INTO TEST_NONE VALUES (ASCII_CHAR(255))", {}, false);
  ok = Expect(
           byte_255.accepted &&
               Contains(byte_255.sblr_payload,
                        "\"insert_values_compact_payload\":"
                        "\"6330|" +
                            std::string(kOctetFromInt64ScalarHex) +
                            "|323535|0\""),
           "Firebird ASCII_CHAR byte 255 was not bound for engine execution") &&
       ok;

  const auto sql_null = session.RunStatement(
      "INSERT INTO TEST_NONE VALUES (ASCII_CHAR(NULL))", {}, false);
  ok = Expect(
           sql_null.accepted &&
               Contains(sql_null.sblr_payload,
                        "\"insert_values_compact_payload\":"
                        "\"6330|" +
                            std::string(kOctetFromInt64ScalarHex) +
                            "||1\""),
           "Firebird ASCII_CHAR(NULL) did not preserve SQL NULL for engine execution") &&
       ok;

  // Range validation belongs to the engine scalar evaluator.  The standalone
  // Firebird parser must bind the literal without manufacturing its result.
  const auto out_of_range = session.RunStatement(
      "INSERT INTO TEST_NONE VALUES (ASCII_CHAR(256))", {}, false);
  ok = Expect(
           out_of_range.accepted &&
               Contains(out_of_range.sblr_payload,
                        "\"insert_values_compact_payload\":"
                        "\"6330|" +
                            std::string(kOctetFromInt64ScalarHex) +
                            "|323536|0\""),
           "Firebird ASCII_CHAR range checking leaked into parser lowering") &&
       ok;

  for (const auto sql : {
           "INSERT INTO TEST_NONE VALUES (ASCII_CHAR(ID))"}) {
    const auto rejected = session.RunStatement(sql, {}, true);
    const std::string diagnostics =
        scratchbird::parser::ipc::MessageVectorToJson(rejected.messages);
    ok = Expect(!rejected.accepted && rejected.sblr_payload.empty() &&
                    Contains(diagnostics,
                             "FIREBIRD.SBLR.LOWERING_UNAVAILABLE") &&
                    !Contains(diagnostics, "FIREBIRD.SERVER.UNAVAILABLE"),
                "unsupported ASCII_CHAR shape did not fail closed before route access") &&
         ok;
  }
  return ok;
}

bool ExpectPreparedDsqlPreflight() {
  scratchbird::parser::ipc::ParserClientConfig config;
  scratchbird::parser::firebird::FirebirdExecutionSession session(config);
  const scratchbird::parser::ipc::ParserTransactionSelector selector{
      41, "00000000-0000-0000-0000-000000000041"};
  bool ok = true;

  const auto parameterized = session.BindAndLowerForPrepare(
      "INSERT INTO T (A) VALUES (?)", selector);
  const std::string parameter_diagnostics =
      scratchbird::parser::ipc::MessageVectorToJson(parameterized.messages);
  ok = Expect(!parameterized.accepted &&
                  parameterized.sblr_payload.empty() &&
                  Contains(parameter_diagnostics,
                           "FIREBIRD.DSQL.PARAMETER_PACKET_UNAVAILABLE"),
              "Firebird prepared parameter preflight did not fail closed") &&
       ok;

  const auto recreate = session.BindAndLowerForPrepare(
      "RECREATE TABLE T (A INTEGER)", selector);
  const std::string recreate_diagnostics =
      scratchbird::parser::ipc::MessageVectorToJson(recreate.messages);
  ok = Expect(!recreate.accepted && recreate.sblr_payload.empty() &&
                  Contains(recreate_diagnostics,
                           "FIREBIRD.SERVER.UNAVAILABLE") &&
                  !Contains(recreate_diagnostics,
                            "FIREBIRD.DSQL.PREPARE_PRELUDE_UNSUPPORTED"),
              "Firebird prepared RECREATE did not admit mutation-free CREATE lowering") &&
       ok;

  const auto transaction =
      session.BindAndLowerForPrepare("COMMIT", selector);
  const std::string transaction_diagnostics =
      scratchbird::parser::ipc::MessageVectorToJson(transaction.messages);
  ok = Expect(
           !transaction.accepted && transaction.sblr_payload.empty() &&
               Contains(
                   transaction_diagnostics,
                   "FIREBIRD.DSQL.PREPARED_TRANSACTION_CONTROL_UNSUPPORTED"),
           "Firebird prepared transaction control bypassed the finality path") &&
       ok;

  const auto prepared_parameter = session.PrepareStatement(
      "UPDATE T SET A = ?", selector);
  const std::string prepared_parameter_diagnostics =
      scratchbird::parser::ipc::MessageVectorToJson(
          prepared_parameter.messages);
  ok = Expect(!prepared_parameter.accepted &&
                  prepared_parameter.prepared_statement_uuid.empty() &&
                  Contains(prepared_parameter_diagnostics,
                           "FIREBIRD.DSQL.PARAMETER_PACKET_UNAVAILABLE"),
              "Firebird parameter refusal installed a prepared UUID") &&
       ok;
  return ok;
}

bool ExpectBetweenPredicateEnvelopes() {
  scratchbird::parser::ipc::ParserClientConfig config;
  scratchbird::parser::firebird::FirebirdExecutionSession session(config);
  bool ok = true;

  const auto counted = session.RunStatement(
      "SELECT COUNT(*) AS CNT FROM TEST WHERE A BETWEEN 4 AND 7",
      {}, false);
  ok = Expect(counted.accepted,
              "Firebird BETWEEN count lowering was rejected") &&
       ok;
  ok = Expect(
           Contains(counted.sblr_payload,
                    "\"operation_id\":\"dml.select_rows\"") &&
               Contains(counted.sblr_payload,
                        "\"result_projection\":\"count\"") &&
               Contains(counted.sblr_payload,
                        "\"actual_column_name\":\"CNT\"") &&
               Contains(counted.sblr_payload,
                        "\"predicate_kind\":\"column_range\"") &&
               Contains(counted.sblr_payload,
                        "\"predicate_column\":\"A\"") &&
               Contains(counted.sblr_payload,
                        "\"predicate_value\":\"4,7\"") &&
               Contains(counted.sblr_payload,
                        "\"predicate_value_type\":\"bigint,bigint\"") &&
               Contains(counted.sblr_payload,
                        "\"target_object_uuid\":\"\"") &&
               !Contains(counted.sblr_payload,
                         "SELECT COUNT"),
           "Firebird BETWEEN count envelope lost its inclusive range bounds") &&
       ok;

  const auto deleted = session.RunStatement(
      "DELETE FROM TEST WHERE A BETWEEN 4 AND 7", {}, false);
  ok = Expect(deleted.accepted,
              "Firebird BETWEEN delete lowering was rejected") &&
       ok;
  ok = Expect(
           Contains(deleted.sblr_payload,
                    "\"operation_id\":\"dml.delete_rows\"") &&
               Contains(deleted.sblr_payload,
                        "\"predicate_kind\":\"column_range\"") &&
               Contains(deleted.sblr_payload,
                        "\"predicate_column\":\"A\"") &&
               Contains(deleted.sblr_payload,
                        "\"predicate_value\":\"4,7\"") &&
               Contains(deleted.sblr_payload,
                        "\"predicate_value_type\":\"bigint,bigint\"") &&
               !Contains(deleted.sblr_payload, "DELETE FROM"),
           "Firebird BETWEEN delete envelope mismatch") &&
       ok;

  const auto quoted = session.RunStatement(
      "SELECT COUNT(*) FROM TEST WHERE LABEL BETWEEN "
      "'A AND B, it''s' AND 'Z, end'",
      {}, false);
  ok = Expect(quoted.accepted,
              "Firebird quoted BETWEEN lowering was rejected") &&
       ok;
  ok = Expect(
           Contains(quoted.sblr_payload,
                    "\"predicate_kind\":\"column_range\"") &&
               Contains(quoted.sblr_payload,
                        "\"predicate_column\":\"LABEL\"") &&
               Contains(quoted.sblr_payload,
                        "\"predicate_value\":\"'A AND B, it''s','Z, end'\"") &&
               Contains(quoted.sblr_payload,
                        "\"predicate_value_type\":\"text,text\""),
           "Firebird quoted BETWEEN bounds were split or escaped incorrectly") &&
       ok;

  for (const auto sql : {
           "SELECT COUNT(*) FROM TEST WHERE A BETWEEN NULL AND 7",
           "SELECT COUNT(*) FROM TEST WHERE A BETWEEN 4 AND NULL"}) {
    const auto null_bound = session.RunStatement(sql, {}, false);
    ok = Expect(
             null_bound.accepted &&
                 Contains(null_bound.sblr_payload,
                          "\"predicate_kind\":\"always_false\"") &&
                 !Contains(null_bound.sblr_payload, "\"predicate_value\":"),
             "Firebird NULL BETWEEN bound did not lower to always_false") &&
         ok;
  }

  const auto expect_lowering_refused = [&](std::string_view sql,
                                           std::string_view label) {
    const auto rejected = session.RunStatement(sql, {}, true);
    const std::string diagnostics =
        scratchbird::parser::ipc::MessageVectorToJson(rejected.messages);
    return Expect(!rejected.accepted,
                  std::string(label) + " was accepted") &&
           Expect(rejected.sblr_payload.empty(),
                  std::string(label) + " produced executable SBLR") &&
           Expect(Contains(diagnostics, "FIREBIRD.SBLR.LOWERING_UNAVAILABLE"),
                  std::string(label) + " did not fail closed in the lowerer") &&
           Expect(!Contains(diagnostics, "FIREBIRD.SERVER.UNAVAILABLE"),
                  std::string(label) + " reached the server route");
  };

  for (const auto& [sql, label] : {
           std::pair<std::string_view, std::string_view>{
               "SELECT COUNT(*) FROM TEST WHERE A BETWEEN ? AND 7",
               "positional BETWEEN bound"},
           {"SELECT COUNT(*) FROM TEST WHERE A BETWEEN :LOWER_BOUND AND 7",
            "named BETWEEN bound"},
           {"SELECT COUNT(*) FROM TEST WHERE A BETWEEN 1 + 1 AND 7",
            "expression BETWEEN lower bound"},
           {"SELECT COUNT(*) FROM TEST WHERE A BETWEEN 1 AND ABS(7)",
            "expression BETWEEN upper bound"},
           {"SELECT COUNT(*) FROM TEST WHERE A BETWEEN 1 AND 7 AND A = 5",
            "compound BETWEEN predicate"}}) {
    ok = expect_lowering_refused(sql, label) && ok;
  }

  const auto malformed = session.RunStatement(
      "DELETE FROM TEST WHERE A BETWEEN 1 AND", {}, true);
  const std::string malformed_diagnostics =
      scratchbird::parser::ipc::MessageVectorToJson(malformed.messages);
  ok = Expect(!malformed.accepted,
              "missing BETWEEN upper bound was accepted") &&
       Expect(malformed.sblr_payload.empty(),
              "missing BETWEEN upper bound produced executable SBLR") &&
       Expect(Contains(malformed_diagnostics,
                       "FIREBIRD.SBLR.LOWERING_UNAVAILABLE") ||
                  Contains(malformed_diagnostics,
                           "FIREBIRD.PARSE.REJECTED"),
              "missing BETWEEN upper bound did not fail closed") &&
       Expect(!Contains(malformed_diagnostics,
                        "FIREBIRD.SERVER.UNAVAILABLE"),
              "missing BETWEEN upper bound reached the server route") &&
       ok;

  return ok;
}

bool ExpectGbakLogicalStream(std::string_view sql,
                             std::string_view operation_family) {
  const auto result = scratchbird::parser::firebird::ParseStatement(sql);
  if (!Expect(result.ok,
              std::string("Firebird gbak logical stream rejected for: ") +
                  std::string(sql))) {
    return false;
  }
  if (!Expect(result.statement_family == "logical_stream_backup_restore",
              std::string("Firebird gbak logical stream family mismatch for: ") +
                  std::string(sql))) {
    return false;
  }
  if (!Expect(result.operation_family == operation_family,
              std::string("Firebird gbak logical stream operation mismatch for: ") +
                  std::string(sql))) {
    return false;
  }
  if (!Expect(!result.scratchbird_lifecycle_api &&
                  !result.real_firebird_file_effects &&
                  !result.reference_engine_sql_executed,
              std::string("Firebird gbak logical stream claimed forbidden authority for: ") +
                  std::string(sql))) {
    return false;
  }
  if (!Expect(Contains(result.sblr_envelope,
                       "\"statement_family\":\"logical_stream_backup_restore\"") &&
                  Contains(result.sblr_envelope,
                           "\"firebird_gbak_logical_stream_evidence\":{") &&
                  Contains(result.sblr_envelope,
                           "\"evidence_contract\":\"firebird_gbak_logical_stream_evidence.v1\"") &&
                  Contains(result.sblr_envelope,
                           "\"remote_client_stream\":true") &&
                  Contains(result.sblr_envelope,
                           "\"single_connected_legacy_database_scope\":true") &&
                  Contains(result.sblr_envelope,
                           "\"server_local_file_access\":\"default_denied\"") &&
                  Contains(result.sblr_envelope,
                           "\"physical_page_copy_allowed\":false") &&
                  Contains(result.sblr_envelope,
                           "\"nbackup_allowed\":false") &&
                  Contains(result.sblr_envelope,
                           "\"sblr_requirement\":\"required_logical_stream_backup_restore_surface\"") &&
                  Contains(result.sblr_envelope,
                           "\"engine_authority\":\"scratchbird\"") &&
                  Contains(result.sblr_envelope,
                           "\"scratchbird_lifecycle_api\":false") &&
                  Contains(result.sblr_envelope,
                           "\"real_firebird_file_effects\":false") &&
                  Contains(result.sblr_envelope,
                           "\"reference_engine_sql_executed\":false") &&
                  Contains(result.parser_evidence_json,
                           "\"parser_transaction_finality_authority\":false") &&
                  Contains(result.parser_evidence_json,
                           "\"parser_storage_authority\":false"),
              std::string("Firebird gbak logical stream evidence mismatch for: ") +
                  std::string(sql))) {
    return false;
  }
  const bool backup = operation_family == "firebird.logical_stream.gbak_backup";
  if (backup) {
    if (!Expect(Contains(result.sblr_envelope, "\"backup_stream\":true") &&
                    Contains(result.sblr_envelope,
                             "\"restore_stream\":false") &&
                    Contains(result.sblr_envelope,
                             "\"stdout_stream_bound\":true") &&
                    Contains(result.sblr_envelope,
                             "\"stdin_stream_bound\":false"),
                std::string("Firebird gbak backup stream evidence mismatch for: ") +
                    std::string(sql))) {
      return false;
    }
  } else if (!Expect(Contains(result.sblr_envelope, "\"backup_stream\":false") &&
                         Contains(result.sblr_envelope,
                                  "\"restore_stream\":true") &&
                         Contains(result.sblr_envelope,
                                  "\"stdout_stream_bound\":false") &&
                         Contains(result.sblr_envelope,
                                  "\"stdin_stream_bound\":true"),
                     std::string("Firebird gbak restore stream evidence mismatch for: ") +
                         std::string(sql))) {
    return false;
  }
  return true;
}

void PutProbeString(std::vector<std::uint8_t>* out,
                    std::string_view value) {
  out->push_back(static_cast<std::uint8_t>(value.size() & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value.size() >> 8u) & 0xffu));
  out->insert(out->end(), value.begin(), value.end());
}

bool ExpectPreparedV2UnknownProjection() {
  using scratchbird::parser::ipc::DecodePrepareResultPayloadV2ForTest;
  using scratchbird::parser::ipc::MessageVectorSet;
  using scratchbird::parser::ipc::ServerPrepareSblrResult;

  std::vector<std::uint8_t> malformed_success;
  PutProbeString(&malformed_success, "accepted");
  ServerPrepareSblrResult malformed;
  MessageVectorSet malformed_messages;
  bool ok = Expect(
      !DecodePrepareResultPayloadV2ForTest(
          malformed_success, &malformed, &malformed_messages) &&
          !malformed.accepted && malformed.outcome_unknown &&
          malformed.caller_cleanup_required &&
          malformed.prepared_statement_uuid.empty() &&
          Contains(scratchbird::parser::ipc::MessageVectorToJson(
                       malformed_messages),
                   "PARSER_SERVER_IPC.OUTCOME_UNKNOWN"),
      "Malformed accepted V2 prepare did not require typed caller cleanup");

  std::vector<std::uint8_t> known_rejection;
  PutProbeString(&known_rejection, "rejected");
  known_rejection.resize(known_rejection.size() + 16, 0);
  PutProbeString(&known_rejection, "sblr.prepare");
  PutProbeString(&known_rejection, "known_not_applied");
  ServerPrepareSblrResult rejected;
  MessageVectorSet rejected_messages;
  ok = Expect(
           !DecodePrepareResultPayloadV2ForTest(
               known_rejection, &rejected, &rejected_messages) &&
               !rejected.accepted && !rejected.outcome_unknown &&
               !rejected.caller_cleanup_required,
           "Explicit V2 prepare rejection incorrectly quarantined the route") &&
       ok;
  ok = Expect(
           !scratchbird::parser::ipc::V2RequestMayRetryAfterWriteForTest(4009),
           "V2 prepare was incorrectly classified as replayable after write") &&
       ok;
  ok = Expect(
           !scratchbird::parser::ipc::
               SessionBoundRequestMayRetryAfterWriteForTest(4013),
           "Session-bound prepared close was incorrectly classified as replayable after write") &&
       ok;
  return ok;
}

} // namespace

int main() {
  using namespace scratchbird::parser::firebird;

  const auto tokens = LexTokens(
      "select \"Mixed Name\", 'it''s', :p from rdb$database -- comment\nwhere id = ?");
  if (!Expect(tokens.size() >= 11, "Firebird lexer produced too few tokens")) {
    return EXIT_FAILURE;
  }
  if (!Expect(tokens[0].kind == "identifier_or_keyword" && tokens[0].lexeme == "select",
              "Firebird lexer first token mismatch")) {
    return EXIT_FAILURE;
  }
  bool saw_quoted_identifier = false;
  bool saw_string_literal = false;
  bool saw_named_parameter = false;
  bool saw_positional_parameter = false;
  bool saw_line_comment = false;
  for (const auto& token : tokens) {
    saw_quoted_identifier = saw_quoted_identifier || token.kind == "quoted_identifier";
    saw_string_literal = saw_string_literal || token.kind == "string_literal";
    saw_named_parameter = saw_named_parameter || token.lexeme == ":p";
    saw_positional_parameter = saw_positional_parameter || token.lexeme == "?";
    saw_line_comment = saw_line_comment || token.kind == "line_comment";
  }
  if (!Expect(saw_quoted_identifier && saw_string_literal && saw_named_parameter &&
                  saw_positional_parameter && saw_line_comment,
              "Firebird lexer did not classify required token forms")) {
    return EXIT_FAILURE;
  }

  if (!Expect(DatatypeSurfaces().size() == 8, "Firebird datatype family count mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(BuiltinFunctionSurfaces().size() == 8,
              "Firebird builtin/function family count mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(CatalogOverlaySurfaces().size() == 7,
              "Firebird catalog overlay family count mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(DiagnosticSurfaces().size() == 7,
              "Firebird diagnostic family count mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(BuiltinFunctionSurfaces()[6].surface, "GEN_ID"),
              "Firebird generator function family missing GEN_ID")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(CatalogOverlaySurfaces()[4].surface, "MON$ATTACHMENTS"),
              "Firebird monitoring catalog family missing MON$ATTACHMENTS")) {
    return EXIT_FAILURE;
  }

  const auto query = ParseStatement(" select 1 from customer ");
  if (!Expect(query.ok, "Firebird query parse failed")) return EXIT_FAILURE;
  if (!Expect(query.statement_family == "query", "Firebird query family mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(query.operation_family == "firebird.query.select",
              "Firebird query operation mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(query.sblr_envelope, "SBLRExecutionEnvelope.v3"),
              "Firebird query did not produce SBLR envelope")) {
    return EXIT_FAILURE;
  }
  if (!Expect(!Contains(query.sblr_envelope, "select 1") &&
                  !Contains(query.sblr_envelope, "customer"),
              "Firebird SBLR envelope leaked SQL text")) {
    return EXIT_FAILURE;
  }
  const auto bare_singleton = ParseStatement("select 1");
  if (!Expect(!bare_singleton.ok,
              "Firebird bare SELECT singleton should fail without RDB$DATABASE")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(bare_singleton.message_vector_json,
                      "FIREBIRD.PARSE.INVALID_INPUT"),
              "Firebird bare SELECT singleton diagnostic mismatch")) {
    return EXIT_FAILURE;
  }
  if (!ExpectRejected("select * from (select rdb$relation_id from rdb$database) "
                      "where sum(rdb$relation_id) = 0",
                      "FIREBIRD.DSQL.AGGREGATE_WHERE")) {
    return EXIT_FAILURE;
  }
  const auto aggregate_subquery = ParseStatement(
      "select i.id from v_test1 i where i.x = "
      "(select max(x.y) from v_test2 x)");
  if (!Expect(aggregate_subquery.ok,
              "Firebird aggregate scalar subquery in WHERE should parse")) {
    std::cerr << aggregate_subquery.message_vector_json << '\n';
    return EXIT_FAILURE;
  }
  const auto redaction_query = ParseStatement(
      "select count(*) from enterprise_secret_rdb_relation where id = :id");
  if (!Expect(redaction_query.ok, "Firebird redaction query parse failed")) {
    return EXIT_FAILURE;
  }
  if (!Expect(!Contains(redaction_query.sblr_envelope,
                        "enterprise_secret_rdb_relation") &&
                  !Contains(redaction_query.parser_evidence_json,
                            "enterprise_secret_rdb_relation"),
              "Firebird parser evidence leaked source text")) {
    return EXIT_FAILURE;
  }

  const ParseCase expression_cases[] = {
      {"SELECT GEN_ID(customer_gen, 1) FROM RDB$DATABASE",
       "query", "firebird.expression.generator.gen_id"},
      {"SELECT NEXT VALUE FOR customer_seq FROM RDB$DATABASE",
       "query", "firebird.expression.generator.next_value_for"},
      {"SELECT RDB$GET_CONTEXT('SYSTEM', 'DB_NAME') FROM RDB$DATABASE",
       "query", "firebird.expression.context.get"},
      {"SELECT RDB$SET_CONTEXT('USER_SESSION', 'k', 'v') FROM RDB$DATABASE",
       "query", "firebird.expression.context.set"},
      {"SELECT CURRENT_CONNECTION FROM RDB$DATABASE",
       "query", "firebird.expression.context.variable"},
      {"SELECT UUID_TO_CHAR(:id) FROM RDB$DATABASE",
       "query", "firebird.expression.uuid.uuid_to_char"},
      {"SELECT CHAR_TO_UUID(:id) FROM RDB$DATABASE",
       "query", "firebird.expression.uuid.char_to_uuid"},
      {"SELECT HASH('abc') FROM RDB$DATABASE",
       "query", "firebird.expression.hash.hash"},
      {"SELECT DATEADD(1 DAY TO CURRENT_DATE) FROM RDB$DATABASE",
       "query", "firebird.expression.temporal"},
      {"SELECT COUNT(*) FROM customer",
       "query", "firebird.expression.aggregate_window"},
      {"SELECT COALESCE(:name, 'n/a') FROM RDB$DATABASE",
       "query", "firebird.expression.conditional"},
      {"SELECT UPPER(name) FROM customer",
       "query", "firebird.expression.string"},
      {"SELECT ABS(total) FROM invoice",
       "query", "firebird.expression.numeric"},
  };
  for (const auto& test : expression_cases) {
    if (!ExpectParseCase(test)) return EXIT_FAILURE;
  }
  if (!ExpectRejected("SELECT GEN_ID(customer_gen, 1",
                      "FIREBIRD.PARSE.INVALID_INPUT")) {
    return EXIT_FAILURE;
  }

  const ParseCase query_cases[] = {
      {"SELECT FIRST 10 SKIP 5 id FROM customer",
       "query", "firebird.query.select.first_skip_rows"},
      {"SELECT * FROM customer ROWS 1 TO 5",
       "query", "firebird.query.select.first_skip_rows"},
      {"SELECT id FROM customer FOR UPDATE",
       "query", "firebird.query.cursor.for_update"},
      {"(SELECT 1 FROM RDB$DATABASE UNION ALL (SELECT 2 FROM RDB$DATABASE))",
       "query", "firebird.query.select"},
  };
  for (const auto& test : query_cases) {
    if (!ExpectParseCase(test)) return EXIT_FAILURE;
  }

  const ParseCase datatype_cases[] = {
      {"CREATE DOMAIN positive_int AS INTEGER CHECK (VALUE > 0)",
       "datatype", "firebird.datatype.domain.create"},
      {"ALTER DOMAIN positive_int TYPE BIGINT",
       "datatype", "firebird.datatype.domain.alter"},
      {"DROP DOMAIN positive_int",
       "datatype", "firebird.datatype.domain.drop"},
      {"SELECT CAST('42' AS INTEGER) FROM RDB$DATABASE",
       "query", "firebird.datatype.cast"},
      {"SELECT DATE '2026-05-08' FROM RDB$DATABASE",
       "query", "firebird.datatype.temporal_literal"},
      {"SELECT CAST(1 AS DECFLOAT(34)) FROM RDB$DATABASE",
       "query", "firebird.datatype.cast"},
      {"SELECT TRUE FROM RDB$DATABASE",
       "query", "firebird.datatype.boolean_literal"},
      {"SELECT CAST('x' AS VARCHAR(10) CHARACTER SET UTF8) FROM RDB$DATABASE",
       "query", "firebird.datatype.cast"},
  };
  for (const auto& test : datatype_cases) {
    if (!ExpectParseCase(test)) return EXIT_FAILURE;
  }

  const ParseCase catalog_overlay_cases[] = {
      {"SELECT * FROM RDB$RELATIONS",
       "catalog_overlay", "firebird.catalog_overlay.rdb_core"},
      {"SELECT * FROM MON$ATTACHMENTS",
       "catalog_overlay", "firebird.catalog_overlay.monitoring"},
      {"SELECT * FROM SEC$USERS",
       "catalog_overlay", "firebird.catalog_overlay.security"},
      {"SELECT * FROM INFORMATION_SCHEMA.TABLES",
       "catalog_overlay", "firebird.catalog_overlay.information_schema"},
      {"WITH q AS (SELECT * FROM RDB$PROCEDURES) SELECT * FROM q",
       "catalog_overlay", "firebird.catalog_overlay.routines_triggers_packages"},
      {"SELECT * FROM RDB$INDICES",
       "catalog_overlay", "firebird.catalog_overlay.constraints_indexes"},
      {"SELECT * FROM RDB$FILTERS",
       "catalog_overlay", "firebird.catalog_overlay.exceptions_collations_charsets"},
  };
  for (const auto& test : catalog_overlay_cases) {
    if (!ExpectParseCase(test)) return EXIT_FAILURE;
    const auto parsed = ParseStatement(test.sql);
    if (!Expect(Contains(parsed.message_vector_json,
                         "FIREBIRD.CATALOG_OVERLAY.PROJECTION"),
                "Firebird catalog overlay diagnostic missing")) {
      return EXIT_FAILURE;
    }
  }
  if (!ExpectRejected("UPDATE RDB$RELATIONS SET RDB$RELATION_NAME = 'X'",
                      "FIREBIRD.CATALOG_OVERLAY.READ_ONLY")) {
    return EXIT_FAILURE;
  }

  const ParseCase dml_cases[] = {
      {"INSERT INTO customer(id, name) VALUES (?, :name)",
       "dml", "firebird.dml.insert"},
      {"INSERT INTO customer(id, name) VALUES (?, :name) RETURNING id",
       "dml", "firebird.dml.insert.returning"},
      {"UPDATE customer SET name = :name WHERE id = ?",
       "dml", "firebird.dml.update"},
      {"UPDATE customer SET name = :name WHERE id = ? RETURNING id",
       "dml", "firebird.dml.update.returning"},
      {"UPDATE OR INSERT INTO customer(id, name) VALUES (1, 'Ada') MATCHING (id)",
       "dml", "firebird.dml.update_or_insert"},
      {"UPDATE OR INSERT INTO customer(id, name) VALUES (1, 'Ada') MATCHING (id) RETURNING id",
       "dml", "firebird.dml.update_or_insert.returning"},
      {"DELETE FROM customer WHERE id = ?",
       "dml", "firebird.dml.delete"},
      {"DELETE FROM customer WHERE id = ? RETURNING id",
       "dml", "firebird.dml.delete.returning"},
      {"UPDATE customer SET name = :name WHERE CURRENT OF c_customer",
       "dml", "firebird.dml.cursor.update_current_of"},
      {"DELETE FROM customer WHERE CURRENT OF c_customer",
       "dml", "firebird.dml.cursor.delete_current_of"},
      {"MERGE INTO customer target USING incoming source ON target.id = source.id "
       "WHEN MATCHED THEN UPDATE SET name = source.name",
       "dml", "firebird.dml.merge"},
      {"MERGE INTO customer target USING incoming source ON target.id = source.id "
       "WHEN MATCHED THEN UPDATE SET name = source.name RETURNING id",
       "dml", "firebird.dml.merge.returning"},
      {"EXECUTE PROCEDURE refresh_customer(:id)",
       "dml", "firebird.dml.execute_procedure"},
      {"CALL refresh_customer(:id)",
       "dml", "firebird.dml.call"},
  };
  for (const auto& test : dml_cases) {
    if (!ExpectParseCase(test)) return EXIT_FAILURE;
  }

  const ParseCase ddl_cases[] = {
      {"CREATE TABLE customer (id INTEGER NOT NULL, name VARCHAR(40))",
       "ddl", "firebird.ddl.create"},
      {"ALTER TABLE customer ADD active BOOLEAN",
       "ddl", "firebird.ddl.alter"},
      {"DROP TABLE customer",
       "ddl", "firebird.ddl.drop"},
      {"RECREATE TABLE customer (id INTEGER)",
       "ddl", "firebird.ddl.recreate"},
      {"CREATE OR ALTER PROCEDURE refresh_customer AS BEGIN END",
       "ddl", "firebird.ddl.create_or_alter.procedure"},
      {"CREATE PROCEDURE refresh_customer AS BEGIN END",
       "ddl", "firebird.ddl.create.procedure"},
      {"RECREATE PROCEDURE refresh_customer AS BEGIN SUSPEND; END",
       "ddl", "firebird.ddl.recreate.procedure"},
      {"ALTER PROCEDURE refresh_customer AS BEGIN END",
       "ddl", "firebird.ddl.alter.procedure"},
      {"DROP PROCEDURE refresh_customer",
       "ddl", "firebird.ddl.drop.procedure"},
      {"CREATE FUNCTION calc_total RETURNS INTEGER AS BEGIN RETURN 1; END",
       "ddl", "firebird.ddl.create.function"},
      {"ALTER FUNCTION calc_total RETURNS INTEGER AS BEGIN RETURN 2; END",
       "ddl", "firebird.ddl.alter.function"},
      {"DROP FUNCTION calc_total",
       "ddl", "firebird.ddl.drop.function"},
      {"CREATE PACKAGE sales_pkg AS BEGIN END",
       "ddl", "firebird.ddl.create.package"},
      {"CREATE PACKAGE BODY sales_pkg AS BEGIN PROCEDURE apply_change AS BEGIN END END",
       "ddl", "firebird.ddl.create.package_body"},
      {"ALTER PACKAGE sales_pkg AS BEGIN END",
       "ddl", "firebird.ddl.alter.package"},
      {"DROP PACKAGE sales_pkg",
       "ddl", "firebird.ddl.drop.package"},
      {"CREATE TRIGGER customer_bi FOR customer BEFORE INSERT AS BEGIN END",
       "ddl", "firebird.ddl.create.trigger"},
      {"ALTER TRIGGER customer_bi INACTIVE",
       "ddl", "firebird.ddl.alter.trigger"},
      {"DROP TRIGGER customer_bi",
       "ddl", "firebird.ddl.drop.trigger"},
      {"CREATE EXCEPTION bad_customer 'bad customer'",
       "ddl", "firebird.ddl.create.exception"},
      {"ALTER EXCEPTION bad_customer 'invalid customer'",
       "ddl", "firebird.ddl.alter.exception"},
      {"DROP EXCEPTION bad_customer",
       "ddl", "firebird.ddl.drop.exception"},
      {"CREATE SEQUENCE customer_seq",
       "ddl", "firebird.ddl.create.sequence"},
      {"ALTER SEQUENCE customer_seq RESTART WITH 1",
       "ddl", "firebird.ddl.alter.sequence"},
      {"DROP SEQUENCE customer_seq",
       "ddl", "firebird.ddl.drop.sequence"},
      {"CREATE ROLE report_role",
       "ddl", "firebird.ddl.create.role"},
      {"ALTER ROLE report_role SET SYSTEM PRIVILEGES TO USER_MANAGEMENT",
       "ddl", "firebird.ddl.alter.role"},
      {"DROP ROLE report_role",
       "ddl", "firebird.ddl.drop.role"},
      {"CREATE USER alice PASSWORD 'secret'",
       "ddl", "firebird.ddl.create.user"},
      {"ALTER USER alice PASSWORD 'changed'",
       "ddl", "firebird.ddl.alter.user"},
      {"DROP USER alice",
       "ddl", "firebird.ddl.drop.user"},
      {"COMMENT ON TABLE customer IS 'compatibility metadata'",
       "ddl", "firebird.ddl.comment"},
      {"GRANT SELECT ON customer TO report_role",
       "ddl", "firebird.ddl.grant"},
      {"REVOKE SELECT ON customer FROM report_role",
       "ddl", "firebird.ddl.revoke"},
  };
  for (const auto& test : ddl_cases) {
    if (!ExpectParseCase(test)) return EXIT_FAILURE;
  }

  const ParseCase psql_cases[] = {
      {"EXECUTE BLOCK AS BEGIN SUSPEND; END",
       "psql", "firebird.psql.execute_block"},
      {"EXECUTE STATEMENT 'select 1 from rdb$database'",
       "psql", "firebird.psql.execute_statement"},
  };
  for (const auto& test : psql_cases) {
    if (!ExpectParseCase(test)) return EXIT_FAILURE;
  }

  const ParseCase non_file_cases[] = {
      {"CREATE DATABASE 'scratchbird-firebird.fdb'",
       "non_file_emulation", "firebird.emulated.database_lifecycle", true},
      {"DROP DATABASE",
       "non_file_emulation", "firebird.emulated.database_lifecycle", true},
      {"ALTER DATABASE ADD DIFFERENCE FILE 'scratchbird-firebird.delta'",
       "non_file_emulation", "firebird.emulated.database_lifecycle", true},
      {"CREATE SHADOW 1 'scratchbird-firebird.shd'",
       "non_file_emulation", "firebird.emulated.shadow_storage", true},
      {"DROP SHADOW 1",
       "non_file_emulation", "firebird.emulated.shadow_storage", true},
      {"CREATE EXTERNAL TABLE ext_log (line VARCHAR(80)) EXTERNAL FILE 'log.txt'",
       "non_file_emulation", "firebird.emulated.external_table_authority", true},
      {"CREATE EXTERNAL FUNCTION legacy_udf RETURNS INTEGER ENTRY_POINT 'fn' MODULE_NAME 'lib'",
       "non_file_emulation", "firebird.emulated.plugin_external_engine", true},
      {"DECLARE EXTERNAL FUNCTION legacy_udf RETURNS INTEGER ENTRY_POINT 'fn' MODULE_NAME 'lib'",
       "non_file_emulation", "firebird.emulated.plugin_external_engine", true},
      {"CREATE FILTER legacy_filter INPUT_TYPE 1 OUTPUT_TYPE 2 ENTRY_POINT 'filter' MODULE_NAME 'lib'",
       "non_file_emulation", "firebird.emulated.plugin_external_engine", true},
      {"CREATE MAPPING trusted_map USING PLUGIN Srp TO USER scratchbird",
       "non_file_emulation", "firebird.emulated.security_projection", true},
      {"CREATE JOURNAL ARCHIVE 'archive-dir'",
       "non_file_emulation", "firebird.emulated.replication_journal", true,
       true},
      {"REPLICATION CONFIG 'replica.conf'",
       "non_file_emulation", "firebird.emulated.replication_journal", true,
       true},
      {"CREATE SHADOW 2 MANUAL RAW DEVICE '/dev/sdb'",
       "non_file_emulation", "firebird.emulated.shadow_raw_storage", true},
      {"BACKUP DATABASE 'scratchbird-firebird.fdb' TO 'scratchbird-firebird.fbk'",
       "non_file_emulation", "firebird.emulated.backup_restore", true},
      {"RESTORE DATABASE 'scratchbird-firebird.fbk' TO 'scratchbird-firebird.fdb'",
       "non_file_emulation", "firebird.emulated.backup_restore", true},
      {"TRACE START SESSION",
       "non_file_emulation", "firebird.emulated.trace_monitoring", true},
      {"SERVICE BACKUP DATABASE 'scratchbird-firebird.fdb'",
       "non_file_emulation", "firebird.emulated.service_api", true},
      {"SERVICE DRIVER CONNECT",
       "non_file_emulation", "firebird.emulated.service_api", true},
      {"SERVICE CONNECTION INFO",
       "non_file_emulation", "firebird.emulated.service_api", true},
      {"SERVICE SOCKET PROTOCOL",
       "non_file_emulation", "firebird.emulated.service_api", true},
      {"FBSVCMGR service_mgr action_backup dbname 'scratchbird-firebird.fdb'",
       "non_file_emulation", "firebird.emulated.reference_native_tool", true},
      {"GSTAT -header scratchbird-firebird.fdb",
       "non_file_emulation", "firebird.emulated.reference_native_tool", true},
      {"GSEC display",
       "non_file_emulation", "firebird.emulated.reference_native_tool", true},
      {"GPRE sample.e",
       "non_file_emulation", "firebird.emulated.reference_native_tool", true},
      {"GSPLIT -join_backup_file split01.fbk split02.fbk",
       "non_file_emulation", "firebird.emulated.reference_native_tool", true},
      {"FB_LOCK_PRINT -d scratchbird-firebird.fdb",
       "non_file_emulation", "firebird.emulated.reference_native_tool", true},
      {"FBGUARD -onetime",
       "non_file_emulation", "firebird.emulated.reference_native_tool", true},
      {"FBTRACEMGR -start -database scratchbird-firebird.fdb",
       "non_file_emulation", "firebird.emulated.reference_native_tool", true},
  };
  for (const auto& test : non_file_cases) {
    if (!ExpectParseCase(test)) return EXIT_FAILURE;
  }

  const ParseCase gbak_logical_stream_cases[] = {
      {"GBAK -backup scratchbird-firebird.fdb stdout",
       "logical_stream_backup_restore",
       "firebird.logical_stream.gbak_backup"},
      {"gbak -b scratchbird-firebird.fdb stdout",
       "logical_stream_backup_restore",
       "firebird.logical_stream.gbak_backup"},
      {"gbak -b scratchbird-firebird.fdb stdout -parallel 2",
       "logical_stream_backup_restore",
       "firebird.logical_stream.gbak_backup"},
      {"GBAK -restore stdin scratchbird-firebird.fdb",
       "logical_stream_backup_restore",
       "firebird.logical_stream.gbak_restore"},
      {"gbak -r stdin scratchbird-firebird.fdb",
       "logical_stream_backup_restore",
       "firebird.logical_stream.gbak_restore"},
      {"gbak -r stdin scratchbird-firebird.fdb -parallel 2",
       "logical_stream_backup_restore",
       "firebird.logical_stream.gbak_restore"},
      {"gbak -create stdin scratchbird-firebird.fdb",
       "logical_stream_backup_restore",
       "firebird.logical_stream.gbak_restore"},
      {"gbak -c stdin scratchbird-firebird.fdb",
       "logical_stream_backup_restore",
       "firebird.logical_stream.gbak_restore"},
      {"gbak -replace stdin scratchbird-firebird.fdb",
       "logical_stream_backup_restore",
       "firebird.logical_stream.gbak_restore"},
  };
  for (const auto& test : gbak_logical_stream_cases) {
    if (!ExpectParseCase(test)) return EXIT_FAILURE;
    if (!ExpectGbakLogicalStream(test.sql, test.operation_family)) {
      return EXIT_FAILURE;
    }
  }

  const std::string_view unsupported_low_level_cases[] = {
      "VALIDATE DATABASE 'scratchbird-firebird.fdb'",
      "GBAK -backup scratchbird-firebird.fdb scratchbird-firebird.fbk",
      "GBAK -backup scratchbird-firebird.fdb scratchbird-firebird.fbk -y stdout",
      "GBAK -restore scratchbird-firebird.fbk scratchbird-firebird.fdb",
      "GBAK -restore -y stdin scratchbird-firebird.fbk scratchbird-firebird.fdb",
      "GBAK -replace scratchbird-firebird.fbk scratchbird-firebird.fdb",
      "GFIX -validate scratchbird-firebird.fdb",
      "NBACKUP DATABASE 'scratchbird-firebird.fdb' LEVEL 0",
  };
  for (const auto sql : unsupported_low_level_cases) {
    if (!ExpectRejected(sql, "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED")) {
      return EXIT_FAILURE;
    }
  }

  const ParseCase isql_cases[] = {
      {"SET LIST ON", "isql_frontend", "firebird.isql.set"},
      {"SHOW TABLES", "isql_frontend", "firebird.isql.show"},
      {"EXTRACT", "isql_frontend", "firebird.isql.extract"},
      {"IN /tmp/input.sql", "isql_frontend", "firebird.isql.input"},
      {"OUT /tmp/output.log", "isql_frontend", "firebird.isql.output"},
      {"INPUT /tmp/input.sql", "isql_frontend", "firebird.isql.input"},
      {"OUTPUT /tmp/output.log", "isql_frontend", "firebird.isql.output"},
      {"CONNECT 'employee' USER 'sysdba' PASSWORD 'masterkey'",
       "isql_frontend", "firebird.isql.connect"},
  };
  for (const auto& test : isql_cases) {
    if (!ExpectParseCase(test)) return EXIT_FAILURE;
  }

  const ParseCase transaction_cases[] = {
      {"SET TRANSACTION READ WRITE WAIT",
       "transaction", "firebird.transaction.set_transaction"},
      {"SAVEPOINT before_customer_update",
       "transaction", "firebird.transaction.savepoint"},
      {"RELEASE SAVEPOINT before_customer_update",
       "transaction", "firebird.transaction.savepoint"},
  };
  for (const auto& test : transaction_cases) {
    if (!ExpectParseCase(test)) return EXIT_FAILURE;
  }

  if (!ExpectCompositeTableKeyEnvelope()) return EXIT_FAILURE;
  if (!ExpectTextResourceCreateEnvelope()) return EXIT_FAILURE;
  if (!ExpectAsciiCharInsertEnvelope()) return EXIT_FAILURE;
  if (!ExpectPreparedDsqlPreflight()) return EXIT_FAILURE;
  if (!ExpectPreparedV2UnknownProjection()) return EXIT_FAILURE;
  if (!ExpectBetweenPredicateEnvelopes()) return EXIT_FAILURE;

  if (!ExpectRejected("INSERT INTO customer(id) VALUES (",
                      "FIREBIRD.PARSE.INVALID_INPUT")) {
    return EXIT_FAILURE;
  }
  if (!ExpectRejected("EXECUTE BLOCK AS BEGIN IF (:id = 1 THEN SUSPEND; END",
                      "FIREBIRD.PARSE.INVALID_INPUT")) {
    return EXIT_FAILURE;
  }

  const auto invalid = ParseStatement("");
  if (!Expect(!invalid.ok, "Empty Firebird input should fail")) return EXIT_FAILURE;
  if (!Expect(Contains(invalid.message_vector_json, "FIREBIRD.PARSE.EMPTY_INPUT"),
              "Empty Firebird diagnostic mismatch")) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
