// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_execution_session.hpp"
#include "firebird_scalar_projection.hpp"
#include "firebird_worker_session.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using scratchbird::parser::firebird::DecodeFirebirdScalarProjectionRows;
using scratchbird::parser::firebird::
    DescribeFirebirdScalarProjectionWireDescriptors;
using scratchbird::parser::firebird::EncodeFirebirdScalarProjectionEnvelope;
using scratchbird::parser::firebird::FirebirdExecutionSession;
using scratchbird::parser::firebird::FirebirdScalarProjectionBindOptions;
using scratchbird::parser::firebird::FirebirdScalarProjectionExpressionKind;
using scratchbird::parser::firebird::FirebirdScalarProjectionOutputKind;
using scratchbird::parser::firebird::FirebirdScalarProjectionRoute;
using scratchbird::parser::firebird::FirebirdScalarProjectionWireRow;
using scratchbird::parser::firebird::ParseFirebirdScalarProjectionRoute;
using scratchbird::parser::firebird::
    ValidateFirebirdScalarProjectionCompletePacket;

bool Contains(std::string_view text, std::string_view expected) {
  return text.find(expected) != std::string_view::npos;
}

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

FirebirdScalarProjectionRoute Parse(std::string_view sql,
                                    std::string charset = "NONE") {
  return ParseFirebirdScalarProjectionRoute(
      sql, FirebirdScalarProjectionBindOptions{std::move(charset)});
}

bool ExpectExactHashBinding() {
  const auto route =
      Parse("SELECT HASH('toto') FROM RDB$DATABASE;");
  bool ok = Expect(route.recognized() && route.items.size() == 1,
                   "canonical Firebird HASH route was not recognized");
  if (!route.recognized() || route.items.size() != 1) return false;

  const auto& item = route.items.front();
  ok = Expect(item.result_name == "HASH" &&
                  item.output_kind ==
                      FirebirdScalarProjectionOutputKind::kInt64 &&
                  !item.nullable && item.scale == 0 && item.subtype == 0 &&
                  item.declared_length == 0,
              "canonical Firebird HASH output metadata drifted") &&
       ok;

  const auto& expression = item.expression;
  ok = Expect(expression.kind ==
                      FirebirdScalarProjectionExpressionKind::kFunction &&
                  expression.type_name == "int64" &&
                  expression.encoded_value.empty() &&
                  expression.function_id == "data.scalar.hash64" &&
                  expression.arguments.size() == 2,
              "Firebird HASH was not bound to the neutral profiled function") &&
       ok;
  if (expression.arguments.size() == 2) {
    const auto& input = expression.arguments[0];
    const auto& profile = expression.arguments[1];
    ok = Expect(input.kind ==
                        FirebirdScalarProjectionExpressionKind::kLiteral &&
                    input.type_name == "character" &&
                    input.encoded_value == "toto" && !input.is_null &&
                    profile.kind ==
                        FirebirdScalarProjectionExpressionKind::kLiteral &&
                    profile.type_name == "text" &&
                    profile.encoded_value == "firebird.weak_hash.v1" &&
                    !profile.is_null,
                "Firebird HASH typed input/profile binding drifted") &&
         ok;
  }

  const auto descriptors =
      DescribeFirebirdScalarProjectionWireDescriptors(route);
  ok = Expect(descriptors.size() == 1 && descriptors[0].name == "HASH" &&
                  descriptors[0].source_name == "c0" &&
                  descriptors[0].relation.empty() &&
                  descriptors[0].owner.empty() &&
                  descriptors[0].sql_type == 580 &&
                  descriptors[0].length == 8 &&
                  descriptors[0].scale == 0 &&
                  descriptors[0].subtype == 0 &&
                  !descriptors[0].nullable,
              "Firebird HASH SQL_INT64 descriptor drifted") &&
       ok;

  const std::string envelope = EncodeFirebirdScalarProjectionEnvelope(route);
  ok = Expect(Contains(envelope,
                       "\"projection_0_function_id\":"
                       "\"data.scalar.hash64\"") &&
                  Contains(envelope,
                           "\"projection_0_function_arg_count\":\"2\"") &&
                  Contains(envelope,
                           "\"projection_0_arg_0_type\":\"character\"") &&
                  Contains(envelope,
                           "\"projection_0_arg_0_value\":\"toto\"") &&
                  Contains(envelope,
                           "\"projection_0_arg_1_type\":\"text\"") &&
                  Contains(envelope,
                           "\"projection_0_arg_1_value\":"
                           "\"firebird.weak_hash.v1\"") &&
                  Contains(envelope,
                           "\"scalar_projection_parser_executes_sql\":"
                           "false") &&
                  Contains(envelope, "\"contains_sql_text\":false") &&
                  !Contains(envelope, "505519") &&
                  !Contains(envelope, "RDB$DATABASE") &&
                  !Contains(envelope, "SELECT "),
              "Firebird HASH envelope evaluated or leaked source SQL") &&
       ok;
  return ok;
}

bool ExpectWorkerPresentationOnlyValidation() {
  const auto route = Parse("SELECT HASH('toto') FROM RDB$DATABASE");
  std::string diagnostic;
  std::vector<FirebirdScalarProjectionWireRow> rows{{{{false, "505519"}}}};
  bool ok = Expect(DecodeFirebirdScalarProjectionRows(route, &rows,
                                                       &diagnostic) &&
                       diagnostic.empty() &&
                       rows[0].cells[0].text == "505519",
                   "worker rejected the neutral engine HASH result carrier");

  // The worker validates the Firebird INT64 carrier only.  It deliberately
  // does not recompute or compare the HASH result; semantic value authority
  // remains in the neutral engine.
  rows[0].cells[0].text = "1";
  diagnostic.clear();
  ok = Expect(DecodeFirebirdScalarProjectionRows(route, &rows, &diagnostic) &&
                  diagnostic.empty() && rows[0].cells[0].text == "1",
              "worker attempted to enforce parser-local HASH semantics") &&
       ok;

  rows[0].cells[0].text = "505519x";
  diagnostic.clear();
  ok = Expect(!DecodeFirebirdScalarProjectionRows(route, &rows, &diagnostic) &&
                  diagnostic == "int64_result_required",
              "worker admitted a malformed HASH INT64 carrier") &&
       ok;

  rows[0].cells[0] = {true, {}};
  diagnostic.clear();
  ok = Expect(!DecodeFirebirdScalarProjectionRows(route, &rows, &diagnostic) &&
                  diagnostic == "unexpected_null_result",
              "worker admitted NULL for non-null canonical HASH") &&
       ok;

  diagnostic.clear();
  ok = Expect(ValidateFirebirdScalarProjectionCompletePacket(
                  route, 1, 1, false, &diagnostic) &&
                  diagnostic.empty(),
              "worker rejected the complete one-row HASH packet") &&
       ok;
  diagnostic.clear();
  ok = Expect(!ValidateFirebirdScalarProjectionCompletePacket(
                  route, 0, 0, false, &diagnostic) &&
                  diagnostic == "exactly_one_complete_result_row_required",
              "worker admitted an empty HASH result packet") &&
       ok;
  return ok;
}

bool ExpectUnsupportedHashShapesFailClosed() {
  const std::vector<std::string_view> unsupported{
      "SELECT HASH() FROM RDB$DATABASE",
      "SELECT HASH(NULL) FROM RDB$DATABASE",
      "SELECT HASH('TOTO') FROM RDB$DATABASE",
      "SELECT HASH('toto', 'firebird.weak_hash.v1') FROM RDB$DATABASE",
      "SELECT HASH('toto' || '') FROM RDB$DATABASE",
      "SELECT HASH(COL) FROM RDB$DATABASE",
      "SELECT \"HASH\"('toto') FROM RDB$DATABASE",
      "SELECT HASH('toto') + 0 FROM RDB$DATABASE",
      "SELECT HASH('toto') FROM OTHER_TABLE",
      "SELECT HASH('toto') FROM RDB$DATABASE WHERE 1 = 1",
  };

  bool ok = true;
  for (const auto sql : unsupported) {
    const auto route = Parse(sql);
    ok = Expect(!route.recognized(),
                std::string("unsupported HASH shape was admitted: ") +
                    std::string(sql)) &&
         ok;
    ok = Expect(EncodeFirebirdScalarProjectionEnvelope(route).empty(),
                "unsupported HASH shape produced executable scalar SBLR") &&
         ok;
  }
  ok = Expect(!Parse("SELECT HASH('toto') FROM RDB$DATABASE", "UTF8")
                       .recognized(),
              "unmeasured UTF8 HASH descriptor shape was admitted") &&
       ok;
  return ok;
}

bool ExpectExecutionSessionRoutingAndRefusal() {
  scratchbird::parser::ipc::ParserClientConfig config;
  FirebirdExecutionSession session(config);
  const auto lowered = session.RunStatement(
      "SELECT HASH('toto') FROM RDB$DATABASE", {}, false, false, 0, false,
      {}, "NONE");
  const auto refused = session.BindAndLowerForPrepare(
      "SELECT HASH('TOTO') FROM RDB$DATABASE", {}, {}, "NONE");

  bool ok = Expect(lowered.accepted &&
                       Contains(lowered.sblr_payload,
                                "\"projection_0_function_id\":"
                                "\"data.scalar.hash64\"") &&
                       Contains(lowered.sblr_payload,
                                "firebird.weak_hash.v1") &&
                       !Contains(lowered.sblr_payload, "505519") &&
                       !Contains(lowered.sblr_payload, "RDB$DATABASE") &&
                       !lowered.parser_executes_sql &&
                       !lowered.cached_storage_authority &&
                       !lowered.cached_finality_authority,
                   "execution session did not route HASH to neutral SBLR");
  ok = Expect(!refused.accepted && refused.sblr_payload.empty() &&
                  refused.server_row_count == 0 &&
                  refused.server_result_payload.empty() &&
                  refused.server_cursor_uuid.empty(),
              "unsupported HASH prepare did not fail closed before results") &&
       ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = ExpectExactHashBinding() && ok;
  ok = ExpectWorkerPresentationOnlyValidation() && ok;
  ok = ExpectUnsupportedHashShapesFailClosed() && ok;
  ok = ExpectExecutionSessionRoutingAndRefusal() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
