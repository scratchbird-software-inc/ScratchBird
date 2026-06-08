#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Canonical SBsql bridge command surface rows.

This module is intentionally data-only. The sync/generator/gate path imports
it so the bridge command list is regenerated into the canonical SBsql surface
matrices and the project/tests proof fixtures from one shared definition.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib


SOURCE_FILE = "public_contract_snapshot"
SOURCE_ANCHOR = "SBSQL_BRIDGE_COMMAND_SURFACE_FULL_TRACKING"
CANONICAL_SPEC = SOURCE_FILE
PARSER_PACKET = "public_input_snapshot"
ENGINE_PACKET = "project/src/engine/sblr/sblr_opcode_registry.cpp#bridge-universal-abi"
DOCUMENTATION_FAMILY = "language_reference_bridge"
SBLR_OPERATION_FAMILY = "sblr.bridge.operation.v3"
BRIDGE_CTEST = "sbsql_bridge_command_route_conformance"
BRIDGE_TEST_SOURCE = "project/tests/sbsql_parser_worker/sbsql_bridge_command_route_conformance.cpp"


@dataclass(frozen=True)
class BridgeCommandSurface:
    label: str
    canonical_name: str
    sql: str
    operation_id: str
    udr_operation: str
    opcode: str
    requires_transaction_context: bool
    cluster_route: bool = False
    udr_request_suffix: str = ""
    expected_refusal_code: str = ""
    expected_udr_operation: str = ""

    @property
    def surface_id(self) -> str:
        digest = hashlib.sha256(self.canonical_name.encode("utf-8")).hexdigest()[:12].upper()
        return f"SBSQL-{digest}"

    @property
    def fixed_uuid_v7(self) -> str:
        digest = hashlib.sha256(f"bridge:{self.canonical_name}".encode("utf-8")).hexdigest()
        return f"019e14c1-{digest[0:4]}-7{digest[4:7]}-8{digest[7:10]}-{digest[10:22]}"

    @property
    def cluster_scope(self) -> str:
        return "cluster_private" if self.cluster_route else "noncluster_or_profile_scoped"

    @property
    def status(self) -> str:
        return "native_now"

    @property
    def surface_kind(self) -> str:
        return "grammar_production"

    @property
    def family(self) -> str:
        return "bridge"

    @property
    def effective_udr_operation(self) -> str:
        return self.expected_udr_operation or self.udr_operation

    @property
    def notes_tokens(self) -> str:
        parts = [
            f"sql={self.sql}",
            f"operation_id={self.operation_id}",
            f"opcode={self.opcode}",
            f"udr_operation={self.effective_udr_operation}",
            f"requires_transaction_context={str(self.requires_transaction_context).lower()}",
            f"cluster_route={str(self.cluster_route).lower()}",
        ]
        if self.udr_request_suffix:
            parts.append(f"udr_request_suffix={self.udr_request_suffix}")
        if self.expected_refusal_code:
            parts.append(f"expected_refusal_code={self.expected_refusal_code}")
        return ";".join(parts)


BRIDGE_COMMAND_SURFACES: tuple[BridgeCommandSurface, ...] = (
    BridgeCommandSurface("show_capabilities", "bridge_show_capabilities", "SHOW BRIDGE CAPABILITIES", "bridge.describe_capabilities", "describe_capabilities", "SBLR_BRIDGE_DESCRIBE_CAPABILITIES", False),
    BridgeCommandSurface("create_bridge", "bridge_create_connection", "CREATE BRIDGE CONNECTION fb_remote", "bridge.connect", "connect", "SBLR_BRIDGE_OPEN_CHANNEL", False),
    BridgeCommandSurface("attach_bridge", "bridge_attach", "ATTACH BRIDGE fb_remote", "bridge.attach", "attach", "SBLR_BRIDGE_OPEN_CHANNEL", False),
    BridgeCommandSurface("authenticate_bridge", "bridge_authenticate", "BRIDGE AUTHENTICATE fb_remote", "bridge.authenticate", "authenticate", "SBLR_BRIDGE_AUTHENTICATE", False),
    BridgeCommandSurface("open_session", "bridge_open_session", "OPEN BRIDGE SESSION fb_remote", "bridge.open_session", "open_session", "SBLR_BRIDGE_OPEN_SESSION", False),
    BridgeCommandSurface("close_session", "bridge_close_session", "CLOSE BRIDGE SESSION fb_remote", "bridge.close_session", "close_session", "SBLR_BRIDGE_CLOSE_SESSION", False),
    BridgeCommandSurface("detach_bridge", "bridge_detach", "DETACH BRIDGE fb_remote", "bridge.detach", "detach", "SBLR_BRIDGE_CLOSE_SESSION", False),
    BridgeCommandSurface("ping_bridge", "bridge_ping", "BRIDGE PING fb_remote", "bridge.ping", "ping", "SBLR_BRIDGE_HEALTH", False),
    BridgeCommandSurface("health_bridge", "bridge_health", "BRIDGE HEALTH fb_remote", "bridge.health", "health", "SBLR_BRIDGE_HEALTH", False),
    BridgeCommandSurface("cancel_bridge", "bridge_cancel", "BRIDGE CANCEL fb_remote", "bridge.cancel", "cancel", "SBLR_BRIDGE_CANCEL", False),
    BridgeCommandSurface("drain_bridge", "bridge_drain", "BRIDGE DRAIN fb_remote", "bridge.drain", "drain", "SBLR_BRIDGE_DRAIN", False),
    BridgeCommandSurface("shutdown_bridge", "bridge_shutdown", "BRIDGE SHUTDOWN fb_remote", "bridge.shutdown", "shutdown", "SBLR_BRIDGE_DRAIN", False),
    BridgeCommandSurface("begin_bridge_tx", "bridge_begin_transaction", "BRIDGE BEGIN fb_remote", "bridge.begin", "begin", "SBLR_BRIDGE_TX_BEGIN", True),
    BridgeCommandSurface("commit_bridge_tx", "bridge_commit_transaction", "BRIDGE COMMIT fb_remote", "bridge.commit", "commit", "SBLR_BRIDGE_TX_COMMIT", True),
    BridgeCommandSurface("rollback_bridge_tx", "bridge_rollback_transaction", "BRIDGE ROLLBACK fb_remote", "bridge.rollback", "rollback", "SBLR_BRIDGE_TX_ROLLBACK", True),
    BridgeCommandSurface("prepare_bridge_tx", "bridge_prepare_transaction", "BRIDGE PREPARE fb_remote", "bridge.prepare", "prepare", "SBLR_BRIDGE_TX_PREPARE", True),
    BridgeCommandSurface("savepoint_bridge_tx", "bridge_savepoint_transaction", "BRIDGE SAVEPOINT fb_remote sp1", "bridge.savepoint", "savepoint", "SBLR_BRIDGE_TX_SAVEPOINT", True),
    BridgeCommandSurface("execute_bridge", "bridge_execute", "BRIDGE EXECUTE fb_remote", "bridge.execute", "execute", "SBLR_BRIDGE_EXECUTE", True),
    BridgeCommandSurface("cursor_open", "bridge_cursor_open", "BRIDGE CURSOR OPEN fb_remote", "bridge.cursor_open", "cursor_open", "SBLR_BRIDGE_CURSOR_OPEN", True),
    BridgeCommandSurface("cursor_fetch", "bridge_cursor_fetch", "BRIDGE CURSOR FETCH fb_remote", "bridge.cursor_fetch", "cursor_fetch", "SBLR_BRIDGE_CURSOR_FETCH", True),
    BridgeCommandSurface("cursor_close", "bridge_cursor_close", "BRIDGE CURSOR CLOSE fb_remote", "bridge.cursor_close", "cursor_close", "SBLR_BRIDGE_CURSOR_CLOSE", True),
    BridgeCommandSurface("logical_restore_stream", "bridge_stream_open_logical_restore", "BRIDGE STREAM OPEN LOGICAL RESTORE fb_remote", "bridge.stream_open", "stream_open", "SBLR_BRIDGE_STREAM_OPEN", True, udr_request_suffix=";stream_kind=logical_restore"),
    BridgeCommandSurface("stream_read", "bridge_stream_read", "BRIDGE STREAM READ fb_remote", "bridge.stream_read", "stream_read", "SBLR_BRIDGE_STREAM_READ", True, udr_request_suffix=";stream_uuid=019e14c0-0000-7000-8000-00000000e001"),
    BridgeCommandSurface("stream_write", "bridge_stream_write", "BRIDGE STREAM WRITE fb_remote", "bridge.stream_write", "stream_write", "SBLR_BRIDGE_STREAM_WRITE", True, udr_request_suffix=";stream_uuid=019e14c0-0000-7000-8000-00000000e001"),
    BridgeCommandSurface("stream_close", "bridge_stream_close", "BRIDGE STREAM CLOSE fb_remote", "bridge.stream_close", "stream_close", "SBLR_BRIDGE_STREAM_CLOSE", True, udr_request_suffix=";stream_uuid=019e14c0-0000-7000-8000-00000000e001"),
    BridgeCommandSurface("cdc_start", "bridge_cdc_start", "BRIDGE CDC START fb_remote", "bridge.cdc_start", "cdc_start", "SBLR_BRIDGE_CDC_START", True),
    BridgeCommandSurface("cdc_read", "bridge_cdc_read", "BRIDGE CDC READ fb_remote", "bridge.cdc_read", "cdc_read", "SBLR_BRIDGE_CDC_READ", True, udr_request_suffix=";idempotency_key=cdc-read-1"),
    BridgeCommandSurface("cdc_apply", "bridge_cdc_apply", "BRIDGE CDC APPLY fb_remote", "bridge.cdc_apply", "cdc_apply", "SBLR_BRIDGE_CDC_APPLY", True, udr_request_suffix=";idempotency_key=cdc-apply-1"),
    BridgeCommandSurface("proxy_route", "bridge_proxy_route", "BRIDGE PROXY ROUTE fb_remote", "bridge.proxy_route", "proxy_route", "SBLR_BRIDGE_PROXY_ROUTE", True),
    BridgeCommandSurface("compare_result", "bridge_compare_result", "BRIDGE COMPARE RESULT fb_remote", "bridge.compare_result", "compare_result", "SBLR_BRIDGE_COMPARE_RESULT", True),
    BridgeCommandSurface("cutover", "bridge_cutover", "BRIDGE CUTOVER fb_remote", "bridge.cutover", "cutover", "SBLR_BRIDGE_CUTOVER", True),
    BridgeCommandSurface("validate_bridge", "bridge_validate_connection", "VALIDATE BRIDGE CONNECTION fb_remote", "bridge.validate", "validate", "SBLR_BRIDGE_VALIDATE", False, expected_udr_operation="describe_capabilities"),
    BridgeCommandSurface("physical_page_copy_denied", "bridge_stream_open_physical_page_copy_denied", "BRIDGE STREAM OPEN PHYSICAL PAGE COPY fb_remote", "bridge.stream_open", "stream_open", "SBLR_BRIDGE_STREAM_OPEN", True, udr_request_suffix=";stream_kind=physical_page_copy", expected_refusal_code="UDR.BRIDGE.SANDBOX_DENIED"),
    BridgeCommandSurface("cluster_route_stub", "bridge_cluster_route_stub", "BRIDGE CLUSTER ROUTE fb_remote", "bridge.cluster_route", "cluster.route", "SBLR_BRIDGE_VALIDATE", True, cluster_route=True, expected_refusal_code="UDR.BRIDGE.UNSUPPORTED"),
)


BRIDGE_COMMAND_BY_SURFACE_ID = {
    row.surface_id: row for row in BRIDGE_COMMAND_SURFACES
}
