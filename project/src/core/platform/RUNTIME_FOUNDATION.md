# Core Platform Runtime Foundation

This package implements the first runtime foundation slice for ScratchBird private implementation work.

## Scope

The package owns:

- fixed-width type aliases;
- compile-time size assertions;
- mandatory compiler `int128` / `uint128` primitive detection;
- endian helpers for deterministic little-endian storage access;
- base status codes;
- base diagnostic records and diagnostic validation helpers;
- typed UUID containers for database, cluster, filespace, schema, object, row, page, transaction, session, and principal identities;
- time containers for monotonic time, wall-clock time, and standardized cluster-time placeholders;
- compile feature gate reporting.

## Diagnostic record closure rules

`MakeDiagnostic` is the central constructor for platform diagnostic records. It records status, diagnostic code, message key, arguments, trace id, source component, and remediation hint.

`ValidateDiagnosticRecord` is the central structural validator. Required-path diagnostics must satisfy these rules:

- diagnostic code is non-empty and uses canonical diagnostic characters;
- message key is non-empty and dotted;
- source component is present;
- non-OK diagnostics include a remediation hint;
- OK diagnostics use `info` or `trace` severity;
- non-OK diagnostics do not use `info` severity;
- diagnostic text must not contain placeholder/stub/TODO/FIXME/generic-error wording.

Subsystem implementation slices must replace placeholder or generic diagnostics before claiming final behavior-complete status.

## Runtime authority rules

- This layer does not execute SQL, SBLR, parser output, storage operations, or cluster authority.
- This layer provides primitives that later layers must use instead of redefining local aliases, local status models, or local UUID containers.
- Cluster time is represented but is not authority for visibility. Visibility authority remains transaction state, catalog state, MGA state, and cluster decision proofs where applicable.

## Runtime capability checks

`runtime_capabilities` implements the mandatory library and provider capability manifest for early runtime startup.

Mandatory capabilities:

- `numeric.int128`
- `numeric.uint128`
- `numeric.real128`
- `compiler.llvm`

Optional capabilities:

- `gpu.cuda`
- `gpu.hip`
- `gpu.opencl`

Missing mandatory capabilities are fatal capability failures. Optional GPU providers may be absent without failing core startup; later GPU-specific profiles must check the provider they require before activation.
