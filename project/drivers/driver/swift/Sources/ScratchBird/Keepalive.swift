// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

final class KeepaliveTracker {
    private var lastActivity = Date()
    private let config: KeepaliveManager.Config

    init(config: KeepaliveManager.Config) {
        self.config = config
    }

    func markActive() {
        lastActivity = Date()
    }

    func needsValidation() -> Bool {
        return Int(Date().timeIntervalSince(lastActivity) * 1000) > config.maxIdleBeforeCheckMs
    }
}

final class KeepaliveManager: @unchecked Sendable {
    struct Config {
        var intervalMs: Int = 120_000
        var maxIdleBeforeCheckMs: Int = 600_000
        var validationTimeoutMs: Int = 5_000
    }

    struct Stats {
        let validationAttempts: Int
        let validationSuccesses: Int
        let validationFailures: Int
        let registeredConnections: Int
    }

    typealias Pinger = () async -> Bool

    private let config: Config
    private let queue = DispatchQueue(label: "scratchbird.keepalive")
    private var trackers: [String: KeepaliveTracker] = [:]
    private var pingers: [String: Pinger] = [:]
    private var timer: DispatchSourceTimer?
    private var validationAttempts = 0
    private var validationSuccesses = 0
    private var validationFailures = 0

    init(config: Config = Config()) {
        self.config = config
    }

    func start() {
        if timer != nil { return }
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + .milliseconds(config.intervalMs), repeating: .milliseconds(config.intervalMs))
        timer.setEventHandler { [weak self] in
            guard let self else { return }
            Task.detached { [weak self] in
                await self?.checkConnections()
            }
        }
        timer.resume()
        self.timer = timer
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    func register(connectionId: String, pinger: @escaping Pinger) -> KeepaliveTracker {
        let tracker = KeepaliveTracker(config: config)
        queue.sync {
            trackers[connectionId] = tracker
            pingers[connectionId] = pinger
        }
        return tracker
    }

    func unregister(connectionId: String) {
        queue.sync {
            trackers.removeValue(forKey: connectionId)
            pingers.removeValue(forKey: connectionId)
        }
    }

    func stats() -> Stats {
        return queue.sync {
            Stats(
                validationAttempts: validationAttempts,
                validationSuccesses: validationSuccesses,
                validationFailures: validationFailures,
                registeredConnections: trackers.count
            )
        }
    }

    private func checkConnections() async {
        let snapshot: [(String, KeepaliveTracker, Pinger)] = queue.sync {
            trackers.compactMap { key, tracker in
                guard let pinger = pingers[key] else { return nil }
                return (key, tracker, pinger)
            }
        }
        for (_, tracker, pinger) in snapshot {
            if tracker.needsValidation() {
                queue.sync {
                    validationAttempts += 1
                }
                let ok = await pingWithTimeout(pinger)
                if ok {
                    queue.sync {
                        validationSuccesses += 1
                    }
                    tracker.markActive()
                } else {
                    queue.sync {
                        validationFailures += 1
                    }
                }
            }
        }
    }

    private func pingWithTimeout(_ pinger: @escaping Pinger) async -> Bool {
        let timeoutNs = UInt64(config.validationTimeoutMs) * 1_000_000
        return await withTaskGroup(of: Bool.self) { group in
            group.addTask { await pinger() }
            group.addTask {
                try? await Task.sleep(nanoseconds: timeoutNs)
                return false
            }
            let result = await group.next() ?? false
            group.cancelAll()
            return result
        }
    }
}
