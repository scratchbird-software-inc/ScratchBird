# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "scratchbird/client"
require "scratchbird/config"
require "scratchbird/errors"
require "scratchbird/metadata"
require "scratchbird/sql"
require "scratchbird/statement"

module Scratchbird
  class Connection
    attr_reader :config
    attr_reader :autocommit

    def initialize(options = nil)
      @config = build_config(options)
      @client = Client.new(@config)
      @autocommit = true
      @closed = false
      @client.connect
    end

    def close
      return if @closed
      begin
        @client.disconnect
      ensure
        @closed = true
      end
    end

    def closed?
      @closed
    end

    def autocommit=(value)
      ensure_open
      next_value = !!value
      return if @autocommit == next_value

      if next_value && in_transaction?
        commit
      end

      @autocommit = next_value
    end

    def begin_transaction(options = nil)
      ensure_open
      @client.begin_transaction(options)
    end

    def commit
      ensure_open
      @client.commit
    end

    def rollback
      ensure_open
      @client.rollback
    end

    def supports_prepared_transactions?
      @client.supports_prepared_transactions?
    end

    def supports_dormant_reattach?
      @client.supports_dormant_reattach?
    end

    def prepare_transaction(gid)
      ensure_open
      @client.prepare_transaction(gid)
    end

    def commit_prepared(gid)
      ensure_open
      @client.commit_prepared(gid)
    end

    def rollback_prepared(gid)
      ensure_open
      @client.rollback_prepared(gid)
    end

    def detach_to_dormant
      ensure_open
      @client.detach_to_dormant
    end

    def reattach_dormant(dormant_id, auth_token = nil)
      ensure_open
      @client.reattach_dormant(dormant_id, auth_token)
    end

    def savepoint(name)
      ensure_open
      @client.savepoint(name)
    end

    def rollback_to_savepoint(name)
      ensure_open
      @client.rollback_to_savepoint(name)
    end

    def release_savepoint(name)
      ensure_open
      @client.release_savepoint(name)
    end

    def in_transaction?
      return false unless @client.respond_to?(:in_transaction?)
      @client.in_transaction?
    end

    def execute(sql, params = nil, options = nil)
      ensure_open
      begin_transaction_if_needed
      @client.query(sql, params, options)
    end

    def query(sql, params = nil, options = nil)
      execute(sql, params, options)
    end

    def stream(sql, params = nil, options = nil)
      ensure_open
      begin_transaction_if_needed
      @client.stream(sql, params, options)
    end

    def native_sql(sql, params = nil)
      ensure_open
      @client.native_sql(sql, params)
    end

    def native_callable_sql(sql, params = nil)
      ensure_open
      @client.native_callable_sql(sql, params)
    end

    def call(sql, params = nil, options = nil)
      ensure_open
      begin_transaction_if_needed
      @client.call(sql, params, options)
    end

    def query_multi(sql, params = nil, options = nil)
      ensure_open
      begin_transaction_if_needed
      @client.query_multi(sql, params, options)
    end

    def execute_multi(sql, params = nil, options = nil)
      query_multi(sql, params, options)
    end

    def execute_batch(sql, batch_params, options = nil)
      ensure_open
      begin_transaction_if_needed
      @client.execute_batch(sql, batch_params, options)
    end

    def query_batch(sql, batch_params, options = nil)
      execute_batch(sql, batch_params, options)
    end

    def execute_with_generated_keys(sql, params = nil, options = nil)
      ensure_open
      begin_transaction_if_needed
      @client.execute_with_generated_keys(sql, params, options)
    end

    def query_metadata(collection_name = "tables", options = nil)
      ensure_open
      begin_transaction_if_needed
      @client.query_metadata(collection_name, options)
    end

    def query_metadata_with_restrictions(collection_name = "tables", restrictions = nil, options = nil)
      ensure_open
      begin_transaction_if_needed
      @client.query_metadata_with_restrictions(collection_name, restrictions, options)
    end

    def get_schema(collection_name = "tables", options = nil, expand_schema_parents: nil)
      ensure_open
      begin_transaction_if_needed
      @client.get_schema(collection_name, options, expand_schema_parents: expand_schema_parents)
    end

    def get_schema_with_restrictions(collection_name = "tables", restrictions = nil, options = nil, expand_schema_parents: nil)
      ensure_open
      begin_transaction_if_needed
      @client.get_schema_with_restrictions(
        collection_name,
        restrictions,
        options,
        expand_schema_parents: expand_schema_parents
      )
    end

    def get_schema_tree(expand_schema_parents: nil, database: nil, default_branch: "default", restrictions: nil)
      ensure_open
      begin_transaction_if_needed
      rows = get_schema_with_restrictions(
        "schemas",
        restrictions,
        nil,
        expand_schema_parents: expand_schema_parents
      )
      Metadata.build_database_default_metadata_rows(
        rows,
        database: database || @config.database,
        expand_schema_parents: false,
        default_branch: default_branch
      )
    end

    def prepare(sql)
      ensure_open
      Statement.new(self, sql)
    end

    def execute_prepared(name, params = nil, options = nil)
      ensure_open
      begin_transaction_if_needed
      @client.execute(name, params, options)
    end

    def stream_prepared(name, params = nil, options = nil)
      ensure_open
      begin_transaction_if_needed
      @client.execute_stream(name, params, options)
    end

    def close_prepared(name)
      ensure_open
      @client.deallocate(name)
    end

    def client
      @client
    end

    def resolved_auth_context
      @client.get_resolved_auth_context
    end

    private

    def ensure_open
      raise ConnectionError, "connection is closed" if @closed
    end

    def begin_transaction_if_needed
      return if autocommit
      # Native MGA sessions own the replacement transaction boundary on the
      # server side. With autocommit disabled, statements execute against that
      # session transaction directly instead of injecting a client-side BEGIN.
    end

    def build_config(options)
      case options
      when Config
        options
      when String
        Config.parse(options)
      when Hash
        cfg = Config.new
        options.each do |key, value|
          key_s = key.to_s
          setter = "#{key_s}="
          if cfg.respond_to?(setter)
            cfg.public_send(setter, value)
          else
            Config.apply_param(cfg, key_s, value)
          end
        end
        cfg
      when nil
        Config.new
      else
        raise ArgumentError, "unsupported connection options"
      end
    end
  end
end
