# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "test_helper"

class TestWireTxnExec < Minitest::Test
  def test_query_resumes_portal_and_continues_rows
    messages = [
      msg(Scratchbird::Protocol::MSG_ROW_DESCRIPTION, :row_desc),
      msg(Scratchbird::Protocol::MSG_DATA_ROW, [1]),
      msg(Scratchbird::Protocol::MSG_PORTAL_SUSPENDED, :suspended),
      msg(Scratchbird::Protocol::MSG_DATA_ROW, [2]),
      msg(Scratchbird::Protocol::MSG_COMMAND_COMPLETE, :command_complete),
      msg(Scratchbird::Protocol::MSG_READY, [1, 19, 0])
    ]
    client, sent = build_client_with_script(messages)
    client.define_singleton_method(:decode_row) { |_columns, values| values }

    with_protocol_parse_stubs(
      parse_row_description: ->(_payload) { [{ name: "value", type_oid: 0, format: 0 }] },
      parse_data_row: ->(payload) { payload },
      parse_command_complete: ->(_payload) { ["SELECT", 2, 0, "SELECT 2"] },
      parse_ready: ->(payload) { payload }
    ) do
      result = client.query("SELECT 1", nil, max_rows: 1)
      assert_equal [[1], [2]], result.rows
      assert_equal 2, result.rowcount
      assert_equal 19, client.txn_id
      assert_equal 1, sent.count { |entry| entry[0] == Scratchbird::Protocol::MSG_QUERY }
      assert_equal 1, sent.count { |entry| entry[0] == Scratchbird::Protocol::MSG_EXECUTE }
    end
  end

  def test_query_multi_handles_single_request_multi_result_framing
    messages = [
      msg(Scratchbird::Protocol::MSG_ROW_DESCRIPTION, :row_desc_first),
      msg(Scratchbird::Protocol::MSG_DATA_ROW, [1]),
      msg(Scratchbird::Protocol::MSG_COMMAND_COMPLETE, :command_first),
      msg(Scratchbird::Protocol::MSG_ROW_DESCRIPTION, :row_desc_second),
      msg(Scratchbird::Protocol::MSG_DATA_ROW, [2]),
      msg(Scratchbird::Protocol::MSG_COMMAND_COMPLETE, :command_second),
      msg(Scratchbird::Protocol::MSG_READY, [0, 0, 0])
    ]
    client, sent = build_client_with_script(messages)
    client.define_singleton_method(:decode_row) { |_columns, values| values }

    with_protocol_parse_stubs(
      parse_row_description: lambda { |payload|
        if payload == :row_desc_first
          [{ name: "first_value", type_oid: 0, format: 0 }]
        else
          [{ name: "second_value", type_oid: 0, format: 0 }]
        end
      },
      parse_data_row: ->(payload) { payload },
      parse_command_complete: lambda { |payload|
        if payload == :command_first
          ["SELECT", 1, 0, "SELECT 1"]
        else
          ["SELECT", 1, 0, "SELECT 1"]
        end
      },
      parse_ready: ->(payload) { payload }
    ) do
      result_sets = client.query_multi("SELECT 1 AS first_value; SELECT 2 AS second_value")
      assert_equal 2, result_sets.length
      assert_equal [[1]], result_sets[0].rows
      assert_equal [[2]], result_sets[1].rows
      assert_equal 1, sent.count { |entry| entry[0] == Scratchbird::Protocol::MSG_QUERY }
    end
  end

  def test_deallocate_waits_for_close_complete_then_ready
    messages = [
      msg(Scratchbird::Protocol::MSG_CLOSE_COMPLETE, :close_complete),
      msg(Scratchbird::Protocol::MSG_READY, [0, 0, 0])
    ]
    client, sent = build_client_with_script(messages)
    client.instance_variable_set(:@prepared, { "stmt_1" => { sql: "SELECT 1", param_count: 0 } })

    with_protocol_parse_stubs(parse_ready: ->(payload) { payload }) do
      assert_equal true, client.deallocate("stmt_1")
      assert_equal false, client.instance_variable_get(:@prepared).key?("stmt_1")
      assert_equal(
        [Scratchbird::Protocol::MSG_CLOSE, Scratchbird::Protocol::MSG_SYNC],
        sent.map { |entry| entry[0] }
      )
    end
  end

  def test_txn_id_transitions_follow_ready_frames
    messages = [
      msg(Scratchbird::Protocol::MSG_READY, [1, 55, 0]),
      msg(Scratchbird::Protocol::MSG_READY, [0, 0, 0]),
      msg(Scratchbird::Protocol::MSG_READY, [1, 66, 0]),
      msg(Scratchbird::Protocol::MSG_READY, [0, 0, 0])
    ]
    client, sent = build_client_with_script(messages)

    with_protocol_parse_stubs(parse_ready: ->(payload) { payload }) do
      client.begin_transaction
      assert_equal 55, client.txn_id
      client.commit
      assert_equal 0, client.txn_id
      client.begin_transaction
      assert_equal 66, client.txn_id
      client.rollback
      assert_equal 0, client.txn_id
    end

    assert_equal(
      [
        Scratchbird::Protocol::MSG_TXN_BEGIN,
        Scratchbird::Protocol::MSG_TXN_COMMIT,
        Scratchbird::Protocol::MSG_TXN_BEGIN,
        Scratchbird::Protocol::MSG_TXN_ROLLBACK
      ],
      sent.map { |entry| entry[0] }
    )
  end

  def test_ready_status_can_report_active_transaction_with_zero_txn_id
    messages = [msg(Scratchbird::Protocol::MSG_READY, [1, 0, 0])]
    client, _sent = build_client_with_script(messages)

    with_protocol_parse_stubs(parse_ready: ->(payload) { payload }) do
      client.send(:drain_until_ready)
      assert_equal true, client.in_transaction?
      assert_equal 0, client.txn_id
    end
  end

  def test_begin_transaction_encodes_read_committed_mode
    messages = [msg(Scratchbird::Protocol::MSG_READY, [1, 55, 0])]
    client, sent = build_client_with_script(messages)

    with_protocol_parse_stubs(parse_ready: ->(payload) { payload }) do
      client.begin_transaction(
        isolation_level: Scratchbird::Protocol::ISOLATION_READ_COMMITTED,
        read_committed_mode: Scratchbird::Protocol::READ_COMMITTED_MODE_READ_CONSISTENCY
      )
    end

    payload = sent.first[1]
    flags = payload.byteslice(0, 2).unpack1("v")
    assert_equal 16, payload.bytesize
    assert_equal Scratchbird::Protocol::TXN_FLAG_HAS_READ_COMMITTED_MODE,
                 flags & Scratchbird::Protocol::TXN_FLAG_HAS_READ_COMMITTED_MODE
    assert_equal Scratchbird::Protocol::READ_COMMITTED_MODE_READ_CONSISTENCY, payload.getbyte(12)
    assert_equal "READ COMMITTED READ CONSISTENCY",
                 Scratchbird::Protocol.canonical_read_committed_mode_label(payload.getbyte(12))
  end

  def test_begin_transaction_rejects_read_committed_mode_with_snapshot_alias
    client, _sent = build_client_with_script([])

    err = assert_raises(Scratchbird::NotSupportedError) do
      client.begin_transaction(
        isolation_level: Scratchbird::Protocol::ISOLATION_SERIALIZABLE,
        read_committed_mode: Scratchbird::Protocol::READ_COMMITTED_MODE_READ_CONSISTENCY
      )
    end

    assert_equal "read_committed_mode requires a READ COMMITTED isolation alias", err.message
  end

  def test_prepared_transaction_helpers_emit_canonical_control_sql
    messages = [
      msg(Scratchbird::Protocol::MSG_READY, [0, 0, 0]),
      msg(Scratchbird::Protocol::MSG_READY, [0, 0, 0]),
      msg(Scratchbird::Protocol::MSG_READY, [0, 0, 0])
    ]
    client, sent = build_client_with_script(messages)

    with_protocol_parse_stubs(parse_ready: ->(payload) { payload }) do
      assert_equal true, client.prepare_transaction("gid'alpha")
      assert_equal true, client.commit_prepared("gid'alpha")
      assert_equal true, client.rollback_prepared("gid'alpha")
    end

    assert_equal "PREPARE TRANSACTION 'gid''alpha'", parse_sql_from_query_payload(sent[0][1])
    assert_equal "COMMIT PREPARED 'gid''alpha'", parse_sql_from_query_payload(sent[1][1])
    assert_equal "ROLLBACK PREPARED 'gid''alpha'", parse_sql_from_query_payload(sent[2][1])
  end

  def test_dormant_helpers_fail_closed_and_capabilities_stay_explicit
    client, _sent = build_client_with_script([])

    assert_equal true, client.supports_prepared_transactions?
    assert_equal false, client.supports_dormant_reattach?

    err = assert_raises(Scratchbird::NotSupportedError) { client.detach_to_dormant }
    assert_equal "dormant detach/reattach is not yet exposed by the public Ruby driver surface", err.message
  end

  def test_resume_portal_requires_explicit_suspended_state
    client, _sent = build_client_with_script([])
    client.instance_variable_set(:@last_max_rows, 1)

    err = assert_raises(Scratchbird::Error) { client.send(:resume_portal) }
    assert_equal "55000", err.sqlstate
  end

  def test_commit_raises_mapped_error_but_applies_ready_state_after_abort
    messages = [
      msg(Scratchbird::Protocol::MSG_ERROR, :txn_abort),
      msg(Scratchbird::Protocol::MSG_READY, [0, 0, 0])
    ]
    client, _sent = build_client_with_script(messages)
    client.update_txn_id(99)

    with_protocol_parse_stubs(
      parse_error_message: ->(_payload) { ["ERROR", "40001", "transaction aborted", "forced abort", "retry"] },
      parse_ready: ->(payload) { payload }
    ) do
      err = assert_raises(Scratchbird::TransactionError) { client.commit }
      assert_equal "40001", err.sqlstate
      assert_equal 0, client.txn_id
    end
  end

  private

  def build_client_with_script(messages)
    cfg = Scratchbird::Config.new
    cfg.user = "tester"
    cfg.database = "testdb"
    client = Scratchbird::Client.new(cfg)
    client.instance_variable_set(:@connected, true)
    sent = []
    queue = messages.dup

    client.define_singleton_method(:send_message) do |type, payload, flags, force_zero|
      sent << [type, payload, flags, force_zero]
      sent.length
    end
    client.define_singleton_method(:recv_message) do
      raise "no queued messages" if queue.empty?
      queue.shift
    end
    client.define_singleton_method(:with_resilience) do |_operation, _sql = nil, &block|
      block.call
    end
    [client, sent]
  end

  def msg(type, payload)
    [type, 0, payload, 1, ("\0" * 16), 0]
  end

  def parse_sql_from_query_payload(payload)
    sql_bytes = payload.byteslice(12, payload.bytesize - 12)
    sql_bytes.sub(/\x00\z/, "")
  end

  def with_protocol_parse_stubs(stubs)
    singleton = Scratchbird::Protocol.singleton_class
    originals = {}
    stubs.each do |name, proc|
      originals[name] = Scratchbird::Protocol.method(name)
      singleton.send(:define_method, name, &proc)
    end
    yield
  ensure
    originals.each do |name, original|
      singleton.send(:define_method, name, original)
    end
  end
end
