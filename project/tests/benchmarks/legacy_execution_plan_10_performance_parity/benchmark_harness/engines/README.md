# Engine Containers

This directory holds Dockerfiles and entrypoints for benchmark target engines:

- Firebird (`sb-benchmark-firebird`)
- MySQL (`sb-benchmark-mysql`)
- PostgreSQL (`sb-benchmark-postgresql`)

## Recommended Control Path

Use `scripts/start-engine.sh` from project root, not raw `docker run` commands.

```bash
cd project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness

./scripts/start-engine.sh firebird start
./scripts/start-engine.sh mysql start
./scripts/start-engine.sh postgresql start

./scripts/start-engine.sh mysql status
./scripts/start-engine.sh mysql logs
./scripts/start-engine.sh mysql stop
```

`start-engine.sh` behavior:

- Ensures one benchmark engine runs at a time for isolation.
- Creates `benchmark-net` network if missing.
- Auto-selects free host ports if defaults are occupied.
- Writes discovered ports to `.benchmark-engine-ports/<engine>.env`.
- Writes version JSON to `results/<engine>-version.json`.

## Port Configuration

Defaults:

- Firebird: `3050`
- MySQL: `3306`
- PostgreSQL: `5432`

Override with environment variables:

- `BENCHMARK_FIREBIRD_PORT`
- `BENCHMARK_MYSQL_PORT`
- `BENCHMARK_POSTGRESQL_PORT`

## Docker Access Requirements

`docker info` must succeed as current user.

If not:

```bash
sudo usermod -aG docker $USER
newgrp docker
```

Or run scripts with `sudo`.

## Integration With Matrix Runs

`scripts/run-benchmark-matrix.sh` starts/stops engines through `start-engine.sh` automatically.
Manual engine control is mainly for debugging single-suite runs.

## Related Docs

- [README.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/README.md)
- [docs/OPERATIONS_RUNBOOK.md](project/tests/benchmarks/legacy_execution_plan_10_performance_parity/benchmark_harness/docs/OPERATIONS_RUNBOOK.md)
