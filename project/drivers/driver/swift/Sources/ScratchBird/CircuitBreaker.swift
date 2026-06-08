// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

final class CircuitBreaker {
    enum State {
        case closed
        case open
        case halfOpen
    }

    struct Config {
        var failureThreshold: Int = 5
        var recoveryTimeoutMs: Int = 30_000
        var successThreshold: Int = 3
        var halfOpenMaxRequests: Int = 10
    }

    private let config: Config
    private let queue = DispatchQueue(label: "scratchbird.circuitbreaker")
    private var state: State = .closed
    private var failureCount = 0
    private var successCount = 0
    private var halfOpenRequests = 0
    private var lastFailureAt: Date?

    init(config: Config = Config()) {
        self.config = config
    }

    func allowRequest() -> Bool {
        return queue.sync {
            switch state {
            case .closed:
                return true
            case .open:
                if let lastFailureAt,
                   Int(Date().timeIntervalSince(lastFailureAt) * 1000) >= config.recoveryTimeoutMs {
                    transitionToHalfOpen()
                    return allowHalfOpenRequest()
                }
                return false
            case .halfOpen:
                return allowHalfOpenRequest()
            }
        }
    }

    func recordSuccess() {
        queue.sync {
            switch state {
            case .closed:
                failureCount = 0
            case .halfOpen:
                halfOpenRequests = max(0, halfOpenRequests - 1)
                successCount += 1
                if successCount >= config.successThreshold {
                    transitionToClosed()
                }
            case .open:
                break
            }
        }
    }

    func recordFailure() {
        queue.sync {
            switch state {
            case .closed:
                failureCount += 1
                if failureCount >= config.failureThreshold {
                    transitionToOpen()
                }
            case .halfOpen:
                halfOpenRequests = max(0, halfOpenRequests - 1)
                transitionToOpen()
            case .open:
                lastFailureAt = Date()
            }
        }
    }

    private func allowHalfOpenRequest() -> Bool {
        if halfOpenRequests < config.halfOpenMaxRequests {
            halfOpenRequests += 1
            return true
        }
        return false
    }

    private func transitionToHalfOpen() {
        state = .halfOpen
        failureCount = 0
        successCount = 0
        halfOpenRequests = 0
    }

    private func transitionToOpen() {
        state = .open
        lastFailureAt = Date()
    }

    private func transitionToClosed() {
        state = .closed
        failureCount = 0
        successCount = 0
        halfOpenRequests = 0
        lastFailureAt = nil
    }
}
