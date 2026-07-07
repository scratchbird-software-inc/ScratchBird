"use strict";
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
Object.defineProperty(exports, "__esModule", { value: true });
exports.ScratchbirdInternalError = exports.ScratchbirdSystemError = exports.ScratchbirdOperatorInterventionError = exports.ScratchbirdLimitError = exports.ScratchbirdResourceError = exports.ScratchbirdSyntaxError = exports.ScratchbirdTransactionError = exports.ScratchbirdAuthError = exports.ScratchbirdIntegrityError = exports.ScratchbirdDataError = exports.ScratchbirdNotSupportedError = exports.ScratchbirdConnectionError = exports.ScratchbirdNoDataError = exports.ScratchbirdWarning = exports.ScratchbirdError = void 0;
exports.mapSqlState = mapSqlState;
exports.retryScopeForSqlState = retryScopeForSqlState;
exports.isRetryableSqlState = isRetryableSqlState;
class ScratchbirdError extends Error {
    constructor(message, code, detail, hint) {
        super(message);
        this.name = this.constructor.name;
        this.code = code;
        this.detail = detail;
        this.hint = hint;
    }
}
exports.ScratchbirdError = ScratchbirdError;
class ScratchbirdWarning extends ScratchbirdError {
}
exports.ScratchbirdWarning = ScratchbirdWarning;
class ScratchbirdNoDataError extends ScratchbirdError {
}
exports.ScratchbirdNoDataError = ScratchbirdNoDataError;
class ScratchbirdConnectionError extends ScratchbirdError {
}
exports.ScratchbirdConnectionError = ScratchbirdConnectionError;
class ScratchbirdNotSupportedError extends ScratchbirdError {
}
exports.ScratchbirdNotSupportedError = ScratchbirdNotSupportedError;
class ScratchbirdDataError extends ScratchbirdError {
}
exports.ScratchbirdDataError = ScratchbirdDataError;
class ScratchbirdIntegrityError extends ScratchbirdError {
}
exports.ScratchbirdIntegrityError = ScratchbirdIntegrityError;
class ScratchbirdAuthError extends ScratchbirdError {
}
exports.ScratchbirdAuthError = ScratchbirdAuthError;
class ScratchbirdTransactionError extends ScratchbirdError {
}
exports.ScratchbirdTransactionError = ScratchbirdTransactionError;
class ScratchbirdSyntaxError extends ScratchbirdError {
}
exports.ScratchbirdSyntaxError = ScratchbirdSyntaxError;
class ScratchbirdResourceError extends ScratchbirdError {
}
exports.ScratchbirdResourceError = ScratchbirdResourceError;
class ScratchbirdLimitError extends ScratchbirdError {
}
exports.ScratchbirdLimitError = ScratchbirdLimitError;
class ScratchbirdOperatorInterventionError extends ScratchbirdError {
}
exports.ScratchbirdOperatorInterventionError = ScratchbirdOperatorInterventionError;
class ScratchbirdSystemError extends ScratchbirdError {
}
exports.ScratchbirdSystemError = ScratchbirdSystemError;
class ScratchbirdInternalError extends ScratchbirdError {
}
exports.ScratchbirdInternalError = ScratchbirdInternalError;
function mapSqlState(code) {
    if (!code || code.length < 2) {
        return ScratchbirdError;
    }
    if (code.length === 5) {
        switch (code) {
            case "01000":
                return ScratchbirdWarning;
            case "02000":
                return ScratchbirdNoDataError;
            case "08001":
            case "08003":
            case "08004":
            case "08006":
            case "08P01":
                return ScratchbirdConnectionError;
            case "0A000":
                return ScratchbirdNotSupportedError;
            case "22001":
            case "22003":
            case "22007":
            case "22012":
            case "22023":
            case "22P02":
            case "22P03":
                return ScratchbirdDataError;
            case "23000":
            case "23502":
            case "23503":
            case "23505":
            case "23514":
                return ScratchbirdIntegrityError;
            case "28000":
            case "28P01":
                return ScratchbirdAuthError;
            case "40001":
            case "40P01":
                return ScratchbirdTransactionError;
            case "42501":
            case "42601":
            case "42703":
            case "42704":
            case "42710":
            case "42883":
            case "42P01":
            case "42P07":
                return ScratchbirdSyntaxError;
            case "53P00":
            case "53100":
            case "53200":
            case "53300":
                return ScratchbirdResourceError;
            case "54000":
                return ScratchbirdLimitError;
            case "57014":
            case "57P01":
            case "57P03":
                return ScratchbirdOperatorInterventionError;
            case "58000":
                return ScratchbirdSystemError;
            case "XX000":
                return ScratchbirdInternalError;
        }
    }
    const stateClass = code.slice(0, 2);
    switch (stateClass) {
        case "01":
            return ScratchbirdWarning;
        case "02":
            return ScratchbirdNoDataError;
        case "08":
            return ScratchbirdConnectionError;
        case "0A":
            return ScratchbirdNotSupportedError;
        case "22":
            return ScratchbirdDataError;
        case "23":
            return ScratchbirdIntegrityError;
        case "28":
            return ScratchbirdAuthError;
        case "40":
            return ScratchbirdTransactionError;
        case "42":
            return ScratchbirdSyntaxError;
        case "53":
            return ScratchbirdResourceError;
        case "54":
            return ScratchbirdLimitError;
        case "57":
            return ScratchbirdOperatorInterventionError;
        case "58":
            return ScratchbirdSystemError;
        case "XX":
            return ScratchbirdInternalError;
    }
    return ScratchbirdError;
}
function retryScopeForSqlState(code) {
    // Drivers are fail-closed: fresh statement restart for 40xxx, reconnect
    // only for 08xxx, and no automatic whole-transaction replay.
    if (!code || code.length !== 5) {
        return "none";
    }
    if (code === "40001" || code === "40P01") {
        return "statement";
    }
    if (code.slice(0, 2) === "08") {
        return "reconnect";
    }
    return "none";
}
function isRetryableSqlState(code) {
    return retryScopeForSqlState(code) !== "none";
}
