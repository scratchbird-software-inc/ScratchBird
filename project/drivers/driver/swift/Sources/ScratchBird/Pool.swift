// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

public struct ScratchBirdPoolStats {
    public let maxSize: Int
    public let createdConnections: Int
    public let openConnections: Int
    public let idleConnections: Int
    public let isClosed: Bool
}

public final class ScratchBirdConnectionPool {
    private let config: ScratchBirdConfig
    private let maxSize: Int
    private let queue = DispatchQueue(label: "scratchbird.pool")
    private var idle: [ScratchBirdConnection] = []
    private var openConnections = 0
    private var createdConnections = 0
    private var closed = false

    public init(config: ScratchBirdConfig, maxSize: Int = 4) {
        self.config = config
        self.maxSize = max(1, maxSize)
    }

    public func acquire() async throws -> ScratchBirdConnection {
        var reused: ScratchBirdConnection?
        var shouldCreate = false
        var acquireError: ScratchBirdDriverException?

        queue.sync {
            if closed {
                acquireError = ScratchBirdOperationalException(message: "Connection pool is closed")
                return
            }
            if let existing = idle.popLast() {
                reused = existing
                return
            }
            if openConnections < maxSize {
                openConnections += 1
                createdConnections += 1
                shouldCreate = true
                return
            }
            acquireError = ScratchBirdOperationalException(message: "Connection pool is exhausted")
        }

        if let acquireError {
            throw acquireError
        }
        if let reused {
            return reused
        }
        if shouldCreate {
            do {
                return try await ScratchBirdConnection.connect(config)
            } catch {
                queue.sync {
                    openConnections = max(0, openConnections - 1)
                }
                throw error
            }
        }

        throw ScratchBirdOperationalException(message: "Connection pool acquire failed")
    }

    public func release(_ connection: ScratchBirdConnection) async {
        var shouldClose = false
        queue.sync {
            if closed {
                openConnections = max(0, openConnections - 1)
                shouldClose = true
                return
            }
            idle.append(connection)
        }
        if shouldClose {
            try? await connection.close()
        }
    }

    public func withConnection<T>(_ body: (ScratchBirdConnection) async throws -> T) async throws -> T {
        let connection = try await acquire()
        do {
            let result = try await body(connection)
            await release(connection)
            return result
        } catch {
            await release(connection)
            throw error
        }
    }

    public func close() async {
        var toClose: [ScratchBirdConnection] = []
        queue.sync {
            if closed { return }
            closed = true
            toClose = idle
            idle.removeAll()
            openConnections = max(0, openConnections - toClose.count)
        }
        for connection in toClose {
            try? await connection.close()
        }
    }

    func debugStats() -> ScratchBirdPoolStats {
        return queue.sync {
            ScratchBirdPoolStats(
                maxSize: maxSize,
                createdConnections: createdConnections,
                openConnections: openConnections,
                idleConnections: idle.count,
                isClosed: closed
            )
        }
    }
}
