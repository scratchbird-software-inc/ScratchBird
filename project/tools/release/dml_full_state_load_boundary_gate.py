#!/usr/bin/env python3
"""Freeze and bound full MGA relation-state loads outside ordinary DML."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


RAW_FULL_LOAD = "LoadMgaRelationStoreState("
REMOVED_HOT_LOADS = (
    "LoadInsertDependencyFullState(",
    "LoadSelectableProcedureDependencyFullState(",
)
SOURCE_SUFFIXES = {".cpp", ".hpp", ".inc"}


def source_files(root: Path):
    for path in root.rglob("*"):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            yield path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--inventory", type=Path)
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    inventory_path = (
        args.inventory.resolve()
        if args.inventory
        else project_root
        / "tools/release/dml_full_state_load_boundary_inventory.json"
    )
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    expected = inventory["raw_full_load_callers"]
    errors: list[str] = []
    actual: dict[str, int] = {}

    hot_roots = (
        project_root / "src/engine/internal_api/dml",
        project_root / "src/server",
    )
    for hot_root in hot_roots:
        for path in source_files(hot_root):
            text = path.read_text(encoding="utf-8", errors="replace")
            relative = path.relative_to(project_root).as_posix()
            count = text.count(RAW_FULL_LOAD)
            if count:
                actual[relative] = count
            for token in REMOVED_HOT_LOADS:
                if token in text:
                    errors.append(
                        f"removed hot-path full loader returned: {relative}: {token}"
                    )

    if actual != expected:
        for path in sorted(set(actual) | set(expected)):
            if actual.get(path, 0) != expected.get(path, 0):
                errors.append(
                    f"raw full-load inventory drift: {path}: expected "
                    f"{expected.get(path, 0)}, found {actual.get(path, 0)}"
                )

    facade_path = project_root / "src/engine/internal_api/dml/transactional_relation_store.cpp"
    facade = facade_path.read_text(encoding="utf-8", errors="replace")
    allowed_methods = set()
    for item in inventory["allowed_full_loads"]:
        method = item.get("method", "")
        classification = item.get("classification", "")
        bounds = (
            item.get("maximum_rows", 0),
            item.get("maximum_bytes", 0),
            item.get("maximum_allocation_units", 0),
        )
        if not method or method in allowed_methods:
            errors.append(f"duplicate or empty full-load method: {method!r}")
        allowed_methods.add(method)
        if not classification or any(value <= 0 for value in bounds):
            errors.append(f"unbounded full-load policy: {method}")
        if f"TransactionalRelationStore::{method}" not in facade:
            errors.append(f"inventoried full-load method missing: {method}")
        if classification not in facade:
            errors.append(
                f"full-load classification missing from implementation: {classification}"
            )
        for value in bounds:
            if str(value) not in facade.replace("'", ""):
                errors.append(
                    f"full-load bound {value} for {method} missing from implementation"
                )

    dml_header = (
        project_root / "src/engine/internal_api/dml/transactional_relation_store.hpp"
    ).read_text(encoding="utf-8", errors="replace")
    declared_full_methods = {
        line.split("(", 1)[0].split()[-1]
        for line in dml_header.splitlines()
        if "FullState(" in line and "MgaRelationStoreResult" in line
    }
    if declared_full_methods != allowed_methods:
        errors.append(
            "full-load facade inventory drift: expected "
            f"{sorted(allowed_methods)}, found {sorted(declared_full_methods)}"
        )

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(
        "DML full-state load boundary: PASS "
        f"({sum(actual.values())} bounded raw calls, "
        f"{len(allowed_methods)} classified methods)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
