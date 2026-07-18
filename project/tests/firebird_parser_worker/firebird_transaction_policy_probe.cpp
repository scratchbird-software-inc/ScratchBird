// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_transaction_policy.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using scratchbird::parser::firebird::EncodeNeutralTransactionSblrFields;
using scratchbird::parser::firebird::FirebirdRecordVersionMode;
using scratchbird::parser::firebird::FirebirdTransactionIsolation;
using scratchbird::parser::firebird::FirebirdTransactionPolicyResult;
using scratchbird::parser::firebird::FirebirdTransactionPolicySource;
using scratchbird::parser::firebird::FirebirdTransactionWaitMode;
using scratchbird::parser::firebird::ParseFirebirdSetTransactionSql;
using scratchbird::parser::firebird::ParseFirebirdTransactionTpb;
using scratchbird::parser::firebird::RenderNeutralTransactionSblrFields;

bool Check(bool condition, std::string_view label) {
  if (condition) return true;
  std::cerr << "FAILED: " << label << '\n';
  return false;
}

FirebirdTransactionPolicyResult ParseTpb(
    std::initializer_list<std::uint8_t> bytes) {
  const std::vector<std::uint8_t> buffer(bytes);
  return ParseFirebirdTransactionTpb(std::span<const std::uint8_t>(buffer));
}

bool HasExactSymbols(const FirebirdTransactionPolicyResult& result,
                     std::initializer_list<std::string_view> expected) {
  if (result.diagnostic.firebird_status_symbols.size() != expected.size()) {
    return false;
  }
  return std::equal(
      result.diagnostic.firebird_status_symbols.begin(),
      result.diagnostic.firebird_status_symbols.end(),
      expected.begin(),
      expected.end(),
      [](const std::string& actual, std::string_view wanted) {
        return actual == wanted;
      });
}

bool HasExactArguments(const FirebirdTransactionPolicyResult& result,
                       std::initializer_list<std::string_view> expected) {
  if (result.diagnostic.arguments.size() != expected.size()) return false;
  return std::equal(result.diagnostic.arguments.begin(),
                    result.diagnostic.arguments.end(),
                    expected.begin(),
                    expected.end(),
                    [](const std::string& actual, std::string_view wanted) {
                      return actual == wanted;
                    });
}

bool CheckTpbFailure(std::initializer_list<std::uint8_t> bytes,
                     std::initializer_list<std::string_view> symbols,
                     std::initializer_list<std::string_view> arguments,
                     std::string_view label) {
  const auto result = ParseTpb(bytes);
  return Check(!result.ok, std::string(label) + " rejected") &&
         Check(HasExactSymbols(result, symbols),
               std::string(label) + " exact symbols") &&
         Check(HasExactArguments(result, arguments),
               std::string(label) + " exact arguments");
}

bool CheckSqlFailure(std::string_view sql,
                     std::int32_t sql_code,
                     std::initializer_list<std::string_view> symbols,
                     std::initializer_list<std::string_view> arguments,
                     std::string_view label) {
  const auto result = ParseFirebirdSetTransactionSql(sql);
  return Check(!result.ok, std::string(label) + " rejected") &&
         Check(result.policy.source == FirebirdTransactionPolicySource::kSql,
               std::string(label) + " retains SQL source") &&
         Check(result.diagnostic.sql_code == sql_code,
               std::string(label) + " exact SQL code") &&
         Check(HasExactSymbols(result, symbols),
               std::string(label) + " exact symbols") &&
         Check(HasExactArguments(result, arguments),
               std::string(label) + " exact arguments");
}

bool CheckDefaultPolicy() {
  const auto empty = ParseTpb({});
  const auto version_only = ParseTpb({3});
  return Check(empty.ok && version_only.ok, "empty and version-only TPB accepted") &&
         Check(empty.policy.isolation == FirebirdTransactionIsolation::kConcurrency,
               "empty TPB defaults to concurrency") &&
         Check(!empty.policy.read_only, "empty TPB defaults read-write") &&
         Check(empty.policy.wait_mode == FirebirdTransactionWaitMode::kWait,
               "empty TPB defaults wait") &&
         Check(!empty.policy.lock_timeout_present,
               "empty TPB has no explicit lock timeout") &&
         Check(version_only.policy.tpb_version == 3,
               "version-only TPB retains v3");
}

bool CheckValidTpbPolicies() {
  bool ok = true;
  const auto snapshot = ParseTpb({1, 8, 6, 2});
  ok = Check(snapshot.ok, "v1 snapshot policy accepted") && ok;
  ok = Check(snapshot.policy.read_only, "v1 read-only mapped") && ok;
  ok = Check(snapshot.policy.isolation ==
                 FirebirdTransactionIsolation::kConcurrency,
             "concurrency mapped") && ok;

  const auto record_version = ParseTpb({3, 15, 17, 9, 7});
  ok = Check(record_version.ok, "v3 record-version policy accepted") && ok;
  ok = Check(record_version.policy.record_version_mode ==
                 FirebirdRecordVersionMode::kRecordVersion,
             "record-version mapped") && ok;
  ok = Check(record_version.policy.wait_mode ==
                 FirebirdTransactionWaitMode::kNoWait,
             "nowait mapped") && ok;

  const auto no_record_version = ParseTpb({3, 18, 15, 6});
  ok = Check(no_record_version.ok,
             "record-version option before read-committed accepted") && ok;
  ok = Check(no_record_version.policy.record_version_mode ==
                 FirebirdRecordVersionMode::kNoRecordVersion,
             "no-record-version mapped") && ok;

  const auto read_consistency = ParseTpb({3, 22, 15, 6});
  ok = Check(read_consistency.ok,
             "read-consistency before read-committed accepted") && ok;
  ok = Check(read_consistency.policy.record_version_mode ==
                 FirebirdRecordVersionMode::kReadConsistency,
             "read-consistency mapped") && ok;

  const auto timeout_one_byte = ParseTpb({3, 21, 1, 10});
  ok = Check(timeout_one_byte.ok &&
                 timeout_one_byte.policy.lock_timeout_seconds == 10,
             "one-byte lock timeout decoded") && ok;

  const auto timeout_two_bytes = ParseTpb({1, 21, 2, 255, 127});
  ok = Check(timeout_two_bytes.ok &&
                 timeout_two_bytes.policy.lock_timeout_seconds == 32767,
             "maximum two-byte lock timeout decoded") && ok;
  return ok;
}

bool CheckTpbDiagnostics() {
  bool ok = true;
  ok = CheckTpbFailure({2},
                       {"isc_bad_tpb_form", "isc_wrotpbver"},
                       {},
                       "wrong version") && ok;
  ok = CheckTpbFailure({3, 255},
                       {"isc_bad_tpb_form"},
                       {},
                       "unknown tag") && ok;
  ok = CheckTpbFailure({3, 2, 1},
                       {"isc_bad_tpb_content",
                        "isc_tpb_multiple_txn_isolation"},
                       {},
                       "multiple isolation") && ok;
  ok = CheckTpbFailure({3, 6, 7},
                       {"isc_bad_tpb_content",
                        "isc_tpb_conflicting_options"},
                       {"isc_tpb_nowait", "isc_tpb_wait"},
                       "wait-nowait conflict") && ok;
  ok = CheckTpbFailure({3, 6, 6},
                       {"isc_bad_tpb_content", "isc_tpb_multiple_spec"},
                       {"isc_tpb_wait"},
                       "duplicate wait") && ok;
  ok = CheckTpbFailure({3, 8, 9},
                       {"isc_bad_tpb_content",
                        "isc_tpb_conflicting_options"},
                       {"isc_tpb_write", "isc_tpb_read"},
                       "read-write conflict") && ok;
  ok = CheckTpbFailure({3, 17},
                       {"isc_bad_tpb_content", "isc_tpb_option_without_rc"},
                       {"isc_tpb_rec_version"},
                       "record-version without read committed") && ok;
  ok = CheckTpbFailure({3, 17, 2},
                       {"isc_bad_tpb_content", "isc_tpb_option_without_rc"},
                       {"isc_tpb_rec_version"},
                       "record-version before concurrency retains diagnostic option") && ok;
  ok = CheckTpbFailure({3, 15, 17, 22},
                       {"isc_bad_tpb_content",
                        "isc_tpb_conflicting_options"},
                       {"isc_tpb_read_consistency", "isc_tpb_rec_version"},
                       "record-version read-consistency conflict") && ok;
  ok = CheckTpbFailure({3, 7, 21, 1, 5},
                       {"isc_bad_tpb_content",
                        "isc_tpb_conflicting_options"},
                       {"isc_tpb_lock_timeout", "isc_tpb_nowait"},
                       "nowait then timeout conflict") && ok;
  ok = CheckTpbFailure({3, 21, 1, 5, 7},
                       {"isc_bad_tpb_content",
                        "isc_tpb_conflicting_options"},
                       {"isc_tpb_nowait", "isc_tpb_lock_timeout"},
                       "timeout then nowait conflict") && ok;
  ok = CheckTpbFailure({3, 21},
                       {"isc_bad_tpb_content", "isc_tpb_missing_len"},
                       {"isc_tpb_lock_timeout"},
                       "timeout missing length") && ok;
  ok = CheckTpbFailure({3, 21, 2},
                       {"isc_bad_tpb_content", "isc_tpb_missing_value"},
                       {"2", "isc_tpb_lock_timeout"},
                       "timeout missing value") && ok;
  ok = CheckTpbFailure({3, 21, 2, 1},
                       {"isc_bad_tpb_content", "isc_tpb_corrupt_len"},
                       {"2", "isc_tpb_lock_timeout"},
                       "timeout corrupt length") && ok;
  ok = CheckTpbFailure({3, 21, 0, 6},
                       {"isc_bad_tpb_content", "isc_tpb_null_len"},
                       {"isc_tpb_lock_timeout"},
                       "timeout null length") && ok;
  ok = CheckTpbFailure({3, 21, 5, 1, 0, 0, 0, 0},
                       {"isc_bad_tpb_content", "isc_tpb_overflow_len"},
                       {"5", "isc_tpb_lock_timeout"},
                       "timeout overflow length") && ok;
  ok = CheckTpbFailure({3, 21, 1, 0},
                       {"isc_bad_tpb_content", "isc_tpb_invalid_value"},
                       {"0", "isc_tpb_lock_timeout"},
                       "timeout zero") && ok;
  ok = CheckTpbFailure({3, 21, 1, 128},
                       {"isc_bad_tpb_content", "isc_tpb_invalid_value"},
                       {"-128", "isc_tpb_lock_timeout"},
                       "timeout signed negative") && ok;
  ok = CheckTpbFailure({3, 21, 3, 0, 0, 1},
                       {"isc_bad_tpb_content", "isc_tpb_invalid_value"},
                       {"65536", "isc_tpb_lock_timeout"},
                       "timeout above maximum") && ok;

  const auto reservation = ParseTpb({3, 10, 1, 'T'});
  ok = Check(!reservation.ok &&
                 reservation.diagnostic.scratchbird_code ==
                     "SB_DIAG_FIREBIRD_TRANSACTION_POLICY_UNSUPPORTED" &&
                 HasExactArguments(reservation, {"isc_tpb_lock_read"}),
             "relation reservation fails closed") && ok;
  const auto snapshot_number = ParseTpb({3, 23, 8, 1, 0, 0, 0, 0, 0, 0, 0});
  ok = Check(!snapshot_number.ok &&
                 snapshot_number.diagnostic.scratchbird_code ==
                     "SB_DIAG_FIREBIRD_TRANSACTION_POLICY_UNSUPPORTED" &&
                 HasExactArguments(snapshot_number,
                                   {"isc_tpb_at_snapshot_number"}),
             "value-bearing snapshot tag fails closed") && ok;
  return ok;
}

bool CheckValidSqlPolicies() {
  bool ok = true;
  const auto defaults = ParseFirebirdSetTransactionSql("SET TRANSACTION");
  ok = Check(defaults.ok && defaults.canonical_tpb.empty(),
             "option-free SET TRANSACTION emits empty TPB") && ok;
  ok = Check(defaults.policy.source == FirebirdTransactionPolicySource::kSql,
             "SQL policy source retained") && ok;

  const auto defaults_with_terminator =
      ParseFirebirdSetTransactionSql("SET TRANSACTION;");
  ok = Check(defaults_with_terminator.ok &&
                 defaults_with_terminator.canonical_tpb.empty(),
             "one trailing DSQL semicolon accepted") && ok;

  const auto snapshot = ParseFirebirdSetTransactionSql(
      "set /* family-local */ transaction read only wait isolation level snapshot");
  ok = Check(snapshot.ok, "snapshot SQL accepted") && ok;
  ok = Check(snapshot.canonical_tpb ==
                 std::vector<std::uint8_t>({1, 8, 6, 2}),
             "snapshot SQL uses fixed TPB order") && ok;

  const auto snapshot_with_terminator = ParseFirebirdSetTransactionSql(
      "SET TRANSACTION READ ONLY WAIT SNAPSHOT;");
  ok = Check(snapshot_with_terminator.ok &&
                 snapshot_with_terminator.canonical_tpb ==
                     std::vector<std::uint8_t>({1, 8, 6, 2}),
             "option-bearing SQL accepts one trailing semicolon") && ok;

  const auto table_stability = ParseFirebirdSetTransactionSql(
      "SET TRANSACTION NO WAIT SNAPSHOT TABLE STABILITY READ WRITE");
  ok = Check(table_stability.ok &&
                 table_stability.policy.isolation ==
                     FirebirdTransactionIsolation::kConsistency,
             "snapshot table stability SQL accepted") && ok;
  ok = Check(table_stability.canonical_tpb ==
                 std::vector<std::uint8_t>({1, 9, 7, 1}),
             "table stability canonical order") && ok;

  const auto record_version = ParseFirebirdSetTransactionSql(
      "SET TRANSACTION READ COMMITTED VERSION LOCK TIMEOUT 10");
  ok = Check(record_version.ok &&
                 record_version.policy.record_version_mode ==
                     FirebirdRecordVersionMode::kRecordVersion,
             "SQL record-version and timeout accepted") && ok;
  ok = Check(record_version.canonical_tpb ==
                 std::vector<std::uint8_t>({1, 15, 17, 21, 2, 10, 0}),
             "SQL timeout encoded little-endian length two") && ok;

  const auto hexadecimal_timeout = ParseFirebirdSetTransactionSql(
      "SET TRANSACTION LOCK TIMEOUT 0xA");
  ok = Check(hexadecimal_timeout.ok &&
                 hexadecimal_timeout.policy.lock_timeout_seconds == 10 &&
                 hexadecimal_timeout.canonical_tpb ==
                     std::vector<std::uint8_t>({1, 21, 2, 10, 0}),
             "Firebird NUMBER32BIT hexadecimal timeout accepted") && ok;

  const auto no_record_version = ParseFirebirdSetTransactionSql(
      "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED NO VERSION");
  ok = Check(no_record_version.ok &&
                 no_record_version.canonical_tpb ==
                     std::vector<std::uint8_t>({1, 15, 18}),
             "READ UNCOMMITTED maps through read-committed TPB") && ok;

  const auto read_consistency = ParseFirebirdSetTransactionSql(
      "SET TRANSACTION READ COMMITTED READ CONSISTENCY");
  ok = Check(read_consistency.ok &&
                 read_consistency.canonical_tpb ==
                     std::vector<std::uint8_t>({1, 15, 22}),
             "SQL read consistency uses canonical TPB version1") && ok;

  const auto no_wait_after_isolation = ParseFirebirdSetTransactionSql(
      "SET TRANSACTION READ COMMITTED NO WAIT");
  ok = Check(no_wait_after_isolation.ok &&
                 no_wait_after_isolation.canonical_tpb ==
                     std::vector<std::uint8_t>({1, 7, 15, 18}),
             "NO WAIT after default version-mode begins next option") && ok;

  const auto access_after_isolation = ParseFirebirdSetTransactionSql(
      "SET TRANSACTION ISOLATION LEVEL READ COMMITTED READ ONLY");
  ok = Check(access_after_isolation.ok &&
                 access_after_isolation.canonical_tpb ==
                     std::vector<std::uint8_t>({1, 8, 15, 18}),
             "READ ONLY after isolation begins next option") && ok;
  return ok;
}

bool CheckSqlDiagnostics() {
  bool ok = true;
  ok = CheckSqlFailure(
           "START TRANSACTION READ ONLY",
           -104,
           {"isc_sqlerr", "isc_dsql_token_unk_err", "isc_random"},
           {"1", "1", "START"},
           "START TRANSACTION exact rejection") && ok;
  ok = CheckSqlFailure(
           "\rSTART TRANSACTION",
           -104,
           {"isc_sqlerr", "isc_dsql_token_unk_err", "isc_random"},
           {"2", "1", "START"},
           "CR advances START diagnostic line") && ok;
  ok = CheckSqlFailure(
           "\r\nSTART TRANSACTION",
           -104,
           {"isc_sqlerr", "isc_dsql_token_unk_err", "isc_random"},
           {"2", "1", "START"},
           "CRLF advances START diagnostic once") && ok;
  ok = CheckSqlFailure("SET TRANSACTION READ ONLY READ WRITE",
                       -637,
                       {"isc_sqlerr", "isc_dsql_duplicate_spec"},
                       {"READ {ONLY | WRITE}"},
                       "duplicate access") && ok;
  ok = CheckSqlFailure("SET TRANSACTION WAIT NO WAIT",
                       -637,
                       {"isc_sqlerr", "isc_dsql_duplicate_spec"},
                       {"[NO] WAIT"},
                       "duplicate wait class") && ok;
  ok = CheckSqlFailure("SET TRANSACTION SNAPSHOT READ COMMITTED",
                       -637,
                       {"isc_sqlerr", "isc_dsql_duplicate_spec"},
                       {"ISOLATION LEVEL"},
                       "duplicate isolation") && ok;
  ok = CheckSqlFailure("SET TRANSACTION LOCK TIMEOUT 1 LOCK TIMEOUT 2",
                       -637,
                       {"isc_sqlerr", "isc_dsql_duplicate_spec"},
                       {"LOCK TIMEOUT"},
                       "duplicate lock timeout") && ok;
  ok = CheckSqlFailure(
           "SET TRANSACTION NO WAIT LOCK TIMEOUT 5",
           0,
           {"isc_bad_tpb_content", "isc_tpb_conflicting_options"},
           {"isc_tpb_lock_timeout", "isc_tpb_nowait"},
           "SQL nowait timeout runtime conflict") && ok;
  ok = CheckSqlFailure(
           "SET TRANSACTION LOCK TIMEOUT 5 NO WAIT",
           0,
           {"isc_bad_tpb_content", "isc_tpb_conflicting_options"},
           {"isc_tpb_lock_timeout", "isc_tpb_nowait"},
           "SQL fixed-order timeout nowait conflict") && ok;
  ok = CheckSqlFailure("SET TRANSACTION LOCK TIMEOUT 32768",
                       -842,
                       {"isc_sqlerr", "isc_expec_short",
                        "isc_dsql_line_col_error"},
                       {"1", "30"},
                       "SQL lock timeout short overflow") && ok;
  ok = CheckSqlFailure(
           "SET TRANSACTION LOCK TIMEOUT 2147483648",
           -104,
           {"isc_sqlerr", "isc_dsql_token_unk_err", "isc_random"},
           {"1", "30", "2147483648"},
           "NUMBER64BIT timeout is a grammar token error") && ok;
  ok = CheckSqlFailure("SET TRANSACTION LOCK TIMEOUT 0",
                       0,
                       {"isc_bad_tpb_content", "isc_tpb_invalid_value"},
                       {"0", "isc_tpb_lock_timeout"},
                       "SQL zero timeout rejected by TPB runtime") && ok;
  ok = CheckSqlFailure("SET TRANSACTION READ",
                       -104,
                       {"isc_sqlerr", "isc_command_end_err2"},
                       {"1", "21"},
                       "SQL unexpected end") && ok;
  ok = CheckSqlFailure("SET TRANSACTION;;",
                       -104,
                       {"isc_sqlerr", "isc_dsql_token_unk_err", "isc_random"},
                       {"1", "17", ";"},
                       "second trailing semicolon rejected") && ok;
  ok = CheckSqlFailure(
           "SET TRANSACTION NO AUTO COMMIT",
           -104,
           {"isc_sqlerr", "isc_dsql_token_unk_err", "isc_random"},
           {"1", "25", "COMMIT"},
           "malformed NO AUTO clause is syntax, not unsupported") && ok;

  const auto reserving =
      ParseFirebirdSetTransactionSql("SET TRANSACTION RESERVING T FOR READ");
  ok = Check(!reserving.ok &&
                 reserving.diagnostic.scratchbird_code ==
                     "SB_DIAG_FIREBIRD_TRANSACTION_POLICY_UNSUPPORTED" &&
                 HasExactArguments(reserving, {"RESERVING"}),
             "SQL reservation fails closed") && ok;
  const auto snapshot_number = ParseFirebirdSetTransactionSql(
      "SET TRANSACTION SNAPSHOT AT NUMBER 1");
  ok = Check(!snapshot_number.ok &&
                 snapshot_number.diagnostic.scratchbird_code ==
                     "SB_DIAG_FIREBIRD_TRANSACTION_POLICY_UNSUPPORTED" &&
                 HasExactArguments(snapshot_number, {"SNAPSHOT AT NUMBER"}),
             "SQL snapshot number fails closed") && ok;
  const auto no_auto_undo =
      ParseFirebirdSetTransactionSql("SET TRANSACTION NO AUTO UNDO");
  ok = Check(!no_auto_undo.ok &&
                 no_auto_undo.policy.source ==
                     FirebirdTransactionPolicySource::kSql &&
                 no_auto_undo.diagnostic.scratchbird_code ==
                     "SB_DIAG_FIREBIRD_TRANSACTION_POLICY_UNSUPPORTED" &&
                 HasExactArguments(no_auto_undo, {"NO AUTO UNDO"}),
             "exact NO AUTO UNDO clause fails closed as unsupported") && ok;
  return ok;
}

bool CheckNeutralRendering() {
  bool ok = true;
  const auto parsed = ParseFirebirdSetTransactionSql(
      "SET TRANSACTION READ ONLY READ COMMITTED READ CONSISTENCY "
      "LOCK TIMEOUT 12");
  ok = Check(parsed.ok, "render source policy accepted") && ok;
  const auto fields = RenderNeutralTransactionSblrFields(parsed.policy);
  ok = Check(fields.size() == 4, "four neutral fields rendered") && ok;
  for (const auto& field : fields) {
    ok = Check(field.name.starts_with("transaction_"),
               "renderer emits transaction namespace only") && ok;
  }
  const std::string encoded = EncodeNeutralTransactionSblrFields(parsed.policy);
  ok = Check(encoded ==
                 "transaction_isolation_level=read_consistency\n"
                 "transaction_read_only=true\n"
                 "transaction_wait_mode=wait\n"
                 "transaction_lock_timeout_ms=12000\n",
             "neutral renderer exact payload") && ok;
  ok = Check(encoded.find("firebird") == std::string::npos &&
                 encoded.find("tpb") == std::string::npos &&
                 encoded.find("transaction_uuid") == std::string::npos &&
                 encoded.find("commit") == std::string::npos &&
                 encoded.find("rollback") == std::string::npos,
             "neutral renderer carries no compatibility or authority fields") && ok;
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok = CheckDefaultPolicy() && ok;
  ok = CheckValidTpbPolicies() && ok;
  ok = CheckTpbDiagnostics() && ok;
  ok = CheckValidSqlPolicies() && ok;
  ok = CheckSqlDiagnostics() && ok;
  ok = CheckNeutralRendering() && ok;
  if (ok) {
    std::cout << "firebird_transaction_policy_probe=passed\n";
  }
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
