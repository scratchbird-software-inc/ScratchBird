#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public reference parser implementation-start evidence manifests.

Historical generation of these manifests is private workplan/controller work.
The public repository keeps only the source-independent reference regression
test manifests and this read-only validation entry point.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

from reference_parser_gate_evidence_closure_gate import validate as validate_public_handoff


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--check", action="store_true", help="accepted for compatibility; validation is always read-only")
    args = parser.parse_args(argv)

    validate_public_handoff(args.repo_root.resolve())
    print("reference_parser_start_evidence_manifests=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
