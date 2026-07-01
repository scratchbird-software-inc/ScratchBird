from __future__ import annotations

import json
import os
from pathlib import Path
from subprocess import PIPE, run
import sys


LANE = Path(__file__).resolve().parents[1]
TOOL = LANE / "tools" / "sb_isql_r2dbc"


def test_r2dbc_tool_fails_closed_without_classpath(tmp_path: Path) -> None:
    script = tmp_path / "input.sbsql"
    script.write_text("select 1;\n", encoding="utf-8")
    env = os.environ.copy()
    env.pop("SCRATCHBIRD_R2DBC_CLASSPATH", None)
    env.pop("CLASSPATH", None)
    env["SCRATCHBIRD_R2DBC_EXTERNAL_ONLY"] = "true"
    command = [
        sys.executable,
        str(TOOL),
        "--database",
        "missing.sbdb",
        "--user",
        "sysdba",
        "--password",
        "masterkey",
        "--namespace",
        "users.public.examples.r2dbc.test",
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
    result = run(command, env=env, text=True, stdout=PIPE, stderr=PIPE, check=False)
    assert result.returncode == 1
    summary = json.loads((tmp_path / "summary.json").read_text(encoding="utf-8"))
    assert summary["driver_name"] == "r2dbc"
    assert summary["status"] == "fail"
    diagnostic = json.loads((tmp_path / "diagnostics.jsonl").read_text(encoding="utf-8").splitlines()[0])
    assert diagnostic["diagnostic_code"] == "SB_DRIVER_R2DBC_RUNTIME_DEPENDENCY_MISSING"
    assert diagnostic["dependency"] == "SCRATCHBIRD_R2DBC_CLASSPATH"
