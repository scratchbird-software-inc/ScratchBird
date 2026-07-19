#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Mocked GitHub/Git tests for the rolling nightly publisher."""

from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "project" / "tools" / "installers" / "publish_rolling_nightly.py"
SPEC = importlib.util.spec_from_file_location("publish_rolling_nightly", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
publisher = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = publisher
SPEC.loader.exec_module(publisher)

GIT_SUFFIX = "." + "git"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_bundle(
    root: Path,
    revision: str,
    run_id: str,
    attempt: str,
    marker: bytes,
    scope: str = "all",
) -> list[publisher.LocalAsset]:
    root.mkdir(parents=True)
    contract = publisher.get_release_contract(scope)
    all_packages = {
        "scratchbird-nightly-linux-x86_64.tar.gz": b"linux-" + marker,
        "scratchbird-nightly-linux-x86_64.deb": b"linux-deb-" + marker,
        "scratchbird-nightly-linux-x86_64.rpm": b"linux-rpm-" + marker,
        "scratchbird-nightly-linux-x86_64-aur.tar.gz": b"linux-aur-" + marker,
        "scratchbird-nightly-windows-x86_64.zip": b"windows-" + marker,
        "scratchbird-nightly-windows-x86_64.msi": b"windows-msi-" + marker,
        "scratchbird-nightly-macos-x86_64.tar.gz": b"mac-x86-" + marker,
        "scratchbird-nightly-macos-x86_64.pkg": b"mac-x86-pkg-" + marker,
        "scratchbird-nightly-macos-arm64.tar.gz": b"mac-" + marker,
        "scratchbird-nightly-macos-arm64.pkg": b"mac-arm-pkg-" + marker,
        "scratchbird-nightly-macos-universal.tar.gz": b"mac-universal-" + marker,
    }
    packages = {
        name: data for name, data in all_packages.items() if name in contract.package_names
    }
    rows = []
    for name, data in packages.items():
        (root / name).write_bytes(data)
        rows.append(
            {
                "name": name,
                "platform": name.split("-")[2],
                "architecture": "test",
                "format": Path(name).suffix.lstrip("."),
                "source_name": f"source-{name}",
                "verification": contract.verification_by_name[name],
                "bytes": len(data),
                "sha256": sha(data),
            }
        )
    manifest = {
        "schema_id": "scratchbird.native_nightly_release.v1",
        "release_scope": contract.scope,
        "release_tag": contract.tag,
        "included_platforms": (
            ["linux", "windows", "macos"] if scope == "all" else [scope]
        ),
        "source_revision": revision,
        "github_run_id": run_id,
        "github_run_attempt": attempt,
        "distribution_surface": "scratchbird_native_no_emulation",
        "public_asset_policy": "fully_verified_native_portable_and_system_installer_artifacts",
        "native_parser": "SBSQL",
        "native_components": [
            {"name": "SBmgr", "role": "manager"},
            {"name": "SBgate", "role": "gateway"},
            {"name": "SBParser", "role": "native_SBSQL_parser"},
            {"name": "SBsrv", "role": "database_server"},
        ],
        "emulation_layers_included": False,
        "artifacts": rows,
    }
    manifest_path = root / contract.manifest_name
    manifest_path.write_text(json.dumps(manifest, sort_keys=True) + "\n", encoding="utf-8")
    checksum_paths = sorted(path for path in root.iterdir())
    (root / contract.checksum_name).write_text(
        "".join(f"{publisher.sha256_file(path)}  {path.name}\n" for path in checksum_paths),
        encoding="utf-8",
    )
    return publisher.load_local_assets(root, revision, run_id, attempt, scope)


class FakeRunner:
    def __init__(self) -> None:
        self.tag = "nightly"
        self.release: dict | None = None
        self.remote_tag_sha: str | None = None
        self.local_tag_sha: str | None = None
        self.next_asset_id = 100
        self.asset_data: dict[int, bytes] = {}
        self.commands: list[list[str]] = []
        self.fail_upload_after: int | None = None
        self.fail_canonical_patch_at: int | None = None
        self.fail_edit_once = False
        self.fail_create_once = False
        self.fail_push_after_update_once = False
        self.tag_visibility_misses_after_push = 0
        self.initial_release_payloads: list[dict] = []
        self.origin_url = "https://github.com/scratchbird-software-inc/ScratchBird" + GIT_SUFFIX
        self._canonical_patch_count = 0
        self._tag_visibility_misses = 0

    def result(self, returncode: int = 0, stdout: str = "", stderr: str = "", *, check: bool) -> publisher.CommandResult:
        value = publisher.CommandResult(returncode, stdout, stderr)
        if check and returncode != 0:
            raise publisher.PublishError(f"mock_command_failed:{stderr or stdout}")
        return value

    def make_asset(self, name: str, data: bytes) -> dict:
        row = {
            "id": self.next_asset_id,
            "name": name,
            "state": "uploaded",
            "size": len(data),
            "digest": f"sha256:{sha(data)}",
        }
        self.asset_data[self.next_asset_id] = data
        self.next_asset_id += 1
        return row

    def seed_release(self, assets: list[publisher.LocalAsset], marker: bytes = b"old", *, immutable: bool = False) -> None:
        self.remote_tag_sha = "b" * 40
        self.release = {
            "id": 7,
            "tag_name": self.tag,
            "name": "Old nightly",
            "body": "old notes",
            "draft": False,
            "prerelease": True,
            "immutable": immutable,
            "target_commitish": self.remote_tag_sha,
            "assets": [
                self.make_asset(
                    asset.name,
                    asset.path.read_bytes()
                    if asset.name == "scratchbird-nightly-manifest.json"
                    else marker + asset.name.encode(),
                )
                for asset in assets
            ],
        }

    def run(self, command: list[str], *, check: bool = True) -> publisher.CommandResult:
        self.commands.append(list(command))
        if command[0] == "git":
            return self.run_git(command[1:], check=check)
        if command[0] != "gh":
            return self.result(1, stderr="unexpected binary", check=check)
        if command[1:3] == ["release", "upload"]:
            return self.release_upload(command[3:], check=check)
        if command[1:3] == ["release", "edit"]:
            return self.release_edit(command[3:], check=check)
        if command[1:3] == ["release", "download"]:
            return self.release_download(command[3:], check=check)
        if command[1] == "api":
            return self.api(command[2:], check=check)
        return self.result(1, stderr="unexpected gh command", check=check)

    def run_git(self, args: list[str], *, check: bool) -> publisher.CommandResult:
        if args[:1] == ["-C"]:
            args = args[2:]
        if args[:3] == ["remote", "get-url", "origin"]:
            return self.result(stdout=self.origin_url + "\n", check=check)
        if args[:2] == ["tag", "-f"]:
            self.local_tag_sha = args[3]
            return self.result(check=check)
        if args and args[0] == "push":
            if f":refs/tags/{self.tag}" in args:
                self.remote_tag_sha = None
            elif "--dry-run" not in args:
                refspec = next(
                    value for value in args if value.endswith(f":refs/tags/{self.tag}")
                )
                self.remote_tag_sha = refspec.split(":", 1)[0]
                self._tag_visibility_misses = self.tag_visibility_misses_after_push
                if self.fail_push_after_update_once:
                    self.fail_push_after_update_once = False
                    return self.result(1, stderr="injected post-update push failure", check=check)
            return self.result(check=check)
        return self.result(1, stderr="unexpected git command", check=check)

    def release_create_api(self, payload: dict, *, check: bool) -> publisher.CommandResult:
        if self.fail_create_once:
            self.fail_create_once = False
            return self.result(1, stderr="injected release create failure", check=check)
        if self.release is not None:
            return self.result(1, stderr="release exists", check=check)
        self.initial_release_payloads.append(payload)
        self.release = {
            "id": 7,
            "tag_name": self.tag,
            "name": payload["name"],
            "body": payload["body"],
            "draft": payload["draft"],
            "prerelease": payload["prerelease"],
            "immutable": False,
            "target_commitish": payload["target_commitish"],
            "assets": [],
        }
        return self.result(stdout=json.dumps(self.release), check=check)

    def release_upload(self, args: list[str], *, check: bool) -> publisher.CommandResult:
        assert self.release is not None
        values = list(args[1:])
        if "--repo" in values:
            index = values.index("--repo")
            del values[index : index + 2]
        paths = [Path(value) for value in values]
        uploaded = 0
        for path in paths:
            if any(row["name"] == path.name for row in self.release["assets"]):
                return self.result(1, stderr="asset name collision", check=check)
            self.release["assets"].append(self.make_asset(path.name, path.read_bytes()))
            uploaded += 1
            if self.fail_upload_after is not None and uploaded >= self.fail_upload_after:
                self.fail_upload_after = None
                return self.result(1, stderr="injected upload failure", check=check)
        return self.result(check=check)

    def release_edit(self, args: list[str], *, check: bool) -> publisher.CommandResult:
        assert self.release is not None
        if self.fail_edit_once and "--draft=false" in args:
            self.fail_edit_once = False
            return self.result(1, stderr="injected edit failure", check=check)
        self.release["name"] = args[args.index("--title") + 1]
        self.release["body"] = Path(args[args.index("--notes-file") + 1]).read_text(encoding="utf-8")
        self.release["draft"] = "--draft=true" in args
        self.release["prerelease"] = "--prerelease=true" in args
        self.release["target_commitish"] = args[args.index("--target") + 1]
        return self.result(check=check)

    def release_download(self, args: list[str], *, check: bool) -> publisher.CommandResult:
        assert self.release is not None
        pattern = args[args.index("--pattern") + 1]
        destination = Path(args[args.index("--dir") + 1])
        matching = [row for row in self.release["assets"] if row["name"] == pattern]
        if len(matching) != 1:
            return self.result(1, stderr="mock release asset missing", check=check)
        destination.mkdir(parents=True, exist_ok=True)
        (destination / pattern).write_bytes(self.asset_data[matching[0]["id"]])
        return self.result(check=check)

    def api(self, args: list[str], *, check: bool) -> publisher.CommandResult:
        endpoint = next((value for value in args if value.startswith("repos/")), "")
        method = args[args.index("--method") + 1] if "--method" in args else "GET"
        if endpoint.endswith(f"/releases/tags/{self.tag}"):
            if self.release is None:
                return self.result(1, stderr="HTTP 404 Not Found", check=check)
            return self.result(stdout=json.dumps(self.release), check=check)
        if endpoint.endswith(f"/git/ref/tags/{self.tag}"):
            if self.remote_tag_sha is None:
                return self.result(1, stderr="HTTP 404 Not Found", check=check)
            if self._tag_visibility_misses > 0:
                self._tag_visibility_misses -= 1
                return self.result(1, stderr="HTTP 404 Not Found", check=check)
            return self.result(
                stdout=json.dumps({"object": {"type": "commit", "sha": self.remote_tag_sha}}),
                check=check,
            )
        if endpoint.endswith("/releases") and method == "POST":
            if "--input" not in args:
                return self.result(1, stderr="missing release payload", check=check)
            payload_path = Path(args[args.index("--input") + 1])
            payload = json.loads(payload_path.read_text(encoding="utf-8"))
            return self.release_create_api(payload, check=check)
        if re.search(r"/releases/[0-9]+$", endpoint) and method == "DELETE":
            if self.release is None or int(endpoint.rsplit("/", 1)[1]) != self.release["id"]:
                return self.result(1, stderr="HTTP 404 release", check=check)
            self.release = None
            self.asset_data.clear()
            return self.result(check=check)
        if "/releases/assets/" in endpoint:
            assert self.release is not None
            asset_id = int(endpoint.rsplit("/", 1)[1])
            row = next((item for item in self.release["assets"] if item["id"] == asset_id), None)
            if row is None:
                return self.result(1, stderr="HTTP 404 asset", check=check)
            if method == "DELETE":
                self.release["assets"].remove(row)
                self.asset_data.pop(asset_id, None)
                return self.result(check=check)
            if method == "PATCH":
                field = args[args.index("-f") + 1]
                new_name = field.removeprefix("name=")
                if new_name.startswith("scratchbird-nightly-"):
                    self._canonical_patch_count += 1
                    if self.fail_canonical_patch_at == self._canonical_patch_count:
                        self.fail_canonical_patch_at = None
                        return self.result(1, stderr="injected canonical rename failure", check=check)
                if any(item["name"] == new_name and item["id"] != asset_id for item in self.release["assets"]):
                    return self.result(1, stderr="asset rename collision", check=check)
                row["name"] = new_name
                return self.result(stdout=json.dumps(row), check=check)
        return self.result(1, stderr=f"unexpected endpoint {endpoint}", check=check)


class RollingNightlyPublisherTest(unittest.TestCase):
    revision = "a" * 40
    run_id = "42"
    attempt = "1"

    def make_publisher(
        self,
        runner: FakeRunner,
        notes: Path,
        scope: str = "all",
    ) -> publisher.RollingPublisher:
        runner.tag = publisher.get_release_contract(scope).tag
        return publisher.RollingPublisher(
            runner,
            repository="scratchbird-software-inc/ScratchBird",
            target_sha=self.revision,
            run_id=self.run_id,
            run_attempt=self.attempt,
            title="ScratchBird Native Nightly Builds",
            notes_file=notes,
            checkout_root=notes.parent,
            release_scope=scope,
        )

    @staticmethod
    def names(runner: FakeRunner) -> set[str]:
        assert runner.release is not None
        return {row["name"] for row in runner.release["assets"]}

    def test_first_creation_uses_draft_then_publishes_exact_assets(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-publish-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(self.revision, runner.remote_tag_sha)
            self.assertIsNotNone(runner.release)
            self.assertFalse(runner.release["draft"])
            self.assertTrue(runner.release["prerelease"])
            self.assertEqual({asset.name for asset in assets}, self.names(runner))
            self.assertEqual(
                [
                    {
                        "body": "Native ScratchBird; no emulation.\n",
                        "draft": True,
                        "make_latest": "false",
                        "name": "ScratchBird Native Nightly Builds",
                        "prerelease": True,
                        "tag_name": "nightly",
                        "target_commitish": self.revision,
                    }
                ],
                runner.initial_release_payloads,
            )
            flattened = [token for command in runner.commands for token in command]
            self.assertNotIn("delete", flattened)
            self.assertNotIn("--clobber", flattened)
            release_commands = [
                command for command in runner.commands if command[:2] == ["gh", "release"]
            ]
            self.assertTrue(release_commands)
            self.assertTrue(all("--repo" in command for command in release_commands))
            canonical_renames = [
                command[command.index("-f") + 1].removeprefix("name=")
                for command in runner.commands
                if command[:2] == ["gh", "api"]
                and "--method" in command
                and command[command.index("--method") + 1] == "PATCH"
                and "-f" in command
                and command[command.index("-f") + 1].startswith("name=scratchbird-nightly-")
            ]
            self.assertEqual(
                ["scratchbird-nightly-manifest.json", "scratchbird-nightly-SHA256SUMS"],
                canonical_renames[-2:],
            )

    def test_first_creation_waits_for_eventual_tag_visibility(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-tag-visibility-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            runner.tag_visibility_misses_after_push = 2
            with mock.patch.object(publisher.time, "sleep") as sleep:
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(
                [
                    mock.call(publisher.TAG_VISIBILITY_DELAY_SECONDS),
                    mock.call(publisher.TAG_VISIBILITY_DELAY_SECONDS),
                ],
                sleep.call_args_list,
            )
            create_index = next(
                index
                for index, command in enumerate(runner.commands)
                if command[:2] == ["gh", "api"]
                and "--method" in command
                and command[command.index("--method") + 1] == "POST"
                and any(value.endswith("/releases") for value in command)
            )
            actual_push_index = next(
                index
                for index, command in enumerate(runner.commands)
                if command[0] == "git"
                and "push" in command
                and "--dry-run" not in command
                and any(value.endswith(":refs/tags/nightly") for value in command)
            )
            tag_lookups_before_create = [
                command
                for command in runner.commands[actual_push_index + 1 : create_index]
                if command[:2] == ["gh", "api"]
                and any("/git/ref/tags/nightly" in value for value in command)
            ]
            self.assertGreaterEqual(len(tag_lookups_before_create), 3)
            self.assertIsNotNone(runner.release)

    def test_linux_scope_publishes_only_its_contract_to_its_own_tag(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-platform-nightly-publish-") as temp:
            root = Path(temp)
            contract = publisher.get_release_contract("linux")
            assets = write_bundle(
                root / "assets",
                self.revision,
                self.run_id,
                self.attempt,
                b"linux",
                "linux",
            )
            notes = root / "notes.md"
            notes.write_text("Linux-only native nightly.\n", encoding="utf-8")
            runner = FakeRunner()
            self.make_publisher(runner, notes, "linux").publish(assets)
            self.assertEqual(contract.tag, runner.tag)
            self.assertEqual(self.revision, runner.remote_tag_sha)
            self.assertIsNotNone(runner.release)
            assert runner.release is not None
            self.assertEqual(contract.tag, runner.release["tag_name"])
            self.assertEqual(contract.canonical_asset_names, self.names(runner))
            self.assertTrue(
                any(
                    f"refs/tags/{contract.tag}" in value
                    for command in runner.commands
                    for value in command
                )
            )
            self.assertFalse(
                any(
                    value.endswith(":refs/tags/nightly")
                    for command in runner.commands
                    for value in command
                )
            )

    def test_existing_release_is_updated_in_place_and_stale_assets_removed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-replace-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("new notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            original_release_id = runner.release["id"]
            runner.release["assets"].append(runner.make_asset("scratchbird-nightly-obsolete-legacy.msi", b"stale msi"))
            self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(original_release_id, runner.release["id"])
            self.assertEqual({asset.name for asset in assets}, self.names(runner))
            self.assertEqual(self.revision, runner.remote_tag_sha)
            remote = {row["name"]: row for row in runner.release["assets"]}
            for asset in assets:
                self.assertEqual(f"sha256:{asset.digest}", remote[asset.name]["digest"])
            edit_commands = [
                command for command in runner.commands if command[:3] == ["gh", "release", "edit"]
            ]
            upload_index = next(
                index
                for index, command in enumerate(runner.commands)
                if command[:3] == ["gh", "release", "upload"]
            )
            hidden_index = runner.commands.index(edit_commands[0])
            published_index = runner.commands.index(edit_commands[-1])
            delete_indexes = [
                index
                for index, command in enumerate(runner.commands)
                if command[:2] == ["gh", "api"]
                and "--method" in command
                and command[command.index("--method") + 1] == "DELETE"
                and "/releases/assets/" in next(
                    (value for value in command if value.startswith("repos/")), ""
                )
            ]
            self.assertIn("--draft=true", edit_commands[0])
            self.assertIn("--draft=false", edit_commands[-1])
            self.assertLess(hidden_index, upload_index)
            self.assertLess(upload_index, published_index)
            self.assertTrue(delete_indexes)
            self.assertLess(max(delete_indexes), published_index)

    def test_release_create_failure_restores_absent_initial_tag(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-create-fail-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.fail_create_once = True
            with self.assertRaisesRegex(publisher.PublishError, "release create failure"):
                self.make_publisher(runner, notes).publish(assets)
            self.assertIsNone(runner.release)
            self.assertIsNone(runner.remote_tag_sha)

    def test_initial_tag_push_reported_failure_removes_partially_created_tag(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-tag-push-fail-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.fail_push_after_update_once = True
            with self.assertRaisesRegex(publisher.PublishError, "post-update push failure"):
                self.make_publisher(runner, notes).publish(assets)
            self.assertIsNone(runner.release)
            self.assertIsNone(runner.remote_tag_sha)

    def test_preexisting_unreleased_nightly_tag_is_refused_as_unmanaged(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-unmanaged-tag-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.remote_tag_sha = "b" * 40
            with self.assertRaisesRegex(publisher.PublishError, "existing_nightly_tag_unmanaged"):
                self.make_publisher(runner, notes).publish(assets)
            self.assertIsNone(runner.release)
            self.assertEqual("b" * 40, runner.remote_tag_sha)
            self.assertFalse(any(command[0] == "git" and "push" in command for command in runner.commands))

    def test_unmanaged_existing_nightly_release_is_refused_without_mutation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-unmanaged-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            manifest = next(
                row for row in runner.release["assets"] if row["name"] == "scratchbird-nightly-manifest.json"
            )
            runner.release["assets"].remove(manifest)
            runner.asset_data.pop(manifest["id"])
            old_tag = runner.remote_tag_sha
            old_names = self.names(runner)
            with self.assertRaisesRegex(publisher.PublishError, "existing_nightly_release_unmanaged"):
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(old_tag, runner.remote_tag_sha)
            self.assertEqual(old_names, self.names(runner))
            self.assertFalse(any(command[:3] == ["gh", "release", "edit"] for command in runner.commands))

    def test_checkout_origin_repository_mismatch_is_refused_first(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-origin-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.origin_url = "https://github.com/example/other" + GIT_SUFFIX
            with self.assertRaisesRegex(publisher.PublishError, "origin_repository_mismatch"):
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(1, len(runner.commands))

    def test_first_creation_upload_failure_fully_removes_draft_and_tag(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-first-fail-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.fail_upload_after = 1
            with self.assertRaisesRegex(publisher.PublishError, "upload failure"):
                self.make_publisher(runner, notes).publish(assets)
            self.assertIsNone(runner.release)
            self.assertIsNone(runner.remote_tag_sha)

    def test_partial_upload_failure_leaves_old_release_and_tag_intact(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-upload-fail-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            old_tag = runner.remote_tag_sha
            old_rows = {row["name"]: row["digest"] for row in runner.release["assets"]}
            runner.fail_upload_after = 1
            with self.assertRaisesRegex(publisher.PublishError, "upload failure"):
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(old_tag, runner.remote_tag_sha)
            self.assertEqual(old_rows, {row["name"]: row["digest"] for row in runner.release["assets"]})

    def test_mid_swap_failure_rolls_every_asset_back(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-swap-fail-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            old_rows = {row["name"]: row["digest"] for row in runner.release["assets"]}
            old_tag = runner.remote_tag_sha
            runner.fail_canonical_patch_at = 2
            with self.assertRaisesRegex(publisher.PublishError, "canonical rename failure"):
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(old_tag, runner.remote_tag_sha)
            self.assertEqual(old_rows, {row["name"]: row["digest"] for row in runner.release["assets"]})
            self.assertFalse(any(name.startswith("sb-nightly-") for name in self.names(runner)))
            self.assertFalse(runner.release["draft"])
            self.assertEqual("Old nightly", runner.release["name"])

    def test_release_publish_failure_leaves_exact_new_generation_draft(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-edit-fail-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("new notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            runner.fail_edit_once = True
            with self.assertRaisesRegex(
                publisher.PublishError,
                "edit failure.*coherent_draft_retry_required",
            ):
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(self.revision, runner.remote_tag_sha)
            self.assertTrue(runner.release["draft"])
            self.assertEqual("Old nightly", runner.release["name"])
            self.assertEqual("old notes", runner.release["body"])
            self.assertEqual({asset.name for asset in assets}, self.names(runner))
            remote = {row["name"]: row for row in runner.release["assets"]}
            for asset in assets:
                self.assertEqual(f"sha256:{asset.digest}", remote[asset.name]["digest"])

    def test_immutable_release_refuses_before_mutation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-immutable-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets, immutable=True)
            command_count = len(runner.commands)
            with self.assertRaisesRegex(publisher.PublishError, "immutable"):
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(command_count + 3, len(runner.commands))  # origin, release, and tag reads only
            self.assertFalse(
                any(command[0] == "git" and "push" in command for command in runner.commands)
            )

    def test_rerun_is_idempotent_when_asset_content_matches(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-idempotent-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"same")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            self.make_publisher(runner, notes).publish(assets)
            first_digests = {row["name"]: row["digest"] for row in runner.release["assets"]}
            self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(first_digests, {row["name"]: row["digest"] for row in runner.release["assets"]})
            self.assertEqual({asset.name for asset in assets}, self.names(runner))


if __name__ == "__main__":
    unittest.main(verbosity=2)
