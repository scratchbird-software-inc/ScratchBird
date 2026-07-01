from __future__ import annotations

import json
import os
from pathlib import Path
from subprocess import PIPE, run
import sys


LANE = Path(__file__).resolve().parents[1]
TOOL = LANE / "tools" / "sb_isql_adbc"


def test_adbc_tool_fails_closed_and_writes_artifacts(tmp_path: Path) -> None:
    script = tmp_path / "input.sbsql"
    script.write_text("select 1;\n", encoding="utf-8")
    env = os.environ.copy()
    env.pop("SCRATCHBIRD_ADBC_DRIVER", None)
    env["SCRATCHBIRD_ADBC_EXTERNAL_ONLY"] = "true"
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
        "users.public.examples.adbc.test",
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
    assert summary["driver_name"] == "adbc"
    assert summary["status"] == "fail"
    assert summary["server_revalidation_required"] is True
    assert summary["driver_or_parser_finality"] == "forbidden"
    diag_line = (tmp_path / "diagnostics.jsonl").read_text(encoding="utf-8").splitlines()[0]
    diagnostic = json.loads(diag_line)
    assert diagnostic["diagnostic_code"] == "SB_DRIVER_ADBC_RUNTIME_DEPENDENCY_MISSING"
    for name in (
        "command-events.jsonl",
        "route-environment.json",
        "process-metrics.jsonl",
        "native-api-coverage.json",
        "junit.xml",
    ):
        assert (tmp_path / name).is_file()
