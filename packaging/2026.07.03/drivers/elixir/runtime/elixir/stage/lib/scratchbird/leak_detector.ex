# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.LeakDetector do
  @moduledoc false

  defmodule Config do
    defstruct threshold_ms: 30_000, capture_stack_trace: false
  end

  defmodule Guard do
    defstruct checkout_time_ms: System.monotonic_time(:millisecond),
              config: %Config{},
              metadata: %{},
              stack_trace: nil

    def held_duration_ms(%__MODULE__{} = guard) do
      System.monotonic_time(:millisecond) - guard.checkout_time_ms
    end
  end

  def checkout(config \\ %Config{}, metadata \\ %{}) do
    stack = if config.capture_stack_trace, do: Process.info(self(), :current_stacktrace), else: nil
    %Guard{config: config, metadata: metadata, stack_trace: stack}
  end

  def release(%Guard{} = guard) do
    if held_duration_ms(guard) > guard.config.threshold_ms do
      IO.puts("POSSIBLE CONNECTION LEAK: held=#{held_duration_ms(guard)}ms metadata=#{inspect(guard.metadata)}")
    end
    :ok
  end

  defp held_duration_ms(%Guard{} = guard) do
    Guard.held_duration_ms(guard)
  end
end
