// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import "testing"

func TestMapSQLStateKnownMappings(t *testing.T) {
	tests := []struct {
		name     string
		sqlState string
		wantKind ErrorKind
	}{
		{name: "warning", sqlState: "01000", wantKind: ErrWarning},
		{name: "no data", sqlState: "02000", wantKind: ErrNoData},
		{name: "connection", sqlState: "08006", wantKind: ErrConnection},
		{name: "not supported", sqlState: "0A000", wantKind: ErrNotSupported},
		{name: "data", sqlState: "22P02", wantKind: ErrData},
		{name: "integrity", sqlState: "23505", wantKind: ErrIntegrity},
		{name: "auth", sqlState: "28P01", wantKind: ErrAuth},
		{name: "transaction", sqlState: "40001", wantKind: ErrTransaction},
		{name: "syntax", sqlState: "42601", wantKind: ErrSyntax},
		{name: "resource", sqlState: "53300", wantKind: ErrResource},
		{name: "limit", sqlState: "54000", wantKind: ErrLimit},
		{name: "operator", sqlState: "57014", wantKind: ErrOperator},
		{name: "system", sqlState: "58000", wantKind: ErrSystem},
		{name: "internal", sqlState: "XX000", wantKind: ErrInternal},
	}

	for _, tc := range tests {
		tc := tc
		t.Run(tc.name, func(t *testing.T) {
			if got := mapSQLState(tc.sqlState); got != tc.wantKind {
				t.Fatalf("mapSQLState(%q) mismatch: got %q want %q", tc.sqlState, got, tc.wantKind)
			}
		})
	}
}

func TestMapSQLStateClassFallbackAndInvalidLength(t *testing.T) {
	if got := mapSQLState("08ZZZ"); got != ErrConnection {
		t.Fatalf("expected class fallback connection kind, got %q", got)
	}
	if got := mapSQLState("22ZZZ"); got != ErrData {
		t.Fatalf("expected class fallback data kind, got %q", got)
	}
	if got := mapSQLState("ZZZZZ"); got != ErrUnknown {
		t.Fatalf("expected unknown kind for unknown SQLSTATE class, got %q", got)
	}
	if got := mapSQLState("123"); got != ErrUnknown {
		t.Fatalf("expected unknown kind for short SQLSTATE, got %q", got)
	}
}

func TestDriverErrorStringFormatting(t *testing.T) {
	err := &Error{Message: "boom", SQLState: "42P01"}
	if got, want := err.Error(), "boom (42P01)"; got != want {
		t.Fatalf("formatted error mismatch: got %q want %q", got, want)
	}

	err = &Error{Message: "boom"}
	if got, want := err.Error(), "boom"; got != want {
		t.Fatalf("plain error mismatch: got %q want %q", got, want)
	}

	var nilErr *Error
	if got := nilErr.Error(); got != "" {
		t.Fatalf("nil error string mismatch: got %q", got)
	}
}

func TestRetryScopeForSQLState(t *testing.T) {
	tests := []struct {
		sqlState string
		want     RetryScope
	}{
		{sqlState: "40001", want: RetryScopeStatement},
		{sqlState: "40P01", want: RetryScopeStatement},
		{sqlState: "08006", want: RetryScopeReconnect},
		{sqlState: "08P01", want: RetryScopeReconnect},
		{sqlState: "57014", want: RetryScopeNone},
		{sqlState: "23505", want: RetryScopeNone},
		{sqlState: "", want: RetryScopeNone},
	}

	for _, tc := range tests {
		if got := RetryScopeForSQLState(tc.sqlState); got != tc.want {
			t.Fatalf("RetryScopeForSQLState(%q) mismatch: got %q want %q", tc.sqlState, got, tc.want)
		}
		if got := IsRetryableSQLState(tc.sqlState); got != (tc.want != RetryScopeNone) {
			t.Fatalf("IsRetryableSQLState(%q) mismatch: got %t", tc.sqlState, got)
		}
	}
}
