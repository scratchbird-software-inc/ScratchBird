# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBirdErrorsTest do
  use ExUnit.Case

  alias ScratchBird.Errors

  test "maps SQLSTATE classes by prefix" do
    assert Errors.sqlstate_class("01000") == :warning
    assert Errors.sqlstate_class("22P02") == :data_exception
    assert Errors.sqlstate_class("23505") == :integrity_constraint_violation
    assert Errors.sqlstate_class("28000") == :invalid_authorization
    assert Errors.sqlstate_class("40P01") == :transaction_rollback
    assert Errors.sqlstate_class("42P01") == :syntax_error_or_access_rule_violation
    assert Errors.sqlstate_class("53300") == :insufficient_resources
    assert Errors.sqlstate_class("XX000") == :internal_error
  end

  test "falls back to generic class for unknown or malformed codes" do
    assert Errors.sqlstate_class("ZZ999") == :generic
    assert Errors.sqlstate_class("X") == :generic
    assert Errors.sqlstate_class(nil) == :generic
  end

  test "retry scope classifies statement and reconnect boundaries" do
    assert Errors.retry_scope("40001") == :statement
    assert Errors.retry_scope("40P01") == :statement
    assert Errors.retry_scope("08006") == :reconnect
    assert Errors.retry_scope("57014") == :none
    assert Errors.retry_scope(nil) == :none
  end

  test "retryable only allows fresh boundary retries" do
    assert Errors.retryable?("40001")
    assert Errors.retryable?("08003")
    refute Errors.retryable?("57014")
    refute Errors.retryable?("")
  end
end
