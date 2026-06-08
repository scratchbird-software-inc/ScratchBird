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
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func requireDriverError(t *testing.T, err error, kind ErrorKind, sqlState string) {
	t.Helper()
	if err == nil {
		t.Fatalf("expected error")
	}
	var sbErr *Error
	if !errors.As(err, &sbErr) {
		t.Fatalf("expected *Error, got %T (%v)", err, err)
	}
	if sbErr.Kind != kind {
		t.Fatalf("expected error kind %q, got %q", kind, sbErr.Kind)
	}
	if sbErr.SQLState != sqlState {
		t.Fatalf("expected SQLSTATE %q, got %q", sqlState, sbErr.SQLState)
	}
}

func TestApplyTLSDisableModeTrimsWhitespace(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()
	defer server.Close()

	conn := &Conn{
		config: defaultConfig(),
		raw:    client,
	}
	conn.config.SSLMode = "  DISABLE  "

	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()

	err := conn.applyTLS(ctx)
	if err != nil {
		t.Fatalf("expected disable mode to bypass TLS, got %v", err)
	}
}

func TestBuildTLSConfigRejectsInvalidRootCertPEM(t *testing.T) {
	caPath := filepath.Join(t.TempDir(), "invalid-ca.pem")
	if err := os.WriteFile(caPath, []byte("not-a-pem"), 0o600); err != nil {
		t.Fatalf("write temp CA file: %v", err)
	}

	conn := &Conn{config: defaultConfig()}
	conn.config.SSLRootCert = caPath

	_, err := conn.buildTLSConfig("verify-full")
	if err == nil {
		t.Fatalf("expected invalid PEM error")
	}
	if !strings.Contains(err.Error(), "failed to parse sslrootcert PEM") {
		t.Fatalf("expected parse PEM error, got %v", err)
	}
}

func TestConnectNormalizesProtocolAliasBeforeDial(t *testing.T) {
	conn := &Conn{config: defaultConfig()}
	conn.config.Protocol = "postgresql"
	conn.config.Host = "127.0.0.1"
	conn.config.Port = 1
	conn.config.SSLMode = "disable"
	conn.config.User = "alice"
	conn.config.Database = "db1"

	err := conn.connect(context.Background())
	requireDriverError(t, err, ErrConnection, "08001")
	if conn.config.Protocol != "native" {
		t.Fatalf("expected protocol alias normalization to native, got %q", conn.config.Protocol)
	}
}

func TestConnectAllowsBinaryTransferFalseAndZstd(t *testing.T) {
	conn := &Conn{config: defaultConfig()}
	conn.config.Host = "127.0.0.1"
	conn.config.Port = 1
	conn.config.BinaryTransfer = false
	conn.config.Compression = "zstd"
	conn.config.SSLMode = "disable"
	conn.config.User = "alice"
	conn.config.Database = "db1"

	err := conn.connect(context.Background())
	requireDriverError(t, err, ErrConnection, "08001")
}

func TestConnectRejectsManagerProxyWithoutTokenBeforeDial(t *testing.T) {
	conn := &Conn{config: defaultConfig()}
	conn.config.FrontDoorMode = "manager_proxy"
	conn.config.User = "alice"
	conn.config.Database = "db1"

	err := conn.connect(context.Background())
	requireDriverError(t, err, ErrConnection, "08001")
	if conn.raw != nil {
		t.Fatalf("expected no network dial for missing manager token")
	}
}

func TestConnectRejectsMissingIdentityBeforeDial(t *testing.T) {
	conn := &Conn{config: defaultConfig()}
	conn.config.Host = "127.0.0.1"
	conn.config.Port = 1
	conn.config.SSLMode = "disable"

	err := conn.connect(context.Background())
	requireDriverError(t, err, ErrConnection, "08001")
	if conn.raw != nil {
		t.Fatalf("expected no network dial for missing identity")
	}
}

func TestDecodeHeaderRejectsPayloadTooLarge(t *testing.T) {
	header := make([]byte, headerSize)
	copy(header[0:4], []byte("SBWP"))
	header[4] = protocolMajor
	header[5] = protocolMinor
	header[6] = byte(msgQuery)
	binary.LittleEndian.PutUint32(header[8:12], uint32(maxMessageSize+1))

	if _, err := decodeHeader(header); err == nil {
		t.Fatalf("expected payload-too-large error")
	}
}

func TestParseAuthContinueRejectsTruncatedPayload(t *testing.T) {
	payload := []byte{
		byte(authScramSha256), 1, 0, 0,
		5, 0, 0, 0, // data length claims 5 bytes
		'a', 'b', // only 2 bytes available
	}
	if _, _, _, err := parseAuthContinue(payload); err == nil {
		t.Fatalf("expected auth continue truncation error")
	}
}

func TestApplyAuthPluginSelectionRejectsInvalidNamespace(t *testing.T) {
	err := ApplyAuthPluginSelection(map[string]string{}, AuthPluginSelection{
		MethodID: "custom.auth.password",
	})
	if err == nil {
		t.Fatalf("expected auth namespace validation error")
	}
}

func TestApplyAuthPluginSelectionSetsParams(t *testing.T) {
	params := map[string]string{}
	err := ApplyAuthPluginSelection(params, AuthPluginSelection{
		MethodID:                "scratchbird.auth.password",
		MethodPayload:           "opaque",
		PayloadJSON:             `{"otp":"123456"}`,
		PayloadB64:              "YWJj",
		ProviderProfile:         "SBsql",
		RequiredMethods:         "SCRAM_SHA_256,TOKEN",
		ForbiddenMethods:        "MD5",
		RequireChannelBinding:   true,
		WorkloadIdentityToken:   "jwt-token",
		ProxyPrincipalAssertion: "signed-assertion",
	})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if params[authParamMethodID] != "scratchbird.auth.password" {
		t.Fatalf("unexpected method id: %q", params[authParamMethodID])
	}
	if params[authParamMethodPayload] != "opaque" {
		t.Fatalf("unexpected method payload: %q", params[authParamMethodPayload])
	}
	if params[authParamPayloadJSON] != `{"otp":"123456"}` {
		t.Fatalf("unexpected payload json: %q", params[authParamPayloadJSON])
	}
	if params[authParamPayloadB64] != "YWJj" {
		t.Fatalf("unexpected payload b64: %q", params[authParamPayloadB64])
	}
	if params[authParamProviderProfile] != "SBsql" {
		t.Fatalf("unexpected provider profile: %q", params[authParamProviderProfile])
	}
	if params[authParamRequiredMethods] != "SCRAM_SHA_256,TOKEN" {
		t.Fatalf("unexpected required methods: %q", params[authParamRequiredMethods])
	}
	if params[authParamForbiddenMethods] != "MD5" {
		t.Fatalf("unexpected forbidden methods: %q", params[authParamForbiddenMethods])
	}
	if params[authParamRequireChannelBinding] != "1" {
		t.Fatalf("unexpected require channel binding flag: %q", params[authParamRequireChannelBinding])
	}
	if params[authParamWorkloadIdentityToken] != "jwt-token" {
		t.Fatalf("unexpected workload identity token: %q", params[authParamWorkloadIdentityToken])
	}
	if params[authParamProxyPrincipalAssertion] != "signed-assertion" {
		t.Fatalf("unexpected proxy principal assertion: %q", params[authParamProxyPrincipalAssertion])
	}
}

func TestHandshakeIncludesAuthPluginSelectionParams(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read startup message: %w", err)
			return
		}
		if msg.header.typ != msgStartup {
			errCh <- fmt.Errorf("expected startup message, got %v", msg.header.typ)
			return
		}
		if len(msg.body) < 12 {
			errCh <- fmt.Errorf("startup payload too short: %d", len(msg.body))
			return
		}
		params := parseStartupParams(msg.body[12:])
		if params["client_flags"] != "257" {
			errCh <- fmt.Errorf("unexpected client flags: %q", params["client_flags"])
			return
		}
		if params[authParamMethodID] != "scratchbird.auth.oidc" {
			errCh <- fmt.Errorf("unexpected auth method id: %q", params[authParamMethodID])
			return
		}
		if params[authParamMethodPayload] != "opaque" {
			errCh <- fmt.Errorf("unexpected auth method payload: %q", params[authParamMethodPayload])
			return
		}
		if params[authParamPayloadJSON] != `{"aud":"sb"}` {
			errCh <- fmt.Errorf("unexpected auth payload json: %q", params[authParamPayloadJSON])
			return
		}
		if params[authParamPayloadB64] != "YWJj" {
			errCh <- fmt.Errorf("unexpected auth payload b64: %q", params[authParamPayloadB64])
			return
		}
		if params[authParamProviderProfile] != "corp" {
			errCh <- fmt.Errorf("unexpected auth provider profile: %q", params[authParamProviderProfile])
			return
		}
		if params[authParamRequiredMethods] != "SCRAM_SHA_256,TOKEN" {
			errCh <- fmt.Errorf("unexpected auth required methods: %q", params[authParamRequiredMethods])
			return
		}
		if params[authParamForbiddenMethods] != "MD5" {
			errCh <- fmt.Errorf("unexpected auth forbidden methods: %q", params[authParamForbiddenMethods])
			return
		}
		if params[authParamRequireChannelBinding] != "1" {
			errCh <- fmt.Errorf("unexpected auth require channel binding: %q", params[authParamRequireChannelBinding])
			return
		}
		if params[authParamWorkloadIdentityToken] != "jwt-token" {
			errCh <- fmt.Errorf("unexpected workload identity token: %q", params[authParamWorkloadIdentityToken])
			return
		}
		if params[authParamProxyPrincipalAssertion] != "signed-assertion" {
			errCh <- fmt.Errorf("unexpected proxy principal assertion: %q", params[authParamProxyPrincipalAssertion])
			return
		}

		authPayload := make([]byte, 20)
		attachment := [16]byte{0x11}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgAuthOk, attachmentID: attachment}, authPayload)); err != nil {
			errCh <- fmt.Errorf("write auth ok: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady, attachmentID: attachment}, testReadyPayload(0, 0, 0))); err != nil {
			errCh <- fmt.Errorf("write ready: %w", err)
			return
		}
		errCh <- nil
	}()

	cfg := defaultConfig()
	cfg.User = "alice"
	cfg.Password = "secret"
	cfg.Database = "db1"
	cfg.ConnectClientFlags = 257
	cfg.AuthMethodID = "scratchbird.auth.oidc"
	cfg.AuthMethodPayload = "opaque"
	cfg.AuthPayloadJSON = `{"aud":"sb"}`
	cfg.AuthPayloadB64 = "YWJj"
	cfg.AuthProviderProfile = "corp"
	cfg.AuthRequiredMethods = "SCRAM_SHA_256,TOKEN"
	cfg.AuthForbiddenMethods = "MD5"
	cfg.AuthRequireChannelBinding = true
	cfg.WorkloadIdentityToken = "jwt-token"
	cfg.ProxyPrincipalAssertion = "signed-assertion"
	conn := &Conn{config: cfg, raw: client}
	if err := conn.handshake(context.Background()); err != nil {
		t.Fatalf("handshake failed: %v", err)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestProbeAuthSurfaceDirectReportsScramSha512(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer ln.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		server, err := ln.Accept()
		if err != nil {
			errCh <- err
			return
		}
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read startup: %w", err)
			return
		}
		if msg.header.typ != msgStartup {
			errCh <- fmt.Errorf("expected startup, got %v", msg.header.typ)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgAuthRequest}, makeAuthRequestPayload(authScramSha512, nil))); err != nil {
			errCh <- fmt.Errorf("write auth request: %w", err)
			return
		}
		errCh <- nil
	}()

	result, err := ProbeAuthSurface(context.Background(), fmt.Sprintf("scratchbird://alice@%s/db1?sslmode=disable", ln.Addr().String()))
	if err != nil {
		t.Fatalf("probe auth surface: %v", err)
	}
	if !result.Reachable {
		t.Fatalf("expected reachable result")
	}
	if result.IngressMode != "direct" {
		t.Fatalf("expected direct ingress, got %q", result.IngressMode)
	}
	if result.RequiredMethod != "SCRAM_SHA_512" {
		t.Fatalf("expected SCRAM_SHA_512, got %q", result.RequiredMethod)
	}
	if result.RequiredPluginMethodID != "scratchbird.auth.scram_sha_512" {
		t.Fatalf("unexpected plugin method id: %q", result.RequiredPluginMethodID)
	}
	if !result.AdditionalContinuationPossible {
		t.Fatalf("expected additional continuation to be possible")
	}
	if len(result.AdmittedMethods) != 1 || !result.AdmittedMethods[0].ExecutableLocally {
		t.Fatalf("expected one locally executable admitted method, got %#v", result.AdmittedMethods)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestProbeAuthSurfaceManagerProxyReportsToken(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer ln.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		server, err := ln.Accept()
		if err != nil {
			errCh <- err
			return
		}
		defer server.Close()

		msgType, _, err := readManagerFrame(server)
		if err != nil {
			errCh <- err
			return
		}
		if msgType != mcpMsgHello {
			errCh <- fmt.Errorf("expected MCP hello, got %d", msgType)
			return
		}
		if err := writeManagerFrame(server, mcpMsgStatusResponse, nil); err != nil {
			errCh <- err
			return
		}
		msgType, _, err = readManagerFrame(server)
		if err != nil {
			errCh <- err
			return
		}
		if msgType != mcpMsgAuthStart {
			errCh <- fmt.Errorf("expected MCP auth start, got %d", msgType)
			return
		}
		if err := writeManagerFrame(server, mcpMsgAuthChallenge, nil); err != nil {
			errCh <- err
			return
		}
		errCh <- nil
	}()

	result, err := ProbeAuthSurface(
		context.Background(),
		fmt.Sprintf("scratchbird://%s/db1?sslmode=disable&front_door_mode=manager_proxy", ln.Addr().String()),
	)
	if err != nil {
		t.Fatalf("probe manager auth surface: %v", err)
	}
	if !result.Reachable {
		t.Fatalf("expected reachable result")
	}
	if result.IngressMode != "manager_proxy" {
		t.Fatalf("expected manager_proxy ingress, got %q", result.IngressMode)
	}
	if result.RequiredMethod != "TOKEN" {
		t.Fatalf("expected TOKEN, got %q", result.RequiredMethod)
	}
	if result.RequiredPluginMethodID != "scratchbird.auth.authkey_token" {
		t.Fatalf("unexpected plugin method id: %q", result.RequiredPluginMethodID)
	}
	if !result.AdditionalContinuationPossible {
		t.Fatalf("expected continuation to be possible")
	}
	if len(result.AdmittedMethods) != 1 || result.AdmittedMethods[0].WireMethod != "TOKEN" {
		t.Fatalf("unexpected admitted methods: %#v", result.AdmittedMethods)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestHandshakeSupportsScramSha512AndResolvedContext(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()

		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read startup message: %w", err)
			return
		}
		if msg.header.typ != msgStartup {
			errCh <- fmt.Errorf("expected startup, got %v", msg.header.typ)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgAuthRequest}, makeAuthRequestPayload(authScramSha512, nil))); err != nil {
			errCh <- fmt.Errorf("write auth request: %w", err)
			return
		}

		clientFirst, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read client first: %w", err)
			return
		}
		if clientFirst.header.typ != msgAuthResponse {
			errCh <- fmt.Errorf("expected auth response, got %v", clientFirst.header.typ)
			return
		}
		clientFirstText := string(clientFirst.body)
		idx := strings.LastIndex(clientFirstText, "r=")
		if idx < 0 {
			errCh <- fmt.Errorf("client first message missing nonce: %q", clientFirstText)
			return
		}
		clientNonce := clientFirstText[idx+2:]
		serverFirst := fmt.Sprintf("r=%sserver,s=c2FsdA==,i=4096", clientNonce)
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgAuthContinue}, makeAuthContinuePayload(authScramSha512, 1, []byte(serverFirst)))); err != nil {
			errCh <- fmt.Errorf("write auth continue: %w", err)
			return
		}

		clientFinal, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read client final: %w", err)
			return
		}
		if clientFinal.header.typ != msgAuthResponse {
			errCh <- fmt.Errorf("expected second auth response, got %v", clientFinal.header.typ)
			return
		}
		if !strings.Contains(string(clientFinal.body), "p=") {
			errCh <- fmt.Errorf("expected SCRAM proof in client final, got %q", string(clientFinal.body))
			return
		}

		attachment := [16]byte{0x22}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgAuthOk, attachmentID: attachment}, makeAuthOKPayload(nil))); err != nil {
			errCh <- fmt.Errorf("write auth ok: %w", err)
			return
		}
		if _, err := server.Write(encodeMessage(messageHeader{typ: msgReady, attachmentID: attachment}, testReadyPayload(0, 0, 0))); err != nil {
			errCh <- fmt.Errorf("write ready: %w", err)
			return
		}
		errCh <- nil
	}()

	cfg := defaultConfig()
	cfg.User = "alice"
	cfg.Password = "secret"
	cfg.Database = "db1"
	conn := &Conn{config: cfg, raw: client}
	if err := conn.handshake(context.Background()); err != nil {
		t.Fatalf("handshake failed: %v", err)
	}
	context := conn.GetResolvedAuthContext()
	if context.ResolvedAuthMethod != "SCRAM_SHA_512" {
		t.Fatalf("expected resolved auth method SCRAM_SHA_512, got %q", context.ResolvedAuthMethod)
	}
	if context.ResolvedAuthPluginID != "scratchbird.auth.scram_sha_512" {
		t.Fatalf("unexpected resolved auth plugin id: %q", context.ResolvedAuthPluginID)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestHandleAuthRequestSupportsTokenAuth(t *testing.T) {
	client, server := net.Pipe()
	defer client.Close()

	errCh := make(chan error, 1)
	go func() {
		defer close(errCh)
		defer server.Close()
		msg, err := readMessage(server)
		if err != nil {
			errCh <- fmt.Errorf("read auth response: %w", err)
			return
		}
		if msg.header.typ != msgAuthResponse {
			errCh <- fmt.Errorf("expected auth response, got %v", msg.header.typ)
			return
		}
		if got := string(msg.body); got != "bearer-token" {
			errCh <- fmt.Errorf("auth token mismatch: got %q", got)
			return
		}
		errCh <- nil
	}()

	cfg := defaultConfig()
	cfg.AuthToken = "bearer-token"
	conn := &Conn{config: cfg, raw: client}
	if _, err := conn.handleAuthRequest(authToken, nil, nil); err != nil {
		t.Fatalf("handle auth request failed: %v", err)
	}
	context := conn.GetResolvedAuthContext()
	if context.ResolvedAuthMethod != "TOKEN" {
		t.Fatalf("expected TOKEN, got %q", context.ResolvedAuthMethod)
	}
	if context.ResolvedAuthPluginID != "scratchbird.auth.authkey_token" {
		t.Fatalf("unexpected token plugin id: %q", context.ResolvedAuthPluginID)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestHandleAuthRequestFailsClosedForPeer(t *testing.T) {
	cfg := defaultConfig()
	conn := &Conn{config: cfg}
	_, err := conn.handleAuthRequest(authPeer, nil, nil)
	requireDriverError(t, err, ErrNotSupported, "0A000")

	context := conn.GetResolvedAuthContext()
	if context.ResolvedAuthMethod != "PEER" {
		t.Fatalf("expected PEER, got %q", context.ResolvedAuthMethod)
	}
	if context.ResolvedAuthPluginID != "scratchbird.auth.peer_uid" {
		t.Fatalf("unexpected peer plugin id: %q", context.ResolvedAuthPluginID)
	}
}

func parseStartupParams(payload []byte) map[string]string {
	params := map[string]string{}
	parts := strings.Split(string(payload), "\x00")
	for i := 0; i+1 < len(parts); i += 2 {
		key := parts[i]
		value := parts[i+1]
		if key == "" {
			break
		}
		params[key] = value
	}
	return params
}

func makeAuthRequestPayload(method authMethod, data []byte) []byte {
	payload := make([]byte, 4+len(data))
	payload[0] = byte(method)
	copy(payload[4:], data)
	return payload
}

func makeAuthContinuePayload(method authMethod, stage byte, data []byte) []byte {
	payload := make([]byte, 8+len(data))
	payload[0] = byte(method)
	payload[1] = stage
	binary.LittleEndian.PutUint32(payload[4:8], uint32(len(data)))
	copy(payload[8:], data)
	return payload
}

func makeAuthOKPayload(info []byte) []byte {
	payload := make([]byte, 20+len(info))
	binary.LittleEndian.PutUint32(payload[16:20], uint32(len(info)))
	copy(payload[20:], info)
	return payload
}

func readManagerFrame(r net.Conn) (uint8, []byte, error) {
	header := make([]byte, managerHeaderSize)
	if _, err := io.ReadFull(r, header); err != nil {
		return 0, nil, err
	}
	if readU32(header[:4]) != managerProtocolMagic {
		return 0, nil, fmt.Errorf("invalid manager magic")
	}
	length := readU32(header[8:12])
	payload := make([]byte, length)
	if length > 0 {
		if _, err := io.ReadFull(r, payload); err != nil {
			return 0, nil, err
		}
	}
	return header[6], payload, nil
}

func writeManagerFrame(w net.Conn, msgType uint8, payload []byte) error {
	frame := make([]byte, 0, managerHeaderSize+len(payload))
	frame = appendU32(frame, managerProtocolMagic)
	frame = appendU16(frame, managerProtocolVersion)
	frame = append(frame, msgType, 0)
	frame = appendU32(frame, uint32(len(payload)))
	frame = append(frame, payload...)
	_, err := w.Write(frame)
	return err
}
