#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Smoke-test the ScratchBird AI HTTP adapter contract."""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib import error, parse, request

ROOT_DIR = Path(__file__).resolve().parents[1]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from scratchbird_ai.http_bridge import (  # noqa: E402
    BridgeCompileResult,
    BridgeExecuteResult,
    BridgeSettings,
    ScratchBirdBridgeApp,
    build_http_server,
)


@dataclass(slots=True)
class SmokeConfig:
    mode: str
    base_url: str
    token: str | None
    dialect: str
    query_text: str
    schema: str
    table: str
    database: str | None
    timeout_sec: float
    skip_metadata: bool


class _FakeBridgeBackend:
    def compile_query(
        self,
        *,
        dialect: str,
        query_text: str,
        context: dict[str, Any],
    ) -> BridgeCompileResult:
        del context
        statement_kind = "read" if query_text.strip().lower().startswith("select") else "mutation"
        return BridgeCompileResult(
            statement_kind=statement_kind,
            sblr_hash=f"{dialect}_smoke_hash",
            diagnostics=[],
            warnings=[],
        )

    def execute_query(
        self,
        *,
        dialect: str,
        query_text: str,
        options: dict[str, Any],
        compile_artifact_id: str,
    ) -> BridgeExecuteResult:
        return BridgeExecuteResult(
            rows=[
                {
                    "dialect": dialect,
                    "query_text": query_text,
                    "compile_artifact_id": compile_artifact_id,
                    "options": options,
                }
            ],
            notices=["smoke backend execute ok"],
        )

    def list_schemas(self, *, dialect: str, database: str | None = None) -> list[str]:
        del dialect, database
        return ["public", "analytics"]

    def list_tables(self, *, dialect: str, schema: str) -> list[str]:
        del dialect
        if schema == "public":
            return ["customers", "orders"]
        return ["events"]

    def describe_table(self, *, dialect: str, schema: str, table: str) -> dict[str, Any]:
        return {
            "dialect": dialect,
            "schema": schema,
            "table": table,
            "columns": [
                {"name": "id", "type": "uuid", "nullable": False},
                {"name": "name", "type": "varchar", "nullable": True},
            ],
        }


def _request_json(
    *,
    method: str,
    url: str,
    token: str | None,
    payload: dict[str, Any] | None,
    timeout_sec: float,
) -> tuple[int, dict[str, Any]]:
    headers = {"Accept": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"

    data = None
    if payload is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(payload).encode("utf-8")

    req = request.Request(url=url, method=method.upper(), data=data, headers=headers)
    try:
        with request.urlopen(req, timeout=timeout_sec) as resp:
            status = resp.status
            body = resp.read().decode("utf-8")
    except error.HTTPError as exc:
        status = exc.code
        body = exc.read().decode("utf-8")

    decoded = json.loads(body) if body else {}
    if not isinstance(decoded, dict):
        raise RuntimeError(f"Expected JSON object response from {method} {url}")
    return status, decoded


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def _run_sequence(config: SmokeConfig) -> None:
    print(f"[smoke] base_url={config.base_url} dialect={config.dialect}")
    health_url = f"{config.base_url}/healthz"
    status, doc = _request_json(
        method="GET",
        url=health_url,
        token=config.token,
        payload=None,
        timeout_sec=config.timeout_sec,
    )
    _require(status == 200, f"healthz failed status={status} body={doc}")
    _require(doc.get("status") == "ok", f"healthz invalid body={doc}")
    print("[smoke] healthz OK")

    compile_url = f"{config.base_url}/v1/dialects/{config.dialect}/compile"
    status, compiled = _request_json(
        method="POST",
        url=compile_url,
        token=config.token,
        payload={"query_text": config.query_text, "context": {}},
        timeout_sec=config.timeout_sec,
    )
    _require(status == 200, f"compile failed status={status} body={compiled}")
    _require(isinstance(compiled.get("sblr_hash"), str), "compile response missing sblr_hash")
    _require(
        compiled.get("statement_kind") in {"read", "mutation", "unknown"},
        f"compile response invalid statement_kind={compiled.get('statement_kind')}",
    )
    print("[smoke] compile OK")

    execute_url = f"{config.base_url}/v1/dialects/{config.dialect}/execute"
    status, executed = _request_json(
        method="POST",
        url=execute_url,
        token=config.token,
        payload={
            "compile_artifact_id": "cmp_smoke",
            "query_text": config.query_text,
            "options": {"max_rows": 5},
        },
        timeout_sec=config.timeout_sec,
    )
    _require(status == 200, f"execute failed status={status} body={executed}")
    _require(isinstance(executed.get("rows"), list), "execute response missing rows list")
    _require(isinstance(executed.get("notices"), list), "execute response missing notices list")
    print("[smoke] execute OK")

    if config.skip_metadata:
        print("[smoke] metadata skipped (--skip-metadata)")
        return

    query_part = ""
    if config.database:
        query_part = "?" + parse.urlencode({"database": config.database})
    schemas_url = f"{config.base_url}/v1/dialects/{config.dialect}/schemas{query_part}"
    status, schemas_doc = _request_json(
        method="GET",
        url=schemas_url,
        token=config.token,
        payload=None,
        timeout_sec=config.timeout_sec,
    )
    _require(status == 200, f"schemas failed status={status} body={schemas_doc}")
    schemas = schemas_doc.get("schemas")
    _require(isinstance(schemas, list), "schemas response missing schemas list")
    schema = config.schema or (schemas[0] if schemas else "")
    _require(bool(schema), "no schema available for tables endpoint")
    print(f"[smoke] schemas OK (initial schema={schema})")

    candidate_schemas = [schema] if config.schema else list(schemas)
    selected_schema = ""
    selected_tables: list[Any] = []
    for candidate_schema in candidate_schemas:
        tables_url = (
            f"{config.base_url}/v1/dialects/{config.dialect}/schemas/{candidate_schema}/tables"
        )
        status, tables_doc = _request_json(
            method="GET",
            url=tables_url,
            token=config.token,
            payload=None,
            timeout_sec=config.timeout_sec,
        )
        _require(status == 200, f"tables failed status={status} body={tables_doc}")
        tables = tables_doc.get("tables")
        _require(isinstance(tables, list), "tables response missing tables list")
        if config.table or tables:
            selected_schema = candidate_schema
            selected_tables = tables
            break

    _require(bool(selected_schema), "no schema with describable tables was discovered")
    table = config.table or (selected_tables[0] if selected_tables else "")
    _require(bool(table), "no table available for describe endpoint")
    schema = selected_schema
    print(f"[smoke] tables OK (using schema={schema}, table={table})")

    describe_url = (
        f"{config.base_url}/v1/dialects/{config.dialect}/schemas/{schema}/tables/{table}"
    )
    status, table_doc = _request_json(
        method="GET",
        url=describe_url,
        token=config.token,
        payload=None,
        timeout_sec=config.timeout_sec,
    )
    _require(status == 200, f"describe failed status={status} body={table_doc}")
    _require(isinstance(table_doc.get("columns"), list), "describe response missing columns list")
    print("[smoke] describe OK")


def _run_selftest(config: SmokeConfig) -> None:
    token = config.token or "smoke-token"
    app = ScratchBirdBridgeApp(
        settings=BridgeSettings(
            host="127.0.0.1",
            port=0,
            api_token=token,
            enabled_dialects=(config.dialect,),
            default_dsn="scratchbird://unused",
        ),
        backend=_FakeBridgeBackend(),
    )

    server = build_http_server(app=app)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    try:
        host, port = server.server_address
        run_config = SmokeConfig(
            mode="selftest",
            base_url=f"http://{host}:{port}",
            token=token,
            dialect=config.dialect,
            query_text=config.query_text,
            schema=config.schema,
            table=config.table,
            database=config.database,
            timeout_sec=config.timeout_sec,
            skip_metadata=config.skip_metadata,
        )
        _run_sequence(run_config)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("selftest", "live"), default="selftest")
    parser.add_argument(
        "--base-url",
        default=os.getenv("SCRATCHBIRD_AI_HTTP_BASE_URL", "http://127.0.0.1:3095"),
    )
    parser.add_argument(
        "--token",
        default=(
            os.getenv("SCRATCHBIRD_AI_HTTP_API_TOKEN")
            or os.getenv("SCRATCHBIRD_AI_BRIDGE_API_TOKEN")
            or ""
        ),
    )
    parser.add_argument(
        "--dialect",
        default=os.getenv("SCRATCHBIRD_AI_SMOKE_DIALECT", "native"),
    )
    parser.add_argument(
        "--query-text",
        default=os.getenv("SCRATCHBIRD_AI_SMOKE_QUERY", "SELECT 1"),
    )
    parser.add_argument(
        "--schema",
        default=os.getenv("SCRATCHBIRD_AI_SMOKE_SCHEMA", ""),
    )
    parser.add_argument(
        "--table",
        default=os.getenv("SCRATCHBIRD_AI_SMOKE_TABLE", ""),
    )
    parser.add_argument(
        "--database",
        default=os.getenv("SCRATCHBIRD_AI_SMOKE_DATABASE", ""),
    )
    parser.add_argument("--timeout-sec", type=float, default=3.0)
    parser.add_argument("--skip-metadata", action="store_true")
    return parser


def main() -> int:
    args = _build_parser().parse_args()
    config = SmokeConfig(
        mode=args.mode,
        base_url=args.base_url.rstrip("/"),
        token=args.token or None,
        dialect=args.dialect.strip().lower(),
        query_text=args.query_text,
        schema=args.schema,
        table=args.table,
        database=args.database or None,
        timeout_sec=args.timeout_sec,
        skip_metadata=bool(args.skip_metadata),
    )

    try:
        if config.mode == "selftest":
            _run_selftest(config)
        else:
            _run_sequence(config)
    except Exception as exc:
        print(f"[smoke] FAILED: {exc}", file=sys.stderr)
        return 1

    print("[smoke] PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
