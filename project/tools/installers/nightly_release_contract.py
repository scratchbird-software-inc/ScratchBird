#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Immutable publication contracts for ScratchBird rolling native nightlies."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class ReleaseContract:
    """The exact public inventory for one rolling nightly channel."""

    scope: str
    tag: str
    manifest_name: str
    checksum_name: str
    artifact_roots: tuple[str, ...]
    package_verification: tuple[tuple[str, str], ...]

    @property
    def package_names(self) -> tuple[str, ...]:
        return tuple(name for name, _ in self.package_verification)

    @property
    def verification_by_name(self) -> dict[str, str]:
        return dict(self.package_verification)

    @property
    def canonical_asset_names(self) -> frozenset[str]:
        return frozenset((*self.package_names, self.manifest_name, self.checksum_name))


_LINUX_PACKAGES = (
    ("scratchbird-nightly-linux-x86_64.tar.gz", "exact_native_payload_extraction"),
    ("scratchbird-nightly-linux-x86_64.deb", "installer_manifest_and_privileged_deb_smoke"),
    ("scratchbird-nightly-linux-x86_64.rpm", "installer_manifest_and_rpm_recipe_verification"),
    ("scratchbird-nightly-linux-x86_64-aur.tar.gz", "installer_manifest_and_aur_recipe_verification"),
)
_WINDOWS_PACKAGES = (
    ("scratchbird-nightly-windows-x86_64.zip", "exact_native_payload_extraction"),
    ("scratchbird-nightly-windows-x86_64.msi", "installer_manifest_and_msi_smoke"),
)
_MACOS_PACKAGES = (
    ("scratchbird-nightly-macos-x86_64.tar.gz", "exact_native_payload_extraction"),
    ("scratchbird-nightly-macos-x86_64.pkg", "installer_manifest_and_pkg_smoke"),
    ("scratchbird-nightly-macos-arm64.tar.gz", "exact_native_payload_extraction"),
    ("scratchbird-nightly-macos-arm64.pkg", "installer_manifest_and_pkg_smoke"),
    ("scratchbird-nightly-macos-universal.tar.gz", "exact_native_payload_extraction"),
)


RELEASE_CONTRACTS = {
    "all": ReleaseContract(
        scope="all",
        tag="nightly",
        manifest_name="scratchbird-nightly-manifest.json",
        checksum_name="scratchbird-nightly-SHA256SUMS",
        artifact_roots=(
            "linux",
            "windows",
            "macos-x86_64",
            "macos-arm64",
            "macos-universal",
        ),
        package_verification=_LINUX_PACKAGES + _WINDOWS_PACKAGES + _MACOS_PACKAGES,
    ),
    "linux": ReleaseContract(
        scope="linux",
        tag="nightly-linux",
        manifest_name="scratchbird-nightly-linux-manifest.json",
        checksum_name="scratchbird-nightly-linux-SHA256SUMS",
        artifact_roots=("linux",),
        package_verification=_LINUX_PACKAGES,
    ),
    "windows": ReleaseContract(
        scope="windows",
        tag="nightly-windows",
        manifest_name="scratchbird-nightly-windows-manifest.json",
        checksum_name="scratchbird-nightly-windows-SHA256SUMS",
        artifact_roots=("windows",),
        package_verification=_WINDOWS_PACKAGES,
    ),
    "macos": ReleaseContract(
        scope="macos",
        tag="nightly-macos",
        manifest_name="scratchbird-nightly-macos-manifest.json",
        checksum_name="scratchbird-nightly-macos-SHA256SUMS",
        artifact_roots=("macos-x86_64", "macos-arm64", "macos-universal"),
        package_verification=_MACOS_PACKAGES,
    ),
}


def get_release_contract(scope: str) -> ReleaseContract:
    """Return a closed, named contract; arbitrary tags/scopes are forbidden."""

    try:
        return RELEASE_CONTRACTS[scope]
    except KeyError as exc:
        raise ValueError(f"release_scope_invalid:{scope}") from exc
