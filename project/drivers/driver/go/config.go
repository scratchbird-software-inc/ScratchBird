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
	"net/url"
	"strconv"
	"strings"
	"time"
)

type Config struct {
	Host                        string
	Port                        int
	FrontDoorMode               string
	TransportMode               string
	IPCMethod                   string
	IPCPath                     string
	Database                    string
	User                        string
	Password                    string
	Schema                      string
	Role                        string
	Protocol                    string
	SSLMode                     string
	SSLRootCert                 string
	SSLCert                     string
	SSLKey                      string
	SSLPassword                 string
	ConnectTimeout              time.Duration
	SocketTimeout               time.Duration
	Application                 string
	Autocommit                  bool
	BinaryTransfer              bool
	Compression                 string
	FetchSize                   uint32
	MetadataExpandSchemaParents bool
	ManagerAuthToken            string
	ManagerUsername             string
	ManagerDatabase             string
	ManagerConnectionProfile    string
	ManagerClientIntent         string
	ManagerClientFlags          uint16
	ManagerAuthFastPath         bool
	ConnectClientFlags          uint16
	AuthToken                   string
	AuthMethodID                string
	AuthMethodPayload           string
	AuthPayloadJSON             string
	AuthPayloadB64              string
	AuthProviderProfile         string
	AuthRequiredMethods         string
	AuthForbiddenMethods        string
	AuthRequireChannelBinding   bool
	WorkloadIdentityToken       string
	ProxyPrincipalAssertion     string
}

func defaultConfig() Config {
	return Config{
		Host:                     "localhost",
		Port:                     3092,
		FrontDoorMode:            "direct",
		TransportMode:            "inet_listener",
		IPCMethod:                "unix",
		Protocol:                 "native",
		SSLMode:                  "require",
		ConnectTimeout:           30 * time.Second,
		SocketTimeout:            0,
		Application:              "scratchbird_go",
		Autocommit:               true,
		BinaryTransfer:           true,
		Compression:              "off",
		FetchSize:                0,
		ManagerConnectionProfile: "SBsql",
		ManagerClientIntent:      "SBsql",
		ManagerAuthFastPath:      true,
		ConnectClientFlags:       0x0100,
	}
}

func ParseConfig(dsn string) (Config, error) {
	if strings.TrimSpace(dsn) == "" {
		return defaultConfig(), nil
	}
	if strings.Contains(dsn, "://") {
		return parseURI(dsn)
	}
	return parseKeyValue(dsn)
}

func parseURI(dsn string) (Config, error) {
	u, err := url.Parse(dsn)
	if err != nil {
		return Config{}, err
	}
	if !strings.EqualFold(u.Scheme, "scratchbird") {
		return Config{}, errors.New("unsupported DSN scheme")
	}
	cfg := defaultConfig()
	if u.Hostname() != "" {
		cfg.Host = u.Hostname()
	}
	if u.Port() != "" {
		if port, err := strconv.Atoi(u.Port()); err == nil {
			cfg.Port = port
		}
	}
	if u.User != nil {
		cfg.User = u.User.Username()
		if pass, ok := u.User.Password(); ok {
			cfg.Password = pass
		}
	}
	if u.Path != "" && u.Path != "/" {
		cfg.Database = strings.TrimPrefix(u.Path, "/")
	}
	for key, values := range u.Query() {
		if len(values) == 0 {
			continue
		}
		if err := applyParam(&cfg, key, values[0]); err != nil {
			return Config{}, err
		}
	}
	return cfg, nil
}

func parseKeyValue(dsn string) (Config, error) {
	cfg := defaultConfig()
	sep := " "
	if strings.Contains(dsn, ";") {
		sep = ";"
	}
	parts := strings.FieldsFunc(dsn, func(r rune) bool {
		return r == rune(sep[0])
	})
	for _, part := range parts {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		idx := strings.Index(part, "=")
		if idx < 0 {
			continue
		}
		key := strings.TrimSpace(part[:idx])
		value := strings.Trim(strings.TrimSpace(part[idx+1:]), "\"")
		if err := applyParam(&cfg, key, value); err != nil {
			return Config{}, err
		}
	}
	return cfg, nil
}

func normalizeNativeProtocol(value string) (string, bool) {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "",
		"native",
		"scratchbird",
		"scratchbird-native",
		"scratchbird_native",
		"sbwp",
		"postgres",
		"postgresql",
		"pg",
		"jdbc",
		"odbc",
		"sql",
		"mysql",
		"mariadb",
		"sqlite",
		"duckdb",
		"firebird":
		return "native", true
	default:
		return "", false
	}
}

func normalizeFrontDoorMode(value string) (string, bool) {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "", "direct":
		return "direct", true
	case "manager_proxy", "manager-proxy", "managed":
		return "manager_proxy", true
	default:
		return "", false
	}
}

func normalizeSSLMode(value string) (string, bool) {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "", "require", "on", "true", "1", "yes", "allow", "prefer":
		return "require", true
	case "verify-ca", "verifyca":
		return "verify-ca", true
	case "verify-full", "verifyfull":
		return "verify-full", true
	case "disable", "off", "false", "0", "no":
		return "disable", true
	default:
		return "", false
	}
}

func normalizeCompressionMode(value string) (string, bool) {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "", "off", "none", "false", "0", "no":
		return "off", true
	case "zstd", "on", "true", "1", "yes":
		return "zstd", true
	default:
		return "", false
	}
}

func applyParam(cfg *Config, key, value string) error {
	switch strings.ToLower(key) {
	case "host", "server", "data source", "datasource":
		cfg.Host = value
	case "port":
		if port, err := strconv.Atoi(value); err == nil {
			cfg.Port = port
		}
	case "front_door_mode", "frontdoormode", "connection_mode", "ingress_mode":
		normalized, ok := normalizeFrontDoorMode(value)
		if !ok {
			return errors.New("front_door_mode must be direct or manager_proxy")
		}
		cfg.FrontDoorMode = normalized
	case "transport_mode", "transportmode", "transport":
		cfg.TransportMode = value
	case "ipc_method", "ipcmethod":
		cfg.IPCMethod = value
	case "ipc_path", "ipcpath", "socket_path", "pipe_name":
		cfg.IPCPath = value
	case "database", "dbname", "initial catalog":
		cfg.Database = value
	case "user", "username", "user id", "uid":
		cfg.User = value
	case "password", "pwd":
		cfg.Password = value
	case "schema", "search_path", "searchpath", "currentschema":
		cfg.Schema = value
	case "role":
		cfg.Role = value
	case "protocol", "parser", "dialect":
		normalized, ok := normalizeNativeProtocol(value)
		if !ok {
			return errors.New("only protocol=native is supported; connect to the native parser listener/port")
		}
		cfg.Protocol = normalized
	case "sslmode", "ssl mode", "ssl":
		normalized, ok := normalizeSSLMode(value)
		if !ok {
			return errors.New("sslmode must be disable, require, verify-ca, or verify-full")
		}
		cfg.SSLMode = normalized
	case "sslrootcert":
		cfg.SSLRootCert = value
	case "sslcert":
		cfg.SSLCert = value
	case "sslkey":
		cfg.SSLKey = value
	case "sslpassword":
		cfg.SSLPassword = value
	case "connect_timeout", "connecttimeout", "timeout":
		if seconds, err := strconv.Atoi(value); err == nil {
			cfg.ConnectTimeout = time.Duration(seconds) * time.Second
		}
	case "socket_timeout", "sockettimeout":
		if seconds, err := strconv.Atoi(value); err == nil {
			cfg.SocketTimeout = time.Duration(seconds) * time.Second
		}
	case "application_name", "applicationname":
		cfg.Application = value
	case "autocommit", "auto_commit", "auto-commit":
		cfg.Autocommit = parseBoolParam(value)
	case "binary_transfer", "binarytransfer":
		cfg.BinaryTransfer = parseBoolParam(value)
	case "compression":
		normalized, ok := normalizeCompressionMode(value)
		if !ok {
			return errors.New("compression must be off or zstd")
		}
		cfg.Compression = normalized
	case "fetch_size", "fetchsize", "default_fetch_size":
		if rows, err := strconv.Atoi(value); err == nil && rows >= 0 {
			cfg.FetchSize = uint32(rows)
		}
	case "metadata_expand_schema_parents", "metadataexpandschemaparents", "expand_schema_parents", "expandschemaparents", "dbeaver_expand_schema_parents":
		cfg.MetadataExpandSchemaParents = parseBoolParam(value)
	case "manager_auth_token", "mcp_auth_token":
		cfg.ManagerAuthToken = value
	case "manager_username", "mcp_username":
		cfg.ManagerUsername = value
	case "manager_database", "mcp_database":
		cfg.ManagerDatabase = value
	case "manager_connection_profile", "mcp_connection_profile":
		cfg.ManagerConnectionProfile = value
	case "manager_client_intent", "mcp_client_intent":
		cfg.ManagerClientIntent = value
	case "manager_client_flags", "mcp_client_flags":
		if flags, err := strconv.ParseUint(value, 10, 16); err == nil {
			cfg.ManagerClientFlags = uint16(flags)
		}
	case "manager_auth_fast_path", "mcp_auth_fast_path":
		cfg.ManagerAuthFastPath = parseBoolParam(value)
	case "client_flags", "connect_client_flags":
		if flags, err := strconv.ParseUint(value, 10, 16); err == nil {
			cfg.ConnectClientFlags = uint16(flags)
		}
	case "auth_token", "authtoken", "bearer_token", "bearertoken", "token":
		cfg.AuthToken = value
	case authParamMethodID, "authmethodid":
		cfg.AuthMethodID = value
	case authParamMethodPayload, "authmethodpayload":
		cfg.AuthMethodPayload = value
	case authParamPayloadJSON, "authpayloadjson":
		cfg.AuthPayloadJSON = value
	case authParamPayloadB64, "authpayloadb64":
		cfg.AuthPayloadB64 = value
	case authParamProviderProfile, "authproviderprofile":
		cfg.AuthProviderProfile = value
	case authParamRequiredMethods, "authrequiredmethods":
		cfg.AuthRequiredMethods = value
	case authParamForbiddenMethods, "authforbiddenmethods":
		cfg.AuthForbiddenMethods = value
	case authParamRequireChannelBinding, "authrequirechannelbinding":
		cfg.AuthRequireChannelBinding = parseBoolParam(value)
	case authParamWorkloadIdentityToken, "workloadidentitytoken":
		cfg.WorkloadIdentityToken = value
	case authParamProxyPrincipalAssertion, "proxyprincipalassertion", "proxy_assertion":
		cfg.ProxyPrincipalAssertion = value
	}
	return nil
}

func parseBoolParam(value string) bool {
	normalized := strings.ToLower(strings.TrimSpace(value))
	return normalized == "1" ||
		normalized == "true" ||
		normalized == "yes" ||
		normalized == "on"
}
