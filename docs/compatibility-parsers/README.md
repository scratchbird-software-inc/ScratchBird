# Compatibility Parser Status

ScratchBird compatibility parsers translate selected reference-system syntax,
protocol-facing operations, metadata requests, and utility-style commands into
ScratchBird-owned outcomes. They do not make the ScratchBird engine execute
reference SQL, and they do not give a reference parser storage, recovery,
security, file-system, cluster, or transaction authority.

Current public beta status: the 25 parser lanes in
`compatibility-parser-status.csv` have public gate evidence that every surfaced
operation in the beta parser declarations is either mapped to ScratchBird
authority, emulated through a controlled ScratchBird route, handled as
parser-only metadata/presentation, or refused with a deterministic diagnostic.

Per-parser searchable surface status is generated under
[`parsers/`](parsers/README.md). Open the parser page for the compatibility
surface you are using and search for the functionality name to see its exact
implementation status, route/SBLR target, refusal policy, diagnostic, and source
anchor.

This is not a drop-in compatibility claim. It is a parser-boundary and
conformance-gate claim for the current beta source tree.

## Public Lanes

| Family | Reference profile | Batch |
| --- | --- | --- |
| Apache Ignite | 2.17.0 | distributed |
| Cassandra | 5.0.8 | nosql |
| ClickHouse | 25.12.10.7-stable | analytic |
| CockroachDB | 26.1.3 | distributed |
| Dolt | 1.86.6 | distributed |
| DuckDB | 1.5.2 | relational |
| Firebird | 5.0.4 | relational |
| FoundationDB | 7.3.77 | distributed |
| immudb | 1.11.0 | distributed |
| InfluxDB | 3.9.0 | analytic |
| MariaDB | 12.2.2 | relational |
| Milvus | 2.6.5 | analytic |
| MongoDB | 8.2.6 | nosql |
| MySQL | 9.7.0 | relational |
| Neo4j | 2026.04.0 | nosql |
| OpenSearch | 3.6.0 | analytic |
| OpenSearch SQL/PPL | 3.6.0-sql-ppl | analytic |
| PostgreSQL | 18.3 | relational |
| Redis | 8.6.2 | nosql |
| SQLite | 3.53.0 | relational |
| TiDB | 8.5.6 | distributed |
| TiKV | 8.5.6 | distributed |
| Vitess | 23.0.3 | distributed |
| XTDB | 2.1.0 | nosql |
| YugabyteDB | 2025.2.2.2 | distributed |

SQL Server, Oracle, and DB2 are tracked only as capability-reference families in
this public source tree. They do not have inbound compatibility parser lanes.

## Outcome Classes

Compatibility parser behavior is intentionally narrow and explicit:

- `admitted_sblr`: the parser emits a ScratchBird SBLR/UUID request that is
  still validated by the server and engine.
- `scratchbird_lifecycle_api`: lifecycle syntax maps to ScratchBird lifecycle
  authority, not to reference-engine file creation or attachment behavior.
- `parser_support_udr`: a trusted ScratchBird parser-support UDR handles a
  compatibility route after normal admission and policy checks.
- `catalog_projection`: metadata is projected through ScratchBird catalog and
  visibility policy.
- `policy_refusal_fail_closed`, `security_refusal_fail_closed`, or
  `unsupported_refusal_fail_closed`: the parser returns a deterministic
  diagnostic and emits no executable engine envelope.

The engine remains SBLR/UUID-only. Original SQL text can be retained as data for
diagnostics or provenance, but not as engine-executed code.

## Explicit Refusal Boundary

The compatibility parsers refuse or route through native ScratchBird management
for surfaces that would otherwise grant unsafe authority:

- arbitrary extension, plugin, module, server-script, or procedural runtime
  loading;
- direct server-local file reads or writes unless an admitted ScratchBird source
  object and per-emulated-database grant exist;
- physical backup/restore, page-copy, repair, verify, shard, placement,
  filespace, and cluster mutation through a compatibility parser;
- reference-engine transaction, recovery, log, binlog, oplog, WAL, or feed-token
  finality;
- cross-dialect parser dependency or reference-engine execution inside the
  ScratchBird engine.

Logical import/export, COPY-style client streams, CDC, replication, ETL, and
migration routes are mapped to ScratchBird bulk-rowset or universal-bridge
routes when policy admits the source, target, transaction grouping, quarantine,
and validation rules.

## Public Proof Gates

The public CTest suite contains the reference-parser gate family:

- `reference_core_framework_conformance`
- `reference_original_regression_import_conformance`
- `reference_native_tool_harness_contract_conformance`
- `reference_parser_start_evidence_manifest_conformance`
- `reference_parser_implementation_start_handoff_conformance`
- `compatibility_reference_parser_lane_registry_gate`
- `compatibility_reference_parser_operation_pattern_gate`
- `compatibility_reference_parser_full_lane_command_vectors`
- `compatibility_sql_first_tranche_original_tool_replay_gate`
- `parser_compatibility_replay_proof_gate`
- `parser_dialect_isolation_audit_gate`

The current local verification run for the replay closure group passed:

```text
reference_native_tool_harness_contract_conformance ......... Passed
compatibility_sql_first_tranche_original_tool_replay_gate ... Passed
parser_compatibility_replay_proof_gate ...................... Passed
parser_dialect_isolation_audit_gate ......................... Passed
```

That run validated the native-tool/client-replay harness contract for the 25
lanes, 121 parser replay cases across the 25 lanes, 8 staged
original/reference tool smoke checks, zero replay blockers, and the parser
dialect-isolation audit.

To reproduce the targeted closure group after configuring a build that enables
tests:

```bash
ctest --test-dir build/reference-parser-master-r0 \
  -L 'compatibility_original_regression_gate|parser_compatibility_replay_proof_gate|parser_dialect_isolation_audit_gate' \
  --output-on-failure
```

For a fresh build, enable the normal project test profile so CMake turns on
`SB_BUILD_COMPAT_SQL_PARSER_FIRST_TRANCHE_TESTS` and
`SB_BUILD_COMPAT_REGRESSION_GATES`.

## Public Payload Boundary

The public repository tracks ScratchBird parser source, parser-support UDR
source, public manifests, acquisition scripts, and CTest gates. Raw upstream
regression payloads and built original/reference tools are local test inputs and
are not committed as public source.

The public gates enforce that these external inputs are test evidence only. They
cannot become ScratchBird storage, recovery, transaction, or security authority.
