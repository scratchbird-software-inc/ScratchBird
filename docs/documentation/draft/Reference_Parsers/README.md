# Reference Parsers (Compatibility Parsers)

## Purpose

This guide explains, in general terms, the **why** and **how** of ScratchBird's reference parsers — the compatibility surfaces that let clients built for other databases connect to ScratchBird using their own dialect and wire protocol. It is deliberately high-level: it describes the model, the behavior emulation, and the boundaries, not any source system's internals or a parser's statement-by-statement coverage.

Each reference parser is responsible for **one source dialect and one wire protocol**, exists for **compatibility only** (it adds no functionality and blends no other dialect), and is **untrusted** by the engine — accepted work is lowered to the engine's internal SBLR representation and rechecked there. The engine keeps all identity, transaction, storage, and security authority.

This is a **draft**; availability of any specific behavior still depends on the build, configuration, policy, and release notes.

## Read first

- [How Reference Parsers Work](how_reference_parsers_work.md) — the model and the request flow.
- [How Compatibility Behaviors Are Emulated](behavior_emulation.md) — index, storage, and transaction/autocommit emulation over the engine.
- [Compatibility Scope And Boundaries](compatibility_scope_and_boundaries.md) — what compatibility covers and what is deliberately blocked.
- [Conformance And Compatibility Targets](conformance_and_status.md) — what each parser is built to match.

## Reference parsers

Each parser presents one system's dialect, wire protocol, and behavior as compatibility, over a single ScratchBird engine.

| Parser | Category |
| --- | --- |
| [DuckDB](parsers/duckdb.md) | Relational |
| [Firebird](parsers/firebird.md) | Relational |
| [MariaDB](parsers/mariadb.md) | Relational |
| [MySQL](parsers/mysql.md) | Relational |
| [PostgreSQL](parsers/postgresql.md) | Relational |
| [SQLite](parsers/sqlite.md) | Relational |
| [ClickHouse](parsers/clickhouse.md) | Analytical |
| [InfluxDB](parsers/influxdb.md) | Analytical |
| [Milvus](parsers/milvus.md) | Analytical |
| [OpenSearch](parsers/opensearch.md) | Analytical |
| [OpenSearch SQL/PPL](parsers/opensearch_sql_ppl.md) | Analytical |
| [Cassandra](parsers/cassandra.md) | NoSQL |
| [MongoDB](parsers/mongodb.md) | NoSQL |
| [Neo4j](parsers/neo4j.md) | NoSQL |
| [Redis](parsers/redis.md) | NoSQL |
| [XTDB](parsers/xtdb.md) | NoSQL |
| [Apache Ignite](parsers/apache_ignite.md) | Distributed |
| [CockroachDB](parsers/cockroachdb.md) | Distributed |
| [Dolt](parsers/dolt.md) | Distributed |
| [FoundationDB](parsers/foundationdb.md) | Distributed |
| [immudb](parsers/immudb.md) | Distributed |
| [TiDB](parsers/tidb.md) | Distributed |
| [TiKV](parsers/tikv.md) | Distributed |
| [Vitess](parsers/vitess.md) | Distributed |
| [YugabyteDB](parsers/yugabytedb.md) | Distributed |

## Related manuals

- [CDE Concepts: Dialect Plurality And Parser Separation](../CDE_Concepts/dialect_plurality_and_parser_separation.md)
- [Client And Driver Guide](../Client_Driver_Guide/README.md) — connection, wire, and authentication specifics.
- [Security Guide: Trust And Separation Architecture](../Security_Guide/trust_and_separation_architecture.md) — why parsers are untrusted.
- [Operations And Administration: Parser Registration And Routes](../Operations_Administration/parser_registration_and_routes.md)

