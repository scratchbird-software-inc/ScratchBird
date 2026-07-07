# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.sblr_artifacts import build_prepared_artifact


class PreparedSblrArtifactTests(unittest.TestCase):
    def test_upstream_prepared_handle_is_rejected_when_compile_id_changes(self) -> None:
        artifact = build_prepared_artifact(
            compile_artifact_id="cmp_final",
            dialect="native",
            statement_kind="read",
            sblr_hash="sblr_hash",
            security_context_hash="security_hash",
            adapter_mode="http",
            adapter_artifact={
                "compile_artifact_id": "cmp_adapter_local",
                "prepared_handle": "prep_adapter_local",
                "server_revalidation_state": "server_revalidated",
            },
        )

        self.assertNotEqual(artifact.prepared_handle, "prep_adapter_local")
        self.assertTrue(artifact.prepared_handle.startswith("prep_"))

    def test_upstream_prepared_handle_is_kept_when_compile_id_matches(self) -> None:
        artifact = build_prepared_artifact(
            compile_artifact_id="cmp_final",
            dialect="native",
            statement_kind="read",
            sblr_hash="sblr_hash",
            security_context_hash="security_hash",
            adapter_mode="http",
            adapter_artifact={
                "compile_artifact_id": "cmp_final",
                "prepared_handle": "prep_server_revalidated",
                "server_revalidation_state": "server_revalidated",
            },
        )

        self.assertEqual(artifact.prepared_handle, "prep_server_revalidated")


if __name__ == "__main__":
    unittest.main()
