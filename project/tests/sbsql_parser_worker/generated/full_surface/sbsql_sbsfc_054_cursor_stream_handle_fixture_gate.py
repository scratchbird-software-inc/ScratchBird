#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-054 cursor/stream/handle fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_054_CURSOR_STREAM_HANDLE_FIXTURES.csv"

TARGETS = {
    "SBSQL-14BCC57267D0": ("cursor_lifetime_class(cursor)", "sb.cursor.lifetime_class"),
    "SBSQL-163833F6642E": ("cursor_open(<select>)", "sb.cursor.open"),
    "SBSQL-21E6F6488A64": ("cursor_to_rowset", "sb.cursor.to_rowset"),
    "SBSQL-38FDC3F10237": ("cursor_close(cursor)", "sb.cursor.close"),
    "SBSQL-60054AA2660F": ("cursor_lifetime_class", "sb.cursor.lifetime_class"),
    "SBSQL-6C87F2E4972C": ("cursor_open", "sb.cursor.open"),
    "SBSQL-892AE352BD3A": ("rowset_to_cursor(rowset)", "sb.cursor.rowset_to_cursor"),
    "SBSQL-8BE380B5BA73": ("stream_close", "sb.stream.close"),
    "SBSQL-92B7E4FF1332": ("cursor_state", "sb.cursor.state"),
    "SBSQL-9CD08935260C": ("cursor_position", "sb.cursor.position"),
    "SBSQL-A1FC30F481BC": ("table_value_to_cursor(tv)", "sb.cursor.table_value_to_cursor"),
    "SBSQL-A2E0FE1E034D": ("cursor_close", "sb.cursor.close"),
    "SBSQL-A339D846AD19": ("current_row_locator(cursor)", "sb.cursor.current_row_locator"),
    "SBSQL-A99EC7329DF0": ("cursor_to_rowset(cursor[,max_rows])", "sb.cursor.to_rowset"),
    "SBSQL-AE071DCD88A8": ("cursor_scrollability(cursor)", "sb.cursor.scrollability"),
    "SBSQL-B062E4E23477": ("handle_kind", "sb.handle.kind"),
    "SBSQL-B9BA63C166A2": ("stream_to_rowset(stream[,max_rows])", "sb.stream.to_rowset"),
    "SBSQL-C3E53B267C4B": ("cursor_holdability", "sb.cursor.holdability"),
    "SBSQL-C4EB99EF9F6F": ("cursor_position(cursor)", "sb.cursor.position"),
    "SBSQL-C682B85033B8": ("cursor_scrollability", "sb.cursor.scrollability"),
    "SBSQL-CD315B828601": ("table_value_to_cursor", "sb.cursor.table_value_to_cursor"),
    "SBSQL-CF69DD85814A": ("rowset_to_cursor", "sb.cursor.rowset_to_cursor"),
    "SBSQL-D059810BF5A0": ("handle_kind(handle)", "sb.handle.kind"),
    "SBSQL-D7858961F2DA": ("cursor_active", "sb.cursor.active"),
    "SBSQL-DCB17997FCA9": ("stream_close(stream)", "sb.stream.close"),
    "SBSQL-EFC58ACD7975": ("cursor_active(name)", "sb.cursor.active"),
    "SBSQL-F0E216005A4E": ("cursor_holdability(cursor)", "sb.cursor.holdability"),
    "SBSQL-F15435ED32F1": ("stream_to_rowset", "sb.stream.to_rowset"),
    "SBSQL-FCD7942CBB69": ("cursor_state(cursor)", "sb.cursor.state"),
}

REQUIRED_INVALID_FIXTURES = {
    "SBSFC054-cursor-to-rowset-missing",
    "SBSFC054-rowset-to-cursor-malformed",
    "SBSFC054-stream-to-rowset-negative-max",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    surfaces = {row["surface_id"]: row for row in read_csv(root / SURFACE_REGISTRY)}
    fixtures = read_csv(root / FIXTURES)
    seed_text = (root / SEED_REGISTRY).read_text(encoding="utf-8")
    registered_ids = set(
        re.findall(r'"(sb\.(?:cursor|stream|handle)\.[^"]+)"', seed_text)
    )

    errors: list[str] = []
    positive_by_surface: dict[str, set[str]] = {surface_id: set() for surface_id in TARGETS}
    invalid_seen: set[str] = set()
    fixture_ids: set[str] = set()

    for row in fixtures:
        fixture_id = row.get("fixture_id", "")
        if not fixture_id:
            errors.append("fixture row missing fixture_id")
            continue
        if fixture_id in fixture_ids:
            errors.append(f"{fixture_id}: duplicate fixture_id")
        fixture_ids.add(fixture_id)

        case_kind = row.get("case_kind", "")
        expected_diag = (row.get("expected_diagnostic_code", "") or "").strip()
        surface_id = row.get("surface_id", "")
        function_id = row.get("function_id", "")
        canonical = row.get("canonical_builtin_id", "")

        try:
            parsed_args = json.loads(row.get("arguments_json", ""))
            if not isinstance(parsed_args, list):
                errors.append(f"{fixture_id}: arguments_json must be a JSON list")
        except json.JSONDecodeError as exc:
            errors.append(f"{fixture_id}: invalid arguments_json: {exc}")

        if case_kind == "invalid_input":
            invalid_seen.add(fixture_id)
            if expected_diag != "SB_DIAG_FUNCTION_INVALID_INPUT":
                errors.append(f"{fixture_id}: invalid_input must declare SB_DIAG_FUNCTION_INVALID_INPUT")
            if function_id not in registered_ids:
                errors.append(f"{fixture_id}: {function_id} not registered in function_seed_registry")
            continue

        if case_kind != "positive":
            errors.append(f"{fixture_id}: unexpected case_kind {case_kind}")
            continue

        target = TARGETS.get(surface_id)
        if target is None:
            errors.append(f"{fixture_id}: unexpected surface_id {surface_id}")
            continue
        expected_name, expected_function_id = target
        if function_id != expected_function_id or canonical != expected_function_id:
            errors.append(f"{fixture_id}: function/canonical id mismatch for {surface_id}")
        if function_id not in registered_ids:
            errors.append(f"{fixture_id}: {function_id} not registered in function_seed_registry")
        if expected_diag:
            errors.append(f"{fixture_id}: positive fixture must not declare diagnostic")
        if row.get("expected_result_descriptor", "") not in {"json_document", "character", "int64", "boolean"}:
            errors.append(f"{fixture_id}: unexpected positive descriptor {row.get('expected_result_descriptor', '')}")

        surface = surfaces.get(surface_id)
        if surface is None:
            errors.append(f"{fixture_id}: surface_id missing from registry")
        else:
            if surface.get("status") != "native_now":
                errors.append(f"{fixture_id}: surface_id is not native_now")
            if surface.get("canonical_name") != expected_name:
                errors.append(
                    f"{fixture_id}: canonical_name mismatch: "
                    f"{surface.get('canonical_name')} != {expected_name}"
                )
            if surface.get("sblr_operation_family") != "sblr.expression.runtime.v3":
                errors.append(f"{fixture_id}: unexpected SBLR family {surface.get('sblr_operation_family')}")
        positive_by_surface[surface_id].add(fixture_id)

    for surface_id, fixtures_for_surface in positive_by_surface.items():
        if not fixtures_for_surface:
            errors.append(f"{surface_id}: missing positive fixture")
    for fixture_id in sorted(REQUIRED_INVALID_FIXTURES - invalid_seen):
        errors.append(f"{fixture_id}: missing required invalid-input fixture")

    print(
        "sbsql_sbsfc_054_cursor_stream_handle_fixture_gate "
        f"fixtures={len(fixtures)} surfaces={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_054_cursor_stream_handle_fixture_gate=failed", file=sys.stderr)
        for error in errors[:100]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_054_cursor_stream_handle_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
