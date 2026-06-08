#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""ORH-125 live route evidence slice.

This gate executes real embedded, local IPC, INET, CLI, and Python-driver route
surfaces where available, captures comparable result/evidence envelopes, and
fails only on stale or internally inconsistent evidence. Missing commercial
closure fields remain exact blockers so ORH-125 cannot be marked complete from
synthetic or static descriptors.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[3]
SBS_WORKER = REPO_ROOT / "project" / "tests" / "sbsql_parser_worker"
PY_DRIVER = REPO_ROOT / "project" / "drivers" / "driver" / "python" / "src"
sys.path.insert(0, str(SBS_WORKER))

import cdp_benchmark_fixture_support as support  # noqa: E402
import cdp_route_split_benchmark_gate as route_split  # noqa: E402


QUERY = "SELECT 1;"
EXPECTED_ROWS = [["1"]]
VERIFIER = route_split.VERIFIER


class Orh125LiveRouteError(RuntimeError):
    pass


@dataclass
class RouteCapture:
    route_kind: str
    route_label: str
    live_route_executed: bool
    static_descriptor_only: bool
    rows: list[list[str]]
    diagnostics: list[str]
    database_parameters_hash: str
    session_rights_digest: str
    session_principal_claim: str
    session_auth_provider_family: str
    security_epoch: int | None
    transaction_snapshot_id: str
    database_identity: str
    snapshot_visible_through_local_transaction_id: int
    local_transaction_id: int
    result_contract_hash: str
    required_ordering: str
    profiler_source_label: str
    authority: str
    evidence_path: str | None = None
    exact_blockers: list[str] = field(default_factory=list)

    def as_json(self) -> dict[str, Any]:
        return {
            "route_kind": self.route_kind,
            "route_label": self.route_label,
            "live_route_executed": self.live_route_executed,
            "static_descriptor_only": self.static_descriptor_only,
            "rows": self.rows,
            "diagnostics": self.diagnostics,
            "database_parameters_hash": self.database_parameters_hash,
            "session_rights_digest": self.session_rights_digest,
            "session_principal_claim": self.session_principal_claim,
            "session_auth_provider_family": self.session_auth_provider_family,
            "security_epoch": self.security_epoch,
            "transaction_snapshot_id": self.transaction_snapshot_id,
            "database_identity": self.database_identity,
            "snapshot_visible_through_local_transaction_id":
                self.snapshot_visible_through_local_transaction_id,
            "local_transaction_id": self.local_transaction_id,
            "result_contract_hash": self.result_contract_hash,
            "required_ordering": self.required_ordering,
            "profiler_source_label": self.profiler_source_label,
            "authority": self.authority,
            "evidence_path": self.evidence_path,
            "exact_blockers": list(self.exact_blockers),
        }


def semantic_rows(stdout: str) -> list[list[str]]:
    rows: list[list[str]] = []
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("Rows affected:"):
            continue
        if line.startswith("Stopping due to error"):
            continue
        rows.append(line.split("|"))
    return rows


def route_port(route: route_split.RouteContext) -> int:
    for arg in route.fixed_args:
        if arg.startswith("--port="):
            return int(arg.split("=", 1)[1])
    raise Orh125LiveRouteError(f"route {route.name} has no TCP port evidence")


def session_digest(route_kind: str,
                   username: str,
                   role: str,
                   auth_method: str,
                   evidence: dict[str, Any]) -> str:
    del route_kind
    return support.hash_json({
        "principal_claim": evidence.get("principal_claim") or username or "",
        "auth_provider_family": evidence.get("auth_provider_family") or auth_method or "",
        "security_epoch": str(evidence.get("security_epoch") or ""),
        "role": role or "",
    })


def result_contract_hash(rows: list[list[str]], diagnostics: list[str]) -> str:
    return support.hash_json({
        "query": QUERY,
        "required_ordering": "single_row_constant",
        "rows": rows,
        "diagnostics": diagnostics,
    })


def base_database_parameters_hash() -> str:
    return support.hash_json({
        "logical_database": "orh125_cross_route_equivalence_fixture",
        "schema": "users.public",
        "query": QUERY,
        "parameters": [],
    })


def auth_evidence() -> str:
    return f"scheme=local_password_v1;principal=alice;verifier={VERIFIER}"


def database_identity(evidence: dict[str, Any]) -> str:
    database_name = str(evidence.get("database_name") or "")
    if not database_name:
        return ""
    try:
        return str(Path(database_name).resolve())
    except OSError:
        return database_name


def build_capture(route_kind: str,
                  route_label: str,
                  rows: list[list[str]],
                  diagnostics: list[str],
                  evidence: dict[str, Any],
                  evidence_path: Path | None,
                  username: str,
                  auth_method: str) -> RouteCapture:
    txn_id = int(evidence.get("local_transaction_id") or 0)
    snapshot_visible_through = int(
        evidence.get("snapshot_visible_through_local_transaction_id") or 0)
    blockers: list[str] = []
    if not evidence.get("live_route_executed"):
        blockers.append("ORH125.ROUTE_EVIDENCE.NOT_LIVE")
    if evidence.get("authority") != "advisory_route_capture_only":
        blockers.append("ORH125.MGA_AUTHORITY.ADVISORY_CAPTURE_MISSING")
    if txn_id <= 0:
        blockers.append("ORH125.TRANSACTION_SNAPSHOT.LOCAL_TXN_ID_MISSING")
    if snapshot_visible_through <= 0:
        blockers.append("ORH125.TRANSACTION_SNAPSHOT.VISIBLE_THROUGH_TXN_MISSING")
    if not evidence.get("security_epoch_available", False):
        blockers.append("ORH125.SESSION_RIGHTS.SECURITY_EPOCH_NOT_EXPOSED")
    security_epoch = int(evidence.get("security_epoch") or 0)
    principal_claim = str(evidence.get("principal_claim") or username or "")
    auth_provider_family = str(evidence.get("auth_provider_family") or auth_method or "")

    return RouteCapture(
        route_kind=route_kind,
        route_label=route_label,
        live_route_executed=bool(evidence.get("live_route_executed")),
        static_descriptor_only=False,
        rows=rows,
        diagnostics=diagnostics,
        database_parameters_hash=base_database_parameters_hash(),
        session_principal_claim=principal_claim,
        session_auth_provider_family=auth_provider_family,
        session_rights_digest=session_digest(
            route_kind,
            username,
            str(evidence.get("role") or ""),
            auth_method,
            evidence),
        security_epoch=security_epoch or None,
        transaction_snapshot_id=str(evidence.get("transaction_snapshot_id") or f"mga-local-txn:{txn_id}"),
        database_identity=database_identity(evidence),
        snapshot_visible_through_local_transaction_id=snapshot_visible_through,
        local_transaction_id=txn_id,
        result_contract_hash=result_contract_hash(rows, diagnostics),
        required_ordering="single_row_constant",
        profiler_source_label=str(evidence.get("source_label") or route_label),
        authority=str(evidence.get("authority") or ""),
        evidence_path=str(evidence_path) if evidence_path else None,
        exact_blockers=blockers,
    )


def run_cli_capture(route: route_split.RouteContext,
                    route_kind: str,
                    route_label: str,
                    lane_id: str) -> RouteCapture:
    case_dir = route.root / "orh125" / lane_id
    case_dir.mkdir(parents=True, exist_ok=True)
    out_path = case_dir / "sb_isql.out"
    err_path = case_dir / "sb_isql.err"
    evidence_path = case_dir / "route_evidence.json"
    env = os.environ.copy()
    env["SCRATCHBIRD_ORH125_ROUTE_EVIDENCE_FILE"] = str(evidence_path)
    env["SCRATCHBIRD_ORH125_ROUTE_LABEL"] = route_label
    completed = subprocess.run(
        route.cli_args(lane_id) + ["-q", "-A", "-t", "-b", "-c", QUERY],
        stdout=out_path.open("wb"),
        stderr=err_path.open("wb"),
        env=env,
        check=False,
        timeout=35,
    )
    stdout = out_path.read_text(encoding="utf-8", errors="replace")
    stderr = err_path.read_text(encoding="utf-8", errors="replace").strip()
    if completed.returncode != 0:
        raise Orh125LiveRouteError(
            f"{route_label} failed rc={completed.returncode} stdout={stdout!r} stderr={stderr!r}")
    if not evidence_path.exists():
        raise Orh125LiveRouteError(f"{route_label} did not write ORH-125 route evidence")
    rows = semantic_rows(stdout)
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    username = str(evidence.get("username") or ("alice" if route_kind in {"ipc", "inet", "cli"} else "embedded_local"))
    auth_method = "local_password_v1" if username == "alice" else "embedded_local_sysarch"
    return build_capture(
        route_kind=route_kind,
        route_label=route_label,
        rows=rows,
        diagnostics=[],
        evidence=evidence,
        evidence_path=evidence_path,
        username=username,
        auth_method=auth_method,
    )


def run_driver_capture(route: route_split.RouteContext) -> RouteCapture:
    sys.path.insert(0, str(PY_DRIVER))
    import scratchbird  # type: ignore  # noqa: E402

    port = route_port(route)
    conn = scratchbird.connect(
        host="127.0.0.1",
        port=port,
        database=str(route.database),
        user="alice",
        password=auth_evidence(),
        sslmode="disable",
        binary_transfer=False,
    )
    try:
        cursor = conn.execute(QUERY)
        row = cursor.fetchone()
        evidence = conn.route_evidence_snapshot()
    finally:
        conn.close()

    rows = [[str(value) for value in row]] if row is not None else []
    evidence.update({
        "source_label": "python_driver_connection",
        "route_label": "driver:python:inet",
        "authority": "advisory_route_capture_only",
    })
    return build_capture(
        route_kind="driver",
        route_label="driver:python:inet",
        rows=rows,
        diagnostics=[],
        evidence=evidence,
        evidence_path=None,
        username="alice",
        auth_method="local_password_v1",
    )


def unsupported_driver_capture(message: str) -> RouteCapture:
    return RouteCapture(
        route_kind="driver",
        route_label="driver:python:inet",
        live_route_executed=False,
        static_descriptor_only=False,
        rows=[],
        diagnostics=[message],
        database_parameters_hash=base_database_parameters_hash(),
        session_rights_digest=session_digest("driver", "alice", "", "local_password_v1", {}),
        session_principal_claim="",
        session_auth_provider_family="",
        security_epoch=None,
        transaction_snapshot_id="",
        database_identity="",
        snapshot_visible_through_local_transaction_id=0,
        local_transaction_id=0,
        result_contract_hash=result_contract_hash([], [message]),
        required_ordering="single_row_constant",
        profiler_source_label="python_driver_connection",
        authority="advisory_route_capture_only",
        exact_blockers=[
            "ORH125.ROUTE_UNSUPPORTED.DRIVER_LIVE_CAPTURE_FAILED",
            "ORH125.TRANSACTION_SNAPSHOT.LOCAL_TXN_ID_MISSING",
            "ORH125.SESSION_RIGHTS.SECURITY_EPOCH_NOT_EXPOSED",
        ],
    )


def validate_captures(captures: list[RouteCapture]) -> list[str]:
    blockers: list[str] = []
    by_kind = {capture.route_kind: capture for capture in captures}
    for required in ("embedded", "ipc", "inet", "cli", "driver"):
        if required not in by_kind:
            blockers.append(f"ORH125.ROUTE_MISSING.{required.upper()}")

    live_rows = [capture for capture in captures if capture.live_route_executed]
    for capture in live_rows:
        if capture.rows != EXPECTED_ROWS:
            raise Orh125LiveRouteError(
                f"{capture.route_label} rows mismatch expected={EXPECTED_ROWS!r} actual={capture.rows!r}")
        if capture.static_descriptor_only:
            raise Orh125LiveRouteError(f"{capture.route_label} used static descriptor evidence")
        if capture.database_parameters_hash != base_database_parameters_hash():
            raise Orh125LiveRouteError(f"{capture.route_label} database parameter hash drifted")

    session_hashes = {capture.session_rights_digest for capture in live_rows}
    if len(session_hashes) > 1:
        non_embedded_hashes = {
            capture.session_rights_digest for capture in live_rows
            if capture.route_kind != "embedded"
        }
        if len(non_embedded_hashes) == 1 and any(capture.route_kind == "embedded" for capture in live_rows):
            embedded_capture = by_kind.get("embedded")
            non_embedded_providers = {
                capture.session_auth_provider_family for capture in live_rows
                if capture.route_kind != "embedded"
            }
            non_embedded_principals = {
                capture.session_principal_claim for capture in live_rows
                if capture.route_kind != "embedded"
            }
            if (embedded_capture is not None and
                    embedded_capture.session_principal_claim == "sysarch" and
                    embedded_capture.session_auth_provider_family == "embedded_sysarch" and
                    non_embedded_principals == {"alice"} and
                    non_embedded_providers == {"local_password"}):
                blockers.append("ORH125.SESSION_RIGHTS.EMBEDDED_SYSARCH_BYPASS_NOT_COMPARABLE")
            else:
                blockers.append("ORH125.SESSION_RIGHTS.DIGEST_MISMATCH_ACROSS_LIVE_ROUTES")
        else:
            blockers.append("ORH125.SESSION_RIGHTS.DIGEST_MISMATCH_ACROSS_LIVE_ROUTES")
    snapshot_keys = {
        (capture.database_identity,
         capture.snapshot_visible_through_local_transaction_id)
        for capture in live_rows
        if capture.database_identity and
        capture.snapshot_visible_through_local_transaction_id > 0
    }
    if len(snapshot_keys) != 1:
        blockers.append("ORH125.TRANSACTION_SNAPSHOT.INDEPENDENT_ROUTE_SESSIONS_NOT_SHARED")
    result_hashes = {capture.result_contract_hash for capture in live_rows}
    if len(result_hashes) > 1:
        raise Orh125LiveRouteError(f"live result contract hashes differ: {sorted(result_hashes)}")

    for capture in captures:
        blockers.extend(capture.exact_blockers)
    return sorted(set(blockers))


def exact_blocker_details(blockers: list[str]) -> list[dict[str, Any]]:
    details: list[dict[str, Any]] = []
    if "ORH125.SESSION_RIGHTS.EMBEDDED_SYSARCH_BYPASS_NOT_COMPARABLE" in blockers:
        details.append({
            "code": "ORH125.SESSION_RIGHTS.EMBEDDED_SYSARCH_BYPASS_NOT_COMPARABLE",
            "classification": "product_design_gap",
            "description": (
                "Embedded direct SBsql reported the explicit local sysarch bypass while "
                "non-embedded routes reported alice/local_password_v1. Authenticated "
                "embedded direct attach is expected when alice credentials and the "
                "database-local auth sidecar are supplied, so this blocker identifies "
                "a regression to the fallback path or a route-evidence mismatch."
            ),
            "policy_refs": [
                {
                    "path": "project/src/parsers/sbsql_worker/wire/sbsql_sbwp_wire.cpp",
                    "search_key": "SbsqlTestWireSession::AuthenticateCredentials",
                    "policy": (
                        "embedded_engine_direct must call authenticated embedded attach "
                        "when credential evidence is supplied and may use sysarch only "
                        "for explicit embedded-sysarch fallback"
                    ),
                },
                {
                    "path": "project/src/parsers/sbsql_worker/embedded/embedded_engine_client.cpp",
                    "search_key": "EmbeddedEngineClient::AuthenticateAndAttach",
                    "policy": (
                        "embedded direct auth must use engine HandleAuthHandoff and "
                        "HandleAttachDatabase before publishing route evidence"
                    ),
                },
                {
                    "path": "project/src/server/session_registry.cpp",
                    "search_key": "HandleEmbeddedSysarchAttach",
                    "policy": (
                        "HandleEmbeddedSysarchAttach remains an explicit fallback and "
                        "must not satisfy the cross-route authenticated-rights gate"
                    ),
                },
            ],
        })
    return details


def start_shared_embedded(args: argparse.Namespace, work: Path, database: Path) -> route_split.RouteContext:
    root = work / "e"
    root.mkdir(parents=True, exist_ok=True)
    database.parent.mkdir(parents=True, exist_ok=True)
    route_split.auth_file(database)
    return route_split.RouteContext(
        name="embedded",
        root=root,
        database=database,
        sb_isql=args.sb_isql,
        fixed_args=[
            args.sb_isql,
            str(database),
            "--mode=embedded",
            "--sslmode=disable",
            "-U",
            "alice",
            "-P",
            auth_evidence(),
        ],
    )


def start_shared_local_ipc(args: argparse.Namespace, work: Path, database: Path) -> route_split.RouteContext:
    root = work / "i"
    control = root / "sc"
    runtime = root / "sr"
    endpoint = control / "s.sock"
    root.mkdir(parents=True, exist_ok=True)
    route_split.auth_file(database)
    server = subprocess.Popen(
        [
            args.server,
            "--foreground",
            "--no-listeners",
            "--create-if-missing",
            "--control-dir",
            str(control),
            "--runtime-dir",
            str(runtime),
            "--database",
            str(database),
            "--sbps-endpoint",
            str(endpoint),
        ],
        stdout=(root / "server.out").open("wb"),
        stderr=(root / "server.err").open("wb"),
    )
    route_split.wait_for_path(endpoint)
    return route_split.RouteContext(
        name="ipc",
        root=root,
        database=database,
        sb_isql=args.sb_isql,
        fixed_args=[
            args.sb_isql,
            str(database),
            "--mode=local-ipc",
            "--ipc-method=unix",
            f"--ipc-path={endpoint}",
            "--sslmode=disable",
            "-U",
            "alice",
            "-P",
            auth_evidence(),
        ],
        processes=[server],
    )


def start_shared_inet(args: argparse.Namespace, work: Path, database: Path) -> route_split.RouteContext:
    root = work / "n"
    server_control = root / "sc"
    server_runtime = root / "sr"
    listener_control = root / "lc"
    listener_runtime = root / "lr"
    endpoint = server_control / "s.sock"
    port = route_split.find_free_port()
    root.mkdir(parents=True, exist_ok=True)
    route_split.auth_file(database)
    server = subprocess.Popen(
        [
            args.server,
            "--foreground",
            "--no-listeners",
            "--create-if-missing",
            "--control-dir",
            str(server_control),
            "--runtime-dir",
            str(server_runtime),
            "--database",
            str(database),
            "--sbps-endpoint",
            str(endpoint),
        ],
        stdout=(root / "server.out").open("wb"),
        stderr=(root / "server.err").open("wb"),
    )
    route_split.wait_for_path(endpoint)
    listener = subprocess.Popen(
        [
            args.listener,
            "--foreground",
            "--protocol-family=sbsql",
            "--listener-profile=default",
            "--bundle-contract-id=bundle.default@1",
            f"--database-selector=dev_bootstrap_path:{database}",
            f"--server-endpoint=unix:{endpoint}",
            f"--parser-executable={args.parser_worker}",
            f"--control-dir={listener_control}",
            f"--runtime-dir={listener_runtime}",
            "--bind-address=127.0.0.1",
            f"--port={port}",
            "--warm-pool-min=1",
            "--warm-pool-max=2",
        ],
        stdout=(root / "listener.out").open("wb"),
        stderr=(root / "listener.err").open("wb"),
    )
    route_split.wait_for_tcp(port)
    return route_split.RouteContext(
        name="inet",
        root=root,
        database=database,
        sb_isql=args.sb_isql,
        fixed_args=[
            args.sb_isql,
            str(database),
            "--host=127.0.0.1",
            f"--port={port}",
            "--sslmode=disable",
            "-U",
            "alice",
            "-P",
            auth_evidence(),
        ],
        processes=[server, listener],
    )


def build_payload(args: argparse.Namespace, work: Path) -> dict[str, Any]:
    shared_database = work / "shared" / "orh125_shared.sbdb"
    captures: list[RouteCapture] = []
    routes: list[route_split.RouteContext] = []
    try:
        embedded = start_shared_embedded(args, work, shared_database)
        captures.append(run_cli_capture(embedded, "embedded", "embedded:sb_isql", "embedded"))

        ipc = start_shared_local_ipc(args, work, shared_database)
        routes.append(ipc)
        captures.append(run_cli_capture(ipc, "ipc", "ipc:sb_isql", "ipc"))
        route_split.stop_route(ipc)
        routes.remove(ipc)

        inet = start_shared_inet(args, work, shared_database)
        routes.append(inet)
        captures.append(run_cli_capture(inet, "inet", "inet:sb_isql", "inet"))
        captures.append(run_cli_capture(inet, "cli", "cli:sb_isql:inet", "cli"))
        try:
            captures.append(run_driver_capture(inet))
        except Exception as exc:  # noqa: BLE001 - exact blocker is the evidence.
            captures.append(unsupported_driver_capture(str(exc)))
    finally:
        for route in reversed(routes):
            route_split.stop_route(route)

    blockers = validate_captures(captures)
    acceptance_status = "complete_live_equivalence" if not blockers else "pending_exact_blockers"
    payload = {
        "schema_version": "ORH125_LIVE_ROUTE_EQUIVALENCE_V1",
        "gate": "optimizer_runtime_hot_path_orh_125_live_route_gate",
        "timestamp_utc": support.utc_timestamp(),
        "acceptance_status": acceptance_status,
        "benchmark_clean": acceptance_status == "complete_live_equivalence",
        "mga_authority": "engine_transaction_snapshot_only",
        "query": QUERY,
        "expected_rows": EXPECTED_ROWS,
        "route_captures": [capture.as_json() for capture in captures],
        "exact_blockers": blockers,
        "exact_blocker_details": exact_blocker_details(blockers),
    }
    support.write_json(work / "orh-125-live-route-evidence.json", payload)
    return payload


def dump_logs(work: Path) -> None:
    for path in sorted(work.rglob("*.out")) + sorted(work.rglob("*.err")):
        if path.exists() and path.stat().st_size:
            print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--build-mode", default="unknown")
    args = parser.parse_args(argv[1:])

    work = route_split.make_work_dir(Path(args.work_dir))
    try:
        payload = build_payload(args, work)
        evidence_path = work / "orh-125-live-route-evidence.json"
        print(f"optimizer_runtime_hot_path_orh_125_live_route_gate=passed output={evidence_path}")
        print(f"acceptance_status={payload['acceptance_status']}")
        if payload["exact_blockers"]:
            print("exact_blockers=" + ",".join(payload["exact_blockers"]))
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest needs the concrete failure.
        print(f"optimizer_runtime_hot_path_orh_125_live_route_gate=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
