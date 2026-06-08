# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.Protocol do
  @moduledoc false
  import Bitwise

  @protocol_magic 0x50574253
  @protocol_major 1
  @protocol_minor 1
  @header_size 40
  @max_message_size 1_073_741_824

  @type message :: %{type: integer, flags: integer, length: integer, sequence: integer, attachment_id: binary, txn_id: integer, payload: binary}

  def protocol_major, do: @protocol_major
  def protocol_minor, do: @protocol_minor
  def header_size, do: @header_size

  def message_type(:startup), do: 0x01
  def message_type(:auth_response), do: 0x02
  def message_type(:query), do: 0x03
  def message_type(:parse), do: 0x04
  def message_type(:bind), do: 0x05
  def message_type(:describe), do: 0x06
  def message_type(:execute), do: 0x07
  def message_type(:close), do: 0x08
  def message_type(:sync), do: 0x09
  def message_type(:flush), do: 0x0A
  def message_type(:cancel), do: 0x0B
  def message_type(:terminate), do: 0x0C
  def message_type(:copy_data), do: 0x0D
  def message_type(:copy_done), do: 0x0E
  def message_type(:copy_fail), do: 0x0F
  def message_type(:sblr_execute), do: 0x10
  def message_type(:subscribe), do: 0x11
  def message_type(:unsubscribe), do: 0x12
  def message_type(:federated_query), do: 0x13
  def message_type(:stream_control), do: 0x14
  def message_type(:txn_begin), do: 0x15
  def message_type(:txn_commit), do: 0x16
  def message_type(:txn_rollback), do: 0x17
  def message_type(:txn_savepoint), do: 0x18
  def message_type(:txn_release), do: 0x19
  def message_type(:txn_rollback_to), do: 0x1A
  def message_type(:ping), do: 0x1B
  def message_type(:set_option), do: 0x1C
  def message_type(:cluster_auth), do: 0x1D
  def message_type(:attach_create), do: 0x1E
  def message_type(:attach_detach), do: 0x1F
  def message_type(:attach_list), do: 0x20
  def message_type(:auth_request), do: 0x40
  def message_type(:auth_ok), do: 0x41
  def message_type(:auth_continue), do: 0x42
  def message_type(:ready), do: 0x43
  def message_type(:row_description), do: 0x44
  def message_type(:data_row), do: 0x45
  def message_type(:command_complete), do: 0x46
  def message_type(:empty_query), do: 0x47
  def message_type(:error), do: 0x48
  def message_type(:notice), do: 0x49
  def message_type(:parse_complete), do: 0x4A
  def message_type(:bind_complete), do: 0x4B
  def message_type(:close_complete), do: 0x4C
  def message_type(:portal_suspended), do: 0x4D
  def message_type(:no_data), do: 0x4E
  def message_type(:parameter_status), do: 0x4F
  def message_type(:parameter_description), do: 0x50
  def message_type(:copy_in_response), do: 0x51
  def message_type(:copy_out_response), do: 0x52
  def message_type(:copy_both_response), do: 0x53
  def message_type(:notification), do: 0x54
  def message_type(:function_result), do: 0x55
  def message_type(:negotiate_version), do: 0x56
  def message_type(:sblr_compiled), do: 0x57
  def message_type(:query_plan), do: 0x58
  def message_type(:stream_ready), do: 0x59
  def message_type(:stream_data), do: 0x5A
  def message_type(:stream_end), do: 0x5B
  def message_type(:txn_status), do: 0x5C
  def message_type(:pong), do: 0x5D
  def message_type(:cluster_auth_ok), do: 0x5E
  def message_type(:federated_result), do: 0x5F
  def message_type(:heartbeat), do: 0x80
  def message_type(:extension), do: 0x81

  def auth_method(:ok), do: 0
  def auth_method(:password), do: 1
  def auth_method(:md5), do: 2
  def auth_method(:scram_sha_256), do: 3
  def auth_method(:scram_sha_512), do: 4
  def auth_method(:token), do: 5
  def auth_method(:peer), do: 6
  def auth_method(:reattach), do: 7

  def flag(:compressed), do: 0x01
  def flag(:continued), do: 0x02
  def flag(:final), do: 0x04
  def flag(:urgent), do: 0x08
  def flag(:encrypted), do: 0x10
  def flag(:checksum), do: 0x20

  def feature(:compression), do: 1 <<< 0
  def feature(:streaming), do: 1 <<< 1
  def feature(:sblr), do: 1 <<< 2
  def feature(:federation), do: 1 <<< 3
  def feature(:notifications), do: 1 <<< 4
  def feature(:query_plan), do: 1 <<< 5
  def feature(:batch), do: 1 <<< 6
  def feature(:pipeline), do: 1 <<< 7
  def feature(:binary_copy), do: 1 <<< 8
  def feature(:savepoints), do: 1 <<< 9
  def feature(:twopc), do: 1 <<< 10
  def feature(:checksums), do: 1 <<< 11

  def query_flag(:describe_only), do: 0x01
  def query_flag(:no_portal), do: 0x02
  def query_flag(:binary_result), do: 0x04
  def query_flag(:include_plan), do: 0x08
  def query_flag(:return_sblr), do: 0x10
  def query_flag(:no_cache), do: 0x20

  def isolation(:read_uncommitted), do: 0
  def isolation(:read_committed), do: 1
  def isolation(:repeatable_read), do: 2
  def isolation(:serializable), do: 3

  def read_committed_mode(:default), do: 0
  def read_committed_mode(:read_consistency), do: 1
  def read_committed_mode(:record_version), do: 2
  def read_committed_mode(:no_record_version), do: 3

  def txn_flag(:has_isolation), do: 0x0001
  def txn_flag(:has_access), do: 0x0002
  def txn_flag(:has_deferrable), do: 0x0004
  def txn_flag(:has_wait), do: 0x0008
  def txn_flag(:has_timeout), do: 0x0010
  def txn_flag(:has_autocommit), do: 0x0020
  def txn_flag(:has_read_committed_mode), do: 0x0100

  def stream_control(:start), do: 0
  def stream_control(:pause), do: 1
  def stream_control(:resume), do: 2
  def stream_control(:cancel), do: 3
  def stream_control(:ack), do: 4

  def subscribe_type(:channel), do: 0
  def subscribe_type(:table), do: 1
  def subscribe_type(:query), do: 2
  def subscribe_type(:event), do: 3

  @auth_param_method_id "auth_method_id"
  @auth_param_method_payload "auth_method_payload"
  @auth_param_payload_json "auth_payload_json"
  @auth_param_payload_b64 "auth_payload_b64"
  @auth_param_provider_profile "auth_provider_profile"
  @auth_param_required_methods "auth_required_methods"
  @auth_param_forbidden_methods "auth_forbidden_methods"
  @auth_param_require_channel_binding "auth_require_channel_binding"
  @auth_param_workload_identity_token "workload_identity_token"
  @auth_param_proxy_principal_assertion "proxy_principal_assertion"

  def apply_auth_plugin_selection(params, selection) when is_map(params) and is_map(selection) do
    method_id = selection |> Map.get(:method_id, "") |> to_string() |> String.trim()
    if method_id != "" and not String.starts_with?(method_id, "scratchbird.auth.") do
      raise ArgumentError, "invalid auth_method_id namespace"
    end

    params
    |> maybe_put(@auth_param_method_id, method_id)
    |> maybe_put(@auth_param_method_payload, Map.get(selection, :method_payload, ""))
    |> maybe_put(@auth_param_payload_json, Map.get(selection, :payload_json, ""))
    |> maybe_put(@auth_param_payload_b64, Map.get(selection, :payload_b64, ""))
    |> maybe_put(@auth_param_provider_profile, Map.get(selection, :provider_profile, ""))
    |> maybe_put(@auth_param_required_methods, Map.get(selection, :required_methods, ""))
    |> maybe_put(@auth_param_forbidden_methods, Map.get(selection, :forbidden_methods, ""))
    |> maybe_put(
      @auth_param_require_channel_binding,
      if(Map.get(selection, :require_channel_binding, false), do: "true", else: "")
    )
    |> maybe_put(@auth_param_workload_identity_token, Map.get(selection, :workload_identity_token, ""))
    |> maybe_put(@auth_param_proxy_principal_assertion, Map.get(selection, :proxy_principal_assertion, ""))
  end

  def encode_message(%{type: type, flags: flags, length: length, sequence: sequence, attachment_id: attachment_id, txn_id: txn_id}, payload) do
    attachment_id = attachment_id || <<0::128>>
    <<
      @protocol_magic::little-32,
      @protocol_major::8,
      @protocol_minor::8,
      type::8,
      flags::8,
      length::little-32,
      sequence::little-32,
      attachment_id::binary-16,
      txn_id::little-64,
      payload::binary
    >>
  end

  def decode_header(<<@protocol_magic::little-32, major::8, minor::8, type::8, flags::8, length::little-32, sequence::little-32, attachment_id::binary-16, txn_id::little-64>>) do
    if major != @protocol_major or minor != @protocol_minor do
      {:error, :unsupported_protocol}
    else
      if length > @max_message_size do
        {:error, :payload_too_large}
      else
        {:ok, %{type: type, flags: flags, length: length, sequence: sequence, attachment_id: attachment_id, txn_id: txn_id}}
      end
    end
  end

  def decode_header(_), do: {:error, :invalid_header}

  def build_startup_payload(features, params) do
    param_bytes = build_param_list(params)
    <<@protocol_major::8, @protocol_minor::8, 0::little-16, features::little-64, param_bytes::binary>>
  end

  def build_query_payload(sql, flags, max_rows, timeout_ms) do
    sql_bytes = sql <> <<0>>
    <<flags::little-32, max_rows::little-32, timeout_ms::little-32, sql_bytes::binary>>
  end

  def build_parse_payload(statement_name, sql, param_types) do
    name_bytes = statement_name || ""
    sql_bytes = sql || ""
    payload = [
      <<byte_size(name_bytes)::little-32>>, name_bytes,
      <<byte_size(sql_bytes)::little-32>>, sql_bytes,
      <<length(param_types)::little-16, 0::little-16>>,
      Enum.map(param_types, fn oid -> <<oid::little-32>> end)
    ]
    IO.iodata_to_binary(payload)
  end

  def build_bind_payload(portal_name, statement_name, params, result_formats) do
    portal = portal_name || ""
    statement = statement_name || ""
    param_formats = Enum.map(params, & &1.format)
    param_count = length(params)
    format_count = length(param_formats)
    result_count = length(result_formats)

    param_chunks =
      Enum.map(params, fn param ->
        if param.is_null do
          <<0xFFFFFFFF::little-32>>
        else
          data = param.data || <<>>
          <<byte_size(data)::little-32, data::binary>>
        end
      end)

    payload = [
      <<byte_size(portal)::little-32>>, portal,
      <<byte_size(statement)::little-32>>, statement,
      <<format_count::little-16>>,
      Enum.map(param_formats, fn fmt -> <<fmt::little-16>> end),
      <<param_count::little-16, 0::little-16>>,
      param_chunks,
      <<result_count::little-16>>,
      Enum.map(result_formats, fn fmt -> <<fmt::little-16>> end)
    ]

    IO.iodata_to_binary(payload)
  end

  def build_describe_payload(kind, name) do
    name_bytes = name || ""
    <<kind::8, 0::little-24, byte_size(name_bytes)::little-32, name_bytes::binary>>
  end

  def build_execute_payload(portal, max_rows) do
    portal_bytes = portal || ""
    <<byte_size(portal_bytes)::little-32, portal_bytes::binary, max_rows::little-32>>
  end

  def build_close_payload(kind, name) do
    name_bytes = name || ""
    <<kind::8, 0::little-24, byte_size(name_bytes)::little-32, name_bytes::binary>>
  end

  def build_cancel_payload(cancel_type, target_sequence) do
    <<cancel_type::little-32, target_sequence::little-32>>
  end

  def build_sblr_execute_payload(sblr_hash, sblr_bytecode, params) do
    bytecode = sblr_bytecode || <<>>
    param_count = length(params)

    param_chunks =
      Enum.map(params, fn param ->
        if param.is_null do
          <<0xFFFFFFFF::little-32>>
        else
          data = param.data || <<>>
          <<byte_size(data)::little-32, data::binary>>
        end
      end)

    IO.iodata_to_binary([
      <<sblr_hash::little-64, byte_size(bytecode)::little-32, param_count::little-16, 0::little-16>>,
      bytecode,
      param_chunks
    ])
  end

  def build_subscribe_payload(subscribe_type, channel, filter_expr) do
    channel_bytes = channel || ""
    filter_bytes = filter_expr || ""
    <<subscribe_type::8, 0::little-24,
      byte_size(channel_bytes)::little-32, channel_bytes::binary,
      byte_size(filter_bytes)::little-32, filter_bytes::binary>>
  end

  def build_unsubscribe_payload(channel) do
    channel_bytes = channel || ""
    <<byte_size(channel_bytes)::little-32, channel_bytes::binary>>
  end

  def build_txn_begin_payload(flags, conflict_action, autocommit_mode, isolation_level, access_mode, deferrable, wait_mode, timeout_ms, read_committed_mode \\ read_committed_mode(:default)) do
    payload =
      <<flags::little-16, conflict_action::8, autocommit_mode::8, isolation_level::8, access_mode::8,
        deferrable::8, wait_mode::8, timeout_ms::little-32>>

    if (flags &&& txn_flag(:has_read_committed_mode)) != 0 do
      <<payload::binary, read_committed_mode::8, 0::24>>
    else
      payload
    end
  end

  def canonical_read_committed_mode_label(mode) do
    case mode do
      0 -> "READ COMMITTED"
      1 -> "READ COMMITTED READ CONSISTENCY"
      2 -> "READ COMMITTED RECORD VERSION"
      3 -> "READ COMMITTED NO RECORD VERSION"
      _ -> "UNKNOWN(#{mode})"
    end
  end

  def build_txn_commit_payload(flags) do
    <<flags::8, 0::24>>
  end

  def build_txn_rollback_payload(flags) do
    <<flags::8, 0::24>>
  end

  def build_txn_savepoint_payload(name) do
    name_bytes = name || ""
    <<byte_size(name_bytes)::little-32, name_bytes::binary>>
  end

  def build_txn_release_payload(name), do: build_txn_savepoint_payload(name)

  def build_txn_rollback_to_payload(name), do: build_txn_savepoint_payload(name)

  def build_set_option_payload(name, value) do
    name_bytes = name || ""
    value_bytes = value || ""
    <<byte_size(name_bytes)::little-32, name_bytes::binary, byte_size(value_bytes)::little-32, value_bytes::binary>>
  end

  def build_stream_control_payload(control_type, window_size, timeout_ms) do
    <<control_type::8, 0::24, window_size::little-32, timeout_ms::little-32>>
  end

  defp maybe_put(map, _key, ""), do: map
  defp maybe_put(map, key, value), do: Map.put(map, key, to_string(value))

  def build_attach_create_payload(emulation_mode, db_name) do
    mode_bytes = emulation_mode || ""
    db_bytes = db_name || ""
    <<byte_size(mode_bytes)::little-32, mode_bytes::binary, byte_size(db_bytes)::little-32, db_bytes::binary>>
  end

  def parse_auth_request(<<method::8, _::24, rest::binary>>) do
    {:ok, method, rest}
  end

  def parse_auth_continue(<<method::8, stage::8, _::16, len::little-32, rest::binary>>) do
    data = binary_part(rest, 0, len)
    {:ok, method, stage, data}
  end

  def parse_auth_ok(<<session_id::binary-16, info_len::little-32, info::binary>>) do
    {:ok, session_id, binary_part(info, 0, info_len)}
  end

  def parse_ready(<<status::8, _::24, txn_id::little-64, _::binary>>) do
    {:ok, status, txn_id}
  end

  def parse_txn_status(<<status::8, _::24, txn_id::little-64, _::binary>>) do
    {:ok, status, txn_id}
  end

  def parse_parameter_status(payload) do
    {name, rest} = read_cstring(payload)
    {value, _} = read_cstring(rest)
    {:ok, name, value}
  end

  def parse_parameter_description(<<count::little-16, _reserved::little-16, rest::binary>>) do
    parse_parameter_types(count, rest, [])
  end

  defp parse_parameter_types(0, rest, acc), do: {Enum.reverse(acc), rest} |> elem(0)

  defp parse_parameter_types(count, <<type_oid::little-32, rest::binary>>, acc) do
    parse_parameter_types(count - 1, rest, [type_oid | acc])
  end

  def parse_error(payload) do
    parse_error_fields(payload, %{})
  end

  def parse_command_complete(<<command_type::8, _::24, rows::little-64, last_id::little-64, rest::binary>>) do
    {tag, _} = read_cstring(rest)
    %{command_type: command_type, rows: rows, last_id: last_id, tag: tag}
  end

  def parse_notification(<<process_id::little-32, channel_len::little-32, rest::binary>>) do
    <<channel::binary-size(channel_len), rest2::binary>> = rest
    <<payload_len::little-32, rest3::binary>> = rest2
    <<payload::binary-size(payload_len), rest4::binary>> = rest3
    {change_type, row_id} =
      case rest4 do
        <<change::binary-1, id::little-64, _::binary>> -> {change, id}
        <<change::binary-1>> -> {change, nil}
        _ -> {nil, nil}
      end
    %{
      process_id: process_id,
      channel: channel,
      payload: payload,
      change_type: change_type,
      row_id: row_id
    }
  end

  def parse_query_plan(<<format::little-32, plan_len::little-32, planning_time_us::little-64, estimated_rows::little-64, estimated_cost::little-64, rest::binary>>) do
    <<plan::binary-size(plan_len), _::binary>> = rest
    %{
      format: format,
      planning_time_us: planning_time_us,
      estimated_rows: estimated_rows,
      estimated_cost: estimated_cost,
      plan: plan
    }
  end

  def parse_sblr_compiled(<<hash::little-64, version::little-32, len::little-32, rest::binary>>) do
    <<bytecode::binary-size(len), _::binary>> = rest
    %{hash: hash, version: version, bytecode: bytecode}
  end

  defp parse_error_fields(<<0>>, acc), do: acc
  defp parse_error_fields(<<code::8, rest::binary>>, acc) do
    {value, next} = read_cstring(rest)
    key = error_field_name(code)
    parse_error_fields(next, Map.put(acc, key, value))
  end

  defp error_field_name(?S), do: :severity
  defp error_field_name(?C), do: :sqlstate
  defp error_field_name(?M), do: :message
  defp error_field_name(?D), do: :detail
  defp error_field_name(?H), do: :hint
  defp error_field_name(_), do: :unknown

  def parse_row_description(<<count::little-16, _reserved::little-16, rest::binary>>) do
    {columns, _} = parse_columns(count, rest, [])
    columns
  end

  defp parse_columns(0, rest, acc), do: {Enum.reverse(acc), rest}

  defp parse_columns(count, <<name_len::little-32, rest::binary>>, acc) do
    <<name::binary-size(name_len), rest2::binary>> = rest
    <<table_oid::little-32, column_idx::little-16, type_oid::little-32, type_size::little-16,
      type_mod::little-32, format::8, nullable::8, _reserved::little-16, rest3::binary>> = rest2

    column = %{
      name: name,
      table_oid: table_oid,
      column_index: column_idx,
      type_oid: type_oid,
      type_size: type_size,
      type_modifier: type_mod,
      format: format,
      nullable: nullable == 1
    }
    parse_columns(count - 1, rest3, [column | acc])
  end

  def parse_data_row(<<count::little-16, null_bytes::little-16, rest::binary>>, column_count) do
    if column_count > 0 and count != column_count do
      raise MatchError, "row data column count mismatch"
    end

    if byte_size(rest) < null_bytes do
      raise MatchError, "row data truncated"
    end

    <<null_bitmap::binary-size(null_bytes), values::binary>> = rest
    parse_row_values(count, values, null_bitmap, 0, [])
  end

  defp parse_row_values(0, rest, _null_bitmap, _index, acc), do: {Enum.reverse(acc), rest}

  defp parse_row_values(count, rest, null_bitmap, index, acc) do
    byte_index = div(index, 8)
    bit_index = rem(index, 8)

    null_byte =
      if byte_index < byte_size(null_bitmap) do
        null_bitmap
        |> binary_part(byte_index, 1)
        |> :binary.decode_unsigned()
      else
        0
      end

    is_null = (null_byte &&& (1 <<< bit_index)) != 0

    if is_null do
      parse_row_values(count - 1, rest, null_bitmap, index + 1, [%{null: true, data: nil} | acc])
    else
      <<len::little-32, rest2::binary>> = rest

      if len < 0 do
        parse_row_values(count - 1, rest2, null_bitmap, index + 1, [%{null: true, data: nil} | acc])
      else
        <<data::binary-size(len), rest3::binary>> = rest2
        parse_row_values(count - 1, rest3, null_bitmap, index + 1, [%{null: false, data: data} | acc])
      end
    end
  end

  def read_cstring(data) do
    case :binary.match(data, <<0>>) do
      {idx, 1} ->
        <<value::binary-size(idx), 0, rest::binary>> = data
        {value, rest}
      :nomatch ->
        {data, <<>>}
    end
  end

  defp build_param_list(params) do
    params
    |> Enum.reduce([], fn {key, value}, acc ->
      [acc, to_string(key), <<0>>, to_string(value), <<0>>]
    end)
    |> IO.iodata_to_binary()
    |> then(fn buf -> <<buf::binary, 0>> end)
  end
end
