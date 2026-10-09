# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Reject missing proof and misclassification, not genuine diagnostic identities."""
import unittest

from sbsql_status_change_authority_gate import (
    POLICY_DIAGNOSTIC_PROOFS, POLICY_DIAGNOSTIC_SURFACE,
    diagnostic_identity_refusal_errors,
)


class DiagnosticIdentityAuthority(unittest.TestCase):
    def setUp(self):
        self.row = dict(surface_id=POLICY_DIAGNOSTIC_SURFACE,
                        canonical_name="SBSQL.POLICY_BLOCKED", status="native_now",
                        cluster_scope="noncluster_or_profile_scoped",
                        current_state="exact_refusal_passed",
                        evidence=";".join(sorted(POLICY_DIAGNOSTIC_PROOFS)))
        self.authority = dict(surface_id=POLICY_DIAGNOSTIC_SURFACE,
                              surface_name="SBSQL.POLICY_BLOCKED",
                              classification="diagnostic_identity",
                              parent_kind="diagnostic", parent_key="SBSQL.POLICY_BLOCKED")

    def test_exact_contract(self):
        self.assertEqual([], diagnostic_identity_refusal_errors(self.row, self.authority))

    def test_every_proof_is_required(self):
        for token in POLICY_DIAGNOSTIC_PROOFS:
            with self.subTest(token=token):
                row = dict(self.row, evidence=";".join(sorted(POLICY_DIAGNOSTIC_PROOFS - {token})))
                self.assertTrue(diagnostic_identity_refusal_errors(row, self.authority))
                # Prefix/suffix substring matches must not stand in for proof.
                row["evidence"] += ";not_" + token + ";" + token + "_not_proven"
                self.assertTrue(diagnostic_identity_refusal_errors(row, self.authority))

    def test_every_authority_field_is_required(self):
        for key in self.authority:
            with self.subTest(key=key):
                self.assertTrue(diagnostic_identity_refusal_errors(
                    self.row, dict(self.authority, **{key: "unknown"})))

    def test_wrong_surface_status_scope_and_positive_claim_are_rejected(self):
        for key in self.row.keys() - {"evidence"}:
            with self.subTest(key=key):
                self.assertTrue(diagnostic_identity_refusal_errors(
                    dict(self.row, **{key: "e2e_passed"}), self.authority))

    def test_callable_observer_cannot_borrow_diagnostic_refusal(self):
        self.assertTrue(diagnostic_identity_refusal_errors(
            dict(self.row, surface_id="SBSQL-E302317C73E2", canonical_name="POLICY_BLOCKED"),
            self.authority))
        self.assertTrue(diagnostic_identity_refusal_errors(
            self.row, dict(self.authority, classification="canonical_builtin_match", parent_kind="builtin")))


if __name__ == "__main__":
    unittest.main()
