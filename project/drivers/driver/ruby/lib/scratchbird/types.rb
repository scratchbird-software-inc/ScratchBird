# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "bigdecimal"
require "time"
require "json"

module Scratchbird
  class JSONB
    attr_reader :raw, :value

    def initialize(raw, value = nil)
      @raw = raw
      @value = value
    end
  end

  class Geometry
    attr_reader :wkb, :srid, :wkt

    def initialize(wkb, srid: nil, wkt: nil)
      @wkb = wkb
      @srid = srid
      @wkt = wkt
    end
  end

  class CompositeField
    attr_reader :oid, :value, :raw

    def initialize(oid:, value: nil, raw: nil)
      @oid = oid
      @value = value
      @raw = raw
    end
  end

  class Composite
    attr_reader :type_oid, :fields

    def initialize(fields = [], type_oid: nil)
      @fields = fields
      @type_oid = type_oid
    end
  end

  class RangeValue
    attr_accessor :lower, :upper, :lower_inclusive, :upper_inclusive,
                  :lower_infinite, :upper_infinite, :empty, :range_oid

    def initialize(opts = {})
      @lower = opts[:lower]
      @upper = opts[:upper]
      @lower_inclusive = opts.fetch(:lower_inclusive, false)
      @upper_inclusive = opts.fetch(:upper_inclusive, false)
      @lower_infinite = opts.fetch(:lower_infinite, false)
      @upper_infinite = opts.fetch(:upper_infinite, false)
      @empty = opts.fetch(:empty, false)
      @range_oid = opts[:range_oid]
    end
  end

  module Types
    FORMAT_TEXT = 0
    FORMAT_BINARY = 1

    # Wire-only sentinel OIDs for tests/decoders
    WIRE_ARRAY = -1
    WIRE_UUID = 2950

    OID_BOOL = 16
    OID_BYTEA = 17
    OID_CHAR = 18
    OID_INT8 = 20
    OID_INT2 = 21
    OID_INT4 = 23
    OID_TEXT = 25
    OID_JSON = 114
    OID_XML = 142
    OID_POINT = 600
    OID_LSEG = 601
    OID_PATH = 602
    OID_BOX = 603
    OID_POLYGON = 604
    OID_LINE = 628
    OID_FLOAT4 = 700
    OID_FLOAT8 = 701
    OID_CIRCLE = 718
    OID_MONEY = 790
    OID_MACADDR = 829
    OID_CIDR = 650
    OID_INET = 869
    OID_MACADDR8 = 774
    OID_BPCHAR = 1042
    OID_VARCHAR = 1043
    OID_DATE = 1082
    OID_TIME = 1083
    OID_TIMESTAMP = 1114
    OID_TIMESTAMPTZ = 1184
    OID_INTERVAL = 1186
    OID_TIMETZ = 1266
    OID_NUMERIC = 1700
    OID_UUID = 2950
    OID_JSONB = 3802
    OID_RECORD = 2249
    OID_INT4RANGE = 3904
    OID_NUMRANGE = 3906
    OID_TSRANGE = 3908
    OID_TSTZRANGE = 3910
    OID_DATERANGE = 3912
    OID_INT8RANGE = 3926
    OID_TSVECTOR = 3614
    OID_TSQUERY = 3615
    OID_SB_VECTOR = 16386

    RANGE_EMPTY = 0x01
    RANGE_LB_INC = 0x02
    RANGE_UB_INC = 0x04
    RANGE_LB_INF = 0x08
    RANGE_UB_INF = 0x10

    UUID_REGEX = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i

    def self.encode_param(value)
      return { param: { format: FORMAT_BINARY, is_null: true }, oid: 0 } if value.nil?

      if value.is_a?(JSONB)
        raw = value.raw
        if (raw.nil? || raw.empty?) && !value.value.nil?
          raw = JSON.generate(value.value)
        end
        raise ArgumentError, "JSONB requires raw payload" if raw.nil? || raw.empty?
        return { param: { format: FORMAT_BINARY, data: encode_length_prefixed(raw) }, oid: OID_JSONB }
      end

      if value.is_a?(Geometry)
        raise ArgumentError, "geometry requires WKB payload" if value.wkb.nil? || value.wkb.empty?
        return { param: { format: FORMAT_BINARY, data: encode_length_prefixed(value.wkb) }, oid: OID_POINT }
      end

      if value.is_a?(RangeValue)
        encoded = encode_range(value)
        return { param: { format: FORMAT_BINARY, data: encoded[:data] }, oid: encoded[:oid] }
      end

      if value.is_a?(Composite)
        data, oid = encode_composite(value)
        return { param: { format: FORMAT_BINARY, data: data }, oid: oid }
      end

      if value.is_a?(Time) || value.is_a?(DateTime)
        return { param: { format: FORMAT_BINARY, data: encode_timestamp(value) }, oid: OID_TIMESTAMPTZ }
      end

      if value.is_a?(Date)
        return { param: { format: FORMAT_BINARY, data: encode_date(value) }, oid: OID_DATE }
      end

      if value == true || value == false
        return { param: { format: FORMAT_BINARY, data: value ? "\x01" : "\x00" }, oid: OID_BOOL }
      end

      if value.is_a?(Integer)
        if value >= -2_147_483_648 && value <= 2_147_483_647
          return { param: { format: FORMAT_BINARY, data: [value].pack("l<") }, oid: OID_INT4 }
        end
        return { param: { format: FORMAT_BINARY, data: [value].pack("q<") }, oid: OID_INT8 }
      end

      if value.is_a?(Float)
        return { param: { format: FORMAT_BINARY, data: [value].pack("E") }, oid: OID_FLOAT8 }
      end

      if value.is_a?(BigDecimal)
        return { param: { format: FORMAT_BINARY, data: encode_length_prefixed(value.to_s("F")) }, oid: OID_NUMERIC }
      end

      if value.is_a?(String)
        if value.match?(UUID_REGEX)
          return { param: { format: FORMAT_BINARY, data: [value.delete("-")].pack("H*") }, oid: OID_UUID }
        end
        return { param: { format: FORMAT_BINARY, data: encode_length_prefixed(value) }, oid: OID_TEXT }
      end

      if value.is_a?(Array)
        if value.all? { |item| item.is_a?(Numeric) }
          literal = format_vector_literal(value)
          return { param: { format: FORMAT_BINARY, data: encode_length_prefixed(literal) }, oid: OID_SB_VECTOR }
        end
        literal = format_array_literal(value)
        return { param: { format: FORMAT_BINARY, data: encode_length_prefixed(literal) }, oid: 0 }
      end

      if value.is_a?(Hash) || value.is_a?(Object)
        raw = JSON.generate(value)
        return { param: { format: FORMAT_BINARY, data: encode_length_prefixed(raw) }, oid: OID_JSON }
      end

      raise ArgumentError, "unsupported parameter type"
    end

    def self.decode(type_oid, data, format = nil)
      return nil if data.nil?
      if type_oid == WIRE_ARRAY
        return parse_array_literal(decode_text_value(data))
      end
      format = type_oid == OID_UUID ? FORMAT_BINARY : FORMAT_TEXT if format.nil?
      if type_oid.to_i == 0
        return parse_unknown_text(decode_text_value(data)) if format == FORMAT_TEXT
        return decode_unknown_binary(data)
      end
      return decode_text_typed_value(type_oid, data) if format == FORMAT_TEXT
      decode_binary_value(type_oid, data)
    end

    def self.oid_name(oid)
      case oid
      when OID_BOOL then "boolean"
      when OID_INT2 then "int2"
      when OID_INT4 then "int4"
      when OID_INT8 then "int8"
      when OID_FLOAT4 then "float4"
      when OID_FLOAT8 then "float8"
      when OID_NUMERIC then "numeric"
      when OID_MONEY then "money"
      when OID_TEXT then "text"
      when OID_VARCHAR then "varchar"
      when OID_CHAR, OID_BPCHAR then "char"
      when OID_BYTEA then "bytea"
      when OID_DATE then "date"
      when OID_TIME then "time"
      when OID_TIMESTAMP then "timestamp"
      when OID_TIMESTAMPTZ then "timestamptz"
      when OID_INTERVAL then "interval"
      when OID_UUID then "uuid"
      when OID_JSON then "json"
      when OID_JSONB then "jsonb"
      when OID_XML then "xml"
      when OID_INET then "inet"
      when OID_CIDR then "cidr"
      when OID_MACADDR then "macaddr"
      when OID_MACADDR8 then "macaddr8"
      when OID_TSVECTOR then "tsvector"
      when OID_TSQUERY then "tsquery"
      when OID_INT4RANGE then "int4range"
      when OID_INT8RANGE then "int8range"
      when OID_NUMRANGE then "numrange"
      when OID_TSRANGE then "tsrange"
      when OID_TSTZRANGE then "tstzrange"
      when OID_DATERANGE then "daterange"
      when OID_SB_VECTOR then "vector"
      else "unknown"
      end
    end

    def self.decode_binary_value(type_oid, data)
      text_fallback = maybe_decode_binary_text_value(type_oid, data)
      return text_fallback unless text_fallback.nil?

      case type_oid
      when OID_BOOL
        data.getbyte(0) == 1
      when OID_INT2
        data.unpack1("s<")
      when OID_INT4
        data.unpack1("l<")
      when OID_INT8
        data.unpack1("q<")
      when OID_FLOAT4
        data.unpack1("g")
      when OID_FLOAT8
        data.unpack1("E")
      when OID_NUMERIC
        strip_length_prefixed(data)
      when OID_MONEY
        cents = data.unpack1("q<")
        BigDecimal(cents) / 100
      when OID_TEXT, OID_VARCHAR, OID_CHAR, OID_BPCHAR, OID_JSON, OID_XML, OID_TSVECTOR, OID_TSQUERY
        strip_length_prefixed(data).force_encoding("UTF-8")
      when OID_JSONB
        JSONB.new(strip_length_prefixed(data))
      when OID_BYTEA
        strip_length_prefixed(data)
      when OID_DATE
        decode_date(data)
      when OID_TIME
        decode_time(data)
      when OID_TIMESTAMP, OID_TIMESTAMPTZ
        decode_timestamp(data)
      when OID_INTERVAL
        decode_interval(data)
      when OID_UUID
        bytes_to_uuid(data)
      when OID_INET, OID_CIDR, OID_MACADDR, OID_MACADDR8
        strip_length_prefixed(data).force_encoding("UTF-8")
      when OID_INT4RANGE, OID_INT8RANGE, OID_NUMRANGE, OID_TSRANGE, OID_TSTZRANGE, OID_DATERANGE
        decode_range(type_oid, data)
      when OID_SB_VECTOR
        parse_vector_literal(strip_length_prefixed(data))
      when OID_POINT, OID_LSEG, OID_PATH, OID_BOX, OID_POLYGON, OID_LINE, OID_CIRCLE
        Geometry.new(strip_length_prefixed(data))
      when OID_RECORD
        decode_composite(data)
      else
        data
      end
    end

    def self.maybe_decode_binary_text_value(type_oid, data)
      text_compatible_oids = [
        OID_TEXT, OID_VARCHAR, OID_CHAR, OID_BPCHAR, OID_JSON, OID_JSONB,
        OID_XML, OID_TSVECTOR, OID_TSQUERY, OID_BYTEA,
        OID_INET, OID_CIDR, OID_MACADDR, OID_MACADDR8, OID_SB_VECTOR
      ]
      return nil unless text_compatible_oids.include?(type_oid)

      candidates = []
      trimmed = strip_trailing_nulls(data)
      if trimmed.bytesize.positive? && looks_like_text(trimmed)
        candidates << trimmed
      end
      if data.bytesize >= 4
        stripped = strip_length_prefixed(data)
        if stripped != data && stripped.bytesize.positive? && looks_like_text(stripped)
          candidates << stripped
        end
      end
      candidates.each do |candidate|
        begin
          return decode_text_typed_value(type_oid, candidate)
        rescue StandardError
          next
        end
      end
      nil
    end

    def self.decode_text_value(data)
      if data.bytesize >= 4
        length = data.byteslice(0, 4).unpack1("V")
        return data.byteslice(4, length).to_s if length <= data.bytesize - 4
      end
      data
    end

    def self.decode_text_typed_value(type_oid, data)
      text = decode_text_value(data).to_s
      stripped = text.strip
      case type_oid
      when OID_BOOL
        lowered = stripped.downcase
        return true if lowered == "true" || lowered == "t" || stripped == "1"
        return false if lowered == "false" || lowered == "f" || stripped == "0"
        text
      when OID_INT2, OID_INT4, OID_INT8
        Integer(stripped, 10)
      when OID_FLOAT4, OID_FLOAT8
        Float(stripped)
      when OID_NUMERIC, OID_MONEY
        BigDecimal(stripped)
      when OID_JSONB
        JSONB.new(stripped)
      when OID_BYTEA
        stripped
      when OID_SB_VECTOR
        parse_vector_literal(stripped)
      else
        text
      end
    end

    def self.decode_unknown_binary(data)
      trimmed = strip_trailing_nulls(data)
      if trimmed.bytesize.positive? && looks_like_text(trimmed)
        return parse_unknown_text(trimmed.force_encoding("UTF-8"))
      end
      case data.bytesize
      when 1 then data.getbyte(0)
      when 2 then data.unpack1("s<")
      when 4 then data.unpack1("l<")
      when 8 then data.unpack1("q<")
      when 16 then bytes_to_uuid(data)
      else data
      end
    end

    def self.parse_unknown_text(text)
      trimmed = text.strip
      return text if trimmed.empty?
      lowered = trimmed.downcase
      return true if lowered == "true"
      return false if lowered == "false"
      if trimmed.match?(/\A[+-]?\d+\z/)
        begin
          return Integer(trimmed, 10)
        rescue ArgumentError
          return trimmed
        end
      end
      if trimmed.match?(/\A[+-]?(?:\d+\.?\d*|\d*\.?\d+)(?:[eE][+-]?\d+)?\z/)
        begin
          return Float(trimmed)
        rescue ArgumentError
          return trimmed
        end
      end
      text
    end

    def self.parse_array_literal(text)
      return [] if text.nil?
      trimmed = text.strip
      return [] if trimmed == "{}"
      return [parse_array_scalar(trimmed)] unless trimmed.start_with?("{") && trimmed.end_with?("}")

      inner = trimmed[1..-2]
      items = []
      buffer = +""
      in_quotes = false
      escape = false
      depth = 0

      inner.each_char do |ch|
        if in_quotes
          if escape
            buffer << ch
            escape = false
          elsif ch == "\\"
            escape = true
          elsif ch == "\""
            in_quotes = false
          else
            buffer << ch
          end
          next
        end

        case ch
        when "\""
          in_quotes = true
        when "{"
          depth += 1
          buffer << ch
        when "}"
          depth -= 1
          buffer << ch
        when ","
          if depth.zero?
            items << parse_array_scalar(buffer)
            buffer.clear
          else
            buffer << ch
          end
        else
          buffer << ch
        end
      end

      items << parse_array_scalar(buffer) unless buffer.empty?
      items
    end

    def self.parse_array_scalar(token)
      return nil if token.nil?
      value = token.strip
      return nil if value.casecmp("NULL").zero?
      if value.start_with?("{") && value.end_with?("}")
        return parse_array_literal(value)
      end
      if value.start_with?("\"") && value.end_with?("\"")
        inner = value[1..-2]
        return inner.gsub("\\\"", "\"").gsub("\\\\", "\\")
      end
      return Integer(value, 10) if value.match?(/\A[+-]?\d+\z/)
      return Float(value) if value.match?(/\A[+-]?(?:\d+\.?\d*|\d*\.?\d+)(?:[eE][+-]?\d+)?\z/)
      value
    end

    def self.strip_trailing_nulls(data)
      end_idx = data.bytesize
      while end_idx.positive? && data.getbyte(end_idx - 1) == 0
        end_idx -= 1
      end
      data.byteslice(0, end_idx)
    end

    def self.looks_like_text(data)
      data.each_byte do |byte|
        next if byte == 9 || byte == 10 || byte == 13
        return false if byte < 0x20 || byte > 0x7e
      end
      true
    end

    def self.encode_length_prefixed(data)
      [data.to_s.bytesize].pack("V") + data.to_s
    end

    def self.encode_composite(value)
      fields = value.fields || []
      type_oid = value.type_oid || OID_RECORD
      buffer = +""
      buffer << [fields.length].pack("l<")
      fields.each do |field|
        field_oid = field.oid
        data = nil
        if !field.raw.nil?
          data = field.raw
        elsif !field.value.nil?
          encoded = encode_param(field.value)
          field_oid = encoded[:oid] if field_oid.to_i == 0
          data = encoded[:param][:is_null] ? nil : encoded[:param][:data]
        end
        raise ArgumentError, "composite field OID is required" if field_oid.to_i == 0
        buffer << [field_oid].pack("L<")
        if data.nil?
          buffer << [-1].pack("l<")
        else
          buffer << [data.bytesize].pack("l<")
          buffer << data
        end
      end
      [buffer, type_oid]
    end

    def self.decode_composite(data)
      return Composite.new([]) if data.bytesize < 4
      count = data.byteslice(0, 4).unpack1("l<")
      offset = 4
      fields = []
      count.times do
        break if offset + 8 > data.bytesize
        oid = data.byteslice(offset, 4).unpack1("L<")
        offset += 4
        length = data.byteslice(offset, 4).unpack1("l<")
        offset += 4
        if length < 0
          fields << CompositeField.new(oid: oid, value: nil, raw: nil)
          next
        end
        break if offset + length > data.bytesize
        raw = data.byteslice(offset, length)
        offset += length
        value = decode_binary_value(oid, raw)
        fields << CompositeField.new(oid: oid, value: value, raw: raw)
      end
      Composite.new(fields)
    end

    def self.strip_length_prefixed(data)
      return data if data.bytesize < 4
      length = data.byteslice(0, 4).unpack1("V")
      return data.byteslice(4, length) if length <= data.bytesize - 4
      data
    end

    def self.decode_date(data)
      days = data.unpack1("l<")
      Time.utc(2000, 1, 1) + (days * 86_400)
    end

    def self.decode_time(data)
      micros = data.unpack1("q<")
      Time.at(0, micros, :usec).utc
    end

    def self.decode_timestamp(data)
      micros = data.unpack1("q<")
      base = Time.utc(2000, 1, 1)
      base + (micros / 1_000_000.0)
    end

    def self.decode_interval(data)
      micros = data.byteslice(0, 8).unpack1("q<")
      days = data.byteslice(8, 4).unpack1("l<")
      months = data.byteslice(12, 4).unpack1("l<")
      { months: months, days: days, micros: micros }
    end

    def self.bytes_to_uuid(data)
      hex = data.unpack1("H*")
      return hex unless hex.length == 32
      "#{hex[0, 8]}-#{hex[8, 4]}-#{hex[12, 4]}-#{hex[16, 4]}-#{hex[20, 12]}"
    end

    def self.encode_timestamp(value)
      t = value.is_a?(Time) ? value.utc : value.to_time.utc
      base = Time.utc(2000, 1, 1)
      micros = ((t.to_f - base.to_f) * 1_000_000).to_i
      [micros].pack("q<")
    end

    def self.encode_date(value)
      date = value.is_a?(Date) ? value : Date.parse(value.to_s)
      base = Date.new(2000, 1, 1)
      days = (date - base).to_i
      [days].pack("l<")
    end

    def self.encode_range(range)
      oid = resolve_range_oid(range)
      flags = 0
      flags |= RANGE_EMPTY if range.empty
      flags |= RANGE_LB_INC if range.lower_inclusive
      flags |= RANGE_UB_INC if range.upper_inclusive
      flags |= RANGE_LB_INF if range.lower_infinite
      flags |= RANGE_UB_INF if range.upper_infinite
      parts = [flags, 0, 0, 0].pack("C4")
      if !range.empty && !range.lower_infinite
        bound = encode_range_bound(oid, range.lower)
        parts << [bound.bytesize].pack("V")
        parts << bound
      end
      if !range.empty && !range.upper_infinite
        bound = encode_range_bound(oid, range.upper)
        parts << [bound.bytesize].pack("V")
        parts << bound
      end
      { data: parts, oid: oid }
    end

    def self.resolve_range_oid(range)
      return range.range_oid if range.range_oid
      sample = range.lower || range.upper
      raise ArgumentError, "range type cannot be inferred" if sample.nil?
      return OID_TSTZRANGE if sample.is_a?(Time) || sample.is_a?(DateTime)
      return OID_DATERANGE if sample.is_a?(Date)
      return (sample >= -2_147_483_648 && sample <= 2_147_483_647) ? OID_INT4RANGE : OID_INT8RANGE if sample.is_a?(Integer)
      OID_NUMRANGE
    end

    def self.encode_range_bound(oid, value)
      case oid
      when OID_INT4RANGE
        [value.to_i].pack("l<")
      when OID_INT8RANGE
        [value.to_i].pack("q<")
      when OID_NUMRANGE
        encode_length_prefixed(value.to_s)
      when OID_DATERANGE
        encode_date(value)
      when OID_TSRANGE, OID_TSTZRANGE
        encode_timestamp(value)
      else
        encode_length_prefixed(value.to_s)
      end
    end

    def self.decode_range(oid, data)
      return RangeValue.new(range_oid: oid) if data.bytesize < 4
      flags = data.getbyte(0)
      offset = 4
      range = RangeValue.new(
        empty: (flags & RANGE_EMPTY) != 0,
        lower_inclusive: (flags & RANGE_LB_INC) != 0,
        upper_inclusive: (flags & RANGE_UB_INC) != 0,
        lower_infinite: (flags & RANGE_LB_INF) != 0,
        upper_infinite: (flags & RANGE_UB_INF) != 0,
        range_oid: oid
      )
      return range if range.empty
      unless range.lower_infinite
        length = data.byteslice(offset, 4).unpack1("V")
        offset += 4
        bound = data.byteslice(offset, length)
        offset += length
        range.lower = decode_range_bound(oid, bound)
      end
      unless range.upper_infinite
        length = data.byteslice(offset, 4).unpack1("V")
        offset += 4
        bound = data.byteslice(offset, length)
        range.upper = decode_range_bound(oid, bound)
      end
      range
    end

    def self.decode_range_bound(oid, data)
      case oid
      when OID_INT4RANGE
        data.unpack1("l<")
      when OID_INT8RANGE
        data.unpack1("q<")
      when OID_NUMRANGE
        strip_length_prefixed(data)
      when OID_DATERANGE
        decode_date(data)
      when OID_TSRANGE, OID_TSTZRANGE
        decode_timestamp(data)
      else
        nil
      end
    end

    def self.format_array_literal(values)
      items = values.map { |value| format_array_item(value) }
      "{#{items.join(',')}}"
    end

    def self.format_array_item(value)
      return "NULL" if value.nil?
      return format_array_literal(value) if value.is_a?(Array)
      return "\"#{value.gsub('"', '\\"')}\"" if value.is_a?(String)
      return value ? "true" : "false" if value == true || value == false
      value.to_s
    end

    def self.format_vector_literal(values)
      parts = values.map { |v| v.finite? ? v.to_s : "0" }
      "[#{parts.join(',')}]"
    end

    def self.parse_vector_literal(text)
      trimmed = text.to_s.strip
      trimmed = trimmed[1..-2] if trimmed.start_with?("[") && trimmed.end_with?("]")
      return [] if trimmed.empty?
      trimmed.split(",").map { |item| item.strip.to_f }
    end
  end
end
