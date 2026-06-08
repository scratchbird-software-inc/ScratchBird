// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

func validateTxnBeginOptions(
    isolationLevel: UInt8?,
    readCommittedMode: ScratchBirdReadCommittedMode?,
    accessMode: UInt8?,
    autocommitMode: UInt8?
) throws {
    if let isolationLevel, isolationLevel > isolationSerializable {
        throw NSError(
            domain: "ScratchBird",
            code: -1,
            userInfo: [NSLocalizedDescriptionKey: "isolation level \(isolationLevel) is not supported"]
        )
    }
    if let isolationLevel, readCommittedMode != nil,
       isolationLevel != isolationReadUncommitted,
       isolationLevel != isolationReadCommitted {
        throw ScratchBirdNotSupportedException(
            message: "readCommittedMode requires a READ COMMITTED isolation alias",
            sqlState: "0A000"
        )
    }
    if let accessMode, accessMode > 1 {
        throw NSError(
            domain: "ScratchBird",
            code: -1,
            userInfo: [NSLocalizedDescriptionKey: "access mode \(accessMode) is not supported"]
        )
    }
    if let autocommitMode, autocommitMode > 1 {
        throw NSError(
            domain: "ScratchBird",
            code: -1,
            userInfo: [NSLocalizedDescriptionKey: "autocommit mode \(autocommitMode) is not supported"]
        )
    }
}

func normalizeSavepointName(_ name: String) throws -> String {
    let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
    if trimmed.isEmpty {
        throw NSError(
            domain: "ScratchBird",
            code: -1,
            userInfo: [NSLocalizedDescriptionKey: "savepoint name is required"]
        )
    }
    return trimmed
}

func requireCancelableSequence(_ sequence: UInt32) throws -> UInt32 {
    if sequence == 0 {
        throw NSError(
            domain: "ScratchBird",
            code: -1,
            userInfo: [NSLocalizedDescriptionKey: "No active query to cancel"]
        )
    }
    return sequence
}

func normalizePortalResumeMaxRows(fetchSize: Int) -> UInt32 {
    if fetchSize <= 0 {
        return 0
    }
    if fetchSize >= Int(UInt32.max) {
        return UInt32.max
    }
    return UInt32(fetchSize)
}

func quoteStringLiteral(_ value: String) -> String {
    let escaped = value.replacingOccurrences(of: "'", with: "''")
    return "'\(escaped)'"
}

func buildPreparedTransactionSql(verb: String, globalTransactionId: String) throws -> String {
    let trimmed = globalTransactionId.trimmingCharacters(in: .whitespacesAndNewlines)
    if trimmed.isEmpty {
        throw ScratchBirdProgrammingException(
            message: "Global transaction id is required",
            sqlState: "42601"
        )
    }
    return "\(verb) \(quoteStringLiteral(trimmed))"
}
