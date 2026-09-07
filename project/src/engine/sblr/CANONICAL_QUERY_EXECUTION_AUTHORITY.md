# Canonical Query Execution Decomposition Project

This document is a source-ownership and decomposition control. It does not
claim runtime conformance or implementation maturity.

## Baseline and objective

The project baseline for `canonical_query_execute.cpp` is 70,749 lines and
3,395,606 bytes. The end state is a small coordinator surrounded by modules
whose names, dependencies, tests, and authority statements match the query
surface they implement.

Splits are performed as behavior-preserving stages. A stage must not combine a
mechanical move with a semantic correction. Any required behavior change is a
separate reviewed change after the affected route has an isolated test gate.

## Non-negotiable boundaries

- Canonical query execution consumes validated SBLR and UUID-addressed typed
  descriptors. It does not parse or execute SQL text.
- Durable MGA transaction inventory and the engine snapshot resolver remain
  visibility and transaction-finality authority.
- Query modules may validate that a supplied statement boundary still matches
  engine authority. They may not manufacture, commit, roll back, or recover a
  transaction.
- Model-family adapters may normalize and publish typed query results only
  through their admitted engine providers.
- Implementation fragments (`.inc` files) are not a decomposition boundary.
- Public query entry points remain in the coordinator until a later stage
  explicitly assigns a new public facade.

## Current module authority

| Module | Owns | Must not own |
| --- | --- | --- |
| `canonical_query_execute.cpp` | Canonical query route selection, admitted planning/execution coordination, and public execution entry points | Parser lowering, durable transaction finality, storage mutation publication |
| `canonical_query_object_free_profile.cpp` | Immutable object-free executor capability projection, physical-DAG planning input construction, and separately carried runtime-memory receipts | Physical callback execution, data access, MGA visibility/finality |
| `canonical_query_physical_registration.cpp` | Revalidation-only MGA authority handles, operator-local physical-DAG scoping and memory preflight, execution-receipt identity checks, and bounded already-materialized source/VALUES registrations | Snapshot construction, transaction begin/commit/rollback/recovery, optimizer grant authority, storage reads, expression evaluation |
| `canonical_query_relational_registration.cpp` | Bounded relational physical registrations over already-materialized typed batches, including query `DISTINCT`, `LIMIT`/`OFFSET`/`FETCH FIRST`, global `COUNT(*)`, and nonrecursive CTE publication | Snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_window_registration.cpp` | Bounded window physical registrations for row numbering, peer ranking/distribution, bucketing, navigation, and aggregate frames | Snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_scalar_support.cpp` | Canonical UUID validation/derivation, scalar equality-key normalization, and exact `int64` wire decoding | Catalog-bound comparison, descriptor construction, provider execution, transaction state |
| `canonical_query_descriptor_support.cpp` | Exact descriptor/result-shape comparison and result-nullability projection | Catalog lookup, persisted descriptor authorization, descriptor construction, transaction state |
| `canonical_query_json_support.cpp` | Bounded wildcard scanning over already-canonical JSON values | SQL/JSON parsing, document-provider execution, result publication, transaction state |
| `canonical_query_runtime_memory_support.cpp` | Overflow-safe logical/live memory accounting over typed values and descriptor batches | Memory grants, plan selection, storage accounting, MGA visibility/finality |
| `canonical_query_time_series_endpoint.cpp` | Exact timestamp-with-zone endpoint validation and normalization to nanoseconds | Snapshot resolution, time-series provider execution, result publication, transaction state |
| `canonical_relational_dag_planner.cpp` | Composition of planning-context validation, alternative enumeration, search, and immutable physical-DAG publication | Data access, execution, transaction visibility, transaction finality |

## Staged decomposition

| Stage | Scope | Required route evidence | Status |
| --- | --- | --- | --- |
| 0 | Add the dedicated source-contract gate, baseline ratchets, authority ledger, and public-entry-point ownership checks | Source gate self-test | complete |
| 1 | Extract dependency-free time-series endpoint parsing | Production SBLR link and RCP-076 production query route | complete |
| 2 | Separate shared scalar, descriptor, JSON, and runtime-memory support | Contract-only query route plus scalar/result conformance | complete |
| 3 | Separate object-free profile preparation and physical executor registrations | QRY-003/QRY-005 and object-free query matrix | in progress: profile publication, source/VALUES/table-subquery/query-DISTINCT/LIMIT-OFFSET-FETCH/global-COUNT/nonrecursive-CTE/ROW_NUMBER/peer-ranking/NTILE/navigation/aggregate-window registration, and shared registration preflight separated |
| 4 | Separate object-free composition coordinators | Set, join, aggregate, window, sort, distinct, limit, pivot and unpivot routes | pending |
| 5 | Extract time-series, vector, search, key-value, graph, and document model-family routes one family per change | The matching RCP production-route test for each family | pending |
| 6 | Extract spatial/columnar and captured multileg model composition | RCP-079/RCP-080 and cross-family join tests | pending |
| 7 | Extract generate-series and match-recognize table-function routes | Table-function and match-recognize production routes | pending |
| 8 | Extract current-heap streaming and composition execution | Engine-backed streaming and current-heap query tests | pending |
| 9 | Reduce the remaining file to route selection and public coordination | Full canonical-query label inventory | pending |

## Gate required after every stage

1. `canonical_query_execution_authority_source_contract_gate` passes and the
   ratchet for every touched module is tightened to the new measured size.
2. Contract-only and production targets compile when the changed module is in
   their dependency scope.
3. `sb_engine_sblr` links, proving moved symbols resolve through real target
   enrollment.
4. Every affected route-specific test runs; a generic build is not a substitute
   for route evidence.
5. The MGA policy gate passes. No query module gains transaction mutation or
   finality authority.
6. Temporary builds and generated artifacts are removed after evidence is
   collected.

### Contract gate status

The configured `sb_qow_sblr_query_route_contract` target now compiles and its
nine principal consumers link. The repair keeps the closure-test route on its
reduced optimizer-admission profile, moves the canonical DAG planning
composition into a focused source shared by the relevant optimizer targets,
and makes the composed contextual SBXN decoder a header-local wire adapter.
Contract compilation no longer depends on the production function registry,
mutable relation-store facade, or production-only contextual proof helper.

The direct endpoint boundary test covers exact epoch conversion, fractional
precision, positive and negative offset normalization, leap-day handling,
signed 64-bit nanosecond endpoints, malformed fields, invalid calendar dates,
offset limits, trailing input, overflow, and a null output pointer. Production
RCP-076 execution and production-route tests remain the integration evidence.

Repairing the formerly unbuildable target also makes its legacy runtime tests
executable. Any runtime-contract failures they report are Stage 2 evidence and
must not be hidden by weakening the reduced admission profile or substituting
the production optimizer route.

### Stage 2 entry evidence

The exposed contract failures have been corrected without expanding the
reduced route into parsing, transaction, or storage authority. The entry gate
now passes:

- `qow_sblr_codec_v1`
- `qow_qry_005_v1`
- `qow_qry_006_v1`
- `qow_live_values_spine_v1`
- `qow_qry_017_having_row_binding_v1`

This evidence establishes a clean baseline for the Stage 2 source separation;
it does not by itself mark that decomposition stage complete.

### Stage 2 extraction evidence

The shared query-support authority is separated into four dependency-light
modules and enrolled in both the reduced contract target and the production
`sb_engine_sblr` target. The coordinator no longer defines the extracted UUID,
scalar-wire, JSON wildcard, exact descriptor/result-shape, or runtime-memory
accounting helpers. Its ratchet is now 69,729 lines and 3,357,577 bytes, a
reduction of 1,020 lines and 38,029 bytes from the project baseline.

The dedicated boundary matrix validates UUID admission and deterministic
derivation, textual and binary canonical `int64` decoding, scalar equality-key
normalization, bounded canonical-JSON wildcard expansion, exact descriptor and
nullability behavior, overflow refusal, typed-value/batch memory accounting,
and bit-vector accounting. The complete Stage 2 entry closure passes:

- `canonical_query_shared_support_boundary_v1`
- `qow_sblr_codec_v1`
- `qow_qry_005_v1`
- `qow_qry_006_v1`
- `qow_live_values_spine_v1`
- `qow_qry_017_having_row_binding_v1`

The clean benchmark-profile build completed 1,332 actions and linked both
contract and production SBLR targets with all instrumentation families off.
The Stage 1 production continuity set also passes after the shared extraction:

- `canonical_query_time_series_endpoint_boundary_v1`
- `qow_ces05_time_series_execution_v1`
- `qow_ces05_time_series_production_route_v1`

### Stage 3 profile and leaf-registration evidence

The immutable object-free node-profile catalog and physical-DAG publication
input preparation now live in `canonical_query_object_free_profile.cpp`.
Runtime producer-memory receipts remain separate from optimizer grants, and
the new module owns no executor callback, data access, snapshot construction,
or transaction finality.

The first dependency-light physical registration boundary now lives in
`canonical_query_physical_registration.cpp`. It owns the bounded
already-materialized source callback and the canonical VALUES specialization.
Borrowed MGA authority may be revalidated before and after publication, but
the registration cannot manufacture or mutate that authority.

The second physical-registration slice moves the common execution receipt
identity check, operator-local physical-DAG projection, and construction of a
revalidation-only MGA handle into the same module. Production resolution still
calls the engine statement-snapshot resolver backed by durable transaction
inventory; the module does not begin, finalize, persist, or recover any
transaction. The reduced contract target retains only its deterministic
revocation seam.

The third slice moves the shared strict unary/binary callback preflight,
operator-local DAG copy accounting, and local retained-cost memory rebind into
the physical-registration module. These helpers consume optimizer-published
grants and admission evidence; they cannot issue a grant or change the selected
physical plan.

The fourth slice moves the bounded table-subquery materialization callback.
The callback consumes a previously selected statement snapshot, revalidates
that authority through the common physical-registration path, and verifies the
typed materialized result without creating or refreshing a snapshot.

The fifth slice starts a focused window-registration module with the
`ROW_NUMBER` callback. It retains the optimizer-published memory bound, creates
only an operator-local view of the published DAG, and either consumes borrowed
MGA authority or builds a revalidation handle over the already-selected
statement context. It cannot create, refresh, commit, or roll back a snapshot.

The sixth slice moves the bounded `NTILE` callback into the window-registration
module. It retains the exact bucket operand and optimizer-published runtime
memory grant, uses only an operator-local view of the selected physical DAG,
and constructs a revalidation handle over the engine-selected statement
context. It cannot create, refresh, commit, or roll back a snapshot.

The seventh slice moves the bounded aggregate-window callback into the same
module. It retains the exact aggregate and frame descriptors, refuses runtime
work that exceeds the optimizer-published comparison, frame-reference,
transition, or memory limits, and revalidates only the engine-selected MGA
statement context. It cannot create, refresh, commit, or roll back a snapshot.

The eighth slice moves the bounded navigation-window callback for `LAG`,
`LEAD`, `FIRST_VALUE`, `LAST_VALUE`, and `NTH_VALUE`. A narrow immutable profile
contract carries only the selected semantic identity, function identity,
display name, and result type. The callback retains exact frame and operand
bindings, refuses comparison, frame-reference, or memory work above the
optimizer-published limits, and can only revalidate the engine-selected MGA
statement context.

The ninth slice moves the bounded peer-ranking callback shared by `RANK`,
`DENSE_RANK`, `PERCENT_RANK`, and `CUME_DIST`. It retains the exact ranking
profile and peer-comparison bound, consumes optimizer-published peer metadata,
and refuses runtime comparison or memory work above the selected node's grant.
Its MGA handle only revalidates the engine-selected statement context.

The tenth slice starts a focused relational-registration module with query
`DISTINCT`. It retains only optimizer-published equality terms and runtime
bounds over an already-materialized typed batch, verifies the execution
receipt and comparison counters, and can only revalidate the engine-selected
MGA statement context.

The eleventh slice moves the bounded `LIMIT`/`OFFSET`/`FETCH FIRST` callback
into the relational-registration module. It retains the selected row bounds,
uses the dispatcher-published memory grant for object-backed execution, checks
the operator receipt and maximum output cardinality, and can only revalidate
the engine-selected MGA statement context.

The twelfth slice moves inline and materialized nonrecursive CTE publication
into the relational-registration module. It retains only the selected logical
node configuration, accounts for typed-batch copies against the
optimizer-published memory grant, validates the output descriptor identity,
and revalidates the existing engine-selected MGA statement context before and
after publication.

The thirteenth slice moves global `COUNT(*)` into the relational-registration
module. It accepts either the bounded materialized input or the exact
engine-published streaming cardinality, verifies aggregate execution and
memory receipts against the selected physical node, and can only revalidate
the existing engine-selected MGA statement context.

These Stage 3 slices reduce the coordinator to 67,040 lines and 3,230,784
bytes, removing another 2,689 lines and 126,793 bytes. The first slice's clean
benchmark-profile build completed 1,332 actions with all instrumentation
families off, followed by an 11-action incremental link proof for the physical
registration extraction. The affected object-free matrix passes:

- `qow_sblr_codec_v1`
- `qow_qry_005_v1`
- `qow_qry_006_v1`
- `qow_live_values_spine_v1`
- `qow_qry_017_having_row_binding_v1`

The second slice completed a fresh 1,293-action benchmark-profile build graph
across its incremental compile repairs and linked both
`sb_qow_sblr_query_route_contract` and `sb_engine_sblr`. The same five-test
object-free matrix passed again. The canonical-query source authority gate,
the repository-wide MGA policy gate, and `git diff --check` also pass. Build,
test, and configure output is retained in the corresponding
`/tmp/scratchbird-canonical-query-stage3b-*.log` files.

The third slice completed another fresh 1,293-action benchmark-profile build
graph across one incremental resume after making the join callback's legitimate
local cost-rebind dependency explicit. Both contract and production SBLR
libraries linked, the same five-test object-free matrix passed, and the source
authority, MGA policy, and diff gates passed. Its evidence logs are retained as
`/tmp/scratchbird-canonical-query-stage3c-*.log`.

The fourth slice completed a fresh 1,293-action benchmark-profile build with
all instrumentation families off. Both contract and production SBLR libraries
linked, followed by a 43-action build of the affected test executables. The
prior five-test object-free matrix and the dedicated `qow_qry_013_table_v1`
table-subquery test all passed. Configure, build, test-build, and CTest output
is retained in `/tmp/scratchbird-canonical-query-stage3d-*.log`.

The fifth slice completed a fresh 1,295-action benchmark-profile build with all
instrumentation families off and linked both the contract and production SBLR
libraries. A 45-action build produced the seven affected test executables. The
live VALUES composition route passed with the extracted `ROW_NUMBER` callback;
the direct window-executor fixture was then aligned with the canonical
datatype/codec identity contract by binding the runtime type UUID instead of
the catalog descriptor UUID. The five-test object-free matrix and the dedicated
`qow_qry_007_window_v1` and `qow_qry_013_table_v1` tests all pass. Configure,
build, test-build, focused rebuild, and CTest output is retained in
`/tmp/scratchbird-canonical-query-stage3e-*.log`.

The sixth slice completed a fresh 1,428-action benchmark-clean production
build and linked `sb_engine_sblr`, followed by an isolated 575-action QOW
contract build that linked `sb_qow_sblr_query_route_contract`. All four trace
families were disabled in both configurations. A 52-action focused build
produced the established seven object-free/window executables plus
`qow_win_006_test_v1`; all eight tests passed, including the dedicated `NTILE`
bucket-boundary and invalid-operand matrix. Configure, production build,
contract build, focused test-build, and CTest output is retained in
`/tmp/scratchbird-canonical-query-stage3f-*.log`.

The seventh slice completed a fresh 1,295-action benchmark-clean closure build
with all four instrumentation families disabled and linked both
`sb_qow_sblr_query_route_contract` and `sb_engine_sblr`. A 51-action focused
build produced the established query-route executables plus aggregate-window
state, all-registry, and frame coverage. That matrix exposed two stale test
helpers that still bound the catalog descriptor UUID as the fixed `int64`
result type UUID; both now perform the same catalog/codec identity lookup as
production, with descriptor identity retained only when no separate codec row
exists. A six-action focused rebuild then passed the direct aggregate-registry
and both affected window tests, and the final eleven-test query/window matrix
passed in full. Configure, clean build, focused builds, initial failure
inventory, repaired tests, final CTest output, and source-policy results are
retained in `/tmp/scratchbird-canonical-query-stage3g-*.log`.

The eighth slice completed a fresh 1,295-action benchmark-clean closure build
with all four instrumentation families disabled and linked both contract and
production SBLR libraries. The extracted navigation module and reduced
coordinator compiled in both targets without repair. A 67-action focused build
produced eighteen tests covering the established query route, direct aggregate
registry, `ROW_NUMBER`, `NTILE`, all five navigation functions, and aggregate
window state/registry/frame behavior; all eighteen passed. Configure, clean
build, focused test-build, CTest, and policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3h-*.log`.

The ninth slice completed a fresh 1,295-action benchmark-clean closure build
with all four instrumentation families disabled and linked both contract and
production SBLR libraries. A 67-action focused build produced the established
eighteen-test query/window matrix. All eighteen passed, including
`qow_win_006_v1` coverage for `RANK`, `DENSE_RANK`, `PERCENT_RANK`, and
`CUME_DIST` peer semantics, comparison bounds, invalid inputs, and forged peer
metadata refusal. Configure, clean build, focused test-build, CTest, and final
policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3i-*.log`.

The tenth slice completed a fresh 1,297-action benchmark-clean closure build
with all four instrumentation families disabled and linked both contract and
production SBLR libraries. A 69-action focused build produced nineteen tests
covering the established query/window matrix, direct typed query `DISTINCT`,
and the production live-values composition route. All nineteen passed,
including `qow_qry_010_v1` and `qow_live_values_spine_v1`. Configure, clean
build, focused test-build, CTest, and final policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3j-*.log`.

The eleventh slice completed a fresh 1,297-action benchmark-clean closure
build with all four instrumentation families disabled and linked both contract
and production SBLR libraries. A 73-action focused build produced twenty-one
tests covering the established query/window matrix, direct `LIMIT`/`OFFSET`,
direct `FETCH FIRST`, and the production live-values composition route. All
twenty-one passed, including `qow_qry_007_sort_limit_v1`,
`qow_qry_010_fetch_top_profile_v1`, and `qow_live_values_spine_v1`.
Configure, clean build, focused test-build, CTest, and final policy-gate output
is retained in `/tmp/scratchbird-canonical-query-stage3k-*.log`.

The twelfth slice completed a fresh 1,297-action benchmark-clean closure build
with all four instrumentation families disabled and linked both contract and
production SBLR libraries. The established 73-action focused build produced
twenty-one query/window tests; all twenty-one passed. The
`qow_live_values_spine_v1` production-composition route includes both inline
and materialized nonrecursive CTE publication, and the direct DISTINCT,
LIMIT/OFFSET, FETCH FIRST, and window matrix also remained green. Configure,
clean build, focused test-build, CTest, and final policy-gate output is retained
in `/tmp/scratchbird-canonical-query-stage3l-*.log`.

The thirteenth slice completed a fresh 1,297-action benchmark-clean closure
build with all four instrumentation families disabled and linked both contract
and production SBLR libraries. A 75-action focused build produced twenty-two
query/window tests; all twenty-two passed. The matrix adds the direct
`qow_qry_007_aggregate_v1` COUNT(*) executor test to the established suite,
while `qow_live_values_spine_v1` covers bounded materialized and exact
streaming-cardinality production compositions. Configure, clean build, focused
test-build, CTest, and final policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3m-*.log`.

Stage 3 remains open until the remaining object-free physical callback
factories have been moved behind the same authority boundary and the complete
matrix has been rerun.

## Review rule

Each stage should be reviewable as one authority move. If a proposed extraction
requires broad access to unrelated private types, first create a narrow
internal contract and stop; do not expose the monolith's anonymous namespace as
a general-purpose API merely to make the move compile.
