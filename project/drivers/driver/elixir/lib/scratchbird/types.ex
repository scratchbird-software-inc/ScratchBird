# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

defmodule ScratchBird.Types do
  @moduledoc false
  import Bitwise
  alias Decimal

  @format_text 0
  @format_binary 1

  @oid_bool 16
  @oid_char 18
  @oid_int8 20
  @oid_int2 21
  @oid_int4 23
  @oid_text 25
  @oid_json 114
  @oid_xml 142
  @oid_point 600
  @oid_float4 700
  @oid_float8 701
  @oid_money 790
  @oid_cidr 650
  @oid_inet 869
  @oid_macaddr 829
  @oid_bpchar 1042
  @oid_varchar 1043
  @oid_date 1082
  @oid_time 1083
  @oid_timestamp 1114
  @oid_timestamptz 1184
  @oid_interval 1186
  @oid_numeric 1700
  @oid_uuid 2950
  @oid_jsonb 3802
  @oid_record 2249
  @oid_int4range 3904
  @oid_numrange 3906
  @oid_tsrange 3908
  @oid_tstzrange 3910
  @oid_daterange 3912
  @oid_int8range 3926
  @oid_tsvector 3614
  @oid_tsquery 3615
  @oid_sb_vector 16386

  @range_empty 0x01
  @range_lb_inc 0x02
  @range_ub_inc 0x04
  @range_lb_inf 0x08
  @range_ub_inf 0x10

  defstruct [:raw, :value]

  defmodule Jsonb do
    defstruct [:raw, :value]
  end

  defmodule Json do
    defstruct [:raw, :value]
  end

  defmodule Geometry do
    defstruct [:wkb, :srid, :wkt]
  end

  defmodule Inet do
    defstruct [:value]
  end

  defmodule Cidr do
    defstruct [:value]
  end

  defmodule Macaddr do
    defstruct [:value]
  end

  defmodule CompositeField do
    defstruct [:oid, :value, :raw]
  end

  defmodule Composite do
    defstruct fields: [], type_oid: 2249
  end

  defmodule Range do
    defstruct [
      :lower,
      :upper,
      lower_inclusive: false,
      upper_inclusive: false,
      lower_infinite: false,
      upper_infinite: false,
      empty: false,
      range_oid: nil
    ]
  end

  defmodule Interval do
    defstruct [:micros, :days, :months]
  end

  defmodule RawValue do
    defstruct [:oid, :data]
  end

  def encode_param(nil), do: {%{format: @format_binary, is_null: true}, 0}

  def encode_param(%RawValue{oid: oid, data: data}) do
    {%{format: @format_binary, data: data, is_null: false}, oid}
  end

  def encode_param(%Jsonb{raw: raw, value: value}) do
    data = raw || encode_json(value)
    {%{format: @format_binary, data: encode_length_prefixed(data), is_null: false}, @oid_jsonb}
  end

  def encode_param(%Json{raw: raw, value: value}) do
    data = raw || encode_json(value)
    {%{format: @format_binary, data: encode_length_prefixed(data), is_null: false}, @oid_json}
  end

  def encode_param(%Geometry{wkb: wkb}) when is_binary(wkb) do
    {%{format: @format_binary, data: encode_length_prefixed(wkb), is_null: false}, @oid_point}
  end

  def encode_param(%Interval{micros: micros, days: days, months: months}) do
    data = <<micros::little-64, days::little-32, months::little-32>>
    {%{format: @format_binary, data: data, is_null: false}, @oid_interval}
  end

  def encode_param(%Composite{} = comp) do
    {data, oid} = encode_composite(comp)
    {%{format: @format_binary, data: data, is_null: false}, oid}
  end

  def encode_param(%Range{} = range) do
    {data, oid} = encode_range(range)
    {%{format: @format_binary, data: data, is_null: false}, oid}
  end

  def encode_param(%Inet{value: value}) do
    {%{format: @format_binary, data: encode_length_prefixed(value), is_null: false}, @oid_inet}
  end

  def encode_param(%Cidr{value: value}) do
    {%{format: @format_binary, data: encode_length_prefixed(value), is_null: false}, @oid_cidr}
  end

  def encode_param(%Macaddr{value: value}) do
    {%{format: @format_binary, data: encode_length_prefixed(value), is_null: false}, @oid_macaddr}
  end

  def encode_param(%Decimal{} = dec) do
    data = Decimal.to_string(dec)
    {%{format: @format_binary, data: encode_length_prefixed(data), is_null: false}, @oid_numeric}
  end

  def encode_param(value) when is_boolean(value) do
    data = if value, do: <<1>>, else: <<0>>
    {%{format: @format_binary, data: data, is_null: false}, @oid_bool}
  end

  def encode_param(value) when is_integer(value) do
    cond do
      value >= -32768 and value <= 32767 ->
        {%{format: @format_binary, data: <<value::little-16>>, is_null: false}, @oid_int2}

      value >= -2_147_483_648 and value <= 2_147_483_647 ->
        {%{format: @format_binary, data: <<value::little-32>>, is_null: false}, @oid_int4}

      true ->
        {%{format: @format_binary, data: <<value::little-64>>, is_null: false}, @oid_int8}
    end
  end

  def encode_param(value) when is_float(value) do
    {%{format: @format_binary, data: <<value::little-float-64>>, is_null: false}, @oid_float8}
  end

  def encode_param(%Date{} = date) do
    base = ~D[2000-01-01]
    days = Date.diff(date, base)
    {%{format: @format_binary, data: <<days::little-32>>, is_null: false}, @oid_date}
  end

  def encode_param(%Time{} = time) do
    micros = time.hour * 3_600_000_000 + time.minute * 60_000_000 + time.second * 1_000_000 + div(time.microsecond |> elem(0), 1)
    {%{format: @format_binary, data: <<micros::little-64>>, is_null: false}, @oid_time}
  end

  def encode_param(%NaiveDateTime{} = ts) do
    data = encode_timestamp(ts)
    {%{format: @format_binary, data: data, is_null: false}, @oid_timestamp}
  end

  def encode_param(%DateTime{} = ts) do
    data = encode_timestamp(ts)
    {%{format: @format_binary, data: data, is_null: false}, @oid_timestamptz}
  end

  def encode_param(value) when is_binary(value) do
    case uuid_to_bytes(value) do
      {:ok, bytes} ->
        {%{format: @format_binary, data: bytes, is_null: false}, @oid_uuid}

      :error ->
        {%{format: @format_binary, data: encode_length_prefixed(value), is_null: false}, @oid_text}
    end
  end

  def encode_param(value) when is_list(value) do
    if Enum.all?(value, &is_float/1) do
      data = format_vector_literal(value)
      {%{format: @format_binary, data: encode_length_prefixed(data), is_null: false}, @oid_sb_vector}
    else
      data = format_array_literal(value)
      {%{format: @format_binary, data: encode_length_prefixed(data), is_null: false}, 0}
    end
  end

  def encode_param(value) when is_map(value) do
    data = encode_json(value)
    {%{format: @format_binary, data: encode_length_prefixed(data), is_null: false}, @oid_json}
  end

  def decode_value(type_oid, data, format) do
    cond do
      data == nil -> nil
      format == @format_text -> data
      true -> decode_binary(type_oid, data)
    end
  end

  defp decode_binary(@oid_bool, <<1, _::binary>>), do: true
  defp decode_binary(@oid_bool, _), do: false
  defp decode_binary(@oid_int2, data), do: :binary.decode_unsigned(data, :little) |> to_signed(16)
  defp decode_binary(@oid_int4, data), do: :binary.decode_unsigned(data, :little) |> to_signed(32)
  defp decode_binary(@oid_int8, data), do: :binary.decode_unsigned(data, :little) |> to_signed(64)
  defp decode_binary(@oid_float4, <<val::little-float-32>>), do: val
  defp decode_binary(@oid_float8, <<val::little-float-64>>), do: val
  defp decode_binary(@oid_numeric, data), do: strip_length_prefix(data)
  defp decode_binary(@oid_money, <<val::little-signed-64>>), do: val
  defp decode_binary(@oid_text, data), do: strip_length_prefix(data)
  defp decode_binary(@oid_varchar, data), do: strip_length_prefix(data)
  defp decode_binary(@oid_char, data), do: strip_length_prefix(data)
  defp decode_binary(@oid_bpchar, data), do: strip_length_prefix(data)
  defp decode_binary(@oid_json, data), do: %Json{raw: strip_length_prefix(data)}
  defp decode_binary(@oid_xml, data), do: strip_length_prefix(data)
  defp decode_binary(@oid_jsonb, data), do: %Jsonb{raw: strip_length_prefix(data)}
  defp decode_binary(@oid_tsvector, data), do: strip_length_prefix(data)
  defp decode_binary(@oid_tsquery, data), do: strip_length_prefix(data)
  defp decode_binary(@oid_point, data), do: %Geometry{wkb: strip_length_prefix(data)}
  defp decode_binary(@oid_uuid, data), do: uuid_from_bytes(data)
  defp decode_binary(@oid_date, <<days::little-32>>), do: Date.add(~D[2000-01-01], days)
  defp decode_binary(@oid_time, <<micros::little-64>>), do: micros
  defp decode_binary(@oid_timestamp, data), do: decode_timestamp(data)
  defp decode_binary(@oid_timestamptz, data), do: decode_timestamp(data)
  defp decode_binary(@oid_interval, <<micros::little-64, days::little-32, months::little-32>>) do
    %Interval{micros: micros, days: days, months: months}
  end

  defp decode_binary(@oid_inet, data), do: strip_length_prefix(data)
  defp decode_binary(@oid_cidr, data), do: strip_length_prefix(data)
  defp decode_binary(@oid_macaddr, data), do: strip_length_prefix(data)
  defp decode_binary(@oid_sb_vector, data), do: parse_vector_literal(strip_length_prefix(data))
  defp decode_binary(@oid_record, data), do: decode_composite(data)

  defp decode_binary(@oid_int4range, data), do: decode_range(@oid_int4range, data)
  defp decode_binary(@oid_int8range, data), do: decode_range(@oid_int8range, data)
  defp decode_binary(@oid_numrange, data), do: decode_range(@oid_numrange, data)
  defp decode_binary(@oid_tsrange, data), do: decode_range(@oid_tsrange, data)
  defp decode_binary(@oid_tstzrange, data), do: decode_range(@oid_tstzrange, data)
  defp decode_binary(@oid_daterange, data), do: decode_range(@oid_daterange, data)

  defp decode_binary(0, data) do
    decoded = strip_length_prefix(data)

    cond do
      String.starts_with?(decoded, "{") -> parse_array_literal(decoded)
      String.starts_with?(decoded, "[") -> parse_vector_literal(decoded)
      true -> %RawValue{oid: 0, data: data}
    end
  end

  defp decode_binary(oid, data), do: %RawValue{oid: oid, data: data}

  defp encode_json(value) when is_binary(value), do: value
  defp encode_json(value) do
    if Code.ensure_loaded?(Jason) do
      Jason.encode!(value)
    else
      raise "Jason is required to encode JSON values"
    end
  end

  defp encode_length_prefixed(data) when is_binary(data) do
    <<byte_size(data)::little-32, data::binary>>
  end

  defp strip_length_prefix(<<len::little-32, rest::binary>>) do
    <<value::binary-size(len), _::binary>> = rest
    value
  end

  defp to_signed(value, bits) do
    sign_bit = 1 <<< (bits - 1)
    if (value &&& sign_bit) != 0 do
      value - (1 <<< bits)
    else
      value
    end
  end

  defp encode_timestamp(%NaiveDateTime{} = ts) do
    base = ~N[2000-01-01 00:00:00]
    micros = NaiveDateTime.diff(ts, base, :microsecond)
    <<micros::little-64>>
  end

  defp encode_timestamp(%DateTime{} = ts) do
    base = ~U[2000-01-01 00:00:00Z]
    micros = DateTime.diff(ts, base, :microsecond)
    <<micros::little-64>>
  end

  defp decode_timestamp(<<micros::little-64>>) do
    base = ~U[2000-01-01 00:00:00Z]
    DateTime.add(base, micros, :microsecond)
  end

  defp uuid_to_bytes(value) do
    if String.match?(value, ~r/^[0-9a-fA-F\-]{36}$/) do
      {:ok, value |> String.replace("-", "") |> Base.decode16!(case: :mixed)}
    else
      :error
    end
  end

  defp uuid_from_bytes(bytes) when byte_size(bytes) == 16 do
    <<a::binary-size(4), b::binary-size(2), c::binary-size(2), d::binary-size(2), e::binary-size(6)>> = Base.encode16(bytes, case: :lower)
    Enum.join([a, b, c, d, e], "-")
  end

  defp decode_range(range_oid, <<flags::8, _pad::binary-size(3), rest::binary>>) do
    range = %Range{
      empty: (flags &&& @range_empty) != 0,
      lower_inclusive: (flags &&& @range_lb_inc) != 0,
      upper_inclusive: (flags &&& @range_ub_inc) != 0,
      lower_infinite: (flags &&& @range_lb_inf) != 0,
      upper_infinite: (flags &&& @range_ub_inf) != 0,
      range_oid: range_oid
    }

    {lower, rest2} = decode_range_bound(range_oid, rest, range.lower_infinite)
    {upper, rest3} = decode_range_bound(range_oid, rest2, range.upper_infinite)
    %{range | lower: lower, upper: upper, range_oid: range_oid} |> then(fn r -> {r, rest3} end) |> elem(0)
  end

  defp decode_range_bound(_range_oid, data, true), do: {nil, data}

  defp decode_range_bound(range_oid, <<len::little-32, rest::binary>>, false) do
    <<raw::binary-size(len), rest2::binary>> = rest
    {decode_range_value(range_oid, raw), rest2}
  end

  defp decode_range_value(@oid_int4range, raw), do: decode_binary(@oid_int4, raw)
  defp decode_range_value(@oid_int8range, raw), do: decode_binary(@oid_int8, raw)
  defp decode_range_value(@oid_numrange, raw), do: strip_length_prefix(raw)
  defp decode_range_value(@oid_daterange, raw), do: decode_binary(@oid_date, raw)
  defp decode_range_value(@oid_tsrange, raw), do: decode_binary(@oid_timestamp, raw)
  defp decode_range_value(@oid_tstzrange, raw), do: decode_binary(@oid_timestamptz, raw)
  defp decode_range_value(_oid, raw), do: raw

  defp encode_composite(%Composite{fields: fields, type_oid: type_oid}) do
    oid = if is_integer(type_oid) and type_oid > 0, do: type_oid, else: @oid_record
    header = <<length(fields)::little-32>>
    payload =
      Enum.reduce(fields, header, fn field, acc ->
        field_oid = field.oid || 0
        {resolved_oid, data} =
          cond do
            is_binary(field.raw) ->
              {field_oid, field.raw}

            field.value != nil ->
              {encoded, inferred_oid} = encode_param(field.value)
              resolved_oid = if field_oid == 0, do: inferred_oid, else: field_oid
              if resolved_oid == 0, do: raise("composite field OID is required")
              payload = if encoded.is_null, do: nil, else: encoded.data
              {resolved_oid, payload}

            true ->
              {field_oid, nil}
          end

        if resolved_oid == 0 do
          raise "composite field OID is required"
        end

        case data do
          nil ->
            acc <> <<resolved_oid::little-32, -1::little-32>>

          _ ->
            acc <> <<resolved_oid::little-32, byte_size(data)::little-32, data::binary>>
        end
      end)

    {payload, oid}
  end

  defp decode_composite(data) when is_binary(data) do
    if byte_size(data) < 4 do
      %Composite{fields: []}
    else
      <<count::little-32, rest::binary>> = data
      {fields, _} = decode_composite_fields(rest, count, [])
      %Composite{fields: fields}
    end
  end

  defp decode_composite_fields(rest, count, fields) when count <= 0 do
    {Enum.reverse(fields), rest}
  end

  defp decode_composite_fields(<<oid::little-32, -1::signed-little-32, rest::binary>>, count, fields) do
    decode_composite_fields(rest, count - 1, [%CompositeField{oid: oid, value: nil, raw: nil} | fields])
  end

  defp decode_composite_fields(<<oid::little-32, len::little-32, rest::binary>>, count, fields) do
    <<raw::binary-size(len), rest2::binary>> = rest
    value = decode_binary(oid, raw)
    decode_composite_fields(rest2, count - 1, [%CompositeField{oid: oid, value: value, raw: raw} | fields])
  end

  defp encode_range(%Range{} = range) do
    oid = resolve_range_oid(range)
    flags = 0
    flags = if range.empty, do: flags ||| @range_empty, else: flags
    flags = if range.lower_inclusive, do: flags ||| @range_lb_inc, else: flags
    flags = if range.upper_inclusive, do: flags ||| @range_ub_inc, else: flags
    flags = if range.lower_infinite, do: flags ||| @range_lb_inf, else: flags
    flags = if range.upper_infinite, do: flags ||| @range_ub_inf, else: flags

    base = <<flags::8, 0::24>>
    lower =
      if range.empty or range.lower_infinite do
        <<>>
      else
        bound = encode_range_bound(oid, range.lower)
        <<byte_size(bound)::little-32, bound::binary>>
      end
    upper =
      if range.empty or range.upper_infinite do
        <<>>
      else
        bound = encode_range_bound(oid, range.upper)
        <<byte_size(bound)::little-32, bound::binary>>
      end
    {base <> lower <> upper, oid}
  end

  defp resolve_range_oid(%Range{range_oid: oid}) when is_integer(oid) and oid > 0, do: oid

  defp resolve_range_oid(%Range{lower: lower, upper: upper}) do
    sample = lower || upper
    cond do
      is_integer(sample) ->
        if sample >= -2_147_483_648 and sample <= 2_147_483_647, do: @oid_int4range, else: @oid_int8range
      is_float(sample) or match?(%Decimal{}, sample) ->
        @oid_numrange
      match?(%Date{}, sample) ->
        @oid_daterange
      match?(%NaiveDateTime{}, sample) ->
        @oid_tsrange
      match?(%DateTime{}, sample) ->
        @oid_tstzrange
      true ->
        raise "unsupported range bound type"
    end
  end

  defp encode_range_bound(@oid_int4range, value) when is_integer(value), do: <<value::little-32>>
  defp encode_range_bound(@oid_int8range, value) when is_integer(value), do: <<value::little-64>>
  defp encode_range_bound(@oid_numrange, %Decimal{} = value), do: encode_length_prefixed(Decimal.to_string(value))
  defp encode_range_bound(@oid_numrange, value), do: encode_length_prefixed(to_string(value))

  defp encode_range_bound(@oid_daterange, %Date{} = date) do
    base = ~D[2000-01-01]
    days = Date.diff(date, base)
    <<days::little-32>>
  end

  defp encode_range_bound(@oid_tsrange, %NaiveDateTime{} = ts), do: encode_timestamp(ts)
  defp encode_range_bound(@oid_tstzrange, %DateTime{} = ts), do: encode_timestamp(ts)

  defp encode_range_bound(_oid, _value), do: raise("unsupported range type")

  defp parse_array_literal(text) do
    trimmed = String.trim(text)
    cond do
      trimmed == "" or trimmed == "{}" -> []
      String.starts_with?(trimmed, "{") and String.ends_with?(trimmed, "}") ->
        trimmed |> String.slice(1..-2//1) |> split_array_items()
      true -> split_array_items(trimmed)
    end
  end

  defp split_array_items(text) do
    {items, buffer, _depth} =
      String.graphemes(text)
      |> Enum.reduce({[], "", 0}, fn ch, {items, buffer, depth} ->
        cond do
          ch == "{" -> {items, buffer <> ch, depth + 1}
          ch == "}" -> {items, buffer <> ch, max(depth - 1, 0)}
          ch == "," and depth == 0 -> {[parse_array_item(buffer) | items], "", depth}
          true -> {items, buffer <> ch, depth}
        end
      end)

    items = if buffer != "" or text != "", do: [parse_array_item(buffer) | items], else: items
    Enum.reverse(items)
  end

  defp parse_array_item(raw) do
    token = String.trim(raw)
    cond do
      token == "" -> ""
      String.upcase(token) == "NULL" -> nil
      String.starts_with?(token, "{") and String.ends_with?(token, "}") -> parse_array_literal(token)
      String.starts_with?(token, "[") and String.ends_with?(token, "]") -> parse_vector_literal(token)
      token == "true" -> true
      token == "false" -> false
      true ->
        case Integer.parse(token) do
          {int, ""} -> int
          _ ->
            case Float.parse(token) do
              {float, ""} -> float
              _ -> token
            end
        end
    end
  end

  defp format_array_literal(values) do
    parts = Enum.map(values, &format_array_item/1)
    "{" <> Enum.join(parts, ",") <> "}"
  end

  defp format_array_item(nil), do: "NULL"
  defp format_array_item(value) when is_binary(value), do: "\"" <> String.replace(value, "\"", "\\\"") <> "\""
  defp format_array_item(value) when is_list(value), do: format_array_literal(value)
  defp format_array_item(value), do: to_string(value)

  defp parse_vector_literal(text) do
    trimmed = String.trim(text)
    trimmed =
      if String.starts_with?(trimmed, "[") and String.ends_with?(trimmed, "]") do
        trimmed |> String.slice(1..-2//1)
      else
        trimmed
      end
    if trimmed == "" do
      []
    else
      trimmed
      |> String.split(",", trim: true)
      |> Enum.map(fn part ->
        case Float.parse(String.trim(part)) do
          {val, _} -> val
          _ -> 0.0
        end
      end)
    end
  end

  defp format_vector_literal(values) do
    parts =
      Enum.map(values, fn v ->
        if is_number(v), do: to_string(v), else: "0"
      end)
    "[" <> Enum.join(parts, ",") <> "]"
  end
end
