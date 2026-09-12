#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Real FileDevice/server ownership component checks, not database/SBsql E2E."""
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Child:
    def __init__(self, executable, mode, path, argument):
        self.process = subprocess.Popen([executable, mode, str(path), str(argument)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True)
        self.lines = queue.Queue()
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        for line in self.process.stdout:
            self.lines.put(line.strip())
        self.lines.put("<eof>")

    def next(self):
        return self.lines.get(timeout=15)

    def expect(self, message):
        require(self.next() == message, "expected " + message)

    def send(self, message):
        self.process.stdin.write(message + "\n")
        self.process.stdin.flush()

    def release(self):
        self.send("release")
        require(self.process.wait(timeout=15) == 0, "owner release failed")

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=15)
        self.thread.join(timeout=15)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()


def main(executable):
    root = Path(tempfile.mkdtemp(prefix="scratchbird_native_file_owner_"))
    print("retained_fixture=" + str(root), flush=True)
    children = []
    payload = b"Z" * 4096

    def fixture(name):
        path = root / name
        path.write_bytes(payload)
        alias = root / (name + ".hardlink")
        os.link(path, alias)
        return path, alias

    def start(mode, path, argument="hold", expected="held"):
        child = Child(executable, mode, path, argument)
        children.append(child)
        child.expect(expected)
        return child

    def probe(mode, path, expected=2):
        result = subprocess.run([executable, mode, str(path), "probe"],
                                capture_output=True, text=True, timeout=15)
        require(result.returncode == expected, (mode, path, result.returncode, result.stderr))

    try:
        for owner_kind in ("server", "read"):
            path, alias = fixture(owner_kind)
            aliases = [alias]
            if os.name != "nt":
                symlink = root / (owner_kind + ".symlink")
                symlink.symlink_to(path)
                aliases.append(symlink)
            owner = start(owner_kind, path)
            for other in aliases:
                for mode in ("read", "write", "truncate", "server"):
                    probe(mode, other)
                    require(path.read_bytes() == payload, "foreign probe changed primary bytes")
            owner.release()
            probe("server", alias, 0)
            probe("read", alias, 0)

        for mode in ("borrow", "reserve-create"):
            if mode == "borrow":
                path, alias = fixture(mode)
            else:
                path, alias = root / mode, root / (mode + ".hardlink")
            reader = start(mode, path, alias)
            for target in (path, alias):
                probe("server", target)
                probe("truncate", target)
            reader.release()
            probe("server", alias, 0)
            probe("read", path, 0)
            require(path.read_bytes() == payload, "retained reader or creator changed bytes")

        path, alias = fixture("retarget")
        replacement = root / "replacement"
        replacement.write_bytes(b"R" * 4096)
        owner = start("retarget", path, replacement)
        saved = root / "saved-original"
        path.rename(saved)
        replacement.rename(path)
        owner.send("swapped")
        owner.expect("checked")
        probe("server", saved)
        require(path.read_bytes() == b"R" * 4096 and saved.read_bytes() == payload,
                "retarget refusal changed old or replacement file")
        owner.release()
        probe("server", saved, 0)

        for ordinal in range(8):
            path, alias = fixture("race-" + str(ordinal))
            contenders = [start("server", path, "race", "ready"),
                          start("read", alias, "race", "ready")]
            for child in contenders:
                child.send("go")
            outcomes = [child.next() for child in contenders]
            require(sorted(outcomes) == ["held", "refused"], "alias race did not have one owner")
            loser = contenders[outcomes.index("refused")]
            require(loser.process.wait(timeout=15) == 2, "race failed for a non-ownership reason")
            contenders[outcomes.index("held")].release()
            require(path.read_bytes() == payload, "race changed file bytes")

        path, alias = fixture("killed")
        owner = start("server", path)
        probe("read", alias)
        owner.process.kill()
        owner.process.wait(timeout=15)
        probe("read", alias, 0)
        for mode in ("truncate", "truncate-owned"):
            path, alias = fixture(mode)
            probe(mode, path, 0)
            require(path.read_bytes() == b"" and alias.read_bytes() == b"",
                    "authorized truncation did not actually truncate")
        print("PASS real native ownership, alias races, retention, retarget refusal and truncation")
    finally:
        for child in children:
            child.close()


if __name__ == "__main__":
    require(len(sys.argv) == 2, "expected native ownership probe executable")
    main(str(Path(sys.argv[1]).resolve()))
