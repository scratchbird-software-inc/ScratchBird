# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Owned Linux regression runtimes: retain diagnostics, not database archives.

Only directories created by this object can be removed. Cleanup is deliberately
refused while a process still refers to the runtime (including idle daemons).
The caller must first wait for its test controller to terminate. This does not
collect fixtures that ignore TMPDIR; those need explicit fixture-level cleanup.
"""

import ctypes
import json
import os
from pathlib import Path
import select
import shutil
import stat
import sys
import tempfile
import time


class CleanupRefused(RuntimeError):
    pass


def _process_exit_confirmed(process):
    # Wait on the kernel's process-lifetime handle, not a guessed delay. A
    # bounded wait lets an exiting process reach its terminal state without
    # accepting an inaccessible live process or blocking cleanup indefinitely.
    try:
        descriptor = os.pidfd_open(int(process.name))
    except ProcessLookupError:
        return True
    except AttributeError:
        # Interpreter built without pidfd_open (seen with a portable CPython
        # 3.11): judge the PID's current occupant by its threads instead.
        return _all_threads_terminal(process)
    except OSError:
        return False
    try:
        poller = select.poll()
        poller.register(descriptor, select.POLLIN)
        events = poller.poll(50)
        if not any(fd == descriptor and flags & select.POLLIN for fd, flags in events):
            return False
        # A reused PID must not borrow the old handle's terminal result. If
        # the current proc entry still exists, it must itself be terminal.
        try:
            status = (process / "stat").read_text()
        except (FileNotFoundError, ProcessLookupError):
            return True
        except (OSError, UnicodeError):
            return False
        if not status.startswith(process.name + " ("):
            return False
        _, delimiter, fields = status.rpartition(") ")
        return bool(delimiter and fields.split() and fields.split()[0] in {"Z", "X"})
    finally:
        os.close(descriptor)


def _all_threads_terminal(process, wait_seconds=0.05):
    """True when every thread of the PID's current occupant is Z or X.

    Such a process holds no address space, files or cwd, whoever it is, so a
    reused PID cannot borrow this answer. A zombie leader whose other threads
    still run is not terminal. Waits as long as the pidfd path does."""
    deadline = time.monotonic() + wait_seconds
    while True:
        try:
            status = (process / "stat").read_text()
            tasks = list((process / "task").iterdir())
        except (FileNotFoundError, ProcessLookupError):
            return True
        except (OSError, UnicodeError):
            return False
        if not status.startswith(process.name + " ("):
            return False
        terminal = True
        for task in tasks:
            try:
                fields = (task / "stat").read_text().rpartition(") ")[2].split()
            except (FileNotFoundError, ProcessLookupError):
                continue
            except (OSError, UnicodeError):
                return False
            if not fields or fields[0] not in {"Z", "X"}:
                terminal = False
                break
        if terminal:
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.005)


def _started_before(process, created_wall_seconds):
    """True when the process started before the runtime directory existed.

    Such a process cannot hold a reference to the runtime's random path unless
    the test handed it over, and the test hands it only to processes it starts
    afterwards. Start time comes from /proc/<pid>/stat (world-readable)."""
    if created_wall_seconds is None:
        return False
    try:
        fields = (process / "stat").read_text().rpartition(") ")[2].split()
        start_ticks = int(fields[19])
        boot = next(int(line.split()[1]) for line in
                    Path("/proc/stat").read_text().splitlines() if line.startswith("btime "))
        started = boot + start_ticks / os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError, IndexError, StopIteration):
        return False
    # One second of slack for the boot-time granularity, in the refusing direction.
    return started + 1.0 < created_wall_seconds


_PR_SET_CHILD_SUBREAPER = 36


def _become_child_subreaper():
    """Keep every process this one starts inside its own process tree.

    Without this a double-forked daemon is reparented to init and its ancestry
    no longer shows who started it. Orphans are deliberately not reaped here:
    waiting on any child could take an exit status a caller's Popen expects."""
    if not sys.platform.startswith("linux"):
        return False
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        return libc.prctl(_PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0
    except (OSError, AttributeError):
        return False


def _status_fields(process):
    """Identity fields that stay world-readable for non-dumpable processes."""
    fields = {}
    try:
        stat_fields = (process / "stat").read_text().rpartition(") ")[2].split()
        fields["ppid"] = stat_fields[1]
        fields["start_ticks"] = stat_fields[19]
    except (OSError, IndexError, UnicodeError):
        pass
    try:
        for line in (process / "status").read_text().splitlines():
            key, _, value = line.partition(":")
            if key in ("Name", "Uid", "NSpid"):
                fields[key] = " ".join(value.split())
    except (OSError, UnicodeError):
        pass
    return fields


def _descends_from(process, ancestor):
    """True unless the ppid chain provably ends without passing ancestor.

    Any unreadable link counts as a descendant, so doubt still refuses."""
    pid = int(process.name)
    for _ in range(4096):
        if pid == ancestor:
            return True
        if pid <= 1:
            return False
        try:
            parent = int(Path("/proc", str(pid), "stat").read_text()
                         .rpartition(") ")[2].split()[1])
        except (OSError, ValueError, IndexError, UnicodeError):
            return True
        if parent == pid:
            return True
        pid = parent
    return True


def _active_references(root, created_wall_seconds=None, tree_root=None):
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
            # Procfs can revoke access while an owned process exits. Only a
            # fresh disappearance or verified exit permits continuing; a live
            # process (including a reused PID) remains a fail-closed refusal.
            try:
                process.stat()
            except (FileNotFoundError, ProcessLookupError):
                continue
            if _process_exit_confirmed(process):
                continue
            # A live process this user cannot inspect (another sandbox or VM
            # running under the same uid) that predates the runtime cannot
            # reference it; skipping it does not weaken the check.
            if _started_before(process, created_wall_seconds):
                continue
            # Nor can one outside the creator's process tree: the random path
            # is only handed to processes the test starts, and as a child
            # subreaper the creator keeps even daemonized ones in its tree.
            # A non-dumpable process from a parallel test lands here.
            if tree_root is not None and not _descends_from(process, tree_root):
                continue
            details = " ".join(f"{key}={value}" for key, value in
                               sorted(_status_fields(process).items()))
            raise CleanupRefused("cannot inspect an owned process: " + process.name +
                                 (" (" + details + ")" if details else "")) from error
    return active


class OwnedTestRuntime:
    def __init__(self, parent=None):
        self._created_wall_seconds = time.time()
        # Set before the test starts anything that is handed the path.
        self._tree_root = os.getpid() if _become_child_subreaper() else None
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

    def _tree_root_here(self):
        # The tree rule only holds in the creating, subreaping process.
        return self._tree_root if self._tree_root == os.getpid() else None

    def cleanup(self, evidence, *, controller_finished):
        if self._removed:
            raise CleanupRefused("runtime already removed")
        if not controller_finished:
            raise CleanupRefused("test controller has not terminated")
        files = self._inventory()
        active = _active_references(self.path, self._created_wall_seconds, self._tree_root_here())
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
        active = _active_references(self.path, self._created_wall_seconds, self._tree_root_here())
        if active:
            raise CleanupRefused("runtime became active: " + repr(active))
        receipt = {"runtime": str(self.path), "diagnostic_bytes_retained": retained,
                   "omitted_diagnostics": omitted,
                   "allocated_bytes_removed": sum(info.st_blocks * 512 for _, info in files)}
        shutil.rmtree(self.path)
        self._removed = True
        (evidence / "runtime-cleanup.json").write_text(json.dumps(receipt, indent=2) + "\n")
        return receipt
