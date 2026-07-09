#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public GitHub Actions release automation shape."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


REQUIRED_WORKFLOWS = {
    "ci-linux.yml": (
        "workflow_dispatch:",
        "cmake --preset public-release-linux",
        "ctest --preset public-release-linux",
        "verify_public_release_bundle.py",
        "public_packaging_history_gate.py",
    ),
    "ci-windows.yml": (
        "workflow_dispatch:",
        "cmake --preset public-release-windows",
        "ctest --preset public-release-windows",
        "verify_public_release_bundle.py",
        "public_packaging_history_gate.py",
    ),
    "ci-macos.yml": (
        "workflow_dispatch:",
        "SB_MACOS_CI_ENABLED",
        "macos-15-intel",
        "macos-15",
        "cmake --preset public-release-macos",
        "run_ctest_chunks.py",
        "--preset public-release-macos",
        "verify_public_release_bundle.py",
        "build_installers.py",
        "make_macos_universal.py",
        "verify_installer_artifacts.py",
        "smoke_install_macos.sh",
    ),
    "verify-installers.yml": (
        "workflow_dispatch:",
        "build_installers.py",
        "verify_installer_artifacts.py",
        "smoke_install_linux.sh",
        "smoke_install_windows.ps1",
        "smoke_install_macos.sh",
        "make_macos_universal.py",
        "public-release-macos",
    ),
    "nightly-installers.yml": (
        "schedule:",
        "workflow_dispatch:",
        "SB_NIGHTLY_INSTALLERS_ENABLED",
        "build_installers.py",
        "verify_installer_artifacts.py",
        "macos",
    ),
    "release-candidate.yml": (
        "workflow_dispatch:",
        "gh release",
        "verify_installer_artifacts.py",
        "INSTALLER_ARTIFACT_MANIFEST.json",
        "macos",
    ),
}


FORBIDDEN_TOKENS = (
    "packaging/",
    "packaging\\",
    "ScratchBird" + "-Private",
    "/home/",
    "docs/workplans",
    "docs/specifications",
)


def fail(message: str) -> None:
    print(f"github_actions_static_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require_token(text: str, token: str, rel: str) -> None:
    if token not in text:
        fail(f"missing_token:{rel}:{token}")


def check_permissions(text: str, rel: str) -> None:
    if "permissions:" not in text:
        fail(f"missing_permissions:{rel}")
    if re.search(r"contents:\s+write", text) and "release-candidate" not in rel and "nightly-installers" not in rel:
        fail(f"unexpected_contents_write:{rel}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    workflow_root = repo_root / ("." + "github") / "workflows"
    if not workflow_root.is_dir():
        fail("workflow_root_missing")
    for name, tokens in REQUIRED_WORKFLOWS.items():
        path = workflow_root / name
        if not path.is_file():
            fail(f"workflow_missing:{name}")
        text = path.read_text(encoding="utf-8")
        check_permissions(text, name)
        for token in tokens:
            require_token(text, token, name)
        for token in FORBIDDEN_TOKENS:
            if token in text:
                fail(f"forbidden_token:{name}:{token}")
    dependabot = repo_root / ("." + "github") / "dependabot.yml"
    if not dependabot.is_file():
        fail("dependabot_missing")
    dependabot_text = dependabot.read_text(encoding="utf-8")
    for token in ("package-ecosystem: github-actions", "directory: /"):
        require_token(dependabot_text, token, "dependabot.yml")
    print("github_actions_static_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
