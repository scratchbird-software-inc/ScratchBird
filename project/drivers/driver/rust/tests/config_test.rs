// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use scratchbird::Config;

#[test]
fn parse_uri() {
    let cfg = Config::from_dsn(
        "scratchbird://user:pass@localhost:3092/mydb?sslmode=require&connect_timeout=3&application_name=app&binary_transfer=false&compression=zstd",
    )
    .unwrap();
    assert_eq!(cfg.host, "localhost");
    assert_eq!(cfg.port, 3092);
    assert_eq!(cfg.database, "mydb");
    assert_eq!(cfg.user, "user");
    assert_eq!(cfg.password, "pass");
    assert_eq!(cfg.sslmode, "require");
    assert_eq!(cfg.connect_timeout_ms, 3000);
    assert_eq!(cfg.application_name, "app");
    assert_eq!(cfg.binary_transfer, false);
    assert_eq!(cfg.compression, "zstd");
}

#[test]
fn parse_key_value() {
    let cfg = Config::from_dsn("Host=server;Port=4000;Database=db;Username=me;Password=secret;SSL Mode=prefer;Timeout=5;Socket_Timeout=7").unwrap();
    assert_eq!(cfg.host, "server");
    assert_eq!(cfg.port, 4000);
    assert_eq!(cfg.database, "db");
    assert_eq!(cfg.user, "me");
    assert_eq!(cfg.password, "secret");
    assert_eq!(cfg.connect_timeout_ms, 5000);
    assert_eq!(cfg.socket_timeout_ms, 7000);
}

#[test]
fn parse_manager_proxy_params() {
    let cfg = Config::from_dsn(
        "scratchbird://admin:secret@localhost:3090/mydb?front_door_mode=manager_proxy&manager_auth_token=token&manager_client_flags=7",
    )
    .unwrap();
    assert_eq!(cfg.front_door_mode, "manager_proxy");
    assert_eq!(cfg.manager_auth_token, "token");
    assert_eq!(cfg.manager_client_flags, 7);
}

#[test]
fn parse_uri_precedence_latest_override_wins() {
    let cfg = Config::from_dsn(
        "scratchbird://user:pass@localhost:3092/pathdb?database=querydb&user=override&connect_timeout=1&connect_timeout=2",
    )
    .unwrap();
    assert_eq!(cfg.database, "querydb");
    assert_eq!(cfg.user, "override");
    assert_eq!(cfg.connect_timeout_ms, 2000);
}

#[test]
fn parse_key_value_precedence_latest_override_wins() {
    let cfg = Config::from_dsn("Host=first;Host=second;Port=3100;Port=4100;Database=a;Database=b")
        .unwrap();
    assert_eq!(cfg.host, "second");
    assert_eq!(cfg.port, 4100);
    assert_eq!(cfg.database, "b");
}

#[test]
fn parse_auth_plugin_selection_params() {
    let cfg = Config::from_dsn(
        "scratchbird://user:pass@localhost:3092/mydb?connect_client_flags=257&auth_token=bearer-token&auth_method_id=scratchbird.auth.password&auth_method_payload=opaque&auth_payload_json=%7B%22tenant%22%3A%22alpha%22%7D&auth_payload_b64=dGVzdA%3D%3D&auth_provider_profile=default&auth_required_methods=SCRAM_SHA_256%2CTOKEN&auth_forbidden_methods=MD5&auth_require_channel_binding=true&workload_identity_token=jwt-token&proxy_principal_assertion=signed-assertion",
    )
    .unwrap();
    assert_eq!(cfg.connect_client_flags, 257);
    assert_eq!(cfg.auth_token, "bearer-token");
    assert_eq!(cfg.auth_method_id, "scratchbird.auth.password");
    assert_eq!(cfg.auth_method_payload, "opaque");
    assert_eq!(cfg.auth_payload_json, "{\"tenant\":\"alpha\"}");
    assert_eq!(cfg.auth_payload_b64, "dGVzdA==");
    assert_eq!(cfg.auth_provider_profile, "default");
    assert_eq!(cfg.auth_required_methods, "SCRAM_SHA_256,TOKEN");
    assert_eq!(cfg.auth_forbidden_methods, "MD5");
    assert!(cfg.auth_require_channel_binding);
    assert_eq!(cfg.workload_identity_token, "jwt-token");
    assert_eq!(cfg.proxy_principal_assertion, "signed-assertion");
}

#[test]
fn parse_protocol_hints_normalize_to_native() {
    let cfg = Config::from_dsn(
        "scratchbird://user:pass@localhost:3092/mydb?protocol=jdbc&parser=postgresql&dialect=odbc",
    )
    .unwrap();
    assert_eq!(cfg.protocol, "native");
}

#[test]
fn parse_rejects_unknown_compression_mode() {
    let err = Config::from_dsn("scratchbird://user:pass@localhost:3092/mydb?compression=gzip")
        .unwrap_err();
    assert_eq!(err.message, "compression must be off or zstd");
}

#[test]
fn parse_metadata_expand_schema_parent_aliases() {
    let cfg = Config::from_dsn(
        "scratchbird://user:pass@localhost:3092/mydb?metadata_expand_schema_parents=true&dbeaver_expand_schema_parents=0&expand_schema_parents=1",
    )
    .unwrap();
    assert!(cfg.metadata_expand_schema_parents);
}
