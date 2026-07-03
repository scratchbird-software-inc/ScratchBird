#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate the public default-policy settings resource and docs.

The default policy catalog is the create-time authority for policy family rows.
This generator expands that catalog into a resource that documents the default
settings carried by each policy and updates the policy-pack manifest hashes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


POLICY_PACK_REL = Path("project/resources/policy-packs/default-local-password")
CATALOG_REL = POLICY_PACK_REL / "policies/default_policy_catalog.json"
DEFAULTS_REL = POLICY_PACK_REL / "policies/policy_defaults.json"
MANIFEST_REL = POLICY_PACK_REL / "POLICY_PACK_MANIFEST.json"
PUBLIC_DOC_REL = Path("docs/policies/default-policy-pack.md")
PACKAGE_DOC_REL = Path("packaging/2026.07.03/docs/default-policy-pack.md")

BASE_CONTENT_PATHS = [
    "policies/security_providers.json",
    "policies/roles.json",
    "policies/groups.json",
    "policies/grants.json",
    "policies/policy_profiles.json",
    "policies/server_memory_cache_policy.json",
    "policies/default_policy_catalog.json",
    "policies/policy_defaults.json",
    "catalog_materialization.json",
]

DOMAIN_USED_BY = {
    "admin": [
        "administration command admission",
        "SBadm/SBmgr management routes",
        "policy mutation authorization",
    ],
    "backup": ["backup service", "archive/restore admission", "database lifecycle"],
    "cache": ["page cache manager", "checkpoint preload/flush agents", "open database policy image"],
    "capability": ["feature gate manager", "database open compatibility checks", "release profile gates"],
    "cluster": ["cluster boundary stub", "listener route negotiation", "unsupported-feature gate"],
    "concurrency": ["lock manager", "deadlock detector", "transaction admission"],
    "configuration": ["server configuration loader", "policy reload path", "durable catalog policy image"],
    "database": ["database lifecycle create/open", "catalog bootstrap", "identity reconciliation"],
    "diagnostics": ["diagnostic message renderer", "error redaction", "client result envelopes"],
    "event": ["event queue", "notification dispatcher", "transaction commit hooks"],
    "evidence": ["audit evidence retention", "support bundle collector", "release proof gates"],
    "executable": ["procedural object executor", "UDR admission", "side-effect guard"],
    "ipc": ["IPC frame validator", "parser/server transport", "backpressure controller"],
    "job": ["scheduler agent", "job catalog admission", "post-open activation"],
    "lifecycle": ["database lifecycle manager", "maintenance fences", "shutdown handling"],
    "listener": ["listener bind path", "TLS/session pool", "route admission"],
    "observability": ["metrics writer", "operational log policy", "diagnostic reporting"],
    "parser": ["parser package admission", "SBParser route", "dynamic SBsql lowering boundary"],
    "policy": ["policy-pack loader", "policy catalog cache", "policy mutation gate"],
    "reference": ["reference-emulation boundary", "compatibility parser admission", "unsupported-feature gate"],
    "replication": ["CDC/changefeed boundary", "cluster-only feature gate", "fail-closed route policy"],
    "resource": ["resource seed-pack loader", "i18n resource activation", "create database bootstrap"],
    "schema": ["catalog root bootstrap", "name/UUID resolver", "schema visibility filters"],
    "security": ["authentication provider", "authorization cache", "security catalog and audit"],
    "sequence": ["sequence generator", "transaction commit hooks", "cache invalidation"],
    "server": ["server route startup", "listener/manager bootstrap", "configuration policy"],
    "session": ["session manager", "disconnect handling", "transaction cleanup"],
    "storage": ["filespace manager", "page allocator", "free-space/page-map agent"],
    "support": ["support bundle collector", "sysarch authorization", "redaction policy"],
    "temp": ["temporary workspace manager", "spill cleanup agent", "resource quota checks"],
    "transaction": ["MGA transaction manager", "commit/rollback path", "visibility horizon"],
    "udr": ["UDR loader", "extension trust policy", "dynamic SBsql parser UDR"],
    "upgrade": ["open compatibility classifier", "migration refusal gate", "catalog version policy"],
    "workload": ["resource quota agent", "admission control", "memory/cache policy"],
}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=False) + "\n", encoding="utf-8")


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


def policy_domain(policy_key: str) -> str:
    return policy_key.split(".", 1)[0]


def infer_default_value(policy: dict[str, Any], property_name: str) -> Any:
    if "=" in property_name:
        _, raw_value = property_name.split("=", 1)
        if raw_value == "true":
            return True
        if raw_value == "false":
            return False
        return raw_value
    if property_name.endswith("_disabled"):
        return True
    if property_name in {
        "cleartext_password_storage",
        "default_password_allowed",
        "post_create_filesystem_authority",
    }:
        return False
    if policy["state"] == "fail_closed":
        return "fail_closed"
    return "required_by_default"


def setting_meaning(policy: dict[str, Any], property_name: str) -> str:
    readable = property_name.replace("_", " ").replace("=", " equals ")
    return (
        f"Requires {readable} for the {policy['policy_key']} policy family "
        f"using profile {policy['default_profile']}. The setting is seeded in "
        "tx1 and is enforced from the durable catalog after create."
    )


def setting_row(policy: dict[str, Any], property_name: str) -> dict[str, Any]:
    domain = policy_domain(policy["policy_key"])
    return {
        "setting_key": property_name,
        "default_value": infer_default_value(policy, property_name),
        "meaning": setting_meaning(policy, property_name),
        "used_by": DOMAIN_USED_BY.get(
            domain,
            ["database lifecycle", "policy catalog cache", "authorization/admission gates"],
        ),
    }


def build_defaults_resource(catalog: dict[str, Any]) -> dict[str, Any]:
    policies = []
    for policy in catalog["policies"]:
        settings = [setting_row(policy, item) for item in policy["required_properties"]]
        policies.append(
            {
                "ordinal": policy["ordinal"],
                "policy_key": policy["policy_key"],
                "default_profile": policy["default_profile"],
                "state": policy["state"],
                "override_class": policy["override_class"],
                "default_values": {
                    "policy_generation": catalog["policy_generation"],
                    "tx1_seed_required": policy["tx1_seed"]["required"],
                    "created_txn": policy["tx1_seed"]["created_txn"],
                    "uuid_source": policy["tx1_seed"]["uuid_source"],
                    "post_create_filesystem_authority": catalog["post_create_filesystem_authority"],
                    "catalog_authority": catalog["catalog_authority"],
                },
                "settings": settings,
                "authority_invariants": policy["authority_invariants"],
            }
        )
    return {
        "schema_version": 1,
        "policy_generation": catalog["policy_generation"],
        "policy_pack_id": "default-local-password",
        "source_catalog": "policies/default_policy_catalog.json",
        "create_time_only": catalog["create_time_only"],
        "post_create_filesystem_authority": catalog["post_create_filesystem_authority"],
        "catalog_authority": catalog["catalog_authority"],
        "identity_authority": catalog["identity_authority"],
        "default_policy_count": catalog["default_policy_count"],
        "generated_from": "default_policy_catalog.required_properties",
        "generated_at_utc": "deterministic-release-resource",
        "policies": policies,
    }


def update_manifest(repo_root: Path) -> str:
    pack_root = repo_root / POLICY_PACK_REL
    manifest_path = repo_root / MANIFEST_REL
    manifest = load_json(manifest_path)
    entries = []
    for rel_path in BASE_CONTENT_PATHS:
        digest = sha256_file(pack_root / rel_path)
        entries.append({"path": rel_path, "sha256": digest})
    manifest["content_manifest"] = entries
    manifest["content_sha256"] = aggregate_digest([(row["path"], row["sha256"]) for row in entries])
    write_json(manifest_path, manifest)
    return manifest["content_sha256"]


def docs_for_policy(policy: dict[str, Any]) -> str:
    lines = [
        f"### {policy['ordinal']:03d}. `{policy['policy_key']}`",
        "",
        f"- Default profile: `{policy['default_profile']}`",
        f"- State: `{policy['state']}`",
        f"- Override class: `{policy['override_class']}`",
        f"- Catalog authority: `{policy['default_values']['catalog_authority']}`",
        "- Settings:",
    ]
    for setting in policy["settings"]:
        used_by = "; ".join(setting["used_by"])
        default_value = setting["default_value"]
        if isinstance(default_value, bool):
            default_text = "true" if default_value else "false"
        else:
            default_text = str(default_value)
        lines.append(
            f"  - `{setting['setting_key']}` default `{default_text}`: "
            f"{setting['meaning']} Used by: {used_by}."
        )
    return "\n".join(lines)


def write_docs(repo_root: Path, defaults: dict[str, Any], content_sha256: str) -> None:
    body = [
        "# ScratchBird Default Policy Pack",
        "",
        "This document describes the default policy pack shipped with the beta "
        "release resources. It is operational documentation, not a private "
        "specification.",
        "",
        "The pack is create-time input only. `CREATE DATABASE` validates the "
        "pack manifest, opens every manifest entry, verifies each SHA-256 hash, "
        "loads the policy defaults, and materializes the durable catalog rows "
        "inside the create transaction. After creation, the durable catalog is "
        "the authority; the filesystem pack is not re-read as policy authority.",
        "",
        f"- Policy pack: `{defaults['policy_pack_id']}`",
        f"- Policy generation: `{defaults['policy_generation']}`",
        f"- Default policy count: `{defaults['default_policy_count']}`",
        f"- Content SHA-256: `{content_sha256}`",
        f"- Source catalog: `{defaults['source_catalog']}`",
        f"- Defaults resource: `policies/policy_defaults.json`",
        "",
        "## Policies",
        "",
    ]
    body.extend(docs_for_policy(policy) + "\n" for policy in defaults["policies"])
    text = "\n".join(body).rstrip() + "\n"
    for rel_path in (PUBLIC_DOC_REL, PACKAGE_DOC_REL):
        path = repo_root / rel_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    catalog = load_json(repo_root / CATALOG_REL)
    defaults = build_defaults_resource(catalog)
    write_json(repo_root / DEFAULTS_REL, defaults)
    content_sha256 = update_manifest(repo_root)
    write_docs(repo_root, defaults, content_sha256)
    print(f"policy_defaults_resource=generated:{content_sha256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
