# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Redacted support bundle helpers for AI MCP packaging."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .language_resources import language_resource_summary


SECRET_KEYS = {
    "api_key",
    "auth_method_payload",
    "auth_payload_b64",
    "auth_payload_json",
    "jwt",
    "manager_auth_token",
    "password",
    "secret",
    "token",
}


def redact(value: Any) -> Any:
    if isinstance(value, dict):
        out: dict[str, Any] = {}
        for key, item in value.items():
            key_text = str(key)
            if key_text.lower() in SECRET_KEYS or any(marker in key_text.lower() for marker in ("secret", "token", "password")):
                out[key_text] = "<redacted>"
            else:
                out[key_text] = redact(item)
        return out
    if isinstance(value, list):
        return [redact(item) for item in value]
    return value


def build_support_bundle_manifest(
    *,
    runtime_settings: dict[str, Any],
    output_dir: Path | None = None,
) -> dict[str, Any]:
    manifest = {
        "schema_id": "scratchbird.ai_mcp_support_bundle_manifest.v1",
        "component_id": "adaptor:scratchbird-ai-mcp",
        "redaction_policy": "secret_like_fields_redacted",
        "runtime_settings": redact(runtime_settings),
        "language_resource_pack": language_resource_summary(verify_hashes=False),
    }
    if output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)
        (output_dir / "ai_mcp_support_bundle_manifest.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return manifest
