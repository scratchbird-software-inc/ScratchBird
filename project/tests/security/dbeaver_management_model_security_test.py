#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static security checks for the ScratchBird DBeaver management model."""

from __future__ import annotations

from pathlib import Path
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[3]
MODEL = ROOT / (
    "project/drivers/adaptor/scratchbird-dbeaver-driver/plugins/"
    "org.jkiss.dbeaver.ext.scratchbird/src/org/jkiss/dbeaver/ext/"
    "scratchbird/model"
)
PLUGIN_XML = ROOT / (
    "project/drivers/adaptor/scratchbird-dbeaver-driver/plugins/"
    "org.jkiss.dbeaver.ext.scratchbird/plugin.xml"
)


def read_source(name: str) -> str:
    path = MODEL / name
    assert path.exists(), f"missing model source: {path}"
    return path.read_text(encoding="utf-8")


def require_all(source: str, needles: list[str], label: str) -> None:
    missing = [needle for needle in needles if needle not in source]
    assert not missing, f"{label} missing required evidence: {missing}"


def test_authorization_context_binds_cache_scope_to_uuid_identity_and_epochs() -> None:
    source = read_source("ScratchBirdAuthorizationContext.java")
    require_all(
        source,
        [
            "databaseUuid",
            "sessionUuid",
            "authenticatedUserUuid",
            "effectiveRoleUuids",
            "effectiveGroupUuids",
            "exactLanguageProfileTag",
            "databaseDefaultLanguageTag",
            "catalogEpoch",
            "grantEpoch",
            "securityPolicyEpoch",
            "descriptorEpoch",
            "localizedNameEpoch",
            "policyEpoch",
            "resolverPolicy",
            "resolverSnapshotIdentity",
            "transactionContextId",
            "serverAdmitted &&",
            "UUID.fromString(value)",
            "CAPABILITY_SBLR_UUID_PASSTHROUGH",
            "sbsql.sblr_uuid_passthrough.v1",
        ],
        "authorization context",
    )


def test_session_scope_and_resolver_cache_are_admission_bound_and_non_disclosing() -> None:
    session = read_source("ScratchBirdSessionScope.java")
    cache = read_source("ScratchBirdAuthorizedResolverCache.java")
    require_all(
        session,
        [
            "ScratchBirdAuthorizedResolverCache",
            "updateAuthorizationContext",
            "invalidateCaches()",
            "cacheCompatibleWith",
            "Isolation rule: cache keys include connection",
            "UUID.nameUUIDFromBytes",
            ".serverAdmitted(false)",
        ],
        "session scope",
    )
    require_all(
        cache,
        [
            "DIRECTION_PATH_TO_UUID",
            "DIRECTION_UUID_TO_PATH",
            "context.canUseResolverCache()",
            "Status.NOT_DISCLOSED",
            "hiddenOrMissing",
            "No authorized resolver entry is available; hidden and missing objects are intentionally indistinguishable.",
        ],
        "authorized resolver cache",
    )


def test_action_admission_metadata_keeps_dbeaver_advisory_only() -> None:
    action = read_source("ScratchBirdActionAdmission.java")
    executor = read_source("ScratchBirdAdminExecutor.java")
    probe = read_source("ScratchBirdPermissionProbe.java")
    apply_executor = read_source("ScratchBirdMutationApplyExecutor.java")
    require_all(
        action,
        [
            "PENDING_SERVER_ADMISSION",
            "SERVER_ADMITTED",
            "SERVER_REFUSED",
            "FALLBACK_TO_SERVER_TEXT",
            "previewHash",
            "commandHash",
            "referencedUuidHash",
            "authorizationFingerprint",
            "transactionContextId",
            "server-side admission, authorization, UUID validation, and transaction-context validation",
            "featureBoundaryStatus",
        ],
        "action admission",
    )
    require_all(
        executor,
        ["admissionMetadata()", "featureBoundaryStatus()", "redactedCommandText()"],
        "admin executor",
    )
    require_all(
        probe,
        ["serverAdmissionRequired", "allowsServerProbe()", "sys.security.permission_probe", "preview_hash", "command_hash"],
        "permission probe",
    )
    require_all(
        apply_executor,
        [
            "applyReadiness",
            "ScratchBirdLiveProbe.mutationAdmissionStatus",
            "ScratchBirdRefusalModel.serverRefused",
            "JDBC/SBsql route",
            "commandHash",
            "previewHash",
        ],
        "mutation apply executor",
    )


def test_redaction_policy_covers_driver_secrets_and_evidence_surfaces() -> None:
    redactor = read_source("ScratchBirdSecurityRedactor.java")
    provider = read_source("ScratchBirdDataSourceProvider.java")
    live_probe = read_source("ScratchBirdLiveProbe.java")
    history = read_source("ScratchBirdProbeHistory.java")
    network = read_source("ScratchBirdNetworkPolicy.java")
    require_all(
        redactor,
        [
            "manager_auth_token",
            "auth_token",
            "auth_method_payload",
            "auth_payload_json",
            "auth_payload_b64",
            "workload_identity_token",
            "proxy_principal_assertion",
            "dormant_reattach_token",
            "channel_binding_token",
            "Authorization",
            "replaceSensitiveAssignments",
            "hashForAudit",
        ],
        "security redactor",
    )
    require_all(
        provider,
        ["sanitizeDescription", "sanitizeDefaultValue", "isSensitiveProperty(desc.name) ? null : desc.choices"],
        "data source provider",
    )
    require_all(
        live_probe + history + network,
        ["ScratchBirdSecurityRedactor.redactEvidenceText", "ScratchBirdSecurityRedactor.redactPropertyValue"],
        "redacted evidence surfaces",
    )


def test_plugin_descriptor_marks_secret_properties_secured_and_password() -> None:
    root = ET.parse(PLUGIN_XML).getroot()
    properties = {
        property_node.get("id"): property_node
        for property_node in root.findall(".//provider-properties/propertyGroup/property")
    }
    sensitive = {
        "manager_auth_token",
        "auth_token",
        "auth_method_payload",
        "auth_payload_json",
        "auth_payload_b64",
        "workload_identity_token",
        "proxy_principal_assertion",
        "dormant_reattach_token",
    }
    for property_id in sensitive:
        node = properties.get(property_id)
        assert node is not None, f"missing sensitive provider property: {property_id}"
        features = {
            feature.strip()
            for feature in (node.get("features") or "").split(",")
            if feature.strip()
        }
        assert {"secured", "password"} <= features, (
            f"{property_id} must use DBeaver secured,password features"
        )
        assert "defaultValue" not in node.attrib, f"{property_id} must not declare a default value"


def test_feature_boundaries_and_support_contracts_do_not_overclaim_server_authority() -> None:
    status = read_source("ScratchBirdFeatureBoundaryStatus.java")
    refusal = read_source("ScratchBirdRefusalModel.java")
    envelope = read_source("ScratchBirdManagementActionEnvelope.java")
    data_editor = read_source("ScratchBirdDataEditorContract.java")
    data_transfer = read_source("ScratchBirdDataTransferContract.java")
    graph = read_source("ScratchBirdObjectGraphContract.java")
    require_all(
        status,
        [
            "REQUIRES_SERVER_ADMISSION",
            "POLICY_DENIED",
            "UNAVAILABLE",
            "ENTERPRISE_ONLY",
            "CLOSED_PROVIDER_ONLY",
            "HIDDEN_OR_MISSING",
            "ScratchBirdRefusalModel.notDisclosed",
        ],
        "feature boundary status",
    )
    require_all(
        refusal,
        ["SERVER_ADMISSION_REQUIRED", "NOT_DISCLOSED", "isDeterministicRefusal", "redactedMessage"],
        "refusal model",
    )
    require_all(
        envelope + data_editor + data_transfer + graph,
        [
            "server_must_revalidate_sblr_uuid",
            "MGA_SERVER_OWNED_ALWAYS_ACTIVE_SESSION",
            "Generated SBLR/UUID update bundles must be revalidated by the server before mutation.",
            "Identifiers are resolved to server-authorized UUIDs before mutation or export visibility decisions.",
            "Hidden objects must not appear",
        ],
        "DBeaver management support contracts",
    )


def main() -> None:
    test_authorization_context_binds_cache_scope_to_uuid_identity_and_epochs()
    test_session_scope_and_resolver_cache_are_admission_bound_and_non_disclosing()
    test_action_admission_metadata_keeps_dbeaver_advisory_only()
    test_redaction_policy_covers_driver_secrets_and_evidence_surfaces()
    test_plugin_descriptor_marks_secret_properties_secured_and_password()
    test_feature_boundaries_and_support_contracts_do_not_overclaim_server_authority()
    print("dbeaver_management_model_security_test: ok")


if __name__ == "__main__":
    main()
