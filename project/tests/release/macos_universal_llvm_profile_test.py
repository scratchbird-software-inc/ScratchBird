#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import tarfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "project" / "tools" / "installers" / "make_macos_universal.py"
SPEC = importlib.util.spec_from_file_location("make_macos_universal", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
universal = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(universal)

sys.path.insert(0, str(REPO_ROOT / "project" / "tools" / "release"))
import stage_native_release_bundle as native  # noqa: E402


class MacosUniversalLlvmProfileTest(unittest.TestCase):
    def write_profile(
        self, root: Path, runtime_library: str, minimum_major: int = 22
    ) -> None:
        path = root / universal.NATIVE_PROFILE_RELATIVE_PATH
        path.parent.mkdir(parents=True)
        path.write_text(
            json.dumps(
                {
                    "schema_id": "scratchbird.native_release_profile.v1",
                    "profile": "native-sbsql-only",
                    "platform": "macos",
                    "native_parser": "SBSQL",
                    "emulation_components": "excluded",
                    "executables": ["SBsrv"],
                    "libraries": ["lib/libSBcore.dylib"],
                    "runtime_dependencies": [],
                    "llvm_runtime": {
                        "link_mode": "dynamic",
                        "runtime_library": runtime_library,
                        "delivery": "external-homebrew",
                        "minimum_major": minimum_major,
                    },
                }
            )
            + "\n",
            encoding="utf-8",
        )

    def test_reconciles_per_architecture_homebrew_paths_explicitly(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            x86 = root / "x86"
            arm = root / "arm"
            merged = root / "merged"
            self.write_profile(x86, "/usr/local/opt/llvm/lib/libLLVM.dylib")
            self.write_profile(arm, "/opt/homebrew/opt/llvm/lib/libLLVM.dylib")
            (merged / universal.NATIVE_PROFILE_RELATIVE_PATH).parent.mkdir(
                parents=True
            )
            universal.reconcile_native_profile(x86, arm, merged)
            profile = json.loads(
                (merged / universal.NATIVE_PROFILE_RELATIVE_PATH).read_text(
                    encoding="utf-8"
                )
            )
            llvm = native.require_llvm_runtime_contract(
                profile["llvm_runtime"], "macos"
            )
            self.assertIsNone(llvm["runtime_library"])
            self.assertEqual(
                {
                    "x86_64": "/usr/local/opt/llvm/lib/libLLVM.dylib",
                    "arm64": "/opt/homebrew/opt/llvm/lib/libLLVM.dylib",
                },
                llvm["runtime_libraries_by_architecture"],
            )

    def test_preserves_equal_newer_minimum_major(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            x86 = root / "x86"
            arm = root / "arm"
            merged = root / "merged"
            self.write_profile(
                x86, "/usr/local/opt/llvm/lib/libLLVM.dylib", minimum_major=24
            )
            self.write_profile(
                arm, "/opt/homebrew/opt/llvm/lib/libLLVM.dylib", minimum_major=24
            )
            (merged / universal.NATIVE_PROFILE_RELATIVE_PATH).parent.mkdir(
                parents=True
            )
            universal.reconcile_native_profile(x86, arm, merged)
            profile = json.loads(
                (merged / universal.NATIVE_PROFILE_RELATIVE_PATH).read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(24, profile["llvm_runtime"]["minimum_major"])

    def test_rejects_silently_reusing_one_architecture_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            x86 = root / "x86"
            arm = root / "arm"
            merged = root / "merged"
            path = "/opt/homebrew/opt/llvm/lib/libLLVM.dylib"
            self.write_profile(x86, path)
            self.write_profile(arm, path)
            (merged / universal.NATIVE_PROFILE_RELATIVE_PATH).parent.mkdir(
                parents=True
            )
            with self.assertRaises(SystemExit):
                universal.reconcile_native_profile(x86, arm, merged)

    def test_tar_extraction_rejects_traversal_and_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for case in ("traversal", "symlink"):
                archive_path = root / f"{case}.tar.gz"
                with tarfile.open(archive_path, "w:gz") as archive:
                    if case == "traversal":
                        data = b"unsafe"
                        member = tarfile.TarInfo("../outside")
                        member.size = len(data)
                        archive.addfile(member, io.BytesIO(data))
                    else:
                        member = tarfile.TarInfo("opt/ScratchBird/bin/SBsrv")
                        member.type = tarfile.SYMTYPE
                        member.linkname = "/tmp/unsafe"
                        archive.addfile(member)
                with self.assertRaises(SystemExit):
                    universal.extract_tarball(archive_path, root / f"extract-{case}")
                self.assertFalse((root / "outside").exists())


if __name__ == "__main__":
    unittest.main()
