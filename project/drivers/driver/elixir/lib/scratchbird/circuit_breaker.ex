# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.CircuitBreaker do
  @moduledoc false

  defstruct [
    state: :closed,
    failure_count: 0,
    success_count: 0,
    half_open_requests: 0,
    last_failure_at: nil,
    config: %{
      failure_threshold: 5,
      recovery_timeout_ms: 30_000,
      success_threshold: 3,
      half_open_max_requests: 10
    }
  ]

  def new(), do: %__MODULE__{}

  def allow_request?(%__MODULE__{} = cb) do
    case cb.state do
      :closed -> {true, cb}
      :open ->
        now = System.monotonic_time(:millisecond)
        if cb.last_failure_at && now - cb.last_failure_at >= cb.config.recovery_timeout_ms do
          cb = transition_to_half_open(cb)
          {allow_half_open_request?(cb), cb}
        else
          {false, cb}
        end

      :half_open ->
        {allow_half_open_request?(cb), cb}
    end
  end

  def record_success(%__MODULE__{} = cb) do
    case cb.state do
      :closed ->
        %{cb | failure_count: 0}

      :half_open ->
        cb = %{cb | half_open_requests: max(cb.half_open_requests - 1, 0), success_count: cb.success_count + 1}
        if cb.success_count >= cb.config.success_threshold do
          transition_to_closed(cb)
        else
          cb
        end

      :open ->
        cb
    end
  end

  def record_failure(%__MODULE__{} = cb) do
    case cb.state do
      :closed ->
        cb = %{cb | failure_count: cb.failure_count + 1}
        if cb.failure_count >= cb.config.failure_threshold do
          transition_to_open(cb)
        else
          cb
        end

      :half_open ->
        transition_to_open(%{cb | half_open_requests: max(cb.half_open_requests - 1, 0)})

      :open ->
        %{cb | last_failure_at: System.monotonic_time(:millisecond)}
    end
  end

  defp allow_half_open_request?(%__MODULE__{} = cb) do
    cb.half_open_requests < cb.config.half_open_max_requests
  end

  defp transition_to_half_open(%__MODULE__{} = cb) do
    %{cb | state: :half_open, failure_count: 0, success_count: 0, half_open_requests: 0}
  end

  defp transition_to_open(%__MODULE__{} = cb) do
    %{cb | state: :open, last_failure_at: System.monotonic_time(:millisecond)}
  end

  defp transition_to_closed(%__MODULE__{} = cb) do
    %{cb | state: :closed, failure_count: 0, success_count: 0, half_open_requests: 0, last_failure_at: nil}
  end
end
