# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Cursor implementation for ScratchBird Python driver."""

from __future__ import annotations

import re
from typing import Iterable, List, Optional

from . import errors
from .sql import normalize_callable_query


class GeneratedKeysResultSet:
    _DESCRIPTION = [
        (
            "GENERATED_KEY",
            20,
            None,
            None,
            None,
            None,
            True,
        )
    ]

    def __init__(self, rows):
        self._rows = list(rows)
        self._pos = 0
        self.description = list(self._DESCRIPTION)

    @property
    def rowcount(self) -> int:
        return len(self._rows)

    def fetchone(self):
        if self._pos >= len(self._rows):
            return None
        row = self._rows[self._pos]
        self._pos += 1
        return row

    def fetchmany(self, size: int = 1):
        if size <= 0:
            return []
        rows = []
        while len(rows) < size:
            row = self.fetchone()
            if row is None:
                break
            rows.append(row)
        return rows

    def fetchall(self):
        if self._pos >= len(self._rows):
            return []
        rows = self._rows[self._pos :]
        self._pos = len(self._rows)
        return list(rows)


class Cursor:
    _BATCHABLE_INSERT_RE = re.compile(
        r"^\s*(INSERT\s+INTO\b.+?\bVALUES\s*)\(([^()]*)\)\s*;?\s*$",
        re.IGNORECASE | re.DOTALL,
    )
    _DEFAULT_EXECUTEMANY_BATCH_ROWS = 1024
    # Keep multi-row VALUES batches below the current native front-door
    # invalid-query boundary observed in the bounded stress probes.
    _MAX_EXECUTEMANY_BATCH_PARAMS = 6144
    _MAX_EXECUTEMANY_BATCH_SQL_BYTES = 256 * 1024

    def __init__(self, connection):
        self._connection = connection
        self._closed = False
        self._results: List = []
        self._result_sets: List[dict] = []
        self._result_set_index = -1
        self._pos = 0
        self._stream = None
        self.description = None
        self.rowcount = -1
        self.arraysize = 1
        self.lastrowid = None
        self.statusmessage = None
        self._generated_keys: List[tuple] = []
        self._last_completion_count = 0

    def execute(self, sql: str, params=None) -> None:
        self._ensure_open()
        if sql is None:
            raise errors.ProgrammingError("sql is required")
        self._reset_state()
        split_results = self._connection._execute_multi_statement_query(sql, params)
        if split_results is not None:
            self._load_buffered_result_sets(split_results)
            return
        page_size = self.arraysize if self.arraysize and self.arraysize > 1 else 0
        self._stream = self._connection._execute_query(sql, params, page_size)
        self._prime_stream_metadata(self._stream)
        self._results = []
        self._pos = 0
        self._update_description(self._stream)
        self.rowcount = -1
        self.statusmessage = None

    def executemany(self, sql: str, seq_of_params: Iterable) -> None:
        self._ensure_open()
        if sql is None:
            raise errors.ProgrammingError("sql is required")
        if seq_of_params is None:
            raise errors.ProgrammingError("seq_of_params is required")
        self._reset_state()
        total = 0
        rowcount_known = True
        page_size = self.arraysize if self.arraysize and self.arraysize > 1 else 0
        batched_insert = self._build_batched_insert(sql)
        if batched_insert is not None:
            statement_prefix, tuple_sql, placeholder_count = batched_insert
            max_batch_rows = self._effective_batched_insert_rows(
                statement_prefix,
                tuple_sql,
                placeholder_count,
            )
            batch_rows: List = []
            for params in seq_of_params:
                batch_rows.append(params)
                if len(batch_rows) < max_batch_rows:
                    continue

                rowcount = self._execute_batched_insert(
                    statement_prefix,
                    tuple_sql,
                    placeholder_count,
                    batch_rows,
                    page_size,
                )
                if rowcount is None or rowcount < 0:
                    rowcount_known = False
                else:
                    total += rowcount
                batch_rows = []

            if batch_rows:
                rowcount = self._execute_batched_insert(
                    statement_prefix,
                    tuple_sql,
                    placeholder_count,
                    batch_rows,
                    page_size,
                )
                if rowcount is None or rowcount < 0:
                    rowcount_known = False
                else:
                    total += rowcount

            self.rowcount = total if rowcount_known else -1
            return

        for params in seq_of_params:
            stream = self._connection._execute_query(sql, params, page_size)
            self._prime_stream_metadata(stream)
            self._stream = stream
            self._results = []
            self._pos = 0
            self._update_description(stream)
            rowcount = self._drain_stream(stream)
            if rowcount is None or rowcount < 0:
                rowcount_known = False
            else:
                total += rowcount
        self.rowcount = total if rowcount_known else -1

    def callproc(self, procname: str, params=None):
        self._ensure_open()
        if not isinstance(procname, str) or not procname.strip():
            raise errors.ProgrammingError("procname is required")

        routine = procname.strip()
        if params is None:
            sql = f"{{call {routine}}}"
            call_params = []
            returned = []
        elif isinstance(params, dict):
            placeholders = ", ".join(f":{key}" for key in params.keys())
            sql = f"{{call {routine}({placeholders})}}" if placeholders else f"{{call {routine}}}"
            call_params = params
            returned = params
        else:
            values = list(params)
            placeholders = ", ".join("?" for _ in values)
            sql = f"{{call {routine}({placeholders})}}" if placeholders else f"{{call {routine}}}"
            call_params = values
            returned = values

        try:
            normalized_sql, ordered_params = normalize_callable_query(sql, call_params)
        except ValueError as exc:
            raise errors.ProgrammingError(str(exc)) from exc

        self._reset_state()
        page_size = self.arraysize if self.arraysize and self.arraysize > 1 else 0
        self._stream = self._connection._execute_query(normalized_sql, ordered_params, page_size)
        self._prime_stream_metadata(self._stream)
        self._results = []
        self._pos = 0
        self._update_description(self._stream)
        self.rowcount = -1
        self.lastrowid = None
        self.statusmessage = None
        return returned

    def __iter__(self):
        return self

    def __next__(self):
        row = self.fetchone()
        if row is None:
            raise StopIteration
        return row

    def fetchone(self):
        self._ensure_open()
        if self._pos < len(self._results):
            row = self._results[self._pos]
            self._pos += 1
            return row
        if self._stream is None:
            return None
        row = self._stream.read_row()
        self._update_description(self._stream)
        if row is None:
            if self._stream.rowcount is not None and self._stream.rowcount >= 0:
                self.rowcount = self._stream.rowcount
            self.lastrowid = getattr(self._stream, "lastrowid", None)
            self.statusmessage = getattr(self._stream, "command", None)
            completion_count = getattr(self._stream, "completion_count", 0)
            if completion_count > self._last_completion_count:
                self._last_completion_count = completion_count
                self._capture_generated_key(self.lastrowid)
            return None
        return row

    def fetchmany(self, size: Optional[int] = None) -> List:
        self._ensure_open()
        if size is None:
            size = self.arraysize
        if size <= 0:
            return []
        rows = []
        while len(rows) < size:
            row = self.fetchone()
            if row is None:
                break
            rows.append(row)
        return rows

    def fetchall(self) -> List:
        self._ensure_open()
        rows = []
        while True:
            row = self.fetchone()
            if row is None:
                break
            rows.append(row)
        return rows

    def nextset(self):
        self._ensure_open()
        if self._result_sets:
            next_index = self._result_set_index + 1
            if next_index >= len(self._result_sets):
                return None
            self._load_buffered_result_set(next_index)
            return True
        if self._stream is None:
            return None
        while self._stream.read_row() is not None:
            continue
        if not self._stream.has_next_result_set():
            return None
        if not self._stream.next_result_set():
            return None
        self._results = []
        self._pos = 0
        self.description = None
        self.rowcount = -1
        self.lastrowid = None
        self.statusmessage = None
        return True

    def close(self) -> None:
        self._closed = True

    def get_generated_keys(self) -> GeneratedKeysResultSet:
        self._ensure_open()
        return GeneratedKeysResultSet(self._generated_keys)

    def setinputsizes(self, sizes) -> None:
        self._ensure_open()

    def setoutputsize(self, size, column=None) -> None:
        self._ensure_open()

    def _ensure_open(self) -> None:
        if self._closed:
            raise errors.InterfaceError("cursor is closed")

    def _reset_state(self) -> None:
        if self._stream is not None:
            self._discard_pending_stream(self._stream)
        self._results = []
        self._result_sets = []
        self._result_set_index = -1
        self._pos = 0
        self._stream = None
        self.description = None
        self.rowcount = -1
        self.lastrowid = None
        self.statusmessage = None
        self._generated_keys = []
        self._last_completion_count = 0

    def _load_buffered_result_sets(self, result_sets: List[dict]) -> None:
        self._result_sets = list(result_sets)
        self._load_buffered_result_set(0)

    def _load_buffered_result_set(self, index: int) -> None:
        result_set = self._result_sets[index]
        self._result_set_index = index
        self._results = list(result_set.get("rows", []))
        self._pos = 0
        self._stream = None
        self.description = result_set.get("fields") or []
        self.rowcount = result_set.get("rowCount", -1)
        self.lastrowid = result_set.get("lastId")
        self.statusmessage = result_set.get("command")
        self._capture_generated_key(self.lastrowid)

    def _update_description(self, stream) -> None:
        if self.description is not None:
            return
        if stream is None or not getattr(stream, "columns", None):
            return
        self.description = [
            (
                col.name,
                col.type_oid,
                None,
                col.type_modifier or None,
                None,
                None,
                col.nullable,
            )
            for col in stream.columns
        ]

    def _prime_stream_metadata(self, stream) -> None:
        if stream is None:
            return
        prime = getattr(stream, "prime_metadata", None)
        if callable(prime):
            prime()

    def _drain_stream(self, stream):
        count = stream.rowcount
        while True:
            row = stream.read_row()
            if row is None:
                break
        self.lastrowid = getattr(stream, "lastrowid", None)
        self.statusmessage = getattr(stream, "command", None)
        self._capture_generated_key(self.lastrowid)
        return stream.rowcount if stream.rowcount is not None else count

    def _discard_pending_stream(self, stream) -> None:
        while stream is not None:
            while stream.read_row() is not None:
                continue
            has_next = getattr(stream, "has_next_result_set", None)
            next_result = getattr(stream, "next_result_set", None)
            if callable(has_next) and has_next() and callable(next_result) and next_result():
                continue
            return

    def _build_batched_insert(self, sql: str):
        match = self._BATCHABLE_INSERT_RE.match(sql)
        if match is None:
            return None

        tuple_body = match.group(2).strip()
        placeholder_count = tuple_body.count("?")
        if placeholder_count <= 0:
            return None

        # Keep batching conservative and only optimize qmark-style single-row
        # VALUES clauses. More complex shapes still use the row-wise fallback.
        if re.search(r":[A-Za-z_]", tuple_body):
            return None

        statement_prefix = match.group(1).rstrip()
        tuple_sql = f"({tuple_body})"
        return statement_prefix, tuple_sql, placeholder_count

    def _effective_batched_insert_rows(
        self,
        statement_prefix: str,
        tuple_sql: str,
        placeholder_count: int,
    ) -> int:
        if placeholder_count <= 0:
            return 1
        max_rows_by_param_count = max(
            1,
            self._MAX_EXECUTEMANY_BATCH_PARAMS // placeholder_count,
        )
        base_sql_bytes = len(statement_prefix) + 1
        per_row_sql_bytes = len(tuple_sql) + 2
        remaining_sql_bytes = max(1, self._MAX_EXECUTEMANY_BATCH_SQL_BYTES - base_sql_bytes)
        max_rows_by_sql_bytes = max(1, remaining_sql_bytes // max(1, per_row_sql_bytes))
        return max(
            1,
            min(
                self._DEFAULT_EXECUTEMANY_BATCH_ROWS,
                max_rows_by_param_count,
                max_rows_by_sql_bytes,
            ),
        )

    def _execute_batched_insert(
        self,
        statement_prefix: str,
        tuple_sql: str,
        placeholder_count: int,
        batch_rows: List,
        page_size: int,
    ):
        flattened = []
        for params in batch_rows:
            if not isinstance(params, (list, tuple)):
                return self._execute_batched_insert_fallback(
                    f"{statement_prefix} {tuple_sql}",
                    batch_rows,
                    page_size,
                )
            if len(params) != placeholder_count:
                raise errors.ProgrammingError(
                    f"expected {placeholder_count} parameters per row, got {len(params)}"
                )
            flattened.extend(params)

        batch_sql = f"{statement_prefix} {', '.join([tuple_sql] * len(batch_rows))}"
        if getattr(self._connection, "_batched_insert_statement_cache", False):
            stream = self._connection._execute_cached_query_shape(batch_sql, flattened, page_size)
        else:
            stream = self._connection._execute_query(batch_sql, flattened, page_size)
        self._prime_stream_metadata(stream)
        self._stream = stream
        self._results = []
        self._pos = 0
        self._update_description(stream)
        return self._drain_stream(stream)

    def _execute_batched_insert_fallback(self, sql: str, batch_rows: List, page_size: int):
        total = 0
        for params in batch_rows:
            stream = self._connection._execute_query(sql, params, page_size)
            self._prime_stream_metadata(stream)
            self._stream = stream
            self._results = []
            self._pos = 0
            self._update_description(stream)
            rowcount = self._drain_stream(stream)
            if rowcount is not None and rowcount >= 0:
                total += rowcount
        return total

    def _capture_generated_key(self, key) -> None:
        if key is None:
            return
        try:
            normalized = int(key)
        except (TypeError, ValueError):
            return
        self._generated_keys.append((normalized,))

    @property
    def connection(self):
        return self._connection
