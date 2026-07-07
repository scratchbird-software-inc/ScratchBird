# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Deterministic compile-repair helpers for recoverable query wrappers."""

from __future__ import annotations

import re
from dataclasses import dataclass


_CODE_FENCE_RE = re.compile(
    r"```(?:\s*(?P<label>[A-Za-z0-9_+-]+))?\s*\n(?P<body>.*?)```",
    re.DOTALL,
)
_LEADING_LABEL_RE = re.compile(r"^\s*(sql|query)\s*:\s*", re.IGNORECASE)


@dataclass(slots=True, frozen=True)
class CompileRepairCandidate:
    strategy_id: str
    query_text: str


def build_compile_repair_candidates(
    query_text: str,
    *,
    max_attempts: int = 3,
) -> list[CompileRepairCandidate]:
    """Return bounded deterministic repair candidates for recoverable wrappers."""

    original = query_text
    candidates: list[CompileRepairCandidate] = [
        CompileRepairCandidate(strategy_id="original", query_text=original),
    ]
    seen = {original}

    def add(strategy_id: str, candidate: str) -> None:
        normalized = candidate.strip()
        if not normalized or normalized in seen:
            return
        seen.add(normalized)
        candidates.append(
            CompileRepairCandidate(strategy_id=strategy_id, query_text=normalized)
        )

    stripped = original.strip()
    if stripped:
        add("trim_whitespace", stripped)

    code_match = _CODE_FENCE_RE.search(original)
    if code_match:
        fenced_body = code_match.group("body").strip()
        add("strip_markdown_fence", fenced_body)

    leading_label_removed = _LEADING_LABEL_RE.sub("", stripped, count=1)
    if leading_label_removed != stripped:
        add("strip_leading_label", leading_label_removed)

    if code_match:
        fenced_body = code_match.group("body").strip()
        fenced_without_label = _LEADING_LABEL_RE.sub("", fenced_body, count=1)
        if fenced_without_label != fenced_body:
            add("strip_markdown_fence_and_label", fenced_without_label)

    return candidates[: max(1, max_attempts)]
