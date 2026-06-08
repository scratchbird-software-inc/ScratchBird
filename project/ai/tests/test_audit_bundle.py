# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.audit_bundle import (
    ATTESTATION_OUTCOMES,
    REPLAY_OUTCOMES,
    create_audit_bundle,
    create_bundle_attestation,
    replay_validate_bundle,
    verify_bundle_attestation,
)


class AuditBundleTests(unittest.TestCase):
    def _base_bundle(self) -> dict:
        return create_audit_bundle(
            trace_id="tr_1",
            request_id="req_1",
            tenant_id="tenant_a",
            actor_id="actor_a",
            dialect="native",
            execution_mode="ai_analysis",
            sql_text_normalized="SELECT 1",
            compile_artifact_id="cmp_1",
            execution_artifact_id="exe_1",
            plan_json={"operator_tree": {"operator_id": "root", "operator_type": "Read", "children": []}},
            plan_hash="plan_1",
            policy_decision="allow",
            policy_rule_id="MODE-ALLOW-READ-001",
            security_context={
                "tenant_id": "tenant_a",
                "actor_id": "actor_a",
                "roles": [],
                "context_version": 1,
            },
            approval_id=None,
            approval_token=None,
            error_code=None,
            created_at_utc="2026-02-24T18:00:00Z",
            statement_kind="read",
            sblr_hash="hash123",
        )

    def test_replay_match(self) -> None:
        bundle = self._base_bundle()
        result = replay_validate_bundle(
            bundle=bundle,
            security_context={
                "tenant_id": "tenant_a",
                "actor_id": "actor_a",
                "roles": [],
                "context_version": 1,
            },
            expected_policy_decision="allow",
            expected_plan_hash="plan_1",
        )
        self.assertEqual(result.outcome, REPLAY_OUTCOMES["match"])

    def test_replay_detects_hash_tamper(self) -> None:
        bundle = self._base_bundle()
        bundle["policy_rule_id"] = "tampered"
        result = replay_validate_bundle(bundle=bundle)
        self.assertEqual(result.outcome, REPLAY_OUTCOMES["mismatch_hash"])

    def test_replay_detects_policy_mismatch(self) -> None:
        bundle = self._base_bundle()
        result = replay_validate_bundle(bundle=bundle, expected_policy_decision="deny")
        self.assertEqual(result.outcome, REPLAY_OUTCOMES["mismatch_policy"])

    def test_hmac_attestation_round_trip(self) -> None:
        bundle = self._base_bundle()
        attestation = create_bundle_attestation(
            bundle=bundle,
            attestor_id="qa-attestor",
            shared_secret="shared-secret",
        )
        verified = verify_bundle_attestation(
            bundle=bundle,
            attestation=attestation,
            shared_secret="shared-secret",
        )
        self.assertEqual(verified.outcome, ATTESTATION_OUTCOMES["verified"])
        self.assertTrue(verified.verified)

    def test_third_party_hmac_attestation_round_trip(self) -> None:
        bundle = self._base_bundle()
        attestation = create_bundle_attestation(
            bundle=bundle,
            attestor_id="external-attestor",
            attestation_mode="third_party_hmac_sha256",
            shared_secret="delegated-secret",
            key_id="approval-key-1",
            external_reference="https://approvals.example.com/bundles/hash_1",
            metadata={"approval_reference": "APR-1001"},
        )
        verified = verify_bundle_attestation(
            bundle=bundle,
            attestation=attestation,
            shared_secret="delegated-secret",
        )
        self.assertEqual(attestation["signing_scope"], "third_party")
        self.assertEqual(verified.outcome, ATTESTATION_OUTCOMES["verified"])
        self.assertTrue(verified.verified)

    def test_attestation_detects_bundle_mismatch(self) -> None:
        bundle = self._base_bundle()
        attestation = create_bundle_attestation(
            bundle=bundle,
            attestor_id="qa-attestor",
            shared_secret="shared-secret",
        )
        tampered = dict(bundle)
        tampered["bundle_hash"] = "tampered"
        verified = verify_bundle_attestation(
            bundle=tampered,
            attestation=attestation,
            shared_secret="shared-secret",
        )
        self.assertEqual(verified.outcome, ATTESTATION_OUTCOMES["bundle_mismatch"])


if __name__ == "__main__":
    unittest.main()
