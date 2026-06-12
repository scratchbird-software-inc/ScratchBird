#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CEIC-021 memory hot-path source gate.

SEARCH_KEY: CEIC_021_MEMORY_SOURCE_GATE
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Iterable


DEFAULT_SCAN_ROOTS = (
    "project/src/engine/executor",
    "project/src/engine/optimizer",
    "project/src/engine/planner",
    "project/src/engine/internal_api/nosql",
    "project/src/storage/page",
    "project/src/core/index",
    "project/src/core/memory",
    "project/src/core/agents",
    "project/src/wire",
    "project/src/server",
)

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".ipp",
}

VALID_CATEGORIES = {
    "direct_new",
    "direct_delete",
    "raw_malloc_free",
    "unbounded_growth",
    "raw_page_buffer",
    "global_allocator_mutex",
}

GLOB_CHARS = set("*?[")
BROAD_PATTERNS = {
    "new",
    "delete",
    "malloc",
    "free",
    "push_back",
    "emplace_back",
    "resize",
    "append",
    "std::mutex",
    "page",
    "buffer",
}

HOT_GROWTH_NAME_MARKERS = (
    "allocator_lifetime_hot_path",
    "direct_binary_result_frame",
    "foreign_memory_reservation",
    "optimizer_plan_cache",
    "page_cache",
    "prepared_execution_template",
    "query_memory_arena",
    "reservation_backed",
    "result_batch",
    "result_cursor_plan_memory_governance",
    "streaming_cursor_manager",
    "streaming_result_window",
    "thread_local_memory_cache",
    "typed_arena",
    "typed_slab",
    "vectorized_result_batch",
)

BOUNDING_MARKERS = (
    ".reserve(",
    "reserve(",
    "capacity",
    "bounded",
    "limit",
    "max_",
    "kmax",
    "policy",
    "budget",
    "quota",
    "reservation",
    "reserved",
    "row_count",
    "column_count",
    "page_count",
    "byte_count",
    "payload_size",
    "frame_size",
    "credit",
    "remaining",
    "available",
    "memorymanager",
    "querymemoryarena",
    "reservationbackedmemoryresource",
    "typedarena",
    "typedslab",
    "sizeclassallocator",
    "threadlocalpercorememorycache",
    "scopedpagebuffer",
    "pagecacheframe",
)

APPROVED_PAGE_BUFFER_MARKERS = (
    "memorymanager",
    "scopedpagebuffer",
    "pagecacheframe",
    "pagecache",
    "reservationbackedmemoryresource",
    "querymemoryarena",
    "typedarena",
    "typedslab",
    "sizeclassallocator",
)

DIAGNOSTIC_GROWTH_MARKERS = (
    ".actions",
    ".diagnostic",
    ".diagnostics",
    ".evidence",
    "->evidence",
    ".refusal_reasons",
    "->refusal_reasons",
    ".support_bundle",
    ".support_bundle_rows",
    ".arguments",
    ".message_vector",
    ".messages",
    "support_evidence",
)

DIRECT_NEW_RULES = (
    re.compile(r"::operator\s+new\s*\("),
    re.compile(r"(?:^|[^\w:])::new\s*\("),
    re.compile(r"(?:^|[^\w:])new\s+(?:\(|[A-Za-z_:])"),
)
DIRECT_DELETE_RULES = (
    re.compile(r"::operator\s+delete\s*\("),
    re.compile(r"(?:^|[^\w:])delete\s*(?:\[\s*\])?\s+[A-Za-z_(\*]"),
)
MALLOC_FREE_RULE = re.compile(r"(?:^|[^\w:])(?:(?:std::|::))?(?:malloc|calloc|realloc|free)\s*\(")
GROWTH_RULE = re.compile(r"(?:\.|->)(?:push_back|emplace_back|append|insert|resize)\s*\(")
RAW_PAGE_NEW_RULE = re.compile(
    r"\bnew\s+(?:std::byte|byte|char|unsigned\s+char|std::uint8_t|uint8_t)\s*\["
)
RAW_PAGE_VECTOR_RULE = re.compile(
    r"std::vector\s*<\s*(?:std::byte|byte|char|unsigned\s+char|std::uint8_t|uint8_t)\s*>\s+"
    r"(?:raw_)?(?:page_buffer|page_bytes|frame_buffer|page_scratch|page_image_buffer)\b"
)
GLOBAL_MUTEX_RULES = (
    re.compile(r"\bstatic\s+std::mutex\b"),
    re.compile(r"\bglobal[_a-zA-Z0-9]*allocator[_a-zA-Z0-9]*mutex\b", re.IGNORECASE),
    re.compile(r"\ballocator[_a-zA-Z0-9]*global[_a-zA-Z0-9]*mutex\b", re.IGNORECASE),
)


@dataclass(frozen=True)
class Violation:
    category: str
    path: str
    line: int
    snippet: str
    rule: str


def repo_relative(repo_root: pathlib.Path, path: pathlib.Path) -> str:
    return path.relative_to(repo_root).as_posix()


def compact(value: str) -> str:
    return " ".join(value.strip().split())


def lowercase_compact(value: str) -> str:
    return compact(value).lower()


def sanitize_cpp_lines(lines: list[str]) -> list[str]:
    sanitized: list[str] = []
    in_block_comment = False
    raw_close: str | None = None

    for line in lines:
        i = 0
        out: list[str] = []
        while i < len(line):
            if raw_close is not None:
                end = line.find(raw_close, i)
                if end == -1:
                    out.append(" " * (len(line) - i))
                    i = len(line)
                    continue
                out.append(" " * (end + len(raw_close) - i))
                i = end + len(raw_close)
                raw_close = None
                continue

            if in_block_comment:
                end = line.find("*/", i)
                if end == -1:
                    out.append(" " * (len(line) - i))
                    i = len(line)
                    continue
                out.append(" " * (end + 2 - i))
                i = end + 2
                in_block_comment = False
                continue

            if line.startswith("//", i):
                out.append(" " * (len(line) - i))
                i = len(line)
                continue
            if line.startswith("/*", i):
                out.append("  ")
                i += 2
                in_block_comment = True
                continue
            if line.startswith('R"', i):
                open_paren = line.find("(", i + 2)
                if open_paren != -1:
                    delimiter = line[i + 2 : open_paren]
                    if len(delimiter) <= 16:
                        raw_close = ")" + delimiter + '"'
                        out.append(" " * (open_paren + 1 - i))
                        i = open_paren + 1
                        continue
            if line[i] in {'"', "'"}:
                quote = line[i]
                out.append(" ")
                i += 1
                escaped = False
                while i < len(line):
                    out.append(" ")
                    ch = line[i]
                    i += 1
                    if escaped:
                        escaped = False
                    elif ch == "\\":
                        escaped = True
                    elif ch == quote:
                        break
                continue

            out.append(line[i])
            i += 1
        sanitized.append("".join(out))

    return sanitized


def iter_source_files(repo_root: pathlib.Path, scan_roots: Iterable[str]) -> list[pathlib.Path]:
    files: list[pathlib.Path] = []
    for root_text in scan_roots:
        root = repo_root / root_text
        if root.is_file():
            candidates = [root]
        elif root.is_dir():
            candidates = sorted(path for path in root.rglob("*") if path.is_file())
        else:
            continue
        for path in candidates:
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            rel = repo_relative(repo_root, path)
            if "/__pycache__/" in rel:
                continue
            files.append(path)
    return sorted(set(files))


def path_in_default_hot_growth_surface(rel_path: str) -> bool:
    lowered = rel_path.lower()
    if "ceic_021_memory_source_gate" in lowered:
        return True
    return any(marker in lowered for marker in HOT_GROWTH_NAME_MARKERS)


def window_text(lines: list[str], index: int, before: int = 12, after: int = 2) -> str:
    start = max(0, index - before)
    end = min(len(lines), index + after + 1)
    return "\n".join(lines[start:end])


def has_bounding_context(lines: list[str], index: int) -> bool:
    local = lowercase_compact(window_text(lines, index, before=16, after=3))
    return any(marker in local for marker in BOUNDING_MARKERS)


def raw_page_context_is_approved(lines: list[str], index: int) -> bool:
    local = lowercase_compact(window_text(lines, index, before=8, after=3))
    return any(marker in local for marker in APPROVED_PAGE_BUFFER_MARKERS)


def is_page_buffer_context(line: str, lines: list[str], index: int) -> bool:
    local = lowercase_compact(line + "\n" + window_text(lines, index, before=2, after=2))
    return any(marker in local for marker in ("page", "frame", "buffer", "kpage", "page_size"))


def is_global_allocator_mutex_context(line: str, lines: list[str], index: int) -> bool:
    local = lowercase_compact(line + "\n" + window_text(lines, index, before=8, after=3))
    if "global" in local and "allocator" in local and "mutex" in local:
        return True
    if "defaultmemorymanager" in local or "memorymanager" in local:
        return True
    return "allocator" in local and "static std::mutex" in lowercase_compact(line)


def detect_growth_violation(rel_path: str, lines: list[str], index: int, line: str) -> bool:
    if not path_in_default_hot_growth_surface(rel_path):
        return False
    if not GROWTH_RULE.search(line):
        return False
    lowered_line = lowercase_compact(line)
    if any(marker in lowered_line for marker in DIAGNOSTIC_GROWTH_MARKERS):
        return False
    if "snapshot" in lowered_line:
        return False
    if has_bounding_context(lines, index):
        return False

    local = lowercase_compact(window_text(lines, index, before=5, after=1))
    if any(marker in local for marker in DIAGNOSTIC_GROWTH_MARKERS):
        return False
    if "snapshot" in local:
        return False
    if "while" in local or "for (;;" in local or "for(;;" in local:
        return True

    risky_names = ("result", "rows", "page", "frame", "payload", "buffer", "scratch", "batch")
    return any(name in lowercase_compact(line) for name in risky_names)


def scan_file(repo_root: pathlib.Path, path: pathlib.Path) -> list[Violation]:
    rel_path = repo_relative(repo_root, path)
    original_lines = path.read_text(encoding="utf-8").splitlines()
    sanitized_lines = sanitize_cpp_lines(original_lines)
    violations: list[Violation] = []

    for index, sanitized in enumerate(sanitized_lines):
        line_no = index + 1
        snippet = compact(original_lines[index])
        if not snippet:
            continue

        if any(rule.search(sanitized) for rule in DIRECT_NEW_RULES):
            violations.append(Violation("direct_new", rel_path, line_no, snippet, "direct new/operator new"))

        if any(rule.search(sanitized) for rule in DIRECT_DELETE_RULES):
            violations.append(Violation("direct_delete", rel_path, line_no, snippet, "direct delete/operator delete"))

        if MALLOC_FREE_RULE.search(sanitized):
            violations.append(Violation("raw_malloc_free", rel_path, line_no, snippet, "malloc/calloc/realloc/free"))

        if detect_growth_violation(rel_path, sanitized_lines, index, sanitized):
            violations.append(Violation("unbounded_growth", rel_path, line_no, snippet, "unbounded vector/string growth"))

        if (
            (RAW_PAGE_NEW_RULE.search(sanitized) or RAW_PAGE_VECTOR_RULE.search(sanitized))
            and is_page_buffer_context(sanitized, sanitized_lines, index)
            and not raw_page_context_is_approved(sanitized_lines, index)
        ):
            violations.append(Violation("raw_page_buffer", rel_path, line_no, snippet, "raw page/frame buffer allocation"))

        if any(rule.search(sanitized) for rule in GLOBAL_MUTEX_RULES) and is_global_allocator_mutex_context(
            sanitized, sanitized_lines, index
        ):
            violations.append(
                Violation(
                    "global_allocator_mutex",
                    rel_path,
                    line_no,
                    snippet,
                    "global/static allocator mutex dependency",
                )
            )

    return violations


def scan_sources(repo_root: pathlib.Path, scan_roots: Iterable[str]) -> list[Violation]:
    violations: list[Violation] = []
    for path in iter_source_files(repo_root, scan_roots):
        violations.extend(scan_file(repo_root, path))
    return violations


def load_allowlist(repo_root: pathlib.Path, allowlist_path: pathlib.Path | None) -> list[dict[str, str]]:
    if allowlist_path is None:
        allowlist_path = repo_root / "project/tools/ceic_memory_source_gate_allowlist.json"
    if not allowlist_path.exists():
        return []
    data = json.loads(allowlist_path.read_text(encoding="utf-8"))
    if not isinstance(data, list):
        raise ValueError("allowlist must be a JSON array")
    entries: list[dict[str, str]] = []
    for raw in data:
        if not isinstance(raw, dict):
            raise ValueError("allowlist entries must be objects")
        entries.append({str(key): str(value) for key, value in raw.items()})
    return entries


def validate_allowlist_entry(
    repo_root: pathlib.Path,
    entry: dict[str, str],
    violations: list[Violation],
) -> tuple[list[str], set[Violation]]:
    errors: list[str] = []
    required = ("category", "path", "pattern", "reason")
    for field in required:
        if not entry.get(field, "").strip():
            errors.append(f"allowlist entry missing {field}: {entry}")
    if errors:
        return errors, set()

    category = entry["category"].strip()
    path = entry["path"].strip()
    pattern = entry["pattern"].strip()
    reason = entry["reason"].strip()

    if category not in VALID_CATEGORIES:
        errors.append(f"allowlist {path} has unknown category {category}")
    if pathlib.PurePosixPath(path).is_absolute() or ".." in pathlib.PurePosixPath(path).parts:
        errors.append(f"allowlist {path} must be a normalized repo-relative path")
    if any(char in path for char in GLOB_CHARS):
        errors.append(f"allowlist {path} must name one exact path without globs")
    if not (repo_root / path).exists():
        errors.append(f"allowlist {path} is stale because the path does not exist")
    if len(reason.split()) < 8:
        errors.append(f"allowlist {path} reason is too short")
    if len(pattern) < 12 or pattern.lower() in BROAD_PATTERNS:
        errors.append(f"allowlist {path} pattern is too broad: {pattern!r}")

    matching_violations = {
        violation
        for violation in violations
        if violation.category == category and violation.path == path and pattern in violation.snippet
    }
    if not matching_violations:
        errors.append(
            f"allowlist {path} category {category} pattern {pattern!r} is stale; no current violation matches"
        )
    elif len(matching_violations) != 1:
        errors.append(
            f"allowlist {path} category {category} pattern {pattern!r} is too broad; "
            f"matched {len(matching_violations)} violations"
        )

    return errors, matching_violations


def apply_allowlist(
    repo_root: pathlib.Path,
    violations: list[Violation],
    allowlist: list[dict[str, str]],
) -> tuple[list[Violation], list[str]]:
    allowlist_errors: list[str] = []
    allowed: set[Violation] = set()
    seen_keys: set[tuple[str, str, str]] = set()

    for entry in allowlist:
        key = (
            entry.get("category", "").strip(),
            entry.get("path", "").strip(),
            entry.get("pattern", "").strip(),
        )
        if key in seen_keys:
            allowlist_errors.append(f"duplicate allowlist entry for {key}")
            continue
        seen_keys.add(key)
        errors, matches = validate_allowlist_entry(repo_root, entry, violations)
        allowlist_errors.extend(errors)
        allowed.update(matches)

    remaining = [violation for violation in violations if violation not in allowed]
    return remaining, allowlist_errors


def print_violations(violations: Iterable[Violation], stream) -> None:
    for violation in violations:
        print(
            f"{violation.category}:{violation.path}:{violation.line}: "
            f"{violation.rule}: {violation.snippet}",
            file=stream,
        )


def run_gate(repo_root: pathlib.Path, scan_roots: Iterable[str], allowlist_path: pathlib.Path | None) -> int:
    repo_root = repo_root.resolve()
    violations = scan_sources(repo_root, scan_roots)
    allowlist = load_allowlist(repo_root, allowlist_path)
    remaining, allowlist_errors = apply_allowlist(repo_root, violations, allowlist)

    if allowlist_errors:
        for error in allowlist_errors:
            print(f"ceic_memory_source_gate=fail:allowlist:{error}", file=sys.stderr)
    if remaining:
        print_violations(remaining, sys.stderr)

    if allowlist_errors or remaining:
        print(
            f"ceic_memory_source_gate=fail:violations={len(remaining)} allowlist_errors={len(allowlist_errors)}",
            file=sys.stderr,
        )
        return 1

    print(
        "ceic_memory_source_gate=pass:"
        f"files={len(iter_source_files(repo_root, scan_roots))} "
        f"allowed={len(allowlist)} "
        "authority=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action"
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="CEIC-021 memory source gate")
    parser.add_argument("--repo-root", default=".", help="Repository root to scan")
    parser.add_argument(
        "--scan-root",
        action="append",
        dest="scan_roots",
        help="Repo-relative source root or file to scan; may be repeated",
    )
    parser.add_argument(
        "--allowlist",
        type=pathlib.Path,
        default=None,
        help="Allowlist JSON path; defaults to project/tools/ceic_memory_source_gate_allowlist.json",
    )
    args = parser.parse_args(argv)

    scan_roots = args.scan_roots if args.scan_roots else list(DEFAULT_SCAN_ROOTS)
    return run_gate(pathlib.Path(args.repo_root), scan_roots, args.allowlist)


if __name__ == "__main__":
    raise SystemExit(main())
