# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBirdIntegrationTest do
  use ExUnit.Case

  alias ScratchBird.Connection

  @dsn System.get_env("SCRATCHBIRD_TEST_DSN")
  @skip_reason if is_binary(@dsn) and String.trim(@dsn) != "",
                 do: nil,
                 else: "SCRATCHBIRD_TEST_DSN not set"

  if @skip_reason do
    @moduletag skip: @skip_reason
  end

  test "connect and basic query" do
    {:ok, conn} = Connection.connect(url: @dsn)

    try do
      {:ok, result, _conn} = Connection.query(conn, "SELECT 1", [])
      assert length(result.rows) > 0
      assert length(List.first(result.rows)) > 0
      assert List.first(List.first(result.rows)) == 1
    after
      Connection.close(conn)
    end
  end

  test "parameterized query" do
    {:ok, conn} = Connection.connect(url: @dsn)

    try do
      {:ok, result, _conn} = Connection.query(conn, "SELECT ?::INTEGER", [42])
      assert length(result.rows) > 0
      assert length(List.first(result.rows)) > 0
      assert List.first(List.first(result.rows)) == 42
    after
      Connection.close(conn)
    end
  end

  test "rollback leaves immediate query usable on native always-in-transaction boundary" do
    {:ok, conn} = Connection.connect(url: @dsn)

    try do
      assert conn.txn_id > 0

      {:ok, conn} = Connection.rollback(conn)
      assert conn.txn_id > 0

      {:ok, baseline_result, _conn} = Connection.query(conn, "SELECT 1", [])

      assert length(baseline_result.rows) == 1
      assert length(List.first(baseline_result.rows)) == 1
      assert List.first(List.first(baseline_result.rows)) == 1
    after
      Connection.close(conn)
    end
  end
end

defmodule ScratchBirdManagerProxyIntegrationTest do
  use ExUnit.Case

  alias ScratchBird.Connection

  @dsn System.get_env("SCRATCHBIRD_TEST_MANAGER_DSN")
  @skip_reason if is_binary(@dsn) and String.trim(@dsn) != "",
                 do: nil,
                 else: "SCRATCHBIRD_TEST_MANAGER_DSN not set"

  if @skip_reason do
    @moduletag skip: @skip_reason
  end

  test "manager-proxy connect and basic query" do
    {:ok, conn} = Connection.connect(url: @dsn)

    try do
      assert conn.config[:front_door_mode] == "manager_proxy"
      {:ok, result, _conn} = Connection.query(conn, "SELECT 1", [])
      assert length(result.rows) > 0
      assert length(List.first(result.rows)) > 0
      assert List.first(List.first(result.rows)) == 1
    after
      Connection.close(conn)
    end
  end
end
