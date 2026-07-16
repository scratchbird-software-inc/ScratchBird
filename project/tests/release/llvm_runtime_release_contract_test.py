#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

from io import BytesIO
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


INSTALLER_TOOLS = Path(__file__).resolve().parents[2] / "tools" / "installers"
sys.path.insert(0, str(INSTALLER_TOOLS))

import build_installers as installers  # noqa: E402


def ar_member(path: Path, requested_name: str) -> bytes:
    data = path.read_bytes()
    if not data.startswith(b"!<arch>\n"):
        raise AssertionError("not an ar archive")
    offset = 8
    while offset < len(data):
        header = data[offset : offset + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            raise AssertionError("invalid ar member header")
        name = header[:16].decode("ascii").strip().rstrip("/")
        size = int(header[48:58].decode("ascii").strip())
        start = offset + 60
        payload = data[start : start + size]
        if name == requested_name:
            return payload
        offset = start + size + (size % 2)
    raise AssertionError(f"ar member not found: {requested_name}")


class LlvmRuntimeReleaseContractTest(unittest.TestCase):
    def fixture_payload(self, root: Path) -> Path:
        payload = root / "payload"
        (payload / "opt" / "ScratchBird").mkdir(parents=True)
        (payload / "opt" / "ScratchBird" / "fixture").write_text(
            "fixture\n", encoding="utf-8"
        )
        (payload / "etc" / "scratchbird").mkdir(parents=True)
        return payload

    def test_debian_control_declares_dlopen_only_llvm_runtime(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "output"
            output.mkdir()
            deb = installers.make_deb(
                self.fixture_payload(root), output, "0.0.0-nightly"
            )
            control_archive = ar_member(deb, "control.tar.gz")
            with tarfile.open(fileobj=BytesIO(control_archive), mode="r:gz") as archive:
                control = archive.extractfile("control")
                self.assertIsNotNone(control)
                text = control.read().decode("utf-8")
            self.assertIn("Depends:", text)
            self.assertIn("libllvm23", text)

    def test_rpm_spec_declares_dlopen_only_llvm_runtime(self) -> None:
        with tempfile.TemporaryDirectory() as temp, mock.patch.object(
            installers.shutil, "which", return_value=None
        ):
            root = Path(temp)
            output = root / "output"
            output.mkdir()
            installers.make_rpm(
                self.fixture_payload(root),
                output,
                "0.0.0-nightly",
                require_rpm=False,
            )
            spec = output / "rpm-build" / "SPECS" / "scratchbird.spec"
            self.assertIn("Requires: llvm-libs >= 23", spec.read_text(encoding="utf-8"))

    def test_aur_recipe_declares_dlopen_only_llvm_runtime(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "output"
            output.mkdir()
            installers.make_aur(
                self.fixture_payload(root), output, "0.0.0-nightly"
            )
            pkgbuild = output / "aur" / "scratchbird" / "PKGBUILD"
            self.assertIn("'llvm-libs>=23'", pkgbuild.read_text(encoding="utf-8"))

    def test_macos_support_contract_is_explicitly_external(self) -> None:
        prerequisites = installers.MACOS_SUPPORT_MATRIX[
            "external_runtime_prerequisites"
        ]
        self.assertEqual(prerequisites["policy"], "qa_packages_do_not_bundle_homebrew_llvm")
        self.assertEqual(prerequisites["homebrew"][0]["formula"], "llvm")
        self.assertEqual(prerequisites["homebrew"][0]["minimum_major"], 22)

    def test_install_smokes_load_the_declared_llvm_runtime(self) -> None:
        linux = (INSTALLER_TOOLS / "smoke_install_linux.sh").read_text(
            encoding="utf-8"
        )
        macos = (INSTALLER_TOOLS / "smoke_install_macos.sh").read_text(
            encoding="utf-8"
        )
        windows = (INSTALLER_TOOLS / "smoke_install_windows.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn("ctypes.CDLL", linux)
        self.assertIn("ctypes.CDLL", macos)
        self.assertIn("NativeLibrary]::Load", windows)


if __name__ == "__main__":
    unittest.main()
