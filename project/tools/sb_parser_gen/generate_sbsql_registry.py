#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate the SBSQL surface registry from frozen execution_plan artifacts.

The generator consumes the FSPE P0/P0L matrices and emits C++ registry constants
used by the parser worker. It intentionally generates assignment metadata only;
canonical behavior remains owned by the referenced contracts and SBLR/API
contracts.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[3]
STATUS_MATRIX = REPO_ROOT / "public_input_snapshot"
SURFACE_REGISTRY = REPO_ROOT / "public_input_snapshot"
ENGINE_PACKET_GAP_PLACEHOLDER = (
    "public_input_snapshot"
)


@dataclass(frozen=True)
class GeneratedRow:
    surface_id: str
    fixed_uuid_v7: str
    canonical_name: str
    surface_kind: str
    family: str
    source_status: str
    cluster_scope: str
    canonical_spec: str
    sblr_operation_family: str
    parser_packet: str
    engine_packet: str
    owner_lane: str
    batch_id: str
    ctest_label: str
    parser_handler_key: str
    udr_handler_key: str
    lowering_handler_key: str
    server_admission_key: str
    engine_rule_key: str
    diagnostic_key: str
    oracle_key: str
    validation_fixture_id: str
    final_acceptance_rule: str
    closure_action: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifact-root",
        type=Path,
        default=Path("project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"),
        help="Directory containing FSPE P0/P0L CSV artifacts.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("project/src/parsers/sbsql_worker/registry/generated"),
        help="Directory for generated C++ registry files.",
    )
    return parser.parse_args()


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def require(value: str, field: str, surface_id: str) -> str:
    if value == "":
        raise ValueError(f"{surface_id}: missing required field {field}")
    return value


def symbol(value: str) -> str:
    chars = []
    for ch in value.lower():
        if ch.isalnum():
            chars.append(ch)
        else:
            chars.append("_")
    collapsed = "_".join(part for part in "".join(chars).split("_") if part)
    return collapsed or "unknown"


def parser_handler_key(row: dict[str, str]) -> str:
    if row["owner_lane"] == "cluster_profile_gate_worker":
        return "parser.cluster_profile_gate"
    if row["surface_kind"] in {"function", "operator", "variable"}:
        return f"parser.expression_runtime.{row['surface_kind']}"
    if row["owner_lane"] == "parser grammar/AST worker":
        return "parser.grammar_ast"
    return f"parser.statement_family.{symbol(row['family'])}"


def udr_handler_key(row: dict[str, str]) -> str:
    if row["cluster_scope"] == "cluster_private":
        return "udr.sbsql_parser_support.cluster_profile_gate"
    if row["source_status"] == "native_future":
        return "udr.sbsql_parser_support.native_future_decision"
    return "udr.sbsql_parser_support.parse_describe_normalize"


def lowering_handler_key(row: dict[str, str]) -> str:
    if row["cluster_scope"] == "cluster_private":
        return "lowering.cluster_profile_gate"
    if row["surface_kind"] in {"function", "operator", "variable"}:
        return f"lowering.expression_runtime.{row['surface_kind']}"
    return f"lowering.sblr_family.{symbol(row['sblr_operation_family'])}"


def server_admission_key(row: dict[str, str]) -> str:
    if row["cluster_scope"] == "cluster_private":
        return "server.admission.cluster_profile_gate"
    return f"server.admission.{symbol(row['sblr_operation_family'])}"


def engine_rule_key(row: dict[str, str]) -> str:
    if row["cluster_scope"] == "cluster_private":
        return "engine.rule.cluster_private_fail_closed_or_profile"
    if row["source_status"] == "native_future":
        return "engine.rule.native_future_promotion_or_refusal"
    if row["engine_packet"] == "covered_by_sblr_operation_matrix":
        return f"engine.rule.{symbol(row['sblr_operation_family'])}"
    return "engine.rule.packet_refusal_or_implementation"


def validate_status_authority(
    row: dict[str, str],
    surface_id: str,
    canonical_status: str,
) -> None:
    if row["source_status"] != canonical_status:
        raise ValueError(
            f"{surface_id}: source_status mismatch with SBSQL_SURFACE_STATUS_MATRIX.csv "
            f"(source_status={row['source_status']} canonical_status={canonical_status})"
        )

    source_status = row["source_status"]
    engine_packet = row["engine_packet"]
    if source_status == "native_future" and engine_packet != ENGINE_PACKET_GAP_PLACEHOLDER:
        raise ValueError(
            f"{surface_id}: native_future row must use engine gap placeholder, found {engine_packet}"
        )
    if source_status == "cluster_private" and engine_packet != ENGINE_PACKET_GAP_PLACEHOLDER:
        raise ValueError(
            f"{surface_id}: cluster_private row must use engine gap placeholder, found {engine_packet}"
        )
    if (
        source_status == "native_now"
        and row["cluster_scope"] != "cluster_private"
        and engine_packet == ENGINE_PACKET_GAP_PLACEHOLDER
    ):
        raise ValueError(
            f"{surface_id}: native_now (non-cluster_private) row cannot retain engine gap placeholder"
        )


def canonicalized_artifact_row(
    row: dict[str, str],
    canonical_by_surface: dict[str, dict[str, str]],
    surface_id: str,
) -> dict[str, str]:
    canonical = canonical_by_surface.get(surface_id)
    if canonical is None:
        raise ValueError(f"{surface_id}: missing from SBSQL_SURFACE_REGISTRY.csv")
    merged = dict(row)
    merged.update(
        {
            "fixed_uuid_v7": canonical["fixed_uuid_v7"],
            "canonical_name": canonical["canonical_name"],
            "surface_kind": canonical["surface_kind"],
            "family": canonical["family"],
            "source_status": canonical["source_status"],
            "cluster_scope": canonical["cluster_scope"],
            "canonical_spec": canonical["canonical_spec"],
            "sblr_operation_family": canonical["sblr_operation_family"],
            "parser_packet": canonical["parser_packet"],
            "engine_packet": canonical["engine_packet"],
        }
    )
    return merged


def diagnostic_key(row: dict[str, str]) -> str:
    if row["cluster_scope"] == "cluster_private":
        return "diagnostic.cluster_profile_fail_closed"
    if row["source_status"] == "native_future":
        return "diagnostic.native_future_decision"
    return "diagnostic.canonical_message_vector"


def make_rows(artifact_root: Path) -> list[GeneratedRow]:
    backlog_rows = read_csv(artifact_root / "SURFACE_IMPLEMENTATION_BACKLOG.csv")
    batch_rows = read_csv(artifact_root / "BATCH_ROW_MEMBERSHIP.csv")
    oracle_rows = read_csv(artifact_root / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv")
    canonical_rows = read_csv(SURFACE_REGISTRY)
    status_rows = read_csv(STATUS_MATRIX)

    canonical_by_surface = {}
    for row in canonical_rows:
        surface_id = require(row["surface_id"], "surface_id", "<unknown>")
        if surface_id in canonical_by_surface:
            raise ValueError(f"{surface_id}: duplicate surface_id in SBSQL_SURFACE_REGISTRY.csv")
        canonical_by_surface[surface_id] = row

    status_by_surface = {}
    for row in status_rows:
        surface_id = require(row["surface_id"], "surface_id", "<unknown>")
        if surface_id in status_by_surface:
            raise ValueError(f"{surface_id}: duplicate surface_id in SBSQL_SURFACE_STATUS_MATRIX.csv")
        status_by_surface[surface_id] = row["source_status"]

    batch_by_surface = {row["surface_id"]: row for row in batch_rows}
    oracle_by_surface = {row["surface_id"]: row for row in oracle_rows}

    expected_surface_count = len(canonical_rows)
    if len(status_rows) != expected_surface_count:
        raise ValueError(
            "SBSQL_SURFACE_STATUS_MATRIX.csv count drift: "
            f"expected {expected_surface_count} found {len(status_rows)}"
        )
    if len(backlog_rows) != expected_surface_count:
        raise ValueError(
            f"expected {expected_surface_count} surface rows, found {len(backlog_rows)}"
        )

    generated: list[GeneratedRow] = []
    seen_surface_ids: set[str] = set()
    seen_uuids: set[str] = set()
    for row in backlog_rows:
        surface_id = require(row["surface_id"], "surface_id", "<unknown>")
        if surface_id in seen_surface_ids:
            raise ValueError(f"{surface_id}: duplicate surface_id")
        seen_surface_ids.add(surface_id)

        row = canonicalized_artifact_row(row, canonical_by_surface, surface_id)

        fixed_uuid = require(row["fixed_uuid_v7"], "fixed_uuid_v7", surface_id)
        if fixed_uuid in seen_uuids:
            raise ValueError(f"{surface_id}: duplicate fixed_uuid_v7 {fixed_uuid}")
        seen_uuids.add(fixed_uuid)

        batch = batch_by_surface.get(surface_id)
        if batch is None:
            raise ValueError(f"{surface_id}: missing batch membership")

        oracle = oracle_by_surface.get(surface_id)
        if oracle is None:
            raise ValueError(f"{surface_id}: missing semantic oracle assignment")

        canonical_status = status_by_surface.get(surface_id)
        if canonical_status is None:
            raise ValueError(f"{surface_id}: missing from SBSQL_SURFACE_STATUS_MATRIX.csv")

        validate_status_authority(row, surface_id, canonical_status)

        generated.append(
            GeneratedRow(
                surface_id=surface_id,
                fixed_uuid_v7=fixed_uuid,
                canonical_name=require(row["canonical_name"], "canonical_name", surface_id),
                surface_kind=require(row["surface_kind"], "surface_kind", surface_id),
                family=require(row["family"], "family", surface_id),
                source_status=require(row["source_status"], "source_status", surface_id),
                cluster_scope=require(row["cluster_scope"], "cluster_scope", surface_id),
                canonical_spec=require(row["canonical_spec"], "canonical_spec", surface_id),
                sblr_operation_family=require(
                    row["sblr_operation_family"], "sblr_operation_family", surface_id
                ),
                parser_packet=require(row["parser_packet"], "parser_packet", surface_id),
                engine_packet=require(row["engine_packet"], "engine_packet", surface_id),
                owner_lane=require(row["owner_lane"], "owner_lane", surface_id),
                batch_id=require(batch["batch_id"], "batch_id", surface_id),
                ctest_label=require(batch["ctest_label"], "ctest_label", surface_id),
                parser_handler_key=parser_handler_key(row),
                udr_handler_key=udr_handler_key(row),
                lowering_handler_key=lowering_handler_key(row),
                server_admission_key=server_admission_key(row),
                engine_rule_key=engine_rule_key(row),
                diagnostic_key=diagnostic_key(row),
                oracle_key=require(oracle["oracle_type"], "oracle_type", surface_id),
                validation_fixture_id=require(
                    row["validation_fixture_id"], "validation_fixture_id", surface_id
                ),
                final_acceptance_rule=require(
                    row["final_acceptance_rule"], "final_acceptance_rule", surface_id
                ),
                closure_action=require(row["closure_action"], "closure_action", surface_id),
            )
        )

    return generated


def cxx_string(value: str) -> str:
    escaped = []
    for ch in value:
        codepoint = ord(ch)
        if ch == "\\":
            escaped.append("\\\\")
        elif ch == '"':
            escaped.append('\\"')
        elif ch == "\n":
            escaped.append("\\n")
        elif ch == "\r":
            escaped.append("\\r")
        elif ch == "\t":
            escaped.append("\\t")
        elif 32 <= codepoint <= 126:
            escaped.append(ch)
        else:
            encoded = ch.encode("utf-8")
            escaped.extend(f"\\x{byte:02x}\"\"" for byte in encoded)
    return '"' + "".join(escaped) + '"'


def write_header(output_dir: Path, row_count: int) -> None:
    header = output_dir / "sbsql_generated_registry.hpp"
    header.write_text(
        """#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace scratchbird::parser::sbsql {

struct GeneratedSurfaceRegistryRow {
  std::string_view surface_id;
  std::string_view fixed_uuid_v7;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view family;
  std::string_view source_status;
  std::string_view cluster_scope;
  std::string_view canonical_spec;
  std::string_view sblr_operation_family;
  std::string_view parser_packet;
  std::string_view engine_packet;
  std::string_view owner_lane;
  std::string_view batch_id;
  std::string_view ctest_label;
  std::string_view parser_handler_key;
  std::string_view udr_handler_key;
  std::string_view lowering_handler_key;
  std::string_view server_admission_key;
  std::string_view engine_rule_key;
  std::string_view diagnostic_key;
  std::string_view oracle_key;
  std::string_view validation_fixture_id;
  std::string_view final_acceptance_rule;
  std::string_view closure_action;
};

inline constexpr std::size_t kGeneratedSurfaceRegistryRowCount = {row_count};

std::span<const GeneratedSurfaceRegistryRow> GeneratedSurfaceRegistryRows();
const GeneratedSurfaceRegistryRow* FindGeneratedSurfaceRegistryRowById(
    std::string_view surface_id);
const GeneratedSurfaceRegistryRow* FindGeneratedSurfaceRegistryRowByCanonicalName(
    std::string_view canonical_name);

} // namespace scratchbird::parser::sbsql
""".replace("{row_count}", str(row_count)),
        encoding="utf-8",
    )


def row_initializer(row: GeneratedRow) -> str:
    values = [
        row.surface_id,
        row.fixed_uuid_v7,
        row.canonical_name,
        row.surface_kind,
        row.family,
        row.source_status,
        row.cluster_scope,
        row.canonical_spec,
        row.sblr_operation_family,
        row.parser_packet,
        row.engine_packet,
        row.owner_lane,
        row.batch_id,
        row.ctest_label,
        row.parser_handler_key,
        row.udr_handler_key,
        row.lowering_handler_key,
        row.server_admission_key,
        row.engine_rule_key,
        row.diagnostic_key,
        row.oracle_key,
        row.validation_fixture_id,
        row.final_acceptance_rule,
        row.closure_action,
    ]
    return "    {" + ", ".join(cxx_string(value) for value in values) + "},"


def write_source(output_dir: Path, rows: Iterable[GeneratedRow]) -> None:
    source = output_dir / "sbsql_generated_registry.cpp"
    row_text = "\n".join(row_initializer(row) for row in rows)
    source.write_text(
        f"""#include "registry/generated/sbsql_generated_registry.hpp"

#include <array>

namespace scratchbird::parser::sbsql {{
namespace {{

constexpr std::array<GeneratedSurfaceRegistryRow, kGeneratedSurfaceRegistryRowCount> kRows{{{{
{row_text}
}}}};

}} // namespace

std::span<const GeneratedSurfaceRegistryRow> GeneratedSurfaceRegistryRows() {{
  return kRows;
}}

const GeneratedSurfaceRegistryRow* FindGeneratedSurfaceRegistryRowById(
    std::string_view surface_id) {{
  for (const auto& row : kRows) {{
    if (row.surface_id == surface_id) return &row;
  }}
  return nullptr;
}}

const GeneratedSurfaceRegistryRow* FindGeneratedSurfaceRegistryRowByCanonicalName(
    std::string_view canonical_name) {{
  for (const auto& row : kRows) {{
    if (row.canonical_name == canonical_name) return &row;
  }}
  return nullptr;
}}

}} // namespace scratchbird::parser::sbsql
""",
        encoding="utf-8",
    )


def write_manifest(output_dir: Path, rows: list[GeneratedRow]) -> None:
    manifest = output_dir / "sbsql_generated_registry.manifest"
    counts: dict[str, int] = {}
    for row in rows:
        counts[row.source_status] = counts.get(row.source_status, 0) + 1
    manifest.write_text(
        "\n".join(
            [
                "generator=project/tools/sb_parser_gen/generate_sbsql_registry.py",
                f"surface_count={len(rows)}",
                f"native_now={counts.get('native_now', 0)}",
                f"native_future={counts.get('native_future', 0)}",
                f"cluster_private={counts.get('cluster_private', 0)}",
                "",
            ]
        ),
        encoding="utf-8",
    )


def main() -> int:
    args = parse_args()
    rows = make_rows(args.artifact_root)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    write_header(args.output_dir, len(rows))
    write_source(args.output_dir, rows)
    write_manifest(args.output_dir, rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
