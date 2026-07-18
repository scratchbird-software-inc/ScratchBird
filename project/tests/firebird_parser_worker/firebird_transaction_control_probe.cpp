// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using scratchbird::parser::firebird::ClassifyFirebirdTransactionControl;
using scratchbird::parser::firebird::FirebirdTransactionControlKind;

bool ExpectControl(std::string_view sql,
                   FirebirdTransactionControlKind expected_kind,
                   bool expected_retaining) {
  const auto actual = ClassifyFirebirdTransactionControl(sql);
  if (actual.kind == expected_kind && actual.retaining == expected_retaining) {
    return true;
  }
  std::cerr << "transaction control mismatch sql=" << sql
            << " actual_kind=" << static_cast<int>(actual.kind)
            << " expected_kind=" << static_cast<int>(expected_kind)
            << " actual_retaining=" << actual.retaining
            << " expected_retaining=" << expected_retaining << '\n';
  return false;
}

} // namespace

int main() {
  using Kind = FirebirdTransactionControlKind;
  bool ok = true;
  ok = ExpectControl("SET TRANSACTION READ ONLY", Kind::kBegin, false) && ok;
  ok = ExpectControl("COMMIT", Kind::kCommit, false) && ok;
  ok = ExpectControl("COMMIT WORK", Kind::kCommit, false) && ok;
  ok = ExpectControl("COMMIT WORK RETAINING SNAPSHOT", Kind::kCommit, true) && ok;
  ok = ExpectControl("COMMIT /* RETAINING */", Kind::kCommit, false) && ok;
  ok = ExpectControl("COMMITMENT", Kind::kNone, false) && ok;
  ok = ExpectControl("COMMIT; ROLLBACK", Kind::kNone, false) && ok;
  ok = ExpectControl("ROLLBACK", Kind::kRollback, false) && ok;
  ok = ExpectControl("ROLLBACK WORK RETAINING", Kind::kRollback, true) && ok;
  ok = ExpectControl("ROLLBACKER", Kind::kNone, false) && ok;
  ok = ExpectControl("ROLLBACK\nTO SAVEPOINT S1",
                     Kind::kRollbackToSavepoint, false) && ok;
  ok = ExpectControl("ROLLBACK /* split */ WORK\nTO \"S 1\"",
                     Kind::kRollbackToSavepoint, false) && ok;
  ok = ExpectControl("ROLLBACK TRANSACTION TO SAVEPOINT S1",
                     Kind::kRollbackToSavepoint, false) && ok;
  ok = ExpectControl("ROLLBACK TO", Kind::kRollbackToSavepoint, false) && ok;
  ok = ExpectControl("SAVEPOINT S1", Kind::kSavepoint, false) && ok;
  ok = ExpectControl("RELEASE SAVEPOINT S1", Kind::kReleaseSavepoint, false) && ok;
  ok = ExpectControl("COMMIT /* unterminated", Kind::kNone, false) && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
