#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run a controlled Firebird QA pytest replay against ScratchBird.

The acquired Firebird QA suite is local-only and ignored by git. Public CTest
runs skip this gate. Local and release replay modes run the original pytest
suite through the Firebird Python driver pointed at the ScratchBird Firebird
compatibility listener.
"""

from __future__ import annotations

import argparse
import ast
import csv
import hashlib
import json
import os
import re
import signal
import shutil
import subprocess
import sys
import time
import tokenize
import warnings
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

import reference_original_tool_smoke as smoke


SKIP_RETURN_CODE = 77
DEFAULT_MODE = "public-no-payload"
VALID_RUN_MODES = {
    "local-optional",
    "release-mandatory",
    "single-family",
    "diagnostic-subset",
}
CANONICAL_SCOPE_PATH = (
    Path(__file__).resolve().parent
    / "reference_release_acquisition/firebird/5.0.4/regression/FIREBIRD_QA_CANONICAL_SCOPE_MANIFEST.csv"
)
CANONICAL_CASE_FILE_COUNT = 3003
CANONICAL_SHARD_COUNT = 12
REQUIRED_FIREBIRD_TOOLS = (
    "isql",
    "gbak",
    "nbackup",
    "gstat",
    "gfix",
    "gsec",
    "fbsvcmgr",
)


def utc_timestamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8", errors="replace")).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_canonical_scope(scope_path: Path = CANONICAL_SCOPE_PATH) -> dict[str, Any]:
    """Load and self-validate the checked 3,003-case acquisition manifest."""
    with scope_path.open(newline="", encoding="utf-8") as handle:
        scope_rows = list(csv.DictReader(handle))
    if len(scope_rows) != 1:
        raise RuntimeError("canonical Firebird QA scope manifest must contain one row")
    metadata: dict[str, Any] = dict(scope_rows[0])
    metadata["case_file_count"] = int(metadata["case_file_count"])
    metadata["shard_count"] = int(metadata["shard_count"])
    metadata["legacy_1949_entry_manifest_authoritative"] = (
        metadata["legacy_1949_entry_manifest_authoritative"].lower() == "true"
    )
    if metadata.get("schema_version") != "scratchbird_firebird_qa_canonical_test_scope_v1":
        raise RuntimeError("invalid canonical Firebird QA scope schema")
    if scope_path.resolve() == CANONICAL_SCOPE_PATH.resolve() and (
        metadata.get("case_file_count") != CANONICAL_CASE_FILE_COUNT
        or metadata.get("shard_count") != CANONICAL_SHARD_COUNT
    ):
        raise RuntimeError(
            "checked Firebird QA production scope must remain 3,003 cases in 12 shards"
        )
    manifest_path = scope_path.parent / str(metadata["manifest_file"])
    entries: list[dict[str, Any]] = []
    manifest = hashlib.sha256()
    with manifest_path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        expected_fields = ["ordinal", "relative_path", "size_bytes", "content_sha256"]
        if reader.fieldnames != expected_fields:
            raise RuntimeError(
                f"invalid canonical Firebird QA manifest columns: {reader.fieldnames}"
            )
        for expected_ordinal, row in enumerate(reader):
            ordinal = int(row["ordinal"])
            relative_path = row["relative_path"]
            content_sha256 = row["content_sha256"]
            if ordinal != expected_ordinal:
                raise RuntimeError(
                    "non-contiguous canonical Firebird QA manifest ordinal: "
                    f"expected={expected_ordinal} actual={ordinal}"
                )
            if not re.fullmatch(r"[0-9a-f]{64}", content_sha256):
                raise RuntimeError(
                    f"invalid canonical content digest at ordinal {ordinal}"
                )
            relative = Path(relative_path)
            if relative.is_absolute() or ".." in relative.parts:
                raise RuntimeError(
                    f"unsafe canonical Firebird QA relative path at ordinal {ordinal}"
                )
            entries.append(
                {
                    "ordinal": ordinal,
                    "path": relative_path,
                    "size_bytes": int(row["size_bytes"]),
                    "content_sha256": content_sha256,
                }
            )
            manifest.update(relative_path.encode("utf-8"))
            manifest.update(b"\0")
            manifest.update(content_sha256.encode("ascii"))
            manifest.update(b"\0")
    paths = [entry["path"] for entry in entries]
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        raise RuntimeError("canonical Firebird QA manifest paths are not sorted and unique")
    if len(entries) != int(metadata["case_file_count"]):
        raise RuntimeError(
            "canonical Firebird QA manifest count mismatch: "
            f"metadata={metadata['case_file_count']} entries={len(entries)}"
        )
    if manifest.hexdigest() != metadata["manifest_sha256"]:
        raise RuntimeError(
            "canonical Firebird QA manifest hash mismatch: "
            f"metadata={metadata['manifest_sha256']} computed={manifest.hexdigest()}"
        )
    metadata["scope_file"] = str(scope_path)
    metadata["manifest_path"] = str(manifest_path)
    metadata["entries"] = entries
    return metadata


def discovery_manifest(qa_root: Path, tests: list[Path]) -> dict[str, Any]:
    entries: list[dict[str, Any]] = []
    manifest = hashlib.sha256()
    for test in tests:
        try:
            relative_path = test.relative_to(qa_root).as_posix()
        except ValueError:
            relative_path = test.resolve().as_posix()
        content = test.read_bytes()
        content_sha256 = hashlib.sha256(content).hexdigest()
        entry = {
            "path": relative_path,
            "size_bytes": len(content),
            "content_sha256": content_sha256,
        }
        entries.append(entry)
        manifest.update(relative_path.encode("utf-8"))
        manifest.update(b"\0")
        manifest.update(content_sha256.encode("ascii"))
        manifest.update(b"\0")
    return {
        "schema_version": "scratchbird_firebird_qa_discovery_manifest_v1",
        "case_file_count": len(entries),
        "manifest_sha256": manifest.hexdigest(),
        "entries": entries,
    }


def _ast_attribute_path(node: ast.AST) -> tuple[str, ...]:
    parts: list[str] = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if not isinstance(node, ast.Name):
        return ()
    parts.append(node.id)
    return tuple(reversed(parts))


def _literal_pytest_mark_skip_reason(decorator: ast.expr) -> str | None:
    if not isinstance(decorator, ast.Call) or _ast_attribute_path(
        decorator.func
    ) != ("pytest", "mark", "skip"):
        return None
    reason_node: ast.expr | None = None
    if len(decorator.args) == 1 and not decorator.keywords:
        reason_node = decorator.args[0]
    elif not decorator.args and len(decorator.keywords) == 1:
        keyword = decorator.keywords[0]
        if keyword.arg == "reason":
            reason_node = keyword.value
    if not isinstance(reason_node, ast.Constant) or type(reason_node.value) is not str:
        return None
    return reason_node.value


def upstream_static_skip_decorators(
    source_path: Path,
) -> dict[str, list[dict[str, Any]]]:
    """Return only direct, literal, unconditional pytest skip decorators.

    Function bodies, fixtures, module-level markers, aliases, skipif, and xfail are
    deliberately outside this audit. If a function has multiple exact literal
    skip decorators, the JUnit message must identify exactly one of them. Any
    conditional, xfail, or nonliteral skip-like decorator rejects the whole node.
    """
    with tokenize.open(source_path) as handle:
        source = handle.read()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", SyntaxWarning)
        tree = ast.parse(source, filename=str(source_path))
    audited: dict[str, list[dict[str, Any]]] = {}

    def visit_body(body: list[ast.stmt], class_path: tuple[str, ...] = ()) -> None:
        for node in body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                skip_like = [
                    decorator
                    for decorator in node.decorator_list
                    if _ast_attribute_path(
                        decorator.func
                        if isinstance(decorator, ast.Call)
                        else decorator
                    )
                    in {
                        ("pytest", "mark", "skip"),
                        ("pytest", "mark", "skipif"),
                        ("pytest", "mark", "xfail"),
                    }
                ]
                if not skip_like:
                    continue
                decorator_audits: list[dict[str, Any]] = []
                for decorator in skip_like:
                    reason = _literal_pytest_mark_skip_reason(decorator)
                    if reason is None:
                        decorator_audits = []
                        break
                    decorator_audits.append(
                        {"reason": reason, "source_line": decorator.lineno}
                    )
                if not decorator_audits:
                    continue
                qualified_name = ".".join((*class_path, node.name))
                audited[qualified_name] = decorator_audits
            elif isinstance(node, ast.ClassDef):
                visit_body(node.body, (*class_path, node.name))

    visit_body(tree.body)
    return audited


def _upstream_static_skip_for_testcase(
    testcase: ET.Element,
    source_path: Path,
    decorators: dict[str, list[dict[str, Any]]],
    junit_message: str,
) -> dict[str, Any] | None:
    raw_name = testcase.attrib.get("name", "")
    function_name = raw_name.split("[", 1)[0]
    classname = testcase.attrib.get("classname", "")
    if not function_name or not classname:
        return None
    candidates: list[dict[str, Any]] = []
    for qualified_name, audits in decorators.items():
        path_parts = qualified_name.split(".")
        if path_parts[-1] != function_name:
            continue
        class_path = ".".join(path_parts[:-1])
        expected_classname = source_path.stem
        if class_path:
            expected_classname += "." + class_path
        if classname != expected_classname and not classname.endswith(
            "." + expected_classname
        ):
            continue
        candidates.extend(
            audit for audit in audits if audit["reason"] == junit_message
        )
    return candidates[0] if len(candidates) == 1 else None


def junit_outcome_evidence(
    path: Path, source_path: Path | None = None
) -> dict[str, Any]:
    counts = {
        "collected": 0,
        "passed": 0,
        "failed": 0,
        "errors": 0,
        "skipped": 0,
        "expected_version_or_platform_deselected": 0,
        "expected_upstream_static_skipped": 0,
        "unexpected_skipped": 0,
        "xfailed": 0,
        "xpassed": 0,
    }
    evidence: dict[str, Any] = {
        "counts": counts,
        "junit_valid": False,
        "parse_error": "",
        "expected_exclusions": [],
        "expected_upstream_static_skips": [],
        "upstream_static_skip_source_error": "",
        "unexpected_skips": [],
        "xfails": [],
        "xpasses": [],
    }
    if not path.exists():
        evidence["parse_error"] = "junit_file_missing"
        return evidence
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as exc:
        evidence["parse_error"] = str(exc)
        return evidence
    evidence["junit_valid"] = True
    testcases = list(root.iter("testcase"))
    static_skip_decorators: dict[str, list[dict[str, Any]]] = {}
    has_pytest_skip = any(
        skipped is not None and skipped.attrib.get("type", "") != "pytest.xfail"
        for testcase in testcases
        for skipped in (testcase.find("skipped"),)
    )
    if source_path is not None and has_pytest_skip:
        try:
            static_skip_decorators = upstream_static_skip_decorators(source_path)
        except (OSError, SyntaxError, UnicodeError) as exc:
            evidence["upstream_static_skip_source_error"] = str(exc)
    counts["collected"] = len(testcases)
    for testcase in testcases:
        node_id = "::".join(
            value
            for value in (
                testcase.attrib.get("classname", ""),
                testcase.attrib.get("name", ""),
            )
            if value
        )
        failure = testcase.find("failure")
        if failure is not None:
            counts["failed"] += 1
            failure_message = failure.attrib.get("message", "") + (failure.text or "")
            if "XPASS" in failure_message.upper():
                counts["xpassed"] += 1
                evidence["xpasses"].append(
                    {"node_id": node_id, "reason": failure_message.strip()}
                )
        elif testcase.find("error") is not None:
            counts["errors"] += 1
        elif testcase.find("skipped") is not None:
            counts["skipped"] += 1
            skipped = testcase.find("skipped")
            assert skipped is not None
            skip_type = skipped.attrib.get("type", "")
            skip_message = skipped.attrib.get("message", "")
            if skip_type == "pytest.xfail":
                counts["xfailed"] += 1
                evidence["xfails"].append(
                    {"node_id": node_id, "reason": skip_message.strip()}
                )
            else:
                static_skip = (
                    _upstream_static_skip_for_testcase(
                        testcase,
                        source_path,
                        static_skip_decorators,
                        skip_message,
                    )
                    if source_path is not None
                    else None
                )
                if skip_type == "pytest.skip" and static_skip is not None:
                    counts["expected_upstream_static_skipped"] += 1
                    evidence["expected_upstream_static_skips"].append(
                        {
                            "node_id": node_id,
                            "reason": skip_message,
                            "source_line": static_skip["source_line"],
                        }
                    )
                    continue
                exclusion = re.fullmatch(
                    r"(?:Skipped:\s*)?Not for ([^\s]+)", skip_message.strip()
                )
                if exclusion is not None:
                    target = exclusion.group(1)
                    counts["expected_version_or_platform_deselected"] += 1
                    evidence["expected_exclusions"].append(
                        {
                            "node_id": node_id,
                            "reason": skip_message.strip(),
                            "excluded_target": target,
                            "exclusion_kind": (
                                "version" if target[0].isdigit() else "platform"
                            ),
                        }
                    )
                else:
                    counts["unexpected_skipped"] += 1
                    evidence["unexpected_skips"].append(
                        {"node_id": node_id, "reason": skip_message.strip()}
                    )
        else:
            counts["passed"] += 1
    return evidence


def junit_outcome_counts(path: Path) -> dict[str, int]:
    return junit_outcome_evidence(path)["counts"]


def pytest_deselected_count(output: str) -> int:
    return sum(int(value) for value in re.findall(r"\b(\d+) deselected\b", output))


def normalized(status: str,
               classification: str,
               command_id: str,
               output: str,
               diagnostic: str = "") -> dict[str, Any]:
    return {
        "schema_version": "scratchbird_reference_replay_normalized_result_v1",
        "status": status,
        "classification": classification,
        "command_id": command_id,
        "output_sha256": sha256_text(output),
        "diagnostic": diagnostic,
        "canonicalization": {
            "whitespace": "pytest_junit_and_tail_hash",
            "ordering": "pytest_reports_node_order",
            "nondeterministic_values": "timestamps_and_temp_paths_in_artifact_only",
        },
    }


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def skip(args: argparse.Namespace, reason: str) -> int:
    payload = {
        "schema_version": "scratchbird_firebird_qa_replay_gate_v1",
        "gate": "reference_firebird_qa_replay_gate",
        "status": "skipped",
        "timestamp_utc": utc_timestamp(),
        "family": "firebird",
        "run_mode": args.mode,
        "reason": reason,
        "normalized_result": normalized("skipped", reason, "firebird.qa.pytest", reason, reason),
        "authority_policy": "firebird_qa_drives_parser_listener_only_engine_mga_security_storage_authority",
    }
    write_json(args.evidence_file, payload)
    write_failure_ledger(args.failure_ledger_file, [], payload)
    print(f"reference_firebird_qa_replay_gate=skipped reason={reason}")
    return SKIP_RETURN_CODE


def module_available(module: str, extra_pythonpath: list[Path]) -> bool:
    env = dict(os.environ)
    if extra_pythonpath:
        env["PYTHONPATH"] = os.pathsep.join(str(path) for path in extra_pythonpath) + (
            os.pathsep + env["PYTHONPATH"] if env.get("PYTHONPATH") else ""
        )
    code = f"import {module}\n"
    proc = subprocess.run(
        [sys.executable, "-c", code],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        check=False,
    )
    return proc.returncode == 0


def local_python_dependency_roots(repo_root: Path) -> list[Path]:
    roots: list[Path] = []
    env_value = os.environ.get("SB_REFERENCE_FIREBIRD_PYTHONPATH")
    if env_value:
        roots.extend(Path(item) for item in env_value.split(os.pathsep) if item)
    roots.append(
        repo_root
        / "project/tests/reference_regression/reference_release_acquisition/firebird/5.0.4/regression/tools/python-deps"
    )
    return [root.resolve() for root in roots if root.is_dir()]


def dependency_issue(repo_root: Path, qa_root: Path) -> str | None:
    pythonpath = [qa_root / "src", *local_python_dependency_roots(repo_root)]
    if not module_available("pytest", pythonpath):
        return "pytest_not_present"
    qa_src = qa_root / "src"
    if not module_available("firebird.driver", pythonpath):
        return "firebird_driver_not_present"
    if not module_available("firebird.qa.plugin", [qa_src, *local_python_dependency_roots(repo_root)]):
        return "firebird_qa_plugin_not_present"
    return None


def write_driver_config(path: Path, port: int, fb_client_library: str | None) -> None:
    library_line = f"fb_client_library = {fb_client_library}\n" if fb_client_library else ""
    path.write_text(
        "[firebird.driver]\n"
        f"{library_line}"
        "servers = scratchbird_reference\n"
        "\n"
        "[firebird.server.defaults]\n"
        "host = localhost\n"
        f"port = {port}\n"
        "user = alice\n"
        "password = local_password\n"
        "\n"
        "[firebird.db.defaults]\n"
        "server = scratchbird_reference\n"
        "user = alice\n"
        "password = local_password\n"
        "\n"
        "[scratchbird_reference]\n"
        "host = localhost\n"
        f"port = {port}\n"
        "user = alice\n"
        "password = local_password\n"
        "\n",
        encoding="utf-8",
    )


def write_firebird_driver_runtime_patch(path: Path, port: int) -> None:
    path.write_text(
        "from firebird.driver.config import DriverConfig\n"
        "\n"
        "_scratchbird_original_register_server = DriverConfig.register_server\n"
        "\n"
        "def _scratchbird_register_server(self, name, config=None):\n"
        "    server = _scratchbird_original_register_server(self, name, config)\n"
        "    if server.host.value is None:\n"
        "        server.host.value = 'localhost'\n"
        "    if server.port.value is None:\n"
        f"        server.port.value = '{port}'\n"
        "    if server.user.value is None:\n"
        "        server.user.value = 'alice'\n"
        "    if server.password.value is None:\n"
        "        server.password.value = 'local_password'\n"
        "    return server\n"
        "\n"
        "DriverConfig.register_server = _scratchbird_register_server\n",
        encoding="utf-8",
    )


def selected_tests(qa_root: Path, repo_root: Path, mode: str) -> list[Path]:
    """Return targets without allowing an unlabeled reduction of release scope."""
    env_value = os.environ.get("SB_REFERENCE_FIREBIRD_QA_TESTS", "")
    if env_value:
        if mode != "diagnostic-subset":
            raise RuntimeError(
                "SB_REFERENCE_FIREBIRD_QA_TESTS is only valid with "
                "SB_REFERENCE_REPLAY_MODE=diagnostic-subset; canonical replay scope "
                "cannot be reduced"
            )
        raw_items = [item.strip() for item in env_value.split(os.pathsep) if item.strip()]
        tests: list[Path] = []
        for item in raw_items:
            path = Path(item)
            if not path.is_absolute():
                candidates = [
                    qa_root / path,
                    repo_root / path,
                    Path.cwd() / path,
                ]
                path = next((candidate for candidate in candidates if candidate.exists()), qa_root / path)
            tests.append(path)
        return tests

    if mode == "diagnostic-subset":
        raise RuntimeError(
            "diagnostic-subset mode requires SB_REFERENCE_FIREBIRD_QA_TESTS"
        )

    test_root = qa_root / "tests"
    if any(test_root.glob("**/*_test.py")) or any(test_root.glob("**/test_*.py")):
        return [test_root]

    raise RuntimeError(f"no Firebird QA test files found under {qa_root / 'tests'}")


def expand_test_cases(targets: list[Path]) -> list[Path]:
    cases: list[Path] = []
    for target in targets:
        if target.is_dir():
            found = sorted(
                {
                    *target.glob("**/*_test.py"),
                    *target.glob("**/test_*.py"),
                }
            )
            if not found:
                found = sorted(target.glob("**/*.py"))
            cases.extend(path for path in found if path.is_file())
        else:
            cases.append(target)
    return cases


def firebird_bin_dir(repo_root: Path) -> Path | None:
    env_value = os.environ.get("SB_REFERENCE_FIREBIRD_BIN_DIR")
    if env_value:
        return Path(env_value)
    isql = os.environ.get("SB_REFERENCE_FIREBIRD_ISQL")
    if isql:
        return Path(isql).parent
    for candidate in (
        repo_root / "build/reference/firebird-5.0.4-release-src/gen/Release/firebird/bin",
        repo_root / "project/tests/reference_regression/firebird/native_tool_harness/tools",
    ):
        if (candidate / "isql").is_file():
            return candidate
    lib_dir = os.environ.get("SB_REFERENCE_FIREBIRD_LIB_DIR")
    if lib_dir:
        candidate = Path(lib_dir).parent / "bin"
        if candidate.is_dir():
            return candidate
    return None


def firebird_tool_bin_dir(work: Path, real_bin_dir: Path | None, listener_port: int) -> Path | None:
    if real_bin_dir is None:
        return None
    wrapper_dir = work / "firebird_tool_wrappers"
    wrapper_dir.mkdir(parents=True, exist_ok=True)
    for tool in REQUIRED_FIREBIRD_TOOLS:
        real_tool = real_bin_dir / tool
        if not real_tool.is_file() or not os.access(real_tool, os.X_OK):
            continue
        wrapper = wrapper_dir / tool
        if wrapper.exists() or wrapper.is_symlink():
            wrapper.unlink()
        wrapper.write_text(
            "#!/usr/bin/env bash\n"
            "set -euo pipefail\n"
            f"real_tool={shlex_quote(str(real_tool))}\n"
            f"tool_name={shlex_quote(tool)}\n"
            f"listener_port={listener_port}\n"
            "args=()\n"
            "for arg in \"$@\"; do\n"
            "  case \"$arg\" in\n"
            "    127.0.0.1:*) args+=(\"127.0.0.1/${listener_port}:${arg#127.0.0.1:}\") ;;\n"
            "    localhost:*) args+=(\"localhost/${listener_port}:${arg#localhost:}\") ;;\n"
            "    *) args+=(\"$arg\") ;;\n"
            "  esac\n"
            "done\n"
            "if [[ \"$tool_name\" == \"isql\" && ! -t 0 ]]; then\n"
            "  tmp_input=\"$(mktemp)\"\n"
            "  perl -pe \"s#\\\\b(127\\\\.0\\\\.0\\\\.1|localhost):#\\$1/${listener_port}:#g\" > \"$tmp_input\"\n"
            "  set +e\n"
            "  \"$real_tool\" \"${args[@]}\" < \"$tmp_input\"\n"
            "  rc=$?\n"
            "  set -e\n"
            "  rm -f \"$tmp_input\"\n"
            "  exit \"$rc\"\n"
            "fi\n"
            "exec \"$real_tool\" \"${args[@]}\"\n",
            encoding="utf-8",
        )
        wrapper.chmod(0o755)
    return wrapper_dir


def firebird_authenticated_readiness_probe(
    isql: Path,
    lib_dir: Path | None,
    work: Path,
    port: int,
    timeout: float,
    *,
    run_fn: Any = subprocess.run,
) -> dict[str, Any]:
    """Prove the ready listener can authenticate and execute through genuine isql."""
    script = work / "firebird_authenticated_readiness.sql"
    output = work / "firebird_authenticated_readiness.out"
    script.write_text(
        "set bail on;\n"
        "select 1 as sb_qa_ready from rdb$database;\n"
        "quit;\n",
        encoding="utf-8",
    )
    command = [
        str(isql),
        "-q",
        "-user",
        "alice",
        "-password",
        "local_password",
        "-i",
        str(script),
        "-o",
        str(output),
        f"127.0.0.1/{port}:default",
    ]
    env = dict(os.environ)
    if lib_dir is not None:
        env["LD_LIBRARY_PATH"] = str(lib_dir) + (
            os.pathsep + env["LD_LIBRARY_PATH"]
            if env.get("LD_LIBRARY_PATH")
            else ""
        )
    try:
        completed = run_fn(
            command,
            cwd=work,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise smoke.OriginalToolStartupError(
            "scratchbird_firebird_authenticated_readiness",
            f"genuine isql readiness probe timed out after {timeout} seconds",
            returncode=None,
            stdout_tail=str(exc.output or ""),
            stderr_tail="",
        ) from exc
    except OSError as exc:
        raise smoke.OriginalToolStartupError(
            "scratchbird_firebird_authenticated_readiness",
            f"genuine isql readiness probe could not start: {exc}",
            returncode=None,
            stdout_tail="",
            stderr_tail="",
        ) from exc
    output_text = smoke.read_tail(output)
    combined = str(completed.stdout or "") + "\n" + output_text
    if completed.returncode != 0:
        raise smoke.OriginalToolStartupError(
            "scratchbird_firebird_authenticated_readiness",
            "genuine isql readiness probe failed authentication or execution",
            returncode=completed.returncode,
            stdout_tail=combined,
            stderr_tail="",
        )
    normalized = combined.upper()
    if "SB_QA_READY" not in normalized or "1" not in normalized:
        raise smoke.OriginalToolStartupError(
            "scratchbird_firebird_authenticated_readiness",
            "genuine isql readiness probe returned unexpected output",
            returncode=completed.returncode,
            stdout_tail=combined,
            stderr_tail="",
        )
    return {
        "status": "passed",
        "tool": str(isql),
        "tool_sha256": sha256_file(isql),
        "port": port,
        "command_id": "firebird.isql.authenticated_readiness",
        "output_sha256": hashlib.sha256(
            combined.encode("utf-8", errors="replace")
        ).hexdigest(),
    }


def shlex_quote(value: str) -> str:
    return "'" + value.replace("'", "'\"'\"'") + "'"


def executable_identity(path_value: str | Path, role: str) -> dict[str, Any]:
    requested = Path(path_value).expanduser()
    resolved = requested.resolve()
    is_file = resolved.is_file()
    executable = is_file and os.access(resolved, os.X_OK)
    return {
        "role": role,
        "requested_path": str(requested),
        "resolved_path": str(resolved),
        "is_file": is_file,
        "executable": executable,
        "size_bytes": resolved.stat().st_size if is_file else 0,
        "sha256": sha256_file(resolved) if is_file else "",
    }


def runtime_tool_identities(
    args: argparse.Namespace, real_bin_dir: Path | None
) -> dict[str, Any]:
    scratchbird = {
        "server": executable_identity(args.server, "scratchbird_server"),
        "listener": executable_identity(args.listener, "scratchbird_firebird_listener"),
        "parser_worker": executable_identity(
            args.parser_worker, "scratchbird_firebird_parser_worker"
        ),
    }
    firebird_tools = {
        tool: executable_identity(real_bin_dir / tool, f"firebird_{tool}")
        for tool in REQUIRED_FIREBIRD_TOOLS
    } if real_bin_dir is not None else {}
    complete = (
        all(item["executable"] for item in scratchbird.values())
        and set(firebird_tools) == set(REQUIRED_FIREBIRD_TOOLS)
        and all(item["executable"] for item in firebird_tools.values())
    )
    return {
        "identity_schema": "scratchbird_replay_tool_identity_v1",
        "identity_complete": complete,
        "scratchbird": scratchbird,
        "firebird_tools": firebird_tools,
        "firebird_tool_adapter": {
            "kind": "endpoint_syntax_rewrite_only",
            "fabricated_tool_output": False,
            "underlying_binaries": "firebird_tools",
        },
    }


def python_package_identities(env: dict[str, str]) -> dict[str, Any]:
    """Resolve package versions and source identities in the pytest environment."""
    code = r'''
import hashlib
import importlib
import importlib.metadata
import importlib.util
import json
from pathlib import Path

packages = {
    "pytest": ("pytest", "pytest", "pytest"),
    "firebird_qa": ("firebird.qa", "firebird-qa", "firebird.qa.__about__"),
    "firebird_driver": ("firebird.driver", "firebird-driver", "firebird.driver"),
}
result = {}
for key, (module_name, distribution_name, version_module) in packages.items():
    spec = importlib.util.find_spec(module_name)
    origin = "" if spec is None or spec.origin is None else spec.origin
    source = Path(origin).resolve() if origin else None
    try:
        version = importlib.metadata.version(distribution_name)
        version_source = "distribution_metadata"
    except importlib.metadata.PackageNotFoundError:
        version = str(getattr(importlib.import_module(version_module), "__version__", "unknown"))
        version_source = "module___version__"
    result[key] = {
        "module": module_name,
        "distribution": distribution_name,
        "version": version,
        "version_source": version_source,
        "origin": str(source) if source else "",
        "origin_sha256": (
            hashlib.sha256(source.read_bytes()).hexdigest()
            if source is not None and source.is_file()
            else ""
        ),
    }
print(json.dumps(result, sort_keys=True))
'''
    proc = subprocess.run(
        [sys.executable, "-c", code],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            "unable to identify pytest environment packages: " + proc.stderr[-1000:]
        )
    packages = json.loads(proc.stdout)
    packages["python"] = executable_identity(sys.executable, "pytest_python")
    return packages


def read_tail(path: Path, limit: int = 12000) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")[-limit:]


def classify_returncode(returncode: int, output: str) -> tuple[str, str]:
    if returncode == 0:
        return "passed", "semantic_probe_passed"
    if (
        returncode == 5
        and " deselected" in output
        and " failed" not in output
        and " error" not in output.lower()
    ):
        return "failed", "scope_reduction_by_version_deselection"
    if "unrecognized arguments" in output or "No module named" in output:
        return "failed", "environment_or_protocol_error"
    return "failed", "mapped_failure"


def run_case_process(
    cmd: list[str],
    *,
    cwd: Path,
    env: dict[str, str],
    timeout: int,
) -> subprocess.CompletedProcess[str]:
    """Run one isolated QA case and reap its complete tool process group."""
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = proc.communicate()
        raise subprocess.TimeoutExpired(
            cmd, timeout, output=stdout, stderr=stderr
        )
    return subprocess.CompletedProcess(cmd, proc.returncode, stdout, stderr)


class CaseStartupAttemptsExhausted(RuntimeError):
    def __init__(
        self,
        startup_error: smoke.OriginalToolStartupError,
        attempts: list[dict[str, Any]],
    ) -> None:
        self.startup_error = startup_error
        self.attempts = attempts
        super().__init__(str(startup_error))


def start_case_runtime_with_retry(
    args: argparse.Namespace,
    case_work: Path,
    *,
    sleep_fn: Any = time.sleep,
    readiness_probe_fn: Any | None = None,
) -> tuple[
    subprocess.Popen[bytes],
    dict[str, Any],
    subprocess.Popen[bytes],
    dict[str, Any],
    Path,
    list[dict[str, Any]],
]:
    """Start a fresh runtime, retrying only readiness/startup failures."""
    records: list[dict[str, Any]] = []
    for attempt in range(1, args.startup_attempts + 1):
        attempt_work = case_work / f"attempt_{attempt}"
        attempt_work.mkdir(parents=True, exist_ok=True)
        server: subprocess.Popen[bytes] | None = None
        listener: subprocess.Popen[bytes] | None = None
        started = time.monotonic()
        try:
            server, server_info = smoke.start_server(args, attempt_work)
            listener, listener_info = smoke.start_listener(args, attempt_work, server_info)
            readiness_evidence = (
                readiness_probe_fn(attempt_work, listener_info)
                if readiness_probe_fn is not None
                else None
            )
            records.append(
                {
                    "attempt": attempt,
                    "status": "ready",
                    "elapsed_seconds": round(time.monotonic() - started, 3),
                    "server_returncode_at_readiness": server.poll(),
                    "listener_returncode_at_readiness": listener.poll(),
                    "authenticated_readiness": readiness_evidence,
                }
            )
            return (
                server,
                server_info,
                listener,
                listener_info,
                attempt_work,
                records,
            )
        except smoke.OriginalToolStartupError as exc:
            smoke.stop_process(listener)
            smoke.stop_process(server)
            records.append(
                {
                    "attempt": attempt,
                    "status": "startup_failed",
                    "elapsed_seconds": round(time.monotonic() - started, 3),
                    **exc.evidence(),
                }
            )
            if attempt == args.startup_attempts:
                raise CaseStartupAttemptsExhausted(exc, records) from exc
            sleep_fn(args.startup_retry_delay)
    raise AssertionError("unreachable startup retry loop")


def failure_rows(payload: dict[str, Any]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for case in payload.get("case_results", []):
        if case.get("status") in {"passed", "skipped"}:
            continue
        digest = case.get("output_sha256", "")
        classification = case.get("classification", "run_failure")
        harness_failure = classification == "harness_startup_failure"
        rows.append(
            {
                "case_id": case.get("case_id", "firebird.qa.unknown_case"),
                "family_id": "firebird",
                "release_profile": "firebird_5_beta2_full",
                "tool_name": "pytest-firebird-qa",
                "tool_version": payload.get("pytest_version", "unknown"),
                "suite_path": payload.get("qa_root", ""),
                "input_digest": case.get("input_sha256", digest),
                "input_kind": "harness_startup" if harness_failure else "sql",
                "expected_digest": "not_applicable",
                "actual_digest": digest,
                "expected_summary": "Firebird QA original case executes through ScratchBird listener",
                "actual_summary": case.get("summary", ""),
                "scratchbird_diagnostic": case.get("diagnostic", "not_applicable"),
                "classification": classification,
                "owner": "test-infrastructure" if harness_failure else "parser",
                "fix_target": (
                    "project/tests/reference_regression/reference_original_tool_smoke.py"
                    if harness_failure
                    else "project/src/parsers/compatibility/firebird"
                ),
                "fix_commit": "",
                "rerun_status": "failed",
                "evidence_path": str(payload.get("evidence_file", "")),
                "public_status_required": "true",
                "notes": "Generated by reference_firebird_qa_replay_gate.",
            }
        )
    if rows or payload["status"] == "passed":
        return rows
    norm = payload["normalized_result"]
    digest = norm["output_sha256"]
    return [
        {
            "case_id": "firebird.qa.original_suite",
            "family_id": "firebird",
            "release_profile": "firebird_5_beta2_full",
            "tool_name": "pytest-firebird-qa",
            "tool_version": payload.get("pytest_version", "unknown"),
            "suite_path": payload.get("qa_root", ""),
            "input_digest": digest,
            "input_kind": "sql",
            "expected_digest": "not_applicable",
            "actual_digest": digest,
            "expected_summary": "Firebird QA original suite executes through ScratchBird listener",
            "actual_summary": payload.get("summary", ""),
            "scratchbird_diagnostic": norm.get("diagnostic", "not_applicable"),
            "classification": norm.get("classification", "run_failure"),
            "owner": "parser",
            "fix_target": "project/src/parsers/compatibility/firebird",
            "fix_commit": "",
            "rerun_status": "failed",
            "evidence_path": str(payload.get("evidence_file", "")),
            "public_status_required": "true",
            "notes": "Generated by reference_firebird_qa_replay_gate.",
        }
    ]


def write_failure_ledger(path: Path, rows: list[dict[str, str]], payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "case_id",
        "family_id",
        "release_profile",
        "tool_name",
        "tool_version",
        "suite_path",
        "input_digest",
        "input_kind",
        "expected_digest",
        "actual_digest",
        "expected_summary",
        "actual_summary",
        "scratchbird_diagnostic",
        "classification",
        "owner",
        "fix_target",
        "fix_commit",
        "rerun_status",
        "evidence_path",
        "public_status_required",
        "notes",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def run_pytest(args: argparse.Namespace, qa_root: Path, work: Path) -> dict[str, Any]:
    canonical_scope = load_canonical_scope()
    canonical_entries = canonical_scope["entries"]
    all_tests = expand_test_cases([qa_root / "tests"])
    full_manifest = discovery_manifest(qa_root, all_tests)
    if (
        full_manifest["case_file_count"] != canonical_scope["case_file_count"]
        or full_manifest["manifest_sha256"] != canonical_scope["manifest_sha256"]
    ):
        raise RuntimeError(
            "Firebird QA live discovery does not match the checked canonical scope: "
            f"expected_count={canonical_scope['case_file_count']} "
            f"actual_count={full_manifest['case_file_count']} "
            f"expected_hash={canonical_scope['manifest_sha256']} "
            f"actual_hash={full_manifest['manifest_sha256']}"
        )
    canonical_by_path = {entry["path"]: entry for entry in canonical_entries}
    selected = selected_tests(qa_root, args.repo_root.resolve(), args.mode)
    if args.mode == "diagnostic-subset":
        selected_files = sorted(set(expand_test_cases(selected)))
        if not selected_files:
            raise RuntimeError("diagnostic Firebird QA selection is empty")
        for test in selected_files:
            try:
                relative_path = test.resolve().relative_to(qa_root).as_posix()
            except ValueError as exc:
                raise RuntimeError(
                    f"diagnostic Firebird QA target is outside the acquired suite: {test}"
                ) from exc
            if relative_path not in canonical_by_path:
                raise RuntimeError(
                    f"diagnostic target is not a canonical Firebird QA test: {relative_path}"
                )
        scope_kind = "diagnostic_subset"
        canonical_scope_claimed = False
    else:
        selected_files = all_tests
        scope_kind = "canonical_full"
        canonical_scope_claimed = True

    shard_count = int(os.environ.get("SB_REFERENCE_FIREBIRD_QA_SHARD_COUNT", "1"))
    shard_index = int(os.environ.get("SB_REFERENCE_FIREBIRD_QA_SHARD_INDEX", "0"))
    canonical_shard_count = int(canonical_scope["shard_count"])
    if shard_count < 1 or shard_index < 0 or shard_index >= shard_count:
        raise RuntimeError(
            "invalid Firebird QA shard: "
            f"index={shard_index} count={shard_count}"
        )
    if args.mode == "diagnostic-subset" and (shard_count != 1 or shard_index != 0):
        raise RuntimeError("diagnostic-subset mode does not produce shard evidence")
    if args.mode != "diagnostic-subset" and shard_count not in {1, canonical_shard_count}:
        raise RuntimeError(
            "canonical Firebird QA replay supports monolithic count=1 or the "
            f"checked count={canonical_shard_count}; actual={shard_count}"
        )
    tests = [
        test for ordinal, test in enumerate(selected_files)
        if ordinal % shard_count == shard_index
    ]
    for test in tests:
        if not test.exists():
            raise RuntimeError(f"selected Firebird QA test does not exist: {test}")

    real_bin_dir = firebird_bin_dir(args.repo_root.resolve())
    if real_bin_dir is not None:
        real_bin_dir = real_bin_dir.resolve()
    tool_identities = runtime_tool_identities(args, real_bin_dir)
    if not tool_identities["identity_complete"]:
        raise RuntimeError(
            "required ScratchBird executables or genuine Firebird command-line tools "
            "are missing or non-executable"
        )
    fb_client_library = os.environ.get("SB_REFERENCE_FIREBIRD_CLIENT_LIBRARY")
    if not fb_client_library:
        lib_dir = os.environ.get("SB_REFERENCE_FIREBIRD_LIB_DIR")
        if lib_dir:
            candidate = Path(lib_dir) / "libfbclient.so.2"
            if candidate.exists():
                fb_client_library = str(candidate)
    if not fb_client_library and real_bin_dir is not None:
        candidate = real_bin_dir.parent / "lib" / "libfbclient.so.2"
        if candidate.exists():
            fb_client_library = str(candidate)
    readiness_lib_dir: Path | None = None
    configured_lib_dir = os.environ.get("SB_REFERENCE_FIREBIRD_LIB_DIR")
    if configured_lib_dir:
        readiness_lib_dir = Path(configured_lib_dir).resolve()
    elif real_bin_dir is not None and (real_bin_dir.parent / "lib").is_dir():
        readiness_lib_dir = (real_bin_dir.parent / "lib").resolve()

    def authenticated_readiness(
        attempt_work: Path, listener_info: dict[str, Any]
    ) -> dict[str, Any]:
        assert real_bin_dir is not None
        return firebird_authenticated_readiness_probe(
            real_bin_dir / "isql",
            readiness_lib_dir,
            attempt_work,
            int(listener_info["port"]),
            min(float(args.tool_timeout), float(args.startup_timeout)),
        )

    log_root = work / "firebird_qa_cases"
    log_root.mkdir(parents=True, exist_ok=True)
    base_env = dict(os.environ)
    inherited_pytest_controls = {
        name: base_env.pop(name, "")
        for name in ("PYTEST_ADDOPTS", "PYTEST_PLUGINS")
    }
    base_env["PYTEST_DISABLE_PLUGIN_AUTOLOAD"] = "1"
    pythonpath_roots = [qa_root / "src", *local_python_dependency_roots(args.repo_root.resolve())]
    base_env["PYTHONPATH"] = os.pathsep.join(str(path) for path in pythonpath_roots)
    lib_dir = os.environ.get("SB_REFERENCE_FIREBIRD_LIB_DIR")
    if lib_dir:
        base_env["LD_LIBRARY_PATH"] = lib_dir + (
            os.pathsep + base_env["LD_LIBRARY_PATH"] if base_env.get("LD_LIBRARY_PATH") else ""
        )
    elif real_bin_dir is not None and (real_bin_dir.parent / "lib").is_dir():
        derived_lib_dir = str(real_bin_dir.parent / "lib")
        base_env["LD_LIBRARY_PATH"] = derived_lib_dir + (
            os.pathsep + base_env["LD_LIBRARY_PATH"] if base_env.get("LD_LIBRARY_PATH") else ""
        )
    package_identities = python_package_identities(base_env)

    case_timeout = int(os.environ.get("SB_REFERENCE_FIREBIRD_QA_CASE_TIMEOUT", "120"))
    pytest_verbosity = (
        ["-vv"]
        if os.environ.get("SB_REFERENCE_FIREBIRD_QA_VERBOSE", "").lower()
        in {"1", "true", "yes", "on"}
        else ["-q"]
    )
    keep_passing_artifacts = os.environ.get(
        "SB_REFERENCE_FIREBIRD_QA_KEEP_PASSING_ARTIFACTS", ""
    ).lower() in {"1", "true", "yes", "on"}
    keep_failed_artifacts = os.environ.get(
        "SB_REFERENCE_FIREBIRD_QA_KEEP_FAILED_ARTIFACTS", ""
    ).lower() in {"1", "true", "yes", "on"}
    case_results: list[dict[str, Any]] = []
    total_output_parts: list[str] = []
    suite_start = time.monotonic()
    for index, test in enumerate(tests, start=1):
        try:
            rel = test.relative_to(qa_root)
        except ValueError:
            rel = Path(test.name)
        case_id = "firebird.qa." + str(rel).replace("/", ".").removesuffix(".py")
        safe_name = str(rel).replace("/", "__").replace("\\", "__")
        junit = log_root / f"{index:05d}_{safe_name}.xml"
        stdout = log_root / f"{index:05d}_{safe_name}.out"
        stderr = log_root / f"{index:05d}_{safe_name}.err"
        case_work = work / "c" / f"{index:05d}"
        case_work.mkdir(parents=True, exist_ok=True)
        server_info: dict[str, Any] = {}
        listener_info: dict[str, Any] = {}
        driver_config = case_work / "not_started_firebird-driver.conf"
        bin_dir: Path | None = None
        timeout_expired = False
        harness_startup_failed = False
        startup_attempt_records: list[dict[str, Any]] = []
        server = None
        listener = None
        try:
            (
                server,
                server_info,
                listener,
                listener_info,
                attempt_work,
                startup_attempt_records,
            ) = start_case_runtime_with_retry(
                args,
                case_work,
                readiness_probe_fn=authenticated_readiness,
            )
            driver_config = attempt_work / "firebird-driver.conf"
            write_driver_config(
                driver_config, int(listener_info["port"]), fb_client_library
            )
            write_firebird_driver_runtime_patch(
                attempt_work / "sitecustomize.py", int(listener_info["port"])
            )
            bin_dir = firebird_tool_bin_dir(
                attempt_work, real_bin_dir, int(listener_info["port"])
            )
            tool_args: list[str] = []
            if bin_dir is not None:
                tool_args = ["--bin-dir", str(bin_dir)]
            cmd = [
                sys.executable,
                "-m",
                "pytest",
                "-p",
                "firebird.qa.plugin",
                "--server",
                "scratchbird_reference",
                "--driver-config",
                str(driver_config),
                *tool_args,
                "--protocol",
                "inet",
                "--disable-db-cache",
                "--skip-deselected",
                "any",
                "--runslow",
                "--save-output",
                "--junitxml",
                str(junit),
                *pytest_verbosity,
                "-ra",
                "-o",
                "addopts=",
                "-o",
                "xfail_strict=true",
                "--tb=short",
                str(test),
            ]
            case_env = dict(base_env)
            case_env["PYTHONPATH"] = (
                str(attempt_work) + os.pathsep + case_env["PYTHONPATH"]
            )
            proc = run_case_process(
                cmd,
                cwd=qa_root,
                env=case_env,
                timeout=case_timeout,
            )
            stdout_text = proc.stdout
            stderr_text = proc.stderr
            returncode = proc.returncode
        except CaseStartupAttemptsExhausted as exc:
            startup_attempt_records = exc.attempts
            harness_startup_failed = True
            stdout_text = ""
            stderr_text = str(exc)
            returncode = 126
        except subprocess.TimeoutExpired as exc:
            timeout_expired = True
            stdout_text = exc.stdout or ""
            stderr_text = exc.stderr or ""
            if isinstance(stdout_text, bytes):
                stdout_text = stdout_text.decode("utf-8", errors="replace")
            if isinstance(stderr_text, bytes):
                stderr_text = stderr_text.decode("utf-8", errors="replace")
            returncode = 124
        except Exception as exc:  # noqa: BLE001 - keep per-case failure evidence.
            stdout_text = ""
            stderr_text = str(exc)
            returncode = 125
        finally:
            smoke.stop_process(listener)
            smoke.stop_process(server)

        stdout.write_text(stdout_text, encoding="utf-8", errors="replace")
        stderr.write_text(stderr_text, encoding="utf-8", errors="replace")
        junit_evidence = junit_outcome_evidence(junit, test)
        node_counts = junit_evidence["counts"]
        deselected_count = pytest_deselected_count(stdout_text + "\n" + stderr_text)
        combined = stdout_text + "\n" + stderr_text + "\n" + read_tail(junit)
        status, classification = classify_returncode(returncode, combined)
        if harness_startup_failed:
            status = "failed"
            classification = "harness_startup_failure"
        elif timeout_expired:
            status = "failed"
            classification = "resource_timeout"
        elif returncode == 125:
            status = "failed"
            classification = "environment_or_protocol_error"
        elif not junit_evidence["junit_valid"]:
            status = "failed"
            classification = "missing_or_invalid_junit_evidence"
        elif status == "passed" and node_counts["collected"] == 0:
            status = "failed"
            classification = "scope_reduction_zero_collected_nodes"
        elif deselected_count != 0:
            status = "failed"
            classification = "scope_reduction_pytest_deselection"
        elif node_counts["xpassed"] != 0:
            status = "failed"
            classification = "unexpected_xpass"
        elif (
            node_counts["unexpected_skipped"] != 0
            or node_counts["xfailed"] != 0
        ):
            status = "failed"
            classification = "scope_reduction_unexpected_skip_or_xfail"
        elif (
            node_counts["passed"]
            + node_counts["failed"]
            + node_counts["errors"]
            + node_counts["skipped"]
            != node_counts["collected"]
            or node_counts["expected_version_or_platform_deselected"]
            + node_counts["expected_upstream_static_skipped"]
            + node_counts["unexpected_skipped"]
            + node_counts["xfailed"]
            != node_counts["skipped"]
        ):
            status = "failed"
            classification = "invalid_junit_node_accounting"
        artifact_retention = "full_artifacts_kept"
        remove_artifacts = (status == "passed" and not keep_passing_artifacts) or (
            status != "passed" and not keep_failed_artifacts
        )
        if remove_artifacts:
            for artifact in (stdout, stderr, junit):
                try:
                    artifact.unlink(missing_ok=True)
                except OSError:
                    pass
            shutil.rmtree(case_work, ignore_errors=True)
            artifact_retention = (
                "passed_case_artifacts_removed_after_hash"
                if status == "passed"
                else "failed_case_artifacts_removed_after_diagnostic_hash"
            )
        canonical_entry = canonical_by_path[rel.as_posix()]
        expected_upstream_static_skips = [
            {"case_id": case_id, **audit}
            for audit in junit_evidence["expected_upstream_static_skips"]
        ]
        case_result = {
            "case_id": case_id,
            "path": str(test),
            "relative_path": rel.as_posix(),
            "manifest_ordinal": canonical_entry["ordinal"],
            "status": status,
            "classification": classification,
            "returncode": returncode,
            "node_outcomes": node_counts,
            "pytest_deselected_count": deselected_count,
            "expected_exclusions": junit_evidence["expected_exclusions"],
            "expected_upstream_static_skips": expected_upstream_static_skips,
            "upstream_static_skip_source_error": junit_evidence[
                "upstream_static_skip_source_error"
            ],
            "unexpected_skips": junit_evidence["unexpected_skips"],
            "xfails": junit_evidence["xfails"],
            "xpasses": junit_evidence["xpasses"],
            "junit_valid": junit_evidence["junit_valid"],
            "junit_parse_error": junit_evidence["parse_error"],
            "timeout_expired": timeout_expired,
            "harness_startup_failed": harness_startup_failed,
            "startup_attempts": startup_attempt_records,
            "startup_retry_count": max(0, len(startup_attempt_records) - 1),
            "stdout": str(stdout),
            "stderr": str(stderr),
            "junit": str(junit),
            "work_dir": str(case_work),
            "server": server_info,
            "listener": listener_info,
            "driver_config": str(driver_config),
            "firebird_bin_dir": str(bin_dir) if bin_dir is not None else "",
            "artifact_retention": artifact_retention,
            "input_sha256": canonical_entry["content_sha256"],
            "output_sha256": sha256_text(combined),
            "diagnostic": combined[-2000:],
            "summary": (stdout_text + stderr_text)[-1200:],
        }
        case_results.append(case_result)
        total_output_parts.append(f"{case_id} {status} {classification} rc={returncode}")

    failed_cases = [case for case in case_results if case["status"] == "failed"]
    harness_failure_classes = {
        "harness_startup_failure",
        "environment_or_protocol_error",
        "resource_timeout",
        "missing_or_invalid_junit_evidence",
        "invalid_junit_node_accounting",
    }
    harness_failed_cases = [
        case for case in failed_cases
        if case["classification"] in harness_failure_classes
    ]
    semantic_failed_cases = [
        case for case in failed_cases
        if case["classification"] in {"mapped_failure", "unexpected_xpass"}
    ]
    scope_policy_failed_cases = [
        case for case in failed_cases
        if case not in harness_failed_cases and case not in semantic_failed_cases
    ]
    startup_failure_event_count = sum(
        1
        for case in case_results
        for attempt in case["startup_attempts"]
        if attempt["status"] == "startup_failed"
    )
    startup_recovered_case_count = len([
        case for case in case_results
        if case["status"] == "passed" and case["startup_retry_count"] > 0
    ])
    status = "passed" if not failed_cases else "failed"
    classification = "semantic_suite_passed" if status == "passed" else "mapped_failures"
    combined = "\n".join(total_output_parts)
    aggregate_node_outcomes = {
        key: sum(case["node_outcomes"][key] for case in case_results)
        for key in (
            "collected",
            "passed",
            "failed",
            "errors",
            "skipped",
            "expected_version_or_platform_deselected",
            "expected_upstream_static_skipped",
            "unexpected_skipped",
            "xfailed",
            "xpassed",
        )
    }
    aggregate_deselected_count = sum(
        int(case["pytest_deselected_count"]) for case in case_results
    )
    assigned_scope_complete = (
        len(case_results) == len(tests)
        and len(case_results) != 0
        and aggregate_node_outcomes["collected"] != 0
        and aggregate_node_outcomes["unexpected_skipped"] == 0
        and aggregate_node_outcomes["xfailed"] == 0
        and aggregate_node_outcomes["xpassed"] == 0
        and aggregate_deselected_count == 0
        and not failed_cases
    )
    canonical_scope_evidence = {
        key: value for key, value in canonical_scope.items() if key != "entries"
    }
    if scope_kind == "diagnostic_subset":
        completion_claim = "diagnostic_subset_only"
    elif shard_count == 1 and assigned_scope_complete:
        completion_claim = "canonical_monolithic_scope_complete"
    elif shard_count == canonical_shard_count and assigned_scope_complete:
        completion_claim = "canonical_shard_evidence_requires_12_shard_aggregate"
    else:
        completion_claim = "no_completion_claim"
    return {
        "schema_version": "scratchbird_firebird_qa_replay_gate_v1",
        "gate": "reference_firebird_qa_replay_gate",
        "status": status,
        "timestamp_utc": utc_timestamp(),
        "family": "firebird",
        "run_mode": args.mode,
        "qa_root": str(qa_root),
        "work_dir": str(work),
        "case_isolation": {
            "mode": "fresh_scratchbird_server_database_per_original_firebird_case",
            "reason": "Firebird QA creates independent .fdb paths; ScratchBird replay must not leak catalog state across cases.",
        },
        "server": {"mode": "per_case", "details": "see case_results[].server"},
        "listener": {"mode": "per_case", "details": "see case_results[].listener"},
        "real_firebird_bin_dir": str(real_bin_dir) if real_bin_dir is not None else "",
        "selected_tests": [str(test) for test in selected],
        "scope_kind": scope_kind,
        "canonical_scope_claimed": canonical_scope_claimed,
        "completion_claim": completion_claim,
        "canonical_scope": canonical_scope_evidence,
        "full_discovery_manifest": full_manifest,
        "full_discovered_case_count": len(all_tests),
        "discovered_case_count": len(tests),
        "shard": {
            "index": shard_index,
            "count": shard_count,
            "assignment": "zero_based_manifest_ordinal_modulo_shard_count",
        },
        "passed_case_count": len([case for case in case_results if case["status"] == "passed"]),
        "skipped_case_count": 0,
        "failed_case_count": len(failed_cases),
        "semantic_failed_case_count": len(semantic_failed_cases),
        "harness_failed_case_count": len(harness_failed_cases),
        "scope_policy_failed_case_count": len(scope_policy_failed_cases),
        "startup_failure_event_count": startup_failure_event_count,
        "startup_recovered_case_count": startup_recovered_case_count,
        "node_outcomes": aggregate_node_outcomes,
        "expected_upstream_static_skipped": aggregate_node_outcomes[
            "expected_upstream_static_skipped"
        ],
        "pytest_deselected_count": aggregate_deselected_count,
        "expected_exclusions": [
            {"case_id": case["case_id"], **exclusion}
            for case in case_results
            for exclusion in case["expected_exclusions"]
        ],
        "expected_upstream_static_skips": [
            audit
            for case in case_results
            for audit in case["expected_upstream_static_skips"]
        ],
        "assigned_scope_complete": assigned_scope_complete,
        "scope_complete": (
            canonical_scope_claimed
            and shard_count == 1
            and assigned_scope_complete
        ),
        "tool_identities": tool_identities,
        "package_identities": package_identities,
        "pytest_version": package_identities["pytest"]["version"],
        "firebird_qa_version": package_identities["firebird_qa"]["version"],
        "firebird_driver_version": package_identities["firebird_driver"]["version"],
        "pytest_scope_controls": {
            "inherited_pytest_addopts_cleared": bool(
                inherited_pytest_controls["PYTEST_ADDOPTS"]
            ),
            "inherited_pytest_plugins_cleared": bool(
                inherited_pytest_controls["PYTEST_PLUGINS"]
            ),
            "plugin_autoload_disabled": True,
            "command_addopts_cleared": True,
            "exact_case_file_per_invocation": True,
        },
        "case_timeout_seconds": case_timeout,
        "startup_policy": {
            "readiness_timeout_seconds": args.startup_timeout,
            "maximum_attempts": args.startup_attempts,
            "retry_delay_seconds": args.startup_retry_delay,
            "retry_scope": "startup_failures_only_with_fresh_case_attempt_directory",
            "semantic_failure_count_excludes_harness_startup_failures": True,
        },
        "artifact_retention_policy": {
            "keep_passing_artifacts": keep_passing_artifacts,
            "keep_failed_artifacts": keep_failed_artifacts,
            "retained_in_result": "status classification hashes summary diagnostic_tail",
        },
        "elapsed_seconds": round(time.monotonic() - suite_start, 3),
        "case_results": case_results,
        "summary": "\n".join(total_output_parts[-200:]),
        "normalized_result": normalized(
            status,
            classification,
            "firebird.qa.pytest.original_suite",
            combined,
            "\n".join(total_output_parts[-50:]),
        ),
        "authority_policy": "firebird_qa_drives_parser_listener_only_engine_mga_security_storage_authority",
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--family", default="firebird")
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    parser.add_argument("--failure-ledger-file", required=True, type=Path)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--tool-timeout", type=int, default=1800)
    parser.add_argument(
        "--startup-timeout",
        type=float,
        default=float(
            os.environ.get("SB_REFERENCE_FIREBIRD_QA_STARTUP_TIMEOUT_SECONDS", "60")
        ),
    )
    parser.add_argument(
        "--startup-attempts",
        type=int,
        default=int(os.environ.get("SB_REFERENCE_FIREBIRD_QA_STARTUP_ATTEMPTS", "2")),
    )
    parser.add_argument(
        "--startup-retry-delay",
        type=float,
        default=float(
            os.environ.get("SB_REFERENCE_FIREBIRD_QA_STARTUP_RETRY_DELAY_SECONDS", "1")
        ),
    )
    args = parser.parse_args(argv)
    if args.startup_timeout <= 0:
        parser.error("--startup-timeout must be positive")
    if args.startup_attempts < 1 or args.startup_attempts > 3:
        parser.error("--startup-attempts must be between 1 and 3")
    if args.startup_retry_delay < 0 or args.startup_retry_delay > 30:
        parser.error("--startup-retry-delay must be between 0 and 30 seconds")

    args.mode = os.environ.get("SB_REFERENCE_REPLAY_MODE", DEFAULT_MODE)
    if args.mode == DEFAULT_MODE:
        return skip(args, "public_no_payload_mode")
    if args.mode not in VALID_RUN_MODES:
        print(f"unsupported SB_REFERENCE_REPLAY_MODE={args.mode}", file=sys.stderr)
        return 1
    if args.family != "firebird":
        return skip(args, "native_original_tool_replay_not_implemented_for_family")

    repo_root = args.repo_root.resolve()
    qa_root = Path(os.environ.get(
        "SB_REFERENCE_FIREBIRD_QA_ROOT",
        repo_root / "project/tests/reference_regression/firebird/original_firebird_qa",
    )).resolve()
    if not qa_root.is_dir():
        if args.mode == "release-mandatory":
            print(f"Firebird QA root is missing: {qa_root}", file=sys.stderr)
            return 1
        return skip(args, "firebird_original_suite_not_present")

    issue = dependency_issue(repo_root, qa_root)
    if issue:
        if args.mode == "release-mandatory":
            print(f"Firebird QA dependency missing: {issue}", file=sys.stderr)
            return 1
        return skip(args, issue)

    work = smoke.make_work_dir(args.work_root)
    try:
        payload = run_pytest(args, qa_root, work)
    except Exception as exc:  # noqa: BLE001 - preserve replay failure context.
        payload = {
            "schema_version": "scratchbird_firebird_qa_replay_gate_v1",
            "gate": "reference_firebird_qa_replay_gate",
            "status": "failed",
            "timestamp_utc": utc_timestamp(),
            "family": "firebird",
            "run_mode": args.mode,
            "qa_root": str(qa_root),
            "work_dir": str(work),
            "summary": str(exc),
            "normalized_result": normalized(
                "failed",
                "environment_or_protocol_error",
                "firebird.qa.pytest.original_suite",
                str(exc),
                str(exc),
            ),
            "authority_policy": "firebird_qa_drives_parser_listener_only_engine_mga_security_storage_authority",
        }
    payload["evidence_file"] = str(args.evidence_file)
    write_json(args.evidence_file, payload)
    write_failure_ledger(args.failure_ledger_file, failure_rows(payload), payload)
    print(
        "reference_firebird_qa_replay_gate="
        f"{payload['status']} evidence={args.evidence_file} ledger={args.failure_ledger_file}"
    )
    return 0 if payload["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
