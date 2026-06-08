# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.Scram do
  @moduledoc false

  defstruct [:username, :client_nonce, :client_first_bare, :server_signature, algorithm: :sha256]

  def new(username, algorithm \\ :sha256) do
    nonce = :crypto.strong_rand_bytes(18) |> Base.encode64()
    %__MODULE__{username: username, client_nonce: nonce, algorithm: algorithm}
  end

  def client_first(%__MODULE__{username: username, client_nonce: nonce} = state) do
    bare = "n=#{escape(username)},r=#{nonce}"
    {"n,," <> bare, %{state | client_first_bare: bare}}
  end

  def handle_server_first(%__MODULE__{} = state, password, server_first) do
    attrs = parse_attrs(server_first)
    nonce = Map.get(attrs, "r", "")
    salt = Map.get(attrs, "s", "")
    iter = Map.get(attrs, "i", "0")

    if nonce == "" or not String.starts_with?(nonce, state.client_nonce) do
      {:error, "SCRAM server nonce mismatch"}
    else
      iterations = String.to_integer(iter)
      salt_bin = Base.decode64!(salt)
      key_len = if state.algorithm == :sha512, do: 64, else: 32
      salted = pbkdf2(password, salt_bin, iterations, key_len, state.algorithm)
      client_key = hmac(state.algorithm, salted, "Client Key")
      stored_key = :crypto.hash(state.algorithm, client_key)
      client_final = "c=biws,r=" <> nonce
      auth_message = state.client_first_bare <> "," <> server_first <> "," <> client_final
      client_sig = hmac(state.algorithm, stored_key, auth_message)
      client_proof = xor_bytes(client_key, client_sig)
      server_key = hmac(state.algorithm, salted, "Server Key")
      server_sig = hmac(state.algorithm, server_key, auth_message)
      response = client_final <> ",p=" <> Base.encode64(client_proof)
      {:ok, response, %{state | server_signature: server_sig}}
    end
  end

  def verify_server_final(%__MODULE__{server_signature: sig}, server_final) do
    attrs = parse_attrs(server_final)
    verifier = Map.get(attrs, "v")
    expected = Base.encode64(sig || <<>>)
    if verifier == expected, do: :ok, else: {:error, "SCRAM server signature mismatch"}
  end

  defp escape(text) do
    text
    |> String.replace("=", "=3D")
    |> String.replace(",", "=2C")
  end

  defp parse_attrs(message) do
    message
    |> String.split(",", trim: true)
    |> Enum.reduce(%{}, fn part, acc ->
      case String.split(part, "=", parts: 2) do
        [k, v] -> Map.put(acc, k, v)
        _ -> acc
      end
    end)
  end

  defp hmac(algorithm, key, data) when is_binary(data) do
    :crypto.mac(:hmac, algorithm, key, data)
  end

  defp hmac(algorithm, key, data) when is_list(data) do
    :crypto.mac(:hmac, algorithm, key, to_string(data))
  end

  defp xor_bytes(left, right) do
    left
    |> :binary.bin_to_list()
    |> Enum.zip(:binary.bin_to_list(right))
    |> Enum.map(fn {l, r} -> Bitwise.bxor(l, r) end)
    |> :binary.list_to_bin()
  end

  defp pbkdf2(password, salt, iterations, key_len, algorithm) do
    :crypto.pbkdf2_hmac(algorithm, password, salt, iterations, key_len)
  end
end
