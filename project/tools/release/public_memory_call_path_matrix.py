#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0


# Public release PCR-016 memory call-path matrix generator.
import argparse
import csv
from pathlib import Path
import sys


ROWS = [
    {
        "row_id": "PCR016-MEM-RESOURCE",
        "subsystem": "core.memory",
        "public_source": "project/src/core/memory/reservation_backed_memory_resource.cpp",
        "call_path": "AcquireReservationBackedMemoryResource",
        "allocation_model": "HierarchicalMemoryBudgetLedger+MemoryManager+BoundedAllocator",
        "container_coverage": "raw_resource_allocation",
        "authority_boundary": "memory_evidence_only",
        "required_tokens": [
            "AcquireReservationBackedMemoryResource",
            "request.reservation_ledger->Reserve",
            "request.reservation_ledger->Commit",
            "request_.memory_manager->Allocate",
            "request_.memory_manager->Deallocate",
            "reservation_backed_memory.authority_scope=evidence_only",
        ],
    },
    {
        "row_id": "PCR016-MEM-PMR-ADAPTER",
        "subsystem": "core.memory",
        "public_source": "project/src/core/memory/reservation_backed_memory_resource.hpp",
        "call_path": "ReservationBackedPmrMemoryResource",
        "allocation_model": "ReservationBackedMemoryResource+std::pmr::memory_resource",
        "container_coverage": "pmr_adapter",
        "authority_boundary": "inherits_reservation_backed_memory_authority",
        "required_tokens": [
            "ReservationBackedPmrMemoryResource final",
            "std::pmr::memory_resource",
            "do_allocate",
            "do_deallocate",
            "ReservationBackedPmrMemoryResourceSnapshot",
        ],
    },
    {
        "row_id": "PCR016-MEM-PMR-CONTAINERS",
        "subsystem": "project.tests.release",
        "public_source": "project/tests/release/public_memory_pmr_call_path_gate.cpp",
        "call_path": "std_pmr_vector_map_string_gate",
        "allocation_model": "ReservationBackedPmrMemoryResource",
        "container_coverage": "std::pmr::vector;std::pmr::map;std::pmr::string",
        "authority_boundary": "runtime_gate_evidence_only",
        "required_tokens": [
            "std::pmr::vector",
            "std::pmr::map",
            "std::pmr::string",
            "ReservationBackedPmrMemoryResource",
            "failed_allocation_count",
        ],
    },
    {
        "row_id": "PCR016-ENGINE-EXECUTOR",
        "subsystem": "engine.executor",
        "public_source": "project/src/engine/executor/reservation_backed_executor_memory_bridge.cpp",
        "call_path": "AllocateExecutorOperatorFromReservedResource",
        "allocation_model": "ReservationBackedMemoryResource",
        "container_coverage": "operator_scratch",
        "authority_boundary": "MGA_security_recheck_required_memory_evidence_only",
        "required_tokens": [
            "AllocateExecutorOperatorFromReservedResource",
            "resource->Allocate",
            "engine_mga_snapshot_bound",
            "transaction_inventory_authoritative",
            "security_recheck_required",
            "after_reservation=true",
        ],
    },
    {
        "row_id": "PCR016-ENGINE-PLANNER",
        "subsystem": "engine.planner",
        "public_source": "project/src/engine/planner/reservation_backed_planner_memory_bridge.cpp",
        "call_path": "BuildPlannerTemporaryWorkFromReservedResource",
        "allocation_model": "ReservationBackedMemoryResource",
        "container_coverage": "planner_temporary_work",
        "authority_boundary": "parser_donor_and_memory_plan_authority_refused",
        "required_tokens": [
            "BuildPlannerTemporaryWorkFromReservedResource",
            "resource->Allocate",
            "parser_or_donor_authority",
            "memory_plan_authority",
            "after_reservation=true",
        ],
    },
    {
        "row_id": "PCR016-ENGINE-OPTIMIZER",
        "subsystem": "engine.optimizer",
        "public_source": "project/src/engine/optimizer/reservation_backed_optimizer_memory_bridge.cpp",
        "call_path": "BuildOptimizerTemporaryWorkFromReservedResource",
        "allocation_model": "ReservationBackedMemoryResource",
        "container_coverage": "optimizer_candidate_work",
        "authority_boundary": "catalog_stats_required_memory_benchmark_refused",
        "required_tokens": [
            "BuildOptimizerTemporaryWorkFromReservedResource",
            "resource->Allocate",
            "catalog_stats_authoritative",
            "memory_benchmark_authority",
            "after_reservation=true",
        ],
    },
    {
        "row_id": "PCR016-ENGINE-RESULT-FRAME",
        "subsystem": "engine.executor",
        "public_source": "project/src/engine/executor/vectorized_result_batch.cpp",
        "call_path": "FinalizeVectorizedResultBatchFromReservedResource",
        "allocation_model": "ReservationBackedMemoryResource",
        "container_coverage": "result_frame_scratch",
        "authority_boundary": "data_transport_only_memory_finality_refused",
        "required_tokens": [
            "FinalizeVectorizedResultBatchFromReservedResource",
            "resource->Allocate",
            "engine_mga_authoritative",
            "memory_finality_or_visibility_authority",
            "data_transport_only_not_transaction_finality",
        ],
    },
    {
        "row_id": "PCR016-ENGINE-SBLR-HANDOFF",
        "subsystem": "engine.sblr",
        "public_source": "project/src/engine/sblr/sblr_parser_memory_handoff.cpp",
        "call_path": "BuildSblrParserHandoffBuffer",
        "allocation_model": "ReservationBackedMemoryResource",
        "container_coverage": "sblr_parser_handoff_buffer",
        "authority_boundary": "translation_buffer_only_not_parser_execution",
        "required_tokens": [
            "BuildSblrParserHandoffBuffer",
            "resource->Allocate",
            "engine_mga_authoritative",
            "parser_or_donor_finality_authority",
            "translation_buffer_only_not_parser_execution",
        ],
    },
    {
        "row_id": "PCR016-MEM-BACKGROUND",
        "subsystem": "core.memory",
        "public_source": "project/src/core/memory/background_memory_reclamation.cpp",
        "call_path": "RunBackgroundMemoryReclamationWithReservedResource",
        "allocation_model": "ReservationBackedMemoryResource",
        "container_coverage": "background_maintenance_scratch",
        "authority_boundary": "engine_mga_required_memory_evidence_only",
        "required_tokens": [
            "RunBackgroundMemoryReclamationWithReservedResource",
            "ReservationBackedMemoryResource* resource",
            "resource->Allocate",
            "background_reclamation.authority_scope=evidence_only",
        ],
    },
    {
        "row_id": "PCR016-INDEX-IN-MEMORY",
        "subsystem": "core.index",
        "public_source": "project/src/core/index/in_memory_index_runtime.cpp",
        "call_path": "InMemoryIndexRuntimeState",
        "allocation_model": "approved_exemption:quota_bounded_candidate_cache",
        "container_coverage": "std::map;std::vector;candidate_entries_only",
        "authority_boundary": "exact_MGA_security_recheck_required_no_index_finality",
        "required_tokens": [
            "memory_quota_bytes",
            "memory_quota_denied",
            "in_memory.candidate_entries_only=true",
            "in_memory.final_rows_authorized=false",
            "in_memory.exact_recheck.required=true",
            "in_memory.mga_recheck.required=true",
            "in_memory.security_recheck.required=true",
        ],
    },
]


FIELDNAMES = [
    "row_id",
    "subsystem",
    "public_source",
    "call_path",
    "allocation_model",
    "container_coverage",
    "authority_boundary",
    "required_tokens",
    "status",
    "missing_tokens",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the public PCR-016 memory call-path matrix."
    )
    parser.add_argument("--project-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def project_relative(path: str) -> Path:
    prefix = "project/"
    if not path.startswith(prefix):
        raise ValueError(f"public source is outside project/: {path}")
    return Path(path[len(prefix):])


def evaluate_row(project_root: Path, row: dict) -> dict:
    source = project_root / project_relative(row["public_source"])
    if not source.is_file():
        missing = list(row["required_tokens"])
    else:
        body = source.read_text(encoding="utf-8")
        missing = [token for token in row["required_tokens"] if token not in body]
    output = {key: row[key] for key in FIELDNAMES if key in row}
    output["required_tokens"] = ";".join(row["required_tokens"])
    output["status"] = "complete" if not missing else "missing"
    output["missing_tokens"] = ";".join(missing)
    return output


def main() -> int:
    args = parse_args()
    project_root = args.project_root.resolve()
    if project_root.name != "project":
        print("--project-root must point at the public project tree", file=sys.stderr)
        return 2

    rows = [evaluate_row(project_root, row) for row in ROWS]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=FIELDNAMES)
        writer.writeheader()
        writer.writerows(rows)

    missing_rows = [row for row in rows if row["status"] != "complete"]
    if missing_rows:
        for row in missing_rows:
            print(
                f"{row['row_id']} missing {row['missing_tokens']}",
                file=sys.stderr,
            )
        return 1
    print(f"wrote {args.output} with {len(rows)} complete rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
