# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "test_helper"

class TestIntegration < Minitest::Test
  def test_select
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      result = conn.query("SELECT 1")
      assert_equal 1, result.first[0]
    ensure
      conn.close
    end
  end

  def test_prepare_bind
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      result = conn.query("SELECT ?::INTEGER", [42])
      assert_equal 42, result.first[0]
    ensure
      conn.close
    end
  end

  def test_types_fixture
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      result = conn.query("SELECT * FROM type_coverage")
      refute result.rows.empty?
    ensure
      conn.close
    end
  end

  def test_manager_proxy_select
    dsn = integration_manager_proxy_dsn
    skip "SCRATCHBIRD_RUBY_MANAGER_PROXY_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      result = conn.query("SELECT 1")
      assert_equal 1, result.first[0]
    ensure
      conn.close
    end
  end

  def test_tls_verify_ca_select
    dsn = integration_tls_verify_ca_dsn
    skip "SCRATCHBIRD_RUBY_TLS_VERIFY_CA_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      result = conn.query("SELECT 1")
      assert_equal 1, result.first[0]
    ensure
      conn.close
    end
  end

  def test_tls_verify_full_select
    dsn = integration_tls_verify_full_dsn
    skip "SCRATCHBIRD_RUBY_TLS_VERIFY_FULL_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      result = conn.query("SELECT 1")
      assert_equal 1, result.first[0]
    ensure
      conn.close
    end
  end

  def test_cancel
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn
    cancel_sql = integration_cancel_sql
    skip "SCRATCHBIRD_RUBY_CANCEL_SQL/SCRATCHBIRD_TEST_CANCEL_SQL not set" unless cancel_sql

    conn = Scratchbird.connect(dsn)
    error = nil
    thread = Thread.new do
      begin
        conn.query(cancel_sql)
      rescue StandardError => e
        error = e
      end
    end
    sleep 0.2
    conn.client.cancel
    thread.join(5)
    conn.close
    refute_nil error
    assert_instance_of Scratchbird::OperatorInterventionError, error
    assert_equal "57", error.sqlstate.to_s[0, 2]
  end

  def test_txn_id_transitions_follow_runtime_ready_frames
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      conn.autocommit = false
      assert_equal true, conn.in_transaction?
      assert_operator conn.client.txn_id, :>, 0
      conn.query("SELECT 1")
      assert_equal true, conn.in_transaction?
      assert_operator conn.client.txn_id, :>, 0
      conn.commit
      assert_equal true, conn.in_transaction?
      assert_operator conn.client.txn_id, :>, 0
      conn.query("SELECT 1")
      assert_equal true, conn.in_transaction?
      conn.rollback
      assert_equal true, conn.in_transaction?
      assert_operator conn.client.txn_id, :>, 0
    ensure
      conn.close
    end
  end

  def test_commit_and_rollback_behavior_after_server_abort
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      conn.autocommit = false
      conn.query("SELECT 1")
      assert_equal true, conn.in_transaction?

      assert_raises(Scratchbird::Error) do
        conn.query("SELECT * FROM __ruby_abort_fixture_missing__")
      end

      begin
        conn.commit
      rescue Scratchbird::Error
        nil
      end
      assert_equal true, conn.in_transaction?
      assert_operator conn.client.txn_id, :>, 0
      conn.rollback
      assert_equal true, conn.in_transaction?
      assert_operator conn.client.txn_id, :>, 0
    ensure
      conn.close
    end
  end

  def test_metadata_collections_and_restrictions_fixture_shape
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      tables = conn.query_metadata("tables")
      types = conn.query_metadata("types")
      ddl_fields = conn.query_metadata("ddl_fields")
      restricted = conn.query_metadata_with_restrictions("tables", { table: "sys.tables" })

      assert tables.respond_to?(:each_hash)
      assert types.respond_to?(:each_hash)
      assert ddl_fields.respond_to?(:each_hash)
      assert restricted.respond_to?(:each_hash)
      sample_row = tables.each_hash.first
      assert sample_row.is_a?(Hash) unless sample_row.nil?
    rescue Scratchbird::NotSupportedError, Scratchbird::SyntaxError => e
      skip("metadata collection not supported by runtime: #{e.message}") if feature_not_supported?(e)
      raise
    ensure
      conn.close
    end
  end

  def test_prepared_close_sequence_roundtrip
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      stmt = conn.prepare("SELECT 9 AS value")
      result = stmt.execute
      assert_equal 9, result.first[0]
      assert_equal true, stmt.close
      assert_equal true, stmt.closed?
    rescue Scratchbird::NotSupportedError, Scratchbird::SyntaxError => e
      skip("prepared close sequence not supported by runtime: #{e.message}") if feature_not_supported?(e)
      raise
    ensure
      conn.close
    end
  end

  def test_constraint_violation_maps_to_integrity_error
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    table_name = "ruby_err_#{Time.now.to_i}"
    conn = Scratchbird.connect(dsn)
    begin
      conn.query("CREATE TABLE #{table_name} (id INTEGER PRIMARY KEY)")
      conn.query("INSERT INTO #{table_name} (id) VALUES (1)")
      err = assert_raises(Scratchbird::IntegrityError) do
        conn.query("INSERT INTO #{table_name} (id) VALUES (1)")
      end
      assert_equal "23", err.sqlstate.to_s[0, 2]
    rescue Scratchbird::NotSupportedError, Scratchbird::SyntaxError => e
      skip("constraint fixture not supported by runtime: #{e.message}") if feature_not_supported?(e)
      raise
    ensure
      begin
        conn.query("DROP TABLE #{table_name}")
      rescue StandardError
        nil
      end
      conn.close
    end
  end

  def test_auth_failure_maps_to_auth_error
    bad_dsn = integration_bad_auth_dsn
    skip "SCRATCHBIRD_RUBY_BAD_AUTH_DSN not set" unless bad_dsn

    err = assert_raises(Scratchbird::AuthError) { Scratchbird.connect(bad_dsn) }
    assert_equal "28", err.sqlstate.to_s[0, 2]
  end

  def test_socket_drop_releases_keepalive_and_leak_tracking_on_close
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      socket = conn.client.instance_variable_get(:@socket)
      socket.close if socket
      assert_raises(Scratchbird::ConnectionError) { conn.query("SELECT 1") }
    ensure
      conn.close
    end

    assert_nil conn.client.instance_variable_get(:@keepalive_tracker)
    assert_nil conn.client.instance_variable_get(:@leak_guard)
  end

  def test_query_multi
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      result_sets = conn.query_multi("SELECT 1 AS first_value; SELECT 2 AS second_value")
      assert_equal 2, result_sets.length
      assert_equal 1, result_sets[0].rows.first[0]
      assert_equal 2, result_sets[1].rows.first[0]
    rescue Scratchbird::NotSupportedError, Scratchbird::SyntaxError => e
      skip("query_multi not supported by runtime: #{e.message}") if feature_not_supported?(e)
      raise
    ensure
      conn.close
    end
  end

  def test_execute_batch
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      batch = conn.execute_batch("SELECT ?::INTEGER AS value", [[11], [22], [33]])
      assert_equal 3, batch.items.length
      assert_equal [0, 1, 2], batch.items.map(&:index)
      assert_operator batch.total_rowcount, :>=, 0
    rescue Scratchbird::NotSupportedError, Scratchbird::SyntaxError => e
      skip("execute_batch not supported by runtime: #{e.message}") if feature_not_supported?(e)
      raise
    ensure
      conn.close
    end
  end

  def test_call_callable_escape_syntax
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      result = conn.call("{ ? = call abs(?) }", [-3])
      refute_nil result.first
      assert_equal 3, result.first[0].to_i.abs
    rescue Scratchbird::NotSupportedError, Scratchbird::SyntaxError => e
      skip("call not supported by runtime: #{e.message}") if feature_not_supported?(e)
      raise
    ensure
      conn.close
    end
  end

  def test_execute_with_generated_keys
    dsn = integration_dsn
    skip "SCRATCHBIRD_RUBY_URL/SCRATCHBIRD_TEST_DSN not set" unless dsn

    conn = Scratchbird.connect(dsn)
    begin
      keys = conn.execute_with_generated_keys("SELECT 1")
      assert keys.is_a?(Array)
      assert keys.all? { |id| id.is_a?(Integer) && id >= 0 }
    rescue Scratchbird::NotSupportedError, Scratchbird::SyntaxError => e
      skip("execute_with_generated_keys not supported by runtime: #{e.message}") if feature_not_supported?(e)
      raise
    ensure
      conn.close
    end
  end

  private

  def integration_dsn
    ENV["SCRATCHBIRD_RUBY_URL"] || ENV["SCRATCHBIRD_TEST_DSN"]
  end

  def integration_cancel_sql
    ENV["SCRATCHBIRD_RUBY_CANCEL_SQL"] || ENV["SCRATCHBIRD_TEST_CANCEL_SQL"]
  end

  def integration_manager_proxy_dsn
    ENV["SCRATCHBIRD_RUBY_MANAGER_PROXY_DSN"]
  end

  def integration_tls_verify_ca_dsn
    ENV["SCRATCHBIRD_RUBY_TLS_VERIFY_CA_DSN"]
  end

  def integration_tls_verify_full_dsn
    ENV["SCRATCHBIRD_RUBY_TLS_VERIFY_FULL_DSN"]
  end

  def integration_bad_auth_dsn
    ENV["SCRATCHBIRD_RUBY_BAD_AUTH_DSN"]
  end

  def feature_not_supported?(error)
    state = error.respond_to?(:sqlstate) ? error.sqlstate.to_s : ""
    state == "0A000"
  end
end
