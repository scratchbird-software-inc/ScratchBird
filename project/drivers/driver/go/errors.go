// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import "fmt"

type ErrorKind string
type RetryScope string

const (
	ErrWarning      ErrorKind = "warning"
	ErrNoData       ErrorKind = "no_data"
	ErrConnection   ErrorKind = "connection"
	ErrNotSupported ErrorKind = "not_supported"
	ErrData         ErrorKind = "data"
	ErrIntegrity    ErrorKind = "integrity"
	ErrAuth         ErrorKind = "auth"
	ErrTransaction  ErrorKind = "transaction"
	ErrSyntax       ErrorKind = "syntax"
	ErrResource     ErrorKind = "resource"
	ErrLimit        ErrorKind = "limit"
	ErrOperator     ErrorKind = "operator"
	ErrSystem       ErrorKind = "system"
	ErrInternal     ErrorKind = "internal"
	ErrUnknown      ErrorKind = "unknown"
)

const (
	RetryScopeNone        RetryScope = "none"
	RetryScopeReconnect   RetryScope = "reconnect"
	RetryScopeStatement   RetryScope = "statement"
	RetryScopeTransaction RetryScope = "transaction"
)

type Error struct {
	Kind     ErrorKind
	Code     uint32
	SQLState string
	Message  string
	Detail   string
	Hint     string
}

func (e *Error) Error() string {
	if e == nil {
		return ""
	}
	if e.SQLState != "" {
		return fmt.Sprintf("%s (%s)", e.Message, e.SQLState)
	}
	return e.Message
}

func mapSQLState(sqlState string) ErrorKind {
	if len(sqlState) == 5 {
		switch sqlState {
		case "01000":
			return ErrWarning
		case "02000":
			return ErrNoData
		case "08001", "08003", "08004", "08006", "08P01":
			return ErrConnection
		case "0A000":
			return ErrNotSupported
		case "22001", "22003", "22007", "22012", "22023", "22P02", "22P03":
			return ErrData
		case "23000", "23502", "23503", "23505", "23514":
			return ErrIntegrity
		case "28000", "28P01":
			return ErrAuth
		case "40001", "40P01":
			return ErrTransaction
		case "42501", "42601", "42703", "42704", "42710", "42883", "42P01", "42P07":
			return ErrSyntax
		case "53P00", "53100", "53200", "53300":
			return ErrResource
		case "54000":
			return ErrLimit
		case "57014", "57P01", "57P03":
			return ErrOperator
		case "58000":
			return ErrSystem
		case "XX000":
			return ErrInternal
		}
		switch sqlState[:2] {
		case "01":
			return ErrWarning
		case "02":
			return ErrNoData
		case "08":
			return ErrConnection
		case "0A":
			return ErrNotSupported
		case "22":
			return ErrData
		case "23":
			return ErrIntegrity
		case "28":
			return ErrAuth
		case "40":
			return ErrTransaction
		case "42":
			return ErrSyntax
		case "53":
			return ErrResource
		case "54":
			return ErrLimit
		case "57":
			return ErrOperator
		case "58":
			return ErrSystem
		case "XX":
			return ErrInternal
		}
	}
	return ErrUnknown
}

// RetryScopeForSQLState classifies whether a driver may retry from a fresh
// boundary. ScratchBird drivers do not replay in-flight transactions:
//
//   - 40001 / 40P01 => retry only from a fresh statement boundary
//   - 08xxx         => reconnect or reopen only
//   - everything else, including 57014, requires explicit caller policy
func RetryScopeForSQLState(sqlState string) RetryScope {
	if len(sqlState) != 5 {
		return RetryScopeNone
	}
	switch sqlState {
	case "40001", "40P01":
		return RetryScopeStatement
	}
	if sqlState[:2] == "08" {
		return RetryScopeReconnect
	}
	return RetryScopeNone
}

func IsRetryableSQLState(sqlState string) bool {
	return RetryScopeForSQLState(sqlState) != RetryScopeNone
}
