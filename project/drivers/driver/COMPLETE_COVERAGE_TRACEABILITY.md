# Driver Complete Coverage Traceability

This appendix is shared lane support material for the ScratchBird driver tree.
Each driver-specific `BASELINE_REQUIREMENT_MAPPING.md` supplies lane-local
source/test anchors. This file supplies the common complete-coverage gate and
registry-row names those lane mappings close against.

## Closure Gates

- `G1_MANIFEST`
- `G2_TRANSPORT`
- `G3_AUTH_SECURITY`
- `G4_EXECUTION`
- `G5_TYPES`
- `G6_TRANSACTIONS`
- `G7_METADATA`
- `G8_RESULTS_STREAMING_BULK`
- `G9_DIAGNOSTICS`
- `G10_CANCEL_TIMEOUT_POOLING`
- `G11_LOCAL_SBSQL`
- `G12_BIC_OVERLAY`
- `G13_RELEASE_EVIDENCE`
- `G14_FULL_SURFACE_CONFORMANCE`

## Registry Sections

- `D1.*` Driver discovery and registration.
- `D2.*` Connection establishment.
- `D3.*` Authentication and bootstrap.
- `D4.*` Transport security.
- `D5.*` DSN and connection string parsing.
- `D6.*` Statement execution.
- `D7.*` Parameter binding.
- `D8.*` Result fetching and decoding.
- `D9.*` Type system coverage.
- `D10.*` Transactions.
- `D11.*` Cancellation, timeouts, and interrupts.
- `D12.*` Diagnostics, errors, and warnings.
- `D13.*` Catalog and metadata access.
- `D14.*` Streaming and bulk transfer.
- `D15.*` Connection pooling.
- `D16.*` Async, reactive, and pipeline.
- `D17.*` Notifications and events.
- `D18.*` Logging, telemetry, and observability.
- `D19.*` Cursor and portal advanced behavior.
- `D20.*` Sharding and routing hints.
- `D21.*` Wrapper, escape, and provider extensions.
- `D22.*` Encoding and locale.
- `D23.*` Driver release-readiness evidence.
- `D24.*` ScratchBird-specific surfaces.
- `D25.*` Local SBsql intelligence.
- `D26.*` Commercial-grade driver release.
- `D27.*` Enterprise driver release.

## Authority Boundary

Driver-local SBsql parsing, predictive completion, SBLR generation, UUID
resolution cache hits, and bundle preparation are client-side optimizations.
They remain untrusted until server admission validates authorization,
visibility, policy epoch, schema epoch, language epoch, transaction context, and
payload integrity. MGA remains the transaction authority.
