# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import types


def load_tool_module():
    tool_path = Path(__file__).resolve().parents[1] / "tools" / "sb_isql_python.py"
    spec = importlib.util.spec_from_file_location("sb_isql_python_tool", tool_path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FakeCursor:
    description = [("VALUE", 4, None, None, None, None, True)]
    rowcount = 1
    statusmessage = "SELECT"
    lastrowid = None

    def __init__(self):
        self.executed = []
        self._rows = [(1,)]

    def execute(self, sql):
        self.executed.append(sql)
        if sql.strip().upper() == "SHOW DATABASE":
            self.description = [("page_size_bytes", 4, None, None, None, None, True)]
            self._rows = [(8192,)]
        else:
            self.description = [("VALUE", 4, None, None, None, None, True)]
            self._rows = [(1,)]

    def fetchall(self):
        return self._rows

    def _prime_stream_metadata(self, stream):
        self.description = getattr(stream, "description", self.description)

    def _update_description(self, stream):
        self.description = getattr(stream, "description", self.description)


class FakeConnection:
    autocommit = True

    def __init__(self):
        self.cursor_obj = FakeCursor()
        self.committed = False
        self.rolled_back = False
        self.closed = False
        self.compiled = []
        self.executed_sblr = []

    def cursor(self):
        return self.cursor_obj

    def query_metadata(self, collection):
        return FakeCursor()

    def compile_sblr(self, sql):
        self.compiled.append(sql)
        return ("sha256:fake", 1, b"sblr")

    def execute_sblr(self, sblr_hash, sblr_bytecode):
        self.executed_sblr.append((sblr_hash, sblr_bytecode))
        return types.SimpleNamespace(
            description=[("VALUE", 4, None, None, None, None, True)],
            rowcount=1,
            lastrowid=None,
            statusmessage="SELECT",
        )

    def commit(self):
        self.committed = True

    def rollback(self):
        self.rolled_back = True

    def close(self):
        self.closed = True


def args_for(tmp_path, parser_mode="server-parser"):
    run_root = tmp_path / "run"
    sql = tmp_path / "input.sql"
    sql.write_text("SELECT 1;", encoding="utf-8")
    return types.SimpleNamespace(
        database="default",
        host="127.0.0.1",
        port=3092,
        user="alice",
        password="password",
        role="sysarch",
        sslmode="require",
        route="listener-parser",
        parser_mode=parser_mode,
        page_size="8k",
        namespace="users.public.examples.python.run.listener-parser.8k.w0",
        input=str(sql),
        output=str(run_root / "stdout.log"),
        error=str(run_root / "stderr.log"),
        diagnostics=str(run_root / "diagnostics.jsonl"),
        metrics=str(run_root / "metrics.json"),
        transcript=str(run_root / "transcript.jsonl"),
        summary=str(run_root / "summary.json"),
        stop_on_error=True,
        expected_refusals=None,
        statement_timeout_ms=30000,
        fetch_size=1000,
        concurrency_worker=0,
        run_id="pytest",
        create_database=False,
        create_emulation_mode="sbsql",
        language_resource_pack="project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack",
        language_resource_identity="sbsql.common_resource_pack.v1",
        language_resource_hash="sha256:test",
        language_profile="en-US",
        syntax_profile="sbsql.v3",
        topology_profile="topology.sbsql.canonical.v1",
        standard_english_fallback=True,
    )


def test_python_native_tool_routes_output_and_artifacts(tmp_path, monkeypatch):
    tool = load_tool_module()
    fake = FakeConnection()
    monkeypatch.setattr(tool, "connect_with_public_api", lambda _: fake)

    rc = tool.run_script(args_for(tmp_path))

    assert rc == 0
    summary = json.loads((tmp_path / "run" / "summary.json").read_text(encoding="utf-8"))
    assert summary["status"] == "pass"
    assert summary["server_revalidation_required"] is True
    assert summary["driver_or_parser_finality"] == "forbidden"
    for filename in (
        "command-events.jsonl",
        "diagnostics.jsonl",
        "wire-transcript.jsonl",
        "timing-groups.json",
        "result-digests.json",
        "metadata-snapshots.json",
        "security-refusals.json",
        "native-api-coverage.json",
        "code-example-review.json",
        "junit.xml",
        "stdout.log",
        "stderr.log",
    ):
        assert (tmp_path / "run" / filename).is_file(), filename

    api_hits = json.loads((tmp_path / "run" / "native-api-coverage.json").read_text(encoding="utf-8"))
    assert api_hits["scratchbird.connect"] == 1
    assert api_hits["cursor.execute"] == 2
    assert api_hits["cursor.fetchall"] == 2
    assert api_hits["conn.query_metadata"] == 1
    assert fake.closed is True


def test_python_native_tool_executes_driver_sblr_mode(tmp_path, monkeypatch):
    tool = load_tool_module()
    fake = FakeConnection()
    monkeypatch.setattr(tool, "connect_with_public_api", lambda _: fake)

    rc = tool.run_script(args_for(tmp_path, parser_mode="driver-sblr-uuid"))

    assert rc == 0
    summary = json.loads((tmp_path / "run" / "summary.json").read_text(encoding="utf-8"))
    assert summary["status"] == "pass"
    assert fake.compiled == ["SELECT 1"]
    assert fake.executed_sblr == [("sha256:fake", b"sblr")]
    api_hits = json.loads((tmp_path / "run" / "native-api-coverage.json").read_text(encoding="utf-8"))
    assert api_hits["conn.compile_sblr"] == 1
    assert api_hits["conn.execute_sblr"] == 1
    assert fake.closed is True
