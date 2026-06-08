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
	"encoding/binary"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"
)

func TestBeginTxRejectsUnsupportedIsolation(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}

	_, err := conn.BeginTx(context.Background(), driver.TxOptions{
		Isolation: driver.IsolationLevel(sql.LevelSnapshot),
	})
	requireDriverError(t, err, ErrNotSupported, "0A000")
}

func TestCanonicalIsolationLabelForDriverIsolation(t *testing.T) {
	tests := []struct {
		name      string
		level     driver.IsolationLevel
		want      string
		wantKnown bool
	}{
		{name: "read uncommitted", level: driver.IsolationLevel(sql.LevelReadUncommitted), want: "READ COMMITTED", wantKnown: true},
		{name: "read committed", level: driver.IsolationLevel(sql.LevelReadCommitted), want: "READ COMMITTED", wantKnown: true},
		{name: "repeatable read", level: driver.IsolationLevel(sql.LevelRepeatableRead), want: "SNAPSHOT", wantKnown: true},
		{name: "serializable", level: driver.IsolationLevel(sql.LevelSerializable), want: "SNAPSHOT TABLE STABILITY", wantKnown: true},
		{name: "unsupported", level: driver.IsolationLevel(sql.LevelSnapshot), wantKnown: false},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got, ok := CanonicalIsolationLabelForDriverIsolation(tc.level)
			if ok != tc.wantKnown {
				t.Fatalf("known mismatch: got %t want %t", ok, tc.wantKnown)
			}
			if got != tc.want {
				t.Fatalf("label mismatch: got %q want %q", got, tc.want)
			}
		})
	}
}

func TestCanonicalReadCommittedModeLabel(t *testing.T) {
	tests := []struct {
		name string
		mode ReadCommittedMode
		want string
	}{
		{name: "default", mode: ReadCommittedModeDefault, want: "READ COMMITTED"},
		{name: "read consistency", mode: ReadCommittedModeReadConsistency, want: "READ COMMITTED READ CONSISTENCY"},
		{name: "record version", mode: ReadCommittedModeRecordVersion, want: "READ COMMITTED RECORD VERSION"},
		{name: "no record version", mode: ReadCommittedModeNoRecordVersion, want: "READ COMMITTED NO RECORD VERSION"},
		{name: "unknown", mode: ReadCommittedMode(99), want: "UNKNOWN(99)"},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if got := CanonicalReadCommittedModeLabel(tc.mode); got != tc.want {
				t.Fatalf("label mismatch: got %q want %q", got, tc.want)
			}
		})
	}
}

func TestBeginTxEncodesIsolationAndReadOnly(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read begin message: %w", err)
			return
		}
		if msg.header.typ != msgTxnBegin {
			errCh <- fmt.Errorf("expected %v, got %v", msgTxnBegin, msg.header.typ)
			return
		}
		if len(msg.body) < 12 {
			errCh <- fmt.Errorf("txn begin payload too short: %d", len(msg.body))
			return
		}

		flags := binary.LittleEndian.Uint16(msg.body[0:2])
		wantFlags := uint16(txnFlagHasIsolation | txnFlagHasAccess)
		if flags != wantFlags {
			errCh <- fmt.Errorf("unexpected txn flags: got %d want %d", flags, wantFlags)
			return
		}
		if msg.body[4] != isolationSerializable {
			errCh <- fmt.Errorf("unexpected isolation byte: got %d want %d", msg.body[4], isolationSerializable)
			return
		}
		if msg.body[5] != 1 {
			errCh <- fmt.Errorf("unexpected access mode byte: got %d want 1", msg.body[5])
			return
		}

		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 77, 0))); err != nil {
			errCh <- fmt.Errorf("write ready: %w", err)
			return
		}

		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}

	tx, err := conn.BeginTx(context.Background(), driver.TxOptions{
		Isolation: driver.IsolationLevel(sql.LevelSerializable),
		ReadOnly:  true,
	})
	if err != nil {
		t.Fatalf("begin tx failed: %v", err)
	}
	if tx == nil {
		t.Fatalf("expected tx, got nil")
	}
	if conn.txnID != 77 {
		t.Fatalf("expected txn id 77, got %d", conn.txnID)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestBeginTxExEncodesReadCommittedMode(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read begin message: %w", err)
			return
		}
		if msg.header.typ != msgTxnBegin {
			errCh <- fmt.Errorf("expected %v, got %v", msgTxnBegin, msg.header.typ)
			return
		}
		if len(msg.body) < 16 {
			errCh <- fmt.Errorf("txn begin payload too short for read committed mode: %d", len(msg.body))
			return
		}

		flags := binary.LittleEndian.Uint16(msg.body[0:2])
		wantFlags := uint16(txnFlagHasIsolation | txnFlagHasReadCommittedMode)
		if flags != wantFlags {
			errCh <- fmt.Errorf("unexpected txn flags: got %d want %d", flags, wantFlags)
			return
		}
		if msg.body[4] != isolationReadCommitted {
			errCh <- fmt.Errorf("unexpected isolation byte: got %d want %d", msg.body[4], isolationReadCommitted)
			return
		}
		if msg.body[12] != readCommittedModeReadConsistency {
			errCh <- fmt.Errorf("unexpected read committed mode byte: got %d want %d", msg.body[12], readCommittedModeReadConsistency)
			return
		}

		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 88, 0))); err != nil {
			errCh <- fmt.Errorf("write ready: %w", err)
			return
		}

		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	mode := ReadCommittedModeReadConsistency
	tx, err := conn.BeginTxEx(context.Background(), TxnBeginOptions{
		ReadCommittedMode: &mode,
	})
	if err != nil {
		t.Fatalf("begin tx ex failed: %v", err)
	}
	if tx == nil {
		t.Fatalf("expected tx, got nil")
	}
	if conn.txnID != 88 {
		t.Fatalf("expected txn id 88, got %d", conn.txnID)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestBeginTxExRejectsReadCommittedModeWithSnapshotAlias(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	mode := ReadCommittedModeReadConsistency
	_, err := conn.BeginTxEx(context.Background(), TxnBeginOptions{
		Isolation:         driver.IsolationLevel(sql.LevelSerializable),
		ReadCommittedMode: &mode,
	})
	requireDriverError(t, err, ErrNotSupported, "0A000")
}

func TestReadyStatusKeepsFreshNativeBoundaryActiveWithZeroTxnID(t *testing.T) {
	conn := &Conn{config: defaultConfig()}
	conn.applyRuntimeReadyState('T', 0)
	if !conn.hasActiveTransaction() {
		t.Fatalf("expected fresh native boundary to remain active")
	}
	if conn.txnID != 0 {
		t.Fatalf("expected txn id 0, got %d", conn.txnID)
	}
}

func TestBeginTxRestartsImplicitBoundaryAndRejectsNestedBegin(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read begin message: %w", err)
			return
		}
		if msg.header.typ != msgTxnBegin {
			errCh <- fmt.Errorf("expected %v, got %v", msgTxnBegin, msg.header.typ)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 44, 0))); err != nil {
			errCh <- fmt.Errorf("write begin ready: %w", err)
			return
		}
		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	conn.applyRuntimeReadyState('T', 0)

	tx, err := conn.BeginTx(context.Background(), driver.TxOptions{})
	if err != nil {
		t.Fatalf("begin tx failed: %v", err)
	}
	if tx == nil {
		t.Fatalf("expected tx, got nil")
	}
	if !conn.explicitTransaction {
		t.Fatalf("expected explicit transaction marker after adoption")
	}
	if !conn.hasActiveTransaction() {
		t.Fatalf("expected active transaction after begin restart")
	}
	if conn.txnID != 44 {
		t.Fatalf("expected current txn id to refresh after begin restart, got %d", conn.txnID)
	}

	_, err = conn.BeginTx(context.Background(), driver.TxOptions{})
	requireDriverError(t, err, ErrTransaction, "25001")
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestBeginTxAllowsNonDefaultRestartOptions(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read begin message: %w", err)
			return
		}
		if msg.header.typ != msgTxnBegin {
			errCh <- fmt.Errorf("expected %v, got %v", msgTxnBegin, msg.header.typ)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 45, 0))); err != nil {
			errCh <- fmt.Errorf("write begin ready: %w", err)
			return
		}
		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	conn.applyRuntimeReadyState('T', 0)

	tx, err := conn.BeginTxEx(context.Background(), TxnBeginOptions{
		Isolation: driver.IsolationLevel(sql.LevelSerializable),
	})
	if err != nil {
		t.Fatalf("begin tx failed: %v", err)
	}
	if tx == nil {
		t.Fatalf("expected tx, got nil")
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestCommitDrainsImmediateReopenBoundary(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read commit message: %w", err)
			return
		}
		if msg.header.typ != msgTxnCommit {
			errCh <- fmt.Errorf("expected %v, got %v", msgTxnCommit, msg.header.typ)
			return
		}

		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload(0, 0, 0))); err != nil {
			errCh <- fmt.Errorf("write commit ready: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 0, 0))); err != nil {
			errCh <- fmt.Errorf("write reopen ready: %w", err)
			return
		}
		time.Sleep(20 * time.Millisecond)
		errCh <- nil
	}()

	conn := &Conn{
		config:              defaultConfig(),
		raw:                 client,
		runtimeTxnActive:    true,
		explicitTransaction: true,
	}
	tx := &Tx{conn: conn}
	if err := tx.Commit(); err != nil {
		t.Fatalf("commit failed: %v", err)
	}
	if !conn.hasActiveTransaction() {
		t.Fatalf("expected fresh boundary to remain active after commit")
	}
	if conn.explicitTransaction {
		t.Fatalf("expected explicit transaction marker cleared after commit")
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestPreparedTransactionHelpersEmitCanonicalControlSQL(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		tests := []struct {
			name string
			want string
		}{
			{name: "prepare", want: "PREPARE TRANSACTION 'gid-1'"},
			{name: "commit prepared", want: "COMMIT PREPARED 'gid-1'"},
			{name: "rollback prepared", want: "ROLLBACK PREPARED 'gid''2'"},
		}

		for idx, tc := range tests {
			msg, err := readMessage(server)
			if err != nil {
				errCh <- fmt.Errorf("read %s message: %w", tc.name, err)
				return
			}
			if msg.header.typ != msgQuery {
				errCh <- fmt.Errorf("%s expected %v, got %v", tc.name, msgQuery, msg.header.typ)
				return
			}
			if got := parseQuerySQL(msg.body); got != tc.want {
				errCh <- fmt.Errorf("%s sql mismatch: got %q want %q", tc.name, got, tc.want)
				return
			}
			if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload(0, uint64(idx+1), 0))); err != nil {
				errCh <- fmt.Errorf("write ready for %s: %w", tc.name, err)
				return
			}
		}

		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}

	if err := conn.PrepareTransaction(context.Background(), "gid-1"); err != nil {
		t.Fatalf("prepare transaction failed: %v", err)
	}
	if err := conn.CommitPrepared(context.Background(), "gid-1"); err != nil {
		t.Fatalf("commit prepared failed: %v", err)
	}
	if err := conn.RollbackPrepared(context.Background(), "gid'2"); err != nil {
		t.Fatalf("rollback prepared failed: %v", err)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestPreparedTransactionHelpersRejectEmptyGid(t *testing.T) {
	conn := &Conn{}
	err := conn.PrepareTransaction(context.Background(), "   ")
	requireDriverError(t, err, ErrSyntax, "42601")
}

func TestDormantHelpersFailClosedAndCapabilitiesStayExplicit(t *testing.T) {
	conn := &Conn{}

	if !conn.SupportsPreparedTransactions() {
		t.Fatalf("expected prepared transactions support to stay explicit")
	}
	if conn.SupportsDormantReattach() {
		t.Fatalf("expected dormant reattach support to stay fail-closed")
	}

	requireDriverError(t, conn.DetachToDormant(context.Background()), ErrNotSupported, "0A000")
	requireDriverError(t, conn.ReattachDormant(context.Background(), "dormant-1", "token-1"), ErrNotSupported, "0A000")
}

func TestSavepointLifecycleEncodesWireCalls(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		if err := expectMessage(server, msgTxnBegin); err != nil {
			errCh <- err
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 77, 0))); err != nil {
			errCh <- fmt.Errorf("write begin ready: %w", err)
			return
		}

		savepointMsg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read savepoint message: %w", err)
			return
		}
		if savepointMsg.header.typ != msgTxnSavepoint {
			errCh <- fmt.Errorf("expected %v, got %v", msgTxnSavepoint, savepointMsg.header.typ)
			return
		}
		if !containsPayloadText(savepointMsg.body, "sp1") {
			errCh <- fmt.Errorf("savepoint payload missing name")
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 77, 0))); err != nil {
			errCh <- fmt.Errorf("write savepoint ready: %w", err)
			return
		}

		rollbackToMsg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read rollback-to message: %w", err)
			return
		}
		if rollbackToMsg.header.typ != msgTxnRollbackTo {
			errCh <- fmt.Errorf("expected %v, got %v", msgTxnRollbackTo, rollbackToMsg.header.typ)
			return
		}
		if !containsPayloadText(rollbackToMsg.body, "sp1") {
			errCh <- fmt.Errorf("rollback-to payload missing name")
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 77, 0))); err != nil {
			errCh <- fmt.Errorf("write rollback-to ready: %w", err)
			return
		}

		releaseMsg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read release message: %w", err)
			return
		}
		if releaseMsg.header.typ != msgTxnRelease {
			errCh <- fmt.Errorf("expected %v, got %v", msgTxnRelease, releaseMsg.header.typ)
			return
		}
		if !containsPayloadText(releaseMsg.body, "sp1") {
			errCh <- fmt.Errorf("release payload missing name")
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 77, 0))); err != nil {
			errCh <- fmt.Errorf("write release ready: %w", err)
			return
		}

		if err := expectMessage(server, msgTxnCommit); err != nil {
			errCh <- err
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload(0, 0, 0))); err != nil {
			errCh <- fmt.Errorf("write commit ready: %w", err)
			return
		}
		time.Sleep(20 * time.Millisecond)
		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	txDriver, err := conn.BeginTx(context.Background(), driver.TxOptions{})
	if err != nil {
		t.Fatalf("begin tx failed: %v", err)
	}
	tx, ok := txDriver.(*Tx)
	if !ok {
		t.Fatalf("expected *Tx, got %T", txDriver)
	}
	if err := tx.Savepoint("sp1"); err != nil {
		t.Fatalf("savepoint failed: %v", err)
	}
	if err := tx.RollbackToSavepoint("sp1"); err != nil {
		t.Fatalf("rollback to savepoint failed: %v", err)
	}
	if err := tx.ReleaseSavepoint("sp1"); err != nil {
		t.Fatalf("release savepoint failed: %v", err)
	}
	if err := tx.Commit(); err != nil {
		t.Fatalf("commit failed: %v", err)
	}
	if conn.txnID != 0 {
		t.Fatalf("expected txn id 0 after commit, got %d", conn.txnID)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func parseQuerySQL(payload []byte) string {
	if len(payload) <= 12 {
		return ""
	}
	raw := payload[12:]
	if idx := strings.IndexByte(string(raw), 0); idx >= 0 {
		raw = raw[:idx]
	}
	return string(raw)
}

func TestSavepointRejectsWhenTransactionInactive(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	err := conn.Savepoint(context.Background(), "sp1")
	requireDriverError(t, err, ErrTransaction, "25000")
}

func TestSavepointRejectsBlankName(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
		txnID:  99,
	}
	err := conn.Savepoint(context.Background(), "   ")
	requireDriverError(t, err, ErrSyntax, "42601")
}

func TestResetSessionRollsBackActiveTransactionAndClearsBorrowState(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read reset rollback message: %w", err)
			return
		}
		if msg.header.typ != msgTxnRollback {
			errCh <- fmt.Errorf("expected %v, got %v", msgTxnRollback, msg.header.typ)
			return
		}
		if msg.header.txnID != 88 {
			errCh <- fmt.Errorf("expected rollback txn id 88, got %d", msg.header.txnID)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload(0, 0, 0))); err != nil {
			errCh <- fmt.Errorf("write reset ready: %w", err)
			return
		}
		errCh <- nil
	}()

	conn := &Conn{
		config:   defaultConfig(),
		raw:      client,
		authed:   true,
		txnID:    88,
		lastPlan: &queryPlan{format: 1},
		lastSblr: &sblrCompiled{hash: 99},
	}

	if err := conn.ResetSession(context.Background()); err != nil {
		t.Fatalf("reset session failed: %v", err)
	}
	if conn.txnID != 0 {
		t.Fatalf("expected txn id 0 after reset, got %d", conn.txnID)
	}
	if conn.LastPlan() != nil {
		t.Fatalf("expected last plan cleared on reset")
	}
	if conn.LastSblr() != nil {
		t.Fatalf("expected last SBLR cleared on reset")
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestResetSessionReturnsBadConnForClosedHandle(t *testing.T) {
	conn := &Conn{
		config:   defaultConfig(),
		closed:   true,
		txnID:    55,
		lastPlan: &queryPlan{format: 1},
		lastSblr: &sblrCompiled{hash: 77},
	}

	err := conn.ResetSession(context.Background())
	if err != driver.ErrBadConn {
		t.Fatalf("expected driver.ErrBadConn, got %v", err)
	}
	if conn.txnID != 0 {
		t.Fatalf("expected txn id cleared for closed handle, got %d", conn.txnID)
	}
	if conn.LastPlan() != nil {
		t.Fatalf("expected last plan cleared for closed handle")
	}
	if conn.LastSblr() != nil {
		t.Fatalf("expected last SBLR cleared for closed handle")
	}
}

func TestExecContextSimpleIgnoresFetchSizeForExec(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read query message: %w", err)
			return
		}
		if msg.header.typ != msgQuery {
			errCh <- fmt.Errorf("expected %v, got %v", msgQuery, msg.header.typ)
			return
		}
		if len(msg.body) < 12 {
			errCh <- fmt.Errorf("query payload too short: %d", len(msg.body))
			return
		}
		maxRows := binary.LittleEndian.Uint32(msg.body[4:8])
		if maxRows != 0 {
			errCh <- fmt.Errorf("exec simple query maxRows mismatch: got %d want 0", maxRows)
			return
		}

		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(3, 9, "UPDATE 3"))); err != nil {
			errCh <- fmt.Errorf("write command complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 88, 0))); err != nil {
			errCh <- fmt.Errorf("write ready: %w", err)
			return
		}

		errCh <- nil
	}()

	cfg := defaultConfig()
	cfg.FetchSize = 128
	conn := &Conn{
		config: cfg,
		raw:    client,
	}

	result, err := conn.ExecContext(context.Background(), "UPDATE demo SET value = 1", nil)
	if err != nil {
		t.Fatalf("exec failed: %v", err)
	}
	rows, err := result.RowsAffected()
	if err != nil {
		t.Fatalf("rows affected failed: %v", err)
	}
	if rows != 3 {
		t.Fatalf("rows affected mismatch: got %d want 3", rows)
	}
	lastID, err := result.LastInsertId()
	if err != nil {
		t.Fatalf("last insert id failed: %v", err)
	}
	if lastID != 9 {
		t.Fatalf("last insert id mismatch: got %d want 9", lastID)
	}
	if conn.txnID != 88 {
		t.Fatalf("expected txn id 88, got %d", conn.txnID)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestExecContextExtendedIgnoresFetchSizeForExec(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		if err := expectMessage(server, msgParse); err != nil {
			errCh <- err
			return
		}
		if err := expectMessage(server, msgDescribe); err != nil {
			errCh <- err
			return
		}
		if err := expectMessage(server, msgSync); err != nil {
			errCh <- err
			return
		}

		if _, err := server.Write(encodeMessage(messageHeader{typ: msgParameterDescription}, testParameterDescriptionPayload(oidInt4))); err != nil {
			errCh <- fmt.Errorf("write parameter description: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 90, 0))); err != nil {
			errCh <- fmt.Errorf("write describe ready: %w", err)
			return
		}

		if err := expectMessage(server, msgBind); err != nil {
			errCh <- err
			return
		}

		execMsg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read execute message: %w", err)
			return
		}
		if execMsg.header.typ != msgExecute {
			errCh <- fmt.Errorf("expected %v, got %v", msgExecute, execMsg.header.typ)
			return
		}
		maxRows, err := executeMaxRows(execMsg.body)
		if err != nil {
			errCh <- err
			return
		}
		if maxRows != 0 {
			errCh <- fmt.Errorf("exec extended maxRows mismatch: got %d want 0", maxRows)
			return
		}

		if err := expectMessage(server, msgSync); err != nil {
			errCh <- err
			return
		}

		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(1, 0, "UPDATE 1"))); err != nil {
			errCh <- fmt.Errorf("write command complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 91, 0))); err != nil {
			errCh <- fmt.Errorf("write ready: %w", err)
			return
		}

		errCh <- nil
	}()

	cfg := defaultConfig()
	cfg.FetchSize = 64
	conn := &Conn{
		config: cfg,
		raw:    client,
	}

	args := []driver.NamedValue{
		{Ordinal: 1, Value: int64(42)},
	}
	result, err := conn.ExecContext(context.Background(), "UPDATE demo SET value = ?", args)
	if err != nil {
		t.Fatalf("exec failed: %v", err)
	}
	rows, err := result.RowsAffected()
	if err != nil {
		t.Fatalf("rows affected failed: %v", err)
	}
	if rows != 1 {
		t.Fatalf("rows affected mismatch: got %d want 1", rows)
	}
	if conn.txnID != 91 {
		t.Fatalf("expected txn id 91, got %d", conn.txnID)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func expectMessage(conn net.Conn, want messageType) error {
	msg, err := readMessage(conn)
	if err != nil {
		return fmt.Errorf("read %v message: %w", want, err)
	}
	if msg.header.typ != want {
		return fmt.Errorf("expected %v, got %v", want, msg.header.typ)
	}
	return nil
}

func executeMaxRows(payload []byte) (uint32, error) {
	if len(payload) < 8 {
		return 0, fmt.Errorf("execute payload too short: %d", len(payload))
	}
	portalLen := int(binary.LittleEndian.Uint32(payload[0:4]))
	if len(payload) < 4+portalLen+4 {
		return 0, fmt.Errorf("execute payload truncated: %d", len(payload))
	}
	return binary.LittleEndian.Uint32(payload[4+portalLen : 8+portalLen]), nil
}

func containsPayloadText(payload []byte, value string) bool {
	return strings.Contains(string(payload), value)
}

func testReadyPayload(status byte, txnID, epoch uint64) []byte {
	payload := make([]byte, 20)
	payload[0] = status
	binary.LittleEndian.PutUint64(payload[4:12], txnID)
	binary.LittleEndian.PutUint64(payload[12:20], epoch)
	return payload
}

func testCommandCompletePayload(rows, lastID uint64, tag string) []byte {
	payload := make([]byte, 20+len(tag)+1)
	binary.LittleEndian.PutUint64(payload[4:12], rows)
	binary.LittleEndian.PutUint64(payload[12:20], lastID)
	copy(payload[20:], []byte(tag))
	return payload
}

func testParameterDescriptionPayload(typeOIDs ...uint32) []byte {
	payload := make([]byte, 4+len(typeOIDs)*4)
	binary.LittleEndian.PutUint16(payload[0:2], uint16(len(typeOIDs)))
	offset := 4
	for _, oid := range typeOIDs {
		binary.LittleEndian.PutUint32(payload[offset:offset+4], oid)
		offset += 4
	}
	return payload
}
