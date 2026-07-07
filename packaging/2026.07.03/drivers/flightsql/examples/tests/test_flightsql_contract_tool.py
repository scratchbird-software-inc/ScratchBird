from __future__ import annotations

import json
from pathlib import Path
from subprocess import PIPE, run
import sys


LANE = Path(__file__).resolve().parents[1]
TOOL = LANE / "tools" / "sb_isql_flightsql"


def test_flightsql_tool_fails_closed_for_unsupported_ipc_route(tmp_path: Path) -> None:
    script = tmp_path / "input.sbsql"
    script.write_text("select 1;\n", encoding="utf-8")
    command = [
        sys.executable,
        str(TOOL),
        "--database",
        "missing.sbdb",
        "--user",
        "sysdba",
        "--password",
        "masterkey",
        "--route",
        "ipc_local",
        "--ipc-path",
        str(tmp_path / "scratchbird.sock"),
        "--namespace",
        "users.public.examples.flightsql.test",
        "--input",
        str(script),
        "--output",
        str(tmp_path / "stdout.log"),
        "--error",
        str(tmp_path / "stderr.log"),
        "--diagnostics",
        str(tmp_path / "diagnostics.jsonl"),
        "--metrics",
        str(tmp_path / "process-metrics.jsonl"),
        "--transcript",
        str(tmp_path / "wire-transcript.jsonl"),
        "--summary",
        str(tmp_path / "summary.json"),
    ]
    result = run(command, text=True, stdout=PIPE, stderr=PIPE, check=False)
    assert result.returncode == 1
    summary = json.loads((tmp_path / "summary.json").read_text(encoding="utf-8"))
    assert summary["driver_name"] == "flightsql"
    assert summary["status"] == "fail"
    diag_line = (tmp_path / "diagnostics.jsonl").read_text(encoding="utf-8").splitlines()[0]
    diagnostic = json.loads(diag_line)
    assert diagnostic["diagnostic_code"] == "SB_DRIVER_FLIGHTSQL_IPC_UNSUPPORTED"
    assert json.loads((tmp_path / "route-environment.json").read_text(encoding="utf-8"))["transport_endpoint_kind"] == "unix_domain_socket"
