#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Shared helpers for beta driver release gates.

Public release helpers do not infer private coordination directories. When a
controller needs external planning matrices, the caller must pass
``--workplan-root`` or set ``SB_DRIVER_BETA_WORKPLAN_ROOT`` explicitly.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any, Iterable


WORKPLAN_DIR_NAME = "beta-driver-tool-adapter-release-implementation-closure"
WORKPLAN_ROOT_ENV = "SB_DRIVER_BETA_WORKPLAN_ROOT"
DBEAVER_COMPONENT_ID = "adaptor:scratchbird-dbeaver-driver"
DBEAVER_NAME = "scratchbird-dbeaver-driver"

INCOMPLETE_STATUSES = {
    "",
    "blocked",
    "deferred",
    "fake_pass",
    "generated_only",
    "implemented_without_evidence",
    "in_progress",
    "not_started",
    "pending",
    "placeholder",
    "server_unspecified",
    "skeleton",
    "started",
    "stub",
    "todo",
    "undocumented_implementation",
    "waived",
}

CLOSING_STATUSES = {
    "accepted",
    "approved",
    "closed",
    "closed_with_proof",
    "complete",
    "completed",
    "done",
    "excluded",
    "implemented_and_proven",
    "not_applicable_with_citation",
    "pass",
    "passed",
    "proven",
    "separate_controller",
    "verified",
}

REQUIRED_MANIFEST_FIELDS = (
    "component_id",
    "category",
    "name",
    "driver_package_uuid",
    "driver_family",
    "driver_status",
    "api_surface_set",
    "ingress_mode_set",
    "wire_protocol_set",
    "dsn_key_set",
    "auth_method_set",
    "tls_profile_set",
    "type_mapping_profile",
    "diagnostic_mapping_profile",
    "metadata_profile",
    "thread_safety_class",
    "pooling_capability",
    "release_bucket",
    "conformance_profile_ref",
    "source_path",
)

FORBIDDEN_PRIVATE_FRAGMENTS = (
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
    "local" + "_work/",
)


def repo_root_from_release_tool(path: Path) -> Path:
    return path.resolve().parents[3]


def default_workplan_root(repo_root: Path) -> Path:
    env_value = os.environ.get(WORKPLAN_ROOT_ENV)
    if env_value:
        return Path(env_value).expanduser()
    return repo_root / "docs" / "contracts" / "driver-release-control" / WORKPLAN_DIR_NAME


def add_common_args(parser: argparse.ArgumentParser, script_path: Path) -> None:
    repo_root = repo_root_from_release_tool(script_path)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument("--workplan-root", type=Path)
    parser.add_argument("--output", type=Path)


def resolve_repo_root(value: Path) -> Path:
    return value.expanduser().resolve()


def resolve_workplan_root(repo_root: Path, value: Path | None) -> Path:
    return (value.expanduser() if value else default_workplan_root(repo_root)).resolve()


def default_report_path(repo_root: Path, name: str) -> Path:
    return repo_root / "build" / "reports" / name


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def load_manifest(repo_root: Path) -> list[dict[str, str]]:
    return read_csv(repo_root / "project" / "drivers" / "DriverPackageManifest.csv")


def load_workplan_csv(workplan_root: Path, filename: str) -> list[dict[str, str]]:
    return read_csv(workplan_root / filename)


def write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def fail(message: str) -> int:
    print(f"failed: {message}", file=sys.stderr)
    return 1


def status_value(row: dict[str, str]) -> str:
    return row.get("status", "").strip().lower()


def is_closing_status(status: str) -> bool:
    normalized = status.strip().lower()
    return normalized in CLOSING_STATUSES and normalized not in INCOMPLETE_STATUSES


def is_incomplete_status(status: str) -> bool:
    return status.strip().lower() in INCOMPLETE_STATUSES


def require_non_empty(row: dict[str, str], fields: Iterable[str], context: str) -> list[str]:
    issues: list[str] = []
    for field in fields:
        if not row.get(field, "").strip():
            issues.append(f"{context}:missing_{field}")
    return issues


def reject_recorded_private_path(value: str, context: str) -> list[str]:
    issues: list[str] = []
    if Path(value).is_absolute():
        issues.append(f"{context}:absolute_path:{value}")
    for fragment in FORBIDDEN_PRIVATE_FRAGMENTS:
        if fragment in value:
            issues.append(f"{context}:private_path_fragment:{value}")
    if ".." in Path(value).parts:
        issues.append(f"{context}:parent_path_escape:{value}")
    return issues


def unique_index(rows: Iterable[dict[str, str]], field: str, context: str) -> tuple[dict[str, dict[str, str]], list[str]]:
    index: dict[str, dict[str, str]] = {}
    issues: list[str] = []
    for row in rows:
        key = row.get(field, "").strip()
        if not key:
            issues.append(f"{context}:empty_{field}")
            continue
        if key in index:
            issues.append(f"{context}:duplicate_{field}:{key}")
            continue
        index[key] = row
    return index, issues


def in_scope_manifest_rows(rows: Iterable[dict[str, str]]) -> list[dict[str, str]]:
    return [row for row in rows if row.get("component_id", "").strip() != DBEAVER_COMPONENT_ID]


def component_label(row: dict[str, str]) -> str:
    return row.get("component_id", "").strip() or f"{row.get('category', '')}:{row.get('name', '')}"


def lane_tokens(value: str) -> set[str]:
    return {token.strip() for token in value.split(";") if token.strip()}


def map_expected_output(expected: str, output_root: Path, repo_root: Path) -> Path | None:
    cleaned = expected.strip()
    if not cleaned or cleaned.lower() == "none":
        return None
    path = Path(cleaned)
    if path.is_absolute():
        return path
    parts = path.parts
    if len(parts) >= 2 and parts[0] == "build" and parts[1] == "output":
        return output_root.joinpath(*parts[2:])
    return repo_root / path


def dbeaver_output_hits(output_root: Path) -> list[str]:
    if not output_root.exists():
        return []
    hits: list[str] = []
    for path in output_root.rglob("*"):
        if DBEAVER_NAME in path.as_posix():
            try:
                hits.append(path.relative_to(output_root).as_posix())
            except ValueError:
                hits.append(path.as_posix())
    return hits


def command_exists(command: str) -> bool:
    if command == "mojo":
        return mojo_launcher_exists()
    if command == "python3":
        return shutil.which(command) is not None or shutil.which("python") is not None
    return shutil.which(command) is not None


def command_version(command: str) -> str | None:
    if command == "mojo":
        launcher = mojo_launcher()
        if launcher and Path(launcher[0]).name == "pixi":
            return "pixi Mojo manifest available"
    executable = shutil.which(command)
    if executable is None and command == "python3":
        executable = shutil.which("python")
    if executable is None:
        return None
    version_args = {
        "go": ((executable, "version"),),
        "fpc": ((executable, "-iV"),),
    }.get(command, ((executable, "--version"), (executable, "-version")))
    for args in version_args:
        try:
            result = subprocess.run(
                args,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                check=False,
                timeout=5,
            )
        except (OSError, subprocess.TimeoutExpired):
            continue
        output = result.stdout.strip().splitlines()
        if output:
            return output[0]
    return executable


def mojo_launcher() -> list[str] | None:
    mojo_bin = os.environ.get("MOJO_BIN", "").strip()
    if mojo_bin:
        return [mojo_bin]
    mojo_path = shutil.which("mojo")
    if mojo_path:
        return [mojo_path]
    pixi_path = shutil.which("pixi")
    if not pixi_path:
        return None
    manifest = os.environ.get("MOJO_PIXI_MANIFEST", "").strip()
    if not manifest:
        manifest = str(Path.home() / "mojo-work" / "sb-mojo")
    manifest_path = Path(manifest).expanduser()
    if not manifest_path.exists():
        return None
    return [pixi_path, "run", "-m", str(manifest_path), "--executable", "mojo"]


def mojo_launcher_exists() -> bool:
    return mojo_launcher() is not None


def toolchain_text_has(text: str, needle: str) -> bool:
    if re.fullmatch(r"[a-z0-9]+", needle):
        return re.search(rf"\b{re.escape(needle)}\b", text) is not None
    return needle in text


def commands_for_toolchain(row: dict[str, str]) -> list[str]:
    text = (
        row.get("required_runtime", "")
        + " "
        + row.get("package_manager", "")
        + " "
        + row.get("minimum_version", "")
    ).lower()
    mapping: tuple[tuple[str, tuple[str, ...]], ...] = (
        ("cmake", ("cmake",)),
        ("ctest", ("ctest",)),
        ("c++ compiler", ("c++",)),
        ("jdk", ("java", "javac")),
        ("gradle", ("gradle",)),
        ("maven", ("mvn",)),
        (".net", ("dotnet",)),
        ("dotnet", ("dotnet",)),
        ("python", ("python3",)),
        ("pip", ("python3",)),
        ("python_build", ("python3",)),
        ("node.js", ("node",)),
        ("node", ("node",)),
        ("npm", ("npm",)),
        ("typescript", ("npm",)),
        ("go", ("go",)),
        ("rust", ("cargo",)),
        ("cargo", ("cargo",)),
        ("swift", ("swift",)),
        ("dart", ("dart",)),
        ("erlang", ("erl",)),
        ("elixir", ("elixir", "mix")),
        ("mix", ("mix",)),
        ("php", ("php",)),
        ("composer", ("composer",)),
        ("phpize", ("phpize",)),
        ("perl", ("perl",)),
        ("cpanm", ("cpanm",)),
        ("ruby", ("ruby", "gem")),
        ("ruby_gem", ("ruby", "gem")),
        ("r cmd check", ("R",)),
        ("rtools", ("R",)),
        ("julia", ("julia",)),
        ("mojo", ("mojo",)),
        ("freepascal", ("fpc",)),
        ("delphi", ("fpc",)),
        ("fpc", ("fpc",)),
        ("powerquery", ("powerquery-packager",)),
        ("power query", ("powerquery-packager",)),
        ("tableau", ("tableau-connector-packager",)),
    )
    commands: list[str] = []
    for needle, command_list in mapping:
        if toolchain_text_has(text, needle):
            for command in command_list:
                if command not in commands:
                    commands.append(command)
    if "contract_package" in text or "contract package" in text:
        commands = [
            command
            for command in commands
            if command not in {"cpanm", "powerquery-packager", "tableau-connector-packager"}
        ]
        if "python3" not in commands:
            commands.append("python3")
    return commands


def report_status(issues: list[str]) -> str:
    return "fail" if issues else "pass"


def print_report_result(label: str, report: dict[str, Any]) -> None:
    print(f"{label}={report['status']}")
    if report.get("issues"):
        for issue in report["issues"][:50]:
            print(f"- {issue}", file=sys.stderr)


def slug(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.:-]+", "_", value.strip())
