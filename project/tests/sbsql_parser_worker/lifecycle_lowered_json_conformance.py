#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Strict framing of actual lowered lifecycle payloads, without executing them."""
import json
import subprocess
import sys


def main():
    result = subprocess.run([sys.argv[1], "--emit-lowered-json"], capture_output=True,
                            text=True, timeout=60, check=True)
    payloads = [line.removeprefix("lowered_payload=") for line in result.stdout.splitlines()
                if line.startswith("lowered_payload=")]
    if not payloads:
        raise AssertionError("the real lifecycle lowerer emitted no payloads")
    operations = set()
    for payload in payloads:
        document = json.loads(payload)
        if not isinstance(document, dict):
            raise AssertionError("lowered envelope is not a JSON object")
        for field in ("descriptor_requirements", "policy_refs", "required_rights",
                      "required_authority_steps"):
            if not isinstance(document[field], list) or not all(
                    isinstance(item, str) for item in document[field]):
                raise AssertionError(f"invalid authority list: {field}")
        operations.add(document["operation_id"])
    required = {"lifecycle.create_database", "lifecycle.inspect_database",
                "lifecycle.verify_database", "lifecycle.shutdown_database"}
    if not required <= operations:
        raise AssertionError(f"missing lowered lifecycle operations: {required - operations}")
    print(f"lifecycle_lowered_json_conformance=passed payloads={len(payloads)}")


if __name__ == "__main__":
    main()
