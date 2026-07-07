# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.Telemetry do
  @moduledoc false

  defmodule Config do
    defstruct enable_tracing: true,
              enable_metrics: true,
              enable_slow_query_log: true,
              slow_query_threshold_ms: 1000,
              sanitize_queries: true,
              sample_rate: 1.0
  end

  defmodule SpanContext do
    defstruct trace_id: nil,
              span_id: nil,
              parent_span_id: nil,
              span_name: nil,
              start_time_ms: nil,
              attributes: %{}

    def new(name) do
      %__MODULE__{
        trace_id: :crypto.strong_rand_bytes(16) |> Base.encode16(case: :lower),
        span_id: :crypto.strong_rand_bytes(8) |> Base.encode16(case: :lower),
        span_name: name,
        start_time_ms: System.monotonic_time(:millisecond)
      }
    end

    def elapsed_ms(%__MODULE__{} = span) do
      System.monotonic_time(:millisecond) - span.start_time_ms
    end

    def with_attribute(%__MODULE__{} = span, key, value) do
      %{span | attributes: Map.put(span.attributes, key, value)}
    end
  end

  defmodule Collector do
    defstruct config: %Config{},
              total_queries: 0,
              successful_queries: 0,
              failed_queries: 0,
              total_query_time_ms: 0,
              slow_queries: []

    def new(config \\ %Config{}) do
      %__MODULE__{config: config}
    end

    def start_span(%__MODULE__{} = collector, name) do
      if collector.config.enable_tracing and :rand.uniform() <= collector.config.sample_rate do
        {SpanContext.new(name), collector}
      else
        {nil, collector}
      end
    end

    def end_span(%__MODULE__{} = collector, span, success) do
      if span == nil or not collector.config.enable_tracing do
        collector
      else
        duration = SpanContext.elapsed_ms(span)
        collector = record_metrics(collector, duration, success)
        if collector.config.enable_slow_query_log and duration > collector.config.slow_query_threshold_ms do
          slow = [
            %{
              trace_id: span.trace_id,
              span_name: span.span_name,
              duration_ms: duration,
              timestamp: DateTime.utc_now() |> DateTime.to_iso8601(),
              attributes: span.attributes
            }
            | collector.slow_queries
          ]
          %{collector | slow_queries: Enum.take(slow, 100)}
        else
          collector
        end
      end
    end

    def sanitize_query(sql) do
      String.replace(sql, ~r/'[^']*'/, "'?'")
    end

    defp record_metrics(%__MODULE__{} = collector, duration, success) do
      if collector.config.enable_metrics do
        collector
        |> Map.update!(:total_queries, &(&1 + 1))
        |> Map.update!(:total_query_time_ms, &(&1 + duration))
        |> Map.update!(if(success, do: :successful_queries, else: :failed_queries), &(&1 + 1))
      else
        collector
      end
    end
  end
end
