# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(SB_PRIVATE_REPO_ROOT "${PROJECT_SOURCE_DIR}/..")
set(SB_COMMERCIAL_READINESS_GATE
  "${CMAKE_CURRENT_SOURCE_DIR}/commercial_readiness_gate.py"
)

function(sb_add_commercial_readiness_gate gate_name gate_mode gate_labels)
  add_test(
    NAME "${gate_name}"
    COMMAND "${Python3_EXECUTABLE}" "${SB_COMMERCIAL_READINESS_GATE}"
            --repo-root "${SB_PRIVATE_REPO_ROOT}"
            --mode "${gate_mode}"
  )
  set_tests_properties("${gate_name}" PROPERTIES
    LABELS "${gate_labels};commercial_readiness;CRP"
  )
endfunction()

sb_add_commercial_readiness_gate(
  commercial_readiness_manifest_gate
  manifest
  "CRP-GATE-001;CRP-GATE-900;commercial_readiness_manifest_gate"
)
sb_add_commercial_readiness_gate(
  commercial_readiness_release_gate
  release
  "CRP-GATE-020;CRP-GATE-010;commercial_readiness_release_gate"
)
sb_add_commercial_readiness_gate(
  commercial_readiness_benchmark_gate
  benchmark
  "CRP-GATE-030;CRP-GATE-033;commercial_readiness_benchmark_gate"
)
sb_add_commercial_readiness_gate(
  commercial_readiness_durability_gate
  durability
  "CRP-GATE-040;CRP-GATE-041;CRP-GATE-042;CRP-GATE-043;commercial_readiness_durability_gate"
)
sb_add_commercial_readiness_gate(
  commercial_readiness_cluster_gate
  cluster
  "CRP-GATE-050;CRP-GATE-051;commercial_readiness_cluster_gate"
)
sb_add_commercial_readiness_gate(
  commercial_readiness_security_provider_gate
  security_provider
  "CRP-GATE-060;CRP-GATE-061;CRP-GATE-062;CRP-GATE-063;CRP-GATE-064;commercial_readiness_security_provider_gate"
)
sb_add_commercial_readiness_gate(
  commercial_readiness_nosql_fusion_gate
  nosql_fusion
  "CRP-GATE-070;commercial_readiness_nosql_fusion_gate"
)
sb_add_commercial_readiness_gate(
  commercial_readiness_support_bundle_gate
  support_bundle
  "CRP-GATE-080;CRP-GATE-081;CRP-GATE-082;commercial_readiness_support_bundle_gate"
)
sb_add_commercial_readiness_gate(
  commercial_readiness_stress_refactor_gate
  stress_refactor
  "CRP-GATE-090;CRP-GATE-091;CRP-GATE-092;CRP-GATE-093;CRP-GATE-100;CRP-GATE-101;commercial_readiness_stress_refactor_gate"
)
sb_add_commercial_readiness_gate(
  commercial_readiness_final_gate
  final
  "CRP-GATE-900;commercial_readiness_final_gate"
)
