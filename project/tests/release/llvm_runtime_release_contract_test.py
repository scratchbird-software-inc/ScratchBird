#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import contextlib
from io import BytesIO, StringIO
import os
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


INSTALLER_TOOLS = Path(__file__).resolve().parents[2] / "tools" / "installers"
sys.path.insert(0, str(INSTALLER_TOOLS))

import build_installers as installers  # noqa: E402
import smoke_install_linux_system as linux_smoke  # noqa: E402


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

    @unittest.skipIf(os.name == "nt", "Debian package semantics require POSIX paths")
    def test_debian_members_are_dpkg_compatible_without_pax_headers(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "output"
            output.mkdir()
            payload = self.fixture_payload(root)
            long_path = payload / "opt/ScratchBird/share"
            for index in range(3):
                long_path /= f"segment-{index}-" + ("x" * 80)
            long_path /= "long-path-fixture.txt"
            self.assertGreater(len(long_path.relative_to(payload).as_posix()), 255)
            self.assertTrue(
                all(
                    len(part) < 255
                    for part in long_path.relative_to(payload).parts
                )
            )
            long_path.parent.mkdir(parents=True)
            long_path.write_text("long path fixture\n", encoding="utf-8")
            deb = installers.make_deb(payload, output, "0.0.0-nightly")
            result = linux_smoke.verify_deb_archive_compatibility(
                deb,
                ar_member(deb, "control.tar.gz"),
                ar_member(deb, "data.tar.gz"),
                root,
            )
            self.assertEqual(result["static_tar_header_gate"], "passed")
            self.assertEqual(result["pax_extended_headers"], "absent")
            self.assertGreater(
                result["header_type_counts"]["data.tar.gz"].get("L", 0), 0
            )
            if shutil.which("dpkg-deb"):
                self.assertEqual(result["dpkg_deb"], "info_and_extract_passed")
            if shutil.which("dpkg"):
                expected = (
                    "isolated_non_root_unpack_passed"
                    if os.geteuid() != 0
                    else "isolated_root_unpack_passed"
                )
                self.assertEqual(result["dpkg_unpack"], expected)

    def test_debian_compatibility_gate_rejects_pax_extended_header(self) -> None:
        stream = BytesIO()
        with tarfile.open(
            fileobj=stream, mode="w:gz", format=tarfile.PAX_FORMAT
        ) as archive:
            info = tarfile.TarInfo("fixture")
            info.size = 0
            info.pax_headers = {"comment": "force-pax-extended-header"}
            archive.addfile(info)
        self.assertIn(
            b"x", linux_smoke.tar_header_typeflags(stream.getvalue(), "fixture")
        )
        with contextlib.redirect_stderr(StringIO()):
            with self.assertRaises(SystemExit):
                linux_smoke.verify_deb_archive_compatibility(
                    Path("fixture.deb"),
                    stream.getvalue(),
                    stream.getvalue(),
                    Path("."),
                )

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
