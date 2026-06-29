# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird Mojo Driver
# Keepalive scaffolding in current Mojo syntax.
# Copyright (c) 2025-2026 Dalton Calford



struct KeepaliveConfig:
    var interval_ms: Int
    var max_idle_before_check_ms: Int
    var validation_timeout_ms: Int

    def __init__(out self):
        self.interval_ms = 120000
        self.max_idle_before_check_ms = 600000
        self.validation_timeout_ms = 5000

    def __init__(
        out self,
        interval_ms: Int,
        max_idle_before_check_ms: Int,
        validation_timeout_ms: Int,
    ):
        self.interval_ms = interval_ms
        self.max_idle_before_check_ms = max_idle_before_check_ms
        self.validation_timeout_ms = validation_timeout_ms


struct KeepaliveTracker:
    var interval_ms: Int
    var max_idle_before_check_ms: Int
    var validation_timeout_ms: Int
    var last_activity_ms: Int

    def __init__(out self, config: KeepaliveConfig):
        self.interval_ms = config.interval_ms
        self.max_idle_before_check_ms = config.max_idle_before_check_ms
        self.validation_timeout_ms = config.validation_timeout_ms
        self.last_activity_ms = 0

    def mark_active(mut self, now_ms: Int):
        if now_ms < 0:
            self.last_activity_ms = 0
            return
        self.last_activity_ms = now_ms

    def needs_validation(self, now_ms: Int) -> Bool:
        if now_ms <= self.last_activity_ms:
            return False
        var idle_ms = now_ms - self.last_activity_ms
        return idle_ms > self.max_idle_before_check_ms

    def idle_duration_ms(self, now_ms: Int) -> Int:
        if now_ms <= self.last_activity_ms:
            return 0
        return now_ms - self.last_activity_ms


def _find_connection_index(connection_ids: List[String], connection_id: String) -> Int:
    for i in range(len(connection_ids)):
        if connection_ids[i] == connection_id:
            return i
    return -1


struct KeepaliveManager:
    var interval_ms: Int
    var max_idle_before_check_ms: Int
    var validation_timeout_ms: Int
    var connection_ids: List[String]
    var last_activity_ms: List[Int]
    var validation_counts: List[Int]
    var running: Bool

    def __init__(out self, config: KeepaliveConfig = KeepaliveConfig()):
        self.interval_ms = config.interval_ms
        self.max_idle_before_check_ms = config.max_idle_before_check_ms
        self.validation_timeout_ms = config.validation_timeout_ms
        self.connection_ids = List[String]()
        self.last_activity_ms = List[Int]()
        self.validation_counts = List[Int]()
        self.running = False

    def start(mut self):
        self.running = True

    def stop(mut self):
        self.running = False

    def register(mut self, connection_id: String, now_ms: Int = 0) -> Int:
        var normalized_now = now_ms
        if normalized_now < 0:
            normalized_now = 0

        var index = _find_connection_index(self.connection_ids, connection_id)
        if index >= 0:
            self.last_activity_ms[index] = normalized_now
        else:
            self.connection_ids.append(connection_id)
            self.last_activity_ms.append(normalized_now)
            self.validation_counts.append(0)

        return len(self.connection_ids)

    def unregister(mut self, connection_id: String):
        var index = _find_connection_index(self.connection_ids, connection_id)
        if index < 0:
            return

        var kept_ids = List[String]()
        var kept_last = List[Int]()
        var kept_counts = List[Int]()

        for i in range(len(self.connection_ids)):
            if i == index:
                continue
            kept_ids.append(self.connection_ids[i])
            kept_last.append(self.last_activity_ms[i])
            kept_counts.append(self.validation_counts[i])

        self.connection_ids = kept_ids^
        self.last_activity_ms = kept_last^
        self.validation_counts = kept_counts^

    def mark_active(mut self, connection_id: String, now_ms: Int):
        var index = _find_connection_index(self.connection_ids, connection_id)
        if index < 0:
            return
        if now_ms < 0:
            self.last_activity_ms[index] = 0
            return
        self.last_activity_ms[index] = now_ms

    def due_for_validation(mut self, now_ms: Int) -> List[String]:
        var due = List[String]()
        for i in range(len(self.connection_ids)):
            var last_seen = self.last_activity_ms[i]
            if now_ms > last_seen and now_ms - last_seen > self.max_idle_before_check_ms:
                due.append(self.connection_ids[i])
                self.validation_counts[i] += 1
        return due^

    def validation_count(self, connection_id: String) -> Int:
        var index = _find_connection_index(self.connection_ids, connection_id)
        if index < 0:
            return 0
        return self.validation_counts[index]

    def get_monitored_count(self) -> Int:
        return len(self.connection_ids)
