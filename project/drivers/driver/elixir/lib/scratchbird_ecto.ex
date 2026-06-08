# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.Ecto do
  use Ecto.Adapters.SQL, driver: ScratchBird.Ecto.Connection

  @impl true
  def supports_ddl_transaction?, do: true

  def supports_ddl? do
    true
  end

  @impl true
  def lock_for_migrations(_meta, _opts, fun) do
    fun.()
  end

  @impl true
  def dumpers({:map, _}, type), do: [&Jason.encode!/1, type]
  def dumpers(:binary_id, type), do: [type, Ecto.UUID]
  def dumpers(_, type), do: [type]

  @impl true
  def loaders({:map, _}, type), do: [&json_decode/1, &Ecto.Type.embedded_load(type, &1, :json)]
  def loaders(:map, type), do: [&json_decode/1, type]
  def loaders(:binary_id, type), do: [Ecto.UUID, type]
  def loaders(_, type), do: [type]

  defp json_decode(x) when is_binary(x), do: {:ok, Jason.decode!(x)}
  defp json_decode(x), do: {:ok, x}
end
