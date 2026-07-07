# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))

import scratchbird


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


class _FakeWireAuthMethodSurface:
    def __init__(self, wire_method: str, plugin_method_id: str, executable_locally: bool, broker_required: bool):
        self.wire_method = wire_method
        self.plugin_method_id = plugin_method_id
        self.executable_locally = executable_locally
        self.broker_required = broker_required


class _FakeWireProbeResult:
    def __init__(self) -> None:
        self.reachable = True
        self.ingress_mode = "direct"
        self.resolved_host = "wire.example"
        self.resolved_port = 43092
        self.admitted_methods = [
            _FakeWireAuthMethodSurface(
                "SCRAM_SHA_512",
                "scratchbird.auth.scram_sha_512",
                True,
                False,
            )
        ]
        self.required_method = "SCRAM_SHA_512"
        self.required_plugin_method_id = "scratchbird.auth.scram_sha_512"
        self.allowed_transport_mask = None
        self.additional_continuation_possible = True


class _FakeWireResolvedAuthContext:
    def __init__(self, attached: bool = True) -> None:
        self.ingress_mode = "direct"
        self.resolved_auth_method = "SCRAM_SHA_512"
        self.resolved_auth_plugin_id = "scratchbird.auth.scram_sha_512"
        self.manager_authenticated = False
        self.attached = attached


class _FakeWireConnection:
    def __init__(self, dsn: str) -> None:
        self.dsn = dsn
        self._txn_id = 0
        self._runtime_txn_active = False
        self._closed = False

    def get_resolved_auth_context(self) -> _FakeWireResolvedAuthContext:
        return _FakeWireResolvedAuthContext(attached=not self._closed)

    def ping(self) -> bool:
        return not self._closed

    def close(self) -> None:
        self._closed = True


class _FakeWireModule:
    def probe_auth_surface(self, *, dsn: str):
        _require("current_schema=users.public" in dsn, "wire probe should receive default session schema dsn")
        return _FakeWireProbeResult()

    def connect(self, *, dsn: str):
        _require("current_schema=users.public" in dsn, "wire connect should receive default session schema dsn")
        return _FakeWireConnection(dsn)


def _test_deterministic_direct_probe_defaults_to_password() -> None:
    probe = scratchbird.probe_auth_surface(
        scratchbird.ScratchBirdConfig("scratchbird://localhost:3092/testdb?sslmode=require")
    )
    _require(probe.reachable, "deterministic direct probe should be reachable")
    _require(probe.ingress_mode == "direct", f"unexpected ingress_mode: {probe.ingress_mode!r}")
    _require(probe.required_method == "PASSWORD", f"unexpected required_method: {probe.required_method!r}")
    _require(
        probe.required_plugin_method_id == "scratchbird.auth.password_compat",
        f"unexpected required_plugin_method_id: {probe.required_plugin_method_id!r}",
    )
    _require(len(probe.admitted_methods) == 1, "deterministic direct probe should expose one admitted method")
    admitted = probe.admitted_methods[0]
    _require(admitted.executable_locally, "password should be locally executable")
    _require(not admitted.broker_required, "password should not require a broker")


def _test_deterministic_peer_probe_fails_closed() -> None:
    probe = scratchbird.probe_auth_surface(
        "scratchbird://localhost:3092/testdb?sslmode=require&auth_method_id=scratchbird.auth.peer_uid"
    )
    _require(probe.required_method == "PEER", f"unexpected required_method: {probe.required_method!r}")
    admitted = probe.admitted_methods[0]
    _require(not admitted.executable_locally, "PEER should not be locally executable")
    _require(admitted.broker_required, "PEER should be broker-required")
    conn = scratchbird.connect(
        scratchbird.ScratchBirdConfig(
            "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&auth_method_id=scratchbird.auth.peer_uid"
        )
    )
    context = conn.get_resolved_auth_context()
    _require(context.resolved_auth_method == "PEER", "deterministic connection should preserve PEER truth")
    _require(context.attached, "deterministic connection should report attached before close")
    conn.close()
    closed = conn.get_resolved_auth_context()
    _require(not closed.attached, "deterministic connection should clear attached on close")


def _test_manager_probe_does_not_require_token() -> None:
    probe = scratchbird.probe_auth_surface(
        "scratchbird://localhost:3092/security?sslmode=require&front_door_mode=manager_proxy"
    )
    _require(probe.ingress_mode == "manager_proxy", f"unexpected ingress_mode: {probe.ingress_mode!r}")
    _require(probe.required_method == "TOKEN", f"unexpected required_method: {probe.required_method!r}")
    _require(
        probe.required_plugin_method_id == "scratchbird.auth.authkey_token",
        f"unexpected required_plugin_method_id: {probe.required_plugin_method_id!r}",
    )


def _test_python_wire_probe_and_context_delegate() -> None:
    original = scratchbird._PYTHON_DRIVER_MODULE
    try:
        scratchbird._PYTHON_DRIVER_MODULE = _FakeWireModule()
        dsn = "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&sb_wire_transport=python"
        probe = scratchbird.probe_auth_surface(dsn)
        _require(probe.required_method == "SCRAM_SHA_512", "wire probe should delegate required method")
        _require(probe.resolved_host == "wire.example", "wire probe should delegate host")
        conn = scratchbird.connect(scratchbird.ScratchBirdConfig(dsn))
        context = conn.get_resolved_auth_context()
        _require(context.resolved_auth_method == "SCRAM_SHA_512", "wire connect should delegate resolved auth method")
        _require(context.attached, "wire connect should report attached before close")
        conn.close()
        closed = conn.get_resolved_auth_context()
        _require(not closed.attached, "wire connect should clear attached on close")
    finally:
        scratchbird._PYTHON_DRIVER_MODULE = original


def main() -> None:
    _test_deterministic_direct_probe_defaults_to_password()
    _test_deterministic_peer_probe_fails_closed()
    _test_manager_probe_does_not_require_token()
    _test_python_wire_probe_and_context_delegate()
    print("auth_bootstrap_contract: OK")


if __name__ == "__main__":
    main()
