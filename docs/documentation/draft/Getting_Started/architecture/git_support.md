# Git-Oriented Workflows

## Purpose

After reading this page you will understand what kinds of database-related files belong in a Git repository, what does not belong there, and — crucially — why Git and the database engine are different systems that should not be confused with each other.

Git is a natural fit for the source assets you write and review before running them against a database: scripts, fixtures, configuration templates, and expected results. What Git cannot do is replace the database. It does not own transaction history, catalog state, backup durability, authorization, or recovery decisions. This page describes the boundary between those two worlds.

Git is useful for managing scripts, configuration templates, fixtures, expected results, and documentation. Git does not replace database backups, recovery, transaction history, authorization, support bundles, or engine-owned catalog state.

ScratchBird also provides a distinct, opt-in capability — **external Git catalog versioning** — that lets you export a snapshot of the catalog into a form an external Git repository can track, diff the live catalog against a tracked snapshot, and produce a rollback *plan*. This is a controlled review-and-versioning convenience, **not** a change of authority: Git never executes against the database, and applying a planned change still flows through the engine's authorized catalog API. That capability is described in its own section below; the rest of this page covers the everyday source-control workflow.

## The Core Distinction

![diagram](./git_support-1.svg)

Git tracks the request material and expected proof artifacts. SBcore owns the durable result after admitted execution.

## External Git Catalog Versioning

Beyond tracking hand-written scripts, ScratchBird can export the **catalog itself** as a set of content-hashed artifacts that an external Git repository can version, review, and diff over time. This lets a team keep a Git-tracked history of catalog structure and compare or reconcile a live database against a committed snapshot — while the engine remains the sole authority over what actually changes.

This capability is **opt-in and policy-gated**. The engine refuses external-git operations unless the request carries `external_git_policy:enabled` (or `allow_external_git_versioning:true`); without it the operation is refused with `external_git_policy_required`.

### What The Engine Provides

| Operation | Surface | Role |
| --- | --- | --- |
| `EXPORT CATALOG ARTIFACT` | SBsql statement (requires `right.catalog_read`) | Exports catalog objects as `sb.catalog.artifact.v1` rows, recorded under `sys.catalog.artifacts`. |
| `IMPORT CATALOG ARTIFACT` | SBsql statement (requires `right.catalog_mutate`) | Applies an authorized catalog artifact through the engine — the only admitted way to *apply* a change. |
| Export external Git snapshot | Engine artifact API / SBLR opcode `artifact.external_git.export_snapshot` | Emits a `sb.external_git.catalog_snapshot.v1` manifest plus one content-hashed object row per catalog object. |
| Diff external Git snapshot | SBLR opcode `artifact.external_git.diff_snapshot` | Compares the live catalog to a candidate snapshot, classifying each object as `unchanged`, `modified`, `added_in_candidate`, or `removed_from_candidate`. |
| Plan external Git rollback | SBLR opcode `artifact.external_git.rollback_plan` | Produces a rollback *plan* (actions such as `restore_current_catalog_artifact`, `reject_candidate_only_object_until_authorized_catalog_create`, or `no_action_required`) — it does not apply anything. |

The three `external_git.*` operations are exposed through the engine artifact API and SBLR opcodes, not as typed SBsql statements; the `EXPORT`/`IMPORT CATALOG ARTIFACT` statements are the SBsql-level entry points. Each object row carries a stable `content_hash`; the engine recomputes it and rejects a snapshot whose supplied hash does not match (`external_git_snapshot_hash_mismatch`), is missing object identity (`external_git_snapshot_object_required`), or repeats a UUID (`external_git_snapshot_duplicate_uuid`).

### The Authority Boundary (Why This Is Safe)

Every external-git result carries explicit authority evidence, and the boundary is enforced, not advisory:

- `external_git_versioning` = `convenience_snapshot_review_only` — versioning and review only.
- `git_runtime_authority` = `false` and `external_git_repository_authority` = `false` — Git never executes and is never an authority.
- `catalog_runtime_authority` = `ScratchBird_catalog_api` and `mga_transaction_authority` = `local_mga_transaction_inventory` — the engine catalog API and MGA keep authority.
- A diff row is marked `requires_authorized_catalog_import` = `true`, and the rollback apply route is `authorized_catalog_api_not_git_repository` — to actually apply a reconciliation you use `IMPORT CATALOG ARTIFACT` through the engine, never a direct Git apply.
- Any attempt to claim direct authority — `git_runtime_authority:true`, `external_git_direct_authority:true`, or `external_git_direct_apply:true` — is refused with `external_git_authority_forbidden`.

In short: Git can hold a versioned, diffable history of your catalog and you can plan a rollback from it, but the engine is the only thing that ever changes the database, through its authorized, MGA-governed catalog API. Object identity in these artifacts is UUID-based (`identity_authority` = `uuid`), so a snapshot tracks durable identity rather than just names.

For the operator workflow and the full operation/format reference, see the Operations and Administration and Language Reference manuals linked at the end of this page.

## What Belongs In Git

Git is appropriate for human-reviewed and reproducible artifacts such as:

- schema creation scripts;
- migration scripts;
- rollback scripts where the project maintains them;
- seed data for development or tests;
- test fixtures;
- expected result files;
- parser compatibility inputs;
- configuration templates;
- policy templates without secrets;
- documentation;
- generated proof summaries that are intended for review;
- release notes and upgrade notes.

These files should be written so another user can understand what they request before executing them.

## What Does Not Belong In Git By Default

Git should not be treated as ordinary storage for:

- live database files;
- temporary database files;
- local build output;
- staged release artifacts unless the release process explicitly tracks them;
- raw support bundles that may contain sensitive operational evidence;
- secrets, passwords, keys, or tokens;
- local machine paths;
- generated caches;
- logs with protected material;
- physical backup page copies.

If a project intentionally tracks a generated artifact, document why it is tracked and how it is regenerated.

## Recommended Repository Shape

A project using ScratchBird can keep database source assets organized without mixing them with live storage.

```text
project_root
|-- database
|   |-- schema
|   |-- migrations
|   |-- seed
|   |-- policy_templates
|   `-- expected_results
|-- tests
|   |-- sbsql
|   |-- fixtures
|   `-- proof
`-- docs
    `-- database_notes
```

This is only an example layout. The important rule is to keep source material separate from live database files and local generated output.

## Migration Scripts

A migration script is a reviewed request to change the database. It is not the durable change by itself — the change only becomes durable after the script is executed and the engine commits the transaction. This distinction matters because two teams could have identical migration scripts yet end up with databases that differ in catalog identity, UUID assignments, or row contents depending on the history of each database.

A good migration workflow records:

- the intended precondition;
- the requested schema or data changes;
- the expected postcondition;
- the transaction boundary;
- the verification query or proof;
- the rollback or recovery plan where applicable;
- the minimum ScratchBird build or feature surface required.

Example structure:

```sql
-- migration: add note status
-- intent: add a status column for application filtering

alter table app.notes
    add column note_status text not null default 'open';

select note_id, note_status
from app.notes
order by note_id;

commit;
```

Use exact syntax from the current Language Reference when writing production scripts.

## Expected Results

Expected result files are useful when a script should produce deterministic output.

For example, a test can record:

- row count;
- column names;
- column types;
- ordered result rows;
- expected diagnostic class for an invalid request;
- expected refusal when policy denies an operation.

When row order matters, scripts should request it explicitly with `order by`.

## Configuration Templates

Configuration templates can live in Git when they do not contain secrets and when environment-specific values are clearly separated.

Good template behavior:

- use template variables for local paths and secret references;
- keep raw secrets out of the file;
- document required resource files;
- make parser route admission explicit;
- make diagnostics and redaction policy explicit;
- keep platform-specific notes separate when needed.

## Git And Database Identity

ScratchBird uses UUID-backed catalog identity. Git tracks text files. These are fundamentally different things, and confusing them leads to subtle problems in migrations and dependency tracking.

Concretely:

- a script can request `create table app.notes`;
- the engine creates or modifies durable catalog objects;
- a later rename may keep the same durable object identity;
- a Git diff of scripts is not the same as a catalog diff;
- replaying a script against a different database may not produce the same durable identity.

Treat Git as source control for requests, not as the catalog.

## Git And Transactions

Git commit history is not database transaction history.

| Git Concept | Database Concept |
| --- | --- |
| Git commit | Versioned source change in a repository. |
| Database commit | Engine admission of a transaction outcome. |
| Git revert | Source-level reversal of a repository change. |
| Database rollback | Discard uncommitted database work. |
| Git branch | Source-control line of development. |
| Schema branch | Database namespace branch. |

The terms can sound similar, but they are different systems with different authority.

## Git And Backups

Git is not a database backup system.

Git can help reproduce scripts and expected states, but a database backup must preserve the database state according to the documented backup and restore surface. Logical backup, logical restore, import, export, and migration behavior should be handled through the relevant ScratchBird tools or SBsql commands where implemented and admitted.

## Review Checklist

Before merging database-related source changes, review:

- Does the script state its intent?
- Does it use qualified names where clarity matters?
- Does it avoid raw secrets?
- Does it include a transaction boundary?
- Does it include verification queries or expected diagnostics?
- Does it avoid relying on implicit row order?
- Does it avoid server-local paths unless the operation is explicitly intended and admitted?
- Does it avoid claiming feature availability that the current build does not prove?

## Where To Go Next

- [SBsql And SBLR](sbsql_and_sblr.md)
- [Storage, Transactions, And Recovery](storage_transactions_and_recovery.md)
- [First SBsql Session](../using_scratchbird/first_sbsql_session.md)
- [Backup, Restore, And Data Movement Overview](../administration/backup_restore_and_data_movement_overview.md)
- [Script Tokens And Identifiers](../../Language_Reference/syntax_reference/script_tokens_and_identifiers.md)
