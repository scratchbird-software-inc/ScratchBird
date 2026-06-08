# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "scratchbird/errors"
require "scratchbird/sql"

module Scratchbird
  class Statement
    def initialize(connection, sql)
      @connection = connection
      @sql = sql
      @name = "sb_stmt_#{object_id}"
      @closed = false
      @connection.client.prepare(@name, @sql)
    end

    def execute(params = nil, options = nil)
      ensure_open
      @connection.execute_prepared(@name, params, options)
    end

    def stream(params = nil, options = nil)
      ensure_open
      @connection.stream_prepared(@name, params, options)
    end

    def close
      return if @closed
      if @connection.respond_to?(:close_prepared)
        begin
          @connection.close_prepared(@name)
        rescue Scratchbird::Error, Scratchbird::ConnectionError
          nil
        end
      end
      @closed = true
    end

    def closed?
      @closed
    end

    private

    def ensure_open
      raise Error, "statement is closed" if @closed
    end
  end
end
