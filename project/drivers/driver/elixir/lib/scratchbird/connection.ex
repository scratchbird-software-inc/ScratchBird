# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.Connection do
  @moduledoc false

  alias ScratchBird.{AuthBootstrap, Config, Protocol, Scram, Types, CircuitBreaker, Keepalive, LeakDetector, Telemetry}
  import Bitwise

  @manager_protocol_magic 0x42444253
  @manager_protocol_version 0x0101
  @manager_header_size 12
  @manager_max_payload_size 16 * 1024 * 1024

  @mcp_protocol_version 0x0100
  @mcp_msg_connect_response 0x02
  @mcp_msg_auth_challenge 0x12
  @mcp_msg_auth_response 0x11
  @mcp_msg_status_response 0x64
  @mcp_msg_hello 0x65
  @mcp_msg_auth_start 0x66
  @mcp_msg_auth_continue 0x67
  @mcp_msg_db_connect 0x69
  @mcp_auth_method_token 4

  @msg_negotiate_version Protocol.message_type(:negotiate_version)
  @msg_auth_request Protocol.message_type(:auth_request)
  @msg_auth_continue Protocol.message_type(:auth_continue)
  @msg_auth_ok Protocol.message_type(:auth_ok)
  @msg_ready Protocol.message_type(:ready)
  @msg_parameter_status Protocol.message_type(:parameter_status)
  @msg_parameter_description Protocol.message_type(:parameter_description)
  @msg_error Protocol.message_type(:error)
  @msg_pong Protocol.message_type(:pong)
  @msg_notification Protocol.message_type(:notification)
  @msg_query_plan Protocol.message_type(:query_plan)
  @msg_sblr_compiled Protocol.message_type(:sblr_compiled)
  @msg_row_description Protocol.message_type(:row_description)
  @msg_data_row Protocol.message_type(:data_row)
  @msg_command_complete Protocol.message_type(:command_complete)
  @msg_txn_status Protocol.message_type(:txn_status)

  defstruct [
    :socket,
    :transport,
    :config,
    sequence: 0,
    attachment_id: <<0::128>>,
    txn_id: 0,
    runtime_txn_active: false,
    runtime_boundary_seen: false,
    params: %{},
    authed: false,
    resolved_auth_context: AuthBootstrap.default_resolved_auth_context("direct"),
    last_query_sequence: 0,
    notification_handlers: [],
    last_plan: nil,
    last_sblr: nil,
    connection_id: nil,
    circuit_breaker: nil,
    telemetry: nil,
    keepalive_tracker: nil,
    leak_guard: nil
  ]

  def connect(opts) do
    config = Config.from_opts(opts)
    # This lane does not perform transparent in-place reconnect on an existing
    # state struct. Replacement sessions are always created by a fresh
    # connect/1 handshake, so attachment/transaction identity is re-seeded from
    # engine truth rather than resurrected from abandoned local state.
    state = %__MODULE__{
      config: config,
      resolved_auth_context: AuthBootstrap.default_resolved_auth_context(config[:front_door_mode] || "direct")
    }

    with :ok <- validate_config(config),
         {:ok, socket, transport} <- open_socket(config),
         {:ok, state} <- maybe_perform_manager_connect(%{state | socket: socket, transport: transport}),
         {:ok, state} <- handshake(state) do
      {:ok, start_resilience(state)}
    end
  end

  def probe_auth_surface(opts) do
    config = Config.from_opts(opts)

    state = %__MODULE__{
      config: config,
      resolved_auth_context: AuthBootstrap.default_resolved_auth_context(config[:front_door_mode] || "direct")
    }

    with :ok <- validate_config(config),
         {:ok, socket, transport} <- open_socket(config) do
      state = %{state | socket: socket, transport: transport}

      try do
        if normalize_front_door_mode(config[:front_door_mode] || "direct") == {:ok, "manager_proxy"} do
          probe_manager_auth_surface(state)
        else
          probe_direct_auth_surface(state)
        end
      after
        close(state)
      end
    end
  end

  def get_resolved_auth_context(%__MODULE__{} = state) do
    state.resolved_auth_context
  end

  def close(%__MODULE__{transport: :ssl, socket: socket} = state) do
    _ = stop_resilience(state)
    :ssl.close(socket)
  end

  def close(%__MODULE__{transport: :tcp, socket: socket} = state) do
    _ = stop_resilience(state)
    :gen_tcp.close(socket)
  end

  defp validate_config(config) do
    sslmode = normalize_ssl_mode(config[:sslmode] || "require")
    protocol = (config[:protocol] || "native") |> to_string() |> String.trim() |> String.downcase()
    front_door_mode = normalize_front_door_mode(config[:front_door_mode] || "direct")

    cond do
      protocol not in ["", "native", "scratchbird", "scratchbird-native", "scratchbird_native"] ->
        {:error, "Only protocol=native is supported; connect to the native parser listener/port."}
      front_door_mode == :error ->
        {:error, "front_door_mode must be direct or manager_proxy."}
      sslmode == :error ->
        {:error, "Unsupported sslmode value"}
      true ->
        :ok
    end
  end

  def query(state, sql, params) when is_list(params) do
    with_resilience(state, "query", sql, fn state ->
      if params == [] do
        send_simple_query(state, sql)
      else
        send_extended_query(state, sql, params)
      end
    end)
  end

  def supports_prepared_transactions, do: true
  def supports_dormant_reattach, do: false
  def supports_portal_resume, do: false

  def build_prepared_transaction_sql(verb, global_transaction_id) do
    trimmed =
      global_transaction_id
      |> to_string()
      |> String.trim()

    if trimmed == "" do
      {:error, %{message: "Global transaction id is required", sqlstate: "42601"}}
    else
      {:ok, "#{verb} #{quote_string_literal(trimmed)}"}
    end
  end

  def prepare_transaction(state, global_transaction_id) do
    case build_prepared_transaction_sql("PREPARE TRANSACTION", global_transaction_id) do
      {:ok, sql} -> query(state, sql, [])
      {:error, reason} -> {:error, reason, state}
    end
  end

  def commit_prepared(state, global_transaction_id) do
    case build_prepared_transaction_sql("COMMIT PREPARED", global_transaction_id) do
      {:ok, sql} -> query(state, sql, [])
      {:error, reason} -> {:error, reason, state}
    end
  end

  def rollback_prepared(state, global_transaction_id) do
    case build_prepared_transaction_sql("ROLLBACK PREPARED", global_transaction_id) do
      {:ok, sql} -> query(state, sql, [])
      {:error, reason} -> {:error, reason, state}
    end
  end

  def detach_to_dormant(state) do
    {:error,
     %{message: "Dormant detach is not supported by the current public front door", sqlstate: "0A000"},
     state}
  end

  def reattach_dormant(state, dormant_id, auth_token \\ nil) do
    _ = dormant_id
    _ = auth_token

    {:error,
     %{message: "Dormant reattach is not supported by the current public front door", sqlstate: "0A000"},
     state}
  end

  def begin(state, opts \\ %{}) do
    with_resilience(state, "txn_begin", nil, fn state ->
      opts = Enum.into(opts, %{})
      cond do
        can_adopt_fresh_native_boundary?(state, opts) ->
          {:ok, state}

        transaction_active?(state) and state.txn_id == 0 ->
          {:error,
           %{
             message: "non-default begin options cannot adopt an already-active fresh native boundary",
             sqlstate: "0A000"
           }, state}

        transaction_active?(state) ->
          {:error, %{message: "transaction already active", sqlstate: "25001"}, state}

        true ->
          read_committed_mode = Map.get(opts, :read_committed_mode)
          flags = 0
          isolation_level = Map.get(opts, :isolation_level, Protocol.isolation(:read_committed))
          flags = if Map.has_key?(opts, :isolation_level), do: flags ||| Protocol.txn_flag(:has_isolation), else: flags
          {flags, isolation_level} =
            if is_nil(read_committed_mode) do
              {flags, isolation_level}
            else
              if Map.has_key?(opts, :isolation_level) &&
                   isolation_level not in [
                     Protocol.isolation(:read_uncommitted),
                     Protocol.isolation(:read_committed)
                   ] do
                raise ArgumentError, "read_committed_mode requires a READ COMMITTED isolation alias"
              end

              if Map.has_key?(opts, :isolation_level) do
                {flags ||| Protocol.txn_flag(:has_read_committed_mode), isolation_level}
              else
                {
                  flags ||| Protocol.txn_flag(:has_read_committed_mode) ||| Protocol.txn_flag(:has_isolation),
                  Protocol.isolation(:read_committed)
                }
              end
            end
          flags = if Map.has_key?(opts, :access_mode), do: flags ||| Protocol.txn_flag(:has_access), else: flags
          flags = if Map.has_key?(opts, :deferrable), do: flags ||| Protocol.txn_flag(:has_deferrable), else: flags
          flags = if Map.has_key?(opts, :wait), do: flags ||| Protocol.txn_flag(:has_wait), else: flags
          flags = if Map.has_key?(opts, :timeout_ms), do: flags ||| Protocol.txn_flag(:has_timeout), else: flags
          flags = if Map.has_key?(opts, :autocommit_mode), do: flags ||| Protocol.txn_flag(:has_autocommit), else: flags
          payload =
            Protocol.build_txn_begin_payload(
              flags,
              Map.get(opts, :conflict_action, 0),
              Map.get(opts, :autocommit_mode, 0),
              isolation_level,
              Map.get(opts, :access_mode, 0),
              if(Map.get(opts, :deferrable), do: 1, else: 0),
              if(Map.get(opts, :wait), do: 1, else: 0),
              Map.get(opts, :timeout_ms, 0),
              Map.get(opts, :read_committed_mode, Protocol.read_committed_mode(:default))
            )
          state = send_message(state, Protocol.message_type(:txn_begin), payload, 0)
          drain_until_ready(state)
      end
    end)
  end

  def commit(state, flags \\ 0) do
    with_resilience(state, "txn_commit", nil, fn state ->
      payload = Protocol.build_txn_commit_payload(flags)
      state = send_message(state, Protocol.message_type(:txn_commit), payload, 0)
      drain_until_ready(state)
    end)
  end

  def rollback(state, flags \\ 0) do
    with_resilience(state, "txn_rollback", nil, fn state ->
      payload = Protocol.build_txn_rollback_payload(flags)
      state = send_message(state, Protocol.message_type(:txn_rollback), payload, 0)
      drain_until_ready(state)
    end)
  end

  def savepoint(state, name) do
    with_resilience(state, "txn_savepoint", nil, fn state ->
      payload = Protocol.build_txn_savepoint_payload(name)
      state = send_message(state, Protocol.message_type(:txn_savepoint), payload, 0)
      drain_until_ready(state)
    end)
  end

  def release_savepoint(state, name) do
    with_resilience(state, "txn_release", nil, fn state ->
      payload = Protocol.build_txn_release_payload(name)
      state = send_message(state, Protocol.message_type(:txn_release), payload, 0)
      drain_until_ready(state)
    end)
  end

  def rollback_to_savepoint(state, name) do
    with_resilience(state, "txn_rollback_to", nil, fn state ->
      payload = Protocol.build_txn_rollback_to_payload(name)
      state = send_message(state, Protocol.message_type(:txn_rollback_to), payload, 0)
      drain_until_ready(state)
    end)
  end

  def set_option(state, name, value) do
    with_resilience(state, "set_option", nil, fn state ->
      payload = Protocol.build_set_option_payload(name, value)
      state = send_message(state, Protocol.message_type(:set_option), payload, 0)
      drain_until_ready(state)
    end)
  end

  def ping(state) do
    state = send_message(state, Protocol.message_type(:ping), <<>>, 0)
    case recv_message(state) do
      {:ok, msg} ->
        case handle_async(state, msg) do
          {:handled, new_state} -> ping(new_state)
          {:ok, new_state} ->
            case msg.type do
              @msg_pong -> {:ok, new_state}
              @msg_ready ->
                {:ok, status, txn_id} = Protocol.parse_ready(msg.payload)
                {:ok, apply_runtime_ready_state(new_state, status, txn_id)}
              @msg_error -> {:error, Protocol.parse_error(msg.payload), new_state}
              _ -> ping(new_state)
            end
        end
      error -> error
    end
  end

  def terminate(state) do
    _ = send_message(state, Protocol.message_type(:terminate), <<>>, 0)
    close(state)
  end

  def subscribe(state, channel, opts \\ %{}) do
    payload = Protocol.build_subscribe_payload(
      Map.get(opts, :subscribe_type, Protocol.subscribe_type(:channel)),
      channel,
      Map.get(opts, :filter_expr, "")
    )
    state = send_message(state, Protocol.message_type(:subscribe), payload, 0)
    drain_until_ready(state)
  end

  def unsubscribe(state, channel) do
    payload = Protocol.build_unsubscribe_payload(channel)
    state = send_message(state, Protocol.message_type(:unsubscribe), payload, 0)
    drain_until_ready(state)
  end

  def execute_sblr(state, sblr_hash, sblr_bytecode, params \\ []) do
    with_resilience(state, "sblr_execute", nil, fn state ->
      {param_values, _param_types} =
        params
        |> Enum.map(&Types.encode_param/1)
        |> Enum.map(fn {param, oid} -> {param, oid} end)
        |> Enum.unzip()
      payload = Protocol.build_sblr_execute_payload(sblr_hash, sblr_bytecode, param_values)
      state = %{state | last_plan: nil, last_sblr: nil}
      sequence = state.sequence
      state = send_message(state, Protocol.message_type(:sblr_execute), payload, 0)
      state = %{state | last_query_sequence: sequence}
      state = send_message(state, Protocol.message_type(:sync), <<>>, 0)
      collect_results(state, [])
    end)
  end

  def stream_control(state, control_type, window_size \\ 0, timeout_ms \\ 0) do
    payload = Protocol.build_stream_control_payload(control_type, window_size, timeout_ms)
    state = send_message(state, Protocol.message_type(:stream_control), payload, 0)
    {:ok, state}
  end

  def attach_create(state, emulation_mode, db_name) do
    with_resilience(state, "attach_create", nil, fn state ->
      payload = Protocol.build_attach_create_payload(emulation_mode, db_name)
      state = send_message(state, Protocol.message_type(:attach_create), payload, 0)
      drain_until_ready(state)
    end)
  end

  def attach_detach(state) do
    with_resilience(state, "attach_detach", nil, fn state ->
      state = send_message(state, Protocol.message_type(:attach_detach), <<>>, 0)
      drain_until_ready(state)
    end)
  end

  def attach_list(state) do
    with_resilience(state, "attach_list", nil, fn state ->
      state = send_message(state, Protocol.message_type(:attach_list), <<>>, 0)
      state = send_message(state, Protocol.message_type(:sync), <<>>, 0)
      collect_results(state, [])
    end)
  end

  def cancel(state) do
    payload = Protocol.build_cancel_payload(0, state.last_query_sequence)
    _ = send_message(state, Protocol.message_type(:cancel), payload, Protocol.flag(:urgent))
    {:ok, state}
  end

  def on_notification(state, handler) when is_function(handler, 1) do
    {:ok, %{state | notification_handlers: state.notification_handlers ++ [handler]}}
  end

  def last_query_plan(state), do: state.last_plan
  def last_sblr_compiled(state), do: state.last_sblr

  defp open_socket(config) do
    host_name = to_string(config[:host] || "localhost")
    host = to_charlist(host_name)
    port = config[:port] || 3092
    sslmode = normalize_ssl_mode(config[:sslmode] || "require")
    connect_timeout = max(to_int(config[:connect_timeout] || 5000, 5000), 1)

    opts = [
      mode: :binary,
      packet: :raw,
      active: false
    ]

    if sslmode == {:ok, "disable"} do
      case :gen_tcp.connect(host, port, opts, connect_timeout) do
        {:ok, socket} -> {:ok, socket, :tcp}
        {:error, reason} -> {:error, "TCP connect failed: #{inspect(reason)}"}
      end
    else
      {:ok, sslmode_value} = sslmode

      verify_mode =
        if sslmode_value in ["verify-ca", "verify-full"] do
          :verify_peer
        else
          :verify_none
        end

      ssl_opts = [
        verify: verify_mode,
        versions: [:"tlsv1.3"],
        server_name_indication: host,
        reuse_sessions: false
      ]

      ssl_opts =
        if sslmode_value == "verify-full" do
          ssl_opts ++ [customize_hostname_check: [match_fun: :public_key.pkix_verify_hostname_match_fun(:https)]]
        else
          ssl_opts
        end

      ssl_opts =
        cond do
          config[:sslrootcert] && to_string(config[:sslrootcert]) != "" ->
            ssl_opts ++ [cacertfile: to_string(config[:sslrootcert])]

          verify_mode == :verify_peer ->
            ssl_opts ++ [cacerts: :public_key.cacerts_get()]

          true ->
            ssl_opts
        end

      ssl_opts =
        if config[:sslcert] && to_string(config[:sslcert]) != "" do
          ssl_opts ++ [certfile: to_string(config[:sslcert])]
        else
          ssl_opts
        end

      ssl_opts =
        if config[:sslkey] && to_string(config[:sslkey]) != "" do
          ssl_opts ++ [keyfile: to_string(config[:sslkey])]
        else
          ssl_opts
        end

      ssl_opts =
        if config[:sslpassword] && to_string(config[:sslpassword]) != "" do
          ssl_opts ++ [password: to_string(config[:sslpassword])]
        else
          ssl_opts
        end

      case :ssl.connect(host, port, ssl_opts ++ opts, connect_timeout) do
        {:ok, socket} -> {:ok, socket, :ssl}
        err -> err
      end
    end
  end

  defp maybe_perform_manager_connect(state) do
    if normalize_front_door_mode(state.config[:front_door_mode] || "direct") == {:ok, "manager_proxy"} do
      perform_manager_connect(state)
    else
      {:ok, state}
    end
  end

  defp perform_manager_connect(state) do
    token = to_string(state.config[:manager_auth_token] || "")

    if String.trim(token) == "" do
      {:error, "manager_proxy mode requires manager_auth_token"}
    else
      manager_user =
        state.config[:manager_username]
        |> blank_to_nil()
        |> fallback_blank(state.config[:user])
        |> fallback_blank("admin")

      manager_database =
        state.config[:manager_database]
        |> blank_to_nil()
        |> fallback_blank(state.config[:database] || "")
        |> to_string()

      manager_profile =
        state.config[:manager_connection_profile]
        |> blank_to_nil()
        |> fallback_blank("SBsql")
        |> to_string()

      manager_intent =
        state.config[:manager_client_intent]
        |> blank_to_nil()
        |> fallback_blank("SBsql")
        |> to_string()

      manager_flags = band(to_int(state.config[:manager_client_flags] || 0, 0), 0xFFFF)
      auth_fast_path = state.config[:manager_auth_fast_path] != false

      hello_payload = <<@mcp_protocol_version::little-16, manager_flags::little-16>>

      with :ok <- send_manager_frame(state, @mcp_msg_hello, hello_payload),
           {:ok, msg_type, _hello_status} <- recv_manager_frame(state),
           :ok <- ensure_manager_type(msg_type, @mcp_msg_status_response, "Expected MCP hello status response"),
           :ok <- manager_auth_exchange(state, manager_user, token, auth_fast_path),
           :ok <- manager_db_connect(state, manager_database, manager_profile, manager_intent) do
        {:ok,
         %{
           state
           | resolved_auth_context:
               %{state.resolved_auth_context | ingress_mode: "manager_proxy", manager_authenticated: true}
         }}
      end
    end
  end

  defp manager_auth_exchange(state, manager_user, token, auth_fast_path) do
    auth_start =
      if auth_fast_path do
        token_bytes = token
        IO.iodata_to_binary([
          manager_lpref(manager_user),
          <<@mcp_auth_method_token::8>>,
          <<byte_size(token_bytes)::little-32>>,
          token_bytes
        ])
      else
        IO.iodata_to_binary([
          manager_lpref(manager_user),
          <<@mcp_auth_method_token::8, 0::little-32>>
        ])
      end

    with :ok <- send_manager_frame(state, @mcp_msg_auth_start, auth_start),
         {:ok, msg_type, payload} <- recv_manager_frame(state),
         {:ok, msg_type, payload} <- maybe_continue_auth_challenge(state, msg_type, payload, token),
         :ok <- ensure_manager_type(msg_type, @mcp_msg_auth_response, "Expected MCP auth response"),
         :ok <- validate_auth_response(payload) do
      :ok
    end
  end

  defp maybe_continue_auth_challenge(state, msg_type, payload, token) do
    if msg_type == @mcp_msg_auth_challenge do
      token_bytes = token
      auth_continue = IO.iodata_to_binary([<<byte_size(token_bytes)::little-32>>, token_bytes])

      with :ok <- send_manager_frame(state, @mcp_msg_auth_continue, auth_continue),
           {:ok, next_type, next_payload} <- recv_manager_frame(state) do
        {:ok, next_type, next_payload}
      end
    else
      {:ok, msg_type, payload}
    end
  end

  defp validate_auth_response(payload) do
    if byte_size(payload) < 261 do
      {:error, "Truncated MCP auth response"}
    else
      <<status::8, _reserved::little-32, rest::binary>> = payload
      if status == 0 do
        :ok
      else
        <<err::binary-size(256), _tail::binary>> = rest
        err =
          err
          |> String.replace(~r/\x00+$/, "")
          |> String.trim()

        {:error, if(err == "", do: "MCP authentication failed", else: err)}
      end
    end
  end

  defp manager_db_connect(state, manager_database, manager_profile, manager_intent) do
    nonce = :crypto.strong_rand_bytes(16)

    payload =
      IO.iodata_to_binary([
        <<"MCP1">>,
        manager_lpref(manager_database),
        manager_lpref(manager_profile),
        manager_lpref(manager_intent),
        <<byte_size(nonce)::little-16>>,
        nonce
      ])

    with :ok <- send_manager_frame(state, @mcp_msg_db_connect, payload),
         {:ok, msg_type, connect_payload} <- recv_manager_frame(state),
         :ok <- ensure_manager_type(msg_type, @mcp_msg_connect_response, "Expected MCP connect response"),
         :ok <- validate_connect_response(connect_payload) do
      :ok
    end
  end

  defp validate_connect_response(payload) do
    minimum_size = 1 + 2 + 2 + 16 + 64 + 32

    if byte_size(payload) < minimum_size do
      {:error, "Truncated MCP connect response"}
    else
      <<status::8, _rest::binary>> = payload

      if status == 0 do
        :ok
      else
        err_offset = minimum_size

        case payload do
          <<_::binary-size(err_offset), err_len::little-32, rest::binary>> when byte_size(rest) >= err_len ->
            <<err::binary-size(err_len), _::binary>> = rest
            {:error, if(err == "", do: "MCP database connect failed", else: err)}

          _ ->
            {:error, "MCP database connect failed"}
        end
      end
    end
  end

  defp ensure_manager_type(actual, expected, message) do
    if actual == expected, do: :ok, else: {:error, message}
  end

  defp send_manager_frame(state, msg_type, payload) do
    payload = payload || <<>>

    frame =
      <<@manager_protocol_magic::little-32, @manager_protocol_version::little-16, msg_type::8, 0::8,
        byte_size(payload)::little-32, payload::binary>>

    case send_raw(state, frame) do
      :ok -> :ok
      {:error, reason} -> {:error, "manager frame write failed: #{inspect(reason)}"}
    end
  end

  defp recv_manager_frame(state) do
    with {:ok, header} <- recv_exact(state, @manager_header_size),
         {:ok, msg_type, payload_len} <- parse_manager_header(header),
         {:ok, payload} <- recv_exact(state, payload_len) do
      {:ok, msg_type, payload}
    end
  end

  defp parse_manager_header(
         <<@manager_protocol_magic::little-32, @manager_protocol_version::little-16, msg_type::8, _reserved::8,
           payload_len::little-32>>
       ) do
    if payload_len > @manager_max_payload_size do
      {:error, "manager payload too large"}
    else
      {:ok, msg_type, payload_len}
    end
  end

  defp parse_manager_header(<<magic::little-32, _::binary>>) when magic != @manager_protocol_magic,
    do: {:error, "manager frame magic mismatch"}

  defp parse_manager_header(_), do: {:error, "manager frame version mismatch"}

  defp manager_lpref(value) do
    bytes = to_string(value || "")
    <<byte_size(bytes)::little-32, bytes::binary>>
  end

  defp blank_to_nil(nil), do: nil

  defp blank_to_nil(value) do
    trimmed = value |> to_string() |> String.trim()
    if trimmed == "", do: nil, else: trimmed
  end

  defp fallback_blank(nil, fallback), do: fallback
  defp fallback_blank(value, _fallback), do: value

  defp normalize_front_door_mode(value) do
    normalized = value |> to_string() |> String.trim() |> String.downcase()

    case normalized do
      "" -> {:ok, "direct"}
      "direct" -> {:ok, "direct"}
      "manager_proxy" -> {:ok, "manager_proxy"}
      "manager-proxy" -> {:ok, "manager_proxy"}
      "managed" -> {:ok, "manager_proxy"}
      _ -> :error
    end
  end

  defp normalize_ssl_mode(value) do
    case value |> to_string() |> String.trim() |> String.downcase() do
      "verify_ca" -> {:ok, "verify-ca"}
      "verify_full" -> {:ok, "verify-full"}
      mode when mode in ["disable", "allow", "prefer", "require", "verify-ca", "verify-full"] -> {:ok, mode}
      _ -> :error
    end
  end

  defp to_int(value, _default) when is_integer(value), do: value

  defp to_int(value, default) do
    case Integer.parse(to_string(value)) do
      {parsed, _} -> parsed
      _ -> default
    end
  end

  defp build_startup_params(state) do
    params = %{
      "database" => state.config[:database] || "",
      "user" => state.config[:user] || "",
      "client_flags" => to_string(state.config[:connect_client_flags] || 0)
    }

    dormant_id = blank_to_nil(state.config[:dormant_id])
    dormant_token = blank_to_nil(state.config[:dormant_reattach_token])

    if !!dormant_id != !!dormant_token do
      raise ArgumentError, "dormant_id and dormant_reattach_token must be provided together"
    end

    params =
      if state.config[:role] do
        Map.put(params, "role", state.config[:role])
      else
        params
      end

    params =
      if state.config[:application_name] do
        Map.put(params, "application_name", state.config[:application_name])
      else
        params
      end

    params =
      if dormant_id && dormant_token do
        params
        |> Map.put("dormant_id", dormant_id)
        |> Map.put("dormant_reattach_token", dormant_token)
      else
        params
      end

    Protocol.apply_auth_plugin_selection(params, %{
      method_id: state.config[:auth_method_id],
      method_payload: state.config[:auth_method_payload],
      payload_json: state.config[:auth_payload_json],
      payload_b64: state.config[:auth_payload_b64],
      provider_profile: state.config[:auth_provider_profile],
      required_methods: state.config[:auth_required_methods],
      forbidden_methods: state.config[:auth_forbidden_methods],
      require_channel_binding: state.config[:auth_require_channel_binding] == true,
      workload_identity_token: state.config[:workload_identity_token],
      proxy_principal_assertion: state.config[:proxy_principal_assertion]
    })
  end

  defp probe_direct_auth_surface(state) do
    payload =
      Protocol.build_startup_payload(
        requested_features(state.config),
        build_startup_params(state)
      )

    state = send_message(state, Protocol.message_type(:startup), payload, 0)

    with {:ok, msg} <- recv_message(state) do
      case msg.type do
        @msg_negotiate_version ->
          probe_direct_auth_surface(state)

        @msg_auth_request ->
          {:ok, method, _data} = Protocol.parse_auth_request(msg.payload)
          method_surface =
            AuthBootstrap.describe_auth_method(method, state.config[:auth_method_id])

          {:ok,
           %AuthBootstrap.AuthProbeResult{
             reachable: true,
             ingress_mode: "direct",
             resolved_host: to_string(state.config[:host] || ""),
             resolved_port: state.config[:port] || 0,
             admitted_methods: if(method_surface, do: [method_surface], else: []),
             required_method: if(method_surface, do: method_surface.wire_method, else: nil),
             required_plugin_method_id:
               if(method_surface, do: method_surface.plugin_method_id, else: nil),
             allowed_transport_mask: nil,
             additional_continuation_possible:
               AuthBootstrap.additional_continuation_possible?(method)
           }}

        @msg_auth_ok ->
          {:ok,
           %AuthBootstrap.AuthProbeResult{
             reachable: true,
             ingress_mode: "direct",
             resolved_host: to_string(state.config[:host] || ""),
             resolved_port: state.config[:port] || 0,
             admitted_methods: [],
             required_method: nil,
             required_plugin_method_id: nil,
             allowed_transport_mask: nil,
             additional_continuation_possible: false
           }}

        @msg_ready ->
          {:ok,
           %AuthBootstrap.AuthProbeResult{
             reachable: true,
             ingress_mode: "direct",
             resolved_host: to_string(state.config[:host] || ""),
             resolved_port: state.config[:port] || 0,
             admitted_methods: [],
             required_method: nil,
             required_plugin_method_id: nil,
             allowed_transport_mask: nil,
             additional_continuation_possible: false
           }}

        @msg_error ->
          {:error, Protocol.parse_error(msg.payload)}

        _ ->
          probe_direct_auth_surface(state)
      end
    end
  end

  defp probe_manager_auth_surface(state) do
    manager_flags = band(to_int(state.config[:manager_client_flags] || 0, 0), 0xFFFF)
    hello_payload = <<@mcp_protocol_version::little-16, manager_flags::little-16>>

    with :ok <- send_manager_frame(state, @mcp_msg_hello, hello_payload),
         {:ok, msg_type, _payload} <- recv_manager_frame(state),
         :ok <- ensure_manager_type(msg_type, @mcp_msg_status_response, "Expected MCP hello status response") do
      method_surface =
        AuthBootstrap.describe_auth_method(Protocol.auth_method(:token), state.config[:auth_method_id])

      {:ok,
       %AuthBootstrap.AuthProbeResult{
         reachable: true,
         ingress_mode: "manager_proxy",
         resolved_host: to_string(state.config[:host] || ""),
         resolved_port: state.config[:port] || 0,
         admitted_methods: if(method_surface, do: [method_surface], else: []),
         required_method: if(method_surface, do: method_surface.wire_method, else: nil),
         required_plugin_method_id: if(method_surface, do: method_surface.plugin_method_id, else: nil),
         allowed_transport_mask: nil,
         additional_continuation_possible: true
       }}
    end
  end

  defp handshake(state) do
    payload =
      Protocol.build_startup_payload(
        requested_features(state.config),
        build_startup_params(state)
      )

    state = send_message(state, Protocol.message_type(:startup), payload, 0)

    loop_auth(state, nil)
  end

  defp loop_auth(state, scram) do
    with {:ok, msg} <- recv_message(state) do
      case msg.type do
        @msg_negotiate_version ->
          loop_auth(state, scram)

        @msg_auth_request ->
          {:ok, method, data} = Protocol.parse_auth_request(msg.payload)
          {state, scram} = handle_auth_request(state, method, data, scram)
          loop_auth(state, scram)

        @msg_auth_continue ->
          {:ok, method, _stage, data} = Protocol.parse_auth_continue(msg.payload)
          {state, scram} = handle_auth_continue(state, method, data, scram)
          loop_auth(state, scram)

        @msg_auth_ok ->
          {:ok, session_id, info} = Protocol.parse_auth_ok(msg.payload)
          attachment_id = if byte_size(session_id) == 16, do: session_id, else: msg.attachment_id
          state = %{state | attachment_id: attachment_id, authed: true}
          state = apply_runtime_txn_id(state, msg.txn_id)

          if scram && byte_size(info) > 0 do
            _ = Scram.verify_server_final(scram, to_string(info))
          end

          loop_auth(state, scram)

        @msg_parameter_status ->
          {:ok, name, value} = Protocol.parse_parameter_status(msg.payload)
          loop_auth(update_parameter_status(state, to_string(name), to_string(value)), scram)

        @msg_ready ->
          {:ok, status, txn_id} = Protocol.parse_ready(msg.payload)
          state = apply_runtime_ready_state(state, status, txn_id)
          state = %{state | resolved_auth_context: %{state.resolved_auth_context | attached: true}}
          state = apply_search_path(state)
          {:ok, state}

        @msg_error ->
          {:error, Protocol.parse_error(msg.payload)}

        _ ->
          loop_auth(state, scram)
      end
    end
  end

  defp handle_auth_request(state, method, _data, scram) do
    case method do
      0 -> {state, scram}

      1 ->
        password = state.config[:password] || ""
        state = update_resolved_auth_context(state, method, "PASSWORD")

        {send_message(state, Protocol.message_type(:auth_response), password, 0), scram}

      3 ->
        scram = scram || Scram.new(state.config[:user] || "", :sha256)
        {client_first, scram} = Scram.client_first(scram)
        state = update_resolved_auth_context(state, method, "SCRAM_SHA_256")

        {send_message(state, Protocol.message_type(:auth_response), client_first, 0), scram}

      4 ->
        scram = scram || Scram.new(state.config[:user] || "", :sha512)
        {client_first, scram} = Scram.client_first(scram)
        state = update_resolved_auth_context(state, method, "SCRAM_SHA_512")

        {send_message(state, Protocol.message_type(:auth_response), client_first, 0), scram}

      5 ->
        token_payload = AuthBootstrap.resolve_token_auth_payload(state.config)

        if is_binary(token_payload) and byte_size(token_payload) > 0 do
          state = update_resolved_auth_context(state, method, "TOKEN")

          {send_message(state, Protocol.message_type(:auth_response), token_payload, 0), scram}
        else
          raise "TOKEN auth requires auth_token or equivalent auth payload input"
        end

      6 ->
        raise "Admitted auth method PEER requires external broker support in this lane"

      2 ->
        raise "Admitted auth method MD5 is not executable locally in this lane"

      7 ->
        raise "Admitted auth method REATTACH is not executable locally in this lane"

      _ ->
        raise "Unsupported auth method"
    end
  end

  defp handle_auth_continue(state, method, data, scram) do
    case method do
      3 ->
        password = state.config[:password] || ""
        {:ok, client_final, scram} = Scram.handle_server_first(scram, password, data)
        {send_message(state, Protocol.message_type(:auth_response), client_final, 0), scram}

      4 ->
        password = state.config[:password] || ""
        {:ok, client_final, scram} = Scram.handle_server_first(scram, password, data)
        {send_message(state, Protocol.message_type(:auth_response), client_final, 0), scram}

      5 ->
        token_payload = AuthBootstrap.resolve_token_auth_payload(state.config)

        if is_binary(token_payload) and byte_size(token_payload) > 0 do
          state = update_resolved_auth_context(state, method, "TOKEN")

          {send_message(state, Protocol.message_type(:auth_response), token_payload, 0), scram}
        else
          raise "TOKEN auth requires auth_token or equivalent auth payload input"
        end

      6 ->
        raise "Admitted auth method PEER requires external broker support in this lane"

      _ ->
        {state, scram}
    end
  end

  defp send_simple_query(state, sql) do
    flags = if state.config[:binary_transfer], do: Protocol.query_flag(:binary_result), else: 0
    payload = Protocol.build_query_payload(sql, flags, 0, 0)
    state = %{state | last_plan: nil, last_sblr: nil}
    sequence = state.sequence
    state = send_message(state, Protocol.message_type(:query), payload, 0)
    state = %{state | last_query_sequence: sequence}
    collect_results(state, [])
  end

  defp apply_search_path(state) do
    schema = state.config[:search_path] || state.config[:schema]
    if is_binary(schema) and String.trim(schema) != "" and String.downcase(schema) not in ["public", "users.public"] do
      case send_simple_query(state, "SET SEARCH_PATH TO " <> schema) do
        {:ok, _result, new_state} -> new_state
        {:error, _reason, new_state} -> new_state
      end
    else
      state
    end
  end

  defp send_extended_query(state, sql, params) do
    sql = normalize_query_placeholders(sql, params)

    {param_values, param_types} =
      params
      |> Enum.map(&Types.encode_param/1)
      |> Enum.map(fn {param, oid} -> {param, oid} end)
      |> Enum.unzip()

    state = send_message(state, Protocol.message_type(:parse), Protocol.build_parse_payload("", sql, param_types), 0)
    case describe_statement(state) do
      {:ok, _count, state} ->
        state = send_message(state, Protocol.message_type(:bind), Protocol.build_bind_payload("", "", param_values, [1]), 0)
        state = %{state | last_plan: nil, last_sblr: nil}
        sequence = state.sequence
        state = send_message(state, Protocol.message_type(:execute), Protocol.build_execute_payload("", 0), 0)
        state = %{state | last_query_sequence: sequence}
        state = send_message(state, Protocol.message_type(:sync), <<>>, 0)
        collect_results(state, [])

      {:error, reason, state} ->
        {:error, reason, state}
    end
  end

  defp describe_statement(state) do
    state = send_message(state, Protocol.message_type(:describe), Protocol.build_describe_payload(?S, ""), 0)
    state = send_message(state, Protocol.message_type(:sync), <<>>, 0)
    describe_loop(state, -1)
  end

  defp describe_loop(state, param_count) do
    case recv_message(state) do
      {:ok, msg} ->
        case handle_async(state, msg) do
          {:handled, new_state} -> describe_loop(new_state, param_count)
          {:ok, new_state} ->
            case msg.type do
              @msg_parameter_description ->
                count = length(Protocol.parse_parameter_description(msg.payload))
                describe_loop(new_state, count)
              @msg_ready ->
                {:ok, status, txn_id} = Protocol.parse_ready(msg.payload)
                {:ok, param_count, apply_runtime_ready_state(new_state, status, txn_id)}
              @msg_error ->
                {:error, Protocol.parse_error(msg.payload), new_state}
              _ ->
                describe_loop(new_state, param_count)
            end
        end
      error -> error
    end
  end

  defp normalize_query_placeholders(sql, params) do
    if String.contains?(sql, "?") do
      {rewritten, count} =
        sql
        |> String.graphemes()
        |> Enum.reduce({[], 0, false}, fn ch, {acc, index, in_string} ->
          cond do
            ch == "'" ->
              {[ch | acc], index, not in_string}

            not in_string and ch == "?" ->
              {[Integer.to_string(index + 1), "$" | acc], index + 1, in_string}

            true ->
              {[ch | acc], index, in_string}
          end
        end)
        |> then(fn {acc, index, _in_string} -> {acc |> Enum.reverse() |> Enum.join(), index} end)

      if count != length(params) do
        raise ArgumentError, "parameter count mismatch"
      end

      rewritten
    else
      sql
    end
  end

  defp quote_string_literal(value) do
    escaped = String.replace(value, "'", "''")
    "'#{escaped}'"
  end

  defp handle_async(state, msg) do
    case msg.type do
      @msg_parameter_status ->
        {:ok, name, value} = Protocol.parse_parameter_status(msg.payload)
        new_state = update_parameter_status(state, to_string(name), to_string(value))
        {:handled, new_state}

      @msg_notification ->
        notice = Protocol.parse_notification(msg.payload)
        Enum.each(state.notification_handlers, fn handler -> handler.(notice) end)
        {:handled, state}

      @msg_query_plan ->
        plan = Protocol.parse_query_plan(msg.payload)
        {:handled, %{state | last_plan: plan}}

      @msg_sblr_compiled ->
        compiled = Protocol.parse_sblr_compiled(msg.payload)
        {:handled, %{state | last_sblr: compiled}}

      @msg_txn_status ->
        {:ok, status, txn_id} = Protocol.parse_txn_status(msg.payload)
        {:handled, apply_runtime_txn_status(state, status, txn_id)}

      _ ->
        {:ok, state}
    end
  end

  defp update_parameter_status(state, name, value) do
    state = %{state | params: Map.put(state.params, name, value)}
    state =
      if name == "attachment_id" do
        case parse_uuid_bytes(value) do
          {:ok, bytes} -> %{state | attachment_id: bytes}
          _ -> state
        end
      else
        state
      end
    if name == "current_txn_id" do
      case Integer.parse(value) do
        {txn_id, _} -> apply_runtime_txn_id(state, txn_id)
        _ -> state
      end
    else
      state
    end
  end

  defp apply_runtime_txn_id(state, txn_id) do
    txn_id = normalize_runtime_txn_id(txn_id)
    if txn_id > 0 do
      %{state | txn_id: txn_id, runtime_txn_active: true, runtime_boundary_seen: true}
    else
      %{state | txn_id: 0, runtime_boundary_seen: true}
    end
  end

  defp apply_runtime_ready_state(state, status, txn_id) do
    txn_id = normalize_runtime_txn_id(txn_id)
    if normalize_runtime_status(status) != 0 do
      # READY is authoritative for native session activity. The engine can
      # reopen a fresh MGA boundary while the public wire header still reports
      # txn_id == 0.
      %{state | txn_id: txn_id, runtime_txn_active: true, runtime_boundary_seen: true}
    else
      clear_transaction_state(%{state | runtime_boundary_seen: true})
    end
  end

  defp apply_runtime_txn_status(state, status, txn_id) do
    txn_id = normalize_runtime_txn_id(txn_id)
    if normalize_runtime_status(status) in [?T, ?t] do
      %{state | txn_id: txn_id, runtime_txn_active: true, runtime_boundary_seen: true}
    else
      clear_transaction_state(%{state | runtime_boundary_seen: true})
    end
  end

  defp clear_transaction_state(state) do
    %{state | txn_id: 0, runtime_txn_active: false}
  end

  defp transaction_active?(state) do
    state.runtime_txn_active || state.txn_id != 0
  end

  defp can_adopt_fresh_native_boundary?(state, opts) do
    state.runtime_boundary_seen &&
      state.runtime_txn_active &&
      state.txn_id == 0 &&
      compatible_default_fresh_boundary?(opts)
  end

  defp compatible_default_fresh_boundary?(opts) do
    isolation = Map.get(opts, :isolation_level, Protocol.isolation(:read_committed))
    read_committed_mode = Map.get(opts, :read_committed_mode, Protocol.read_committed_mode(:default))

    Map.get(opts, :conflict_action, 0) == 0 &&
      Map.get(opts, :autocommit_mode, 0) == 0 &&
      Map.get(opts, :access_mode, 0) == 0 &&
      Map.get(opts, :timeout_ms, 0) == 0 &&
      !Map.get(opts, :deferrable, false) &&
      !Map.get(opts, :wait, false) &&
      isolation in [Protocol.isolation(:read_uncommitted), Protocol.isolation(:read_committed)] &&
      read_committed_mode == Protocol.read_committed_mode(:default)
  end

  defp normalize_runtime_txn_id(txn_id) when is_integer(txn_id), do: txn_id
  defp normalize_runtime_txn_id(txn_id) when is_binary(txn_id) do
    case Integer.parse(txn_id) do
      {value, _} -> value
      _ -> 0
    end
  end
  defp normalize_runtime_txn_id(_txn_id), do: 0

  defp normalize_runtime_status(status) when is_integer(status), do: status
  defp normalize_runtime_status(status) when is_binary(status) and byte_size(status) > 0 do
    :binary.first(status)
  end
  defp normalize_runtime_status(_status), do: 0

  defp parse_uuid_bytes(value) do
    hex =
      value
      |> String.replace("-", "")
      |> String.trim()
    if String.match?(hex, ~r/^[0-9A-Fa-f]{32}$/) do
      Base.decode16(hex, case: :mixed)
    else
      :error
    end
  end

  defp drain_until_ready(state) do
    case recv_message(state) do
      {:ok, msg} ->
        case handle_async(state, msg) do
          {:handled, new_state} -> drain_until_ready(new_state)
          {:ok, new_state} ->
            case msg.type do
              @msg_ready ->
                {:ok, status, txn_id} = Protocol.parse_ready(msg.payload)
                {:ok, apply_runtime_ready_state(new_state, status, txn_id)}
              @msg_error ->
                {:error, Protocol.parse_error(msg.payload), new_state}
              _ ->
                drain_until_ready(new_state)
            end
        end
      error -> error
    end
  end

  defp start_resilience(state) do
    connection_id = :crypto.strong_rand_bytes(8) |> Base.encode16(case: :lower)
    %{
      state
      | connection_id: connection_id,
        circuit_breaker: CircuitBreaker.new(),
        telemetry: Telemetry.Collector.new(),
        keepalive_tracker: Keepalive.new_tracker(),
        leak_guard: LeakDetector.checkout(%LeakDetector.Config{}, %{driver: "elixir"})
    }
  end

  defp stop_resilience(state) do
    if state.leak_guard do
      LeakDetector.release(state.leak_guard)
    end
    state
  end

  defp validate_if_idle(state) do
    tracker = state.keepalive_tracker
    if tracker && Keepalive.Tracker.needs_validation?(tracker) do
      case ping(state) do
        {:ok, new_state} ->
          tracker = Keepalive.Tracker.mark_active(tracker)
          {:ok, %{new_state | keepalive_tracker: tracker}}

        {:error, reason, new_state} ->
          {:error, reason, new_state}
      end
    else
      {:ok, state}
    end
  end

  defp with_resilience(state, operation, sql, fun) do
    cb = state.circuit_breaker || CircuitBreaker.new()
    {allowed, cb} = CircuitBreaker.allow_request?(cb)
    state = %{state | circuit_breaker: cb}
    if not allowed do
      {:error, "Circuit breaker is OPEN", state}
    else
      case validate_if_idle(state) do
        {:error, reason, new_state} ->
          {:error, reason, new_state}

        {:ok, new_state} ->
          telemetry = new_state.telemetry || Telemetry.Collector.new()
          {span, telemetry} = Telemetry.Collector.start_span(telemetry, operation)
          span =
            if span && sql do
              Telemetry.SpanContext.with_attribute(span, "db.statement", Telemetry.Collector.sanitize_query(sql))
            else
              span
            end

          case fun.(%{new_state | telemetry: telemetry}) do
            {:ok, result, after_state} ->
              {:ok, result, finish_operation(after_state, span, true)}

            {:ok, after_state} ->
              {:ok, finish_operation(after_state, span, true)}

            {:error, reason, after_state} ->
              {:error, reason, finish_operation(after_state, span, false)}
          end
      end
    end
  end

  defp finish_operation(state, span, success) do
    cb =
      if success do
        CircuitBreaker.record_success(state.circuit_breaker)
      else
        CircuitBreaker.record_failure(state.circuit_breaker)
      end

    tracker =
      if success && state.keepalive_tracker do
        Keepalive.Tracker.mark_active(state.keepalive_tracker)
      else
        state.keepalive_tracker
      end

    telemetry = Telemetry.Collector.end_span(state.telemetry, span, success)

    %{state | circuit_breaker: cb, telemetry: telemetry, keepalive_tracker: tracker}
  end

  defp collect_results(state, rows) do
    case recv_message(state) do
      {:ok, msg} ->
        case handle_async(state, msg) do
          {:handled, new_state} -> collect_results(new_state, rows)
          {:ok, new_state} ->
            case msg.type do
              @msg_row_description ->
                columns = Protocol.parse_row_description(msg.payload)
                collect_rows(new_state, columns, rows)

              @msg_data_row ->
                {values, _} = Protocol.parse_data_row(msg.payload, 0)
                collect_results(new_state, [values | rows])

              @msg_command_complete ->
                collect_results(new_state, rows)

              @msg_ready ->
                {:ok, status, txn_id} = Protocol.parse_ready(msg.payload)
                {:ok, %{rows: Enum.reverse(rows), columns: []}, apply_runtime_ready_state(new_state, status, txn_id)}

              @msg_error ->
                {:error, Protocol.parse_error(msg.payload), new_state}

              _ ->
                collect_results(new_state, rows)
            end
        end
      error -> error
    end
  end

  defp collect_rows(state, columns, rows) do
    case recv_message(state) do
      {:ok, msg} ->
        case handle_async(state, msg) do
          {:handled, new_state} -> collect_rows(new_state, columns, rows)
          {:ok, new_state} ->
            case msg.type do
              @msg_data_row ->
                {values, _} = Protocol.parse_data_row(msg.payload, length(columns))
                decoded = decode_row(columns, values)
                collect_rows(new_state, columns, [decoded | rows])

              @msg_command_complete ->
                collect_rows(new_state, columns, rows)

              @msg_ready ->
                {:ok, status, txn_id} = Protocol.parse_ready(msg.payload)
                {:ok, %{rows: Enum.reverse(rows), columns: columns}, apply_runtime_ready_state(new_state, status, txn_id)}

              @msg_error ->
                {:error, Protocol.parse_error(msg.payload), new_state}

              _ ->
                collect_rows(new_state, columns, rows)
            end
        end

      error -> error
    end
  end

  defp decode_row(columns, values) do
    Enum.zip(columns, values)
    |> Enum.map(fn {col, val} ->
      if val.null do
        nil
      else
        Types.decode_value(col.type_oid, val.data, col.format)
      end
    end)
  end

  defp update_resolved_auth_context(state, method, resolved_method) do
    %AuthBootstrap.ResolvedAuthContext{} = resolved_auth_context = state.resolved_auth_context

    put_in(
      state.resolved_auth_context,
      %AuthBootstrap.ResolvedAuthContext{
        resolved_auth_context
        | resolved_auth_method: resolved_method,
          resolved_auth_plugin_id:
            AuthBootstrap.auth_plugin_id_for_method(method, state.config[:auth_method_id])
      }
    )
  end

  defp requested_features(config) do
    features = 0
    features = if config[:compression] == "zstd", do: features ||| Protocol.feature(:compression), else: features
    features = if config[:binary_transfer], do: features ||| Protocol.feature(:streaming), else: features
    features
  end

  defp send_message(state, type, payload, flags) do
    header = %{
      type: type,
      flags: flags,
      length: byte_size(payload),
      sequence: state.sequence,
      attachment_id: state.attachment_id,
      txn_id: state.txn_id
    }
    data = Protocol.encode_message(header, payload)
    _ = send_raw(state, data)
    %{state | sequence: state.sequence + 1}
  end

  defp send_raw(state, data) do
    case state.transport do
      :ssl -> :ssl.send(state.socket, data)
      :tcp -> :gen_tcp.send(state.socket, data)
    end
  end

  defp recv_message(state) do
    with {:ok, header_bin} <- recv_exact(state, Protocol.header_size()),
         {:ok, header} <- Protocol.decode_header(header_bin),
         {:ok, payload} <- recv_exact(state, header.length) do
      {:ok, Map.put(header, :payload, payload)}
    end
  end

  defp recv_exact(_state, 0), do: {:ok, <<>>}

  defp recv_exact(state, bytes) do
    case state.transport do
      :ssl -> :ssl.recv(state.socket, bytes)
      :tcp -> :gen_tcp.recv(state.socket, bytes)
    end
  end
end
