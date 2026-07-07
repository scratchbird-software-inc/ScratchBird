# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Shared SBsql language-resource loader for AI MCP tools."""

from __future__ import annotations

import hashlib
import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_RESOURCE_PACK_REL = Path(
    "project/resources/seed-packs/initial-resource-pack/resources/i18n/"
    "sbsql-language-resource-pack"
)


class LanguageResourceError(RuntimeError):
    """Raised when the shared SBsql resource pack is missing or corrupt."""


def default_repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


def default_resource_pack_root(repo_root: Path | None = None) -> Path:
    configured = os.getenv("SCRATCHBIRD_SBSQL_LANGUAGE_RESOURCE_PACK", "").strip()
    if configured:
        return Path(configured).expanduser().resolve()
    root = repo_root or default_repo_root()
    return root / DEFAULT_RESOURCE_PACK_REL


def _load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise LanguageResourceError(f"unable to load {path}: {exc}") from exc


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return "sha256:" + digest.hexdigest()


@dataclass(slots=True)
class LanguageResourcePack:
    root: Path
    manifest: dict[str, Any]

    @classmethod
    def load(cls, root: Path | None = None, *, verify_hashes: bool = True) -> LanguageResourcePack:
        pack_root = (root or default_resource_pack_root()).resolve()
        manifest_path = pack_root / "manifest.sblrp.json"
        if not manifest_path.is_file():
            raise LanguageResourceError(f"missing SBsql language resource manifest: {manifest_path}")
        manifest = _load_json(manifest_path)
        if not isinstance(manifest, dict):
            raise LanguageResourceError("SBsql language resource manifest must be an object")
        pack = cls(root=pack_root, manifest=manifest)
        pack.validate_manifest(verify_hashes=verify_hashes)
        return pack

    def validate_manifest(self, *, verify_hashes: bool = True) -> None:
        authority = self.manifest.get("authority")
        if not isinstance(authority, dict):
            raise LanguageResourceError("language pack manifest missing authority object")
        required_truth = {
            "local_sblr_uuid_streams_are_untrusted": True,
            "normalization_before_uuid_resolution": True,
            "server_revalidates_sblr_uuid_descriptor_authorization_policy_and_mga": True,
        }
        for key, expected in required_truth.items():
            if authority.get(key) is not expected:
                raise LanguageResourceError(f"language pack authority.{key} must be {expected}")
        profiles = self.manifest.get("profiles")
        if not isinstance(profiles, list) or not profiles:
            raise LanguageResourceError("language pack manifest must list profiles")
        if verify_hashes:
            for row in self.manifest.get("files", []):
                if not isinstance(row, dict):
                    raise LanguageResourceError("language pack file rows must be objects")
                rel = str(row.get("path", "")).strip()
                expected = str(row.get("sha256", "")).strip()
                if not rel or not expected:
                    raise LanguageResourceError("language pack file row missing path or sha256")
                target = self.root / rel
                if not target.is_file():
                    raise LanguageResourceError(f"language pack file missing: {rel}")
                actual = _sha256(target)
                if actual != expected:
                    raise LanguageResourceError(
                        f"language pack hash mismatch for {rel}: expected {expected}, got {actual}"
                    )

    def list_profiles(self) -> list[dict[str, Any]]:
        return [dict(row) for row in self.manifest.get("profiles", []) if isinstance(row, dict)]

    def profile_tags(self) -> list[str]:
        return [str(row.get("exact_tag", "")) for row in self.list_profiles() if row.get("exact_tag")]

    def load_resource(self, rel_path: str) -> Any:
        path = self.root / rel_path
        if not path.is_file():
            raise LanguageResourceError(f"missing language resource: {rel_path}")
        return _load_json(path)

    def predictive_grammar(self) -> dict[str, Any]:
        payload = self.load_resource("resources/predictive/predictive-grammar.json")
        if not isinstance(payload, dict):
            raise LanguageResourceError("predictive grammar must be an object")
        return payload

    def canonical_summary(self) -> dict[str, Any]:
        return {
            "schema_version": self.manifest.get("schema_version"),
            "resource_identity": self.manifest.get("resource_identity"),
            "dialect_profile_uuid": self.manifest.get("dialect_profile_uuid"),
            "topology_profile_uuid": self.manifest.get("topology_profile_uuid"),
            "profile_tags": self.profile_tags(),
            "authority": dict(self.manifest.get("authority", {})),
            "file_count": len(self.manifest.get("files", [])),
        }


def language_resource_summary(root: Path | None = None, *, verify_hashes: bool = True) -> dict[str, Any]:
    pack = LanguageResourcePack.load(root, verify_hashes=verify_hashes)
    return pack.canonical_summary()
