# Datatype, Result, And Metadata Closure Model

Search key: `DRIVER-SERVER-RECONCILIATION-DATATYPE-RESULT-METADATA-MODEL`.

## Purpose

Close D7, D8, D9, D13, and D22 checklist rows by making type encoding,
parameter binding, result metadata, catalog metadata, charset, collation, and
timezone behavior implementable and testable.

## Required Type Authority

Every `CanonicalTypeId` that can appear on the wire must have:

- stable type id,
- binary payload layout,
- null encoding,
- empty-value distinction where applicable,
- precision/scale/length rules,
- text/charset/collation interaction,
- driver binding/fetch carrier rules for ODBC/JDBC/.NET,
- invalid conversion diagnostics,
- round-trip test vector.

## Required Parameter Authority

`ParameterDataPacket` must encode:

- positional and named parameters,
- explicit null versus string `'NULL'`,
- type id or inferred-type marker,
- precision, scale, size, charset, collation,
- input/output/input-output/return value direction,
- per-row array-bind sets,
- LOB chunk append references,
- structured/composite/range/array values,
- redaction map link.

## Required Result Authority

`RowDescription` and related descriptors must expose enough metadata for:

- ODBC `SQLDescribeCol`, `SQLColAttribute`, `SQLGetTypeInfo`,
- JDBC `ResultSetMetaData` and `DatabaseMetaData`,
- .NET `GetSchemaTable`, `GetColumnSchema`, `DbColumn`,
- nullability, precision, scale, display size, identity/autoincrement,
  case-sensitivity, currency, read-only/writable flags, searchability, collation,
  base table/schema/catalog, domain/type UUID.

## Required Metadata Authority

Metadata APIs must source from grant-visible `sys.information` projections, not
raw `sys.catalog` tables. Catalog UUID identity remains authoritative; names are
resolved/rendered through the identity resolver and security filters.

## Required Tests

- Round-trip every D9 type through bind, engine, fetch, and equality rules.
- Null/empty/NaN/Inf/-0.0/subnormal/supplementary Unicode tests.
- Metadata shape tests for every D13 row.
- Charset/collation/timezone tests for D9.44, D9.45, and D22.
- Negative conversion tests for silent narrowing and unsupported casts.

## DSR-014 Closure Slice

Search key: `DRIVER-SERVER-RECONCILIATION-DSR-014-CLOSURE`.

DSR-014 adds a manifest-listed native-wire byte-layout authority:

- `public_contract_snapshot`

That appendix closes the byte layout for:

- `CanonicalTypeId` and `CanonicalTypeRef`;
- `ParameterDescription`;
- `ParameterDataPacket`;
- `RowDescription`;
- RowDescription metadata bitmap;
- null-vs-empty value state;
- generated-key result discriminator;
- OUT/INOUT/RETURN result path.

## Checklist Binding

| Checklist rows | Closure authority |
| --- | --- |
| D8.6-D8.15 | `RowDescription` metadata bitmap, result-set discriminator, LOB/locator bits, cursor rowset discriminator, and explicit empty-rowset/no-result separation. |
| D9.1-D9.50 | `CanonicalTypeId` family/code table plus `CanonicalTypeRef` descriptor UUID, modifiers, charset/collation/timezone/calendar/numeric-locale fields, and null-vs-empty value state. |
| D13.1-D13.33 | Grant-visible `sys.information.*` and `sys.information.scratchbird_*` projections; driver metadata must not read raw `sys.catalog` rows as external authority. |
| D22.1-D22.4 | Charset, collation, connection encoding, decimal rendering, timezone, and calendar policy fields in descriptor metadata. |

## Cross-Spec Authority Links

| Area | Authority |
| --- | --- |
| Driver type metadata semantics | `public_contract_snapshot` search key `TMD-DSR-014-DATATYPE-RESULT-METADATA-CLOSURE`. |
| Native byte layouts | `public_contract_snapshot` search key `NATIVE-WIRE-TYPE-PARAMETER-RESULT-METADATA-LAYOUT`. |
| Message family boundary | `public_contract_snapshot`. |
| sys.information projections | `public_contract_snapshot` search key `SB_SPEC_INFORMATION_SCHEMA_DSR014_DRIVER_METADATA_PROJECTIONS`. |
| 128-bit numeric metadata | `public_contract_snapshot`. |
| Cast/comparison metadata | `public_contract_snapshot` and `public_contract_snapshot`. |
| Index/write-profile metadata inputs | Manifest-listed index and optimizer appendices; this DSR-014 slice references them but does not edit them because they are outside the datatype_metadata_agent write scope. |

## Driver-Facing Authority Rules

1. UUID identity remains authoritative inside catalog and execution descriptors.
2. Ordinary driver metadata receives grant-visible names/codes, not raw UUIDs.
3. `CanonicalTypeId` is a compact wire code only; it never replaces
   `descriptor_uuid` or `domain_uuid` authority.
4. `int128`, `uint128`, and `real128` metadata must report exact support,
   precision, signedness, backend profile requirement, and unsupported reason
   when unavailable.
5. Casts, comparisons, index eligibility, and silent-narrowing refusal are
   descriptor/registry metadata, not reference or driver convention.
6. Generated keys are visible only through the explicit
   `result_set_kind=generated_keys` discriminator.
7. OUT/INOUT/RETURN values are returned through explicit result-column classes
   mapped back to parameter ordinals.
8. Insert/update write profiles may optimize write execution, but they must not
   change generated-key metadata, OUT/INOUT metadata, null/default semantics, or
   MGA publication requirements.

## Remaining Implementation Evidence Needed

DSR-014 is contract-closed by this slice, but implementation evidence
remains pending for DSR-023:

- C++ encoder/decoder structures matching the new native-wire layout.
- Driver metadata tests for ODBC/JDBC/.NET/native result metadata.
- D9 round-trip corpus including null/empty, NaN/Inf/-0.0/subnormal, Unicode
  supplementary plane, and 128-bit numeric cases.
- D13 metadata visibility tests proving grant-visible projection behavior.
- D22 charset/collation/timezone/calendar/decimal rendering tests.
