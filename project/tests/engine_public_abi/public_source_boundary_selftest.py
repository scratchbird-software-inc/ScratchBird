#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Exercise the actual source gate without waiving any production boundary."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="sb-source-boundary-selftest-") as temp:
        root = Path(temp)
        engine, server = root / "engine", root / "server"
        engine.mkdir()
        server.mkdir()

        def run():
            return subprocess.run([str(executable), str(engine), str(server)],
                                  capture_output=True, text=True, check=False)

        assert run().returncode == 0, "valid empty sources refused"
        (engine / "locks.cpp").write_text("SpinLock a; atomic_flag b;\n")
        result = run()
        assert result.returncode == 2, result.stderr
        assert "SpinLock" in result.stderr and "atomic_flag" in result.stderr, result.stderr
        (server / "first.cpp").write_text('#include "api_types.hpp"\n')
        nested = server / "nested"
        nested.mkdir()
        (nested / "second.hpp").write_text('#include "engine/internal_api/private.hpp"\n')
        (server / "CMakeLists.txt").write_text("target_link_libraries(server sb_engine_internal_api)\n")
        result = run()
        assert result.returncode == 3, result.stderr
        for token in ("locks.cpp", "SpinLock", "atomic_flag", "first.cpp", "api_types.hpp",
                      "second.hpp", "engine/internal_api", "CMakeLists.txt", "sb_engine_internal_api"):
            assert token in result.stderr, (token, result.stderr)
        # Gate failure is not masked by an unrelated allowed source file.
        (server / "allowed.cpp").write_text('#include "scratchbird/engine.h"\n')
        assert run().returncode == 3
        (engine / "locks.cpp").unlink()
        (server / "first.cpp").unlink()
        (nested / "second.hpp").unlink()
        (server / "CMakeLists.txt").unlink()
        assert run().returncode == 0
    print("PASS source boundary complete diagnostics and unchanged refusals")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
