#!/usr/bin/env python3
"""Freeze compatibility projection use outside canonical MGA DML."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


PROJECTION_CALL = "BuildCrudCompatibilityStateFromMga("
SOURCE_SUFFIXES = {".cpp", ".hpp", ".inc"}


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
        / "tools/release/crud_compatibility_boundary_inventory.json"
    )
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    allowed_classes = set(inventory["allowed_classifications"])
    expected = inventory["consumers"]

    errors: list[str] = []
    actual: dict[str, int] = {}
    source_root = project_root / "src"
    for path in source_root.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(project_root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        count = text.count(PROJECTION_CALL)
        if count:
            actual[relative] = count

    if actual != {path: item["count"] for path, item in expected.items()}:
        for path in sorted(set(actual) | set(expected)):
            expected_count = expected.get(path, {}).get("count", 0)
            actual_count = actual.get(path, 0)
            if expected_count != actual_count:
                errors.append(
                    f"compatibility projection inventory drift: {path}: "
                    f"expected {expected_count}, found {actual_count}"
                )

    for path, item in expected.items():
        classification = item.get("classification", "")
        if classification not in allowed_classes:
            errors.append(
                f"unrecognized compatibility consumer classification: "
                f"{path}: {classification}"
            )

    canonical_dml = project_root / "src/engine/internal_api/dml"
    forbidden = ("CrudState", PROJECTION_CALL, ".sb.crud_events")
    for path in canonical_dml.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in forbidden:
            if token in text:
                errors.append(
                    f"canonical DML compatibility boundary violation: "
                    f"{path.relative_to(project_root)} contains {token!r}"
                )

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(
        "crud compatibility boundary: PASS "
        f"({sum(actual.values())} frozen uses in {len(actual)} classified files)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
