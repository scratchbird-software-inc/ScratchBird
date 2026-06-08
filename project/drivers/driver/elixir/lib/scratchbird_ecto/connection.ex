# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.Ecto.Connection do
  @behaviour DBConnection

  alias ScratchBird.Connection
  alias Ecto.Adapters.Postgres.Connection, as: PostgresConnection

  defstruct [:conn, :config, in_transaction: false]

  @impl true
  def connect(opts) do
    case Connection.connect(opts) do
      {:ok, conn} -> {:ok, %__MODULE__{conn: conn, config: opts}}
      {:error, reason} -> {:error, reason}
    end
  end

  @impl true
  def disconnect(_err, state) do
    # DBConnection reconnects this lane by calling connect/1 for a new state.
    # We intentionally tear down the current wire/session here instead of
    # preserving any local transaction claim across the disconnect boundary.
    _ = Connection.close(state.conn)
    :ok
  end

  @impl true
  def checkout(state), do: {:ok, state}

  def checkin(state), do: {:ok, state}

  def all(query), do: PostgresConnection.all(query)
  def all(query, as_prefix), do: PostgresConnection.all(query, as_prefix)

  def update_all(query), do: PostgresConnection.update_all(query)
  def update_all(query, prefix), do: PostgresConnection.update_all(query, prefix)

  def delete_all(query), do: PostgresConnection.delete_all(query)

  def insert(prefix, table, header, rows, on_conflict, returning, placeholders) do
    PostgresConnection.insert(prefix, table, header, rows, on_conflict, returning, placeholders)
  end

  def update(prefix, table, fields, filters, returning) do
    PostgresConnection.update(prefix, table, fields, filters, returning)
  end

  def delete(prefix, table, filters, returning) do
    PostgresConnection.delete(prefix, table, filters, returning)
  end

  @impl true
  def ping(state) do
    case Connection.ping(state.conn) do
      {:ok, conn} -> {:ok, %{state | conn: conn}}
      {:error, reason, conn} -> {:disconnect, to_db_error(reason), %{state | conn: conn}}
      {:error, reason} -> {:disconnect, to_db_error(reason), state}
    end
  end

  @impl true
  def handle_begin(_opts, state), do: {:ok, :begun, %{state | in_transaction: true}}

  @impl true
  def handle_commit(_opts, state) do
    case Connection.commit(state.conn) do
      {:ok, _result, conn} -> {:ok, :committed, %{state | conn: conn, in_transaction: false}}
      {:error, reason, conn} -> {:error, to_db_error(reason), %{state | conn: conn}}
    end
  end

  @impl true
  def handle_rollback(_opts, state) do
    case Connection.rollback(state.conn) do
      {:ok, _result, conn} -> {:ok, :rolled_back, %{state | conn: conn, in_transaction: false}}
      {:error, reason, conn} -> {:error, to_db_error(reason), %{state | conn: conn}}
    end
  end

  @impl true
  def handle_prepare(query, _opts, state) do
    {:ok, query, state}
  end

  @impl true
  def handle_execute(query, params, _opts, state) do
    sql = query_statement(query)

    case Connection.query(state.conn, sql, params) do
      {:ok, result, conn} ->
        {:ok, query, to_db_result(result), %{state | conn: conn}}

      {:error, reason, conn} ->
        {:error, to_db_error(reason), %{state | conn: conn}}
    end
  end

  @impl true
  def handle_close(_query, _opts, state), do: {:ok, nil, state}

  @impl true
  def handle_declare(query, params, _opts, state) do
    sql = query_statement(query)

    case Connection.query(state.conn, sql, params) do
      {:ok, result, conn} ->
        cursor = %{
          columns: result.columns || [],
          rows: result.rows || [],
          offset: 0
        }

        {:ok, query, cursor, %{state | conn: conn}}

      {:error, reason, conn} ->
        {:error, to_db_error(reason), %{state | conn: conn}}
    end
  end

  @impl true
  def handle_fetch(_query, cursor, _opts, state) do
    result = %{
      columns: map_column_names(cursor.columns),
      rows: cursor.rows,
      num_rows: length(cursor.rows)
    }

    {:halt, result, state}
  end

  @impl true
  def handle_deallocate(_query, _cursor, _opts, state), do: {:ok, nil, state}

  @impl true
  def handle_status(_opts, state) do
    status = if state.in_transaction, do: :transaction, else: :idle
    {status, state}
  end

  def handle_info(_msg, state), do: {:noreply, state}

  defp query_statement(query) do
    cond do
      is_map(query) and Map.has_key?(query, :statement) -> Map.fetch!(query, :statement)
      is_map(query) and Map.has_key?(query, "statement") -> Map.fetch!(query, "statement")
      true -> raise ArgumentError, "query must expose a statement field"
    end
  end

  defp to_db_result(result) do
    %{
      columns: map_column_names(result.columns || []),
      rows: result.rows || [],
      num_rows: length(result.rows || [])
    }
  end

  defp map_column_names(columns) do
    Enum.map(columns, fn
      %{name: name} -> name
      %{column_name: name} -> name
      name when is_binary(name) -> name
      other -> to_string(other)
    end)
  end

  defp to_db_error(reason) do
    message =
      case reason do
        %{message: msg} -> msg
        _ -> "ScratchBird query failed"
      end

    %DBConnection.ConnectionError{message: message}
  end
end
