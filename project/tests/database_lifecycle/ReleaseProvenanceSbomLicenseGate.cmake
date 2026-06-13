# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

function(sb_add_database_lifecycle_release_provenance_sbom_license_gate)
  set(SB_RELEASE_PROVENANCE_REPO_ROOT "${PROJECT_SOURCE_DIR}/..")
  set(SB_RELEASE_PROVENANCE_GENERATOR
    "${PROJECT_SOURCE_DIR}/tools/release_provenance/generate_release_evidence.py"
  )
  set(SB_RELEASE_PROVENANCE_GATE
    "${PROJECT_SOURCE_DIR}/tests/database_lifecycle/release_provenance_sbom_license_gate.py"
  )

  add_custom_target(database_lifecycle_release_provenance_sbom_license_gate_check
    COMMAND "${Python3_EXECUTABLE}" "${SB_RELEASE_PROVENANCE_GATE}"
            --repo-root "${SB_RELEASE_PROVENANCE_REPO_ROOT}"
            --build-root "${CMAKE_BINARY_DIR}"
            --generator "${SB_RELEASE_PROVENANCE_GENERATOR}"
    COMMENT "Validating release provenance SBOM license and generated artifact evidence"
  )

  add_test(
    NAME database_lifecycle_release_provenance_sbom_license_gate
    COMMAND "${Python3_EXECUTABLE}" "${SB_RELEASE_PROVENANCE_GATE}"
            --repo-root "${SB_RELEASE_PROVENANCE_REPO_ROOT}"
            --build-root "${CMAKE_BINARY_DIR}"
            --generator "${SB_RELEASE_PROVENANCE_GENERATOR}"
  )
  set_tests_properties(database_lifecycle_release_provenance_sbom_license_gate PROPERTIES
    LABELS "database_lifecycle_release_provenance_sbom_license_gate;CBQ-035;CBQ_GATE_RELEASE_PROVENANCE_SBOM_LICENSE;ELER-108;ELER-GATE-108;release;provenance;sbom;license;checksum;artifact;package;manifest;release_artifact_trust;linux_proof;database_lifecycle"
  )
endfunction()
