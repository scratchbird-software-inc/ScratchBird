# Full Route Acceptance Fixture

Search key: `PUBLIC_RELEASE_FOUNDATION_FULL_ROUTE_FIXTURE`

## Required Route

```text
client
  -> SBWP/TLS and INET layer
  -> listener
  -> pool-allocated SBSQL parser
  -> SBPS IPC
  -> sb_server
  -> engine
  -> authentication policy and verification
  -> engine session and transaction
  -> SBLR execution
  -> response through parser and SBWP/TLS
  -> client
```

## Required Scenario

1. Create or open an example database.
2. Authenticate through the engine authority using benchmark/test credentials.
3. Create catalog-visible table, constraint, and index-backed key metadata.
4. Insert valid rows and verify `sys.catalog_readable` projection.
5. Attempt invalid constraint write and verify canonical diagnostic.
6. Roll back a transaction and verify catalog and data visibility.
7. Commit a transaction and verify catalog generation publication.
8. Create a backup manifest and restore/verify it.
9. Create a PITR restore point and reject an out-of-coverage restore target.
10. Shut down cleanly and reopen with recovery checks.

## Acceptance

The `full_route_acceptance_fixture_gate` passes when the fixture contract is
implemented and the `public_release_foundation_full_route_gate` executes the
route without bypassing SBWP/TLS, parser IPC, engine security, SBLR admission,
or MGA transaction authority.
