# Management And Operations Statements

This page is part of the SBsql Language Reference Manual. It explains the user-facing language contract while preserving the ScratchBird authority model: SQL text parses to SBLR, durable identity is UUID based, descriptors own type behavior, security is materialized from catalog policy, and MGA owns transaction finality.

Generation task: `syntax_reference_management_operations`

Related pages: [Agents And Agent Management](agent.md), [Security And Privileges](security_and_privilege_statements.md), [Transaction Control](transaction_control.md), [Database Lifecycle](database.md), [Filespace Lifecycle](filespace.md), [Backup, Restore, Replication, And Migration](backup_restore_replication_migration.md), [Cluster-Gated Statements](cluster_gated_statements.md), [Refusal Vectors](refusal_vectors.md), [Management Statement EBNF](ebnf/management_statement.md), [SHOW Statement EBNF](ebnf/show_statement.md), and [EXPLAIN Statement EBNF](ebnf/explain_statement.md).

## Purpose

Management and operations statements inspect, validate, diagnose, and control runtime state. They are still ordinary SBsql surfaces: parse, bind, authorize, route, execute, and return either a typed report, command result, job descriptor, support bundle descriptor, or canonical message vector.

Management statements do not bypass catalog security, transaction rules, sandbox roots, recovery fencing, or protected-material policy. A command that can stop work, drain a service, cancel a statement, generate diagnostics, reload configuration, or expose runtime metadata must be authorized explicitly.

Common operation families include:

- runtime health, readiness, and liveness;
- sessions, connections, statements, transactions, locks, waits, and jobs;
- agents, agent lifecycle, agent policies, action approval, overrides, and evidence;
- listeners, local managers, parser pools, workers, IPC endpoints, and service lifecycle;
- configuration validation, reload, history, effective values, and refusal details;
- support bundle generation, redaction, manifest inspection, and bundle lifecycle;
- storage, filespace, cache, memory, temporary work, index readiness, and cleanup summaries;
- package, parser, UDR, extension, and acceleration readiness;
- `SHOW`, `DESCRIBE`, `EXPLAIN`, `EXPLAIN ANALYZE`, and diagnostics.

## Statement Families

```ebnf
management_statement ::=
      show_management
    | alter_management
    | config_statement
    | support_bundle_statement ;

observability_statement ::=
      show_statement
    | explain_statement ;
```

SBsql is context sensitive. `SHOW`, `ALTER MANAGEMENT`, `CONFIG`, `SUPPORT BUNDLE`, and `EXPLAIN` words are command words inside their statement families and should not be treated as globally reserved identifiers outside those contexts.

## Shared Execution Contract

Every management operation follows the same route:

1. Parse the command family and target.
2. Bind target names, service identifiers, session identifiers, job identifiers, configuration keys, and result descriptors.
3. Authorize the effective user or agent UUID for the requested inspection or control action.
4. Check sandbox, disclosure, recovery, and service-state policy.
5. Admit the SBLR management or observability route.
6. Execute the operation or return a fail-closed message vector.
7. Redact protected fields before rendering the result.

The parser cannot make a management command authoritative by accepting text. Runtime control belongs to the admitted management route, and durable database state still belongs to engine transaction rules.

## SHOW And DESCRIBE Inspection

`SHOW` returns compact runtime or catalog projections. `DESCRIBE` returns detailed authorized metadata for one target. Both are inspection surfaces.

```sql
show health;
show readiness;
show liveness;
show management listeners;
show management parser pools;
show agents extended;
show agent memory_governor;
show management sessions;
show management jobs;
show management config effective;
show diagnostics;
describe database current;
describe filespace primary;
```

Inspection targets are deliberately broad because operational users need to answer different questions without reading private files or raw internal state.

| Target family | Typical command | Required result contract |
| --- | --- | --- |
| Health | `SHOW HEALTH` | Overall service status, degraded state, recovery fences, and message vectors. |
| Readiness | `SHOW READINESS` | Whether the service should receive new work. |
| Liveness | `SHOW LIVENESS` | Whether the process or endpoint is alive enough for supervision. |
| Build/runtime | `SHOW VERSION`, `SHOW BUILD`, `SHOW RUNTIME` | Version, build profile, platform, feature gates, and public capabilities. |
| Sessions | `SHOW MANAGEMENT SESSIONS` | Authorized session summaries, not raw secrets or hidden principals. |
| Connections | `SHOW MANAGEMENT CONNECTIONS` | Endpoint, parser, manager, and service connection state with redaction. |
| Statements | `SHOW MANAGEMENT STATEMENTS` | Running statement IDs, state, elapsed time, wait reason, and cancel eligibility. |
| Transactions | `SHOW MANAGEMENT TRANSACTIONS` | Transaction IDs, state, snapshot age, blockers, and cleanup impact. |
| Locks/waits | `SHOW MANAGEMENT WAITS` | Wait graph summaries, lock families, and blocked work. |
| Jobs | `SHOW MANAGEMENT JOBS` | Background job state, progress, owner, refusal, and cancellation eligibility. |
| Storage | `SHOW MANAGEMENT STORAGE` | Filespace, page, growth, sync, recovery, and cleanup summaries. |
| Indexes | `SHOW MANAGEMENT INDEXES` | Readiness, rebuild state, divergence refusal, and maintenance progress. |
| Memory/cache | `SHOW MANAGEMENT MEMORY`, `SHOW MANAGEMENT CACHE` | High-water marks, cache pressure, spill state, and policy limits. |
| Parser/runtime packages | `SHOW MANAGEMENT PARSERS`, `SHOW MANAGEMENT UDR` | Package identity, version, ABI, capability, readiness, and load diagnostics. |
| Configuration | `SHOW MANAGEMENT CONFIG EFFECTIVE` | Effective configuration after policy, defaults, environment, and reload history. |
| Diagnostics | `SHOW DIAGNOSTICS` | Current diagnostic records visible to the caller. |

`SHOW` output must be stable enough for tools to parse, but it is not a promise to expose protected internal fields.

## Management Control

`ALTER MANAGEMENT` requests operational control over runtime targets. These commands require stronger privileges than inspection.

```ebnf
alter_management ::=
    ALTER MANAGEMENT management_target management_action management_filter? management_option_list? ;
```

Examples:

```sql
alter management listener default drain;
alter management parser pool sbsql reload;
alter management statement :statement_id cancel;
alter management job :job_id cancel;
alter management cache default flush with scope metadata;
alter management diagnostics rotate;
```

Control actions:

| Action | Meaning |
| --- | --- |
| `DRAIN` | Stop admitting new work while allowing admitted work to finish according to policy. |
| `UNDRAIN` | Resume admission after a drain. |
| `START` | Start an admitted runtime target that is configured but stopped. |
| `STOP` | Stop an admitted runtime target gracefully where policy allows it. |
| `RESTART` | Stop and start under one authorized lifecycle request. |
| `RELOAD` | Reload parser, package, UDR, configuration, or policy surfaces where admitted. |
| `CANCEL` | Request cancellation of a statement, job, stream, or operation. |
| `TERMINATE` | Force termination where policy explicitly admits forced action. |
| `FLUSH` | Flush cache, metadata, diagnostics, or temporary state where safe. |
| `ROTATE` | Rotate diagnostics, logs, support manifests, or other operational records. |
| `VALIDATE` | Validate configuration, package, filespace, service, or readiness without changing state. |

Forced operations must fail closed when they would risk corruption, lose protected evidence, violate transaction finality, or bypass recovery fencing.

## Configuration Operations

`CONFIG` statements validate, inspect, and reload configuration.

```ebnf
config_statement ::=
      CONFIG SHOW config_target? config_option_list?
    | CONFIG VALIDATE config_target? config_option_list?
    | CONFIG RELOAD config_target? config_option_list?
    | CONFIG HISTORY config_target? config_option_list?
    | CONFIG EFFECTIVE config_target? config_option_list? ;
```

Examples:

```sql
config show;
config validate;
config reload;
config history with limit 20;
config effective with include_defaults true;
```

Configuration rules:

| Concern | Rule |
| --- | --- |
| Validation | Syntax, descriptors, value ranges, references, policy gates, and service compatibility are checked before activation. |
| Reload | A reload can apply only reloadable keys. Non-reloadable changes must be reported. |
| History | History reports authorized reload attempts, effective version, actor, result, and message vectors. |
| Defaults | Effective output distinguishes explicit values from defaults when disclosure policy permits it. |
| Protected values | Secrets and protected material are rendered as references or redacted values. |
| Failure | Failed reload leaves the prior effective configuration active. |

## Support Bundles

Support bundles collect authorized diagnostic evidence for troubleshooting. They must be redacted, manifest-driven, and reproducible enough for support work without exposing secrets or protected material.

```ebnf
support_bundle_statement ::=
    SUPPORT BUNDLE support_bundle_action support_bundle_target? support_bundle_option_list? ;
```

Examples:

```sql
support bundle create with scope current_database, redact strict;
support bundle describe :bundle_id;
support bundle export :bundle_id to client stream;
support bundle drop :bundle_id;
```

Bundle actions:

| Action | Contract |
| --- | --- |
| `CREATE` | Collect admitted diagnostics and create a bundle descriptor. |
| `DESCRIBE` | Return manifest, scope, redaction mode, size, readiness, and refusal details. |
| `EXPORT` | Stream the bundle to an authorized client route. |
| `VERIFY` | Check manifest integrity and redaction proof. |
| `DROP` | Remove bundle material according to retention policy. |

Support bundles should include enough authorized evidence to diagnose startup refusal, recovery-required state, configuration failure, transaction blocking cleanup, index readiness, storage pressure, parser/UDR load failure, bridge refusal, and management denial.

## EXPLAIN And EXPLAIN ANALYZE

`EXPLAIN` returns an authorized plan report. `EXPLAIN ANALYZE` executes the statement and returns measured execution details in addition to the plan.

```sql
explain
select o.order_id
from app.orders o
where o.customer_id = :customer_id;
```

```sql
explain analyze
select o.order_id
from app.orders o
where o.customer_id = :customer_id;
```

Plan reports can include:

- statement and plan UUIDs;
- descriptor-bound query shape;
- selected indexes and access paths;
- join order and join methods;
- estimated rows, costs, and selectivity;
- actual rows and timing for `EXPLAIN ANALYZE`;
- spill, memory, cache, and temporary work indicators;
- recheck requirements for index, document, vector, search, graph, or bridge evidence;
- refusal vectors and recovery fences.

Plan reports must not expose hidden object names, protected predicates, secrets, raw protected values, or unauthorized row counts.

## Runtime Packages, Parsers, And UDRs

Management inspection covers parser and UDR packages because they are runtime boundaries.

```sql
show management parsers;
show management parser pool sbsql;
show management udr packages;
show management udr package app_text;
alter management parser pool sbsql reload;
alter management udr package app_text reload;
```

Reported fields should include package name, UUID, ABI/profile version, load path class, capability set, readiness, refusal details, active sessions, crash/restart counts, and redacted diagnostics. Raw filesystem paths, secrets, and protected configuration values must not be disclosed unless policy admits them.

## Sessions, Statements, And Cancellation

Operational users need to identify and safely cancel work.

```sql
show management statements with state running;
show management waits;
alter management statement :statement_id cancel;
alter management session :session_id drain;
```

Cancellation is cooperative unless policy admits stronger action. A cancellation request must:

- identify the target through an authorized descriptor;
- record a diagnostic trail;
- preserve transaction finality;
- avoid leaving partial durable state;
- return a clear message vector when the target is already finished, hidden, or not cancellable.

## Jobs And Maintenance

Background work includes index rebuilds, cleanup, validation, bundle creation, configuration reloads, backup/restore flows, replication flows, and other admitted maintenance.

```sql
show management jobs;
show management job :job_id;
alter management job :job_id cancel;
alter management storage validate;
alter management index app.orders_customer_idx rebuild;
```

Job reports should include state, owner, progress, checkpoint, current phase, last message vector, safe cancellation state, retry policy, and recovery behavior.

## Storage And Recovery State

Storage management reports must be precise but redacted.

```sql
show management storage;
show management filespaces;
show management recovery;
show management cleanup;
```

These reports should explain:

- whether recovery is required;
- whether write admission is fenced;
- which filespaces are primary, attached, detached, read-only, or degraded;
- whether cleanup is blocked by snapshots or transactions;
- whether index, document, vector, search, graph, or time-series evidence is stale;
- what operator action is required, if any.

Repair and low-level file manipulation are not generic management shortcuts. They must use the explicit SBsql lifecycle or diagnostic surfaces that own those operations.

## Security And Disclosure

Management statements are security-sensitive.

| Rule | Contract |
| --- | --- |
| Inspection privilege | A user can inspect only authorized runtime and catalog projections. |
| Control privilege | Drain, stop, reload, cancel, terminate, validate, flush, rotate, and bundle operations require explicit authority. |
| Sandbox root | Sandboxed sessions cannot see or control targets outside their root. |
| Protected values | Secrets, raw credentials, protected material, file locations, snippets, and raw payloads are redacted by default. |
| Message vectors | Refusals identify the diagnostic class without leaking hidden targets. |
| Audit | Control actions should produce durable audit or diagnostic evidence where policy admits it. |

## Diagnostics And Refusals

| Condition | Expected diagnostic class |
| --- | --- |
| Target hidden or not found | Object resolution or sandbox denied. |
| Missing inspection privilege | Authorization denied. |
| Missing control privilege | Management control denied. |
| Target not drainable, stoppable, reloadable, or cancellable | Operation unsupported for target. |
| Configuration validation failed | Configuration validation error. |
| Reload failed | Configuration reload refused; prior configuration remains active. |
| Support bundle redaction failed | Support bundle refused. |
| Explain target hidden | Observability denied. |
| Recovery-required state | Operation fenced until recovery action completes. |
| Provider or package unavailable | Runtime package unavailable or incompatible. |
| Protected field requested | Protected-material redaction or denial. |

## Proof Expectations

The management proof suite should include:

- `SHOW HEALTH`, `SHOW READINESS`, `SHOW LIVENESS`, and `SHOW DIAGNOSTICS`;
- `SHOW MANAGEMENT` for sessions, statements, waits, jobs, config, storage, indexes, memory, cache, parser pools, UDR packages, and support;
- `ALTER MANAGEMENT` drain, undrain, reload, cancel, validate, flush, rotate, and refusal paths;
- configuration validate, reload success, reload failure, history, and effective rendering;
- support bundle create, describe, verify, export-to-client-stream, drop, redaction, and manifest proof;
- `EXPLAIN` and `EXPLAIN ANALYZE` with authorized and hidden objects;
- cancellation during long-running reads, writes, index work, bundle generation, and streaming work;
- recovery-required, sandbox denied, unauthorized, unavailable package, resource pressure, and malformed target refusals;
- proof that management commands do not bypass transaction finality, recovery fencing, or protected-material policy.

## Verification Checklist

| Check | Required outcome |
| --- | --- |
| Parse | Management and observability statements are recognized by SBsql. |
| Bind | Targets, identifiers, options, parameters, and result descriptors resolve. |
| Authorize | Effective user or agent UUID may inspect or control the target. |
| Admit | SBLR management or observability route is accepted by the engine verifier. |
| Execute | Runtime action preserves service, transaction, and recovery invariants. |
| Redact | Protected fields are omitted or redacted before rendering. |
| Diagnose | Refusals return canonical message vectors. |
| Prove | Full-test runs regenerate management proofs without external directories. |
