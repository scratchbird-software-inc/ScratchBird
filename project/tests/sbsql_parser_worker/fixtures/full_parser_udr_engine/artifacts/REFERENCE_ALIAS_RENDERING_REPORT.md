# Reference Alias Rendering Report

Status: complete
Search key: `FSPE-REFERENCE_ALIAS_RENDERING_REPORT`

Owning slice: `FSPE-011B`

## Scope

This report records generated reference alias mapping and reference-facing rendering
coverage. The gate verifies every reference alias matrix row has a native SBSQL
mapping or exact refusal policy, a stable reference fixture ID, reference-facing message
vector rendering metadata, and result-shape policy. It does not transfer SQL or
reference command text into engine authority; engine behavior remains SBLR/internal
API only.

## Coverage Counts

| Coverage input | Covered rows |
| --- | ---: |
| `REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv` | 312 |
| `REFERENCE_ALIAS_COVERAGE_BACKLOG.csv` | 312 |
| Reference profiles | 24 |
| Alias kinds | 13 |
| Reference alias rendering fixture policies | 13 |

## Reference Profiles

The matrix covers these reference profiles with 13 alias kinds each:

```text
apache_ignite
cassandra
clickhouse
cockroachdb
dolt
duckdb
firebird
foundationdb
immudb
influxdb
mariadb
milvus
mongodb
mysql
neo4j
opensearch
postgresql
redis
sqlite
tidb
tikv
vitess
xtdb
yugabytedb
```

## Rendering Policy

The reusable fixture policy is:

```text
project/tests/sbsql_parser_worker/generated/reference_alias/REFERENCE_ALIAS_RENDERING_FIXTURES.csv
```

Every alias kind defines result metadata fields, command-tag policy,
affected-row policy, warning/error rendering, and catalog/introspection shape.
Every diagnostic path must render through reference-profile message vectors with the
active profile UUID and without leaking UUID, descriptor, security, transaction,
MGA, storage, metric, or SBLR execution authority to the reference parser layer.

## Validation

The FSPE-011B gate is:

```bash
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_reference_alias_rendering_conformance --output-on-failure
```
