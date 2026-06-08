# Oracle Exact Rowset Extraction Status

Search key: SB_REFERENCE_DONOR_ORACLE_EXACT_ROWSET_STATUS

Scope correction: Oracle is not emulated by ScratchBird. Exact Oracle rowset extraction is not required for ScratchBird runtime catalog seeding and must not be used as runtime authority. This file is retained only as a superseded reference artifact.

## Status

Oracle private reference material identifies required dictionary, datatype, package, PL/SQL, and protocol surfaces. Exact rowsets from a newly-created Oracle database profile have not been captured.

The Oracle capability-reference pack is not a catalog-seeding target. Future work must create `CommercialCapabilityReferenceRecord` rows and, where needed, an outbound C++ connectivity UDR profile.

## Required extraction profiles

The final seed material must define at least:

- exact Oracle version profile
- exact database creation recipe
- default `DUAL` row
- default built-in schemas/users/roles/privileges
- default public synonyms
- default dictionary view rowsets for supported USER/ALL/DBA surfaces
- supplied package metadata and overload signatures
- built-in type metadata
- NLS defaults
- directory and LOB metadata where profile-created
- V$/GV$ visibility and runtime-generated-value profile
- redaction/generation rules for installation-specific values

## Generated-value classification

Every generated value must be classified as:

- stable donor alias over ScratchBird UUID
- deterministic Oracle profile value
- runtime instance value
- runtime connection value
- runtime transaction value
- local metric projection from `sys.metrics.*`
- cluster metric projection from `cluster.sys.metrics.*` only when cluster exists
- redacted secret
- forbidden host-private value
- source-backed pending

## Diagnostics

| Condition | Diagnostic vector |
|---|---|
| Exact default rowset missing | DONOR.ORACLE.CATALOG.EXACT_ROWSET_MISSING |
| Catalog column omitted | DONOR.ORACLE.CATALOG.COLUMN_MISSING |
| Catalog column order unknown | DONOR.ORACLE.CATALOG.COLUMN_ORDER_UNKNOWN |
| Package signature missing | DONOR.ORACLE.CATALOG.PACKAGE_SIGNATURE_MISSING |
| Raw secret copied into seed material | DONOR.ORACLE.CATALOG.RAW_SECRET_FORBIDDEN |
| Donor ID used as ScratchBird authority | DONOR.ORACLE.CATALOG.DONOR_ID_AUTHORITY_FORBIDDEN |
| GV$ surface exposed without cluster schema | DONOR.ORACLE.CATALOG.CLUSTER_SCHEMA_ABSENT |
| Source-backed pending row used as implemented | DONOR.ORACLE.SOURCE_BACKED_PENDING |

## Conformance gates

| Gate | Required result |
|---|---|
| ORACLE-SEED-001 | Newly-created Oracle profile dictionary rowsets match private seed manifest after generated-value normalization. |
| ORACLE-SEED-002 | USER/ALL/DBA visibility differs exactly according to donor privilege rules. |
| ORACLE-SEED-003 | Package, type, method, argument, and source metadata matches exact profile signatures. |
| ORACLE-SEED-004 | V$/GV$ report surfaces use only authorized local or cluster metrics. |
| ORACLE-SEED-005 | No raw secret, host path, private key, or installation-specific value appears in seed artifacts. |
| ORACLE-SEED-006 | Every donor identifier maps to a ScratchBird UUID-backed object or an explicitly classified runtime alias. |
