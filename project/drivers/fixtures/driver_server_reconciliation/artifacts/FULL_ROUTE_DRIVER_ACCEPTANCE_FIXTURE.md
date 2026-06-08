# Full Route Driver Acceptance Fixture

Search key: `DRIVER-SERVER-RECONCILIATION-FULL-ROUTE-FIXTURE`.

## Required Route

The full-route fixture for this execution_plan is:

```text
client(cli/gui/driver/adaptor)
  <-> SBWP/TLS and INET layer or admitted local IPC
  <-> listener/net/port or manager proxy
  <-> pool-allocated parser
  <-> parser dialect worker
  <-> parser-server IPC
  <-> sb_server
  <-> engine authentication policy
  <-> authentication plugin/verification/accept/deny when configured
  <-> engine session and transaction creation when accepted
  <-> SBLR execution under MGA
  <-> response/message vector/result path back to client
```

The engine is the only authentication, authorization, and transaction/finality
authority. If the engine is its own provider, it compares protected verifier
material and never stores or compares unencrypted passwords.

## Required Fixture Database

The fixture must create or reuse an example database containing:

- `sys` schema tree,
- `sys.catalog`,
- `sys.information`,
- `sys.security`,
- `sys.metrics`,
- `users`,
- `users.public`,
- at least one `users.<account>` home schema,
- default policies,
- default roles/groups,
- timezone data,
- character set/collation data,
- metrics seed data.

All created catalog/system rows must use generated UUIDv7 values. Catalog
identity remains UUID-backed; human-readable names are resolved through the
identity resolver and rendered through `sys.information`.

## Required Driver Scenarios

- Connect by DSN/URL and structured config.
- Authenticate by every admitted auth method and fail closed for unsupported
  methods.
- Prepare/bind/execute direct SQL and prepared SQL.
- Execute stored procedure with OUT/INOUT once wire support exists.
- Execute multiple-result statement once `MultiResultEnvelope` is specified.
- Bind/fetch every D9 type.
- Exercise autocommit, explicit transaction, savepoint, cancel, timeout, reset,
  reconnect, and no hidden retry.
- Run metadata calls for every D13 row.
- Run bulk insert/export, LOB read/write, notification, and telemetry rows when
  the corresponding server surface is specified.

## Benchmark Validity Rule

Benchmark evidence is valid only if it uses this route. A benchmark that calls
the engine API directly is a useful unit benchmark but does not prove driver
route behavior.
