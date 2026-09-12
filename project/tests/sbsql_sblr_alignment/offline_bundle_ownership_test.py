#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Independent-process offline bundle ownership and unchanged-input checks."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from native_file_ownership_test import Child, require


def main(tool, owner_probe, bundle_probe):
    root = Path(tempfile.mkdtemp(prefix="scratchbird_offline_bundle_owner_"))
    print("retained_fixture=" + str(root), flush=True)
    failures = []
    children = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    def run(arguments):
        return subprocess.run([tool, *map(str, arguments)], capture_output=True,
                              text=True, timeout=15)

    seed = root / "seed"
    result = run(["--self-test", "--work-dir", seed, "--out", seed / "bundle"])
    check(result.returncode == 0, "self-test failed: " + result.stderr.strip())
    primary = seed / "public-resource.sbdb"
    secondary = seed / "public-resource-secondary.sbfs"
    original = {primary: primary.read_bytes(), secondary: secondary.read_bytes()}

    def arguments(out, database=primary, filespace=secondary):
        return ["--database", database, "--filespace", filespace, "--out", out]

    def start(mode, path):
        child = Child(owner_probe, mode, path, "hold")
        children.append(child)
        child.expect("held")
        return child

    try:
        ordinal = 0
        for target in (primary, secondary):
            hardlink = target.with_suffix(".hardlink")
            os.link(target, hardlink)
            aliases = [target, hardlink]
            if os.name != "nt":
                symlink = target.with_suffix(".symlink")
                symlink.symlink_to(target)
                aliases.append(symlink)
            for mode in ("server", "read", "write"):
                owner = start(mode, target)
                for alias in aliases:
                    out = root / ("refused-" + str(ordinal))
                    ordinal += 1
                    result = run(arguments(out, alias if target == primary else primary,
                                           alias if target == secondary else secondary))
                    check(result.returncode != 0 and "OWNER-LOCK-HELD" in result.stderr,
                          "foreign owner bypass: " + mode + " " + str(alias))
                    check(not out.exists(), "refused bundle was published: " + str(out))
                    check(all(p.read_bytes() == b for p, b in original.items()),
                          "foreign-owner attempt changed input")
                owner.release()
                out = root / ("released-" + str(ordinal))
                result = run(arguments(out))
                check(result.returncode == 0 and out.is_file() and
                      b"database.health=ok" in out.read_bytes(), "real post-release bundle failed")
                # A failed secondary acquisition must not leak primary ownership.
                after = start("server", primary)
                after.release()

        owner = start("server", primary)
        owner.process.kill()
        owner.process.wait(timeout=15)
        result = run(arguments(root / "crash-handoff"))
        check(result.returncode == 0, "crash handoff failed")

        # Every overwrite attempt targets only fresh disposable header fixtures.
        for name in ("primary", "primary-hardlink", "secondary", "existing-output"):
            case = root / name
            case.mkdir()
            db, fs = case / "db", case / "fs"
            db.write_bytes(original[primary])
            fs.write_bytes(original[secondary])
            out = db if name == "primary" else fs
            if name == "primary-hardlink":
                out = case / "db-alias"
                os.link(db, out)
            if name == "existing-output":
                out = case / "old-report"
                out.write_bytes(b"existing report must survive")
            expected_output = out.read_bytes()
            result = run(arguments(out, db, fs))
            check(result.returncode != 0, "overwrite accepted: " + name)
            check(db.read_bytes() == original[primary] and fs.read_bytes() == original[secondary]
                  and out.read_bytes() == expected_output, "overwrite changed input/output: " + name)

        missing_out = root / "missing-refusal"
        result = run(arguments(missing_out, filespace=root / "absent"))
        check(result.returncode != 0 and not missing_out.exists(), "missing input fabricated bundle")

        def bundle_child(mode, directory, expected="held"):
            child = Child(bundle_probe, mode, directory, "hold")
            children.append(child)
            child.expect(expected)
            return child

        def probe_held(target):
            result = subprocess.run([bundle_probe, "probe-file", str(target), "probe"],
                                    capture_output=True, text=True, timeout=15)
            check(result.returncode == 2 and "OWNER-LOCK-HELD" in result.stderr,
                  "input owner disappeared: " + str(target))

        for mode in ("hold", "hold-aux"):
            held = bundle_child(mode, seed)
            targets = [primary, secondary]
            if mode == "hold-aux":
                targets += [seed / "filespace.registry", seed / "dirty.manifest"]
            for target in targets:
                probe_held(target)
            held.send("build")
            held.expect("checked")
            for target in targets:
                probe_held(target)
            held.release()
            for target in targets:
                released = start("server", target)
                released.release()

        # Partial acquisition releases immediately even while the failed caller lives.
        second_owner = start("server", secondary)
        partial = bundle_child("fail-acquire", seed, "refused")
        first_owner = start("server", primary)
        first_owner.release()
        partial.release()
        second_owner.release()

        killed = bundle_child("hold", seed)
        probe_held(primary)
        probe_held(secondary)
        killed.process.kill()
        killed.process.wait(timeout=15)
        for target in (primary, secondary):
            released = start("server", target)
            released.release()
        result = run(arguments(root / "bundle-owner-crash-handoff"))
        check(result.returncode == 0, "bundle owner death did not release the input set")

        for ordinal in range(8):
            contenders = []
            for side in ("a", "b"):
                directory = root / ("race-" + str(ordinal) + side)
                directory.mkdir()
                os.link(primary, directory / primary.name)
                os.link(secondary, directory / secondary.name)
                contenders.append(bundle_child("race", directory, "ready"))
            for child in contenders:
                child.send("go")
            outcomes = [child.next() for child in contenders]
            require(sorted(outcomes) == ["held", "refused"], "set race did not admit one owner")
            loser = contenders[outcomes.index("refused")]
            require(loser.process.wait(timeout=15) == 2, "set race failed for non-ownership reason")
            winner = contenders[outcomes.index("held")]
            winner.send("build")
            winner.expect("checked")
            probe_held(primary)
            probe_held(secondary)
            winner.release()
        check(all(p.read_bytes() == b for p, b in original.items()), "held set or races changed inputs")
        for failure in failures:
            print("FAIL " + failure, flush=True)
        require(not failures, str(len(failures)) + " offline bundle failures")
        print("PASS offline bundle foreign-owner exclusion, real handoff and non-overwriting output")
    finally:
        for child in children:
            child.close()


if __name__ == "__main__":
    require(len(sys.argv) == 4, "expected tool, owner probe and bundle probe")
    main(*(str(Path(argument).resolve()) for argument in sys.argv[1:]))
