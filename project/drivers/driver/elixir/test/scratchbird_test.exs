# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

alias ScratchBird.Config

defmodule ScratchBirdConfigTest do
  use ExUnit.Case

  test "parses basic dsn" do
    cfg = Config.from_opts(url: "scratchbird://user:pass@localhost:3092/testdb")
    assert cfg.user == "user"
    assert cfg.password == "pass"
    assert cfg.host == "localhost"
    assert cfg.database == "testdb"
  end

  test "parses manager_proxy params and aliases" do
    cfg =
      Config.from_opts(
        url:
          "scratchbird://admin:secret@localhost:3092/mydb?" <>
            "frontdoormode=managed&mcp_auth_token=token&mcp_client_flags=7&mcp_auth_fast_path=false"
      )

    assert cfg.front_door_mode == "manager_proxy"
    assert cfg.manager_auth_token == "token"
    assert cfg.manager_client_flags == 7
    assert cfg.manager_auth_fast_path == false
  end

  test "parses staged auth bootstrap params and aliases" do
    cfg =
      Config.from_opts(
        url:
          "scratchbird://user:pass@localhost:3092/db?" <>
            "authtoken=token-123&connectclientflags=9&authmethodid=scratchbird.auth.scram_sha_512" <>
            "&authmethodpayload=opaque&authpayloadjson=%7B%22k%22%3A1%7D&authpayloadb64=YmluYXJ5" <>
            "&authproviderprofile=corp&authrequiredmethods=TOKEN&authforbiddenmethods=MD5" <>
            "&authrequirechannelbinding=true&workloadidentitytoken=widt&proxyprincipalassertion=proxy" <>
            "&dormantid=abc&dormantreattachtoken=def"
      )

    assert cfg.auth_token == "token-123"
    assert cfg.connect_client_flags == 9
    assert cfg.auth_method_id == "scratchbird.auth.scram_sha_512"
    assert cfg.auth_method_payload == "opaque"
    assert cfg.auth_payload_json == ~s({"k":1})
    assert cfg.auth_payload_b64 == "YmluYXJ5"
    assert cfg.auth_provider_profile == "corp"
    assert cfg.auth_required_methods == "TOKEN"
    assert cfg.auth_forbidden_methods == "MD5"
    assert cfg.auth_require_channel_binding == true
    assert cfg.workload_identity_token == "widt"
    assert cfg.proxy_principal_assertion == "proxy"
    assert cfg.dormant_id == "abc"
    assert cfg.dormant_reattach_token == "def"
  end

  test "rejects invalid front_door_mode" do
    assert_raise ArgumentError, fn ->
      Config.from_opts(url: "scratchbird://localhost:3092/db?front_door_mode=invalid")
    end
  end

  test "rejects invalid protocol" do
    assert_raise ArgumentError, fn ->
      Config.from_opts(url: "scratchbird://localhost:3092/db?protocol=postgres")
    end
  end
end
