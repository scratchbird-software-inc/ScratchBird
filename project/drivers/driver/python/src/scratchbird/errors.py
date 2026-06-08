# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DB-API 2.0 error hierarchy and MGA retry-scope helpers."""

from __future__ import annotations

import re
from typing import Optional, Union


class Warning(Exception):
    pass


class Error(Exception):
    pass


class InterfaceError(Error):
    pass


class DatabaseError(Error):
    pass


class DataError(DatabaseError):
    pass


class OperationalError(DatabaseError):
    pass


class IntegrityError(DatabaseError):
    pass


class InternalError(DatabaseError):
    pass


class ProgrammingError(DatabaseError):
    pass


class NotSupportedError(DatabaseError):
    pass


RETRY_SCOPE_NONE = "none"
RETRY_SCOPE_RECONNECT = "reconnect"
RETRY_SCOPE_STATEMENT = "statement"
RETRY_SCOPE_TRANSACTION = "transaction"

_SQLSTATE_PREFIX_RE = re.compile(r"^\[([0-9A-Z]{5})\]")


def extract_sqlstate(error: Union[BaseException, str, None]) -> Optional[str]:
    """Extract a SQLSTATE prefix from an exception or message string."""

    if error is None:
        return None
    text = str(error)
    match = _SQLSTATE_PREFIX_RE.match(text)
    if match:
        return match.group(1)
    return None


def retry_scope_for_sqlstate(sqlstate: Optional[str]) -> str:
    """Return the allowed retry boundary for a SQLSTATE.

    ScratchBird's MGA contract is fail-closed:

    - class 40 (`40001`, `40P01`) means restart from a fresh statement boundary
    - class 08 means reconnect or reopen only
    - cancel / operator intervention (`57014`) is not auto-retryable here
    - no SQLSTATE currently authorizes whole-transaction replay in the driver
    """

    if not sqlstate or len(sqlstate) != 5:
        return RETRY_SCOPE_NONE
    if sqlstate in {"40001", "40P01"}:
        return RETRY_SCOPE_STATEMENT
    if sqlstate.startswith("08"):
        return RETRY_SCOPE_RECONNECT
    return RETRY_SCOPE_NONE


def is_retryable_sqlstate(sqlstate: Optional[str]) -> bool:
    """Return whether a SQLSTATE is retryable from a fresh boundary."""

    return retry_scope_for_sqlstate(sqlstate) != RETRY_SCOPE_NONE
