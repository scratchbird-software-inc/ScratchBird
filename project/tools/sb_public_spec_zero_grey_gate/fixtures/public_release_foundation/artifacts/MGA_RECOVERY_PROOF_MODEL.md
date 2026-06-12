# MGA Recovery Proof Model

Search key: `PUBLIC_RELEASE_FOUNDATION_MGA_RECOVERY_PROOF_MODEL`

## Purpose

This artifact is the required proof boundary for `PRF-040`. No P4 recovery,
checkpoint, dirty-manifest, backup, delta, PITR, or durability-ordering code may
be treated as ready to implement until this model is reviewed and the matching
gate evidence is recorded.

## Authority

The authoritative recovery model remains MGA. Recovery proof must use:

- transaction inventory state
- transaction visibility and commit order
- page generation state
- checkpoint generation records
- dirty object manifests
- operation envelopes
- checksums and coverage proofs

Recovery proof must not introduce WAL, redo-log, reference MVCC, parser-owned
transaction semantics, or external stream authority. Backup, delta, archive, and
PITR streams are evidence and transport surfaces only; they are not the source of
truth for visibility or transaction finality.

## Required Proof Inputs

Every P4 implementation slice must identify the exact proof inputs it consumes.

| Input | Required property |
| --- | --- |
| Transaction inventory | Identifies committed, active, rolled back, limbo, and unknown transaction states under MGA rules. |
| Commit order | Defines visibility order without relying on log replay as authority. |
| Page generation | Binds page versions to durable generation and allocation state. |
| Checkpoint generation | Marks a safe recovery boundary only after durability fences are satisfied. |
| Dirty manifest | Lists modified objects/pages with hashes and transaction evidence for accelerated recovery. |
| Operation envelope | Carries deterministic operation evidence for replay validation where replay is allowed. |
| Coverage proof | Proves backup, delta, and PITR streams are contiguous and complete for the requested recovery target. |

## Required Failure Behavior

- Missing proof fails closed.
- Broken checksums fail closed or quarantine according to policy.
- Coverage gaps fail restore, delta apply, and PITR selection.
- Unknown transaction state is never promoted to committed.
- Dirty manifest absence may fall back to bounded scan only when the persistent
  format policy permits it.
- Cluster paths remain fail-closed unless the public single-node path explicitly
  owns the operation.

## Required Gates

`PRF-040` is complete only when this artifact is reflected in:

- `mga_checkpoint_generation_gate`
- `mga_dirty_manifest_emission_gate`
- `mga_recovery_idempotency_gate`
- `backup_restore_manifest_proof_gate`
- `mga_delta_coverage_gate`
- `pitr_rollforward_gate`
- `page_durability_ordering_gate`

## Acceptance Rules

- PRF-041 must not start until PRF-040 records this artifact as reviewed.
- Any implementation that requires WAL, redo-log replay, or parser transaction
  authority fails the execution_plan.
- Every recovery test must state which proof inputs it corrupts, removes, or
  validates.
- The final audit must cite this artifact when closing every
  `mga_recovery_backup_pitr` target gap.
