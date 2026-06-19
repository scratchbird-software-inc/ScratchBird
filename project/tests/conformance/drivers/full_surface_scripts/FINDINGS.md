# Full-Surface Suite — Coverage Extension Findings

Status: round-2 (language-surface extension) analysis
Scope: extending the driver full-surface conformance suite to the full SBsql language.

This file records (1) runner requirements for executing the suite exactly as
written, and (2) syntax names that could not be grounded in executable source
or a confirmed result shape. Those names are not guessed into the suite.

## A. Runner requirement — SET TERM- and comment-aware statement chunking

**Finding.** The full-surface suite includes procedural functions, procedures,
selectable procedures, dynamic execution, triggers, and in-band COPY payload
markers. Driver runners must use the shared statement chunker semantics:

- Honor `SET TERM <terminator> ;` client directives.
- Preserve routine/trigger bodies as one executable statement.
- Treat line comments before a statement as statement trivia, not as the first
  command token.
- Allow `-- SB_COPY_INPUT ...` rows to travel with the following `COPY ... FROM
  STDIN` statement so the runner can stream those rows as SBWP CopyData frames.

**Consequence.** A runner that falls back to a simple semicolon splitter will
mis-split routine/trigger bodies and will fail the procedural scripts. A runner
that ignores the `SB_COPY_INPUT` marker will fail the COPY script by sending
CopyFail or an empty stream.

Current covered procedural/COPY scripts:

| Construct | Script(s) |
| --- | --- |
| Expression-body function | `052` |
| Procedural-body function | `054` |
| Non-selectable procedure | `056` |
| Selectable procedure | `057` |
| Dynamic execution result-set procedure | `058` |
| Trigger bodies | `053` |
| COPY FROM STDIN | `059` |

## B. Refusal mechanism note (for authors)

`expected_refusals.json` keys are `<script-name>:<statement-index>` (1-based index
of the statement within that script after `;`-splitting), **not** a line number.
(`080_metadata_security_authorization.sbsql:6` is the 6th statement of that script.)
Negative tests must therefore be placed at a known statement index, registered in
the script's `expected_refusals` (manifest) and in `expected_refusals.json`
(`statement_ids` + `expected_diagnostics`).

## C. Source-grounding boundaries

Not documented in the cited source and therefore not guessed into executable
driver scripts:
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
  EBNF; including them would conflate cluster refusal vectors with executable driver behavior.

The suite does not invent forms from prose-only mentions. When the executable
source grammar expands, new statements must be added as positive tests or as
registered refusals in `expected_refusals.json`.

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
`sys.catalog.sequence` / `sys.catalog.function` (DDL descriptor refs), but their SELECT
queryability and columns are not used by the suite, so `012` (sequence) and `052` (function)
prove via substitute/invocation rather than those surfaces. (c) `014` proves nested schema
resolution functionally without relying on an unconfirmed parent-linkage column.

## E. Required executable-script coverage

The required executable driver scripts now include positive coverage for COPY,
multimodel routes, and admin/session/artifact route families:

| Family | Script(s) | Coverage |
| --- | --- | --- |
| COPY | `059` | in-band `COPY ... FROM STDIN` through SBWP CopyData/CopyDone |
| Multimodel | `092` | document collection, KV, graph, time-series, search, vector descriptors and operations |
| Database/filespace/agent/admin | `093` | database lifecycle, filespace, agent, management, configuration, backup/restore/archive/replication/changefeed/migration |
| Catalog artifacts/external Git | `093` | export snapshot, diff snapshot, and rollback-plan routes through server ABI |

`script_tokens_and_identifiers.md` is lexical and is exercised by every script;
`refusal_vectors.md` is a concept page whose mechanism is exercised via
`expected_refusals`; `README.md` is not a statement page.

COPY is now covered by `SBDFS-059`: the runner extracts in-band `SB_COPY_INPUT`
rows, streams them as SBWP CopyData, sends CopyDone, and the script asserts the
imported rows through normal SELECTs.

External Git support is a catalog review/convenience surface only: it exports,
diffs, and plans rollback from catalog artifacts while preserving
`git_runtime_authority=false` and `external_git_repository_authority=false`.
Applying changes remains under the normal ScratchBird catalog/security path.
