# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Owned Linux regression runtimes: retain diagnostics, not database archives.

Only directories created by this object can be removed. Cleanup is deliberately
refused while a process still refers to the runtime (including idle daemons).
The caller must first wait for its test controller to terminate. This does not
collect fixtures that ignore TMPDIR; those need explicit fixture-level cleanup.
"""

import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile


class CleanupRefused(RuntimeError):
    pass


def _active_references(root):
    if not sys.platform.startswith("linux"):
        raise CleanupRefused("live-process inspection requires the Linux runner")
    needle = os.fsencode(root)
    active = []
    for process in Path("/proc").iterdir():
        if not process.name.isdigit() or int(process.name) == os.getpid():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            found = False
            for name in ("cmdline", "environ", "maps"):
                if needle in (process / name).read_bytes():
                    found = True
                    break
            if not found:
                for link in [process / "cwd", process / "exe", *(process / "fd").iterdir()]:
                    try:
                        if needle in os.fsencode(os.readlink(link)):
                            found = True
                            break
                    except FileNotFoundError:
                        continue
            if found:
                active.append(int(process.name))
        except (FileNotFoundError, ProcessLookupError):
            continue  # Process terminated during inspection.
        except PermissionError as error:
            raise CleanupRefused("cannot inspect an owned process: " + process.name) from error
    return active


class OwnedTestRuntime:
    def __init__(self, parent=None):
        self.path = Path(tempfile.mkdtemp(prefix="sbuidreg", dir=parent))
        info = self.path.lstat()
        self._identity = (info.st_dev, info.st_ino, info.st_uid)
        self._removed = False

    def _inventory(self):
        info = self.path.lstat()
        if (not stat.S_ISDIR(info.st_mode) or
                (info.st_dev, info.st_ino, info.st_uid) != self._identity or
                info.st_uid != os.getuid() or info.st_mode & 0o077):
            raise CleanupRefused("runtime identity, owner or private mode changed")
        found = []
        pending = [self.path]
        while pending:
            directory = pending.pop()
            for child in directory.iterdir():
                metadata = child.lstat()
                if metadata.st_uid != os.getuid() or metadata.st_dev != info.st_dev:
                    raise CleanupRefused("foreign owner or mount inside runtime: " + str(child))
                found.append((child, metadata))
                if stat.S_ISDIR(metadata.st_mode):
                    pending.append(child)
                elif not any(check(metadata.st_mode) for check in
                             (stat.S_ISREG, stat.S_ISLNK, stat.S_ISSOCK, stat.S_ISFIFO)):
                    raise CleanupRefused("unexpected runtime object: " + str(child))
        return found

    def cleanup(self, evidence, *, controller_finished):
        if self._removed:
            raise CleanupRefused("runtime already removed")
        if not controller_finished:
            raise CleanupRefused("test controller has not terminated")
        files = self._inventory()
        active = _active_references(self.path)
        if active:
            raise CleanupRefused("runtime still referenced by PIDs " + repr(active))
        evidence = Path(evidence).resolve()
        if evidence == self.path or self.path in evidence.parents:
            raise CleanupRefused("diagnostic destination is inside runtime")
        destination = evidence / "runtime-diagnostics"
        destination.mkdir(parents=True, exist_ok=False)
        retained = 0
        omitted = []
        suffixes = {".log", ".out", ".err", ".json", ".jsonl", ".xml", ".sql", ".tsv", ".sbobs"}
        for path, metadata in files:
            relative = path.relative_to(self.path)
            # Exported source trees and dependency/build copies are regenerable,
            # not run diagnostics. Never follow symlinks or read FIFOs/sockets.
            if (not stat.S_ISREG(metadata.st_mode) or path.suffix not in suffixes or
                    any(part in {"project", "node_modules", ".build", "build", ".git"}
                        for part in relative.parts)):
                continue
            if metadata.st_size > 4 * 1024 * 1024 or retained + metadata.st_size > 32 * 1024 * 1024:
                omitted.append({"path": str(relative), "bytes": metadata.st_size,
                                "reason": "diagnostic retention budget"})
                continue
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            # Refuse a file substituted since enumeration, rather than copying
            # an external target into an apparent diagnostic receipt.
            with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW), "rb") as source:
                current = os.fstat(source.fileno())
                if (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns) != (
                        metadata.st_dev, metadata.st_ino, metadata.st_size, metadata.st_mtime_ns):
                    raise CleanupRefused("diagnostic changed during cleanup: " + str(path))
                with target.open("xb") as output:
                    shutil.copyfileobj(source, output)
            retained += metadata.st_size
        self._inventory()
        active = _active_references(self.path)
        if active:
            raise CleanupRefused("runtime became active: " + repr(active))
        receipt = {"runtime": str(self.path), "diagnostic_bytes_retained": retained,
                   "omitted_diagnostics": omitted,
                   "allocated_bytes_removed": sum(info.st_blocks * 512 for _, info in files)}
        shutil.rmtree(self.path)
        self._removed = True
        (evidence / "runtime-cleanup.json").write_text(json.dumps(receipt, indent=2) + "\n")
        return receipt
