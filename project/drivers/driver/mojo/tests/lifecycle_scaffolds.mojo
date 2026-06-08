# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from collections import List
import circuit_breaker
import keepalive
import leak_detector
import pipeline
import telemetry


fn _require(condition: Bool, message: String) raises:
    if not condition:
        raise Error(message)


fn main() raises:
    var keepalive_cfg = keepalive.KeepaliveConfig(100, 1000, 50)
    var tracker = keepalive.KeepaliveTracker(keepalive_cfg)
    tracker.mark_active(10)
    _require(not tracker.needs_validation(500), "keepalive tracker should not validate early")
    _require(tracker.needs_validation(1200), "keepalive tracker should validate after idle budget")

    var manager = keepalive.KeepaliveManager(keepalive_cfg)
    manager.start()
    _ = manager.register("conn_a", 0)
    _ = manager.register("conn_b", 500)
    var due = manager.due_for_validation(1600)
    _require(len(due) == 2 and due[0] == "conn_a" and due[1] == "conn_b", "keepalive manager due set mismatch")
    manager.mark_active("conn_a", 1700)
    _require(manager.validation_count("conn_a") == 1, "keepalive validation counter mismatch")
    manager.unregister("conn_b")
    _require(manager.get_monitored_count() == 1, "keepalive unregister mismatch")
    manager.stop()

    var cb_cfg = circuit_breaker.CircuitBreakerConfig()
    cb_cfg.failure_threshold = 2
    cb_cfg.recovery_timeout_ms = 50
    cb_cfg.success_threshold = 2
    cb_cfg.half_open_max_requests = 1
    var breaker = circuit_breaker.CircuitBreaker(cb_cfg, "lane_smoke")
    _require(breaker.get_state() == circuit_breaker.STATE_CLOSED, "circuit breaker initial state mismatch")
    _require(breaker.allow_request(0), "closed circuit should allow requests")
    breaker.record_failure(10)
    _require(breaker.get_state() == circuit_breaker.STATE_CLOSED, "single failure should keep circuit closed")
    breaker.record_failure(20)
    _require(breaker.get_state() == circuit_breaker.STATE_OPEN, "threshold failures should open circuit")
    _require(not breaker.allow_request(40), "open circuit should reject requests before timeout")
    _require(breaker.allow_request(80), "open circuit should allow a half-open probe after timeout")
    _require(not breaker.allow_request(81), "half-open circuit should enforce probe request cap")
    breaker.record_success()
    _require(breaker.get_state() == circuit_breaker.STATE_HALF_OPEN, "first half-open success should not close yet")
    _require(breaker.allow_request(82), "half-open circuit should allow second probe")
    breaker.record_success()
    _require(breaker.get_state() == circuit_breaker.STATE_CLOSED, "success threshold should close circuit")
    breaker.reset()
    _require(breaker.get_state() == circuit_breaker.STATE_CLOSED, "reset should force circuit closed")

    var leak_cfg = leak_detector.LeakDetectionConfig()
    leak_cfg.threshold_ms = 100
    var leaks = leak_detector.LeakDetector(leak_cfg)
    leaks.start()
    var token_a = leaks.checkout("conn_a", "role=primary", 0)
    _ = leaks.checkout("conn_b", "role=analytics", 80)
    _require(leaks.get_active_count() == 2, "leak detector active count mismatch after checkout")
    var leak_report = leaks.check_leaks(150)
    _require(len(leak_report) == 1 and "conn_a" in leak_report[0], "leak detector report mismatch")
    _require(leaks.checkin("conn_b", 120) == 40, "leak detector held duration mismatch for conn_b")
    _require(leaks.release_checkout(token_a, 220) == 220, "leak detector token release mismatch")
    _require(leaks.get_active_count() == 0, "leak detector active count mismatch after release")
    var warnings = leaks.get_warnings()
    _require(len(warnings) >= 1, "leak detector warnings should capture threshold breach")
    leaks.clear_warnings()
    _require(len(leaks.get_warnings()) == 0, "leak detector clear_warnings mismatch")
    leaks.stop()

    var telemetry_cfg = telemetry.TelemetryConfig()
    telemetry_cfg.slow_query_threshold_ms = 5
    var collector = telemetry.TelemetryCollector(telemetry_cfg)
    var fast_span = collector.start_span("query", 100)
    collector.end_span(fast_span, 102, True)
    var slow_span = collector.start_span("query", 200)
    collector.end_span(slow_span, 220, False)
    _require(len(collector.get_metrics()) >= 4, "telemetry metrics should be populated")
    _require(len(collector.get_slow_queries()) == 1, "telemetry slow-query log should record one span")
    var prometheus = collector.export_prometheus_metrics()
    _require("scratchbird_queries_total" in prometheus, "prometheus export missing query counter")

    var pipeline_cfg = pipeline.PipelineConfig()
    pipeline_cfg.max_in_flight = 3
    pipeline_cfg.auto_flush = False
    pipeline_cfg.auto_flush_threshold = 2
    var query_pipeline = pipeline.QueryPipeline(pipeline_cfg)
    query_pipeline.start("conn_a")
    var params = List[String]()
    params.append("1")
    _require(query_pipeline.queue("SELECT $1", params), "pipeline queue should accept first request")
    _require(query_pipeline.queue("SELECT $1", params), "pipeline queue should accept second request")
    _require(query_pipeline.pending_count() == 2, "pipeline pending count mismatch before flush")
    query_pipeline.flush()
    _require(query_pipeline.pending_count() == 0, "pipeline pending count mismatch after flush")
    _require(query_pipeline.completed_count() == 2, "pipeline completed count mismatch")
    _require(query_pipeline.failed_count() == 0, "pipeline failed count mismatch")
    query_pipeline.stop()

    var builder = pipeline.PipelineBuilder()
    builder.add("SELECT 1")
    builder.add("SELECT 2")
    var batch = builder.build()
    _require(len(batch) == 2, "pipeline builder batch size mismatch")

    print("Mojo lifecycle scaffold tests OK")
