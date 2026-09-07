#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Canonical SBsql trigger lifecycle surface and evidence identities."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib


SOURCE_FILE = "public_contract_snapshot"
CANONICAL_SPEC = SOURCE_FILE
PARSER_PACKET = "public_input_snapshot"
ENGINE_PACKET = "covered_by_sblr_operation_matrix"
DOCUMENTATION_FAMILY = "language_reference_ddl"
SBLR_OPERATION_FAMILY = "sblr.catalog.mutation.v3"
FULL_ROUTE_CTEST = "sb_listener_sbp_sbsql_sbwp_tls_engine_auth_route_smoke"
FULL_ROUTE_TEST_SOURCE = (
    "project/tests/sbsql_parser_worker/sbsql_sbwp_tls_engine_auth_route_smoke.py"
)
PROCESS_TEST_SOURCE = (
    "project/tests/sbsql_sblr_alignment/ia01_source_map_process_e2e.py"
)
PROCESS_CLIENT_SOURCE = (
    "project/tests/sbsql_sblr_alignment/ia01_source_map_process_client.cpp"
)


def allocated_surface_id(canonical_name: str) -> str:
    digest = hashlib.sha256(canonical_name.encode("utf-8")).hexdigest()[:12]
    return f"SBSQL-{digest.upper()}"


def allocated_fixed_uuid_v7(canonical_name: str, collision_counter: int = 0) -> str:
    seed = (
        "sbsql_command_surface_registry\n"
        f"{canonical_name}\n"
        f"{collision_counter}"
    ).encode("ascii")
    digest = hashlib.sha256(seed).hexdigest()
    variant = format(0x8 | (int(digest[3], 16) & 0x3), "x")
    return (
        f"019e1500-0000-7{digest[:3]}-"
        f"{variant}{digest[4:7]}-{digest[7:19]}"
    )


@dataclass(frozen=True)
class TriggerCommandSurface:
    surface_id: str
    fixed_uuid_v7: str
    validation_fixture_id: str
    canonical_name: str
    verb: str
    sql: str
    operation_id: str
    opcode: str
    opcode_code: int
    operand: str
    request_magic: str
    bound_magic: str
    operand_magic: str
    result_magic: str
    request_bytes: int
    descriptor_bytes: int
    result_bytes: int
    engine_api: str
    process_ctest: str
    runtime_ctest: str
    availability_ctest: str
    cancellation_ctest: str
    csc_start: int
    new_surface: bool = False
    insertion_anchor: str = ""
    batch_insertion_anchor: str = ""

    def validate(self) -> None:
        if self.new_surface:
            if self.surface_id != allocated_surface_id(self.canonical_name):
                raise ValueError(f"{self.canonical_name}: deterministic surface ID drift")
            if self.fixed_uuid_v7 != allocated_fixed_uuid_v7(self.canonical_name):
                raise ValueError(f"{self.canonical_name}: deterministic UUID drift")
        if not self.validation_fixture_id or not self.sql:
            raise ValueError(f"{self.canonical_name}: incomplete public fixture identity")


TRIGGER_COMMAND_SURFACES: tuple[TriggerCommandSurface, ...] = (
    TriggerCommandSurface(
        surface_id="SBSQL-5127560F8031",
        fixed_uuid_v7="019dffbb-f000-7337-89da-26fb0cd118c4",
        validation_fixture_id="SBSQL-SURFACE-B1C95C652651",
        canonical_name="create_trigger_stmt",
        verb="CREATE",
        sql=(
            "CREATE TRIGGER users.public.route_trig_items_ai AFTER INSERT ON TABLE "
            "users.public.trig_items FOR EACH ROW AS BEGIN INSERT INTO "
            "users.public.trig_audit (...) VALUES (...); END"
        ),
        operation_id="engine.op.ddl_create_trigger",
        opcode="SBLR_DDL_CREATE_TRIGGER",
        opcode_code=1551,
        operand="create_trigger_descriptor",
        request_magic="TVQX",
        bound_magic="TVDX",
        operand_magic="TVDO",
        result_magic="TVRS",
        request_bytes=1664,
        descriptor_bytes=488,
        result_bytes=320,
        engine_api="EngineCreateTrigger",
        process_ctest="sbsql_sblr_alignment_ia08_ddl_create_trigger_process_e2e",
        runtime_ctest="sbsql_sblr_alignment_ia08_ddl_create_trigger_runtime",
        availability_ctest="sbsql_sblr_alignment_ia08_ddl_create_trigger_availability",
        cancellation_ctest="sbsql_sblr_alignment_ia08_ddl_create_trigger_cancellation_fault",
        csc_start=2621,
    ),
    TriggerCommandSurface(
        surface_id="SBSQL-AA3896D3895F",
        fixed_uuid_v7="019e1500-0000-7bfb-9d15-4a93899ca4ef",
        validation_fixture_id="SBSQL-SURFACE-AA3896D3895F",
        canonical_name="alter_trigger_statement",
        verb="ALTER",
        sql=(
            "ALTER TRIGGER users.public.route_trig_items_ai INACTIVE SET ORDER 7 "
            "SET SECURITY DEFINER COMPILE VALIDATE"
        ),
        operation_id="engine.op.ddl_alter_trigger",
        opcode="SBLR_DDL_ALTER_TRIGGER",
        opcode_code=1552,
        operand="alter_trigger_descriptor",
        request_magic="TAQX",
        bound_magic="TADX",
        operand_magic="TADO",
        result_magic="TARS",
        request_bytes=864,
        descriptor_bytes=488,
        result_bytes=320,
        engine_api="EngineAlterTrigger",
        process_ctest="sbsql_sblr_alignment_ia08_ddl_alter_trigger_process_e2e",
        runtime_ctest="sbsql_sblr_alignment_ia08_ddl_alter_trigger_runtime",
        availability_ctest="sbsql_sblr_alignment_ia08_ddl_alter_trigger_availability",
        cancellation_ctest="sbsql_sblr_alignment_ia08_ddl_alter_trigger_cancellation_fault",
        csc_start=2625,
        new_surface=True,
        insertion_anchor="alter_subject_action",
        batch_insertion_anchor="SBSQL-A8B6D2477DBB",
    ),
    TriggerCommandSurface(
        surface_id="SBSQL-E64AF6FD5CD3",
        fixed_uuid_v7="019e1500-0000-7593-893a-b8b4b57c59d4",
        validation_fixture_id="SBSQL-SURFACE-E64AF6FD5CD3",
        canonical_name="drop_trigger_statement",
        verb="DROP",
        sql="DROP TRIGGER users.public.route_trig_items_ai RESTRICT",
        operation_id="engine.op.ddl_drop_trigger",
        opcode="SBLR_DDL_DROP_TRIGGER",
        opcode_code=1553,
        operand="drop_trigger_descriptor",
        request_magic="TDQX",
        bound_magic="TDDX",
        operand_magic="TDDO",
        result_magic="TDRS",
        request_bytes=864,
        descriptor_bytes=488,
        result_bytes=320,
        engine_api="EngineDropTrigger",
        process_ctest="sbsql_sblr_alignment_ia08_ddl_drop_trigger_process_e2e",
        runtime_ctest="sbsql_sblr_alignment_ia08_ddl_drop_trigger_runtime",
        availability_ctest="sbsql_sblr_alignment_ia08_ddl_drop_trigger_availability",
        cancellation_ctest="sbsql_sblr_alignment_ia08_ddl_drop_trigger_cancellation_fault",
        csc_start=2629,
        new_surface=True,
        insertion_anchor="drop_schedule_stmt",
        batch_insertion_anchor="SBSQL-E589270E0A27",
    ),
)

for _surface in TRIGGER_COMMAND_SURFACES:
    _surface.validate()

TRIGGER_COMMAND_BY_SURFACE_ID = {
    row.surface_id: row for row in TRIGGER_COMMAND_SURFACES
}
NEW_TRIGGER_COMMAND_SURFACES = tuple(
    row for row in TRIGGER_COMMAND_SURFACES if row.new_surface
)
