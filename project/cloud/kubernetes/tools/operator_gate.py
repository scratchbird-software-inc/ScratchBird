#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""ScratchBird public single-node Kubernetes operator dry-run gate."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any

import yaml


API_VERSION = "cloud.scratchbird.io/v1alpha1"
CONTRACT_PATH = Path("project/cloud/kubernetes/contracts/public-single-node-operator-contract.yaml")
CRD_BUNDLE_PATH = Path("project/cloud/kubernetes/crds/public-single-node-crds.yaml")
PACKAGE_MANIFEST_PATH = Path("project/cloud/kubernetes/package-manifest.yaml")


class GateError(Exception):
    """Raised when a gate condition fails."""


def load_yaml_documents(path: Path) -> list[dict[str, Any]]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            docs = [doc for doc in yaml.safe_load_all(handle) if doc is not None]
    except FileNotFoundError as exc:
        raise GateError(f"missing YAML file: {path}") from exc
    except yaml.YAMLError as exc:
        raise GateError(f"invalid YAML in {path}: {exc}") from exc
    for index, doc in enumerate(docs):
        if not isinstance(doc, dict):
            raise GateError(f"{path}: document {index + 1} is not a mapping")
    return docs


def load_single_yaml(path: Path) -> dict[str, Any]:
    docs = load_yaml_documents(path)
    if len(docs) != 1:
        raise GateError(f"{path}: expected exactly one YAML document")
    return docs[0]


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def stable_id(prefix: str, *values: Any) -> str:
    digest = hashlib.sha256()
    for value in values:
        digest.update(canonical_json(value).encode("utf-8"))
        digest.update(b"\n")
    return f"{prefix}-{digest.hexdigest()[:24]}"


def json_path(path: tuple[str, ...]) -> str:
    return ".".join(path) if path else "$"


def load_contract(repo_root: Path) -> dict[str, Any]:
    contract = load_single_yaml(repo_root / CONTRACT_PATH)
    if contract.get("profile") != "public-single-node":
        raise GateError("operator contract profile must be public-single-node")
    return contract


def load_crd_schemas(repo_root: Path) -> dict[str, dict[str, Any]]:
    schemas: dict[str, dict[str, Any]] = {}
    for crd in load_yaml_documents(repo_root / CRD_BUNDLE_PATH):
        if crd.get("apiVersion") != "apiextensions.k8s.io/v1":
            raise GateError("CRD bundle contains non-v1 CRD document")
        if crd.get("kind") != "CustomResourceDefinition":
            raise GateError("CRD bundle contains non-CRD document")
        spec = crd.get("spec", {})
        names = spec.get("names", {})
        kind = names.get("kind")
        if not isinstance(kind, str):
            raise GateError("CRD document missing spec.names.kind")
        versions = spec.get("versions", [])
        if len(versions) != 1:
            raise GateError(f"{kind}: expected one served storage version")
        version = versions[0]
        if version.get("name") != "v1alpha1" or not version.get("served") or not version.get("storage"):
            raise GateError(f"{kind}: v1alpha1 must be served and storage")
        schema = version.get("schema", {}).get("openAPIV3Schema")
        if not isinstance(schema, dict):
            raise GateError(f"{kind}: missing openAPIV3Schema")
        schemas[kind] = schema
    return schemas


def iter_mapping_paths(value: Any, path: tuple[str, ...] = ()) -> list[tuple[tuple[str, ...], Any]]:
    found: list[tuple[tuple[str, ...], Any]] = []
    if isinstance(value, dict):
        for key, child in value.items():
            key_path = path + (str(key),)
            found.append((key_path, child))
            found.extend(iter_mapping_paths(child, key_path))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            found.extend(iter_mapping_paths(child, path + (str(index),)))
    return found


def retryability_for(code: str) -> str:
    if code == "SB-K8S-CRD-INVALID":
        return "retry_after_manifest_fix"
    if code == "SB-K8S-RECONCILE-CONFLICT":
        return "retry_with_original_idempotency_key_or_new_key"
    if code == "SB-K8S-PREREQUISITE-MISSING":
        return "retry_after_prerequisite_ready"
    return "not_retryable_without_policy_change"


def make_diagnostic(contract: dict[str, Any], code: str, path: tuple[str, ...], message: str) -> dict[str, Any]:
    return {
        "code": code,
        "fieldPath": json_path(path),
        "message": message,
        "retryability": retryability_for(code),
        "auditRequired": True,
        "finality": "no_lifecycle_action",
    }


def private_cluster_diagnostic(resource: dict[str, Any], contract: dict[str, Any]) -> dict[str, Any] | None:
    forbidden_kinds = set(contract["forbiddenKinds"])
    if resource.get("kind") in forbidden_kinds:
        return make_diagnostic(
            contract,
            contract["diagnostics"]["clusterFieldRefused"],
            ("kind",),
            f"{resource.get('kind')} is private cluster scope and is refused by the public single-node profile",
        )
    forbidden_fields = set(contract["forbiddenPrivateClusterFields"])
    for path, _value in iter_mapping_paths(resource):
        if path and path[-1] in forbidden_fields:
            return make_diagnostic(
                contract,
                contract["diagnostics"]["clusterFieldRefused"],
                path,
                f"{path[-1]} is private cluster scope and is refused by the public single-node profile",
            )
    return None


def schema_type_matches(value: Any, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    return True


def validate_schema_value(value: Any, schema: dict[str, Any], path: tuple[str, ...], contract: dict[str, Any]) -> list[dict[str, Any]]:
    diagnostics: list[dict[str, Any]] = []
    expected_type = schema.get("type")
    if isinstance(expected_type, str) and not schema_type_matches(value, expected_type):
        diagnostics.append(
            make_diagnostic(contract, contract["diagnostics"]["invalidCrd"], path, f"expected {expected_type}")
        )
        return diagnostics

    if "enum" in schema and value not in schema["enum"]:
        diagnostics.append(
            make_diagnostic(contract, contract["diagnostics"]["invalidCrd"], path, f"value {value!r} is not allowed")
        )
        return diagnostics

    if isinstance(value, dict):
        properties = schema.get("properties", {})
        required = schema.get("required", [])
        for key in required:
            if key not in value:
                diagnostics.append(
                    make_diagnostic(contract, contract["diagnostics"]["invalidCrd"], path + (key,), "required field missing")
                )
        if schema.get("additionalProperties") is False:
            for key in value:
                if key not in properties:
                    diagnostics.append(
                        make_diagnostic(contract, contract["diagnostics"]["invalidCrd"], path + (str(key),), "unknown field")
                    )
        for key, child_schema in properties.items():
            if key in value and isinstance(child_schema, dict):
                diagnostics.extend(validate_schema_value(value[key], child_schema, path + (key,), contract))
    elif isinstance(value, list):
        item_schema = schema.get("items")
        if isinstance(item_schema, dict):
            for index, item in enumerate(value):
                diagnostics.extend(validate_schema_value(item, item_schema, path + (str(index),), contract))
    return diagnostics


def validate_resource_document(
    resource: dict[str, Any], schemas: dict[str, dict[str, Any]], contract: dict[str, Any]
) -> list[dict[str, Any]]:
    diagnostic = private_cluster_diagnostic(resource, contract)
    if diagnostic is not None:
        return [diagnostic]

    kind = resource.get("kind")
    if kind not in schemas:
        return [
            make_diagnostic(
                contract,
                contract["diagnostics"]["invalidCrd"],
                ("kind",),
                f"{kind!r} is not a public single-node ScratchBird CRD kind",
            )
        ]
    if resource.get("apiVersion") != API_VERSION:
        return [
            make_diagnostic(
                contract,
                contract["diagnostics"]["invalidCrd"],
                ("apiVersion",),
                f"apiVersion must be {API_VERSION}",
            )
        ]
    diagnostics = validate_schema_value(resource, schemas[kind], (), contract)
    spec = resource.get("spec", {})
    if isinstance(spec, dict):
        for field in contract["commonRequiredSpecFields"]:
            if field not in spec:
                diagnostics.append(
                    make_diagnostic(contract, contract["diagnostics"]["invalidCrd"], ("spec", field), "required public field missing")
                )
        if kind == "ScratchBirdDatabase" and spec.get("lifecycleIntent") == "shutdown" and not spec.get("maintenanceWindow"):
            diagnostics.append(
                make_diagnostic(
                    contract,
                    contract["diagnostics"]["shutdownRefused"],
                    ("spec", "maintenanceWindow"),
                    "shutdown requires maintenance window evidence",
                )
            )
    return diagnostics


def operation_for(resource: dict[str, Any], contract: dict[str, Any]) -> str:
    kind = resource["kind"]
    mapping = contract["lifecycleOperations"][kind]
    if kind == "ScratchBirdDatabase":
        lifecycle_intent = resource["spec"].get("lifecycleIntent", "create_or_open")
        return mapping[lifecycle_intent]
    return mapping["default"]


def resource_identity(resource: dict[str, Any]) -> dict[str, str]:
    metadata = resource.get("metadata", {})
    return {
        "apiVersion": resource.get("apiVersion", ""),
        "kind": resource.get("kind", ""),
        "namespace": metadata.get("namespace", "default"),
        "name": metadata.get("name", ""),
    }


def dry_run_plan(resource: dict[str, Any], schemas: dict[str, dict[str, Any]], contract: dict[str, Any]) -> dict[str, Any]:
    diagnostics = validate_resource_document(resource, schemas, contract)
    if diagnostics:
        return {
            "accepted": False,
            "diagnostics": diagnostics,
            "plannedOperations": [],
            "status": refusal_status(resource, diagnostics),
        }

    operation = operation_for(resource, contract)
    identity = resource_identity(resource)
    evidence_id = stable_id("sb-k8s-evidence", identity, resource.get("spec", {}), operation)
    operation_id = stable_id("sb-k8s-operation", identity, resource["spec"].get("idempotencyKey"), operation)
    return {
        "accepted": True,
        "resource": identity,
        "plannedOperations": [
            {
                "operation": operation,
                "operationId": operation_id,
                "authorityPath": "scratchbird_manager_server_engine",
                "directDatabaseFileMutation": False,
                "idempotencyKey": resource["spec"]["idempotencyKey"],
                "auditReason": resource["spec"]["auditReason"],
                "redactionPolicyRef": resource["spec"]["redactionPolicyRef"],
                "evidenceRef": evidence_id,
            }
        ],
        "diagnostics": [],
        "status": accepted_status(resource, evidence_id, operation),
    }


def observed_generation(resource: dict[str, Any]) -> int:
    generation = resource.get("metadata", {}).get("generation", 1)
    return generation if isinstance(generation, int) else 1


def accepted_status(resource: dict[str, Any], evidence_id: str, operation: str) -> dict[str, Any]:
    generation = observed_generation(resource)
    return {
        "observedGeneration": generation,
        "scratchbirdState": "lifecycle_plan_accepted",
        "conditions": [
            {
                "type": "Validated",
                "status": "True",
                "reason": "PublicSingleNodeDryRunAccepted",
                "observedGeneration": generation,
            },
            {
                "type": "ManagerAuthorityRequired",
                "status": "True",
                "reason": "EngineLifecycleAuthorityPreserved",
                "observedGeneration": generation,
            },
        ],
        "diagnostics": [],
        "evidenceRefs": [
            {
                "id": evidence_id,
                "kind": "operator_dry_run_reconcile_plan",
                "operation": operation,
                "finality": "not_transaction_finality",
            }
        ],
    }


def refusal_status(resource: dict[str, Any], diagnostics: list[dict[str, Any]]) -> dict[str, Any]:
    generation = observed_generation(resource)
    return {
        "observedGeneration": generation,
        "scratchbirdState": "blocked",
        "conditions": [
            {
                "type": "Validated",
                "status": "False",
                "reason": diagnostics[0]["code"],
                "observedGeneration": generation,
            }
        ],
        "diagnostics": diagnostics,
        "evidenceRefs": [],
    }


def reconcile_resource(resource: dict[str, Any], schemas: dict[str, dict[str, Any]], contract: dict[str, Any]) -> dict[str, Any]:
    plan = dry_run_plan(resource, schemas, contract)
    plan["reconcileHash"] = stable_id("sb-k8s-reconcile", plan.get("resource", resource_identity(resource)), plan["status"])
    plan["idempotent"] = True
    return plan


def validate_contract(repo_root: Path, contract: dict[str, Any], schemas: dict[str, dict[str, Any]]) -> None:
    required = set(contract["requiredKinds"])
    actual = set(schemas)
    if actual != required:
        raise GateError(f"public CRD set mismatch: expected {sorted(required)}, found {sorted(actual)}")
    forbidden = set(contract["forbiddenKinds"])
    if actual & forbidden:
        raise GateError(f"public CRD bundle includes private cluster kinds: {sorted(actual & forbidden)}")
    for kind, schema in schemas.items():
        spec_schema = schema.get("properties", {}).get("spec", {})
        if spec_schema.get("additionalProperties") is not False:
            raise GateError(f"{kind}: spec must reject unknown fields")
        forbidden_diag = private_cluster_diagnostic({"apiVersion": API_VERSION, "kind": kind, "spec": spec_schema}, contract)
        if forbidden_diag is not None:
            raise GateError(f"{kind}: schema exposes private field {forbidden_diag['fieldPath']}")
    package = load_single_yaml(repo_root / PACKAGE_MANIFEST_PATH)
    for artifact in package.get("artifacts", []):
        rel = artifact.get("path")
        if not rel or not (repo_root / rel).exists():
            raise GateError(f"package manifest missing artifact: {rel}")


def first_document(path: Path) -> dict[str, Any]:
    docs = load_yaml_documents(path)
    if not docs:
        raise GateError(f"{path}: no resource documents found")
    return docs[0]


def validate_resource_file(repo_root: Path, path: Path, schemas: dict[str, dict[str, Any]], contract: dict[str, Any]) -> list[dict[str, Any]]:
    diagnostics: list[dict[str, Any]] = []
    for resource in load_yaml_documents(path):
        diagnostics.extend(validate_resource_document(resource, schemas, contract))
    return diagnostics


def run_gate(repo_root: Path) -> dict[str, Any]:
    contract = load_contract(repo_root)
    schemas = load_crd_schemas(repo_root)
    validate_contract(repo_root, contract, schemas)

    evidence: list[str] = []
    for rel in contract["fixtures"]["positive"]:
        path = repo_root / rel
        diagnostics = validate_resource_file(repo_root, path, schemas, contract)
        if diagnostics:
            raise GateError(f"{rel}: expected accepted resources, got {diagnostics}")
        for resource in load_yaml_documents(path):
            plan = dry_run_plan(resource, schemas, contract)
            reconcile_a = reconcile_resource(resource, schemas, contract)
            reconcile_b = reconcile_resource(resource, schemas, contract)
            if reconcile_a != reconcile_b:
                raise GateError(f"{rel}: reconcile is not deterministic for {resource.get('kind')}")
            if not plan["accepted"]:
                raise GateError(f"{rel}: dry-run was not accepted for {resource.get('kind')}")
            status = plan["status"]
            if status["evidenceRefs"][0]["finality"] != contract["statusFinality"]:
                raise GateError(f"{rel}: status finality is not orchestration-only evidence")
            evidence.append(status["evidenceRefs"][0]["id"])

    cluster_negative = repo_root / contract["fixtures"]["negative"]["clusterField"]
    cluster_diag = validate_resource_file(repo_root, cluster_negative, schemas, contract)
    if len(cluster_diag) != 1 or cluster_diag[0]["code"] != contract["diagnostics"]["clusterFieldRefused"]:
        raise GateError("cluster field refusal diagnostic mismatch")

    shutdown_negative = repo_root / contract["fixtures"]["negative"]["shutdownRefused"]
    shutdown_diag = validate_resource_file(repo_root, shutdown_negative, schemas, contract)
    if len(shutdown_diag) != 1 or shutdown_diag[0]["code"] != contract["diagnostics"]["shutdownRefused"]:
        raise GateError("shutdown refusal diagnostic mismatch")

    return {
        "gate": "kubernetes_operator_lifecycle_gate",
        "status": "passed",
        "evidenceRefs": evidence,
        "requiredKinds": sorted(schemas),
    }


def build_context(repo_root: Path) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    contract = load_contract(repo_root)
    schemas = load_crd_schemas(repo_root)
    return contract, schemas


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["contract", "validate", "dry-run", "reconcile", "gate"])
    parser.add_argument("--repo-root", default=".", type=Path)
    parser.add_argument("--resource", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    try:
        contract, schemas = build_context(repo_root)
        if args.mode == "contract":
            validate_contract(repo_root, contract, schemas)
            print(json.dumps({"contract": "passed", "requiredKinds": sorted(schemas)}, indent=2, sort_keys=True))
            return 0
        if args.mode == "gate":
            print(json.dumps(run_gate(repo_root), indent=2, sort_keys=True))
            return 0
        if args.resource is None:
            raise GateError(f"{args.mode} requires --resource")
        resource_path = args.resource if args.resource.is_absolute() else repo_root / args.resource
        resource = first_document(resource_path)
        if args.mode == "validate":
            diagnostics = validate_resource_document(resource, schemas, contract)
            print(json.dumps({"accepted": not diagnostics, "diagnostics": diagnostics}, indent=2, sort_keys=True))
            return 1 if diagnostics else 0
        if args.mode == "dry-run":
            print(json.dumps(dry_run_plan(resource, schemas, contract), indent=2, sort_keys=True))
            return 0
        if args.mode == "reconcile":
            print(json.dumps(reconcile_resource(resource, schemas, contract), indent=2, sort_keys=True))
            return 0
    except GateError as exc:
        print(f"KUBERNETES_OPERATOR_LIFECYCLE_GATE=failed: {exc}", file=sys.stderr)
        return 1
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
