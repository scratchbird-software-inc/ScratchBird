# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

module Scratchbird
  class Error < StandardError
    attr_reader :sqlstate, :detail, :hint

    def initialize(message, sqlstate = "", detail = "", hint = "")
      super(message)
      @sqlstate = sqlstate
      @detail = detail
      @hint = hint
    end
  end

  class Warning < Error; end
  class NoDataError < Error; end
  class ConnectionError < Error; end
  class NotSupportedError < Error; end
  class DataError < Error; end
  class IntegrityError < Error; end
  class AuthError < Error; end
  class TransactionError < Error; end
  class SyntaxError < Error; end
  class ResourceError < Error; end
  class LimitError < Error; end
  class OperatorInterventionError < Error; end
  class SystemError < Error; end
  class InternalError < Error; end

  module ErrorMapper
    RETRY_SCOPE_NONE = "none"
    RETRY_SCOPE_RECONNECT = "reconnect"
    RETRY_SCOPE_STATEMENT = "statement"
    RETRY_SCOPE_TRANSACTION = "transaction"

    def self.from_sqlstate(sqlstate, message, detail = "", hint = "")
      sqlstate = sqlstate.to_s
      text = [message, detail, hint].compact.join("\n").downcase
      if sqlstate.start_with?("42") && integrity_message?(text)
        sqlstate = "23000"
      end
      if sqlstate.length == 5
        case sqlstate
        when "01000" then return Warning.new(message, sqlstate, detail, hint)
        when "02000" then return NoDataError.new(message, sqlstate, detail, hint)
        when "08001", "08003", "08004", "08006", "08P01"
          return ConnectionError.new(message, sqlstate, detail, hint)
        when "0A000" then return NotSupportedError.new(message, sqlstate, detail, hint)
        when "22001", "22003", "22007", "22012", "22023", "22P02", "22P03"
          return DataError.new(message, sqlstate, detail, hint)
        when "23000", "23502", "23503", "23505", "23514"
          return IntegrityError.new(message, sqlstate, detail, hint)
        when "28000", "28P01" then return AuthError.new(message, sqlstate, detail, hint)
        when "40001", "40P01" then return TransactionError.new(message, sqlstate, detail, hint)
        when "42501", "42601", "42703", "42704", "42710", "42883", "42P01", "42P07"
          return SyntaxError.new(message, sqlstate, detail, hint)
        when "53P00", "53100", "53200", "53300"
          return ResourceError.new(message, sqlstate, detail, hint)
        when "54000" then return LimitError.new(message, sqlstate, detail, hint)
        when "57014", "57P01", "57P03"
          return OperatorInterventionError.new(message, sqlstate, detail, hint)
        when "58000" then return SystemError.new(message, sqlstate, detail, hint)
        when "XX000" then return InternalError.new(message, sqlstate, detail, hint)
        end

        case sqlstate[0, 2]
        when "01" then return Warning.new(message, sqlstate, detail, hint)
        when "02" then return NoDataError.new(message, sqlstate, detail, hint)
        when "08" then return ConnectionError.new(message, sqlstate, detail, hint)
        when "0A" then return NotSupportedError.new(message, sqlstate, detail, hint)
        when "22" then return DataError.new(message, sqlstate, detail, hint)
        when "23" then return IntegrityError.new(message, sqlstate, detail, hint)
        when "28" then return AuthError.new(message, sqlstate, detail, hint)
        when "40" then return TransactionError.new(message, sqlstate, detail, hint)
        when "42" then return SyntaxError.new(message, sqlstate, detail, hint)
        when "53" then return ResourceError.new(message, sqlstate, detail, hint)
        when "54" then return LimitError.new(message, sqlstate, detail, hint)
        when "57" then return OperatorInterventionError.new(message, sqlstate, detail, hint)
        when "58" then return SystemError.new(message, sqlstate, detail, hint)
        when "XX" then return InternalError.new(message, sqlstate, detail, hint)
        end
      end
      Error.new(message, sqlstate, detail, hint)
    end

    def self.retry_scope_for_sqlstate(sqlstate)
      # Drivers are fail-closed: fresh statement restart for 40xxx,
      # reconnect only for 08xxx, and no automatic whole-transaction replay.
      sqlstate = sqlstate.to_s
      return RETRY_SCOPE_NONE unless sqlstate.length == 5
      return RETRY_SCOPE_STATEMENT if sqlstate == "40001" || sqlstate == "40P01"
      return RETRY_SCOPE_RECONNECT if sqlstate.start_with?("08")

      RETRY_SCOPE_NONE
    end

    def self.retryable_sqlstate?(sqlstate)
      retry_scope_for_sqlstate(sqlstate) != RETRY_SCOPE_NONE
    end

    def self.integrity_message?(text)
      return false if text.empty?

      text.include?("constraint violation") ||
        text.include?("duplicate value") ||
        text.include?("duplicate key") ||
        text.include?("unique index violation") ||
        text.include?("primary key") ||
        text.include?("foreign key") ||
        text.include?("not null")
    end
  end
end
