#!/usr/bin/env python3
"""Ratchet storage module ownership boundaries without claiming runtime proof."""

from __future__ import annotations

import pathlib
import sys


PROJECT_ROOT = pathlib.Path(__file__).resolve().parents[2]
INTERNAL_API = PROJECT_ROOT / "src" / "engine" / "internal_api"
CMAKE = INTERNAL_API / "CMakeLists.txt"

MODULES = {
    "mga_relation_store/mga_relation_store.cpp": (
        20_600,
        890_000,
        "SB_ENGINE_MGA_RELATION_ROW_STORE_IMPLEMENTATION_AUTHORITY",
    ),
    "mga_relation_store/mga_bulk_import_publication.cpp": (
        1_150,
        50_000,
        "SB_ENGINE_MGA_BULK_IMPORT_PUBLICATION_IMPLEMENTATION_AUTHORITY",
    ),
    "mga_relation_store/mga_event_sequence_allocator.cpp": (
        430,
        17_000,
        "SB_ENGINE_MGA_EVENT_SEQUENCE_ALLOCATOR_IMPLEMENTATION_AUTHORITY",
    ),
    "mga_relation_store/mga_large_value_store.cpp": (
        550,
        23_000,
        "SB_ENGINE_MGA_LARGE_VALUE_STORE_IMPLEMENTATION_AUTHORITY",
    ),
    "mga_relation_store/mga_secondary_index_coordination.cpp": (
        2_100,
        95_000,
        "SB_ENGINE_MGA_SECONDARY_INDEX_COORDINATION_IMPLEMENTATION_AUTHORITY",
    ),
    "mga_relation_store/mga_relation_statistics.cpp": (
        240,
        10_000,
        "SB_ENGINE_MGA_RELATION_STATISTICS_IMPLEMENTATION_AUTHORITY",
    ),
    "mga_relation_store/mga_heap_executor.cpp": (
        3_200,
        150_000,
        "SB_ENGINE_MGA_HEAP_EXECUTION_AUTHORITY",
    ),
    "dml/insert_physical_integration.cpp": (
        600,
        32_000,
        "SB_ENGINE_INSERT_PHYSICAL_AUTHORITY_COORDINATOR",
    ),
    "dml/direct_physical_bulk_append.cpp": (
        11_000,
        460_000,
        "SB_ENGINE_DIRECT_PHYSICAL_BULK_APPEND_COORDINATOR",
    ),
    "dml/direct_bulk_generated_projection.cpp": (
        700,
        24_000,
        "SB_ENGINE_DIRECT_BULK_GENERATED_PROJECTION_IMPLEMENTATION_AUTHORITY",
    ),
    "dml/direct_bulk_uuid_authority.cpp": (
        320,
        12_000,
        "SB_ENGINE_DIRECT_BULK_UUID_IMPLEMENTATION_AUTHORITY",
    ),
}

FORBIDDEN_BY_MODULE = {
    "mga_relation_store/mga_bulk_import_publication.cpp": (
        "PersistLocalTransactionInventory",
        "WritePhysicalMgaCowUnpublishedMutation",
        "FinalizePhysicalMgaCowTransaction",
        "AppendMgaRowVersion",
    ),
    "mga_relation_store/mga_event_sequence_allocator.cpp": (
        "PersistLocalTransactionInventory",
        "WritePhysicalMgaCowUnpublishedMutation",
        "FinalizePhysicalMgaCowTransaction",
    ),
    "mga_relation_store/mga_large_value_store.cpp": (
        "PersistLocalTransactionInventory",
        "WritePhysicalMgaCowUnpublishedMutation",
        "FinalizePhysicalMgaCowTransaction",
        "AppendMgaRowVersion",
    ),
    "mga_relation_store/mga_secondary_index_coordination.cpp": (
        "PersistLocalTransactionInventory",
        "WritePhysicalMgaCowUnpublishedMutation",
        "FinalizePhysicalMgaCowTransaction",
        "AppendMgaRowVersion",
    ),
    "mga_relation_store/mga_relation_statistics.cpp": (
        "PersistLocalTransactionInventory",
        "WritePhysicalMgaCowUnpublishedMutation",
        "FinalizePhysicalMgaCowTransaction",
        "AppendMgaRowVersion",
    ),
    "mga_relation_store/mga_heap_executor.cpp": (
        "PersistLocalTransactionInventory",
        "WritePhysicalMgaCowUnpublishedMutation",
        "FinalizePhysicalMgaCowTransaction",
    ),
    "dml/insert_physical_integration.cpp": (
        "ExecuteDirectPhysicalBulkAppend",
        "PersistLocalTransactionInventory",
        "FinalizePhysicalMgaCowTransaction",
    ),
    "dml/direct_physical_bulk_append.cpp": (
        "ExecuteInsertPhysicalIntegration",
        "PersistLocalTransactionInventory",
    ),
    "dml/direct_bulk_generated_projection.cpp": (
        "transaction_inventory",
        "WritePhysicalMgaCowUnpublishedMutation",
        "FinalizePhysicalMgaCowTransaction",
    ),
    "dml/direct_bulk_uuid_authority.cpp": (
        "transaction_inventory",
        "WritePhysicalMgaCowUnpublishedMutation",
        "FinalizePhysicalMgaCowTransaction",
    ),
}


def main() -> int:
    failures: list[str] = []
    cmake_text = CMAKE.read_text(encoding="utf-8")
    seen_keys: set[str] = set()

    for relative, (maximum_lines, maximum_bytes, search_key) in MODULES.items():
        path = INTERNAL_API / relative
        if not path.is_file():
            failures.append(f"missing authority module: {relative}")
            continue
        raw = path.read_bytes()
        text = raw.decode("utf-8")
        line_count = len(text.splitlines())
        if line_count > maximum_lines:
            failures.append(
                f"{relative}: {line_count} lines exceeds ratchet {maximum_lines}"
            )
        if len(raw) > maximum_bytes:
            failures.append(
                f"{relative}: {len(raw)} bytes exceeds ratchet {maximum_bytes}"
            )
        if text.count(search_key) != 1:
            failures.append(f"{relative}: requires exactly one {search_key}")
        if search_key in seen_keys:
            failures.append(f"duplicate authority search key: {search_key}")
        seen_keys.add(search_key)
        if f"  {relative}\n" not in cmake_text:
            failures.append(f"{relative}: not enrolled exactly in internal API CMake")
        if '#include "' in text and any(
            line.lstrip().startswith("#include") and ".inc" in line
            for line in text.splitlines()
        ):
            failures.append(f"{relative}: implementation fragments are forbidden")
        for token in FORBIDDEN_BY_MODULE.get(relative, ()):
            if token in text:
                failures.append(f"{relative}: forbidden authority token {token!r}")

    relation = (INTERNAL_API / "mga_relation_store/mga_relation_store.cpp").read_text(
        encoding="utf-8"
    )
    if "namespace scratchbird::engine::executor" in relation:
        failures.append("mga_relation_store.cpp: executor authority leaked into store")
    direct = (INTERNAL_API / "dml/direct_physical_bulk_append.cpp").read_text(
        encoding="utf-8"
    )
    if direct.count("DirectPhysicalBulkAppendResult ExecuteDirectPhysicalBulkAppend(") != 1:
        failures.append("direct bulk coordinator must own its one public entrypoint")

    if failures:
        for failure in failures:
            print(f"storage_module_authority_gate: FAIL: {failure}", file=sys.stderr)
        return 1
    print(
        "storage_module_authority_gate: PASS "
        f"modules={len(MODULES)} classification=source_contract_not_runtime_proof"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
