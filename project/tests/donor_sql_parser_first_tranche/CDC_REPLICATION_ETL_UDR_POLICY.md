# Donor CDC, Replication, and ETL UDR Policy

Search key: `DONOR_CDC_REPLICATION_ETL_UDR_POLICY`

This tracked test policy records the public-release boundary for donor-native
CDC, replication, remote synchronization, streaming import/export, and ETL
surfaces.

When a donor exposes a logical CDC, replication, ETL, or remote synchronization
method, the donor parser must recognize the request and route it to that
donor's parser-support UDR. The parser-support UDR is the admission and lowering
boundary. It validates donor-specific options, applies ScratchBird policy,
translates admitted logical records or change events to ScratchBird SBLR/engine
calls, and renders donor-compatible result or diagnostic records.

The donor parser must not directly claim transaction finality, recovery
authority, storage authority, cluster authority, or donor-engine execution
authority. MGA transaction and recovery authority remain engine-owned. Index,
catalog, security, cleanup, and durability finality remain ScratchBird engine
authority.

Server-local file access remains deny-by-default. Client/remote logical streams
may be admitted where the original donor engine exposes that method, but physical
page-copy backups, storage snapshots, raw filespaces, donor storage files,
repository storage files, backup-agent artifacts, and low-level repair/verify
utilities remain denied from donor-parser authority.

Cluster-specific commands are separate from donor CDC/replication/ETL. Donor
cluster operations must be normalized through the cluster command surface and
compile-time cluster stub boundary, not through ad hoc donor replication or
metrics SBLR. If cluster support is not compiled in, those routes fail closed as
unsupported. If cluster support is compiled in for public builds, the call is
routed to the open-source cluster provider stub and returns unlicensed.

The matrix for each donor parser is:

`project/tests/donor_sql_parser_first_tranche/cdc_replication_etl_udr_policy_matrix.csv`

The route is not complete unless:

* the donor statement/API surface has a concrete parser pattern;
* the pattern uses `MappingDisposition::kParserSupportUdr`;
* the message vector uses the donor UDR diagnostic such as
  `<DONOR>.EMULATION.CDC_ROUTE`, `<DONOR>.EMULATION.REPLICATION_ROUTE`,
  `<DONOR>.EMULATION.ETL_ROUTE`, or a donor-specific equivalent;
* direct `cluster.replication.consume_cluster_event`,
  `sblr.replication.consumer`, `cluster.metrics.emit_event`, and equivalent
  direct evidence routes are absent from donor parser sources;
* full CTest regenerates proof evidence under `project/tests`.
