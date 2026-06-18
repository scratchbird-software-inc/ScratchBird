// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func openTestDB(t *testing.T) *sql.DB {
	dsn := os.Getenv("SCRATCHBIRD_GO_URL")
	if dsn == "" {
		t.Skip("SCRATCHBIRD_GO_URL not set")
	}
	db, err := sql.Open("scratchbird", dsn)
	if err != nil {
		t.Fatalf("open error: %v", err)
	}
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	return db
}

func openDirectConn(t *testing.T) *Conn {
	t.Helper()
	dsn := os.Getenv("SCRATCHBIRD_GO_URL")
	if dsn == "" {
		t.Skip("SCRATCHBIRD_GO_URL not set")
	}
	cfg, err := ParseConfig(dsn)
	if err != nil {
		t.Fatalf("parse config error: %v", err)
	}
	conn := &Conn{config: cfg}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := conn.Ping(ctx); err != nil {
		t.Fatalf("connect ping error: %v", err)
	}
	return conn
}

func TestIntegrationSelect(t *testing.T) {
	db := openTestDB(t)
	defer db.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	table := createTempTable(t, db, ctx)
	defer db.Exec("DROP TABLE " + table)
	rows, err := db.QueryContext(ctx, "SELECT value FROM "+table)
	if err != nil {
		t.Fatalf("query error: %v", err)
	}
	rows.Close()
}

func TestIntegrationPrepareBind(t *testing.T) {
	db := openTestDB(t)
	defer db.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	table := createTempTable(t, db, ctx)
	defer db.Exec("DROP TABLE " + table)
	rows, err := db.QueryContext(ctx, "SELECT value FROM "+table+" WHERE value = ?::INTEGER", 42)
	if err != nil {
		t.Fatalf("query error: %v", err)
	}
	rows.Close()
}

func TestIntegrationTypesFixture(t *testing.T) {
	db := openTestDB(t)
	defer db.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	applyFixtures(t, db, ctx)
	rows, err := db.QueryContext(ctx, "SELECT * FROM type_coverage")
	if err != nil {
		t.Fatalf("query error: %v", err)
	}
	rows.Close()
}

func TestIntegrationCancel(t *testing.T) {
	db := openTestDB(t)
	defer db.Close()
	cancelSQL := os.Getenv("SCRATCHBIRD_GO_CANCEL_SQL")
	if cancelSQL == "" {
		t.Skip("SCRATCHBIRD_GO_CANCEL_SQL not set")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	_, err := db.ExecContext(ctx, cancelSQL)
	if err == nil {
		t.Fatalf("expected cancel error")
	}
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Logf("cancel error: %v", err)
	}
}

func TestIntegrationDirectConnAdoptsFreshBoundaryAndRejectsNestedBegin(t *testing.T) {
	conn := openDirectConn(t)
	defer conn.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	txDriver, err := conn.BeginTx(ctx, driver.TxOptions{})
	if err != nil {
		t.Fatalf("begin tx error: %v", err)
	}
	if !conn.hasActiveTransaction() {
		t.Fatalf("expected active native transaction boundary after begin")
	}
	if !conn.explicitTransaction {
		t.Fatalf("expected explicit transaction marker after begin")
	}

	_, err = conn.BeginTx(ctx, driver.TxOptions{})
	if err == nil {
		t.Fatalf("expected nested begin rejection")
	}
	var driverErr *Error
	if !errors.As(err, &driverErr) || driverErr.SQLState != "25001" {
		t.Fatalf("expected 25001 nested begin error, got %v", err)
	}

	tx := txDriver.(*Tx)
	if err := tx.Rollback(); err != nil {
		t.Fatalf("rollback error: %v", err)
	}
	if !conn.hasActiveTransaction() {
		t.Fatalf("expected fresh native boundary after rollback")
	}
}

func TestIntegrationDirectConnPostRollbackQueryObservesActualResult(t *testing.T) {
	conn := openDirectConn(t)
	defer conn.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	txDriver, err := conn.BeginTx(ctx, driver.TxOptions{})
	if err != nil {
		t.Fatalf("begin tx error: %v", err)
	}
	if err := txDriver.(*Tx).Rollback(); err != nil {
		t.Fatalf("rollback error: %v", err)
	}

	rowsIface, err := conn.QueryContext(ctx, "SELECT 2", nil)
	if err != nil {
		t.Fatalf("query error: %v", err)
	}
	rows := rowsIface.(*Rows)
	defer rows.Close()

	dest := make([]driver.Value, 1)
	if err := rows.Next(dest); err != nil {
		t.Fatalf("next error: %v", err)
	}
	if got := fmt.Sprint(dest[0]); got != "2" {
		t.Fatalf("unexpected row value: got %q want %q", got, "2")
	}
}

func createTempTable(t *testing.T, db *sql.DB, ctx context.Context) string {
	t.Helper()
	table := fmt.Sprintf("scratchbird_go_tmp_%d", time.Now().UnixNano())
	createSQL := "CREATE TABLE " + table + " (value INTEGER)"
	if _, err := db.ExecContext(ctx, createSQL); err != nil {
		t.Fatalf("create table error: %v", err)
	}
	if _, err := db.ExecContext(ctx, "INSERT INTO "+table+" (value) VALUES (42)"); err != nil {
		t.Fatalf("insert error: %v", err)
	}
	return table
}

func applyFixtures(t *testing.T, db *sql.DB, ctx context.Context) {
	t.Helper()
	fixtureDir := os.Getenv("SCRATCHBIRD_FIXTURES_DIR")
	if fixtureDir == "" {
		t.Skip("SCRATCHBIRD_FIXTURES_DIR not set")
	}
	if rows, err := db.QueryContext(ctx, "SELECT 1 FROM type_coverage"); err == nil {
		rows.Close()
		return
	}
	fixtures := []string{"core_fixture.sql", "types_fixture.sql"}
	for _, fixture := range fixtures {
		path := filepath.Join(fixtureDir, fixture)
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatalf("read fixture error: %v", err)
		}
		statements := splitSQLStatements(string(data))
		for _, statement := range statements {
			if _, err := db.ExecContext(ctx, statement); err != nil {
				if strings.Contains(strings.ToLower(err.Error()), "already exists") {
					continue
				}
				t.Fatalf("fixture error: %v", err)
			}
		}
	}
}

// splitSQLStatements delegates to the canonical SET TERM- and comment-aware
// statement chunker shared across the Go driver.
func splitSQLStatements(input string) []string {
	return SplitTopLevelStatements(input)
}
