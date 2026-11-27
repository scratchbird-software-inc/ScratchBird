#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Focused contract/evidence regression; never regenerates any artifacts."""
import inspect
import sys
import unittest

sys.dont_write_bytecode = True

import engine_reached_refusal_generated_evidence as refusal
import generate_function_semantic_oracle_matrix as oracle
import generate_per_row_evidence_manifest as manifest
import generate_strict_row_coverage_ledger as ledger


PGP = {
    "SBSQL-AF4E7BFEFDD1": "pgcrypto",
    "SBSQL-6358314B6883": "pgp_pub_decrypt",
    "SBSQL-2854D8B0790B": "pgp_pub_encrypt",
    "SBSQL-6DBE85C4B814": "pgp_sym_decrypt",
    "SBSQL-C98EC981ACD7": "pgp_sym_encrypt",
}


class NativePgpMetadataTest(unittest.TestCase):
    def record(self, surface_id):
        return oracle.SBSFC057_CRYPTO_HASH_ORACLE_OVERRIDES[surface_id]

    def surface(self, surface_id, status="native_now"):
        return {
            "surface_id": surface_id, "canonical_name": PGP[surface_id],
            "surface_kind": "function", "family": "general", "status": status,
            "source_status": status, "cluster_scope": "noncluster_or_profile_scoped",
            "sblr_operation_family": "sblr.expression.runtime.v3",
            "engine_packet": "", "parser_packet": "",
        }

    def row(self, surface_id):
        record = self.record(surface_id)
        return oracle._full_oracle_row(self.surface(surface_id),
                                      record["canonical_name"], record, "Core native PGP contract")

    def test_exact_public_decrypt_overloads_survive_row_serialization(self):
        record = self.record("SBSQL-6358314B6883")
        self.assertEqual(len(record["overloads"]), 2)
        old, explicit = record["overloads"]
        self.assertEqual(old["signature"],
                         "pgp_pub_decrypt(message bytea, private_key bytea [, options text])")
        self.assertIn("exactly 2 or 3", old["argument_rule"])
        self.assertIn("never an inferred passphrase", old["argument_rule"])
        self.assertEqual(explicit["signature"],
                         "pgp_pub_decrypt(message bytea, private_key bytea, passphrase text, options text)")
        self.assertIn("exactly 4", explicit["argument_rule"])
        emitted = self.row("SBSQL-6358314B6883")["argument_descriptor_rule"]
        self.assertIn(old["signature"], emitted)
        self.assertIn(explicit["signature"], emitted)
        self.assertEqual(record["return_type_rule"], "bytea")

    def test_other_pgp_arities_and_typed_returns(self):
        signatures = {
            "SBSQL-2854D8B0790B": ("pgp_pub_encrypt(message bytea, public_key bytea [, options text])", "bytea"),
            "SBSQL-6DBE85C4B814": ("pgp_sym_decrypt(message bytea, password text [, options text])", "text"),
            "SBSQL-C98EC981ACD7": ("pgp_sym_encrypt(message text, password text [, options text])", "bytea"),
        }
        for surface_id, (signature, result) in signatures.items():
            with self.subTest(surface_id=surface_id):
                record = self.record(surface_id)
                self.assertEqual(len(record["overloads"]), 1)
                self.assertEqual(record["overloads"][0]["signature"], signature)
                self.assertIn("exactly 2 or 3", record["overloads"][0]["argument_rule"])
                self.assertNotIn("exactly 4", record["overloads"][0]["argument_rule"])
                self.assertEqual(record["return_type_rule"], result)
                self.assertIn("exact declared bytea/text", record["coercion_rule"])
                self.assertIn("before NULL", record["null_behavior"])
                self.assertIn("exact declared carrier", record["coercion_rule"])

    def test_package_is_never_a_scalar_marker_success(self):
        record = self.record("SBSQL-AF4E7BFEFDD1")
        self.assertEqual(record["volatility"], "not_executable")
        self.assertEqual(record["error_semantics"], "CRYPTO.PACKAGE.NOT_CALLABLE")
        self.assertIn("not a callable value", record["return_type_rule"])
        self.assertEqual(record["overloads"][0]["signature"], "pgcrypto()")

    def test_all_three_generators_agree_on_native_binding_and_noncompletion(self):
        for surface_id, name in PGP.items():
            with self.subTest(surface_id=surface_id):
                record = self.record(surface_id)
                self.assertEqual(record["status"], "evidence_required")
                self.assertEqual(record["execution_admission"], "refused_until_exact_semantics")
                self.assertNotIn("builtin_uuid", record)  # Never re-issue catalog identities.
                expected = f"sblr.expr.crypto_{name}.v3"
                self.assertEqual(record["sblr_binding"], expected)
                self.assertIn("status=evidence_required", self.row(surface_id)["notes"])
                kind = ("package_parser_refusal" if refusal.is_package_refusal(surface_id)
                        else "engine_reached_refusal")
                for module in (manifest, ledger):
                    evidence = module.SBSFC057_CRYPTO_HASH_ROW_EVIDENCE[surface_id]
                    self.assertEqual(evidence["sblr_binding"], expected)
                    # owner 2026-11-27: the pgp_* rows are engine-reached refusals and pgcrypto is a
                    # parser package refusal (both exact_refusal_passed); neither is a positive route
                    self.assertEqual(evidence["qualification"], kind)
                    self.assertEqual(evidence["proof"], refusal.fixture_id_for(surface_id))

    def test_actual_classifiers_never_promote_pgp_rows_to_e2e_or_pre_sblr_evidence(self):
        for surface_id in PGP:
            for status in ("native_now", "evidence_required"):
                with self.subTest(surface_id=surface_id, status=status):
                    surface = self.surface(surface_id, status)
                    row = self.row(surface_id)
                    classified = ledger.sbsfc057_crypto_hash_classification(
                        surface, {"status": status},
                        {"sblr_operation_family": "sblr.expression.runtime.v3"}, row)
                    # never the positive state, never the legacy envelope evidence
                    self.assertEqual(classified["current_state"], "exact_refusal_passed")
                    self.assertEqual(classified["evidence_complete"], "yes")
                    text = ";".join(classified.values())
                    self.assertNotIn("SBPGP1", text)
                    self.assertNotIn("bounded_crypto_hash_route", text)
                    package = refusal.is_package_refusal(surface_id)
                    # engine-reached rows must NOT carry the false pre-SBLR token; pgcrypto must
                    self.assertEqual("executable_sblr_emitted=false" in text.split(";"), package)
                    self.assertEqual("executable_sblr_emitted=true" in text.split(";"), not package)
                    self.assertEqual([], refusal.refusal_kind_errors(
                        surface_id, classified.values(), None))
                    # Exercise the actual production manifest classifier without
                    # loading ledgers, running fixtures, or writing generated CSV.
                    arguments = {key: None for key in inspect.signature(manifest.classify).parameters}
                    arguments.update(surface=surface, ledger_row=classified, oracle_row=row)
                    result = manifest.classify(**arguments)
                    self.assertEqual(result["final_state"], "exact_refusal_passed")
                    self.assertEqual([], refusal.refusal_kind_errors(
                        surface_id, result.values(), None))
                    self.assertNotIn("sbsql_e2e_passed", result["ctest_label"])

    def test_non_pgp_evidence_not_reclassified_by_this_repair(self):
        for module in (manifest, ledger):
            evidence = module.SBSFC057_CRYPTO_HASH_ROW_EVIDENCE["SBSQL-C8996122850A"]
            self.assertEqual(evidence["qualification"], "legacy_row_evidence")
            self.assertEqual(evidence["proof"], "SBSFC057-scrypt")
            self.assertEqual(evidence["sblr_binding"], "sblr.expr.crypto_hash.scrypt.v3")


if __name__ == "__main__":
    unittest.main()
