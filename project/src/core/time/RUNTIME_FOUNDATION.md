# Core Time Runtime Foundation

This package implements `RUNTIME-003`: monotonic time, wall-clock time, standardized cluster-time placeholders, and UUIDv7 timestamp coordination support.

## Scope

The package owns:

- monotonic time readings for durations and scheduling;
- wall-clock readings for diagnostics, metrics, and timestamp conversion;
- UUIDv7 Unix epoch millisecond extraction from wall-clock time;
- single-node local time authority observation using OS wall-clock and monotonic clock snapshots;
- monotonic-clock enforcement for elapsed durations, lease/cooldown math, timeout math, and freshness windows;
- wall-clock rollback, forward-jump, monotonic-regression, and invalid wall-clock diagnostics;
- standardized cluster-time containers with explicit uncertainty;
- diagnostics for invalid or unsafe time authority usage;
- ISO-8601 UTC formatting for evidence and reports.

## Hard rule

Time is not transaction authority.

Cluster-standardized time and UUIDv7 timestamps may support identity generation, leases, metrics, diagnostics, TTL calculations, retry scheduling, and operator reasoning. They must not prove commit finality, row visibility, cleanup eligibility, route authority, authorization, conflict resolution, or strong replica-read safety.

## Single-node authority

In a non-cluster build, wall-clock time is sourced from the operating system and is accepted only as local wall time. Monotonic time is the required source for elapsed durations and must be used for timeout, lease, cooldown, and freshness calculations.

`ObserveLocalNodeClock` maintains the local observation state and fails closed by policy for monotonic regression and wall-clock rollback. Wall-clock forward jumps are detectable and may fail closed when the selected policy requires it.

The local time authority records observations; it does not authorize MGA visibility, cleanup, commit finality, route decisions, or security decisions.
