# ScratchBird Core Beta QA Examples

This pack gives QA a small runnable smoke set for the core beta surface. The
examples are intentionally build-root aware and keep temporary files under
`/tmp` or CTest-managed locations.

The boundary is deliberate: client tools or parser-facing routes may accept
SBsql text, but the core engine examples exercise SBLR/internal API execution
through public fixtures. SQL text is not runtime authority inside the engine.
Object identity, transaction finality, and catalog authority stay inside the
engine/MGA layers.

## Scripts

Run from the repository root after configuring and building the referenced
targets:

```bash
project/examples/core_beta_qa/admin_lifecycle_smoke.sh "$PWD" "$PWD/build"
project/examples/core_beta_qa/embedded_public_abi_smoke.sh "$PWD" "$PWD/build"
project/examples/core_beta_qa/driver_route_smoke.sh "$PWD" "$PWD/build"
```

`admin_lifecycle_smoke.sh` runs the built admin lifecycle fixture, which covers
core create/open/attach/detach administration behavior through the current
database lifecycle gate.

`embedded_public_abi_smoke.sh` runs the public C++ ABI and SBLR admission
fixtures, covering embedded/public ABI dispatch and SBLR admission without
parser-side execution.

`driver_route_smoke.sh` runs representative driver/package gates through CTest:
the package manifest gate plus current Python and C++ driver gates. Optional
driver toolchains keep their own deterministic skip/waiver behavior in those
gates.
