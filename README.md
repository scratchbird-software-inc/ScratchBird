# ScratchBird Convergent Data Engine

## What is this?

ScratchBird Convergent Data Engine (SBcde, or SB) is an experimental database-engine project released for source review, testing, research, and community contribution.

The project explores a convergent database-engine architecture: a single engine core with separate parser and protocol-facing components, internal UUID-based object identity, an SBLR execution boundary, and a multigenerational storage and transaction model.

This repository is a public beta source release. It is intended to let interested developers, database engineers, driver authors, testers, translators, and reviewers examine the code, build it, run the available tests, and provide feedback.

## Release status

This is a early beta release. This is the first public release of what was a totally private project.  There was a earlier project, but it was a proof of concept project and did not show more than the basic ideas whereas this is the opensource portion made public while the closed source cluster implementation is not made available.

It is not presented as a production-ready system, a commercially supported product, or a drop-in replacement for any existing database engine. No claim is made about fitness for a particular use, operational suitability, performance, compatibility completeness, security certification, or support availability.

The presence of source files, directories, tests, benchmark harnesses, configuration options, generated artifacts, parser profiles, or compatibility profiles should not be interpreted as a claim that any particular feature is complete, supported, suitable for production use, or available on every platform.

The authoritative status of each area of the project is determined by the corresponding source, tests, proof artifacts, release notes, and issue tracking for that area.

## Build output

The CMake build stages public standalone artifacts under `build/output/<platform>`, where `<platform>` is `linux`, `windows`, or `bsd`.  These are locally generated artifacts that will appear after you successfully compile the full project.

The staged tree contains the branded runtime and CLI binaries, libraries, configuration templates, resource seed packs, policy packs, public API documents, release documents, and public examples needed for local testing or release packaging.

Public binary names are:

- `SBsql` - ScratchBird SQL
- `SBadm` - ScratchBird Administrator
- `SBbak` - ScratchBird Backup Manager
- `SBsec` - ScratchBird Security
- `SBdoc` - ScratchBird Doctor
- `SBcop` - ScratchBird Conformance Officer
- `SBcore` - ScratchBird Engine shared library
- `SBgate` - ScratchBird Listener
- `SBsrv` - ScratchBird IPC Server
- `SBmgr` - ScratchBird Single Node Manager
- `SBParser` - ScratchBird Core Parser
- `SB_FBSQL_Parser` - ScratchBird Firebird Parser

`SBcmgr` is reserved for the ScratchBird Cluster Manager in cluster-enabled builds. The public standalone build does not emit `SBcmgr` unless a public cluster-manager target is present.

## Why this repository exists

This repository is being published to make the current source tree available for independent review and testing.

The immediate goals are:

1. allow developers to build and inspect the project;
2. make test and benchmark material available for reproduction and review;
3. invite help with driver implementations;
4. invite help with platform testing, especially platforms the core team cannot fully validate directly;
5. invite help reviewing multilingual and cross-language behavior;
6. collect technical feedback through issues, patches, test results, and reproducible reports.

This is not a marketing release. It is a source and testing release.

## Reference systems

During design and compatibility research, many existing database systems were studied as reference systems for behavior, architecture, feature coverage, performance characteristics, wire-protocol expectations, client compatibility, testing practices, and operational trade-offs.

Reference systems studied include Apache Ignite, Cassandra, ClickHouse, CockroachDB, Dolt, DuckDB, Firebird, FoundationDB, immudb, InfluxDB, MariaDB, Milvus, MongoDB, MySQL, Neo4j, OpenSearch, PostgreSQL, Redis, SQLite, TiDB, TiKV, Vitess, XTDB, and YugabyteDB.

These systems were used for research, comparison, requirements validation, and compatibility testing only. ScratchBird implementation source is not derived from donor engine source. Separately identified third-party and resource files retain their own upstream notices and licenses.

Where this repository uses external tools, client binaries, server binaries, command-line utilities, drivers, test fixtures, or regression test suites from reference systems, those artifacts remain separate upstream materials governed by their own licenses. They are used only as external test targets or compatibility references.

## Compatibility profiles

Compatibility profile files describe intended parser, protocol, catalog, datatype, diagnostic, migration, test, or emulation boundaries.

A compatibility profile is not, by itself, a feature-completeness claim.

A parser profile, manifest row, test harness, benchmark entry, or compatibility directory means that the area is tracked by the project. Its actual release status depends on the current implementation, platform test results, proof artifacts, and documented limitations for that area.

## Benchmarks

Benchmark material in this repository is provided for reproducibility and review.

Benchmark results depend on hardware, operating system, compiler, configuration, storage, workload shape, and test setup. The project does not make performance claims against other systems.

Users and reviewers are encouraged to run the benchmark material themselves and report reproducible results, configuration details, failures, or suggested improvements.

## Documentation

Documentation in this repository may include generated material, implementation notes, manifests, test descriptions, and work-in-progress developer documentation.

Generated documentation should be treated as technical reference material, not as a polished user manual. Where documentation and source behavior disagree, source code, tests, and current release notes should be treated as the basis for review.

## Security

Do not report security-sensitive issues in public issues if the repository provides a private security reporting process.

Security reports should include enough detail to reproduce the issue without including private credentials, production data, private keys, tokens, or sensitive logs.

## License

Use of this repository is governed by the license file included with the release.

If license terms, contribution terms, or third-party notices are unclear, do not assume additional rights beyond those explicitly granted in the repository.
