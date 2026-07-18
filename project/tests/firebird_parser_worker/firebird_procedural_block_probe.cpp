// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_execution_session.hpp"
#include "firebird_procedural_block.hpp"

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

}  // namespace

int main() {
  bool ok = true;
  const firebird::FirebirdExecuteBlockBindingContext win1252_attachment{
      "UTF8", "WIN1252"};

  const auto core3047 = firebird::ParseFirebirdBoundedExecuteBlockRoute(
      R"SQL(
        execute block returns (c varchar(10) collate win_ptbr) as
        begin
        end;
      )SQL",
      win1252_attachment);
  ok = Require(
           core3047.kind ==
                   firebird::FirebirdBoundedExecuteBlockKind::kEmptyResult &&
               core3047.slot_type ==
                   firebird::FirebirdProceduralSlotType::kCharacter &&
               core3047.character_length == 10 && core3047.nullable &&
               core3047.bound_character_set == "WIN1252" &&
               core3047.bound_collation == "WIN_PTBR" &&
               core3047.collation_bound_from_attachment,
           "CORE-3047 did not bind the unqualified collation to the attachment charset") &&
       ok;
  const auto wrong_attachment =
      firebird::ParseFirebirdBoundedExecuteBlockRoute(
          "execute block returns (c varchar(10) collate win_ptbr) as begin end",
          {"WIN1252", "UTF8"});
  ok = Require(
           !wrong_attachment.recognized(),
           "CORE-3047 incorrectly used the database default charset instead of the attachment charset") &&
       ok;

  const auto core4184 = firebird::ParseFirebirdBoundedExecuteBlockRoute(
      "execute block returns (id integer not null) as begin end;",
      {"NONE", "NONE"});
  ok = Require(
           core4184.kind ==
                   firebird::FirebirdBoundedExecuteBlockKind::kEmptyResult &&
               core4184.slot_type ==
                   firebird::FirebirdProceduralSlotType::kInt32 &&
               !core4184.nullable && core4184.has_output(),
           "CORE-4184 empty NOT NULL output was not bound as a zero-yield result slot") &&
       ok;

  const auto core6033 = firebird::ParseFirebirdBoundedExecuteBlockRoute(
      R"SQL(
        execute block as
          declare c varchar(100);
        begin
          c = substring(current_timestamp from 1);
        end;
      )SQL",
      {"NONE", "NONE"});
  ok = Require(
           core6033.kind == firebird::FirebirdBoundedExecuteBlockKind::
                                kTimestampSubstringAssignment &&
               core6033.has_local() && core6033.character_length == 100,
           "CORE-6033 timestamp substring assignment was not bound exactly") &&
       ok;

  const std::string core3047_envelope =
      firebird::EncodeFirebirdBoundedExecuteBlockEnvelope(core3047);
  ok = Require(
           Contains(core3047_envelope,
                    "\"operation_id\":\"transaction.execute_block\"") &&
               Contains(core3047_envelope,
                        "\"procedural_ir_contract\":\"sblr.procedural.block.v1\"") &&
               Contains(core3047_envelope,
                        "\"procedural_slot_0_kind\":\"result\"") &&
               Contains(core3047_envelope,
                        "\"procedural_slot_0_character_length\":\"10\"") &&
               Contains(core3047_envelope,
                        "\"procedural_yield_count\":\"0\"") &&
               Contains(core3047_envelope,
                        "\"contains_sql_text\":false") &&
               !Contains(core3047_envelope, "WIN1252") &&
               !Contains(core3047_envelope, "WIN_PTBR") &&
               !Contains(core3047_envelope, "EXECUTE BLOCK") &&
               !Contains(core3047_envelope, "\"c\"") &&
               !Contains(core3047_envelope, "source_sql") &&
               !Contains(core3047_envelope, "raw_sql"),
           "CORE-3047 lowering leaked Firebird presentation or source SQL into neutral SBLR") &&
       ok;

  const std::string core4184_envelope =
      firebird::EncodeFirebirdBoundedExecuteBlockEnvelope(core4184);
  ok = Require(
           Contains(core4184_envelope,
                    "\"procedural_slot_0_type\":\"int32\"") &&
               Contains(core4184_envelope,
                        "\"procedural_slot_0_nullable\":\"false\"") &&
               Contains(core4184_envelope,
                        "\"procedural_instruction_count\":\"0\"") &&
               !Contains(core4184_envelope, "ID"),
           "CORE-4184 lowering did not preserve zero-yield NOT NULL semantics") &&
       ok;

  const std::string core6033_envelope =
      firebird::EncodeFirebirdBoundedExecuteBlockEnvelope(core6033);
  ok = Require(
           Contains(core6033_envelope,
                    "\"procedural_slot_0_kind\":\"local\"") &&
               Contains(core6033_envelope,
                        "\"procedural_instruction_0_expression_kind\":\"substring\"") &&
               Contains(core6033_envelope,
                        "\"procedural_instruction_0_source_id\":\"ctx_current_timestamp\"") &&
               Contains(core6033_envelope,
                        "\"procedural_instruction_0_start_value\":\"1\"") &&
               Contains(core6033_envelope,
                        "\"procedural_instruction_0_length_kind\":\"to_end\"") &&
               !Contains(core6033_envelope, "CURRENT_TIMESTAMP") &&
               !Contains(core6033_envelope, "SUBSTRING"),
           "CORE-6033 did not lower to the neutral context/cast/substring instruction") &&
       ok;

  for (const std::string_view unsupported : {
           "execute block as begin suspend; end",
           "execute block returns (id integer not null) as begin suspend; end",
           "execute block as declare c varchar(100); begin c = substring(current_date from 1); end",
           "execute block as declare c varchar(100); begin c = substring(current_timestamp from 2); end",
           "execute block as declare c varchar(100); begin c = substring(current_timestamp from 1); c = 'x'; end"}) {
    ok = Require(
             !firebird::ParseFirebirdBoundedExecuteBlockRoute(
                  unsupported, {"NONE", "NONE"})
                  .recognized(),
             "out-of-slice EXECUTE BLOCK shape was promoted") &&
         ok;
  }

  ipc::ParserClientConfig config;
  firebird::FirebirdExecutionSession parse_only(config);
  const auto lowered = parse_only.RunStatement(
      "execute block returns (c varchar(10) collate win_ptbr) as begin end",
      {}, false, false, 0, false, "UTF8", "WIN1252");
  ok = Require(
           lowered.accepted && lowered.procedural_block_route.recognized() &&
               lowered.sblr_payload == core3047_envelope,
           "Firebird execution session did not select the same-family procedural lowerer") &&
       ok;

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
