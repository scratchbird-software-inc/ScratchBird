#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""PCF-041 Kubernetes operator public single-node lifecycle gate."""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import sys


OPERATOR_GATE = "project/cloud/kubernetes/tools/operator_gate.py"


def load_operator_gate(repo_root: Path):
    gate_path = repo_root / OPERATOR_GATE
    spec = importlib.util.spec_from_file_location("scratchbird_operator_gate", gate_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load operator gate from {gate_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    gate = load_operator_gate(repo_root)

    try:
        contract, schemas = gate.build_context(repo_root)
        result = gate.run_gate(repo_root)

        sample = repo_root / "project/cloud/kubernetes/manifests/public-single-node/sample-stack.yaml"
        resources = gate.load_yaml_documents(sample)
        database = next(resource for resource in resources if resource["kind"] == "ScratchBirdDatabase")
        plan = gate.dry_run_plan(database, schemas, contract)
        reconcile_a = gate.reconcile_resource(database, schemas, contract)
        reconcile_b = gate.reconcile_resource(database, schemas, contract)

        require(plan["accepted"], "valid database dry-run was rejected")
        require(reconcile_a == reconcile_b, "reconcile output is not deterministic")
        require(plan["plannedOperations"][0]["authorityPath"] == "scratchbird_manager_server_engine",
                "operator did not route lifecycle through manager/server/engine authority")
        require(plan["plannedOperations"][0]["directDatabaseFileMutation"] is False,
                "operator plan allowed direct database file mutation")
        require(plan["status"]["evidenceRefs"][0]["finality"] == "not_transaction_finality",
                "operator status claimed transaction finality")

        cluster_fixture = repo_root / contract["fixtures"]["negative"]["clusterField"]
        cluster_resource = gate.first_document(cluster_fixture)
        cluster_plan = gate.dry_run_plan(cluster_resource, schemas, contract)
        require(not cluster_plan["accepted"], "cluster-field resource was accepted")
        require(cluster_plan["diagnostics"][0]["code"] == "SB-K8S-CLUSTER-FIELD-REFUSED",
                "cluster-field refusal diagnostic mismatch")
        require(cluster_plan["plannedOperations"] == [], "cluster-field refusal planned a lifecycle action")

        shutdown_fixture = repo_root / contract["fixtures"]["negative"]["shutdownRefused"]
        shutdown_resource = gate.first_document(shutdown_fixture)
        shutdown_plan = gate.dry_run_plan(shutdown_resource, schemas, contract)
        require(not shutdown_plan["accepted"], "shutdown without maintenance window was accepted")
        require(shutdown_plan["diagnostics"][0]["code"] == "SB-K8S-SHUTDOWN-REFUSED",
                "shutdown refusal diagnostic mismatch")

    except Exception as exc:
        print(f"KUBERNETES_OPERATOR_LIFECYCLE_GATE=failed: {exc}", file=sys.stderr)
        return 1

    print("KUBERNETES_OPERATOR_LIFECYCLE_GATE=passed")
    print("PCF-041_PUBLIC_SINGLE_NODE_CRDS=passed")
    print("PCF-041_DRY_RUN_RECONCILE_IDEMPOTENCY=passed")
    print("PCF-041_CLUSTER_FIELD_REFUSAL=passed")
    print("PCF-041_STATUS_FINALITY_BOUNDARY=passed")
    print("PCF-041_EVIDENCE_REFS=" + ",".join(result["evidenceRefs"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
