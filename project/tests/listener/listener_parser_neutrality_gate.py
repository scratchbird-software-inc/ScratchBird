#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Reject parser-family knowledge and parser implementation from SBgate."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import asdict, dataclass


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
GRAMMAR_SUFFIX_RE = re.compile(r"(?i)\b[A-Za-z0-9_.+-]+\.(?:g4|yy|ll|y|l)\b")
PARSE_ENTRYPOINT_RE = re.compile(r"\bParseStatement\s*\(")
PARSER_INCLUDE_RE = re.compile(
    r"(?im)^\s*#\s*include\s*[<\"][^>\"\n]*"
    r"(?:dialect|grammar|lexer|parser_pipeline|sql_parser)[^>\"\n]*[>\"]"
)
FAMILY_AUTH_ALIAS_RE = re.compile(
    r"\bSB_(?:COMPATIBILITY|REFERENCE)_(?!AUTH_)"
    r"[A-Z0-9]+_(?:PASSWORD|VERIFIER|PRINCIPAL_UUID|AUTH_TOKEN|CREDENTIAL)\b"
)
ADD_EXECUTABLE_RE = re.compile(
    r"(?is)\badd_executable\s*\(\s*([A-Za-z0-9_.:+${}-]+)"
)
AUTHORITY_WORD_RE = re.compile(
    r"(?i)(?:authenticate|authentication|authorization|credential|password|"
    r"verifier|principal_uuid|auth_token)"
)
STRING_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
PARSER_BINARY_NAME_RE = re.compile(r"(?i)\bsbp_[a-z0-9_]+\b")
COMPILED_PROFILE_REGISTRY_RE = re.compile(
    r"\b(?:ReferenceProtocolProfile|ReferenceProtocolProfiles|"
    r"FindReferenceProtocolProfile|ApplyProfileDefaults|"
    r"default_parser_package|default_wire_protocol|default_port)\b"
)
ENDPOINT_STRING_DEFAULT_RE = re.compile(
    r'\b(?:protocol_family|parser_executable|dialect|bind_address)\s*'
    r'(?:=|\{)\s*"[^"\n]+"'
)
PORT_DEFAULT_RE = re.compile(
    r"(?i)(?:\bport\s*(?:=|\{)\s*[1-9][0-9]*|"
    r"\b[A-Za-z_][A-Za-z0-9_]*default[A-Za-z0-9_]*port[A-Za-z0-9_]*"
    r"\s*(?:=|\{)\s*[1-9][0-9]*)"
)
GENERIC_PROTOCOL_NAMES = ("native", "sbsql")
GENERIC_CREDENTIAL_HANDOFF_NAMES = (
    "SB_COMPATIBILITY_AUTH_PASSWORD",
    "SB_COMPATIBILITY_AUTH_VERIFIER",
    "SB_COMPATIBILITY_AUTH_PRINCIPAL_UUID",
)
AMBIENT_CHILD_ENVIRONMENT_RE = re.compile(
    r"\b(?:GetEnvironmentStrings[AW]?|clearenv)\s*\(|::setenv\s*\("
)


@dataclass(frozen=True)
class Violation:
    rule: str
    path: str
    evidence: str


def read_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def compatibility_families(project_root: pathlib.Path) -> list[str]:
    root = project_root / "src" / "parsers" / "compatibility"
    if not root.is_dir():
        return []
    return sorted(
        child.name.lower()
        for child in root.iterdir()
        if child.is_dir() and child.name not in {"common"}
    )


def listener_files(listener_root: pathlib.Path) -> list[pathlib.Path]:
    return sorted(
        path
        for path in listener_root.rglob("*")
        if path.is_file()
        and (path.suffix.lower() in SOURCE_SUFFIXES or path.name == "CMakeLists.txt")
    )


def line_evidence(text: str, offset: int) -> str:
    start = text.rfind("\n", 0, offset) + 1
    end = text.find("\n", offset)
    if end < 0:
        end = len(text)
    return text[start:end].strip()[:240]


def family_auth_branch_violations(
    path: pathlib.Path, text: str, families: list[str], relative: str
) -> list[Violation]:
    violations: list[Violation] = []
    lowered = text.lower()
    for family in families:
        for match in re.finditer(rf"\b{re.escape(family)}\b", lowered):
            start = max(0, match.start() - 320)
            end = min(len(text), match.end() + 320)
            window = text[start:end]
            if AUTHORITY_WORD_RE.search(window) is None:
                continue
            violations.append(
                Violation(
                    "family_specific_auth_branch",
                    relative,
                    line_evidence(text, match.start()),
                )
            )
    return violations


def compiled_family_literal_violations(
    path: pathlib.Path, text: str, families: list[str], relative: str
) -> list[Violation]:
    del path
    tokens = sorted(set(families).union(GENERIC_PROTOCOL_NAMES), key=len, reverse=True)
    violations: list[Violation] = []
    for literal in STRING_LITERAL_RE.finditer(text):
        value = literal.group(0)[1:-1]
        for family in tokens:
            if re.search(rf"(?i)\b{re.escape(family)}\b", value) is None:
                continue
            violations.append(
                Violation(
                    "compiled_protocol_family_literal",
                    relative,
                    line_evidence(text, literal.start()),
                )
            )
            break
    return violations


def audit_project(project_root: pathlib.Path) -> list[Violation]:
    project_root = project_root.resolve()
    listener_root = project_root / "src" / "listener"
    if not listener_root.is_dir():
        return [
            Violation(
                "listener_source_root_missing",
                "src/listener",
                "generic listener source root is required",
            )
        ]

    families = compatibility_families(project_root)
    violations: list[Violation] = []
    for path in listener_files(listener_root):
        text = read_text(path)
        relative = path.relative_to(project_root).as_posix()
        for match in PARSE_ENTRYPOINT_RE.finditer(text):
            violations.append(
                Violation(
                    "listener_parser_entrypoint",
                    relative,
                    line_evidence(text, match.start()),
                )
            )
        for match in GRAMMAR_SUFFIX_RE.finditer(text):
            violations.append(
                Violation(
                    "listener_grammar_artifact",
                    relative,
                    line_evidence(text, match.start()),
                )
            )
        for match in PARSER_INCLUDE_RE.finditer(text):
            violations.append(
                Violation(
                    "listener_parser_include",
                    relative,
                    line_evidence(text, match.start()),
                )
            )
        for match in FAMILY_AUTH_ALIAS_RE.finditer(text):
            violations.append(
                Violation(
                    "family_specific_auth_alias",
                    relative,
                    line_evidence(text, match.start()),
                )
            )
        violations.extend(
            family_auth_branch_violations(path, text, families, relative)
        )
        violations.extend(
            compiled_family_literal_violations(path, text, families, relative)
        )
        for match in PARSER_BINARY_NAME_RE.finditer(text):
            violations.append(
                Violation(
                    "compiled_parser_executable",
                    relative,
                    line_evidence(text, match.start()),
                )
            )
        for match in COMPILED_PROFILE_REGISTRY_RE.finditer(text):
            violations.append(
                Violation(
                    "compiled_parser_profile_registry",
                    relative,
                    line_evidence(text, match.start()),
                )
            )
        for match in ENDPOINT_STRING_DEFAULT_RE.finditer(text):
            violations.append(
                Violation(
                    "compiled_endpoint_string_default",
                    relative,
                    line_evidence(text, match.start()),
                )
            )
        for match in PORT_DEFAULT_RE.finditer(text):
            violations.append(
                Violation(
                    "compiled_listener_port_default",
                    relative,
                    line_evidence(text, match.start()),
                )
            )

    parser_pool = listener_root / "parser_pool.cpp"
    if parser_pool.is_file():
        parser_pool_text = read_text(parser_pool)
        for match in AMBIENT_CHILD_ENVIRONMENT_RE.finditer(parser_pool_text):
            violations.append(
                Violation(
                    "ambient_parser_child_environment",
                    parser_pool.relative_to(project_root).as_posix(),
                    line_evidence(parser_pool_text, match.start()),
                )
            )
        if "::execve(" not in parser_pool_text:
            violations.append(
                Violation(
                    "closed_parser_child_exec_missing",
                    parser_pool.relative_to(project_root).as_posix(),
                    "POSIX parser launch must use execve with an explicit environment",
                )
            )
        for environment_name in GENERIC_CREDENTIAL_HANDOFF_NAMES:
            if environment_name not in parser_pool_text:
                violations.append(
                    Violation(
                        "generic_credential_handoff_missing",
                        parser_pool.relative_to(project_root).as_posix(),
                        environment_name,
                    )
                )

    executable_targets: list[tuple[str, str]] = []
    source_root = project_root / "src"
    for cmake in sorted(source_root.rglob("CMakeLists.txt")):
        text = read_text(cmake)
        relative = cmake.relative_to(project_root).as_posix()
        executable_targets.extend(
            (match.group(1), relative) for match in ADD_EXECUTABLE_RE.finditer(text)
        )
    listener_targets = [
        (target, path)
        for target, path in executable_targets
        if "listener" in target.lower()
    ]
    generic_targets = [row for row in listener_targets if row[0] == "sb_listener"]
    if len(generic_targets) != 1:
        violations.append(
            Violation(
                "listener_executable_count",
                "src/**/CMakeLists.txt",
                f"expected one sb_listener target, found {len(generic_targets)}",
            )
        )
    for target, path in listener_targets:
        if target != "sb_listener":
            violations.append(
                Violation(
                    "per_family_listener_executable",
                    path,
                    target,
                )
            )

    unique = {(row.rule, row.path, row.evidence): row for row in violations}
    return sorted(unique.values(), key=lambda row: (row.rule, row.path, row.evidence))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    violations = audit_project(args.project_root)
    payload = {
        "gate": "generic_listener_parser_neutrality",
        "status": "passed" if not violations else "failed",
        "generic_listener_executable": "sb_listener",
        "violations": [asdict(row) for row in violations],
    }
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0 if not violations else 1


if __name__ == "__main__":
    sys.exit(main())
