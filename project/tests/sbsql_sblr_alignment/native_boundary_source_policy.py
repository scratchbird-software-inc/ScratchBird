#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_BOUNDARY_SOURCE_POLICY (S5, controller Q4, 2026-12-09). A NATIVE session's COMMIT / ROLLBACK never reaches the legacy
engine helpers: (1) the native step RunNativeTransactionBoundary does not mention any legacy commit / rollback / replacement helper; (2) the
legacy cleanup helper RollbackUnpublishedTransaction returns on a native session BEFORE it builds an engine rollback request; (3) in the
canonical finality block the native replacement is taken from the loop's result and the legacy BeginOrdinaryReplacementTransactionForSession
is called only in the else branch of `native_boundary.has_value()`. A source-policy check (not a runtime proof)."""
import re
import sys

LEGACY = ("EngineCommitTransaction", "EngineRollbackTransaction", "BeginOrdinaryReplacementTransactionForSession", "BeginReplacementTransactionForSession",
          "RollbackUnpublishedTransaction", "FinalizeAutocommitBoundaryForSession")


def fail(message):
    print("FAIL native_boundary_source_policy: " + message)
    sys.exit(1)


def main():
    path = sys.argv[1]
    text = open(path, encoding="utf-8").read()
    start = text.index("NativeBoundaryOutcome RunNativeTransactionBoundary(")
    end = text.index("SessionOperationResult HandleExecuteSblrImpl(", start)
    body = text[start:end]
    for name in LEGACY:
        if name in body:
            fail(f"RunNativeTransactionBoundary mentions the legacy helper {name}")
    if "SubmitCompleteAndBeginWork" not in body:
        fail("RunNativeTransactionBoundary does not call the loop's CompleteAndBegin")
    helper = text[text.index("bool RollbackUnpublishedTransaction("):]
    guard = helper.index("native_session_approved")
    legacy_call = helper.index("EngineRollbackTransactionRequest")
    if not guard < legacy_call:
        fail("the native guard of RollbackUnpublishedTransaction does not precede the engine rollback request")
    if "return false;" not in helper[guard:guard + 400]:
        fail("the native guard of RollbackUnpublishedTransaction does not return")
    block = text[text.index("bool replacement_begun = false;"):]
    block = block[:block.index("if (!replacement_begun)")]
    if not re.search(r"if \(native_boundary\.has_value\(\)\) \{[^}]*replacement = native_boundary->replacement;[^}]*\} else \{[^}]*BeginOrdinaryReplacementTransactionForSession", block, re.S):
        fail("the legacy replacement begin is not confined to the else branch of native_boundary.has_value()")
    print("native_boundary_source_policy: passed")


if __name__ == "__main__":
    main()
