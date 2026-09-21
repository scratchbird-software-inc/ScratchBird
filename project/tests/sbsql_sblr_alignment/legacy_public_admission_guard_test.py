# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Source-policy tripwire, not runtime or complete feature conformance."""
from pathlib import Path
import re
import unittest


class LegacyPublicAdmissionGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (Path(__file__).resolve().parents[2] / "src/engine/public_abi.cpp").read_text()
        start = source.index("sb_engine_status_t dispatch_operation_envelope(")
        end = source.index("\nstd::string behavior_payload()", start)
        cls.body = source[start:end]

    def test_unadmitted_surface_requires_immutable_token(self):
        self.assertIn('"immutable_server_admission_token_required"', self.body)
        self.assertRegex(self.body, r"return\s+fail_result\(\s*SB_ENGINE_STATUS_UNSUPPORTED")

    def test_no_success_or_fabricated_effect_publication(self):
        for forbidden in ("SB_ENGINE_STATUS_OK", "make_result(", "->payload", "std::ofstream",
                          "parent_success_barrier=passed", "DispatchSblr"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.body)

    def test_no_operation_selection_before_admission(self):
        for forbidden in ("operation_payload", "refresh_payload", "DecodeSblrEnvelope(",
                          "DecodeSblrOpcodeStream(", "effective_user_uuid.bytes[0]"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.body)

    def test_no_disabled_execution_implementation(self):
        self.assertIsNone(re.search(r"^\s*#\s*if\s+0\b", self.body, re.MULTILINE))


if __name__ == "__main__":
    unittest.main()
