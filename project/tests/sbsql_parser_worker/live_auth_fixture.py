#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Durable authentication fixtures for live SBsql route tests."""

from __future__ import annotations

import hashlib
import hmac
from pathlib import Path


DEFAULT_PRINCIPAL_UUID = "019f0a11-ce00-7000-8000-000000000001"


def _hex_text(value: str) -> str:
    return value.encode("utf-8").hex()


def local_password_fingerprint(verifier: str) -> str:
    digest = hashlib.sha256(verifier.encode("utf-8")).hexdigest()
    return f"local-password-verifier:v1:sha256:{digest}"


def temporary_token_fingerprint(
    token: str,
    token_handle: str,
    state: str = "active",
    expires_at_ms: str = "0",
) -> str:
    digest = hashlib.sha256(token.encode("utf-8")).hexdigest()
    payload = f"{digest}|{state or 'active'}|{expires_at_ms or '0'}".encode("utf-8")
    mac = hmac.new(token_handle.encode("utf-8"), payload, hashlib.sha256).hexdigest()
    return f"security-temporary-token:v1:hmac-sha256:{mac}"


def write_temporary_token_auth_fixture(
    database: Path,
    principal: str,
    token: str,
    token_handle: str,
    principal_uuid: str = DEFAULT_PRINCIPAL_UUID,
    state: str = "active",
    expires_at_ms: str = "0",
) -> None:
    event = "\t".join(
        [
            "SBSECPL1",
            "PRINCIPAL",
            "0",
            principal_uuid,
            _hex_text(principal),
            "user",
            state,
            _hex_text(temporary_token_fingerprint(token, token_handle, state, expires_at_ms)),
            "1",
            "0",
        ]
    )
    Path(str(database) + ".sb.security_principal_events").write_text(
        event + "\n", encoding="utf-8"
    )


def write_local_password_auth_fixture(
    database: Path,
    principal: str,
    verifier: str,
    principal_uuid: str = DEFAULT_PRINCIPAL_UUID,
    append: bool = False,
) -> None:
    auth_mode = "a" if append else "w"
    with Path(str(database) + ".sb.local_password_auth").open(auth_mode, encoding="utf-8") as auth:
        auth.write(f"{principal}\tlocal_password\t{verifier}\n")
    event = "\t".join(
        [
            "SBSECPL1",
            "PRINCIPAL",
            "0",
            principal_uuid,
            _hex_text(principal),
            "user",
            "active",
            _hex_text(local_password_fingerprint(verifier)),
            "1",
            "0",
        ]
    )
    event_mode = "a" if append else "w"
    with Path(str(database) + ".sb.security_principal_events").open(event_mode, encoding="utf-8") as events:
        events.write(event + "\n")


def local_password_evidence(
    principal: str,
    verifier: str,
    principal_uuid: str = DEFAULT_PRINCIPAL_UUID,
    authorization_tags: str = "right:CONNECT",
) -> str:
    evidence = (
        f"scheme=local_password_v1;principal={principal};"
        f"principal_uuid={principal_uuid};"
        "storage_authority=mga_security_principal_lifecycle"
    )
    if authorization_tags:
        evidence += f";authorization_tags={authorization_tags}"
    evidence += f";verifier={verifier}"
    return evidence
