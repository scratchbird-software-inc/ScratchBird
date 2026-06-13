#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate static driver resource, cache, and soak requirements."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


PERFORMANCE_REL = Path("project/tests/performance/driver_resource_soak_requirements.json")
RESOURCE_REL = Path("project/tests/conformance/resources/driver_resource_soak_requirements.json")
REPORT_REL = Path("build/reports/driver_resource_limit_soak.json")
REQUIRED_LANGUAGES = {"en-US", "en-CA", "fr-CA", "fr-FR", "de-DE", "it-IT", "es-ES"}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def validate_doc(doc: dict[str, Any], label: str) -> list[str]:
    errors: list[str] = []
    if doc.get("requires_live_driver") is not False:
        errors.append(f"{label}:requires_live_driver_must_be_false_for_static_gate")
    boundary = str(doc.get("authority_boundary", ""))
    if "validated statically only" not in boundary and "static conformance inputs" not in boundary:
        errors.append(f"{label}:missing_static_only_boundary")
    budgets = [
        item
        for item in as_list(doc.get("budgets") or doc.get("cases"))
        if isinstance(item, dict)
    ]
    if not budgets:
        errors.append(f"{label}:missing_budgets")
    coverage: set[str] = set()
    for budget in budgets:
        budget_id = str(budget.get("budget_id") or budget.get("case_id") or "")
        if not budget_id.startswith("BDRV-"):
            errors.append(f"{label}:{budget_id or '<missing>'}:invalid_budget_id")
        if budget.get("category") != "resource_soak_requirement":
            errors.append(f"{label}:{budget_id}:category_not_resource_soak_requirement")
        if budget.get("requires_live_driver") is not False:
            errors.append(f"{label}:{budget_id}:requires_live_driver_not_false")
        coverage.update(str(item) for item in as_list(budget.get("coverage")))
        if int(budget.get("maximum_stale_cache_acceptances", 0) or 0) != 0:
            errors.append(f"{label}:{budget_id}:stale_cache_acceptance_nonzero")
        if int(budget.get("maximum_driver_finality_frames", 0) or 0) != 0:
            errors.append(f"{label}:{budget_id}:driver_finality_frame_nonzero")
        language_values = set(str(item) for item in as_list(budget.get("required_profiles") or budget.get("required_language_profiles")))
        if language_values and language_values != REQUIRED_LANGUAGES:
            errors.append(f"{label}:{budget_id}:language_profile_set_incomplete")
    if "resource_soak_requirement" not in coverage:
        errors.append(f"{label}:missing_resource_soak_requirement_coverage")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--all-in-scope", action="store_true", required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    errors: list[str] = []
    documents: dict[str, str] = {}
    for label, rel in (("performance", PERFORMANCE_REL), ("resource", RESOURCE_REL)):
        try:
            doc = load_json(repo_root / rel)
            documents[label] = str(rel)
            errors.extend(validate_doc(doc, label))
        except (OSError, json.JSONDecodeError) as exc:
            errors.append(f"{label}:load_failed:{exc}")
    report = {
        "command": "driver_resource_limit_soak.py",
        "status": "fail" if errors else "pass",
        "summary": {"all_in_scope": args.all_in_scope, "documents": len(documents)},
        "documents": documents,
        "issues": errors,
    }
    output = args.output or repo_root / REPORT_REL
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"driver_resource_limit_soak={report['status']}")
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
