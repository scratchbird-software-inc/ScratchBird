# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird Ruby Driver - Connection Leak Detector
# Copyright (c) 2025-2026 Dalton Calford

module Scratchbird
  module LeakLogLevel
    DEBUG = :debug
    WARN = :warn
    ERROR = :error
  end
  
  class LeakDetectionConfig
    attr_accessor :threshold_ms, :capture_stack_trace, :check_interval_ms, :log_level
    
    def initialize(options = {})
      @threshold_ms = options[:threshold_ms] || 30_000
      @capture_stack_trace = options[:capture_stack_trace] || false
      @check_interval_ms = options[:check_interval_ms] || 10_000
      @log_level = options[:log_level] || LeakLogLevel::WARN
    end
  end
  
  class CheckoutInfo
    attr_reader :checkout_time, :thread_id, :stack_trace, :metadata
    
    def initialize(capture_stack_trace, metadata = {})
      @checkout_time = Time.now
      @thread_id = Thread.current.object_id
      @metadata = metadata.dup
      @stack_trace = capture_stack_trace ? caller : nil
    end
    
    def held_duration_ms
      ((Time.now - @checkout_time) * 1000).to_i
    end
  end
  
  class LeakDetectionGuard
    def initialize(detector, connection_id)
      @detector = detector
      @connection_id = connection_id
      @released = false
    end
    
    def release
      return if @released
      @detector.checkin(@connection_id)
      @released = true
    end
    
    alias_method :close, :release
  end
  
  class LeakDetector
    def initialize(config = LeakDetectionConfig.new)
      @config = config
      @checkouts = {}
      @mutex = Mutex.new
      @running = false
      @thread = nil
    end
    
    def start
      return if @running
      @running = true
      @thread = Thread.new { monitor_loop }
    end
    
    def stop
      return unless @running
      @running = false
      @thread&.join(5)
    end
    
    def checkout(connection_id, metadata = {})
      info = CheckoutInfo.new(@config.capture_stack_trace, metadata)
      @mutex.synchronize { @checkouts[connection_id] = info }
      LeakDetectionGuard.new(self, connection_id)
    end
    
    def checkin(connection_id)
      info = nil
      @mutex.synchronize do
        info = @checkouts.delete(connection_id)
      end
      
      if info && info.held_duration_ms > @config.threshold_ms
        # Log held too long
      end
    end
    
    def active_count
      @mutex.synchronize { @checkouts.size }
    end
    
    def stats
      potential_leaks = 0
      @mutex.synchronize do
        @checkouts.each_value do |info|
          potential_leaks += 1 if info.held_duration_ms > @config.threshold_ms
        end
      end
      { active_checkouts: active_count, potential_leaks: potential_leaks }
    end
    
    private
    
    def monitor_loop
      while @running
        sleep(@config.check_interval_ms / 1000.0)
        check_leaks
      end
    end
    
    def check_leaks
      @mutex.synchronize do
        @checkouts.each do |conn_id, info|
          log_leak(conn_id, info) if info.held_duration_ms > @config.threshold_ms
        end
      end
    end
    
    def log_leak(conn_id, info)
      puts "POSSIBLE CONNECTION LEAK: conn=#{conn_id}, held=#{info.held_duration_ms}ms"
    end
  end
end
