# Source Integrity Gate Policy

Search key: `SBSQL-SURFACE-SBLR-SOURCE-INTEGRITY-GATE-POLICY`

## Purpose

The source integrity gate prevents a row from closing through generic success, ignored tests, disabled fixtures, or unimplemented fallthrough paths.

## Required Rejections

The gate must fail if implementation or regression files contain any of these patterns in executable paths:

- Success returned from an unimplemented handler.
- A parser branch accepting syntax without a row-identifiable surface id.
- A lowering path that emits only an operation family when an exact function or API operation id is required.
- A server admission path that bypasses authentication, authorization, transaction, resource, or SBLR verifier checks.
- An engine path that dispatches by SQL text, parser branch name, donor command name, or user-facing object name.
- A fixture that is registered but not executed by CTest.
- A test assertion that is disabled, skipped, or converted to unconditional success without a recorded exact refusal policy.
- Any TODO-style work marker in files owned by an active slice.

## Required Evidence

Every closed slice must record:

- Source files scanned.
- Fixture files scanned.
- CTest labels scanned.
- Rejected tokens and allowed exceptions.
- Human-reviewed exception rows, if any, with search-key-based references.

The policy is satisfied only by an executable CTest gate named `sbsql_no_stub_source_integrity_gate`.
