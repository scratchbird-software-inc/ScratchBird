// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

final class TelemetryConfig {
    var enableTracing = true
    var enableMetrics = true
    var enableSlowQueryLog = true
    var slowQueryThresholdMs: Int = 1000
    var sanitizeQueries = true
    var sampleRate: Double = 1.0
}

final class SpanContext {
    let traceId: String
    let spanId: String
    let parentSpanId: String?
    let spanName: String
    let startTime = Date()
    var attributes: [String: String] = [:]

    init(name: String, parent: SpanContext? = nil) {
        traceId = parent?.traceId ?? UUID().uuidString.replacingOccurrences(of: "-", with: "")
        spanId = UUID().uuidString.prefix(16).description
        parentSpanId = parent?.spanId
        spanName = name
    }

    func withAttribute(_ key: String, _ value: String) -> SpanContext {
        attributes[key] = value
        return self
    }

    func elapsedMs() -> Int {
        return Int(Date().timeIntervalSince(startTime) * 1000)
    }
}

final class TelemetryCollector {
    private let config: TelemetryConfig
    private let queue = DispatchQueue(label: "scratchbird.telemetry")
    private var spans: [SpanContext] = []
    private var totalQueries = 0
    private var successfulQueries = 0
    private var failedQueries = 0
    private var totalQueryTimeMs = 0
    private var slowQueries: [[String: Any]] = []

    init(config: TelemetryConfig = TelemetryConfig()) {
        self.config = config
    }

    func startSpan(_ name: String) -> SpanContext? {
        if !config.enableTracing { return nil }
        if Double.random(in: 0...1) > config.sampleRate { return nil }
        let span = SpanContext(name: name)
        queue.sync {
            spans.append(span)
            if spans.count > 1000 {
                spans.removeFirst()
            }
        }
        return span
    }

    func endSpan(_ span: SpanContext?, success: Bool) {
        guard let span, config.enableTracing else { return }
        let durationMs = span.elapsedMs()
        recordQueryMetrics(span.spanName, durationMs, success)
        if config.enableSlowQueryLog, durationMs > config.slowQueryThresholdMs {
            recordSlowQuery(span, durationMs)
        }
    }

    static func sanitizeQuery(_ sql: String) -> String {
        return sql.replacingOccurrences(of: "'[^']*'", with: "'?'", options: .regularExpression)
    }

    private func recordQueryMetrics(_ operation: String, _ durationMs: Int, _ success: Bool) {
        if !config.enableMetrics { return }
        queue.sync {
            totalQueries += 1
            if success { successfulQueries += 1 } else { failedQueries += 1 }
            totalQueryTimeMs += durationMs
        }
    }

    private func recordSlowQuery(_ span: SpanContext, _ durationMs: Int) {
        queue.sync {
            slowQueries.append([
                "traceId": span.traceId,
                "spanName": span.spanName,
                "durationMs": durationMs,
                "timestamp": ISO8601DateFormatter().string(from: Date()),
                "attributes": span.attributes
            ])
            if slowQueries.count > 100 {
                slowQueries.removeFirst()
            }
        }
    }
}
