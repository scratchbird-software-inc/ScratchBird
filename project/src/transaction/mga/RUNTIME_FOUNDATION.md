# Transaction MGA Runtime Foundation

This package implements `RUNTIME-014`: local transaction identity, transaction UUID identity, transaction states, and legal transition checks.

## Scope

The package owns:

- local transaction IDs for node-local inventory efficiency;
- transaction UUID identity for durable/global correlation;
- transaction scope classification for local-node and cluster-global transaction identity;
- the initial transaction state enum;
- the initial legal transition table;
- deterministic diagnostics for invalid identities and illegal transitions.

## Identity rules

- Local transaction IDs are local inventory handles only.
- Transaction UUIDs are engine identity and must be UUIDv7.
- UUIDv1 through UUIDv6 are reference/client compatibility values only and cannot be transaction identity.
- A transaction identity must carry both a valid local transaction ID and a typed transaction UUID.

## State rules

- State transitions must be table-driven.
- Recovery-only transitions require explicit recovery context.
- Terminal states may only advance to archival cleanup.
- This slice does not implement visibility, version chains, cleanup horizons, commit finality, or cluster decision proofs.

Those later responsibilities are covered by subsequent runtime slices.

## RUNTIME-015 row version metadata

This package also implements `RUNTIME-015`: row UUID identity, row-version metadata, version-chain links, and the first bounded visibility API.

The row-version layer owns:

- typed row identity;
- row-version identity as `row UUID + creator transaction + version sequence`;
- previous and next version-chain sequence links;
- row-version states for uncommitted, prepared, committed, rolled back, delete marker, limbo, and recovery-required records;
- conservative bounded visibility decisions.

## Row identity rules

- Row UUIDs are engine identity and must be UUIDv7.
- UUIDv1 through UUIDv6 are reference/client compatibility values only and cannot be row identity.
- Version identity is not a separate UUID kind in this slice; it is the row UUID plus the creator transaction identity plus a local version sequence.
- Version-chain links are sequence references only. They are not disk pointers and they are not cleanup authority.

## Visibility rules

- The bounded visibility API is not final MGA visibility.
- Limbo and recovery-required records return `requires_recovery`.
- Prepared and uncommitted records return `wait_for_transaction` unless the reader is allowed to see its own uncommitted version.
- Committed records are visible only when the creator transaction state is committed and the bounded read point permits it.
- Real OIT/OAT/OST, snapshot, cleanup horizon, and cluster finality logic are later slices.

## RUNTIME-016 copy-on-write mutation and cleanup

This package also implements `RUNTIME-016`: copy-on-write mutation planning, mutation phase transitions, and cleanup horizon policy inputs.

The copy-on-write layer owns:

- mutation intent for insert, update, delete, and system catalog update;
- mutation phase tracking from planned state through unpublished payload, transaction-pending publication, publish, rollback, and recovery-required states;
- evidence-before-publish checks;
- cleanup horizon structures;
- cleanup eligibility diagnostics.

## Copy-on-write rules

- MGA copy-on-write is the internal journaling model.
- WAL is not part of this mutation model.
- Inserts must not have a base version.
- Updates, deletes, and catalog updates must have a base version.
- A new version sequence must be after the base version sequence when a base version exists.
- Published mutation state requires evidence to be written first.

## Cleanup authority rules

- Cleanup eligibility requires authoritative inventory and horizon inputs before destructive reclaim.
- Non-authoritative horizons block cleanup.
- Invalid horizons block cleanup.
- Limbo and recovery-required row versions block cleanup.
- Archive and backup holds block cleanup.
- `eligible_requires_authority` is only a planning result and must not be used as destructive cleanup authority.

## DBOPEN-004 local transaction inventory

This package now includes the first local transaction inventory skeleton for the database create/open vertical slice.

The inventory layer owns:

- allocation of local transaction IDs for node-local inventory efficiency;
- binding local transaction IDs to transaction UUIDv7 identity;
- begin, commit, and rollback helpers over the existing table-driven transaction-state transition model;
- evidence-before-success requirements for terminal commit/rollback states;
- lookup by local transaction ID.

This API still does not implement OIT/OAT/OST, snapshots, final visibility, sweep, archive, recovery, cluster finality, or distributed decision proofs.
