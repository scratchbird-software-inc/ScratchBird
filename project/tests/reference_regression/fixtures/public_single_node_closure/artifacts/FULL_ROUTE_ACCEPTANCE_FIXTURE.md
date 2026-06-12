# Full Route Acceptance Fixture

Search key: `PUBLIC_SINGLE_NODE_FULL_ROUTE_ACCEPTANCE_FIXTURE`

The final fixture must exercise:

- SBSQL parser path.
- At least one reference parser path from each implemented reference family batch.
- Driver/tool route over SBWP/TLS.
- Listener, parser pool, parser process, IPC, server, engine security, SBLR
  execution, MGA transaction, and response return path.
- Catalog, datatype, index, storage, security, audit, and wire metadata surfaces.
- Commit, rollback, reconnect, cancel/timeout, shutdown notification, and
  recovery reopen behavior.
- Original reference regression replay where reference tools are available.

The fixture fails if it bypasses the required route or if any layer becomes
transaction, storage, or security authority outside its declared role.
