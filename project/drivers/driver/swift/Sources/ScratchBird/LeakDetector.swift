// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

final class LeakDetector {
    struct Config {
        var thresholdMs: Int = 30_000
        var captureStackTrace: Bool = false
        var checkIntervalMs: Int = 10_000
    }

    struct Stats {
        let activeCheckouts: Int
        let detectedLeaks: Int
    }

    final class CheckoutInfo {
        let checkoutTime = Date()
        let stackTrace: String?
        let metadata: [String: String]

        init(metadata: [String: String], captureStackTrace: Bool) {
            self.metadata = metadata
            self.stackTrace = captureStackTrace ? Thread.callStackSymbols.joined(separator: "\n") : nil
        }

        func heldDurationMs() -> Int {
            return Int(Date().timeIntervalSince(checkoutTime) * 1000)
        }
    }

    final class Guard {
        private var released = false
        private let detector: LeakDetector
        private let connectionId: String

        init(detector: LeakDetector, connectionId: String) {
            self.detector = detector
            self.connectionId = connectionId
        }

        func release() {
            if released { return }
            released = true
            detector.checkin(connectionId: connectionId)
        }
    }

    private let config: Config
    private let queue = DispatchQueue(label: "scratchbird.leakdetector")
    private var checkouts: [String: CheckoutInfo] = [:]
    private var timer: DispatchSourceTimer?
    private var detectedLeaks = 0

    init(config: Config = Config()) {
        self.config = config
    }

    func start() {
        if timer != nil { return }
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + .milliseconds(config.checkIntervalMs), repeating: .milliseconds(config.checkIntervalMs))
        timer.setEventHandler { [weak self] in
            self?.checkLeaks()
        }
        timer.resume()
        self.timer = timer
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    func checkout(connectionId: String, metadata: [String: String]) -> Guard {
        let info = CheckoutInfo(metadata: metadata, captureStackTrace: config.captureStackTrace)
        queue.sync {
            checkouts[connectionId] = info
        }
        return Guard(detector: self, connectionId: connectionId)
    }

    func checkin(connectionId: String) {
        queue.sync {
            _ = checkouts.removeValue(forKey: connectionId)
        }
    }

    func stats() -> Stats {
        return queue.sync {
            Stats(activeCheckouts: checkouts.count, detectedLeaks: detectedLeaks)
        }
    }

    private func checkLeaks() {
        for (connId, info) in checkouts {
            if info.heldDurationMs() > config.thresholdMs {
                detectedLeaks += 1
                print("POSSIBLE CONNECTION LEAK: conn=\(connId) held=\(info.heldDurationMs())ms")
            }
        }
    }
}
