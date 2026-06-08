# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import contextlib
import importlib.util
import json
import socket
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = REPO_ROOT / "tools" / "run_live_native_conformance.py"
SPEC = importlib.util.spec_from_file_location("run_live_native_conformance", TOOL_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class LiveNativeConformanceToolTests(unittest.TestCase):
    def test_inspect_engine_endpoint_diagnostics_reports_missing_and_stale_sockets(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            root = Path(tmp_dir)
            profiles_dir = root / "profiles"
            control_dir = root / "control"
            profiles_dir.mkdir(parents=True, exist_ok=True)
            control_dir.mkdir(parents=True, exist_ok=True)

            runtime_env = profiles_dir / "runtime.env"
            runtime_env.write_text("export SCRATCHBIRD_NATIVE_HOST='127.0.0.1'\n", encoding="utf-8")

            engine_endpoint = control_dir / "sb_engine.main.sock"
            parser_endpoint = Path(f"{engine_endpoint}.parser_v1")
            stale_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            stale_socket.bind(str(parser_endpoint))
            stale_socket.close()

            runtime_ownership = {
                "publication": {
                    "native": {
                        "owner": {
                            "engine_endpoint": str(engine_endpoint),
                        }
                    }
                }
            }
            (profiles_dir / "runtime_ownership.json").write_text(
                json.dumps(runtime_ownership),
                encoding="utf-8",
            )

            config = MODULE.LiveNativeConfig(
                repo_root=REPO_ROOT,
                output_dir=root / "artifacts",
                interface_profile_id="service_internal_v0",
                covered_interface_profiles=("service_internal_v0",),
                live_enabled=True,
                adapter_mode="http",
                base_url="http://127.0.0.1:3095",
                token="secret-token",
                dialect="native",
                query_text="SELECT 1",
                schema="public",
                table="customers",
                database=None,
                timeout_sec=3.0,
                skip_metadata=False,
                scratchbird_server_version="0.1.0-test",
                parser_compiler_version="0.1.0-test",
                driver_runtime_version=None,
                transport_profile="http_json_request_response",
                auth_mode="bearer_token",
                test_dataset_version="smoke-fixtures-v1",
                seed_or_fixture_version="seed-v1",
                runtime_env_path=str(runtime_env),
            )

            diagnostics = MODULE._inspect_engine_endpoint_diagnostics(
                config,
                {
                    "host": "127.0.0.1",
                    "port": 13092,
                    "database": "main",
                    "user": "SysArch",
                    "password": "replaceme",
                    "sslmode": "disable",
                },
            )

            self.assertIsNotNone(diagnostics)
            assert diagnostics is not None
            self.assertEqual(diagnostics["engine_endpoint"], str(engine_endpoint))
            self.assertEqual(diagnostics["base_socket"]["connect_state"], "missing")
            self.assertEqual(
                diagnostics["parser_socket"]["connect_state"],
                "stale_or_not_listening",
            )

    def test_run_native_preflight_appends_engine_endpoint_diagnostics_on_connect_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            root = Path(tmp_dir)
            profiles_dir = root / "profiles"
            control_dir = root / "control"
            profiles_dir.mkdir(parents=True, exist_ok=True)
            control_dir.mkdir(parents=True, exist_ok=True)

            runtime_env = profiles_dir / "runtime.env"
            runtime_env.write_text(
                "\n".join(
                    [
                        "export SCRATCHBIRD_NATIVE_HOST='127.0.0.1'",
                        "export SCRATCHBIRD_NATIVE_PORT='13092'",
                        "export SCRATCHBIRD_NATIVE_DB='main'",
                        "export SCRATCHBIRD_NATIVE_USER='SysArch'",
                        "export SCRATCHBIRD_NATIVE_PASSWORD='replaceme'",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            engine_endpoint = control_dir / "sb_engine.main.sock"
            parser_endpoint = Path(f"{engine_endpoint}.parser_v1")
            stale_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            stale_socket.bind(str(parser_endpoint))
            stale_socket.close()

            runtime_ownership = {
                "publication": {
                    "native": {
                        "owner": {
                            "engine_endpoint": str(engine_endpoint),
                        }
                    }
                }
            }
            (profiles_dir / "runtime_ownership.json").write_text(
                json.dumps(runtime_ownership),
                encoding="utf-8",
            )

            config = MODULE.LiveNativeConfig(
                repo_root=REPO_ROOT,
                output_dir=root / "artifacts",
                interface_profile_id="service_internal_v0",
                covered_interface_profiles=("service_internal_v0",),
                live_enabled=True,
                adapter_mode="http",
                base_url="http://127.0.0.1:3095",
                token="secret-token",
                dialect="native",
                query_text="SELECT 1",
                schema="public",
                table="customers",
                database=None,
                timeout_sec=3.0,
                skip_metadata=False,
                scratchbird_server_version="0.1.0-test",
                parser_compiler_version="0.1.0-test",
                driver_runtime_version=None,
                transport_profile="http_json_request_response",
                auth_mode="bearer_token",
                test_dataset_version="smoke-fixtures-v1",
                seed_or_fixture_version="seed-v1",
                runtime_env_path=str(runtime_env),
            )

            class _FailingDriver:
                @staticmethod
                def connect(**kwargs):
                    del kwargs
                    raise RuntimeError("[08001] connect() failed: No such file or directory")

            original_import = MODULE._import_scratchbird_driver
            original_driver_src = MODULE._ensure_bridge_driver_src
            try:
                MODULE._import_scratchbird_driver = lambda driver_src: _FailingDriver  # type: ignore[assignment]
                MODULE._ensure_bridge_driver_src = lambda cfg: None  # type: ignore[assignment]
                result = MODULE.run_native_preflight(config)
            finally:
                MODULE._import_scratchbird_driver = original_import  # type: ignore[assignment]
                MODULE._ensure_bridge_driver_src = original_driver_src  # type: ignore[assignment]

            self.assertFalse(result.passed)
            self.assertIsNotNone(result.runtime_diagnostics)
            self.assertIsNotNone(result.error)
            assert result.error is not None
            self.assertIn("engine endpoint diagnostics:", result.error)
            self.assertIn("base_socket=missing", result.error)
            self.assertIn("parser_socket=stale_or_not_listening", result.error)

    def test_validate_config_fails_closed_when_live_metadata_missing(self) -> None:
        config = MODULE.LiveNativeConfig(
            repo_root=REPO_ROOT,
            output_dir=REPO_ROOT / "artifacts" / "tmp-live-native-test",
            interface_profile_id="service_internal_v0",
            covered_interface_profiles=("service_internal_v0",),
            live_enabled=False,
            adapter_mode="mock",
            base_url="http://127.0.0.1:3095",
            token=None,
            dialect="native",
            query_text="SELECT 1",
            schema="public",
            table="customers",
            database=None,
            timeout_sec=3.0,
            skip_metadata=False,
            scratchbird_server_version="",
            parser_compiler_version="",
            driver_runtime_version=None,
            transport_profile="http_json_request_response",
            auth_mode="none",
            test_dataset_version="",
            seed_or_fixture_version="",
        )
        settings = MODULE.runtime_settings_from_config(config)

        errors = MODULE.validate_live_native_config(config, runtime_settings=settings)

        self.assertTrue(any("LIVE_NATIVE_ENABLED" in error for error in errors))
        self.assertTrue(any("adapter_mode must be http or hybrid" in error for error in errors))
        self.assertTrue(any("scratchbird_server_version" in error for error in errors))
        self.assertTrue(any("parser_compiler_version" in error for error in errors))
        self.assertTrue(any("test_dataset_version" in error for error in errors))
        self.assertTrue(any("seed_or_fixture_version" in error for error in errors))

    def test_run_live_native_conformance_writes_pass_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            output_dir = Path(tmp_dir)
            config = MODULE.LiveNativeConfig(
                repo_root=REPO_ROOT,
                output_dir=output_dir,
                interface_profile_id="service_internal_v0",
                covered_interface_profiles=("service_internal_v0",),
                live_enabled=True,
                adapter_mode="http",
                base_url="http://127.0.0.1:3095",
                token="secret-token",
                dialect="native",
                query_text="SELECT 1",
                schema="public",
                table="customers",
                database=None,
                timeout_sec=3.0,
                skip_metadata=False,
                scratchbird_server_version="0.1.0-test",
                parser_compiler_version="0.1.0-test",
                driver_runtime_version=None,
                transport_profile="http_json_request_response",
                auth_mode="bearer_token",
                test_dataset_version="smoke-fixtures-v1",
                seed_or_fixture_version="seed-v1",
            )

            def fake_runner(run_config: MODULE.LiveNativeConfig) -> MODULE.SmokeCommandResult:
                self.assertEqual(run_config.base_url, "http://127.0.0.1:3095")
                return MODULE.SmokeCommandResult(
                    command=["python", "tools/smoke_http_contract.py", "--mode", "live"],
                    returncode=0,
                    stdout="[smoke] PASS\n",
                    stderr="",
                    duration_sec=0.125,
                )

            def fake_service_probe(
                _config: MODULE.LiveNativeConfig,
                *,
                service: object,
            ) -> MODULE.ProfileProbeResult:
                self.assertIsNotNone(service)
                return MODULE.ProfileProbeResult(
                    profile_id="service_internal_v0",
                    passed=True,
                    checks=[
                        "service_internal_execute_readonly_query",
                        "service_internal_explain_query",
                        "service_internal_workload_batch",
                        "service_internal_audit_replay",
                    ],
                    errors=[],
                )

            exit_code = MODULE.run_live_native_conformance(
                config,
                smoke_runner=fake_runner,
                service_internal_probe_runner=fake_service_probe,
            )

            self.assertEqual(exit_code, 0)
            summary = json.loads((output_dir / "summary.json").read_text(encoding="utf-8"))
            manifest = json.loads((output_dir / "environment_manifest.json").read_text(encoding="utf-8"))
            run_log = json.loads((output_dir / "run_log.json").read_text(encoding="utf-8"))
            runtime_mode_report = json.loads(
                (output_dir / "runtime_mode_report.json").read_text(encoding="utf-8")
            )
            junit_text = (output_dir / "test_report.junit.xml").read_text(encoding="utf-8")

            self.assertEqual(summary["status"], "PASS")
            self.assertTrue(summary["smoke_passed"])
            self.assertEqual(summary["covered_interface_profiles"], ["service_internal_v0"])
            self.assertEqual(summary["profile_results"][0]["profile_id"], "service_internal_v0")
            self.assertEqual(summary["runtime_mode_id"], "listener_direct")
            self.assertEqual(manifest["status"], "PASS")
            self.assertEqual(
                manifest["certification_manifest"]["live_run_metadata"]["interface_profile_id"],
                "service_internal_v0",
            )
            self.assertEqual(
                manifest["certification_manifest"]["live_run_metadata"]["runtime_mode_id"],
                "listener_direct",
            )
            self.assertEqual(
                manifest["certification_manifest"]["live_run_metadata"]["test_dataset_version"],
                "smoke-fixtures-v1",
            )
            self.assertEqual(run_log["smoke"]["returncode"], 0)
            self.assertEqual(runtime_mode_report["bridge_server_setup"], "listener-only")
            self.assertIn("configuration_validation", junit_text)
            self.assertIn("native_direct_preflight", junit_text)
            self.assertIn("http_bridge_smoke", junit_text)

    def test_run_live_native_conformance_writes_fail_artifacts_without_running_smoke(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            output_dir = Path(tmp_dir)
            config = MODULE.LiveNativeConfig(
                repo_root=REPO_ROOT,
                output_dir=output_dir,
                interface_profile_id="service_internal_v0",
                covered_interface_profiles=("service_internal_v0",),
                live_enabled=False,
                adapter_mode="mock",
                base_url="http://127.0.0.1:3095",
                token=None,
                dialect="native",
                query_text="SELECT 1",
                schema="public",
                table="customers",
                database=None,
                timeout_sec=3.0,
                skip_metadata=False,
                scratchbird_server_version="",
                parser_compiler_version="",
                driver_runtime_version=None,
                transport_profile="http_json_request_response",
                auth_mode="none",
                test_dataset_version="",
                seed_or_fixture_version="",
            )
            called = False

            def fake_runner(_: MODULE.LiveNativeConfig) -> MODULE.SmokeCommandResult:
                nonlocal called
                called = True
                raise AssertionError("smoke runner must not be called when config validation fails")

            exit_code = MODULE.run_live_native_conformance(config, smoke_runner=fake_runner)

            self.assertEqual(exit_code, 1)
            self.assertFalse(called)
            summary = json.loads((output_dir / "summary.json").read_text(encoding="utf-8"))
            run_log = json.loads((output_dir / "run_log.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "FAIL")
            self.assertTrue(summary["config_errors"])
            self.assertIsNone(run_log["smoke"])

    def test_run_live_native_conformance_launches_local_bridge_and_records_preflight(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            output_dir = Path(tmp_dir)
            runtime_env = output_dir / "runtime.env"
            runtime_env.write_text(
                "\n".join(
                    [
                        "export SCRATCHBIRD_NATIVE_HOST='127.0.0.1'",
                        "export SCRATCHBIRD_NATIVE_PORT='13092'",
                        "export SCRATCHBIRD_NATIVE_DB='main'",
                        "export SCRATCHBIRD_NATIVE_USER='SysArch'",
                        "export SCRATCHBIRD_NATIVE_PASSWORD='replaceme'",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            config = MODULE.LiveNativeConfig(
                repo_root=REPO_ROOT,
                output_dir=output_dir,
                interface_profile_id="service_internal_v0",
                covered_interface_profiles=("service_internal_v0",),
                live_enabled=True,
                adapter_mode="http",
                base_url="",
                token=None,
                dialect="native",
                query_text="SELECT 1",
                schema="",
                table="",
                database=None,
                timeout_sec=3.0,
                skip_metadata=False,
                scratchbird_server_version="0.1.0-test",
                parser_compiler_version="0.1.0-test",
                driver_runtime_version=None,
                transport_profile="http_json_request_response",
                auth_mode="bearer_token",
                test_dataset_version="smoke-fixtures-v1",
                seed_or_fixture_version="seed-v1",
                launch_bridge=True,
                bridge_host="127.0.0.1",
                bridge_port=3095,
                runtime_env_path=str(runtime_env),
            )

            def fake_runner(run_config: MODULE.LiveNativeConfig) -> MODULE.SmokeCommandResult:
                self.assertEqual(run_config.base_url, "http://127.0.0.1:3095")
                self.assertEqual(run_config.token, "launch-token")
                return MODULE.SmokeCommandResult(
                    command=["python", "tools/smoke_http_contract.py", "--mode", "live"],
                    returncode=0,
                    stdout="[smoke] PASS\n",
                    stderr="",
                    duration_sec=0.125,
                )

            def fake_service_probe(
                _config: MODULE.LiveNativeConfig,
                *,
                service: object,
            ) -> MODULE.ProfileProbeResult:
                self.assertIsNotNone(service)
                return MODULE.ProfileProbeResult(
                    profile_id="service_internal_v0",
                    passed=True,
                    checks=[
                        "service_internal_execute_readonly_query",
                        "service_internal_explain_query",
                        "service_internal_workload_batch",
                        "service_internal_audit_replay",
                    ],
                    errors=[],
                )

            def fake_native_probe(_: MODULE.LiveNativeConfig) -> MODULE.NativePreflightResult:
                return MODULE.NativePreflightResult(
                    passed=True,
                    duration_sec=0.05,
                    dsn_redacted="scratchbird://SysArch:***@127.0.0.1:13092/main?sslmode=disable",
                    row_sample=[1],
                    schema_count=3,
                )

            @contextlib.contextmanager
            def fake_bridge_launcher(_: MODULE.LiveNativeConfig):
                yield MODULE.BridgeLaunchSession(
                    base_url="http://127.0.0.1:3095",
                    token="launch-token",
                    log_path=str(output_dir / "bridge.log"),
                    pid=12345,
                    dsn_redacted="scratchbird://SysArch:***@127.0.0.1:13092/main?sslmode=disable",
                    driver_src="/tmp/fake-driver-src",
                )

            exit_code = MODULE.run_live_native_conformance(
                config,
                smoke_runner=fake_runner,
                native_probe_runner=fake_native_probe,
                bridge_launcher=fake_bridge_launcher,
                service_internal_probe_runner=fake_service_probe,
            )

            self.assertEqual(exit_code, 0)
            summary = json.loads((output_dir / "summary.json").read_text(encoding="utf-8"))
            run_log = json.loads((output_dir / "run_log.json").read_text(encoding="utf-8"))

            self.assertEqual(summary["status"], "PASS")
            self.assertTrue(summary["native_preflight_passed"])
            self.assertEqual(summary["base_url"], "http://127.0.0.1:3095")
            self.assertTrue(run_log["native_preflight"]["passed"])
            self.assertEqual(run_log["bridge_launch"]["base_url"], "http://127.0.0.1:3095")
            self.assertTrue(run_log["bridge_launch"]["token_present"])

    def test_validate_config_rejects_unknown_runtime_mode(self) -> None:
        config = MODULE.LiveNativeConfig(
            repo_root=REPO_ROOT,
            output_dir=REPO_ROOT / "artifacts" / "tmp-live-native-test",
            interface_profile_id="service_internal_v0",
            covered_interface_profiles=("service_internal_v0",),
            live_enabled=True,
            adapter_mode="http",
            base_url="http://127.0.0.1:3095",
            token="secret-token",
            dialect="native",
            query_text="SELECT 1",
            schema="public",
            table="customers",
            database=None,
            timeout_sec=3.0,
            skip_metadata=False,
            scratchbird_server_version="0.1.0-test",
            parser_compiler_version="0.1.0-test",
            driver_runtime_version=None,
            transport_profile="http_json_request_response",
            auth_mode="bearer_token",
            test_dataset_version="smoke-fixtures-v1",
            seed_or_fixture_version="seed-v1",
            runtime_mode_id="unknown_mode",
        )
        settings = MODULE.runtime_settings_from_config(config)
        errors = MODULE.validate_live_native_config(config, runtime_settings=settings)
        self.assertTrue(any("runtime_mode_id must be one of" in error for error in errors))

    def test_probe_service_internal_exercises_explain_workload_and_audit(self) -> None:
        config = MODULE.LiveNativeConfig(
            repo_root=REPO_ROOT,
            output_dir=REPO_ROOT / "artifacts" / "tmp-live-native-test",
            interface_profile_id="service_internal_v0",
            covered_interface_profiles=("service_internal_v0",),
            live_enabled=True,
            adapter_mode="http",
            base_url="http://127.0.0.1:3095",
            token="secret-token",
            dialect="native",
            query_text="SELECT 1",
            schema="public",
            table="customers",
            database=None,
            timeout_sec=3.0,
            skip_metadata=False,
            scratchbird_server_version="0.1.0-test",
            parser_compiler_version="0.1.0-test",
            driver_runtime_version=None,
            transport_profile="http_json_request_response",
            auth_mode="bearer_token",
            test_dataset_version="smoke-fixtures-v1",
            seed_or_fixture_version="seed-v1",
        )

        class _Response:
            def __init__(self, trace_id: str) -> None:
                self.row_count = 1
                self.trace_id = trace_id

        class _FakeService:
            def __init__(self) -> None:
                self._trace = 0

            def execute_readonly_query(self, **kwargs):
                return {"row_count": 1, "trace_id": "tr_read"}

            def explain_query(self, **kwargs):
                return {"plan_hash": "plan_1", "operator_tree": {"operator_id": "root"}}

            def run_query(self, **kwargs):
                self._trace += 1
                return _Response(f"tr_{self._trace}")

            def latest_audit_bundle(self):
                return {"plan_hash": "plan_1", "bundle_hash": "hash_1"}

            def replay_audit_bundle(self, **kwargs):
                return {"matches": True}

        result = MODULE._probe_service_internal(config, service=_FakeService())
        self.assertTrue(result.passed)
        self.assertIn("service_internal_explain_query", result.checks)
        self.assertIn("service_internal_workload_batch", result.checks)
        self.assertIn("service_internal_audit_replay", result.checks)

    def test_probe_retrieval_ingest_covers_managed_contract_probe(self) -> None:
        config = MODULE.LiveNativeConfig(
            repo_root=REPO_ROOT,
            output_dir=REPO_ROOT / "artifacts" / "tmp-live-native-test",
            interface_profile_id="retrieval_ingest_v0",
            covered_interface_profiles=("retrieval_ingest_v0",),
            live_enabled=True,
            adapter_mode="http",
            base_url="http://127.0.0.1:3095",
            token="secret-token",
            dialect="native",
            query_text="SELECT 1",
            schema="public",
            table="customers",
            database=None,
            timeout_sec=3.0,
            skip_metadata=False,
            scratchbird_server_version="0.1.0-test",
            parser_compiler_version="0.1.0-test",
            driver_runtime_version=None,
            transport_profile="http_json_request_response",
            auth_mode="bearer_token",
            test_dataset_version="smoke-fixtures-v1",
            seed_or_fixture_version="seed-v1",
        )

        class _FakeService:
            def create_vector_index(self, *, index_id: str, **kwargs):
                return {"index": {"index_id": index_id}}

            def add_embeddings(self, *, records: list[dict], **kwargs):
                return {"accepted": len(records)}

            def vector_search(self, **kwargs):
                return {"results": [{"vector_id": "doc-1#1"}]}

            def describe_vector_index(self, *, index_id: str, **kwargs):
                return {"index": {"index_id": index_id}}

            def hybrid_search(self, **kwargs):
                return {"results": [{"document_id": "doc-1"}]}

        result = MODULE._probe_retrieval_ingest(config, service=_FakeService())
        self.assertTrue(result.passed)
        self.assertIn("retrieval_hybrid_search", result.checks)
        self.assertIn("managed_retrieval_contract_create", result.checks)
        self.assertIn("managed_retrieval_contract_where_pushdown", result.checks)


if __name__ == "__main__":
    unittest.main()
