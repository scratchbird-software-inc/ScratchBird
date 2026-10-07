#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC static traceability coverage gate."""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import sys
import tempfile


TRACEABILITY_AUDIT = "project/tools/database_lifecycle/lifecycle_traceability_audit.py"


def load_audit(repo_root: Path):
    audit_path = repo_root / TRACEABILITY_AUDIT
    spec = importlib.util.spec_from_file_location("lifecycle_traceability_audit", audit_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load traceability audit from {audit_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    parser = argparse.ArgumentParser(description="DBLC_STATIC_TRACEABILITY_COVERAGE")
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--report", type=Path,
                        help="generated evidence path outside the source tree")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    module = load_audit(repo_root)
    # An ordinary regression must not regenerate the tracked assurance artifact.
    # CTest retains its report in the build tree; standalone runs use owned temp.
    if args.report is not None:
        report = args.report.resolve()
        if report.is_relative_to(repo_root / "project"):
            parser.error("--report must be outside the project source tree")
        rc = module.run_audit(repo_root, report)
    else:
        with tempfile.TemporaryDirectory(prefix="sb-traceability-") as temporary:
            rc = module.run_audit(repo_root, Path(temporary) / "report.md")
    if rc == 0:
        print("DBLC_STATIC_TRACEABILITY_COVERAGE=passed")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
