# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBirdAuthBootstrapContractTest do
  use ExUnit.Case

  alias ScratchBird.{Connection, Protocol}

  @manager_protocol_magic 0x42444253
  @manager_protocol_version 0x0101
  @manager_header_size 12
  @mcp_msg_status_response 0x64
  @mcp_msg_hello 0x65

  test "probe_auth_surface reports direct SCRAM_SHA_512" do
    server =
      start_loopback_server(fn socket ->
        {:ok, msg} = recv_protocol_message(socket)
        assert msg.type == Protocol.message_type(:startup)

        send_protocol_message(
          socket,
          Protocol.message_type(:auth_request),
          auth_request_payload(4)
        )
      end)

    {:ok, probe} =
      ScratchBird.probe_auth_surface(
        host: "127.0.0.1",
        port: server.port,
        database: "db",
        user: "user",
        sslmode: "disable"
      )

    assert probe.reachable
    assert probe.ingress_mode == "direct"
    assert probe.required_method == "SCRAM_SHA_512"
    assert probe.required_plugin_method_id == "scratchbird.auth.scram_sha_512"
    assert length(probe.admitted_methods) == 1
    [method] = probe.admitted_methods
    assert method.wire_method == "SCRAM_SHA_512"
    assert method.executable_locally
    assert probe.additional_continuation_possible
    await_loopback_server(server)
  end

  test "probe_auth_surface reports manager TOKEN ingress" do
    server =
      start_loopback_server(fn socket ->
        {:ok, msg_type, _payload} = recv_manager_frame(socket)
        assert msg_type == @mcp_msg_hello
        send_manager_frame(socket, @mcp_msg_status_response, <<>>)
      end)

    {:ok, probe} =
      ScratchBird.probe_auth_surface(
        host: "127.0.0.1",
        port: server.port,
        front_door_mode: "manager_proxy",
        database: "db",
        user: "admin",
        sslmode: "disable"
      )

    assert probe.reachable
    assert probe.ingress_mode == "manager_proxy"
    assert probe.required_method == "TOKEN"
    assert probe.required_plugin_method_id == "scratchbird.auth.authkey_token"
    assert length(probe.admitted_methods) == 1
    [method] = probe.admitted_methods
    assert method.wire_method == "TOKEN"
    assert method.executable_locally
    assert probe.additional_continuation_possible
    await_loopback_server(server)
  end

  test "connect tracks resolved SCRAM_SHA_512 auth context" do
    server =
      start_loopback_server(fn socket ->
        {:ok, msg} = recv_protocol_message(socket)
        assert msg.type == Protocol.message_type(:startup)

        send_protocol_message(
          socket,
          Protocol.message_type(:auth_request),
          auth_request_payload(4)
        )

        {:ok, first_response} = recv_protocol_message(socket)
        assert first_response.type == Protocol.message_type(:auth_response)
        client_first = first_response.payload |> IO.iodata_to_binary() |> to_string()
        client_nonce = extract_scram_nonce(client_first)
        server_first = "r=#{client_nonce}server,s=#{Base.encode64("elixir-salt")},i=4096"

        send_protocol_message(
          socket,
          Protocol.message_type(:auth_continue),
          auth_continue_payload(4, 0, server_first)
        )

        {:ok, final_response} = recv_protocol_message(socket)
        assert final_response.type == Protocol.message_type(:auth_response)
        assert byte_size(final_response.payload) > 0

        send_protocol_message(
          socket,
          Protocol.message_type(:auth_ok),
          auth_ok_payload(""),
          session_id()
        )

        send_protocol_message(
          socket,
          Protocol.message_type(:ready),
          ready_payload(),
          session_id()
        )
      end)

    {:ok, conn} =
      Connection.connect(
        host: "127.0.0.1",
        port: server.port,
        database: "db",
        user: "user",
        password: "secret",
        sslmode: "disable"
      )

    context = ScratchBird.get_resolved_auth_context(conn)
    assert context.ingress_mode == "direct"
    assert context.resolved_auth_method == "SCRAM_SHA_512"
    assert context.resolved_auth_plugin_id == "scratchbird.auth.scram_sha_512"
    assert context.manager_authenticated == false
    assert context.attached == true

    Connection.close(conn)
    await_loopback_server(server)
  end

  test "connect tracks resolved TOKEN auth context" do
    server =
      start_loopback_server(fn socket ->
        {:ok, msg} = recv_protocol_message(socket)
        assert msg.type == Protocol.message_type(:startup)

        send_protocol_message(
          socket,
          Protocol.message_type(:auth_request),
          auth_request_payload(5)
        )

        {:ok, auth_response} = recv_protocol_message(socket)
        assert auth_response.type == Protocol.message_type(:auth_response)
        assert auth_response.payload == "token-123"

        send_protocol_message(
          socket,
          Protocol.message_type(:auth_ok),
          auth_ok_payload(""),
          session_id()
        )

        send_protocol_message(
          socket,
          Protocol.message_type(:ready),
          ready_payload(),
          session_id()
        )
      end)

    {:ok, conn} =
      Connection.connect(
        host: "127.0.0.1",
        port: server.port,
        database: "db",
        user: "user",
        sslmode: "disable",
        auth_token: "token-123"
      )

    context = ScratchBird.get_resolved_auth_context(conn)
    assert context.resolved_auth_method == "TOKEN"
    assert context.resolved_auth_plugin_id == "scratchbird.auth.authkey_token"
    assert context.attached == true

    Connection.close(conn)
    await_loopback_server(server)
  end

  test "connect fails closed for PEER auth" do
    server =
      start_loopback_server(fn socket ->
        {:ok, msg} = recv_protocol_message(socket)
        assert msg.type == Protocol.message_type(:startup)

        send_protocol_message(
          socket,
          Protocol.message_type(:auth_request),
          auth_request_payload(6)
        )
      end)

    assert_raise RuntimeError, ~r/requires external broker support/, fn ->
      Connection.connect(
        host: "127.0.0.1",
        port: server.port,
        database: "db",
        user: "user",
        sslmode: "disable"
      )
    end

    await_loopback_server(server)
  end

  defp start_loopback_server(handler) do
    {:ok, listener} =
      :gen_tcp.listen(0, [
        :binary,
        packet: :raw,
        active: false,
        reuseaddr: true,
        ip: {127, 0, 0, 1}
      ])

    {:ok, port} = :inet.port(listener)

    task =
      Task.async(fn ->
        {:ok, socket} = :gen_tcp.accept(listener)

        try do
          handler.(socket)
          :gen_tcp.close(socket)
          :gen_tcp.close(listener)
          :ok
        rescue
          error ->
            :gen_tcp.close(socket)
            :gen_tcp.close(listener)
            reraise(error, __STACKTRACE__)
        catch
          kind, reason ->
            :gen_tcp.close(socket)
            :gen_tcp.close(listener)
            raise "loopback server caught #{inspect(kind)}: #{inspect(reason)}"
        end
      end)

    %{port: port, task: task}
  end

  defp await_loopback_server(server) do
    Task.await(server.task, 1_000)
  end

  defp recv_protocol_message(socket) do
    with {:ok, header_bin} <- :gen_tcp.recv(socket, Protocol.header_size(), 1_000),
         {:ok, header} <- Protocol.decode_header(header_bin),
         {:ok, payload} <- :gen_tcp.recv(socket, header.length, 1_000) do
      {:ok, Map.put(header, :payload, payload)}
    end
  end

  defp send_protocol_message(socket, type, payload, attachment_id \\ <<0::128>>) do
    encoded =
      Protocol.encode_message(
        %{
          type: type,
          flags: 0,
          length: byte_size(payload),
          sequence: 0,
          attachment_id: attachment_id,
          txn_id: 0
        },
        payload
      )

    :ok = :gen_tcp.send(socket, encoded)
  end

  defp recv_manager_frame(socket) do
    with {:ok, header} <- :gen_tcp.recv(socket, @manager_header_size, 1_000),
         {:ok, msg_type, payload_len} <- parse_manager_header(header),
         {:ok, payload} <- :gen_tcp.recv(socket, payload_len, 1_000) do
      {:ok, msg_type, payload}
    end
  end

  defp send_manager_frame(socket, msg_type, payload) do
    frame =
      <<@manager_protocol_magic::little-32, @manager_protocol_version::little-16, msg_type::8,
        0::8, byte_size(payload)::little-32, payload::binary>>

    :ok = :gen_tcp.send(socket, frame)
  end

  defp parse_manager_header(
         <<@manager_protocol_magic::little-32, @manager_protocol_version::little-16, msg_type::8,
           _::8, payload_len::little-32>>
       ) do
    {:ok, msg_type, payload_len}
  end

  defp parse_manager_header(_), do: {:error, :invalid_manager_header}

  defp auth_request_payload(method), do: <<method::8, 0::24>>

  defp auth_continue_payload(method, stage, value) do
    data = IO.iodata_to_binary(value)
    <<method::8, stage::8, 0::16, byte_size(data)::little-32, data::binary>>
  end

  defp auth_ok_payload(info) do
    info = IO.iodata_to_binary(info)
    <<0::128, byte_size(info)::little-32, info::binary>>
  end

  defp ready_payload, do: <<0::8, 0::24, 0::little-64>>

  defp session_id,
    do:
      <<0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD,
        0xEF>>

  defp extract_scram_nonce("n,," <> bare) do
    bare
    |> String.split(",", trim: true)
    |> Enum.find_value(fn part ->
      case String.split(part, "=", parts: 2) do
        ["r", value] -> value
        _ -> nil
      end
    end)
  end

  defp extract_scram_nonce(value) do
    flunk("unable to extract SCRAM nonce from #{inspect(value)}")
  end
end
