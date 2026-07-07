# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.AuthBootstrap do
  @moduledoc false

  alias ScratchBird.Protocol

  defmodule AuthMethodSurface do
    @enforce_keys [:wire_method, :plugin_method_id, :executable_locally, :broker_required]
    defstruct [:wire_method, :plugin_method_id, :executable_locally, :broker_required]
  end

  defmodule AuthProbeResult do
    @enforce_keys [
      :reachable,
      :ingress_mode,
      :resolved_host,
      :resolved_port,
      :admitted_methods,
      :required_method,
      :required_plugin_method_id,
      :allowed_transport_mask,
      :additional_continuation_possible
    ]
    defstruct [
      :reachable,
      :ingress_mode,
      :resolved_host,
      :resolved_port,
      :admitted_methods,
      :required_method,
      :required_plugin_method_id,
      :allowed_transport_mask,
      :additional_continuation_possible
    ]
  end

  defmodule ResolvedAuthContext do
    @enforce_keys [:ingress_mode, :resolved_auth_method, :resolved_auth_plugin_id, :manager_authenticated, :attached]
    defstruct [:ingress_mode, :resolved_auth_method, :resolved_auth_plugin_id, :manager_authenticated, :attached]
  end

  def auth_method_name(method) do
    case method do
      0 -> "OK"
      1 -> "PASSWORD"
      2 -> "MD5"
      3 -> "SCRAM_SHA_256"
      4 -> "SCRAM_SHA_512"
      5 -> "TOKEN"
      6 -> "PEER"
      7 -> "REATTACH"
      _ -> nil
    end
  end

  def auth_plugin_id_for_method(method, configured_method_id \\ nil) do
    configured_method_id =
      configured_method_id
      |> to_string()
      |> String.trim()

    if configured_method_id != "" do
      configured_method_id
    else
      case method do
        0 -> "scratchbird.auth.none"
        1 -> "scratchbird.auth.password_compat"
        2 -> "scratchbird.auth.md5_legacy"
        3 -> "scratchbird.auth.scram_sha_256"
        4 -> "scratchbird.auth.scram_sha_512"
        5 -> "scratchbird.auth.authkey_token"
        6 -> "scratchbird.auth.peer_uid"
        7 -> "scratchbird.auth.reattach"
        _ -> nil
      end
    end
  end

  def auth_method_executable_locally?(method) do
    method in [
      Protocol.auth_method(:password),
      Protocol.auth_method(:scram_sha_256),
      Protocol.auth_method(:scram_sha_512),
      Protocol.auth_method(:token)
    ]
  end

  def auth_method_broker_required?(method) do
    method == Protocol.auth_method(:peer)
  end

  def additional_continuation_possible?(method) do
    method in [
      Protocol.auth_method(:scram_sha_256),
      Protocol.auth_method(:scram_sha_512),
      Protocol.auth_method(:token),
      Protocol.auth_method(:peer)
    ]
  end

  def describe_auth_method(method, configured_method_id \\ nil) do
    case auth_method_name(method) do
      nil ->
        nil

      wire_method ->
        %AuthMethodSurface{
          wire_method: wire_method,
          plugin_method_id: auth_plugin_id_for_method(method, configured_method_id),
          executable_locally: auth_method_executable_locally?(method),
          broker_required: auth_method_broker_required?(method)
        }
    end
  end

  def default_resolved_auth_context(ingress_mode \\ "direct") do
    %ResolvedAuthContext{
      ingress_mode: if(ingress_mode in [nil, ""], do: "direct", else: ingress_mode),
      resolved_auth_method: nil,
      resolved_auth_plugin_id: nil,
      manager_authenticated: false,
      attached: false
    }
  end

  def resolve_token_auth_payload(config) do
    config[:auth_token] ||
      config[:auth_method_payload] ||
      decode_auth_payload_b64(config[:auth_payload_b64]) ||
      config[:auth_payload_json] ||
      config[:workload_identity_token] ||
      config[:proxy_principal_assertion]
  end

  defp decode_auth_payload_b64(nil), do: nil
  defp decode_auth_payload_b64(""), do: nil

  defp decode_auth_payload_b64(value) do
    case Base.decode64(to_string(value)) do
      {:ok, decoded} -> decoded
      :error -> raise ArgumentError, "invalid auth_payload_b64"
    end
  end
end
