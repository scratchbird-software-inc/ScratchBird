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


def _grant_uuid(principal_uuid: str, right: str) -> str:
    digest = hashlib.sha256(f"{principal_uuid}:{right}".encode("utf-8")).hexdigest()
    return f"019f0a11-ce00-7000-8000-{digest[:12]}"


def _authorization_context_successor(authority_event: str, generation: int) -> str:
    if generation <= 0:
        raise ValueError("security context generation must be positive")
    fields = authority_event.split("\t")
    if len(fields) < 3 or fields[0] != "SBSECPL1":
        raise ValueError("security authority event is malformed")
    evidence = hashlib.sha256((authority_event + "\n").encode("utf-8")).hexdigest()
    return "\t".join(
        [
            "SBSECPL1",
            "AUTH_CONTEXT_SUCCESSOR",
            fields[2],
            str(generation),
            f"security-context-successor:v1:sha256:{evidence}",
        ]
    )


def _last_authorization_context_generation(path: Path) -> int:
    if not path.exists():
        return 0
    generation = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) < 2 or fields[:2] != ["SBSECPL1", "AUTH_CONTEXT_SUCCESSOR"]:
            continue
        if len(fields) != 5:
            raise ValueError("security context successor is malformed")
        next_generation = int(fields[3])
        if next_generation <= generation:
            raise ValueError("security context successor generation is not monotonic")
        generation = next_generation
    return generation


def _authority_events_with_successors(
    authority_events: list[str], starting_generation: int
) -> list[str]:
    committed: list[str] = []
    generation = starting_generation
    for authority_event in authority_events:
        generation += 1
        committed.append(authority_event)
        committed.append(_authorization_context_successor(authority_event, generation))
    return committed


def _grant_events(
    principal_uuid: str,
    authorization_tags: str,
    grantor_uuid: str | None = None,
    starting_generation: int = 2,
) -> list[str]:
    events: list[str] = []
    generation = starting_generation
    grantor = grantor_uuid or principal_uuid
    for tag in authorization_tags.split(","):
        tag = tag.strip()
        if not tag.startswith("right:"):
            continue
        right = tag.removeprefix("right:").strip()
        if not right:
            continue
        events.append(
            "\t".join(
                [
                    "SBSECPL1",
                    "GRANT",
                    "0",
                    _grant_uuid(principal_uuid, right),
                    principal_uuid,
                    "principal",
                    "",
                    "",
                    right,
                    grantor,
                    "allow",
                    str(generation),
                    "0",
                ]
            )
        )
        generation += 1
    return events


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
        "\n".join(_authority_events_with_successors([event], 0)) + "\n",
        encoding="utf-8",
    )


def write_local_password_auth_fixture(
    database: Path,
    principal: str,
    verifier: str,
    principal_uuid: str = DEFAULT_PRINCIPAL_UUID,
    authorization_tags: str = "right:CONNECT",
    append: bool = False,
) -> None:
    auth_mode = "a" if append else "w"
    with Path(str(database) + ".sb.local_password_auth").open(auth_mode, encoding="utf-8") as auth:
        auth.write(f"{principal}\tlocal_password\t{verifier}\n")
    event_path = Path(str(database) + ".sb.security_principal_events")
    prior_context_generation = (
        _last_authorization_context_generation(event_path) if append else 0
    )
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
            str(prior_context_generation + 1),
            "0",
        ]
    )
    authority_events = [
        event,
        *_grant_events(
            principal_uuid,
            authorization_tags,
            starting_generation=prior_context_generation + 2,
        ),
    ]
    committed_events = _authority_events_with_successors(
        authority_events, prior_context_generation
    )
    event_mode = "a" if append else "w"
    with event_path.open(event_mode, encoding="utf-8") as events:
        for committed_event in committed_events:
            events.write(committed_event + "\n")


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
