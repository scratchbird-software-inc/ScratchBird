# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird Ruby Driver - OpenTelemetry Telemetry
# Copyright (c) 2025-2026 Dalton Calford

require 'securerandom'

module Scratchbird
  class TelemetryConfig
    attr_accessor :enable_tracing, :enable_metrics, :enable_slow_query_log,
                  :slow_query_threshold_ms, :sanitize_queries, :sample_rate
    
    def initialize(options = {})
      @enable_tracing = options.fetch(:enable_tracing, true)
      @enable_metrics = options.fetch(:enable_metrics, true)
      @enable_slow_query_log = options.fetch(:enable_slow_query_log, true)
      @slow_query_threshold_ms = options.fetch(:slow_query_threshold_ms, 1000)
      @sanitize_queries = options.fetch(:sanitize_queries, true)
      @sample_rate = options.fetch(:sample_rate, 1.0)
    end
  end
  
  class SpanContext
    attr_reader :trace_id, :span_id, :parent_span_id, :span_name, :start_time, :attributes
    
    def initialize(name, parent = nil)
      @trace_id = parent ? parent.trace_id : SecureRandom.hex(16)
      @span_id = SecureRandom.hex(8)
      @parent_span_id = parent ? parent.span_id : nil
      @span_name = name
      @start_time = Time.now
      @attributes = {}
    end
    
    def with_attribute(key, value)
      @attributes[key] = value
      self
    end
    
    def elapsed
      ((Time.now - @start_time) * 1000).to_i
    end
  end
  
  class LatencyHistogram
    attr_accessor :ms0_10, :ms10_100, :ms100_1000, :ms1000_10000, :ms_over_10000
    
    def initialize
      @ms0_10 = @ms10_100 = @ms100_1000 = @ms1000_10000 = @ms_over_10000 = 0
      @mutex = Mutex.new
    end
    
    def record(duration_ms)
      @mutex.synchronize do
        case duration_ms
        when 0..10 then @ms0_10 += 1
        when 11..100 then @ms10_100 += 1
        when 101..1000 then @ms100_1000 += 1
        when 1001..10000 then @ms1000_10000 += 1
        else @ms_over_10000 += 1
        end
      end
    end
  end
  
  class OperationMetrics
    attr_reader :count, :total_time_ms, :avg_time_ms, :error_count
    
    def initialize
      @count = @total_time_ms = @avg_time_ms = @error_count = 0
      @mutex = Mutex.new
    end
    
    def record(duration_ms, success)
      @mutex.synchronize do
        @count += 1
        @total_time_ms += duration_ms
        @avg_time_ms = @total_time_ms / @count
        @error_count += 1 unless success
      end
    end
  end
  
  class TelemetryCollector
    def initialize(config = TelemetryConfig.new)
      @config = config
      @spans = []
      @total_queries = @successful_queries = @failed_queries = @total_query_time_ms = 0
      @histogram = LatencyHistogram.new
      @operation_metrics = {}
      @slow_queries = []
      @mutex = Mutex.new
    end
    
    def start_span(name)
      return nil unless @config.enable_tracing
      return nil if rand > @config.sample_rate
      
      span = SpanContext.new(name)
      @mutex.synchronize do
        @spans << span
        @spans.shift if @spans.size > 1000
      end
      span
    end
    
    def end_span(span, success = true)
      return unless span && @config.enable_tracing
      duration_ms = span.elapsed
      record_query_metrics(span.span_name, duration_ms, success)
      if @config.enable_slow_query_log && duration_ms > @config.slow_query_threshold_ms
        record_slow_query(span, duration_ms)
      end
    end
    
    def metrics
      @mutex.synchronize do
        {
          total_queries: @total_queries,
          successful_queries: @successful_queries,
          failed_queries: @failed_queries,
          total_query_time_ms: @total_query_time_ms,
          latency_histogram: @histogram,
          operation_metrics: @operation_metrics.transform_values { |m| { count: m.count, avg_time_ms: m.avg_time_ms } }
        }
      end
    end
    
    def slow_queries
      @mutex.synchronize { @slow_queries.dup }
    end
    
    def self.sanitize_query(sql)
      return sql unless sql
      sql.gsub(/'[^']*'/, "'?'")
    end
    
    def export_prometheus_metrics
      m = metrics
      h = m[:latency_histogram]
      <<~METRICS
        # HELP scratchbird_queries_total Total number of queries
        # TYPE scratchbird_queries_total counter
        scratchbird_queries_total #{m[:total_queries]}
        # HELP scratchbird_query_duration_ms Query duration histogram
        # TYPE scratchbird_query_duration_ms histogram
        scratchbird_query_duration_ms_bucket{le="10"} #{h.ms0_10}
        scratchbird_query_duration_ms_bucket{le="100"} #{h.ms0_10 + h.ms10_100}
        scratchbird_query_duration_ms_bucket{le="1000"} #{h.ms0_10 + h.ms10_100 + h.ms100_1000}
      METRICS
    end
    
    private
    
    def record_query_metrics(operation, duration_ms, success)
      return unless @config.enable_metrics
      @mutex.synchronize do
        @total_queries += 1
        success ? @successful_queries += 1 : @failed_queries += 1
        @total_query_time_ms += duration_ms
        @histogram.record(duration_ms)
        @operation_metrics[operation] ||= OperationMetrics.new
        @operation_metrics[operation].record(duration_ms, success)
      end
    end
    
    def record_slow_query(span, duration_ms)
      @mutex.synchronize do
        @slow_queries << {
          trace_id: span.trace_id,
          span_name: span.span_name,
          duration_ms: duration_ms,
          timestamp: Time.now,
          attributes: span.attributes
        }
        @slow_queries.shift if @slow_queries.size > 100
      end
    end
  end
end
