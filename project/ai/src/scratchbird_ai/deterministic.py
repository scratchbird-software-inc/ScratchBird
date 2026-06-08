# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Deterministic serialization and hashing helpers."""

from __future__ import annotations

import hashlib
import json
import math
from datetime import date, datetime, time
from decimal import Decimal
from typing import Any


def _normalize_number(value: float) -> int | float:
    """Normalize float representation to avoid NaN/inf and negative zero drift."""
    if not math.isfinite(value):
        raise ValueError("non-finite float values are not permitted")
    if value == 0.0:
        return 0
    return value


def normalize_for_hash(value: Any) -> Any:
    """Normalize Python values into deterministic JSON-safe forms.

    This is intentionally strict and engine-agnostic. It is sufficient for
    repeatable hashing contracts used in this repository.
    """
    if value is None or isinstance(value, (str, bool, int)):
        return value
    if isinstance(value, float):
        return _normalize_number(value)
    if isinstance(value, Decimal):
        # RFC8785-compatible numeric output is outside stdlib; keep deterministic.
        return str(value.normalize())
    if isinstance(value, (datetime, date, time)):
        return value.isoformat()
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, list):
        return [normalize_for_hash(item) for item in value]
    if isinstance(value, tuple):
        return [normalize_for_hash(item) for item in value]
    if isinstance(value, dict):
        normalized_items = {
            str(key): normalize_for_hash(val) for key, val in value.items()
        }
        return dict(sorted(normalized_items.items(), key=lambda kv: kv[0]))
    return str(value)


def canonical_json(value: Any) -> str:
    """Return deterministic JSON string for hashing."""
    normalized = normalize_for_hash(value)
    return json.dumps(
        normalized,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    )


def sha256_hex(value: str | bytes) -> str:
    if isinstance(value, str):
        payload = value.encode("utf-8")
    else:
        payload = value
    return hashlib.sha256(payload).hexdigest()


def deterministic_id(prefix: str, payload: Any, *, max_hex_chars: int = 24) -> str:
    digest = sha256_hex(canonical_json(payload))
    return f"{prefix}_{digest[:max_hex_chars]}"
