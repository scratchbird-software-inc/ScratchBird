#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Non-SQL donor SBLR surface proof gate.

This gate keeps the Cassandra, MongoDB, Redis, OpenSearch SQL/PPL,
OpenSearch REST, Neo4j, InfluxDB, Milvus, Apache Ignite, TiKV,
FoundationDB, and XTDB beta parsers honest about the current cluster boundary:

* every non-cluster admitted route must emit a donor SBLR operation;
* parser-support UDR routes must emit a donor SBLR operation;
* exact cluster refusals must fail closed and must not emit SBLR authority;
* normalized cluster routes must emit cluster SBLR provider-boundary metadata;
* every non-cluster donor route must have an explicit SBLR or parser-support route.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path


DIALECTS = {
    "cassandra": {
        "source": "project/src/parsers/donor/cassandra/cassandra_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "CASSANDRA.AUTHORITY.UNSUPPORTED_DENIED",
        },
    },
    "mongodb": {
        "source": "project/src/parsers/donor/mongodb/mongodb_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "MONGODB.AUTHORITY.SHARDING_ADMIN_DENIED",
            "MONGODB.AUTHORITY.REPLICA_ADMIN_DENIED",
        },
    },
    "redis": {
        "source": "project/src/parsers/donor/redis/redis_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "REDIS.AUTHORITY.CLUSTER_DENIED",
            "REDIS.AUTHORITY.REPLICATION_DENIED",
        },
    },
    "opensearch_sql_ppl": {
        "source": "project/src/parsers/donor/opensearch_sql_ppl/opensearch_sql_ppl_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "OPENSEARCH_SQL_PPL.AUTHORITY.CLUSTER_CONTROL_RESERVED",
        },
    },
    "opensearch": {
        "source": "project/src/parsers/donor/opensearch/opensearch_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "OPENSEARCH.AUTHORITY.CLUSTER_CONTROL_RESERVED",
        },
    },
    "neo4j": {
        "source": "project/src/parsers/donor/neo4j/neo4j_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "NEO4J.AUTHORITY.CLUSTER_CONTROL_RESERVED",
        },
    },
    "influxdb": {
        "source": "project/src/parsers/donor/influxdb/influxdb_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "INFLUXDB.AUTHORITY.CLUSTER_CONTROL_RESERVED",
        },
    },
    "milvus": {
        "source": "project/src/parsers/donor/milvus/milvus_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "MILVUS.AUTHORITY.CLUSTER_CONTROL_RESERVED",
        },
    },
    "apache_ignite": {
        "source": "project/src/parsers/donor/apache_ignite/apache_ignite_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "APACHE_IGNITE.AUTHORITY.CLUSTER_CONTROL_RESERVED",
        },
    },
    "tikv": {
        "source": "project/src/parsers/donor/tikv/tikv_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "TIKV.AUTHORITY.CLUSTER_CONTROL_RESERVED",
        },
    },
    "foundationdb": {
        "source": "project/src/parsers/donor/foundationdb/foundationdb_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "FOUNDATIONDB.AUTHORITY.CLUSTER_CONTROL_RESERVED",
        },
    },
    "xtdb": {
        "source": "project/src/parsers/donor/xtdb/xtdb_dialect.cpp",
        "cluster_reserved_diagnostics": {
            "XTDB.AUTHORITY.CLUSTER_CONTROL_RESERVED",
        },
        "external_reserved_diagnostics": {
            "XTDB.AUTHORITY.MODULE_CONFIGURATION_DENIED",
        },
    },
}

PATTERN = re.compile(
    r'\{"(?P<match>[^"]+)",\s*PatternMatch::k(?P<match_kind>\w+),\s*'
    r'"(?P<statement_family>[^"]*)",\s*"(?P<operation_family>[^"]*)",\s*'
    r'MappingDisposition::k(?P<disposition>\w+),\s*"(?P<mapping_key>[^"]*)",\s*'
    r'"(?P<sblr_operation>[^"]*)",\s*"(?P<engine_api_function>[^"]*)",\s*'
    r'"(?P<diagnostic_code>[^"]*)"',
    re.MULTILINE | re.DOTALL,
)

NON_CLUSTER_DISPOSITIONS = {
    "AdmittedSblr",
    "ScratchBirdLifecycleApi",
    "ParserSupportUdr",
    "CatalogProjection",
}

ENGINE_SBLR_MATRIX = "project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml"
SBSQL_DONOR_BACKFILL_REGISTER = (
    "public_execution_plan"
    "SBSQL_DONOR_ROUTE_BACKFILL_REGISTER.csv"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_patterns(path: Path) -> list[dict[str, str]]:
    text = path.read_text(encoding="utf-8")
    rows = [match.groupdict() for match in PATTERN.finditer(text)]
    require(rows, f"{path}: no OperationPattern rows parsed")
    return rows


def load_engine_sblr_symbols(repo_root: Path) -> set[str]:
    text = (repo_root / ENGINE_SBLR_MATRIX).read_text(encoding="utf-8")
    return set(re.findall(r"sblr_operation:\s*(SBLR_[A-Z0-9_]+)", text))


def load_sbsql_backfill_symbols(repo_root: Path) -> set[str]:
    symbols: set[str] = set()
    path = repo_root / SBSQL_DONOR_BACKFILL_REGISTER
    with path.open(newline="", encoding="utf-8") as handle:
        for row in csv.DictReader(handle):
            for symbol in row["parser_sblr_symbols"].split(";"):
                symbol = symbol.strip()
                if symbol:
                    symbols.add(symbol)
    return symbols


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    engine_sblr_symbols = load_engine_sblr_symbols(repo_root)
    sbsql_backfill_symbols = load_sbsql_backfill_symbols(repo_root)
    evidence: dict[str, object] = {
        "gate": "non_sql_donor_sblr_surface_gate",
        "status": "passed",
        "cluster_specific_work_implemented": False,
        "cluster_boundary": "reserved_for_normalized_cluster_control_sblr",
        "sblr_registration_rule": {
            "native_engine_symbols": ENGINE_SBLR_MATRIX,
            "donor_symbols": SBSQL_DONOR_BACKFILL_REGISTER,
        },
        "dialects": {},
    }

    for dialect, spec in DIALECTS.items():
        source = repo_root / spec["source"]
        rows = parse_patterns(source)
        cluster_diagnostics = set(spec.get("cluster_reserved_diagnostics", set()))
        external_diagnostics = set(spec.get("external_reserved_diagnostics", set()))
        reserved_diagnostics = cluster_diagnostics | external_diagnostics
        sblr_symbols: set[str] = set()
        cluster_reserved = []
        external_reserved = []
        non_cluster_count = 0

        for row in rows:
            diagnostic = row["diagnostic_code"]
            disposition = row["disposition"]
            sblr_operation = row["sblr_operation"]
            operation_family = row["operation_family"]
            if diagnostic in reserved_diagnostics:
                require(
                    disposition in {"PolicyRefusal", "SecurityRefusal", "UnsupportedRefusal"},
                    f"{dialect} {operation_family}: reserved route did not fail closed",
                )
                require(
                    not sblr_operation,
                    f"{dialect} {operation_family}: reserved route emitted {sblr_operation}",
                )
                if diagnostic in cluster_diagnostics:
                    cluster_reserved.append(operation_family)
                else:
                    external_reserved.append(operation_family)
                continue
            if (
                sblr_operation.startswith("sblr.cluster.")
                or sblr_operation.startswith("required_new:sblr.cluster.")
                or sblr_operation.startswith("sblr.replication.")
            ):
                require(
                    disposition == "AdmittedSblr",
                    f"{dialect} {operation_family}: normalized cluster route "
                    f"has disposition {disposition}",
                )
                require(
                    row["mapping_key"].startswith("cluster."),
                    f"{dialect} {operation_family}: normalized cluster route "
                    f"has mapping {row['mapping_key']}",
                )
                require(
                    row["engine_api_function"].startswith("cluster."),
                    f"{dialect} {operation_family}: normalized cluster route "
                    f"has provider boundary {row['engine_api_function']}",
                )
                cluster_reserved.append(operation_family)
                sblr_symbols.add(sblr_operation)
                continue
            require(
                disposition in NON_CLUSTER_DISPOSITIONS,
                f"{dialect} {operation_family}: non-cluster route has refusal disposition {disposition}",
            )
            require(
                bool(sblr_operation),
                f"{dialect} {operation_family}: non-cluster route has no SBLR operation",
            )
            require(
                sblr_operation.startswith("SBLR_"),
                f"{dialect} {operation_family}: malformed SBLR operation {sblr_operation}",
            )
            if sblr_operation.startswith("SBLR_DONOR_"):
                require(
                    sblr_operation in sbsql_backfill_symbols,
                    f"{dialect} {operation_family}: donor SBLR symbol "
                    f"{sblr_operation} missing from SBsql backfill register",
                )
            else:
                require(
                    sblr_operation in engine_sblr_symbols,
                    f"{dialect} {operation_family}: native SBLR symbol "
                    f"{sblr_operation} missing from engine SBLR matrix",
                )
            sblr_symbols.add(sblr_operation)
            non_cluster_count += 1

        require(non_cluster_count > 0, f"{dialect}: no non-cluster SBLR routes proven")
        require(cluster_reserved, f"{dialect}: no cluster boundary proof rows")
        evidence["dialects"][dialect] = {
            "source": spec["source"],
            "operation_pattern_count": len(rows),
            "non_cluster_sblr_route_count": non_cluster_count,
            "sblr_symbols": sorted(sblr_symbols),
            "cluster_reserved_routes": sorted(cluster_reserved),
            "external_reserved_routes": sorted(external_reserved),
        }

    args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
    args.evidence_file.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
