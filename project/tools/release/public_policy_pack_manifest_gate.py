#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the public default policy pack manifest for PCR-128."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path


UUID_RE = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"
)

EXTERNAL_PROVIDERS = {
    "ldap_ad",
    "oidc_jwt",
    "saml",
    "kerberos_pac",
    "pam",
    "radius",
    "webauthn",
    "certificate_mtls",
    "workload_identity",
    "managed_identity",
    "custom_cpp_plugin",
}

REQUIRED_POLICY_AREAS = {
    "security_provider_selection",
    "standard_roles_groups_grants",
    "default_security_posture",
    "memory_resource_governance",
    "storage_filespace_page_policy",
    "transaction_mga_cleanup_archive_backup_forward",
    "optimizer_statistics_feedback",
    "index_maintenance",
    "agent_policy",
    "diagnostics",
    "observability",
    "unsupported_feature_behavior",
    "cluster_boundary",
    "release_default_configuration",
}

REQUIRED_CATALOG_ROW_FAMILIES = {
    "PolicyPackRecord",
    "SecurityProviderRecord",
    "SecurityRoleRecord",
    "SecurityGroupRecord",
    "SecurityGrantRecord",
    "PolicyProfileRecord",
    "DefaultPolicyRecord",
}

REQUIRED_DEFAULT_POLICY_COUNT = 58
REQUIRED_MEMORY_HARD_LIMIT_BYTES = 1024 * 1024 * 1024
REQUIRED_MEMORY_PAGE_BUFFER_POOL_BYTES = 512 * 1024 * 1024

REQUIRED_DEFAULT_POLICY_KEYS = {
    "admin.management_command_authorization",
    "backup.archive_restore_snapshot_shadow",
    "cache.checkpoint_preload_flush",
    "capability.feature_gate",
    "cluster.boundary_fail_closed",
    "concurrency.lock_wait_deadlock",
    "configuration.override_reload",
    "configuration.source_precedence",
    "database.bootstrap.tx1",
    "database.create.failure_cleanup",
    "database.first_open.tx2_activation",
    "database.identity",
    "diagnostics.message_vector",
    "event.queue_notification",
    "evidence.retention",
    "executable.side_effect",
    "ipc.frame_auth_backpressure",
    "job.scheduler",
    "lifecycle.maintenance_restricted",
    "lifecycle.ownership_stale_owner",
    "lifecycle.recovery_dirty_open",
    "lifecycle.shutdown_force",
    "lifecycle.shutdown_graceful_drain",
    "listener.bind_tls_pool",
    "observability.metrics_log",
    "parser.package_admission",
    "policy.catalog.bootstrap",
    "reference.emulation_profile",
    "replication.cdc_changefeed_boundary",
    "resource.seed_i18n",
    "resource.signature_provenance",
    "schema.bootstrap.roots",
    "security.audit",
    "security.authentication_provider",
    "security.authorization_default",
    "security.authority_selection",
    "security.bootstrap_password",
    "security.encryption_key_admission",
    "security.principal_role_group_seed",
    "security.protected_material",
    "security.redaction",
    "security.user_home_schema",
    "sequence.generator_cache",
    "server.route_listener_startup",
    "session.disconnect_timeout",
    "storage.allocation_freespace_pagemap",
    "storage.filespace_lifecycle",
    "storage.filespace_profile",
    "support.bundle",
    "temp.spill_workspace",
    "transaction.admission",
    "transaction.commit_durability",
    "transaction.default_isolation_snapshot",
    "transaction.mga_gc_retention",
    "transaction.rollback_savepoint_limbo",
    "udr.extension_trust_resource",
    "upgrade.migration_refusal",
    "workload.resource_quota",
}

REQUIRED_AUTHORITY_INVARIANTS = {
    "policy_catalog_is_authority",
    "mga_visibility_required",
    "wal_not_authority",
    "parser_not_authority",
    "reference_not_authority",
    "uuid_order_not_finality",
}

KNOWN_RIGHTS = {
    "CONNECT",
    "SELECT",
    "INSERT",
    "UPDATE",
    "DELETE",
    "SEC_IDENTITY_ADMIN",
    "SEC_MEMBERSHIP_ADMIN",
    "SEC_GRANT_ADMIN",
    "AUDIT_READ",
    "OBS_MANAGEMENT_INSPECT",
}

PRIVATE_REFERENCE_PATTERNS = (
    "docs/" + "execution-plans",
    "docs/" + "completed-execution-plans",
    "docs/" + "findings",
    "/" + "home/",
    "Cli" + "Work",
    "." + "git",
)


def load_json(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"{path}: invalid JSON: {exc}") from exc


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def require_uuid(value: str, field: str) -> None:
    require(bool(UUID_RE.fullmatch(value)), f"{field}: invalid UUID {value!r}")


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def aggregate_digest(entries: list[tuple[str, str]]) -> str:
    digest = hashlib.sha256()
    for rel_path, file_digest in entries:
        digest.update(rel_path.encode("utf-8"))
        digest.update(b"\0")
        digest.update(file_digest.encode("ascii"))
        digest.update(b"\n")
    return digest.hexdigest()


def scan_private_references(pack_root: Path) -> None:
    for path in sorted(pack_root.rglob("*")):
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for pattern in PRIVATE_REFERENCE_PATTERNS:
            require(pattern not in text, f"{path}: private reference pattern {pattern!r}")


def validate_manifest(pack_root: Path) -> dict:
    manifest_path = pack_root / "POLICY_PACK_MANIFEST.json"
    manifest = load_json(manifest_path)
    require(isinstance(manifest, dict), "manifest must be a JSON object")
    require(manifest.get("schema_version") == 1, "manifest schema_version must be 1")
    require(manifest.get("min_supported_schema_version") == 1, "manifest min schema must be 1")
    require(manifest.get("max_supported_schema_version") == 1, "manifest max schema must be 1")
    require_uuid(str(manifest.get("policy_pack_uuid", "")), "policy_pack_uuid")
    require(manifest.get("policy_pack_id") == "default-local-password",
            "default policy pack id mismatch")
    require(manifest.get("content_hash_algorithm") == "sha256",
            "manifest must use sha256 content hashes")
    require(manifest.get("signature_status") == "signature-ready-unsigned",
            "manifest must be signature-ready unsigned")
    provenance = manifest.get("provenance")
    require(isinstance(provenance, dict), "manifest provenance must be an object")
    require(provenance.get("source") == "public-project-tree",
            "manifest provenance source must be public-project-tree")
    require(provenance.get("private_inputs_required") is False,
            "manifest must not require private inputs")
    require(provenance.get("external_provider_runtime_required") is False,
            "manifest must not require external provider runtime")
    require(manifest.get("create_time_only") is True,
            "manifest create_time_only must be true")
    require(manifest.get("post_create_filesystem_authority") is False,
            "manifest must deny post-create filesystem authority")

    provider_policy = manifest.get("default_provider_policy")
    require(isinstance(provider_policy, dict), "default provider policy must be an object")
    require(provider_policy.get("mode") == "local-password-only",
            "default provider mode must be local-password-only")
    require(provider_policy.get("enabled_provider_families") == ["local_password"],
            "only local_password may be enabled by default")
    disabled = set(provider_policy.get("external_provider_families_disabled_by_default", []))
    require(EXTERNAL_PROVIDERS <= disabled,
            "all external provider families must be disabled by default")

    entries: list[tuple[str, str]] = []
    seen_paths: set[str] = set()
    for item in manifest.get("content_manifest", []):
        require(isinstance(item, dict), "content_manifest entries must be objects")
        rel_path = str(item.get("path", ""))
        require(rel_path and rel_path not in seen_paths,
                f"content path missing or duplicate: {rel_path!r}")
        require(not rel_path.startswith("/") and ".." not in Path(rel_path).parts,
                f"content path is not pack-relative safe: {rel_path!r}")
        path = pack_root / rel_path
        require(path.is_file(), f"content file missing: {rel_path}")
        digest = sha256_file(path)
        require(item.get("sha256") == digest,
                f"content hash mismatch for {rel_path}")
        entries.append((rel_path, digest))
        seen_paths.add(rel_path)

    require(entries, "manifest content_manifest must not be empty")
    require("policies/default_policy_catalog.json" in seen_paths,
            "manifest content_manifest must include default policy catalog")
    require("policies/server_memory_cache_policy.json" in seen_paths,
            "manifest content_manifest must include server memory/cache policy")
    require(manifest.get("content_sha256") == aggregate_digest(entries),
            "manifest aggregate content_sha256 mismatch")
    return manifest


def validate_security_providers(pack_root: Path) -> None:
    doc = load_json(pack_root / "policies/security_providers.json")
    providers = doc.get("providers") if isinstance(doc, dict) else None
    require(isinstance(providers, list) and providers, "providers array is required")
    enabled = [p for p in providers if p.get("enabled_by_default") is True]
    require([p.get("provider_family") for p in enabled] == ["local_password"],
            "exactly local_password must be enabled")
    for provider in providers:
        require_uuid(str(provider.get("provider_uuid", "")), "provider_uuid")
        family = provider.get("provider_family")
        require(isinstance(family, str) and family, "provider_family required")
        if family in EXTERNAL_PROVIDERS:
            require(provider.get("enabled_by_default") is False,
                    f"external provider enabled by default: {family}")
            require(provider.get("authority") == "unsupported_by_default",
                    f"external provider must be unsupported_by_default: {family}")


def validate_roles_groups_grants(pack_root: Path) -> None:
    roles_doc = load_json(pack_root / "policies/roles.json")
    groups_doc = load_json(pack_root / "policies/groups.json")
    grants_doc = load_json(pack_root / "policies/grants.json")
    require(roles_doc.get("identity_authority") == "uuid",
            "roles identity authority must be uuid")
    require(groups_doc.get("identity_authority") == "uuid",
            "groups identity authority must be uuid")
    require(grants_doc.get("identity_authority") == "uuid",
            "grants identity authority must be uuid")

    role_uuids = {role["role_uuid"] for role in roles_doc.get("roles", [])}
    group_uuids = {group["group_uuid"] for group in groups_doc.get("groups", [])}
    require(len(role_uuids) >= 5, "standard role set is incomplete")
    require(len(group_uuids) >= 6, "standard group set is incomplete")
    for value in role_uuids:
        require_uuid(value, "role_uuid")
    for value in group_uuids:
        require_uuid(value, "group_uuid")

    subject_uuids = role_uuids | group_uuids
    for membership in groups_doc.get("memberships", []):
        require_uuid(str(membership.get("member_uuid", "")), "membership.member_uuid")
        require_uuid(str(membership.get("parent_uuid", "")), "membership.parent_uuid")
        require(membership.get("member_kind") == "group",
                "default memberships should attach groups")
        require(membership.get("parent_kind") == "role",
                "default memberships should attach to roles")
        require(membership["member_uuid"] in group_uuids,
                "membership member UUID not declared as group")
        require(membership["parent_uuid"] in role_uuids,
                "membership parent UUID not declared as role")

    grants = grants_doc.get("grants", [])
    require(isinstance(grants, list) and grants, "grants array is required")
    rights = set()
    for grant in grants:
        require_uuid(str(grant.get("grant_uuid", "")), "grant_uuid")
        require_uuid(str(grant.get("subject_uuid", "")), "grant.subject_uuid")
        require(grant.get("subject_uuid") in subject_uuids,
                "grant subject UUID is not declared")
        require(grant.get("subject_kind") in {"role", "group"},
                "grant subject kind must be role or group")
        require("subject_name" not in grant and "role_name" not in grant and "group_name" not in grant,
                "grant must not bind authority by name")
        right = grant.get("right")
        require(right in KNOWN_RIGHTS, f"unknown or unsupported default grant right: {right}")
        require(grant.get("effect") in {"allow", "deny"},
                "grant effect must be allow or deny")
        rights.add(right)
    require({"CONNECT", "SEC_GRANT_ADMIN"} <= rights,
            "default grants must include CONNECT and SEC_GRANT_ADMIN")


def validate_policy_profiles(pack_root: Path) -> None:
    profiles_doc = load_json(pack_root / "policies/policy_profiles.json")
    profiles = profiles_doc.get("profiles") if isinstance(profiles_doc, dict) else None
    require(isinstance(profiles, list), "policy profiles array is required")
    seen = set()
    for profile in profiles:
        require_uuid(str(profile.get("profile_uuid", "")), "profile_uuid")
        area = profile.get("area")
        require(isinstance(area, str) and area, "policy profile area required")
        require(area not in seen, f"duplicate policy profile area: {area}")
        require(isinstance(profile.get("mode"), str) and profile.get("mode"),
                f"policy profile mode required for {area}")
        seen.add(area)
    missing = REQUIRED_POLICY_AREAS - seen
    require(not missing, "missing default policy areas: " + ",".join(sorted(missing)))
    memory_profiles = [p for p in profiles if p.get("area") == "memory_resource_governance"]
    require(memory_profiles and memory_profiles[0].get("resource_file") ==
            "policies/server_memory_cache_policy.json",
            "memory_resource_governance must point at server memory/cache policy")


def validate_server_memory_cache_policy(pack_root: Path) -> None:
    doc = load_json(pack_root / "policies/server_memory_cache_policy.json")
    require(isinstance(doc, dict), "server memory/cache policy must be an object")
    require(doc.get("schema_version") == 1,
            "server memory/cache policy schema_version must be 1")
    require(doc.get("policy_generation") == 1,
            "server memory/cache policy generation must be 1")
    require(doc.get("profile_area") == "memory_resource_governance",
            "server memory/cache policy profile area mismatch")
    require(doc.get("policy_name") == "default_local_server_memory_cache_v1",
            "server memory/cache policy name mismatch")
    limits = doc.get("limits")
    require(isinstance(limits, dict), "server memory/cache policy limits required")
    for field in (
        "hard_limit_bytes",
        "soft_limit_bytes",
        "per_context_limit_bytes",
        "page_buffer_pool_limit_bytes",
        "min_startup_available_bytes",
    ):
        require(isinstance(limits.get(field), int),
                f"server memory/cache {field} must be an integer")
    require(limits.get("hard_limit_bytes") >= REQUIRED_MEMORY_HARD_LIMIT_BYTES,
            "server memory/cache hard limit must be at least 1 GiB")
    require(limits.get("min_startup_available_bytes") >= REQUIRED_MEMORY_HARD_LIMIT_BYTES,
            "server memory/cache startup floor must be at least 1 GiB")
    require(limits.get("page_buffer_pool_limit_bytes") >=
            REQUIRED_MEMORY_PAGE_BUFFER_POOL_BYTES,
            "server memory/cache page-buffer pool must be at least 512 MiB")
    require(limits.get("soft_limit_bytes") <= limits.get("hard_limit_bytes"),
            "server memory/cache soft limit exceeds hard limit")
    require(limits.get("per_context_limit_bytes") <= limits.get("hard_limit_bytes"),
            "server memory/cache per-context limit exceeds hard limit")
    require(limits.get("page_buffer_pool_limit_bytes") <= limits.get("hard_limit_bytes"),
            "server memory/cache page-buffer pool exceeds hard limit")
    allocation = doc.get("allocation")
    require(isinstance(allocation, dict), "server memory/cache allocation required")
    require(allocation.get("failure_mode") == "return_error",
            "server memory/cache failure mode must return errors")
    require(allocation.get("track_allocations") is True,
            "server memory/cache must track allocations")
    adaptive = doc.get("adaptive_cache")
    require(isinstance(adaptive, dict), "server memory/cache adaptive cache required")
    require(adaptive.get("enabled") is True,
            "server memory/cache adaptive cache must be enabled")
    require(adaptive.get("index_read_optimization") is True,
            "server memory/cache must enable index read optimization")
    require(adaptive.get("ordinary_disconnect_heap_trim") is False,
            "server memory/cache must not trim heap on ordinary disconnect")
    require(adaptive.get("dirty_writeback_required") is True,
            "server memory/cache must require dirty writeback")
    require(adaptive.get("cache_finality_authority") is False,
            "server memory/cache must not be transaction finality authority")
    require(adaptive.get("cache_visibility_authority") is False,
            "server memory/cache must not be visibility authority")
    require(adaptive.get("wal_or_redo_authority") is False,
            "server memory/cache must not introduce WAL/redo authority")


def validate_default_policy_catalog(pack_root: Path) -> None:
    doc = load_json(pack_root / "policies/default_policy_catalog.json")
    require(isinstance(doc, dict), "default policy catalog must be an object")
    require(doc.get("schema_version") == 1, "default policy catalog schema_version must be 1")
    require(doc.get("policy_generation") == 1, "default policy catalog generation must be 1")
    require(doc.get("identity_authority") == "uuid", "default policy identity authority must be uuid")
    require(doc.get("catalog_authority") == "durable_catalog_after_create",
            "default policy catalog authority must be durable catalog after create")
    require(doc.get("create_time_only") is True,
            "default policy catalog must be create-time only")
    require(doc.get("post_create_filesystem_authority") is False,
            "default policy catalog must reject post-create filesystem authority")
    require(doc.get("default_policy_count") == REQUIRED_DEFAULT_POLICY_COUNT,
            "default policy count mismatch")
    policies = doc.get("policies")
    require(isinstance(policies, list), "default policy policies array is required")
    require(len(policies) == REQUIRED_DEFAULT_POLICY_COUNT,
            "default policy array length mismatch")
    seen: set[str] = set()
    for policy in policies:
        require(isinstance(policy, dict), "default policy entries must be objects")
        key = policy.get("policy_key")
        require(isinstance(key, str) and key, "default policy key required")
        require(key not in seen, f"duplicate default policy key: {key}")
        seen.add(key)
        require(isinstance(policy.get("default_profile"), str) and policy["default_profile"],
                f"{key}: default profile required")
        require(policy.get("state") in {"enabled", "fail_closed"},
                f"{key}: invalid state")
        require(policy.get("override_class") in {
            "no_override",
            "create_database_only",
            "security_admin",
            "sysarch",
            "policy_defined",
            "cluster_only",
        }, f"{key}: invalid override class")
        required_properties = policy.get("required_properties")
        require(isinstance(required_properties, list) and required_properties,
                f"{key}: required_properties must be non-empty")
        require(all(isinstance(item, str) and item for item in required_properties),
                f"{key}: required_properties entries must be strings")
        tx1_seed = policy.get("tx1_seed")
        require(isinstance(tx1_seed, dict), f"{key}: tx1_seed required")
        require(tx1_seed.get("required") is True, f"{key}: tx1_seed.required must be true")
        require(tx1_seed.get("policy_generation") == 1,
                f"{key}: tx1_seed policy generation must be 1")
        require(tx1_seed.get("created_txn") == "tx1",
                f"{key}: tx1_seed created_txn must be tx1")
        require(tx1_seed.get("uuid_source") == "fresh_uuidv7",
                f"{key}: tx1_seed uuid_source must be fresh_uuidv7")
        invariants = set(policy.get("authority_invariants") or [])
        require(REQUIRED_AUTHORITY_INVARIANTS <= invariants,
                f"{key}: authority invariants incomplete")
    missing = REQUIRED_DEFAULT_POLICY_KEYS - seen
    extra = seen - REQUIRED_DEFAULT_POLICY_KEYS
    require(not missing, "missing default policies: " + ",".join(sorted(missing)))
    require(not extra, "unexpected default policies: " + ",".join(sorted(extra)))


def validate_catalog_materialization(pack_root: Path, manifest: dict) -> None:
    rel_path = manifest.get("catalog_materialization_metadata")
    require(isinstance(rel_path, str) and rel_path,
            "catalog materialization metadata path is required")
    metadata = load_json(pack_root / rel_path)
    require(metadata.get("create_time_only") is True,
            "catalog metadata must be create-time only")
    require(metadata.get("materialize_inside_create_transaction") is True,
            "catalog metadata must require create transaction materialization")
    require(metadata.get("post_create_filesystem_authority") is False,
            "catalog metadata must reject post-create filesystem authority")
    require(metadata.get("requires_mga_catalog_commit") is True,
            "catalog metadata must require MGA catalog commit")
    require(metadata.get("catalog_identity_authority") == "uuid",
            "catalog identity authority must be uuid")
    families = set(metadata.get("catalog_row_families", []))
    require(REQUIRED_CATALOG_ROW_FAMILIES <= families,
            "catalog materialization row families incomplete")


def validate_database_lifecycle_descriptor(project_root: Path, manifest: dict) -> None:
    header = project_root / "src/storage/database/database_lifecycle.hpp"
    source = project_root / "src/storage/database/database_lifecycle.cpp"
    header_text = header.read_text(encoding="utf-8")
    source_text = source.read_text(encoding="utf-8")
    require("PolicySeedPackDescriptor" in header_text,
            "database lifecycle must expose PolicySeedPackDescriptor")
    require("DefaultPolicyPackDescriptor" in header_text,
            "database lifecycle must expose DefaultPolicyPackDescriptor")
    require("DEFAULT_POLICY_PACK_IMPORT" in source_text,
            "database lifecycle source must carry DEFAULT_POLICY_PACK_IMPORT search key")
    require(str(manifest["policy_pack_id"]) in source_text,
            "database lifecycle descriptor pack id does not match manifest")
    require(str(manifest["policy_pack_uuid"]) in source_text,
            "database lifecycle descriptor UUID does not match manifest")
    require(str(manifest["policy_pack_version"]) in source_text,
            "database lifecycle descriptor version does not match manifest")
    require(str(manifest["content_sha256"]) in source_text,
            "database lifecycle descriptor content hash does not match manifest")
    require("post_create_filesystem_authority = false" in source_text,
            "database lifecycle descriptor must reject post-create filesystem authority")
    require("local_password_only = true" in source_text,
            "database lifecycle descriptor must record local-password-only default")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True)
    args = parser.parse_args(argv)
    project_root = Path(args.project_root).resolve()
    pack_root = project_root / "resources/policy-packs/default-local-password"
    require(pack_root.is_dir(), f"default policy pack directory missing: {pack_root}")
    scan_private_references(pack_root)
    manifest = validate_manifest(pack_root)
    validate_security_providers(pack_root)
    validate_roles_groups_grants(pack_root)
    validate_policy_profiles(pack_root)
    validate_server_memory_cache_policy(pack_root)
    validate_default_policy_catalog(pack_root)
    validate_catalog_materialization(pack_root, manifest)
    validate_database_lifecycle_descriptor(project_root, manifest)
    print("public_policy_pack_manifest_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
