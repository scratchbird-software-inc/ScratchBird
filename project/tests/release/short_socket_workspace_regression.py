#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Exercise actual allocators against independent Linux socket path oracles."""
import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import socket
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--repo-root', type=Path, required=True)
args, remaining = parser.parse_known_args()
ROOT = args.repo_root.resolve()
sys.dont_write_bytecode = True
for directory in ('sbsql_parser_worker', 'sbsql_sblr_alignment'):
    sys.path.insert(0, str(ROOT / 'project/tests' / directory))
CASES = (
    ('sbsql_parser_worker/cdp_profiler_evidence_gate', 'make_work_dir', 'i/sc', 'n/lc'),
    ('sbsql_parser_worker/cdp_route_split_benchmark_gate', 'make_work_dir', 'i/sc', 'n/lc'),
    ('sbsql_parser_worker/sbsql_copy_persistence_full_route_gate', 'make_work_dir', 'plain/restart/sc', 'plain/restart/lc'),
    ('sbsql_parser_worker/cdp_copy_route_gate', 'make_work_dir', 'ipc/sc', 'inet/lc'),
    ('sbsql_parser_worker/live_server_agent_storage_benchmark_gate', 'make_work_dir', 'sc1', 'lc'),
    ('sbsql_sblr_alignment/ia01_package_process_e2e', 'allocate_work', 'sc', 'lc'),
)


@unittest.skipUnless(sys.platform.startswith('linux'), 'Linux AF_UNIX boundary regression')
class ShortSocketWorkspaces(unittest.TestCase):
    def test_actual_allocators(self):
        for name, function, server_dir, listener_dir in CASES:
            path = ROOT / 'project/tests' / (name + '.py')
            spec = importlib.util.spec_from_file_location(Path(name).name, path)
            module = importlib.util.module_from_spec(spec)
            sys.modules[spec.name] = module
            spec.loader.exec_module(module)
            for scenario in ('long', 'unicode', 'registered'):
                with self.subTest(script=name, scenario=scenario):
                    prefix = '\U0001f9ea' * 8 if scenario == 'unicode' else 'sbuidreg'
                    with tempfile.TemporaryDirectory(prefix=prefix, dir='/tmp') as temporary:
                        root = Path(temporary)
                        preferred = root if scenario == 'unicode' else root / ('nested-' * 20)
                        previous = tempfile.tempdir
                        tempfile.tempdir = str(preferred if scenario == 'long' else root)
                        work = second = None
                        try:
                            work = getattr(module, function)(preferred)
                            second = getattr(module, function)(preferred)
                            self.assertNotEqual(work, second)
                            self.assertEqual(work.stat().st_mode & 0o777, 0o700)
                            endpoints = (work / server_dir / 's.sock',
                                work / listener_dir / ('sbsql_' + '0' * 32 + '.management.sock'))
                            for endpoint in endpoints:
                                self.assertLess(len(os.fsencode(endpoint)), 100)
                                endpoint.parent.mkdir(parents=True, exist_ok=True)
                                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                                    listener.bind(str(endpoint))
                                    listener.listen(1)
                                    listener.settimeout(5)
                                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                                        client.settimeout(5)
                                        client.connect(str(endpoint))
                                        peer, _ = listener.accept()
                                        with peer:
                                            peer.sendall(b'bound')
                                            self.assertEqual(client.recv(5), b'bound')
                        finally:
                            tempfile.tempdir = previous
                            for path in (work, second):
                                if path is not None:
                                    shutil.rmtree(path)


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *remaining])
