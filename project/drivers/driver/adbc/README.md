# ScratchBird ADBC Driver Lane

This lane is a host-route ADBC package for the ScratchBird driver matrix.
It provides a source package and a native `sb_isql_adbc` route tool that
uses the Apache Arrow ADBC driver manager API when the required runtime driver
is available.

The tool does not delegate to another ScratchBird driver or to the shared CLI.
If the Python ADBC driver-manager package, the ScratchBird ADBC driver library,
or a requested runtime surface is unavailable, it fails closed and writes the
same diagnostics, summary, transcript, metrics, and evidence artifacts expected
by the native full-surface matrix.

## Runtime Dependencies

- Python 3.11 or newer.
- `adbc_driver_manager` Python package.
- A ScratchBird ADBC driver library identified by `SCRATCHBIRD_ADBC_DRIVER`.

Optional environment variables:

- `SCRATCHBIRD_ADBC_DRIVER`: ADBC driver library path or registered driver
  name. Required for live execution.
- `SCRATCHBIRD_ADBC_ENTRYPOINT`: optional ADBC driver entrypoint.

## Native Tool

```bash
project/drivers/driver/adbc/tools/sb_isql_adbc \
  --database example.sbdb \
  --host 127.0.0.1 \
  --port 3092 \
  --user sysdba \
  --password masterkey \
  --route listener-parser \
  --parser-mode server-parser \
  --page-size 8k \
  --namespace users.public.examples.adbc.manual \
  --input script.sbsql \
  --output build/adbc/stdout.log \
  --error build/adbc/stderr.log \
  --diagnostics build/adbc/diagnostics.jsonl \
  --metrics build/adbc/process-metrics.jsonl \
  --transcript build/adbc/wire-transcript.jsonl \
  --summary build/adbc/summary.json
```

Unsupported parser modes, embedded transport, create-database mode, or missing
ADBC runtime dependencies return non-zero with `SB_DRIVER_ADBC_*` diagnostics.

## Authority Boundary

ADBC requests are an API surface only. ScratchBird engine authentication,
authorization, metadata visibility, SBLR admission, and MGA transaction
finality remain server-owned.
