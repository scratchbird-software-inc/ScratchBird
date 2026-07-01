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
	"strings"
	"sync"
	"testing"
	"time"
)

type runtimeGateOptions struct {
	managerProxy       bool
	authMethod         authMethod
	zeroTxnReadyStatus byte
}

type runtimeGateServer struct {
	listener net.Listener
	options  runtimeGateOptions

	mu              sync.Mutex
	activeConn      net.Conn
	startupFeatures uint64
	queryFlags      []uint32
	authResponses   [][]byte
	managerFrames   []uint8

	errCh chan error
	done  chan struct{}
}

func startRuntimeGateServer(t *testing.T, options runtimeGateOptions) *runtimeGateServer {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen failed: %v", err)
	}
	server := &runtimeGateServer{
		listener: listener,
		options:  options,
		errCh:    make(chan error, 1),
		done:     make(chan struct{}),
	}
	if server.options.zeroTxnReadyStatus == 0 {
		server.options.zeroTxnReadyStatus = 0
	}
	go server.run()
	return server
}

func (s *runtimeGateServer) Addr() *net.TCPAddr {
	return s.listener.Addr().(*net.TCPAddr)
}

func (s *runtimeGateServer) Close(t *testing.T) {
	t.Helper()
	_ = s.listener.Close()
	s.mu.Lock()
	active := s.activeConn
	s.mu.Unlock()
	if active != nil {
		_ = active.Close()
	}
	select {
	case err := <-s.errCh:
		if err != nil && !errors.Is(err, net.ErrClosed) {
			t.Fatalf("runtime gate server failed: %v", err)
		}
	case <-s.done:
	case <-time.After(3 * time.Second):
		t.Fatalf("runtime gate server close timeout")
	}
}

func (s *runtimeGateServer) StartupFeatures() uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.startupFeatures
}

func (s *runtimeGateServer) QueryFlags() []uint32 {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]uint32, len(s.queryFlags))
	copy(out, s.queryFlags)
	return out
}

func (s *runtimeGateServer) run() {
	defer close(s.done)
	conn, err := s.listener.Accept()
	if err != nil {
		s.errCh <- err
		return
	}
	s.mu.Lock()
	s.activeConn = conn
	s.mu.Unlock()
	defer conn.Close()
	defer func() {
		s.mu.Lock()
		s.activeConn = nil
		s.mu.Unlock()
	}()

	if s.options.managerProxy {
		if err := s.handleManagerProxy(conn); err != nil {
			s.errCh <- err
			return
		}
	}
	if err := s.handleSBWP(conn); err != nil {
		s.errCh <- err
		return
	}
	s.errCh <- nil
}

func (s *runtimeGateServer) handleManagerProxy(conn net.Conn) error {
	msgType, authStartPayload, err := s.readManagerFrame(conn)
	if err != nil {
		return err
	}
	if msgType != mcpMsgHello {
		return fmt.Errorf("expected MCP hello frame, got %d", msgType)
	}
	s.appendManagerFrame(msgType)
	if err := s.writeManagerFrame(conn, mcpMsgStatusResponse, []byte{0}); err != nil {
		return err
	}

	msgType, authStartPayload, err = s.readManagerFrame(conn)
	if err != nil {
		return err
	}
	if msgType != mcpMsgAuthStart {
		return fmt.Errorf("expected MCP auth start frame, got %d", msgType)
	}
	s.appendManagerFrame(msgType)

	tokenLen := extractManagerAuthStartTokenLen(authStartPayload)
	if tokenLen == 0 {
		if err := s.writeManagerFrame(conn, mcpMsgAuthChallenge, []byte{0}); err != nil {
			return err
		}
		msgType, _, err = s.readManagerFrame(conn)
		if err != nil {
			return err
		}
		if msgType != mcpMsgAuthContinue {
			return fmt.Errorf("expected MCP auth continue frame, got %d", msgType)
		}
		s.appendManagerFrame(msgType)
	}

	authResponse := make([]byte, 1+4+256)
	authResponse[0] = 0
	if err := s.writeManagerFrame(conn, mcpMsgAuthResponse, authResponse); err != nil {
		return err
	}

	msgType, _, err = s.readManagerFrame(conn)
	if err != nil {
		return err
	}
	if msgType != mcpMsgDbConnect {
		return fmt.Errorf("expected MCP db connect frame, got %d", msgType)
	}
	s.appendManagerFrame(msgType)

	connectResponse := make([]byte, 1+2+2+16+64+32)
	connectResponse[0] = 0
	return s.writeManagerFrame(conn, mcpMsgConnectResponse, connectResponse)
}

func (s *runtimeGateServer) handleSBWP(conn net.Conn) error {
	attachment := [16]byte{0x22}
	sequence := uint32(0)
	txnID := uint64(0)

	startup, err := readMessage(conn)
	if err != nil {
		return err
	}
	if startup.header.typ != msgStartup {
		return fmt.Errorf("expected startup message, got %v", startup.header.typ)
	}
	if len(startup.body) < 12 {
		return fmt.Errorf("startup payload too short: %d", len(startup.body))
	}
	s.mu.Lock()
	s.startupFeatures = binary.LittleEndian.Uint64(startup.body[4:12])
	s.mu.Unlock()

	if s.options.authMethod != authOK {
		authRequest := []byte{byte(s.options.authMethod), 0, 0, 0}
		if err := writeSBWP(conn, &sequence, attachment, txnID, msgAuthRequest, authRequest); err != nil {
			return err
		}
		response, err := readMessage(conn)
		if err != nil {
			return err
		}
		if response.header.typ != msgAuthResponse {
			return fmt.Errorf("expected auth response, got %v", response.header.typ)
		}
		s.mu.Lock()
		s.authResponses = append(s.authResponses, append([]byte{}, response.body...))
		s.mu.Unlock()
	}

	authOKPayload := make([]byte, 20)
	if err := writeSBWP(conn, &sequence, attachment, txnID, msgAuthOk, authOKPayload); err != nil {
		return err
	}
	if err := writeSBWP(conn, &sequence, attachment, txnID, msgReady, runtimeReadyPayload(s.readyStatus(txnID), txnID, 0)); err != nil {
		return err
	}

	for {
		msg, err := readMessage(conn)
		if err != nil {
			if errors.Is(err, io.EOF) {
				return nil
			}
			return err
		}
		switch msg.header.typ {
		case msgTxnBegin:
			txnID = 77
			if err := writeSBWP(conn, &sequence, attachment, txnID, msgReady, runtimeReadyPayload(s.readyStatus(txnID), txnID, 0)); err != nil {
				return err
			}
		case msgTxnSavepoint, msgTxnRelease, msgTxnRollbackTo:
			if err := writeSBWP(conn, &sequence, attachment, txnID, msgReady, runtimeReadyPayload(s.readyStatus(txnID), txnID, 0)); err != nil {
				return err
			}
		case msgTxnCommit, msgTxnRollback:
			txnID = 0
			if err := writeSBWP(conn, &sequence, attachment, txnID, msgReady, runtimeReadyPayload(s.readyStatus(txnID), txnID, 0)); err != nil {
				return err
			}
		case msgSetOption:
			if err := writeSBWP(conn, &sequence, attachment, txnID, msgReady, runtimeReadyPayload(s.readyStatus(txnID), txnID, 0)); err != nil {
				return err
			}
		case msgPing:
			if err := writeSBWP(conn, &sequence, attachment, txnID, msgPong, nil); err != nil {
				return err
			}
		case msgQuery:
			flags, sqlText, err := parseQueryMessage(msg.body)
			if err != nil {
				return err
			}
			s.mu.Lock()
			s.queryFlags = append(s.queryFlags, flags)
			s.mu.Unlock()
			if err := s.handleQuery(conn, &sequence, attachment, txnID, sqlText); err != nil {
				return err
			}
			if err := writeSBWP(conn, &sequence, attachment, txnID, msgReady, runtimeReadyPayload(s.readyStatus(txnID), txnID, 0)); err != nil {
				return err
			}
		case msgTerminate:
			return nil
		default:
			return fmt.Errorf("unexpected message type %v", msg.header.typ)
		}
	}
}

func (s *runtimeGateServer) handleQuery(
	conn net.Conn,
	sequence *uint32,
	attachment [16]byte,
	txnID uint64,
	sqlText string,
) error {
	switch {
	case strings.EqualFold(strings.TrimSpace(sqlText), "SELECT 1; SELECT 2"):
		if err := writeResultSet(conn, sequence, attachment, txnID, []runtimeColumn{{name: "first_value", oid: oidInt4, format: formatText}}, [][]any{{"1"}}, "SELECT 1"); err != nil {
			return err
		}
		return writeResultSet(conn, sequence, attachment, txnID, []runtimeColumn{{name: "second_value", oid: oidInt4, format: formatText}}, [][]any{{"2"}}, "SELECT 1")
	case strings.Contains(strings.ToLower(sqlText), "from sys.tables"):
		return writeResultSet(
			conn,
			sequence,
			attachment,
			txnID,
			[]runtimeColumn{
				{name: "table_id", oid: oidInt4, format: formatText},
				{name: "schema_id", oid: oidInt4, format: formatText},
				{name: "table_name", oid: oidText, format: formatText},
				{name: "table_type", oid: oidText, format: formatText},
				{name: "owner_id", oid: oidInt4, format: formatText},
			},
			[][]any{{"1", "7", "events", "TABLE", "11"}},
			"SELECT 1",
		)
	case strings.EqualFold(strings.TrimSpace(sqlText), "SELECT runtime_type_probe"):
		tsValue := encodeTimestamp(time.Date(2026, time.March, 1, 12, 34, 56, 0, time.UTC))
		return writeResultSet(
			conn,
			sequence,
			attachment,
			txnID,
			[]runtimeColumn{
				{name: "tsz", oid: oidTimestamptz, format: formatBinary},
				{name: "amount", oid: oidNumeric, format: formatBinary},
				{name: "raw", oid: oidBytea, format: formatBinary},
			},
			[][]any{{tsValue, encodeLengthPrefixed([]byte("12.34")), encodeLengthPrefixed([]byte("ab"))}},
			"SELECT 1",
		)
	default:
		return writeResultSet(conn, sequence, attachment, txnID, []runtimeColumn{{name: "value", oid: oidInt4, format: formatText}}, [][]any{{"1"}}, "SELECT 1")
	}
}

func (s *runtimeGateServer) readManagerFrame(conn net.Conn) (uint8, []byte, error) {
	header := make([]byte, managerHeaderSize)
	if _, err := io.ReadFull(conn, header); err != nil {
		return 0, nil, err
	}
	if binary.LittleEndian.Uint32(header[0:4]) != managerProtocolMagic {
		return 0, nil, fmt.Errorf("invalid manager magic")
	}
	if binary.LittleEndian.Uint16(header[4:6]) != managerProtocolVersion {
		return 0, nil, fmt.Errorf("invalid manager version")
	}
	msgType := header[6]
	payloadLen := int(binary.LittleEndian.Uint32(header[8:12]))
	payload := make([]byte, payloadLen)
	if payloadLen > 0 {
		if _, err := io.ReadFull(conn, payload); err != nil {
			return 0, nil, err
		}
	}
	return msgType, payload, nil
}

func (s *runtimeGateServer) writeManagerFrame(conn net.Conn, msgType uint8, payload []byte) error {
	header := make([]byte, managerHeaderSize)
	binary.LittleEndian.PutUint32(header[0:4], managerProtocolMagic)
	binary.LittleEndian.PutUint16(header[4:6], managerProtocolVersion)
	header[6] = msgType
	header[7] = 0
	binary.LittleEndian.PutUint32(header[8:12], uint32(len(payload)))
	if _, err := conn.Write(append(header, payload...)); err != nil {
		return err
	}
	return nil
}

func (s *runtimeGateServer) appendManagerFrame(msgType uint8) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.managerFrames = append(s.managerFrames, msgType)
}

type runtimeColumn struct {
	name   string
	oid    uint32
	format uint16
}

func writeResultSet(
	conn net.Conn,
	sequence *uint32,
	attachment [16]byte,
	txnID uint64,
	columns []runtimeColumn,
	rows [][]any,
	tag string,
) error {
	if err := writeSBWP(conn, sequence, attachment, txnID, msgRowDescription, runtimeRowDescriptionPayload(columns)); err != nil {
		return err
	}
	for _, row := range rows {
		if err := writeSBWP(conn, sequence, attachment, txnID, msgDataRow, runtimeDataRowPayload(row)); err != nil {
			return err
		}
	}
	return writeSBWP(conn, sequence, attachment, txnID, msgCommandComplete, testCommandCompletePayload(uint64(len(rows)), 0, tag))
}

func writeSBWP(conn net.Conn, sequence *uint32, attachment [16]byte, txnID uint64, typ messageType, payload []byte) error {
	header := messageHeader{
		typ:          typ,
		sequence:     *sequence,
		attachmentID: attachment,
		txnID:        txnID,
	}
	*sequence = (*sequence + 1) & 0xFFFFFFFF
	_, err := conn.Write(encodeMessage(header, payload))
	return err
}

func parseQueryMessage(payload []byte) (uint32, string, error) {
	if len(payload) < 12 {
		return 0, "", fmt.Errorf("query payload too short: %d", len(payload))
	}
	flags := binary.LittleEndian.Uint32(payload[0:4])
	sqlBytes := payload[12:]
	for idx, b := range sqlBytes {
		if b == 0 {
			return flags, string(sqlBytes[:idx]), nil
		}
	}
	return flags, string(sqlBytes), nil
}

func runtimeRowDescriptionPayload(columns []runtimeColumn) []byte {
	payload := make([]byte, 0, 64)
	header := make([]byte, 4)
	binary.LittleEndian.PutUint16(header[0:2], uint16(len(columns)))
	payload = append(payload, header...)
	for idx, column := range columns {
		nameBytes := []byte(column.name)
		tmp := make([]byte, 4)
		binary.LittleEndian.PutUint32(tmp, uint32(len(nameBytes)))
		payload = append(payload, tmp...)
		payload = append(payload, nameBytes...)
		binary.LittleEndian.PutUint32(tmp, 0)
		payload = append(payload, tmp...)
		ord := make([]byte, 2)
		binary.LittleEndian.PutUint16(ord, uint16(idx+1))
		payload = append(payload, ord...)
		binary.LittleEndian.PutUint32(tmp, column.oid)
		payload = append(payload, tmp...)
		binary.LittleEndian.PutUint16(ord, 0)
		payload = append(payload, ord...)
		binary.LittleEndian.PutUint32(tmp, 0)
		payload = append(payload, tmp...)
		payload = append(payload, byte(column.format))
		payload = append(payload, 1)
		payload = append(payload, 0, 0)
	}
	return payload
}

func runtimeDataRowPayload(values []any) []byte {
	count := len(values)
	nullBytes := (count + 7) / 8
	if nullBytes == 0 {
		nullBytes = 1
	}
	payload := make([]byte, 0, 32)
	header := make([]byte, 4)
	binary.LittleEndian.PutUint16(header[0:2], uint16(count))
	binary.LittleEndian.PutUint16(header[2:4], uint16(nullBytes))
	payload = append(payload, header...)
	nullBitmap := make([]byte, nullBytes)
	for idx, value := range values {
		if value == nil {
			nullBitmap[idx/8] |= 1 << uint(idx%8)
		}
	}
	payload = append(payload, nullBitmap...)
	for _, value := range values {
		if value == nil {
			continue
		}
		var raw []byte
		switch typed := value.(type) {
		case []byte:
			raw = typed
		case string:
			raw = []byte(typed)
		default:
			raw = []byte(fmt.Sprint(value))
		}
		length := make([]byte, 4)
		binary.LittleEndian.PutUint32(length, uint32(len(raw)))
		payload = append(payload, length...)
		payload = append(payload, raw...)
	}
	return payload
}

func runtimeReadyPayload(status byte, txnID, epoch uint64) []byte {
	payload := make([]byte, 20)
	payload[0] = status
	binary.LittleEndian.PutUint64(payload[4:12], txnID)
	binary.LittleEndian.PutUint64(payload[12:20], epoch)
	return payload
}

func (s *runtimeGateServer) readyStatus(txnID uint64) byte {
	if txnID == 0 {
		return s.options.zeroTxnReadyStatus
	}
	return 'T'
}

func extractManagerAuthStartTokenLen(payload []byte) uint32 {
	if len(payload) < 4 {
		return 0
	}
	userLen := int(binary.LittleEndian.Uint32(payload[0:4]))
	offset := 4 + userLen + 1
	if offset+4 > len(payload) {
		return 0
	}
	return binary.LittleEndian.Uint32(payload[offset : offset+4])
}

func TestRuntimeGateManagerProxyTxnExecWithoutEnv(t *testing.T) {
	server := startRuntimeGateServer(t, runtimeGateOptions{managerProxy: true, authMethod: authToken})
	defer server.Close(t)

	cfg := defaultConfig()
	cfg.Host = server.Addr().IP.String()
	cfg.Port = server.Addr().Port
	cfg.User = "alice"
	cfg.Password = "secret"
	cfg.Database = "runtime_db"
	cfg.Protocol = "jdbc"
	cfg.SSLMode = "disable"
	cfg.BinaryTransfer = false
	cfg.Compression = "zstd"
	cfg.FrontDoorMode = "manager_proxy"
	cfg.ManagerAuthToken = "token"
	cfg.ManagerAuthFastPath = false
	cfg.AuthPayloadJSON = `{"token":"abc"}`

	conn := &Conn{config: cfg}
	ctx := context.Background()
	txDriver, err := conn.BeginTx(ctx, driver.TxOptions{})
	if err != nil {
		t.Fatalf("begin tx failed: %v", err)
	}
	tx := txDriver.(*Tx)
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

	sets, err := conn.QueryMultiContext(ctx, "SELECT 1; SELECT 2", nil)
	if err != nil {
		t.Fatalf("query multi failed: %v", err)
	}
	if len(sets) != 2 {
		t.Fatalf("expected 2 result sets, got %d", len(sets))
	}
	if len(sets[0].Rows) != 1 || len(sets[0].Rows[0]) != 1 || len(sets[1].Rows) != 1 || len(sets[1].Rows[0]) != 1 {
		t.Fatalf("expected one row and one column in each result set, got %#v", sets)
	}
	if sets[0].Rows[0][0] != driver.Value("1") || sets[1].Rows[0][0] != driver.Value("2") {
		t.Fatalf("unexpected multi-result payload: %#v", sets)
	}
	if err := conn.Close(); err != nil {
		t.Fatalf("close failed: %v", err)
	}

	features := server.StartupFeatures()
	if features&featureCompression != 0 {
		t.Fatalf("expected compression feature bit cleared until server admission")
	}
	if features&featureStreaming != 0 {
		t.Fatalf("expected streaming feature bit cleared when binary transfer is disabled")
	}
	for _, flags := range server.QueryFlags() {
		if flags&queryFlagBinaryResult != 0 {
			t.Fatalf("expected binary query flag to be disabled, saw %#x", flags)
		}
	}
}

func TestRuntimeGateMetadataWithoutEnv(t *testing.T) {
	server := startRuntimeGateServer(t, runtimeGateOptions{managerProxy: false, authMethod: authOK})
	defer server.Close(t)

	cfg := defaultConfig()
	cfg.Host = server.Addr().IP.String()
	cfg.Port = server.Addr().Port
	cfg.User = "alice"
	cfg.Password = "secret"
	cfg.Database = "runtime_db"
	cfg.Protocol = "postgresql"
	cfg.SSLMode = "disable"
	cfg.BinaryTransfer = false
	cfg.Compression = "zstd"

	conn := &Conn{config: cfg}
	rowsDriver, err := conn.QueryMetadataWithRestrictions(context.Background(), "tables", map[string]any{"table": "events"})
	if err != nil {
		t.Fatalf("metadata query failed: %v", err)
	}
	rows, ok := rowsDriver.(*metadataRows)
	if !ok {
		t.Fatalf("expected metadata rows, got %T", rowsDriver)
	}
	dest := make([]driver.Value, 5)
	if err := rows.Next(dest); err != nil {
		t.Fatalf("metadata next failed: %v", err)
	}
	if got := dest[2]; got != driver.Value("events") {
		t.Fatalf("unexpected metadata row: %#v", dest)
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("metadata close failed: %v", err)
	}
	if err := conn.Close(); err != nil {
		t.Fatalf("close failed: %v", err)
	}
}

func TestRuntimeGateTypeDecodeWithoutEnv(t *testing.T) {
	server := startRuntimeGateServer(t, runtimeGateOptions{managerProxy: false, authMethod: authOK})
	defer server.Close(t)

	cfg := defaultConfig()
	cfg.Host = server.Addr().IP.String()
	cfg.Port = server.Addr().Port
	cfg.User = "alice"
	cfg.Password = "secret"
	cfg.Database = "runtime_db"
	cfg.SSLMode = "disable"
	cfg.BinaryTransfer = true
	cfg.Compression = "off"

	conn := &Conn{config: cfg}
	rowsIface, err := conn.QueryContext(context.Background(), "SELECT runtime_type_probe", nil)
	if err != nil {
		t.Fatalf("query failed: %v", err)
	}
	rows := rowsIface.(*Rows)
	dest := make([]driver.Value, 3)
	if err := rows.Next(dest); err != nil {
		t.Fatalf("next failed: %v", err)
	}
	tsz, ok := dest[0].(time.Time)
	if !ok {
		t.Fatalf("expected timestamptz value, got %T", dest[0])
	}
	if tsz.IsZero() {
		t.Fatalf("expected non-zero timestamptz value")
	}
	if got := dest[1]; got != driver.Value("12.34") {
		t.Fatalf("unexpected numeric decode: %#v", got)
	}
	if got, ok := dest[2].([]byte); !ok || string(got) != "ab" {
		t.Fatalf("unexpected bytea decode: %#v", dest[2])
	}
	if err := rows.Close(); err != nil {
		t.Fatalf("rows close failed: %v", err)
	}
	if err := conn.Close(); err != nil {
		t.Fatalf("close failed: %v", err)
	}
}
