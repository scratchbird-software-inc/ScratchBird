# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Prepared SBLR/UUID artifact contracts for the AI MCP layer.

The AI service, MCP runtime, and HTTP bridge are outside the engine authority
boundary. They may lower SQL text through a parser/driver path and carry the
original SQL as data, but engine execution authority is the admitted SBLR/UUID
envelope after server-side validation.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from typing import Any

from .deterministic import deterministic_id


ARTIFACT_SCHEMA_VERSION = "scratchbird.ai.prepared_sblr_uuid_artifact.v1"
SBLR_FORMAT = "sblr_uuid_envelope"
SOURCE_SQL_TEXT_POLICY = "data_packet_only_not_execution_authority"
EXECUTION_AUTHORITY = "engine_sblr_uuid_only"
DRIVER_PARSER_AUTHORITY = "lowering_only_untrusted"

SERVER_REVALIDATED = "server_revalidated"
SERVER_REVALIDATION_REQUIRED = "server_revalidation_required"
TEST_ONLY_MOCK = "test_only_mock"
ALLOWED_REVALIDATION_STATES = {
    SERVER_REVALIDATED,
    SERVER_REVALIDATION_REQUIRED,
    TEST_ONLY_MOCK,
}


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def normalize_revalidation_state(raw: Any, *, adapter_mode: str) -> str:
    value = str(raw or "").strip()
    if value in ALLOWED_REVALIDATION_STATES:
        return value
    if adapter_mode.strip().lower() == "mock":
        return TEST_ONLY_MOCK
    return SERVER_REVALIDATION_REQUIRED


def _string_list(value: Any) -> list[str]:
    if not isinstance(value, list):
        return []
    return [str(item) for item in value if str(item).strip()]


def _int_or_zero(value: Any) -> int:
    if isinstance(value, bool):
        return 0
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


@dataclass(slots=True)
class PreparedSblrArtifact:
    artifact_schema_version: str
    compile_artifact_id: str
    dialect: str
    statement_kind: str
    sblr_hash: str
    sblr_format: str
    uuid_dependencies: list[str]
    descriptor_epoch: int
    security_epoch: int
    policy_epoch: int
    catalog_epoch: int
    security_context_hash: str
    prepared_handle: str
    server_revalidation_state: str
    server_revalidation_required: bool
    source_sql_text_policy: str
    execution_authority: str
    local_sql_execution_authority: bool
    driver_parser_authority: str
    source: str
    created_at_utc: str = field(default_factory=utc_now)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def build_prepared_artifact(
    *,
    compile_artifact_id: str,
    dialect: str,
    statement_kind: str,
    sblr_hash: str,
    security_context_hash: str,
    adapter_mode: str,
    context: dict[str, Any] | None = None,
    adapter_artifact: dict[str, Any] | None = None,
) -> PreparedSblrArtifact:
    ctx = dict(context or {})
    upstream = dict(adapter_artifact or {})
    upstream_compile_artifact_id = str(upstream.get("compile_artifact_id", "")).strip()
    if upstream_compile_artifact_id and upstream_compile_artifact_id != compile_artifact_id:
        upstream.pop("prepared_handle", None)
    state = normalize_revalidation_state(
        upstream.get("server_revalidation_state", ctx.get("server_revalidation_state")),
        adapter_mode=adapter_mode,
    )
    uuid_dependencies = _string_list(
        upstream.get("uuid_dependencies", ctx.get("uuid_dependencies", []))
    )
    descriptor_epoch = _int_or_zero(upstream.get("descriptor_epoch", ctx.get("descriptor_epoch", 0)))
    security_epoch = _int_or_zero(upstream.get("security_epoch", ctx.get("security_epoch", 0)))
    policy_epoch = _int_or_zero(upstream.get("policy_epoch", ctx.get("policy_epoch", 0)))
    catalog_epoch = _int_or_zero(upstream.get("catalog_epoch", ctx.get("catalog_epoch", 0)))
    prepared_handle = str(upstream.get("prepared_handle", "")).strip()
    if not prepared_handle:
        prepared_handle = deterministic_id(
            "prep",
            {
                "compile_artifact_id": compile_artifact_id,
                "sblr_hash": sblr_hash,
                "security_context_hash": security_context_hash,
                "uuid_dependencies": uuid_dependencies,
                "descriptor_epoch": descriptor_epoch,
                "security_epoch": security_epoch,
                "policy_epoch": policy_epoch,
                "catalog_epoch": catalog_epoch,
            },
        )

    return PreparedSblrArtifact(
        artifact_schema_version=ARTIFACT_SCHEMA_VERSION,
        compile_artifact_id=compile_artifact_id,
        dialect=dialect,
        statement_kind=statement_kind,
        sblr_hash=sblr_hash,
        sblr_format=str(upstream.get("sblr_format", SBLR_FORMAT)).strip() or SBLR_FORMAT,
        uuid_dependencies=uuid_dependencies,
        descriptor_epoch=descriptor_epoch,
        security_epoch=security_epoch,
        policy_epoch=policy_epoch,
        catalog_epoch=catalog_epoch,
        security_context_hash=security_context_hash,
        prepared_handle=prepared_handle,
        server_revalidation_state=state,
        server_revalidation_required=state != SERVER_REVALIDATED,
        source_sql_text_policy=SOURCE_SQL_TEXT_POLICY,
        execution_authority=EXECUTION_AUTHORITY,
        local_sql_execution_authority=False,
        driver_parser_authority=DRIVER_PARSER_AUTHORITY,
        source=str(upstream.get("source", ctx.get("source", adapter_mode or "unknown"))),
    )


def validate_prepared_artifact(payload: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if payload.get("artifact_schema_version") != ARTIFACT_SCHEMA_VERSION:
        errors.append("invalid artifact_schema_version")
    if payload.get("sblr_format") != SBLR_FORMAT:
        errors.append("sblr_format must be sblr_uuid_envelope")
    if payload.get("execution_authority") != EXECUTION_AUTHORITY:
        errors.append("execution_authority must be engine_sblr_uuid_only")
    if payload.get("local_sql_execution_authority") is not False:
        errors.append("local_sql_execution_authority must be false")
    if payload.get("driver_parser_authority") != DRIVER_PARSER_AUTHORITY:
        errors.append("driver_parser_authority must be lowering_only_untrusted")
    state = payload.get("server_revalidation_state")
    if state not in ALLOWED_REVALIDATION_STATES:
        errors.append("server_revalidation_state is invalid")
    if state == SERVER_REVALIDATED and payload.get("server_revalidation_required") is not False:
        errors.append("server_revalidated artifacts must set server_revalidation_required=false")
    if not str(payload.get("compile_artifact_id", "")).strip():
        errors.append("compile_artifact_id is required")
    if not str(payload.get("sblr_hash", "")).strip():
        errors.append("sblr_hash is required")
    if not str(payload.get("security_context_hash", "")).strip():
        errors.append("security_context_hash is required")
    return errors
