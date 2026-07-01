# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.Config do
  @default_port 3092

  def from_opts(opts) when is_list(opts) do
    opts = Enum.into(opts, %{})
    from_map(opts)
  end

  def from_map(opts) when is_map(opts) do
    dsn =
      Map.get(opts, :url) ||
        Map.get(opts, :dsn) ||
        Map.get(opts, "url") ||
        Map.get(opts, "dsn")

    base =
      dsn
      |> parse_dsn()

    merged = Map.merge(base, normalize_keys(opts))

    merged
    |> Map.put_new(:port, @default_port)
    |> Map.put_new(:protocol, "native")
    |> Map.put_new(:front_door_mode, "direct")
    |> Map.put_new(:sslmode, "require")
    |> Map.put_new(:binary_transfer, true)
    |> Map.put_new(:compression, "off")
    |> Map.put_new(:manager_auth_token, "")
    |> Map.put_new(:manager_username, "")
    |> Map.put_new(:manager_database, "")
    |> Map.put_new(:manager_connection_profile, "SBsql")
    |> Map.put_new(:manager_client_intent, "SBsql")
    |> Map.put_new(:manager_client_flags, 0)
    |> Map.put_new(:manager_auth_fast_path, true)
    |> Map.put_new(:connect_client_flags, 0)
    |> Map.put_new(:auth_token, "")
    |> Map.put_new(:auth_method_id, "")
    |> Map.put_new(:auth_method_payload, "")
    |> Map.put_new(:auth_payload_json, "")
    |> Map.put_new(:auth_payload_b64, "")
    |> Map.put_new(:auth_provider_profile, "")
    |> Map.put_new(:auth_required_methods, "")
    |> Map.put_new(:auth_forbidden_methods, "")
    |> Map.put_new(:auth_require_channel_binding, false)
    |> Map.put_new(:workload_identity_token, "")
    |> Map.put_new(:proxy_principal_assertion, "")
    |> Map.put_new(:dormant_id, "")
    |> Map.put_new(:dormant_reattach_token, "")
    |> normalize_values()
  end

  def parse_dsn(nil), do: %{}

  def parse_dsn("") do
    %{}
  end

  def parse_dsn(dsn) when is_binary(dsn) do
    if String.contains?(dsn, "://") do
      parse_uri(dsn)
    else
      parse_kv(dsn)
    end
  end

  defp parse_uri(dsn) do
    uri = URI.parse(dsn)
    query = URI.decode_query(uri.query || "")
    user_info = parse_userinfo(uri.userinfo)

    %{}
    |> maybe_put(:host, uri.host)
    |> maybe_put(:port, uri.port)
    |> maybe_put(:user, user_info.user)
    |> maybe_put(:password, user_info.password)
    |> maybe_put(:database, uri.path && String.trim_leading(uri.path, "/"))
    |> Map.merge(normalize_keys(query))
  end

  defp parse_kv(dsn) do
    dsn
    |> String.split(~r/\s+/, trim: true)
    |> Enum.reduce(%{}, fn part, acc ->
      case String.split(part, "=", parts: 2) do
        [key, value] -> Map.merge(acc, normalize_keys(%{key => value}))
        _ -> acc
      end
    end)
  end

  defp normalize_keys(opts) do
    opts
    |> Enum.reduce(%{}, fn {key, value}, acc ->
      atom_key =
        key
        |> to_string()
        |> String.downcase()
        |> normalize_alias()
        |> String.to_atom()

      Map.put(acc, atom_key, value)
    end)
  end

  defp normalize_alias("dbname"), do: "database"
  defp normalize_alias("username"), do: "user"
  defp normalize_alias("applicationname"), do: "application_name"
  defp normalize_alias("searchpath"), do: "search_path"
  defp normalize_alias("binarytransfer"), do: "binary_transfer"
  defp normalize_alias("ipcpath"), do: "ipc_path"
  defp normalize_alias("unixsocket"), do: "ipc_path"
  defp normalize_alias("socketpath"), do: "ipc_path"
  defp normalize_alias("parser"), do: "protocol"
  defp normalize_alias("dialect"), do: "protocol"
  defp normalize_alias("frontdoormode"), do: "front_door_mode"
  defp normalize_alias("connection_mode"), do: "front_door_mode"
  defp normalize_alias("ingress_mode"), do: "front_door_mode"
  defp normalize_alias("mcp_auth_token"), do: "manager_auth_token"
  defp normalize_alias("mcp_username"), do: "manager_username"
  defp normalize_alias("mcp_database"), do: "manager_database"
  defp normalize_alias("mcp_connection_profile"), do: "manager_connection_profile"
  defp normalize_alias("mcp_client_intent"), do: "manager_client_intent"
  defp normalize_alias("mcp_client_flags"), do: "manager_client_flags"
  defp normalize_alias("mcp_auth_fast_path"), do: "manager_auth_fast_path"
  defp normalize_alias("authtoken"), do: "auth_token"
  defp normalize_alias("bearertoken"), do: "auth_token"
  defp normalize_alias("token"), do: "auth_token"
  defp normalize_alias("connectclientflags"), do: "connect_client_flags"
  defp normalize_alias("authmethodid"), do: "auth_method_id"
  defp normalize_alias("authmethodpayload"), do: "auth_method_payload"
  defp normalize_alias("authpayloadjson"), do: "auth_payload_json"
  defp normalize_alias("authpayloadb64"), do: "auth_payload_b64"
  defp normalize_alias("authproviderprofile"), do: "auth_provider_profile"
  defp normalize_alias("authrequiredmethods"), do: "auth_required_methods"
  defp normalize_alias("authforbiddenmethods"), do: "auth_forbidden_methods"
  defp normalize_alias("authrequirechannelbinding"), do: "auth_require_channel_binding"
  defp normalize_alias("workloadidentitytoken"), do: "workload_identity_token"
  defp normalize_alias("proxyprincipalassertion"), do: "proxy_principal_assertion"
  defp normalize_alias("dormantid"), do: "dormant_id"
  defp normalize_alias("dormantreattachtoken"), do: "dormant_reattach_token"
  defp normalize_alias("sslrootcert"), do: "sslrootcert"
  defp normalize_alias("sslcert"), do: "sslcert"
  defp normalize_alias("sslkey"), do: "sslkey"
  defp normalize_alias("sslpassword"), do: "sslpassword"
  defp normalize_alias("connecttimeout"), do: "connect_timeout"
  defp normalize_alias("sockettimeout"), do: "socket_timeout"
  defp normalize_alias(name), do: name

  defp maybe_put(map, _key, nil), do: map
  defp maybe_put(map, key, value), do: Map.put(map, key, value)

  defp normalize_values(cfg) do
    cfg
    |> Map.update!(:protocol, &normalize_native_protocol/1)
    |> Map.update!(:front_door_mode, &normalize_front_door_mode/1)
    |> Map.update!(:sslmode, fn value ->
      value |> to_string() |> String.trim() |> String.downcase()
    end)
    |> Map.update!(:port, &to_int(&1, @default_port))
    |> Map.update(:connect_timeout, 5000, &to_int(&1, 5000))
    |> Map.update(:socket_timeout, 0, &to_int(&1, 0))
    |> Map.update!(:binary_transfer, &to_bool(&1, true))
    |> Map.update!(:manager_client_flags, &to_int(&1, 0))
    |> Map.update!(:manager_auth_fast_path, &to_bool(&1, true))
    |> Map.update!(:connect_client_flags, &to_int(&1, 0))
    |> Map.update!(:auth_require_channel_binding, &to_bool(&1, false))
  end

  defp parse_userinfo(nil), do: %{user: nil, password: nil}

  defp parse_userinfo(userinfo) when is_binary(userinfo) do
    case String.split(userinfo, ":", parts: 2) do
      [user, pass] -> %{user: user, password: pass}
      [user] -> %{user: user, password: nil}
      _ -> %{user: nil, password: nil}
    end
  end

  defp normalize_native_protocol(value) do
    normalized = value |> to_string() |> String.trim() |> String.downcase()

    case normalized do
      "" ->
        "native"

      "native" ->
        "native"

      "scratchbird" ->
        "native"

      "scratchbird-native" ->
        "native"

      "scratchbird_native" ->
        "native"

      _ ->
        raise ArgumentError,
              "Only protocol=native is supported; connect to the native parser listener/port."
    end
  end

  defp normalize_front_door_mode(value) do
    normalized = value |> to_string() |> String.trim() |> String.downcase()

    case normalized do
      "" -> "direct"
      "direct" -> "direct"
      "manager_proxy" -> "manager_proxy"
      "manager-proxy" -> "manager_proxy"
      "managed" -> "manager_proxy"
      _ -> raise ArgumentError, "front_door_mode must be direct or manager_proxy."
    end
  end

  defp to_int(value, _default) when is_integer(value), do: value

  defp to_int(value, default) do
    case Integer.parse(to_string(value)) do
      {parsed, _} -> parsed
      _ -> default
    end
  end

  defp to_bool(value, _default) when is_boolean(value), do: value

  defp to_bool(value, default) do
    case value |> to_string() |> String.trim() |> String.downcase() do
      "1" -> true
      "true" -> true
      "yes" -> true
      "on" -> true
      "0" -> false
      "false" -> false
      "no" -> false
      "off" -> false
      _ -> default
    end
  end
end
