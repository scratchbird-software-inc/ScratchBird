#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Classify UUID literals in the public project tree.

The gate is intentionally evidence-only. It inventories literals and verifies
that fixed engine identities are explicitly tied to an approved authority class;
it does not turn test fixtures, generated evidence, or release metadata into
durable engine authority.
"""

from __future__ import annotations

import argparse
import collections
import contextlib
import io
import fnmatch
import json
import pathlib
import re
import sys
import tempfile
from dataclasses import dataclass
from typing import Iterable


UUID_RE = re.compile(
    r"(?<![0-9A-Fa-f])"
    r"([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-"
    r"[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})"
    r"(?![0-9A-Fa-f])"
)

TEXT_SUFFIXES = {
    "",
    ".c",
    ".cc",
    ".cmake",
    ".cpp",
    ".csv",
    ".h",
    ".hpp",
    ".inc",
    ".json",
    ".md",
    ".py",
    ".txt",
    ".yaml",
    ".yml",
}

DOT_GIT_DIR = "." + "git"

SKIP_DIRS = {
    DOT_GIT_DIR,
    ".mypy_cache",
    ".pytest_cache",
    "__pycache__",
    "build",
    "cmake-build-debug",
    "cmake-build-release",
    "node_modules",
}

FIXED_AUTHORITIES = {
    "accepted_engine_authority",
    "builtin_registry",
    "codec",
    "package",
    "platform",
    "sentinel",
}

NON_AUTHORITY_CATEGORIES = {
    "documentation_fixture",
    "generated_surface_evidence",
    "negative_fixture",
    "parser_literal_fixture",
    "probe_fixture_identity",
    "release_evidence",
    "test_fixture_identity",
}

PROHIBITED_CATEGORIES = {"generated_sql_object_uuid"}

UUID_LITERAL_ASSIGNMENT_RE = re.compile(
    r"(?P<receiver>(?:[A-Za-z_][A-Za-z0-9_]*\.)?)(?P<field>"
    r"(?:requested_(?:table|index|column|constraint|domain|schema|sequence|"
    r"statistics|view|synonym|trigger|procedure|function)_uuid|"
    r"target_object\.uuid)"
    r"\.canonical)\s*=\s*\"(?P<literal>"
    r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-"
    r"[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})\""
)

UUID_LITERAL_FIELD_ARGUMENT_RE = re.compile(
    r"\"(?P<field>remap_uuid)\"\s*,\s*\"(?P<literal>"
    r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-"
    r"[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})\""
)

GENERATED_SQL_OBJECT_KIND_RE = re.compile(
    r"target_object\.object_kind\s*=\s*\""
    r"(?:table|index|column|constraint|domain|schema|sequence|statistics|"
    r"view|synonym|trigger|procedure|function|procedure_parameter|function_parameter)"
    r"\""
)

CREATE_SQL_OBJECT_REQUEST_RE = re.compile(
    r"EngineCreate(?:Schema|Table|Index|Domain|Sequence|View|Synonym|Trigger|"
    r"Procedure|Function|Statistics|Constraint)Request\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\b"
)


@dataclass(frozen=True)
class Occurrence:
    path: str
    line: int
    column: int
    literal: str
    context: str

    @property
    def key(self) -> str:
        return f"{self.path}:{self.line}:{self.column}:{self.literal}"


@dataclass(frozen=True)
class Rule:
    rule_id: str
    path_glob: str
    category: str
    authority: str
    fixed_identity: bool
    rationale: str
    evidence_only: bool = False
    literal: str | None = None
    literal_prefix: str | None = None
    context_regex: re.Pattern[str] | None = None
    exclude_context_regex: re.Pattern[str] | None = None

    @staticmethod
    def from_json(raw: dict[str, object]) -> "Rule":
        required = [
            "id",
            "path_glob",
            "category",
            "authority",
            "fixed_identity",
            "rationale",
        ]
        missing = [field for field in required if field not in raw]
        if missing:
            raise ValueError(f"allowlist rule missing {','.join(missing)}: {raw!r}")
        context = raw.get("context_regex")
        exclude_context = raw.get("exclude_context_regex")
        return Rule(
            rule_id=str(raw["id"]),
            path_glob=str(raw["path_glob"]),
            category=str(raw["category"]),
            authority=str(raw["authority"]),
            fixed_identity=bool(raw["fixed_identity"]),
            rationale=str(raw["rationale"]),
            evidence_only=bool(raw.get("evidence_only", False)),
            literal=str(raw["literal"]).lower() if "literal" in raw else None,
            literal_prefix=str(raw["literal_prefix"]).lower()
            if "literal_prefix" in raw
            else None,
            context_regex=re.compile(str(context)) if context is not None else None,
            exclude_context_regex=re.compile(str(exclude_context))
            if exclude_context is not None
            else None,
        )

    def matches(self, occurrence: Occurrence) -> bool:
        if not fnmatch.fnmatchcase(occurrence.path, self.path_glob):
            return False
        literal = occurrence.literal.lower()
        if self.literal is not None and literal != self.literal:
            return False
        if self.literal_prefix is not None and not literal.startswith(self.literal_prefix):
            return False
        if self.context_regex is not None and not self.context_regex.search(
            occurrence.context
        ):
            return False
        if self.exclude_context_regex is not None and self.exclude_context_regex.search(
            occurrence.context
        ):
            return False
        return True


@dataclass
class ClassifiedOccurrence:
    occurrence: Occurrence
    rule: Rule | None
    version: int
    variant: str
    engine_identity_shape: bool


def relpath(repo_root: pathlib.Path, path: pathlib.Path) -> str:
    return path.relative_to(repo_root).as_posix()


def iter_files(repo_root: pathlib.Path, roots: Iterable[pathlib.Path]) -> Iterable[pathlib.Path]:
    for root in roots:
        if root.is_file():
            yield root
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            parts = set(path.relative_to(repo_root).parts)
            if parts & SKIP_DIRS:
                continue
            if path.suffix.lower() not in TEXT_SUFFIXES:
                continue
            yield path


def scan_file(repo_root: pathlib.Path, path: pathlib.Path) -> list[Occurrence]:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return []
    found: list[Occurrence] = []
    for line_no, line in enumerate(text.splitlines(), start=1):
        for match in UUID_RE.finditer(line):
            found.append(
                Occurrence(
                    path=relpath(repo_root, path),
                    line=line_no,
                    column=match.start(1) + 1,
                    literal=match.group(1).lower(),
                    context=line.strip(),
                )
            )
    return found


def source_window(lines: list[str], index: int, radius: int = 5) -> str:
    start = max(0, index - radius)
    end = min(len(lines), index + radius + 1)
    return "\n".join(lines[start:end])


def generated_sql_object_binding_violations(
    repo_root: pathlib.Path,
    roots: Iterable[pathlib.Path],
) -> list[str]:
    errors: list[str] = []
    for path in iter_files(repo_root, roots):
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        lines = text.splitlines()
        for line_index, line in enumerate(lines):
            for match in UUID_LITERAL_ASSIGNMENT_RE.finditer(line):
                receiver = match.group("receiver").removesuffix(".")
                field = match.group("field")
                literal = match.group("literal").lower()
                requested_generated_object = field.startswith("requested_")
                window = source_window(lines, line_index)
                create_receivers = {
                    item.group("name")
                    for item in CREATE_SQL_OBJECT_REQUEST_RE.finditer(window)
                }
                target_generated_object = (
                    field == "target_object.uuid.canonical"
                    and receiver in create_receivers
                    and GENERATED_SQL_OBJECT_KIND_RE.search(
                        window
                    )
                )
                if not requested_generated_object and not target_generated_object:
                    continue
                errors.append(
                    "generated_sql_object_uuid_binding:"
                    f"{relpath(repo_root, path)}:{line_index + 1}:"
                    f"{match.start('literal') + 1}:{field}:{literal}"
                )
            for match in UUID_LITERAL_FIELD_ARGUMENT_RE.finditer(line):
                field = match.group("field")
                literal = match.group("literal").lower()
                errors.append(
                    "generated_sql_object_uuid_binding:"
                    f"{relpath(repo_root, path)}:{line_index + 1}:"
                    f"{match.start('literal') + 1}:{field}:{literal}"
                )
    return errors


def uuid_version(literal: str) -> int:
    try:
        return int(literal[14], 16)
    except ValueError:
        return -1


def uuid_variant(literal: str) -> str:
    try:
        nibble = int(literal[19], 16)
    except ValueError:
        return "invalid"
    if (nibble & 0b1000) == 0:
        return "ncs"
    if (nibble & 0b1100) == 0b1000:
        return "rfc4122"
    if (nibble & 0b1110) == 0b1100:
        return "microsoft"
    return "future"


def engine_identity_shape(literal: str) -> bool:
    return uuid_version(literal) == 7 and uuid_variant(literal) == "rfc4122"


def load_rules(path: pathlib.Path) -> list[Rule]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if raw.get("version") != 1:
        raise ValueError("allowlist version must be 1")
    rules_raw = raw.get("rules")
    if not isinstance(rules_raw, list):
        raise ValueError("allowlist rules must be an array")
    rules = [Rule.from_json(item) for item in rules_raw]
    seen: set[str] = set()
    for rule in rules:
        if rule.rule_id in seen:
            raise ValueError(f"duplicate allowlist rule id {rule.rule_id}")
        seen.add(rule.rule_id)
        if len(rule.rationale.split()) < 6:
            raise ValueError(f"allowlist rule {rule.rule_id} rationale is too short")
        if rule.fixed_identity:
            if rule.authority not in FIXED_AUTHORITIES:
                raise ValueError(
                    f"allowlist rule {rule.rule_id} has invalid fixed authority {rule.authority}"
                )
        elif not rule.evidence_only and rule.category not in PROHIBITED_CATEGORIES:
            raise ValueError(
                f"allowlist rule {rule.rule_id} must mark non-fixed identities evidence_only"
            )
    return rules


def classify(occurrences: list[Occurrence], rules: list[Rule]) -> list[ClassifiedOccurrence]:
    classified: list[ClassifiedOccurrence] = []
    for occurrence in occurrences:
        matched = next((rule for rule in rules if rule.matches(occurrence)), None)
        classified.append(
            ClassifiedOccurrence(
                occurrence=occurrence,
                rule=matched,
                version=uuid_version(occurrence.literal),
                variant=uuid_variant(occurrence.literal),
                engine_identity_shape=engine_identity_shape(occurrence.literal),
            )
        )
    return classified


def violations(classified: list[ClassifiedOccurrence]) -> list[str]:
    errors: list[str] = []
    for item in classified:
        occurrence = item.occurrence
        rule = item.rule
        if rule is None:
            errors.append(f"uncategorized:{occurrence.key}")
            continue
        if rule.category in PROHIBITED_CATEGORIES:
            errors.append(
                f"prohibited:{occurrence.key}:category={rule.category}:rule={rule.rule_id}"
            )
            continue
        if rule.fixed_identity:
            if rule.authority not in FIXED_AUTHORITIES:
                errors.append(
                    f"bad_fixed_authority:{occurrence.key}:authority={rule.authority}:rule={rule.rule_id}"
                )
            if not item.engine_identity_shape and rule.authority not in {"sentinel", "codec"}:
                errors.append(
                    f"fixed_identity_not_engine_v7:{occurrence.key}:version={item.version}:variant={item.variant}:rule={rule.rule_id}"
                )
        else:
            if rule.category not in NON_AUTHORITY_CATEGORIES:
                errors.append(
                    f"bad_non_authority_category:{occurrence.key}:category={rule.category}:rule={rule.rule_id}"
                )
            if not rule.evidence_only:
                errors.append(f"non_fixed_not_evidence_only:{occurrence.key}:rule={rule.rule_id}")
        if occurrence.path.startswith("project/src/") and not rule.fixed_identity:
            errors.append(f"source_uuid_must_be_fixed_identity:{occurrence.key}:rule={rule.rule_id}")
    return errors


def build_summary(classified: list[ClassifiedOccurrence]) -> dict[str, object]:
    by_category: collections.Counter[str] = collections.Counter()
    by_authority: collections.Counter[str] = collections.Counter()
    by_rule: collections.Counter[str] = collections.Counter()
    files: set[str] = set()
    for item in classified:
        files.add(item.occurrence.path)
        if item.rule is None:
            by_category["uncategorized"] += 1
            by_authority["uncategorized"] += 1
            by_rule["uncategorized"] += 1
        else:
            by_category[item.rule.category] += 1
            by_authority[item.rule.authority] += 1
            by_rule[item.rule.rule_id] += 1
    return {
        "literal_count": len(classified),
        "file_count": len(files),
        "by_category": dict(sorted(by_category.items())),
        "by_authority": dict(sorted(by_authority.items())),
        "by_rule": dict(sorted(by_rule.items())),
    }


def run_scan(
    repo_root: pathlib.Path,
    roots: list[pathlib.Path],
    allowlist_path: pathlib.Path,
    summary_json: pathlib.Path | None,
    enforce_generated_sql_object_bindings: bool = False,
) -> int:
    rules = load_rules(allowlist_path)
    occurrences: list[Occurrence] = []
    for path in iter_files(repo_root, roots):
        occurrences.extend(scan_file(repo_root, path))
    classified = classify(occurrences, rules)
    errors = violations(classified)
    if enforce_generated_sql_object_bindings:
        errors.extend(generated_sql_object_binding_violations(repo_root, roots))
    summary = build_summary(classified)
    summary["status"] = "passed" if not errors else "failed"
    summary["generated_sql_object_binding_gate"] = (
        "enforced" if enforce_generated_sql_object_bindings else "not_enforced"
    )
    summary["violations"] = errors[:200]
    if summary_json is not None:
        summary_json.parent.mkdir(parents=True, exist_ok=True)
        summary_json.write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    if errors:
        for error in errors[:50]:
            print(f"public_uuid_literal_classifier=fail:{error}", file=sys.stderr)
        if len(errors) > 50:
            print(
                f"public_uuid_literal_classifier=fail:additional_violations={len(errors) - 50}",
                file=sys.stderr,
            )
        return 1
    print(
        "public_uuid_literal_classifier=passed "
        f"literals={summary['literal_count']} files={summary['file_count']} "
        f"categories={len(summary['by_category'])}"
    )
    return 0


def write_json(path: pathlib.Path, data: object) -> None:
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run_self_test() -> int:
    def quiet_scan(*args: object) -> int:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(
            io.StringIO()
        ):
            return run_scan(*args)  # type: ignore[arg-type]

    with tempfile.TemporaryDirectory(prefix="sb_uuid_classifier_") as tmp:
        root = pathlib.Path(tmp)
        project = root / "project"
        sample = project / "src" / "sample.cpp"
        sample.parent.mkdir(parents=True)
        sample.write_text(
            'const char* kSentinel = "00000000-0000-7000-8000-000000000001";\n',
            encoding="utf-8",
        )
        allowlist = root / "allow.json"
        write_json(
            allowlist,
            {
                "version": 1,
                "rules": [
                    {
                        "id": "sentinel",
                        "path_glob": "project/src/**",
                        "category": "sentinel",
                        "authority": "sentinel",
                        "fixed_identity": True,
                        "rationale": "Self-test sentinel UUID proves fixed identity classification.",
                    }
                ],
            },
        )
        if quiet_scan(root, [project / "src"], allowlist, None) != 0:
            print("self_test_valid_fixture_failed", file=sys.stderr)
            return 1

        write_json(allowlist, {"version": 1, "rules": []})
        if quiet_scan(root, [project / "src"], allowlist, None) == 0:
            print("self_test_missing_rule_did_not_fail", file=sys.stderr)
            return 1

        write_json(
            allowlist,
            {
                "version": 1,
                "rules": [
                    {
                        "id": "prohibited",
                        "path_glob": "project/src/**",
                        "category": "generated_sql_object_uuid",
                        "authority": "none",
                        "fixed_identity": False,
                        "evidence_only": True,
                        "rationale": "Self-test generated SQL object literals must fail.",
                    }
                ],
            },
        )
        if quiet_scan(root, [project / "src"], allowlist, None) == 0:
            print("self_test_prohibited_rule_did_not_fail", file=sys.stderr)
            return 1

        source_file = project / "src" / "binding.cpp"
        source_file.write_text(
            "void ok() {\n"
            "  request.target_object.uuid.canonical = "
            "\"00000000-0000-7000-8000-000000000010\";\n"
            "  request.target_object.object_kind = \"policy\";\n"
            "}\n",
            encoding="utf-8",
        )
        if generated_sql_object_binding_violations(root, [project / "src"]):
            print("self_test_policy_target_binding_was_rejected", file=sys.stderr)
            return 1

        source_file.write_text(
            "void bad_requested() {\n"
            "  create.requested_table_uuid.canonical = "
            "\"00000000-0000-7000-8000-000000000011\";\n"
            "}\n",
            encoding="utf-8",
        )
        if not generated_sql_object_binding_violations(root, [project / "src"]):
            print("self_test_requested_table_uuid_binding_did_not_fail", file=sys.stderr)
            return 1

        source_file.write_text(
            "void bad_target() {\n"
            "  EngineCreateTableRequest request;\n"
            "  request.target_object.uuid.canonical = "
            "\"00000000-0000-7000-8000-000000000012\";\n"
            "  request.target_object.object_kind = \"table\";\n"
            "}\n",
            encoding="utf-8",
        )
        if not generated_sql_object_binding_violations(root, [project / "src"]):
            print("self_test_target_table_uuid_binding_did_not_fail", file=sys.stderr)
            return 1

        source_file.write_text(
            "void bad_remap() {\n"
            "  SetField(&row, \"remap_uuid\", "
            "\"00000000-0000-7000-8000-000000000013\");\n"
            "}\n",
            encoding="utf-8",
        )
        if not generated_sql_object_binding_violations(root, [project / "src"]):
            print("self_test_remap_uuid_literal_did_not_fail", file=sys.stderr)
            return 1
    print("public_uuid_literal_classifier_self_test=passed")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "roots",
        nargs="*",
        default=["project/src", "project/tests", "project/tools"],
        help="Repo-relative roots to scan.",
    )
    parser.add_argument(
        "--repo-root",
        default=".",
        help="Repository root. Defaults to current directory.",
    )
    parser.add_argument(
        "--allowlist",
        default="project/tools/release/public_uuid_literal_allowlist.json",
        help="Project-local UUID literal allowlist JSON.",
    )
    parser.add_argument("--summary-json", help="Optional summary JSON output path.")
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run classifier failure-mode self-tests before the live scan.",
    )
    parser.add_argument(
        "--enforce-generated-sql-object-bindings",
        action="store_true",
        help=(
            "Fail canonical UUID literals bound directly to generated SQL-object "
            "request fields in the scanned roots."
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = pathlib.Path(args.repo_root).resolve()
    allowlist = (repo_root / args.allowlist).resolve()
    roots = [
        (repo_root / root).resolve() if not pathlib.Path(root).is_absolute() else pathlib.Path(root)
        for root in args.roots
    ]
    if args.self_test and run_self_test() != 0:
        return 1
    summary_json = pathlib.Path(args.summary_json).resolve() if args.summary_json else None
    return run_scan(
        repo_root,
        roots,
        allowlist,
        summary_json,
        args.enforce_generated_sql_object_bindings,
    )


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
