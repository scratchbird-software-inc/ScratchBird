// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import XCTest
@testable import ScratchBird

final class ErrorResilienceTests: XCTestCase {
    func testDecodeHeaderRejectsInvalidLength() {
        XCTAssertThrowsError(try decodeHeader(Data(count: headerSize - 1))) { error in
            guard case ProtocolError.invalidHeader = error else {
                return XCTFail("Expected ProtocolError.invalidHeader, got \(error)")
            }
        }
    }

    func testDecodeHeaderRejectsInvalidMagic() {
        var headerData = makeValidHeaderData()
        headerData.replaceSubrange(0..<4, with: withUnsafeBytes(of: UInt32(0).littleEndian, Array.init))

        XCTAssertThrowsError(try decodeHeader(headerData)) { error in
            guard case ProtocolError.invalidHeader = error else {
                return XCTFail("Expected ProtocolError.invalidHeader, got \(error)")
            }
        }
    }

    func testDecodeHeaderRejectsUnsupportedVersion() {
        var headerData = makeValidHeaderData()
        headerData[4] = protocolMajor &+ 1

        XCTAssertThrowsError(try decodeHeader(headerData)) { error in
            guard case ProtocolError.unsupportedVersion = error else {
                return XCTFail("Expected ProtocolError.unsupportedVersion, got \(error)")
            }
        }
    }

    func testDecodeHeaderRejectsPayloadTooLarge() {
        var headerData = makeValidHeaderData()
        let oversized = maxMessageSize &+ 1
        headerData.replaceSubrange(8..<12, with: withUnsafeBytes(of: oversized.littleEndian, Array.init))

        XCTAssertThrowsError(try decodeHeader(headerData)) { error in
            guard case ProtocolError.payloadTooLarge = error else {
                return XCTFail("Expected ProtocolError.payloadTooLarge, got \(error)")
            }
        }
    }

    func testParseErrorMessageExtractsSeveritySqlStateDetailAndHint() throws {
        let payload = makeErrorPayload(
            severity: "ERROR",
            sqlState: "23505",
            message: "duplicate key",
            detail: "Key (id)=(1) already exists",
            hint: "Use a new id"
        )
        let parsed = try parseErrorMessage(payload)
        XCTAssertEqual(parsed.severity, "ERROR")
        XCTAssertEqual(parsed.sqlState, "23505")
        XCTAssertEqual(parsed.message, "duplicate key")
        XCTAssertEqual(parsed.detail, "Key (id)=(1) already exists")
        XCTAssertEqual(parsed.hint, "Use a new id")
    }

    func testParseErrorMessageRejectsTruncatedField() {
        var payload = Data()
        payload.append(ascii("M"))
        payload.append(Data("broken".utf8))

        XCTAssertThrowsError(try parseErrorMessage(payload)) { error in
            guard case ProtocolError.errorMessageTruncated = error else {
                return XCTFail("Expected ProtocolError.errorMessageTruncated, got \(error)")
            }
        }
    }

    func testBuildScratchBirdNSErrorIncludesStructuredFields() {
        let payload = makeErrorPayload(
            severity: "ERROR",
            sqlState: "23505",
            message: "duplicate key",
            detail: "Key (id)=(1) already exists",
            hint: "Use a new id"
        )

        let error = buildScratchBirdNSError(from: payload, fallbackMessage: "Query failed")
        let nsError = error as NSError
        XCTAssertEqual(nsError.domain, "ScratchBird")
        XCTAssertTrue(nsError.localizedDescription.contains("duplicate key"))
        XCTAssertTrue(nsError.localizedDescription.contains("SQLSTATE 23505"))
        XCTAssertTrue(nsError.localizedDescription.contains("DETAIL: Key (id)=(1) already exists"))
        XCTAssertTrue(nsError.localizedDescription.contains("HINT: Use a new id"))
        XCTAssertEqual(nsError.userInfo[scratchBirdErrorSQLStateKey] as? String, "23505")
        XCTAssertEqual(nsError.userInfo[scratchBirdErrorSeverityKey] as? String, "ERROR")
        XCTAssertEqual(nsError.userInfo[scratchBirdErrorDetailKey] as? String, "Key (id)=(1) already exists")
        XCTAssertEqual(nsError.userInfo[scratchBirdErrorHintKey] as? String, "Use a new id")
    }

    func testBuildScratchBirdNSErrorFallsBackWhenPayloadMalformed() {
        let payload = Data([ascii("M"), ascii("b"), ascii("a"), ascii("d")])
        let error = buildScratchBirdNSError(from: payload, fallbackMessage: "Query failed")
        let nsError = error as NSError
        XCTAssertEqual(nsError.localizedDescription, "Query failed")
        XCTAssertNil(nsError.userInfo[scratchBirdErrorSQLStateKey])
    }

    func testBuildScratchBirdErrorMapsSqlStateToTypedIntegrityException() {
        let payload = makeErrorPayload(
            severity: "ERROR",
            sqlState: "23505",
            message: "duplicate key",
            detail: "",
            hint: ""
        )

        let error = buildScratchBirdError(from: payload, fallbackMessage: "Query failed")
        XCTAssertTrue(error is ScratchBirdIntegrityException)
        XCTAssertEqual(error.sqlState, "23505")
        XCTAssertEqual(error.severity, "ERROR")
        XCTAssertTrue(error.message.contains("SQLSTATE 23505"))
    }

    func testBuildScratchBirdErrorMapsSqlStateClassToTypedDataException() {
        let payload = makeErrorPayload(
            severity: "ERROR",
            sqlState: "22ABC",
            message: "bad numeric",
            detail: "",
            hint: ""
        )

        let error = buildScratchBirdError(from: payload, fallbackMessage: "Query failed")
        XCTAssertTrue(error is ScratchBirdDataException)
        XCTAssertEqual(error.sqlState, "22ABC")
    }

    func testBuildScratchBirdErrorUsesDefaultSqlStateForMalformedPayload() {
        let payload = Data([ascii("M"), ascii("x"), ascii("y"), ascii("z")])
        let error = buildScratchBirdError(
            from: payload,
            fallbackMessage: "Authentication failed",
            defaultSqlState: "28000"
        )

        XCTAssertTrue(error is ScratchBirdAuthorizationException)
        XCTAssertEqual(error.sqlState, "28000")
        XCTAssertEqual(error.message, "Authentication failed")
    }

    func testRetryScopeClassifiesStatementAndReconnectBoundaries() {
        XCTAssertEqual(retryScope(forSqlState: "40001"), .statement)
        XCTAssertEqual(retryScope(forSqlState: "40P01"), .statement)
        XCTAssertEqual(retryScope(forSqlState: "08006"), .reconnect)
        XCTAssertEqual(retryScope(forSqlState: "57014"), .none)
        XCTAssertEqual(retryScope(forSqlState: nil), .none)
    }

    func testIsRetryableOnlyAllowsFreshBoundaryRetries() {
        XCTAssertTrue(isRetryable(sqlState: "40001"))
        XCTAssertTrue(isRetryable(sqlState: "08003"))
        XCTAssertFalse(isRetryable(sqlState: "57014"))
        XCTAssertFalse(isRetryable(sqlState: ""))
    }

    func testCircuitBreakerTransitionsClosedOpenHalfOpenClosed() async {
        let breaker = CircuitBreaker(
            config: .init(
                failureThreshold: 2,
                recoveryTimeoutMs: 20,
                successThreshold: 2,
                halfOpenMaxRequests: 1
            )
        )

        XCTAssertTrue(breaker.allowRequest())
        breaker.recordFailure()
        breaker.recordFailure()

        XCTAssertFalse(breaker.allowRequest())
        try? await Task.sleep(nanoseconds: 30_000_000)

        XCTAssertTrue(breaker.allowRequest())
        XCTAssertFalse(breaker.allowRequest())
        breaker.recordSuccess()
        XCTAssertTrue(breaker.allowRequest())
        breaker.recordSuccess()

        XCTAssertTrue(breaker.allowRequest())
    }

    func testCircuitBreakerHalfOpenFailureReopens() async {
        let breaker = CircuitBreaker(
            config: .init(
                failureThreshold: 2,
                recoveryTimeoutMs: 20,
                successThreshold: 2,
                halfOpenMaxRequests: 1
            )
        )

        breaker.recordFailure()
        breaker.recordFailure()
        XCTAssertFalse(breaker.allowRequest())

        try? await Task.sleep(nanoseconds: 30_000_000)
        XCTAssertTrue(breaker.allowRequest())
        breaker.recordFailure()

        XCTAssertFalse(breaker.allowRequest())
    }

    func testKeepaliveTrackerNeedsValidationOnlyAfterIdleThreshold() async {
        let config = KeepaliveManager.Config(intervalMs: 100, maxIdleBeforeCheckMs: 50, validationTimeoutMs: 20)
        let tracker = KeepaliveTracker(config: config)

        XCTAssertFalse(tracker.needsValidation())
        try? await Task.sleep(nanoseconds: 70_000_000)
        XCTAssertTrue(tracker.needsValidation())

        tracker.markActive()
        XCTAssertFalse(tracker.needsValidation())
    }

    func testKeepaliveManagerPingsIdleConnection() async {
        actor PingCounter {
            var count = 0
            func increment() { count += 1 }
            func value() -> Int { count }
        }

        let counter = PingCounter()
        let manager = KeepaliveManager(
            config: .init(
                intervalMs: 20,
                maxIdleBeforeCheckMs: 0,
                validationTimeoutMs: 100
            )
        )

        _ = manager.register(connectionId: "conn-1") {
            await counter.increment()
            return true
        }
        manager.start()
        try? await Task.sleep(nanoseconds: 150_000_000)
        manager.stop()

        let pingCount = await counter.value()
        XCTAssertGreaterThanOrEqual(pingCount, 1)
    }

    func testLeakDetectorGuardReleaseIsIdempotent() {
        let detector = LeakDetector(
            config: .init(
                thresholdMs: 10_000,
                captureStackTrace: false,
                checkIntervalMs: 10_000
            )
        )

        let guardHandle = detector.checkout(connectionId: "conn-1", metadata: ["schema": "users.alice"])
        XCTAssertEqual(checkoutCount(detector), 1)

        guardHandle.release()
        XCTAssertEqual(checkoutCount(detector), 0)

        guardHandle.release()
        XCTAssertEqual(checkoutCount(detector), 0)
    }

    func testLeakDetectorCheckoutInfoCapturesStackAndMetadata() {
        let withStack = LeakDetector.CheckoutInfo(metadata: ["branch": "sys"], captureStackTrace: true)
        XCTAssertEqual(withStack.metadata["branch"], "sys")
        XCTAssertNotNil(withStack.stackTrace)
        XCTAssertGreaterThanOrEqual(withStack.heldDurationMs(), 0)

        let withoutStack = LeakDetector.CheckoutInfo(metadata: ["branch": "users"], captureStackTrace: false)
        XCTAssertEqual(withoutStack.metadata["branch"], "users")
        XCTAssertNil(withoutStack.stackTrace)
    }

    func testTelemetryStartSpanReturnsNilWhenTracingDisabled() {
        let config = TelemetryConfig()
        config.enableTracing = false

        let collector = TelemetryCollector(config: config)
        XCTAssertNil(collector.startSpan("select"))
    }

    func testTelemetryRecordsSuccessAndFailureMetrics() {
        let config = TelemetryConfig()
        config.enableTracing = true
        config.enableMetrics = true
        config.enableSlowQueryLog = false
        config.sampleRate = 1.0

        let collector = TelemetryCollector(config: config)
        let successSpan = collector.startSpan("query-success")
        collector.endSpan(successSpan, success: true)
        let failureSpan = collector.startSpan("query-failure")
        collector.endSpan(failureSpan, success: false)

        XCTAssertEqual(intField(collector, "totalQueries"), 2)
        XCTAssertEqual(intField(collector, "successfulQueries"), 1)
        XCTAssertEqual(intField(collector, "failedQueries"), 1)
        XCTAssertGreaterThanOrEqual(intField(collector, "totalQueryTimeMs"), 0)
    }

    func testTelemetrySanitizeQueryReplacesStringLiterals() {
        let sql = "select * from users where email='alice@example.com' and role='admin'"
        let sanitized = TelemetryCollector.sanitizeQuery(sql)
        XCTAssertEqual(sanitized, "select * from users where email='?' and role='?'")
    }

    func testCloseClearsAbandonedSessionStateForFreshReconnectBoundaries() async throws {
        let conn = ScratchBirdConnection.debugCreateForTesting(
            config: ScratchBirdConfig(database: "mydb", user: "user")
        )
        conn.debugSeedAbandonedSessionStateForTesting(
            sequence: 9,
            lastQuerySequence: 7,
            attachmentId: Data((1...16).map { UInt8($0) }),
            txnId: 42,
            transactionActive: true,
            explicitTransaction: true,
            parameters: [
                "attachment_id": "11111111-1111-1111-1111-111111111111",
                "current_txn_id": "42",
            ],
            lastPlan: QueryPlanMessage(
                format: 1,
                planningTimeUs: 2,
                estimatedRows: 3,
                estimatedCost: 4,
                plan: Data([1, 2, 3])
            ),
            lastSblr: SblrCompiledMessage(
                hash: 5,
                version: 6,
                bytecode: Data([4, 5, 6])
            )
        )

        try await conn.close()

        XCTAssertNil(conn.lastQueryPlan())
        XCTAssertNil(conn.lastSblrCompiled())
        XCTAssertEqual(field(conn, named: "sequence") as UInt32?, 0)
        XCTAssertEqual(field(conn, named: "lastQuerySequence") as UInt32?, 0)
        XCTAssertEqual(field(conn, named: "txnId") as UInt64?, 0)
        XCTAssertEqual(field(conn, named: "transactionActive") as Bool?, false)
        XCTAssertEqual(field(conn, named: "explicitTransaction") as Bool?, false)
        XCTAssertEqual(field(conn, named: "parameters") as [String: String]?, [:])
        XCTAssertEqual(field(conn, named: "attachmentId") as Data?, Data(repeating: 0, count: 16))
    }

    private func makeValidHeaderData() -> Data {
        let header = MessageHeader(
            type: .query,
            flags: 0,
            length: 0,
            sequence: 1,
            attachmentId: Data(repeating: 0, count: 16),
            txnId: 0
        )
        return Data(encodeMessage(header: header, payload: Data()).prefix(headerSize))
    }

    private func checkoutCount(_ detector: LeakDetector) -> Int {
        let mirror = Mirror(reflecting: detector)
        for child in mirror.children where child.label == "checkouts" {
            if let checkouts = child.value as? [String: LeakDetector.CheckoutInfo] {
                return checkouts.count
            }
        }
        return -1
    }

    private func intField(_ collector: TelemetryCollector, _ label: String) -> Int {
        let mirror = Mirror(reflecting: collector)
        for child in mirror.children where child.label == label {
            if let value = child.value as? Int {
                return value
            }
        }
        XCTFail("Missing telemetry field \(label)")
        return -1
    }

    private func field<T>(_ object: Any, named label: String) -> T? {
        let mirror = Mirror(reflecting: object)
        for child in mirror.children where child.label == label {
            return child.value as? T
        }
        return nil
    }

    private func ascii(_ char: Character) -> UInt8 {
        return char.asciiValue ?? 0
    }

    private func makeErrorPayload(
        severity: String,
        sqlState: String,
        message: String,
        detail: String,
        hint: String
    ) -> Data {
        var payload = Data()

        func appendField(_ tag: Character, _ value: String) {
            if value.isEmpty {
                return
            }
            payload.append(ascii(tag))
            payload.append(Data(value.utf8))
            payload.append(0)
        }

        appendField("S", severity)
        appendField("C", sqlState)
        appendField("M", message)
        appendField("D", detail)
        appendField("H", hint)
        payload.append(0)
        return payload
    }
}
