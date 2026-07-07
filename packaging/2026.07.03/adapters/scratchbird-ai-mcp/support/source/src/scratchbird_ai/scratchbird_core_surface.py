# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Machine-readable current ScratchBird core AI surface truth."""

from __future__ import annotations

from typing import Any


SCRATCHBIRD_CORE_AI_PACKET_VERSION = "2026-04-18"
SCRATCHBIRD_CORE_AI_RELEASE_CEILING = "bounded_current_tree"
ENGINE_MANAGED_CONTRACT_SCAFFOLD_BACKEND = "engine_managed_contract_scaffold"
AI_REPO_LIVE_VALIDATION_STATE = "current"

_ENGINE_OWNED_RETRIEVAL_FAMILIES = (
    {
        "family_id": "vector_distance",
        "semantic_contract": "vector similarity retrieval",
        "support_state": "implemented",
    },
    {
        "family_id": "ann_hnsw",
        "semantic_contract": "k_nn_ann_retrieval",
        "support_state": "implemented",
    },
    {
        "family_id": "full_text_inverted",
        "semantic_contract": "full_text_retrieval",
        "support_state": "implemented",
    },
)

_RETRIEVAL_METADATA_RELATIONS = (
    {
        "relation_name": "opensearch_meta.index_metadata",
        "support_state": "implemented",
    },
    {
        "relation_name": "opensearch_meta.mapping_fields",
        "support_state": "implemented",
    },
    {
        "relation_name": "opensearch_meta.analyzer_settings",
        "support_state": "implemented",
    },
    {
        "relation_name": "opensearch_meta.knn_index_metadata",
        "support_state": "implemented",
    },
    {
        "relation_name": "opensearch_meta.aliases",
        "support_state": "implemented",
    },
)

_RUNTIME_MODES = (
    {
        "mode_id": "listener_direct",
        "transport_family": "native_network",
        "support_state": "implemented",
        "required_conditions": [],
    },
    {
        "mode_id": "manager_proxy",
        "transport_family": "native_network_manager_fronted",
        "support_state": "implemented",
        "required_conditions": [
            "manager_control_handshake",
            "dbbt_binding_truth",
            "connect_flag_manager_dbbt",
        ],
    },
    {
        "mode_id": "local_ipc",
        "transport_family": "local_ipc",
        "support_state": "implemented",
        "required_conditions": [
            "local_runtime_topology",
            "driver_supports_local_ipc",
        ],
    },
    {
        "mode_id": "embedded_local_only",
        "transport_family": "embedded_no_public_ipc",
        "support_state": "implemented",
        "required_conditions": [
            "single_connection_only",
            "driver_supports_embedded",
        ],
    },
)


def _clone_rows(rows: tuple[dict[str, Any], ...]) -> list[dict[str, Any]]:
    return [dict(row) for row in rows]


def build_scratchbird_core_surface_packet(
    *,
    engine_managed_binding_state: str = "local_contract_scaffold",
    live_validation_state: str = AI_REPO_LIVE_VALIDATION_STATE,
) -> dict[str, Any]:
    return {
        "packet_version": SCRATCHBIRD_CORE_AI_PACKET_VERSION,
        "release_ceiling": SCRATCHBIRD_CORE_AI_RELEASE_CEILING,
        "support_state": "implemented_in_scratchbird_core",
        "engine_managed_retrieval_profile": {
            "profile_id": "engine_managed_retrieval_v0",
            "support_state": "implemented_in_scratchbird_core",
            "release_interpretation": "bounded_current_tree_contract",
            "binding_state": engine_managed_binding_state,
        },
        "engine_owned_retrieval_contract": {
            "support_state": "implemented",
            "families": _clone_rows(_ENGINE_OWNED_RETRIEVAL_FAMILIES),
        },
        "retrieval_metadata_discovery_packet": {
            "support_state": "implemented",
            "catalog_namespace": "opensearch_meta",
            "relations": _clone_rows(_RETRIEVAL_METADATA_RELATIONS),
            "required_semantics": [
                "queryability_state",
                "metrics_confidence_class",
            ],
        },
        "runtime_mode_truth_packet": {
            "support_state": "implemented",
            "admitted_modes": _clone_rows(_RUNTIME_MODES),
        },
        "verification_state": {
            "core_code_verified": True,
            "ai_repo_live_validation_state": live_validation_state,
            "ai_repo_live_validation_current": live_validation_state == "current",
        },
        "authority": {
            "scratchbird_spec_section": "32_Architecture_and_Component_Boundaries",
            "current_packets": [
                "CURRENT_AI_ENGINE_SURFACE_AND_RELEASE_CEILING_MODEL",
                "CURRENT_ENGINE_OWNED_RETRIEVAL_CONTRACT_MODEL",
                "CURRENT_RETRIEVAL_METADATA_AND_DISCOVERY_PACKET_MODEL",
                "CURRENT_AI_RUNTIME_MODE_TRUTH_PACKET_MODEL",
                "CURRENT_AI_VERIFICATION_AND_RELEASE_EVIDENCE_PACKET_MODEL",
            ],
        },
    }
