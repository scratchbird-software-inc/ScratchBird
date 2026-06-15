# Type Mapping

## Purpose

This page describes the `sbsql_core` type-mapping profile — the profile used by the majority
of ScratchBird drivers. It provides a shared table of canonical SBsql types and their common
client-side representations, verified against the Python driver type implementation.

The `sbsql_core` profile governs how values move between the engine's canonical SBsql type
system and the host language. The profile does not invent types; it maps the types defined
in the SBsql type system to the nearest idiomatic representation in each language.

Sources used: `project/drivers/driver/python/S5_TYPE_IMPLEMENTATION.md`,
`project/drivers/DriverPackageManifest.csv`,
`project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` (OID constants).

For the canonical SBsql type definitions, see
[Language Reference: Data Types](../Language_Reference/data_types/index.md).

This is a **draft**. Components are in `beta_2` / `release_candidate` status.

---

## Type Mapping Profiles by Driver Family

| Profile | Used by |
| --- | --- |
| `sbsql_core` | cpp, dart, dotnet, elixir, go, jdbc, julia, mojo, node, odbc, pascal, perl, php, python, r2dbc, ruby, rust, swift; also scratchbird-prisma-adapter and scratchbird-typeorm-adapter |
| `arrow_recordbatch` | adbc, flightsql |
| `python_dbapi_mapping` | scratchbird-airbyte, scratchbird-dbt-adapter, scratchbird-sqlalchemy-dialect, scratchbird-superset-driver |
| `jdbc_mapping` | scratchbird-dbeaver-driver, scratchbird-hibernate-dialect, scratchbird-looker, scratchbird-metabase-driver |
| `powerquery_mapping` | scratchbird-powerbi |
| `tableau_mapping` | scratchbird-tableau |

Source: `DriverPackageManifest.csv` column `type_mapping_profile`.

The per-driver pages (under `drivers/` and `adaptors/`) document profile-specific detail.
This page covers only the `sbsql_core` profile.

---

## sbsql_core: Canonical Type to Client Representation

The following table maps SBsql canonical type families to their common client representations
in the `sbsql_core` profile, as verified from the Python driver implementation. Languages
with stronger or weaker native types will vary; see per-driver pages for precision.

| SBsql Type Family | Canonical Name(s) | Common Client Representation | Notes |
| --- | --- | --- | --- |
| Boolean | `boolean`, `bool` | Native boolean | Text tokens `true`/`t` map to true; others map to false |
| Signed integer 32-bit | `int32`, `int`, `integer` | Native 32-bit integer | OID 23 |
| Signed integer 64-bit | `int64`, `bigint` | Native 64-bit integer | OID 20; unknown-text integer inference capped at signed int64 range |
| Signed integer 8/16-bit | `int8`, `int16`, `smallint` | Native integer of appropriate width | OID 21 (int2); unknown binary 1-byte decoded as signed int8 |
| Decimal / numeric | `decimal(p,s)`, `numeric(p,s)` | Language decimal or high-precision type | OID 1700; Python: `decimal.Decimal` |
| Approximate real | `real`, `float`, `double precision` | Native float | OID 701 (float8) |
| Text | `varchar(n)`, `text`, `char(n)`, `clob` | Native string | OID 25 (text) |
| Binary | `bytea`, `blob`, `binary(n)`, `varbinary(n)` | Native bytes / byte array | Binary payloads decoded; hex (`\x` prefix or `0x`) and octal escape formats decoded |
| UUID | `uuid` | Language UUID type or string | 16-byte UUID; Python: `uuid.UUID` |
| Date | `date` | Native date | Python: `datetime.date` |
| Time (without timezone) | `time(p)` | Native time (no offset) | Rejects timezone-offset payloads |
| Time with timezone | `timetz` | Offset-aware time | Requires explicit timezone offset; 12-byte binary payload (microseconds + zone seconds west) |
| Timestamp (without timezone) | `timestamp(p)` | Naive datetime | Rejects timezone-offset payloads; materializes naive datetime |
| Timestamp with timezone | `timestamptz` | UTC-aware datetime | Materializes UTC-aware datetime |
| Interval | `interval` | Language interval or duration type | Representation varies by language |
| XML | `xml` | String or XML document type | Python: `SqlXml` wrapper routes to OID_XML |
| Array | `array<T>` | Native collection / array of T | Typed array OID inference: `bool[]`, `int4[]`, `float8[]`, `text[]`, `date[]`, `timetz[]`, `timestamp[]`, `timestamptz[]`, `numeric[]`, `uuid[]`, `bytea[]`, etc. Mixed int+float widens to float8[] |
| JSON / document | `json_document`, `document` | String or native map/document | Representation varies |
| Vector | `vector` | Float array | Float-only sequences encode as explicit vector OID |
| Range | `daterange`, `tsrange`, `tstzrange` | Language range type or pair | String temporal bounds coerced to UTC-backed binary |
| LOB wrappers | — | `Blob`, `Clob`, `RowId`, `Ref`, `SqlXml` | Python: explicit wrapper types; `Blob`/`RowId` route to `OID_BYTEA`; `Clob`/`Ref` route to `OID_TEXT` |

Source: `project/drivers/driver/python/S5_TYPE_IMPLEMENTATION.md` — type codec parity entries
covering TIMETZ, BYTEA, typed arrays, wrapper families, temporal ranges, and unknown-type inference.

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp` — OID constants:
```cpp
constexpr std::uint32_t kOidBool    = 16;
constexpr std::uint32_t kOidInt8    = 20;
constexpr std::uint32_t kOidInt4    = 23;
constexpr std::uint32_t kOidText    = 25;
constexpr std::uint32_t kOidFloat8  = 701;
constexpr std::uint32_t kOidNumeric = 1700;
```

---

## Temporal Family Rules

The `sbsql_core` profile enforces JDBC-aligned timezone family rules for temporal types:

| Type | Rule |
| --- | --- |
| `time` | Rejects payloads that carry a timezone offset. Must be tz-naive. |
| `timestamp` | Rejects payloads that carry a timezone offset. Materializes as naive datetime. |
| `timetz` | Requires an explicit timezone offset. |
| `timestamptz` | Materializes as UTC-aware datetime. |

Trailing `Z` in text payloads is normalized to UTC offset form.

Source: `project/drivers/driver/python/S5_TYPE_IMPLEMENTATION.md` — "Temporal typed-text parsing
now enforces JDBC family semantics".

---

## Unknown Type Fallback

When the driver receives a value with an unrecognized OID:

- **Binary unknown (1-byte):** decoded as a signed int8.
- **Text unknown (integer-shaped):** auto-coercion is limited to signed int64 range; values
  outside that range remain text.
- **Other objects (parameter encoding):** Python `Enum` values encode using enum member names;
  unsupported objects fall back to `str(value)` with `OID_TEXT`.

Source: `project/drivers/driver/python/S5_TYPE_IMPLEMENTATION.md` — "unknown-binary scalar
parity" and "unknown-text integer bounds parity".

---

## COPY Format

The wire protocol defines a canonical row-fields text format for COPY operations:

```cpp
constexpr std::uint8_t kCopyFormatCanonicalRowFieldsText = 0x00;
constexpr std::uint32_t kCopyDefaultWindowBytes = 64u * 1024u;
```

Source: `project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp`.

---

## Cross-References

- [Language Reference: Data Types](../Language_Reference/data_types/index.md) — canonical SBsql type names and definitions
- [Language Reference: Data Types: Type System Overview](../Language_Reference/data_types/type_system_overview.md) — descriptor binding model
- [Language Reference: Data Types: Temporal Types](../Language_Reference/data_types/temporal_types.md) — temporal type detail
- [Language Reference: Data Types: Numeric Types](../Language_Reference/data_types/numeric_types.md) — numeric type detail
- [wire_protocol_sbwp.md](wire_protocol_sbwp.md) — OID values in SBWP frames
