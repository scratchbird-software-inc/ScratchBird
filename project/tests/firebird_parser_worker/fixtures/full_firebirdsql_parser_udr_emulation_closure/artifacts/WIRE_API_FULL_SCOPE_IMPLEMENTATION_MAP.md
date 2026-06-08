# Firebird Wire/API Full-Scope Implementation Map

Status: seeded
Search key: `FIREBIRD_WIRE_API_FULL_SCOPE_IMPLEMENTATION_MAP`

## Rule

All Firebird wire, API, service, replication, proxy, attach, transaction, statement, blob, event, trace, backup, restore, validation, security, and utility surfaces are in scope.

No Role A row may close without implementation or emulation ownership.

## Required Surface Families

- Wire attach and authentication.
- Statement prepare, describe, execute, fetch, close, and cancel.
- SQLDA and message metadata.
- DPB, TPB, SPB, and BPB parameter buffers.
- Blob segment, array, and slice APIs.
- Event API.
- Services API.
- Backup and restore APIs.
- Validation and statistics APIs.
- User, role, and security service APIs.
- Trace and profiler APIs.
- Replication and migration feed APIs.
- Proxy and migration topology APIs.

## Required Closure

Every row must have implementation or emulation owner, diagnostic contract, test owner, and CTest label.
