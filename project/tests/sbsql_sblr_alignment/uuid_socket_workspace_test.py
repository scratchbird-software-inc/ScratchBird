#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Exercise actual AF_UNIX binds for native UUID fixture socket names."""
import importlib
import os
from pathlib import Path
import shutil
import socket
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sbsql_parser_worker"))


def main():
    modules = [importlib.import_module(name) for name in
               ("cdp_config_defaults_rollback_gate", "cdp_security_api_abi_gate")]
    old_tempdir = tempfile.tempdir
    with tempfile.TemporaryDirectory(prefix="sbws_") as directory:
        base = Path(directory)
        # Both normal regression TMPDIR nesting and byte-heavy Unicode paths.
        for preferred in (base / ("nested_" * 12), base / ("é" * 45)):
            tempfile.tempdir = str(preferred)
            selected = []
            try:
                for module in modules:
                    for _ in range(2):
                        root = module.make_work_dir(preferred / "requested")
                        selected.append(root)
                        assert root not in selected[:-1], "workspace collision"
                        assert root.stat().st_uid == os.getuid()
                        assert root.stat().st_mode & 0o077 == 0, "workspace is not private"
                        probes = (root / "ipc/sc/s.sock",
                                  root / "inet/lc" / ("sbsql_" + "f" * 32 + ".management.sock"))
                        for probe in probes:
                            assert len(os.fsencode(probe)) < 100, "socket byte bound changed"
                            probe.parent.mkdir(parents=True, exist_ok=True)
                            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                                listener.bind(str(probe))
                                listener.listen(1)
                                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                                    client.connect(str(probe))
                                    connection, _ = listener.accept()
                                    with connection:
                                        client.sendall(b"uuid-socket")
                                        assert connection.recv(11) == b"uuid-socket"
            finally:
                tempfile.tempdir = old_tempdir
                for root in selected:
                    shutil.rmtree(root)
    print("uuid_socket_workspace=passed real_binds=16")


if __name__ == "__main__":
    main()
