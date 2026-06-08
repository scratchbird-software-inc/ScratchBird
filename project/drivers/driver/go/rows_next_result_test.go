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
	"errors"
	"fmt"
	"io"
	"net"
	"testing"
)

func TestRowsNextResultSetSeparatesSimpleQueryResults(t *testing.T) {
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
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgRowDescription}, testRowDescriptionPayload("first_value"))); err != nil {
			errCh <- fmt.Errorf("write first row description: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgDataRow}, testDataRowPayload("1"))); err != nil {
			errCh <- fmt.Errorf("write first data row: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(1, 0, "SELECT 1"))); err != nil {
			errCh <- fmt.Errorf("write first command complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgRowDescription}, testRowDescriptionPayload("second_value"))); err != nil {
			errCh <- fmt.Errorf("write second row description: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgDataRow}, testDataRowPayload("2"))); err != nil {
			errCh <- fmt.Errorf("write second data row: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(1, 0, "SELECT 1"))); err != nil {
			errCh <- fmt.Errorf("write second command complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 42, 0))); err != nil {
			errCh <- fmt.Errorf("write ready: %w", err)
			return
		}

		errCh <- nil
	}()

	conn := &Conn{config: defaultConfig(), raw: client}
	rowsDriver, err := conn.QueryContext(context.Background(), "SELECT 1; SELECT 2", nil)
	if err != nil {
		t.Fatalf("query context failed: %v", err)
	}
	rows, ok := rowsDriver.(*Rows)
	if !ok {
		t.Fatalf("expected *Rows, got %T", rowsDriver)
	}

	dest := make([]driver.Value, 1)
	if err := rows.Next(dest); err != nil {
		t.Fatalf("first Next failed: %v", err)
	}
	if got := dest[0]; got != "1" {
		t.Fatalf("unexpected first row value: got %#v want %q", got, "1")
	}
	if err := rows.Next(dest); !errors.Is(err, io.EOF) {
		t.Fatalf("expected EOF at first result boundary, got %v", err)
	}
	if !rows.HasNextResultSet() {
		t.Fatalf("expected next result set after first command complete")
	}
	if err := rows.NextResultSet(); err != nil {
		t.Fatalf("advance to next result set failed: %v", err)
	}

	if err := rows.Next(dest); err != nil {
		t.Fatalf("second Next failed: %v", err)
	}
	if got := dest[0]; got != "2" {
		t.Fatalf("unexpected second row value: got %#v want %q", got, "2")
	}
	if err := rows.Next(dest); !errors.Is(err, io.EOF) {
		t.Fatalf("expected EOF after final result set, got %v", err)
	}
	if rows.HasNextResultSet() {
		t.Fatalf("did not expect next result set after ready")
	}
	if err := rows.NextResultSet(); !errors.Is(err, io.EOF) {
		t.Fatalf("expected EOF from NextResultSet after final ready, got %v", err)
	}
	if conn.txnID != 42 {
		t.Fatalf("expected txn id 42, got %d", conn.txnID)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestRowsCloseDrainsAcrossResultSetBoundaries(t *testing.T) {
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
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgRowDescription}, testRowDescriptionPayload("value"))); err != nil {
			errCh <- fmt.Errorf("write first row description: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgDataRow}, testDataRowPayload("1"))); err != nil {
			errCh <- fmt.Errorf("write first row: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(1, 0, "SELECT 1"))); err != nil {
			errCh <- fmt.Errorf("write first complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgRowDescription}, testRowDescriptionPayload("value"))); err != nil {
			errCh <- fmt.Errorf("write second row description: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgDataRow}, testDataRowPayload("2"))); err != nil {
			errCh <- fmt.Errorf("write second row: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgCommandComplete}, testCommandCompletePayload(1, 0, "SELECT 1"))); err != nil {
			errCh <- fmt.Errorf("write second complete: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady}, testReadyPayload('T', 13, 0))); err != nil {
			errCh <- fmt.Errorf("write ready: %w", err)
			return
		}

		errCh <- nil
	}()

	conn := &Conn{config: defaultConfig(), raw: client}
	rowsDriver, err := conn.QueryContext(context.Background(), "SELECT 1; SELECT 2", nil)
	if err != nil {
		t.Fatalf("query context failed: %v", err)
	}
	rows, ok := rowsDriver.(*Rows)
	if !ok {
		t.Fatalf("expected *Rows, got %T", rowsDriver)
	}

	dest := make([]driver.Value, 1)
	if err := rows.Next(dest); err != nil {
		t.Fatalf("first Next failed: %v", err)
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("rows close failed: %v", err)
	}
	if !rows.done {
		t.Fatalf("expected rows to be done after close")
	}
	if conn.txnID != 13 {
		t.Fatalf("expected txn id 13 after close drain, got %d", conn.txnID)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func testRowDescriptionPayload(columnName string) []byte {
	nameBytes := []byte(columnName)
	payload := make([]byte, 4+4+len(nameBytes)+4+2+4+2+4+1+1+2)
	binary.LittleEndian.PutUint16(payload[0:2], 1)
	offset := 4
	binary.LittleEndian.PutUint32(payload[offset:offset+4], uint32(len(nameBytes)))
	offset += 4
	copy(payload[offset:offset+len(nameBytes)], nameBytes)
	offset += len(nameBytes)
	binary.LittleEndian.PutUint32(payload[offset:offset+4], 0)
	offset += 4
	binary.LittleEndian.PutUint16(payload[offset:offset+2], 1)
	offset += 2
	binary.LittleEndian.PutUint32(payload[offset:offset+4], oidText)
	offset += 4
	binary.LittleEndian.PutUint16(payload[offset:offset+2], 0)
	offset += 2
	binary.LittleEndian.PutUint32(payload[offset:offset+4], 0)
	offset += 4
	payload[offset] = uint8(formatText)
	offset++
	payload[offset] = 1
	return payload
}

func testDataRowPayload(value string) []byte {
	valueBytes := []byte(value)
	payload := make([]byte, 2+2+1+4+len(valueBytes))
	binary.LittleEndian.PutUint16(payload[0:2], 1)
	binary.LittleEndian.PutUint16(payload[2:4], 1)
	payload[4] = 0
	binary.LittleEndian.PutUint32(payload[5:9], uint32(len(valueBytes)))
	copy(payload[9:], valueBytes)
	return payload
}
