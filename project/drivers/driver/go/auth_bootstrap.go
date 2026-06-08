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
	"encoding/base64"
	"net"
	"strconv"
	"strings"
)

type AuthMethodSurface struct {
	WireMethod        string
	PluginMethodID    string
	ExecutableLocally bool
	BrokerRequired    bool
}

type AuthProbeResult struct {
	Reachable                      bool
	IngressMode                    string
	ResolvedHost                   string
	ResolvedPort                   int
	AdmittedMethods                []AuthMethodSurface
	RequiredMethod                 string
	RequiredPluginMethodID         string
	AllowedTransportMask           *uint32
	AdditionalContinuationPossible bool
}

type ResolvedAuthContext struct {
	IngressMode          string
	ResolvedAuthMethod   string
	ResolvedAuthPluginID string
	ManagerAuthenticated bool
	Attached             bool
}

var defaultAuthPluginIDs = map[authMethod]string{
	authPassword:    "scratchbird.auth.password_compat",
	authMD5:         "scratchbird.auth.md5_legacy",
	authScramSha256: "scratchbird.auth.scram_sha_256",
	authScramSha512: "scratchbird.auth.scram_sha_512",
	authToken:       "scratchbird.auth.authkey_token",
	authPeer:        "scratchbird.auth.peer_uid",
	authReattach:    "scratchbird.auth.reattach",
}

func authMethodName(method authMethod) string {
	switch method {
	case authPassword:
		return "PASSWORD"
	case authMD5:
		return "MD5"
	case authScramSha256:
		return "SCRAM_SHA_256"
	case authScramSha512:
		return "SCRAM_SHA_512"
	case authToken:
		return "TOKEN"
	case authPeer:
		return "PEER"
	case authReattach:
		return "REATTACH"
	default:
		return ""
	}
}

func authPluginIDForMethod(method authMethod, configuredMethodID string) string {
	if strings.TrimSpace(configuredMethodID) != "" {
		return strings.TrimSpace(configuredMethodID)
	}
	return defaultAuthPluginIDs[method]
}

func authMethodExecutableLocally(method authMethod) bool {
	switch method {
	case authPassword, authScramSha256, authScramSha512, authToken:
		return true
	default:
		return false
	}
}

func authMethodBrokerRequired(method authMethod) bool {
	return method == authPeer
}

func describeAuthMethod(method authMethod, configuredMethodID string) (AuthMethodSurface, bool) {
	wireMethod := authMethodName(method)
	if wireMethod == "" {
		return AuthMethodSurface{}, false
	}
	return AuthMethodSurface{
		WireMethod:        wireMethod,
		PluginMethodID:    authPluginIDForMethod(method, configuredMethodID),
		ExecutableLocally: authMethodExecutableLocally(method),
		BrokerRequired:    authMethodBrokerRequired(method),
	}, true
}

func resolveTokenAuthPayload(cfg Config) ([]byte, error) {
	switch {
	case strings.TrimSpace(cfg.AuthToken) != "":
		return []byte(cfg.AuthToken), nil
	case strings.TrimSpace(cfg.AuthMethodPayload) != "":
		return []byte(cfg.AuthMethodPayload), nil
	case strings.TrimSpace(cfg.AuthPayloadB64) != "":
		decoded, err := base64.StdEncoding.DecodeString(cfg.AuthPayloadB64)
		if err != nil {
			return nil, &Error{Kind: ErrData, Message: "invalid auth_payload_b64 encoding", SQLState: "22023"}
		}
		return decoded, nil
	case strings.TrimSpace(cfg.AuthPayloadJSON) != "":
		return []byte(cfg.AuthPayloadJSON), nil
	case strings.TrimSpace(cfg.WorkloadIdentityToken) != "":
		return []byte(cfg.WorkloadIdentityToken), nil
	case strings.TrimSpace(cfg.ProxyPrincipalAssertion) != "":
		return []byte(cfg.ProxyPrincipalAssertion), nil
	default:
		return nil, &Error{
			Kind:     ErrAuth,
			Message:  "TOKEN authentication requires auth_token, auth_method_payload, auth_payload_json, auth_payload_b64, workload_identity_token, or proxy_principal_assertion",
			SQLState: "28000",
		}
	}
}

func (c *Conn) resetResolvedAuthContext() {
	ingressMode, ok := normalizeFrontDoorMode(c.config.FrontDoorMode)
	if !ok {
		ingressMode = c.config.FrontDoorMode
	}
	c.resolvedAuthContext = ResolvedAuthContext{
		IngressMode: ingressMode,
	}
}

func (c *Conn) GetResolvedAuthContext() ResolvedAuthContext {
	return ResolvedAuthContext{
		IngressMode:          c.resolvedAuthContext.IngressMode,
		ResolvedAuthMethod:   c.resolvedAuthContext.ResolvedAuthMethod,
		ResolvedAuthPluginID: c.resolvedAuthContext.ResolvedAuthPluginID,
		ManagerAuthenticated: c.resolvedAuthContext.ManagerAuthenticated,
		Attached:             c.resolvedAuthContext.Attached,
	}
}

func (c *Conn) buildStartupParams() (map[string]string, error) {
	params := map[string]string{
		"database":     c.config.Database,
		"user":         c.config.User,
		"client_flags": strconv.FormatUint(uint64(c.config.ConnectClientFlags), 10),
	}
	if c.config.Role != "" {
		params["role"] = c.config.Role
	}
	if c.config.Application != "" {
		params["application_name"] = c.config.Application
	}
	selection := AuthPluginSelection{
		MethodID:                c.config.AuthMethodID,
		MethodPayload:           c.config.AuthMethodPayload,
		PayloadJSON:             c.config.AuthPayloadJSON,
		PayloadB64:              c.config.AuthPayloadB64,
		ProviderProfile:         c.config.AuthProviderProfile,
		RequiredMethods:         c.config.AuthRequiredMethods,
		ForbiddenMethods:        c.config.AuthForbiddenMethods,
		RequireChannelBinding:   c.config.AuthRequireChannelBinding,
		WorkloadIdentityToken:   c.config.WorkloadIdentityToken,
		ProxyPrincipalAssertion: c.config.ProxyPrincipalAssertion,
	}
	if err := ApplyAuthPluginSelection(params, selection); err != nil {
		return nil, &Error{Kind: ErrData, Message: err.Error(), SQLState: "22023"}
	}
	return params, nil
}

func (c *Conn) openSocket(ctx context.Context, requireIdentity bool, requireManagerToken bool) error {
	protocol, ok := normalizeNativeProtocol(c.config.Protocol)
	if !ok {
		return &Error{Kind: ErrNotSupported, Message: "only protocol=native is supported; connect to the native parser listener/port", SQLState: "0A000"}
	}
	c.config.Protocol = protocol
	frontDoorMode, ok := normalizeFrontDoorMode(c.config.FrontDoorMode)
	if !ok {
		return &Error{Kind: ErrNotSupported, Message: "front_door_mode must be direct or manager_proxy", SQLState: "0A000"}
	}
	c.config.FrontDoorMode = frontDoorMode
	sslMode, ok := normalizeSSLMode(c.config.SSLMode)
	if !ok {
		return &Error{Kind: ErrNotSupported, Message: "sslmode must be disable, require, verify-ca, or verify-full", SQLState: "0A000"}
	}
	c.config.SSLMode = sslMode
	compressionMode, ok := normalizeCompressionMode(c.config.Compression)
	if !ok {
		return &Error{Kind: ErrNotSupported, Message: "compression must be off or zstd", SQLState: "0A000"}
	}
	c.config.Compression = compressionMode
	if requireIdentity && (strings.TrimSpace(c.config.User) == "" || strings.TrimSpace(c.config.Database) == "") {
		return &Error{Kind: ErrConnection, Message: "user and database are required", SQLState: "08001"}
	}
	if requireManagerToken && c.config.FrontDoorMode == "manager_proxy" && strings.TrimSpace(c.config.ManagerAuthToken) == "" {
		return &Error{Kind: ErrConnection, Message: "manager_proxy mode requires manager_auth_token", SQLState: "08001"}
	}

	address := c.config.Host
	if strings.TrimSpace(address) == "" {
		address = "localhost"
		c.config.Host = address
	}
	if c.config.Port == 0 {
		c.config.Port = 3092
	}

	dialer := &net.Dialer{Timeout: c.config.ConnectTimeout}
	conn, err := dialer.DialContext(ctx, "tcp", net.JoinHostPort(address, strconv.Itoa(c.config.Port)))
	if err != nil {
		return &Error{Kind: ErrConnection, Message: err.Error(), SQLState: "08001"}
	}
	c.raw = conn
	if err := c.applyTLS(ctx); err != nil {
		_ = c.raw.Close()
		c.raw = nil
		return err
	}
	return nil
}

func (c *Conn) disconnectSocketForReconnect() {
	c.authed = false
	c.resolvedAuthContext.Attached = false
	c.attachmentID = [16]byte{}
	c.txnID = 0
	c.runtimeTxnActive = false
	c.explicitTransaction = false
	c.sequence = 0
	c.pending = nil
	c.params = map[string]string{}
	c.notificationHandlers = nil
	c.lastPlan = nil
	c.lastSblr = nil
	if c.keepaliveMgr != nil && c.keepaliveTracker != nil {
		c.keepaliveMgr.Unregister(c.connID)
		c.keepaliveTracker = nil
	}
	if c.raw != nil {
		_ = c.raw.Close()
		c.raw = nil
	}
}

func (c *Conn) probeAuthSurface(ctx context.Context) (AuthProbeResult, error) {
	c.initResilience()
	c.resetResolvedAuthContext()
	if err := c.openSocket(ctx, false, false); err != nil {
		return AuthProbeResult{}, err
	}
	defer c.disconnectSocketForReconnect()

	resolvedHost := c.config.Host
	if strings.TrimSpace(resolvedHost) == "" {
		resolvedHost = "localhost"
	}
	resolvedPort := c.config.Port
	if resolvedPort == 0 {
		resolvedPort = 3092
	}
	if c.config.FrontDoorMode == "manager_proxy" {
		return c.probeManagerAuthSurface(resolvedHost, resolvedPort)
	}
	return c.probeDirectAuthSurface(resolvedHost, resolvedPort)
}

func (c *Conn) probeDirectAuthSurface(resolvedHost string, resolvedPort int) (AuthProbeResult, error) {
	params, err := c.buildStartupParams()
	if err != nil {
		return AuthProbeResult{}, err
	}
	payload := buildStartupPayload(c.requestedFeatures(), params)
	if err := c.sendMessage(msgStartup, payload, 0, true); err != nil {
		return AuthProbeResult{}, err
	}

	for {
		msg, err := c.receive()
		if err != nil {
			return AuthProbeResult{}, err
		}
		if c.handleAsyncMessage(msg) {
			continue
		}
		switch msg.header.typ {
		case msgNegotiateVersion:
			continue
		case msgAuthRequest:
			method, _, err := parseAuthRequest(msg.body)
			if err != nil {
				return AuthProbeResult{}, err
			}
			surface, ok := describeAuthMethod(method, c.config.AuthMethodID)
			var admitted []AuthMethodSurface
			var requiredMethod string
			var requiredPluginMethodID string
			if ok {
				admitted = []AuthMethodSurface{surface}
				requiredMethod = surface.WireMethod
				requiredPluginMethodID = surface.PluginMethodID
			}
			return AuthProbeResult{
				Reachable:                      true,
				IngressMode:                    "direct",
				ResolvedHost:                   resolvedHost,
				ResolvedPort:                   resolvedPort,
				AdmittedMethods:                admitted,
				RequiredMethod:                 requiredMethod,
				RequiredPluginMethodID:         requiredPluginMethodID,
				AllowedTransportMask:           nil,
				AdditionalContinuationPossible: method == authScramSha256 || method == authScramSha512 || method == authToken || method == authPeer,
			}, nil
		case msgAuthOk, msgReady:
			return AuthProbeResult{
				Reachable:            true,
				IngressMode:          "direct",
				ResolvedHost:         resolvedHost,
				ResolvedPort:         resolvedPort,
				AdmittedMethods:      nil,
				AllowedTransportMask: nil,
			}, nil
		case msgError:
			return AuthProbeResult{}, buildProtocolError(msg.body)
		}
	}
}

func (c *Conn) probeManagerAuthSurface(resolvedHost string, resolvedPort int) (AuthProbeResult, error) {
	managerUser := c.config.ManagerUsername
	if managerUser == "" {
		if c.config.User != "" {
			managerUser = c.config.User
		} else {
			managerUser = "admin"
		}
	}

	helloPayload := make([]byte, 0, 4)
	helloPayload = appendU16(helloPayload, mcpProtocolVersion)
	helloPayload = appendU16(helloPayload, c.config.ManagerClientFlags)
	if err := c.sendManagerFrame(mcpMsgHello, helloPayload); err != nil {
		return AuthProbeResult{}, err
	}
	msgType, _, err := c.receiveManagerFrame()
	if err != nil {
		return AuthProbeResult{}, err
	}
	if msgType != mcpMsgStatusResponse {
		return AuthProbeResult{}, &Error{Kind: ErrConnection, Message: "expected MCP hello status response", SQLState: "08P01"}
	}

	authStart := make([]byte, 0, 64)
	authStart = appendLengthPrefixedString(authStart, managerUser)
	authStart = append(authStart, mcpAuthMethodToken)
	authStart = appendU32(authStart, 0)
	if err := c.sendManagerFrame(mcpMsgAuthStart, authStart); err != nil {
		return AuthProbeResult{}, err
	}
	msgType, _, err = c.receiveManagerFrame()
	if err != nil {
		return AuthProbeResult{}, err
	}

	return AuthProbeResult{
		Reachable:    true,
		IngressMode:  "manager_proxy",
		ResolvedHost: resolvedHost,
		ResolvedPort: resolvedPort,
		AdmittedMethods: []AuthMethodSurface{{
			WireMethod:        "TOKEN",
			PluginMethodID:    authPluginIDForMethod(authToken, ""),
			ExecutableLocally: true,
			BrokerRequired:    false,
		}},
		RequiredMethod:                 "TOKEN",
		RequiredPluginMethodID:         authPluginIDForMethod(authToken, ""),
		AllowedTransportMask:           nil,
		AdditionalContinuationPossible: msgType == mcpMsgAuthChallenge,
	}, nil
}

func ProbeAuthSurface(ctx context.Context, name string) (AuthProbeResult, error) {
	cfg, err := ParseConfig(name)
	if err != nil {
		return AuthProbeResult{}, err
	}
	conn := &Conn{config: cfg}
	return conn.probeAuthSurface(ctx)
}
