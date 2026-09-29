#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Readiness fixture controls; live policy tests own execution conformance."""

from pathlib import Path
import subprocess
import sys
import tempfile

from ia01_source_map_process_e2e import ProofError, wait_service_ready


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="sbready_") as temporary:
        root = Path(temporary)
        endpoint = root / "s.sock"
        state = root / "sb_server.lifecycle.state"
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(10)"])
        controls = 0
        try:
            valid = ("format=SB_SERVER_LIFECYCLE_STATE_V1\n"
                     "generation=1\nstate=service_ready\nservice_ready=true\n"
                     f"sbps_endpoint={endpoint}\n")
            invalid = [
                b"",
                valid.replace("service_ready=true", "service_ready=false").encode(),
                valid.replace("state=service_ready", "state=starting").encode(),
                valid.replace("generation=1", "generation=0").encode(),
                valid.replace("generation=1", "generation=invalid").encode(),
                valid.replace("STATE_V1", "STATE_V0").encode(),
                valid.replace(str(endpoint), str(root / "other.sock")).encode(),
                (valid + "service_ready=true\n").encode(),
                (valid + "partial_field").encode(),
                valid.encode() + b"\xff",
            ]
            for payload in invalid:
                state.write_bytes(payload)
                try:
                    wait_service_ready(child, endpoint, timeout=0.02)
                except ProofError:
                    controls += 1
                else:
                    raise AssertionError("incomplete or mismatched readiness was accepted")
            state.write_text(valid)
            wait_service_ready(child, endpoint, timeout=1)
            controls += 1
            child.terminate()
            child.wait(timeout=5)
            try:
                wait_service_ready(child, endpoint, timeout=1)
            except ProofError:
                controls += 1
            else:
                raise AssertionError("exited server with stale readiness was accepted")
        finally:
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=5)
        print(f"server_ready_fixture=passed controls={controls}")


if __name__ == "__main__":
    main()
