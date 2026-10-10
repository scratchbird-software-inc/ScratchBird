#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Negative controls for the acceleration gate's startup/mutation boundary."""

import hashlib
import struct
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import sbsql_acceleration_full_route_gate as gate
import sbsql_copy_persistence_full_route_gate as route_support
import acceleration_journal_oracle as oracle


def field(value):
    return struct.pack("<I", len(value)) + value


class JournalTests(unittest.TestCase):
    table = bytes.fromhex("019d0000000070008000000000000001")
    other = bytes.fromhex("019d0000000070008000000000000002")

    def metadata(self):
        fields = [b"SBMGA1", b"TABLE_METADATA", b"1", b"1", self.table,
                  b"sys.agent_durable_catalog_state", b"columns", b"0", b"",
                  bytes(16), b""]
        payload = struct.pack("<I", len(fields)) + b"".join(map(field, fields))
        frame = b"SBMGAM02" + struct.pack("<Q", len(payload)) + payload
        return frame + hashlib.sha256(frame).digest()

    def rows(self, table=None, kind=b"agent_catalog_image", deleted=0):
        names = sorted(oracle.AGENT_FIELDS)
        values = {name: b"value\x00\n\tSBMRBIN1" for name in names}
        values[b"record_kind"] = kind
        values[b"authority_evidence_uuid"] = self.other
        values[b"storage_commit_evidence_uuid"] = self.table
        header = b"SBMRBIN1" + struct.pack("<HHIQ", 8, 0, len(names), 1)
        header += b"".join(map(field, names)) + field(b"text") * len(names)
        identity = (table or self.table) + self.other + self.other + bytes(32)
        row = struct.pack("<QQQB", 1, 2, 0, deleted) + identity + bytes(len(names))
        return header + row + b"".join(field(values[name]) for name in names)

    def check(self, rows, metadata=None):
        before = {".sb.api_events": b"", ".sb.mga_row_versions": b"baseline",
                  ".sb.mga_relation_metadata": metadata or self.metadata()}
        after = dict(before)
        after[".sb.mga_row_versions"] += rows
        gate.require_no_application_journal_mutation(before, after)

    def test_multiple_binary_batches_with_embedded_newlines(self):
        self.check(self.rows() * 2)

    def test_application_table_cannot_spoof_agent_fields(self):
        with self.assertRaises(gate.AccelerationGateError):
            self.check(self.rows(table=self.other))

    def test_textual_uuid_payload_rejected_despite_text_annotation(self):
        rows = self.rows().replace(field(self.other), field(b"019d0000-0000-7000-8000-000000000002"))
        with self.assertRaises(gate.AccelerationGateError):
            self.check(rows)

    def test_unknown_type_annotation_rejected(self):
        with self.assertRaises(gate.AccelerationGateError):
            self.check(self.rows().replace(field(b"text"), field(b"unknown"), 1))

    def test_nil_or_invalid_evidence_identity_rejected(self):
        for identity in (bytes(16), bytes([255]) * 16):
            with self.subTest(identity=identity), self.assertRaises(gate.AccelerationGateError):
                self.check(self.rows().replace(field(self.other), field(identity)))

    def test_application_record_cannot_hide_after_agent_batch(self):
        with self.assertRaises(gate.AccelerationGateError):
            self.check(self.rows() + self.rows(kind=b"application"))

    def test_deletion_is_not_operational_update(self):
        with self.assertRaises(gate.AccelerationGateError):
            self.check(self.rows(deleted=1))

    def test_every_nonempty_truncation_is_rejected(self):
        rows = self.rows()
        for size in range(1, len(rows)):
            with self.subTest(size=size), self.assertRaises(gate.AccelerationGateError):
                self.check(rows[:size])

    def test_corrupted_catalog_checksum_is_rejected(self):
        metadata = self.metadata()
        with self.assertRaises(gate.AccelerationGateError):
            self.check(self.rows(), metadata[:-1] + bytes([metadata[-1] ^ 1]))

    def test_unknown_format_and_trailing_bytes_rejected(self):
        for rows in (self.rows() + b"\n", b"SBMGA1\tROW_VERSION\tagent-catalog-runtime-root\n",
                     self.rows().replace(b"SBMRBIN1", b"SBMRBIN2", 1)):
            with self.subTest(rows=rows[:16]), self.assertRaises(gate.AccelerationGateError):
                self.check(rows)


class ReadinessTests(unittest.TestCase):
    def test_authentication_only_never_sends_query(self):
        sock = Mock()
        with patch.object(gate, "authenticate_tls", return_value=(sock, b"attachment", 7, 3)), \
                patch.object(gate, "send_frame") as send:
            gate.require_authenticated_readiness(1234)
        send.assert_called_once_with(sock, gate.MSG_TERMINATE, 7, attachment=b"attachment")
        sock.close.assert_called_once_with()

    def test_termination_failure_still_closes_socket(self):
        sock = Mock()
        with patch.object(gate, "authenticate_tls", return_value=(sock, b"attachment", 7, 3)), \
                patch.object(gate, "send_frame", side_effect=RuntimeError("terminate refused")):
            with self.assertRaisesRegex(RuntimeError, "terminate refused"):
                gate.require_authenticated_readiness(1234)
        sock.close.assert_called_once_with()

    def test_failed_authentication_does_not_leak_socket(self):
        sock = Mock()
        with patch.object(route_support, "connect_tls", return_value=sock), \
                patch.object(route_support, "authenticate", side_effect=RuntimeError("auth refused")):
            with self.assertRaisesRegex(RuntimeError, "auth refused"):
                route_support.authenticate_tls(1234)
        sock.close.assert_called_once_with()

    def exercise_gate(self, mutation=None, auth_failure=False):
        state = {"ready": False, "shows": 0, "fingerprints": 0}
        route = SimpleNamespace(port=1234)

        def authenticate_only(port):
            self.assertEqual(port, route.port)
            self.assertEqual(state["shows"], 0)
            if auth_failure:
                raise RuntimeError("readiness refused")
            state["ready"] = True

        def fingerprint(database):
            self.assertTrue(state["ready"], "baseline precedes acknowledged readiness")
            state["fingerprints"] += 1
            if state["fingerprints"] == 1:
                self.assertEqual(state["shows"], 0, "warm-up query concealed an effect")
            changed = ((mutation == "first_show" and state["shows"] > 0) or
                       (mutation == "restart" and state["shows"] == 3))
            return {".sb.catalog_object_events": "changed" if changed else "initial"}

        def show(port):
            self.assertTrue(state["ready"])
            self.assertGreater(state["fingerprints"], 0)
            state["shows"] += 1
            return (("immutable-provider-row",),)

        with patch.object(Path, "mkdir"), \
                patch.object(gate, "generate_server_cert", return_value=(Path("cert"), Path("key"))), \
                patch.object(gate, "start_route", return_value=route), \
                patch.object(gate, "stop_route"), \
                patch.object(gate, "require_authenticated_readiness", side_effect=authenticate_only), \
                patch.object(gate, "durable_mutation_fingerprint", side_effect=fingerprint), \
                patch.object(gate, "operational_journal_snapshot", return_value={}), \
                patch.object(gate, "execute_show", side_effect=show), \
                patch.object(gate, "require_authentication_refusal"), \
                patch.object(gate, "require_exact_trace") as trace:
            if auth_failure:
                with self.assertRaisesRegex(RuntimeError, "readiness refused"):
                    gate.run_gate(SimpleNamespace(openssl="openssl"), Path("unused"))
                self.assertEqual(state["fingerprints"], 0)
                self.assertEqual(state["shows"], 0)
            elif mutation:
                with self.assertRaisesRegex(gate.AccelerationGateError, "mutated"):
                    gate.run_gate(SimpleNamespace(openssl="openssl"), Path("unused"))
            else:
                gate.run_gate(SimpleNamespace(openssl="openssl"), Path("unused"))
                self.assertEqual(state["shows"], 3)
                self.assertEqual(state["fingerprints"], 3)
                self.assertEqual([call.kwargs["expected_observations"] for call in trace.call_args_list], [2, 1])

    def test_baseline_after_readiness_before_first_query(self):
        self.exercise_gate()

    def test_first_query_mutation_is_not_hidden(self):
        self.exercise_gate(mutation="first_show")

    def test_restart_does_not_reset_baseline(self):
        self.exercise_gate(mutation="restart")

    def test_failed_readiness_never_samples_or_executes(self):
        self.exercise_gate(auth_failure=True)


if __name__ == "__main__":
    unittest.main()
