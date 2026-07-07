# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Dialect routing and capability gate enforcement."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .capability_matrix import get_dialect_capabilities, list_dialects


class RoutingError(RuntimeError):
    """Raised when dialect routing/capability checks fail."""


def _unsupported_dialect_message(dialect: str) -> str:
    return (
        f"Unsupported dialect '{dialect}'. "
        "ScratchBird AI supports ScratchBird-native AI workflows only."
    )


@dataclass(slots=True)
class DialectRouter:
    matrix: dict[str, Any]

    def available_dialects(self) -> list[str]:
        return list_dialects(self.matrix)

    def require_capability(self, dialect: str, capability: str) -> None:
        try:
            caps = get_dialect_capabilities(self.matrix, dialect)
        except KeyError as exc:
            raise RoutingError(_unsupported_dialect_message(dialect)) from exc
        value = caps.get(capability)
        if value is not True:
            status = caps.get("status", "unknown")
            raise RoutingError(
                f"Dialect '{dialect}' lacks required capability '{capability}' (status={status})"
            )

    def capabilities(self, dialect: str) -> dict[str, Any]:
        try:
            return get_dialect_capabilities(self.matrix, dialect)
        except KeyError as exc:
            raise RoutingError(_unsupported_dialect_message(dialect)) from exc
