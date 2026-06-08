# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "test_helper"
require "base64"
require "openssl"

class TestConnAuthProtocol < Minitest::Test
  FakeSocket = Struct.new(:close_calls) do
    def close
      self.close_calls += 1
    end
  end

  def test_connect_requires_user_and_database
    cfg = base_config
    cfg.user = ""
    cfg.database = ""
    client = Scratchbird::Client.new(cfg)

    err = assert_raises(Scratchbird::ConnectionError) { client.connect }
    assert_equal "user and database are required", err.message
  end

  def test_connect_allows_binary_transfer_false
    cfg = base_config
    cfg.binary_transfer = false
    client = Scratchbird::Client.new(cfg)
    client.define_singleton_method(:connect_tcp) { raise "tcp_invoked" }

    err = assert_raises(RuntimeError) { client.connect }
    assert_equal "tcp_invoked", err.message
  end

  def test_connect_allows_zstd_compression
    cfg = base_config
    cfg.compression = "zstd"
    client = Scratchbird::Client.new(cfg)
    client.define_singleton_method(:connect_tcp) { raise "tcp_invoked" }

    err = assert_raises(RuntimeError) { client.connect }
    assert_equal "tcp_invoked", err.message
  end

  def test_connect_rejects_non_native_protocol
    cfg = base_config
    cfg.protocol = "postgresql"
    client = Scratchbird::Client.new(cfg)

    err = assert_raises(Scratchbird::NotSupportedError) { client.connect }
    assert_match(/Only protocol=native is supported/, err.message)
  end

  def test_connect_rejects_invalid_front_door_mode
    cfg = base_config
    cfg.front_door_mode = "invalid"
    client = Scratchbird::Client.new(cfg)

    err = assert_raises(Scratchbird::NotSupportedError) { client.connect }
    assert_equal "front_door_mode must be direct or manager_proxy.", err.message
  end

  def test_wrap_tls_passthroughs_sslmode_disable
    cfg = base_config
    cfg.sslmode = "disable"
    client = Scratchbird::Client.new(cfg)
    socket = FakeSocket.new(0)

    assert_same socket, client.send(:wrap_tls, socket)
  end

  def test_connect_closes_socket_when_manager_proxy_auth_token_missing
    cfg = base_config
    cfg.front_door_mode = "manager_proxy"
    cfg.manager_auth_token = ""
    client = Scratchbird::Client.new(cfg)
    fake_socket = FakeSocket.new(0)

    client.define_singleton_method(:connect_tcp) { fake_socket }
    client.define_singleton_method(:wrap_tls) { |raw_socket| raw_socket }

    err = assert_raises(Scratchbird::ConnectionError) { client.connect }
    assert_equal "manager_proxy mode requires manager_auth_token", err.message
    assert_equal 0, fake_socket.close_calls
    assert_equal false, client.connected?
  end

  def test_manager_proxy_auth_failure_raises_auth_error
    cfg = base_config
    cfg.front_door_mode = "manager_proxy"
    cfg.manager_auth_token = "bad-token"
    client = Scratchbird::Client.new(cfg)
    sent = []
    responses = [
      [Scratchbird::Client::MCP_MSG_STATUS_RESPONSE, "".b],
      [Scratchbird::Client::MCP_MSG_AUTH_RESPONSE, manager_auth_error_payload("bad token")]
    ]

    client.define_singleton_method(:send_manager_frame) do |type, payload|
      sent << [type, payload]
    end
    client.define_singleton_method(:recv_manager_frame) do
      frame = responses.shift
      raise "missing fake manager frame" unless frame
      frame
    end

    err = assert_raises(Scratchbird::AuthError) { client.send(:perform_manager_connect) }
    assert_equal "bad token", err.message
    assert_equal(
      [Scratchbird::Client::MCP_MSG_HELLO, Scratchbird::Client::MCP_MSG_AUTH_START],
      sent.map(&:first)
    )
  end

  def test_manager_proxy_connect_success
    cfg = base_config
    cfg.front_door_mode = "manager_proxy"
    cfg.manager_auth_token = "token"
    client = Scratchbird::Client.new(cfg)
    sent = []
    responses = [
      [Scratchbird::Client::MCP_MSG_STATUS_RESPONSE, "".b],
      [Scratchbird::Client::MCP_MSG_AUTH_RESPONSE, manager_auth_success_payload],
      [Scratchbird::Client::MCP_MSG_CONNECT_RESPONSE, manager_connect_success_payload]
    ]

    client.define_singleton_method(:send_manager_frame) do |type, payload|
      sent << [type, payload]
    end
    client.define_singleton_method(:recv_manager_frame) do
      frame = responses.shift
      raise "missing fake manager frame" unless frame
      frame
    end

    assert_equal true, client.send(:perform_manager_connect)
    assert_equal true, client.get_resolved_auth_context.manager_authenticated
    assert_equal(
      [
        Scratchbird::Client::MCP_MSG_HELLO,
        Scratchbird::Client::MCP_MSG_AUTH_START,
        Scratchbird::Client::MCP_MSG_DB_CONNECT
      ],
      sent.map(&:first)
    )
  end

  def test_probe_auth_surface_direct_reports_scram_sha512
    cfg = base_config
    client = Scratchbird::Client.new(cfg)
    auth_request_payload = build_auth_request_payload(Scratchbird::Protocol::AUTH_SCRAM_SHA512)

    client.define_singleton_method(:open_socket) do |require_identity:, require_manager_token:|
      @open_socket_args = [require_identity, require_manager_token]
    end
    client.define_singleton_method(:disconnect_socket_for_reconnect) do
      @disconnect_called = true
    end
    client.define_singleton_method(:send_message) do |type, payload, _flags, _force_zero|
      @startup_payload = payload if type == Scratchbird::Protocol::MSG_STARTUP
      true
    end
    client.define_singleton_method(:recv_message) do
      [Scratchbird::Protocol::MSG_AUTH_REQUEST, 0, auth_request_payload, 0, ("\0" * 16).b, 0]
    end

    result = client.probe_auth_surface
    assert_equal [false, false], client.instance_variable_get(:@open_socket_args)
    assert_equal true, client.instance_variable_get(:@disconnect_called)
    assert_equal "direct", result.ingress_mode
    assert_equal "SCRAM_SHA_512", result.required_method
    assert_equal "scratchbird.auth.scram_sha_512", result.required_plugin_method_id
    assert_equal true, result.additional_continuation_possible
    assert_equal 1, result.admitted_methods.length
    assert_equal true, result.admitted_methods.first.executable_locally
  end

  def test_probe_auth_surface_manager_proxy_reports_token
    cfg = base_config
    cfg.front_door_mode = "manager_proxy"
    client = Scratchbird::Client.new(cfg)
    sent = []
    responses = [
      [Scratchbird::Client::MCP_MSG_STATUS_RESPONSE, "".b],
      [Scratchbird::Client::MCP_MSG_AUTH_CHALLENGE, "".b]
    ]

    client.define_singleton_method(:open_socket) do |require_identity:, require_manager_token:|
      @open_socket_args = [require_identity, require_manager_token]
    end
    client.define_singleton_method(:disconnect_socket_for_reconnect) do
      @disconnect_called = true
    end
    client.define_singleton_method(:send_manager_frame) do |type, payload|
      sent << [type, payload]
    end
    client.define_singleton_method(:recv_manager_frame) do
      frame = responses.shift
      raise "missing fake manager frame" unless frame
      frame
    end

    result = client.probe_auth_surface
    assert_equal [false, false], client.instance_variable_get(:@open_socket_args)
    assert_equal true, client.instance_variable_get(:@disconnect_called)
    assert_equal(
      [Scratchbird::Client::MCP_MSG_HELLO, Scratchbird::Client::MCP_MSG_AUTH_START],
      sent.map(&:first)
    )
    assert_equal "manager_proxy", result.ingress_mode
    assert_equal "TOKEN", result.required_method
    assert_equal "scratchbird.auth.authkey_token", result.required_plugin_method_id
    assert_equal true, result.additional_continuation_possible
  end

  def test_handshake_scram_success_with_server_verifier
    cfg = base_config
    cfg.password = "secret-password"
    client = Scratchbird::Client.new(cfg)
    attachment = ("a" * 16).b
    sent = []
    step = 0
    client_first = nil
    client_final = nil
    server_first = nil
    auth_request_payload = build_auth_request_payload(Scratchbird::Protocol::AUTH_SCRAM_SHA256)
    auth_continue_builder = method(:build_auth_continue_payload)
    auth_ok_builder = method(:build_auth_ok_payload)
    verifier_builder = method(:compute_scram_server_verifier)
    ready_payload = build_ready_payload(0, 77)

    client.define_singleton_method(:send_message) do |type, payload, _flags, _force_zero|
      sent << [type, payload]
      if type == Scratchbird::Protocol::MSG_AUTH_RESPONSE
        if client_first.nil?
          client_first = payload
        else
          client_final = payload
        end
      end
      sent.length
    end

    client.define_singleton_method(:recv_message) do
      step += 1
      case step
      when 1
        [Scratchbird::Protocol::MSG_AUTH_REQUEST, 0, auth_request_payload, 0, attachment, 0]
      when 2
        nonce = client_first.to_s.split("r=", 2).last
        salt = Base64.strict_encode64("testsalt")
        server_first = "r=#{nonce}server,s=#{salt},i=4096"
        [Scratchbird::Protocol::MSG_AUTH_CONTINUE, 0, auth_continue_builder.call(server_first), 0, attachment, 0]
      when 3
        verifier = verifier_builder.call(
          password: cfg.password,
          client_first: client_first,
          server_first: server_first,
          client_final: client_final
        )
        [Scratchbird::Protocol::MSG_AUTH_OK, 0, auth_ok_builder.call("v=#{verifier}"), 0, attachment, 77]
      when 4
        [Scratchbird::Protocol::MSG_READY, 0, ready_payload, 0, attachment, 77]
      else
        raise "unexpected recv step #{step}"
      end
    end

    client.send(:handshake)
    assert_equal 0, client.txn_id
    assert_equal attachment, client.instance_variable_get(:@attachment_id)
    assert_equal "SCRAM_SHA_256", client.get_resolved_auth_context.resolved_auth_method
    assert_equal "scratchbird.auth.scram_sha_256", client.get_resolved_auth_context.resolved_auth_plugin_id
    assert_equal Scratchbird::Protocol::MSG_STARTUP, sent[0][0]
    assert_equal Scratchbird::Protocol::MSG_AUTH_RESPONSE, sent[1][0]
    assert_equal Scratchbird::Protocol::MSG_AUTH_RESPONSE, sent[2][0]
  end

  def test_handshake_scram_sha512_success
    cfg = base_config
    cfg.password = "secret-password"
    client = Scratchbird::Client.new(cfg)
    attachment = ("z" * 16).b
    step = 0
    client_first = nil
    client_final = nil
    server_first = nil
    auth_request_payload = build_auth_request_payload(Scratchbird::Protocol::AUTH_SCRAM_SHA512)
    auth_continue_builder = method(:build_auth_continue_payload)
    verifier_builder = method(:compute_scram_server_verifier)
    auth_ok_builder = method(:build_auth_ok_payload)
    ready_payload = build_ready_payload(0, 0)

    client.define_singleton_method(:send_message) do |type, payload, _flags, _force_zero|
      if type == Scratchbird::Protocol::MSG_AUTH_RESPONSE
        if client_first.nil?
          client_first = payload
        else
          client_final = payload
        end
      end
      true
    end

    client.define_singleton_method(:recv_message) do
      step += 1
      case step
      when 1
        [Scratchbird::Protocol::MSG_AUTH_REQUEST, 0, auth_request_payload, 0, attachment, 0]
      when 2
        nonce = client_first.to_s.split("r=", 2).last
        salt = Base64.strict_encode64("testsalt")
        server_first = "r=#{nonce}server,s=#{salt},i=4096"
        [Scratchbird::Protocol::MSG_AUTH_CONTINUE, 0, auth_continue_builder.call(server_first, Scratchbird::Protocol::AUTH_SCRAM_SHA512), 0, attachment, 0]
      when 3
        verifier = verifier_builder.call(
          password: cfg.password,
          client_first: client_first,
          server_first: server_first,
          client_final: client_final,
          digest: "sha512"
        )
        [Scratchbird::Protocol::MSG_AUTH_OK, 0, auth_ok_builder.call("v=#{verifier}"), 0, attachment, 0]
      when 4
        [Scratchbird::Protocol::MSG_READY, 0, ready_payload, 0, attachment, 0]
      else
        raise "unexpected recv step #{step}"
      end
    end

    client.send(:handshake)
    assert_equal "SCRAM_SHA_512", client.get_resolved_auth_context.resolved_auth_method
    assert_equal "scratchbird.auth.scram_sha_512", client.get_resolved_auth_context.resolved_auth_plugin_id
  end

  def test_handshake_token_auth_uses_auth_token
    cfg = base_config
    cfg.auth_token = "bearer-token"
    client = Scratchbird::Client.new(cfg)
    attachment = ("t" * 16).b
    sent = []
    step = 0
    auth_request_payload = build_auth_request_payload(Scratchbird::Protocol::AUTH_TOKEN)
    auth_ok_payload = build_auth_ok_payload("")
    ready_payload = build_ready_payload(0, 0)

    client.define_singleton_method(:send_message) do |type, payload, _flags, _force_zero|
      sent << [type, payload]
      true
    end
    client.define_singleton_method(:recv_message) do
      step += 1
      case step
      when 1
        [Scratchbird::Protocol::MSG_AUTH_REQUEST, 0, auth_request_payload, 0, attachment, 0]
      when 2
        [Scratchbird::Protocol::MSG_AUTH_OK, 0, auth_ok_payload, 0, attachment, 0]
      when 3
        [Scratchbird::Protocol::MSG_READY, 0, ready_payload, 0, attachment, 0]
      else
        raise "unexpected recv step #{step}"
      end
    end

    client.send(:handshake)
    assert_equal "bearer-token", sent[1][1]
    assert_equal "TOKEN", client.get_resolved_auth_context.resolved_auth_method
    assert_equal "scratchbird.auth.authkey_token", client.get_resolved_auth_context.resolved_auth_plugin_id
  end

  def test_handshake_peer_fails_closed
    cfg = base_config
    client = Scratchbird::Client.new(cfg)
    auth_request_payload = build_auth_request_payload(Scratchbird::Protocol::AUTH_PEER)

    client.define_singleton_method(:send_message) { |_type, _payload, _flags, _force_zero| true }
    client.define_singleton_method(:recv_message) do
      [Scratchbird::Protocol::MSG_AUTH_REQUEST, 0, auth_request_payload, 0, ("\0" * 16).b, 0]
    end

    err = assert_raises(Scratchbird::NotSupportedError) { client.send(:handshake) }
    assert_equal "0A000", err.sqlstate
    assert_equal "PEER", client.get_resolved_auth_context.resolved_auth_method
    assert_equal "scratchbird.auth.peer_uid", client.get_resolved_auth_context.resolved_auth_plugin_id
  end

  def test_handshake_scram_rejects_bad_server_verifier
    cfg = base_config
    cfg.password = "secret-password"
    client = Scratchbird::Client.new(cfg)
    attachment = ("b" * 16).b
    step = 0
    client_first = nil
    server_first = nil
    auth_request_payload = build_auth_request_payload(Scratchbird::Protocol::AUTH_SCRAM_SHA256)
    auth_continue_builder = method(:build_auth_continue_payload)
    auth_ok_builder = method(:build_auth_ok_payload)

    client.define_singleton_method(:send_message) do |type, payload, _flags, _force_zero|
      if type == Scratchbird::Protocol::MSG_AUTH_RESPONSE && client_first.nil?
        client_first = payload
      end
      true
    end

    client.define_singleton_method(:recv_message) do
      step += 1
      case step
      when 1
        [Scratchbird::Protocol::MSG_AUTH_REQUEST, 0, auth_request_payload, 0, attachment, 0]
      when 2
        nonce = client_first.to_s.split("r=", 2).last
        salt = Base64.strict_encode64("testsalt")
        server_first = "r=#{nonce}server,s=#{salt},i=4096"
        [Scratchbird::Protocol::MSG_AUTH_CONTINUE, 0, auth_continue_builder.call(server_first), 0, attachment, 0]
      when 3
        [Scratchbird::Protocol::MSG_AUTH_OK, 0, auth_ok_builder.call("v=invalid-signature"), 0, attachment, 1]
      else
        raise "unexpected recv step #{step}"
      end
    end

    err = assert_raises(RuntimeError) { client.send(:handshake) }
    assert_equal "SCRAM server signature mismatch", err.message
  end

  def test_handshake_startup_includes_auth_plugin_and_pinning_params
    cfg = base_config
    cfg.auth_method_id = "scratchbird.auth.proxy_assertion"
    cfg.auth_method_payload = "opaque"
    cfg.auth_payload_json = "{\"subject\":\"alice\"}"
    cfg.auth_payload_b64 = "YWJj"
    cfg.auth_provider_profile = "corp_primary"
    cfg.auth_required_methods = "SCRAM_SHA_256,TOKEN"
    cfg.auth_forbidden_methods = "MD5"
    cfg.auth_require_channel_binding = true
    cfg.workload_identity_token = "jwt-token"
    cfg.proxy_principal_assertion = "signed-assertion"
    client = Scratchbird::Client.new(cfg)
    startup_payload = nil

    client.define_singleton_method(:send_message) do |type, payload, _flags, _force_zero|
      if type == Scratchbird::Protocol::MSG_STARTUP
        startup_payload = payload
        raise "startup_sent"
      end
      1
    end

    err = assert_raises(RuntimeError) { client.send(:handshake) }
    assert_equal "startup_sent", err.message
    startup_text = startup_payload.to_s
    assert_includes startup_text, "client_flags\000256\000"
    assert_includes startup_text, "auth_method_id\000scratchbird.auth.proxy_assertion\000"
    assert_includes startup_text, "auth_method_payload\000opaque\000"
    assert_includes startup_text, "auth_payload_json\000{\"subject\":\"alice\"}\000"
    assert_includes startup_text, "auth_payload_b64\000YWJj\000"
    assert_includes startup_text, "auth_provider_profile\000corp_primary\000"
    assert_includes startup_text, "auth_required_methods\000SCRAM_SHA_256,TOKEN\000"
    assert_includes startup_text, "auth_forbidden_methods\000MD5\000"
    assert_includes startup_text, "auth_require_channel_binding\0001\000"
    assert_includes startup_text, "workload_identity_token\000jwt-token\000"
    assert_includes startup_text, "proxy_principal_assertion\000signed-assertion\000"
  end

  def test_handshake_rejects_invalid_auth_method_namespace
    cfg = base_config
    cfg.auth_method_id = "invalid.namespace"
    client = Scratchbird::Client.new(cfg)

    err = assert_raises(Scratchbird::AuthError) { client.send(:handshake) }
    assert_equal "invalid auth_method_id namespace", err.message
  end

  def test_protocol_parse_auth_continue_round_trip
    payload = [Scratchbird::Protocol::AUTH_SCRAM_SHA256, 2, 0, 0].pack("C4")
    payload << [5].pack("V")
    payload << "nonce"

    method, stage, data = Scratchbird::Protocol.parse_auth_continue(payload)
    assert_equal Scratchbird::Protocol::AUTH_SCRAM_SHA256, method
    assert_equal 2, stage
    assert_equal "nonce", data
  end

  def test_protocol_parse_auth_continue_rejects_truncated_payload
    payload = [Scratchbird::Protocol::AUTH_SCRAM_SHA256, 1, 0, 0].pack("C4")
    payload << [8].pack("V")
    payload << "tiny"

    err = assert_raises(RuntimeError) { Scratchbird::Protocol.parse_auth_continue(payload) }
    assert_equal "Auth continue truncated", err.message
  end

  private

  def base_config
    cfg = Scratchbird::Config.new
    cfg.user = "test_user"
    cfg.database = "test_db"
    cfg
  end

  def manager_auth_error_payload(message)
    [1].pack("C") + [0].pack("V") + message.to_s.b.ljust(256, "\0")
  end

  def manager_auth_success_payload
    [0].pack("C") + [0].pack("V") + ("\0" * 256)
  end

  def manager_connect_success_payload
    "\0".b + ("\0" * (1 + 2 + 2 + 16 + 64 + 32 - 1))
  end

  def build_auth_request_payload(method)
    [method, 0, 0, 0].pack("C4")
  end

  def build_auth_continue_payload(data, method = Scratchbird::Protocol::AUTH_SCRAM_SHA256)
    [method, 1, 0, 0].pack("C4") + [data.bytesize].pack("V") + data
  end

  def build_auth_ok_payload(server_info)
    session_id = ("s" * 16).b
    session_id + [server_info.bytesize].pack("V") + server_info
  end

  def build_ready_payload(status, txn_id)
    [status].pack("C") + "\0\0\0" + [txn_id, 0].pack("Q<Q<")
  end

  def compute_scram_server_verifier(password:, client_first:, server_first:, client_final:, digest: "sha256")
    client_first_bare = client_first.to_s.sub(/\An,,/, "")
    client_final_without_proof = client_final.to_s.split(",p=", 2).first
    salt_b64 = server_first.split(",").find { |part| part.start_with?("s=") }.to_s.sub(/\As=/, "")
    iterations = server_first.split(",").find { |part| part.start_with?("i=") }.to_s.sub(/\Ai=/, "").to_i
    salt = Base64.decode64(salt_b64)
    digest_length = OpenSSL::Digest.new(digest).digest_length
    salted = OpenSSL::PKCS5.pbkdf2_hmac(password, salt, iterations, digest_length, digest)
    server_key = OpenSSL::HMAC.digest(digest, salted, "Server Key")
    auth_message = "#{client_first_bare},#{server_first},#{client_final_without_proof}"
    Base64.strict_encode64(OpenSSL::HMAC.digest(digest, server_key, auth_message))
  end
end
