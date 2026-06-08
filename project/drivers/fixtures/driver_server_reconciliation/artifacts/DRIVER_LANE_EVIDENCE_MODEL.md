# Driver Lane Evidence Model

Search key: `DRIVER-SERVER-RECONCILIATION-DRIVER-LANE-EVIDENCE-MODEL`.

## Required Per-Lane Artifacts

Every driver, adapter, and tool lane must produce:

| Artifact | Requirement |
| --- | --- |
| Row-status YAML | Uses `public_contract_snapshot`. |
| Package manifest | Identifies lane, API family, version, ingress modes, auth methods, TLS profile, type mapping, diagnostics profile, and conformance profile. |
| Build evidence | Reproducible commands, OS/runtime/architecture matrix, dependency lock evidence where applicable. |
| Unit test evidence | Lane-native unit test report. |
| CTest evidence | CTest label and test name for every implemented row. |
| Server-verification packet | Live route command, auth config, expected output, pass/fail rule. |
| Compatibility matrix | Comparison against ODBC/JDBC/.NET/BIC behavior as applicable. |
| Benchmark evidence | Uses production route and records TPS, latency, allocation, TLS overhead, prepared reuse, and bulk throughput where applicable. |
| Release evidence | SBOM, package signing, registry publication readiness, known-gap list. |

## Required Status Rules

- Required rows close only as `implemented_and_proven`.
- Conditional rows close only as `implemented_and_proven` or
  `not_applicable_with_citation`.
- `implemented_without_evidence` is a blocker.
- `server_unspecified` is a blocker.
- `undocumented_implementation` is a blocker.
- `not_started` is a blocker.

## Route Evidence

A row that depends on server behavior must prove the declared route:

```text
client/driver/tool
  <-> SBWP/TLS or admitted IPC
  <-> manager/listener as configured
  <-> parser pool
  <-> parser
  <-> IPC
  <-> sb_server
  <-> engine authentication/authorization/policy
  <-> SBLR execution and MGA transaction authority
  <-> response return path
```

Direct engine calls are useful unit tests but do not satisfy driver full-route
evidence.

## Adapter Evidence

Adapters consume driver lanes but still must prove:

- DSN/auth/TLS option forwarding,
- schema/metadata introspection,
- query/DML parameter forwarding,
- transaction/cancel/async behavior through the host framework,
- diagnostic preservation,
- clean shutdown and packaging evidence.
