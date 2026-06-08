# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBirdEctoAdapterTest do
  use ExUnit.Case

  import Ecto.Query

  alias ScratchBird.Connection

  defmodule ExampleSchema do
    use Ecto.Schema

    @primary_key {:id, :id, []}
    schema "example_records" do
      field :name, :string
    end
  end

  test "prepare all generates executable SQL" do
    query =
      from record in ExampleSchema,
        where: record.name == ^"alpha",
        select: record.name,
        limit: 1

    {planned_query, _cast_params, _dump_params} =
      Ecto.Adapter.Queryable.plan_query(:all, ScratchBird.Ecto, query)

    assert {:cache, {_cache_key, sql}} = ScratchBird.Ecto.prepare(:all, planned_query)
    assert sql =~ "SELECT"
    assert sql =~ "FROM \"example_records\" AS"
    assert sql =~ "WHERE"
    assert sql =~ "$1"
  end

  test "prepare update_all generates executable SQL" do
    query =
      from(record in ExampleSchema,
        where: record.id == ^1,
        update: [set: [name: "beta"]]
      )

    {planned_query, _cast_params, _dump_params} =
      Ecto.Adapter.Queryable.plan_query(:update_all, ScratchBird.Ecto, query)

    assert {:cache, {_cache_key, sql}} = ScratchBird.Ecto.prepare(:update_all, planned_query)
    assert sql =~ "UPDATE \"example_records\" AS"
    assert sql =~ "SET"
    assert sql =~ "WHERE"
  end

  test "prepare delete_all generates executable SQL" do
    query =
      from(record in ExampleSchema,
        where: record.id == ^1
      )

    {planned_query, _cast_params, _dump_params} =
      Ecto.Adapter.Queryable.plan_query(:delete_all, ScratchBird.Ecto, query)

    assert {:cache, {_cache_key, sql}} = ScratchBird.Ecto.prepare(:delete_all, planned_query)
    assert sql =~ "DELETE FROM \"example_records\" AS"
    assert sql =~ "WHERE"
  end

  test "connection schema builders remain available" do
    insert_sql =
      ScratchBird.Ecto.Connection.insert(nil, "example_records", [:name], [["gamma"]], {:raise, [], []}, [], [])
      |> IO.iodata_to_binary()

    update_sql =
      ScratchBird.Ecto.Connection.update(nil, "example_records", [:name], [id: 1], [])
      |> IO.iodata_to_binary()

    delete_sql =
      ScratchBird.Ecto.Connection.delete(nil, "example_records", [id: 1], [])
      |> IO.iodata_to_binary()

    assert insert_sql =~ "INSERT INTO \"example_records\""
    assert update_sql =~ "UPDATE \"example_records\""
    assert delete_sql =~ "DELETE FROM \"example_records\""
  end

  test "disconnect closes the current wire and leaves replacement to fresh connect" do
    {:ok, listener} = :gen_tcp.listen(0, [:binary, packet: :raw, active: false, reuseaddr: true, ip: {127, 0, 0, 1}])
    {:ok, port} = :inet.port(listener)
    parent = self()

    acceptor =
      spawn_link(fn ->
        {:ok, accepted} = :gen_tcp.accept(listener)
        send(parent, {:accepted_socket, accepted})

        receive do
          :stop -> :gen_tcp.close(accepted)
        end
      end)

    {:ok, socket} = :gen_tcp.connect({127, 0, 0, 1}, port, [:binary, active: false])
    assert_receive {:accepted_socket, _accepted_socket}

    conn = %Connection{
      transport: :tcp,
      socket: socket,
      txn_id: 77,
      authed: true,
      attachment_id: <<1::128>>
    }

    state = %ScratchBird.Ecto.Connection{conn: conn, config: [], in_transaction: true}

    assert :ok == ScratchBird.Ecto.Connection.disconnect(:network_error, state)
    assert {:error, :closed} = :gen_tcp.send(socket, "x")

    send(acceptor, :stop)
    :gen_tcp.close(listener)
  end
end
