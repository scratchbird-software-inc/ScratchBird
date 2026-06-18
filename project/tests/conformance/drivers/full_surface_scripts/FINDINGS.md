# Full-Surface Suite — Coverage Extension Findings

Status: round-2 (language-surface extension) analysis
Scope: extending the driver full-surface conformance suite to the full SBsql language.

This file records (1) a harness limitation that bounds what the suite can test, and
(2) constructs that could not be grounded in the sources (so they were omitted
rather than guessed). These are candidate partial-implementation / harness gaps
for the engine and driver teams.

## A. HARNESS LIMITATION — the runner cannot carry multi-statement procedural bodies

**Finding.** The driver runners split the compiled chain into statements with a
**quote-aware, top-level `;`** splitter and **no terminator directive** (`SET TERM`
is not honored) and **no procedural-block awareness**:

- Python: `scratchbird.sql.split_top_level_statements` (drivers/driver/python/src/scratchbird/sql.py) — splits on `;` outside single/double quotes.
- C++: `splitStatements` (drivers/driver/cpp/tools/sb_isql_cpp.cpp:103) — same rule (`ch == ';' && !single && !dbl`).
- The compiler (`compile_full_surface_script_suite.py`) does **not** split at all; it substitutes placeholders and concatenates. Splitting is entirely the runner's job.

**Consequence.** A normal SBsql procedural body uses inner `;` and relies on a
terminator change (`SET TERM ^` … `^`) to be submitted as one statement. Because
the runners ignore `SET TERM` and split on every top-level `;`, any body containing
inner `;` is **mis-split into fragments** and cannot be executed as one statement.

This blocks end-to-end testing of, per `function.md` grammar
(`function_body ::= procedural_block | RETURN expression | EXTERNAL UDR …`):

| Construct | Carriable by current harness? |
| --- | --- |
| **Expression-body function** (`AS return <expr>;`) | **Yes** — single statement, no inner `;` |
| Procedural-block function (`AS begin … ; … end;`) | **No** — inner `;` mis-split |
| `CREATE PROCEDURE` with a body | **No** — inner `;` mis-split |
| `CREATE TRIGGER` with a multi-statement body | **No** — inner `;` mis-split |
| Procedural language (`procedural_sql_blocks/_control_flow/_cursors/_exceptions`) | **No** — only exercisable inside a body |

**What this round therefore does:** it tests `CREATE FUNCTION` via the
**expression-body** form (create → invoke → assert), which is grounded and
carriable, and it records the procedural-body surface as **blocked by the harness**.

**Recommended fix (engine/driver team, out of scope here):** teach the driver
runners' statement splitter to honor a terminator directive (`SET TERM`) or to be
procedural-block aware (track `BEGIN … END` / routine bodies). Until then, the
suite cannot exercise the procedural-SQL surface end-to-end. This is itself a
high-value gap: a procedural body that "parses but does not execute" cannot be
caught by this suite as currently built.

## B. Refusal mechanism note (for authors)

`expected_refusals.json` keys are `<script-name>:<statement-index>` (1-based index
of the statement within that script after `;`-splitting), **not** a line number.
(`080_metadata_security_authorization.sbsql:6` is the 6th statement of that script.)
Negative tests must therefore be placed at a known statement index, registered in
the script's `expected_refusals` (manifest) and in `expected_refusals.json`
(`statement_ids` + `expected_diagnostics`).

## C. Constructs omitted because they could not be grounded / are not carriable

Harness-blocked (per §A — multi-statement bodies):
- Procedural-SQL surface — procedures, procedural-block functions, multi-statement
  triggers, control flow, cursors, exceptions (`procedural_sql*`, `procedure.md`,
  `trigger.md`). Only expression-body functions are carriable (covered in `052`).
- Table-valued functions (`RETURNS TABLE` with `FOR … SUSPEND`) — procedural body.

Not documented in the cited source (omitted rather than guessed):
- `NATURAL JOIN` — not in `from.md` `join_type` grammar (only INNER/LEFT/RIGHT/FULL).
- `DISTINCT ON (expr)` — not in `select.md` (`set_quantifier` is only `ALL | DISTINCT`).
- `COLLATE` in `ORDER BY` — not in `order_by_limit_offset.md` `sort_spec`.
- `ALTER VIEW … SET SECURITY INVOKER` and `RECREATE VIEW` — named in `view.md` lifecycle
  prose but no EBNF production present; not expressed.
- `RECREATE TABLE`, `ALTER TABLE ALTER COLUMN <type>` (conversion route), `VALIDATE
  CONSTRAINT` — named in `table.md` but exact syntax not in an EBNF block; not expressed.
- `RENAME SCHEMA`, `SHOW/DESCRIBE SCHEMA` as assertion rows — documented but conflict with
  harness teardown / not assertion-row-shaped; omitted.
- `DROP CLUSTER`, distributed-transaction cluster verbs — not in `cluster_gated_statements.md`
  EBNF; omitted (would conflate refusal vectors).

Deferred (grounded but needs follow-up data/scope):
- `INTERSECT ALL` / `EXCEPT ALL` — grammar allows `ALL`, but a deterministic assertion needs
  deliberate cross-side duplicate data; only `UNION ALL` asserted in `045`.
- `ALTER TABLE ADD FOREIGN KEY` — needs a second table; deferred to a join-lifecycle script.
- Updatable-view `WITH CHECK OPTION` refused-DML — needs a mid-script refusal interaction.

## D. Unconfirmed introspection surfaces — assertions that may need name correction

Some catalog-introspection assertions use `sys.*` surface names that follow the documented
naming pattern (`sys.schemas`/`sys.tables`/`sys.columns`, which ARE confirmed) but are **not
themselves listed in `Language_Reference/catalog_reference/`**. If the engine surface name
differs, these specific assertion rows will fail at run time and should be corrected against
the real surface (this is a *name-confirmation* issue, not necessarily an engine bug — and
the missing catalog documentation is itself a gap worth closing):

| Script | Surface used (provisional) | Confirmed? |
| --- | --- | --- |
| `052` | `sys.routines` (`routine_kind`, `routine_name`) for function introspection | not in catalog_reference |
| `086` | `sys.security.roles`, `sys.security.groups`, `sys.policies`, `sys.masks`, `sys.rls_rules` | not in catalog_reference |
| `012` | (avoided `sys.sequences` — unconfirmed — used `sys.schemas` presence instead) | n/a |
| `014` | (avoided `sys.schemas.parent_schema_id` — unconfirmed — proved nesting functionally) | n/a |

Confirmed/grounded surfaces used: `sys.schemas`, `sys.tables`, `sys.columns` (from `080`),
and `sys.catalog.domain_descriptor` (from `catalog_reference/sys_catalog_domain_descriptor.md`).

### Resolution (P1 catalog-structure verification)

Verified against the authoritative SHOW-command → `sys.*` surface mapping in
`src/parsers/sbsql_worker/lowering/lowering.cpp` and corrected the scripts:

| Provisional (wrong) | Confirmed surface (lowering.cpp) | Backing SHOW | Action taken |
| --- | --- | --- | --- |
| `sys.security.groups` | `sys.security.principals` | `SHOW GROUPS` | `086` corrected |
| `sys.policies` | `sys.security.policies` | `SHOW POLICIES` | `086` corrected |
| `sys.masks` | `sys.security.masks` | `SHOW MASKS` | `086` corrected |
| `sys.rls_rules` | `sys.security.rls` | `SHOW RLS` | `086` corrected |
| `sys.security.roles` | `sys.security.roles` (already correct) | `SHOW ROLES` | kept |
| `sys.routines` | *no confirmed client-queryable function surface* | — | `052` reworked to invocation-based proof; `sys.routines` removed |

Notes: (a) `086` assertions are now **count-based (`COUNT(*) >= 1`)** on the confirmed
surfaces, because the `rs.security.principal.v1` / `rs.security.policy.v1` result-shape
**column names are still unconfirmed** (so name/`schema_id` filters were dropped) — matches
the `080` `sys.security.users` precedent. (b) Real surfaces for sequences/functions are
`sys.catalog.sequence` / `sys.catalog.function` (DDL descriptor refs), but their SELECT-
queryability and columns are unconfirmed, so `012` (sequence) and `052` (function) prove via
substitute/invocation rather than those surfaces. (c) `014` schema parent-linkage column is
still unconfirmed; nesting is proved functionally. These open items remain in the private
workplan `CATALOG_SURFACE_VERIFICATION.csv`.

## E. Pending implementation items (no deferred rounds)

Not yet covered, but required for full-surface completion in this cycle:
`copy.md` (in-band COPY), `multimodel_statements.md` (needs backing-object CREATE DDL to be confirmed — see the example-DB findings),
`database.md`,
`filespace.md`, `agent.md`, `backup_restore_replication_migration.md`,
`management_and_operations.md`, `catalog_artifacts_and_external_git.md`, `set`/session config.
`script_tokens_and_identifiers.md` is lexical (implicitly exercised); `refusal_vectors.md` is a
concept page whose mechanism is exercised via `expected_refusals`; `README.md` is not a statement page.
