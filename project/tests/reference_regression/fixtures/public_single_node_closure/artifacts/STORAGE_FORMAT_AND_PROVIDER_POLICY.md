# Storage Format And Provider Policy

Search key: `PUBLIC_SINGLE_NODE_STORAGE_FORMAT_AND_PROVIDER_POLICY`

Storage closure must preserve MGA authority and durable page/filespace identity.

Required behavior:

- Format changes require version, upgrade, downgrade refusal, and repair
  diagnostics.
- Every filespace, including the first database file, has its own UUID and role.
- Database lifecycle transaction 1, transaction 2, and final shutdown
  transaction remain visible through MGA rules.
- Cloud-provider APIs must be real provider-boundary implementations with local
  emulator tests when credentials are not supplied.
- Provider snapshots are not database-consistent unless coordinated by
  ScratchBird lifecycle and checkpoint rules.
- Foreign filespaces must enter quarantine until inspected and released.
