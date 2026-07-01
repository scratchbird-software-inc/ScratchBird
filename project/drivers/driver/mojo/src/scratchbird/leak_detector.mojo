# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird Mojo Driver - Leak detector scaffolding in current Mojo syntax.
# Copyright (c) 2025-2026 Dalton Calford


comptime LOG_DEBUG = 0
comptime LOG_WARN = 1
comptime LOG_ERROR = 2


struct LeakDetectionConfig:
    var threshold_ms: Int
    var capture_stack_trace: Bool
    var check_interval_ms: Int
    var log_level: Int

    def __init__(out self):
        self.threshold_ms = 30000
        self.capture_stack_trace = False
        self.check_interval_ms = 10000
        self.log_level = LOG_WARN


def _find_checkout_index(connection_ids: List[String], connection_id: String) -> Int:
    for i in range(len(connection_ids)):
        if connection_ids[i] == connection_id:
            return i
    return -1


struct LeakDetector:
    var threshold_ms: Int
    var capture_stack_trace: Bool
    var check_interval_ms: Int
    var log_level: Int
    var running: Bool
    var connection_ids: List[String]
    var checkout_times_ms: List[Int]
    var metadata_entries: List[String]
    var warnings: List[String]

    def __init__(out self):
        var config = LeakDetectionConfig()
        self.threshold_ms = config.threshold_ms
        self.capture_stack_trace = config.capture_stack_trace
        self.check_interval_ms = config.check_interval_ms
        self.log_level = config.log_level
        self.running = False
        self.connection_ids = List[String]()
        self.checkout_times_ms = List[Int]()
        self.metadata_entries = List[String]()
        self.warnings = List[String]()

    def __init__(out self, config: LeakDetectionConfig):
        self.threshold_ms = config.threshold_ms
        self.capture_stack_trace = config.capture_stack_trace
        self.check_interval_ms = config.check_interval_ms
        self.log_level = config.log_level
        self.running = False
        self.connection_ids = List[String]()
        self.checkout_times_ms = List[Int]()
        self.metadata_entries = List[String]()
        self.warnings = List[String]()

    def start(mut self):
        self.running = True

    def stop(mut self):
        self.running = False

    def checkout(mut self, connection_id: String, metadata: String = "", checkout_time_ms: Int = 0) -> String:
        var index = _find_checkout_index(self.connection_ids, connection_id)
        if index >= 0:
            self.checkout_times_ms[index] = checkout_time_ms
            self.metadata_entries[index] = metadata
        else:
            self.connection_ids.append(connection_id)
            self.checkout_times_ms.append(checkout_time_ms)
            self.metadata_entries.append(metadata)
        return connection_id

    def checkin(mut self, connection_id: String, now_ms: Int = 0) -> Int:
        var index = _find_checkout_index(self.connection_ids, connection_id)
        if index < 0:
            return 0

        var held_ms = 0
        if now_ms > self.checkout_times_ms[index]:
            held_ms = now_ms - self.checkout_times_ms[index]
        if held_ms > self.threshold_ms:
            self.warnings.append("conn=" + connection_id + ",held_ms=" + String(held_ms))

        var kept_ids = List[String]()
        var kept_times = List[Int]()
        var kept_metadata = List[String]()
        for i in range(len(self.connection_ids)):
            if i == index:
                continue
            kept_ids.append(self.connection_ids[i])
            kept_times.append(self.checkout_times_ms[i])
            kept_metadata.append(self.metadata_entries[i])

        self.connection_ids = kept_ids^
        self.checkout_times_ms = kept_times^
        self.metadata_entries = kept_metadata^
        return held_ms

    def release_checkout(mut self, token: String, now_ms: Int = 0) -> Int:
        return self.checkin(token, now_ms)

    def get_active_count(self) -> Int:
        return len(self.connection_ids)

    def check_leaks(mut self, now_ms: Int) -> List[String]:
        var leaks = List[String]()
        for i in range(len(self.connection_ids)):
            var held_ms = 0
            if now_ms > self.checkout_times_ms[i]:
                held_ms = now_ms - self.checkout_times_ms[i]
            if held_ms > self.threshold_ms:
                var summary = "conn=" + self.connection_ids[i] + ",held_ms=" + String(held_ms)
                leaks.append(summary)
                self.warnings.append(summary)
        return leaks^

    def get_warnings(self) -> List[String]:
        return self.warnings.copy()

    def clear_warnings(mut self):
        self.warnings = List[String]()
