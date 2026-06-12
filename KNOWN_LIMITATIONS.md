# Known Limitations

ScratchBird is an early beta public source-review release.

## Public Release Scope

- The public repository is focused on the single-node open-core engine and public test/review surface.
- The production cluster provider is not included in this public source tree.
- Reference-system parser implementation source, reference-system documentation, raw upstream regression payloads, and native reference tools are not part of the public GitHub source surface. Reference regression tests are external fixtures consumed by ScratchBird-owned CTest harnesses.
- Driver and adaptor support is limited to the lanes that have current build, route, conformance, packaging, and release evidence.
- Benchmark harnesses are included for reproducibility. They are not performance claims.
- AI-facing services are release-candidate or tracked surfaces unless a release evidence matrix says otherwise.
- Unsupported features should fail closed with diagnostics rather than silently executing.

## Review Guidance

The presence of a source file, generated artifact, manifest row, test fixture, benchmark entry, or compatibility profile means the area is tracked. It is not by itself a support claim.
