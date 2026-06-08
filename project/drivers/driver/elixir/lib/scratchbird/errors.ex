# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.Errors do
  @moduledoc false

  def sqlstate_class(code) when is_binary(code) and byte_size(code) >= 2 do
    case binary_part(code, 0, 2) do
      "01" -> :warning
      "02" -> :no_data
      "08" -> :connection_exception
      "0A" -> :feature_not_supported
      "22" -> :data_exception
      "23" -> :integrity_constraint_violation
      "28" -> :invalid_authorization
      "40" -> :transaction_rollback
      "42" -> :syntax_error_or_access_rule_violation
      "53" -> :insufficient_resources
      "54" -> :program_limit_exceeded
      "57" -> :operator_intervention
      "58" -> :system_error
      "XX" -> :internal_error
      _ -> :generic
    end
  end

  def sqlstate_class(_), do: :generic

  def retry_scope(code) when is_binary(code) and byte_size(code) == 5 do
    # Drivers are fail-closed: fresh statement restart for 40xxx,
    # reconnect only for 08xxx, and no automatic whole-transaction replay.
    cond do
      code in ["40001", "40P01"] -> :statement
      String.starts_with?(code, "08") -> :reconnect
      true -> :none
    end
  end

  def retry_scope(_), do: :none

  def retryable?(code), do: retry_scope(code) != :none
end
