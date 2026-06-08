# Donor Alias Rendering Report

Status: complete
Search key: `FSPE-DONOR_ALIAS_RENDERING_REPORT`

Owning slice: `FSPE-011B`

## Scope

This report records generated donor alias mapping and donor-facing rendering
coverage. The gate verifies every donor alias matrix row has a native SBSQL
mapping or exact refusal policy, a stable donor fixture ID, donor-facing message
vector rendering metadata, and result-shape policy. It does not transfer SQL or
donor command text into engine authority; engine behavior remains SBLR/internal
API only.

## Coverage Counts

| Coverage input | Covered rows |
| --- | ---: |
| `DONOR_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv` | 312 |
| `DONOR_ALIAS_COVERAGE_BACKLOG.csv` | 312 |
| Donor profiles | 24 |
| Alias kinds | 13 |
| Donor alias rendering fixture policies | 13 |

## Donor Profiles

The matrix covers these donor profiles with 13 alias kinds each:

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
project/tests/sbsql_parser_worker/generated/donor_alias/DONOR_ALIAS_RENDERING_FIXTURES.csv
```

Every alias kind defines result metadata fields, command-tag policy,
affected-row policy, warning/error rendering, and catalog/introspection shape.
Every diagnostic path must render through donor-profile message vectors with the
active profile UUID and without leaking UUID, descriptor, security, transaction,
MGA, storage, metric, or SBLR execution authority to the donor parser layer.

## Validation

The FSPE-011B gate is:

```bash
ctest --test-dir build/sbsql_parser_worker_validation -L sbsql_donor_alias_rendering_conformance --output-on-failure
```
