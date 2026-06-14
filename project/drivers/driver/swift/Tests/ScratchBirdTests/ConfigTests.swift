// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import XCTest
@testable import ScratchBird

final class ConfigTests: XCTestCase {
    func testParseDsn() throws {
        let cfg = ScratchBirdConfig(dsn: "scratchbird://user:pass@localhost:3092/db")
        XCTAssertEqual(cfg.user, "user")
        XCTAssertEqual(cfg.password, "pass")
        XCTAssertEqual(cfg.database, "db")
    }

    func testParseManagerProxyParams() throws {
        let cfg = ScratchBirdConfig(
            dsn: "scratchbird://admin:secret@localhost:3092/mydb?front_door_mode=manager_proxy&manager_auth_token=token&manager_client_flags=7"
        )
        XCTAssertEqual(cfg.frontDoorMode, "manager_proxy")
        XCTAssertEqual(cfg.managerAuthToken, "token")
        XCTAssertEqual(cfg.managerClientFlags, 7)
    }

    func testParseBootstrapAuthParams() throws {
        let cfg = ScratchBirdConfig(
            dsn: "scratchbird://admin:secret@localhost:3092/mydb?auth_token=token-1&auth_method_id=scratchbird.auth.scram_sha_512&auth_payload_json=%7B%22k%22%3A1%7D&auth_required_methods=TOKEN,SCRAM_SHA_512&auth_require_channel_binding=true&workload_identity_token=wid-1&proxy_principal_assertion=proxy-1&connect_client_flags=9"
        )
        XCTAssertEqual(cfg.authToken, "token-1")
        XCTAssertEqual(cfg.authMethodId, "scratchbird.auth.scram_sha_512")
        XCTAssertEqual(cfg.authPayloadJson, "{\"k\":1}")
        XCTAssertEqual(cfg.authRequiredMethods, "TOKEN,SCRAM_SHA_512")
        XCTAssertEqual(cfg.workloadIdentityToken, "wid-1")
        XCTAssertEqual(cfg.proxyPrincipalAssertion, "proxy-1")
        XCTAssertTrue(cfg.authRequireChannelBinding)
        XCTAssertEqual(cfg.connectClientFlags, 9)
    }

    func testNormalizeFrontDoorModeRejectsInvalid() throws {
        XCTAssertThrowsError(try normalizeFrontDoorMode("invalid"))
    }

    func testParseTlsOptions() throws {
        let cfg = ScratchBirdConfig(
            dsn: "scratchbird://user:pass@localhost:3092/db?sslmode=verify-full&sslrootcert=/tmp/ca.pem&sslcert=/tmp/client.pem&sslkey=/tmp/client.key&sslpassword=secret"
        )
        XCTAssertEqual(cfg.sslmode, "verify-full")
        XCTAssertEqual(cfg.sslrootcert, "/tmp/ca.pem")
        XCTAssertEqual(cfg.sslcert, "/tmp/client.pem")
        XCTAssertEqual(cfg.sslkey, "/tmp/client.key")
        XCTAssertEqual(cfg.sslpassword, "secret")
    }

    func testParseResilienceOptions() throws {
        let cfg = ScratchBirdConfig(
            dsn: "scratchbird://user:pass@localhost:3092/db?keepalive_interval_ms=25&keepalive_max_idle_before_check_ms=5&keepalive_validation_timeout_ms=100&leak_detection_threshold_ms=20&leak_detection_check_interval_ms=10&leak_detection_capture_stack_trace=true"
        )
        XCTAssertEqual(cfg.keepaliveIntervalMs, 25)
        XCTAssertEqual(cfg.keepaliveMaxIdleBeforeCheckMs, 5)
        XCTAssertEqual(cfg.keepaliveValidationTimeoutMs, 100)
        XCTAssertEqual(cfg.leakDetectionThresholdMs, 20)
        XCTAssertEqual(cfg.leakDetectionCheckIntervalMs, 10)
        XCTAssertEqual(cfg.leakDetectionCaptureStackTrace, true)
    }

    func testNormalizeSslMode() throws {
        XCTAssertEqual(try normalizeSslMode("verify_ca"), "verify-ca")
        XCTAssertEqual(try normalizeSslMode("verify-full"), "verify-full")
        XCTAssertEqual(try normalizeSslMode("require"), "require")
        XCTAssertThrowsError(try normalizeSslMode("invalid"))
    }

    func testConnectAllowsDisableSslModeAndReachesSocketConnect() async {
        let config = ScratchBirdConfig(
            host: "127.0.0.1",
            port: 1,
            database: "mydb",
            user: "user",
            sslmode: "disable"
        )

        do {
            _ = try await ScratchBirdConnection.connect(config)
            XCTFail("Expected connect attempt to fail on socket reachability")
        } catch {
            let nsError = error as NSError
            XCTAssertFalse(nsError.localizedDescription.contains("TLS is required"))
        }
    }
}
