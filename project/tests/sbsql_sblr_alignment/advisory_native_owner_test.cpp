// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/functions/families/data_scalar_functions.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace fn = scratchbird::engine::functions;
namespace s = scratchbird::engine::sblr;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  s::SblrExecutionContext a;
  a.session_uuid = scratchbird::tests::FixtureUuid(1064, 1);
  a.transaction_uuid = scratchbird::tests::FixtureUuid(1064, 2);
  a.transaction_context_present = true;
  a.session_runtime_state = std::make_shared<s::SblrSessionRuntimeState>();
  auto other = a; other.session_uuid = scratchbird::tests::FixtureUuid(1064, 3);
  bool accepted = false;
  Check(fn::AcquireSessionAdvisoryLock(a, "lock", 17, &accepted) && accepted);
  Check(fn::AcquireSessionAdvisoryLock(a, "lock", 17, &accepted) && accepted);
  Check(fn::AcquireSessionAdvisoryLock(other, "lock", 17, &accepted) && !accepted);
  const auto& blocked = a.session_runtime_state->advisory_lock_evidence.back();
  Check(blocked.owner.kind == s::SblrAdvisoryLockOwnerKind::session &&
        blocked.owner.uuid == a.session_uuid && blocked.owner.numeric_reference == 0 &&
        blocked.action == "other_owner" && blocked.key == 17);
  Check(fn::ReleaseSessionAdvisoryLock(other, "unlock", 17, &accepted) && !accepted);
  Check(fn::ReleaseSessionAdvisoryLock(a, "unlock", 17, &accepted) && accepted);
  Check(a.session_runtime_state->advisory_lock_evidence.back().remaining_acquisitions == 1);
  Check(fn::ReleaseSessionAdvisoryLock(a, "unlock", 17, &accepted) && accepted);
  Check(a.session_runtime_state->advisory_lock_entries.empty());
  Check(fn::AcquireTransactionAdvisoryLock(a, "xact_lock", 18, &accepted) && accepted);
  const auto& txn = a.session_runtime_state->transaction_advisory_lock_evidence.back();
  Check(txn.owner.kind == s::SblrAdvisoryLockOwnerKind::transaction && txn.owner.uuid == a.transaction_uuid);
  other.transaction_uuid = scratchbird::tests::FixtureUuid(1064, 4);
  Check(fn::AcquireTransactionAdvisoryLock(other, "xact_lock", 18, &accepted) && !accepted);
  Check(a.session_runtime_state->transaction_advisory_lock_evidence.back().owner.uuid == a.transaction_uuid);
  s::SblrTransactionAdvisoryLockEntry local{19, {}, 1, UINT64_MAX};
  const auto local_owner = fn::TransactionAdvisoryLockOwnerEvidence(local);
  Check(local_owner.uuid.is_nil() && local_owner.numeric_reference == UINT64_MAX &&
        local_owner.kind == s::SblrAdvisoryLockOwnerKind::local_transaction);
  auto timeout = fn::MakeAdvisoryLockEvidence("get_lock", "timeout", 20, local_owner);
  timeout.timeout_seconds = -1;
  Check(timeout.timeout_seconds == -1 && timeout.owner.numeric_reference == UINT64_MAX);
}
