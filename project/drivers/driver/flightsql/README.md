# ScratchBird FlightSQL Driver Lane

This lane is an Apache Arrow Flight SQL host-route package for ScratchBird.
It provides source, documentation, tests, and the native `sb_isql_flightsql`
tool required by the driver matrix.

The tool uses PyArrow Flight SQL surfaces directly when they are available. It
does not delegate to another ScratchBird driver or to the shared CLI. Missing
PyArrow Flight SQL support, unsupported parser modes, unsupported routes, or
connection failures are reported through deterministic `SB_DRIVER_FLIGHTSQL_*`
diagnostics and non-zero exit status.

## Runtime Dependencies

- Python 3.11 or newer.
- `pyarrow.flight` with `FlightSqlClient` support.
- A ScratchBird Flight SQL endpoint.

## Native Tool

```bash
project/drivers/driver/flightsql/tools/sb_isql_flightsql \
  --database example.sbdb \
  --host 127.0.0.1 \
  --port 3092 \
  --user sysdba \
  --password masterkey \
  --route listener-parser \
  --parser-mode server-parser \
  --page-size 8k \
  --namespace users.public.examples.flightsql.manual \
  --input script.sbsql \
  --output build/flightsql/stdout.log \
  --error build/flightsql/stderr.log \
  --diagnostics build/flightsql/diagnostics.jsonl \
  --metrics build/flightsql/process-metrics.jsonl \
  --transcript build/flightsql/wire-transcript.jsonl \
  --summary build/flightsql/summary.json
```

## Authority Boundary

Flight SQL is a transport/API surface. ScratchBird engine authentication,
authorization, metadata visibility, SBLR admission, and MGA transaction
finality remain server-owned.
