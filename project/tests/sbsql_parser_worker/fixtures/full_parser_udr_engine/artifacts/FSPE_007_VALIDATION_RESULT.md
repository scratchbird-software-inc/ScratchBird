# FSPE-007 Validation Result

Status: complete
Search key: `FSPE-007-SBLR-LOWERING-VERIFIER-CONFORMANCE-VALIDATION`
Generated: 2026-05-07 23:38:59 EDT
Owning slice: `FSPE-007`

## Implemented Outputs

- Expanded parser-side `SblrEnvelope` with BoundAST-derived surface, command, operation, result, diagnostic, resource, epoch, UUID/ref, rights, authority-step, source-artifact, and trace metadata.
- Updated `LowerToSblr` to emit SBLR execution-envelope evidence without embedding SQL/source text as authority.
- Added `VerifySblrEnvelope` parser-side verifier checks for envelope version, operation key consistency, surface/contract metadata, authority evidence, payload presence, and forbidden SQL/source text embedding.
- Added generated-style SBLR lowering/verifier conformance probe at `project/tests/sbsql_parser_worker/generated/lowering/sbsql_sblr_lowering_verifier_conformance_probe.cpp`.
- CTest label: `sbsql_sblr_lowering_verifier_conformance`.

## Coverage

The conformance probe validates:

- positive lowering and verifier admission for name-free `SELECT 1`;
- lowering and verifier admission for a resolver-backed object query with externally supplied UUID evidence;
- lowering and verifier admission for security statements with server security authority evidence;
- preservation of operation family, SBLR operation key, surface key, command family, result-shape, diagnostic-shape, resource-contract, epochs, resolved UUIDs, descriptor refs, policy refs, rights, and authority steps;
- unbound statements do not produce SBLR payloads and are not verifier-admitted;
- malformed envelopes with operation mismatch and embedded SQL text are rejected deterministically;
- source artifacts are represented as span/checksum-style evidence and SQL text is not embedded into engine-facing payloads.

## Validation

Commands run:

```text
cmake -S project -B build/sbsql_parser_worker_validation
cmake --build build/sbsql_parser_worker_validation --target sbp_sbsql_sblr_lowering_verifier_conformance_probe -j 4
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_sblr_lowering_verifier_conformance --output-on-failure
ctest --test-dir build/sbsql_parser_worker_validation --output-on-failure
```

Results:

- `sbsql_sblr_lowering_verifier_conformance`: `1/1` passed.
- Focused SBSQL parser-worker validation shard: `23/23` passed.

## Boundary Notes

FSPE-007 closes only BoundAST-to-SBLR lowering, parser-side envelope evidence emission, parser-side verifier-admitted positive fixtures, deterministic negative verifier fixtures, and source/reverse-render artifact preservation. It does not execute through `sb_server` or `sb_engine`, does not prove server admission/runtime behavior, and does not close engine gaps.

ScratchBird engine execution remains SBLR/internal-procedure only; no SQL text, donor text, meta-command text, AST, or parser authorization decision becomes engine authority. Parser-side names and source artifacts are diagnostic/rendering evidence only; UUID, descriptor, security, transaction, policy, capability, and MGA authority remain server/engine-owned.

The current parser-worker envelope is JSON evidence for the worker route and conformance gates. Final engine binary `SBLRExecutionEnvelopeV1` admission, opcode validation, execution dispatch, result-envelope behavior, and full route execution remain owned by later server/engine slices.

Recovery remains MGA-based, not WAL.
