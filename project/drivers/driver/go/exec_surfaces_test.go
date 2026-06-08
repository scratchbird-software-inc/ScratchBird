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
	"database/sql/driver"
	"encoding/binary"
	"fmt"
	"net"
	"testing"
)

func TestQueryMultiContextSummarizesResultSets(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		if err := expectMessage(server, msgQuery); err != nil {
			errCh <- err
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(2, 11, "UPDATE 2"))); err != nil {
			errCh <- fmt.Errorf("write first command complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(1, 12, "UPDATE 1"))); err != nil {
			errCh <- fmt.Errorf("write second command complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload(0, 44, 0))); err != nil {
			errCh <- fmt.Errorf("write ready: %w", err)
			return
		}
		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	sets, err := conn.QueryMultiContext(context.Background(), "UPDATE t SET v = 1; UPDATE t SET v = 2", nil)
	if err != nil {
		t.Fatalf("query multi failed: %v", err)
	}
	if len(sets) != 2 {
		t.Fatalf("expected 2 result sets, got %d", len(sets))
	}
	if sets[0].RowCount != 2 || sets[0].Command != "UPDATE 2" || sets[0].LastInsertID != 11 {
		t.Fatalf("unexpected first set summary: %+v", sets[0])
	}
	if sets[1].RowCount != 1 || sets[1].Command != "UPDATE 1" || sets[1].LastInsertID != 12 {
		t.Fatalf("unexpected second set summary: %+v", sets[1])
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestExecuteBatchContextSummarizesItems(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		for i, rows := range []uint64{3, 4} {
			if err := expectMessage(server, msgQuery); err != nil {
				errCh <- err
				return
			}
			lastID := uint64(20 + i)
			tag := fmt.Sprintf("UPDATE %d", rows)
			if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(rows, lastID, tag))); err != nil {
				errCh <- fmt.Errorf("write command complete: %w", err)
				return
			}
			if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload(0, 0, 0))); err != nil {
				errCh <- fmt.Errorf("write ready: %w", err)
				return
			}
		}
		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	summary, err := conn.ExecuteBatchContext(context.Background(), "UPDATE t SET v = 1", [][]driver.NamedValue{
		{},
		{},
	})
	if err != nil {
		t.Fatalf("execute batch failed: %v", err)
	}
	if summary.TotalRowCount != 7 {
		t.Fatalf("total row count mismatch: got %d want 7", summary.TotalRowCount)
	}
	if len(summary.Items) != 2 {
		t.Fatalf("expected 2 batch items, got %d", len(summary.Items))
	}
	if summary.Items[0].Index != 0 || summary.Items[0].RowCount != 3 || summary.Items[0].Command != "UPDATE 3" || summary.Items[0].LastInsertID != 20 {
		t.Fatalf("unexpected first batch item: %+v", summary.Items[0])
	}
	if summary.Items[1].Index != 1 || summary.Items[1].RowCount != 4 || summary.Items[1].Command != "UPDATE 4" || summary.Items[1].LastInsertID != 21 {
		t.Fatalf("unexpected second batch item: %+v", summary.Items[1])
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestExecuteWithGeneratedKeysContextCollectsAllKeys(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		if err := expectMessage(server, msgQuery); err != nil {
			errCh <- err
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(1, 101, "INSERT 1"))); err != nil {
			errCh <- fmt.Errorf("write first command complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(1, 102, "INSERT 1"))); err != nil {
			errCh <- fmt.Errorf("write second command complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload(0, 0, 0))); err != nil {
			errCh <- fmt.Errorf("write ready: %w", err)
			return
		}
		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	keys, err := conn.ExecuteWithGeneratedKeysContext(context.Background(), "INSERT INTO t(v) VALUES (1); INSERT INTO t(v) VALUES (2)", nil)
	if err != nil {
		t.Fatalf("execute with generated keys failed: %v", err)
	}
	if len(keys) != 2 || keys[0] != 101 || keys[1] != 102 {
		t.Fatalf("unexpected generated keys: %+v", keys)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestCallContextNormalizesCallableEscapeQuery(t *testing.T) {
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
		gotSQL, err := extractQuerySQL(msg.body)
		if err != nil {
			errCh <- err
			return
		}
		if gotSQL != "call maintenance.reindex" {
			errCh <- fmt.Errorf("query mismatch: got %q", gotSQL)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(0, 0, "CALL"))); err != nil {
			errCh <- fmt.Errorf("write command complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload(0, 0, 0))); err != nil {
			errCh <- fmt.Errorf("write ready: %w", err)
			return
		}
		errCh <- nil
	}()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	rows, err := conn.CallContext(context.Background(), "{call maintenance.reindex}", nil)
	if err != nil {
		t.Fatalf("call context failed: %v", err)
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("close rows failed: %v", err)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestNativeCallableSQLNormalizesFunctionEscape(t *testing.T) {
	conn := &Conn{
		config: defaultConfig(),
	}
	sqlText, err := conn.NativeCallableSQL("{? = call math.add(?, ?)}", []driver.NamedValue{
		{Ordinal: 1, Value: int64(5)},
		{Ordinal: 2, Value: int64(7)},
	})
	if err != nil {
		t.Fatalf("native callable sql failed: %v", err)
	}
	if sqlText != "select math.add($1, $2) as return_value" {
		t.Fatalf("unexpected callable SQL: %s", sqlText)
	}
}

func extractQuerySQL(payload []byte) (string, error) {
	if len(payload) < 12 {
		return "", fmt.Errorf("query payload too short: %d", len(payload))
	}
	raw := payload[12:]
	nullPos := -1
	for i, ch := range raw {
		if ch == 0 {
			nullPos = i
			break
		}
	}
	if nullPos < 0 {
		return "", fmt.Errorf("query payload missing null terminator")
	}
	return string(raw[:nullPos]), nil
}

func TestNativeSQLNormalizesPositionalParameters(t *testing.T) {
	conn := &Conn{
		config: defaultConfig(),
	}
	sqlText, err := conn.NativeSQL("SELECT ?::int AS id", []driver.NamedValue{
		{Ordinal: 1, Value: int64(42)},
	})
	if err != nil {
		t.Fatalf("native sql failed: %v", err)
	}
	if sqlText != "SELECT $1::int AS id" {
		t.Fatalf("unexpected SQL: %s", sqlText)
	}
}

func TestExecuteBatchContextRejectsNilBatchArgs(t *testing.T) {
	conn := &Conn{
		config: defaultConfig(),
	}
	_, err := conn.ExecuteBatchContext(context.Background(), "UPDATE t SET v = 1", nil)
	requireDriverError(t, err, ErrSyntax, "07001")
}

func TestExtractQuerySQLTruncatedPayload(t *testing.T) {
	_, err := extractQuerySQL([]byte{1, 2, 3})
	if err == nil {
		t.Fatalf("expected payload parse error")
	}
}

func TestExtractQuerySQLWithoutTerminator(t *testing.T) {
	payload := make([]byte, 12+4)
	binary.LittleEndian.PutUint32(payload[0:4], 0)
	copy(payload[12:], []byte("test"))
	_, err := extractQuerySQL(payload)
	if err == nil {
		t.Fatalf("expected missing terminator error")
	}
}
