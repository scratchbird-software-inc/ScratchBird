// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import "testing"

func TestParseURI(t *testing.T) {
	cfg, err := ParseConfig("scratchbird://user:pass@localhost:3092/mydb?sslmode=require&connect_timeout=3&application_name=app&binary_transfer=false&compression=zstd")
	if err != nil {
		t.Fatalf("parse error: %v", err)
	}
	if cfg.Host != "localhost" || cfg.Port != 3092 {
		t.Fatalf("unexpected host/port: %s:%d", cfg.Host, cfg.Port)
	}
	if cfg.Database != "mydb" || cfg.User != "user" || cfg.Password != "pass" {
		t.Fatalf("unexpected credentials: %s/%s", cfg.User, cfg.Database)
	}
	if cfg.SSLMode != "require" || cfg.Application != "app" {
		t.Fatalf("unexpected ssl/app values")
	}
	if cfg.Compression != "zstd" || cfg.BinaryTransfer {
		t.Fatalf("unexpected compression/binary")
	}
	if cfg.Protocol != "native" {
		t.Fatalf("expected protocol=native, got %q", cfg.Protocol)
	}
}

func TestParseKeyValue(t *testing.T) {
	cfg, err := ParseConfig("Host=server;Port=4000;Database=db;Username=me;Password=secret;SSL Mode=prefer;Timeout=5;Socket_Timeout=7")
	if err != nil {
		t.Fatalf("parse error: %v", err)
	}
	if cfg.Host != "server" || cfg.Port != 4000 {
		t.Fatalf("unexpected host/port: %s:%d", cfg.Host, cfg.Port)
	}
	if cfg.Database != "db" || cfg.User != "me" || cfg.Password != "secret" {
		t.Fatalf("unexpected credentials: %s/%s", cfg.User, cfg.Database)
	}
	if cfg.ConnectTimeout.Seconds() != 5 || cfg.SocketTimeout.Seconds() != 7 {
		t.Fatalf("unexpected timeouts: %v/%v", cfg.ConnectTimeout, cfg.SocketTimeout)
	}
	if cfg.Protocol != "native" {
		t.Fatalf("expected protocol=native, got %q", cfg.Protocol)
	}
}

func TestParseRejectsNonNativeProtocol(t *testing.T) {
	cfg, err := ParseConfig("scratchbird://localhost:3092/db?protocol=jdbc&parser=postgresql&dialect=odbc")
	if err != nil {
		t.Fatalf("unexpected parse failure for protocol hints: %v", err)
	}
	if cfg.Protocol != "native" {
		t.Fatalf("expected protocol=native, got %q", cfg.Protocol)
	}
}

func TestParseManagerProxyParams(t *testing.T) {
	cfg, err := ParseConfig("scratchbird://user:pass@localhost:3090/mydb?front_door_mode=manager_proxy&manager_auth_token=token&manager_username=admin&manager_database=mydb&manager_connection_profile=SBsql&manager_client_intent=SBsql&manager_client_flags=7&manager_auth_fast_path=false")
	if err != nil {
		t.Fatalf("parse error: %v", err)
	}
	if cfg.FrontDoorMode != "manager_proxy" {
		t.Fatalf("expected manager_proxy mode, got %q", cfg.FrontDoorMode)
	}
	if cfg.ManagerAuthToken != "token" || cfg.ManagerUsername != "admin" || cfg.ManagerDatabase != "mydb" {
		t.Fatalf("unexpected manager config fields")
	}
	if cfg.ManagerConnectionProfile != "SBsql" || cfg.ManagerClientIntent != "SBsql" {
		t.Fatalf("unexpected manager profile/intent values")
	}
	if cfg.ManagerClientFlags != 7 {
		t.Fatalf("expected manager_client_flags=7, got %d", cfg.ManagerClientFlags)
	}
	if cfg.ManagerAuthFastPath {
		t.Fatalf("expected manager_auth_fast_path=false")
	}
}

func TestParseRejectsInvalidFrontDoorMode(t *testing.T) {
	_, err := ParseConfig("scratchbird://localhost:3092/db?front_door_mode=invalid")
	if err == nil {
		t.Fatalf("expected parse failure for invalid front_door_mode")
	}
}

func TestParseMetadataExpandSchemaParents(t *testing.T) {
	cfg, err := ParseConfig("scratchbird://localhost:3092/db?metadataExpandSchemaParents=true")
	if err != nil {
		t.Fatalf("parse error: %v", err)
	}
	if !cfg.MetadataExpandSchemaParents {
		t.Fatalf("expected metadataExpandSchemaParents=true from URI alias")
	}

	cfg, err = ParseConfig("Host=localhost;Database=db;dbeaver_expand_schema_parents=on")
	if err != nil {
		t.Fatalf("parse error: %v", err)
	}
	if !cfg.MetadataExpandSchemaParents {
		t.Fatalf("expected metadataExpandSchemaParents=true from dbeaver alias")
	}

	cfg, err = ParseConfig("Host=localhost;Database=db;metadata_expand_schema_parents=false")
	if err != nil {
		t.Fatalf("parse error: %v", err)
	}
	if cfg.MetadataExpandSchemaParents {
		t.Fatalf("expected metadataExpandSchemaParents=false when explicitly disabled")
	}
}

func TestParseRejectsUnknownCompressionValue(t *testing.T) {
	if _, err := ParseConfig("scratchbird://localhost:3092/db?compression=gzip"); err == nil {
		t.Fatalf("expected parse failure for unsupported compression")
	}
}

func TestParseAuthPluginStartupParams(t *testing.T) {
	cfg, err := ParseConfig(
		"scratchbird://user:pass@localhost:3092/mydb?" +
			"connect_client_flags=257&auth_token=bearer-token&auth_method_id=scratchbird.auth.oidc&auth_method_payload=opaque&" +
			"auth_payload_json=%7B%22aud%22%3A%22sb%22%7D&auth_payload_b64=YWJj&" +
			"auth_provider_profile=corp&auth_required_methods=SCRAM_SHA_256%2CTOKEN&" +
			"auth_forbidden_methods=MD5&auth_require_channel_binding=true&" +
			"workload_identity_token=jwt-token&proxy_principal_assertion=signed-assertion",
	)
	if err != nil {
		t.Fatalf("parse error: %v", err)
	}
	if cfg.ConnectClientFlags != 257 {
		t.Fatalf("unexpected connect client flags: %d", cfg.ConnectClientFlags)
	}
	if cfg.AuthToken != "bearer-token" {
		t.Fatalf("unexpected auth token: %q", cfg.AuthToken)
	}
	if cfg.AuthMethodID != "scratchbird.auth.oidc" {
		t.Fatalf("unexpected auth method id: %q", cfg.AuthMethodID)
	}
	if cfg.AuthMethodPayload != "opaque" {
		t.Fatalf("unexpected auth method payload: %q", cfg.AuthMethodPayload)
	}
	if cfg.AuthPayloadJSON != `{"aud":"sb"}` {
		t.Fatalf("unexpected auth payload json: %q", cfg.AuthPayloadJSON)
	}
	if cfg.AuthPayloadB64 != "YWJj" {
		t.Fatalf("unexpected auth payload b64: %q", cfg.AuthPayloadB64)
	}
	if cfg.AuthProviderProfile != "corp" {
		t.Fatalf("unexpected auth provider profile: %q", cfg.AuthProviderProfile)
	}
	if cfg.AuthRequiredMethods != "SCRAM_SHA_256,TOKEN" {
		t.Fatalf("unexpected auth required methods: %q", cfg.AuthRequiredMethods)
	}
	if cfg.AuthForbiddenMethods != "MD5" {
		t.Fatalf("unexpected auth forbidden methods: %q", cfg.AuthForbiddenMethods)
	}
	if !cfg.AuthRequireChannelBinding {
		t.Fatalf("expected auth require channel binding=true")
	}
	if cfg.WorkloadIdentityToken != "jwt-token" {
		t.Fatalf("unexpected workload identity token: %q", cfg.WorkloadIdentityToken)
	}
	if cfg.ProxyPrincipalAssertion != "signed-assertion" {
		t.Fatalf("unexpected proxy principal assertion: %q", cfg.ProxyPrincipalAssertion)
	}
}
