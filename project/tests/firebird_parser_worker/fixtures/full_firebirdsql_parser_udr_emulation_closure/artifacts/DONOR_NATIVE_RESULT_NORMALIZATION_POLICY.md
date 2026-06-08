# Firebird Donor-Native Result Normalization Policy

Status: draft
Search key: `FIREBIRD_DONOR_NATIVE_RESULT_NORMALIZATION_POLICY`

## Purpose

Normalize original Firebird tool and regression output so ScratchBird can compare donor-visible behavior without false failures from environment-specific values.

## Normalize

- Absolute paths.
- Temporary directory names.
- Process identifiers.
- Connection identifiers.
- Transaction identifiers.
- Page identifiers.
- Attachment identifiers.
- Timestamps and durations.
- Hostnames and loopback addresses.
- Object UUIDs where ScratchBird identity differs from Firebird internal IDs.
- Warning ordering only when the Firebird status-vector contract permits equivalent warning chains.

## Do Not Normalize

- SQL result values.
- Column labels.
- Type names.
- Precision, scale, nullability, charset, collation, and descriptor metadata.
- Status-vector symbolic names.
- SQLCODE and SQLSTATE.
- Affected row counts.
- Command tags and service action results.

## Required Gate

`firebird_donor_native_result_normalization_gate` fails if a comparison depends on unapproved normalization.
