#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static source/build/resource ownership gate for standalone parser families."""

from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import hashlib
import json
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Iterable


CPP_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ipp", ".tpp"
}
OWNED_FILE_SUFFIXES = CPP_SUFFIXES | {
    ".inc", ".def", ".g4", ".l", ".y", ".re2c", ".json", ".yaml", ".yml",
    ".toml", ".xml", ".csv", ".tbl", ".dat", ".manifest", ".sql", ".sbsql"
}
TEXT_SUFFIXES = OWNED_FILE_SUFFIXES | {".cmake", ".md", ".txt"}
INCLUDE_RE = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", re.MULTILINE)
CMAKE_FILE_TOKEN_RE = re.compile(
    r"(?P<path>[A-Za-z0-9_./${}:+-]+\.(?:c|cc|cpp|cxx|h|hh|hpp|hxx|ipp|tpp|inc|def|g4|l|y|re2c|json|ya?ml|toml|xml|csv|tbl|dat|manifest|sql|sbsql))\b",
    re.IGNORECASE,
)
SEMANTIC_POLICY_WORD_RE = re.compile(
    r"(?:semantic|profile|policy|diagnostic|datatype|identifier|constraint|"
    r"sequence|transaction|session|catalog|optimizer|statistics|lock|ddl|dml)",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class RootClaim:
    owner: str | None
    path: str
    neutral: bool = False


@dataclass(frozen=True)
class Violation:
    kind: str
    diagnostic: str
    path: str
    line: int
    owner: str
    foreign_owner: str
    evidence: str

    def to_json(self) -> dict[str, object]:
        return {
            "kind": self.kind,
            "diagnostic": self.diagnostic,
            "path": self.path,
            "line": self.line,
            "owner": self.owner,
            "foreign_owner": self.foreign_owner,
            "evidence": self.evidence,
        }


@dataclass
class ScanResult:
    violations: list[Violation]
    files_scanned: int
    family_files: dict[str, int]
    neutral_files: int
    source_scan_hash: str


def _normalize_rel(value: str) -> str:
    normalized = pathlib.PurePosixPath(value.replace("\\", "/")).as_posix()
    while normalized.startswith("./"):
        normalized = normalized[2:]
    return normalized.rstrip("/")


def _line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _read_text(path: pathlib.Path) -> str | None:
    if path.name != "CMakeLists.txt" and path.suffix.lower() not in TEXT_SUFFIXES:
        return None
    try:
        if path.stat().st_size > 8 * 1024 * 1024:
            return None
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return None


def _strip_cpp_comments(text: str, *, strip_literals: bool) -> str:
    output: list[str] = []
    index = 0
    state = "code"
    quote = ""
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == "/" and next_char == "/":
                output.extend("  ")
                index += 2
                state = "line_comment"
                continue
            if char == "/" and next_char == "*":
                output.extend("  ")
                index += 2
                state = "block_comment"
                continue
            if char in {'"', "'"}:
                quote = char
                output.append(" " if strip_literals else char)
                index += 1
                state = "literal"
                continue
            output.append(char)
            index += 1
            continue
        if state == "line_comment":
            output.append("\n" if char == "\n" else " ")
            index += 1
            if char == "\n":
                state = "code"
            continue
        if state == "block_comment":
            if char == "*" and next_char == "/":
                output.extend("  ")
                index += 2
                state = "code"
            else:
                output.append("\n" if char == "\n" else " ")
                index += 1
            continue
        if state == "literal":
            if char == "\\" and next_char:
                if strip_literals:
                    output.extend("  ")
                else:
                    output.extend((char, next_char))
                index += 2
                continue
            output.append(" " if strip_literals and char != "\n" else char)
            index += 1
            if char == quote:
                state = "code"
    return "".join(output)


def _cmake_without_comments(text: str) -> str:
    # Keep byte offsets stable enough for useful line reporting.
    output: list[str] = []
    for line in text.splitlines(keepends=True):
        quote = False
        escaped = False
        comment_at: int | None = None
        for index, char in enumerate(line):
            if char == "#" and not quote:
                comment_at = index
                break
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quote = not quote
        if comment_at is None:
            output.append(line)
        else:
            ending = "\n" if line.endswith("\n") else ""
            output.append(line[:comment_at] + " " * (len(line) - comment_at - len(ending)) + ending)
    return "".join(output)


class OwnershipRegistry:
    def __init__(self, repo_root: pathlib.Path, data: dict[str, object]):
        self.repo_root = repo_root
        self.data = data
        family_data = data.get("families")
        if not isinstance(family_data, dict) or not family_data:
            raise ValueError("ownership map must define non-empty families")
        self.families: dict[str, dict[str, object]] = {}
        self.claims: list[RootClaim] = []
        for family, raw in sorted(family_data.items()):
            if not isinstance(raw, dict):
                raise ValueError(f"family entry is not an object: {family}")
            roots = raw.get("roots")
            if not isinstance(roots, list) or not roots:
                raise ValueError(f"family has no roots: {family}")
            normalized_roots = [_normalize_rel(str(root)) for root in roots]
            namespace_stems = raw.get("namespace_stems", [family])
            target_prefixes = raw.get(
                "target_prefixes",
                [
                    f"sbl_{family}_parser",
                    f"sbp_{family}",
                    f"sbu_{family}_parser",
                    f"sbup_{family}",
                ],
            )
            entry = dict(raw)
            entry["roots"] = normalized_roots
            entry["namespace_stems"] = [str(item) for item in namespace_stems]
            entry["target_prefixes"] = [str(item) for item in target_prefixes]
            self.families[family] = entry
            self.claims.extend(RootClaim(family, root) for root in normalized_roots)
        neutral_roots = data.get("neutral_roots", [])
        if not isinstance(neutral_roots, list):
            raise ValueError("neutral_roots must be an array")
        self.neutral_roots = [_normalize_rel(str(root)) for root in neutral_roots]
        self.claims.extend(RootClaim(None, root, True) for root in self.neutral_roots)
        strict_families = data.get("strict_standalone_families", [])
        if not isinstance(strict_families, list):
            raise ValueError("strict_standalone_families must be an array")
        self.strict_standalone_families = {str(item) for item in strict_families}
        unknown_strict = self.strict_standalone_families - set(self.families)
        if unknown_strict:
            raise ValueError(
                f"strict standalone families are not registered: {sorted(unknown_strict)}"
            )
        legacy_roots = data.get("legacy_shared_parser_roots", [])
        if not isinstance(legacy_roots, list):
            raise ValueError("legacy_shared_parser_roots must be an array")
        self.legacy_shared_parser_roots = {
            _normalize_rel(str(root)) for root in legacy_roots
        }
        legacy_targets = data.get("legacy_shared_parser_targets", [])
        if not isinstance(legacy_targets, list):
            raise ValueError("legacy_shared_parser_targets must be an array")
        self.legacy_shared_parser_targets = {str(target) for target in legacy_targets}
        forbidden_neutral_targets = data.get("forbidden_neutral_parser_targets", [])
        if not isinstance(forbidden_neutral_targets, list):
            raise ValueError("forbidden_neutral_parser_targets must be an array")
        self.forbidden_neutral_parser_targets = {
            str(target) for target in forbidden_neutral_targets
        }
        misclassified = self.legacy_shared_parser_roots & set(self.neutral_roots)
        if misclassified:
            raise ValueError(
                "legacy shared parser roots cannot be family-neutral: "
                f"{sorted(misclassified)}"
            )
        for root in sorted(self.legacy_shared_parser_roots):
            if not (repo_root / root).exists():
                raise ValueError(f"legacy shared parser root is missing: {root}")
        self.claims.sort(key=lambda claim: (-len(claim.path), claim.path))
        self._validate_claims()

    @classmethod
    def load(cls, repo_root: pathlib.Path, map_path: pathlib.Path) -> "OwnershipRegistry":
        return cls(repo_root, json.loads(map_path.read_text(encoding="utf-8")))

    def _validate_claims(self) -> None:
        seen: dict[str, RootClaim] = {}
        for claim in self.claims:
            if claim.path in seen:
                raise ValueError(f"duplicate ownership root: {claim.path}")
            seen[claim.path] = claim
            root = self.repo_root / claim.path
            if not root.is_dir():
                raise ValueError(f"ownership root is missing: {claim.path}")
        for index, claim in enumerate(self.claims):
            for other in self.claims[index + 1:]:
                if claim.path.startswith(other.path + "/") and claim.owner != other.owner:
                    raise ValueError(
                        f"ownership roots overlap across owners: {claim.path} and {other.path}"
                    )

    def owner_for_path(self, rel_path: str) -> RootClaim | None:
        rel_path = _normalize_rel(rel_path)
        for claim in self.claims:
            if rel_path == claim.path or rel_path.startswith(claim.path + "/"):
                return claim
        return None

    def family_namespaces(self, family: str) -> list[str]:
        stems = self.families[family]["namespace_stems"]
        assert isinstance(stems, list)
        namespaces: list[str] = []
        for stem in stems:
            namespaces.extend(
                [
                    f"scratchbird::parser::{stem}",
                    f"scratchbird::udr::{stem}_parser_support",
                ]
            )
        return namespaces

    def target_prefixes(self, family: str) -> list[str]:
        prefixes = self.families[family]["target_prefixes"]
        assert isinstance(prefixes, list)
        return prefixes

    def target_owner(self, token: str) -> str | None:
        matches: list[tuple[int, str]] = []
        for family in self.families:
            for prefix in self.target_prefixes(family):
                if token.startswith(prefix):
                    matches.append((len(prefix), family))
        if not matches:
            return None
        matches.sort(reverse=True)
        return matches[0][1]

    def path_fragments(self, family: str) -> list[str]:
        roots = self.families[family]["roots"]
        assert isinstance(roots, list)
        fragments: set[str] = set()
        for root in roots:
            fragments.add(root)
            if root.startswith("project/"):
                fragments.add(root[len("project/"):])
        return sorted(fragments, key=lambda item: (-len(item), item))


def _walk_claim_files(registry: OwnershipRegistry) -> list[tuple[RootClaim, pathlib.Path, str]]:
    files: list[tuple[RootClaim, pathlib.Path, str]] = []
    visited: set[str] = set()
    for claim in registry.claims:
        root = registry.repo_root / claim.path
        for path in sorted(root.rglob("*")):
            if not path.is_file() and not path.is_symlink():
                continue
            rel = path.relative_to(registry.repo_root).as_posix()
            effective = registry.owner_for_path(rel)
            if effective != claim or rel in visited:
                continue
            visited.add(rel)
            files.append((claim, path, rel))
    return files


def _discovery_violations(registry: OwnershipRegistry) -> list[Violation]:
    discovery = registry.data.get("discovery", {})
    if not isinstance(discovery, dict):
        raise ValueError("discovery must be an object")
    violations: list[Violation] = []
    compatibility_root = discovery.get("compatibility_root")
    if compatibility_root:
        root = registry.repo_root / str(compatibility_root)
        exclusions = {str(item) for item in discovery.get("compatibility_exclusions", [])}
        discovered = {
            path.name for path in root.iterdir() if path.is_dir() and path.name not in exclusions
        }
        registered = {
            family
            for family, entry in registry.families.items()
            if _normalize_rel(f"{compatibility_root}/{family}") in entry["roots"]
        }
        for family in sorted(discovered ^ registered):
            violations.append(
                Violation(
                    "unregistered_parser_family",
                    "PARSER.PACKAGE.NON_STANDALONE",
                    _normalize_rel(str(compatibility_root)),
                    1,
                    "registry",
                    family,
                    "compatibility parser directory and ownership registry disagree",
                )
            )
    udr_root = discovery.get("udr_root")
    udr_glob = discovery.get("udr_glob")
    if udr_root and udr_glob:
        root = registry.repo_root / str(udr_root)
        discovered = {path.name for path in root.iterdir() if path.is_dir() and fnmatch.fnmatch(path.name, str(udr_glob))}
        registered = {
            pathlib.PurePosixPath(root_path).name
            for entry in registry.families.values()
            for root_path in entry["roots"]
            if pathlib.PurePosixPath(root_path).parent.as_posix() == _normalize_rel(str(udr_root))
            and fnmatch.fnmatch(pathlib.PurePosixPath(root_path).name, str(udr_glob))
        }
        for package in sorted(discovered ^ registered):
            violations.append(
                Violation(
                    "unregistered_parser_udr",
                    "PARSER.UDR.FAMILY_MISMATCH",
                    _normalize_rel(str(udr_root)),
                    1,
                    "registry",
                    package,
                    "parser-support UDR directory and ownership registry disagree",
                )
            )
    return violations


def _foreign_families(registry: OwnershipRegistry, claim: RootClaim) -> Iterable[str]:
    for family in registry.families:
        if claim.owner != family:
            yield family


def _make_violation(
    claim: RootClaim,
    kind: str,
    diagnostic: str,
    rel: str,
    line: int,
    foreign: str,
    evidence: str,
) -> Violation:
    return Violation(
        kind,
        diagnostic,
        rel,
        line,
        claim.owner if claim.owner is not None else "family_neutral",
        foreign,
        evidence[:240],
    )


def _unique_owned_names(
    files: list[tuple[RootClaim, pathlib.Path, str]],
) -> dict[str, str]:
    candidates: dict[str, set[str]] = {}
    for claim, path, _ in files:
        if claim.owner is None or path.suffix.lower() not in OWNED_FILE_SUFFIXES:
            continue
        candidates.setdefault(path.name, set()).add(claim.owner)
    return {
        name: next(iter(owners))
        for name, owners in candidates.items()
        if len(owners) == 1
    }


def _scan_cpp(
    registry: OwnershipRegistry,
    claim: RootClaim,
    rel: str,
    text: str,
    unique_names: dict[str, str],
) -> list[Violation]:
    violations: list[Violation] = []
    for match in INCLUDE_RE.finditer(text):
        include = match.group(1).replace("\\", "/")
        foreign_owner = unique_names.get(pathlib.PurePosixPath(include).name)
        if foreign_owner is not None and foreign_owner != claim.owner:
            violations.append(
                _make_violation(
                    claim,
                    "source_foreign_include" if claim.owner else "neutral_family_include",
                    "PARSER.PACKAGE.CROSS_FAMILY_DEPENDENCY" if claim.owner else "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                    rel,
                    _line_number(text, match.start()),
                    foreign_owner,
                    f"#include {include}",
                )
            )
            continue
        for foreign in _foreign_families(registry, claim):
            if any(fragment in include for fragment in registry.path_fragments(foreign)):
                violations.append(
                    _make_violation(
                        claim,
                        "source_foreign_include" if claim.owner else "neutral_family_include",
                        "PARSER.PACKAGE.CROSS_FAMILY_DEPENDENCY" if claim.owner else "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                        rel,
                        _line_number(text, match.start()),
                        foreign,
                        f"#include {include}",
                    )
                )
                break

    code_only = _strip_cpp_comments(text, strip_literals=True)
    for foreign in _foreign_families(registry, claim):
        for namespace in registry.family_namespaces(foreign):
            # Match qualified use as well as a namespace declaration.  The
            # latter is important for rejecting family compatibility aliases
            # placed inside a supposedly neutral header.
            pattern = re.compile(re.escape(namespace) + r"(?=\s*(?:::|\{|;))")
            for match in pattern.finditer(code_only):
                violations.append(
                    _make_violation(
                        claim,
                        "source_foreign_symbol" if claim.owner else "neutral_family_symbol",
                        "PARSER.PACKAGE.CROSS_FAMILY_DEPENDENCY" if claim.owner else "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                        rel,
                        _line_number(code_only, match.start()),
                        foreign,
                        namespace,
                    )
                )

    comments_removed = _strip_cpp_comments(text, strip_literals=False)
    if claim.owner in registry.strict_standalone_families:
        legacy_markers = set(registry.legacy_shared_parser_roots)
        legacy_markers.update(
            root.removeprefix("project/")
            for root in registry.legacy_shared_parser_roots
        )
        legacy_markers.update(registry.legacy_shared_parser_targets)
        legacy_markers.update(registry.forbidden_neutral_parser_targets)
        legacy_markers.update(
            {
                "compatibility_dialect.hpp",
                "compatibility_worker_session.hpp",
                "scratchbird::parser::compatibility::ParseStatement",
                "scratchbird::parser::compatibility::LexTokens",
                "scratchbird::parser::compatibility::HandleWorkerCommand",
                "scratchbird::parser::compatibility::ServeTextWorkerSession",
            }
        )
        for marker in sorted(legacy_markers):
            match = re.search(re.escape(marker), comments_removed)
            if match:
                violations.append(
                    _make_violation(
                        claim,
                        "source_legacy_shared_parser_dependency",
                        "PARSER.PACKAGE.CROSS_FAMILY_DEPENDENCY",
                        rel,
                        _line_number(comments_removed, match.start()),
                        "legacy_shared_parser",
                        marker,
                    )
                )

    if claim.neutral:
        # A neutral component may carry parser-independent values, but it may
        # not select a parser family or own a family's semantic/default policy.
        # Includes and namespace checks cannot see these string-driven dispatch
        # forms, which previously let compatibility_dialect.cpp appear neutral.
        family_alternation = "|".join(
            re.escape(family)
            for family in sorted(registry.families, key=lambda item: (-len(item), item))
        )
        selector_re = re.compile(
            rf"(?:\b(?:dialect(?:_id)?|parser_family|family)(?:\s*\.\s*dialect_id)?\b)"
            rf"\s*(?:==|!=)\s*\"(?P<family>{family_alternation})\"|"
            rf"\"(?P<reverse_family>{family_alternation})\"\s*(?:==|!=)\s*"
            rf"(?:\b(?:dialect(?:_id)?|parser_family|family)(?:\s*\.\s*dialect_id)?\b)"
        )
        for match in selector_re.finditer(comments_removed):
            family = match.group("family") or match.group("reverse_family")
            violations.append(
                _make_violation(
                    claim,
                    "neutral_family_semantic_selector",
                    "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                    rel,
                    _line_number(comments_removed, match.start()),
                    family,
                    match.group(0),
                )
            )

        family_policy_literal_re = re.compile(
            rf'\"(?P<literal>(?P<family>{family_alternation})[._][^\"\n]+)\"',
            re.IGNORECASE,
        )
        for match in family_policy_literal_re.finditer(comments_removed):
            literal = match.group("literal")
            if SEMANTIC_POLICY_WORD_RE.search(literal) is None:
                continue
            violations.append(
                _make_violation(
                    claim,
                    "neutral_family_semantic_policy",
                    "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                    rel,
                    _line_number(comments_removed, match.start()),
                    match.group("family").lower(),
                    literal,
                )
            )

        parser_surface_re = re.compile(
            r"\b(?:ParseStatement|LexTokens|HandleWorkerCommand|"
            r"ServeTextWorkerSession|DialectProfile|OperationPattern|"
            r"PatternMatch)\b"
        )
        for match in parser_surface_re.finditer(code_only):
            violations.append(
                _make_violation(
                    claim,
                    "neutral_sql_parser_surface",
                    "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                    rel,
                    _line_number(code_only, match.start()),
                    "shared_sql_parser",
                    match.group(0),
                )
            )

        semantic_renderer_re = re.compile(
            r"\b(?:[A-Za-z0-9_]*Semantic(?:Defaults)?Descriptor|"
            r"[A-Za-z0-9_]*SemanticEvidenceJson|"
            r"DatatypeDescriptorEvidenceJson|EnterpriseReadinessEvidenceJson|"
            r"ProceduralBodySourceRetentionEvidenceJson|"
            r"ProceduralFunctionalEncodingEvidenceJson)\b"
        )
        for match in semantic_renderer_re.finditer(code_only):
            violations.append(
                _make_violation(
                    claim,
                    "neutral_parser_semantic_renderer",
                    "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                    rel,
                    _line_number(code_only, match.start()),
                    "parser_family_semantics",
                    match.group(0),
                )
            )

    for foreign in _foreign_families(registry, claim):
        for fragment in registry.path_fragments(foreign):
            pattern = re.compile(re.escape(fragment) + r"(?=$|[/\s\"')])")
            for match in pattern.finditer(comments_removed):
                violations.append(
                    _make_violation(
                        claim,
                        "resource_foreign_reference" if claim.owner else "neutral_family_resource",
                        "PARSER.RESOURCE.FOREIGN_FAMILY" if claim.owner else "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                        rel,
                        _line_number(comments_removed, match.start()),
                        foreign,
                        fragment,
                    )
                )
    for match in re.finditer(r'"([^"\n]+)"', comments_removed):
        name = pathlib.PurePosixPath(match.group(1).replace("\\", "/")).name
        foreign_owner = unique_names.get(name)
        if foreign_owner is not None and foreign_owner != claim.owner and pathlib.PurePosixPath(name).suffix.lower() in OWNED_FILE_SUFFIXES:
            violations.append(
                _make_violation(
                    claim,
                    "resource_foreign_reference" if claim.owner else "neutral_family_resource",
                    "PARSER.RESOURCE.FOREIGN_FAMILY" if claim.owner else "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                    rel,
                    _line_number(comments_removed, match.start()),
                    foreign_owner,
                    match.group(1),
                )
            )
    return violations


def _scan_cmake(
    registry: OwnershipRegistry,
    claim: RootClaim,
    rel: str,
    text: str,
    unique_names: dict[str, str],
) -> list[Violation]:
    violations: list[Violation] = []
    clean = _cmake_without_comments(text)
    if claim.owner in registry.strict_standalone_families:
        forbidden_targets = (
            registry.legacy_shared_parser_targets
            | registry.forbidden_neutral_parser_targets
        )
        for target in sorted(forbidden_targets):
            match = re.search(rf"(?<![A-Za-z0-9_]){re.escape(target)}(?![A-Za-z0-9_])", clean)
            if match:
                violations.append(
                    _make_violation(
                        claim,
                        "cmake_legacy_shared_parser_target",
                        "PARSER.PACKAGE.CROSS_FAMILY_DEPENDENCY",
                        rel,
                        _line_number(clean, match.start()),
                        "legacy_shared_parser",
                        target,
                    )
                )
        for root in sorted(registry.legacy_shared_parser_roots):
            for marker in (root, root.removeprefix("project/")):
                match = re.search(re.escape(marker), clean)
                if match:
                    violations.append(
                        _make_violation(
                            claim,
                            "cmake_legacy_shared_parser_path",
                            "PARSER.PACKAGE.CROSS_FAMILY_DEPENDENCY",
                            rel,
                            _line_number(clean, match.start()),
                            "legacy_shared_parser",
                            marker,
                        )
                    )
    target_re = re.compile(r"(?<![A-Za-z0-9_])(?:sbl|sbp|sbu|sbup|sb)_[A-Za-z0-9_]+")
    for match in target_re.finditer(clean):
        token = match.group(0)
        foreign = registry.target_owner(token)
        if foreign is not None and foreign != claim.owner:
            violations.append(
                _make_violation(
                    claim,
                    "cmake_foreign_target" if claim.owner else "neutral_family_target",
                    "PARSER.PACKAGE.CROSS_FAMILY_DEPENDENCY" if claim.owner else "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                    rel,
                    _line_number(clean, match.start()),
                    foreign,
                    token,
                )
            )
    for foreign in _foreign_families(registry, claim):
        for fragment in registry.path_fragments(foreign):
            pattern = re.compile(re.escape(fragment) + r"(?=$|[/\s\"')])")
            for match in pattern.finditer(clean):
                violations.append(
                    _make_violation(
                        claim,
                        "cmake_foreign_path" if claim.owner else "neutral_family_path",
                        "PARSER.PACKAGE.CROSS_FAMILY_DEPENDENCY" if claim.owner else "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                        rel,
                        _line_number(clean, match.start()),
                        foreign,
                        fragment,
                    )
                )
    for match in CMAKE_FILE_TOKEN_RE.finditer(clean):
        name = pathlib.PurePosixPath(match.group("path")).name
        foreign_owner = unique_names.get(name)
        if foreign_owner is not None and foreign_owner != claim.owner:
            violations.append(
                _make_violation(
                    claim,
                    "cmake_foreign_owned_file" if claim.owner else "neutral_family_owned_file",
                    "PARSER.RESOURCE.FOREIGN_FAMILY" if claim.owner else "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                    rel,
                    _line_number(clean, match.start()),
                    foreign_owner,
                    match.group("path"),
                )
            )
    return violations


def _scan_resource_text(
    registry: OwnershipRegistry,
    claim: RootClaim,
    rel: str,
    text: str,
) -> list[Violation]:
    violations: list[Violation] = []
    for foreign in _foreign_families(registry, claim):
        for fragment in registry.path_fragments(foreign):
            pattern = re.compile(re.escape(fragment) + r"(?=$|[/\s\"')])")
            for match in pattern.finditer(text):
                violations.append(
                    _make_violation(
                        claim,
                        "resource_foreign_reference" if claim.owner else "neutral_family_resource",
                        "PARSER.RESOURCE.FOREIGN_FAMILY" if claim.owner else "PARSER.NEUTRAL_COMPONENT.MISCLASSIFIED",
                        rel,
                        _line_number(text, match.start()),
                        foreign,
                        fragment,
                    )
                )
    return violations


def scan_repository(registry: OwnershipRegistry) -> ScanResult:
    files = _walk_claim_files(registry)
    unique_names = _unique_owned_names(files)
    violations = _discovery_violations(registry)
    digest = hashlib.sha256()
    family_files = {family: 0 for family in registry.families}
    neutral_files = 0
    files_scanned = 0
    for claim, path, rel in files:
        if path.is_symlink():
            try:
                resolved = path.resolve(strict=True).relative_to(registry.repo_root.resolve()).as_posix()
            except (OSError, ValueError):
                resolved = "outside_repository"
            resolved_claim = registry.owner_for_path(resolved) if resolved != "outside_repository" else None
            if resolved_claim is None or resolved_claim.owner != claim.owner or resolved_claim.neutral != claim.neutral:
                violations.append(
                    _make_violation(
                        claim,
                        "ownership_escaping_symlink",
                        "PARSER.PACKAGE.NON_STANDALONE",
                        rel,
                        1,
                        resolved_claim.owner if resolved_claim and resolved_claim.owner else "unowned",
                        resolved,
                    )
                )
            continue
        text = _read_text(path)
        if text is None:
            continue
        files_scanned += 1
        if claim.owner is None:
            neutral_files += 1
        else:
            family_files[claim.owner] += 1
        digest.update(rel.encode("utf-8"))
        digest.update(b"\0")
        digest.update(text.encode("utf-8"))
        digest.update(b"\0")
        if path.name == "CMakeLists.txt" or path.suffix.lower() == ".cmake":
            violations.extend(_scan_cmake(registry, claim, rel, text, unique_names))
        elif path.suffix.lower() in CPP_SUFFIXES:
            violations.extend(_scan_cpp(registry, claim, rel, text, unique_names))
        else:
            violations.extend(_scan_resource_text(registry, claim, rel, text))
    unique = {
        (v.kind, v.path, v.line, v.owner, v.foreign_owner, v.evidence): v
        for v in violations
    }
    return ScanResult(
        sorted(unique.values(), key=lambda v: (v.path, v.line, v.kind, v.foreign_owner, v.evidence)),
        files_scanned,
        family_files,
        neutral_files,
        digest.hexdigest(),
    )


def evidence_payload(registry: OwnershipRegistry, result: ScanResult) -> dict[str, object]:
    failed_diagnostics = sorted({violation.diagnostic for violation in result.violations})
    passed = [] if result.violations else ["PARSER-ISO-001", "PARSER-ISO-002", "PARSER-ISO-003", "PARSER-ISO-014"]
    failed = ["PARSER-ISO-001", "PARSER-ISO-002", "PARSER-ISO-003"] if result.violations else []
    return {
        "gate": "parser_family_isolation_static_gate",
        "registry_id": registry.data.get("registry_id", "unknown"),
        "evidence_timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source_scan_hash": f"sha256:{result.source_scan_hash}",
        "families": sorted(registry.families),
        "family_count": len(registry.families),
        "files_scanned": result.files_scanned,
        "family_file_counts": result.family_files,
        "neutral_files_scanned": result.neutral_files,
        "cross_parser_dependency_count": len(result.violations),
        "passed_gate_ids": passed,
        "failed_gate_ids": failed,
        "diagnostic_codes": failed_diagnostics,
        "violations": [violation.to_json() for violation in result.violations],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--ownership-map", required=True, type=pathlib.Path)
    parser.add_argument("--evidence-file", type=pathlib.Path)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    map_path = args.ownership_map
    if not map_path.is_absolute():
        map_path = repo_root / map_path
    try:
        registry = OwnershipRegistry.load(repo_root, map_path)
        result = scan_repository(registry)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"parser_family_isolation_static_gate: configuration error: {error}", file=sys.stderr)
        return 2
    payload = evidence_payload(registry, result)
    if args.evidence_file:
        args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_file.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if result.violations:
        print(json.dumps(payload, indent=2, sort_keys=True), file=sys.stderr)
        return 1
    print(
        "parser_family_isolation_static_gate=passed "
        f"families={len(registry.families)} files={result.files_scanned}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
