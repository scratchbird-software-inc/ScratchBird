#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import json
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parent
REQUIRED = (
    "compose.yaml",
    "lab.sh",
    "smoke.sh",
    "scripts/browser_webauthn_smoke.py",
    "scripts/generate_pki.sh",
    "fixtures/keycloak/scratchbird-realm.json",
    "fixtures/openldap/10-scratchbird.ldif",
    "fixtures/freeradius/clients.conf",
    "fixtures/freeradius/authorize",
    "fixtures/mtls/nginx.conf",
    "fixtures/proxy_assertion/Containerfile",
    "fixtures/proxy_assertion/server.py",
    "fixtures/samba_ad/Containerfile",
    "fixtures/samba_ad/entrypoint.sh",
    "fixtures/unix_auth/Containerfile",
    "fixtures/spire/server.conf",
    "fixtures/spire/agent.conf",
)


def fail(message: str) -> None:
    print(f"authentication_lab_definition_error={message}", file=sys.stderr)
    raise SystemExit(1)


for relative_path in REQUIRED:
    if not (ROOT / relative_path).is_file():
        fail(f"missing:{relative_path}")

compose = (ROOT / "compose.yaml").read_text(encoding="utf-8")
for forbidden in (":latest\n", ":latest}", "network_mode: host", "privileged: true"):
    if forbidden in compose:
        fail(f"forbidden_compose_setting:{forbidden.strip()}")

expected_services = {
    "keycloak",
    "openldap",
    "radius",
    "mtls",
    "proxy-assertion",
    "toxiproxy",
    "samba-ad",
    "unix-auth",
    "selenium",
    "spire-server",
    "spire-agent",
}
service_names = set(re.findall(r"^  ([a-z][a-z0-9-]+):$", compose, re.MULTILINE))
if not expected_services.issubset(service_names):
    fail("missing_services:" + ",".join(sorted(expected_services - service_names)))

realm = json.loads(
    (ROOT / "fixtures/keycloak/scratchbird-realm.json").read_text(encoding="utf-8")
)
if realm.get("realm") != "scratchbird":
    fail("keycloak_realm_name")
users = {user.get("username") for user in realm.get("users", [])}
required_users = {"alice", "admin-alice", "mfa-alice", "disabled-alice"}
if not required_users.issubset(users):
    fail("keycloak_fixture_users")
clients = {client.get("clientId") for client in realm.get("clients", [])}
if not {"scratchbird-cli", "urn:scratchbird:test:saml"}.issubset(clients):
    fail("keycloak_fixture_clients")

ldap = (ROOT / "fixtures/openldap/10-scratchbird.ldif").read_text(encoding="utf-8")
for expected in ("uid=alice", "cn=database-users", "cn=nested-groups"):
    if expected not in ldap:
        fail(f"ldap_fixture:{expected}")

print("authentication_lab_static_definition=passed")
