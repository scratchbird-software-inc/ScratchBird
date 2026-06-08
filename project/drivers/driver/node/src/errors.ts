// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

export class ScratchbirdError extends Error {
  code?: string;
  detail?: string;
  hint?: string;
  constructor(message: string, code?: string, detail?: string, hint?: string) {
    super(message);
    this.name = this.constructor.name;
    this.code = code;
    this.detail = detail;
    this.hint = hint;
  }
}

export class ScratchbirdWarning extends ScratchbirdError {}
export class ScratchbirdNoDataError extends ScratchbirdError {}
export class ScratchbirdConnectionError extends ScratchbirdError {}
export class ScratchbirdNotSupportedError extends ScratchbirdError {}
export class ScratchbirdDataError extends ScratchbirdError {}
export class ScratchbirdIntegrityError extends ScratchbirdError {}
export class ScratchbirdAuthError extends ScratchbirdError {}
export class ScratchbirdTransactionError extends ScratchbirdError {}
export class ScratchbirdSyntaxError extends ScratchbirdError {}
export class ScratchbirdResourceError extends ScratchbirdError {}
export class ScratchbirdLimitError extends ScratchbirdError {}
export class ScratchbirdOperatorInterventionError extends ScratchbirdError {}
export class ScratchbirdSystemError extends ScratchbirdError {}
export class ScratchbirdInternalError extends ScratchbirdError {}

export type RetryScope = "none" | "reconnect" | "statement" | "transaction";

export function mapSqlState(code?: string): new (...args: any[]) => ScratchbirdError {
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

export function retryScopeForSqlState(code?: string): RetryScope {
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

export function isRetryableSqlState(code?: string): boolean {
  return retryScopeForSqlState(code) !== "none";
}
