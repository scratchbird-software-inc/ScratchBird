# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Manifest-consumable beta readiness and advisory-cache guardrails."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Mapping, Optional


DRIVER_READINESS_SCHEMA_VERSION = "scratchbird.driver.readiness.v1"
DRIVER_COMPONENT_ID = "driver:python"
DRIVER_PACKAGE_UUID = "019e12a0-0012-7000-8000-000000000012"
DRIVER_STATUS = "beta_2"
DRIVER_RELEASE_BUCKET = "release_candidate"
DRIVER_CONFORMANCE_PROFILE = "driver_python_gate"
DRIVER_SOURCE_PATH = "project/drivers/driver/python"
DRIVER_LICENSE = "MPL-2.0"
STANDARD_ENGLISH_LANGUAGE = "en_US"


@dataclass(frozen=True)
class ReadinessDiagnostic:
    code: str
    sqlstate: str
    message: str

    def __str__(self) -> str:
        return f"[{self.sqlstate}] {self.message}" if self.sqlstate else self.message


@dataclass(frozen=True)
class AdvisoryCacheContext:
    database_uuid: str = ""
    schema_epoch: str = ""
    policy_epoch: str = ""
    language_epoch: str = ""
    capability_epoch: str = ""
    principal_uuid: str = ""
    role_set_hash: str = ""
    group_set_hash: str = ""
    transaction_uuid: str = ""


@dataclass(frozen=True)
class PreparedBundleContext:
    database_uuid: str = ""
    schema_epoch: str = ""
    policy_epoch: str = ""
    principal_uuid: str = ""
    transaction_uuid: str = ""
    server_admitted: bool = False


@dataclass(frozen=True)
class LanguageProfileResolution:
    requested: str
    selected: str
    fallback: bool = False
    reason: str = ""


@dataclass(frozen=True)
class LanguageResourceState:
    locale: str = ""
    schema_version: str = ""
    content_hash: str = ""
    signature: str = ""
    epoch: str = ""
    expected_epoch: str = ""


def beta_driver_readiness_status() -> dict:
    return {
        "schema_version": DRIVER_READINESS_SCHEMA_VERSION,
        "component_id": DRIVER_COMPONENT_ID,
        "driver_package_uuid": DRIVER_PACKAGE_UUID,
        "driver_status": DRIVER_STATUS,
        "release_bucket": DRIVER_RELEASE_BUCKET,
        "conformance_profile_ref": DRIVER_CONFORMANCE_PROFILE,
        "source_path": DRIVER_SOURCE_PATH,
        "package_name": "scratchbird",
        "import_path": "scratchbird",
        "license": DRIVER_LICENSE,
        "runtime_mapping": {
            "api_surface": "dbapi_2",
            "ingress_modes": ["direct_listener", "manager_proxy"],
            "wire_protocols": ["sbwp_v1_1"],
            "dsn_keys": ["database", "host", "port", "user", "auth_method"],
            "auth_methods": ["engine_local_password", "scram_ready"],
            "tls_profile": "scratchbird_tls_1_3_floor",
            "type_mapping_profile": "sbsql_core",
            "diagnostic_mapping_profile": "native_sqlstate",
            "metadata_profile": "sys_information_recursive",
            "thread_safety_class": "thread_safe",
            "pooling_capability": "connection_pool",
        },
        "authority_boundary": {
            "local_sblr_is_advisory": True,
            "local_uuid_cache_is_advisory": True,
            "local_result_cache_is_advisory": True,
            "server_revalidation_required": True,
            "transaction_finality_owner": "engine_mga_transaction_inventory",
            "language_fallback_profile": STANDARD_ENGLISH_LANGUAGE,
            "cache_invalidation_requirement": "policy_schema_language_capability_transaction_epoch",
        },
    }


def validate_advisory_cache_context(
    cached: AdvisoryCacheContext,
    current: AdvisoryCacheContext,
) -> Optional[ReadinessDiagnostic]:
    checks = (
        ("database_uuid", cached.database_uuid, current.database_uuid, "SB_DRIVER_CACHE_DATABASE_MISMATCH"),
        ("schema_epoch", cached.schema_epoch, current.schema_epoch, "SB_DRIVER_CACHE_SCHEMA_EPOCH_STALE"),
        ("policy_epoch", cached.policy_epoch, current.policy_epoch, "SB_DRIVER_CACHE_POLICY_EPOCH_STALE"),
        ("language_epoch", cached.language_epoch, current.language_epoch, "SB_DRIVER_CACHE_LANGUAGE_EPOCH_STALE"),
        (
            "capability_epoch",
            cached.capability_epoch,
            current.capability_epoch,
            "SB_DRIVER_CACHE_CAPABILITY_EPOCH_STALE",
        ),
        ("principal_uuid", cached.principal_uuid, current.principal_uuid, "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH"),
        ("role_set_hash", cached.role_set_hash, current.role_set_hash, "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH"),
        ("group_set_hash", cached.group_set_hash, current.group_set_hash, "SB_DRIVER_CACHE_AUTH_CONTEXT_MISMATCH"),
    )
    for name, observed, expected, code in checks:
        if observed != expected:
            return ReadinessDiagnostic(
                code=code,
                sqlstate="42501",
                message=f"advisory cache entry refused: {name} does not match current context",
            )
    if cached.transaction_uuid or current.transaction_uuid:
        if cached.transaction_uuid != current.transaction_uuid:
            return ReadinessDiagnostic(
                code="SB_DRIVER_CACHE_TRANSACTION_CONTEXT_MISMATCH",
                sqlstate="25001",
                message="advisory cache entry refused: transaction context does not match current MGA boundary",
            )
    return None


def validate_prepared_bundle_reuse(
    bundle: PreparedBundleContext,
    current: AdvisoryCacheContext,
) -> Optional[ReadinessDiagnostic]:
    if not bundle.server_admitted:
        return ReadinessDiagnostic(
            code="SB_DRIVER_SBLR_SERVER_ADMISSION_REQUIRED",
            sqlstate="0A000",
            message="driver-prepared SBLR/UUID bundle is advisory until server admission succeeds",
        )
    return validate_advisory_cache_context(
        AdvisoryCacheContext(
            database_uuid=bundle.database_uuid,
            schema_epoch=bundle.schema_epoch,
            policy_epoch=bundle.policy_epoch,
            principal_uuid=bundle.principal_uuid,
            transaction_uuid=bundle.transaction_uuid,
        ),
        AdvisoryCacheContext(
            database_uuid=current.database_uuid,
            schema_epoch=current.schema_epoch,
            policy_epoch=current.policy_epoch,
            principal_uuid=current.principal_uuid,
            transaction_uuid=current.transaction_uuid,
        ),
    )


def resolve_language_profile(
    requested: str,
    available: Optional[Mapping[str, bool]] = None,
) -> LanguageProfileResolution:
    selected_request = requested or STANDARD_ENGLISH_LANGUAGE
    if available and available.get(selected_request):
        return LanguageProfileResolution(requested=selected_request, selected=selected_request)
    return LanguageProfileResolution(
        requested=selected_request,
        selected=STANDARD_ENGLISH_LANGUAGE,
        fallback=True,
        reason="unsupported_or_unavailable_language_profile",
    )


def validate_language_resource_state(state: LanguageResourceState) -> Optional[ReadinessDiagnostic]:
    if not state.locale or not state.schema_version or not state.content_hash or not state.signature:
        return ReadinessDiagnostic(
            code="SB_DRIVER_LANGUAGE_RESOURCE_INCOMPLETE",
            sqlstate="0A000",
            message="language resource refused: locale, schema version, content hash, and signature are required",
        )
    if state.expected_epoch and state.epoch != state.expected_epoch:
        return ReadinessDiagnostic(
            code="SB_DRIVER_LANGUAGE_RESOURCE_EPOCH_STALE",
            sqlstate="0A000",
            message="language resource refused: language epoch does not match current context",
        )
    return None
