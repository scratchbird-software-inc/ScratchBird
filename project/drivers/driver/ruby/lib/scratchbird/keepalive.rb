# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird Ruby Driver
# Keepalive Manager - Prevents connection timeouts
# Copyright (c) 2025-2026 Dalton Calford

require 'timeout'

module Scratchbird
  # Configuration for keepalive
  class KeepaliveConfig
    attr_accessor :interval_ms, :max_idle_before_check_ms, :validation_timeout_ms
    
    DEFAULT_INTERVAL = 120_000           # 2 minutes
    DEFAULT_MAX_IDLE = 600_000           # 10 minutes
    DEFAULT_VALIDATION_TIMEOUT = 5_000   # 5 seconds
    
    def initialize(options = {})
      @interval_ms = options[:interval_ms] || DEFAULT_INTERVAL
      @max_idle_before_check_ms = options[:max_idle_before_check_ms] || DEFAULT_MAX_IDLE
      @validation_timeout_ms = options[:validation_timeout_ms] || DEFAULT_VALIDATION_TIMEOUT
    end
  end
  
  # Tracks activity for a single connection
  class KeepaliveTracker
    def initialize(config)
      @config = config
      @last_activity = Time.now
      @mutex = Mutex.new
    end
    
    def mark_active
      @mutex.synchronize { @last_activity = Time.now }
    end
    
    def needs_validation?
      idle_duration_ms > @config.max_idle_before_check_ms
    end
    
    def idle_duration_ms
      ((Time.now - @last_activity) * 1000).to_i
    end
  end
  
  # Manages keepalive for multiple connections
  class KeepaliveManager
    def initialize(config = KeepaliveConfig.new)
      @config = config
      @trackers = {}
      @connections = {}
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
    
    def register(connection_id, connection, &pinger)
      tracker = KeepaliveTracker.new(@config)
      @mutex.synchronize do
        @trackers[connection_id] = tracker
        @connections[connection_id] = { connection: connection, pinger: pinger }
      end
      tracker
    end
    
    def unregister(connection_id)
      @mutex.synchronize do
        @trackers.delete(connection_id)
        @connections.delete(connection_id)
      end
    end
    
    def monitored_count
      @mutex.synchronize { @trackers.size }
    end
    
    private
    
    def monitor_loop
      while @running
        sleep(@config.interval_ms / 1000.0)
        check_connections
      end
    end
    
    def check_connections
      trackers_snapshot = @mutex.synchronize { @trackers.dup }
      
      trackers_snapshot.each do |conn_id, tracker|
        if tracker.needs_validation?
          conn_info = @mutex.synchronize { @connections[conn_id] }
          next unless conn_info
          
          begin
            Timeout.timeout(@config.validation_timeout_ms / 1000.0) do
              is_healthy = conn_info[:pinger].call
              tracker.mark_active if is_healthy
            end
          rescue Timeout::Error, StandardError
            # Validation failed
          end
        end
      end
    end
  end
end
