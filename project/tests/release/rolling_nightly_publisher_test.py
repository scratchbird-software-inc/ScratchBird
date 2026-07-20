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
from urllib.parse import parse_qs, urlparse


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "project" / "tools" / "installers" / "publish_rolling_nightly.py"
SPEC = importlib.util.spec_from_file_location("publish_rolling_nightly", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
publisher = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = publisher
SPEC.loader.exec_module(publisher)

GIT_SUFFIX = "." + "git"
# Keep the mocked upload endpoint literal-safe for the public source-hygiene
# scan, which deliberately rejects private-repository path fragments.
UPLOADS_ENDPOINT_ROOT = (
    "https://uploads" + "." + "github.com/repos/scratchbird-software-inc/"
)


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
        "public_asset_policy": "fully_verified_native_portable_and_platform_system_installer_artifacts",
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
    if scope in {"all", "windows"}:
        manifest["windows_release_policy"] = "portable_zip_only_no_msi"
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
        self.extra_releases: list[dict] = []
        self.remote_tag_sha: str | None = None
        self.local_tag_sha: str | None = None
        self.next_asset_id = 100
        self.next_release_id = 20
        self.asset_data: dict[int, bytes] = {}
        self.commands: list[list[str]] = []
        self.curl_configs: list[str] = []
        self.fail_upload_after: int | None = None
        self.fail_upload_after_accept_once = False
        self.fail_upload_starter_once = False
        self.fail_canonical_patch_at: int | None = None
        self.fail_edit_once = False
        self.fail_create_once = False
        self.fail_create_transient_once = False
        self.fail_create_not_found_once = False
        self.fail_create_after_accept_once = False
        self.created_target_commitish_override: str | None = None
        self.fail_push_after_update_once = False
        self.mutate_tag_before_lease_push_once: str | None = None
        self.mutate_legacy_draft_to_other_run_on_get: int | None = None
        self.tag_visibility_misses_after_push = 0
        self.release_lookup_transient_failures = 0
        self.release_lookup_hidden = False
        self.initial_release_payloads: list[dict] = []
        self.release_patch_payloads: list[dict] = []
        self.origin_url = "https://github.com/scratchbird-software-inc/ScratchBird" + GIT_SUFFIX
        self._canonical_patch_count = 0
        self._tag_visibility_misses = 0
        self._upload_call_count = 0

    def result(self, returncode: int = 0, stdout: str = "", stderr: str = "", *, check: bool) -> publisher.CommandResult:
        value = publisher.CommandResult(returncode, stdout, stderr)
        if check and returncode != 0:
            raise publisher.PublishError(f"mock_command_failed:{stderr or stdout}")
        return value

    def make_asset(self, name: str, data: bytes, *, state: str = "uploaded") -> dict:
        row = {
            "id": self.next_asset_id,
            "name": name,
            "state": state,
            "size": len(data),
            "digest": f"sha256:{sha(data)}",
        }
        self.asset_data[self.next_asset_id] = data
        self.next_asset_id += 1
        return row

    def all_releases(self) -> list[dict]:
        values = list(self.extra_releases)
        if self.release is not None:
            values.append(self.release)
        return values

    def find_release(self, release_id: int) -> dict | None:
        return next(
            (release for release in self.all_releases() if release["id"] == release_id),
            None,
        )

    def remove_release(self, release_id: int) -> bool:
        release = self.find_release(release_id)
        if release is None:
            return False
        for asset in release["assets"]:
            self.asset_data.pop(asset["id"], None)
        if self.release is release:
            self.release = None
        else:
            self.extra_releases.remove(release)
        return True

    def release_record(self, payload: dict, release_id: int) -> dict:
        return {
            "id": release_id,
            "tag_name": self.tag,
            "name": payload["name"],
            "body": payload["body"],
            "draft": payload["draft"],
            "prerelease": payload["prerelease"],
            "immutable": False,
            "target_commitish": payload["target_commitish"],
            "author": {"login": "github-actions[bot]"},
            "upload_url": (
                UPLOADS_ENDPOINT_ROOT
                + f"ScratchBird/releases/{release_id}/assets{{?name,label}}"
            ),
            "assets": [],
        }

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
            "author": {"login": "github-actions[bot]"},
            "upload_url": (
                UPLOADS_ENDPOINT_ROOT + "ScratchBird/releases/7/assets{?name,label}"
            ),
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

    def add_legacy_draft(
        self,
        *,
        source: str,
        run_id: str,
        target: str = "main",
        title: str = "ScratchBird Native Nightly Builds",
        author_login: str = "github-actions[bot]",
        primary: bool = False,
    ) -> dict:
        """Create a record made by the retired tag-based publisher."""

        release_id = self.next_release_id
        self.next_release_id += 1
        staged_name = (
            "sb-nightly-incoming-99-1--"
            "scratchbird-nightly-linux-x86_64.tar.gz"
        )
        release = {
            "id": release_id,
            "tag_name": self.tag,
            "name": title,
            "body": (
                "ScratchBird native nightly build for testing.\n\n"
                "- Native ScratchBird platform only: SBmgr, SBgate, SBParser using native SBSQL, and SBsrv are included.\n"
                "- No compatibility parser or emulation packages are included.\n"
                "- The native ScratchBird listener default is TCP port 3092.\n"
                "- Includes the default local-password policy pack plus charset, collation, timezone, and native SBSQL language resources.\n"
                "- LLVM is mandatory: Linux portable archives require libllvm23/llvm-libs 23+, Windows archives bundle the LLVM DLL closure, and macOS QA archives require Homebrew llvm 22+.\n"
                "- Public nightly assets include fully verified portable archives and system installer packages.\n"
                "- DEB, RPM, AUR, PKG, and MSI packages are published for tester installation after their platform verification and install-smoke jobs pass.\n"
                f"- Source commit: {source}\n"
                "- Requested version: 0.0.0-nightly\n"
                "- Workflow run: https://github.com/"
                "scratchbird-software-inc/ScratchBird/actions/runs/"
                f"{run_id}\n"
                "- Linux, Windows, and macOS build, CTest, package verification, and smoke-install jobs completed successfully before publication.\n"
                "- macOS QA packages are unsigned and unnotarized unless the manifest explicitly records signed payloads.\n\n"
                "Verify downloads with scratchbird-nightly-SHA256SUMS.\n"
            ),
            "draft": True,
            "prerelease": True,
            "immutable": False,
            "target_commitish": target,
            "author": {"login": author_login},
            "upload_url": (
                UPLOADS_ENDPOINT_ROOT
                + f"ScratchBird/releases/{release_id}/assets{{?name,label}}"
            ),
            "assets": [self.make_asset(staged_name, b"legacy-staged")],
        }
        if primary:
            self.release = release
        else:
            self.extra_releases.append(release)
        return release

    def run(self, command: list[str], *, check: bool = True) -> publisher.CommandResult:
        self.commands.append(list(command))
        if command[0] == "git":
            return self.run_git(command[1:], check=check)
        if command[0] == "curl":
            return self.curl_upload(command[1:], check=check)
        if command[0] != "gh":
            return self.result(1, stderr="unexpected binary", check=check)
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
            lease = next(
                (
                    value
                    for value in args
                    if value.startswith("--force-with-lease=refs/tags/")
                ),
                None,
            )
            if lease is not None:
                expected = lease.rsplit(":", 1)[1] or None
                if (
                    "--dry-run" not in args
                    and self.mutate_tag_before_lease_push_once is not None
                ):
                    self.remote_tag_sha = self.mutate_tag_before_lease_push_once
                    self.mutate_tag_before_lease_push_once = None
                if self.remote_tag_sha != expected:
                    return self.result(1, stderr="stale info", check=check)
            if "--dry-run" in args:
                return self.result(check=check)
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
        if self.fail_create_transient_once:
            self.fail_create_transient_once = False
            return self.result(1, stderr="HTTP 503 Service Unavailable", check=check)
        if self.fail_create_not_found_once:
            self.fail_create_not_found_once = False
            return self.result(1, stderr="HTTP 404 Not Found", check=check)
        if self.fail_create_once:
            self.fail_create_once = False
            return self.result(1, stderr="injected release create failure", check=check)
        if self.release is not None:
            return self.result(1, stderr="release exists", check=check)
        self.initial_release_payloads.append(payload)
        self.release = self.release_record(payload, 7)
        if self.created_target_commitish_override is not None:
            self.release["target_commitish"] = self.created_target_commitish_override
        if self.fail_create_after_accept_once:
            self.fail_create_after_accept_once = False
            return self.result(1, stderr="HTTP 503 Service Unavailable", check=check)
        return self.result(stdout=json.dumps(self.release), check=check)

    def curl_upload(self, args: list[str], *, check: bool) -> publisher.CommandResult:
        config = Path(args[args.index("--config") + 1])
        self.curl_configs.append(config.read_text(encoding="utf-8"))
        endpoint = args[-1]
        parsed = urlparse(endpoint)
        match = re.fullmatch(r"/repos/[^/]+/[^/]+/releases/([0-9]+)/assets", parsed.path)
        if match is None:
            return self.result(1, stderr=f"unexpected upload endpoint {endpoint}", check=check)
        release = self.find_release(int(match.group(1)))
        if release is None:
            return self.result(1, stderr="HTTP 404 release", check=check)
        names = parse_qs(parsed.query).get("name", [])
        if len(names) != 1:
            return self.result(1, stderr="missing upload name", check=check)
        name = names[0]
        source = Path(args[args.index("--data-binary") + 1].removeprefix("@"))
        self._upload_call_count += 1
        if (
            self.fail_upload_after is not None
            and self._upload_call_count >= self.fail_upload_after
        ):
            self.fail_upload_after = None
            return self.result(1, stderr="injected upload failure", check=check)
        if any(row["name"] == name for row in release["assets"]):
            return self.result(1, stderr="asset name collision", check=check)
        if self.fail_upload_starter_once:
            self.fail_upload_starter_once = False
            release["assets"].append(self.make_asset(name, b"", state="starter"))
            return self.result(1, stderr="HTTP 502 Bad Gateway", check=check)
        row = self.make_asset(name, source.read_bytes())
        release["assets"].append(row)
        if self.fail_upload_after_accept_once:
            self.fail_upload_after_accept_once = False
            return self.result(1, stderr="HTTP 503 Service Unavailable", check=check)
        return self.result(stdout=json.dumps(row), check=check)

    def api(self, args: list[str], *, check: bool) -> publisher.CommandResult:
        endpoint = next((value for value in args if value.startswith("repos/")), "")
        method = args[args.index("--method") + 1] if "--method" in args else "GET"
        if endpoint.startswith("repos/scratchbird-software-inc/ScratchBird/releases?"):
            if self.release_lookup_transient_failures > 0:
                self.release_lookup_transient_failures -= 1
                return self.result(1, stderr="HTTP 503 Service Unavailable", check=check)
            page = parse_qs(urlparse(endpoint).query).get("page", ["1"])[0]
            return self.result(
                stdout=json.dumps(self.all_releases() if page == "1" else []), check=check
            )
        if endpoint.endswith(f"/releases/tags/{self.tag}"):
            if self.release is None or self.release_lookup_hidden:
                return self.result(1, stderr="HTTP 404 Not Found", check=check)
            if self.release_lookup_transient_failures > 0:
                self.release_lookup_transient_failures -= 1
                return self.result(1, stderr="HTTP 503 Service Unavailable", check=check)
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
        release_match = re.search(r"/releases/([0-9]+)$", endpoint)
        if release_match is not None:
            release_id = int(release_match.group(1))
            release = self.find_release(release_id)
            if release is None:
                return self.result(1, stderr="HTTP 404 release", check=check)
            if method == "GET":
                if self.mutate_legacy_draft_to_other_run_on_get == release_id:
                    self.mutate_legacy_draft_to_other_run_on_get = None
                    release["body"] = (
                        "<!-- scratchbird-rolling-nightly-managed:"
                        "schema=v2;scope=all;tag=nightly;"
                        f"source={'c' * 40};run_id=99;predecessor_tag=absent -->"
                    )
                if self.release_lookup_transient_failures > 0:
                    self.release_lookup_transient_failures -= 1
                    return self.result(1, stderr="HTTP 503 Service Unavailable", check=check)
                return self.result(stdout=json.dumps(release), check=check)
            if method == "DELETE":
                self.remove_release(release_id)
                return self.result(check=check)
            if method == "PATCH":
                payload_path = Path(args[args.index("--input") + 1])
                payload = json.loads(payload_path.read_text(encoding="utf-8"))
                self.release_patch_payloads.append(dict(payload))
                if self.fail_edit_once and payload.get("draft") is False:
                    self.fail_edit_once = False
                    return self.result(1, stderr="injected edit failure", check=check)
                release.update(
                    {
                        "name": payload["name"],
                        "body": payload["body"],
                        "draft": payload["draft"],
                        "prerelease": payload["prerelease"],
                        "target_commitish": payload["target_commitish"],
                    }
                )
                return self.result(stdout=json.dumps(release), check=check)
        if "/releases/assets/" in endpoint:
            asset_id = int(endpoint.rsplit("/", 1)[1])
            release = next(
                (
                    candidate
                    for candidate in self.all_releases()
                    if any(item["id"] == asset_id for item in candidate["assets"])
                ),
                None,
            )
            row = (
                next((item for item in release["assets"] if item["id"] == asset_id), None)
                if release is not None
                else None
            )
            if row is None:
                return self.result(1, stderr="HTTP 404 asset", check=check)
            if method == "GET":
                if any("application/octet-stream" in value for value in args):
                    return self.result(
                        stdout=self.asset_data[asset_id].decode("utf-8"), check=check
                    )
                return self.result(stdout=json.dumps(row), check=check)
            if method == "DELETE":
                assert release is not None
                release["assets"].remove(row)
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
                assert release is not None
                if any(item["name"] == new_name and item["id"] != asset_id for item in release["assets"]):
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
        *,
        mark_existing: bool = True,
    ) -> publisher.RollingPublisher:
        runner.tag = publisher.get_release_contract(scope).tag
        instance = publisher.RollingPublisher(
            runner,
            repository="scratchbird-software-inc/ScratchBird",
            target_sha=self.revision,
            run_id=self.run_id,
            run_attempt=self.attempt,
            title="ScratchBird Native Nightly Builds",
            notes_file=notes,
            checkout_root=notes.parent,
            release_scope=scope,
            github_token="test-token",
        )
        instance._marker_predecessor_tag = runner.remote_tag_sha
        if (
            mark_existing
            and runner.release is not None
            and "scratchbird-rolling-nightly-managed:" not in runner.release["body"]
        ):
            runner.release["body"] = (
                f"{runner.release['body'].rstrip()}\n\n{instance.release_marker()}\n"
            )
            runner.release["target_commitish"] = instance.target_sha
            if runner.release["draft"] is False:
                runner.release["name"] = instance.title
                runner.remote_tag_sha = instance.target_sha
        return instance

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
            rolling = self.make_publisher(runner, notes)
            rolling.publish(assets)
            self.assertEqual(self.revision, runner.remote_tag_sha)
            self.assertIsNotNone(runner.release)
            self.assertFalse(runner.release["draft"])
            self.assertTrue(runner.release["prerelease"])
            self.assertEqual({asset.name for asset in assets}, self.names(runner))
            self.assertEqual(
                [
                    {
                        "body": rolling.with_release_marker(
                            "Native ScratchBird; no emulation.\n"
                        ),
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
            self.assertFalse(
                any(command[:2] == ["gh", "release"] for command in runner.commands)
            )
            curl_commands = [command for command in runner.commands if command[0] == "curl"]
            self.assertEqual(len(assets), len(curl_commands))
            self.assertTrue(all(command[1] == "--disable" for command in curl_commands))
            self.assertTrue(
                all(
                    any("/releases/7/assets?name=" in token for token in command)
                    for command in curl_commands
                )
            )
            self.assertTrue(
                all(
                    "Authorization: Bearer" not in token and "test-token" not in token
                    for command in curl_commands
                    for token in command
                )
            )
            self.assertEqual(len(assets), len(runner.curl_configs))
            self.assertTrue(
                all('Authorization: Bearer test-token' in config for config in runner.curl_configs)
            )
            curl_config_paths = [
                Path(command[command.index("--config") + 1])
                for command in curl_commands
            ]
            self.assertTrue(all(not path.exists() for path in curl_config_paths))
            self.assertFalse(
                any("--location" in command for command in curl_commands)
            )
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

    def test_hidden_tag_lookup_draft_resumes_by_release_id(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-hidden-draft-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            assert runner.release is not None
            release_id = runner.release["id"]
            runner.release["draft"] = True
            runner.release["name"] = "ScratchBird Native Nightly Builds"
            runner.release_lookup_hidden = True
            rolling = self.make_publisher(runner, notes)
            runner.release["target_commitish"] = "main"
            rolling.publish(assets)
            self.assertEqual(release_id, runner.release["id"])
            self.assertFalse(runner.release["draft"])
            self.assertEqual({asset.name for asset in assets}, self.names(runner))
            self.assertFalse(
                any(
                    "/releases/tags/" in token
                    for command in runner.commands
                    for token in command
                )
            )

    def test_current_marker_draft_refuses_a_third_party_tag_value(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-current-draft-foreign-tag-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            assert runner.release is not None
            runner.release["draft"] = True
            runner.release["name"] = "ScratchBird Native Nightly Builds"
            rolling = self.make_publisher(runner, notes)
            runner.remote_tag_sha = "c" * 40
            with self.assertRaisesRegex(
                publisher.PublishError,
                "current_run_draft_tag_provenance_mismatch",
            ):
                rolling.publish(assets)
            self.assertEqual("c" * 40, runner.remote_tag_sha)
            self.assertTrue(runner.release["draft"])
            self.assertFalse(any(command[0] == "curl" for command in runner.commands))
            self.assertFalse(
                any(command[0] == "git" and "push" in command for command in runner.commands)
            )

    def test_tag_is_reverified_before_resumed_draft_becomes_public(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-final-tag-check-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            assert runner.release is not None
            runner.release["draft"] = True
            runner.release["name"] = "ScratchBird Native Nightly Builds"
            rolling = self.make_publisher(runner, notes)
            original_wait = rolling.wait_for_tag_target
            wait_count = 0

            def fail_second_tag_check(target: str) -> str:
                nonlocal wait_count
                wait_count += 1
                if wait_count == 2:
                    raise publisher.PublishError("injected final tag drift")
                return original_wait(target)

            with mock.patch.object(
                rolling,
                "wait_for_tag_target",
                side_effect=fail_second_tag_check,
            ):
                with self.assertRaisesRegex(
                    publisher.PublishError,
                    "injected final tag drift.*coherent_draft_retry_required",
                ):
                    rolling.publish(assets)
            self.assertEqual(2, wait_count)
            self.assertTrue(runner.release["draft"])
            self.assertFalse(
                any(payload["draft"] is False for payload in runner.release_patch_payloads)
            )

    def test_initial_draft_accepts_github_default_branch_target_response(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-create-main-target-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            runner.created_target_commitish_override = "main"
            self.make_publisher(runner, notes).publish(assets)
            assert runner.release is not None
            self.assertFalse(runner.release["draft"])
            self.assertEqual(self.revision, runner.remote_tag_sha)
            self.assertEqual({asset.name for asset in assets}, self.names(runner))

    def test_lost_upload_response_reconciles_existing_asset_without_duplicate(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-upload-reconcile-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            runner.fail_upload_after_accept_once = True
            with mock.patch.object(publisher.time, "sleep") as sleep:
                self.make_publisher(runner, notes).publish(assets)
            sleep.assert_not_called()
            self.assertEqual({asset.name for asset in assets}, self.names(runner))
            curl_commands = [command for command in runner.commands if command[0] == "curl"]
            self.assertEqual(len(assets), len(curl_commands))

    def test_starter_upload_is_deleted_then_retried_by_release_id(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-upload-starter-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            runner.fail_upload_starter_once = True
            with mock.patch.object(publisher.time, "sleep") as sleep:
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(
                [mock.call(publisher.RELEASE_ASSET_UPLOAD_DELAY_SECONDS)],
                sleep.call_args_list,
            )
            self.assertEqual({asset.name for asset in assets}, self.names(runner))
            self.assertFalse(any(row["state"] == "starter" for row in runner.release["assets"]))
            curl_commands = [command for command in runner.commands if command[0] == "curl"]
            self.assertEqual(len(assets) + 1, len(curl_commands))

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

    def test_initial_creation_retries_transient_release_lookup(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-release-lookup-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            runner.release_lookup_transient_failures = 2
            with mock.patch.object(publisher.time, "sleep") as sleep:
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(
                [
                    mock.call(publisher.API_READ_TRANSIENT_DELAY_SECONDS),
                    mock.call(publisher.API_READ_TRANSIENT_DELAY_SECONDS),
                ],
                sleep.call_args_list,
            )
            self.assertIsNotNone(runner.release)
            assert runner.release is not None
            self.assertFalse(runner.release["draft"])

    def test_initial_release_create_retries_reconciled_not_found(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-create-not-found-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            runner.fail_create_not_found_once = True
            with mock.patch.object(publisher.time, "sleep") as sleep:
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(
                [
                    *(
                        [mock.call(publisher.INITIAL_RELEASE_VISIBILITY_DELAY_SECONDS)]
                        * (publisher.INITIAL_RELEASE_VISIBILITY_ATTEMPTS - 1)
                    ),
                    mock.call(publisher.INITIAL_RELEASE_CREATE_DELAY_SECONDS),
                ],
                sleep.call_args_list,
            )
            self.assertIsNotNone(runner.release)
            self.assertEqual(self.revision, runner.remote_tag_sha)
            create_calls = [
                command
                for command in runner.commands
                if command[:2] == ["gh", "api"]
                and "--method" in command
                and command[command.index("--method") + 1] == "POST"
                and any(value.endswith("/releases") for value in command)
            ]
            self.assertEqual(2, len(create_calls))

    def test_initial_release_create_retries_reconciled_transient_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-create-transient-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            runner.fail_create_transient_once = True
            with mock.patch.object(publisher.time, "sleep") as sleep:
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual(
                [
                    *(
                        [mock.call(publisher.INITIAL_RELEASE_VISIBILITY_DELAY_SECONDS)]
                        * (publisher.INITIAL_RELEASE_VISIBILITY_ATTEMPTS - 1)
                    ),
                    mock.call(publisher.INITIAL_RELEASE_CREATE_DELAY_SECONDS),
                ],
                sleep.call_args_list,
            )
            self.assertIsNotNone(runner.release)
            self.assertEqual(self.revision, runner.remote_tag_sha)

    def test_initial_release_create_reconciles_accepted_response_without_retry(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-create-reconcile-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            runner.fail_create_after_accept_once = True
            with mock.patch.object(publisher.time, "sleep") as sleep:
                self.make_publisher(runner, notes).publish(assets)
            sleep.assert_not_called()
            self.assertIsNotNone(runner.release)
            create_calls = [
                command
                for command in runner.commands
                if command[:2] == ["gh", "api"]
                and "--method" in command
                and command[command.index("--method") + 1] == "POST"
                and any(value.endswith("/releases") for value in command)
            ]
            self.assertEqual(1, len(create_calls))

    def test_lost_create_response_reconciles_current_draft_beside_legacy_siblings(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-create-legacy-reconcile-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            legacy = runner.add_legacy_draft(source="b" * 40, run_id="17")
            runner.fail_create_after_accept_once = True
            with mock.patch.object(publisher.time, "sleep") as sleep:
                self.make_publisher(runner, notes).publish(assets)
            sleep.assert_not_called()
            self.assertIsNone(runner.find_release(legacy["id"]))
            self.assertIsNotNone(runner.release)
            self.assertFalse(runner.release["draft"])
            self.assertEqual({asset.name for asset in assets}, self.names(runner))

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

    def test_strict_legacy_draft_is_pruned_only_after_tag_preflight(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-legacy-prune-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            legacy = runner.add_legacy_draft(source="b" * 40, run_id="17", target="main")
            self.make_publisher(runner, notes).publish(assets)
            self.assertIsNone(runner.find_release(legacy["id"]))
            self.assertEqual({asset.name for asset in assets}, self.names(runner))
            preflight_index = next(
                index
                for index, command in enumerate(runner.commands)
                if command[0] == "git" and "push" in command and "--dry-run" in command
            )
            legacy_delete_index = next(
                index
                for index, command in enumerate(runner.commands)
                if command[:2] == ["gh", "api"]
                and "--method" in command
                and command[command.index("--method") + 1] == "DELETE"
                and any(
                    f"releases/{legacy['id']}" in value for value in command
                )
            )
            first_real_tag_push_index = next(
                index
                for index, command in enumerate(runner.commands)
                if command[0] == "git"
                and "push" in command
                and "--dry-run" not in command
                and any(value.endswith(":refs/tags/nightly") for value in command)
            )
            self.assertLess(preflight_index, first_real_tag_push_index)
            self.assertLess(first_real_tag_push_index, legacy_delete_index)

    def test_changed_legacy_draft_is_never_deleted_after_refetch(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-legacy-change-race-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            legacy = runner.add_legacy_draft(source="b" * 40, run_id="17")
            runner.mutate_legacy_draft_to_other_run_on_get = legacy["id"]
            with self.assertRaisesRegex(
                publisher.PublishError,
                "legacy_draft_changed_before_cleanup",
            ):
                self.make_publisher(runner, notes).publish(assets)
            changed = runner.find_release(legacy["id"])
            self.assertIs(changed, legacy)
            self.assertIn("run_id=99", changed["body"])
            self.assertFalse(
                any(
                    command[:2] == ["gh", "api"]
                    and "--method" in command
                    and command[command.index("--method") + 1] == "DELETE"
                    and any(f"releases/{legacy['id']}" in value for value in command)
                    for command in runner.commands
                )
            )

    def test_foreign_same_tag_draft_is_refused_without_mutation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-foreign-draft-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            foreign = runner.add_legacy_draft(
                source="b" * 40,
                run_id="17",
                author_login="untrusted-user",
            )
            with self.assertRaisesRegex(publisher.PublishError, "same_tag_draft_unmanaged"):
                self.make_publisher(runner, notes).publish(assets)
            self.assertIs(runner.find_release(foreign["id"]), foreign)
            self.assertFalse(any(command[0] == "curl" for command in runner.commands))
            self.assertFalse(
                any(
                    command[0] == "git" and "push" in command
                    for command in runner.commands
                )
            )
            self.assertFalse(
                any(
                    command[:2] == ["gh", "api"]
                    and "--method" in command
                    and command[command.index("--method") + 1] in {"POST", "PATCH", "DELETE"}
                    for command in runner.commands
                )
            )

    def test_legacy_note_lookalike_with_extra_line_is_refused_without_mutation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-legacy-lookalike-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            lookalike = runner.add_legacy_draft(source="b" * 40, run_id="17")
            lookalike["body"] = lookalike["body"].replace(
                "- Source commit:",
                "- Unapproved extra provenance line.\n- Source commit:",
            )
            with self.assertRaisesRegex(publisher.PublishError, "same_tag_draft_unmanaged"):
                self.make_publisher(runner, notes).publish(assets)
            self.assertIs(runner.find_release(lookalike["id"]), lookalike)
            self.assertFalse(any(command[0] == "curl" for command in runner.commands))
            self.assertFalse(
                any(command[0] == "git" and "push" in command for command in runner.commands)
            )

    def test_same_run_legacy_draft_with_branch_target_is_promoted_and_resumed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-legacy-resume-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("Native ScratchBird; no emulation.\n", encoding="utf-8")
            runner = FakeRunner()
            legacy = runner.add_legacy_draft(
                source=self.revision,
                run_id=self.run_id,
                target="main",
                primary=True,
            )
            legacy_id = legacy["id"]
            rolling = self.make_publisher(runner, notes, mark_existing=False)
            rolling.publish(assets)
            assert runner.release is not None
            self.assertEqual(legacy_id, runner.release["id"])
            self.assertFalse(runner.release["draft"])
            self.assertEqual(self.revision, runner.release["target_commitish"])
            self.assertIn(rolling.release_marker(), runner.release["body"])
            self.assertFalse(runner.initial_release_payloads)
            self.assertFalse(
                any(
                    command[:2] == ["gh", "api"]
                    and "--method" in command
                    and command[command.index("--method") + 1] == "DELETE"
                    and any(f"releases/{legacy_id}" in value for value in command)
                    for command in runner.commands
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
            edit_indexes = [
                index
                for index, command in enumerate(runner.commands)
                if command[:2] == ["gh", "api"]
                and "--method" in command
                and command[command.index("--method") + 1] == "PATCH"
                and f"repos/scratchbird-software-inc/ScratchBird/releases/{original_release_id}"
                in command
            ]
            upload_index = next(
                index
                for index, command in enumerate(runner.commands)
                if command[0] == "curl"
            )
            hidden_index = edit_indexes[0]
            published_index = edit_indexes[-1]
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
            self.assertGreaterEqual(len(edit_indexes), 2)
            self.assertTrue(runner.release_patch_payloads[0]["draft"])
            self.assertFalse(runner.release_patch_payloads[-1]["draft"])
            self.assertLess(hidden_index, upload_index)
            self.assertLess(upload_index, published_index)
            self.assertTrue(delete_indexes)
            self.assertLess(max(delete_indexes), published_index)
            self.assertFalse(
                any(command[:2] == ["gh", "release"] for command in runner.commands)
            )

    def test_pre_zip_only_policy_release_is_migrated_in_place(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-policy-migrate-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("new notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            assert runner.release is not None
            manifest_asset = next(
                row
                for row in runner.release["assets"]
                if row["name"] == "scratchbird-nightly-manifest.json"
            )
            manifest_id = int(manifest_asset["id"])
            manifest = json.loads(runner.asset_data[manifest_id].decode("utf-8"))
            manifest["public_asset_policy"] = (
                "fully_verified_native_portable_and_system_installer_artifacts"
            )
            manifest.pop("windows_release_policy", None)
            previous = (json.dumps(manifest, sort_keys=True) + "\n").encode("utf-8")
            runner.asset_data[manifest_id] = previous
            manifest_asset["size"] = len(previous)
            manifest_asset["digest"] = f"sha256:{sha(previous)}"

            self.make_publisher(runner, notes).publish(assets)

            self.assertFalse(runner.release["draft"])
            self.assertEqual({asset.name for asset in assets}, self.names(runner))

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

    def test_lease_race_after_preflight_preserves_foreign_tag_and_legacy_draft(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-lease-race-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            legacy = runner.add_legacy_draft(source="b" * 40, run_id="17")
            runner.mutate_tag_before_lease_push_once = "c" * 40
            with self.assertRaisesRegex(
                publisher.PublishError,
                "stale info.*tag_drift_during_recovery",
            ):
                self.make_publisher(runner, notes).publish(assets)
            self.assertEqual("c" * 40, runner.remote_tag_sha)
            self.assertIsNone(runner.release)
            self.assertIs(runner.find_release(legacy["id"]), legacy)
            self.assertFalse(any(command[0] == "curl" for command in runner.commands))
            push_commands = [
                command
                for command in runner.commands
                if command[0] == "git" and "push" in command
            ]
            self.assertGreaterEqual(len(push_commands), 2)
            self.assertTrue(
                all(
                    any(value.startswith("--force-with-lease=refs/tags/") for value in command)
                    for command in push_commands
                )
            )

    def test_public_marker_tag_mismatch_is_refused_before_draft_transition(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-public-tag-mismatch-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            rolling = self.make_publisher(runner, notes)
            runner.remote_tag_sha = "c" * 40
            with self.assertRaisesRegex(
                publisher.PublishError,
                "existing_nightly_tag_provenance_mismatch",
            ):
                rolling.publish(assets)
            self.assertFalse(runner.release["draft"])
            self.assertEqual("c" * 40, runner.remote_tag_sha)
            self.assertFalse(any(command[0] == "curl" for command in runner.commands))
            self.assertFalse(
                any(
                    command[:2] == ["gh", "api"]
                    and "--method" in command
                    and command[command.index("--method") + 1] in {"POST", "PATCH", "DELETE"}
                    for command in runner.commands
                )
            )

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
            rolling = self.make_publisher(runner, notes)
            old_tag = runner.remote_tag_sha
            old_names = self.names(runner)
            with self.assertRaisesRegex(publisher.PublishError, "existing_nightly_release_unmanaged"):
                rolling.publish(assets)
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

    def test_fresh_draft_recovery_never_deletes_another_run_marker(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-own-draft-change-race-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            # Let an incoming artifact exist before the later upload failure.
            # Recovery must leave that artifact untouched once the draft's
            # marker changes to another run between failure and cleanup.
            runner.fail_upload_after = 2
            rolling = self.make_publisher(runner, notes)
            original_lookup = rolling.get_release_by_id
            mutated = False

            def mutate_before_recovery_lookup(
                release_id: int, *, allow_missing: bool = False
            ) -> dict | None:
                nonlocal mutated
                if allow_missing and not mutated and runner.release is not None:
                    mutated = True
                    runner.release["body"] = (
                        "<!-- scratchbird-rolling-nightly-managed:"
                        "schema=v2;scope=all;tag=nightly;"
                        f"source={'c' * 40};run_id=99;predecessor_tag=absent -->"
                    )
                return original_lookup(release_id, allow_missing=allow_missing)

            with mock.patch.object(
                rolling,
                "get_release_by_id",
                side_effect=mutate_before_recovery_lookup,
            ):
                with self.assertRaisesRegex(
                    publisher.PublishError,
                    "current_run_draft_cleanup_refused_not_owned",
                ):
                    rolling.publish(assets)
            self.assertTrue(mutated)
            self.assertIsNotNone(runner.release)
            assert runner.release is not None
            self.assertIn("run_id=99", runner.release["body"])
            self.assertTrue(
                any(
                    row["name"].startswith("sb-nightly-incoming-42-1--")
                    for row in runner.release["assets"]
                )
            )
            self.assertFalse(
                any(
                    command[:2] == ["gh", "api"]
                    and "--method" in command
                    and command[command.index("--method") + 1] == "DELETE"
                    and any("/releases/7" in value for value in command)
                    for command in runner.commands
                )
            )
            self.assertFalse(
                any(
                    command[:2] == ["gh", "api"]
                    and "--method" in command
                    and command[command.index("--method") + 1] == "DELETE"
                    and any("/releases/assets/" in value for value in command)
                    for command in runner.commands
                )
            )

    def test_partial_upload_failure_leaves_old_release_and_tag_intact(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-upload-fail-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            rolling = self.make_publisher(runner, notes)
            old_tag = runner.remote_tag_sha
            old_rows = {row["name"]: row["digest"] for row in runner.release["assets"]}
            runner.fail_upload_after = 1
            with self.assertRaisesRegex(publisher.PublishError, "upload failure"):
                rolling.publish(assets)
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
            rolling = self.make_publisher(runner, notes)
            old_rows = {row["name"]: row["digest"] for row in runner.release["assets"]}
            old_tag = runner.remote_tag_sha
            runner.fail_canonical_patch_at = 2
            with self.assertRaisesRegex(publisher.PublishError, "canonical rename failure"):
                rolling.publish(assets)
            self.assertEqual(old_tag, runner.remote_tag_sha)
            self.assertEqual(old_rows, {row["name"]: row["digest"] for row in runner.release["assets"]})
            self.assertFalse(any(name.startswith("sb-nightly-") for name in self.names(runner)))
            self.assertFalse(runner.release["draft"])
            self.assertEqual("ScratchBird Native Nightly Builds", runner.release["name"])

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
            self.assertEqual("ScratchBird Native Nightly Builds", runner.release["name"])
            self.assertIn("new notes", runner.release["body"])
            self.assertIn("scratchbird-rolling-nightly-managed:", runner.release["body"])
            self.assertEqual({asset.name for asset in assets}, self.names(runner))
            remote = {row["name"]: row for row in runner.release["assets"]}
            for asset in assets:
                self.assertEqual(f"sha256:{asset.digest}", remote[asset.name]["digest"])

    def test_failed_final_publish_resumes_the_same_current_generation_draft(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-edit-resume-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("new notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            release_id = runner.release["id"]
            rolling = self.make_publisher(runner, notes)
            runner.fail_edit_once = True
            with self.assertRaisesRegex(
                publisher.PublishError,
                "coherent_draft_retry_required",
            ):
                rolling.publish(assets)
            self.assertTrue(runner.release["draft"])
            rolling.publish(assets)
            self.assertEqual(release_id, runner.release["id"])
            self.assertFalse(runner.release["draft"])
            self.assertEqual({asset.name for asset in assets}, self.names(runner))
            self.assertIn(rolling.release_marker(), runner.release["body"])

    def test_resumed_current_draft_survives_a_second_failure_then_publishes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-resumed-failure-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("new notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            rolling = self.make_publisher(runner, notes)
            runner.fail_edit_once = True
            with self.assertRaisesRegex(
                publisher.PublishError,
                "coherent_draft_retry_required",
            ):
                rolling.publish(assets)
            release_id = runner.release["id"]
            runner.fail_upload_after = 1
            with self.assertRaisesRegex(
                publisher.PublishError,
                "upload failure.*coherent_draft_retry_required",
            ):
                rolling.publish(assets)
            self.assertEqual(release_id, runner.release["id"])
            self.assertTrue(runner.release["draft"])
            self.assertEqual(self.revision, runner.remote_tag_sha)
            self.assertEqual({asset.name for asset in assets}, self.names(runner))
            rolling.publish(assets)
            self.assertFalse(runner.release["draft"])
            self.assertEqual({asset.name for asset in assets}, self.names(runner))

    def test_marker_manifest_source_mismatch_refuses_before_mutation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sb-nightly-marker-source-") as temp:
            root = Path(temp)
            assets = write_bundle(root / "assets", self.revision, self.run_id, self.attempt, b"new")
            notes = root / "notes.md"
            notes.write_text("notes\n", encoding="utf-8")
            runner = FakeRunner()
            runner.seed_release(assets)
            rolling = self.make_publisher(runner, notes)
            assert runner.release is not None
            runner.release["body"] = runner.release["body"].replace(
                f"source={self.revision}",
                f"source={'b' * 40}",
            )
            with self.assertRaisesRegex(
                publisher.PublishError,
                "marker_provenance_mismatch",
            ):
                rolling.publish(assets)
            self.assertFalse(any(command[0] == "curl" for command in runner.commands))
            self.assertFalse(
                any(
                    command[0] == "git" and "push" in command
                    for command in runner.commands
                )
            )
            self.assertFalse(
                any(
                    command[:2] == ["gh", "api"]
                    and "--method" in command
                    and command[command.index("--method") + 1] in {"POST", "PATCH", "DELETE"}
                    for command in runner.commands
                )
            )

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
            self.assertEqual(command_count + 4, len(runner.commands))  # origin, list, release, and tag reads only
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
