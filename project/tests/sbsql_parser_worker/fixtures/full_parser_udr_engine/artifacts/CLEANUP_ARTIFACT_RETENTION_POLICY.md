# Cleanup and Artifact Retention Policy

Status: complete
Search key: `FSPE-CLEANUP-ARTIFACT-RETENTION-POLICY`
Generated: 2026-05-07 20:32:37 EDT

## Purpose

This pre-generation policy prevents the full SBSQL fixture and validation corpus from filling disk while preserving evidence required for audit and reruns.

## Current Baseline Risk

P0B recorded the filesystem at high utilization in `artifacts/baseline/df.log`. Generated conformance work must therefore track disk growth from the start of P1, not wait for final cleanup.

## Retention Classes

| Class | Examples | Retention rule |
| --- | --- | --- |
| Required evidence | P0 reports, final audits, validation summaries, generated coverage matrices | Keep in execution_plan artifacts. |
| Reusable fixtures | CTest fixture manifests, golden expected outputs, replay manifests | Keep under `project/tests/...` when promoted to durable regression assets. |
| Large transient outputs | temporary databases, fuzz corpora expansions, server logs, socket dirs, generated build trees | Keep outside canonical specs; delete after summary unless explicitly retained. |
| Sensitive diagnostics | security/redaction logs, hidden-object probes, auth material traces | Redact before retention; keep only safe message-vector evidence. |
| Build logs | configure/build/ctest logs | Keep focused logs under owning execution_plan artifact subdirectory. |

## Disk Budget Rules

- Each generated fixture batch must record expected fixture count and estimated size before generation.
- Any shard expected to exceed 250 MiB requires coordinator approval and cleanup plan.
- Any full validation run that creates temporary databases or sockets must record cleanup paths before it starts.
- If free disk falls below 10 percent or 50 GiB, whichever is larger, the coordinator pauses new generation and records a failure inventory row.

## Cleanup Rules

- Do not delete canonical specs, execution_plan evidence, or durable regression fixtures as cleanup.
- Temporary databases, socket directories, and generated scratch logs must have explicit path patterns before removal.
- Destructive cleanup commands require human approval unless an exact approved cleanup rule already exists.
- Final audit must link retained evidence and list discarded transient artifact classes.

## Acceptance

P1+ slices that generate registries, fixtures, logs, temporary databases, or replay corpora must use this policy and update their slice evidence with retained paths and cleanup decisions.

## Final-Run Retention Evidence

| Artifact class | Retained path or pattern | Final decision |
| --- | --- | --- |
| Execution_Plan evidence | `project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof/artifacts/` | Retain all reports, gates, CSVs, validation results, and final audits. |
| Generated parser-worker fixtures | `project/tests/sbsql_parser_worker/generated/` | Retain as durable regression assets. |
| Generated registry outputs | `project/src/parsers/sbsql_worker/registry/generated/` | Retain as deterministic generated source. |
| Deterministic manifest | `project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv` | Retain and update when generated tracked artifacts change. |
| Validation build tree | `build/sbsql_parser_worker_validation/` | Retain until human review completes; safe to delete afterward. |
| Temporary logs | `/tmp/sb_*.log` | Discard after summarized validation evidence is retained. |
| Temporary sockets | `/tmp/sb_*.sock` | Discard after test completion. |
| Temporary databases | `/tmp/sb_*.sbdb` and `/tmp/sb_*.sbdb.*` | Discard after persistence/restart evidence is retained. |
| Python bytecode caches | `**/__pycache__/` | Discardable transient interpreter output. |

## Final Cleanup Commands

These commands are publication guidance and should be run only after final review or with explicit human approval where destructive cleanup policy requires it.

```bash
rm -rf build/sbsql_parser_worker_validation
rm -f /tmp/sb_*.log
rm -f /tmp/sb_*.sock
rm -f /tmp/sb_*.sbdb /tmp/sb_*.sbdb.*
find project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/public_proof -type d -name __pycache__ -prune -print
```

Do not delete canonical specs, tracked generated fixtures, deterministic manifests, validation-result reports, or final closure artifacts as cleanup.

## FSPE-014E Closure

The final policy keeps audit evidence and durable regression inputs while classifying build trees, temporary logs, sockets, temporary databases, and interpreter caches as disposable after review.
