#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Require current, independent, evidence-bearing critical-path approvals.

The GitHub workflow runs this file from the trusted default branch under
``pull_request_target``. It never checks out or executes pull-request code.
The resulting commit status is written directly to the pull request head SHA,
which makes the result usable as a ruleset-required status check.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
from pathlib import Path
import re
import sys
from typing import Any, Iterable
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen


POLICY_SCHEMA = "scratchbird.critical_path_review_policy.v1"
POLICY_MODES = {"advisory", "required"}
DEFAULT_POLICY = Path(".github/critical-path-reviewers.json")
PR_SECTIONS = (
    "Invariants manually reviewed",
    "Runtime tests executed",
    "Remaining uncertainty",
)
REVIEW_FIELDS = (
    "Invariants reviewed",
    "Runtime evidence reviewed",
    "Remaining uncertainty",
)
DECISIVE_REVIEW_STATES = {"APPROVED", "CHANGES_REQUESTED", "DISMISSED"}
PLACEHOLDER_ONLY = re.compile(
    r"^(?:n/?a|none|no(?:ne)?|todo|tbd|unknown|not applicable|"
    r"replace(?: me)?|fill(?: this)?(?: in)?|pending)[.!\s-]*$",
    re.IGNORECASE,
)
HTML_COMMENT = re.compile(r"<!--.*?-->", re.DOTALL)
MARKDOWN_HEADING = re.compile(r"(?m)^\s{0,3}#{1,6}\s+(.+?)\s*$")


class GateError(RuntimeError):
    """Raised for invalid policy, API, or event data."""


def _clean_path(value: str) -> str:
    path = value.strip().replace("\\", "/")
    while path.startswith("./"):
        path = path[2:]
    if not path or path.startswith("/") or ".." in path.split("/"):
        raise GateError(f"unsafe changed path: {value!r}")
    return path


def load_policy(path: Path) -> dict[str, Any]:
    try:
        policy = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GateError(f"cannot load policy {path}: {exc}") from exc
    if policy.get("schema") != POLICY_SCHEMA:
        raise GateError(f"unsupported policy schema in {path}")
    if policy.get("enforcement_mode") not in POLICY_MODES:
        raise GateError(
            "enforcement_mode must be either 'advisory' or 'required'"
        )
    domains = policy.get("domains")
    if not isinstance(domains, list) or not domains:
        raise GateError("policy must define at least one domain")
    seen: set[str] = set()
    for domain in domains:
        name = domain.get("name")
        owners = domain.get("owners")
        patterns = domain.get("paths")
        if not isinstance(name, str) or not name or name in seen:
            raise GateError(f"invalid or duplicate policy domain: {name!r}")
        seen.add(name)
        if not isinstance(owners, list) or not owners or not all(
            isinstance(owner, str) and owner.strip() for owner in owners
        ):
            raise GateError(f"domain {name!r} has no valid owners")
        if not isinstance(patterns, list) or not patterns or not all(
            isinstance(pattern, str) and pattern.strip() for pattern in patterns
        ):
            raise GateError(f"domain {name!r} has no valid paths")
    minimum = policy.get("minimum_approvals_per_domain")
    if not isinstance(minimum, int) or isinstance(minimum, bool) or minimum < 1:
        raise GateError("minimum_approvals_per_domain must be a positive integer")
    context = policy.get("status_context")
    if not isinstance(context, str) or not context.strip():
        raise GateError("status_context must be a non-empty string")
    return policy


def affected_domains(
    changed_paths: Iterable[str], policy: dict[str, Any]
) -> dict[str, set[str]]:
    """Return each affected domain and the paths that caused the match."""

    result: dict[str, set[str]] = {}
    paths = [_clean_path(path) for path in changed_paths]
    for domain in policy["domains"]:
        matched = {
            path
            for path in paths
            if any(
                fnmatch.fnmatchcase(path, pattern)
                for pattern in domain["paths"]
            )
        }
        if matched:
            result[domain["name"]] = matched
    return result


def _meaningful(value: str) -> bool:
    without_comments = HTML_COMMENT.sub("", value)
    normalized = re.sub(r"(?m)^\s*[-*+]\s*(?:\[[ xX]\]\s*)?", "", without_comments)
    normalized = " ".join(normalized.split()).strip()
    if len(normalized) < 16 or len(re.findall(r"[A-Za-z]", normalized)) < 10:
        return False
    return PLACEHOLDER_ONLY.fullmatch(normalized) is None


def _markdown_sections(body: str) -> dict[str, str]:
    matches = list(MARKDOWN_HEADING.finditer(body or ""))
    sections: dict[str, str] = {}
    for index, match in enumerate(matches):
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(body)
        sections[match.group(1).strip().casefold()] = body[start:end].strip()
    return sections


def validate_pr_body(body: str) -> list[str]:
    sections = _markdown_sections(body or "")
    missing: list[str] = []
    for heading in PR_SECTIONS:
        value = sections.get(heading.casefold(), "")
        if not _meaningful(value):
            missing.append(heading)
    return missing


def _review_fields(body: str) -> dict[str, str]:
    """Extract the three evidence fields from a submitted review body."""

    value = HTML_COMMENT.sub("", body or "")
    positions: list[tuple[int, int, str]] = []
    for field in REVIEW_FIELDS:
        match = re.search(
            rf"(?im)^\s*(?:#{1,6}\s*)?{re.escape(field)}\s*:\s*",
            value,
        )
        if match is not None:
            positions.append((match.start(), match.end(), field))
    positions.sort()
    fields: dict[str, str] = {}
    for index, (_, content_start, field) in enumerate(positions):
        content_end = positions[index + 1][0] if index + 1 < len(positions) else len(value)
        fields[field] = value[content_start:content_end].strip()
    return fields


def validate_review_body(body: str) -> list[str]:
    fields = _review_fields(body or "")
    return [field for field in REVIEW_FIELDS if not _meaningful(fields.get(field, ""))]


def _review_sort_key(review: dict[str, Any]) -> tuple[str, int]:
    return (str(review.get("submitted_at") or ""), int(review.get("id") or 0))


def latest_decisive_reviews(reviews: Iterable[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """Return the latest approval/change/dismissal state for every reviewer."""

    latest: dict[str, dict[str, Any]] = {}
    for review in sorted(reviews, key=_review_sort_key):
        user = review.get("user") or {}
        login = str(user.get("login") or "")
        state = str(review.get("state") or "").upper()
        if login and state in DECISIVE_REVIEW_STATES:
            latest[login.casefold()] = review
    return latest


def evaluate(
    *,
    policy: dict[str, Any],
    changed_paths: Iterable[str],
    author_login: str,
    head_sha: str,
    pr_body: str,
    reviews: Iterable[dict[str, Any]],
) -> tuple[bool, list[str], dict[str, set[str]]]:
    """Evaluate the critical review policy without performing network I/O."""

    affected = affected_domains(changed_paths, policy)
    if not affected:
        return True, ["no configured critical paths changed"], affected

    failures: list[str] = []
    missing_sections = validate_pr_body(pr_body)
    if missing_sections:
        failures.append("PR body missing substantive sections: " + ", ".join(missing_sections))

    latest = latest_decisive_reviews(reviews)
    author = author_login.casefold()
    minimum = policy["minimum_approvals_per_domain"]
    domain_by_name = {domain["name"]: domain for domain in policy["domains"]}

    for domain_name in sorted(affected):
        domain = domain_by_name[domain_name]
        allowed = {str(owner).casefold() for owner in domain["owners"]}
        accepted: list[str] = []
        rejected: list[str] = []
        for login_key, review in latest.items():
            if login_key not in allowed:
                continue
            user = review.get("user") or {}
            login = str(user.get("login") or "")
            if login_key == author:
                rejected.append(f"{login}:self-review")
                continue
            if str(user.get("type") or "").casefold() != "user" or login_key.endswith("[bot]"):
                rejected.append(f"{login}:not-human")
                continue
            if str(review.get("state") or "").upper() != "APPROVED":
                rejected.append(f"{login}:{str(review.get('state') or 'unknown').lower()}")
                continue
            if str(review.get("commit_id") or "") != head_sha:
                rejected.append(f"{login}:stale")
                continue
            missing_fields = validate_review_body(str(review.get("body") or ""))
            if missing_fields:
                rejected.append(f"{login}:missing-review-evidence")
                continue
            accepted.append(login)
        if len(accepted) < minimum:
            detail = f"; rejected={','.join(rejected)}" if rejected else ""
            failures.append(
                f"{domain_name} requires {minimum} current independent human approval(s) "
                f"from {', '.join(domain['owners'])}{detail}"
            )

    if failures:
        return False, failures, affected
    return True, [f"critical domains approved: {', '.join(sorted(affected))}"], affected


def apply_enforcement_mode(
    policy: dict[str, Any], success: bool, messages: list[str]
) -> tuple[bool, str, list[str]]:
    """Convert strict evaluation into the release-phase workflow result."""

    if policy["enforcement_mode"] == "required":
        return success, "pass" if success else "fail", messages
    advisory_messages = [f"advisory only: {message}" for message in messages]
    return True, "advisory", advisory_messages


class GitHubClient:
    def __init__(self, repository: str, token: str) -> None:
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise GateError(f"invalid GitHub repository: {repository!r}")
        if not token:
            raise GateError("GitHub token is empty")
        self.repository = repository
        self.token = token

    def request(self, method: str, endpoint: str, payload: Any = None) -> Any:
        data = None if payload is None else json.dumps(payload).encode("utf-8")
        request = Request(
            f"https://api.github.com{endpoint}",
            data=data,
            method=method,
            headers={
                "Accept": "application/vnd.github+json",
                "Authorization": f"Bearer {self.token}",
                "User-Agent": "scratchbird-critical-path-review-gate",
                "X-GitHub-Api-Version": "2022-11-28",
            },
        )
        try:
            with urlopen(request, timeout=30) as response:
                return json.loads(response.read().decode("utf-8"))
        except HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")[:500]
            raise GateError(f"GitHub API {method} {endpoint} failed: {exc.code}: {detail}") from exc
        except (OSError, URLError, json.JSONDecodeError) as exc:
            raise GateError(f"GitHub API {method} {endpoint} failed: {exc}") from exc

    def paged(self, endpoint: str) -> list[dict[str, Any]]:
        values: list[dict[str, Any]] = []
        separator = "&" if "?" in endpoint else "?"
        for page in range(1, 101):
            batch = self.request("GET", f"{endpoint}{separator}per_page=100&page={page}")
            if not isinstance(batch, list):
                raise GateError(f"GitHub API returned a non-list for {endpoint}")
            values.extend(batch)
            if len(batch) < 100:
                return values
        raise GateError(f"GitHub API pagination limit exceeded for {endpoint}")

    def pull_request(self, number: int) -> dict[str, Any]:
        result = self.request("GET", f"/repos/{self.repository}/pulls/{number}")
        if not isinstance(result, dict):
            raise GateError("GitHub API returned an invalid pull request")
        return result

    def changed_files(self, number: int) -> list[str]:
        rows = self.paged(f"/repos/{self.repository}/pulls/{number}/files")
        return [str(row.get("filename") or "") for row in rows]

    def reviews(self, number: int) -> list[dict[str, Any]]:
        return self.paged(f"/repos/{self.repository}/pulls/{number}/reviews")

    def publish_status(
        self, *, sha: str, context: str, success: bool, description: str
    ) -> None:
        target_url = ""
        if os.environ.get("GITHUB_SERVER_URL") and os.environ.get("GITHUB_REPOSITORY") and os.environ.get("GITHUB_RUN_ID"):
            target_url = (
                f"{os.environ['GITHUB_SERVER_URL']}/{os.environ['GITHUB_REPOSITORY']}"
                f"/actions/runs/{os.environ['GITHUB_RUN_ID']}"
            )
        payload: dict[str, Any] = {
            "state": "success" if success else "failure",
            "context": context,
            "description": description[:140],
        }
        if target_url:
            payload["target_url"] = target_url
        self.request(
            "POST",
            f"/repos/{self.repository}/statuses/{quote(sha, safe='')}",
            payload,
        )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--pull-request", required=True, type=int)
    parser.add_argument("--publish-status", action="store_true")
    parser.add_argument("--token-env", default="GITHUB_TOKEN")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    token = os.environ.get(args.token_env, "")
    client: GitHubClient | None = None
    head_sha = ""
    context = "critical-path/independent-review"
    success = False
    result_label = "fail"
    messages: list[str] = []
    try:
        policy = load_policy(args.policy)
        context = policy["status_context"]
        client = GitHubClient(args.repository, token)
        pull = client.pull_request(args.pull_request)
        head_sha = str((pull.get("head") or {}).get("sha") or "")
        author = str((pull.get("user") or {}).get("login") or "")
        if not head_sha or not author:
            raise GateError("pull request is missing head SHA or author")
        success, messages, affected = evaluate(
            policy=policy,
            changed_paths=client.changed_files(args.pull_request),
            author_login=author,
            head_sha=head_sha,
            pr_body=str(pull.get("body") or ""),
            reviews=client.reviews(args.pull_request),
        )
        success, result_label, messages = apply_enforcement_mode(
            policy, success, messages
        )
        print("critical_path_review_domains=" + (",".join(sorted(affected)) or "none"))
    except GateError as exc:
        messages = [str(exc)]
        success = False

    for message in messages:
        print(f"critical_path_review_gate={result_label}:{message}")

    if args.publish_status:
        if client is None or not head_sha:
            print("critical_path_review_gate=fail:cannot publish status without PR head SHA", file=sys.stderr)
            return 1
        description = messages[0] if messages else "critical review policy evaluated"
        try:
            client.publish_status(
                sha=head_sha,
                context=context,
                success=success,
                description=description,
            )
        except GateError as exc:
            print(f"critical_path_review_gate=fail:{exc}", file=sys.stderr)
            return 1
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
