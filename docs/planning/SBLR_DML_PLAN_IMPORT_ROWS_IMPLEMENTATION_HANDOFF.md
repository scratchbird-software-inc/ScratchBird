# SBLR `dml.plan_import_rows` Implementation Handoff

Status: non-normative development guidance

Document class: private planning artifact; not a specification, decision record, opcode registry, or release-completion claim
Unique search key: `SBLR-DML-PLAN-IMPORT-ROWS-IMPLEMENTATION-HANDOFF-2026-08-28`

## 1. Authority and use

This document hands the audited `dml.plan_import_rows` gap to the development
team. It records current implementation evidence, prerequisite authority
decisions, expected runtime work, and the minimum proof required before the gap
can be called closed.

This document has no authority to assign an opcode, define a wire descriptor,
select transaction semantics, register a diagnostic, or amend Core behavior.
The implementation team must copy the final values from the approved master
Core specification. If this document differs from approved Core authority, Core
authority wins and this handoff must be updated before implementation continues.

Authority order and relevant anchors:

| Root or file | Path + unique search key | Use in this handoff |
| --- | --- | --- |
| Master Core controller | `/home/dcalford/Sandbox/Specifications/Core/AUTHORITY.md` + `CORE-SPECIFICATION-LOCATION-AUTHORITY-2026-08-15` | Establishes master authority and precedence. |
| SBLR coverage rule | `/home/dcalford/Sandbox/Specifications/Core/AUTHORITY.md` + `SBSQL-SURFACE-SBLR-FUNCTION-COVERAGE-AUTHORITY` | Prevents treating an operation-family row or generated coverage row as implementation completion. |
| MGA invariant | `/home/dcalford/Sandbox/Specifications/Core/AUTHORITY.md` + `MGA and WAL authority invariant` | Preserves engine-owned MGA finality and forbids WAL recovery authority. |
| Single-node finality decision | `/home/dcalford/Sandbox/Specifications/Core/decisions/DR-MGA-0001-single-node-transaction-authority.md` + `dr_mga_0001_single_node_transaction_authority` | Makes durable transaction inventory the single-node finality authority. |
| Core manifest | `/home/dcalford/Sandbox/Specifications/Core/MANIFEST.yaml` + `authority_files` | Confirms authority membership for every canonical dependency. |
| Preserved mirror | `/home/dcalford/CliWork/ScratchBird-Private/docs/Specifications/Core/` + matching relative path and master search key | Preservation only; never the source of the opcode or descriptor decision. |
| Active implementation | `/home/dcalford/CliWork/ScratchBird/project/` + implementation anchors listed below | Current product behavior and implementation target. |

## 2. Audited defect and specification closure

The operation was not wholly absent and its engine function was not a stub. The
implementation already had a symbolic route and a concrete planning-only API,
but Core had not assigned a numeric opcode or bound an operand, result,
executor, authority, and cluster rule. The specification correction dated
2026-08-28 now closes that authority gap.

Canonical closure:

| Surface | Path + unique search key | Approved state |
| --- | --- | --- |
| Operation | `/home/dcalford/Sandbox/Specifications/Core/registries/sblr-operation-matrix.yaml` + `operation_id: dml.plan_import_rows` | Complete row: `SBLR_DML_PLAN_IMPORT_ROWS`, `0x0319`, `import_rows_plan_descriptor`, `import_plan_result`, `engine.bound_import_descriptor_registry`, read effect, parser text forbidden, conditional cluster gateway. |
| Opcode and semantic closure | `/home/dcalford/Sandbox/Specifications/Core/registries/sblr-opcodes.yaml` + `SBLR_DML_PLAN_IMPORT_ROWS` | Required data-mutation-family opcode `0x0319`/793, read effect, object authorization, exact executor and semantic key. |
| Operand, result, and authority | `/home/dcalford/Sandbox/Specifications/Core/registries/sblr-operand-descriptors.yaml` + `SBLR-DML-PLAN-IMPORT-ROWS-ZERO-GREY-V1` | Exact top-level operand, 512-byte `IPLP`, four child carriers, result extension, `IPEV` evidence, INSERT right, live-MGA and no-mutation rules. |
| Diagnostic identity | `/home/dcalford/Sandbox/Specifications/Core/registries/consolidated-diagnostic-code-registry.csv` + `csv:canonical_code=MGA.TRANSACTION_INVALID` | UUID `3026c9cd-a50c-57cd-8115-5194555de301`, SQLSTATE `25000`, no native numeric code, retry only after authority/input change, and reject without mutation. |
| Executor closure | `/home/dcalford/Sandbox/Specifications/Core/registries/sblr-opcode-executor-zero-grey-closure.csv` + `SBLR_DML_PLAN_IMPORT_ROWS` | One required `dml.plan_import_rows` executor-evidence row. |
| Cluster ownership | `/home/dcalford/Sandbox/Specifications/Core/registries/cluster-sblr-gateway-opcode-ownership.yaml` + `SBLR_DML_PLAN_IMPORT_ROWS` | Read-only conditional gateway ownership, case `CGOM-0319`; no local fallback when the cluster predicate is true. |
| Conformance | `/home/dcalford/Sandbox/Specifications/Core/conformance_manifests/sbsql_sblr_command_zero_grey_closure.yaml` + `SBSQL-SBLR-ZG-017` through `019` | Identity, validation-only/no-effect, malformed/refusal, executor-evidence, and cluster-route assertions. |

Runtime evidence remains incomplete:

| Surface | Path + unique search key | Development gap |
| --- | --- | --- |
| Internal SBLR matrix | `project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml` + `sblr_operation: SBLR_DML_PLAN_IMPORT_ROWS` | Add code 793 and the exact approved descriptor/result/authority metadata. |
| Runtime opcode registry | `project/src/engine/sblr/sblr_opcode_registry.cpp` + `SB_ENGINE_SBLR_CANONICAL_OPCODE_NUMERIC_AUTHORITY_V1` and `Entry("dml.plan_import_rows"` | The symbolic entry exists, but the canonical numeric table omits 793 and lookup therefore yields code zero. |
| Engine API | `project/src/engine/internal_api/dml/import_api.hpp` + `EnginePlanImportRowsRequest`; `import_api.cpp` + `SB_ENGINE_INTERNAL_API_DML_IMPORT_API_BEHAVIOR` | Concrete planning validator exists; bind it to the exact descriptors and live authority generations. |
| SBLR adapter | `project/src/engine/sblr/sblr_dispatch.cpp` + `TypedPlanImportRowsRequest` | It drops approved fields and synthesizes source/format defaults; strict ingress must instead decode `IPLP` and its four child rows. |
| Parser/server route | `project/src/parsers/sbsql_worker/lowering/lowering.cpp` + `operation_id = "dml.plan_import_rows"`; server admission/dispatch anchors | Symbolic routes exist; make their numeric, binary, security, MGA, and cluster handling exact. |

Implementation closure means all of the following are simultaneously true:

1. Runtime lookup returns nonzero opcode 793 with exactly the approved metadata.
2. Binary encode, decode, admission, dispatch, and ABI paths preserve the exact
   approved carriers and fail closed on every malformed or stale field.
3. Parser-produced envelopes carry UUID-resolved canonical values and never SQL
   text, object-name authority, sensitive source handles, or finality decisions.
4. `EnginePlanImportRows` remains validation-only. Only
   `dml.execute_import_rows` may enter row mutation under MGA.
5. The focused positive, mutation, race, cancellation, cluster, and no-effect
   tests pass with accepted runtime evidence.

## 3. Approved Core contract

These values are fixed by Core and are implementation inputs, not suggestions:

1. Mnemonic `SBLR_DML_PLAN_IMPORT_ROWS`; code `0x0319` (decimal 793);
   operation `dml.plan_import_rows`; version `1.0`; family
   `data-mutation`.
2. Operand `import_rows_plan_descriptor`: one SBOP v1
   `descriptor_ref`, exactly 24 bytes
   `[descriptor_uuid_16, descriptor_generation_u64_le]`, class
   `sblr.import_rows_plan.v1`; defaults are forbidden.
3. Authority row `engine.bound_import_descriptor_registry.v1`, owned only by
   the engine import binder and keyed by authenticated statement receipt,
   structural occurrence, descriptor UUID, and generation.
4. Parent carrier `IPLP` v1 is exactly 512 little-endian bytes. Its digest is
   SHA-256 over the exact ASCII domain
   `ScratchBird.SblrImportRowsPlanDescriptor.V1` with no separator followed
   immediately by bytes 0..479. The digest occupies bytes 480..511 and is
   excluded from its own projection.
5. Only four operation-owned child rows use 56-byte
   `[UUID,generation,SHA-256]` references:
   - `ISRC` source envelope, 64-byte payload;
   - `IFMT` format-family envelope, 8-byte payload;
   - `IMAP` mapping vector, 96 bytes per mapping;
   - `IPOL` import policy, 72-byte payload.
   Every child uses the common 104-byte header. Its digest excludes bytes
   72..103, and the child contains the parent UUID/generation but never the
   parent digest.
   `IPOL` uses these exact nonzero `u8` maps: reject mode
   `{fail_fast:1, reject_row:2, reject_table:3, quarantine:4}`; reject-payload
   policy `{diagnostic_only:1, redacted_payload_reference:2,
   encrypted_payload_reference:3}`; resume policy `{fail_closed:1,
   resume_from_checkpoint:2, operator_review_required:3}`. Zero, unknown,
   unlisted, and reserved enum codes refuse as `SBLR.OPERAND_INVALID`. These
   are newly selected stable v1 wire assignments; they are not inferred from
   C++ declaration order, string-list order, or implementation-local enums.
6. Transaction, MGA snapshot, security snapshot, policy snapshot, resource
   admission, catalog, target relation, and executor availability remain exact
   live UUID/generation authorities in `IPLP`; child hashes do not replace
   those owners.
7. Result `import_plan_result` extends `engine.api.result.v1` with the exact
   twelve fields and numeric enum codes in Core. An accepted result is
   planning-only, requires `dml.execute_import_rows`, has
   `row_execution_completed=false` and
   `row_persistence_claimed=false`, and binds the validated IPLP
   UUID/generation/digest and IMAP count.
8. Accepted executor evidence is `IPEV` v1, exactly 208 bytes, with all ten
   validation bits set and a self-excluding SHA-256 projection over bytes
   0..175 under its exact domain.
9. Transaction effect is `read`; security is `object_authorized`; required
   right is `INSERT`. Planning must use the live engine MGA snapshot but
   creates no row, catalog, savepoint, commit, rollback, transaction-inventory,
   or finality effect.
10. Cluster context, cluster transaction, or route-fence presence requires
    `cluster.context_execution.v1` with read-route-security/read-only evidence.
    A missing or refusing gateway has no local fallback.
11. Planning creates no durable plan handle. `dml.execute_import_rows` must
    bind and revalidate its own execution descriptor.
12. The diagnostic precedence is exactly `SBLR.OPCODE_INVALID`,
    `SBLR.OPERAND_INVALID`, `SECURITY.ACCESS_DENIED`,
    `MGA.TRANSACTION_INVALID`,
    `MGA.AUTHORITY_MISMATCH`,
    `CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN`, `PROCESS.CANCELLED`,
    `SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING`, and
    `SBLR.OPERATION_UNSUPPORTED`, in that order. Malformed, zero, unknown, or
    internally inconsistent source/format/mapping/policy values map to
    `SBLR.OPERAND_INVALID`; an absent, ended, or non-active transaction maps to
    `MGA.TRANSACTION_INVALID`; a valid transaction whose authority, snapshot,
    or profile conflicts with the live context maps to
    `MGA.AUTHORITY_MISMATCH`; a recognized but unadmitted source/format pair or
    policy profile maps to `SBLR.OPERATION_UNSUPPORTED`. No free-text
    diagnostic branch was allocated by this correction.

All seven amended canonical files were already direct MANIFEST members, so no
Core member or README change was required. The master and preserved mirror
retain their unrelated whole-file history; the exact plan-import selector
projection is equal across both roots. Development must never use a whole-file
copy to erase unrelated root drift.

## 4. CDEFA and workplan dependency handling

The current CDEFA workplan consumes the affected registries:

| Workplan path + unique search key | Consequence |
| --- | --- |
| `docs/workplans/05-cde-data-operating-system-future-architecture-specification-workplan/DEPENDENCIES.csv` + `CDEFA-DEP-186V` | `sblr-operation-matrix.yaml` is an input to CDEFA-186, CDEFA-190, and CDEFA-200. |
| Same file + `CDEFA-DEP-186W` | `sblr-opcodes.yaml` is an input to CDEFA-186, CDEFA-190, and CDEFA-200. |
| Same file + `CDEFA-DEP-186Y` | `consolidated-diagnostic-code-registry.csv` is an input to CDEFA-186, CDEFA-190, and CDEFA-200; its selected gateway diagnostic row is unchanged but its whole-file preimage advanced. |
| `CANONICAL_DELIVERABLES.csv` + `CDEFA-DEL-186V`, `CDEFA-DEL-186W`, and `CDEFA-DEL-186Y` | CDEFA-186 will later amend the same registries for cloud-capacity operations and diagnostics. |
| `TRACKER.csv` + `CDEFA-186`, `CDEFA-190`, and `CDEFA-200` | These slices are planned and must consume revalidated preimages. |

Therefore:

- Close this DML defect as a bounded prerequisite, not as cloud-capacity scope.
- Revalidate and rebaseline `CDEFA-DEP-186V`, `CDEFA-DEP-186W`, and
  `CDEFA-DEP-186Y` hashes after the canonical correction. The 186Y selector
  projection must remain unchanged even though its containing-file hash moves.
- Refresh the CDEFA package input/preimage evidence before CDEFA-186 authoring.
- Do not mark any CDEFA proposal, slice, or gate complete from this correction.
- If CDEFA-186 has begun against the old preimage, stop both efforts and obtain
  a coordinated owner-approved rebase before either edits the shared files.

The active engine-listener workplan rows `ELER-020` and `ELER-089` are regression
consumers because they assert a planning-only import contract and parser/SBLR
sync. They do not own the canonical opcode decision.

## 5. Runtime implementation changes

All product changes belong under `/home/dcalford/CliWork/ScratchBird/project/`
and must be made only after Section 3 is approved.

### 5.1 Opcode identity and metadata

1. Update `project/src/engine/sblr/sblr_opcode_registry.cpp` at
   `SB_ENGINE_SBLR_CANONICAL_OPCODE_NUMERIC_AUTHORITY_V1` from the approved
   canonical registry. Add the approved nonzero code without deriving it from
   vector order, text hashing, aliases, or a private sequence.
2. Replace or augment the generic row anchored by
   `Entry("dml.plan_import_rows"` so lookup exposes the approved family,
   transaction effect, security class, operand contract, result contract,
   executor ID, transaction/security requirements, cluster rule, executor
   evidence requirement, accepted evidence state, and missing-evidence
   diagnostic.
3. Update `project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml` at
   `sblr_operation: SBLR_DML_PLAN_IMPORT_ROWS` with the approved code and exact
   descriptor IDs. The row may retain `opcode_registered` only after runtime
   identity tests pass.
4. Keep `project/src/engine/internal_api/ENGINE_API_SURFACE_REGISTRY.yaml` at
   `operation_id: dml.plan_import_rows` aligned with the approved transaction,
   security, cluster, and descriptor contract.
5. Do not add a code-zero alias or remap any existing opcode. In particular,
   preserve the approved identities for native bulk ingest, execute import,
   checkpoint normalization, and reject normalization.

### 5.2 Operand and result codec

Implement the exact Core contract at
`SBLR-DML-PLAN-IMPORT-ROWS-ZERO-GREY-V1` through these routing anchors:

- `project/src/engine/sblr/sblr_engine_envelope.hpp` +
  `SblrOperationEnvelope`
- `project/src/engine/sblr/sblr_engine_envelope.cpp` +
  `ValidateSblrEnvelope`
- `project/src/engine/sblr/sblr_opcode_stream.hpp` +
  `SblrOpcodeStream`
- `project/src/engine/sblr/sblr_opcode_stream.cpp` +
  `DecodeSblrOpcodeStream`
- `project/src/engine/sblr/sblr_dispatch.cpp` +
  `TypedPlanImportRowsRequest`

Required carrier work:

1. Add the single 24-byte SBOP descriptor reference and exact-generation lookup
   in `engine.bound_import_descriptor_registry.v1`.
2. Encode/decode the exact 512-byte `IPLP` layout. Check magic, version, all
   extents, zero flags, every nonzero UUID/generation, live-context equality,
   and the exact self-excluding digest before semantic validation.
3. Encode/decode the common 104-byte child header and the exact `ISRC`,
   `IFMT`, `IMAP`, and `IPOL` payloads. Verify child ownership and digest
   before accepting the parent.
4. Treat source-kind, format-family, and insert-mode values as the exact numeric
   closed enums. Unknown/zero values exact-refuse. Do not infer an enum from a
   parser surface name.
5. `IFMT` intentionally plans only the format family. Encoding, delimiter,
   quoting, null/date/time, and other byte-decoding options belong to the
   separately revalidated execution descriptor; planning must not invent them.
6. `IMAP` uses exact 96-byte records, increasing source ordinals, unique target
   column UUIDs, and current descriptor/type/codec generations. A zero mapping
   count authorizes no implicit execution mapping.
7. `IPOL` uses the exact three nonzero enum maps in Section 3, the exact bit
   layout, ppm bound, nullable reject-target tuple, and relaxed/strict policy
   rule. Unknown or zero enum values and unknown bits exact-refuse as
   `SBLR.OPERAND_INVALID`; no implementation-local ordinal assignment is
   permitted. `fail_fast` requires `diagnostic_only`, zero limits, and no
   reject target. `reject_row` requires a nonzero row or ppm limit.
   `reject_table` additionally requires a reject target. `quarantine` requires
   a reject target and either the redacted or encrypted payload-reference
   policy. If both limits are nonzero, the row limit is the effective execution
   limit and ppm remains bound policy evidence.
8. Emit the exact twelve success extensions of `import_plan_result`, with
   `mapped_column_count` equal to the IMAP count and the validated IPLP
   identity/digest copied exactly. Refusals use only the base ordered diagnostic
   shape and no success extensions.
9. Emit `IPEV` only after all ten validation gates pass. Verify its 208-byte
   layout and self-excluding digest. It is evidence of read-only validation,
   never of row execution or finality.

Required codec behavior:

- byte-identical encode/decode/re-encode for every canonical carrier;
- checked little-endian arithmetic for every offset, count, and extent;
- refusal of truncation, trailing bytes, nonzero reserved fields, wrong
  magic/version, invalid UUID/generation, wrong digest, parent/child mismatch,
  duplicate mapping, invalid enum/bit, overflow, and over-limit count before
  semantic dispatch;
- no raw SQL, object names, source locator, secret, credential, row payload,
  donor command, or parser branch label in these carriers;
- unknown version or unregistered opcode fails closed; no symbolic or code-zero
  fallback.

### 5.3 Typed dispatch completeness

At `project/src/engine/sblr/sblr_dispatch.cpp` +
`TypedPlanImportRowsRequest`:

1. Decode and preserve every approved operand field.
2. Remove implicit `native_sbsql_import` and `csv` substitution at strict SBLR
   ingress unless the approved descriptor explicitly encodes those defaults.
3. Keep target identity UUID-based. Never accept `localized_names` or an object
   name as engine authority.
4. Preserve bounded vector order and reject duplicate target-column mappings.
5. Validate limits before allocating unbounded strings or vectors.
6. Ensure `op == "dml.plan_import_rows"` calls only `EnginePlanImportRows`; it
   must not call row storage, import execution, commit, rollback, or a donor
   engine.

### 5.4 Engine planning API

At `project/src/engine/internal_api/dml/import_api.hpp` +
`struct EnginePlanImportRowsRequest` and
`project/src/engine/internal_api/dml/import_api.cpp` +
`SB_ENGINE_INTERNAL_API_DML_IMPORT_API_BEHAVIOR`:

1. Reconcile the C++ fields and validation outcomes to the approved descriptor.
2. Require the approved live MGA context and pinned catalog/security/policy
   evidence before returning an accepted plan.
3. Keep the result planning-only. It may normalize and validate, but it must not
   create a record version, write a page, update an index, persist a checkpoint,
   publish transaction state, or claim durable success.
4. Make execution binding explicit: accepted plans route row work only through
   `dml.execute_import_rows`.
5. On refusal, return the approved diagnostic without an executable plan or
   success evidence.
6. Do not make the planning result an execution handle. Build
   `dml.execute_import_rows` from its separately authorized execution
   descriptor and revalidate transaction, snapshot, target, catalog, security,
   policy, resource, mapping, format-detail, and route-fence generations before
   its first effect.

### 5.5 Parser, server, and public ABI

1. At `project/src/parsers/sbsql_worker/lowering/lowering.cpp` +
   `operation_id = "dml.plan_import_rows"`, emit the approved numeric opcode and
   descriptor for every currently admitted COPY/LOAD/BULK/INGEST surface.
2. Parser code may decode client syntax and bytes, resolve names to UUIDs, and
   produce canonical descriptors. It must not execute SQL, write rows, decide
   commit/rollback, or publish persistence evidence.
3. At `project/src/server/sblr_admission.cpp` + `dml.plan_import_rows`, validate
   numeric identity, security and transaction declarations, descriptor bounds,
   and cluster interception requirements before dispatch.
4. At `project/src/server/sblr_dispatch_server.cpp` +
   `SBLR_DML_PLAN_IMPORT_ROWS`, preserve the canonical binary descriptor and
   result. Do not reconstruct missing fields from SQL or parser-only metadata.
5. At `project/src/engine/public_abi.cpp` + `opcode_code`, carry the approved
   numeric identity and binary operand/result without a symbolic code-zero
   compatibility path.
6. Redact or omit sensitive handles and source endpoints from diagnostics,
   traces, result payloads, and generated fixtures.

## 6. MGA and non-mutation constraints

`dml.plan_import_rows` is transaction-sensitive but non-mutating.

Required boundaries:

- It runs only with the approved engine-issued live MGA transaction and pinned
  visibility/catalog/security/policy context.
- It may inspect the target descriptor and authorization state. It must not
  allocate or publish row versions.
- It must not commit, roll back, create/release a savepoint, advance transaction
  inventory state, change visibility, or satisfy an autocommit finality step.
- A plan success is not a row success, persistence success, commit success, or
  recovery guarantee.
- `dml.execute_import_rows` owns mutation entry; the engine MGA transaction
  manager and durable transaction inventory own commit/rollback/finality.
- Rollback after planning leaves no row/page/index/catalog effects because the
  plan created none.
- Cluster context, cluster transaction, or route-fence presence must invoke the
  approved gateway/interception rule. A local planner must not guess cluster
  authority.
- WAL, donor logs, SQLite journals, CRUD text events, parser state, timestamps,
  UUID ordering, and test fixtures are never finality or recovery authority.
- No donor or embedded database may be used to make conformance tests pass.

## 7. Diagnostics and refusal rules

Use only approved registry diagnostics. Preserve this precedence unless the
approved descriptor states a stricter order:

1. opcode/operation identity mismatch;
2. malformed or version-invalid operand;
3. security or INSERT authorization denial;
4. an absent, ended, or non-active MGA transaction identity;
5. a valid transaction with an incompatible MGA authority/snapshot/profile
   binding;
6. cluster route or authority refusal;
7. cancellation at an approved observation point;
8. missing executor evidence;
9. operation-specific source, format, mapping, or policy refusal;
10. unsupported operation/profile refusal.

The exact emitted identities, in precedence order, are:

1. `SBLR.OPCODE_INVALID` for code/key/mnemonic/operation identity mismatch;
2. `SBLR.OPERAND_INVALID` for malformed carriers and invalid operation-specific
   source, format, mapping, or policy values;
3. `SECURITY.ACCESS_DENIED`;
4. `MGA.TRANSACTION_INVALID` when the transaction identity is absent, ended,
   or not active for this MGA operation;
5. `MGA.AUTHORITY_MISMATCH` when an otherwise valid transaction's authority,
   snapshot, or profile conflicts with the live MGA context;
6. `CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN` when the conditional cluster
   predicate is true and the authenticated gateway refuses or is absent;
7. `PROCESS.CANCELLED`;
8. `SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING`;
9. `SBLR.OPERATION_UNSUPPORTED` for a well-formed, recognized but unadmitted
   source/format pair or policy profile.

The consolidated `MGA.TRANSACTION_INVALID` row is UUID
`3026c9cd-a50c-57cd-8115-5194555de301`, SQLSTATE `25000`, retry class
`never_retry_without_authority_or_input_change`, outcome
`reject_without_mutation`, and has no native numeric binding. That diagnostic
does not decide rollback, commit, recovery, visibility, or finality; durable MGA
transaction inventory remains authoritative.

The former draft spellings are not aliases and must not be emitted. Stop if any
implementation path would need a different identity or a free-text diagnostic.

Operation-specific details must remain bounded and redacted. They may identify
the rejected field or enum value but must not echo raw input rows, SQL text,
credentials, tokens, unredacted paths, or source handles.

## 8. Compatibility and versioning

- Treat the approved opcode as an additive identity. Do not renumber or alias
  existing opcodes.
- Numeric code zero remains invalid and receives no legacy mapping.
- There is no valid prior binary descriptor for this operation, so version 1
  begins only at the approved descriptor. Existing symbolic code-zero behavior
  is implementation-ahead evidence, not a wire compatibility contract.
- Readers reject unknown descriptor versions unless Core explicitly defines a
  bounded forward-compatible extension mechanism.
- Writers emit only the configured supported version and canonical encoding.
- If a released protocol/profile is discovered that emitted this symbolic
  operation without an approved numeric identity, stop and obtain a formal
  compatibility decision; do not infer an alias from deployed behavior.
- Generated fixtures and coverage declarations must be regenerated from the
  approved identity. Do not hand-edit only the expected output.

## 9. Required tests and gates

Extend existing tests where practical. Add a focused test only when an existing
target cannot assert the approved descriptor contract. Every implementation
anchor added to a test must use path + unique symbol/search key, not a line
number.

### 9.1 Opcode and registry identity

Extend `project/tests/sbsql_sblr_alignment/ia01_opcode_contract_test.cpp` +
`ValidateSblrOpcodeIdentity` and the CTest
`sbsql_sblr_alignment_ia01_opcode_contract` to prove:

- target lookup returns the approved nonzero code;
- operation ID, mnemonic, family, transaction effect, security class, operand,
  result, executor ID, and executor evidence exactly match approved Core;
- code zero is refused;
- wrong code, wrong mnemonic, wrong operation ID, duplicate code, and neighboring
  import opcode substitution are refused deterministically;
- registry iteration contains exactly one target row.

Extend `project/tests/sblr_surface/` +
`sblr_surface_function_registry_dispatch_conformance` to prove the canonical
registry, runtime registry, and dispatch route agree.

### 9.2 Binary descriptor and round trip

Extend `project/tests/sbsql_parser_worker/qow_sblr_codec.cpp` +
`QOW-CMAKE-SBLR-CODEC-V1` and CTest `qow_sblr_codec_v1` to cover:

- golden binary bytes for the minimum and fully populated approved descriptor;
- byte-identical encode/decode/re-encode;
- version, length, checksum, reserved-byte, truncation, trailing-data, invalid
  enum, invalid UUID, duplicate-field, and over-limit refusals;
- deterministic result encoding for acceptance and each refusal class;
- no SQL, object names, credentials, raw source handles, or parser branch text.

Update generated round-trip fixtures and run CTest
`sbsql_sblr_binary_round_trip_fixture_gate`. The fixture must assert the numeric
opcode and descriptor version, not only symbolic strings.

### 9.3 Typed dispatch and parser route

Extend `project/tests/sbsql_parser_worker/sbsql_dml_exact_route_conformance.cpp`
at `RequireEngineDispatch("dml.plan_import_rows"` and run CTest
`sbsql_dml_exact_route_conformance` to prove:

- every admitted COPY/LOAD/BULK/INGEST surface emits the approved numeric opcode;
- all approved operand fields survive parser, server admission, public ABI, and
  engine typed dispatch;
- omitted required fields fail instead of receiving implicit dispatch defaults;
- UUID binding is retained and localized names are refused;
- server/public-ABI results preserve canonical diagnostics and planning evidence;
- source endpoints and handles are redacted as approved.

Extend
`project/tests/sbsql_parser_worker/bulk/sbsql_missing_functionality_bulk_import_export_conformance.cpp`
at `SBLR_DML_PLAN_IMPORT_ROWS` and run CTest
`sbsql_missing_functionality_bulk_import_export_conformance` for the broader
bulk surface.

### 9.4 Engine planning and MGA non-mutation

Extend `project/tests/database_lifecycle/cdp_copy_append_batching_gate.cpp` +
`TestPlanContractIsCompleteAndExecutionBound` and CTest
`cdp_copy_append_batching_gate` to prove:

- valid live MGA context accepts a plan;
- missing/stale transaction, denied INSERT, missing UUID, localized name,
  malformed mapping, unsupported source/format, invalid policy, and stale plan
  receipt fail closed with approved diagnostics;
- accepted planning reports planning-only, requires
  `dml.execute_import_rows`, reports `row_execution_completed=false`, and emits
  no row-persistence claim;
- row/page/index/catalog counts and transaction inventory state are unchanged by
  planning success and refusal;
- planning followed by rollback leaves no mutation;
- planning followed by commit with no execute step leaves no mutation;
- execution remains separately covered for commit visibility and rollback
  invisibility without letting the plan own finality.

### 9.5 Cross-surface and release gates

Run these exact CTest targets in the configured implementation build:

```text
sbsql_sblr_alignment_ia01_opcode_contract
qow_sblr_codec_v1
sbsql_sblr_binary_round_trip_fixture_gate
sbsql_dml_exact_route_conformance
sbsql_missing_functionality_bulk_import_export_conformance
cdp_copy_append_batching_gate
sblr_surface_function_registry_dispatch_conformance
sblr_surface_reference_interface_closure_gate
sblr_surface_sbsql_sync_guardrail_gate
engine_listener_sbsql_parser_sync_gate
public_sbsql_parser_sync_gate
```

A focused invocation may use:

```sh
ctest --test-dir <configured-build-dir> --output-on-failure -R '^(sbsql_sblr_alignment_ia01_opcode_contract|qow_sblr_codec_v1|sbsql_sblr_binary_round_trip_fixture_gate|sbsql_dml_exact_route_conformance|sbsql_missing_functionality_bulk_import_export_conformance|cdp_copy_append_batching_gate|sblr_surface_function_registry_dispatch_conformance|sblr_surface_reference_interface_closure_gate|sblr_surface_sbsql_sync_guardrail_gate|engine_listener_sbsql_parser_sync_gate|public_sbsql_parser_sync_gate)$'
```

Run the MGA authority scan from the ScratchBird repository root:

```sh
python3 /home/dcalford/.codex/skills/scratchbird-mga-transaction-authority/scripts/mga_policy_gate.py --repo /home/dcalford/CliWork/ScratchBird
```

Run the canonical Core registry and workplan gates from
`/home/dcalford/Sandbox`:

```sh
python3 Workplans/sbsql-sblr-implementation-alignment/validate_core_registry_suite.py
python3 Workplans/sbsql-sblr-implementation-alignment/validate_core_operand_registry.py
python3 Workplans/sbsql-sblr-implementation-alignment/validate_core_zero_grey_recovery.py
python3 Workplans/sbsql-sblr-implementation-alignment/validate_workplan.py
```

A runtime pass cannot waive an invalid Core registry. The handback must also
prove equality of the exact `dml.plan_import_rows` selector projection across
master and mirror; it must not require whole-file equality or erase unrelated
root-specific history.

### 9.6 Pre-existing global registry debt

The 2026-08-28 specification snapshot contains two independent global defects
outside this bounded correction:

- the cluster gateway ownership registry has 472 unique operation rows for 599
  required-or-optional opcode identities, leaving 127 other opcode identities
  without generated gateway rows; the `SBLR_DML_PLAN_IMPORT_ROWS` row itself is
  present and exact;
- the command closure registry has 805 rows but only 804 unique `surface_id`
  values because `SBSQL-17068E518638` occurs twice, and the command-state totals
  retained in the shared conformance manifest predate that registry state.

Do not silently regenerate, delete, or renumber those unrelated rows in the
plan-import implementation change. Report them as separate owner work and keep
plan-import evidence selector-scoped. Passing the four structural scripts above
does not authorize a claim that either global debt is closed.

## 10. Evidence required for handback

The development handback must provide:

1. Approved canonical path + search key for the opcode, operation, operand,
   result, executor, transaction, security, cluster, and diagnostic decisions.
2. Master/mirror equality evidence for the exact plan-import selector
   projection, plus proof that unrelated whole-file root drift was preserved.
3. Runtime path + unique search key for every changed implementation and test
   surface.
4. Exact build configuration, commit/worktree identity, and CTest result for
   every target in Section 9.
5. Golden binary fixture hashes and proof that the approved numeric opcode is
   nonzero and unique.
6. Positive and negative diagnostic vectors, including redaction evidence.
7. MGA non-mutation proof for planning success, refusal, rollback, and
   commit-without-execute.
8. MGA policy-gate output.
9. Revalidated CDEFA dependency/preimage evidence for `CDEFA-DEP-186V`,
   `CDEFA-DEP-186W`, and `CDEFA-DEP-186Y`, including unchanged selected-row
   projection evidence for 186Y.
10. A residual statement naming every unsupported profile or platform; absence
    of evidence must remain pending, not passed.

## 11. Stop conditions

Stop implementation and return the exact conflict to the owning team if any of
the following occurs:

- The master Core correction is not approved, manifest-listed, or mirrored.
- Opcode code, descriptor ID/version, transaction effect, right, cluster rule,
  or diagnostic mapping remains undecided or differs across canonical files.
- The candidate numeric code collides with any admitted or reserved identity.
- Closure would require accepting numeric code zero, renumbering an existing
  opcode, or inferring a compatibility alias.
- The approved descriptor cannot represent a required C++ request/result field,
  or the runtime would silently drop or synthesize a required field.
- Parser, server, driver, donor adapter, or test fixture would gain row-storage,
  commit, rollback, visibility, recovery, or finality authority.
- Names or SQL text would cross the engine boundary as object authority.
- Raw source handles, paths, credentials, tokens, or row payloads would leak to
  diagnostics, traces, fixtures, or result evidence.
- Planning would create a row/page/index/catalog mutation or publish transaction
  finality.
- A donor/embedded database, WAL, redo/undo log, CRUD event stream, timestamp,
  UUID ordering, or parser state would become transaction/recovery authority.
- Any required test is skipped, waived, expected-failing, nondeterministic, or
  passes only through a synthetic/string-only route.
- The MGA policy gate reports a finding.
- Product-code changes overlap unrelated user work and cannot be safely
  separated.
- CDEFA-186 has begun from the old registry preimage and no coordinated rebase
  has been approved.

No stop condition may be converted into a local assumption or silent waiver.
The handback must identify the blocking path, unique search key, observed value,
expected authority source, and responsible owner.
