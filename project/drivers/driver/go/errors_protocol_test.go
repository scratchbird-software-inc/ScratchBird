// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"errors"
	"strings"
	"testing"
)

func TestBuildProtocolErrorMapsSQLStateAndCarriesDetailHint(t *testing.T) {
	payload := testErrorMessagePayload("ERROR", "23505", "duplicate key", "Key (id)=(1) exists", "Use a new id")

	err := buildProtocolError(payload)
	requireDriverError(t, err, ErrIntegrity, "23505")

	var sbErr *Error
	if !errors.As(err, &sbErr) {
		t.Fatalf("expected *Error, got %T", err)
	}
	if sbErr.Message != "duplicate key" {
		t.Fatalf("message mismatch: got %q", sbErr.Message)
	}
	if sbErr.Detail != "Key (id)=(1) exists" {
		t.Fatalf("detail mismatch: got %q", sbErr.Detail)
	}
	if sbErr.Hint != "Use a new id" {
		t.Fatalf("hint mismatch: got %q", sbErr.Hint)
	}
}

func TestBuildProtocolErrorUsesUnknownKindForUnmappedSQLState(t *testing.T) {
	payload := testErrorMessagePayload("ERROR", "99999", "unexpected engine failure", "", "")

	err := buildProtocolError(payload)
	requireDriverError(t, err, ErrUnknown, "99999")
}

func TestBuildProtocolErrorReturnsParseErrorForTruncatedPayload(t *testing.T) {
	err := buildProtocolError([]byte{'M', 'b', 'a', 'd'})
	if err == nil {
		t.Fatalf("expected parse error for truncated payload")
	}
	if !strings.Contains(err.Error(), "error message truncated") {
		t.Fatalf("unexpected parse error: %v", err)
	}
}

func testErrorMessagePayload(severity, sqlState, message, detail, hint string) []byte {
	var payload []byte
	appendField := func(tag byte, value string) {
		if value == "" {
			return
		}
		payload = append(payload, tag)
		payload = append(payload, value...)
		payload = append(payload, 0)
	}
	appendField('S', severity)
	appendField('C', sqlState)
	appendField('M', message)
	appendField('D', detail)
	appendField('H', hint)
	payload = append(payload, 0)
	return payload
}
