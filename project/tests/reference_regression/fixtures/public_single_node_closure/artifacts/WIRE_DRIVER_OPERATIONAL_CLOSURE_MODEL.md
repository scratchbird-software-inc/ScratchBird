# Wire Driver Operational Closure Model

Search key: `PUBLIC_SINGLE_NODE_WIRE_DRIVER_OPERATIONAL_CLOSURE_MODEL`

Wire and driver closure must use declared routes:

```text
client/tool/driver
  -> SBWP/TLS and INET layer
  -> listener
  -> parser pool
  -> parser
  -> IPC
  -> sb_server
  -> engine security
  -> SBLR execution and MGA transaction authority
  -> response path back to client
```

Required behavior:

- Server and listener lifecycle commands have clean shutdown and force-shutdown
  behavior with client notification.
- SBCT controls parser pool lifecycle and health.
- Local IPC and SBWP protocol versions fail closed on mismatch.
- DriverPackageManifest is enforced for every driver/adaptor/tool lane.
- The manifest includes native, language, interface, tool, and application
  adaptor lanes required by `SB-PUBLIC-GAP-0080` through `SB-PUBLIC-GAP-0086`,
  including ADBC, Flight SQL, R2DBC, Perl DBI, Julia, dbt, Airbyte, PowerBI,
  Tableau, and Looker.
- Package-contract gates must prove each package declares engine-owned
  authentication/authorization, SBWP/TLS, MGA finality, `sys.information`
  metadata, UUID identity, and no hidden replay.
- Driver tests are in CTest and rerun in full regression.
- Reconnect finality forbids hidden replay; idempotency is explicit.
