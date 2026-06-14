# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "test_helper"

class TestConfig < Minitest::Test
  def test_parse_uri
    cfg = Scratchbird::Config.parse(
      "scratchbird://user:pass@localhost:3092/mydb?sslmode=require&connect_timeout=3&application_name=app&binary_transfer=false&compression=zstd"
    )
    assert_equal "localhost", cfg.host
    assert_equal 3092, cfg.port
    assert_equal "mydb", cfg.database
    assert_equal "user", cfg.user
    assert_equal "pass", cfg.password
    assert_equal "require", cfg.sslmode
    assert_equal 3000, cfg.connect_timeout_ms
    assert_equal "app", cfg.application_name
    assert_equal false, cfg.binary_transfer
    assert_equal "zstd", cfg.compression
  end

  def test_parse_key_value
    cfg = Scratchbird::Config.parse(
      "Host=server;Port=4000;Database=db;Username=me;Password=secret;SSL Mode=prefer;Timeout=5;Socket_Timeout=7"
    )
    assert_equal "server", cfg.host
    assert_equal 4000, cfg.port
    assert_equal "db", cfg.database
    assert_equal "me", cfg.user
    assert_equal "secret", cfg.password
    assert_equal 5000, cfg.connect_timeout_ms
    assert_equal 7000, cfg.socket_timeout_ms
  end

  def test_parse_manager_proxy_params
    cfg = Scratchbird::Config.parse(
      "scratchbird://admin:secret@localhost:3092/mydb?front_door_mode=manager_proxy&manager_auth_token=token&manager_client_flags=7"
    )
    assert_equal "manager_proxy", cfg.front_door_mode
    assert_equal "token", cfg.manager_auth_token
    assert_equal 7, cfg.manager_client_flags
  end

  def test_parse_metadata_expand_schema_parents_aliases
    cfg = Scratchbird::Config.parse(
      "scratchbird://user:pass@localhost:3092/mydb?metadata_expand_schema_parents=true"
    )
    assert_equal true, cfg.metadata_expand_schema_parents

    kv = Scratchbird::Config.parse("Host=server;Database=db;User=me;Expand_Schema_Parents=1")
    assert_equal true, kv.metadata_expand_schema_parents
  end

  def test_invalid_front_door_mode_raises
    assert_raises(ArgumentError) do
      Scratchbird::Config.parse("scratchbird://localhost:3092/db?front_door_mode=invalid")
    end
  end

  def test_parse_auth_plugin_and_pinning_params
    cfg = Scratchbird::Config.parse(
      "scratchbird://user:pass@localhost:3092/mydb" \
      "?connect_client_flags=257" \
      "&auth_token=bearer-token" \
      "&auth_method_id=scratchbird.auth.proxy_assertion" \
      "&auth_method_payload=opaque" \
      "&auth_payload_json=%7B%22subject%22%3A%22alice%22%7D" \
      "&auth_payload_b64=YWJj" \
      "&auth_provider_profile=corp_primary" \
      "&auth_required_methods=SCRAM_SHA_256%2CTOKEN" \
      "&auth_forbidden_methods=MD5" \
      "&auth_require_channel_binding=true" \
      "&workload_identity_token=jwt-token" \
      "&proxy_principal_assertion=signed-assertion"
    )
    assert_equal 257, cfg.connect_client_flags
    assert_equal "bearer-token", cfg.auth_token
    assert_equal "scratchbird.auth.proxy_assertion", cfg.auth_method_id
    assert_equal "opaque", cfg.auth_method_payload
    assert_equal "{\"subject\":\"alice\"}", cfg.auth_payload_json
    assert_equal "YWJj", cfg.auth_payload_b64
    assert_equal "corp_primary", cfg.auth_provider_profile
    assert_equal "SCRAM_SHA_256,TOKEN", cfg.auth_required_methods
    assert_equal "MD5", cfg.auth_forbidden_methods
    assert_equal true, cfg.auth_require_channel_binding
    assert_equal "jwt-token", cfg.workload_identity_token
    assert_equal "signed-assertion", cfg.proxy_principal_assertion
  end
end
