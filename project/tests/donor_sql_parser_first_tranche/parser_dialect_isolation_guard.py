#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FPR-P5 donor parser dialect-isolation proof gate."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import re
import subprocess
import sys


DIALECTS = (
    "firebird",
    "postgresql",
    "mysql",
    "sqlite",
    "mariadb",
    "duckdb",
    "clickhouse",
    "tidb",
    "vitess",
    "cockroachdb",
    "yugabytedb",
    "cassandra",
    "mongodb",
    "redis",
    "opensearch_sql_ppl",
    "opensearch",
    "neo4j",
    "influxdb",
    "milvus",
    "dolt",
    "apache_ignite",
    "tikv",
    "foundationdb",
    "immudb",
    "xtdb",
)

DISTINCTIVE_PROBES = {
    "firebird": "RECREATE TABLE fb_iso_t (id INTEGER)",
    "postgresql": "LISTEN sb_channel",
    "mysql": "XA START 'x'",
    "sqlite": "LOAD_EXTENSION('mod_spatialite')",
    "mariadb": "HANDLER accounts OPEN",
    "duckdb": "READ_PARQUET('/tmp/sb.parquet')",
    "clickhouse": "SYSTEM FLUSH LOGS",
    "tidb": "ADMIN CHECKSUM TABLE t",
    "vitess": "VTCTL REPARENT commerce/0",
    "cockroachdb": "EXPERIMENTAL_RELOCATE LEASE",
    "yugabytedb": "YB_SERVER_REGION()",
    "cassandra": "NODETOOL REPAIR ks",
    "mongodb": "REPLSET GETSTATUS",
    "redis": "BGREWRITEAOF",
    "opensearch_sql_ppl": "FIELDS status | STATS count()",
    "opensearch": 'POST /_bulk {"index":{}}',
    "neo4j": "MATCH (n:Account) RETURN n",
    "influxdb": 'FROM(bucket: "metrics") |> RANGE(start: -1h)',
    "milvus": "RERANK collection accounts",
    "dolt": "PUSH origin main",
    "apache_ignite": "CONTROL.SH --BASELINE",
    "tikv": "RAW_BATCH_GET k1 k2",
    "foundationdb": "DIRECTORY_CREATE /app",
    "immudb": "VERIFIED_GET account:1",
    "xtdb": 'XTDB_Q [:find ?e :where [?e :name "Ada"]]',
}

EXPECTED_POSITIVE_TOKENS = {
    "firebird": '"operation_family":"firebird.ddl.recreate"',
    "postgresql": '"operation_family":"postgresql.events.listen"',
    "mysql": '"operation_family":"mysql.transaction.xa"',
    "sqlite": '"operation_family":"sqlite.extension.load_extension"',
    "mariadb": '"operation_family":"mariadb.handler.cursor"',
    "duckdb": '"operation_family":"duckdb.external_scan.read_parquet"',
    "clickhouse": '"operation_family":"clickhouse.system.command"',
    "tidb": '"operation_family":"tidb.admin.checksum_table"',
    "vitess": '"operation_family":"vitess.topology.reparent"',
    "cockroachdb": '"operation_family":"cockroachdb.cluster.experimental_relocate"',
    "yugabytedb": '"operation_family":"yugabytedb.catalog_overlay.server_region"',
    "cassandra": '"operation_family":"cassandra.admin.nodetool"',
    "mongodb": '"operation_family":"mongodb.replica_set.command"',
    "redis": '"operation_family":"redis.persistence.bgrewriteaof"',
    "opensearch_sql_ppl": '"operation_family":"opensearch_sql_ppl.ppl.stats"',
    "opensearch": '"operation_family":"opensearch.bulk.write"',
    "neo4j": '"operation_family":"neo4j.query.match"',
    "influxdb": '"operation_family":"influxdb.flux.from"',
    "milvus": '"operation_family":"milvus.query.rerank"',
    "dolt": '"operation_family":"dolt.remote.push"',
    "apache_ignite": '"operation_family":"apache_ignite.admin.control_script"',
    "tikv": '"operation_family":"tikv.raw.batch_get"',
    "foundationdb": '"operation_family":"foundationdb.directory.create"',
    "immudb": '"operation_family":"immudb.kv.verified_get"',
    "xtdb": '"operation_family":"xtdb.datalog.query"',
}

KNOWN_FAMILY_OVERLAPS = {
    ("mariadb", "mysql"): "mysql_mariadb_shared_xa_surface_must_remain_mariadb_owned",
}

SBSQL_NEGATIVE_PROBE = "PLAN FORMAT json"
SOURCE_SUFFIXES = (".cpp", ".hpp")
MATRIX_REL = (
    "public_execution_plan"
    "DIALECT_ISOLATION_GUARD_MATRIX.csv"
)
TRACKER_REL = "public_execution_plan"
GATES_REL = "public_execution_plan"
MANIFEST_REL = "project/src/parsers/donor/DonorCompatibilityProfileManifest.csv"

EXPECTED_MATRIX_COUNTS = {
    "manifest_standalone": len(DIALECTS),
    "source_parser_boundary": len(DIALECTS),
    "source_udr_boundary": len(DIALECTS),
    "runtime_distinctive_probe": len(DIALECTS),
    "runtime_sbsql_negative_probe": len(DIALECTS),
}
EXPECTED_FOREIGN_REJECTIONS = len(DIALECTS) * (len(DIALECTS) - 1) - len(KNOWN_FAMILY_OVERLAPS)

REQUIRED_COLUMNS = {
    "guard_id",
    "guard_type",
    "donor",
    "parser_package",
    "parser_support_udr",
    "manifest_profile_class",
    "probe_owner",
    "positive_probe",
    "expected_positive_token",
    "foreign_probe_policy",
    "known_family_overlap",
    "source_boundary",
    "expected_runtime_behavior",
    "test_destination",
    "proof_kind",
    "status",
    "notes",
}

FORBIDDEN_COMPLETION_RE = re.compile(
    r"todo|fixme|stub|skeleton|placeholder|defer|deferred|"
    r"not implemented|future work|file presence",
    re.I,
)
FORBIDDEN_LEAK_RE = re.compile(r"(?:/(?:home|Users)/[^\s,;]+|[A-Za-z]:[\\/][^\s,;]+|local workspace|https?://)")


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(repo_root: pathlib.Path, rel_path: str) -> list[dict[str, str]]:
    path = repo_root / rel_path
    try:
        with path.open(newline="") as handle:
            reader = csv.DictReader(handle)
            rows = list(reader)
    except FileNotFoundError:
        fail(f"missing CSV: {rel_path}")
    require(reader.fieldnames is not None, f"CSV has no header: {rel_path}")
    return rows


def require_columns(rows: list[dict[str, str]], required: set[str], rel_path: str) -> None:
    require(rows, f"CSV has no rows: {rel_path}")
    missing = sorted(required - set(rows[0]))
    require(not missing, f"{rel_path} missing columns: {missing}")
    for row in rows:
        for column in required:
            require(row[column], f"{rel_path} empty {column} in {row}")


def parser_package(donor: str) -> str:
    return f"project/src/parsers/donor/{donor}"


def udr_package(donor: str) -> str:
    return f"project/src/udr/sbu_{donor}_parser_support"


def binary_path(build_root: pathlib.Path, donor: str) -> pathlib.Path:
    return build_root / "src/parsers/donor" / donor / f"sbp_{donor}"


def diagnostic_prefix(donor: str) -> str:
    return donor.upper()


def run_parser(binary: pathlib.Path, text: str) -> tuple[int, str, str]:
    completed = subprocess.run(
        [str(binary), text],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
        check=False,
    )
    return completed.returncode, completed.stdout, completed.stderr


def allowed_hit(dialect: str, forbidden: str, line: str) -> bool:
    if dialect == "mariadb" and forbidden == "mysql":
        return (
            "MYSQL." in line
            or "MYSQL_" in line
            or "mysql_schema" in line
            or ("MYSQL" in line and "catalog_overlays" in line)
        )
    if dialect == "opensearch_sql_ppl" and forbidden == "opensearch":
        return "OpenSearch SQL/PPL" in line or "opensearch_sql_ppl" in line
    return False


def forbidden_tokens_for(root_kind: str, dialect: str) -> list[str]:
    forbidden = [name for name in DIALECTS if name != dialect]
    if root_kind == "parser":
        forbidden.append("sbsql")
    return forbidden


def scan_source_root(
    repo_root: pathlib.Path,
    dialect: str,
    root_kind: str,
    root_path: pathlib.Path,
) -> list[dict[str, object]]:
    forbidden = forbidden_tokens_for(root_kind, dialect)
    pattern = re.compile(
        r"\b(" + "|".join(re.escape(name) for name in forbidden) + r")\b",
        re.IGNORECASE,
    )
    violations: list[dict[str, object]] = []
    for path in sorted(root_path.rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8")
        for line_no, line in enumerate(text.splitlines(), start=1):
            for match in pattern.finditer(line):
                hit = match.group(1).lower()
                if allowed_hit(dialect, hit, line):
                    continue
                violations.append({
                    "dialect": dialect,
                    "source_root": root_kind,
                    "path": path.relative_to(repo_root).as_posix(),
                    "line": line_no,
                    "forbidden_donor_token": hit,
                })
    return violations


def validate_tracker_and_gate_state(
    tracker_rows: list[dict[str, str]],
    gate_rows: list[dict[str, str]],
) -> None:
    tracker = {row["slice_id"]: row for row in tracker_rows}
    for phase, status in {
        "FPR-P0": "p0_input_readiness_verified",
        "FPR-P1": "p1_noncluster_remap_verified",
        "FPR-P2": "p2_cluster_route_remap_verified",
        "FPR-P3": "p3_ast_boundast_envelope_verified",
        "FPR-P4": "p4_refusal_reduction_verified",
        "FPR-P5": "p5_dialect_isolation_verified",
    }.items():
        require(tracker[phase]["status"] == status, f"{phase} not verified")
    require(tracker["FPR-P6"]["status"] in {"pending", "p6_donor_replay_proof_verified"},
            "FPR-P6 must be pending or verified after the donor replay proof gate closes")
    require(tracker["FPR-P7"]["status"] in {"pending", "p7_parser_remap_audit_verified"},
            "FPR-P7 must be pending or verified after the independent audit closes")

    gates = {row["gate_id"]: row for row in gate_rows}
    for gate, status in {
        "FPR-GATE-001": "p0_input_readiness_verified",
        "FPR-GATE-002": "p1_noncluster_remap_verified",
        "FPR-GATE-003": "p2_cluster_route_remap_verified",
        "FPR-GATE-004": "p3_ast_boundast_envelope_verified",
        "FPR-GATE-005": "p4_refusal_reduction_verified",
        "FPR-GATE-006": "p5_dialect_isolation_verified",
    }.items():
        require(gates[gate]["status"] == status, f"{gate} not verified")
    require(gates["FPR-GATE-007"]["status"] in {"pending", "p6_donor_replay_proof_verified"},
            "FPR-GATE-007 must be pending or verified after the donor replay proof gate closes")
    require(gates["FPR-GATE-008"]["status"] in {"pending", "p7_parser_remap_audit_verified"},
            "FPR-GATE-008 must be pending or verified after the independent audit closes")


def validate_matrix(
    repo_root: pathlib.Path,
    rows: list[dict[str, str]],
) -> dict[str, list[dict[str, str]]]:
    require_columns(rows, REQUIRED_COLUMNS, MATRIX_REL)
    require(len(rows) == sum(EXPECTED_MATRIX_COUNTS.values()), "P5 matrix row count drift")
    by_guard_id: set[str] = set()
    grouped: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        require(row["guard_id"] not in by_guard_id, f"duplicate guard_id: {row['guard_id']}")
        by_guard_id.add(row["guard_id"])
        grouped.setdefault(row["guard_type"], []).append(row)
        require(row["donor"] in DIALECTS, f"unknown donor in P5 matrix: {row['donor']}")
        require(row["parser_package"] == parser_package(row["donor"]),
                f"parser package mismatch: {row['guard_id']}")
        require(row["parser_support_udr"] == udr_package(row["donor"]),
                f"UDR package mismatch: {row['guard_id']}")
        require(row["manifest_profile_class"] == "donor_emulation",
                f"wrong manifest profile class: {row['guard_id']}")
        require(row["test_destination"] ==
                "project/tests/donor_sql_parser_first_tranche/parser_dialect_isolation_guard.py",
                f"wrong test destination: {row['guard_id']}")
        require(row["status"] == "p5_dialect_isolation_verified",
                f"wrong P5 status: {row['guard_id']}")
        combined = ";".join(row.values())
        require(not FORBIDDEN_LEAK_RE.search(combined),
                f"private path or URL leaked: {row['guard_id']}")
        for column in ("notes", "expected_runtime_behavior"):
            require(not FORBIDDEN_COMPLETION_RE.search(row[column]),
                    f"unclosed wording in {column}: {row['guard_id']}")

    for guard_type, expected in EXPECTED_MATRIX_COUNTS.items():
        require(len(grouped.get(guard_type, [])) == expected,
                f"{guard_type} count drift")

    for guard_type in EXPECTED_MATRIX_COUNTS:
        donors = {row["donor"] for row in grouped[guard_type]}
        require(donors == set(DIALECTS), f"{guard_type} donor coverage drift")

    manifest = {
        row["family_id"]: row
        for row in read_csv(repo_root, MANIFEST_REL)
        if row["profile_class"] == "donor_emulation"
    }
    for donor in DIALECTS:
        require(donor in manifest, f"manifest lacks donor profile: {donor}")
        require(manifest[donor]["parser_cross_dialect_dependency"] == "false",
                f"manifest cross-dialect dependency enabled: {donor}")
        require((repo_root / parser_package(donor)).is_dir(),
                f"parser package missing: {donor}")
        require((repo_root / udr_package(donor)).is_dir(),
                f"parser-support UDR missing: {donor}")

    runtime_rows = {row["donor"]: row for row in grouped["runtime_distinctive_probe"]}
    for donor, row in runtime_rows.items():
        require(row["probe_owner"] == donor, f"runtime probe owner mismatch: {row['guard_id']}")
        require(row["positive_probe"] == DISTINCTIVE_PROBES[donor],
                f"runtime probe text mismatch: {row['guard_id']}")
        require(row["expected_positive_token"] == EXPECTED_POSITIVE_TOKENS[donor],
                f"runtime expected token mismatch: {row['guard_id']}")
        expected_overlap = ""
        if donor == "mysql":
            expected_overlap = KNOWN_FAMILY_OVERLAPS[("mariadb", "mysql")]
        require(row["known_family_overlap"] == (expected_overlap or "none"),
                f"known overlap mismatch: {row['guard_id']}")
        require(row["proof_kind"] == "runtime_positive_and_pairwise_foreign_rejection",
                f"runtime proof kind mismatch: {row['guard_id']}")

    for row in grouped["runtime_sbsql_negative_probe"]:
        require(row["probe_owner"] == "sbsql",
                f"SBsql negative row has wrong probe owner: {row['guard_id']}")
        require(row["positive_probe"] == SBSQL_NEGATIVE_PROBE,
                f"SBsql negative probe mismatch: {row['guard_id']}")
        require(row["proof_kind"] == "runtime_sbsql_rejection",
                f"SBsql proof kind mismatch: {row['guard_id']}")

    return grouped


def validate_source_boundaries(repo_root: pathlib.Path) -> dict[str, int]:
    all_violations: list[dict[str, object]] = []
    roots_checked = 0
    for donor in DIALECTS:
        for root_kind, rel_path in (
            ("parser", parser_package(donor)),
            ("udr", udr_package(donor)),
        ):
            roots_checked += 1
            all_violations.extend(
                scan_source_root(repo_root, donor, root_kind, repo_root / rel_path)
            )
    if all_violations:
        fail(json.dumps(all_violations, indent=2, sort_keys=True))
    return {"source_roots_checked": roots_checked}


def require_positive_parse(
    build_root: pathlib.Path,
    donor: str,
    probe: str,
    expected_token: str,
) -> None:
    binary = binary_path(build_root, donor)
    require(binary.exists(), f"parser binary missing: {binary}")
    code, stdout, stderr = run_parser(binary, probe)
    output = stdout + stderr
    require(code == 0, f"{donor} positive probe failed: {stderr}")
    for token in [
        "SBLRExecutionEnvelope.v3",
        f'"dialect":"{donor}"',
        expected_token,
        '"donor_engine_sql_executed":false',
        '"sql_text_included":false',
    ]:
        require(token in output, f"{donor} positive probe missing token {token}")


def require_rejected_without_envelope(
    build_root: pathlib.Path,
    target: str,
    source: str,
    probe: str,
) -> None:
    binary = binary_path(build_root, target)
    code, stdout, stderr = run_parser(binary, probe)
    output = stdout + stderr
    require(
        "SBLRExecutionEnvelope.v3" not in output,
        f"{target} admitted foreign {source} probe: {probe}",
    )
    require(code != 0, f"{target} returned success for foreign {source} probe")
    prefix = diagnostic_prefix(target)
    require(
        f"{prefix}.PARSE.UNSUPPORTED_SURFACE" in output
        or f"{prefix}.PARSE.INVALID_INPUT" in output,
        f"{target} foreign {source} rejection missing parser diagnostic",
    )


def require_known_overlap_stays_owned_by_target(
    build_root: pathlib.Path,
    target: str,
    source: str,
    probe: str,
) -> None:
    binary = binary_path(build_root, target)
    code, stdout, stderr = run_parser(binary, probe)
    output = stdout + stderr
    require(code == 0, f"{target}/{source} family overlap failed: {stderr}")
    for token in [
        "SBLRExecutionEnvelope.v3",
        f'"dialect":"{target}"',
        f'"operation_family":"{target}.',
    ]:
        require(token in output, f"{target}/{source} overlap missing {token}")
    require(f'"dialect":"{source}"' not in output,
            f"{target}/{source} overlap changed dialect authority")
    require(f"sbp_{source}" not in output and f"sbup_{source}" not in output,
            f"{target}/{source} overlap leaked source parser package")


def validate_runtime(build_root: pathlib.Path) -> dict[str, int]:
    foreign_rejections = 0
    family_overlaps = 0
    for donor, probe in DISTINCTIVE_PROBES.items():
        require_positive_parse(
            build_root,
            donor,
            probe,
            EXPECTED_POSITIVE_TOKENS[donor],
        )
    for target in DIALECTS:
        for source, probe in DISTINCTIVE_PROBES.items():
            if target == source:
                continue
            if (target, source) in KNOWN_FAMILY_OVERLAPS:
                require_known_overlap_stays_owned_by_target(build_root, target, source, probe)
                family_overlaps += 1
                continue
            require_rejected_without_envelope(build_root, target, source, probe)
            foreign_rejections += 1
    for donor in DIALECTS:
        require_rejected_without_envelope(build_root, donor, "sbsql", SBSQL_NEGATIVE_PROBE)
    require(foreign_rejections == EXPECTED_FOREIGN_REJECTIONS,
            "foreign rejection count drift")
    require(family_overlaps == len(KNOWN_FAMILY_OVERLAPS),
            "family overlap count drift")
    return {
        "runtime_positive_probes": len(DIALECTS),
        "runtime_foreign_rejections": foreign_rejections,
        "runtime_family_overlaps_verified": family_overlaps,
        "runtime_sbsql_rejections": len(DIALECTS),
    }


def write_evidence(
    evidence_file: pathlib.Path,
    matrix_counts: dict[str, int],
    source_counts: dict[str, int],
    runtime_counts: dict[str, int],
) -> None:
    evidence_file.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "gate": "parser_dialect_isolation_guard",
        "authority_note": "diagnostic_output_only_not_source_authority",
        "matrix_counts": matrix_counts,
        "source_counts": source_counts,
        "runtime_counts": runtime_counts,
        "dialects": list(DIALECTS),
        "sbsql_negative_probe": SBSQL_NEGATIVE_PROBE,
        "known_family_overlaps": {
            f"{target}:{source}": policy
            for (target, source), policy in KNOWN_FAMILY_OVERLAPS.items()
        },
        "parser_modules_are_standalone": True,
        "parser_support_udr_modules_are_standalone": True,
        "cross_donor_detection_or_dispatch_allowed": False,
        "sbsql_surface_allowed_in_donor_parser": False,
    }
    evidence_file.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--evidence-file", required=True)
    args = parser.parse_args()
    repo_root = pathlib.Path(args.repo_root).resolve()
    build_root = pathlib.Path(args.build_root).resolve()
    evidence_file = pathlib.Path(args.evidence_file)

    matrix_rows = read_csv(repo_root, MATRIX_REL)
    tracker_rows = read_csv(repo_root, TRACKER_REL)
    gate_rows = read_csv(repo_root, GATES_REL)
    grouped = validate_matrix(repo_root, matrix_rows)
    validate_tracker_and_gate_state(tracker_rows, gate_rows)
    source_counts = validate_source_boundaries(repo_root)
    runtime_counts = validate_runtime(build_root)
    matrix_counts = {guard_type: len(rows) for guard_type, rows in grouped.items()}
    write_evidence(evidence_file, matrix_counts, source_counts, runtime_counts)
    print("parser_dialect_isolation_guard=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
