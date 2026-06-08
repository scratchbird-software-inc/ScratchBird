# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

require "test_helper"
require "bigdecimal"
require "date"
require "json"
require "time"

class TestTypes < Minitest::Test
  def test_decode_uuid
    bytes = ["12345678123456781234567812345678"].pack("H*")
    out = Scratchbird::Types.decode(Scratchbird::Types::WIRE_UUID, bytes)
    assert_equal "12345678-1234-5678-1234-567812345678", out
  end

  def test_decode_array
    data = "{1,2,3}".dup.force_encoding("UTF-8")
    out = Scratchbird::Types.decode(Scratchbird::Types::WIRE_ARRAY, data)
    assert_equal [1, 2, 3], out
  end

  def test_encode_decode_integer_float_and_text_roundtrip
    int_out = decode_encoded(42)
    int8_out = decode_encoded(4_000_000_000)
    float_out = decode_encoded(3.5)
    text_out = decode_encoded("hello")

    assert_equal 42, int_out
    assert_equal 4_000_000_000, int8_out
    assert_in_delta 3.5, float_out, 0.0000001
    assert_equal "hello", text_out
  end

  def test_encode_decode_date_and_timestamp_roundtrip
    date_value = Date.new(2026, 3, 5)
    ts_value = Time.utc(2026, 3, 5, 13, 14, 15)

    decoded_date = decode_encoded(date_value)
    decoded_ts = decode_encoded(ts_value)

    assert_equal date_value, decoded_date.to_date
    assert_in_delta ts_value.to_f, decoded_ts.to_f, 0.000001
  end

  def test_encode_decode_numeric_json_and_jsonb_roundtrip
    numeric = Scratchbird::Types.encode_param(BigDecimal("123.456"))
    json = Scratchbird::Types.encode_param({ "a" => 1, "b" => true })
    jsonb = Scratchbird::Types.encode_param(Scratchbird::JSONB.new(nil, { "k" => "v" }))

    decoded_numeric = Scratchbird::Types.decode(numeric[:oid], numeric[:param][:data], numeric[:param][:format])
    decoded_json = Scratchbird::Types.decode(json[:oid], json[:param][:data], json[:param][:format])
    decoded_jsonb = Scratchbird::Types.decode(jsonb[:oid], jsonb[:param][:data], jsonb[:param][:format])

    assert_equal "123.456", decoded_numeric
    assert_equal({ "a" => 1, "b" => true }, JSON.parse(decoded_json))
    assert_instance_of Scratchbird::JSONB, decoded_jsonb
    assert_equal({ "k" => "v" }, JSON.parse(decoded_jsonb.raw))
  end

  def test_encode_decode_range_and_composite
    range_value = Scratchbird::RangeValue.new(
      lower: 10,
      upper: 20,
      lower_inclusive: true,
      upper_inclusive: false
    )
    range_encoded = Scratchbird::Types.encode_param(range_value)
    range_decoded = Scratchbird::Types.decode(
      range_encoded[:oid],
      range_encoded[:param][:data],
      range_encoded[:param][:format]
    )
    assert_instance_of Scratchbird::RangeValue, range_decoded
    assert_equal 10, range_decoded.lower
    assert_equal 20, range_decoded.upper
    assert_equal true, range_decoded.lower_inclusive
    assert_equal false, range_decoded.upper_inclusive

    composite = Scratchbird::Composite.new(
      [
        Scratchbird::CompositeField.new(oid: Scratchbird::Types::OID_INT4, value: 7),
        Scratchbird::CompositeField.new(oid: Scratchbird::Types::OID_TEXT, value: "bird")
      ],
      type_oid: Scratchbird::Types::OID_RECORD
    )
    comp_encoded = Scratchbird::Types.encode_param(composite)
    comp_decoded = Scratchbird::Types.decode(
      comp_encoded[:oid],
      comp_encoded[:param][:data],
      comp_encoded[:param][:format]
    )
    assert_instance_of Scratchbird::Composite, comp_decoded
    assert_equal 2, comp_decoded.fields.length
    assert_equal 7, comp_decoded.fields[0].value
    assert_equal "bird", comp_decoded.fields[1].value
  end

  def test_encode_decode_vector_and_null
    vector_encoded = Scratchbird::Types.encode_param([1.0, 2.5, 3.25])
    vector_decoded = Scratchbird::Types.decode(
      vector_encoded[:oid],
      vector_encoded[:param][:data],
      vector_encoded[:param][:format]
    )
    null_encoded = Scratchbird::Types.encode_param(nil)

    assert_equal Scratchbird::Types::OID_SB_VECTOR, vector_encoded[:oid]
    assert_equal [1.0, 2.5, 3.25], vector_decoded
    assert_equal true, null_encoded[:param][:is_null]
    assert_equal 0, null_encoded[:oid]
  end

  def test_decode_unknown_oid_text_and_binary_paths
    assert_equal true, Scratchbird::Types.decode(0, "true", Scratchbird::Types::FORMAT_TEXT)
    assert_equal 101, Scratchbird::Types.decode(0, "101", Scratchbird::Types::FORMAT_TEXT)
    assert_in_delta 1.25, Scratchbird::Types.decode(0, "1.25", Scratchbird::Types::FORMAT_TEXT), 0.0000001

    raw_int = [1234].pack("l<")
    raw_uuid = ["12345678123456781234567812345678"].pack("H*")
    assert_equal 1234, Scratchbird::Types.decode(0, raw_int, Scratchbird::Types::FORMAT_BINARY)
    assert_equal "12345678-1234-5678-1234-567812345678", Scratchbird::Types.decode(0, raw_uuid, Scratchbird::Types::FORMAT_BINARY)
  end

  private

  def decode_encoded(value)
    encoded = Scratchbird::Types.encode_param(value)
    Scratchbird::Types.decode(encoded[:oid], encoded[:param][:data], encoded[:param][:format])
  end
end
