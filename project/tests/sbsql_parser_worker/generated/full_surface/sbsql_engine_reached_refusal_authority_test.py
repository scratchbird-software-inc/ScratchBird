#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Negative self-tests for the closed SBSFC-057 refusal kinds.

Part 1 tests the pure validator (``refusal_kind_errors``): every token of the closed
vector is required, bait strings never stand in for a token, contradictions and
forbidden tokens fail, a code absent from the engine diagnostic catalog fails, a
proof naming an unregistered ctest fails, and a row outside the closed sets cannot
claim a kind.

Part 2 feeds a deliberately malformed row to each gate that enumerates the refusal
states, through an overlay copy of the repository (symlinks except the mutated file),
and asserts the gate FAILS; an unmutated overlay must PASS (so a failure is caused by
the mutation, not by the overlay)."""

from __future__ import annotations

import csv
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve()
ROOT = HERE.parents[5]
sys.path.insert(0, str(ROOT / "project/tools/sb_parser_gen"))
import engine_reached_refusal_generated_evidence as erf  # noqa: E402

ARTIFACTS = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
FULL_SURFACE = "project/tests/sbsql_parser_worker/generated/full_surface"
PGP_ID = "SBSQL-C98EC981ACD7"
PGCRYPTO_ID = "SBSQL-AF4E7BFEFDD1"
OTHER_ID = "SBSQL-CE3790BA0486"


def pgp_values() -> list[str]:
    return [";".join(erf.vector_for(PGP_ID)), f"canonical_message_vector_set;{erf.PGP_CODE}"]


def package_values() -> list[str]:
    return [";".join(erf.vector_for(PGCRYPTO_ID)), f"canonical_message_vector_set;{erf.PACKAGE_CODE}"]


class ValidatorNegativeTests(unittest.TestCase):
    def test_complete_rows_pass(self):
        self.assertEqual([], erf.refusal_kind_errors(PGP_ID, pgp_values(), ROOT))
        self.assertEqual([], erf.refusal_kind_errors(PGCRYPTO_ID, package_values(), ROOT))
        for surface_id in erf.ENGINE_REACHED_REFUSAL_SURFACE_IDS:
            values = [";".join(erf.vector_for(surface_id)), f"canonical_message_vector_set;{erf.PGP_CODE}"]
            self.assertEqual([], erf.refusal_kind_errors(surface_id, values, ROOT))

    def test_every_token_is_required_with_bait(self):
        for surface_id, base in ((PGP_ID, pgp_values), (PGCRYPTO_ID, package_values)):
            for token in erf.vector_for(surface_id):
                with self.subTest(surface=surface_id, token=token):
                    tokens = [t for t in erf.vector_for(surface_id) if t != token]
                    code = erf.refusal_code_for(surface_id)
                    values = [";".join(tokens), f"canonical_message_vector_set;{code}"]
                    self.assertTrue(erf.refusal_kind_errors(surface_id, values, ROOT))
                    # prefix/suffix bait must not stand in for the missing token
                    values.append(f"not_{token};{token}_not_proven")
                    self.assertTrue(erf.refusal_kind_errors(surface_id, values, ROOT))

    def test_missing_diagnostic_and_message_vector_fail(self):
        values = [";".join(erf.vector_for(PGP_ID))]
        self.assertTrue(erf.refusal_kind_errors(PGP_ID, values, ROOT))
        values = [";".join(erf.vector_for(PGP_ID)), f"canonical_message_vector_set"]
        self.assertTrue(erf.refusal_kind_errors(PGP_ID, values, ROOT))
        values = [";".join(erf.vector_for(PGP_ID)), erf.PGP_CODE]
        self.assertTrue(erf.refusal_kind_errors(PGP_ID, values, ROOT))

    def test_the_false_pre_sblr_token_is_refused_on_engine_reached_rows(self):
        for bad in ("executable_sblr_emitted=false", "pre_sblr_refusal=true",
                    "engine_dispatch_not_reached=true", "engine_reached=false"):
            with self.subTest(bad=bad):
                self.assertTrue(erf.refusal_kind_errors(PGP_ID, pgp_values() + [bad], ROOT))

    def test_contradictory_and_positive_claims_fail(self):
        for bad in ("result_rows=1", "mutation=catalog", "engine_reached=false", "refusal_code=CRYPTO.PGP.INVALID_KEY",
                    "executable_sblr_emitted=false", "refusal_operation_field=pgp_pub_encrypt", "engine_reached_route=native_database"):
            with self.subTest(bad=bad):
                self.assertTrue(erf.refusal_kind_errors(PGP_ID, pgp_values() + [bad], ROOT))
        for bad in ("executable_sblr_emitted=true", "engine_reached=true", "result_published=true", "mutation=row"):
            with self.subTest(bad=bad):
                self.assertTrue(erf.refusal_kind_errors(PGCRYPTO_ID, package_values() + [bad], ROOT))

    def test_other_rows_cannot_claim_a_closed_kind(self):
        self.assertTrue(erf.refusal_kind_errors(OTHER_ID, pgp_values(), ROOT))
        self.assertTrue(erf.refusal_kind_errors(OTHER_ID, package_values(), ROOT))
        self.assertTrue(erf.refusal_kind_errors("SBSQL-0000000000EE", [f"refusal_kind={erf.KIND_ENGINE_REACHED}"], ROOT))
        # a pgp row cannot take the package kind and vice versa
        self.assertTrue(erf.refusal_kind_errors(PGP_ID, package_values(), ROOT))
        self.assertTrue(erf.refusal_kind_errors(PGCRYPTO_ID, pgp_values(), ROOT))
        # a row outside the closed sets that makes no claim is not this validator's business
        self.assertEqual([], erf.refusal_kind_errors(OTHER_ID, ["executable_sblr_emitted=false"], ROOT))

    def test_unregistered_proof_ctest_fails(self):
        values = pgp_values()
        values[0] = values[0].replace(f"proof=ctest:{erf.ROUTE_CTEST}", "proof=ctest:no_such_registered_ctest_xyz")
        errors = erf.refusal_kind_errors(PGP_ID, values, ROOT)
        self.assertTrue(any("unregistered ctest" in e or "missing token" in e for e in errors))
        values = pgp_values()
        values[0] += ";native_route_proof=ctest:sbsql_sblr_alignment_native_pgp_refusal_zz;proof=ctest:" + "not_a_test"
        self.assertTrue(any("unregistered ctest" in e for e in erf.refusal_kind_errors(PGP_ID, values, ROOT)))

    def test_ctest_templates_and_catalog_are_checked_against_the_tree(self):
        self.assertTrue(erf.ctest_is_registered(ROOT, "sbsql_sblr_alignment_native_pgp_refusal_8k"))
        self.assertTrue(erf.ctest_is_registered(ROOT, erf.ROUTE_CTEST))
        self.assertFalse(erf.ctest_is_registered(ROOT, "sbsql_pgp_refusal_route_conformance_absent"))
        self.assertTrue(erf.diagnostic_code_in_catalog(ROOT, erf.PGP_CODE))
        self.assertFalse(erf.diagnostic_code_in_catalog(ROOT, "CRYPTO.PGP.NO_SUCH_CODE"))

    def test_code_missing_from_the_diagnostic_catalog_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            fake = Path(tmp)
            (fake / "project/src/core/diagnostics").mkdir(parents=True)
            (fake / erf.DIAGNOSTIC_CATALOG).write_text('{"CRYPTO.SOMETHING.ELSE",x},\n', encoding="utf-8")
            cmake = fake / "project/tests/CMakeLists.txt"
            cmake.parent.mkdir(parents=True)
            names = [erf.ROUTE_CTEST, erf.RUNTIME_CTEST, *erf.NATIVE_CTESTS]
            cmake.write_text("".join(f"add_test(NAME {n} COMMAND x)\n" for n in names), encoding="utf-8")
            errors = erf.refusal_kind_errors(PGP_ID, pgp_values(), fake)
            self.assertTrue(any("not in the engine diagnostic catalog" in e for e in errors))
            # same fake tree with the code present and every ctest registered: passes
            (fake / erf.DIAGNOSTIC_CATALOG).write_text(
                f'{{"{erf.PGP_CODE}",x}},\n{{"{erf.PACKAGE_CODE}",x}},\n', encoding="utf-8")
            self.assertEqual([], erf.refusal_kind_errors(PGP_ID, pgp_values(), fake))
            # and with one proof ctest unregistered: fails
            cmake.write_text("".join(f"add_test(NAME {n} COMMAND x)\n" for n in names[1:]), encoding="utf-8")
            self.assertTrue(erf.refusal_kind_errors(PGP_ID, pgp_values(), fake))


# ---------------------------------------------------------------------------
# Part 2: gate-level negative tests through an overlay of the repository
# ---------------------------------------------------------------------------
def build_overlay(real_root: Path, overlay_root: Path, mutated: dict[str, bytes]) -> None:
    """overlay_root mirrors real_root with symlinks, except each mutated file (a real copy)."""
    overlay_root.mkdir(parents=True, exist_ok=True)
    pending: dict[str, dict[str, bytes]] = {}
    for rel, data in mutated.items():
        head, _, tail = rel.partition("/")
        pending.setdefault(head, {})[tail] = data

    def fill(real_dir: Path, over_dir: Path, mutated_here: dict[str, dict[str, bytes]]) -> None:
        for child in sorted(real_dir.iterdir()):
            target = over_dir / child.name
            if child.name in mutated_here:
                sub = mutated_here[child.name]
                if "" in sub:  # file mutation
                    target.write_bytes(sub[""])
                else:
                    target.mkdir()
                    nested: dict[str, dict[str, bytes]] = {}
                    for rel, data in sub.items():
                        head, _, tail = rel.partition("/")
                        nested.setdefault(head, {})[tail] = data
                    fill(child, target, nested)
            else:
                os.symlink(child, target)

    fill(real_root, overlay_root, pending)


def run_gate(overlay: Path, script: str, extra: list[str]) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", LC_ALL="C", TZ="UTC")
    return subprocess.run(
        [sys.executable, "-B", str(overlay / script), "--repo-root", str(overlay), *extra],
        cwd=str(overlay / "project"), env=env, capture_output=True, text=True, timeout=300)


def edit_csv(path: Path, surface_id: str, edit) -> bytes:
    text = path.read_bytes().decode("utf-8")
    out = []
    n = 0
    for line in text.split("\n"):
        if line.split(",", 1)[0] == surface_id:
            line = edit(line)
            n += 1
        out.append(line)
    assert n == 1, (path, surface_id, n)
    return "\n".join(out).encode("utf-8")


GATES = {
    "no_grey": (f"{FULL_SURFACE}/sbsql_no_grey_row_coverage_gate.py", ["--artifact-root", ARTIFACTS],
                f"{ARTIFACTS}/STRICT_ROW_COVERAGE_LEDGER.csv"),
    "per_row": (f"{FULL_SURFACE}/sbsql_per_row_evidence_manifest_gate.py", ["--artifact-root", ARTIFACTS],
                f"{ARTIFACTS}/PER_ROW_EVIDENCE_MANIFEST.csv"),
    "closure": ("project/tests/sbsql_parser_worker/sbsql_message_result_shape_diagnostic_closure_gate.py", [],
                f"{ARTIFACTS}/PER_ROW_EVIDENCE_MANIFEST.csv"),
    "crash": ("project/tests/sbsql_parser_worker/sbsql_integrated_crash_recovery_certification_gate.py", [],
              f"{ARTIFACTS}/PER_ROW_EVIDENCE_MANIFEST.csv"),
}
FIXTURE_GATES = {
    "route": (f"{FULL_SURFACE}/sbsql_authenticated_route_fixture_gate.py", ["--artifact-root", ARTIFACTS],
              f"{FULL_SURFACE}/authenticated_route/{{sid}}.route.yaml"),
    "round_trip": (f"{FULL_SURFACE}/sbsql_sblr_binary_round_trip_fixture_gate.py", ["--artifact-root", ARTIFACTS],
                   f"{FULL_SURFACE}/sblr_binary_round_trip/{{sid}}.round_trip.yaml"),
}


class GateNegativeTests(unittest.TestCase):
    """Each gate must FAIL on a malformed row and PASS on the unmutated overlay."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="sb_erf_selftest_"))
        # the Core-root validators read <repo>/../Specifications: mirror the parent link
        spec = ROOT.parent / "Specifications"
        if spec.exists():
            os.symlink(spec, cls.tmp / "Specifications")
        cls.count = 0

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def overlay(self, mutated: dict[str, bytes]) -> Path:
        type(self).count += 1
        target = self.tmp / f"r{self.count}"
        build_overlay(ROOT, target, mutated)
        return target

    def test_unmutated_overlays_pass(self):
        overlay = self.overlay({})
        for name, (script, extra, _) in {**GATES, **FIXTURE_GATES}.items():
            with self.subTest(gate=name):
                result = run_gate(overlay, script, extra)
                self.assertEqual(0, result.returncode, result.stderr[-600:])

    def test_csv_gates_fail_on_malformed_rows(self):
        cases = {
            "engine_token_removed": (PGP_ID, lambda l: l.replace("engine_reached=true", "engine_reached_x", 1)),
            "false_pre_sblr_token_added": (PGP_ID, lambda l: l.replace("result_rows=0", "result_rows=0;executable_sblr_emitted=false", 1)),
            "wrong_code": (PGP_ID, lambda l: l.replace("refusal_code=CRYPTO.PGP.PROFILE_UNAVAILABLE", "refusal_code=CRYPTO.PGP.INVALID_KEY")),
            "unregistered_ctest": (PGP_ID, lambda l: l.replace("proof=ctest:sbsql_pgp_refusal_route_conformance", "proof=ctest:not_registered_anywhere")),
            "package_token_removed": (PGCRYPTO_ID, lambda l: l.replace("verifier_admitted=false", "verifier_admitted_x", 1)),
            "unadmitted_row_claims_kind": (OTHER_ID, lambda l: l.replace(
                "canonical_message_vector_set", "canonical_message_vector_set;refusal_kind=engine_reached_refusal", 1)),
        }
        skipped: list[str] = []
        for gate, (script, extra, relpath) in GATES.items():
            for case, (sid, edit) in cases.items():
                path = ROOT / relpath
                try:
                    data = edit_csv(path, sid, edit)
                except AssertionError:
                    self.fail(f"{gate}: row {sid} not found")
                if data == path.read_bytes():
                    skipped.append(f"{gate}/{case}")
                    continue  # this gate's artifact does not carry the mutated text
                with self.subTest(gate=gate, case=case):
                    overlay = self.overlay({relpath: data})
                    result = run_gate(overlay, script, extra)
                    self.assertNotEqual(0, result.returncode, f"{gate}/{case} passed a malformed row")
        self.assertEqual([], skipped, "a malformed-row case did not mutate its artifact")

    def test_fixture_gates_fail_on_malformed_fixtures(self):
        skipped: list[str] = []
        for gate, (script, extra, template) in FIXTURE_GATES.items():
            for sid, old, new in (
                (PGP_ID, "refusal_kind=engine_reached_refusal", "refusal_kind=engine_reached_refusal_x"),
                (PGP_ID, "engine_reached=true", "engine_reached=false"),
                (PGP_ID, "result_rows=0", "result_rows=0;executable_sblr_emitted=false"),
                (PGP_ID, "refusal_code=CRYPTO.PGP.PROFILE_UNAVAILABLE", "refusal_code=CRYPTO.PGP.NO_SUCH"),
                (PGCRYPTO_ID, "verifier_admitted=false", "verifier_admitted=true"),
                (PGCRYPTO_ID, "pre_sblr_refusal=true", "pre_sblr_refusal=false"),
            ):
                relpath = template.format(sid=sid)
                text = (ROOT / relpath).read_text(encoding="utf-8")
                if old not in text:
                    skipped.append(f"{gate}/{sid}/{old}")
                    continue  # this fixture kind does not carry that token
                with self.subTest(gate=gate, sid=sid, old=old):
                    overlay = self.overlay({relpath: text.replace(old, new, 1).encode("utf-8")})
                    result = run_gate(overlay, script, extra)
                    self.assertNotEqual(0, result.returncode, f"{gate} passed a malformed fixture ({old})")
        self.assertEqual([], skipped, "a malformed-fixture case did not mutate its artifact")


if __name__ == "__main__":
    unittest.main()
