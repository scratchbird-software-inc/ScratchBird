#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static gate for sys.catalog physical index profile boundaries."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def _extract_index_blocks(source: str) -> list[str]:
    starts = [match.start() for match in re.finditer(r'auto\s+\w+\s*=\s*Index\("', source)]
    blocks: list[str] = []
    for index, start in enumerate(starts):
        end = starts[index + 1] if index + 1 < len(starts) else source.find("return indexes;", start)
        blocks.append(source[start:end])
    return blocks


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    root = Path(args.repo_root)
    source = (root / "project/src/core/catalog/catalog_index_profile.cpp").read_text(
        encoding="utf-8"
    )

    blocks = _extract_index_blocks(source)
    if not blocks:
        raise AssertionError("no catalog physical index profiles found")

    uuid_hash_count = 0
    btree_count = 0
    resolver_count = 0
    for block in blocks:
        supports_uuid = "supports_uuid_exact_lookup = true" in block
        uses_hash = "CatalogIndexMethod::hash_equality" in block
        uses_btree = "CatalogIndexMethod::btree_ordered" in block
        if supports_uuid:
            uuid_hash_count += 1
            if not uses_hash:
                raise AssertionError("UUID exact lookup profile is not hash_equality")
            if uses_btree:
                raise AssertionError("UUID exact lookup profile also requests B-tree")
        if uses_btree:
            btree_count += 1
            ordered_need = any(
                token in block
                for token in (
                    "supports_ordered_scan = true",
                    "supports_group_scan = true",
                    "supports_prefix_scan = true",
                    "supports_catalog_generation_visibility = true",
                    "supports_transaction_history = true",
                    "supports_name_resolution = true",
                )
            )
            if not ordered_need:
                raise AssertionError("B-tree profile lacks ordered/group/prefix/generation need")
        if "supports_name_resolution = true" in block:
            resolver_count += 1
            if "sys.catalog.object_name_entries" not in block:
                raise AssertionError("name-resolution profile does not target resolver table")
            if "authority_boundary = \"identity_resolver" not in block:
                raise AssertionError("name-resolution profile lacks identity resolver boundary")

    if uuid_hash_count < 3:
        raise AssertionError("expected UUID hash profiles for objects, rows, and indexes")
    if btree_count < 4:
        raise AssertionError("expected ordered B-tree profiles for history/generation/resolver paths")
    if resolver_count < 2:
        raise AssertionError("expected authoritative resolver and resolver accelerator profiles")
    if "ValidateBuiltinCatalogIndexProfiles" not in source:
        raise AssertionError("built-in catalog index profile validation function missing")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"DBLC_STATIC_CATALOG_INDEX_PROFILE_BOUNDARY failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
