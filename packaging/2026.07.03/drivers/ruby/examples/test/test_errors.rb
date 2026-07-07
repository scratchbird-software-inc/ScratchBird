# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "test_helper"

class TestErrors < Minitest::Test
  def test_sqlstate_mappings_cover_core_error_families
    cases = {
      "01000" => Scratchbird::Warning,
      "02000" => Scratchbird::NoDataError,
      "08006" => Scratchbird::ConnectionError,
      "08ZZZ" => Scratchbird::ConnectionError,
      "0A000" => Scratchbird::NotSupportedError,
      "22012" => Scratchbird::DataError,
      "22ZZZ" => Scratchbird::DataError,
      "23505" => Scratchbird::IntegrityError,
      "28P01" => Scratchbird::AuthError,
      "40001" => Scratchbird::TransactionError,
      "42P01" => Scratchbird::SyntaxError,
      "53100" => Scratchbird::ResourceError,
      "54000" => Scratchbird::LimitError,
      "57014" => Scratchbird::OperatorInterventionError,
      "58000" => Scratchbird::SystemError,
      "XX000" => Scratchbird::InternalError
    }

    cases.each do |sqlstate, expected_class|
      err = Scratchbird::ErrorMapper.from_sqlstate(sqlstate, "message", "detail", "hint")
      assert_instance_of expected_class, err
      assert_equal sqlstate, err.sqlstate
      assert_equal "detail", err.detail
      assert_equal "hint", err.hint
    end
  end

  def test_unknown_sqlstate_falls_back_to_base_error
    err = Scratchbird::ErrorMapper.from_sqlstate("99999", "fallback")
    assert_instance_of Scratchbird::Error, err
    refute_instance_of Scratchbird::InternalError, err
    assert_equal "99999", err.sqlstate
  end

  def test_client_handle_query_error_preserves_typed_sqlstate_mapping
    client = Scratchbird::Client.new(Scratchbird::Config.new)
    payload = build_error_payload(
      severity: "ERROR",
      sqlstate: "23505",
      message: "duplicate key",
      detail: "Key (id)=(1) already exists",
      hint: "Use a different id"
    )

    err = assert_raises(Scratchbird::IntegrityError) { client.send(:handle_query_error, payload) }
    assert_equal "23505", err.sqlstate
    assert_includes err.message, "duplicate key"
    assert_includes err.message, "DETAIL: Key (id)=(1) already exists"
    assert_includes err.message, "HINT: Use a different id"
  end

  def test_42xxx_unique_index_violation_remaps_to_integrity_error
    err = Scratchbird::ErrorMapper.from_sqlstate("42000", "[42000] UNIQUE index violation on index 't_pkey'")
    assert_instance_of Scratchbird::IntegrityError, err
    assert_equal "23000", err.sqlstate
  end

  def test_retry_scope_classifies_statement_and_reconnect_boundaries
    assert_equal Scratchbird::ErrorMapper::RETRY_SCOPE_STATEMENT,
                 Scratchbird::ErrorMapper.retry_scope_for_sqlstate("40001")
    assert_equal Scratchbird::ErrorMapper::RETRY_SCOPE_STATEMENT,
                 Scratchbird::ErrorMapper.retry_scope_for_sqlstate("40P01")
    assert_equal Scratchbird::ErrorMapper::RETRY_SCOPE_RECONNECT,
                 Scratchbird::ErrorMapper.retry_scope_for_sqlstate("08006")
    assert_equal Scratchbird::ErrorMapper::RETRY_SCOPE_NONE,
                 Scratchbird::ErrorMapper.retry_scope_for_sqlstate("57014")
    assert_equal Scratchbird::ErrorMapper::RETRY_SCOPE_NONE,
                 Scratchbird::ErrorMapper.retry_scope_for_sqlstate(nil)
  end

  def test_retryable_sqlstate_only_allows_fresh_boundary_retries
    assert Scratchbird::ErrorMapper.retryable_sqlstate?("40001")
    assert Scratchbird::ErrorMapper.retryable_sqlstate?("08003")
    refute Scratchbird::ErrorMapper.retryable_sqlstate?("57014")
    refute Scratchbird::ErrorMapper.retryable_sqlstate?("")
  end

  private

  def build_error_payload(severity:, sqlstate:, message:, detail: "", hint: "")
    payload = +""
    payload << "S" << severity << "\0"
    payload << "C" << sqlstate << "\0"
    payload << "M" << message << "\0"
    payload << "D" << detail << "\0" unless detail.empty?
    payload << "H" << hint << "\0" unless hint.empty?
    payload << "\0"
    payload
  end
end
