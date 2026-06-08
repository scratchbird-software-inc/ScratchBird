# Concurrent Session Transaction Report

Status: complete
Search key: `FSPE-CONCURRENT_SESSION_TRANSACTION_REPORT`

FSPE-011E materialized `sbsql_concurrent_session_transaction_conformance` as a runnable CTest gate.

## Coverage

- Server session isolation: one session cannot fetch another session's cursor or execute another session's prepared SBLR.
- Prepared SBLR invalidation: server execution rejects prepared SBLR when catalog/security/policy epochs no longer match the active session.
- Disconnect cleanup: parser disconnect removes the session, closes prepared SBLR, and marks active cursor finality as `parser_disconnected`.
- Transaction visibility: read-committed readers see committed rows, snapshot readers keep their begin high-water and do not see later commits, rollback hides uncommitted rows, and savepoint rollback discards post-savepoint rows.
- Reservation evidence: insert paths expose identity-range and page-reservation evidence.
- Lock behavior: the MGA lock table covers grant, no-wait timeout, bounded wait-required state, deadlock detection, and release-all cleanup without busy waiting.
- Parser cache behavior: cache keys separate catalog, security-policy, descriptor, search-path, and dialect profile epochs; threaded store/lookup pressure remains deterministic.

## Corrected Issues

- `SblrTemplateCache::Store` now computes the stable cache key before moving the key into the cache entry.
- Server prepared-SBLR execution now rejects cross-session prepared UUID use and stale prepared authority epochs.
- MGA savepoint parsing now decodes encoded savepoint names before lookup.
- MGA savepoints now carry row, metadata, and index cutoffs so rollback-to-savepoint filters table/index metadata as well as row/index sidecars.
- Prepared transaction commit cleanup no longer reloads relation state as though the prepared transaction must still be active.

## Validation

- `cmake --build build/sbsql_parser_worker_validation --target sbsql_concurrent_session_transaction_conformance -j 4`: passed.
- `build/sbsql_parser_worker_validation/tests/sbsql_parser_worker/sbsql_concurrent_session_transaction_conformance`: `sbsql_concurrent_session_transaction_conformance=passed`.
- `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_concurrent_session_transaction_conformance --output-on-failure`: 1/1 passed.
- `ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure`: 28/28 passed.
