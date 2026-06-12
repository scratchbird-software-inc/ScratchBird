# Persistence Restart Conformance Report

Status: complete
Search key: `FSPE-PERSISTENCE_RESTART_CONFORMANCE_REPORT`

Owning slice: `FSPE-011D`

## Scope

This report records the persistence/restart gate for SBSQL-visible effects that
are admitted as server SBLR and executed by engine-owned SBLR/internal
procedures.

The gate does not send SQL text, reference command text, parser AST, or BoundAST to
`sb_engine`. Server admission rejects SQL-text-marked envelopes before dispatch.

## Coverage

- Database create uses `project/resources/seed-packs/initial-resource-pack`, not minimal bootstrap.
- Lifecycle open/shutdown/reopen reports `clean_checkpoint_path` with write admission unfenced.
- Typed catalog records, resource seed catalog, database UUID, and filespace UUID survive reopen.
- Schema/table DDL, two row inserts, MGA table metadata, index metadata, and index entries survive reopen.
- Post-restart select reaches the persisted row through a persisted index lookup.
- Domain descriptor state survives reopen, including encoded visibility-policy metadata.
- Function catalog record, security identity, grant record, management config record, and parser package registration survive lookup after reopen.
- Durable-local event channel subscription and publication survive reopen and poll as deliverable.
- Job-control manager surface remains available after restart through `agents.show`.

## Corrected Issues

- `catalog.lookup_object` now consults MGA relation metadata for tables and indexes.
- `catalog.get_descriptor` now consults MGA relation metadata for table descriptors.
- Domain persistence events now write to `.sb.domain_events` instead of appending to the main database file. The loader keeps fallback compatibility for old embedded domain-event records.
- SBLR dispatch now preserves derived create-table, insert, and select request fields instead of losing them during base-request conversion.

## Validation

```bash
cmake --build build/sbsql_parser_worker_validation --target sbsql_persistence_restart_conformance -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_persistence_restart_conformance --output-on-failure
build/sbsql_parser_worker_validation/tests/sbsql_parser_worker/sbsql_persistence_restart_conformance
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_parser_worker --output-on-failure
python3 project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/p0_precode_validation.py --gate all
```

Observed results:

- `sbsql_persistence_restart_conformance`: 1/1 passed.
- Direct gate summary: `sbsql_persistence_restart_conformance=passed ... rows=2 table_uuid=019e078d-f11d-7000-8000-000000000102`.
- `sbsql_parser_worker`: 27/27 passed.
- `p0_precode_validation.py --gate all`: all gates passed.
