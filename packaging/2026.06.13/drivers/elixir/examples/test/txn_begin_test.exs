# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBirdTxnBeginTest do
  use ExUnit.Case
  import Bitwise

  alias ScratchBird.{Connection, Protocol}

  test "protocol payload expands for read_committed_mode" do
    flags =
      Protocol.txn_flag(:has_isolation) |||
        Protocol.txn_flag(:has_read_committed_mode)

    payload =
      Protocol.build_txn_begin_payload(
        flags,
        0,
        0,
        Protocol.isolation(:read_committed),
        0,
        0,
        0,
        0,
        Protocol.read_committed_mode(:read_consistency)
      )

    assert byte_size(payload) == 16
    assert :binary.at(payload, 12) == Protocol.read_committed_mode(:read_consistency)

    assert ScratchBird.canonical_read_committed_mode_label(:binary.at(payload, 12)) ==
             "READ COMMITTED READ CONSISTENCY"
  end

  test "begin rejects read_committed_mode with snapshot alias" do
    assert_raise ArgumentError, "read_committed_mode requires a READ COMMITTED isolation alias", fn ->
      Connection.begin(%Connection{}, %{
        isolation_level: Protocol.isolation(:serializable),
        read_committed_mode: Protocol.read_committed_mode(:read_consistency)
      })
    end
  end

  test "begin adopts a compatible fresh native boundary" do
    state = %Connection{runtime_boundary_seen: true, runtime_txn_active: true, txn_id: 0}

    assert {:ok, adopted_state} = Connection.begin(state)
    assert adopted_state.txn_id == 0
    assert adopted_state.runtime_txn_active
    assert adopted_state.runtime_boundary_seen
  end

  test "begin rejects non-default fresh native boundary adoption" do
    state = %Connection{runtime_boundary_seen: true, runtime_txn_active: true, txn_id: 0}

    assert {:error,
            %{
              sqlstate: "0A000",
              message: "non-default begin options cannot adopt an already-active fresh native boundary"
            },
            rejected_state} =
             Connection.begin(state, %{isolation_level: Protocol.isolation(:serializable)})
    assert rejected_state.txn_id == 0
    assert rejected_state.runtime_txn_active
    assert rejected_state.runtime_boundary_seen
  end

  test "begin rejects nested explicit transaction state" do
    state = %Connection{runtime_boundary_seen: true, runtime_txn_active: true, txn_id: 42}

    assert {:error, %{sqlstate: "25001", message: "transaction already active"}, rejected_state} =
             Connection.begin(state)
    assert rejected_state.txn_id == 42
    assert rejected_state.runtime_txn_active
    assert rejected_state.runtime_boundary_seen
  end

  test "prepared transaction SQL builder emits canonical control SQL" do
    assert {:ok, "PREPARE TRANSACTION 'gid-1'"} ==
             Connection.build_prepared_transaction_sql("PREPARE TRANSACTION", "gid-1")

    assert {:ok, "COMMIT PREPARED 'gid-1'"} ==
             Connection.build_prepared_transaction_sql("COMMIT PREPARED", "gid-1")

    assert {:ok, "ROLLBACK PREPARED 'gid''2'"} ==
             Connection.build_prepared_transaction_sql("ROLLBACK PREPARED", "gid'2")
  end

  test "prepared transaction SQL builder rejects blank global transaction id" do
    assert {:error, %{sqlstate: "42601", message: "Global transaction id is required"}} ==
             Connection.build_prepared_transaction_sql("PREPARE TRANSACTION", "   ")
  end

  test "prepared and dormant capability surfaces stay explicit" do
    state = %Connection{}

    assert Connection.supports_prepared_transactions()
    refute Connection.supports_dormant_reattach()
    refute Connection.supports_portal_resume()

    assert ScratchBird.supports_prepared_transactions()
    refute ScratchBird.supports_dormant_reattach()
    refute ScratchBird.supports_portal_resume()

    assert {:error, %{sqlstate: "42601", message: "Global transaction id is required"}, ^state} =
             Connection.prepare_transaction(state, "   ")

    assert {:error,
            %{sqlstate: "0A000", message: "Dormant detach is not supported by the current public front door"},
            ^state} = Connection.detach_to_dormant(state)

    assert {:error,
            %{sqlstate: "0A000", message: "Dormant reattach is not supported by the current public front door"},
            ^state} = Connection.reattach_dormant(state, "dormant-1", "token")
  end
end
