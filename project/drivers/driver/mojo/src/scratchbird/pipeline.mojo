# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird Mojo Driver - Query pipeline scaffolding in current Mojo syntax.
# Copyright (c) 2025-2026 Dalton Calford

from collections import List


struct PipelineConfig:
    var max_in_flight: Int
    var auto_flush: Bool
    var auto_flush_threshold: Int
    var flush_timeout_ms: Int

    fn __init__(out self):
        self.max_in_flight = 100
        self.auto_flush = True
        self.auto_flush_threshold = 10
        self.flush_timeout_ms = 5000


struct QueryPipeline:
    var max_in_flight: Int
    var auto_flush: Bool
    var auto_flush_threshold: Int
    var flush_timeout_ms: Int
    var connection_id: String
    var running: Bool
    var pending_sql: List[String]
    var pending_param_count: List[Int]
    var in_flight: Int
    var completed_total: Int
    var failed_total: Int

    fn __init__(out self, config: PipelineConfig = PipelineConfig()):
        self.max_in_flight = config.max_in_flight
        self.auto_flush = config.auto_flush
        self.auto_flush_threshold = config.auto_flush_threshold
        self.flush_timeout_ms = config.flush_timeout_ms
        self.connection_id = ""
        self.running = False
        self.pending_sql = List[String]()
        self.pending_param_count = List[Int]()
        self.in_flight = 0
        self.completed_total = 0
        self.failed_total = 0

    fn start(mut self, connection_id: String):
        self.connection_id = connection_id
        self.running = True

    fn stop(mut self):
        self.running = False

    fn queue(mut self, sql: String, params: List[String]) -> Bool:
        if not self.has_capacity():
            self.failed_total += 1
            return False

        self.pending_sql.append(sql)
        self.pending_param_count.append(len(params))

        if self.auto_flush and len(self.pending_sql) >= self.auto_flush_threshold:
            self.flush()

        return True

    fn pending_count(self) -> Int:
        return len(self.pending_sql)

    fn in_flight_count(self) -> Int:
        return self.in_flight

    fn has_capacity(self) -> Bool:
        return self.in_flight + len(self.pending_sql) < self.max_in_flight

    fn completed_count(self) -> Int:
        return self.completed_total

    fn failed_count(self) -> Int:
        return self.failed_total

    fn flush(mut self):
        if not self.running:
            return

        var batch_sql = self.pending_sql.copy()
        var batch_param_count = self.pending_param_count.copy()
        self.pending_sql = List[String]()
        self.pending_param_count = List[Int]()

        self._process_batch(batch_sql^, batch_param_count^)

    fn _process_batch(mut self, batch_sql: List[String], batch_param_count: List[Int]):
        _ = batch_param_count
        self.in_flight += len(batch_sql)

        for i in range(len(batch_sql)):
            self._execute_request(batch_sql[i])

        self.in_flight -= len(batch_sql)

    fn _execute_request(mut self, sql: String):
        if not self.running:
            self.failed_total += 1
            return

        var normalized = sql.strip().lower()
        if normalized == "":
            self.failed_total += 1
            return

        self.completed_total += 1


struct PipelineBuilder:
    var queries: List[String]

    fn __init__(out self):
        self.queries = List[String]()

    fn add(mut self, sql: String):
        self.queries.append(sql)

    fn build(self) -> List[String]:
        return self.queries.copy()
