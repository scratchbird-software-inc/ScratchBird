# ScratchBird Julia Driver

Julia DBInterface/Tables lane for ScratchBird.

This lane now has a real Julia package layout:

- `Project.toml`
- `src/ScratchBird.jl`
- `tools/sb_isql_julia.jl`
- `test/runtests.jl`
- [Baseline requirement mapping](BASELINE_REQUIREMENT_MAPPING.md)

## Status

The lane is native route-runner source at this stage. It exposes the intended
DBInterface/Tables API surface and native transport setup, then fails closed
with SQLSTATE-style diagnostics for unsupported TLS, embedded transport,
attach/create, MGA transaction commands, and SBWP statement execution.

It does not delegate to another ScratchBird driver or command-line tool, and it
does not return fabricated rows.

## Runtime Dependencies

Instantiate this lane before live use:

```bash
julia --project=project/drivers/driver/julia -e 'using Pkg; Pkg.instantiate()'
```

Required Julia packages:

- `DBInterface`
- `Tables`

If either package is missing, `tools/sb_isql_julia.jl` exits non-zero and writes
the complete artifact set with a precise dependency diagnostic.

## Native Tool

The native conformance tool is:

```bash
julia --project=project/drivers/driver/julia project/drivers/driver/julia/tools/sb_isql_julia.jl \
  --database example.sbdb \
  --host 127.0.0.1 \
  --port 3092 \
  --user sysdba \
  --password masterkey \
  --role '' \
  --sslmode disable \
  --route listener-parser \
  --parser-mode server-parser \
  --page-size 8k \
  --namespace users.public.examples.julia.manual \
  --input script.sbsql \
  --output build/julia/stdout.log \
  --error build/julia/stderr.log \
  --diagnostics build/julia/diagnostics.jsonl \
  --metrics build/julia/process-metrics.jsonl \
  --transcript build/julia/wire-transcript.jsonl \
  --summary build/julia/summary.json
```

The tool accepts the common complete-coverage matrix arguments and writes:

- `command-events.jsonl`
- `summary.json`
- `diagnostics.jsonl`
- `wire-transcript.jsonl`
- `timing-groups.json`
- `result-digests.json`
- `metadata-snapshots.json`
- `route-environment.json`
- `process-metrics.jsonl`
- `security-refusals.json`
- `native-api-coverage.json`
- `code-example-review.json`
- `junit.xml`
- `stdout.log`
- `stderr.log`

## API Sketch

```julia
using DBInterface
using Tables
using ScratchBird

conn = DBInterface.connect(
    ScratchBirdDriver();
    database = "example.sbdb",
    host = "127.0.0.1",
    port = 3092,
    user = "sysdba",
    password = "masterkey",
    sslmode = "disable",
)

stmt = DBInterface.prepare(conn, "SELECT 1")
result = DBInterface.execute(stmt)
rows = collect(Tables.rows(result))
close(conn)
```

Until the Julia SBWP executor is present, `DBInterface.execute` fails
closed with `0A000`.
