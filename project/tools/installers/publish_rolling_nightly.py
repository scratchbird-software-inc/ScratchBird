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
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Protocol

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
        self.api_headers = ["-H", f"X-GitHub-Api-Version: {API_VERSION}"]

    def gh(self, *arguments: str, check: bool = True) -> CommandResult:
        return self.runner.run([self.gh_bin, *arguments], check=check)

    def git(self, *arguments: str, check: bool = True) -> CommandResult:
        return self.runner.run(
            [self.git_bin, "-C", str(self.checkout_root), *arguments], check=check
        )

    def api(self, endpoint: str, *arguments: str, check: bool = True) -> CommandResult:
        return self.gh("api", *self.api_headers, endpoint, *arguments, check=check)

    @staticmethod
    def parse_json(result: CommandResult, context: str) -> dict[str, Any]:
        try:
            value = json.loads(result.stdout)
        except json.JSONDecodeError as exc:
            raise PublishError(f"github_json_invalid:{context}:{exc}") from exc
        if not isinstance(value, dict):
            raise PublishError(f"github_json_not_object:{context}")
        return value

    def get_release(self, *, allow_missing: bool = False) -> dict[str, Any] | None:
        endpoint = f"repos/{self.repository}/releases/tags/{self.contract.tag}"
        result = self.api(endpoint, check=not allow_missing)
        if result.returncode != 0:
            combined = f"{result.stdout}\n{result.stderr}"
            if allow_missing and ("404" in combined or "Not Found" in combined):
                return None
            raise PublishError(f"release_lookup_failed:{combined.strip()}")
        return self.parse_json(result, "release")

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

    def require_managed_release(self, release: dict[str, Any]) -> None:
        marker = self.index_assets(release).get(self.contract.manifest_name)
        if marker is None:
            raise PublishError("existing_nightly_release_unmanaged:manifest_missing")
        with tempfile.TemporaryDirectory(prefix="scratchbird-nightly-managed-proof-") as temp_name:
            self.gh(
                "release",
                "download",
                self.contract.tag,
                "--repo",
                self.repository,
                "--pattern",
                self.contract.manifest_name,
                "--dir",
                temp_name,
            )
            path = Path(temp_name) / self.contract.manifest_name
            if not path.is_file():
                raise PublishError("existing_nightly_release_unmanaged:manifest_download_missing")
            if marker.get("state") != "uploaded" or marker.get("size") != path.stat().st_size:
                raise PublishError("existing_nightly_release_unmanaged:manifest_metadata_mismatch")
            if marker.get("digest") != f"sha256:{sha256_file(path)}":
                raise PublishError("existing_nightly_release_unmanaged:manifest_digest_mismatch")
            try:
                manifest = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
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

    def delete_new_draft_release(self, release: dict[str, Any]) -> None:
        release_id = release.get("id")
        if not isinstance(release_id, int) or release.get("draft") is not True:
            raise PublishError("new_release_cleanup_refused_not_draft")
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

    def update_tag(self, sha: str, *, dry_run: bool = False) -> None:
        arguments = ["push"]
        if dry_run:
            arguments.append("--dry-run")
        arguments.extend(("origin", f"{sha}:refs/tags/{self.contract.tag}", "--force"))
        self.git(*arguments)

    def delete_tag(self) -> None:
        self.git("push", "origin", f":refs/tags/{self.contract.tag}")

    def edit_release(self, *, draft: bool, prerelease: bool, title: str, notes_file: Path, target: str) -> None:
        self.gh(
            "release",
            "edit",
            self.contract.tag,
            "--repo",
            self.repository,
            "--title",
            title,
            "--notes-file",
            str(notes_file),
            f"--draft={'true' if draft else 'false'}",
            f"--prerelease={'true' if prerelease else 'false'}",
            "--latest=false",
            "--target",
            target,
        )

    def verify_canonical_inventory(
        self,
        assets: list[LocalAsset],
        *,
        exact: bool = False,
    ) -> dict[str, Any]:
        release = self.get_release()
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

    def cleanup_named_prefix(self, prefix: str) -> None:
        release = self.get_release(allow_missing=True)
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
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False) as handle:
            handle.write(body)
            notes = Path(handle.name)
        try:
            self.edit_release(
                draft=draft,
                prerelease=prerelease,
                title=title,
                notes_file=notes,
                target=target,
            )
        finally:
            notes.unlink(missing_ok=True)

    def restore_release_metadata(self, release: dict[str, Any]) -> list[str]:
        errors: list[str] = []
        try:
            self.edit_release_snapshot(release)
        except Exception as exc:
            errors.append(f"release_metadata_restore:{exc}")
        return errors

    def publish(self, assets: list[LocalAsset]) -> None:
        self.verify_origin_repository()
        release = self.get_release(allow_missing=True)
        old_tag_sha = self.get_tag_sha()
        if release is not None and release.get("immutable") is not False:
            raise PublishError("rolling_release_immutable_or_state_unknown")
        if release is not None and old_tag_sha is None:
            raise PublishError("rolling_release_tag_missing")
        if release is None and old_tag_sha is not None:
            raise PublishError("existing_nightly_tag_unmanaged")
        if release is not None:
            self.require_managed_release(release)

        self.update_tag(self.target_sha, dry_run=True)
        original_release = dict(release) if release is not None else None
        incoming_prefix = f"sb-nightly-incoming-{self.run_key}--"
        backup_prefix = f"sb-nightly-backup-{self.run_key}--"
        swaps: list[Swap] = []
        created = False
        tag_moved = False
        tag_update_attempted = False
        irreversible_cleanup_started = False
        committed = False

        try:
            if release is None:
                tag_update_attempted = True
                self.update_tag(self.target_sha)
                tag_moved = True
                self.gh(
                    "release",
                    "create",
                    self.contract.tag,
                    "--repo",
                    self.repository,
                    "--title",
                    self.title,
                    "--notes-file",
                    str(self.notes_file),
                    "--draft",
                    "--prerelease",
                    "--latest=false",
                    "--verify-tag",
                )
                release = self.get_release()
                if release is None:
                    raise PublishError("first_release_disappeared_after_creation")
                if release.get("immutable") is not False or release.get("draft") is not True:
                    raise PublishError("first_release_not_mutable_draft")
                created = True
            elif release.get("draft") is not True:
                self.edit_release_snapshot(release, draft_override=True)
                hidden = self.get_release()
                if hidden is None or hidden.get("draft") is not True:
                    raise PublishError("existing_release_draft_transition_failed")

            ordered_assets = sorted(
                assets,
                key=lambda asset: (
                    2
                    if asset.name == self.contract.checksum_name
                    else 1 if asset.name == self.contract.manifest_name else 0,
                    asset.name,
                ),
            )
            with tempfile.TemporaryDirectory(prefix="scratchbird-nightly-incoming-") as temp_name:
                upload_paths: list[Path] = []
                local_by_incoming: dict[str, LocalAsset] = {}
                for local in ordered_assets:
                    incoming_name = f"{incoming_prefix}{local.name}"
                    incoming_path = Path(temp_name) / incoming_name
                    try:
                        os.link(local.path, incoming_path)
                    except OSError:
                        shutil.copy2(local.path, incoming_path)
                    upload_paths.append(incoming_path)
                    local_by_incoming[incoming_name] = local
                self.gh(
                    "release",
                    "upload",
                    self.contract.tag,
                    "--repo",
                    self.repository,
                    *(str(path) for path in upload_paths),
                )

                uploaded_release = self.get_release()
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
                    current_release = self.get_release()
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

            self.verify_canonical_inventory(assets)
            # Deleting the previous generation is the point of no return for an
            # existing release. It happens while the release is still draft, so
            # backup or stale assets can never become publicly visible beside the
            # new canonical generation. A later failure leaves the new generation
            # draft for a clean retry instead of attempting an incomplete rollback.
            cleanup_release = self.get_release()
            if cleanup_release is None:
                raise PublishError("release_disappeared_before_private_cleanup")
            irreversible_cleanup_started = original_release is not None
            expected_names = {asset.name for asset in assets}
            for name, row in self.index_assets(cleanup_release).items():
                if name not in expected_names:
                    self.delete_asset(int(row["id"]))
            self.verify_canonical_inventory(assets, exact=True)
            if not created:
                tag_update_attempted = True
                self.update_tag(self.target_sha)
                tag_moved = True
            self.edit_release(
                draft=False,
                prerelease=True,
                title=self.title,
                notes_file=self.notes_file,
                target=self.target_sha,
            )
            published = self.verify_canonical_inventory(assets, exact=True)
            if published.get("draft") is not False or published.get("prerelease") is not True:
                raise PublishError("published_release_state_mismatch")
            published_tag_sha = self.get_tag_sha()
            if published_tag_sha != self.target_sha:
                raise PublishError(
                    "published_tag_revision_mismatch:"
                    f"expected={self.target_sha}:actual={published_tag_sha}"
                )
            committed = True
        except Exception as exc:
            recovery_errors: list[str] = []
            if not committed:
                current_release: dict[str, Any] | None = None
                try:
                    current_release = self.get_release(allow_missing=True)
                except Exception as lookup_exc:
                    recovery_errors.append(f"release_lookup_during_recovery:{lookup_exc}")
                if current_release is not None and current_release.get("draft") is not True:
                    try:
                        self.edit_release_snapshot(current_release, draft_override=True)
                    except Exception as hide_exc:
                        recovery_errors.append(f"release_hide_before_rollback:{hide_exc}")
                if irreversible_cleanup_started:
                    # Previous-generation bytes may already be deleted. Keep the
                    # exact new generation private and retryable; never republish a
                    # partial rollback or restore a tag that would disagree with it.
                    recovery_errors.append("coherent_draft_retry_required")
                else:
                    recovery_errors.extend(self.rollback_swaps(swaps))
                if original_release is None and tag_update_attempted:
                    try:
                        current_release = self.get_release(allow_missing=True)
                        if current_release is not None:
                            self.cleanup_named_prefix(incoming_prefix)
                            self.delete_new_draft_release(current_release)
                        if old_tag_sha is None:
                            if self.get_tag_sha() is not None:
                                self.delete_tag()
                        else:
                            self.update_tag(old_tag_sha)
                    except Exception as initial_restore_exc:
                        recovery_errors.append(f"initial_release_restore:{initial_restore_exc}")
                elif not irreversible_cleanup_started:
                    if tag_update_attempted and old_tag_sha is not None:
                        try:
                            current_tag_sha = self.get_tag_sha()
                            if tag_moved or current_tag_sha != old_tag_sha:
                                self.update_tag(old_tag_sha)
                        except Exception as restore_exc:
                            recovery_errors.append(f"tag_restore:{restore_exc}")
                    try:
                        self.cleanup_named_prefix(incoming_prefix)
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

        final_release = self.verify_canonical_inventory(assets, exact=True)
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
