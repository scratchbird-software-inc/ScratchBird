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
from scratchbird_ai.tool_schema import ToolContractError


class ApprovalLedgerTests(unittest.TestCase):
    def test_registers_and_replays_same_statement(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            ledger = ApprovalLedger(path=str(Path(tmp_dir) / "approval-ledger.json"))
            first = ledger.validate_or_register(
                approval_token="approved-token",
                approval_evidence={"approved_by": "operator_a"},
                tenant_id="tenant_a",
                actor_id="actor_a",
                statement_hash="stmt_hash_1",
            )
            second = ledger.validate_or_register(
                approval_token="approved-token",
                approval_evidence={"approval_id": first.approval_id},
                tenant_id="tenant_a",
                actor_id="actor_a",
                statement_hash="stmt_hash_1",
            )
            self.assertEqual(first.approval_id, second.approval_id)
            self.assertEqual(second.use_count, 2)

    def test_rejects_statement_mismatch_replay(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            ledger = ApprovalLedger(path=str(Path(tmp_dir) / "approval-ledger.json"))
            first = ledger.validate_or_register(
                approval_token="approved-token",
                approval_evidence=None,
                tenant_id="tenant_a",
                actor_id="actor_a",
                statement_hash="stmt_hash_1",
            )
            with self.assertRaises(ToolContractError) as ctx:
                ledger.validate_or_register(
                    approval_token="approved-token",
                    approval_evidence={"approval_id": first.approval_id},
                    tenant_id="tenant_a",
                    actor_id="actor_a",
                    statement_hash="stmt_hash_2",
                )
            self.assertEqual(ctx.exception.error_code, "E_APPROVAL_INVALID")

    def test_rejects_revoked_approval(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            ledger = ApprovalLedger(path=str(Path(tmp_dir) / "approval-ledger.json"))
            record = ledger.validate_or_register(
                approval_token="approved-token",
                approval_evidence=None,
                tenant_id="tenant_a",
                actor_id="actor_a",
                statement_hash="stmt_hash_1",
            )
            ledger.revoke(record.approval_id, reason="operator revoked")
            with self.assertRaises(ToolContractError) as ctx:
                ledger.validate_or_register(
                    approval_token="approved-token",
                    approval_evidence={"approval_id": record.approval_id},
                    tenant_id="tenant_a",
                    actor_id="actor_a",
                    statement_hash="stmt_hash_1",
                )
            self.assertEqual(ctx.exception.error_code, "E_APPROVAL_INVALID")

    def test_rejects_expired_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            ledger = ApprovalLedger(path=str(Path(tmp_dir) / "approval-ledger.json"))
            with self.assertRaises(ToolContractError) as ctx:
                ledger.validate_or_register(
                    approval_token="approved-token",
                    approval_evidence={"expires_at": "2020-01-01T00:00:00Z"},
                    tenant_id="tenant_a",
                    actor_id="actor_a",
                    statement_hash="stmt_hash_1",
                )
            self.assertEqual(ctx.exception.error_code, "E_APPROVAL_INVALID")

    def test_lists_records_and_reports_summary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            ledger = ApprovalLedger(path=str(Path(tmp_dir) / "approval-ledger.json"))
            first = ledger.validate_or_register(
                approval_token="approved-token-a",
                approval_evidence={"approved_by": "operator_a"},
                tenant_id="tenant_a",
                actor_id="actor_a",
                statement_hash="stmt_hash_1",
            )
            second = ledger.validate_or_register(
                approval_token="approved-token-b",
                approval_evidence={"approved_by": "operator_b"},
                tenant_id="tenant_b",
                actor_id="actor_b",
                statement_hash="stmt_hash_2",
            )
            ledger.revoke(first.approval_id, reason="cleanup", revoked_by="sysarch")

            tenant_rows = ledger.list_records(tenant_id="tenant_a")
            self.assertEqual([row.approval_id for row in tenant_rows], [first.approval_id])

            active_rows = ledger.list_records(include_revoked=False)
            self.assertEqual([row.approval_id for row in active_rows], [second.approval_id])

            summary = ledger.summary()
            self.assertEqual(summary["total_records"], 2)
            self.assertEqual(summary["revoked_records"], 1)
            self.assertEqual(summary["active_records"], 1)


if __name__ == "__main__":
    unittest.main()
