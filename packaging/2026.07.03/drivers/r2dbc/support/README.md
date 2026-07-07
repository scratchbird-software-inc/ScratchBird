# ScratchBird R2DBC Driver Lane

This lane is an R2DBC host-route package for ScratchBird. It provides the
source package structure, Java SPI surface anchors, tests, and the native
`sb_isql_r2dbc` tool expected by the driver matrix.

The native tool uses the Java R2DBC SPI directly through JPype when a runtime
classpath is available. It does not delegate to another ScratchBird driver or
to the shared CLI. Missing JVM bridge support, missing R2DBC SPI classes,
unsupported parser modes, unsupported routes, or connection failures are
reported through deterministic `SB_DRIVER_R2DBC_*` diagnostics and non-zero
exit status.

## Runtime Dependencies

- Python 3.11 or newer.
- `jpype` Python package.
- Java runtime.
- R2DBC SPI and Reactor jars in `SCRATCHBIRD_R2DBC_CLASSPATH` or `CLASSPATH`.
- A ScratchBird R2DBC provider on that classpath.

Optional environment variables:

- `SCRATCHBIRD_R2DBC_CLASSPATH`: path-separated R2DBC/Reactor/provider jars.
- `SCRATCHBIRD_R2DBC_DRIVER`: R2DBC driver option value, default `scratchbird`.

## Native Tool

```bash
project/drivers/driver/r2dbc/tools/sb_isql_r2dbc \
  --database example.sbdb \
  --host 127.0.0.1 \
  --port 3092 \
  --user sysdba \
  --password masterkey \
  --route listener-parser \
  --parser-mode server-parser \
  --page-size 8k \
  --namespace users.public.examples.r2dbc.manual \
  --input script.sbsql \
  --output build/r2dbc/stdout.log \
  --error build/r2dbc/stderr.log \
  --diagnostics build/r2dbc/diagnostics.jsonl \
  --metrics build/r2dbc/process-metrics.jsonl \
  --transcript build/r2dbc/wire-transcript.jsonl \
  --summary build/r2dbc/summary.json
```

## Authority Boundary

R2DBC is a reactive API surface only. ScratchBird engine authentication,
authorization, metadata visibility, SBLR admission, and MGA transaction
finality remain server-owned.
