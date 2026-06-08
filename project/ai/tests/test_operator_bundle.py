# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scratchbird_ai.approval_store import ApprovalLedger
from scratchbird_ai.compatibility import build_compatibility_manifest
from scratchbird_ai.environment_manifest import build_certification_manifest
from scratchbird_ai.operator_bundle import (
    build_runtime_diagnostics,
    generate_operator_runbook_bundle,
)
from scratchbird_ai.settings import RuntimeSettings
from scratchbird_ai.structured_logging import StructuredEventLogger


class OperatorBundleTests(unittest.TestCase):
    def test_generate_operator_runbook_bundle_writes_expected_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            settings = RuntimeSettings(
                approval_ledger_path=str(tmp_path / "approval-ledger.json"),
                structured_event_log_path=str(tmp_path / "structured-events.jsonl"),
                operator_bundle_output_dir=str(tmp_path / "operator-bundle"),
                audit_attestation_secret="bundle-secret",
                audit_attestation_delegated_secret="delegated-secret",
                operator_target_profiles=("listener_direct", "manager_proxy"),
            )
            logger = StructuredEventLogger(path=settings.structured_event_log_path)
            logger.emit(
                event_type="query_execution",
                status="success",
                trace_id="tr_1",
                request_id="req_1",
                security_context={"tenant_id": "tenant_a", "actor_id": "actor_a"},
                duration_ms=20.0,
                attributes={"tool_name": "run_query"},
            )
            logger.emit(
                event_type="query_execution",
                status="error",
                trace_id="tr_2",
                request_id="req_2",
                security_context={"tenant_id": "tenant_a", "actor_id": "actor_a"},
                duration_ms=30.0,
                attributes={
                    "error_code": "E_TOOL_INPUT_INVALID",
                    "policy_rule_id": "RULE-2",
                    "message": "bad request",
                },
            )

            ledger = ApprovalLedger(path=settings.approval_ledger_path)
            ledger.validate_or_register(
                approval_token="approved-token",
                approval_evidence={"approved_by": "operator_a"},
                tenant_id="tenant_a",
                actor_id="actor_a",
                statement_hash="stmt_hash_1",
            )
            compatibility_manifest = build_compatibility_manifest(
                adapter_mode=settings.normalized_mode(),
                matrix_version="test-matrix",
                runtime_settings=settings,
            )
            certification_manifest = build_certification_manifest(
                settings=settings,
                adapter_mode=settings.normalized_mode(),
                matrix_version="test-matrix",
                compatibility_manifest=compatibility_manifest,
            )

            diagnostics = build_runtime_diagnostics(
                settings=settings,
                certification_manifest=certification_manifest,
                event_logger=logger,
                approval_ledger=ledger,
            )
            self.assertEqual(diagnostics["event_summary"]["total_events"], 2)
            self.assertEqual(diagnostics["approval_summary"]["total_records"], 1)

            bundle = generate_operator_runbook_bundle(
                output_dir=settings.operator_bundle_output_dir,
                settings=settings,
                certification_manifest=certification_manifest,
                event_logger=logger,
                approval_ledger=ledger,
            )
            self.assertEqual(bundle["status"], "WARN")
            self.assertTrue(Path(bundle["files"]["summary"]).is_file())
            self.assertTrue(Path(bundle["files"]["runtime_diagnostics"]).is_file())
            self.assertTrue(Path(bundle["files"]["slo_report"]).is_file())
            self.assertTrue(Path(bundle["files"]["dashboard_manifest"]).is_file())
            self.assertTrue(Path(bundle["files"]["package_manifest"]).is_file())
            self.assertTrue(Path(bundle["files"]["runbook"]).is_file())
            self.assertIn("listener_direct", bundle["packages"])
            self.assertTrue(
                Path(bundle["packages"]["listener_direct"]["files"]["target_manifest"]).is_file()
            )
            self.assertTrue(
                Path(bundle["packages"]["manager_proxy"]["files"]["runbook"]).is_file()
            )


if __name__ == "__main__":
    unittest.main()
