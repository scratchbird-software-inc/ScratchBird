#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Scan public release inputs for secrets, credentials, local paths, and private endpoints."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import sys
from typing import Any, Iterable


# PUBLIC_SECRET_CREDENTIAL_SCAN
# PUBLIC_SECRET_SCAN_GATE

DOT_GIT = "." + "git"
PRIVATE_HOME_POSIX = "/" + "home" + "/" + "dcalford"
PRIVATE_USER_POSIX = "/" + "Users" + "/" + "dcalford"
PRIVATE_USER_WINDOWS = "Users" + "\\" + "dcalford"

PUBLIC_SCAN_ROOTS = (
    "project",
    "docs/build_requirements",
    "docs/legal",
    "release",
    "data",
    "LICENSE",
    "NOTICE",
    "LICENSES",
)

SKIP_DIRS = {
    "__pycache__",
    DOT_GIT,
    ".pytest_cache",
    ".mypy_cache",
    "CMakeFiles",
    "Testing",
    "build",
    "cmake-build-debug",
    "cmake-build-release",
    "node_modules",
    ".dart_tool",
    "target",
    "vendor",
}

SKIP_SUFFIXES = {
    ".a",
    ".dll",
    ".dylib",
    ".exe",
    ".jar",
    ".o",
    ".obj",
    ".pdf",
    ".png",
    ".pyc",
    ".sbdb",
    ".so",
    ".zip",
}

SELF_ALLOWLIST = {
    "project/tools/release/public_secret_credential_scan.py",
}

PRIVATE_LEGAL_WORKING_FILES = {
    "docs/legal/ScratchBird_legacy_poc_vs_private_cluster_boundary_audit.md",
}

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "ScratchBird" + "-Private",
    "local" + "_work/",
)

SECRET_PATTERNS: tuple[dict[str, Any], ...] = (
    {
        "category": "private_key",
        "pattern": re.compile(
            r"-----BEGIN (?:RSA |DSA |EC |OPENSSH |)?PRIVATE KEY-----"
        ),
    },
    {
        "category": "cloud_access_key",
        "pattern": re.compile(r"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b"),
    },
    {
        "category": "github_token",
        "pattern": re.compile(r"\bgh[pousr]_[A-Za-z0-9_]{20,}\b"),
    },
    {
        "category": "bearer_token",
        "pattern": re.compile(r"\bBearer\s+[A-Za-z0-9._~+/=-]{32,}\b"),
    },
    {
        "category": "credential_assignment",
        "pattern": re.compile(
            r"(?i)\b(?:password|passwd|api[_-]?key|secret[_-]?key|"
            r"access[_-]?token|signing[_-]?key|private[_-]?key)\b"
            r"\s*[:=]\s*[\"']([A-Za-z0-9._~+/=-]{16,})[\"']"
        ),
    },
)

LOCAL_PATH_PATTERNS = (
    re.compile(re.escape(PRIVATE_HOME_POSIX) + r"(?:/|\b)"),
    re.compile(re.escape(PRIVATE_USER_POSIX) + r"(?:/|\b)"),
    re.compile(r"\b[A-Z]:\\" + re.escape(PRIVATE_USER_WINDOWS) + r"(?:\\|\b)"),
)

PRIVATE_ENDPOINT_PATTERNS = (
    re.compile(
        r"https?://(?:10\.\d{1,3}\.\d{1,3}\.\d{1,3}|"
        r"192\.168\.\d{1,3}\.\d{1,3}|"
        r"172\.(?:1[6-9]|2\d|3[0-1])\.\d{1,3}\.\d{1,3}|"
        r"[A-Za-z0-9.-]+\.(?:internal|corp))"
        r"(?::\d+)?[^\s\"'<>]*"
    ),
)

PLACEHOLDER_WORDS = {
    "dummy",
    "example",
    "fixture",
    "placeholder",
    "public",
    "redacted",
    "sample",
    "test",
}

EVIDENCE_CHECKS: tuple[dict[str, Any], ...] = (
    {
        "surface": "public_export_private_reference_scan",
        "path": "project/tools/release/public_project_export_gate.py",
        "tokens": (
            "scan_private_references",
            "banned_needles",
            "GIT_REFERENCE_ALLOWLIST",
            "local_home_path_reference",
            "private_repo_reference",
        ),
    },
    {
        "surface": "reproducible_export_private_reference_scan",
        "path": "project/tools/release/public_reproducible_export.py",
        "tokens": (
            "PUBLIC_REPRODUCIBLE_EXPORT",
            "scan_private_references",
            "FORBIDDEN_REFERENCE_FRAGMENTS",
            "deterministic_two_pass_generation",
        ),
    },
    {
        "surface": "support_bundle_redaction_boundary",
        "path": "project/tools/release/public_support_bundle_incident_gate.py",
        "tokens": (
            "PUBLIC_SUPPORT_BUNDLE_INCIDENT_GATE",
            "redaction_applied",
            "forbidden_fields_absent",
            "OPS.SUPPORT_BUNDLE.PROTECTED_MATERIAL_FORBIDDEN",
        ),
    },
    {
        "surface": "release_gate_wiring",
        "path": "project/tests/release/CMakeLists.txt",
        "tokens": (
            "PUBLIC_SECRET_SCAN_GATE",
            "public_secret_scan_gate",
            "PCR-GATE-153",
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_secret_scan_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def read_text(repo_root: Path, relative_path: str) -> str:
    reject_private_reference(relative_path, "source_path")
    path = repo_root / relative_path
    require(path.is_file(), f"source_missing:{relative_path}")
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{relative_path}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{relative_path}:{exc}")


def iter_public_files(repo_root: Path) -> Iterable[Path]:
    for root_text in PUBLIC_SCAN_ROOTS:
        reject_private_reference(root_text, "scan_root")
        root = repo_root / root_text
        if not root.exists():
            continue
        if root.is_file():
            yield root
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [
                name
                for name in dirnames
                if name not in SKIP_DIRS and not name.startswith(DOT_GIT)
            ]
            for filename in filenames:
                path = Path(dirpath) / filename
                relative_path = path.resolve().relative_to(repo_root.resolve()).as_posix()
                if relative_path in PRIVATE_LEGAL_WORKING_FILES:
                    continue
                if path.suffix in SKIP_SUFFIXES:
                    continue
                yield path


def rel(path: Path, repo_root: Path) -> str:
    try:
        value = path.resolve().relative_to(repo_root.resolve()).as_posix()
    except ValueError:
        fail(f"path_outside_repo:{path.name}")
    reject_private_reference(value, "scan_path")
    return value


def looks_like_placeholder(value: str) -> bool:
    lowered = value.lower()
    return any(word in lowered for word in PLACEHOLDER_WORDS)


def looks_like_secret_value(value: str) -> bool:
    if looks_like_placeholder(value):
        return False
    if len(value) < 24:
        return False
    classes = sum(
        [
            any(ch.islower() for ch in value),
            any(ch.isupper() for ch in value),
            any(ch.isdigit() for ch in value),
            any(not ch.isalnum() for ch in value),
        ]
    )
    return classes >= 3


def record_finding(findings: list[dict[str, Any]], relative_path: str, category: str, match: str) -> None:
    findings.append(
        {
            "path": relative_path,
            "category": category,
            "match_sha256": sha256_text(match),
            "match_length": len(match),
        }
    )


def scan_text(relative_path: str, text: str) -> list[dict[str, Any]]:
    if relative_path in SELF_ALLOWLIST:
        return []
    findings: list[dict[str, Any]] = []
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in text:
            record_finding(findings, relative_path, "private_reference", fragment)
    for item in SECRET_PATTERNS:
        category = item["category"]
        pattern = item["pattern"]
        for match in pattern.finditer(text):
            value = match.group(1) if category == "credential_assignment" else match.group(0)
            if category == "credential_assignment" and not looks_like_secret_value(value):
                continue
            record_finding(findings, relative_path, category, match.group(0))
    for pattern in LOCAL_PATH_PATTERNS:
        for match in pattern.finditer(text):
            record_finding(findings, relative_path, "local_machine_path", match.group(0))
    for pattern in PRIVATE_ENDPOINT_PATTERNS:
        for match in pattern.finditer(text):
            record_finding(findings, relative_path, "private_endpoint", match.group(0))
    return findings


def validate_static_scan(repo_root: Path) -> dict[str, Any]:
    findings: list[dict[str, Any]] = []
    scanned_files = 0
    scanned_bytes = 0
    for path in iter_public_files(repo_root):
        relative_path = rel(path, repo_root)
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        scanned_files += 1
        scanned_bytes += len(text.encode("utf-8"))
        findings.extend(scan_text(relative_path, text))
    if findings:
        for finding in findings[:200]:
            print(
                f"{finding['path']}:{finding['category']}:"
                f"{finding['match_sha256']}:{finding['match_length']}",
                file=sys.stderr,
            )
        if len(findings) > 200:
            print(f"... {len(findings) - 200} additional findings omitted", file=sys.stderr)
        fail(f"static_scan_findings:{len(findings)}")
    return {
        "surface": "static_public_input_scan",
        "path": ";".join(PUBLIC_SCAN_ROOTS),
        "token_count": len(SECRET_PATTERNS) + len(LOCAL_PATH_PATTERNS) + len(PRIVATE_ENDPOINT_PATTERNS),
        "source_sha256": sha256_text(f"{scanned_files}:{scanned_bytes}"),
        "token_digest_sha256": sha256_text("secret_credential_local_path_private_endpoint\n"),
        "status": "pass",
        "scanned_files": scanned_files,
        "scanned_bytes": scanned_bytes,
    }


def validate_evidence_check(repo_root: Path, check: dict[str, Any]) -> dict[str, Any]:
    surface = check["surface"]
    path_text = check["path"]
    tokens = check["tokens"]
    require(isinstance(surface, str) and surface, "surface_invalid")
    require(isinstance(path_text, str) and path_text, f"path_invalid:{surface}")
    require(isinstance(tokens, tuple) and tokens, f"tokens_invalid:{surface}")
    text = read_text(repo_root, path_text)
    token_digests: list[str] = []
    for token in tokens:
        require(isinstance(token, str) and token, f"token_invalid:{surface}")
        if token not in text:
            fail(f"token_missing:{surface}:{path_text}:{token}")
        token_digests.append(sha256_text(token))
    return {
        "surface": surface,
        "path": path_text,
        "token_count": len(tokens),
        "source_sha256": sha256_text(text),
        "token_digest_sha256": sha256_text("\n".join(token_digests) + "\n"),
        "status": "pass",
    }


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "surface",
                "path",
                "token_count",
                "source_sha256",
                "token_digest_sha256",
                "status",
                "scanned_files",
                "scanned_bytes",
            ],
        )
        writer.writeheader()
        for record in records:
            writer.writerow(
                {
                    "surface": record["surface"],
                    "path": record["path"],
                    "token_count": record["token_count"],
                    "source_sha256": record["source_sha256"],
                    "token_digest_sha256": record["token_digest_sha256"],
                    "status": record["status"],
                    "scanned_files": record.get("scanned_files", ""),
                    "scanned_bytes": record.get("scanned_bytes", ""),
                }
            )


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    records = [validate_static_scan(repo_root)]
    records.extend(validate_evidence_check(repo_root, check) for check in EVIDENCE_CHECKS)
    matrix_text = "\n".join(
        f"{record['surface']},{record['path']},{record['status']}" for record in records
    ) + "\n"
    payload = {
        "gate": "PUBLIC_SECRET_SCAN_GATE",
        "marker": "PUBLIC_SECRET_CREDENTIAL_SCAN",
        "status": "pass",
        "checks": records,
        "check_count": len(records),
        "matrix_sha256": sha256_text(matrix_text),
        "scan_policy": {
            "private_keys_rejected": True,
            "cloud_tokens_rejected": True,
            "credential_assignments_rejected": True,
            "local_machine_paths_rejected": True,
            "private_endpoints_rejected": True,
            "public_release_inputs_only": True,
        },
    }
    write_csv(args.csv_output, records)
    write_json(args.evidence_output, payload)
    print(
        "public_secret_scan_gate=passed "
        f"checks={len(records)} matrix_sha256={payload['matrix_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
