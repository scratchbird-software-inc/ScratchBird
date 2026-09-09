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
| `canonical_query_aggregate_composition.cpp` | Admitted object-free grouped COUNT/SUM/HAVING and global aggregate coordination across canonical aggregate profiles | Snapshot construction, transaction finality, storage reads, parser lowering, public route selection |
| `canonical_query_aggregate_registration.cpp` | Exact aggregate datatype, value/key binding, FILTER truth, equality-authority, and grouped/global aggregate callback registration over bounded typed inputs | Plan selection, snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_correlated_registration.cpp` | Bounded comparison-authority binding and correlated-subquery/LATERAL/APPLY registrations over two already-materialized typed inputs | Plan selection, snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_filter_registration.cpp` | Exact-cardinality and bounded object-heap three-valued `FILTER` registrations over already-materialized typed batches | Predicate-receipt construction, snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_filter_project_composition.cpp` | Admitted object-free FILTER/PROJECT coordination, including standalone, combined, SORT, DISTINCT, LIMIT, OFFSET, and FETCH profiles | Snapshot construction, transaction finality, storage reads, parser lowering, public route selection |
| `canonical_query_order_limit_composition.cpp` | Admitted standalone object-free SORT, LIMIT, and DISTINCT/SORT/LIMIT/OFFSET/FETCH coordination | Snapshot construction, transaction finality, storage reads, parser lowering, public route selection |
| `canonical_query_pivot_composition.cpp` | Admitted object-free PIVOT/UNPIVOT coordination and route-exclusive execution-receipt validation | Snapshot construction, transaction finality, storage reads, parser lowering, public route selection |
| `canonical_query_object_free_composition_support.cpp` | Shared object-free VALUES materialization and canonical API success/refusal publication used by composition coordinators | Plan selection, physical execution, data access, snapshot construction, transaction finality, parser lowering |
| `canonical_query_object_free_profile.cpp` | Immutable object-free executor capability projection, physical-DAG planning input construction, and separately carried runtime-memory receipts | Physical callback execution, data access, MGA visibility/finality |
| `canonical_query_physical_registration.cpp` | Revalidation-only MGA authority handles, operator-local physical-DAG scoping and memory preflight, execution-receipt identity checks, cancellation-policy evidence adaptation, and bounded already-materialized source/VALUES registrations | Snapshot construction, transaction begin/commit/rollback/recovery, optimizer grant authority, storage reads, expression evaluation |
| `canonical_query_predicate_support.cpp` | Conservative payload and structural scratch bounds for admitted row-predicate expressions shared by FILTER and JOIN registrations | Predicate evaluation, optimizer grant authority, data access, snapshot construction, transaction finality |
| `canonical_query_projection_registration.cpp` | Direct-column and expression `PROJECT` registration plus descriptor-exact expression materialization over one bounded, already-materialized typed input | Plan selection, snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_join_registration.cpp` | Bounded JOIN-kind registration, runtime predicate evaluation, memory preflight, cancellation handling, and exact result receipt validation over two already-materialized typed inputs | Plan selection, snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_join_composition.cpp` | Admitted object-free JOIN profile recognition, bounded predicate materialization, physical-DAG assembly, and result publication requests over typed VALUES inputs | Snapshot construction, transaction finality, storage reads, parser lowering, public route selection |
| `canonical_query_join_pipeline_composition.cpp` | Admitted object-free INNER JOIN/FILTER/PROJECT pipeline coordination, including optional DISTINCT, SORT, LIMIT, OFFSET, and FETCH tails | Snapshot construction, transaction finality, storage reads, parser lowering, public route selection |
| `canonical_query_node_composition.cpp` | Admitted node-driven object-free composition across unary tails, joins, set operations, recursive/correlated subqueries, aggregates, and windows | Snapshot construction, transaction finality, storage reads, parser lowering, public route selection |
| `canonical_query_recursive_registration.cpp` | Bounded signed-`int64` recursive-term preparation/execution, recursive-root memory binding, exact node binding, descriptor-only term registration, and recursive UNION/SEARCH/CYCLE root registration | Plan selection, snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_relational_registration.cpp` | Bounded relational physical registrations over already-materialized typed batches, including direct descriptor projection, scalar/row cardinality and predicate subqueries, query `DISTINCT`, typed `SORT`, `MATCH_RECOGNIZE`, `LIMIT`/`OFFSET`/`FETCH FIRST`, global `COUNT(*)`, and nonrecursive CTE publication | Snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_set_registration.cpp` | Binary set-operation registration and execution/memory receipt validation over two bounded, already-materialized typed inputs | Plan selection, snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_set_composition.cpp` | Admitted object-free binary and nested set-operation coordination, bounded planning inputs, physical-DAG assembly, and result publication requests | Snapshot construction, transaction finality, storage reads, parser lowering, public route selection |
| `canonical_query_sort_registration.cpp` | Expression-key materialization, exact sort-key receipt issuance, and expression-aware SORT registration over one bounded typed input | Plan selection, snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_window_registration.cpp` | Bounded window physical registrations for row numbering, peer ranking/distribution, bucketing, navigation, and aggregate frames | Snapshot construction, transaction finality, optimizer grant authority, storage reads, parser lowering |
| `canonical_query_scalar_support.cpp` | Canonical UUID validation/derivation, scalar equality-key normalization, and exact `int64` wire decoding | Catalog-bound comparison, descriptor construction, provider execution, transaction state |
| `canonical_query_descriptor_support.cpp` | Exact descriptor/result-shape comparison, encoded descriptor-field lookup, and result-nullability projection | Catalog lookup, persisted descriptor authorization, descriptor construction, transaction state |
| `canonical_query_json_support.cpp` | Bounded wildcard scanning over already-canonical JSON values | SQL/JSON parsing, document-provider execution, result publication, transaction state |
| `canonical_query_runtime_memory_support.cpp` | Overflow-safe logical/live memory accounting over typed values and descriptor batches | Memory grants, plan selection, storage accounting, MGA visibility/finality |
| `canonical_query_time_series_endpoint.cpp` | Exact timestamp-with-zone endpoint validation and normalization to nanoseconds | Snapshot resolution, time-series provider execution, result publication, transaction state |
| `canonical_query_time_series_composition.cpp` | Admitted production time-series source execution and its bounded relational composition over engine-issued statement authority | Transaction begin/commit/rollback/recovery, parser lowering, unrelated model-family routes, public route selection |
| `canonical_query_vector_composition.cpp` | Admitted production vector-nearest source execution over engine-issued statement authority, including bounded filter, metric, provider, result-shape, and selected-plan coordination | Transaction begin/commit/rollback/recovery, parser lowering, unrelated model-family routes, public route selection |
| `canonical_query_search_composition.cpp` | Admitted production search source execution and its bounded relational composition over engine-issued statement authority, including recursive, set, aggregate, window, sort, limit, and mixed-join tails | Transaction begin/commit/rollback/recovery, parser lowering, unrelated model-family routes, public route selection |
| `canonical_query_key_value_composition.cpp` | Admitted production key-value source execution and its bounded relational composition over engine-issued statement authority, including recursive, set, aggregate, window, sort, limit, and mixed-join tails | Transaction begin/commit/rollback/recovery, parser lowering, unrelated model-family routes, public route selection |
| `canonical_query_graph_composition.cpp` | Admitted production graph match/expand source execution and its bounded relational composition over engine-issued statement authority, including recursive, set, aggregate, window, sort, limit, and mixed-join tails | Transaction begin/commit/rollback/recovery, parser lowering, unrelated model-family routes, public route selection |
| `canonical_query_document_composition.cpp` | Admitted production document expression-unnest and persisted document-path source execution with bounded relational composition over engine-issued statement authority | Transaction begin/commit/rollback/recovery, parser lowering, unrelated model-family routes, public route selection |
| `canonical_relational_dag_planner.cpp` | Composition of planning-context validation, alternative enumeration, search, and immutable physical-DAG publication | Data access, execution, transaction visibility, transaction finality |

## Staged decomposition

| Stage | Scope | Required route evidence | Status |
| --- | --- | --- | --- |
| 0 | Add the dedicated source-contract gate, baseline ratchets, authority ledger, and public-entry-point ownership checks | Source gate self-test | complete |
| 1 | Extract dependency-free time-series endpoint parsing | Production SBLR link and RCP-076 production query route | complete |
| 2 | Separate shared scalar, descriptor, JSON, and runtime-memory support | Contract-only query route plus scalar/result conformance | complete |
| 3 | Separate object-free profile preparation and physical executor registrations | QRY-003/QRY-005 and object-free query matrix | complete |
| 4 | Separate object-free composition coordinators | Set, join, aggregate, window, sort, distinct, limit, pivot and unpivot routes | complete |
| 5 | Extract time-series, vector, search, key-value, graph, and document model-family routes one family per change | The matching RCP production-route test for each family | complete |
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

The fourteenth slice moves the object-free typed `SORT` callback into the
relational-registration module and assigns its shared cancellation adapter to
the physical-registration support boundary. The callback retains exact order
terms and tie evidence, refuses comparison or workspace use above the
optimizer-published bounds, binds one exact cancellation-policy evidence row,
and can only revalidate the engine-selected MGA statement context.

The fifteenth slice moves the bounded object-free `MATCH_RECOGNIZE` callback
into the relational-registration module. It retains only the selected A+
row-pattern bounds, orders one exact canonical `int64` input, accounts its
workspace against the optimizer-published memory grant, binds the selected
cancellation-policy evidence, and can only revalidate the engine-selected MGA
statement context. Its cancellation state machine remains private to the
relational module; callbacks not yet extracted retain their own private copy.

The sixteenth slice moves scalar and row cardinality-subquery registration
into the relational-registration module behind a narrow immutable profile.
The callback accepts only the selected bounded materialized typed batch,
preserves exact descriptor and value identity, verifies the canonical
subquery execution receipt, and can only construct and revalidate authority
for the engine-selected MGA statement context.

The seventeenth slice moves bounded predicate-subquery registration and its
independent causal truth verifier into the relational-registration module.
The callback retains only the selected `EXISTS`, `IN`, `ANY`, or `ALL`
profile, consumes engine-provided typed comparison authority when required,
verifies three-valued truth and execution receipts, and can only construct
and revalidate authority for the engine-selected MGA statement context.

The eighteenth slice moves bounded direct descriptor-projection registration
into the relational-registration module. The callback retains only the
selected descriptor ordinals and optional logical-node configurations,
consumes the optimizer-published memory grant, and either borrows MGA authority
or constructs only a revalidation handle over the engine-selected statement
context.

The nineteenth slice removes the unreferenced legacy object-heap `SORT`
registration factory from the coordinator. No planner or production route
selected that factory; active canonical typed and expression `SORT`
registration remains owned by the relational path. Removing the unreachable
callback also removes a redundant path that could construct a revalidation
handle without changing the active engine-selected MGA authority model.

The twentieth slice creates a narrow predicate-support boundary and moves the
conservative expression payload and structural scratch analyzer used by both
FILTER and JOIN registrations. The analyzer consumes immutable relational DAG
and row-binding inputs, publishes only bounded byte counts or a refusal, and
owns no predicate evaluation, optimizer grant, data-access, snapshot, or
transaction-finality authority. This isolates the remaining active FILTER
registration from its largest shared planning dependency before that callback
is moved.

The twenty-first slice creates the focused filter-registration boundary and
moves both exact-cardinality and bounded object-heap `FILTER` callback
factories. The callbacks retain only immutable predicate bindings, typed
relational inputs, runtime expression services, and optimizer-published row and
memory ceilings. Predicate evaluation remains reachable solely through the
private receipt-issuing bridge, while physical scoping and MGA handling remain
limited to operator-local revalidation of the engine-selected statement
context. The module cannot construct a snapshot or begin, commit, roll back,
persist, or recover a transaction.

The twenty-second slice creates a focused projection-registration boundary and
moves the direct-column and expression `PROJECT` callback factory together
with descriptor-exact expression materialization. The coordinator publishes a
narrow immutable profile containing only selected column ordinals, expression
bindings, and output descriptors; preparation diagnostics, result bindings,
and broader prepared-root state do not cross the boundary. The callback
consumes only an already-materialized typed input and optimizer-published
memory bounds, while MGA handling remains limited to revalidation of the
engine-selected statement context. The module cannot select a plan, construct
a snapshot, access storage, or begin, commit, roll back, persist, or recover a
transaction.

The twenty-third slice creates a focused correlated-registration boundary and
moves the shared bounded comparison-authority binder together with ordinary
correlated-subquery and `LATERAL`/`APPLY` callback factories. The coordinator
and the module share one immutable profile containing only selected correlation
columns, descriptor identities, admitted row/pair/output bounds, comparison
memory, and implementation identity. Both callbacks consume two
already-materialized typed inputs, poll the admitted cancellation policy, and
either construct only a revalidation handle or borrow the engine-selected MGA
authority. The module cannot select a plan, construct a snapshot, access
storage, or begin, commit, roll back, persist, or recover a transaction.

The twenty-fourth slice creates a focused binary set-operation registration
boundary and moves the `UNION`/`INTERSECT`/`EXCEPT` callback together with its
execution and logical-memory receipt validators. The coordinator publishes a
narrow immutable node map containing only the admitted set semantics, result
columns, collation bindings, implementation identity, and hard comparison and
output bounds; preparation diagnostics and result bindings do not cross the
boundary. The callback consumes two already-materialized typed inputs and an
optimizer-published operator-local memory grant, while MGA handling is limited
to constructing a revalidation handle for the engine-selected statement
context. The module cannot select a plan, construct a snapshot, access
storage, or begin, commit, roll back, persist, or recover a transaction.

The twenty-fifth slice starts a focused recursive-registration boundary by
moving the immutable recursive profile and bounded signed-`int64` term
preparation, execution, node-binding, and descriptor-only leaf callback. The
callback emits no rows itself: it publishes the exact empty typed term schema
and revalidates only the engine-selected MGA statement context. Recursive
working-table iteration remains owned by the coordinator until the larger root
callback can move as one authority unit. The module cannot select a plan,
construct a snapshot, access storage, or begin, commit, roll back, persist, or
recover a transaction.

These Stage 3 slices reduce the coordinator to 63,172 lines and 3,053,444
bytes, removing another 6,557 lines and 304,133 bytes. The first slice's clean
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

The fourteenth slice completed a fresh 1,298-action benchmark-clean closure
build after integrating the current upstream trigger/schema work, with all
four instrumentation families disabled. Both contract and production SBLR
libraries linked. The established 75-action focused build produced twenty-two
query/window tests; all twenty-two passed, including direct typed sort/limit
coverage in `qow_qry_007_sort_limit_v1` and production sort, project-sort,
filter-project-sort, join-sort, DISTINCT-sort, LIMIT, OFFSET, and FETCH
compositions in `qow_live_values_spine_v1`. Configure, clean build, focused
test-build, CTest, and final policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3n-*.log`.

The fifteenth slice completed the fresh 1,298-action benchmark-clean closure
graph after one narrow compile repair that made the existing canonical
`int64` decoder dependency explicit. All four instrumentation families were
disabled, and both contract and production SBLR libraries linked. The
established 75-action focused build produced twenty-two query/window tests;
all twenty-two passed. The dedicated full-product
`qow_match_recognize_generate_series_e2e_v1` route remains registered but is
not enrolled by the intentionally isolated QOW closure configuration used for
this build. Configure, closure build, focused test-build, CTest, and final
policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3o-*.log`.

The sixteenth slice completed a fresh 1,298-action benchmark-clean closure
build with all four instrumentation families disabled, and both contract and
production SBLR libraries linked. A 79-action focused build produced
twenty-four query/window tests; all twenty-four passed. The matrix adds direct
scalar- and row-cardinality execution coverage in `qow_qry_013_scalar_v1` and
`qow_qry_013_row_v1`, while `qow_live_values_spine_v1` continues to cover the
live production composition route. Configure, closure build, focused
test-build, CTest, and final policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3p-*.log`.

The seventeenth slice completed a fresh 1,298-action benchmark-clean closure
build with all four instrumentation families disabled, and both contract and
production SBLR libraries linked. An 83-action focused build produced
twenty-six query/window tests; all twenty-six passed. The matrix adds direct
`EXISTS` and quantified-subquery execution coverage in
`qow_qry_013_exists_v1` and `qow_qry_013_quantified_v1`, while
`qow_live_values_spine_v1` continues to cover the live production composition
route. Configure, closure build, focused test-build, CTest, and final
policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3q-*.log`.

The eighteenth slice completed a fresh 1,298-action benchmark-clean closure
build with all four instrumentation families disabled, and both contract and
production SBLR libraries linked. A 107-action focused build produced
twenty-nine tests; all twenty-nine passed. In addition to the established
query/window matrix, `qow_ces05_time_series_production_route_v1`,
`qow_ces05_search_production_route_v1`, and
`qow_ces05_multimodel_production_route_v1` exercise direct descriptor
projection through independent production composition paths. Configure,
closure build, focused test-build, CTest, and final policy-gate output is
retained in `/tmp/scratchbird-canonical-query-stage3r-*.log`.

The nineteenth slice completed a fresh 1,298-action benchmark-clean closure
build with all four instrumentation families disabled, and both contract and
production SBLR libraries linked without the unreferenced object-heap `SORT`
factory. A 107-action focused build produced twenty-nine tests; all twenty-nine
passed. Direct typed `SORT`/`LIMIT` coverage and the live production
row-dependent, project-sort, filter-project-sort, join-sort, distinct-sort,
limit, offset, and fetch compositions continue to exercise the active
relational registrations. Configure, closure build, focused test-build, CTest,
and final policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3s-*.log`.

The twentieth slice completed a fresh 1,300-action benchmark-clean closure
build with all four instrumentation families disabled, and both contract and
production SBLR libraries linked with predicate scratch analysis behind its
narrow shared support contract. A 117-action focused build produced
thirty-four tests spanning dedicated `FILTER` and `JOIN` executors, live
row-dependent compositions, window filtering, and the production cross-family
JOIN route. The expanded matrix exposed one stale direct JOIN fixture whose
serialized descriptor said `non_null` while its executor column still said
nullable. Aligning that test carrier with exact descriptor identity required
no runtime change; the focused recheck and all thirty-four tests then passed.
Configure, closure build, focused test-build, initial failure, recheck, final
CTest, and final policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3t-*.log`.

The twenty-first slice completed a fresh 1,302-action benchmark-clean closure
build with all four instrumentation families disabled. The focused FILTER
module and reduced coordinator compiled in both profiles, and both contract
and production SBLR libraries linked. A 117-action focused build produced the
same thirty-four-test matrix used for the predicate-support boundary; all
thirty-four passed, including direct FILTER and HAVING execution, window
filtering, live row-dependent FILTER/JOIN compositions, and the production
cross-family JOIN route. Configure, closure build, focused test-build, CTest,
and final policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3u-*.log`.

The twenty-second slice completed a fresh 1,304-action benchmark-clean closure
build with all four instrumentation families disabled. The focused PROJECT
module and reduced coordinator compiled in both profiles, and both contract
and production SBLR libraries linked. A 117-action focused build produced the
same thirty-four-test matrix used for the predicate-support and FILTER
boundaries; all thirty-four passed. The live VALUES spine directly covered
row-dependent expression projection, empty filtered projection, and PROJECT
compositions with FILTER, SORT, DISTINCT, LIMIT/OFFSET/FETCH, JOIN, aggregate,
and set-operation routes. Configure, closure build, focused test-build, CTest,
and final policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3v-*.log`.

The twenty-third slice completed a fresh 1,306-action benchmark-clean closure
graph across one targeted mechanical compile repair and resume, with all four
instrumentation families disabled. The focused correlated/LATERAL registration
module and reduced coordinator compiled in both profiles, and both contract
and production SBLR libraries linked. A 123-action focused build produced
thirty-seven tests; all thirty-seven passed. Dedicated correlated and
LATERAL/APPLY executors, the live VALUES spine's typed and integer variants,
and the spatial-columnar production route directly exercise the comparison and
row-registration authority moved in this slice. Configure, closure build and
repair diagnostics, focused test-build, CTest, and final policy-gate output is
retained in `/tmp/scratchbird-canonical-query-stage3w-*.log`.

The twenty-fourth slice completed an uninterrupted fresh 1,308-action
benchmark-clean closure build with all four instrumentation families disabled.
The focused set-operation module and reduced coordinator compiled in both
profiles, and both contract and production SBLR libraries linked. A 135-action
focused build produced forty-three tests; all forty-three passed. The matrix
adds the six dedicated QRY-016 ALL, DISTINCT, BY NAME, nesting, type
reconciliation, and NULL/collation executors, while the live VALUES spine and
the time-series and search production routes exercise the extracted binary
set-operation registration through the canonical coordinator. Configure,
closure build, focused target-resolution diagnostic and test-build, CTest, and
final policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3x-*.log`.

The twenty-fifth slice completed an uninterrupted fresh 1,310-action
benchmark-clean closure build with all four instrumentation families disabled.
The recursive-term module and reduced coordinator compiled in both profiles,
and both contract and production SBLR libraries linked. A 147-action focused
build produced forty-nine tests; the first run exposed a stale standalone
SEARCH/CYCLE fixture that still encoded catalog descriptor UUIDs where the
executor now requires datatype codec type UUIDs. Aligning that test carrier
with the executor's canonical type-codec lookup required no runtime change.
The focused recheck and final forty-nine-test matrix then passed, including all
six QRY-014 working, UNION, SEARCH/CYCLE, resource, cancellation, and MGA
executors; the live VALUES spine; and five production routes. Configure, clean
closure build, focused build, initial failure, fixture recheck, final CTest,
and policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3y-*.log`.

The twenty-sixth slice completes the recursive-registration boundary by moving
the recursive root profile, peak payload and resident-structure binding, and
the full UNION/SEARCH/CYCLE callback beside the already-separated recursive
term. The callback consumes exact optimizer-published resource and cancellation
evidence, scopes only the selected physical subgraph, and revalidates the
engine-selected MGA statement context without creating or finalizing it.

The twenty-seventh slice creates the focused JOIN-registration boundary. It
moves the full bounded JOIN callback and its per-node runtime predicate profile
as one authority unit, including retained-memory preflight, cancellation
polling, three-valued predicate evaluation, join-kind execution, descriptor
rebinding, and receipt validation. It cannot select a plan, read storage, or
alter MGA transaction authority.

The twenty-eighth slice creates the expression SORT-registration boundary. It
moves expression-key materialization, the private one-shot sort-key receipt
issuer, and the complete expression-aware SORT callback together. The module
validates exact logical/physical order identity, accounts key and sorting
workspace against the published grant, binds cancellation evidence, and only
revalidates the supplied MGA statement context.

The twenty-ninth slice creates the aggregate-registration boundary and closes
Stage 3. It moves exact aggregate datatype and value/key binding, FILTER truth
materialization, descriptor equality authority, and both global registry and
grouped COUNT/SUM callbacks. The callbacks consume bounded typed input and
optimizer-published memory/comparison limits, verify runtime receipts, and
cannot construct a snapshot or begin, commit, roll back, persist, or recover a
transaction.

After these moves, no Stage 3 physical callback factory remains defined in the
coordinator. Captured-model/RCP-079 callbacks remain deliberately assigned to
Stage 6 rather than being mixed into this object-free registration stage.

The completed Stage 3 layout reduces the coordinator to 59,207 lines and
2,868,800 bytes: 10,522 lines and 488,777 bytes below the Stage 2 exit, and
11,542 lines and 526,806 bytes below the project baseline. A fresh 1,316-action
benchmark-clean closure graph compiled the final nineteen-module source layout
with all four instrumentation families disabled and linked both
`sb_qow_sblr_query_route_contract` and `sb_engine_sblr`. A 147-action focused
build then produced the complete forty-nine-test query, recursive, set,
aggregate, JOIN, SORT, window, correlated, and production-route matrix; all
forty-nine tests passed. Configure, clean build, focused build, CTest, and final
policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage3-final-*.log`.

### Stage 4 object-free composition evidence

The first Stage 4 slice creates a narrow shared composition-support boundary.
It moves VALUES materialization and canonical API success/refusal publication
out of the coordinator without moving plan selection, physical-DAG execution,
storage access, snapshot construction, parser lowering, or transaction
finality. The coordinator is reduced to 58,890 lines and 2,854,601 bytes; the
new support module is 343 lines and 14,902 bytes.

A fresh 1,321-action benchmark-clean closure graph compiled the twenty-module
layout in both contract and production profiles with all four instrumentation
families disabled, and linked both `sb_qow_sblr_query_route_contract` and
`sb_engine_sblr`. A 39-action focused build produced the codec, QRY-005,
QRY-006, and live VALUES-spine tests. The three query-route tests passed. The
codec test exposed an unrelated pre-existing baseline mismatch introduced by
`f54bfdb0b`: the runtime now admits operation aliases sharing one exact numeric
code and mnemonic, while the older test still classifies every repeated code
as ambiguous. No Stage 4 source participates in that failing path. Configure,
clean build, focused build, CTest, source-authority gate, MGA-policy gate, and
diff-check output is retained in
`/tmp/scratchbird-canonical-query-stage4a-*.log`.

The second Stage 4 slice moves both the binary and nested object-free
set-operation coordinators behind a focused internal contract. The module owns
bounded set planning inputs, physical-DAG assembly, executor registration, and
canonical result-publication requests for already-admitted VALUES inputs. A
narrow gateway in the coordinator retains selected-DAG execution and MGA
revalidation authority; the extracted module cannot create a snapshot, access
storage, or publish transaction finality. The coordinator is reduced to
58,124 lines and 2,819,730 bytes; the new composition module is 820 lines and
36,819 bytes.

A fresh 1,323-action benchmark-clean closure graph compiled the twenty-one
module layout in both contract and production profiles with all four
instrumentation families disabled, and linked both
`sb_qow_sblr_query_route_contract` and `sb_engine_sblr`. A 16-action focused
build produced the live VALUES spine and all seven QRY-016 executables. All
eight tests passed, covering ALL, arity refusal, BY NAME, DISTINCT, nesting,
NULL/collation equality, type reconciliation, and live set composition.
Configure, clean build, focused build, CTest, and final policy-gate output is
retained in `/tmp/scratchbird-canonical-query-stage4b-*.log`.

The third Stage 4 slice moves the standalone object-free JOIN coordinator and
its bounded root-preparation carrier behind a focused internal contract. The
module assembles already-admitted VALUES inputs, installs the exact physical
JOIN callback, and requests canonical result publication. Selected-DAG
execution and MGA revalidation remain behind the coordinator gateway; the
extracted module cannot select a plan, create a snapshot, access storage, or
publish transaction finality. The coordinator is reduced to 57,656 lines and
2,798,713 bytes; the new JOIN composition module is 523 lines and 22,815
bytes.

A fresh 1,325-action benchmark-clean closure graph compiled the twenty-two
module layout in both contract and production profiles with all four
instrumentation families disabled, and linked both
`sb_qow_sblr_query_route_contract` and `sb_engine_sblr`. A six-action focused
build produced the direct JOIN executor, live VALUES spine, and production
cross-family JOIN route. All three tests passed. The live spine covers INNER
and CROSS execution, row-dependent predicates, duplicate multiplicity,
empty/refusal behavior, deterministic evidence, and downstream JOIN
compositions. Configure, clean build, focused build, CTest, and final
policy-gate output is retained in
`/tmp/scratchbird-canonical-query-stage4c-*.log`.

The fourth Stage 4 slice moves the combined object-free INNER
JOIN/FILTER/PROJECT coordinator, including its optional DISTINCT, SORT, LIMIT,
OFFSET, and FETCH tails, behind a focused internal contract. Narrow
preparation gateways expose already-bound filter, projection, distinct, sort,
limit, and row-bound carriers without exposing the coordinator's anonymous
namespace. Selected-DAG execution and MGA revalidation remain behind the
coordinator gateway; the extracted module cannot create a snapshot, access
storage, or publish transaction finality. The coordinator is reduced to
56,911 lines and 2,764,072 bytes; the new JOIN-pipeline composition module is
831 lines and 38,130 bytes.

A fresh 1,327-action benchmark-clean closure graph completed across one
targeted forward-declaration repair and resume. The twenty-three-module layout
compiled in both contract and production profiles with all four
instrumentation families disabled, and linked both
`sb_qow_sblr_query_route_contract` and `sb_engine_sblr`. A six-action focused
build produced the direct JOIN executor, live VALUES spine, and production
cross-family JOIN route. All three tests passed; the live spine directly
covers RCP-041 through RCP-045, including filtered/projected INNER JOIN,
ordered and limited variants, query DISTINCT, OFFSET, and FETCH. Configure,
clean build and repair diagnostics, focused build, CTest, and final policy-gate
output is retained in `/tmp/scratchbird-canonical-query-stage4d-*.log`.

The fifth Stage 4 slice moves the seven related standalone object-free
FILTER/PROJECT coordinators behind one focused internal contract: FILTER,
PROJECT, FILTER/PROJECT, PROJECT/SORT, FILTER/PROJECT/SORT,
FILTER/PROJECT/SORT/LIMIT, and
FILTER/PROJECT/DISTINCT/SORT/LIMIT/OFFSET/FETCH. The module owns bounded
composition assembly and registration for already-admitted VALUES inputs. A
narrow descriptor-direct projection gateway retains coordinator preparation,
selected-DAG execution, and MGA revalidation authority. The extracted module
cannot select a plan, create a snapshot, access storage, or publish transaction
finality. The coordinator is reduced to 54,522 lines and 2,654,624 bytes; the
new composition module is 2,448 lines and 111,926 bytes.

A fresh 1,329-action benchmark-clean closure graph compiled the twenty-four
module layout in both contract and production profiles with all four
instrumentation families disabled, and linked both
`sb_qow_sblr_query_route_contract` and `sb_engine_sblr`. A six-action focused
build produced the live VALUES spine, direct FILTER, and direct SORT/LIMIT
executables. All three tests passed. The live spine directly covers RCP-031,
RCP-032, and RCP-034 through RCP-040, including empty filtered expression
projection, DISTINCT, OFFSET, and FETCH composition. The clean graph retained
pre-existing narrowing warnings in unrelated SBLR codec sources and one GCC
STL inlining warning from the contextual-text sidecar; no changed Stage 4
source emitted a warning. Configure, clean build, focused build, CTest,
source-authority gate, MGA-policy gate, instrumentation check, and diff-check
output is retained in
`/tmp/scratchbird-canonical-query-stage4e-*.log`.

The sixth Stage 4 slice moves the standalone object-free LIMIT, SORT, and
DISTINCT/SORT/LIMIT/OFFSET/FETCH coordinators behind a focused internal
contract. A narrow expression-sort preparation gateway joins the existing
descriptor sort, distinct, limit, row-bound, and selected-DAG gateways. The
module owns bounded composition assembly and executor registration for
already-admitted VALUES inputs; it cannot select a plan, create a snapshot,
access storage, or publish transaction finality. The coordinator is reduced
to 53,615 lines and 2,613,775 bytes; the new composition module is 969 lines
and 43,206 bytes.

A fresh 1,331-action benchmark-clean closure graph completed across one
targeted missing scalar-support include repair and a 784-action resume. The
twenty-five-module layout compiled in both contract and production profiles
with all four instrumentation families disabled, and linked both
`sb_qow_sblr_query_route_contract` and `sb_engine_sblr`. An eight-action
focused build produced the live VALUES spine, direct SORT/LIMIT, QRY-010, and
QRY-010 FETCH/TOP-profile executables. All four tests passed, covering the
live RCP-033 sort route plus standalone LIMIT and the admitted
DISTINCT/SORT/OFFSET/LIMIT-or-FETCH tails and their refusal boundaries. The
clean graph retained the same unrelated SBLR codec narrowing warnings and GCC
STL inlining warning recorded by Stage 4e; no changed Stage 4 source emitted a
warning after the include repair. Configure, clean build and repair
diagnostics, focused build, CTest, source-authority gate, MGA-policy gate,
instrumentation check, and diff-check output is retained in
`/tmp/scratchbird-canonical-query-stage4f-*.log`.

The seventh Stage 4 slice moves the standalone object-free PIVOT and UNPIVOT
coordinators and their route-exclusive causal/output receipt validation behind
a focused internal contract. A narrow aggregate-root preparation gateway
exposes only the already-bound aggregate carrier, while selected-DAG execution
and MGA statement-context revalidation remain behind the coordinator gateway.
The extracted module cannot select a plan, create a snapshot, access storage,
or publish transaction finality. The coordinator is reduced to 52,110 lines
and 2,543,948 bytes; the new composition module is 1,574 lines and 72,281
bytes.

A fresh 1,333-action benchmark-clean closure graph completed across one
targeted localization of the aggregate-kernel base-memory constant and a
781-action resume. The twenty-six-module layout compiled in both contract and
production profiles with all four instrumentation families disabled, and
linked both `sb_qow_sblr_query_route_contract` and `sb_engine_sblr`. A
six-action focused build produced the direct QRY-019 PIVOT and UNPIVOT
executables and the live VALUES spine. All three tests passed, directly
covering RCP-048 through both leaf executors and the composed canonical route.
The clean graph retained the same unrelated SBLR codec narrowing warnings and
GCC STL inlining warning recorded by Stages 4e and 4f; no changed Stage 4
source emitted a warning after the constant localization. Configure, clean
build and repair diagnostics, focused build, CTest, source-authority gate,
MGA-policy gate, instrumentation check, and diff-check output is retained in
`/tmp/scratchbird-canonical-query-stage4g-*.log`.

The eighth Stage 4 slice moves the standalone object-free grouped
COUNT/SUM/HAVING and global aggregate coordinators behind a focused internal
contract. Immutable aggregate-profile carriers and narrow grouped/global
preparation gateways avoid exposing the coordinator's anonymous namespace; a
bounded DISTINCT-planning gateway returns only its peak-memory receipt.
Selected-DAG execution and MGA statement-context revalidation remain behind
the coordinator gateway. The extracted module cannot create a snapshot,
access storage, or publish transaction finality. The coordinator is reduced
to 50,480 lines and 2,465,865 bytes; the new composition module is 1,693 lines
and 81,143 bytes.

A fresh 1,335-action benchmark-clean closure graph completed across targeted
repairs for one missing filter-registration include and two formerly implicit
aggregate-preparation default arguments, followed by a 794-action resume. The
twenty-seven-module layout compiled in both contract and production profiles
with all four instrumentation families disabled, and linked both
`sb_qow_sblr_query_route_contract` and `sb_engine_sblr`. A six-action focused
build produced the direct QRY-007 aggregate executor, the QRY-017 grouped
HAVING row-binding route, and the live VALUES spine. All three tests passed,
covering RCP-026, RCP-027, RCP-028, and RCP-049 aggregate composition. The
clean graph retained the same unrelated SBLR codec narrowing warnings and GCC
STL inlining warning recorded by the preceding Stage 4 slices; no changed
Stage 4 source emitted a warning after the targeted repairs. Configure, clean
build and repair diagnostics, focused build, CTest, source-authority gate,
MGA-policy gate, instrumentation check, and diff-check output is retained in
`/tmp/scratchbird-canonical-query-stage4h-*.log`.

The ninth and final Stage 4 slice moves the generic node-driven object-free
composition coordinator behind a focused internal contract. The module owns
the admitted unary-tail, branching, correlated/LATERAL subquery, recursive
CTE, set-operation, aggregate, sort, and window composition path. Narrow
preparation adapters expose only typed bindings and immutable planning
carriers shared with remaining routes. Public route selection and the
selected-DAG MGA revalidation boundary remain in the coordinator. The new
module cannot create or refresh a snapshot, access durable storage, or
publish transaction finality. The coordinator is reduced to 44,226 lines and
2,160,641 bytes; the new composition module is 6,438 lines and 313,200 bytes.

A fresh benchmark-clean closure tree compiled and linked the twenty-eight
module layout in both contract and production profiles with all four
instrumentation families disabled. The first broad all-target pass also
reported an independent pre-existing `qow_qry_015_test_v1` link gap for
optimizer-continuation symbols; the required SBLR targets were then completed
from the same clean tree and did not depend on that unrelated executable. A
focused build produced the live VALUES spine plus direct window, quantified
subquery, LATERAL, recursive UNION, and recursive SEARCH/CYCLE executables.
All six tests passed. The live spine covers the composed RCP-026 through
RCP-049 families, while the direct leaves preserve the isolated window,
subquery, and recursive boundaries. No changed Stage 4 source emitted a
warning. Configure, broad failure-collection build, targeted repair/resume
builds, focused build, CTest, source-authority gate, MGA-policy gate,
instrumentation check, and diff-check output is retained in
`/tmp/scratchbird-canonical-query-stage4i-*.log`.

The first Stage 5 slice moves the complete production time-series source and
its bounded relational tail behind a focused internal route contract. Shared
captured-leg state is an explicit immutable carrier, while narrow coordinator
adapters retain optimizer capability construction, selected-plan execution,
and cross-family cancellation/cardinality helpers. The route continues to ask
the transaction subsystem for the current engine-issued statement snapshot,
loads only its admitted relation descriptor, and revalidates the selected MGA
statement context before result publication. It cannot begin, commit, roll
back, recover, or persist a transaction, and it owns no parser or unrelated
model-family route. The coordinator is reduced to 39,554 lines and 1,923,197
bytes; the new time-series composition module is 4,783 lines and 242,149
bytes.

A fresh 1,338-action benchmark-clean closure graph completed across two
targeted boundary repairs and linked both the contract-only and production
SBLR libraries. The focused RCP-076 build linked the direct time-series
execution and production query-route executables; both tests passed. The
source-authority gate passes with twenty-nine modules, the MGA policy gate
passes, all four instrumentation families remain disabled, and the diff check
is clean. The only build warnings were the previously recorded narrowing and
GCC STL inlining warnings in unrelated runtime sources. Configure, clean
build, targeted repair builds, focused build, CTest, source-authority gate,
MGA-policy gate, instrumentation check, and diff-check output is retained in
`/tmp/scratchbird-canonical-query-stage5a-*.log`.

The second Stage 5 slice moves the complete production vector-nearest source
route behind its focused internal contract. The module owns vector metric and
filter admission, provider execution, exact typed result validation, bounded
relational-tail planning, and result publication. It consumes and revalidates
the engine-issued MGA statement snapshot but cannot create, refresh, finalize,
or recover transaction authority; parser lowering and all other model-family
routes remain outside the module. The coordinator is reduced to 38,506 lines
and 1,868,418 bytes; the vector composition module is 1,104 lines and 56,866
bytes.

The clean benchmark-profile closure links both the contract-only and
production SBLR libraries. The focused RCP-077 direct vector execution and
production query-route tests provide route evidence. The source-authority gate
now tracks thirty modules and rejects transaction-finality, parser, WAL, or
unrelated public-route authority in either extracted model-family module.
Build, focused test, source-authority, MGA-policy, instrumentation, and diff
evidence is retained in `/tmp/scratchbird-canonical-query-stage5b-*.log`.

The third Stage 5 slice moves the complete production search source and its
bounded relational tail behind a focused internal route contract. The module
owns exact search operation/analyzer admission, engine-bound provider reads,
typed result validation, and admitted recursive, set, aggregate, window,
sort, limit, and mixed-join composition. It reuses the Stage 4 preparation
contracts and consumes the engine-issued MGA statement snapshot without
creating, refreshing, finalizing, or recovering transaction authority. Parser
lowering and every unrelated model-family route remain outside the module.
The coordinator is reduced to 36,058 lines and 1,744,150 bytes; the search
composition module is 2,528 lines and 127,548 bytes.

The clean benchmark-profile closure links both the contract-only and
production SBLR libraries. The focused RCP-078 direct search execution and
production query-route tests provide route evidence. The source-authority gate
now tracks thirty-one modules and applies the model-family finality/parser/WAL
boundary to time-series, vector, and search alike. Build, focused test,
source-authority, MGA-policy, instrumentation, and diff evidence is retained
in `/tmp/scratchbird-canonical-query-stage5c-*.log`.

The fourth Stage 5 slice moves the complete production key-value source and
its bounded relational tail behind a focused internal route contract. The
module owns exact get, multi-get, and prefix admission, engine-bound provider
reads, typed result validation, and admitted recursive, set, aggregate,
window, sort, limit, and mixed-join composition. Its persisted-row descriptor
callback only validates against existing engine authority, and the module
consumes the engine-issued MGA statement snapshot without creating,
refreshing, finalizing, or recovering transaction authority. Parser lowering
and every unrelated model-family route remain outside the module. The
coordinator is reduced to 33,760 lines and 1,628,862 bytes; the key-value
composition module is 2,396 lines and 119,306 bytes.

The clean benchmark-profile closure links both the contract-only and
production SBLR libraries. The focused RCP-075 direct key-value execution and
production query-route tests provide route evidence. The source-authority gate
now tracks thirty-two modules and applies the model-family
finality/parser/WAL boundary to time-series, vector, search, and key-value.
Build, focused test, source-authority, MGA-policy, instrumentation, and diff
evidence is retained in `/tmp/scratchbird-canonical-query-stage5d-*.log`.

The fifth Stage 5 slice moves the complete production graph match/expand
source and its bounded relational tail behind a focused internal route
contract. The module owns exact graph object, pattern/traversal, provider,
descriptor, planner, and typed-result coordination together with admitted
recursive, set, aggregate, window, sort, limit, and mixed-join composition.
It consumes and revalidates the engine-issued MGA statement snapshot without
creating, refreshing, finalizing, or recovering transaction authority. Parser
lowering and every unrelated model-family route remain outside the module. The
coordinator is reduced to 31,093 lines and 1,493,399 bytes; the graph
composition module is 2,756 lines and 139,015 bytes.

The clean benchmark-profile closure links both the contract-only and
production SBLR libraries. The focused RCP-074 direct graph execution and
production query-route tests provide route evidence. The source-authority gate
now tracks thirty-three modules and applies the model-family
finality/parser/WAL boundary to time-series, vector, search, key-value, and
graph. Build, focused test, source-authority, MGA-policy, instrumentation, and
diff evidence is retained in `/tmp/scratchbird-canonical-query-stage5e-*.log`.

The sixth and final Stage 5 slice moves the complete production document
source behind a focused internal route contract. The module owns both exact
expression-backed document unnest and persisted document-path execution,
including provider, descriptor, authorization, planner, typed-result, and
admitted recursive/set/aggregate/window/sort/limit coordination. It consumes
and revalidates the engine-issued MGA statement snapshot without creating,
refreshing, finalizing, or recovering transaction authority. Parser lowering,
the captured multileg RCP-079 helpers, and every unrelated model-family route
remain outside the module. The coordinator is reduced to 27,446 lines and
1,308,143 bytes; the document composition module is 3,738 lines and 188,993
bytes.

The clean benchmark-profile closure links both the contract-only and
production SBLR libraries. The focused RCP-073 direct document execution,
production query-route, and collection-isolation tests provide route evidence.
The source-authority gate now tracks thirty-four modules and applies the
model-family finality/parser/WAL boundary to all six Stage 5 family modules.
Build, focused test, source-authority, MGA-policy, instrumentation, and diff
evidence is retained in `/tmp/scratchbird-canonical-query-stage5f-*.log`.

## Review rule

Each stage should be reviewable as one authority move. If a proposed extraction
requires broad access to unrelated private types, first create a narrow
internal contract and stop; do not expose the monolith's anonymous namespace as
a general-purpose API merely to make the move compile.
