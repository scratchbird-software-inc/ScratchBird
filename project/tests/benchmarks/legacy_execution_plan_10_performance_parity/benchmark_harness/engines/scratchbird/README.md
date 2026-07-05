# ScratchBird Benchmark Engine

This directory documents the ScratchBird benchmark service contract used by the
single-engine and matrix runners.

`ScratchBird-Benchmarks` now includes logical benchmark target registration for:

- `scratchbird-postgresql`
- `scratchbird-mysql`
- `scratchbird-firebird`
- `scratchbird-native`

Those targets are declared in `index-comparison-tests/registry/target_registry.json`.
Each target must resolve to an explicit ScratchBird runtime profile before a
matrix run can claim engine parity for that reference surface.

Runtime profile material belongs here when containerized ScratchBird benchmark
execution is required:

- service startup
- health checking
- benchmark credentials and database bootstrap
- mode-specific configuration for emulation or native execution

Native local execution uses `.benchmark-engine-ports/scratchbird.env`, generated
by `scripts/start-engine.sh` or an equivalent local runner.
