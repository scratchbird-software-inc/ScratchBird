# Reference catalog seed reference tree

Search key: `SB_REFERENCE_REFERENCE_CATALOG_SEEDS_ROOT`

This private reference tree stores reference catalog seed manifests required by `SB_SPEC_REFERENCE_CATALOG_SEED_EMULATION_CONTRACT`.

Rules:

1. Each true reference family has a subdirectory.
2. Each supported reference version/profile must have a seed manifest before emulation setup is implementation-ready.
3. Release/profile variants may have profile-derived seed manifests, but they do not create reference families or runtime per-family index entries.
4. Raw secrets are forbidden.
5. Runtime-generated values must be represented by deterministic emulation rules.
6. Seed manifests must not depend on material outside the private `docs` tree.

## Actual per-family beta2 seed manifests

Search key: `SB_REFERENCE_REFERENCE_CATALOG_SEED_ACTUAL_MANIFEST_INDEX`

The concrete private seed manifests are indexed by `actual_per_family_seed_manifest_index.yaml`. SQL Server, Oracle, and DB2 remain capability-reference only and are not runtime reference seed manifests.

- `project/tests/reference_regression/reference_catalog_seeds/firebird/firebird_5_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_FIREBIRD_BETA2_FULL` - manifest hash `f55720ee0141bf879a73e6aff3549239457478e1908919133bb901a29a4829d3`
- `project/tests/reference_regression/reference_catalog_seeds/postgresql/postgresql_18_1_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_POSTGRESQL_BETA2_FULL` - manifest hash `f980cb764dd2052ead9d29fad53dc876f087b8e548d362c17e304000a3bfc0c0`
- `project/tests/reference_regression/reference_catalog_seeds/mysql/mysql_8_4_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_MYSQL_BETA2_FULL` - manifest hash `514f8c6d40570038b561dec29f5ae42315f5c898b28d8cba85621efeab24e47a`
- `project/tests/reference_regression/reference_catalog_seeds/mariadb/mariadb_11_4_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_MARIADB_BETA2_FULL` - manifest hash `1271a1a4292d1fb31602e3587f06a883aa248b82eec7bc5f5b848db5aaeac13e`
- `project/tests/reference_regression/reference_catalog_seeds/sqlite/sqlite_3_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_SQLITE_BETA2_FULL` - manifest hash `197d11f1143ade5984e4e532cd640fa4a293107bf9d51a3ebbfea26dbcc6c138`
- `project/tests/reference_regression/reference_catalog_seeds/duckdb/duckdb_1_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_DUCKDB_BETA2_FULL` - manifest hash `f1e9c5f8ea8082a59da1f7e5bdd95ff2632ac32463b494ff1dc3e3a64632697a`
- `project/tests/reference_regression/reference_catalog_seeds/clickhouse/clickhouse_25_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_CLICKHOUSE_BETA2_FULL` - manifest hash `a8c6d03e75ff912a04e24756c9fc0a2088ea7e538c9e37b78a8b85552f3c8d7d`
- `project/tests/reference_regression/reference_catalog_seeds/cassandra/cassandra_5_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_CASSANDRA_BETA2_FULL` - manifest hash `01c5b082ec08e5c19ebfa189d9036c41d5c5b2b96bcef753e54beb01c00d7687`
- `project/tests/reference_regression/reference_catalog_seeds/mongodb/mongodb_8_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_MONGODB_BETA2_FULL` - manifest hash `ea31950d98c5d5ce5fc49dc720e0632ab9505cd92835348f2755b94dc781d3ac`
- `project/tests/reference_regression/reference_catalog_seeds/redis/redis_8_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_REDIS_BETA2_FULL` - manifest hash `16b1f5b571adc3824ae29fd8e1b76bac486ad24044bb1c6f3506c2db8a4c39df`
- `project/tests/reference_regression/reference_catalog_seeds/neo4j/neo4j_2025_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_NEO4J_BETA2_FULL` - manifest hash `cf5d8667a7f1fe9b7599d9cac0b8eca50394c2cf435c58dba59b9db99607d388`
- `project/tests/reference_regression/reference_catalog_seeds/opensearch/opensearch_2_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_OPENSEARCH_BETA2_FULL` - manifest hash `a50fb0746e42b8c27ed6c7a3dc8c465d320e1e6402efc4020d1e1089ef7e093b`
- `project/tests/reference_regression/reference_catalog_seeds/influxdb/influxdb_3_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_INFLUXDB_BETA2_FULL` - manifest hash `52bd59be993b003e2d42338320adda45540d2b8e1029a48fc9994280fc524e9a`
- `project/tests/reference_regression/reference_catalog_seeds/milvus/milvus_2_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_MILVUS_BETA2_FULL` - manifest hash `adc37b5b53dfb3f6e5508fbc3862cc7ba17e8f971fe83578e5b57b034bff9553`
- `project/tests/reference_regression/reference_catalog_seeds/cockroachdb/cockroachdb_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_COCKROACHDB_BETA2_FULL` - manifest hash `51f7eddcaf9dad90b47f4a9a8b5760af46225a05d3ef720375f250f11b590fc8`
- `project/tests/reference_regression/reference_catalog_seeds/tidb/tidb_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_TIDB_BETA2_FULL` - manifest hash `c99e31d3a2688f56bdd4688b8f9816c5a5225d7882fa0bcfd273d825d4916e4f`
- `project/tests/reference_regression/reference_catalog_seeds/tikv/tikv_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_TIKV_BETA2_FULL` - manifest hash `0864431322deb0e0e9bfd381dd22db6f92a417e58b4190d623848bb8559b1852`
- `project/tests/reference_regression/reference_catalog_seeds/yugabytedb/yugabytedb_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_YUGABYTEDB_BETA2_FULL` - manifest hash `7c0a708595596f6e42622ecd3d670ad885875060cbca477bcd2ca5a911d073de`
- `project/tests/reference_regression/reference_catalog_seeds/vitess/vitess_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_VITESS_BETA2_FULL` - manifest hash `3c2020ceed4ef9d81d82aeb09edabc6b3c4f4ecc1257396d84169b0612ebb44c`
- `project/tests/reference_regression/reference_catalog_seeds/foundationdb/foundationdb_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_FOUNDATIONDB_BETA2_FULL` - manifest hash `181fbb890860222d137fb43be9b635e424f634a844e63d5e52205322e7c22cb5`
- `project/tests/reference_regression/reference_catalog_seeds/apache_ignite/apache_ignite_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_APACHE_IGNITE_BETA2_FULL` - manifest hash `371c66cf057ae776cc9ef8f4c5e13e8aa1e57b81e3fae8c1b5e5f666c3983e4d`
- `project/tests/reference_regression/reference_catalog_seeds/dolt/dolt_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_DOLT_BETA2_FULL` - manifest hash `01549597ebcdd78e3de7f201640e5d0d260d54a20dbd53cfe1a170b6b486f951`
- `project/tests/reference_regression/reference_catalog_seeds/immudb/immudb_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_IMMUDB_BETA2_FULL` - manifest hash `53e0ab7bd2fa0787890b69a0a625546e701a1be20df5f06ab7af33626495ff28`
- `project/tests/reference_regression/reference_catalog_seeds/xtdb/xtdb_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_XTDB_BETA2_FULL` - manifest hash `5fca1ad9a1e7290f508e913cd916a85cbf9aa1b60d7c1651f3b114ced4fc3128`

## Profile-derived release seed manifests

These packets preserve release-profile evidence without adding runtime reference families to `actual_per_family_seed_manifest_index.yaml`.

- `project/tests/reference_regression/reference_catalog_seeds/mysql_lts/mysql_lts_beta2_full_seed_manifest.yaml` - `SB_REFERENCE_REFERENCE_CATALOG_SEED_MYSQL_LTS_BETA2_FULL` - MySQL release/profile variant `mysql_lts` - manifest hash `8c7a02f1c38e3c5692e81f2188be13a750ee230a49950e6d40d4b8b61de95c7b`
