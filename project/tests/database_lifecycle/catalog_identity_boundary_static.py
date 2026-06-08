#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static gate for sys.catalog identity-resolver name isolation."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


BASE_TABLES = (
    "sys.catalog.object_identity",
    "sys.catalog.object_versions",
    "sys.catalog.object_dependencies",
    "sys.catalog.index_definitions",
)

RESOLVER_ONLY_ROLES = (
    "resolver_raw_name",
    "resolver_display_name",
    "resolver_normalized_lookup_key",
    "resolver_exact_lookup_key",
    "resolver_full_path_lookup_key",
)


def _extract_table_block(source: str, table_path: str) -> str:
    marker = f'Table("{table_path}"'
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"missing catalog table profile: {table_path}")
    next_table = source.find('Table("', start + len(marker))
    if next_table < 0:
        next_table = source.find("};", start)
    return source[start:next_table]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    root = Path(args.repo_root)
    profile = root / "project/src/core/catalog/catalog_index_profile.cpp"
    projection = root / "project/src/engine/internal_api/catalog/sys_information_projection.cpp"
    header = root / "project/src/engine/internal_api/catalog/sys_information_projection.hpp"
    text = profile.read_text(encoding="utf-8")
    projection_text = projection.read_text(encoding="utf-8")
    header_text = header.read_text(encoding="utf-8")

    for table in BASE_TABLES:
        block = _extract_table_block(text, table)
        if "CatalogTableSurface::base_catalog" not in block:
            raise AssertionError(f"{table} is not declared as base_catalog")
        for role in RESOLVER_ONLY_ROLES:
            if f"CatalogColumnRole::{role}" in block:
                raise AssertionError(f"{table} duplicates resolver-only role {role}")
        if re.search(r'"(?:raw_|display_)?name(?:_text)?"', block):
            raise AssertionError(f"{table} includes human-name text column spelling")

    resolver_block = _extract_table_block(text, "sys.catalog.object_name_entries")
    for role in RESOLVER_ONLY_ROLES:
        if f"CatalogColumnRole::{role}" not in resolver_block:
            raise AssertionError(f"identity resolver lacks resolver role {role}")
    if "CatalogColumnRole::resolver_comment_text" in resolver_block:
        raise AssertionError("identity resolver table owns comment text instead of comment resolver")

    comment_block = _extract_table_block(text, "sys.catalog.object_comments")
    if "CatalogTableSurface::comment_resolver" not in comment_block:
        raise AssertionError("comment resolver table is not declared as comment_resolver")
    if "CatalogColumnRole::resolver_comment_text" not in comment_block:
        raise AssertionError("comment resolver lacks resolver_comment_text role")

    if "resolver_join_required = true" not in header_text:
        raise AssertionError("sys.information projection contract lost resolver join default")
    if "SysInformationProjectionColumnNameExposesUuid" not in projection_text:
        raise AssertionError("sys.information UUID exposure guard is not implemented")
    if "exposes_internal_uuid = false" not in projection_text:
        raise AssertionError("sys.information projection definitions must hide internal UUIDs")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"DBLC_STATIC_CATALOG_IDENTITY_BOUNDARY failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
