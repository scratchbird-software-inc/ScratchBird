#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Check or install ScratchBird's default-branch critical-review ruleset.

Checking is read-only and is the default. During the pre-public-beta advisory
phase, ``--apply`` is refused. After an explicit policy transition to required
mode, it creates or updates the named ruleset only when every configured owner
has GitHub write permission and all platform PR-CI variables are enabled.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_POLICY = REPO_ROOT / ".github" / "critical-path-reviewers.json"
DEFAULT_RULESET = REPO_ROOT / ".github" / "rulesets" / "main-critical-path.json"
REQUIRED_CI_VARIABLES = (
    "SB_LINUX_CI_ENABLED",
    "SB_WINDOWS_CI_ENABLED",
    "SB_MACOS_CI_ENABLED",
)


class ConfigurationError(RuntimeError):
    pass


def gh_json(arguments: list[str], *, input_path: Path | None = None) -> Any:
    command = ["gh", "api", *arguments]
    if input_path is not None:
        command.extend(["--input", str(input_path)])
    try:
        process = subprocess.run(command, text=True, capture_output=True, check=False)
    except OSError as exc:
        raise ConfigurationError(f"cannot execute GitHub CLI: {exc}") from exc
    if process.returncode != 0:
        detail = (process.stderr or process.stdout).strip()
        raise ConfigurationError(f"{' '.join(command)} failed: {detail}")
    try:
        return json.loads(process.stdout)
    except json.JSONDecodeError as exc:
        raise ConfigurationError(f"gh returned invalid JSON: {exc}") from exc


def configured_owners(policy: dict[str, Any]) -> set[str]:
    return {
        str(owner)
        for domain in policy.get("domains", [])
        for owner in domain.get("owners", [])
    }


def preflight(repository: str, policy: dict[str, Any]) -> list[str]:
    failures: list[str] = []
    collaborators = gh_json(
        ["--paginate", f"repos/{repository}/collaborators"]
    )
    by_login = {
        str(row.get("login") or "").casefold(): row
        for row in collaborators
        if isinstance(row, dict)
    }
    writable = 0
    for owner in sorted(configured_owners(policy), key=str.casefold):
        row = by_login.get(owner.casefold())
        if row is not None and str(row.get("type") or "").casefold() != "user":
            failures.append(f"configured owner @{owner} is not a GitHub User")
            continue
        has_write = bool((row or {}).get("permissions", {}).get("push"))
        if has_write:
            writable += 1
        else:
            failures.append(f"configured owner @{owner} lacks repository write permission")
    if writable < 2:
        failures.append("at least two configured owners need write permission for independent native review")

    variables = gh_json([f"repos/{repository}/actions/variables"])
    values = {
        str(row.get("name") or ""): str(row.get("value") or "")
        for row in variables.get("variables", [])
    }
    for name in REQUIRED_CI_VARIABLES:
        if values.get(name, "").casefold() != "true":
            failures.append(f"repository variable {name} must be true")
    return failures


def find_ruleset(repository: str, name: str) -> dict[str, Any] | None:
    rows = gh_json(["--paginate", f"repos/{repository}/rulesets"])
    for row in rows:
        if row.get("name") == name:
            return gh_json([f"repos/{repository}/rulesets/{row['id']}"])
    return None


def validate_live_ruleset(live: dict[str, Any] | None, desired: dict[str, Any]) -> list[str]:
    if live is None:
        return [f"ruleset {desired['name']!r} is not installed"]
    failures: list[str] = []
    if live.get("enforcement") != "active":
        failures.append("ruleset enforcement is not active")
    by_type = {rule.get("type"): rule for rule in live.get("rules", [])}
    pull = by_type.get("pull_request", {}).get("parameters", {})
    for key, expected in {
        "dismiss_stale_reviews_on_push": True,
        "require_code_owner_review": True,
        "require_last_push_approval": True,
        "required_approving_review_count": 1,
    }.items():
        if pull.get(key) != expected:
            failures.append(f"pull-request rule {key} must be {expected!r}")
    required = by_type.get("required_status_checks", {}).get("parameters", {})
    if required.get("strict_required_status_checks_policy") is not True:
        failures.append("required status checks must require an up-to-date branch")
    desired_contexts = {
        row["context"]: row.get("integration_id")
        for rule in desired["rules"]
        if rule["type"] == "required_status_checks"
        for row in rule["parameters"]["required_status_checks"]
    }
    live_contexts = {
        row.get("context"): row.get("integration_id")
        for row in required.get("required_status_checks", [])
    }
    missing = sorted(set(desired_contexts) - set(live_contexts))
    if missing:
        failures.append("required status contexts missing: " + ", ".join(missing))
    wrong_sources = sorted(
        context
        for context, integration_id in desired_contexts.items()
        if context in live_contexts and live_contexts[context] != integration_id
    )
    if wrong_sources:
        failures.append(
            "required status contexts have the wrong integration: "
            + ", ".join(wrong_sources)
        )
    return failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", default="scratchbird-software-inc/ScratchBird")
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    parser.add_argument("--ruleset", type=Path, default=DEFAULT_RULESET)
    parser.add_argument("--apply", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        policy = json.loads(args.policy.read_text(encoding="utf-8"))
        desired = json.loads(args.ruleset.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, ConfigurationError) as exc:
        print(f"critical_review_ruleset=fail:{exc}", file=sys.stderr)
        return 1

    try:
        live = find_ruleset(args.repository, desired["name"])
        if (
            policy.get("enforcement_mode") != "required"
            or desired.get("enforcement") != "active"
        ):
            if args.apply:
                raise ConfigurationError(
                    "public-beta enforcement is not armed; policy must be required "
                    "and the versioned ruleset must be active"
                )
            if live is not None and live.get("enforcement") == "active":
                raise ConfigurationError(
                    "live critical-review ruleset is active during advisory phase"
                )
            print(
                "critical_review_ruleset=advisory:pre-public-beta policy is staged "
                "and no merge restrictions are active"
            )
            return 0

        failures = preflight(args.repository, policy)
        if failures:
            for failure in failures:
                print(f"critical_review_ruleset=fail:{failure}", file=sys.stderr)
            return 1
        if args.apply:
            if live is None:
                gh_json(
                    ["--method", "POST", f"repos/{args.repository}/rulesets"],
                    input_path=args.ruleset,
                )
            else:
                gh_json(
                    [
                        "--method",
                        "PUT",
                        f"repos/{args.repository}/rulesets/{live['id']}",
                    ],
                    input_path=args.ruleset,
                )
            live = find_ruleset(args.repository, desired["name"])
    except ConfigurationError as exc:
        print(f"critical_review_ruleset=fail:{exc}", file=sys.stderr)
        return 1

    failures = validate_live_ruleset(live, desired)
    if failures:
        for failure in failures:
            print(f"critical_review_ruleset=fail:{failure}", file=sys.stderr)
        if not args.apply:
            print("Run again with --apply after correcting the prerequisites.", file=sys.stderr)
        return 1
    print("critical_review_ruleset=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
