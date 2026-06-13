# Reference Driver Compatibility Route Snapshot

This public contract defines the evidence shape used when a reference client
tool or compatibility driver is compared against ScratchBird.

Required ScratchBird route:

- client tool or driver
- SBWP/TLS or admitted IPC transport
- listener
- parser worker
- server admission
- engine authorization policy
- engine MGA transaction inventory

Rejected evidence:

- parser-only success
- direct engine calls
- direct storage calls
- result-only comparisons without diagnostics and metadata

Comparison scope:

- row values
- row counts
- SQLSTATE-compatible diagnostics
- metadata names and types
- transaction finality through engine-owned MGA state

Unsupported profile features must fail closed with a compatibility-profile
refusal diagnostic. A driver, adapter, or parser may package a request, but the
server remains the authority for authorization, UUID admission, and transaction
finality.
