# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "test_helper"

class TestResourceResilience < Minitest::Test
  class DisconnectFailClient
    attr_reader :disconnect_calls

    def initialize(error)
      @error = error
      @disconnect_calls = 0
    end

    def disconnect
      @disconnect_calls += 1
      raise @error if @disconnect_calls == 1
      true
    end
  end

  class FakeSocket
    attr_reader :close_calls, :writes

    def initialize(error = nil)
      @error = error
      @close_calls = 0
      @writes = []
    end

    def write(data)
      @writes << data
      data.bytesize
    end

    def close
      @close_calls += 1
      raise @error if @error
      true
    end
  end

  class FakeThread
    attr_reader :raised

    def initialize(alive: true)
      @alive = alive
      @raised = []
    end

    def alive?
      @alive
    end

    def raise(error)
      @raised << error
      true
    end
  end

  class FakeKeepaliveManager
    attr_reader :unregistered_ids, :stop_calls

    def initialize
      @running = true
      @unregistered_ids = []
      @stop_calls = 0
    end

    def unregister(connection_id)
      @unregistered_ids << connection_id
      true
    end

    def stop
      return unless @running
      @running = false
      @stop_calls += 1
      true
    end
  end

  class FakeLeakDetector
    attr_reader :stop_calls

    def initialize
      @running = true
      @stop_calls = 0
    end

    def stop
      return unless @running
      @running = false
      @stop_calls += 1
      true
    end
  end

  class FakeLeakGuard
    attr_reader :release_calls

    def initialize(error = nil)
      @error = error
      @release_calls = 0
    end

    def release
      @release_calls += 1
      raise @error if @error
      true
    end
  end

  class FakeTracker
    attr_reader :mark_active_calls

    def initialize(needs_validation: false)
      @needs_validation = needs_validation
      @mark_active_calls = 0
    end

    def needs_validation?
      @needs_validation
    end

    def mark_active
      @mark_active_calls += 1
      true
    end
  end

  class FakeCircuitBreaker
    attr_reader :allow_request_calls, :record_success_calls, :record_failure_calls

    def initialize(allow: true)
      @allow = allow
      @allow_request_calls = 0
      @record_success_calls = 0
      @record_failure_calls = 0
    end

    def allow_request?
      @allow_request_calls += 1
      @allow
    end

    def record_success
      @record_success_calls += 1
      true
    end

    def record_failure
      @record_failure_calls += 1
      true
    end
  end

  class FakeSpan
    attr_reader :name, :attributes

    def initialize(name)
      @name = name
      @attributes = {}
    end

    def with_attribute(key, value)
      @attributes[key] = value
      self
    end
  end

  class FakeTelemetry
    attr_reader :started_spans, :ended_spans

    def initialize
      @started_spans = []
      @ended_spans = []
    end

    def start_span(name)
      span = FakeSpan.new(name)
      @started_spans << span
      span
    end

    def end_span(span, success)
      @ended_spans << {
        name: span&.name,
        success: success,
        attributes: span ? span.attributes.dup : {}
      }
      true
    end
  end

  def test_connection_close_marks_closed_when_disconnect_raises_once
    client = DisconnectFailClient.new(Scratchbird::ConnectionError.new("disconnect failed"))
    conn = Scratchbird::Connection.allocate
    conn.instance_variable_set(:@client, client)
    conn.instance_variable_set(:@autocommit, true)
    conn.instance_variable_set(:@closed, false)
    conn.instance_variable_set(:@config, Scratchbird::Config.new)

    err = assert_raises(Scratchbird::ConnectionError) { conn.close }
    assert_equal "disconnect failed", err.message
    assert_equal true, conn.closed?

    conn.close
    assert_equal 1, client.disconnect_calls
  end

  def test_statement_close_is_idempotent_when_close_prepared_raises
    client = Object.new
    close_calls = []
    client.define_singleton_method(:prepare) do |_name, _sql|
      true
    end

    connection = Object.new
    connection.define_singleton_method(:client) { client }
    connection.define_singleton_method(:close_prepared) do |name|
      close_calls << name
      raise Scratchbird::ConnectionError, "deallocate failed"
    end

    statement = Scratchbird::Statement.new(connection, "SELECT 1")
    statement.close
    statement.close

    assert_equal true, statement.closed?
    assert_equal 1, close_calls.length
  end

  def test_client_close_cleans_resilience_helpers_when_socket_absent
    client = Scratchbird::Client.new(Scratchbird::Config.new)
    keepalive_manager = FakeKeepaliveManager.new
    leak_detector = FakeLeakDetector.new
    leak_guard = FakeLeakGuard.new
    tracker = FakeTracker.new

    client.instance_variable_set(:@socket, nil)
    client.instance_variable_set(:@connected, true)
    client.instance_variable_set(:@connection_id, "conn-res")
    client.instance_variable_set(:@keepalive_manager, keepalive_manager)
    client.instance_variable_set(:@keepalive_tracker, tracker)
    client.instance_variable_set(:@leak_detector, leak_detector)
    client.instance_variable_set(:@leak_guard, leak_guard)

    assert_equal true, client.close
    assert_equal false, client.connected?
    assert_nil client.instance_variable_get(:@keepalive_tracker)
    assert_nil client.instance_variable_get(:@leak_guard)
    assert_equal ["conn-res"], keepalive_manager.unregistered_ids
    assert_equal 1, keepalive_manager.stop_calls
    assert_equal 1, leak_guard.release_calls
    assert_equal 1, leak_detector.stop_calls
  end

  def test_client_close_is_idempotent_when_socket_close_raises
    client = Scratchbird::Client.new(Scratchbird::Config.new)
    socket = FakeSocket.new(IOError.new("socket already closed"))
    keepalive_manager = FakeKeepaliveManager.new
    leak_detector = FakeLeakDetector.new
    leak_guard = FakeLeakGuard.new
    tracker = FakeTracker.new

    client.instance_variable_set(:@socket, socket)
    client.instance_variable_set(:@connected, true)
    client.instance_variable_set(:@connection_id, "conn-idempotent")
    client.instance_variable_set(:@keepalive_manager, keepalive_manager)
    client.instance_variable_set(:@keepalive_tracker, tracker)
    client.instance_variable_set(:@leak_detector, leak_detector)
    client.instance_variable_set(:@leak_guard, leak_guard)

    assert_equal true, client.close
    assert_equal true, client.close
    assert_equal 1, socket.close_calls
    assert_equal 1, keepalive_manager.unregistered_ids.length
    assert_equal 1, keepalive_manager.stop_calls
    assert_equal 1, leak_guard.release_calls
    assert_equal 1, leak_detector.stop_calls
  end

  def test_client_connect_clears_abandoned_session_state_on_same_instance_reuse
    config = Scratchbird::Config.new
    config.user = "me"
    config.database = "db"
    client = Scratchbird::Client.new(config)
    stale_socket = FakeSocket.new
    fresh_socket = FakeSocket.new
    keepalive_manager = FakeKeepaliveManager.new
    leak_detector = FakeLeakDetector.new
    tracker = FakeTracker.new
    leak_guard = FakeLeakGuard.new

    keepalive_manager.define_singleton_method(:start) do
      @start_calls ||= 0
      @start_calls += 1
      true
    end
    keepalive_manager.define_singleton_method(:start_calls) do
      @start_calls || 0
    end
    keepalive_manager.define_singleton_method(:register) do |_connection_id, _owner, &_block|
      @register_calls ||= 0
      @register_calls += 1
      FakeTracker.new
    end
    keepalive_manager.define_singleton_method(:register_calls) do
      @register_calls || 0
    end
    leak_detector.define_singleton_method(:start) do
      @start_calls ||= 0
      @start_calls += 1
      true
    end
    leak_detector.define_singleton_method(:start_calls) do
      @start_calls || 0
    end
    leak_detector.define_singleton_method(:checkout) do |_connection_id, driver:|
      raise "unexpected driver #{driver}" unless driver == "ruby"
      leak_guard
    end

    client.instance_variable_set(:@socket, stale_socket)
    client.instance_variable_set(:@connected, true)
    client.instance_variable_set(:@connection_id, "conn-ruby-reconnect")
    client.instance_variable_set(:@keepalive_manager, keepalive_manager)
    client.instance_variable_set(:@keepalive_tracker, tracker)
    client.instance_variable_set(:@leak_detector, leak_detector)
    client.instance_variable_set(:@leak_guard, leak_guard)
    client.instance_variable_set(:@prepared, { "stmt" => { sql: "select 1", param_count: 0 } })
    client.instance_variable_set(:@parameters, { "attachment_id" => "stale", "current_txn_id" => "77" })
    client.instance_variable_set(:@last_plan, Object.new)
    client.instance_variable_set(:@last_sblr, Object.new)
    client.instance_variable_set(:@transaction_active, true)
    client.instance_variable_set(:@txn_id, 77)

    client.define_singleton_method(:connect_tcp) { fresh_socket }
    client.define_singleton_method(:wrap_tls) { |raw| raw }
    client.define_singleton_method(:handshake) { true }
    client.define_singleton_method(:apply_schema) { true }

    client.connect

    assert_equal 1, stale_socket.close_calls
    assert_equal true, client.connected?
    assert_equal 1, keepalive_manager.start_calls
    assert_equal 1, keepalive_manager.register_calls
    assert_equal 1, leak_detector.start_calls
    assert_equal({}, client.parameters)
    assert_equal({}, client.instance_variable_get(:@prepared))
    assert_nil client.instance_variable_get(:@last_plan)
    assert_nil client.instance_variable_get(:@last_sblr)
    assert_equal false, client.in_transaction?
    assert_equal 0, client.txn_id
    assert_raises(ArgumentError) { client.execute("stmt") }
  end

  def test_with_resilience_success_records_telemetry_and_circuit_success
    client = Scratchbird::Client.new(Scratchbird::Config.new)
    breaker = FakeCircuitBreaker.new
    telemetry = FakeTelemetry.new
    tracker = FakeTracker.new(needs_validation: false)

    client.instance_variable_set(:@circuit_breaker, breaker)
    client.instance_variable_set(:@telemetry, telemetry)
    client.instance_variable_set(:@keepalive_tracker, tracker)

    result = client.send(:with_resilience, "query", "SELECT 'secret'") { :ok }

    assert_equal :ok, result
    assert_equal 1, breaker.allow_request_calls
    assert_equal 1, breaker.record_success_calls
    assert_equal 0, breaker.record_failure_calls
    assert_equal 1, tracker.mark_active_calls
    assert_equal 1, telemetry.started_spans.length
    assert_equal 1, telemetry.ended_spans.length
    assert_equal true, telemetry.ended_spans.first[:success]
    assert_equal "SELECT '?'", telemetry.ended_spans.first[:attributes]["db.statement"]
  end

  def test_with_resilience_failure_records_telemetry_and_circuit_failure
    client = Scratchbird::Client.new(Scratchbird::Config.new)
    breaker = FakeCircuitBreaker.new
    telemetry = FakeTelemetry.new
    tracker = FakeTracker.new(needs_validation: false)

    client.instance_variable_set(:@circuit_breaker, breaker)
    client.instance_variable_set(:@telemetry, telemetry)
    client.instance_variable_set(:@keepalive_tracker, tracker)

    err = assert_raises(RuntimeError) do
      client.send(:with_resilience, "query", "SELECT 1") { raise "boom" }
    end

    assert_equal "boom", err.message
    assert_equal 1, breaker.allow_request_calls
    assert_equal 0, breaker.record_success_calls
    assert_equal 1, breaker.record_failure_calls
    assert_equal 0, tracker.mark_active_calls
    assert_equal 1, telemetry.started_spans.length
    assert_equal 1, telemetry.ended_spans.length
    assert_equal false, telemetry.ended_spans.first[:success]
  end

  def test_with_resilience_runs_ping_when_keepalive_validation_required
    client = Scratchbird::Client.new(Scratchbird::Config.new)
    breaker = FakeCircuitBreaker.new
    telemetry = FakeTelemetry.new
    tracker = FakeTracker.new(needs_validation: true)
    ping_calls = 0
    client.define_singleton_method(:ping) do
      ping_calls += 1
      true
    end

    client.instance_variable_set(:@circuit_breaker, breaker)
    client.instance_variable_set(:@telemetry, telemetry)
    client.instance_variable_set(:@keepalive_tracker, tracker)

    result = client.send(:with_resilience, "query", nil) { :ok }

    assert_equal :ok, result
    assert_equal 1, ping_calls
    assert_equal 2, tracker.mark_active_calls
  end

  def test_with_resilience_raises_when_circuit_is_open
    client = Scratchbird::Client.new(Scratchbird::Config.new)
    breaker = FakeCircuitBreaker.new(allow: false)
    telemetry = FakeTelemetry.new
    tracker = FakeTracker.new(needs_validation: true)

    client.instance_variable_set(:@circuit_breaker, breaker)
    client.instance_variable_set(:@telemetry, telemetry)
    client.instance_variable_set(:@keepalive_tracker, tracker)

    err = assert_raises(Scratchbird::CircuitBreakerOpenError) do
      client.send(:with_resilience, "query", "SELECT 1") { :ok }
    end

    assert_equal "Circuit breaker is OPEN", err.message
    assert_equal 1, breaker.allow_request_calls
    assert_equal 0, telemetry.started_spans.length
    assert_equal 0, tracker.mark_active_calls
  end

  def test_cancel_targets_last_query_sequence_and_interrupts_active_thread
    client = Scratchbird::Client.new(Scratchbird::Config.new)
    socket = FakeSocket.new
    active_thread = FakeThread.new

    client.instance_variable_set(:@socket, socket)
    client.instance_variable_set(:@connected, true)
    client.instance_variable_set(:@last_query_sequence, 77)
    client.instance_variable_set(:@active_thread, active_thread)

    assert_equal true, client.cancel
    assert_equal 1, socket.close_calls
    assert_equal 1, socket.writes.length

    header = socket.writes.first.byteslice(0, Scratchbird::Protocol::HEADER_SIZE)
    type, flags, length, _sequence, _attachment_id, _txn_id = Scratchbird::Protocol.decode_header(header)
    payload = socket.writes.first.byteslice(Scratchbird::Protocol::HEADER_SIZE, length)

    assert_equal Scratchbird::Protocol::MSG_CANCEL, type
    assert_equal Scratchbird::Protocol::MSG_FLAG_URGENT, flags
    assert_equal Scratchbird::Protocol.build_cancel_payload(0, 77), payload
    assert_equal 1, active_thread.raised.length
    assert_kind_of Scratchbird::OperatorInterventionError, active_thread.raised.first
    assert_equal "57014", active_thread.raised.first.sqlstate
  end

  def test_cancel_skips_current_or_inactive_thread_raise
    client = Scratchbird::Client.new(Scratchbird::Config.new)

    current_socket = FakeSocket.new
    client.instance_variable_set(:@socket, current_socket)
    client.instance_variable_set(:@connected, true)
    client.instance_variable_set(:@last_query_sequence, 12)
    client.instance_variable_set(:@active_thread, Thread.current)
    assert_equal true, client.cancel
    assert_equal 1, current_socket.writes.length

    inactive_socket = FakeSocket.new
    inactive_thread = FakeThread.new(alive: false)
    client.instance_variable_set(:@socket, inactive_socket)
    client.instance_variable_set(:@connected, true)
    client.instance_variable_set(:@last_query_sequence, 13)
    client.instance_variable_set(:@active_thread, inactive_thread)
    assert_equal true, client.cancel
    assert_equal 1, inactive_socket.writes.length
    assert_empty inactive_thread.raised
  end
end
