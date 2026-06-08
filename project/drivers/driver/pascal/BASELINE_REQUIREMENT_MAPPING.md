# ScratchBird Driver Baseline Requirement Mapping (S0)

## Scope
- Lane-local S0 artifact for `lanes/active/drivers/pascal` only.
- Maps this lane's current capability coverage to JDBCBL groups: `CONN`, `TXN`, `EXEC`, `META`, `TYPE`, `ERR`, `RES`.
- All statements below are anchored to lane-local source and test files.

## MGA Recovery Contract
- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- `BeginTransactionEx(...)` exposes the canonical MGA begin payload fields for
  isolation/access/deferrable/wait/timeout/autocommit/conflict-action, and
  the overloaded `BeginTransactionEx(..., ReadCommittedMode)` plus adapter
  `StartTransactionEx(..., ReadCommittedMode)` surfaces now expose the
  canonical `READ COMMITTED` sub-mode selector directly.
  `CanonicalReadCommittedModeName(...)` makes that selector explicit in lane
  source, including `READ COMMITTED READ CONSISTENCY`, while
  lane source now spells out the current isolation-byte meaning:
  `READ UNCOMMITTED` remains a legacy compatibility alias,
  `READ COMMITTED` => canonical `READ COMMITTED`,
  `REPEATABLE READ` => canonical `SNAPSHOT`,
  `SERIALIZABLE` => canonical `SNAPSHOT TABLE STABILITY`.
- `RetryScopeForSqlState(...)` makes the retry boundary explicit:
  `40001`/`40P01` => fresh statement only, `08xxx` => reconnect or reopen
  only, everything else => no automatic replay.
- `SupportsPreparedTransactions()` plus `PrepareTransaction(...)`,
  `CommitPrepared(...)`, and `RollbackPrepared(...)` make prepared / limbo
  control explicit through canonical transaction-control SQL.
- `SupportsDormantReattach() -> false`, `DetachToDormant()`, and
  `ReattachDormant(...)` keep dormant truth explicit and fail closed until
  the public front door exposes a real dormant token flow.
- `AllowPortalResume` / `ResumeSuspendedPortal(...)` keep result resume
  limited to explicit `MSG_PORTAL_SUSPENDED` state instead of blind
  continuation.
- See `../../../../public_audit_summary`.

## CONN (JDBCBL: CONN)
- Current status: Implemented
- Lane-local source anchors:
  - `src/ScratchBird.AuthBootstrap.pas` (shared auth/bootstrap method naming, plugin mapping, executable/broker rules, token payload resolution, startup auth-plugin selection, and resolved-auth context model)
  - `src/ScratchBird.Config.pas:55`, `src/ScratchBird.Config.pas:106`, `src/ScratchBird.Config.pas:324`
  - `src/ScratchBird.Transport.Native.pas:51`, `src/ScratchBird.Transport.Native.pas:59`, `src/ScratchBird.Transport.Native.pas:82`
  - `src/ScratchBird.Tls.Context.pas:136`, `src/ScratchBird.Tls.Context.pas:303`, `src/ScratchBird.Tls.Context.pas:334`
  - `src/ScratchBird.Tls.X509.pas:72`, `src/ScratchBird.Tls.X509.pas:173`
  - `src/ScratchBird.Client.pas:449`, `src/ScratchBird.Client.pas:481`, `src/ScratchBird.Client.pas:1070`, `src/ScratchBird.Client.pas:1201`
  - `src/ScratchBird.Client.pas` (`ProbeAuthSurface`, `GetResolvedAuthContext`, `BuildStartupParams`, `ProbeDirectAuthSurface`, `ProbeManagerAuthSurface`, `PerformManagerConnect`, `HandshakeAndAuth`)
  - `src/ScratchBird.Scram.pas` (algorithm-aware SCRAM helper with SHA-256/SHA-512 client-first/server-first handling)
  - `src/ScratchBird.Protocol.pas:362`, `src/ScratchBird.Protocol.pas:627`
- Lane-local test anchors:
  - `tests/ConfigTests.pas:38`, `tests/ConfigTests.pas:55`, `tests/ConfigTests.pas:61`
  - `tests/ConnectionAuthBootstrapContractTests.pas` (deterministic direct probe, manager probe, resolved auth context, `SCRAM_SHA_512`, `TOKEN`, and fail-closed `PEER` coverage)
  - `tests/ConnectionManagerProxyTests.pas:250` (deterministic manager-proxy connect success path with MCP negotiation + password auth handshake)
  - `tests/ConnectionManagerProxyTests.pas:290` (deterministic manager-proxy auth failure path maps to `28000` and remains disconnected)
  - `tests/ConnectionDirectAuthMatrixTests.pas:166`, `tests/ConnectionDirectAuthMatrixTests.pas:194` (deterministic direct front-door password + SCRAM auth matrix coverage through READY state with startup/auth frame assertions)
  - `tests/ConnectionAuthProtocolTests.pas:48`, `tests/ConnectionAuthProtocolTests.pas:59`, `tests/ConnectionAuthProtocolTests.pas:77`, `tests/ConnectionAuthProtocolTests.pas:98`, `tests/ConnectionAuthProtocolTests.pas:124`
  - `tests/TlsCryptoAndPolicyTests.pas:127`, `tests/TlsCryptoAndPolicyTests.pas:149`
  - `tests/IntegrationTest.pas:437`, `tests/IntegrationTest.pas:444` (env-gated live connect path and connected client setup)
- Parity notes:
  - Live connect checks are env-gated (`tests/IntegrationTest.pas:436-440`), with deterministic lane tests covering staged auth discovery, direct, manager-proxy, compatibility policy paths (`sslmode=disable`, `binary_transfer=false`, `compression=zstd|none`), `SCRAM_SHA_512`, generic `TOKEN`, and fail-closed unsupported admitted methods.

## TXN (JDBCBL: TXN)
- Current status: Implemented
- Lane-local source anchors:
  - `src/ScratchBird.Client.pas:451`, `src/ScratchBird.Client.pas:457`, `src/ScratchBird.Client.pas:459`, `src/ScratchBird.Client.pas:477`, `src/ScratchBird.Client.pas:489`, `src/ScratchBird.Client.pas:501`, `src/ScratchBird.Client.pas:512`, `src/ScratchBird.Client.pas:523`
  - `src/ScratchBird.Client.pas:1243`, `src/ScratchBird.Client.pas:1249`, `src/ScratchBird.Client.pas:1255`
  - `src/ScratchBird.Client.pas` (`SupportsPreparedTransactions`, `PrepareTransaction`, `CommitPrepared`, `RollbackPrepared`, `SupportsDormantReattach`, `DetachToDormant`, `ReattachDormant`)
  - `src/ScratchBird.Protocol.pas:545`, `src/ScratchBird.Protocol.pas:553`, `src/ScratchBird.Protocol.pas:558`, `src/ScratchBird.Protocol.pas:563`, `src/ScratchBird.Protocol.pas:571`, `src/ScratchBird.Protocol.pas:576`
  - `src/ScratchBird.FireDAC.pas:120`, `src/ScratchBird.FireDAC.pas:125`, `src/ScratchBird.FireDAC.pas:131`, `src/ScratchBird.FireDAC.pas:136` (adapter transaction begin/begin-ex/commit/rollback forwarding)
  - `src/ScratchBird.IBX.pas:118`, `src/ScratchBird.IBX.pas:123`, `src/ScratchBird.IBX.pas:129`, `src/ScratchBird.IBX.pas:134` (adapter transaction begin/begin-ex/commit/rollback forwarding)
  - `src/ScratchBird.Zeos.pas:119`, `src/ScratchBird.Zeos.pas:124`, `src/ScratchBird.Zeos.pas:130`, `src/ScratchBird.Zeos.pas:135` (adapter transaction begin/begin-ex/commit/rollback forwarding)
  - `src/ScratchBird.SQLdb.pas:119`, `src/ScratchBird.SQLdb.pas:124`, `src/ScratchBird.SQLdb.pas:130`, `src/ScratchBird.SQLdb.pas:135` (adapter transaction begin/begin-ex/commit/rollback forwarding)
- Lane-local test anchors:
  - `tests/TxnExecParityTests.pas:66`, `tests/TxnExecParityTests.pas:87`, `tests/TxnExecParityTests.pas:100`, `tests/TxnExecParityTests.pas:121`, `tests/TxnExecParityTests.pas:174`
  - `tests/TxnExecParityTests.pas` (prepared control SQL emission, blank global-transaction-id validation, and dormant fail-closed capability surface)
  - `tests/AdapterTransactionOptionsTests.pas:32`, `tests/AdapterTransactionOptionsTests.pas:50`, `tests/AdapterTransactionOptionsTests.pas:68`, `tests/AdapterTransactionOptionsTests.pas:86` (adapter `StartTransactionEx` disconnected guard parity across FireDAC/IBX/Zeos/SQLdb)
  - `tests/TxnStateTransitionsTests.pas:228`, `tests/TxnStateTransitionsTests.pas:275` (deterministic wire-ready transaction state transitions across begin/savepoint/release/rollback-to/commit and begin/rollback lifecycle paths, `BeginTransactionEx` option-matrix payload assertions, and injected `40001` conflict-path retry/no-active-txn guard behavior)
  - `tests/IntegrationTest.pas:225`, `tests/IntegrationTest.pas:251`, `tests/IntegrationTest.pas:446` (env-gated live transaction lifecycle coverage for begin/savepoint/release/rollback-to/commit and begin/rollback)
- Parity notes:
  - Live transaction lifecycle checks remain env-gated in `tests/IntegrationTest.pas`, while deterministic transaction matrix and conflict-path coverage is always-on in lane-local tests.

## EXEC (JDBCBL: EXEC)
- Current status: Implemented
- Lane-local source anchors:
  - `src/ScratchBird.Client.pas:656`, `src/ScratchBird.Client.pas:689`, `src/ScratchBird.Client.pas:713`, `src/ScratchBird.Client.pas:718`, `src/ScratchBird.Client.pas:1621`, `src/ScratchBird.Client.pas:1636`
  - `src/ScratchBird.Client.pas:175`, `src/ScratchBird.Client.pas:775` (first-class `ExecuteBatch` API with per-statement summary output)
  - `src/ScratchBird.Client.pas:176`, `src/ScratchBird.Client.pas:798` (first-class `QueryMulti` API with rowset materialization per statement)
  - `src/ScratchBird.Client.pas` (`AllowPortalResume` / `ResumeSuspendedPortal(...)` gate `MSG_PORTAL_SUSPENDED` continuation and reject unsuspended resume with `55000`)
  - `src/ScratchBird.Client.pas:1358` (`HandleAsyncMessage` now treats `MSG_NOTICE` as informational async traffic during result-stream reads)
  - `src/ScratchBird.Client.pas:48`, `src/ScratchBird.Client.pas:371`, `src/ScratchBird.Client.pas:372` (`TScratchBirdResultStream` generated-key exposure via `LastInsertId`/`HasLastInsertId` from `MSG_COMMAND_COMPLETE`)
  - `src/ScratchBird.Client.pas:274`, `src/ScratchBird.Common.pas:111`
  - `src/ScratchBird.Sql.pas:50`, `src/ScratchBird.Sql.pas:114`, `src/ScratchBird.Sql.pas:151`, `src/ScratchBird.Sql.pas:157`
  - `src/ScratchBird.FireDAC.pas:35`, `src/ScratchBird.FireDAC.pas:149`, `src/ScratchBird.FireDAC.pas:178` (adapter query prepare + exec routed through overridable execution hooks)
  - `src/ScratchBird.IBX.pas:34`, `src/ScratchBird.IBX.pas:167`, `src/ScratchBird.IBX.pas:196` (adapter query prepare + exec routed through overridable execution hooks)
  - `src/ScratchBird.Zeos.pas:34`, `src/ScratchBird.Zeos.pas:168`, `src/ScratchBird.Zeos.pas:197` (adapter query prepare + exec routed through overridable execution hooks)
  - `src/ScratchBird.SQLdb.pas:34`, `src/ScratchBird.SQLdb.pas:168`, `src/ScratchBird.SQLdb.pas:197` (adapter query prepare + exec routed through overridable execution hooks)
  - `docs/fixtures/core_fixture.sql:19`, `scripts/driver_runtime_stack.sh:185` (fixture-backed generated-key table provisioning for env-gated live integration default path)
- Lane-local test anchors:
  - `tests/BatchExecutionTests.pas:200` (`ExecuteBatch` returns per-statement rows/tag/generated-key summaries and emits expected wire query payloads)
  - `tests/BatchExecutionTests.pas:244` (`ExecuteBatch` preserves SQL blank-text guard behavior with `42601`)
  - `tests/QueryMultiTests.pas:269` (`QueryMulti` materializes per-statement rowsets including column/row data and generated-key metadata)
  - `tests/QueryMultiTests.pas:320` (`QueryMulti` preserves SQL blank-text guard behavior with `42601`)
  - `tests/StreamControlBackpressureTests.pas:200` (client `StreamControl` emits `MSG_STREAM_CONTROL` with encoded window/timeout payload)
  - `tests/StreamControlBackpressureTests.pas:221` (`MSG_PORTAL_SUSPENDED` read loop triggers guarded `MSG_EXECUTE` resume/backpressure follow-up only after the explicit suspended state is observed)
  - `tests/StreamControlBackpressureTests.pas:257` (result stream captures generated key metadata via `LastInsertId`/`HasLastInsertId`)
  - `tests/StreamControlBackpressureTests.pas:286` (result stream ignores async `MSG_NOTICE` frames without surfacing unsupported-message failures)
  - `tests/AdapterPrepareLifecycleTests.pas:146` (adapter prepare guardrails for missing connection/database assignment)
  - `tests/AdapterPrepareLifecycleTests.pas:218` (FireDAC prepare snapshot and normalized parameter ordering reuse on exec)
  - `tests/AdapterPrepareLifecycleTests.pas:247` (IBX prepare snapshot and normalized parameter ordering reuse on exec)
  - `tests/AdapterPrepareLifecycleTests.pas:276` (Zeos prepare snapshot and normalized parameter ordering reuse on exec)
  - `tests/AdapterPrepareLifecycleTests.pas:305` (SQLdb prepare snapshot and normalized parameter ordering reuse on exec)
  - `tests/TxnExecParityTests.pas:142`, `tests/TxnExecParityTests.pas:193`
  - `tests/SqlTests.pas:42`, `tests/SqlTests.pas:54`, `tests/SqlTests.pas:63`
  - `tests/IntegrationTest.pas:193`, `tests/IntegrationTest.pas:210`, `tests/IntegrationTest.pas:250`, `tests/IntegrationTest.pas:258`, `tests/IntegrationTest.pas:266`, `tests/IntegrationTest.pas:272`, `tests/IntegrationTest.pas:403`, `tests/IntegrationTest.pas:417`, `tests/IntegrationTest.pas:440`, `tests/IntegrationTest.pas:442`, `tests/IntegrationTest.pas:444`, `tests/IntegrationTest.pas:449` (env-gated live prepared query, batch, multi-result, stream-control, and optional generated-key execution coverage; generated-key live verification runs only when `SCRATCHBIRD_PASCAL_GENERATED_KEY_SQL` is set)
- Parity notes:
  - Live advanced execution checks remain env-gated in `tests/IntegrationTest.pas`. Generated-key live verification is opt-in via `SCRATCHBIRD_PASCAL_GENERATED_KEY_SQL`, while deterministic lane coverage continues to prove batch/multi-result, stream-control/backpressure, generated-key extraction, and async notice handling.

## META (JDBCBL: META)
- Current status: Implemented
- Lane-local source anchors:
  - `src/ScratchBird.Metadata.pas:160` (`NormalizeMetadataCollectionName`, alias normalization across schema/table/index/constraint/routine/catalog/key/privilege/type metadata families)
  - `src/ScratchBird.Metadata.pas:184` (`ResolveMetadataCollectionQuery`, metadata collection to SQL resolution)
  - `src/ScratchBird.Metadata.pas:443`, `src/ScratchBird.Metadata.pas:696` (`MetadataCollectionRestrictionKeys` includes routines restriction aliases; `FilterMetadataRowsByRestrictions` provides collection-scoped wildcard/null filtering)
  - `src/ScratchBird.Metadata.pas:775`, `src/ScratchBird.Metadata.pas:780`, `src/ScratchBird.Metadata.pas:785`, `src/ScratchBird.Metadata.pas:790`, `src/ScratchBird.Metadata.pas:795`, `src/ScratchBird.Metadata.pas:800`, `src/ScratchBird.Metadata.pas:805`, `src/ScratchBird.Metadata.pas:810`, `src/ScratchBird.Metadata.pas:815`, `src/ScratchBird.Metadata.pas:820`, `src/ScratchBird.Metadata.pas:825`, `src/ScratchBird.Metadata.pas:830`, `src/ScratchBird.Metadata.pas:835`, `src/ScratchBird.Metadata.pas:840` (table/column/index/index-column/constraint/routine/catalog/key/privilege/type query builders with schema/table/index alias enrichment plus routine/type JDBC-oriented aliases; procedures/functions/routines sourced via `information_schema.routines`)
  - `src/ScratchBird.Metadata.pas:843` (`ExpandSchemaPaths`, dotted parent expansion + de-duplication)
  - `src/ScratchBird.Metadata.pas:878` (`ListMetadataSchemaPaths`, metadata-row schema extraction + optional parent expansion)
  - `src/ScratchBird.Metadata.pas:906` (`ExpandSchemaMetadataRows`, synthetic ancestor-row shaping for recursive navigation)
  - `src/ScratchBird.Metadata.pas:1077` (`BuildMetadataSchemaTree`, recursive schema tree with per-parent uniqueness and terminal-node semantics)
  - `src/ScratchBird.Client.pas:724`, `src/ScratchBird.Client.pas:729` (generic client metadata stream API via `QueryMetadata`/`GetSchema`)
  - `src/ScratchBird.Client.pas:734`, `src/ScratchBird.Client.pas:742`, `src/ScratchBird.Client.pas:779`, `src/ScratchBird.Client.pas:784` (materialized metadata-row API with optional restrictions via `QueryMetadataRows`/`GetSchemaRows`)
  - `src/ScratchBird.Client.pas:789`, `src/ScratchBird.Client.pas:834`, `src/ScratchBird.Client.pas:859` (typed metadata wrapper methods, including `index_columns`, for catalogs/routines/type_info families)
  - `src/ScratchBird.FireDAC.pas:148`, `src/ScratchBird.FireDAC.pas:158`, `src/ScratchBird.FireDAC.pas:178`, `src/ScratchBird.FireDAC.pas:243` (adapter-level metadata stream/rows/typed wrapper forwarding)
  - `src/ScratchBird.IBX.pas:141`, `src/ScratchBird.IBX.pas:151`, `src/ScratchBird.IBX.pas:171`, `src/ScratchBird.IBX.pas:236` (adapter-level metadata stream/rows/typed wrapper forwarding)
  - `src/ScratchBird.Zeos.pas:142`, `src/ScratchBird.Zeos.pas:152`, `src/ScratchBird.Zeos.pas:172`, `src/ScratchBird.Zeos.pas:237` (adapter-level metadata stream/rows/typed wrapper forwarding)
  - `src/ScratchBird.SQLdb.pas:142`, `src/ScratchBird.SQLdb.pas:152`, `src/ScratchBird.SQLdb.pas:172`, `src/ScratchBird.SQLdb.pas:237` (adapter-level metadata stream/rows/typed wrapper forwarding)
- Lane-local test anchors:
  - `tests/AdapterMetadataApiTests.pas:33`, `tests/AdapterMetadataApiTests.pas:122`, `tests/AdapterMetadataApiTests.pas:211`, `tests/AdapterMetadataApiTests.pas:300` (adapter metadata API disconnected/not-supported guard and forwarding coverage across FireDAC/IBX/Zeos/SQLdb)
  - `tests/MetadataRecursiveSchemaTests.pas:129` (database/default branch-style metadata-row expansion)
  - `tests/MetadataRecursiveSchemaTests.pas:164` (dotted parent expansion ordering + uniqueness)
  - `tests/MetadataRecursiveSchemaTests.pas:184` (per-parent uniqueness semantics)
  - `tests/MetadataRecursiveSchemaTests.pas:208` (same leaf name under different parents)
  - `tests/MetadataRecursiveSchemaTests.pas:234` (metadata collection alias/query resolution coverage including catalogs/keys/privileges/type_info/routines)
  - `tests/MetadataRecursiveSchemaTests.pas:274` (restriction filtering coverage for aliases/wildcards/null semantics and unsupported restriction ignore behavior)
  - `tests/MetadataRecursiveSchemaTests.pas:338` (restriction filtering coverage for routines via `procedure`/`function` aliases)
  - `tests/MetadataRecursiveSchemaTests.pas:373` (client metadata stream API guards: unsupported collection => `0A000`, disconnected supported collection => `08003`)
  - `tests/MetadataRecursiveSchemaTests.pas:405` (client metadata rows API guards for unsupported/disconnected paths)
  - `tests/MetadataRecursiveSchemaTests.pas:434` (typed metadata wrapper API guards on disconnected client)
  - `tests/MetadataExecutionFlowTests.pas:217`, `tests/MetadataExecutionFlowTests.pas:277`, `tests/MetadataExecutionFlowTests.pas:320`, `tests/MetadataExecutionFlowTests.pas:361` (deterministic metadata execution flow coverage for all typed wrapper query paths, including catalogs/indexes/index_columns/keys/privileges/procedures/functions/routines/type_info, plus restriction-aware `QueryMetadataRows` materialization across additional metadata families)
  - `tests/IntegrationTest.pas:139`, `tests/IntegrationTest.pas:154`, `tests/IntegrationTest.pas:291`, `tests/IntegrationTest.pas:356`, `tests/IntegrationTest.pas:367`, `tests/IntegrationTest.pas:450` (env-gated live metadata stream/wrapper execution plus restriction-aware `QueryMetadataRows` assertions across supported metadata families)
- Parity notes:
  - Live metadata checks remain env-gated in `tests/IntegrationTest.pas`, while deterministic metadata query-shape, alias, restriction, recursive schema, and wrapper-flow coverage is always-on in lane-local tests.

## TYPE (JDBCBL: TYPE)
- Current status: Implemented
- Lane-local source anchors:
  - `src/ScratchBird.Types.pas:53`, `src/ScratchBird.Types.pas:57`, `src/ScratchBird.Types.pas:66`
  - `src/ScratchBird.Types.pas:550`, `src/ScratchBird.Types.pas:559` (`TIMETZ` encode helpers including zone-offset payload handling)
  - `src/ScratchBird.Types.pas:814`, `src/ScratchBird.Types.pas:845`, `src/ScratchBird.Types.pas:1254` (geometry wrapper OID-preserving encode/decode routing across supported geometry OIDs)
  - `src/ScratchBird.Types.pas:737`, `src/ScratchBird.Types.pas:1172` (composite decode guardrails and `OID_RECORD` fail-closed null materialization on malformed composite frames)
  - `src/ScratchBird.Types.pas:1147` (strict vector literal parsing rejects malformed/non-numeric token sequences instead of coercing to zero)
  - `src/ScratchBird.Types.pas:247`, `src/ScratchBird.Types.pas:994`, `src/ScratchBird.Types.pas:1051`, `src/ScratchBird.Types.pas:1087`, `src/ScratchBird.Types.pas:1172` (payload-length guardrails and malformed range/interval decode handling via `HasBytes` + guarded per-OID dispatch)
  - `src/ScratchBird.Types.pas:1024`, `src/ScratchBird.Types.pas:1172` (`TIMETZ` decode and guarded per-OID decode dispatch)
  - `src/ScratchBird.Client.pas:274`
- Lane-local test anchors:
  - `tests/TypesCodecTests.pas:166`, `tests/TypesCodecTests.pas:441` (primitive encode/decode matrix coverage for bool/int/float/text/date-variant routes and mixed-array fallback behavior)
  - `tests/TypesCodecTests.pas:272`, `tests/TypesCodecTests.pas:285` (`BYTEA` variant-array decode semantics and unknown fixed-width fallback decode behavior including 1/2/4/8/16-byte widths)
  - `tests/TypesCodecTests.pas:215`, `tests/TypesCodecTests.pas:227` (vector decode success path plus malformed vector literal null-guard coverage)
  - `tests/TypesCodecTests.pas:309`, `tests/TypesCodecTests.pas:335` (null/limit plus malformed/truncated payload-shape coverage for empty payloads, short `TIMETZ`, empty vectors, empty/infinite ranges, and truncated scalar/temporal/range frames)
  - `tests/TypesCodecTests.pas:238`, `tests/TypesCodecTests.pas:261` (composite round-trip coverage plus malformed composite frame guards for negative/truncated payloads)
  - `tests/TypesCodecTests.pas:364`, `tests/TypesCodecTests.pas:405` (scalar, text-family, temporal, and interval per-OID decode coverage)
  - `tests/TypesCodecTests.pas:487` (jsonb/geometry/range object encode paths plus range decode assertions for int and timestamp range families, including custom geometry OID encode path)
  - `tests/TypesCodecTests.pas:597` (geometry-family decode wrapper coverage for point/lseg/path/box/polygon/line/circle OIDs, including OID preservation on decoded wrappers)
  - `tests/TypesCodecTests.pas:620`, `tests/TypesCodecTests.pas:638`, `tests/TypesCodecTests.pas:654` (`TIMETZ` decode/encode coverage for 12-byte, backward-compatible 8-byte, and sign/offset payload semantics)
  - `tests/IntegrationTest.pas:377`, `tests/IntegrationTest.pas:451` (env-gated live `type_coverage` fixture execution path)
- Parity notes:
  - Live type-fixture validation remains env-gated in `tests/IntegrationTest.pas`, with deterministic lane tests covering scalar/text/temporal/interval/vector/composite/range/geometry/jsonb encode-decode and malformed payload guards.

## ERR (JDBCBL: ERR)
- Current status: Implemented
- Lane-local source anchors:
  - `src/ScratchBird.Errors.pas:52`, `src/ScratchBird.Errors.pas:60`, `src/ScratchBird.Errors.pas:100`
  - `src/ScratchBird.Protocol.pas:821`
  - `src/ScratchBird.Client.pas:997`
- Lane-local test anchors:
  - `tests/ErrorMappingTests.pas:40` (`MapSqlState` category assertions preserve SQLSTATE/detail/hint metadata)
  - `tests/ErrorMappingTests.pas:55` (category mapping matrix: warning/no-data/connection/not-supported/data/integrity/auth/txn/syntax/resource/limit/operator/system/internal)
  - `tests/ErrorMappingTests.pas:73` (fallback behavior for unknown SQLSTATE class)
  - `tests/ErrorMappingTests.pas:78` (fallback behavior for invalid SQLSTATE length)
  - `tests/ErrorMappingTests.pas:83` (`RetryScopeForSqlState` / `IsRetryableSqlState` make statement-vs-reconnect-vs-none boundaries explicit)
- Gaps/next actions:
  - `BuildQueryError` parses severity from wire payload but categorization is SQLSTATE-driven (`src/ScratchBird.Client.pas:997`).

## RES (JDBCBL: RES)
- Current status: Implemented
- Lane-local source anchors:
  - `src/ScratchBird.Client.pas:375`, `src/ScratchBird.Client.pas:385`, `src/ScratchBird.Client.pas:435`, `src/ScratchBird.Client.pas:448`, `src/ScratchBird.Client.pas:1426`
  - `src/SBCircuitBreaker.pas:136`, `src/SBCircuitBreaker.pas:173`, `src/SBCircuitBreaker.pas:197`
  - `src/SBKeepalive.pas:55`, `src/SBKeepalive.pas:177`, `src/SBKeepalive.pas:205`, `src/SBKeepalive.pas:264`
  - `src/SBLeakDetector.pas:40`, `src/SBLeakDetector.pas:148`, `src/SBLeakDetector.pas:193`, `src/SBLeakDetector.pas:253`
  - `src/ScratchBird.Common.pas:45`, `src/ScratchBird.Common.pas:111`, `src/ScratchBird.Common.pas:117`
- Lane-local test anchors:
  - `tests/ResourceResilienceTests.pas:69` (keepalive tracker idle-window validation and `MarkActive` reset behavior)
  - `tests/ResourceResilienceTests.pas:92` (keepalive manager register/update/unregister plus idle pinger invocation)
  - `tests/ResourceResilienceTests.pas:126` (checkout metadata capture semantics)
  - `tests/ResourceResilienceTests.pas:139` (leak detector checkout/checkin replacement and active-count lifecycle)
  - `tests/ResourceResilienceTests.pas:168` (leak detector background thread start/stop lifecycle)
  - `tests/IntegrationTest.pas:391`, `tests/IntegrationTest.pas:398`, `tests/IntegrationTest.pas:456` (env-gated optional cancel path under live execution)
- Gaps/next actions:
  - Add live integration assertions for keepalive/leak behavior under real network disruption and reconnect scenarios (current coverage is deterministic lane-local behavior and lifecycle tests).
