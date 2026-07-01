# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird Mojo Driver - Circuit breaker scaffolding in current Mojo syntax.
# Copyright (c) 2025-2026 Dalton Calford

comptime STATE_CLOSED = 0
comptime STATE_OPEN = 1
comptime STATE_HALF_OPEN = 2


struct CircuitBreakerConfig:
    var failure_threshold: Int
    var recovery_timeout_ms: Int
    var success_threshold: Int
    var half_open_max_requests: Int

    def __init__(out self):
        self.failure_threshold = 5
        self.recovery_timeout_ms = 30000
        self.success_threshold = 3
        self.half_open_max_requests = 10


struct CircuitBreakerError:
    var message: String

    def __init__(out self, message: String = "Circuit breaker is OPEN"):
        self.message = message


struct CircuitBreaker:
    var failure_threshold: Int
    var recovery_timeout_ms: Int
    var success_threshold: Int
    var half_open_max_requests: Int
    var name: String
    var state: Int
    var failure_count: Int
    var success_count: Int
    var half_open_requests: Int
    var last_failure_time_ms: Int

    def __init__(out self):
        var config = CircuitBreakerConfig()
        self.failure_threshold = config.failure_threshold
        self.recovery_timeout_ms = config.recovery_timeout_ms
        self.success_threshold = config.success_threshold
        self.half_open_max_requests = config.half_open_max_requests
        self.name = "default"
        self.state = STATE_CLOSED
        self.failure_count = 0
        self.success_count = 0
        self.half_open_requests = 0
        self.last_failure_time_ms = 0

    def __init__(out self, config: CircuitBreakerConfig, name: String = "default"):
        self.failure_threshold = config.failure_threshold
        self.recovery_timeout_ms = config.recovery_timeout_ms
        self.success_threshold = config.success_threshold
        self.half_open_max_requests = config.half_open_max_requests
        self.name = name
        self.state = STATE_CLOSED
        self.failure_count = 0
        self.success_count = 0
        self.half_open_requests = 0
        self.last_failure_time_ms = 0

    def get_state(self) -> Int:
        return self.state

    def allow_request(mut self, now_ms: Int = 0) -> Bool:
        if self.state == STATE_CLOSED:
            return True

        if self.state == STATE_OPEN:
            if (
                now_ms > 0
                and self.last_failure_time_ms > 0
                and now_ms - self.last_failure_time_ms >= self.recovery_timeout_ms
            ):
                self.state = STATE_HALF_OPEN
                self.failure_count = 0
                self.success_count = 0
                self.half_open_requests = 0
                return self._allow_half_open()
            return False

        return self._allow_half_open()

    def _allow_half_open(mut self) -> Bool:
        if self.half_open_requests < self.half_open_max_requests:
            self.half_open_requests += 1
            return True
        return False

    def record_success(mut self):
        if self.state == STATE_CLOSED:
            self.failure_count = 0
            return

        if self.state == STATE_HALF_OPEN:
            if self.half_open_requests > 0:
                self.half_open_requests -= 1
            self.success_count += 1
            if self.success_count >= self.success_threshold:
                self.state = STATE_CLOSED
                self.failure_count = 0
                self.success_count = 0
                self.half_open_requests = 0

    def record_failure(mut self, now_ms: Int = 0):
        if self.state == STATE_CLOSED:
            self.failure_count += 1
            if self.failure_count >= self.failure_threshold:
                self.state = STATE_OPEN
                self.last_failure_time_ms = now_ms
            return

        if self.state == STATE_HALF_OPEN:
            if self.half_open_requests > 0:
                self.half_open_requests -= 1
            self.state = STATE_OPEN
            self.success_count = 0
            self.last_failure_time_ms = now_ms
            return

        if self.state == STATE_OPEN:
            self.last_failure_time_ms = now_ms

    def reset(mut self):
        self.state = STATE_CLOSED
        self.failure_count = 0
        self.success_count = 0
        self.half_open_requests = 0
        self.last_failure_time_ms = 0

    def is_open(self) -> Bool:
        return self.state == STATE_OPEN

    def is_half_open(self) -> Bool:
        return self.state == STATE_HALF_OPEN
