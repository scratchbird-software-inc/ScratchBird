# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Base adapter interfaces for compile/execute/metadata paths."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Protocol


@dataclass(slots=True)
class AdapterCompileResult:
    statement_kind: str
    sblr_hash: str
    diagnostics: list[str]
    warnings: list[str]


@dataclass(slots=True)
class AdapterExecuteResult:
    rows: list[dict[str, Any]]
    notices: list[str]


class CompilerAdapter(Protocol):
    def compile_query(self, query_text: str, context: dict[str, Any]) -> AdapterCompileResult:
        ...


class ExecutorAdapter(Protocol):
    def execute_compiled(
        self,
        *,
        compile_artifact_id: str,
        query_text: str,
        options: dict[str, Any],
    ) -> AdapterExecuteResult:
        ...


class MetadataAdapter(Protocol):
    def list_schemas(self, database: str | None = None) -> list[str]:
        ...

    def list_tables(self, schema: str) -> list[str]:
        ...

    def describe_table(self, schema: str, table: str) -> dict[str, Any]:
        ...


class DialectAdapter(Protocol):
    @property
    def dialect(self) -> str:
        ...

    @property
    def compiler(self) -> CompilerAdapter:
        ...

    @property
    def executor(self) -> ExecutorAdapter:
        ...

    @property
    def metadata(self) -> MetadataAdapter:
        ...
