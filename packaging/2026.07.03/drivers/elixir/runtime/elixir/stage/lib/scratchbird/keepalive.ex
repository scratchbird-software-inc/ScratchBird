# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.Keepalive do
  @moduledoc false

  defmodule Config do
    defstruct interval_ms: 120_000,
              max_idle_before_check_ms: 600_000,
              validation_timeout_ms: 5_000
  end

  defmodule Tracker do
    defstruct last_activity_ms: System.monotonic_time(:millisecond),
              config: %Config{}

    def mark_active(%__MODULE__{} = tracker) do
      %{tracker | last_activity_ms: System.monotonic_time(:millisecond)}
    end

    def needs_validation?(%__MODULE__{} = tracker) do
      System.monotonic_time(:millisecond) - tracker.last_activity_ms >
        tracker.config.max_idle_before_check_ms
    end
  end

  def new_tracker(config \\ %Config{}) do
    %Tracker{config: config}
  end
end
