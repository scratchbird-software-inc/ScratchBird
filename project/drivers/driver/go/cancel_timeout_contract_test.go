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
	"encoding/binary"
	"errors"
	"net"
	"testing"
	"time"
)

func startReadMessage(conn net.Conn) (<-chan protocolMessage, <-chan error) {
	msgCh := make(chan protocolMessage, 1)
	errCh := make(chan error, 1)
	go func() {
		msg, err := readMessage(conn)
		if err != nil {
			errCh <- err
			return
		}
		msgCh <- msg
	}()
	return msgCh, errCh
}

func readMessageWithTimeout(t *testing.T, msgCh <-chan protocolMessage, errCh <-chan error) protocolMessage {
	t.Helper()
	select {
	case msg := <-msgCh:
		return msg
	case err := <-errCh:
		t.Fatalf("failed to read protocol message: %v", err)
	case <-time.After(2 * time.Second):
		t.Fatalf("timed out waiting for protocol message")
	}
	return protocolMessage{}
}

func TestDrainUntilReadyContextCancelSendsUrgentCancel(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	msgCh, errCh := startReadMessage(server)
	_, _, _, err := conn.drainUntilReady(ctx)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("expected context canceled, got %v", err)
	}

	msg := readMessageWithTimeout(t, msgCh, errCh)
	if msg.header.typ != msgCancel {
		t.Fatalf("expected cancel message type, got %v", msg.header.typ)
	}
	if msg.header.flags&msgFlagUrgent == 0 {
		t.Fatalf("expected urgent cancel flag, got flags=%#x", msg.header.flags)
	}
}

func TestRowsContextCancelSendsUrgentCancel(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	ctx, cancel := context.WithCancel(context.Background())
	msgCh, errCh := startReadMessage(server)
	_ = newRows(conn, ctx)
	cancel()

	msg := readMessageWithTimeout(t, msgCh, errCh)
	if msg.header.typ != msgCancel {
		t.Fatalf("expected cancel message type, got %v", msg.header.typ)
	}
	if msg.header.flags&msgFlagUrgent == 0 {
		t.Fatalf("expected urgent cancel flag, got flags=%#x", msg.header.flags)
	}
}

func TestSendSimpleQueryUsesContextDeadlineForTimeout(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	ctx, cancel := context.WithTimeout(context.Background(), 250*time.Millisecond)
	defer cancel()

	msgCh, errCh := startReadMessage(server)
	if err := conn.sendSimpleQueryWithMaxRows("SELECT 1", ctx, 0); err != nil {
		t.Fatalf("send simple query with deadline: %v", err)
	}

	msg := readMessageWithTimeout(t, msgCh, errCh)
	if msg.header.typ != msgQuery {
		t.Fatalf("expected query message type, got %v", msg.header.typ)
	}
	if len(msg.body) < 12 {
		t.Fatalf("query payload too short: %d", len(msg.body))
	}
	timeoutMs := binary.LittleEndian.Uint32(msg.body[8:12])
	if timeoutMs == 0 {
		t.Fatalf("expected non-zero timeout in query payload")
	}
	if timeoutMs > 5000 {
		t.Fatalf("unexpected timeout value %dms", timeoutMs)
	}
}
