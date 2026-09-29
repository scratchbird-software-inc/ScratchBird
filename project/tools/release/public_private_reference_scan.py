#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import ast
import os
from pathlib import Path
import re


SKIP_DIRS = {
    "__pycache__",
    ".pytest_cache",
    "build",
    "cmake-build-debug",
    "cmake-build-release",
    "node_modules",
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
    ".so",
    ".zip",
}

SKIP_PATH_PREFIXES = (
    Path("tests/reference_regression/reference_release_acquisition"),
    Path("tests/reference_regression/firebird/original_firebird_qa"),
)

REFERENCE_REGRESSION_METADATA_ALLOWLIST = {
    Path("tests/reference_regression/acquire_reference_regression_assets.py"),
    Path("tests/reference_regression/reference_parser_gate_evidence_closure_gate.py"),
    Path("tests/reference_regression/reference_regression_acquisition_sources.csv"),
}

GIT_REFERENCE_ALLOWLIST = {
    Path("drivers/driver/cpp/include/nlohmann/json.hpp"),
    Path("drivers/tool/cli/include/nlohmann/json.hpp"),
    Path("drivers/driver/php/composer.lock"),
    Path("drivers/driver/mojo/README.md"),
    Path("drivers/driver/mojo/BASELINE_REQUIREMENT_MAPPING.md"),
    Path("drivers/driver/swift/Package.swift"),
    Path("drivers/driver/swift/Package.resolved"),
    Path("drivers/adaptor/scratchbird-metabase-driver/deps.edn"),
    Path(
        "tests/firebird_parser_worker/fixtures/full_firebirdsql_parser_udr_emulation_closure/"
        "artifacts/FIREBIRD_QA_CANDIDATE_ASSET_HASH_MANIFEST.csv"
    ),
    Path("resources/seed-packs/initial-resource-pack/resources/timezones/CONTRIBUTING"),
    Path("resources/seed-packs/initial-resource-pack/resources/timezones/NEWS"),
    Path("resources/seed-packs/initial-resource-pack/resources/timezones/theory.html"),
    Path("resources/seed-packs/initial-resource-pack/resources/timezones/tz-link.html"),
}

LABEL_REFERENCE_ALLOWLIST = {
    (
        "git_metadata_reference",
        Path("tests/reference_regression/acquire_reference_regression_assets.py"),
    ),
    (
        "git_metadata_reference",
        Path("tests/reference_regression/reference_regression_acquisition_sources.csv"),
    ),
    (
        "git_metadata_reference",
        Path("tools/release/github_actions_static_gate.py"),
    ),
    (
        "git_metadata_reference",
        Path("tools/release/public_packaging_history_gate.py"),
    ),
    (
        "private_execution_plan_reference",
        Path("tests/reference_regression/reference_parser_gate_evidence_closure_gate.py"),
    ),
}

GIT_METADATA_REFERENCE_RE = re.compile(
    r"(?<![A-Za-z0-9_])\." + r"git(?:modules\b|(?![A-Za-z0-9_]))"
)


def io_path(path: Path) -> str:
    text = str(path.resolve())
    if os.name != "nt":
        return text
    if text.startswith("\\\\?\\"):
        return text
    if text.startswith("\\\\"):
        return "\\\\?\\UNC\\" + text.lstrip("\\")
    return "\\\\?\\" + text


def normal_path(path: Path) -> Path:
    text = str(path)
    if os.name == "nt":
        if text.startswith("\\\\?\\UNC\\"):
            return Path("\\\\" + text.removeprefix("\\\\?\\UNC\\"))
        if text.startswith("\\\\?\\"):
            return Path(text.removeprefix("\\\\?\\"))
    return path


def allow_git_reference(rel: Path) -> bool:
    if rel.name == "." + "gitignore":
        return True
    return rel in GIT_REFERENCE_ALLOWLIST


def allow_labeled_reference(label: str, rel: Path) -> bool:
    if label == "git_metadata_reference" and allow_git_reference(rel):
        return True
    return (label, rel) in LABEL_REFERENCE_ALLOWLIST


def banned_needles() -> list[tuple[str, str]]:
    private_docs = "docs" + "/"
    return [
        ("private_execution_plan_reference", private_docs + "execution-plans"),
        ("private_completed_execution_plan_reference", private_docs + "completed-execution-plans"),
        ("private_findings_reference", private_docs + "findings"),
        ("git_metadata_reference", "." + "git"),
        ("local_home_path_reference", "/" + "home" + "/" + "dcalford"),
        ("private_repo_reference", "ScratchBird" + "-Private"),
        ("legacy_repo_runtime_reference", "local workspace" + "/" + "ScratchBird"),
    ]


def contains_banned_reference(label: str, needle: str, text: str) -> bool:
    if label == "git_metadata_reference":
        return GIT_METADATA_REFERENCE_RE.search(text) is not None
    return needle in text


def python_git_dependency_text(text: str) -> str:
    """Exclude only literal metadata-directory deny predicates, not file access.

    A positive ``any(part in {literal names} for part in path.parts)`` term
    that unconditionally continues the loop excludes those directories. It
    does not depend on their contents. Negations, conjunctions, filtered
    generators, arbitrary expressions and other occurrences remain scanned.
    """
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError):
        return text
    for node in ast.walk(tree):
        if ((isinstance(node, ast.Name) and node.id == "any" and isinstance(node.ctx, ast.Store)) or
                (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)) and node.name == "any") or
                (isinstance(node, ast.arg) and node.arg == "any") or
                (isinstance(node, ast.alias) and (node.asname or node.name) == "any")):
            return text
    encoded = text.encode("utf-8")
    offsets = [0]
    for line in encoded.splitlines(keepends=True):
        offsets.append(offsets[-1] + len(line))
    masked = bytearray(encoded)
    for branch in ast.walk(tree):
        if (not isinstance(branch, ast.If) or branch.orelse or
                len(branch.body) != 1 or not isinstance(branch.body[0], ast.Continue)):
            continue
        terms = branch.test.values if isinstance(branch.test, ast.BoolOp) and isinstance(branch.test.op, ast.Or) else [branch.test]
        for term in terms:
            if (not isinstance(term, ast.Call) or not isinstance(term.func, ast.Name) or
                    term.func.id != "any" or term.keywords or len(term.args) != 1 or
                    not isinstance(term.args[0], ast.GeneratorExp)):
                continue
            generator = term.args[0]
            if len(generator.generators) != 1:
                continue
            iteration = generator.generators[0]
            comparison = generator.elt
            if (iteration.ifs or iteration.is_async or not isinstance(iteration.target, ast.Name) or
                    not isinstance(iteration.iter, ast.Attribute) or iteration.iter.attr != "parts" or
                    not isinstance(comparison, ast.Compare) or len(comparison.ops) != 1 or
                    not isinstance(comparison.ops[0], ast.In) or
                    not isinstance(comparison.left, ast.Name) or comparison.left.id != iteration.target.id or
                    not isinstance(comparison.comparators[0], ast.Set)):
                continue
            literals = comparison.comparators[0].elts
            if not all(isinstance(value, ast.Constant) and isinstance(value.value, str) for value in literals):
                continue
            for value in literals:
                if value.value == "." + "git":
                    start = offsets[value.lineno - 1] + value.col_offset
                    end = offsets[value.end_lineno - 1] + value.end_col_offset
                    masked[start:end] = b" " * (end - start)
    return masked.decode("utf-8")


def iter_files(root: Path):
    walk_root = io_path(root) if os.name == "nt" else str(root)
    for dirpath, dirnames, filenames in os.walk(walk_root):
      dirnames[:] = [name for name in dirnames if name not in SKIP_DIRS]
      for filename in filenames:
          path = normal_path(Path(dirpath) / filename)
          rel = path.relative_to(root)
          if any(rel == prefix or prefix in rel.parents for prefix in SKIP_PATH_PREFIXES):
              continue
          if path.suffix in SKIP_SUFFIXES:
              continue
          yield path


def scan(root: Path) -> list[str]:
    findings: list[str] = []
    for path in iter_files(root):
        try:
            with open(io_path(path), "r", encoding="utf-8") as handle:
                text = handle.read()
        except UnicodeDecodeError:
            continue
        except FileNotFoundError:
            continue
        rel = path.relative_to(root)
        git_text = (python_git_dependency_text(text)
                    if path.suffix == ".py" and GIT_METADATA_REFERENCE_RE.search(text)
                    else text)
        for label, needle in banned_needles():
            if allow_labeled_reference(label, rel):
                continue
            candidate = git_text if label == "git_metadata_reference" else text
            if contains_banned_reference(label, needle, candidate):
                findings.append(f"{rel}: {label}: {needle}")
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()

    root = args.project_root.resolve()
    findings = scan(root)
    if findings:
        print("public private-reference scan failed")
        for finding in findings[:200]:
            print(finding)
        if len(findings) > 200:
            print(f"... {len(findings) - 200} additional findings omitted")
        return 1
    print("public private-reference scan passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
