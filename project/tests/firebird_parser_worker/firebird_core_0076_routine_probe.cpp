// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_execution_session.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace firebird = scratchbird::parser::firebird;
namespace ipc = scratchbird::parser::ipc;

bool Require(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool Contains(std::string_view text, std::string_view value) {
  return text.find(value) != std::string_view::npos;
}

} // namespace

int main() {
  constexpr std::string_view kSchemaUuid =
      "11111111-1111-7111-8111-111111111111";
  constexpr std::string_view kRelationUuid =
      "22222222-2222-7222-8222-222222222222";
  constexpr std::string_view kColumnUuid =
      "33333333-3333-7333-8333-333333333333";
  constexpr std::string_view kProcedureUuid =
      "44444444-4444-7444-8444-444444444444";

  bool ok = true;
  ipc::SbpsClient capability_client("core-0076-capability-probe");
  const auto capability_hello =
      capability_client.PreparedMetadataTransferV1HelloPayloadForTest();
  constexpr std::uint8_t kRequiredPreparedTransferCapabilities =
      0x01u | 0x02u | 0x04u;
  ok = Require(
           capability_hello.size() >= 32 &&
               (capability_hello[capability_hello.size() - 32] &
                kRequiredPreparedTransferCapabilities) ==
                   kRequiredPreparedTransferCapabilities,
           "SBPS HELLO did not request baseline, transaction routing, and prepared metadata transfer") &&
       ok;
  const auto metadata = firebird::ParseFirebirdBoundedProcedureRoute(
      "create or alter procedure test_del as begin end;");
  ok = Require(
           metadata.kind == firebird::FirebirdBoundedProcedureRouteKind::
                                kCreateOrAlterMetadataOnly &&
               metadata.procedure_name == "TEST_DEL",
           "CORE-0076 metadata-only CREATE OR ALTER was not recognized") &&
       ok;
  const std::string metadata_envelope =
      firebird::EncodeFirebirdBoundedProcedureEnvelope(
          metadata, kSchemaUuid, {}, {}, {});
  ok = Require(
           Contains(metadata_envelope, "\"operation_id\":\"ddl.create_procedure\"") &&
               Contains(metadata_envelope,
                        "\"executable_descriptor_kind\":\"create_or_alter_procedure\"") &&
               Contains(metadata_envelope, "\"executor\":\"metadata_only\"") &&
               !Contains(metadata_envelope, "target_object_uuid") &&
               !Contains(metadata_envelope, "procedure_object_uuid") &&
               !Contains(metadata_envelope, "create or alter procedure"),
           "metadata-only route did not preserve engine-owned create/alter identity") &&
       ok;

  const auto program = firebird::ParseFirebirdBoundedProcedureRoute(R"SQL(
    create or alter procedure test_del (l integer, r integer)
      returns (rc integer) as
    begin
      delete from test where a between :l and :r;
      rc = row_count;
      suspend;
    end;
  )SQL");
  ok = Require(
           program.kind == firebird::FirebirdBoundedProcedureRouteKind::
                               kCreateOrAlterDeleteColumnRangeCount &&
               program.parameter_names.size() == 2 &&
               program.parameter_names[0] == "L" &&
               program.parameter_names[1] == "R" &&
               program.return_name == "RC" &&
               program.relation_name == "TEST" &&
               program.column_name == "A",
           "CORE-0076 delete/range/ROW_COUNT program was not recognized exactly") &&
       ok;
  const std::string program_envelope =
      firebird::EncodeFirebirdBoundedProcedureEnvelope(
          program, kSchemaUuid, kRelationUuid, kColumnUuid, {});
  const std::string expected_descriptor =
      "engine.routine.delete_column_range_count.v1|" +
      std::string(kRelationUuid) + "|" + std::string(kColumnUuid) +
      "|0|1|2|2";
  ok = Require(
           Contains(program_envelope, expected_descriptor) &&
               Contains(program_envelope,
                        "\"routine_parameter_count\":\"2\"") &&
               Contains(program_envelope,
                        "\"routine_return_0_name\":\"RC\"") &&
               Contains(program_envelope, "\"contains_sql_text\":false") &&
               !Contains(program_envelope, "DELETE FROM") &&
               !Contains(program_envelope, "ROW_COUNT") &&
               !Contains(program_envelope, "target_object_uuid") &&
               !Contains(program_envelope, "procedure_object_uuid"),
           "CORE-0076 program envelope lost UUID-only engine routine authority") &&
       ok;

  const auto invocation = firebird::ParseFirebirdBoundedProcedureRoute(
      "execute procedure test_del (4, 7);");
  ok = Require(
           invocation.kind == firebird::FirebirdBoundedProcedureRouteKind::
                                  kInvokeLiteralIntegerPair &&
               invocation.literal_arguments.size() == 2 &&
               invocation.literal_arguments[0] == 4 &&
               invocation.literal_arguments[1] == 7,
           "CORE-0076 literal invocation was not recognized") &&
       ok;
  const std::string invoke_envelope =
      firebird::EncodeFirebirdBoundedProcedureEnvelope(
          invocation, {}, {}, {}, kProcedureUuid);
  ok = Require(
           Contains(invoke_envelope,
                    "\"operation_id\":\"routine.procedure_invoke\"") &&
               Contains(invoke_envelope,
                        "\"target_object_uuid\":\"" +
                            std::string(kProcedureUuid) + "\"") &&
               Contains(invoke_envelope,
                        "\"routine_argument_0_value\":\"4\"") &&
               Contains(invoke_envelope,
                        "\"routine_argument_1_value\":\"7\"") &&
               !Contains(invoke_envelope, "statement_metadata_snapshot") &&
               !Contains(invoke_envelope, "visible_through") &&
               !Contains(invoke_envelope, "executable_generation") &&
               !Contains(invoke_envelope, "TEST_DEL") &&
               !Contains(invoke_envelope, "EXECUTE PROCEDURE"),
           "CORE-0076 invocation envelope did not use exact procedure UUID authority") &&
       ok;

  for (const std::string_view unsupported : {
           "create procedure test_del as begin end;",
           "create or alter procedure test_del(l int, r integer) returns (rc integer) as begin delete from test where a between :l and :r; rc=row_count; suspend; end;",
           "create or alter procedure test_del(l integer, r integer) returns (rc integer) as begin delete from test where a between :r and :l; rc=row_count; suspend; end;",
           "create or alter procedure test_del(l integer, r integer) returns (rc integer) as begin delete from test where a between :l and :r; rc=row_count; rc=0; suspend; end;",
           "execute procedure test_del(:l, :r);",
           "call test_del(4, 7);"}) {
    ok = Require(!firebird::ParseFirebirdBoundedProcedureRoute(unsupported)
                      .recognized(),
                 "out-of-slice procedure shape was promoted") &&
         ok;
  }

  ipc::ParserClientConfig config;
  firebird::FirebirdExecutionSession parse_only(config);
  const auto text_blob = parse_only.RunStatement(
      "CREATE TABLE T_LOB (TXT BLOB SUB_TYPE TEXT CHARACTER SET WIN1250 "
      "COLLATE WIN_CZ, BIN_DATA BLOB, TXT_NUM BLOB SUB_TYPE 1)",
      {}, false);
  ok = Require(
           text_blob.accepted &&
               Contains(text_blob.sblr_payload,
                        "\"column_0_type\":\"BLOB SUB_TYPE TEXT\"") &&
               Contains(text_blob.sblr_payload,
                        "\"column_0_descriptor\":\"type=BLOB;nullable=true;text_resource_storage=large_object\"") &&
               Contains(text_blob.sblr_payload,
                        "\"column_1_descriptor\":\"type=BLOB;nullable=true\"") &&
               Contains(text_blob.sblr_payload,
                        "\"column_2_descriptor\":\"type=BLOB;nullable=true;text_resource_storage=large_object\"") &&
               !Contains(text_blob.sblr_payload,
                         "type=BLOB;nullable=true;character_length=0"),
           "Firebird text-BLOB descriptor marker/canonical base type was not exact") &&
       ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
