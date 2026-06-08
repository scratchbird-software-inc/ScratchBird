# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird Ruby Driver - Circuit Breaker
# Copyright (c) 2025-2026 Dalton Calford

module Scratchbird
  module CircuitState
    CLOSED = :closed
    OPEN = :open
    HALF_OPEN = :half_open
  end
  
  class CircuitBreakerConfig
    attr_accessor :failure_threshold, :recovery_timeout_ms, :success_threshold, :half_open_max_requests
    
    def initialize(options = {})
      @failure_threshold = options[:failure_threshold] || 5
      @recovery_timeout_ms = options[:recovery_timeout_ms] || 30_000
      @success_threshold = options[:success_threshold] || 3
      @half_open_max_requests = options[:half_open_max_requests] || 10
    end
  end
  
  class CircuitBreakerOpenError < StandardError; end
  
  class CircuitBreaker
    attr_reader :state, :name
    
    def initialize(config = CircuitBreakerConfig.new, name = 'default')
      @config = config
      @name = name
      @state = CircuitState::CLOSED
      @failure_count = 0
      @success_count = 0
      @half_open_requests = 0
      @last_failure_time = nil
      @mutex = Mutex.new
    end
    
    def allow_request?
      @mutex.synchronize do
        case @state
        when CircuitState::CLOSED
          true
        when CircuitState::OPEN
          if @last_failure_time && ((Time.now - @last_failure_time) * 1000) >= @config.recovery_timeout_ms
            @state = CircuitState::HALF_OPEN
            @failure_count = @success_count = @half_open_requests = 0
            allow_half_open_request?
          else
            false
          end
        when CircuitState::HALF_OPEN
          allow_half_open_request?
        else
          false
        end
      end
    end
    
    def record_success
      @mutex.synchronize do
        case @state
        when CircuitState::CLOSED
          @failure_count = 0
        when CircuitState::HALF_OPEN
          @half_open_requests -= 1
          @success_count += 1
          if @success_count >= @config.success_threshold
            @state = CircuitState::CLOSED
            @failure_count = @success_count = 0
          end
        end
      end
    end
    
    def record_failure
      @mutex.synchronize do
        case @state
        when CircuitState::CLOSED
          @failure_count += 1
          if @failure_count >= @config.failure_threshold
            @state = CircuitState::OPEN
            @last_failure_time = Time.now
          end
        when CircuitState::HALF_OPEN
          @half_open_requests -= 1
          @state = CircuitState::OPEN
          @last_failure_time = Time.now
        when CircuitState::OPEN
          @last_failure_time = Time.now
        end
      end
    end
    
    def reset
      @mutex.synchronize do
        @state = CircuitState::CLOSED
        @failure_count = @success_count = @half_open_requests = 0
        @last_failure_time = nil
      end
    end
    
    def execute
      raise CircuitBreakerOpenError, "Circuit breaker #{@name} is OPEN" unless allow_request?
      
      begin
        result = yield
        record_success
        result
      rescue => e
        record_failure
        raise e
      end
    end
    
    def stats
      @mutex.synchronize do
        {
          state: @state,
          failure_count: @failure_count,
          success_count: @success_count,
          half_open_requests: @half_open_requests,
          last_failure_time: @last_failure_time
        }
      end
    end
    
    private
    
    def allow_half_open_request?
      return false if @half_open_requests >= @config.half_open_max_requests
      @half_open_requests += 1
      true
    end
  end
end
