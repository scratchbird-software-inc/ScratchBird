# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Authorization-filtered metadata resolution contract for AI MCP."""

from __future__ import annotations

from typing import Any

from .deterministic import deterministic_id


def build_metadata_resolution_contract(*, security_context: dict[str, Any] | None = None) -> dict[str, Any]:
    """Return the AI MCP metadata contract used by drivers and tools.

    The AI layer does not expose raw catalog walks as authority. It uses the
    same server/driver metadata surfaces as other clients and treats all path
    and UUID resolution as authorization-filtered by the server session.
    """

    ctx = security_context or {}
    actor = str(ctx.get("actor_id", "")).strip()
    tenant = str(ctx.get("tenant_id", "")).strip()
    return {
        "schema_id": "scratchbird.ai.metadata_resolution_contract.v1",
        "resolution_authority": "server_authorization_filtered_sys_information",
        "client_cache_authority": "advisory_only",
        "uuid_path_resolution": "server_filtered_uuid_to_path",
        "path_uuid_resolution": "server_filtered_path_to_uuid",
        "hidden_objects_policy": "not_returned",
        "required_server_revalidation": True,
        "supported_metadata_surfaces": [
            "sys.information.object_tree",
            "sys.catalog.schemas",
            "sys.catalog.tables",
            "sys.catalog.columns",
        ],
        "trace_id": deterministic_id(
            "tr",
            {
                "operation": "metadata_resolution_contract",
                "tenant_id": tenant,
                "actor_id": actor,
            },
        ),
    }
