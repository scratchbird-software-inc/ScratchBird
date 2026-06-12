# Database Lifecycle Hardening Report

Search key: `DATABASE-LIFECYCLE-HARDENING-REPORT`

Gate: `DBLC_P17_HARDENED`

## Scope

This report records DBLC-017 hardening evidence for the lifecycle fault-injection and authority-drift closure gate.

Covered gate labels:

| Gate | Evidence |
| --- | --- |
| `database_lifecycle_fault_injection` | Runtime conformance test covering partial tx1 create evidence loss, interrupted tx2 activation evidence loss, unclean startup recovery, stale owner classification, identity mismatch refusal, engine authentication denial, parser SQL bypass refusal, reference non-file diagnostic behavior, cluster fail-closed behavior, and MGA recovery evidence without write-ahead terminology. |
| `DBLC_STATIC_AUTHORITY_DRIFT_GATES` | Static authority scan over accepted lifecycle, parser-admission, reference-mapping, server-session, MGA, cluster-boundary, and backup/archive authority paths. |
| `DBLC_STATIC_NO_LIFECYCLE_PLACEHOLDERS` | Static scan proving accepted lifecycle code paths do not retain TODO, FIXME, NotImplemented, not implemented, stub, placeholder, future work, or deferred wording. |
| `mga_policy_gate` | External ScratchBird MGA authority scanner must pass before DBLC-017 closure. |

## Authority Rules

The hardening gate preserves these rules:

| Rule | Closure evidence |
| --- | --- |
| MGA is the transaction and recovery authority. | Partial tx1, interrupted tx2, and unclean startup cases are checked against durable startup and transaction-inventory evidence. |
| Parser dialects do not execute SQL or own finality. | Raw SQL and SQL-text-bearing SBLR envelopes are rejected before engine admission; transaction control envelopes require engine/public-ABI dispatch. |
| Reference dialect support has no reference storage or reference SQL authority. | Firebird non-file surfaces return exact diagnostics or ScratchBird lifecycle SBLR mappings with reference SQL and file effects disabled. |
| Cluster paths fail closed until cluster mapping exists. | Standalone cluster route and cluster transaction admission refuse before entering cluster route details. |
| Accepted lifecycle code paths do not rely on placeholder behavior. | Static placeholder gate scans lifecycle source paths and requires explicit platform-unavailable diagnostics instead of implementation-placeholder language. |

## Validation

Validated command set:

```bash
cmake -S project -B build
cmake --build build --target database_lifecycle_fault_injection_conformance -j2
ctest --test-dir build --output-on-failure -L database_lifecycle_fault_injection
ctest --test-dir build --output-on-failure -L mga_transaction_regression
python3 -B ${PUBLIC_TOOL_ROOT}/skills/scratchbird-mga-transaction-authority/scripts/mga_policy_gate.py --repo ${PROJECT_ROOT}
```

Validation results:

| Command | Result |
| --- | --- |
| `cmake -S project -B build` | passed |
| `cmake --build build --target database_lifecycle_fault_injection_conformance -j2` | passed |
| `ctest --test-dir build --output-on-failure -L database_lifecycle_fault_injection` | passed, 5/5 |
| `cmake --build build -j2` | passed |
| `ctest --test-dir build --output-on-failure -L mga_transaction_regression` | passed, 50/50 |
| `mga_policy_gate.py --repo ${PROJECT_ROOT}` | passed |
