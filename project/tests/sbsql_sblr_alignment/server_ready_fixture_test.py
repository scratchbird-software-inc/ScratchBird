#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Readiness fixture controls; live policy tests own execution conformance."""

from pathlib import Path
from contextlib import redirect_stderr
from io import StringIO
import subprocess
import sys
import tempfile
from unittest.mock import patch

from ia01_source_map_process_e2e import ProofError, wait_service_ready
from ia01_package_process_e2e import finish_fixture_work, stop


def cleanup_controls(root: Path) -> int:
    controls = 0
    work = root / "failed"
    work.mkdir()
    # Stream multiple chunks, including a Unicode boundary and malformed UTF-8;
    # every diagnostic byte has a visible representation in retained stderr.
    content = "SECURITY.CONTEXT.EXPIRED\n" + "x" * 65530 + "\u20ac" + "y" * 70000 + "\nlast cause\n"
    (work / "server.err").write_bytes(content.encode() + b"\xff")
    (work / "server.out").write_text("exact preceding server output")
    (work / "node.sbdb").write_bytes(b"private reproducible database")
    evidence = StringIO()
    with redirect_stderr(evidence):
        assert finish_fixture_work(work, [], failed=True)
    assert not work.exists()
    assert content in evidence.getvalue() and "\ufffd" in evidence.getvalue()
    assert "exact preceding server output" in evidence.getvalue()
    assert "private reproducible database" not in evidence.getvalue()
    controls += 1

    work = root / "success"
    work.mkdir()
    (work / "server.err").write_text("not a failed diagnostic")
    evidence = StringIO()
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"],
                             start_new_session=True)
    try:
        with redirect_stderr(evidence):
            assert finish_fixture_work(work, [child], failed=False)
        assert child.poll() is not None and not work.exists() and not evidence.getvalue()
        controls += 1
    finally:
        stop(child)

    work = root / "live_group"
    work.mkdir()
    (work / "node.sbdb").write_bytes(b"retain while used")
    child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"],
                             start_new_session=True)
    try:
        # An actual still-live group must block deletion even if the parent
        # teardown function returned. This is a fixture-unit fault injection,
        # not a qualification of server shutdown or database finality.
        with patch("ia01_package_process_e2e.stop"):
            try:
                finish_fixture_work(work, [child], failed=True)
            except ProofError as exc:
                assert "process group still exists" in str(exc)
            else:
                raise AssertionError("deleted a live fixture group's database")
        assert child.poll() is None and (work / "node.sbdb").read_bytes() == b"retain while used"
        controls += 1
    finally:
        stop(child)
    assert finish_fixture_work(work, [child], failed=True)

    work = root / "parent_stop_failure"
    work.mkdir()
    children = [subprocess.Popen([sys.executable, "-c", "import time; time.sleep(30)"],
                                 start_new_session=True) for _ in range(2)]
    attempted = []

    def failed_stop(process):
        attempted.append(process.pid)
        if process is children[1]:
            raise RuntimeError("injected parent stop failure")
        stop(process)

    try:
        with patch("ia01_package_process_e2e.stop", side_effect=failed_stop):
            try:
                finish_fixture_work(work, children, failed=True)
            except ProofError as exc:
                assert "injected parent stop failure" in str(exc)
            else:
                raise AssertionError("ignored an owned parent teardown failure")
        assert attempted == [children[1].pid, children[0].pid]
        assert children[0].poll() is not None and children[1].poll() is None and work.exists()
        controls += 1
    finally:
        for process in children:
            stop(process)
    assert finish_fixture_work(work, children, failed=True)

    work = root / "substituted_log"
    work.mkdir()
    protected = root / "outside.err"
    protected.write_text("do not disclose or delete")
    (work / "server.err").symlink_to(protected)
    evidence = StringIO()
    with redirect_stderr(evidence):
        try:
            finish_fixture_work(work, [], failed=True)
        except OSError:
            pass
        else:
            raise AssertionError("followed a substituted diagnostic symlink")
    assert work.exists() and protected.read_text() == "do not disclose or delete"
    assert "do not disclose" not in evidence.getvalue()
    controls += 1
    (work / "server.err").unlink()
    assert finish_fixture_work(work, [], failed=True)

    work = root / "non_regular_log"
    work.mkdir()
    (work / "server.err").mkdir()
    try:
        finish_fixture_work(work, [], failed=True)
    except ProofError as exc:
        assert "not a regular file" in str(exc)
    else:
        raise AssertionError("accepted a non-regular diagnostic")
    assert work.exists()
    (work / "server.err").rmdir()
    assert finish_fixture_work(work, [], failed=True)
    return controls + 1


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
        controls += cleanup_controls(root)
        print(f"server_ready_fixture=passed controls={controls}")


if __name__ == "__main__":
    main()
