// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"database/sql/driver"
	"testing"
)

func TestRewritePositional(t *testing.T) {
	query := "SELECT * FROM t WHERE id = ? AND name = ?"
	args := []driver.NamedValue{
		{Ordinal: 1, Value: 42},
		{Ordinal: 2, Value: "Ada"},
	}
	out, err := normalizeQuery(query, args)
	if err != nil {
		t.Fatalf("rewrite error: %v", err)
	}
	expected := "SELECT * FROM t WHERE id = $1 AND name = $2"
	if out.sql != expected {
		t.Fatalf("unexpected query: %s", out.sql)
	}
	if len(out.args) != 2 {
		t.Fatalf("unexpected args: %d", len(out.args))
	}
}

func TestRewriteNamed(t *testing.T) {
	query := "SELECT * FROM users WHERE name = @name AND active = :active"
	args := []driver.NamedValue{
		{Name: "name", Value: "Ada"},
		{Name: "active", Value: true},
	}
	out, err := normalizeQuery(query, args)
	if err != nil {
		t.Fatalf("rewrite error: %v", err)
	}
	expected := "SELECT * FROM users WHERE name = $1 AND active = $2"
	if out.sql != expected {
		t.Fatalf("unexpected query: %s", out.sql)
	}
	if len(out.args) != 2 {
		t.Fatalf("unexpected args: %d", len(out.args))
	}
}

func TestNormalizeCallableProcedureEscape(t *testing.T) {
	query := "{call admin.refresh_cache(?)}"
	args := []driver.NamedValue{
		{Ordinal: 1, Value: int64(42)},
	}
	out, err := normalizeCallableQuery(query, args)
	if err != nil {
		t.Fatalf("callable rewrite error: %v", err)
	}
	if out.sql != "call admin.refresh_cache($1)" {
		t.Fatalf("unexpected callable query: %s", out.sql)
	}
	if len(out.args) != 1 {
		t.Fatalf("unexpected args: %d", len(out.args))
	}
}

func TestNormalizeCallableFunctionEscape(t *testing.T) {
	query := "{? = call math.add(?, ?)}"
	args := []driver.NamedValue{
		{Ordinal: 1, Value: int64(5)},
		{Ordinal: 2, Value: int64(7)},
	}
	out, err := normalizeCallableQuery(query, args)
	if err != nil {
		t.Fatalf("callable function rewrite error: %v", err)
	}
	if out.sql != "select math.add($1, $2) as return_value" {
		t.Fatalf("unexpected callable function query: %s", out.sql)
	}
	if len(out.args) != 2 {
		t.Fatalf("unexpected args: %d", len(out.args))
	}
}

func TestNormalizeCallableRejectsInvalidEscapeSyntax(t *testing.T) {
	if _, err := normalizeCallableSQL("{call bad(}"); err == nil {
		t.Fatalf("expected invalid callable escape syntax error")
	}
}
