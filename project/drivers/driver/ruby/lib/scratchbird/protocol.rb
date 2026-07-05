# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

module Scratchbird
  module Protocol
    MAGIC_BYTES = "SBWP".b
    VERSION_MAJOR = 1
    VERSION_MINOR = 1
    PROTOCOL_VERSION = (VERSION_MAJOR << 8) | VERSION_MINOR
    HEADER_SIZE = 40
    MAX_MESSAGE_SIZE = 1024 * 1024 * 1024
    CONNECT_VALUE_TEXT = 1
    P1_ROW_DESCRIPTION_HEADER_BYTES = 72
    P1_CANONICAL_TYPE_REF_BYTES = 144

    MSG_STARTUP = 0x01
    MSG_AUTH_RESPONSE = 0x02
    MSG_QUERY = 0x03
    MSG_PARSE = 0x04
    MSG_BIND = 0x05
    MSG_DESCRIBE = 0x06
    MSG_EXECUTE = 0x07
    MSG_CLOSE = 0x08
    MSG_SYNC = 0x09
    MSG_FLUSH = 0x0A
    MSG_CANCEL = 0x0B
    MSG_TERMINATE = 0x0C
    MSG_COPY_DATA = 0x0D
    MSG_COPY_DONE = 0x0E
    MSG_COPY_FAIL = 0x0F
    MSG_SBLR_EXECUTE = 0x10
    MSG_SUBSCRIBE = 0x11
    MSG_UNSUBSCRIBE = 0x12
    MSG_FEDERATED_QUERY = 0x13
    MSG_STREAM_CONTROL = 0x14
    MSG_TXN_BEGIN = 0x15
    MSG_TXN_COMMIT = 0x16
    MSG_TXN_ROLLBACK = 0x17
    MSG_TXN_SAVEPOINT = 0x18
    MSG_TXN_RELEASE = 0x19
    MSG_TXN_ROLLBACK_TO = 0x1A
    MSG_PING = 0x1B
    MSG_SET_OPTION = 0x1C
    MSG_CLUSTER_AUTH = 0x1D
    MSG_ATTACH_CREATE = 0x1E
    MSG_ATTACH_DETACH = 0x1F
    MSG_ATTACH_LIST = 0x20

    MSG_AUTH_REQUEST = 0x40
    MSG_AUTH_OK = 0x41
    MSG_AUTH_CONTINUE = 0x42
    MSG_READY = 0x43
    MSG_ROW_DESCRIPTION = 0x44
    MSG_DATA_ROW = 0x45
    MSG_COMMAND_COMPLETE = 0x46
    MSG_EMPTY_QUERY = 0x47
    MSG_ERROR = 0x48
    MSG_NOTICE = 0x49
    MSG_PARSE_COMPLETE = 0x4A
    MSG_BIND_COMPLETE = 0x4B
    MSG_CLOSE_COMPLETE = 0x4C
    MSG_PORTAL_SUSPENDED = 0x4D
    MSG_NO_DATA = 0x4E
    MSG_PARAMETER_STATUS = 0x4F
    MSG_PARAMETER_DESCRIPTION = 0x50
    MSG_COPY_IN_RESPONSE = 0x51
    MSG_COPY_OUT_RESPONSE = 0x52
    MSG_COPY_BOTH_RESPONSE = 0x53
    MSG_NOTIFICATION = 0x54
    MSG_FUNCTION_RESULT = 0x55
    MSG_NEGOTIATE_VERSION = 0x56
    MSG_SBLR_COMPILED = 0x57
    MSG_QUERY_PLAN = 0x58
    MSG_STREAM_READY = 0x59
    MSG_STREAM_DATA = 0x5A
    MSG_STREAM_END = 0x5B
    MSG_TXN_STATUS = 0x5C
    MSG_PONG = 0x5D
    MSG_CLUSTER_AUTH_OK = 0x5E
    MSG_FEDERATED_RESULT = 0x5F
    MSG_HEARTBEAT = 0x80
    MSG_EXTENSION = 0x81

    AUTH_OK = 0
    AUTH_PASSWORD = 1
    AUTH_MD5 = 2
    AUTH_SCRAM_SHA256 = 3
    AUTH_SCRAM_SHA512 = 4
    AUTH_TOKEN = 5
    AUTH_PEER = 6
    AUTH_REATTACH = 7
    AUTH_CERTIFICATE = 8
    AUTH_GSSAPI = 9
    AUTH_SSPI = 10
    AUTH_LDAP = 11
    AUTH_SAML = 12
    AUTH_OIDC = 13
    AUTH_MFA_TOTP = 14
    AUTH_CLUSTER_PKI = 15

    MSG_FLAG_COMPRESSED = 0x01
    MSG_FLAG_CONTINUED = 0x02
    MSG_FLAG_FINAL = 0x04
    MSG_FLAG_URGENT = 0x08
    MSG_FLAG_ENCRYPTED = 0x10
    MSG_FLAG_CHECKSUM = 0x20

    FEATURE_COMPRESSION = 1
    FEATURE_STREAMING = 2
    FEATURE_SBLR = 4
    FEATURE_FEDERATION = 8
    FEATURE_NOTIFICATIONS = 16
    FEATURE_QUERY_PLAN = 32
    FEATURE_BATCH = 64
    FEATURE_PIPELINE = 128
    FEATURE_BINARY_COPY = 256
    FEATURE_SAVEPOINTS = 512
    FEATURE_2PC = 1024
    FEATURE_CHECKSUMS = 2048

    QUERY_FLAG_DESCRIBE_ONLY = 0x01
    QUERY_FLAG_NO_PORTAL = 0x02
    QUERY_FLAG_BINARY_RESULT = 0x04
    QUERY_FLAG_INCLUDE_PLAN = 0x08
    QUERY_FLAG_RETURN_SBLR = 0x10
    QUERY_FLAG_NO_CACHE = 0x20

    DESCRIBE_STATEMENT = "S".ord
    DESCRIBE_PORTAL = "P".ord

    ISOLATION_READ_UNCOMMITTED = 0
    ISOLATION_READ_COMMITTED = 1
    ISOLATION_REPEATABLE_READ = 2
    ISOLATION_SERIALIZABLE = 3

    READ_COMMITTED_MODE_DEFAULT = 0
    READ_COMMITTED_MODE_READ_CONSISTENCY = 1
    READ_COMMITTED_MODE_RECORD_VERSION = 2
    READ_COMMITTED_MODE_NO_RECORD_VERSION = 3

    TXN_FLAG_HAS_ISOLATION = 0x0001
    TXN_FLAG_HAS_ACCESS = 0x0002
    TXN_FLAG_HAS_DEFERRABLE = 0x0004
    TXN_FLAG_HAS_WAIT = 0x0008
    TXN_FLAG_HAS_TIMEOUT = 0x0010
    TXN_FLAG_HAS_AUTOCOMMIT = 0x0020
    TXN_FLAG_HAS_READ_COMMITTED_MODE = 0x0100

    STREAM_START = 0
    STREAM_PAUSE = 1
    STREAM_RESUME = 2
    STREAM_CANCEL = 3
    STREAM_ACK = 4

    SUB_TYPE_CHANNEL = 0
    SUB_TYPE_TABLE = 1
    SUB_TYPE_QUERY = 2
    SUB_TYPE_EVENT = 3

    def self.encode_message(type, payload, flags, sequence, attachment_id, txn_id)
      header = +"".b
      header << MAGIC_BYTES
      header << [VERSION_MAJOR, VERSION_MINOR, type, flags].pack("C4")
      header << [payload.bytesize, sequence].pack("V2")
      header << pad_bytes(attachment_id, 16)
      header << [txn_id].pack("Q<")
      header + payload
    end

    def self.decode_header(data)
      raise "Invalid header length" unless data.bytesize == HEADER_SIZE
      magic = data.byteslice(0, 4)
      raise "Invalid protocol magic" unless magic == MAGIC_BYTES
      major, minor, type, flags, length, sequence = data.byteslice(4, 12).unpack("CCCCVV")
      raise "Unsupported protocol version" unless major == VERSION_MAJOR && minor == VERSION_MINOR
      raise "Payload too large" if length > MAX_MESSAGE_SIZE
      attachment_id = data.byteslice(16, 16)
      txn_id = data.byteslice(32, 8).unpack1("Q<")
      [type, flags, length, sequence, attachment_id, txn_id]
    end

    def self.build_startup_payload(features, params)
      payload = +"".b
      payload << [PROTOCOL_VERSION, PROTOCOL_VERSION, 0].pack("vvV")
      payload << [features.to_i, 0, 0].pack("Q<Q<Q<")
      payload << ("\0" * 16)
      payload << ("\0" * 16)
      payload << ("\0" * 16)
      keys = params.keys.map(&:to_s).sort
      payload << [keys.length].pack("V")
      keys.each do |key|
        value = params[key] || params[key.to_sym] || ""
        append_length_prefixed_string(payload, key)
        payload << [CONNECT_VALUE_TEXT].pack("v")
        append_length_prefixed_string(payload, value.to_s)
      end
      payload << [0].pack("V")
      payload
    end

    def self.append_length_prefixed_string(payload, value)
      encoded = value.to_s.b
      payload << [encoded.bytesize].pack("V")
      payload << encoded
    end

    def self.build_param_list(params)
      buf = +"".b
      params.each do |key, value|
        buf << key.to_s << "\0" << value.to_s << "\0"
      end
      buf << "\0"
      buf
    end

    def self.parse_auth_request(payload)
      raise "Auth request truncated" if payload.bytesize < 4
      method = payload.getbyte(0)
      data = payload.byteslice(4, payload.bytesize - 4) || ""
      [method, data]
    end

    def self.parse_auth_continue(payload)
      raise "Auth continue truncated" if payload.bytesize < 8
      method = payload.getbyte(0)
      stage = payload.getbyte(1)
      data_len = payload.byteslice(4, 4).unpack1("V")
      raise "Auth continue truncated" if 8 + data_len > payload.bytesize
      data = payload.byteslice(8, data_len) || ""
      [method, stage, data]
    end

    def self.parse_auth_ok(payload)
      raise "Auth ok truncated" if payload.bytesize < 20
      session_id = payload.byteslice(0, 16)
      info_len = payload.byteslice(16, 4).unpack1("V")
      raise "Auth ok truncated" if 20 + info_len > payload.bytesize
      server_info = payload.byteslice(20, info_len) || ""
      [session_id, server_info]
    end

    def self.build_query_payload(sql, flags, max_rows, timeout_ms)
      payload = [flags, max_rows, timeout_ms].pack("VVV")
      payload << sql.to_s.b
      payload << "\0".b
      payload
    end

    def self.build_parse_payload(statement_name, sql, param_types)
      name_bytes = statement_name.to_s.b
      sql_bytes = sql.to_s.b
      payload = [name_bytes.bytesize].pack("V") + name_bytes
      payload << [sql_bytes.bytesize].pack("V") + sql_bytes
      payload << [param_types.length].pack("v")
      payload << [0].pack("v")
      param_types.each { |oid| payload << [oid].pack("V") }
      payload
    end

    def self.build_bind_payload(portal_name, statement_name, params, result_formats)
      portal_bytes = portal_name.to_s.b
      stmt_bytes = statement_name.to_s.b
      payload = [portal_bytes.bytesize].pack("V") + portal_bytes
      payload << [stmt_bytes.bytesize].pack("V") + stmt_bytes
      payload << [params.length].pack("v")
      params.each { |param| payload << [param[:format]].pack("v") }
      payload << [params.length].pack("v")
      payload << [0].pack("v")
      params.each do |param|
        if param[:is_null]
          payload << [-1].pack("l<")
        else
          data = (param[:data] || "").b
          payload << [data.bytesize].pack("l<")
          payload << data
        end
      end
      payload << [result_formats.length].pack("v")
      result_formats.each { |fmt| payload << [fmt].pack("v") }
      payload
    end

    def self.build_execute_payload(portal_name, max_rows)
      portal_bytes = portal_name.to_s.b
      [portal_bytes.bytesize].pack("V") + portal_bytes + [max_rows].pack("V")
    end

    def self.build_close_payload(close_type, name)
      name_bytes = name.to_s.b
      payload = [close_type].pack("C")
      payload << "\0\0\0"
      payload << [name_bytes.bytesize].pack("V")
      payload << name_bytes
      payload
    end

    def self.build_describe_payload(describe_type, name)
      name_bytes = name.to_s.b
      payload = [describe_type].pack("C")
      payload << "\0\0\0"
      payload << [name_bytes.bytesize].pack("V") + name_bytes
      payload
    end

    def self.build_cancel_payload(cancel_type, target_sequence)
      [cancel_type, target_sequence].pack("VV")
    end

    def self.build_sblr_execute_payload(sblr_hash, sblr_bytecode, params)
      bytecode = (sblr_bytecode || +"").b
      payload = [sblr_hash].pack("Q<")
      payload << [bytecode.bytesize].pack("V")
      payload << [params.length].pack("v")
      payload << [0].pack("v")
      payload << bytecode
      params.each do |param|
        if param[:is_null]
          payload << [-1].pack("l<")
        else
          data = (param[:data] || "").b
          payload << [data.bytesize].pack("l<")
          payload << data
        end
      end
      payload
    end

    def self.build_copy_data_payload(data)
      data.to_s.b
    end

    def self.build_copy_done_payload
      +"".b
    end

    def self.build_copy_fail_payload(error_message)
      message = error_message.to_s.b
      [message.bytesize].pack("V") + message
    end

    def self.build_subscribe_payload(subscribe_type, channel, filter_expr = "")
      channel_bytes = channel.to_s.b
      filter_bytes = filter_expr.to_s.b
      payload = [subscribe_type].pack("C")
      payload << "\0\0\0"
      payload << [channel_bytes.bytesize].pack("V") + channel_bytes
      payload << [filter_bytes.bytesize].pack("V") + filter_bytes
      payload
    end

    def self.build_unsubscribe_payload(channel)
      channel_bytes = channel.to_s.b
      [channel_bytes.bytesize].pack("V") + channel_bytes
    end

    def self.build_txn_begin_payload(flags, conflict_action, autocommit_mode, isolation_level, access_mode, deferrable, wait_mode, timeout_ms, read_committed_mode = READ_COMMITTED_MODE_DEFAULT)
      payload = [flags, conflict_action, autocommit_mode, isolation_level, access_mode, deferrable, wait_mode, timeout_ms].pack("vCCCCCCV")
      if (flags & TXN_FLAG_HAS_READ_COMMITTED_MODE) != 0
        payload << [read_committed_mode, 0, 0, 0].pack("CCCC")
      end
      payload
    end

    def self.canonical_read_committed_mode_label(mode)
      case mode
      when READ_COMMITTED_MODE_DEFAULT
        "READ COMMITTED"
      when READ_COMMITTED_MODE_READ_CONSISTENCY
        "READ COMMITTED READ CONSISTENCY"
      when READ_COMMITTED_MODE_RECORD_VERSION
        "READ COMMITTED RECORD VERSION"
      when READ_COMMITTED_MODE_NO_RECORD_VERSION
        "READ COMMITTED NO RECORD VERSION"
      else
        "UNKNOWN(#{mode})"
      end
    end

    def self.build_txn_commit_payload(flags)
      [flags].pack("C") + "\0\0\0"
    end

    def self.build_txn_rollback_payload(flags)
      [flags].pack("C") + "\0\0\0"
    end

    def self.build_txn_savepoint_payload(name)
      name_bytes = name.to_s.b
      [name_bytes.bytesize].pack("V") + name_bytes
    end

    def self.build_txn_release_payload(name)
      build_txn_savepoint_payload(name)
    end

    def self.build_txn_rollback_to_payload(name)
      build_txn_savepoint_payload(name)
    end

    def self.build_set_option_payload(name, value)
      name_bytes = name.to_s.b
      value_bytes = value.to_s.b
      payload = [name_bytes.bytesize].pack("V") + name_bytes
      payload << [value_bytes.bytesize].pack("V") + value_bytes
      payload
    end

    def self.build_stream_control_payload(control_type, window_size, timeout_ms)
      [control_type, window_size, timeout_ms].pack("C3xVV")
    end

    def self.build_attach_create_payload(emulation_mode, db_name)
      mode_bytes = emulation_mode.to_s.b
      db_bytes = db_name.to_s.b
      payload = [mode_bytes.bytesize].pack("V") + mode_bytes
      payload << [db_bytes.bytesize].pack("V") + db_bytes
      payload
    end

    def self.parse_ready(payload)
      if payload.bytesize >= 76 && [0x49, 0x54, 0x45, 0x52, 0x41].include?(payload.getbyte(56))
        txn_id = payload.byteslice(48, 8).unpack1("Q<")
        status = [0x54, 0x45].include?(payload.getbyte(56)) ? 1 : 0
        return [status, txn_id, txn_id]
      end
      raise "Ready truncated" if payload.bytesize < 20
      status = payload.getbyte(0)
      txn_id = payload.byteslice(4, 8).unpack1("Q<")
      visibility = payload.byteslice(12, 8).unpack1("Q<")
      [status, txn_id, visibility]
    end

    def self.parse_parameter_statuses(payload)
      raise "Parameter status truncated" if payload.bytesize < 8
      count = payload.byteslice(0, 4).unpack1("V")
      if count.positive? && count <= 256
        offset = 4
        values = []
        begin
          count.times do
            raise "Parameter status truncated" if offset + 4 > payload.bytesize
            name_len = payload.byteslice(offset, 4).unpack1("V")
            offset += 4
            raise "Parameter status truncated" if offset + name_len + 7 > payload.bytesize
            name = payload.byteslice(offset, name_len).to_s
            offset += name_len
            offset += 3
            value_len = payload.byteslice(offset, 4).unpack1("V")
            offset += 4
            raise "Parameter status truncated" if offset + value_len > payload.bytesize
            value = payload.byteslice(offset, value_len).to_s
            offset += value_len
            values << [name, value]
          end
          return values if offset == payload.bytesize
        rescue StandardError
          # Fall through to the legacy single name/value layout.
        end
      end

      offset = 0
      name_len = payload.byteslice(offset, 4).unpack1("V")
      offset += 4
      name = payload.byteslice(offset, name_len).to_s
      offset += name_len
      value_len = payload.byteslice(offset, 4).unpack1("V")
      offset += 4
      value = payload.byteslice(offset, value_len).to_s
      [[name, value]]
    end

    def self.parse_parameter_status(payload)
      statuses = parse_parameter_statuses(payload)
      raise "Parameter status truncated" if statuses.empty?
      statuses.first
    end

    def self.parse_parameter_description(payload)
      if p1_row_description?(payload)
        count = payload.byteslice(68, 4).unpack1("V")
        offset = P1_ROW_DESCRIPTION_HEADER_BYTES
        types = []
        count.times do
          raise "P1 parameter description truncated" if offset + 4 + 4 + 8 + 8 + P1_CANONICAL_TYPE_REF_BYTES + 4 + 5 > payload.bytesize
          type_offset = offset + 4 + 4 + 8 + 8
          types << oid_from_canonical_type_ref(payload, type_offset)
          offset = type_offset + P1_CANONICAL_TYPE_REF_BYTES + 4
          _text, offset = read_nullable_text(payload, offset)
        end
        return types
      end
      raise "Parameter description truncated" if payload.bytesize < 4
      count = payload.byteslice(0, 2).unpack1("v")
      offset = 4
      types = []
      count.times do
        raise "Parameter description truncated" if offset + 4 > payload.bytesize
        types << payload.byteslice(offset, 4).unpack1("V")
        offset += 4
      end
      types
    end

    def self.parse_row_description(payload)
      return parse_p1_row_description(payload) if p1_row_description?(payload)

      raise "Row description truncated" if payload.bytesize < 4
      offset = 0
      count = payload.byteslice(offset, 2).unpack1("v")
      offset += 4
      columns = []
      count.times do
        name_len = payload.byteslice(offset, 4).unpack1("V")
        offset += 4
        name = payload.byteslice(offset, name_len).to_s
        offset += name_len
        table_oid = payload.byteslice(offset, 4).unpack1("V")
        offset += 4
        column_index = payload.byteslice(offset, 2).unpack1("v")
        offset += 2
        type_oid = payload.byteslice(offset, 4).unpack1("V")
        offset += 4
        type_size = payload.byteslice(offset, 2).unpack1("s<")
        offset += 2
        type_modifier = payload.byteslice(offset, 4).unpack1("l<")
        offset += 4
        format = payload.getbyte(offset)
        offset += 1
        nullable = payload.getbyte(offset) == 1
        offset += 1
        offset += 2
        columns << { name: name, table_oid: table_oid, column_index: column_index, type_oid: type_oid,
                    type_size: type_size, type_modifier: type_modifier, format: format, nullable: nullable }
      end
      columns
    end

    def self.p1_row_description?(payload)
      payload.bytesize >= P1_ROW_DESCRIPTION_HEADER_BYTES &&
        payload.byteslice(0, 2).unpack1("v") == 1 &&
        payload.getbyte(3) == 1
    end

    def self.parse_p1_row_description(payload)
      count = payload.byteslice(4, 4).unpack1("l<")
      raise "P1 row description column count invalid" if count.negative?

      offset = P1_ROW_DESCRIPTION_HEADER_BYTES
      columns = []
      count.times do |idx|
        fixed_column_bytes = 4 + 4 + 8 + P1_CANONICAL_TYPE_REF_BYTES + 56
        raise "P1 row description truncated" if offset + fixed_column_bytes > payload.bytesize
        ordinal = payload.byteslice(offset, 4).unpack1("l<")
        offset += 4
        offset += 1
        format = payload.getbyte(offset) == 1 ? 0 : 1
        offset += 1
        nullable = payload.getbyte(offset) == 1
        offset += 1
        offset += 1
        offset += 8
        type_oid = oid_from_canonical_type_ref(payload, offset)
        offset += P1_CANONICAL_TYPE_REF_BYTES
        offset += 16 * 3
        offset += 4
        offset += 2
        offset += 2
        name, offset = read_nullable_text(payload, offset)
        columns << {
          name: name.empty? ? "column#{idx + 1}" : name,
          table_oid: 0,
          column_index: ordinal.zero? ? idx : ordinal - 1,
          type_oid: type_oid,
          type_size: type_size_for_oid(type_oid),
          type_modifier: -1,
          format: format,
          nullable: nullable
        }
      end
      columns
    end

    def self.oid_from_canonical_type_ref(payload, offset)
      return Types::OID_TEXT if offset + 4 > payload.bytesize
      family = payload.byteslice(offset, 2).unpack1("v")
      code = payload.byteslice(offset + 2, 2).unpack1("v")
      return Types::OID_BOOL if family == 1 && code == 1
      if family == 2
        return Types::OID_INT2 if code <= 2
        return Types::OID_INT4 if code == 3
        return Types::OID_INT8 if code == 4
        return Types::OID_NUMERIC
      end
      if family == 3
        return Types::OID_INT4 if code <= 2
        return Types::OID_INT8 if code <= 4
        return Types::OID_NUMERIC
      end
      return Types::OID_NUMERIC if family == 4 || family == 5
      return Types::OID_MONEY if family == 7
      return Types::OID_FLOAT4 if family == 6 && code <= 2
      return Types::OID_FLOAT8 if family == 6
      return Types::OID_TEXT if family == 8
      return Types::OID_BYTEA if family == 9 || family == 14 || family == 25 || family == 27
      return Types::OID_TEXT if family == 10
      if family == 11
        return Types::OID_DATE if code == 1
        return Types::OID_TIME if code == 2
        return Types::OID_TIMESTAMP
      end
      return Types::OID_INTERVAL if family == 12
      return Types::OID_UUID if family == 13
      return Types::WIRE_ARRAY if family == 15
      return Types::OID_RECORD if family == 16
      return Types::OID_TEXT if family == 17 || family == 18
      if family == 19
        return Types::OID_MACADDR if code == 3
        return Types::OID_INET
      end
      if family == 20
        return Types::OID_XML if code == 5
        return Types::OID_JSONB if code == 3
        return Types::OID_JSON
      end
      return Types::OID_POINT if family == 21
      return Types::OID_SB_VECTOR if family == 22
      return Types::OID_TEXT if family >= 23 && family <= 30
      Types::OID_TEXT
    end

    def self.type_size_for_oid(type_oid)
      return 1 if type_oid == Types::OID_BOOL
      return 4 if type_oid == Types::OID_INT4
      return 8 if [Types::OID_INT8, Types::OID_FLOAT8].include?(type_oid)
      return 16 if type_oid == Types::OID_UUID
      -1
    end

    def self.read_nullable_text(payload, offset)
      raise "nullable text truncated" if offset + 5 > payload.bytesize
      tag = payload.getbyte(offset)
      offset += 1
      length = payload.byteslice(offset, 4).unpack1("l<")
      offset += 4
      raise "nullable text length invalid" if length.negative?
      return ["", offset] if tag.zero?
      raise "nullable text truncated" if offset + length > payload.bytesize
      [payload.byteslice(offset, length).to_s, offset + length]
    end

    def self.parse_data_row(payload)
      raise "Row data truncated" if payload.bytesize < 4
      offset = 0
      count = payload.byteslice(offset, 2).unpack1("v")
      offset += 2
      null_bytes = payload.byteslice(offset, 2).unpack1("v")
      offset += 2
      null_bitmap = payload.byteslice(offset, null_bytes)
      offset += null_bytes
      values = []
      count.times do |idx|
        byte_index = idx / 8
        bit_index = idx % 8
        is_null = byte_index < null_bytes && ((null_bitmap.getbyte(byte_index) & (1 << bit_index)) != 0)
        if is_null
          values << { data: nil }
          next
        end
        length = payload.byteslice(offset, 4).unpack1("l<")
        offset += 4
        if length < 0
          values << { data: nil }
          next
        end
        data = payload.byteslice(offset, length)
        offset += length
        values << { data: data }
      end
      values
    end

    def self.parse_command_complete(payload)
      raise "Command complete truncated" if payload.bytesize < 20
      command_type = payload.getbyte(0)
      rows = payload.byteslice(4, 8).unpack1("Q<")
      last_id = payload.byteslice(12, 8).unpack1("Q<")
      tag_bytes = payload.byteslice(20, payload.bytesize - 20)
      tag = tag_bytes ? tag_bytes.split("\0", 2).first.to_s : ""
      [command_type, rows, last_id, tag]
    end

    def self.parse_notification(payload)
      raise "Notification truncated" if payload.bytesize < 12
      offset = 0
      process_id = payload.byteslice(offset, 4).unpack1("V")
      offset += 4
      channel_len = payload.byteslice(offset, 4).unpack1("V")
      offset += 4
      raise "Notification truncated" if offset + channel_len + 4 > payload.bytesize
      channel = payload.byteslice(offset, channel_len).to_s
      offset += channel_len
      payload_len = payload.byteslice(offset, 4).unpack1("V")
      offset += 4
      raise "Notification truncated" if offset + payload_len > payload.bytesize
      data = payload.byteslice(offset, payload_len) || ""
      offset += payload_len
      change_type = nil
      row_id = nil
      if offset < payload.bytesize
        change_type = payload.getbyte(offset).chr
        offset += 1
        if offset + 8 <= payload.bytesize
          row_id = payload.byteslice(offset, 8).unpack1("Q<")
        end
      end
      [process_id, channel, data, change_type, row_id]
    end

    def self.parse_query_plan(payload)
      raise "Query plan truncated" if payload.bytesize < 32
      format = payload.byteslice(0, 4).unpack1("V")
      plan_len = payload.byteslice(4, 4).unpack1("V")
      planning = payload.byteslice(8, 8).unpack1("Q<")
      estimated_rows = payload.byteslice(16, 8).unpack1("Q<")
      estimated_cost = payload.byteslice(24, 8).unpack1("Q<")
      raise "Query plan truncated" if 32 + plan_len > payload.bytesize
      plan = payload.byteslice(32, plan_len) || ""
      [format, planning, estimated_rows, estimated_cost, plan]
    end

    def self.parse_sblr_compiled(payload)
      raise "SBLR compiled truncated" if payload.bytesize < 16
      hash = payload.byteslice(0, 8).unpack1("Q<")
      version = payload.byteslice(8, 4).unpack1("V")
      length = payload.byteslice(12, 4).unpack1("V")
      raise "SBLR compiled truncated" if 16 + length > payload.bytesize
      bytecode = payload.byteslice(16, length) || ""
      { hash: hash, version: version, bytecode: bytecode }
    end

    def self.parse_copy_in_response(payload)
      raise "copy in response truncated" if payload.bytesize < 5
      format = payload.getbyte(0)
      window_bytes = payload.byteslice(1, 4).unpack1("V")
      [format, window_bytes]
    end

    def self.parse_error_message(payload)
      offset = 0
      severity = ""
      sqlstate = ""
      message = ""
      detail = ""
      hint = ""
      while offset < payload.bytesize
        field = payload.getbyte(offset)
        offset += 1
        break if field == 0
        start = offset
        offset += 1 while offset < payload.bytesize && payload.getbyte(offset) != 0
        break if offset >= payload.bytesize
        value = payload.byteslice(start, offset - start).to_s
        offset += 1
        case field.chr
        when "S" then severity = value
        when "C" then sqlstate = value
        when "M" then message = value
        when "D" then detail = value
        when "H" then hint = value
        end
      end
      [severity, sqlstate, message, detail, hint]
    end

    def self.pad_bytes(data, length)
      return data if data.bytesize == length
      return data.byteslice(0, length) if data.bytesize > length
      data + "\0" * (length - data.bytesize)
    end
  end
end
