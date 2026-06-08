// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

public class ScratchBirdDriverException: NSObject, Error, CustomNSError, LocalizedError {
    public static var errorDomain: String { "ScratchBird" }

    public let code: Int
    public let message: String
    public let sqlState: String?
    public let severity: String?
    public let detail: String?
    public let hint: String?

    public init(
        message: String,
        code: Int = -1,
        sqlState: String? = nil,
        severity: String? = nil,
        detail: String? = nil,
        hint: String? = nil
    ) {
        self.message = message
        self.code = code
        self.sqlState = sqlState
        self.severity = severity
        self.detail = detail
        self.hint = hint
    }

    public var errorDescription: String? {
        return message
    }

    public var errorCode: Int {
        return code
    }

    public var errorUserInfo: [String: Any] {
        var userInfo: [String: Any] = [NSLocalizedDescriptionKey: message]
        if let sqlState, !sqlState.isEmpty {
            userInfo[scratchBirdErrorSQLStateKey] = sqlState
        }
        if let severity, !severity.isEmpty {
            userInfo[scratchBirdErrorSeverityKey] = severity
        }
        if let detail, !detail.isEmpty {
            userInfo[scratchBirdErrorDetailKey] = detail
            userInfo[NSLocalizedFailureReasonErrorKey] = detail
        }
        if let hint, !hint.isEmpty {
            userInfo[scratchBirdErrorHintKey] = hint
            userInfo[NSLocalizedRecoverySuggestionErrorKey] = hint
        }
        return userInfo
    }
}

public final class ScratchBirdConnectionException: ScratchBirdDriverException {}
public final class ScratchBirdAuthorizationException: ScratchBirdDriverException {}
public final class ScratchBirdDataException: ScratchBirdDriverException {}
public final class ScratchBirdIntegrityException: ScratchBirdDriverException {}
public final class ScratchBirdTransactionException: ScratchBirdDriverException {}
public final class ScratchBirdProgrammingException: ScratchBirdDriverException {}
public final class ScratchBirdNotSupportedException: ScratchBirdDriverException {}
public final class ScratchBirdTimeoutException: ScratchBirdDriverException {}
public final class ScratchBirdOperationalException: ScratchBirdDriverException {}

public enum ScratchBirdRetryScope: String {
    case none
    case reconnect
    case statement
    case transaction
}

func mapSqlStateError(
    message: String,
    sqlState: String?,
    severity: String?,
    detail: String?,
    hint: String?,
    code: Int = -1
) -> ScratchBirdDriverException {
    let state = sqlState ?? ""
    if let exact = mapExactSqlState(
        state,
        message: message,
        severity: severity,
        detail: detail,
        hint: hint,
        code: code
    ) {
        return exact
    }
    if state.count >= 2 {
        let stateClass = String(state.prefix(2))
        if let classMapped = mapSqlStateClass(
            stateClass,
            message: message,
            sqlState: state,
            severity: severity,
            detail: detail,
            hint: hint,
            code: code
        ) {
            return classMapped
        }
    }
    return ScratchBirdOperationalException(
        message: message,
        code: code,
        sqlState: state.isEmpty ? nil : state,
        severity: severity,
        detail: detail,
        hint: hint
    )
}

private func mapExactSqlState(
    _ state: String,
    message: String,
    severity: String?,
    detail: String?,
    hint: String?,
    code: Int
) -> ScratchBirdDriverException? {
    switch state {
    case "08001", "08003", "08004", "08006", "08P01", "57P01", "57P03":
        return ScratchBirdConnectionException(
            message: message,
            code: code,
            sqlState: state,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "28000", "28P01", "42501":
        return ScratchBirdAuthorizationException(
            message: message,
            code: code,
            sqlState: state,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "0A000":
        return ScratchBirdNotSupportedException(
            message: message,
            code: code,
            sqlState: state,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "22001", "22003", "22007", "22012", "22023", "22P02", "22P03":
        return ScratchBirdDataException(
            message: message,
            code: code,
            sqlState: state,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "23000", "23502", "23503", "23505", "23514":
        return ScratchBirdIntegrityException(
            message: message,
            code: code,
            sqlState: state,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "40001", "40P01", "25P02":
        return ScratchBirdTransactionException(
            message: message,
            code: code,
            sqlState: state,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "42601", "42703", "42704", "42710", "42883", "42P01", "42P07":
        return ScratchBirdProgrammingException(
            message: message,
            code: code,
            sqlState: state,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "57014":
        return ScratchBirdTimeoutException(
            message: message,
            code: code,
            sqlState: state,
            severity: severity,
            detail: detail,
            hint: hint
        )
    default:
        return nil
    }
}

private func mapSqlStateClass(
    _ stateClass: String,
    message: String,
    sqlState: String,
    severity: String?,
    detail: String?,
    hint: String?,
    code: Int
) -> ScratchBirdDriverException? {
    switch stateClass {
    case "08":
        return ScratchBirdConnectionException(
            message: message,
            code: code,
            sqlState: sqlState,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "22":
        return ScratchBirdDataException(
            message: message,
            code: code,
            sqlState: sqlState,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "23":
        return ScratchBirdIntegrityException(
            message: message,
            code: code,
            sqlState: sqlState,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "28":
        return ScratchBirdAuthorizationException(
            message: message,
            code: code,
            sqlState: sqlState,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "40":
        return ScratchBirdTransactionException(
            message: message,
            code: code,
            sqlState: sqlState,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "42":
        return ScratchBirdProgrammingException(
            message: message,
            code: code,
            sqlState: sqlState,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "0A":
        return ScratchBirdNotSupportedException(
            message: message,
            code: code,
            sqlState: sqlState,
            severity: severity,
            detail: detail,
            hint: hint
        )
    case "53", "54", "55", "57", "58":
        return ScratchBirdOperationalException(
            message: message,
            code: code,
            sqlState: sqlState,
            severity: severity,
            detail: detail,
            hint: hint
        )
    default:
        return nil
    }
}

public func retryScope(forSqlState sqlState: String?) -> ScratchBirdRetryScope {
    // Drivers are fail-closed: fresh statement restart for 40xxx, reconnect
    // only for 08xxx, and no automatic whole-transaction replay.
    guard let sqlState, sqlState.count == 5 else {
        return .none
    }
    switch sqlState {
    case "40001", "40P01":
        return .statement
    default:
        return String(sqlState.prefix(2)) == "08" ? .reconnect : .none
    }
}

public func isRetryable(sqlState: String?) -> Bool {
    return retryScope(forSqlState: sqlState) != .none
}
