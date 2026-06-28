# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "socket"
require "openssl"
require "base64"

require "scratchbird/config"
require "scratchbird/errors"
require "scratchbird/protocol"
require "scratchbird/result"
require "scratchbird/scram"
require "scratchbird/sql"
require "scratchbird/types"
require "scratchbird/circuit_breaker"
require "scratchbird/keepalive"
require "scratchbird/leak_detector"
require "scratchbird/telemetry"
require "scratchbird/metadata"

module Scratchbird
  class Client
    QUERY_FLAG_BINARY_RESULT = 0x04
    MANAGER_PROTOCOL_MAGIC = 0x42444253
    MANAGER_PROTOCOL_VERSION = 0x0101
    MANAGER_HEADER_SIZE = 12
    MANAGER_MAX_PAYLOAD_SIZE = 16 * 1024 * 1024
    MCP_PROTOCOL_VERSION = 0x0100

    MCP_MSG_CONNECT_RESPONSE = 0x02
    MCP_MSG_AUTH_CHALLENGE = 0x12
    MCP_MSG_AUTH_RESPONSE = 0x11
    MCP_MSG_STATUS_RESPONSE = 0x64
    MCP_MSG_HELLO = 0x65
    MCP_MSG_AUTH_START = 0x66
    MCP_MSG_AUTH_CONTINUE = 0x67
    MCP_MSG_DB_CONNECT = 0x69
    MCP_AUTH_METHOD_TOKEN = 4

    AuthMethodSurface = Struct.new(
      :wire_method,
      :plugin_method_id,
      :executable_locally,
      :broker_required,
      keyword_init: true
    )

    AuthProbeResult = Struct.new(
      :reachable,
      :ingress_mode,
      :resolved_host,
      :resolved_port,
      :admitted_methods,
      :required_method,
      :required_plugin_method_id,
      :allowed_transport_mask,
      :additional_continuation_possible,
      keyword_init: true
    )

    ResolvedAuthContext = Struct.new(
      :ingress_mode,
      :resolved_auth_method,
      :resolved_auth_plugin_id,
      :manager_authenticated,
      :attached,
      keyword_init: true
    )

    DEFAULT_AUTH_PLUGIN_IDS = {
      Protocol::AUTH_PASSWORD => "scratchbird.auth.password_compat",
      Protocol::AUTH_MD5 => "scratchbird.auth.md5_legacy",
      Protocol::AUTH_SCRAM_SHA256 => "scratchbird.auth.scram_sha_256",
      Protocol::AUTH_SCRAM_SHA512 => "scratchbird.auth.scram_sha_512",
      Protocol::AUTH_TOKEN => "scratchbird.auth.authkey_token",
      Protocol::AUTH_PEER => "scratchbird.auth.peer_uid",
      Protocol::AUTH_REATTACH => "scratchbird.auth.reattach"
    }.freeze

    MetadataQueryResult = Struct.new(:rows, :rowcount, :fields, :command, :last_insert_id, keyword_init: true) do
      def each
        return enum_for(:each) unless block_given?
        rows.each { |row| yield row }
      end

      def each_hash
        return enum_for(:each_hash) unless block_given?
        rows.each { |row| yield row }
      end
    end

    attr_reader :parameters, :txn_id

    def initialize(config)
      @config = config
      @socket = nil
      @connected = false
      @attachment_id = "\0" * 16
      @txn_id = 0
      @sequence = 0
      @last_query_sequence = 0
      @parameters = {}
      @prepared = {}
      @socket_timeout = config.socket_timeout_ms.to_i
      @last_max_rows = 0
      @notification_handlers = []
      @last_plan = nil
      @last_sblr = nil
      @connection_id = "conn-#{object_id}"
      @circuit_breaker = CircuitBreaker.new(CircuitBreakerConfig.new, "ruby")
      @telemetry = TelemetryCollector.new
      @keepalive_manager = KeepaliveManager.new
      @keepalive_tracker = nil
      @leak_detector = LeakDetector.new
      @leak_guard = nil
      @cancel_requested = false
      @cancel_timeout_seconds = 0.2
      @active_thread = nil
      @transaction_active = false
      @synthetic_txn_id = 0
      @portal_resume_pending = false
      reset_resolved_auth_context
    end

    def connect
      begin
        reset_resolved_auth_context
        close
        clear_abandoned_session_state
        open_socket(require_identity: true, require_manager_token: true)
        perform_manager_connect if @config.front_door_mode == "manager_proxy"
        handshake
        apply_schema
        @connected = true
        @resolved_auth_context.attached = true
        @keepalive_manager.start
        @keepalive_tracker = @keepalive_manager.register(@connection_id, self) { ping }
        @leak_detector.start
        @leak_guard = @leak_detector.checkout(@connection_id, driver: "ruby")
      rescue StandardError
        close
        raise
      end
    end

    def connected?
      @connected
    end

    def close
      socket = @socket
      begin
        socket.close if socket
      rescue IOError, SystemCallError
        nil
      ensure
        @socket = nil
        @connected = false
        if @keepalive_tracker
          begin
            @keepalive_manager.unregister(@connection_id)
          rescue StandardError
            nil
          ensure
            @keepalive_tracker = nil
          end
        end
        begin
          @keepalive_manager.stop
        rescue StandardError
          nil
        end
        if @leak_guard
          begin
            @leak_guard.release
          rescue StandardError
            nil
          ensure
            @leak_guard = nil
          end
        end
        begin
          @leak_detector.stop
        rescue StandardError
          nil
        end
        @resolved_auth_context.attached = false
        clear_abandoned_session_state
      end
      true
    end

    def disconnect
      close
    end

    def get_resolved_auth_context
      ResolvedAuthContext.new(**@resolved_auth_context.to_h)
    end

    def probe_auth_surface
      reset_resolved_auth_context
      open_socket(require_identity: false, require_manager_token: false)
      resolved_host = @config.host.to_s.empty? ? "localhost" : @config.host
      resolved_port = @config.port.to_i.zero? ? 3092 : @config.port.to_i
      if @config.front_door_mode == "manager_proxy"
        probe_manager_auth_surface(resolved_host, resolved_port)
      else
        probe_direct_auth_surface(resolved_host, resolved_port)
      end
    ensure
      disconnect_socket_for_reconnect
    end

    def begin_transaction(options = nil)
      ensure_connected
      opts = options || {}
      read_committed_mode = opts[:read_committed_mode]
      isolation_level = opts.fetch(:isolation_level, Protocol::ISOLATION_READ_COMMITTED)
      flags = 0
      flags |= Protocol::TXN_FLAG_HAS_ISOLATION if opts.key?(:isolation_level)
      if read_committed_mode
        if opts.key?(:isolation_level) &&
           ![Protocol::ISOLATION_READ_UNCOMMITTED, Protocol::ISOLATION_READ_COMMITTED].include?(isolation_level)
          raise NotSupportedError, "read_committed_mode requires a READ COMMITTED isolation alias"
        end
        flags |= Protocol::TXN_FLAG_HAS_READ_COMMITTED_MODE
        unless opts.key?(:isolation_level)
          isolation_level = Protocol::ISOLATION_READ_COMMITTED
          flags |= Protocol::TXN_FLAG_HAS_ISOLATION
        end
      end
      flags |= Protocol::TXN_FLAG_HAS_ACCESS if opts.key?(:access_mode)
      flags |= Protocol::TXN_FLAG_HAS_DEFERRABLE if opts.key?(:deferrable)
      flags |= Protocol::TXN_FLAG_HAS_WAIT if opts.key?(:wait)
      flags |= Protocol::TXN_FLAG_HAS_TIMEOUT if opts.key?(:timeout_ms)
      flags |= Protocol::TXN_FLAG_HAS_AUTOCOMMIT if opts.key?(:autocommit_mode)
      payload = Protocol.build_txn_begin_payload(
        flags,
        opts.fetch(:conflict_action, 0),
        opts.fetch(:autocommit_mode, 0),
        isolation_level,
        opts.fetch(:access_mode, 0),
        opts[:deferrable] ? 1 : 0,
        opts[:wait] ? 1 : 0,
        opts.fetch(:timeout_ms, 0),
        read_committed_mode || Protocol::READ_COMMITTED_MODE_DEFAULT
      )
      send_message(Protocol::MSG_TXN_BEGIN, payload, 0, false)
      drain_until_ready
      adopt_transaction_after_begin
    end

    def commit
      ensure_connected
      payload = Protocol.build_txn_commit_payload(0)
      send_message(Protocol::MSG_TXN_COMMIT, payload, 0, false)
      drain_until_ready
    end

    def rollback
      ensure_connected
      payload = Protocol.build_txn_rollback_payload(0)
      send_message(Protocol::MSG_TXN_ROLLBACK, payload, 0, false)
      drain_until_ready
    end

    def supports_prepared_transactions?
      true
    end

    def supports_dormant_reattach?
      false
    end

    def prepare_transaction(gid)
      ensure_connected
      sql = build_prepared_transaction_sql("PREPARE TRANSACTION", gid)
      with_resilience("prepare_transaction", sql) do
        send_simple_query(sql, nil)
        drain_until_ready
        true
      end
    end

    def commit_prepared(gid)
      ensure_connected
      sql = build_prepared_transaction_sql("COMMIT PREPARED", gid)
      with_resilience("commit_prepared", sql) do
        send_simple_query(sql, nil)
        drain_until_ready
        true
      end
    end

    def rollback_prepared(gid)
      ensure_connected
      sql = build_prepared_transaction_sql("ROLLBACK PREPARED", gid)
      with_resilience("rollback_prepared", sql) do
        send_simple_query(sql, nil)
        drain_until_ready
        true
      end
    end

    def detach_to_dormant
      raise NotSupportedError, "dormant detach/reattach is not yet exposed by the public Ruby driver surface"
    end

    def reattach_dormant(_dormant_id, _auth_token = nil)
      raise NotSupportedError, "dormant detach/reattach is not yet exposed by the public Ruby driver surface"
    end

    def allow_portal_resume!
      @portal_resume_pending = true
    end

    def savepoint(name)
      ensure_connected
      payload = Protocol.build_txn_savepoint_payload(name)
      send_message(Protocol::MSG_TXN_SAVEPOINT, payload, 0, false)
      drain_until_ready
    end

    def release_savepoint(name)
      ensure_connected
      payload = Protocol.build_txn_release_payload(name)
      send_message(Protocol::MSG_TXN_RELEASE, payload, 0, false)
      drain_until_ready
    end

    def rollback_to_savepoint(name)
      ensure_connected
      payload = Protocol.build_txn_rollback_to_payload(name)
      send_message(Protocol::MSG_TXN_ROLLBACK_TO, payload, 0, false)
      drain_until_ready
    end

    def set_option(name, value)
      ensure_connected
      payload = Protocol.build_set_option_payload(name, value)
      send_message(Protocol::MSG_SET_OPTION, payload, 0, false)
      drain_until_ready
    end

    def ping
      ensure_connected
      send_message(Protocol::MSG_PING, +"", 0, false)
      loop do
        type, _flags, payload, _sequence, _attachment_id, _txn_id = recv_message
        next if handle_async_message(type, payload)
        case type
        when Protocol::MSG_PONG
          return true
        when Protocol::MSG_READY
          status, txn_id = Protocol.parse_ready(payload)
          apply_runtime_ready_state(status, txn_id)
          return true
        when Protocol::MSG_ERROR
          handle_query_error(payload)
        end
      end
    end

    def subscribe(channel, sub_type = Protocol::SUB_TYPE_CHANNEL, filter_expr = "")
      ensure_connected
      payload = Protocol.build_subscribe_payload(sub_type, channel, filter_expr)
      send_message(Protocol::MSG_SUBSCRIBE, payload, 0, false)
      drain_until_ready
    end

    def unsubscribe(channel)
      ensure_connected
      payload = Protocol.build_unsubscribe_payload(channel)
      send_message(Protocol::MSG_UNSUBSCRIBE, payload, 0, false)
      drain_until_ready
    end

    def execute_sblr(hash, bytecode = nil, params = [])
      ensure_connected
      with_resilience("sblr_execute", nil) do
        values = params.map do |param|
          encoded = Types.encode_param(param)
          encoded[:param]
        end
        payload = Protocol.build_sblr_execute_payload(hash, bytecode, values)
        send_message(Protocol::MSG_SBLR_EXECUTE, payload, 0, false)
        send_message(Protocol::MSG_SYNC, +"", 0, false)
        ResultStream.new(self)
      end
    end

    def stream_control(control_type, window_size, timeout_ms)
      ensure_connected
      payload = Protocol.build_stream_control_payload(control_type, window_size, timeout_ms)
      send_message(Protocol::MSG_STREAM_CONTROL, payload, 0, false)
    end

    def attach_create(emulation_mode, db_name)
      ensure_connected
      payload = Protocol.build_attach_create_payload(emulation_mode, db_name)
      send_message(Protocol::MSG_ATTACH_CREATE, payload, 0, false)
      drain_until_ready
    end

    def attach_detach
      ensure_connected
      send_message(Protocol::MSG_ATTACH_DETACH, +"", 0, false)
      drain_until_ready
    end

    def attach_list
      ensure_connected
      send_message(Protocol::MSG_ATTACH_LIST, +"", 0, false)
      send_message(Protocol::MSG_SYNC, +"", 0, false)
      ResultStream.new(self)
    end

    def on_notification(&block)
      @notification_handlers << block if block
    end

    def last_plan
      @last_plan
    end

    def last_sblr
      @last_sblr
    end

    def query(sql, params = nil, options = nil)
      ensure_connected
      normalized = Sql.normalize(sql, params)
      execute_query(normalized.sql, normalized.params, options)
    end

    def stream(sql, params = nil, options = nil)
      ensure_connected
      normalized = Sql.normalize(sql, params)
      execute_query_stream(normalized.sql, normalized.params, options)
    end

    def native_sql(sql, params = nil)
      Sql.normalize(sql, params).sql
    rescue ArgumentError => e
      raise SyntaxError.new(e.message, "07001")
    end

    def native_callable_sql(sql, params = nil)
      Sql.normalize_callable(sql, params).sql
    rescue ArgumentError => e
      raise SyntaxError.new(e.message, "07001")
    end

    def call(sql, params = nil, options = nil)
      ensure_connected
      normalized = Sql.normalize_callable(sql, params)
      execute_query(normalized.sql, normalized.params, options)
    end

    def query_multi(sql, params = nil, options = nil)
      ensure_connected
      normalized = Sql.normalize(sql, params)
      results = execute_query_multi(normalized.sql, normalized.params, options)
      if results.length <= 1 && normalized.params.empty?
        statements = split_sql_statements(normalized.sql)
        if statements.length > 1
          results = statements.filter_map do |statement|
            stripped = statement.to_s.strip
            next if stripped.empty?
            execute_query(stripped, [], options)
          end
        end
      end
      results.map { |result| summarize_result(result) }
    end

    def execute_multi(sql, params = nil, options = nil)
      query_multi(sql, params, options)
    end

    def execute_batch(sql, batch_params, options = nil)
      params_set = Array(batch_params)
      raise ArgumentError, "batch parameters are required" if params_set.empty?

      items = []
      total_rowcount = 0
      params_set.each_with_index do |entry, idx|
        result_sets = query_multi(sql, entry, options)
        rowcount = result_sets.sum { |set| [set.rowcount.to_i, 0].max }
        fields = result_sets.reverse.find { |set| !Array(set.fields).empty? }&.fields || []
        command = result_sets.reverse.find { |set| !set.command.to_s.empty? }&.command.to_s
        last_insert_id = result_sets.reverse.find { |set| set.last_insert_id.to_i != 0 }&.last_insert_id.to_i
        total_rowcount += rowcount
        items << BatchItemSummary.new(
          index: idx,
          rowcount: rowcount,
          fields: fields,
          command: command,
          last_insert_id: last_insert_id
        )
      end

      BatchSummary.new(items: items, total_rowcount: total_rowcount)
    end

    def query_batch(sql, batch_params, options = nil)
      execute_batch(sql, batch_params, options)
    end

    def execute_with_generated_keys(sql, params = nil, options = nil)
      query_multi(sql, params, options)
        .map(&:last_insert_id)
        .map(&:to_i)
        .reject(&:zero?)
    end

    def query_metadata(collection_name = "tables", options = nil)
      query_metadata_with_restrictions(collection_name, nil, options)
    end

    def query_metadata_with_restrictions(collection_name = "tables", restrictions = nil, options = nil)
      ensure_connected
      normalized_collection = normalize_metadata_collection_name(collection_name)
      result = query(Metadata.resolve_collection_query(normalized_collection), nil, options)
      metadata_result_with_restrictions(result, normalized_collection, restrictions)
    end

    def get_schema(collection_name = "tables", options = nil, expand_schema_parents: nil)
      get_schema_with_restrictions(collection_name, nil, options, expand_schema_parents: expand_schema_parents)
    end

    def get_schema_with_restrictions(collection_name = "tables", restrictions = nil, options = nil, expand_schema_parents: nil)
      normalized_collection = normalize_metadata_collection_name(collection_name)
      result = query_metadata_with_restrictions(normalized_collection, restrictions, options)
      rows = result.respond_to?(:each_hash) ? result.each_hash.to_a : []
      return rows unless normalized_collection == "schemas"

      expand = metadata_expand_schema_parents?(expand_schema_parents)
      return rows unless expand

      Metadata.expand_schema_metadata_rows(rows)
    end

    def get_schema_tree(expand_schema_parents: nil, database: nil, restrictions: nil)
      rows = get_schema_with_restrictions(
        "schemas",
        restrictions,
        nil,
        expand_schema_parents: expand_schema_parents
      )
      Metadata.build_schema_tree(
        Metadata.schema_paths_for_navigation(
          rows,
          expand_schema_parents: metadata_expand_schema_parents?(expand_schema_parents)
        )
      )
    end

    def prepare(name, sql)
      raise ArgumentError, "name is required" if name.to_s.empty?
      ensure_connected
      prepared_sql = Sql.normalize_prepared_sql(sql)
      payload = Protocol.build_parse_payload(name, prepared_sql, [])
      send_message(Protocol::MSG_PARSE, payload, 0, false)
      param_count = describe_statement(name)
      @prepared[name] = { sql: sql, prepared_sql: prepared_sql, param_count: param_count }
    end

    def execute(name, params = nil, options = nil)
      ensure_connected
      entry = @prepared[name]
      raise ArgumentError, "unknown prepared statement: #{name}" unless entry
      normalized = Sql.normalize(entry[:sql], params)
      if entry[:param_count].to_i >= 0 && entry[:param_count].to_i != normalized.params.length
        raise Error.new("parameter count mismatch", "07001")
      end
      execute_prepared(name, normalized.params, options)
    end

    def execute_stream(name, params = nil, options = nil)
      ensure_connected
      entry = @prepared[name]
      raise ArgumentError, "unknown prepared statement: #{name}" unless entry
      normalized = Sql.normalize(entry[:sql], params)
      if entry[:param_count].to_i >= 0 && entry[:param_count].to_i != normalized.params.length
        raise Error.new("parameter count mismatch", "07001")
      end
      execute_prepared_stream(name, normalized.params, options)
    end

    def deallocate(name)
      ensure_connected
      statement_name = name.to_s
      raise ArgumentError, "name is required" if statement_name.empty?
      payload = Protocol.build_close_payload(Protocol::DESCRIBE_STATEMENT, statement_name)
      send_message(Protocol::MSG_CLOSE, payload, 0, false)
      send_message(Protocol::MSG_SYNC, +"", 0, false)
      drain_until_ready
      @prepared.delete(statement_name)
      true
    end

    def cancel
      ensure_connected
      @cancel_requested = true
      payload = Protocol.build_cancel_payload(0, @last_query_sequence.to_i)
      send_message(Protocol::MSG_CANCEL, payload, Protocol::MSG_FLAG_URGENT, false)
      if @active_thread && @active_thread.alive? && @active_thread != Thread.current
        @active_thread.raise(OperatorInterventionError.new("query canceled", "57014"))
      end
      begin
        @socket.close if @socket
      rescue IOError, SystemCallError
        nil
      end
    end

    def update_txn_id(txn_id)
      apply_runtime_txn_id(txn_id)
    end

    def update_ready_state(status, txn_id)
      apply_runtime_ready_state(status, txn_id)
    end

    def in_transaction?
      @transaction_active || @txn_id.to_i != 0
    end

    def recv_message
      header = read_exact(Protocol::HEADER_SIZE)
      type, flags, length, sequence, attachment_id, txn_id = Protocol.decode_header(header)
      payload = length.positive? ? read_exact(length) : +""
      clear_cancel_request
      [type, flags, payload, sequence, attachment_id, txn_id]
    end

    def decode_row(columns, values)
      row = []
      values.each_with_index do |value, idx|
        col = columns[idx]
        type_oid = col ? col[:type_oid] : 0
        format = col ? col[:format] : Types::FORMAT_BINARY
        row << Types.decode(type_oid, value[:data], format)
      end
      row
    end

    def handle_query_error(payload)
      clear_transaction_state if @transaction_active
      raise build_query_error(payload)
    end

    def build_query_error(payload)
      _severity, sqlstate, message, detail, hint = Protocol.parse_error_message(payload)
      parts = []
      parts << message if message && !message.empty?
      parts << "DETAIL: #{detail}" if detail && !detail.empty?
      parts << "HINT: #{hint}" if hint && !hint.empty?
      text = parts.empty? ? "query failed" : parts.join("\n")
      text = "[#{sqlstate}] #{text}" if sqlstate && !sqlstate.empty?
      ErrorMapper.from_sqlstate(sqlstate, text, detail, hint)
    rescue Error => e
      e
    rescue StandardError
      Error.new("query failed")
    end

    def drain_until_ready
      pending_error = nil
      loop do
        type, _flags, payload, _sequence, _attachment_id, _txn_id = recv_message
        if handle_async_message(type, payload)
          next
        end
        if type == Protocol::MSG_ERROR
          pending_error ||= build_query_error(payload)
          next
        end
        if type == Protocol::MSG_READY
          status, txn_id = Protocol.parse_ready(payload)
          apply_runtime_ready_state(status, txn_id)
          raise pending_error if pending_error
          return
        end
      end
    end

    private

    def handle_async_message(type, payload)
      case type
      when Protocol::MSG_PARAMETER_STATUS
        name, value = Protocol.parse_parameter_status(payload)
        @parameters[name] = value
        update_attachment_from_param(name, value)
        true
      when Protocol::MSG_NOTIFICATION
        notice = Protocol.parse_notification(payload)
        @notification_handlers.each { |handler| handler.call(notice) }
        true
      when Protocol::MSG_QUERY_PLAN
        @last_plan = Protocol.parse_query_plan(payload)
        true
      when Protocol::MSG_SBLR_COMPILED
        @last_sblr = Protocol.parse_sblr_compiled(payload)
        true
      else
        false
      end
    end

    def update_attachment_from_param(name, value)
      case name
      when "attachment_id"
        parsed = parse_uuid_bytes(value)
        @attachment_id = parsed if parsed
      when "current_txn_id"
        apply_runtime_txn_id(value.to_i)
      end
    end

    def parse_uuid_bytes(value)
      hex = value.to_s.delete("-").strip
      return nil unless hex.match?(/\A[0-9a-fA-F]{32}\z/)
      [hex].pack("H*")
    end

    def normalize_metadata_collection_name(collection_name)
      Metadata.normalize_collection_name(collection_name)
    rescue ArgumentError => e
      raise NotSupportedError, e.message
    end

    def metadata_expand_schema_parents?(override)
      return override unless override.nil?
      return false unless @config.respond_to?(:metadata_expand_schema_parents)

      @config.metadata_expand_schema_parents == true
    end

    def metadata_result_with_restrictions(result, normalized_collection, restrictions)
      normalized_restrictions = Metadata.normalize_restrictions(restrictions)
      return result if normalized_restrictions.empty?

      rows = result.respond_to?(:each_hash) ? result.each_hash.to_a : []
      filtered_rows = Metadata.filter_rows_by_restrictions(
        rows,
        normalized_restrictions,
        collection_name: normalized_collection
      )

      MetadataQueryResult.new(
        rows: filtered_rows,
        rowcount: filtered_rows.length,
        fields: metadata_result_fields(result, filtered_rows),
        command: metadata_result_command(result),
        last_insert_id: metadata_result_last_insert_id(result)
      )
    rescue ArgumentError => e
      raise NotSupportedError, e.message
    end

    def metadata_result_fields(result, rows)
      return Array(result.fields) if result.respond_to?(:fields)
      return rows.first.keys.map(&:to_s) if rows.first.is_a?(Hash)

      []
    end

    def metadata_result_command(result)
      return result.command_tag.to_s if result.respond_to?(:command_tag)
      return result.command.to_s if result.respond_to?(:command)

      "SELECT"
    end

    def metadata_result_last_insert_id(result)
      return result.last_insert_id.to_i if result.respond_to?(:last_insert_id)

      0
    end

    def reset_resolved_auth_context
      ingress_mode = Config.normalize_front_door_mode(@config.front_door_mode)
      @resolved_auth_context = ResolvedAuthContext.new(
        ingress_mode: ingress_mode,
        resolved_auth_method: nil,
        resolved_auth_plugin_id: nil,
        manager_authenticated: false,
        attached: false
      )
    rescue ArgumentError
      @resolved_auth_context = ResolvedAuthContext.new(
        ingress_mode: @config.front_door_mode.to_s,
        resolved_auth_method: nil,
        resolved_auth_plugin_id: nil,
        manager_authenticated: false,
        attached: false
      )
    end

    def auth_method_name(method)
      {
        Protocol::AUTH_PASSWORD => "PASSWORD",
        Protocol::AUTH_MD5 => "MD5",
        Protocol::AUTH_SCRAM_SHA256 => "SCRAM_SHA_256",
        Protocol::AUTH_SCRAM_SHA512 => "SCRAM_SHA_512",
        Protocol::AUTH_TOKEN => "TOKEN",
        Protocol::AUTH_PEER => "PEER",
        Protocol::AUTH_REATTACH => "REATTACH"
      }[method]
    end

    def auth_plugin_id_for_method(method)
      configured = @config.auth_method_id.to_s.strip
      return configured unless configured.empty?

      DEFAULT_AUTH_PLUGIN_IDS[method]
    end

    def auth_method_executable_locally?(method)
      [
        Protocol::AUTH_PASSWORD,
        Protocol::AUTH_SCRAM_SHA256,
        Protocol::AUTH_SCRAM_SHA512,
        Protocol::AUTH_TOKEN
      ].include?(method)
    end

    def auth_method_broker_required?(method)
      method == Protocol::AUTH_PEER
    end

    def describe_auth_method(method)
      wire_method = auth_method_name(method)
      return nil if wire_method.nil?

      AuthMethodSurface.new(
        wire_method: wire_method,
        plugin_method_id: auth_plugin_id_for_method(method),
        executable_locally: auth_method_executable_locally?(method),
        broker_required: auth_method_broker_required?(method)
      )
    end

    def resolve_token_auth_payload
      return @config.auth_token.to_s.b unless @config.auth_token.to_s.empty?
      return @config.auth_method_payload.to_s.b unless @config.auth_method_payload.to_s.empty?
      unless @config.auth_payload_b64.to_s.empty?
        begin
          return Base64.strict_decode64(@config.auth_payload_b64)
        rescue ArgumentError
          raise DataError.new("invalid auth_payload_b64 encoding", "22023")
        end
      end
      return @config.auth_payload_json.to_s.b unless @config.auth_payload_json.to_s.empty?
      return @config.workload_identity_token.to_s.b unless @config.workload_identity_token.to_s.empty?
      return @config.proxy_principal_assertion.to_s.b unless @config.proxy_principal_assertion.to_s.empty?

      raise AuthError.new(
        "TOKEN authentication requires auth_token, auth_method_payload, auth_payload_json, " \
        "auth_payload_b64, workload_identity_token, or proxy_principal_assertion",
        "28000"
      )
    end

    def build_startup_params
      params = {
        "database" => @config.database,
        "user" => @config.user,
        "client_flags" => @config.connect_client_flags.to_i.to_s
      }
      params["role"] = @config.role if @config.role.to_s != ""
      params["application_name"] = @config.application_name if @config.application_name.to_s != ""
      if @config.auth_method_id.to_s != ""
        unless @config.auth_method_id.start_with?("scratchbird.auth.")
          raise AuthError, "invalid auth_method_id namespace"
        end
        params["auth_method_id"] = @config.auth_method_id
      end
      params["auth_method_payload"] = @config.auth_method_payload if @config.auth_method_payload.to_s != ""
      params["auth_payload_json"] = @config.auth_payload_json if @config.auth_payload_json.to_s != ""
      params["auth_payload_b64"] = @config.auth_payload_b64 if @config.auth_payload_b64.to_s != ""
      params["auth_provider_profile"] = @config.auth_provider_profile if @config.auth_provider_profile.to_s != ""
      params["auth_required_methods"] = @config.auth_required_methods if @config.auth_required_methods.to_s != ""
      params["auth_forbidden_methods"] = @config.auth_forbidden_methods if @config.auth_forbidden_methods.to_s != ""
      params["auth_require_channel_binding"] = "1" if @config.auth_require_channel_binding
      params["workload_identity_token"] = @config.workload_identity_token if @config.workload_identity_token.to_s != ""
      params["proxy_principal_assertion"] = @config.proxy_principal_assertion if @config.proxy_principal_assertion.to_s != ""
      params
    end

    def open_socket(require_identity:, require_manager_token:)
      begin
        @config.protocol = Config.normalize_native_protocol(@config.protocol)
        @config.front_door_mode = Config.normalize_front_door_mode(@config.front_door_mode)
        @config.transport = Config.normalize_transport(@config.transport)
      rescue ArgumentError => e
        raise NotSupportedError, e.message
      end
      raise ConnectionError, "user and database are required" if require_identity && (@config.user.to_s.empty? || @config.database.to_s.empty?)
      if require_manager_token && @config.front_door_mode == "manager_proxy" && @config.manager_auth_token.to_s.empty?
        raise ConnectionError, "manager_proxy mode requires manager_auth_token"
      end
      if @config.transport == "embedded"
        raise NotSupportedError, "embedded transport is not supported by the Ruby driver; no ScratchBird C++ library boundary is exposed"
      end
      if @config.transport == "ipc"
        @socket = connect_ipc
        return
      end
      raw_socket = connect_tcp
      @socket = wrap_tls(raw_socket)
    end

    def disconnect_socket_for_reconnect
      socket = @socket
      begin
        socket.close if socket
      rescue IOError, SystemCallError
        nil
      ensure
        @socket = nil
        @connected = false
        @resolved_auth_context.attached = false if @resolved_auth_context
        clear_abandoned_session_state
      end
    end

    def probe_direct_auth_surface(resolved_host, resolved_port)
      features = 0
      features |= Protocol::FEATURE_COMPRESSION if @config.compression.to_s.downcase == "zstd"
      features |= Protocol::FEATURE_STREAMING if @config.binary_transfer
      startup = Protocol.build_startup_payload(features, build_startup_params)
      send_message(Protocol::MSG_STARTUP, startup, 0, true)

      loop do
        type, _flags, payload, _sequence, _attachment_id, _txn_id = recv_message
        next if handle_async_message(type, payload)
        next if type == Protocol::MSG_NEGOTIATE_VERSION

        case type
        when Protocol::MSG_AUTH_REQUEST
          method, _data = Protocol.parse_auth_request(payload)
          surface = describe_auth_method(method)
          admitted_methods = surface.nil? ? [] : [surface]
          return AuthProbeResult.new(
            reachable: true,
            ingress_mode: "direct",
            resolved_host: resolved_host,
            resolved_port: resolved_port,
            admitted_methods: admitted_methods,
            required_method: surface&.wire_method,
            required_plugin_method_id: surface&.plugin_method_id,
            allowed_transport_mask: nil,
            additional_continuation_possible: [
              Protocol::AUTH_SCRAM_SHA256,
              Protocol::AUTH_SCRAM_SHA512,
              Protocol::AUTH_TOKEN,
              Protocol::AUTH_PEER
            ].include?(method)
          )
        when Protocol::MSG_AUTH_OK, Protocol::MSG_READY
          return AuthProbeResult.new(
            reachable: true,
            ingress_mode: "direct",
            resolved_host: resolved_host,
            resolved_port: resolved_port,
            admitted_methods: [],
            required_method: nil,
            required_plugin_method_id: nil,
            allowed_transport_mask: nil,
            additional_continuation_possible: false
          )
        when Protocol::MSG_ERROR
          handle_query_error(payload)
        end
      end
    end

    def probe_manager_auth_surface(resolved_host, resolved_port)
      manager_user = @config.manager_username.to_s.empty? ? (@config.user.to_s.empty? ? "admin" : @config.user) : @config.manager_username
      hello = [MCP_PROTOCOL_VERSION, @config.manager_client_flags.to_i & 0xFFFF].pack("v2")
      send_manager_frame(MCP_MSG_HELLO, hello)
      type, _payload = recv_manager_frame
      raise ConnectionError, "expected MCP hello status response" unless type == MCP_MSG_STATUS_RESPONSE

      auth_start = +""
      auth_start << manager_lpref(manager_user)
      auth_start << [MCP_AUTH_METHOD_TOKEN].pack("C")
      auth_start << [0].pack("V")
      send_manager_frame(MCP_MSG_AUTH_START, auth_start)
      type, _payload = recv_manager_frame

      AuthProbeResult.new(
        reachable: true,
        ingress_mode: "manager_proxy",
        resolved_host: resolved_host,
        resolved_port: resolved_port,
        admitted_methods: [
          AuthMethodSurface.new(
            wire_method: "TOKEN",
            plugin_method_id: auth_plugin_id_for_method(Protocol::AUTH_TOKEN),
            executable_locally: true,
            broker_required: false
          )
        ],
        required_method: "TOKEN",
        required_plugin_method_id: auth_plugin_id_for_method(Protocol::AUTH_TOKEN),
        allowed_transport_mask: nil,
        additional_continuation_possible: type == MCP_MSG_AUTH_CHALLENGE
      )
    end

    def connect_tcp
      timeout = @config.connect_timeout_ms.to_i / 1000.0
      socket = Socket.tcp(@config.host, @config.port, connect_timeout: timeout)
      socket.setsockopt(Socket::IPPROTO_TCP, Socket::TCP_NODELAY, 1)
      socket
    end

    def connect_ipc
      path = @config.ipc_path.to_s
      raise ConnectionError, "ipc_path is required for local IPC transport" if path.empty?
      unless defined?(UNIXSocket)
        raise NotSupportedError, "Unix-domain socket IPC transport is not supported by this Ruby runtime"
      end
      UNIXSocket.new(path)
    end

    def wrap_tls(raw_socket)
      mode = @config.sslmode.to_s.downcase
      return raw_socket if mode == "disable"

      ctx = OpenSSL::SSL::SSLContext.new
      if ctx.respond_to?(:min_version=) && defined?(OpenSSL::SSL::TLS1_3_VERSION)
        ctx.min_version = OpenSSL::SSL::TLS1_3_VERSION
      end
      verify = %w[verify-full verify-ca require].include?(mode)
      ctx.verify_mode = verify ? OpenSSL::SSL::VERIFY_PEER : OpenSSL::SSL::VERIFY_NONE
      ctx.ca_file = @config.sslrootcert if @config.sslrootcert
      if @config.sslcert && @config.sslkey
        ctx.cert = OpenSSL::X509::Certificate.new(File.read(@config.sslcert))
        ctx.key = OpenSSL::PKey.read(File.read(@config.sslkey), @config.sslpassword)
      end

      ssl_socket = OpenSSL::SSL::SSLSocket.new(raw_socket, ctx)
      ssl_socket.sync_close = true
      ssl_socket.hostname = @config.host if ssl_socket.respond_to?(:hostname=)
      ssl_socket.connect
      if mode == "verify-full" && ssl_socket.respond_to?(:post_connection_check)
        ssl_socket.post_connection_check(@config.host)
      end
      ssl_socket
    rescue OpenSSL::SSL::SSLError
      raw_socket.close
      raise
    end

    def manager_lpref(text)
      encoded = text.to_s.b
      [encoded.bytesize].pack("V") + encoded
    end

    def send_manager_frame(type, payload)
      frame = +""
      frame << [MANAGER_PROTOCOL_MAGIC].pack("V")
      frame << [MANAGER_PROTOCOL_VERSION].pack("v")
      frame << [type, 0].pack("C2")
      frame << [payload.bytesize].pack("V")
      frame << payload
      total = 0
      while total < frame.bytesize
        written = @socket.write(frame.byteslice(total, frame.bytesize - total))
        raise ConnectionError, "manager frame write failed" if written.nil? || written.zero?
        total += written
      end
    end

    def recv_manager_frame
      header = read_exact(MANAGER_HEADER_SIZE)
      magic = header.byteslice(0, 4).unpack1("V")
      raise ConnectionError, "manager frame magic mismatch" unless magic == MANAGER_PROTOCOL_MAGIC
      version = header.byteslice(4, 2).unpack1("v")
      raise ConnectionError, "manager frame version mismatch" unless version == MANAGER_PROTOCOL_VERSION
      type = header.getbyte(6)
      length = header.byteslice(8, 4).unpack1("V")
      raise ConnectionError, "manager payload too large" if length > MANAGER_MAX_PAYLOAD_SIZE
      payload = length.positive? ? read_exact(length) : +""
      [type, payload]
    end

    def perform_manager_connect
      token = @config.manager_auth_token.to_s
      raise ConnectionError, "manager_proxy mode requires manager_auth_token" if token.empty?

      manager_user = @config.manager_username.to_s.empty? ? (@config.user.to_s.empty? ? "admin" : @config.user) : @config.manager_username
      manager_database = @config.manager_database.to_s.empty? ? @config.database.to_s : @config.manager_database
      manager_profile = @config.manager_connection_profile.to_s.empty? ? "SBsql" : @config.manager_connection_profile
      manager_intent = @config.manager_client_intent.to_s.empty? ? "SBsql" : @config.manager_client_intent
      manager_flags = @config.manager_client_flags.to_i & 0xFFFF
      auth_fast_path = @config.manager_auth_fast_path != false

      hello = [MCP_PROTOCOL_VERSION, manager_flags].pack("v2")
      send_manager_frame(MCP_MSG_HELLO, hello)
      type, _payload = recv_manager_frame
      raise ConnectionError, "expected MCP hello status response" unless type == MCP_MSG_STATUS_RESPONSE

      auth_start = +""
      auth_start << manager_lpref(manager_user)
      auth_start << [MCP_AUTH_METHOD_TOKEN].pack("C")
      if auth_fast_path
        auth_start << [token.bytesize].pack("V")
        auth_start << token.b
      else
        auth_start << [0].pack("V")
      end
      send_manager_frame(MCP_MSG_AUTH_START, auth_start)
      type, payload = recv_manager_frame
      if type == MCP_MSG_AUTH_CHALLENGE
        auth_continue = [token.bytesize].pack("V") + token.b
        send_manager_frame(MCP_MSG_AUTH_CONTINUE, auth_continue)
        type, payload = recv_manager_frame
      end
      raise ConnectionError, "expected MCP auth response" unless type == MCP_MSG_AUTH_RESPONSE
      raise ConnectionError, "truncated MCP auth response" if payload.bytesize < (1 + 4 + 256)
      if payload.getbyte(0) != 0
        err = payload.byteslice(5, 256).to_s.sub(/\x00+\z/, "")
        raise AuthError, (err.empty? ? "MCP authentication failed" : err)
      end

      db_connect = +"MCP1"
      db_connect << manager_lpref(manager_database)
      db_connect << manager_lpref(manager_profile)
      db_connect << manager_lpref(manager_intent)
      nonce = OpenSSL::Random.random_bytes(16)
      db_connect << [nonce.bytesize].pack("v")
      db_connect << nonce
      send_manager_frame(MCP_MSG_DB_CONNECT, db_connect)
      type, payload = recv_manager_frame
      raise ConnectionError, "expected MCP connect response" unless type == MCP_MSG_CONNECT_RESPONSE
      raise ConnectionError, "truncated MCP connect response" if payload.bytesize < (1 + 2 + 2 + 16 + 64 + 32)
      if payload.getbyte(0) != 0
        err = "MCP database connect failed"
        err_offset = 1 + 2 + 2 + 16 + 64 + 32
        if payload.bytesize >= err_offset + 4
          err_len = payload.byteslice(err_offset, 4).unpack1("V")
          if payload.bytesize >= err_offset + 4 + err_len
            err = payload.byteslice(err_offset + 4, err_len).to_s
          end
        end
        raise AuthError, err
      end
      @resolved_auth_context.manager_authenticated = true
    end

    def handshake
      reset_resolved_auth_context if @resolved_auth_context.nil?
      features = 0
      features |= Protocol::FEATURE_COMPRESSION if @config.compression.to_s.downcase == "zstd"
      features |= Protocol::FEATURE_STREAMING if @config.binary_transfer
      startup = Protocol.build_startup_payload(features, build_startup_params)
      send_message(Protocol::MSG_STARTUP, startup, 0, true)

      scram = nil

      loop do
        type, _flags, payload, _sequence, attachment_id, txn_id = recv_message
        if handle_async_message(type, payload)
          next
        end
        case type
        when Protocol::MSG_NEGOTIATE_VERSION
          next
        when Protocol::MSG_AUTH_REQUEST
          method, data = Protocol.parse_auth_request(payload)
          if method == Protocol::AUTH_OK
            next
          end
          @resolved_auth_context.resolved_auth_method = auth_method_name(method)
          @resolved_auth_context.resolved_auth_plugin_id = auth_plugin_id_for_method(method)
          if method == Protocol::AUTH_PASSWORD
            send_message(Protocol::MSG_AUTH_RESPONSE, @config.password.to_s, 0, true)
            next
          end
          if [Protocol::AUTH_SCRAM_SHA256, Protocol::AUTH_SCRAM_SHA512].include?(method)
            scram ||= Scram.new(@config.user, method == Protocol::AUTH_SCRAM_SHA512 ? "sha512" : "sha256")
            client_first = scram.client_first_message
            send_message(Protocol::MSG_AUTH_RESPONSE, client_first, 0, true)
            next
          end
          if method == Protocol::AUTH_TOKEN
            send_message(Protocol::MSG_AUTH_RESPONSE, resolve_token_auth_payload, 0, true)
            next
          end
          if method == Protocol::AUTH_MD5
            raise NotSupportedError.new(
              "MD5 authentication is admitted by the server but not executable in the Ruby lane",
              "0A000"
            )
          end
          if method == Protocol::AUTH_PEER
            raise NotSupportedError.new(
              "PEER authentication requires broker or platform assistance in the Ruby lane",
              "0A000"
            )
          end
          if method == Protocol::AUTH_REATTACH
            raise NotSupportedError.new(
              "REATTACH authentication negotiation is not executable through the generic Ruby auth lane",
              "0A000"
            )
          end
          raise NotSupportedError.new("unsupported auth method", "0A000")
        when Protocol::MSG_AUTH_CONTINUE
          method, _stage, data = Protocol.parse_auth_continue(payload)
          unless [Protocol::AUTH_SCRAM_SHA256, Protocol::AUTH_SCRAM_SHA512].include?(method) && scram
            raise NotSupportedError.new("unsupported auth continue", "0A000")
          end
          client_final = scram.handle_server_first(@config.password.to_s, data.to_s)
          send_message(Protocol::MSG_AUTH_RESPONSE, client_final, 0, true)
          next
        when Protocol::MSG_AUTH_OK
          _session_id, server_info = Protocol.parse_auth_ok(payload)
          @attachment_id = attachment_id
          apply_runtime_txn_id(txn_id)
          if scram && server_info.to_s.start_with?("v=")
            scram.verify_server_final(server_info.to_s)
          end
          next
        when Protocol::MSG_PARAMETER_STATUS
          name, value = Protocol.parse_parameter_status(payload)
          @parameters[name] = value
          next
        when Protocol::MSG_READY
          status, txn_id = Protocol.parse_ready(payload)
          apply_runtime_ready_state(status, txn_id)
          return
        when Protocol::MSG_ERROR
          handle_query_error(payload)
        else
          next
        end
      end
    end

    def apply_schema
      schema = @config.schema.to_s.strip
      return if schema.empty? || schema.casecmp("public").zero?
      statement = build_schema_statement(schema)
      return if statement.empty?
      execute_simple(statement)
    end

    def send_message(type, payload, flags, force_zero)
      raise ConnectionError, "no active socket" unless @socket
      sequence = @sequence
      @sequence += 1
      attachment_id = force_zero ? "\0" * 16 : @attachment_id
      txn_id = force_zero ? 0 : @txn_id
      data = Protocol.encode_message(type, payload, flags, sequence, attachment_id, txn_id)
      total = 0
      while total < data.bytesize
        written = @socket.write(data.byteslice(total, data.bytesize - total))
        raise ConnectionError, "socket closed" if written.nil? || written.zero?
        total += written
      end
      sequence
    rescue IOError, SystemCallError => e
      raise ConnectionError, "socket write failed: #{e.message}"
    end

    def read_exact(size)
      raise ConnectionError, "no active socket" unless @socket
      buffer = +""
      while buffer.bytesize < size
        next unless wait_readable
        chunk = @socket.readpartial(size - buffer.bytesize)
        if chunk.nil? || chunk.empty?
          if @cancel_requested
            clear_cancel_request
            raise OperatorInterventionError.new("query canceled", "57014")
          end
          raise ConnectionError, "connection closed"
        end
        buffer << chunk
      end
      buffer
    rescue EOFError
      if @cancel_requested
        clear_cancel_request
        raise OperatorInterventionError.new("query canceled", "57014")
      end
      raise ConnectionError, "connection closed"
    rescue IOError, SystemCallError => e
      if @cancel_requested
        clear_cancel_request
        raise OperatorInterventionError.new("query canceled", "57014")
      end
      raise ConnectionError, "socket read failed: #{e.message}"
    end

    def wait_readable
      timeout = @socket_timeout > 0 ? (@socket_timeout / 1000.0) : 0.25
      if @cancel_requested
        timeout = timeout ? [timeout, @cancel_timeout_seconds].min : @cancel_timeout_seconds
      end
      ready = IO.select([@socket], nil, nil, timeout)
      unless ready
        if @cancel_requested
          clear_cancel_request
          raise OperatorInterventionError.new("query canceled", "57014")
        end
        raise ConnectionError, "socket timed out" if @socket_timeout > 0
        return false
      end
      true
    end

    def ensure_connected
      raise ConnectionError, "client is not connected" unless @connected
    end

    def with_resilience(operation, sql = nil)
      raise CircuitBreakerOpenError, "Circuit breaker is OPEN" unless @circuit_breaker.allow_request?
      if @keepalive_tracker && @keepalive_tracker.needs_validation?
        ping
        @keepalive_tracker.mark_active
      end
      span = @telemetry.start_span(operation)
      if span && sql
        span.with_attribute("db.statement", TelemetryCollector.sanitize_query(sql))
      end
      success = false
      prior_thread = @active_thread
      @active_thread = Thread.current
      begin
        result = yield
        success = true
        @circuit_breaker.record_success
        @keepalive_tracker&.mark_active
        result
      rescue => e
        @circuit_breaker.record_failure
        raise e
      ensure
        @active_thread = prior_thread
        @telemetry.end_span(span, success)
      end
    end

    def execute_query(sql, params, options)
      with_resilience("query", sql) do
        if params.empty?
          send_simple_query(sql, options)
        else
          send_extended_query(sql, params, options)
        end
        execute_query_loop
      end
    end

    def execute_query_multi(sql, params, options)
      with_resilience("query_multi", sql) do
        if params.empty?
          send_simple_query(sql, options)
        else
          send_extended_query(sql, params, options)
        end
        execute_query_multi_loop
      end
    end

    def execute_prepared(name, params, options)
      with_resilience("execute_prepared", nil) do
        send_bind_execute(name, params, options)
        execute_query_loop
      end
    end

    def execute_query_stream(sql, params, options)
      with_resilience("query_stream", sql) do
        if params.empty?
          send_simple_query(sql, options)
        else
          send_extended_query(sql, params, options)
        end
        ResultStream.new(self)
      end
    end

    def execute_prepared_stream(name, params, options)
      with_resilience("execute_prepared_stream", nil) do
        send_bind_execute(name, params, options)
        ResultStream.new(self)
      end
    end

    def execute_query_loop
      columns = []
      rows = []
      rowcount = -1
      command_tag = ""
      last_insert_id = 0

      loop do
        type, _flags, payload, _sequence, _attachment_id, _txn_id = recv_message
        if handle_async_message(type, payload)
          next
        end
        case type
        when Protocol::MSG_ERROR
          handle_query_error(payload)
        when Protocol::MSG_ROW_DESCRIPTION
          columns = Protocol.parse_row_description(payload)
        when Protocol::MSG_DATA_ROW
          values = Protocol.parse_data_row(payload)
          rows << decode_row(columns, values)
        when Protocol::MSG_COMMAND_COMPLETE
          _command_type, rows_count, parsed_last_id, tag = Protocol.parse_command_complete(payload)
          command_tag = tag
          rowcount = rows_count
          last_insert_id = parsed_last_id.to_i
        when Protocol::MSG_PORTAL_SUSPENDED
          allow_portal_resume!
          resume_portal if @last_max_rows.to_i > 0
        when Protocol::MSG_READY
          status, txn_id = Protocol.parse_ready(payload)
          apply_runtime_ready_state(status, txn_id)
          break
        else
          next
        end
      end

      rowcount = rows.length if rowcount < 0
      Result.new(columns, rows, rowcount, command_tag, last_insert_id)
    end

    def execute_query_multi_loop
      results = []
      columns = []
      rows = []
      rowcount = -1
      command_tag = ""
      last_insert_id = 0
      result_open = false

      loop do
        type, _flags, payload, _sequence, _attachment_id, _txn_id = recv_message
        if handle_async_message(type, payload)
          next
        end
        case type
        when Protocol::MSG_ERROR
          handle_query_error(payload)
        when Protocol::MSG_ROW_DESCRIPTION
          if result_open && (!columns.empty? || !rows.empty? || rowcount >= 0 || !command_tag.empty?)
            results << Result.new(columns, rows, rowcount >= 0 ? rowcount : rows.length, command_tag, last_insert_id)
            rows = []
            rowcount = -1
            command_tag = ""
            last_insert_id = 0
          end
          columns = Protocol.parse_row_description(payload)
          result_open = true
        when Protocol::MSG_DATA_ROW
          values = Protocol.parse_data_row(payload)
          rows << decode_row(columns, values)
          result_open = true
        when Protocol::MSG_COMMAND_COMPLETE
          _command_type, rows_count, parsed_last_id, tag = Protocol.parse_command_complete(payload)
          command_tag = tag
          rowcount = rows_count
          last_insert_id = parsed_last_id.to_i
          results << Result.new(columns, rows, rowcount >= 0 ? rowcount : rows.length, command_tag, last_insert_id)
          columns = []
          rows = []
          rowcount = -1
          command_tag = ""
          last_insert_id = 0
          result_open = false
        when Protocol::MSG_PORTAL_SUSPENDED
          allow_portal_resume!
          resume_portal if @last_max_rows.to_i > 0
        when Protocol::MSG_READY
          status, txn_id = Protocol.parse_ready(payload)
          apply_runtime_ready_state(status, txn_id)
          if result_open && (!columns.empty? || !rows.empty? || rowcount >= 0 || !command_tag.empty?)
            results << Result.new(columns, rows, rowcount >= 0 ? rowcount : rows.length, command_tag, last_insert_id)
          end
          break
        end
      end

      results = [Result.new([], [], 0, "", 0)] if results.empty?
      results
    end

    def send_simple_query(sql, options)
      flags = @config.binary_transfer ? QUERY_FLAG_BINARY_RESULT : 0
      if options
        flags |= Protocol::QUERY_FLAG_INCLUDE_PLAN if options[:include_plan]
        flags |= Protocol::QUERY_FLAG_RETURN_SBLR if options[:return_sblr]
        flags |= Protocol::QUERY_FLAG_DESCRIBE_ONLY if options[:describe_only]
        flags |= Protocol::QUERY_FLAG_NO_CACHE if options[:no_cache]
      end
      max_rows = options && options[:max_rows] ? options[:max_rows].to_i : 0
      @last_max_rows = max_rows
      timeout_ms = options && options[:timeout_ms] ? options[:timeout_ms].to_i : 0
      payload = Protocol.build_query_payload(sql, flags, max_rows, timeout_ms)
      @last_query_sequence = send_message(Protocol::MSG_QUERY, payload, 0, false)
    end

    def send_extended_query(sql, params, options)
      param_values = []
      param_types = []
      params.each do |param|
        encoded = Types.encode_param(param)
        param_values << encoded[:param]
        param_types << encoded[:oid]
      end
      parse_payload = Protocol.build_parse_payload("", sql, param_types)
      send_message(Protocol::MSG_PARSE, parse_payload, 0, false)
      param_count = describe_statement("")
      if param_count.to_i >= 0 && param_count.to_i != params.length
        raise Error.new("parameter count mismatch", "07001")
      end

      result_formats = @config.binary_transfer ? [Types::FORMAT_BINARY] : []
      bind_payload = Protocol.build_bind_payload("", "", param_values, result_formats)
      send_message(Protocol::MSG_BIND, bind_payload, 0, false)

      max_rows = options && options[:max_rows] ? options[:max_rows].to_i : 0
      @last_max_rows = max_rows
      exec_payload = Protocol.build_execute_payload("", max_rows)
      @last_query_sequence = send_message(Protocol::MSG_EXECUTE, exec_payload, 0, false)
      send_message(Protocol::MSG_SYNC, +"", 0, false) if max_rows == 0
    end

    def send_bind_execute(statement_name, params, options)
      param_values = []
      params.each do |param|
        encoded = Types.encode_param(param)
        param_values << encoded[:param]
      end
      result_formats = @config.binary_transfer ? [Types::FORMAT_BINARY] : []
      bind_payload = Protocol.build_bind_payload("", statement_name, param_values, result_formats)
      send_message(Protocol::MSG_BIND, bind_payload, 0, false)

      max_rows = options && options[:max_rows] ? options[:max_rows].to_i : 0
      @last_max_rows = max_rows
      exec_payload = Protocol.build_execute_payload("", max_rows)
      @last_query_sequence = send_message(Protocol::MSG_EXECUTE, exec_payload, 0, false)
      send_message(Protocol::MSG_SYNC, +"", 0, false) if max_rows == 0
    end

    def resume_portal
      raise Error.new("portal resume requires explicit suspended state", "55000") unless @portal_resume_pending

      @portal_resume_pending = false
      exec_payload = Protocol.build_execute_payload("", @last_max_rows.to_i)
      send_message(Protocol::MSG_EXECUTE, exec_payload, 0, false)
    end

    def describe_statement(statement_name)
      payload = Protocol.build_describe_payload("S".ord, statement_name)
      send_message(Protocol::MSG_DESCRIBE, payload, 0, false)
      send_message(Protocol::MSG_SYNC, +"", 0, false)
      param_count = -1
      loop do
        type, _flags, payload, _sequence, _attachment_id, txn_id = recv_message
        if handle_async_message(type, payload)
          next
        end
        case type
        when Protocol::MSG_PARAMETER_DESCRIPTION
          param_count = Protocol.parse_parameter_description(payload).length
        when Protocol::MSG_ERROR
          handle_query_error(payload)
        when Protocol::MSG_READY
          status, txn = Protocol.parse_ready(payload)
          apply_runtime_ready_state(status, txn)
          break
        else
          next
        end
      end
      param_count
    end

    def execute_simple(sql)
      send_simple_query(sql, nil)
      drain_until_ready
      true
    rescue StandardError
      false
    end

    def build_schema_statement(schema)
      trimmed = schema.strip
      return "" if trimmed.empty?
      if trimmed.include?(",")
        parts = trimmed.split(",").map(&:strip).reject(&:empty?).map { |part| quote_identifier(part) }
        return "" if parts.empty?
        return "SET SEARCH_PATH TO #{parts.join(", ")}"
      end
      "SET SCHEMA #{quote_identifier(trimmed)}"
    end

    def quote_identifier(name)
      "\"#{name.to_s.gsub('"', '""')}\""
    end

    def summarize_result(result)
      fields = Array(result.columns).map do |col|
        FieldSummary.new(
          name: col.respond_to?(:name) ? col.name : col[:name],
          type_oid: col.respond_to?(:type_oid) ? col.type_oid : col[:type_oid],
          format: col.respond_to?(:format) ? col.format : col[:format],
          nullable: col.respond_to?(:nullable) ? col.nullable : col[:nullable]
        )
      end
      ResultSetSummary.new(
        rows: Array(result.rows),
        rowcount: result.rowcount.to_i,
        fields: fields,
        command: result.command_tag.to_s,
        last_insert_id: result.respond_to?(:last_insert_id) ? result.last_insert_id.to_i : 0
      )
    end

    SET_TERM_RE = /\Aset\s+term\s+(\S.*?)\s*\z/i.freeze

    # Detect a `SET TERM <terminator>` client directive in a cut chunk.
    #
    # Leading full-line `--` comments and blank lines are ignored when matching,
    # so a directive may be preceded by comment lines in the same chunk. Returns
    # the new terminator string, or nil when the chunk is not a SET TERM directive.
    def chunk_set_term(chunk)
      meaningful = []
      chunk.each_line do |line|
        stripped = line.strip
        next if stripped.empty? || stripped.start_with?("--")
        meaningful << stripped
      end
      return nil if meaningful.empty?
      match = SET_TERM_RE.match(meaningful.join(" "))
      match ? match[1].strip : nil
    end

    # Split SQL into top-level statements on the active terminator.
    #
    # Quote-aware (single/double quotes, including SQL-style doubled-quote
    # escapes) and `--` line-comment aware. Honors the `SET TERM <terminator>`
    # client directive: the directive changes the
    # active terminator and is consumed -- it is not emitted as a statement and
    # is not counted. With no SET TERM directive present the behavior is identical
    # to a plain quote-aware top-level `;` split, so existing scripts are unchanged.
    def split_sql_statements(sql)
      return [] if sql.to_s.strip.empty?

      statements = []
      term = ";"
      buffer = +""
      in_single = false
      in_double = false
      i = 0

      flush = lambda do
        chunk = buffer.strip
        next if chunk.empty?
        new_term = chunk_set_term(chunk)
        if new_term
          term = new_term
          next
        end
        statements << chunk
      end

      while i < sql.length
        ch = sql[i]
        if !in_single && !in_double && ch == "-" && i + 1 < sql.length && sql[i + 1] == "-"
          # `--` line comment: copy verbatim to end of line without scanning for
          # the terminator or quotes inside it.
          eol = sql.index("\n", i)
          eol = sql.length if eol.nil?
          buffer << sql[i...eol]
          i = eol
          next
        end
        if ch == "'" && !in_double
          buffer << ch
          if in_single && i + 1 < sql.length && sql[i + 1] == "'"
            buffer << "'"
            i += 2
            next
          end
          in_single = !in_single
          i += 1
          next
        end
        if ch == '"' && !in_single
          buffer << ch
          if in_double && i + 1 < sql.length && sql[i + 1] == '"'
            buffer << '"'
            i += 2
            next
          end
          in_double = !in_double
          i += 1
          next
        end
        if !in_single && !in_double && !term.empty? && sql[i, term.length] == term
          matched_len = term.length # capture before flush, which may change term
          flush.call
          buffer.clear
          i += matched_len
          next
        end
        buffer << ch
        i += 1
      end

      flush.call
      statements
    end

    def clear_cancel_request
      @cancel_requested = false
    end

    def apply_runtime_txn_id(txn_id)
      txn = txn_id.to_i
      if txn.positive?
        @txn_id = txn
        @transaction_active = true
      else
        @txn_id = 0
      end
    end

    def apply_runtime_ready_state(status, txn_id)
      txn = txn_id.to_i
      if status.to_i != 0
        # READY status is authoritative for transaction activity. Native
        # engine-endpoint sessions can expose an active fresh transaction with
        # txn_id == 0 across the public wire contract.
        @txn_id = txn
        @transaction_active = true
      else
        @txn_id = 0
        @transaction_active = false
      end
    end

    def adopt_transaction_after_begin
      @transaction_active = true
      return if @txn_id.to_i.positive?

      @synthetic_txn_id += 1
      @txn_id = @synthetic_txn_id
    end

    def clear_transaction_state
      @transaction_active = false
      @txn_id = 0
    end

    def clear_abandoned_session_state
      # MGA reconnect creates a fresh attachment/transaction boundary. Prepared
      # handles, attachment parameters, and cached plan/SBLR frames from the
      # abandoned session must be dropped instead of being treated as resumable
      # local state on the replacement handshake.
      @attachment_id = "\0" * 16
      @txn_id = 0
      @sequence = 0
      @last_query_sequence = 0
      @parameters = {}
      @prepared = {}
      @last_plan = nil
      @last_sblr = nil
      @cancel_requested = false
      @active_thread = nil
      @transaction_active = false
      @synthetic_txn_id = 0
      @portal_resume_pending = false
    end

    def build_prepared_transaction_sql(verb, gid)
      normalized = gid.to_s.strip
      raise SyntaxError.new("global transaction id is required", "42601") if normalized.empty?

      "#{verb} '#{normalized.gsub("'", "''")}'"
    end
  end

  class ResultStream
    attr_reader :columns, :rowcount, :command_tag, :last_insert_id

    def initialize(client)
      @client = client
      @columns = []
      @rowcount = -1
      @seen_rows = 0
      @command_tag = ""
      @last_insert_id = 0
      @consumed = false
    end

    def each
      return enum_for(:each) unless block_given?
      raise Error, "stream already consumed" if @consumed
      @consumed = true

      loop do
        type, _flags, payload, _sequence, _attachment_id, _txn_id = @client.recv_message
        next if @client.send(:handle_async_message, type, payload)
        case type
        when Protocol::MSG_ERROR
          @client.handle_query_error(payload)
        when Protocol::MSG_ROW_DESCRIPTION
          @columns = Protocol.parse_row_description(payload)
        when Protocol::MSG_DATA_ROW
          values = Protocol.parse_data_row(payload)
          yield @client.decode_row(@columns, values)
          @seen_rows += 1
        when Protocol::MSG_COMMAND_COMPLETE
          _command_type, rows_count, parsed_last_id, tag = Protocol.parse_command_complete(payload)
          @command_tag = tag
          @rowcount = rows_count
          @last_insert_id = parsed_last_id.to_i
        when Protocol::MSG_PORTAL_SUSPENDED
          @client.allow_portal_resume! if @client.respond_to?(:allow_portal_resume!, true)
          @client.resume_portal if @client.instance_variable_get(:@last_max_rows).to_i > 0
        when Protocol::MSG_READY
          status, txn_id = Protocol.parse_ready(payload)
          @client.update_ready_state(status, txn_id)
          break
        else
          next
        end
      end

      @rowcount = @seen_rows if @rowcount < 0
    end

    def each_hash
      return enum_for(:each_hash) unless block_given?
      each do |row|
        yield to_hash(row)
      end
    end

    def to_a
      rows = []
      each { |row| rows << row }
      rows
    end

    private

    def to_hash(row)
      data = {}
      @columns.each_with_index do |col, idx|
        key = col[:name] || idx
        data[key] = row[idx]
      end
      data
    end
  end
end
