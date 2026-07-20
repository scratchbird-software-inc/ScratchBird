#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Safely create or update ScratchBird's fixed ``nightly`` GitHub Release."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
from typing import Any, Protocol
from urllib.parse import quote

TOOL_ROOT = Path(__file__).resolve().parent
if str(TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOL_ROOT))

from nightly_release_contract import RELEASE_CONTRACTS, ReleaseContract, get_release_contract


DEFAULT_CONTRACT = get_release_contract("all")
# Compatibility aliases for the complete, all-platform nightly contract.
TAG = DEFAULT_CONTRACT.tag
MANIFEST_NAME = DEFAULT_CONTRACT.manifest_name
CHECKSUM_NAME = DEFAULT_CONTRACT.checksum_name
MANIFEST_SCHEMA = "scratchbird.native_nightly_release.v1"
PUBLIC_ASSET_POLICY = "fully_verified_native_portable_and_system_installer_artifacts"
API_VERSION = "2026-03-10"
TAG_VISIBILITY_ATTEMPTS = 15
TAG_VISIBILITY_DELAY_SECONDS = 2.0
API_READ_TRANSIENT_ATTEMPTS = 30
API_READ_TRANSIENT_DELAY_SECONDS = 5.0
INITIAL_RELEASE_CREATE_ATTEMPTS = 4
INITIAL_RELEASE_CREATE_DELAY_SECONDS = 5.0
INITIAL_RELEASE_VISIBILITY_ATTEMPTS = 15
INITIAL_RELEASE_VISIBILITY_DELAY_SECONDS = 2.0
RELEASE_LIST_PAGE_SIZE = 100
RELEASE_LIST_MAX_PAGES = 1000
RELEASE_ASSET_UPLOAD_ATTEMPTS = 4
RELEASE_ASSET_UPLOAD_DELAY_SECONDS = 5.0
MANAGED_RELEASE_MARKER_SCHEMA = "v2"
MANAGED_RELEASE_AUTHOR = "github-actions[bot]"
CANONICAL_ASSET_NAMES = DEFAULT_CONTRACT.canonical_asset_names
REQUIRED_ARTIFACT_VERIFICATION = DEFAULT_CONTRACT.verification_by_name
NATIVE_COMPONENTS = [
    {"name": "SBmgr", "role": "manager"},
    {"name": "SBgate", "role": "gateway"},
    {"name": "SBParser", "role": "native_SBSQL_parser"},
    {"name": "SBsrv", "role": "database_server"},
]


class PublishError(RuntimeError):
    """A fail-closed rolling publication error."""


@dataclass
class CommandResult:
    returncode: int
    stdout: str = ""
    stderr: str = ""


class Runner(Protocol):
    def run(self, command: list[str], *, check: bool = True) -> CommandResult: ...


class SubprocessRunner:
    def run(self, command: list[str], *, check: bool = True) -> CommandResult:
        completed = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        result = CommandResult(completed.returncode, completed.stdout, completed.stderr)
        if check and completed.returncode != 0:
            raise PublishError(
                f"command_failed:{command[0]}:exit={completed.returncode}:"
                f"{(completed.stderr or completed.stdout).strip()}"
            )
        return result


@dataclass(frozen=True)
class LocalAsset:
    path: Path
    name: str
    size: int
    digest: str


@dataclass
class Swap:
    canonical_name: str
    incoming_name: str
    incoming_id: int
    old_id: int | None
    backup_name: str | None


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_checksums(path: Path) -> dict[str, str]:
    rows: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise PublishError(f"checksum_read_failed:{exc}") from exc
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  (scratchbird-nightly-[^/\\]+)", line)
        if match is None:
            raise PublishError(f"checksum_row_invalid:{line}")
        digest, name = match.groups()
        if name in rows:
            raise PublishError(f"checksum_name_duplicate:{name}")
        rows[name] = digest
    if not rows:
        raise PublishError("checksums_empty")
    return rows


def load_local_assets(
    asset_root: Path,
    target_sha: str,
    run_id: str,
    run_attempt: str,
    release_scope: str = "all",
) -> list[LocalAsset]:
    try:
        contract = get_release_contract(release_scope)
    except ValueError as exc:
        raise PublishError(str(exc)) from exc
    if asset_root.is_symlink():
        raise PublishError("asset_root_symlink_forbidden")
    root = asset_root.resolve()
    if not root.is_dir():
        raise PublishError(f"asset_root_missing:{root}")
    for path in root.iterdir():
        if path.is_symlink() or not path.is_file():
            raise PublishError(f"non_regular_release_asset:{path.name}")
        if not path.name.startswith("scratchbird-nightly-"):
            raise PublishError(f"release_asset_name_not_managed:{path.name}")
    checksums_path = root / contract.checksum_name
    manifest_path = root / contract.manifest_name
    if not checksums_path.is_file() or not manifest_path.is_file():
        raise PublishError("release_metadata_missing")
    checksums = parse_checksums(checksums_path)
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PublishError(f"release_manifest_invalid:{exc}") from exc
    if not isinstance(manifest, dict) or manifest.get("schema_id") != MANIFEST_SCHEMA:
        raise PublishError("release_manifest_schema_mismatch")
    if manifest.get("distribution_surface") != "scratchbird_native_no_emulation" or manifest.get("emulation_layers_included") is not False:
        raise PublishError("release_manifest_not_native_only")
    if manifest.get("public_asset_policy") != PUBLIC_ASSET_POLICY:
        raise PublishError("release_manifest_public_asset_policy_mismatch")
    if manifest.get("native_parser") != "SBSQL":
        raise PublishError("release_manifest_native_parser_mismatch")
    if manifest.get("native_components") != NATIVE_COMPONENTS:
        raise PublishError("release_manifest_native_components_mismatch")
    expected_platforms = (
        ["linux", "windows", "macos"]
        if contract.scope == "all"
        else [contract.scope]
    )
    if manifest.get("included_platforms") != expected_platforms:
        raise PublishError("release_manifest_platform_scope_mismatch")
    if manifest.get("release_scope") != contract.scope:
        raise PublishError("release_manifest_scope_mismatch")
    if manifest.get("release_tag") != contract.tag:
        raise PublishError("release_manifest_tag_mismatch")
    if str(manifest.get("source_revision") or "").lower() != target_sha.lower():
        raise PublishError("release_manifest_revision_mismatch")
    if str(manifest.get("github_run_id") or "") != run_id or str(manifest.get("github_run_attempt") or "") != run_attempt:
        raise PublishError("release_manifest_run_identity_mismatch")
    artifact_rows = manifest.get("artifacts")
    if not isinstance(artifact_rows, list) or not artifact_rows:
        raise PublishError("release_manifest_artifacts_missing")
    manifest_names = {
        row.get("name")
        for row in artifact_rows
        if isinstance(row, dict) and isinstance(row.get("name"), str)
    }
    if len(manifest_names) != len(artifact_rows):
        raise PublishError("release_manifest_artifact_names_invalid")
    expected_package_names = set(checksums) - {contract.manifest_name}
    if manifest_names != expected_package_names:
        raise PublishError("release_manifest_checksum_inventory_mismatch")
    if manifest_names != set(contract.package_names):
        raise PublishError("release_manifest_verification_inventory_mismatch")
    for row in artifact_rows:
        if not isinstance(row, dict) or not isinstance(row.get("name"), str):
            raise PublishError("release_manifest_artifact_row_invalid")
        name = str(row["name"])
        package_path = root / name
        if not package_path.is_file() or package_path.is_symlink():
            raise PublishError(f"release_manifest_file_missing:{name}")
        if row.get("bytes") != package_path.stat().st_size:
            raise PublishError(f"release_manifest_size_mismatch:{name}")
        if row.get("sha256") != sha256_file(package_path):
            raise PublishError(f"release_manifest_digest_mismatch:{name}")
        if row.get("verification") != contract.verification_by_name[name]:
            raise PublishError(f"release_manifest_verification_mismatch:{name}")

    actual_names = {path.name for path in root.iterdir()}
    expected_names = set(checksums) | {contract.checksum_name}
    if expected_names != contract.canonical_asset_names:
        raise PublishError(
            f"release_canonical_inventory_mismatch:expected={sorted(contract.canonical_asset_names)}:"
            f"actual={sorted(expected_names)}"
        )
    if actual_names != expected_names:
        raise PublishError(
            f"release_asset_inventory_mismatch:expected={sorted(expected_names)}:actual={sorted(actual_names)}"
        )
    for name, expected_digest in checksums.items():
        actual = sha256_file(root / name)
        if actual != expected_digest:
            raise PublishError(f"release_asset_checksum_mismatch:{name}")
    return [
        LocalAsset(path, path.name, path.stat().st_size, sha256_file(path))
        for path in sorted(root.iterdir(), key=lambda item: item.name)
    ]


class RollingPublisher:
    def __init__(
        self,
        runner: Runner,
        *,
        repository: str,
        target_sha: str,
        run_id: str,
        run_attempt: str,
        title: str,
        notes_file: Path,
        checkout_root: Path,
        release_scope: str = "all",
        gh_bin: str = "gh",
        git_bin: str = "git",
        curl_bin: str = "curl",
        github_token: str | None = None,
    ) -> None:
        if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) is None:
            raise PublishError("repository_invalid")
        if re.fullmatch(r"[0-9a-fA-F]{40,64}", target_sha) is None:
            raise PublishError("target_sha_invalid")
        if not run_id.isdigit() or not run_attempt.isdigit():
            raise PublishError("run_identity_invalid")
        if notes_file.is_symlink() or not notes_file.is_file():
            raise PublishError(f"notes_file_missing:{notes_file}")
        if checkout_root.is_symlink() or not checkout_root.is_dir():
            raise PublishError(f"checkout_root_invalid:{checkout_root}")
        try:
            self.contract = get_release_contract(release_scope)
        except ValueError as exc:
            raise PublishError(str(exc)) from exc
        self.runner = runner
        self.repository = repository
        self.target_sha = target_sha.lower()
        self.run_id = run_id
        self.run_attempt = run_attempt
        self.run_key = f"{run_id}-{run_attempt}"
        self.title = title
        self.notes_file = notes_file
        self.checkout_root = checkout_root.resolve()
        self.gh_bin = gh_bin
        self.git_bin = git_bin
        self.curl_bin = curl_bin
        self._github_token = github_token
        self._marker_predecessor_tag: str | None = None
        self.api_headers = [
            "-H",
            "Accept: application/vnd.github+json",
            "-H",
            f"X-GitHub-Api-Version: {API_VERSION}",
        ]

    def gh(self, *arguments: str, check: bool = True) -> CommandResult:
        return self.runner.run([self.gh_bin, *arguments], check=check)

    def git(self, *arguments: str, check: bool = True) -> CommandResult:
        return self.runner.run(
            [self.git_bin, "-C", str(self.checkout_root), *arguments], check=check
        )

    def curl(self, *arguments: str, check: bool = True) -> CommandResult:
        return self.runner.run([self.curl_bin, *arguments], check=check)

    @staticmethod
    def is_transient_api_failure(result: CommandResult) -> bool:
        detail = f"{result.stdout}\n{result.stderr}"
        return re.search(r"\b(?:429|500|502|503|504)\b", detail) is not None

    def api(
        self,
        endpoint: str,
        *arguments: str,
        check: bool = True,
        accept: str | None = None,
    ) -> CommandResult:
        method = "GET"
        if "--method" in arguments:
            method = arguments[arguments.index("--method") + 1].upper()
        attempts = API_READ_TRANSIENT_ATTEMPTS if method == "GET" else 1
        headers = (
            self.api_headers
            if accept is None
            else [
                "-H",
                f"Accept: {accept}",
                "-H",
                f"X-GitHub-Api-Version: {API_VERSION}",
            ]
        )
        command = [self.gh_bin, "api", *headers, endpoint, *arguments]
        for attempt in range(attempts):
            result = self.gh("api", *headers, endpoint, *arguments, check=False)
            if result.returncode == 0:
                return result
            transient = method == "GET" and self.is_transient_api_failure(result)
            if transient and attempt + 1 < attempts:
                time.sleep(API_READ_TRANSIENT_DELAY_SECONDS)
                continue
            detail = (result.stderr or result.stdout).strip()
            if transient:
                raise PublishError(
                    "github_api_read_transient_timeout:"
                    f"endpoint={endpoint}:attempts={attempts}:"
                    f"exit={result.returncode}:{detail}"
                )
            if check:
                raise PublishError(
                    f"command_failed:{command[0]}:exit={result.returncode}:{detail}"
                )
            return result
        raise PublishError(f"github_api_read_retry_unreachable:{endpoint}")

    @staticmethod
    def parse_json(result: CommandResult, context: str) -> dict[str, Any]:
        try:
            value = json.loads(result.stdout)
        except json.JSONDecodeError as exc:
            raise PublishError(f"github_json_invalid:{context}:{exc}") from exc
        if not isinstance(value, dict):
            raise PublishError(f"github_json_not_object:{context}")
        return value

    @staticmethod
    def parse_json_array(result: CommandResult, context: str) -> list[dict[str, Any]]:
        try:
            value = json.loads(result.stdout)
        except json.JSONDecodeError as exc:
            raise PublishError(f"github_json_invalid:{context}:{exc}") from exc
        if not isinstance(value, list) or any(not isinstance(row, dict) for row in value):
            raise PublishError(f"github_json_not_array_of_objects:{context}")
        return value

    @staticmethod
    def release_id(release: dict[str, Any]) -> int:
        release_id = release.get("id")
        if not isinstance(release_id, int) or release_id <= 0:
            raise PublishError("release_id_invalid")
        return release_id

    def release_marker(self) -> str:
        predecessor = self._marker_predecessor_tag or "absent"
        return (
            "<!-- scratchbird-rolling-nightly-managed:"
            f"schema={MANAGED_RELEASE_MARKER_SCHEMA};"
            f"scope={self.contract.scope};tag={self.contract.tag};"
            f"source={self.target_sha};run_id={self.run_id};"
            f"predecessor_tag={predecessor} -->"
        )

    def contract_managed_marker_details(
        self, body: str
    ) -> tuple[str, str, str | None] | None:
        pattern = (
            r"<!-- scratchbird-rolling-nightly-managed:"
            rf"schema={re.escape(MANAGED_RELEASE_MARKER_SCHEMA)};"
            rf"scope={re.escape(self.contract.scope)};"
            rf"tag={re.escape(self.contract.tag)};"
            r"source=([0-9a-f]{40,64});run_id=([0-9]+);"
            r"predecessor_tag=(absent|[0-9a-f]{40,64}) -->"
        )
        matches = re.findall(pattern, body)
        if len(matches) != 1:
            return None
        source, run_id, predecessor = matches[0]
        return source, run_id, None if predecessor == "absent" else predecessor

    def contract_managed_marker_identity(self, body: str) -> tuple[str, str] | None:
        details = self.contract_managed_marker_details(body)
        return None if details is None else details[:2]

    def has_contract_managed_marker(self, body: str) -> bool:
        return self.contract_managed_marker_identity(body) is not None

    def has_current_run_marker(self, body: str) -> bool:
        return self.contract_managed_marker_identity(body) == (
            self.target_sha,
            self.run_id,
        )

    def legacy_draft_provenance(self, body: str) -> tuple[str, str] | None:
        """Parse the exact pre-marker Actions release-note provenance."""

        source = r"(?P<source>[0-9a-f]{40,64})"
        version = r"(?P<version>[0-9A-Za-z.+~_-]+)"
        run_id = r"(?P<run_id>[0-9]+)"
        repository = re.escape(self.repository)
        if self.contract.scope == "all":
            pattern = (
                r"ScratchBird native nightly build for testing[.]\n\n"
                r"- Native ScratchBird platform only: SBmgr, SBgate, SBParser using native SBSQL, and SBsrv are included[.]\n"
                r"- No compatibility parser or emulation packages are included[.]\n"
                r"- The native ScratchBird listener default is TCP port 3092[.]\n"
                r"- Includes the default local-password policy pack plus charset, collation, timezone, and native SBSQL language resources[.]\n"
                r"- LLVM is mandatory: Linux portable archives require libllvm23/llvm-libs 23[+], Windows archives bundle the LLVM DLL closure, and macOS QA archives require Homebrew llvm 22[+][.]\n"
                r"- Public nightly assets include fully verified portable archives and system installer packages[.]\n"
                r"- DEB, RPM, AUR, PKG, and MSI packages are published for tester installation after their platform verification and install-smoke jobs pass[.]\n"
                rf"- Source commit: {source}\n"
                rf"- Requested version: {version}\n"
                rf"- Workflow run: https://github[.]com/{repository}/actions/runs/{run_id}\n"
                r"- Linux, Windows, and macOS build, CTest, package verification, and smoke-install jobs completed successfully before publication[.]\n"
                r"- macOS QA packages are unsigned and unnotarized unless the manifest explicitly records signed payloads[.]\n"
                r"\nVerify downloads with scratchbird-nightly-SHA256SUMS[.]\n"
            )
        else:
            scope = re.escape(self.contract.scope)
            pattern = (
                rf"ScratchBird native {scope} nightly build for testing[.]\n\n"
                r"- This is a platform-scoped rolling prerelease, not the complete cross-platform `nightly` release[.]\n"
                rf"- Only the independently built, CTest-verified, package-verified, and install-smoke-verified {scope} assets in this release are approved for testing[.]\n"
                r"- Native ScratchBird only: SBmgr, SBgate, SBParser using native SBSQL, and SBsrv are included[.]\n"
                r"- No compatibility parser or emulation package is included[.]\n"
                r"- The native ScratchBird listener default is TCP port 3092[.]\n"
                r"- Includes the default local-password policy pack plus charset, collation, timezone, and native SBSQL language resources[.]\n"
                r"- The `scratchbird` operating-system identity is a headless service account only; it is not a database or security authority[.]\n"
                rf"- Source commit: {source}\n"
                rf"- Requested version: {version}\n"
                rf"- Verified workflow run: https://github[.]com/{repository}/actions/runs/{run_id}\n"
                r"\nVerify downloads with the platform-specific SHA256SUMS asset[.]\n"
            )
        match = re.fullmatch(pattern, body)
        if match is None:
            return None
        return match.group("source"), match.group("run_id")

    def with_release_marker(self, body: str) -> str:
        marker = self.release_marker()
        if marker in body:
            return body
        return f"{body.rstrip()}\n\n{marker}\n"

    def get_release_by_id(
        self, release_id: int, *, allow_missing: bool = False
    ) -> dict[str, Any] | None:
        endpoint = f"repos/{self.repository}/releases/{release_id}"
        result = self.api(endpoint, check=not allow_missing)
        if result.returncode != 0:
            combined = f"{result.stdout}\n{result.stderr}"
            if allow_missing and ("404" in combined or "Not Found" in combined):
                return None
            raise PublishError(f"release_lookup_failed:{combined.strip()}")
        release = self.parse_json(result, "release")
        if self.release_id(release) != release_id:
            raise PublishError("release_lookup_id_mismatch")
        return release

    def list_releases(self) -> list[dict[str, Any]]:
        """Return every release record visible to this workflow token.

        GitHub deliberately hides draft releases from the tag endpoint.  The
        authenticated list endpoint is therefore the authority for detecting
        stale workflow drafts before any tag- or asset-mutating operation.
        """

        releases: list[dict[str, Any]] = []
        seen_ids: set[int] = set()
        for page in range(1, RELEASE_LIST_MAX_PAGES + 1):
            endpoint = (
                f"repos/{self.repository}/releases?per_page={RELEASE_LIST_PAGE_SIZE}"
                f"&page={page}"
            )
            rows = self.parse_json_array(self.api(endpoint), f"release_list:{page}")
            for row in rows:
                release_id = self.release_id(row)
                if release_id in seen_ids:
                    raise PublishError(f"release_list_id_duplicate:{release_id}")
                seen_ids.add(release_id)
                releases.append(row)
            if len(rows) < RELEASE_LIST_PAGE_SIZE:
                return releases
        raise PublishError(
            "release_list_page_limit_exceeded:"
            f"limit={RELEASE_LIST_MAX_PAGES}:page_size={RELEASE_LIST_PAGE_SIZE}"
        )

    def get_tag_sha(self) -> str | None:
        endpoint = f"repos/{self.repository}/git/ref/tags/{self.contract.tag}"
        result = self.api(endpoint, check=False)
        if result.returncode != 0:
            combined = f"{result.stdout}\n{result.stderr}"
            if "404" in combined or "Not Found" in combined:
                return None
            raise PublishError(f"tag_lookup_failed:{combined.strip()}")
        data = self.parse_json(result, "tag")
        obj = data.get("object")
        if not isinstance(obj, dict) or obj.get("type") != "commit" or not isinstance(obj.get("sha"), str):
            raise PublishError("nightly_tag_not_lightweight_commit")
        return str(obj["sha"]).lower()

    @staticmethod
    def is_retryable_initial_release_create_failure(result: CommandResult) -> bool:
        """Return whether an initial-create response can be reconciled safely.

        Generic non-GET API calls remain single-attempt operations.  A failed
        first release creation is the one exception because GitHub can return
        a false 404/5xx during an incident after accepting or rejecting the
        request.  The caller reconciles the release record before it retries,
        so it never sends another create request while a draft already exists.
        """

        detail = f"{result.stdout}\n{result.stderr}"
        return re.search(r"\b(?:404|429|500|502|503|504)\b", detail) is not None

    def is_managed_draft(self, release: dict[str, Any]) -> bool:
        """Return whether a private draft is provably owned by this workflow.

        New records carry an invisible, contract-bound marker.  The second
        branch is a deliberately narrow migration rule for drafts made by the
        prior publisher: only an Actions-bot draft with the exact scoped title,
        the two release-note provenance fields, and no assets except this
        publisher's staging prefixes can be reclaimed automatically.
        """

        if (
            release.get("tag_name") != self.contract.tag
            or release.get("draft") is not True
            or release.get("prerelease") is not True
            or release.get("name") != self.title
        ):
            return False
        author = release.get("author")
        if not isinstance(author, dict) or author.get("login") != MANAGED_RELEASE_AUTHOR:
            return False
        body = release.get("body")
        if not isinstance(body, str):
            return False
        assets = release.get("assets")
        if not isinstance(assets, list):
            return False
        names: list[str] = []
        for asset in assets:
            if not isinstance(asset, dict) or not isinstance(asset.get("name"), str):
                return False
            names.append(str(asset["name"]))

        def is_staging_name(name: str) -> bool:
            match = re.fullmatch(
                r"sb-nightly-(?:incoming|backup)-[0-9]+-[0-9]+--(.+)", name
            )
            return match is not None and match.group(1) in self.contract.canonical_asset_names

        if self.has_contract_managed_marker(body):
            return all(name in self.contract.canonical_asset_names or is_staging_name(name) for name in names)
        return self.legacy_draft_provenance(body) is not None and all(
            is_staging_name(name) for name in names
        )

    def discover_release_state(
        self,
    ) -> tuple[dict[str, Any] | None, list[dict[str, Any]], dict[str, Any] | None]:
        """Select at most one public release and inventory private siblings.

        Release-by-tag is not used here because GitHub intentionally returns
        404 for drafts.  An unknown same-tag draft is never touched: that is a
        concurrent or manual record the publisher is not authorized to mutate.
        """

        public: list[dict[str, Any]] = []
        legacy_drafts: list[dict[str, Any]] = []
        resumable_draft: dict[str, Any] | None = None
        for release in self.list_releases():
            if release.get("tag_name") != self.contract.tag:
                continue
            if release.get("draft") is False:
                public.append(release)
                continue
            if release.get("draft") is not True:
                raise PublishError("release_draft_state_invalid")
            if not self.is_managed_draft(release):
                raise PublishError("same_tag_draft_unmanaged")
            body = str(release.get("body") or "")
            if self.has_contract_managed_marker(body):
                if not self.has_current_run_marker(body):
                    raise PublishError("same_tag_draft_owned_by_other_run")
                if resumable_draft is not None:
                    raise PublishError("same_tag_current_run_draft_duplicate")
                resumable_draft = release
            else:
                provenance = self.legacy_draft_provenance(body)
                assert provenance is not None  # proved by is_managed_draft above
                if provenance == (self.target_sha, self.run_id):
                    if resumable_draft is not None:
                        raise PublishError("same_tag_current_run_draft_duplicate")
                    # The retired gh-release implementation sometimes stored
                    # the literal branch name (for example ``main``) here even
                    # though its release notes contain the immutable source SHA.
                    # For a legacy record, the exact bot/title/body/staging
                    # proof above is authoritative; the resume path immediately
                    # replaces this opaque field with the verified SHA and the
                    # current managed marker before it moves the tag.
                    resumable_draft = release
                else:
                    legacy_drafts.append(release)
        if len(public) > 1:
            raise PublishError("same_tag_public_release_duplicate")
        if public and resumable_draft is not None:
            raise PublishError("same_tag_public_and_current_run_draft")
        return (public[0] if public else None), legacy_drafts, resumable_draft

    def reconcile_initial_create_response(self) -> dict[str, Any] | None:
        """Find a draft accepted despite a failed create response.

        The list endpoint, not the tag endpoint, sees draft releases.  Polling
        it before another POST prevents false-404/5xx responses from creating
        duplicate hidden drafts.
        """

        for attempt in range(INITIAL_RELEASE_VISIBILITY_ATTEMPTS):
            candidates: list[dict[str, Any]] = []
            for release in self.list_releases():
                if release.get("tag_name") != self.contract.tag:
                    continue
                if release.get("draft") is not True or not self.is_managed_draft(release):
                    raise PublishError("initial_release_reconcile_same_tag_unmanaged")
                body = str(release.get("body") or "")
                if self.has_current_run_marker(body):
                    candidates.append(release)
                    continue
                if self.has_contract_managed_marker(body):
                    raise PublishError("initial_release_reconcile_other_run_draft")
                # A lost POST response can coexist with strict legacy drafts
                # that were discovered before this transaction.  They are
                # known stale siblings, not the newly-created marker record;
                # ignore them here and delete them only after a later leased
                # tag update has established a viable private transaction.
                provenance = self.legacy_draft_provenance(body)
                if provenance == (self.target_sha, self.run_id):
                    raise PublishError("initial_release_reconcile_legacy_current_run")
            if len(candidates) > 1:
                raise PublishError("initial_release_reconcile_duplicate_drafts")
            if len(candidates) == 1:
                release_id = self.release_id(candidates[0])
                observed = self.get_release_by_id(release_id)
                assert observed is not None
                return self.validate_initial_draft_release(observed)
            if attempt + 1 < INITIAL_RELEASE_VISIBILITY_ATTEMPTS:
                time.sleep(INITIAL_RELEASE_VISIBILITY_DELAY_SECONDS)
        return None

    def validate_initial_draft_release(self, release: dict[str, Any]) -> dict[str, Any]:
        if release.get("tag_name") != self.contract.tag:
            raise PublishError("initial_release_tag_mismatch")
        if release.get("draft") is not True or release.get("prerelease") is not True:
            raise PublishError("initial_release_not_draft_prerelease")
        if release.get("immutable") is not False:
            raise PublishError("initial_release_immutable_or_state_unknown")
        if not self.has_current_run_marker(str(release.get("body") or "")):
            raise PublishError("initial_release_marker_missing")
        # GitHub documents target_commitish as a tag-creation input.  Because
        # this publisher creates the lightweight tag first, GitHub may ignore
        # that input and report the repository default branch (for example
        # ``main``) on a perfectly valid draft.  The release marker and the
        # verified tag ref are the authoritative source binding instead.
        self.release_id(release)
        return release

    def wait_for_tag_target(self, target_sha: str) -> str:
        """Wait for GitHub's API to resolve the just-pushed lightweight tag.

        A successful ``git push`` can precede visibility of that ref to the
        GitHub Release API. Wait for the API to observe the exact expected
        target before creating a draft release, rather than treating that
        short propagation window as a publication failure.
        """

        last_observed = "absent"
        for attempt in range(TAG_VISIBILITY_ATTEMPTS):
            observed = self.get_tag_sha()
            if observed == target_sha:
                return observed
            if observed is not None:
                last_observed = observed
            if attempt + 1 < TAG_VISIBILITY_ATTEMPTS:
                time.sleep(TAG_VISIBILITY_DELAY_SECONDS)
        raise PublishError(
            "tag_visibility_timeout:"
            f"expected={target_sha}:last_observed={last_observed}:"
            f"attempts={TAG_VISIBILITY_ATTEMPTS}"
        )

    def create_initial_draft_release(self) -> dict[str, Any]:
        """Create the first draft through the documented REST endpoint.

        ``gh release create --verify-tag`` returned a false 404 for the
        platform-scoped tags even after ``get_tag_sha`` resolved the exact
        lightweight tag.  The REST endpoint accepts the already-verified tag
        and an explicit target SHA, while preserving the same draft-first
        publication and rollback protocol.
        """

        try:
            notes = self.notes_file.read_text(encoding="utf-8")
        except OSError as exc:
            raise PublishError(f"initial_release_notes_read_failed:{exc}") from exc
        payload = {
            "tag_name": self.contract.tag,
            "target_commitish": self.target_sha,
            "name": self.title,
            "body": self.with_release_marker(notes),
            "draft": True,
            "prerelease": True,
            "make_latest": "false",
        }
        with tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", delete=False
        ) as handle:
            json.dump(payload, handle, sort_keys=True)
            payload_path = Path(handle.name)
        try:
            for attempt in range(INITIAL_RELEASE_CREATE_ATTEMPTS):
                result = self.api(
                    f"repos/{self.repository}/releases",
                    "--method",
                    "POST",
                    "-H",
                    "Content-Type: application/json",
                    "--input",
                    str(payload_path),
                    check=False,
                )
                if result.returncode == 0:
                    return self.validate_initial_draft_release(
                        self.parse_json(result, "initial_release_create")
                    )

                # Reconcile before considering another POST.  The tag endpoint
                # hides draft releases, so query the authenticated release list
                # by contract instead.  A record that appeared after a failed
                # response is authoritative and makes retrying unsafe.
                observed = self.reconcile_initial_create_response()
                if observed is not None:
                    return self.validate_initial_draft_release(observed)

                detail = (result.stderr or result.stdout).strip()
                if (
                    not self.is_retryable_initial_release_create_failure(result)
                    or attempt + 1 >= INITIAL_RELEASE_CREATE_ATTEMPTS
                ):
                    raise PublishError(
                        "initial_release_create_failed:"
                        f"attempts={attempt + 1}:exit={result.returncode}:{detail}"
                    )
                time.sleep(INITIAL_RELEASE_CREATE_DELAY_SECONDS)
        finally:
            payload_path.unlink(missing_ok=True)
        raise PublishError("initial_release_create_retry_unreachable")

    def verify_origin_repository(self) -> None:
        result = self.git("remote", "get-url", "origin")
        remote = result.stdout.strip()
        patterns = (
            r"https://github[.]com/(?P<repo>[^/]+/[^/]+?)(?:[.]git)?/?$",
            r"git@github[.]com:(?P<repo>[^/]+/[^/]+?)(?:[.]git)?$",
            r"ssh://git@github[.]com/(?P<repo>[^/]+/[^/]+?)(?:[.]git)?/?$",
        )
        matched = next(
            (match for pattern in patterns if (match := re.fullmatch(pattern, remote))),
            None,
        )
        if matched is None or matched.group("repo").lower() != self.repository.lower():
            raise PublishError(
                f"origin_repository_mismatch:expected={self.repository}:actual={remote}"
            )

    @staticmethod
    def index_assets(release: dict[str, Any]) -> dict[str, dict[str, Any]]:
        rows = release.get("assets")
        if not isinstance(rows, list):
            raise PublishError("release_assets_not_list")
        indexed: dict[str, dict[str, Any]] = {}
        for row in rows:
            if not isinstance(row, dict) or not isinstance(row.get("name"), str) or not isinstance(row.get("id"), int):
                raise PublishError("release_asset_row_invalid")
            name = str(row["name"])
            if name in indexed:
                raise PublishError(f"release_asset_name_duplicate:{name}")
            indexed[name] = row
        return indexed

    def require_managed_release(self, release: dict[str, Any], *, tag_sha: str) -> None:
        author = release.get("author")
        if (
            release.get("tag_name") != self.contract.tag
            or release.get("draft") is not False
            or release.get("prerelease") is not True
            or release.get("name") != self.title
            or not isinstance(author, dict)
            or author.get("login") != MANAGED_RELEASE_AUTHOR
        ):
            raise PublishError("existing_nightly_release_unmanaged:metadata_mismatch")
        marker_identity = self.contract_managed_marker_identity(
            str(release.get("body") or "")
        )
        if marker_identity is None:
            raise PublishError("existing_nightly_release_unmanaged:marker_missing")
        marker = self.index_assets(release).get(self.contract.manifest_name)
        if marker is None:
            raise PublishError("existing_nightly_release_unmanaged:manifest_missing")
        asset_id = marker.get("id")
        if not isinstance(asset_id, int) or asset_id <= 0:
            raise PublishError("existing_nightly_release_unmanaged:manifest_id_invalid")
        result = self.api(
            f"repos/{self.repository}/releases/assets/{asset_id}",
            accept="application/octet-stream",
        )
        manifest_text = result.stdout
        manifest_bytes = manifest_text.encode("utf-8")
        if (
            marker.get("state") != "uploaded"
            or marker.get("size") != len(manifest_bytes)
        ):
            raise PublishError("existing_nightly_release_unmanaged:manifest_metadata_mismatch")
        if marker.get("digest") != f"sha256:{hashlib.sha256(manifest_bytes).hexdigest()}":
            raise PublishError("existing_nightly_release_unmanaged:manifest_digest_mismatch")
        try:
            manifest = json.loads(manifest_text)
        except json.JSONDecodeError as exc:
            raise PublishError(
                f"existing_nightly_release_unmanaged:manifest_invalid:{exc}"
            ) from exc
        if (
            not isinstance(manifest, dict)
            or manifest.get("schema_id") != MANIFEST_SCHEMA
            or manifest.get("release_scope") != self.contract.scope
            or manifest.get("release_tag") != self.contract.tag
            or manifest.get("distribution_surface") != "scratchbird_native_no_emulation"
            or manifest.get("public_asset_policy")
            != PUBLIC_ASSET_POLICY
            or manifest.get("native_parser") != "SBSQL"
            or manifest.get("emulation_layers_included") is not False
            or manifest.get("native_components") != NATIVE_COMPONENTS
            or manifest.get("included_platforms")
            != (
                ["linux", "windows", "macos"]
                if self.contract.scope == "all"
                else [self.contract.scope]
            )
        ):
            raise PublishError("existing_nightly_release_unmanaged:identity_mismatch")
        marker_source, marker_run_id = marker_identity
        if (
            str(manifest.get("source_revision") or "").lower() != marker_source
            or str(manifest.get("github_run_id") or "") != marker_run_id
        ):
            raise PublishError("existing_nightly_release_unmanaged:marker_provenance_mismatch")
        if tag_sha != marker_source:
            raise PublishError("existing_nightly_tag_provenance_mismatch")

    def delete_new_draft_release(self, release: dict[str, Any]) -> None:
        if release.get("draft") is not True or not self.is_managed_draft(release):
            raise PublishError("new_release_cleanup_refused_not_draft")
        release_id = self.release_id(release)
        endpoint = f"repos/{self.repository}/releases/{release_id}"
        self.api(endpoint, "--method", "DELETE")

    @staticmethod
    def verify_remote_asset(row: dict[str, Any], local: LocalAsset, remote_name: str) -> None:
        if row.get("name") != remote_name:
            raise PublishError(f"remote_asset_name_mismatch:{remote_name}")
        if row.get("state") != "uploaded":
            raise PublishError(f"remote_asset_state_invalid:{remote_name}:{row.get('state')}")
        if row.get("size") != local.size:
            raise PublishError(f"remote_asset_size_mismatch:{remote_name}")
        if row.get("digest") != f"sha256:{local.digest}":
            raise PublishError(f"remote_asset_digest_mismatch:{remote_name}")

    def rename_asset(self, asset_id: int, new_name: str) -> None:
        endpoint = f"repos/{self.repository}/releases/assets/{asset_id}"
        self.api(endpoint, "--method", "PATCH", "-f", f"name={new_name}")

    def delete_asset(self, asset_id: int) -> None:
        endpoint = f"repos/{self.repository}/releases/assets/{asset_id}"
        self.api(endpoint, "--method", "DELETE")

    def release_asset_upload_endpoint(
        self, release: dict[str, Any], asset_name: str
    ) -> str:
        release_id = self.release_id(release)
        upload_url = release.get("upload_url")
        expected_prefix = (
            f"https://uploads.github.com/repos/{self.repository}/releases/"
            f"{release_id}/assets"
        )
        if not isinstance(upload_url, str) or not upload_url.startswith(expected_prefix):
            raise PublishError("release_upload_url_invalid")
        endpoint = upload_url.split("{", 1)[0]
        if endpoint != expected_prefix:
            raise PublishError("release_upload_url_template_invalid")
        return f"{endpoint}?name={quote(asset_name, safe='')}"

    def get_github_token(self) -> str:
        token = self._github_token or os.environ.get("GH_TOKEN") or os.environ.get(
            "GITHUB_TOKEN"
        )
        if not token:
            raise PublishError("github_token_missing_for_release_asset_upload")
        return token

    @staticmethod
    def is_retryable_release_asset_upload_failure(result: CommandResult) -> bool:
        detail = f"{result.stdout}\n{result.stderr}"
        return re.search(r"\b(?:404|429|500|502|503|504)\b", detail) is not None

    def upload_incoming_asset(
        self,
        release_id: int,
        incoming_name: str,
        local: LocalAsset,
    ) -> None:
        """Upload one unique staging asset to an explicit draft-release ID.

        GitHub documents a 502 as potentially leaving a ``starter`` asset.
        Every retry therefore reads that exact release ID first, accepts only a
        byte-identical uploaded asset, deletes only a same-run starter asset,
        and never issues a blind duplicate upload.
        """

        for attempt in range(RELEASE_ASSET_UPLOAD_ATTEMPTS):
            current = self.get_release_by_id(release_id)
            assert current is not None
            current_asset = self.index_assets(current).get(incoming_name)
            if current_asset is not None:
                if current_asset.get("state") == "starter":
                    self.delete_asset(int(current_asset["id"]))
                else:
                    self.verify_remote_asset(current_asset, local, incoming_name)
                    return

            endpoint = self.release_asset_upload_endpoint(current, incoming_name)
            token = self.get_github_token()
            if "\n" in token or "\r" in token or '"' in token or "\\" in token:
                raise PublishError("github_token_invalid_for_curl_config")
            with tempfile.NamedTemporaryFile(
                "w", encoding="utf-8", delete=False
            ) as handle:
                os.fchmod(handle.fileno(), 0o600)
                handle.write(f'header = "Authorization: Bearer {token}"\n')
                curl_config = Path(handle.name)
            try:
                result = self.curl(
                    "--disable",
                    "--config",
                    str(curl_config),
                    "--fail-with-body",
                    "--silent",
                    "--show-error",
                    "--request",
                    "POST",
                    "--header",
                    "Accept: application/vnd.github+json",
                    "--header",
                    f"X-GitHub-Api-Version: {API_VERSION}",
                    "--header",
                    "Content-Type: application/octet-stream",
                    "--data-binary",
                    f"@{local.path}",
                    endpoint,
                    check=False,
                )
            finally:
                curl_config.unlink(missing_ok=True)
            if result.returncode == 0:
                uploaded = self.parse_json(result, "release_asset_upload")
                self.verify_remote_asset(uploaded, local, incoming_name)
                return

            # The upload response can be lost after GitHub accepted the bytes.
            # Reconcile by release ID before any retry or starter cleanup.
            refreshed = self.get_release_by_id(release_id)
            assert refreshed is not None
            observed = self.index_assets(refreshed).get(incoming_name)
            if observed is not None:
                if observed.get("state") == "starter":
                    self.delete_asset(int(observed["id"]))
                else:
                    self.verify_remote_asset(observed, local, incoming_name)
                    return
            if (
                not self.is_retryable_release_asset_upload_failure(result)
                or attempt + 1 >= RELEASE_ASSET_UPLOAD_ATTEMPTS
            ):
                detail = (result.stderr or result.stdout).strip()
                raise PublishError(
                    "release_asset_upload_failed:"
                    f"name={incoming_name}:attempts={attempt + 1}:"
                    f"exit={result.returncode}:{detail}"
                )
            time.sleep(RELEASE_ASSET_UPLOAD_DELAY_SECONDS)

    def update_tag(
        self,
        sha: str,
        expected_current_sha: str | None,
        *,
        dry_run: bool = False,
        check: bool = True,
    ) -> CommandResult:
        """Compare-and-swap the rolling tag to ``sha``.

        Release workflow concurrency serializes Actions runs, but it cannot
        serialize a direct ref mutation.  The lease makes every write fail
        closed if the value observed at transaction start has changed.
        """

        expected = expected_current_sha or ""
        arguments = ["push"]
        if dry_run:
            arguments.append("--dry-run")
        arguments.extend(
            (
                f"--force-with-lease=refs/tags/{self.contract.tag}:{expected}",
                "origin",
                f"{sha}:refs/tags/{self.contract.tag}",
            )
        )
        return self.git(*arguments, check=check)

    def delete_tag(
        self, expected_current_sha: str, *, check: bool = True
    ) -> CommandResult:
        return self.git(
            "push",
            f"--force-with-lease=refs/tags/{self.contract.tag}:{expected_current_sha}",
            "origin",
            f":refs/tags/{self.contract.tag}",
            check=check,
        )

    def restore_tag_if_transaction_owned(self, old_tag_sha: str | None) -> str | None:
        """Restore a rolling tag only when it still points to this transaction.

        A failed force-push can race with a manual or independently authorized
        ref update.  Never delete or overwrite that later ref during recovery:
        the release remains private/rolled back and the explicit error directs
        an operator to reconcile the tag intentionally.
        """

        if old_tag_sha is None:
            result = self.delete_tag(self.target_sha, check=False)
        else:
            result = self.update_tag(old_tag_sha, self.target_sha, check=False)
        if result.returncode == 0:
            return None
        try:
            current_tag_sha = self.get_tag_sha()
        except Exception as exc:
            return f"tag_drift_during_recovery:lease_check_failed:{exc}"
        # A response can be lost after Git accepted the lease-protected push.
        # The desired restored ref proves that recovery did complete.
        if current_tag_sha == old_tag_sha:
            return None
        observed = current_tag_sha if current_tag_sha is not None else "absent"
        return (
            "tag_drift_during_recovery:"
            f"expected={self.target_sha}:actual={observed}:"
            f"lease_exit={result.returncode}"
        )

    def patch_release(
        self,
        release_id: int,
        *,
        draft: bool,
        prerelease: bool,
        title: str,
        body: str,
        target: str,
    ) -> dict[str, Any]:
        payload = {
            "name": title,
            "body": body,
            "draft": draft,
            "prerelease": prerelease,
            "make_latest": "false",
            "target_commitish": target,
        }
        with tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", delete=False
        ) as handle:
            json.dump(payload, handle, sort_keys=True)
            payload_path = Path(handle.name)
        try:
            result = self.api(
                f"repos/{self.repository}/releases/{release_id}",
                "--method",
                "PATCH",
                "-H",
                "Content-Type: application/json",
                "--input",
                str(payload_path),
            )
        finally:
            payload_path.unlink(missing_ok=True)
        updated = self.parse_json(result, "release_edit")
        if self.release_id(updated) != release_id:
            raise PublishError("release_edit_id_mismatch")
        return updated

    def edit_release(
        self,
        release_id: int,
        *,
        draft: bool,
        prerelease: bool,
        title: str,
        notes_file: Path,
        target: str,
    ) -> dict[str, Any]:
        try:
            notes = notes_file.read_text(encoding="utf-8")
        except OSError as exc:
            raise PublishError(f"release_notes_read_failed:{exc}") from exc
        return self.patch_release(
            release_id,
            draft=draft,
            prerelease=prerelease,
            title=title,
            body=self.with_release_marker(notes),
            target=target,
        )

    def verify_canonical_inventory(
        self,
        release_id: int,
        assets: list[LocalAsset],
        *,
        exact: bool = False,
    ) -> dict[str, Any]:
        release = self.get_release_by_id(release_id)
        if release is None:
            raise PublishError("release_disappeared_during_canonical_verification")
        indexed = self.index_assets(release)
        for local in assets:
            row = indexed.get(local.name)
            if row is None:
                raise PublishError(f"canonical_asset_missing:{local.name}")
            self.verify_remote_asset(row, local, local.name)
        if exact:
            expected_names = {asset.name for asset in assets}
            actual_names = set(indexed)
            if actual_names != expected_names:
                raise PublishError(
                    "canonical_asset_inventory_not_exact:"
                    f"expected={sorted(expected_names)}:actual={sorted(actual_names)}"
                )
        return release

    def cleanup_named_prefix(self, release_id: int, prefix: str) -> None:
        release = self.get_release_by_id(release_id, allow_missing=True)
        if release is None:
            return
        for name, row in self.index_assets(release).items():
            if name.startswith(prefix):
                self.delete_asset(int(row["id"]))

    def rollback_swaps(self, swaps: list[Swap]) -> list[str]:
        errors: list[str] = []
        for swap in reversed(swaps):
            try:
                self.rename_asset(swap.incoming_id, swap.incoming_name)
            except Exception as exc:  # best-effort recovery must continue
                errors.append(f"incoming_restore:{swap.canonical_name}:{exc}")
            if swap.old_id is not None:
                try:
                    self.rename_asset(swap.old_id, swap.canonical_name)
                except Exception as exc:  # best-effort recovery must continue
                    errors.append(f"backup_restore:{swap.canonical_name}:{exc}")
        return errors

    def edit_release_snapshot(
        self,
        release: dict[str, Any],
        *,
        draft_override: bool | None = None,
    ) -> None:
        title = str(release.get("name") or "ScratchBird Nightly Installers")
        body = str(release.get("body") or "")
        draft = bool(release.get("draft")) if draft_override is None else draft_override
        prerelease = bool(release.get("prerelease"))
        target = str(release.get("target_commitish") or self.target_sha)
        self.patch_release(
            self.release_id(release),
            draft=draft,
            prerelease=prerelease,
            title=title,
            body=body,
            target=target,
        )

    def restore_release_metadata(self, release: dict[str, Any]) -> list[str]:
        errors: list[str] = []
        try:
            self.edit_release_snapshot(release)
        except Exception as exc:
            errors.append(f"release_metadata_restore:{exc}")
        return errors

    def publish(self, assets: list[LocalAsset]) -> None:
        self.verify_origin_repository()
        release, legacy_drafts, resumable_draft = self.discover_release_state()
        old_tag_sha = self.get_tag_sha()
        self._marker_predecessor_tag = old_tag_sha
        resuming_initial = resumable_draft is not None

        if release is not None:
            release_id = self.release_id(release)
            release = self.get_release_by_id(release_id)
            assert release is not None
            if release.get("immutable") is not False:
                raise PublishError("rolling_release_immutable_or_state_unknown")
            if old_tag_sha is None:
                raise PublishError("rolling_release_tag_missing")
            self.require_managed_release(release, tag_sha=old_tag_sha)
        elif resuming_initial:
            assert resumable_draft is not None
            release_id = self.release_id(resumable_draft)
            release = self.get_release_by_id(release_id)
            assert release is not None
            resume_body = str(release.get("body") or "")
            resume_legacy = (
                not self.has_contract_managed_marker(resume_body)
                and self.legacy_draft_provenance(resume_body)
                == (self.target_sha, self.run_id)
            )
            if (
                release.get("immutable") is not False
                or release.get("draft") is not True
                or (not self.has_current_run_marker(resume_body) and not resume_legacy)
            ):
                raise PublishError("current_run_draft_resume_invalid")
            marker_details = self.contract_managed_marker_details(resume_body)
            if marker_details is not None:
                predecessor_tag = marker_details[2]
                if old_tag_sha not in {self.target_sha, predecessor_tag}:
                    raise PublishError("current_run_draft_tag_provenance_mismatch")
            if resume_legacy and old_tag_sha not in {None, self.target_sha}:
                raise PublishError("current_legacy_draft_tag_provenance_mismatch")
        elif old_tag_sha is not None:
            raise PublishError("existing_nightly_tag_unmanaged")

        # Validate that the tag force-update is authorized before reclaiming
        # any legacy private draft.  A failed preflight must not discard a
        # recoverable draft when publication cannot proceed.
        self.update_tag(self.target_sha, old_tag_sha, dry_run=True)

        original_release = (
            dict(release) if release is not None and not resuming_initial else None
        )
        release_id: int | None = self.release_id(release) if release is not None else None
        incoming_prefix = f"sb-nightly-incoming-{self.run_key}--"
        backup_prefix = f"sb-nightly-backup-{self.run_key}--"
        expected_names = {asset.name for asset in assets}
        swaps: list[Swap] = []
        draft_created_this_attempt = False
        resumed_existing_draft = resuming_initial
        tag_update_attempted = False
        tag_expected_sha = old_tag_sha
        irreversible_cleanup_started = False
        committed = False

        try:
            if release is None:
                tag_update_attempted = True
                self.update_tag(self.target_sha, tag_expected_sha)
                self.wait_for_tag_target(self.target_sha)
                tag_expected_sha = self.target_sha
                release = self.create_initial_draft_release()
                if release.get("immutable") is not False or release.get("draft") is not True:
                    raise PublishError("first_release_not_mutable_draft")
                draft_created_this_attempt = True
                release_id = self.release_id(release)
            elif resuming_initial:
                assert release_id is not None
                if not self.has_current_run_marker(str(release.get("body") or "")):
                    # A same-source/run legacy draft is safe to promote in
                    # place; all other legacy records were either pruned under
                    # the strict migration rule or rejected before mutation.
                    release = self.patch_release(
                        release_id,
                        draft=True,
                        prerelease=True,
                        title=self.title,
                        body=self.with_release_marker(str(release.get("body") or "")),
                        target=self.target_sha,
                    )
                tag_update_attempted = True
                self.update_tag(self.target_sha, tag_expected_sha)
                self.wait_for_tag_target(self.target_sha)
                tag_expected_sha = self.target_sha
                resumed = self.get_release_by_id(release_id)
                if (
                    resumed is None
                    or resumed.get("draft") is not True
                    or not self.has_current_run_marker(str(resumed.get("body") or ""))
                ):
                    raise PublishError("current_run_draft_resume_lost")
            elif release.get("draft") is not True:
                assert release_id is not None
                # Move the private working copy to this transaction's marker
                # before byte replacement.  A failure before cleanup restores
                # ``original_release`` verbatim; a later failure leaves an
                # exact, current-run draft that a retry can resume by ID.
                self.edit_release(
                    release_id,
                    draft=True,
                    prerelease=True,
                    title=self.title,
                    notes_file=self.notes_file,
                    target=self.target_sha,
                )
                hidden = self.get_release_by_id(release_id)
                if (
                    hidden is None
                    or hidden.get("draft") is not True
                    or not self.has_current_run_marker(str(hidden.get("body") or ""))
                ):
                    raise PublishError("existing_release_draft_transition_failed")
                # Once a managed public release has become private, move the
                # tag under a lease before any byte staging.  Consequently a
                # recoverable current-run marker always pairs with the target
                # tag; a retry never infers authority from an arbitrary ref.
                tag_update_attempted = True
                self.update_tag(self.target_sha, tag_expected_sha)
                self.wait_for_tag_target(self.target_sha)
                tag_expected_sha = self.target_sha

            # These pre-marker records passed the narrow bot/title/exact-note/
            # staging-name proof during discovery.  Delete them only after a
            # lease-protected tag update and an active private draft exist, so
            # a ref race cannot discard a recoverable draft before a viable
            # replacement transaction is established.
            for legacy in legacy_drafts:
                stale = self.get_release_by_id(self.release_id(legacy))
                if stale is None:
                    raise PublishError("legacy_draft_disappeared_before_cleanup")
                self.delete_new_draft_release(stale)

            if release_id is None:
                raise PublishError("active_release_id_missing")
            ordered_assets = sorted(
                assets,
                key=lambda asset: (
                    2
                    if asset.name == self.contract.checksum_name
                    else 1 if asset.name == self.contract.manifest_name else 0,
                    asset.name,
                ),
            )
            local_by_incoming = {
                f"{incoming_prefix}{local.name}": local for local in ordered_assets
            }
            for incoming_name, local in local_by_incoming.items():
                self.upload_incoming_asset(release_id, incoming_name, local)

            uploaded_release = self.get_release_by_id(release_id)
            if uploaded_release is None:
                raise PublishError("release_disappeared_after_upload")
            uploaded_index = self.index_assets(uploaded_release)
            for incoming_name, local in local_by_incoming.items():
                row = uploaded_index.get(incoming_name)
                if row is None:
                    raise PublishError(f"incoming_asset_missing:{incoming_name}")
                self.verify_remote_asset(row, local, incoming_name)

            for local in ordered_assets:
                incoming_name = f"{incoming_prefix}{local.name}"
                current_release = self.get_release_by_id(release_id)
                if current_release is None:
                    raise PublishError("release_disappeared_during_asset_swap")
                current = self.index_assets(current_release)
                incoming = current.get(incoming_name)
                if incoming is None:
                    raise PublishError(f"incoming_asset_disappeared:{incoming_name}")
                old = current.get(local.name)
                if (
                    old is not None
                    and old.get("state") == "uploaded"
                    and old.get("size") == local.size
                    and old.get("digest") == f"sha256:{local.digest}"
                ):
                    self.delete_asset(int(incoming["id"]))
                    continue
                backup_name: str | None = None
                old_id: int | None = None
                if old is not None:
                    old_id = int(old["id"])
                    backup_name = f"{backup_prefix}{local.name}"
                swaps.append(
                    Swap(local.name, incoming_name, int(incoming["id"]), old_id, backup_name)
                )
                if old_id is not None:
                    self.rename_asset(old_id, str(backup_name))
                self.rename_asset(int(incoming["id"]), local.name)

            self.verify_canonical_inventory(release_id, assets)
            # Deleting the previous generation is the point of no return for an
            # existing release. It happens while the release is still draft, so
            # backup or stale assets can never become publicly visible beside the
            # new canonical generation. A later failure leaves the new generation
            # draft for a clean retry instead of attempting an incomplete rollback.
            cleanup_release = self.get_release_by_id(release_id)
            if cleanup_release is None:
                raise PublishError("release_disappeared_before_private_cleanup")
            irreversible_cleanup_started = original_release is not None
            for name, row in self.index_assets(cleanup_release).items():
                if name not in expected_names:
                    self.delete_asset(int(row["id"]))
            self.verify_canonical_inventory(release_id, assets, exact=True)
            # Asset staging and rename can take long enough for another actor
            # to move the rolling tag.  Reassert and verify it immediately
            # before changing the release from private draft to public, on
            # every transaction path (fresh, resumed, and replacement).
            tag_update_attempted = True
            self.update_tag(self.target_sha, tag_expected_sha)
            self.wait_for_tag_target(self.target_sha)
            tag_expected_sha = self.target_sha
            self.edit_release(
                release_id,
                draft=False,
                prerelease=True,
                title=self.title,
                notes_file=self.notes_file,
                target=self.target_sha,
            )
            published = self.verify_canonical_inventory(release_id, assets, exact=True)
            if (
                published.get("draft") is not False
                or published.get("prerelease") is not True
                or published.get("tag_name") != self.contract.tag
                or published.get("name") != self.title
                or not self.has_current_run_marker(str(published.get("body") or ""))
            ):
                raise PublishError("published_release_state_mismatch")
            self.wait_for_tag_target(self.target_sha)
            committed = True
        except Exception as exc:
            recovery_errors: list[str] = []
            if not committed:
                current_release: dict[str, Any] | None = None
                if release_id is not None:
                    try:
                        current_release = self.get_release_by_id(
                            release_id, allow_missing=True
                        )
                    except Exception as lookup_exc:
                        recovery_errors.append(f"release_lookup_during_recovery:{lookup_exc}")
                if current_release is not None and current_release.get("draft") is not True:
                    try:
                        self.edit_release_snapshot(current_release, draft_override=True)
                    except Exception as hide_exc:
                        recovery_errors.append(f"release_hide_before_rollback:{hide_exc}")
                if resumed_existing_draft or irreversible_cleanup_started:
                    # Previous-generation bytes may already be deleted. Keep the
                    # exact current generation private and retryable; never
                    # republish a partial rollback or restore a tag that would
                    # disagree with it.  A resumed draft may already contain a
                    # coherent generation from a prior interrupted attempt.
                    recovery_errors.append("coherent_draft_retry_required")
                else:
                    recovery_errors.extend(self.rollback_swaps(swaps))
                if (
                    original_release is None
                    and tag_update_attempted
                    and not resumed_existing_draft
                    and (draft_created_this_attempt or release_id is None)
                ):
                    try:
                        current_release = (
                            self.get_release_by_id(release_id, allow_missing=True)
                            if release_id is not None
                            else None
                        )
                        if current_release is not None:
                            assert release_id is not None
                            self.cleanup_named_prefix(release_id, incoming_prefix)
                            self.delete_new_draft_release(current_release)
                        tag_recovery = self.restore_tag_if_transaction_owned(old_tag_sha)
                        if tag_recovery is not None:
                            recovery_errors.append(tag_recovery)
                    except Exception as initial_restore_exc:
                        recovery_errors.append(f"initial_release_restore:{initial_restore_exc}")
                elif not resumed_existing_draft and not irreversible_cleanup_started:
                    if tag_update_attempted:
                        try:
                            tag_recovery = self.restore_tag_if_transaction_owned(
                                old_tag_sha
                            )
                            if tag_recovery is not None:
                                recovery_errors.append(tag_recovery)
                        except Exception as restore_exc:
                            recovery_errors.append(f"tag_restore:{restore_exc}")
                    try:
                        assert release_id is not None
                        self.cleanup_named_prefix(release_id, incoming_prefix)
                    except Exception as cleanup_exc:
                        recovery_errors.append(f"incoming_cleanup:{cleanup_exc}")
                    if original_release is not None and not recovery_errors:
                        recovery_errors.extend(
                            self.restore_release_metadata(original_release)
                        )
                # Never delete backup-prefixed assets on the failure path. A
                # failed restore must leave the last-good bytes recoverable.
            detail = f":recovery_errors={'|'.join(recovery_errors)}" if recovery_errors else ""
            if isinstance(exc, PublishError):
                raise PublishError(f"{exc}{detail}") from exc
            raise PublishError(f"publication_failed:{exc}{detail}") from exc

        if release_id is None:
            raise PublishError("final_release_id_missing")
        final_release = self.verify_canonical_inventory(release_id, assets, exact=True)
        final_names = set(self.index_assets(final_release))
        if final_names != expected_names:
            raise PublishError(
                f"final_release_inventory_mismatch:expected={sorted(expected_names)}:actual={sorted(final_names)}"
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--asset-root", type=Path, required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--target-sha", required=True)
    parser.add_argument("--github-run-id", required=True)
    parser.add_argument("--github-run-attempt", required=True)
    parser.add_argument("--release-scope", choices=tuple(RELEASE_CONTRACTS), default="all")
    parser.add_argument("--title", default="ScratchBird Native Nightly Builds")
    parser.add_argument("--notes-file", type=Path, required=True)
    parser.add_argument("--checkout-root", type=Path, required=True)
    parser.add_argument("--gh-bin", default="gh")
    parser.add_argument("--git-bin", default="git")
    args = parser.parse_args()
    try:
        assets = load_local_assets(
            args.asset_root,
            args.target_sha,
            args.github_run_id,
            args.github_run_attempt,
            args.release_scope,
        )
        publisher = RollingPublisher(
            SubprocessRunner(),
            repository=args.repository,
            target_sha=args.target_sha,
            run_id=args.github_run_id,
            run_attempt=args.github_run_attempt,
            title=args.title,
            notes_file=args.notes_file,
            checkout_root=args.checkout_root,
            release_scope=args.release_scope,
            gh_bin=args.gh_bin,
            git_bin=args.git_bin,
        )
        publisher.publish(assets)
    except PublishError as exc:
        print(f"publish_rolling_nightly=fail:{exc}", file=sys.stderr)
        return 1
    contract = get_release_contract(args.release_scope)
    print(
        "publish_rolling_nightly=passed:"
        f"https://github.com/{args.repository}/releases/tag/{contract.tag}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
