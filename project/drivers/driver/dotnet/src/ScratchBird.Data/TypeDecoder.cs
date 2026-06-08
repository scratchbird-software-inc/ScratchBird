// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Buffers.Binary;
using System.Collections;
using System.Data;
using System.Linq;
using System.Globalization;
using System.Text;
using System.Text.Json;

namespace ScratchBird.Data;

public sealed class ScratchBirdJsonb
{
    public byte[] Raw { get; }
    public JsonElement? Value { get; }

    public ScratchBirdJsonb(byte[] raw, JsonElement? value = null)
    {
        Raw = raw ?? Array.Empty<byte>();
        Value = value;
    }
}

public sealed class ScratchBirdJson
{
    public byte[] Raw { get; }
    public JsonElement? Value { get; }

    public ScratchBirdJson(byte[] raw, JsonElement? value = null)
    {
        Raw = raw ?? Array.Empty<byte>();
        Value = value;
    }
}

public sealed class ScratchBirdGeometry
{
    public byte[] Wkb { get; }
    public uint? Srid { get; }
    public string? Wkt { get; }

    public ScratchBirdGeometry(byte[] wkb, uint? srid = null, string? wkt = null)
    {
        Wkb = wkb ?? Array.Empty<byte>();
        Srid = srid;
        Wkt = wkt;
    }
}

public sealed class ScratchBirdRange<T>
{
    public T? Lower { get; set; }
    public T? Upper { get; set; }
    public bool LowerInclusive { get; set; }
    public bool UpperInclusive { get; set; }
    public bool LowerInfinite { get; set; }
    public bool UpperInfinite { get; set; }
    public bool Empty { get; set; }
    public uint? RangeOid { get; set; }
}

public sealed class ScratchBirdInterval
{
    public long Micros { get; }
    public int Days { get; }
    public int Months { get; }

    public ScratchBirdInterval(long micros, int days = 0, int months = 0)
    {
        Micros = micros;
        Days = days;
        Months = months;
    }
}

public sealed class ScratchBirdDate
{
    public DateOnly Value { get; }

    public ScratchBirdDate(DateOnly value)
    {
        Value = value;
    }
}

public sealed class ScratchBirdTime
{
    public long Micros { get; }

    public ScratchBirdTime(long micros)
    {
        Micros = micros;
    }
}

public sealed class ScratchBirdTimeTz
{
    public long Micros { get; }
    public int UtcOffsetSeconds { get; }

    public ScratchBirdTimeTz(long micros, int utcOffsetSeconds)
    {
        Micros = micros;
        UtcOffsetSeconds = utcOffsetSeconds;
    }

    public DateTimeOffset ToDateTimeOffset()
    {
        var time = TimeSpan.FromTicks(Micros * 10);
        var offset = TimeSpan.FromSeconds(UtcOffsetSeconds);
        return new DateTimeOffset(DateOnly.MinValue.ToDateTime(TimeOnly.MinValue) + time, offset);
    }
}

public sealed class ScratchBirdTimestamp
{
    public DateTime Value { get; }

    public ScratchBirdTimestamp(DateTime value)
    {
        Value = value;
    }
}

public sealed class ScratchBirdTimestampTz
{
    public DateTimeOffset Value { get; }

    public ScratchBirdTimestampTz(DateTimeOffset value)
    {
        Value = value;
    }
}

public sealed class ScratchBirdDecimal
{
    public string Value { get; }

    public ScratchBirdDecimal(string value)
    {
        Value = value;
    }
}

public sealed class ScratchBirdMoney
{
    public long Cents { get; }

    public ScratchBirdMoney(long cents)
    {
        Cents = cents;
    }
}

public sealed class ScratchBirdRaw
{
    public uint Oid { get; }
    public byte[] Data { get; }

    public ScratchBirdRaw(uint oid, byte[] data)
    {
        Oid = oid;
        Data = data ?? Array.Empty<byte>();
    }
}

public sealed class ScratchBirdCompositeField
{
    public uint Oid { get; }
    public object? Value { get; }
    public byte[]? Raw { get; }

    public ScratchBirdCompositeField(uint oid, object? value = null, byte[]? raw = null)
    {
        Oid = oid;
        Value = value;
        Raw = raw;
    }
}

public sealed class ScratchBirdComposite
{
    public uint TypeOid { get; }
    public IReadOnlyList<ScratchBirdCompositeField> Fields { get; }

    public ScratchBirdComposite(IReadOnlyList<ScratchBirdCompositeField> fields, uint typeOid = 0)
    {
        Fields = fields;
        TypeOid = typeOid;
    }
}

internal static class TypeDecoder
{
    public const ushort FormatText = 0;
    public const ushort FormatBinary = 1;

    public const uint OidBool = 16;
    public const uint OidBytea = 17;
    public const uint OidChar = 18;
    public const uint OidInt8 = 20;
    public const uint OidInt2 = 21;
    public const uint OidInt4 = 23;
    public const uint OidText = 25;
    public const uint OidJson = 114;
    public const uint OidXml = 142;
    public const uint OidPoint = 600;
    public const uint OidLseg = 601;
    public const uint OidPath = 602;
    public const uint OidBox = 603;
    public const uint OidPolygon = 604;
    public const uint OidLine = 628;
    public const uint OidFloat4 = 700;
    public const uint OidFloat8 = 701;
    public const uint OidCircle = 718;
    public const uint OidMoney = 790;
    public const uint OidMacaddr = 829;
    public const uint OidCidr = 650;
    public const uint OidInet = 869;
    public const uint OidMacaddr8 = 774;
    public const uint OidBpchar = 1042;
    public const uint OidVarchar = 1043;
    public const uint OidDate = 1082;
    public const uint OidTime = 1083;
    public const uint OidTimestamp = 1114;
    public const uint OidTimestamptz = 1184;
    public const uint OidInterval = 1186;
    public const uint OidTimetz = 1266;
    public const uint OidNumeric = 1700;
    public const uint OidUuid = 2950;
    public const uint OidJsonb = 3802;
    public const uint OidRecord = 2249;
    public const uint OidInt4Range = 3904;
    public const uint OidNumRange = 3906;
    public const uint OidTsRange = 3908;
    public const uint OidTstzRange = 3910;
    public const uint OidDateRange = 3912;
    public const uint OidInt8Range = 3926;
    public const uint OidTsVector = 3614;
    public const uint OidTsQuery = 3615;
    public const uint OidSbVector = 16386;

    private const byte RangeEmpty = 0x01;
    private const byte RangeLowerInclusive = 0x02;
    private const byte RangeUpperInclusive = 0x04;
    private const byte RangeLowerInfinite = 0x08;
    private const byte RangeUpperInfinite = 0x10;

    private static readonly DateTimeOffset Epoch2000 = new(new DateTime(2000, 1, 1, 0, 0, 0, DateTimeKind.Utc));

    public static (ParamValue Param, uint Oid) EncodeParameter(ScratchBirdParameter parameter)
    {
        return EncodeParam(parameter.Value, parameter.DbType == DbType.Object ? null : parameter.DbType);
    }

    public static (ParamValue Param, uint Oid) EncodeParam(object? value, DbType? dbType = null)
    {
        if (value == null || value == DBNull.Value)
        {
            return (new ParamValue { Format = FormatBinary, IsNull = true }, DbTypeToOid(dbType));
        }

        if (value is ScratchBirdRaw raw)
        {
            return (new ParamValue { Data = raw.Data, Format = FormatBinary }, raw.Oid);
        }

        if (value is ScratchBirdComposite composite)
        {
            var encoded = EncodeComposite(composite);
            return (new ParamValue { Data = encoded.Data, Format = FormatBinary }, encoded.Oid);
        }

        if (value is ScratchBirdJsonb jsonb)
        {
            var rawBytes = jsonb.Raw;
            if ((rawBytes == null || rawBytes.Length == 0) && jsonb.Value.HasValue)
            {
                rawBytes = Encoding.UTF8.GetBytes(jsonb.Value.Value.GetRawText());
            }
            if (rawBytes == null || rawBytes.Length == 0)
            {
                throw new InvalidOperationException("JSONB requires raw payload");
            }
            return (new ParamValue { Data = EncodeLengthPrefixed(rawBytes), Format = FormatBinary }, OidJsonb);
        }

        if (value is ScratchBirdJson json)
        {
            var rawBytes = json.Raw;
            if ((rawBytes == null || rawBytes.Length == 0) && json.Value.HasValue)
            {
                rawBytes = Encoding.UTF8.GetBytes(json.Value.Value.GetRawText());
            }
            if (rawBytes == null || rawBytes.Length == 0)
            {
                throw new InvalidOperationException("JSON requires raw payload");
            }
            return (new ParamValue { Data = EncodeLengthPrefixed(rawBytes), Format = FormatBinary }, OidJson);
        }

        if (value is ScratchBirdGeometry geometry)
        {
            if (geometry.Wkb.Length == 0)
            {
                throw new InvalidOperationException("geometry requires WKB payload");
            }
            return (new ParamValue { Data = EncodeLengthPrefixed(geometry.Wkb), Format = FormatBinary }, OidPoint);
        }

        if (value is ScratchBirdRange<object> objRange)
        {
            var encoded = EncodeRange(objRange);
            return (new ParamValue { Data = encoded.Data, Format = FormatBinary }, encoded.Oid);
        }

        if (value is ScratchBirdRange<int> intRange)
        {
            var encoded = EncodeRange(CastRange(intRange));
            return (new ParamValue { Data = encoded.Data, Format = FormatBinary }, encoded.Oid);
        }

        if (value is ScratchBirdRange<decimal> decRange)
        {
            var encoded = EncodeRange(CastRange(decRange));
            return (new ParamValue { Data = encoded.Data, Format = FormatBinary }, encoded.Oid);
        }

        if (value is ScratchBirdRange<string> textRange)
        {
            var encoded = EncodeRange(CastRange(textRange));
            return (new ParamValue { Data = encoded.Data, Format = FormatBinary }, encoded.Oid);
        }

        if (value is ScratchBirdRange<long> longRange)
        {
            var encoded = EncodeRange(CastRange(longRange));
            return (new ParamValue { Data = encoded.Data, Format = FormatBinary }, encoded.Oid);
        }

        if (value is ScratchBirdRange<DateOnly> dateRange)
        {
            var encoded = EncodeRange(CastRange(dateRange));
            return (new ParamValue { Data = encoded.Data, Format = FormatBinary }, encoded.Oid);
        }

        if (value is ScratchBirdRange<DateTime> tsRange)
        {
            var encoded = EncodeRange(CastRange(tsRange));
            return (new ParamValue { Data = encoded.Data, Format = FormatBinary }, encoded.Oid);
        }

        if (value is ScratchBirdRange<DateTimeOffset> tstzRange)
        {
            var encoded = EncodeRange(CastRange(tstzRange));
            return (new ParamValue { Data = encoded.Data, Format = FormatBinary }, encoded.Oid);
        }

        if (value is ScratchBirdDate date)
        {
            return (new ParamValue { Data = EncodeDate(date.Value), Format = FormatBinary }, OidDate);
        }

        if (value is DateOnly dateOnly)
        {
            return (new ParamValue { Data = EncodeDate(dateOnly), Format = FormatBinary }, OidDate);
        }

        if (value is ScratchBirdTime time)
        {
            return (new ParamValue { Data = EncodeTimeMicros(time.Micros), Format = FormatBinary }, OidTime);
        }

        if (value is ScratchBirdTimeTz timetz)
        {
            return (new ParamValue { Data = EncodeTimeTzMicros(timetz.Micros, timetz.UtcOffsetSeconds), Format = FormatBinary }, OidTimetz);
        }

        if (value is TimeOnly timeOnly)
        {
            var micros = (long)timeOnly.ToTimeSpan().Ticks / 10;
            return (new ParamValue { Data = EncodeTimeMicros(micros), Format = FormatBinary }, OidTime);
        }

        if (value is ScratchBirdTimestamp timestamp)
        {
            return (new ParamValue { Data = EncodeTimestamp(timestamp.Value), Format = FormatBinary }, OidTimestamp);
        }

        if (value is ScratchBirdTimestampTz timestampTz)
        {
            return (new ParamValue { Data = EncodeTimestamp(timestampTz.Value.UtcDateTime), Format = FormatBinary }, OidTimestamptz);
        }

        if (value is DateTimeOffset dto)
        {
            return (new ParamValue { Data = EncodeTimestamp(dto.UtcDateTime), Format = FormatBinary }, OidTimestamptz);
        }

        if (value is DateTime dt)
        {
            var utc = dt.Kind == DateTimeKind.Utc ? dt : dt.ToUniversalTime();
            return (new ParamValue { Data = EncodeTimestamp(utc), Format = FormatBinary }, OidTimestamptz);
        }

        if (value is ScratchBirdInterval interval)
        {
            return (new ParamValue { Data = EncodeInterval(interval.Micros, interval.Days, interval.Months), Format = FormatBinary }, OidInterval);
        }

        if (value is TimeSpan span)
        {
            var micros = span.Ticks / 10;
            return (new ParamValue { Data = EncodeInterval(micros, span.Days, 0), Format = FormatBinary }, OidInterval);
        }

        if (value is ScratchBirdDecimal dec)
        {
            return (new ParamValue { Data = EncodeLengthPrefixed(Encoding.UTF8.GetBytes(dec.Value)), Format = FormatBinary }, OidNumeric);
        }

        if (value is ScratchBirdMoney money)
        {
            return (new ParamValue { Data = EncodeInt64(money.Cents), Format = FormatBinary }, OidMoney);
        }

        if (value is decimal decimalValue)
        {
            return (new ParamValue { Data = EncodeLengthPrefixed(Encoding.UTF8.GetBytes(decimalValue.ToString(CultureInfo.InvariantCulture))), Format = FormatBinary }, dbType == DbType.Currency ? OidMoney : OidNumeric);
        }

        if (value is bool boolean)
        {
            return (new ParamValue { Data = new[] { boolean ? (byte)1 : (byte)0 }, Format = FormatBinary }, OidBool);
        }

        if (value is short int16)
        {
            return (new ParamValue { Data = EncodeInt16(int16), Format = FormatBinary }, OidInt2);
        }

        if (value is int int32)
        {
            return (new ParamValue { Data = EncodeInt32(int32), Format = FormatBinary }, OidInt4);
        }

        if (value is long int64)
        {
            return (new ParamValue { Data = EncodeInt64(int64), Format = FormatBinary }, OidInt8);
        }

        if (value is float float32)
        {
            return (new ParamValue { Data = EncodeFloat32(float32), Format = FormatBinary }, OidFloat4);
        }

        if (value is double float64)
        {
            return (new ParamValue { Data = EncodeFloat64(float64), Format = FormatBinary }, OidFloat8);
        }

        if (value is Guid guid)
        {
            return (new ParamValue { Data = GuidToBytes(guid), Format = FormatBinary }, OidUuid);
        }

        if (value is byte[] bytes)
        {
            return (new ParamValue { Data = EncodeLengthPrefixed(bytes), Format = FormatBinary }, OidBytea);
        }

        if (value is ReadOnlyMemory<byte> rom)
        {
            return (new ParamValue { Data = EncodeLengthPrefixed(rom.ToArray()), Format = FormatBinary }, OidBytea);
        }

        if (value is Memory<byte> mem)
        {
            return (new ParamValue { Data = EncodeLengthPrefixed(mem.ToArray()), Format = FormatBinary }, OidBytea);
        }

        if (value is float[] vector)
        {
            var literal = FormatVectorLiteral(vector.Select(v => (double)v));
            return (new ParamValue { Data = EncodeLengthPrefixed(Encoding.UTF8.GetBytes(literal)), Format = FormatBinary }, OidSbVector);
        }

        if (value is double[] vector64)
        {
            var literal = FormatVectorLiteral(vector64);
            return (new ParamValue { Data = EncodeLengthPrefixed(Encoding.UTF8.GetBytes(literal)), Format = FormatBinary }, OidSbVector);
        }

        if (value is IEnumerable enumerable && value is not string)
        {
            var items = new List<object?>();
            foreach (var item in enumerable)
            {
                items.Add(item);
            }
            if (items.Count > 0 && items.All(IsNumeric))
            {
                var nums = items.Select(item => Convert.ToDouble(item, CultureInfo.InvariantCulture));
                var literal = FormatVectorLiteral(nums);
                return (new ParamValue { Data = EncodeLengthPrefixed(Encoding.UTF8.GetBytes(literal)), Format = FormatBinary }, OidSbVector);
            }
            var arrayLiteral = FormatArrayLiteral(items);
            return (new ParamValue { Data = EncodeLengthPrefixed(Encoding.UTF8.GetBytes(arrayLiteral)), Format = FormatBinary }, 0);
        }

        if (value is string text)
        {
            if (Guid.TryParse(text, out var parsedGuid))
            {
                return (new ParamValue { Data = GuidToBytes(parsedGuid), Format = FormatBinary }, OidUuid);
            }
            return (new ParamValue { Data = EncodeLengthPrefixed(Encoding.UTF8.GetBytes(text)), Format = FormatBinary }, OidText);
        }

        if (value is JsonElement jsonElement)
        {
            var rawJson = Encoding.UTF8.GetBytes(jsonElement.GetRawText());
            return (new ParamValue { Data = EncodeLengthPrefixed(rawJson), Format = FormatBinary }, OidJson);
        }

        if (value is IDictionary)
        {
            var rawDict = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(value));
            return (new ParamValue { Data = EncodeLengthPrefixed(rawDict), Format = FormatBinary }, OidJson);
        }

        if (value is object)
        {
            var rawObject = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(value));
            return (new ParamValue { Data = EncodeLengthPrefixed(rawObject), Format = FormatBinary }, OidJson);
        }

        throw new InvalidOperationException("Unsupported parameter type");
    }

    public static object? Decode(uint typeOid, byte[]? data, byte format)
    {
        if (data == null)
        {
            return null;
        }
        if (typeOid == 0)
        {
            if (format == FormatText)
            {
                return ParseUnknownText(DecodeTextValue(data).ToString() ?? string.Empty);
            }
            return DecodeUnknownBinary(data);
        }
        if (format == FormatText)
        {
            return DecodeTextValue(data);
        }
        return DecodeBinaryValue(typeOid, data);
    }

    public static string OidToString(uint oid)
    {
        return oid switch
        {
            OidBool => "boolean",
            OidInt2 => "int2",
            OidInt4 => "int4",
            OidInt8 => "int8",
            OidFloat4 => "float4",
            OidFloat8 => "float8",
            OidNumeric => "numeric",
            OidMoney => "money",
            OidText => "text",
            OidVarchar => "varchar",
            OidChar or OidBpchar => "char",
            OidBytea => "bytea",
            OidDate => "date",
            OidTime => "time",
            OidTimetz => "timetz",
            OidTimestamp => "timestamp",
            OidTimestamptz => "timestamptz",
            OidInterval => "interval",
            OidUuid => "uuid",
            OidJson => "json",
            OidJsonb => "jsonb",
            OidXml => "xml",
            OidInet => "inet",
            OidCidr => "cidr",
            OidMacaddr => "macaddr",
            OidMacaddr8 => "macaddr8",
            OidTsVector => "tsvector",
            OidTsQuery => "tsquery",
            OidInt4Range => "int4range",
            OidInt8Range => "int8range",
            OidNumRange => "numrange",
            OidTsRange => "tsrange",
            OidTstzRange => "tstzrange",
            OidDateRange => "daterange",
            OidSbVector => "vector",
            _ => "unknown"
        };
    }

    public static Type GetClrType(uint typeOid)
    {
        return typeOid switch
        {
            OidBool => typeof(bool),
            OidInt2 => typeof(short),
            OidInt4 => typeof(int),
            OidInt8 => typeof(long),
            OidFloat4 => typeof(float),
            OidFloat8 => typeof(double),
            OidNumeric => typeof(decimal),
            OidMoney => typeof(decimal),
            OidText => typeof(string),
            OidVarchar => typeof(string),
            OidChar => typeof(string),
            OidBpchar => typeof(string),
            OidBytea => typeof(byte[]),
            OidDate => typeof(DateOnly),
            OidTime => typeof(TimeOnly),
            OidTimetz => typeof(ScratchBirdTimeTz),
            OidTimestamp => typeof(DateTime),
            OidTimestamptz => typeof(DateTimeOffset),
            OidInterval => typeof(ScratchBirdInterval),
            OidUuid => typeof(Guid),
            OidJson => typeof(string),
            OidJsonb => typeof(ScratchBirdJsonb),
            OidXml => typeof(string),
            OidInet => typeof(string),
            OidCidr => typeof(string),
            OidMacaddr => typeof(string),
            OidMacaddr8 => typeof(string),
            OidTsVector => typeof(string),
            OidTsQuery => typeof(string),
            OidInt4Range => typeof(ScratchBirdRange<int>),
            OidInt8Range => typeof(ScratchBirdRange<long>),
            OidNumRange => typeof(ScratchBirdRange<object>),
            OidTsRange => typeof(ScratchBirdRange<DateTime>),
            OidTstzRange => typeof(ScratchBirdRange<DateTimeOffset>),
            OidDateRange => typeof(ScratchBirdRange<DateOnly>),
            OidSbVector => typeof(float[]),
            _ => typeof(object)
        };
    }

    private static object DecodeBinaryValue(uint typeOid, byte[] data)
    {
        switch (typeOid)
        {
            case OidBool:
            {
                if (TryDecodeBoolPayload(data, out var boolValue))
                {
                    return boolValue;
                }
                if (TryGetLengthPrefixedPayload(data, out var prefixedBool) &&
                    TryDecodeBoolPayload(prefixedBool, out boolValue))
                {
                    return boolValue;
                }
                throw new ArgumentOutOfRangeException(nameof(data), "BOOL payload is empty");
            }
            case OidInt2:
            {
                if (TryDecodeInt16Payload(data, out var int16Value))
                {
                    return int16Value;
                }
                if (TryGetLengthPrefixedPayload(data, out var prefixedInt16) &&
                    TryDecodeInt16Payload(prefixedInt16, out int16Value))
                {
                    return int16Value;
                }
                throw new ArgumentOutOfRangeException(nameof(data), "INT2 payload is empty");
            }
            case OidInt4:
            {
                if (TryDecodeInt32Payload(data, out var int32Value))
                {
                    return int32Value;
                }
                if (TryGetLengthPrefixedPayload(data, out var prefixedInt32) &&
                    TryDecodeInt32Payload(prefixedInt32, out int32Value))
                {
                    return int32Value;
                }
                throw new ArgumentOutOfRangeException(nameof(data), "INT4 payload is empty");
            }
            case OidInt8:
            {
                if (TryDecodeInt64Payload(data, out var int64Value))
                {
                    return int64Value;
                }
                if (TryGetLengthPrefixedPayload(data, out var prefixedInt64) &&
                    TryDecodeInt64Payload(prefixedInt64, out int64Value))
                {
                    return int64Value;
                }
                throw new ArgumentOutOfRangeException(nameof(data), "INT8 payload is empty");
            }
            case OidFloat4:
            {
                if (TryDecodeFloatPayload(data, out var floatValue))
                {
                    return floatValue;
                }
                if (TryGetLengthPrefixedPayload(data, out var prefixedFloat) &&
                    TryDecodeFloatPayload(prefixedFloat, out floatValue))
                {
                    return floatValue;
                }
                throw new ArgumentOutOfRangeException(nameof(data), "FLOAT4 payload is empty");
            }
            case OidFloat8:
            {
                if (TryDecodeDoublePayload(data, out var doubleValue))
                {
                    return doubleValue;
                }
                if (TryGetLengthPrefixedPayload(data, out var prefixedDouble) &&
                    TryDecodeDoublePayload(prefixedDouble, out doubleValue))
                {
                    return doubleValue;
                }
                throw new ArgumentOutOfRangeException(nameof(data), "FLOAT8 payload is empty");
            }
            case OidNumeric:
                return ParseDecimal(StripLengthPrefix(data));
            case OidMoney:
                return ReadInt64(data) / 100m;
            case OidText:
            case OidVarchar:
            case OidChar:
            case OidBpchar:
            case OidJson:
            case OidXml:
            case OidTsVector:
            case OidTsQuery:
                return Encoding.UTF8.GetString(StripLengthPrefix(data));
            case OidJsonb:
                return new ScratchBirdJsonb(StripLengthPrefix(data));
            case OidBytea:
                return StripLengthPrefix(data);
            case OidDate:
                return DecodeDate(data);
            case OidTime:
                return DecodeTime(data);
            case OidTimetz:
                return DecodeTimeTz(data);
            case OidTimestamp:
                return DecodeTimestamp(data).DateTime;
            case OidTimestamptz:
                return DecodeTimestamp(data);
            case OidInterval:
                return DecodeInterval(data);
            case OidUuid:
                return DecodeUuidBinary(data);
            case OidInet:
            case OidCidr:
            case OidMacaddr:
            case OidMacaddr8:
                return Encoding.UTF8.GetString(StripLengthPrefix(data));
            case OidInt4Range:
            case OidInt8Range:
            case OidNumRange:
            case OidTsRange:
            case OidTstzRange:
            case OidDateRange:
                return DecodeRange(typeOid, data);
            case OidSbVector:
                return ParseVectorLiteral(Encoding.UTF8.GetString(StripLengthPrefix(data)));
            case OidPoint:
            case OidLseg:
            case OidPath:
            case OidBox:
            case OidPolygon:
            case OidLine:
            case OidCircle:
                return new ScratchBirdGeometry(StripLengthPrefix(data));
            case OidRecord:
                return DecodeComposite(data);
            default:
                return data;
        }
    }

    private static object DecodeTextValue(byte[] data)
    {
        if (data.Length >= 4)
        {
            var length = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(0, 4));
            if (length <= data.Length - 4)
            {
                return Encoding.UTF8.GetString(data, 4, (int)length);
            }
        }
        return Encoding.UTF8.GetString(data);
    }

    private static object DecodeUnknownBinary(byte[] data)
    {
        var trimmed = StripTrailingNulls(data);
        if (trimmed.Length > 0 && LooksLikeText(trimmed))
        {
            return ParseUnknownText(Encoding.UTF8.GetString(trimmed));
        }
        return data.Length switch
        {
            1 => (int)data[0],
            2 => BinaryPrimitives.ReadInt16LittleEndian(data),
            4 => BinaryPrimitives.ReadInt32LittleEndian(data),
            8 => BinaryPrimitives.ReadInt64LittleEndian(data),
            16 => Guid.Parse(BytesToUuid(data)),
            _ => data
        };
    }

    private static object ParseUnknownText(string text)
    {
        var trimmed = text.Trim();
        if (trimmed.Length == 0)
        {
            return text;
        }
        if (string.Equals(trimmed, "true", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }
        if (string.Equals(trimmed, "false", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        if (long.TryParse(trimmed, out var intValue))
        {
            if (intValue >= int.MinValue && intValue <= int.MaxValue)
            {
                return (int)intValue;
            }
            return intValue;
        }
        if (double.TryParse(trimmed, out var floatValue))
        {
            return floatValue;
        }
        return text;
    }

    private static byte[] StripTrailingNulls(byte[] data)
    {
        var end = data.Length;
        while (end > 0 && data[end - 1] == 0)
        {
            end--;
        }
        if (end == data.Length)
        {
            return data;
        }
        var trimmed = new byte[end];
        Buffer.BlockCopy(data, 0, trimmed, 0, end);
        return trimmed;
    }

    private static bool LooksLikeText(byte[] data)
    {
        foreach (var b in data)
        {
            if (b == 0x09 || b == 0x0a || b == 0x0d)
            {
                continue;
            }
            if (b < 0x20 || b > 0x7e)
            {
                return false;
            }
        }
        return true;
    }

    private static byte[] EncodeLengthPrefixed(byte[] data)
    {
        var buffer = new byte[4 + data.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(0, 4), (uint)data.Length);
        Buffer.BlockCopy(data, 0, buffer, 4, data.Length);
        return buffer;
    }

    private static (byte[] Data, uint Oid) EncodeComposite(ScratchBirdComposite composite)
    {
        var fields = composite.Fields ?? Array.Empty<ScratchBirdCompositeField>();
        var typeOid = composite.TypeOid == 0 ? OidRecord : composite.TypeOid;
        var buffer = new List<byte>(4 + fields.Count * 12);
        buffer.AddRange(BitConverter.GetBytes(fields.Count));
        foreach (var field in fields)
        {
            var fieldOid = field.Oid;
            byte[]? data = null;
            if (field.Raw != null)
            {
                data = field.Raw;
            }
            else if (field.Value != null)
            {
                var encoded = EncodeParam(field.Value);
                if (fieldOid == 0)
                {
                    fieldOid = encoded.Oid;
                }
                data = encoded.Param.IsNull ? null : encoded.Param.Data;
            }

            if (fieldOid == 0)
            {
                throw new InvalidOperationException("Composite field OID is required");
            }
            buffer.AddRange(BitConverter.GetBytes(fieldOid));
            if (data == null)
            {
                buffer.AddRange(BitConverter.GetBytes(-1));
                continue;
            }
            buffer.AddRange(BitConverter.GetBytes(data.Length));
            buffer.AddRange(data);
        }
        return (buffer.ToArray(), typeOid);
    }

    private static ScratchBirdComposite DecodeComposite(byte[] data)
    {
        if (data.Length < 4)
        {
            return new ScratchBirdComposite(Array.Empty<ScratchBirdCompositeField>());
        }
        var count = BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(0, 4));
        var offset = 4;
        var fields = new List<ScratchBirdCompositeField>(count);
        for (var i = 0; i < count; i++)
        {
            if (offset + 8 > data.Length)
            {
                break;
            }
            var oid = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(offset, 4));
            offset += 4;
            var length = BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(offset, 4));
            offset += 4;
            if (length < 0)
            {
                fields.Add(new ScratchBirdCompositeField(oid, null, null));
                continue;
            }
            if (offset + length > data.Length)
            {
                break;
            }
            var raw = data.Skip(offset).Take(length).ToArray();
            offset += length;
            var value = DecodeBinaryValue(oid, raw);
            fields.Add(new ScratchBirdCompositeField(oid, value, raw));
        }
        return new ScratchBirdComposite(fields, OidRecord);
    }

    private static byte[] StripLengthPrefix(byte[] data)
    {
        if (data.Length < 4)
        {
            return data;
        }
        var length = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(0, 4));
        if (length <= data.Length - 4)
        {
            return data.Skip(4).Take((int)length).ToArray();
        }
        return data;
    }

    private static byte[] EncodeInt16(short value)
    {
        var buffer = new byte[2];
        BinaryPrimitives.WriteInt16LittleEndian(buffer.AsSpan(0, 2), value);
        return buffer;
    }

    private static byte[] EncodeInt32(int value)
    {
        var buffer = new byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(0, 4), value);
        return buffer;
    }

    private static byte[] EncodeInt64(long value)
    {
        var buffer = new byte[8];
        BinaryPrimitives.WriteInt64LittleEndian(buffer.AsSpan(0, 8), value);
        return buffer;
    }

    private static byte[] EncodeFloat32(float value)
    {
        var buffer = new byte[4];
        BinaryPrimitives.WriteSingleLittleEndian(buffer.AsSpan(0, 4), value);
        return buffer;
    }

    private static byte[] EncodeFloat64(double value)
    {
        var buffer = new byte[8];
        BinaryPrimitives.WriteDoubleLittleEndian(buffer.AsSpan(0, 8), value);
        return buffer;
    }

    private static byte[] EncodeDate(DateOnly value)
    {
        var baseDate = new DateTime(2000, 1, 1, 0, 0, 0, DateTimeKind.Utc);
        var target = value.ToDateTime(TimeOnly.MinValue, DateTimeKind.Utc);
        var days = (int)(target - baseDate).TotalDays;
        return EncodeInt32(days);
    }

    private static byte[] EncodeTimeMicros(long micros)
    {
        return EncodeInt64(micros);
    }

    private static byte[] EncodeTimeTzMicros(long micros, int utcOffsetSeconds)
    {
        if (utcOffsetSeconds < -86400 || utcOffsetSeconds > 86400)
        {
            throw new InvalidOperationException("time with time zone offset must be between -86400 and 86400 seconds");
        }

        var payload = new byte[12];
        BinaryPrimitives.WriteInt64LittleEndian(payload.AsSpan(0, 8), micros);
        // Wire payload stores offset as seconds west of UTC.
        BinaryPrimitives.WriteInt32LittleEndian(payload.AsSpan(8, 4), -utcOffsetSeconds);
        return payload;
    }

    private static byte[] EncodeTimestamp(DateTime value)
    {
        var utc = value.Kind == DateTimeKind.Utc ? value : value.ToUniversalTime();
        var micros = (utc - Epoch2000.UtcDateTime).Ticks / 10;
        return EncodeInt64(micros);
    }

    private static byte[] EncodeInterval(long micros, int days, int months)
    {
        var buffer = new byte[16];
        BinaryPrimitives.WriteInt64LittleEndian(buffer.AsSpan(0, 8), micros);
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(8, 4), days);
        BinaryPrimitives.WriteInt32LittleEndian(buffer.AsSpan(12, 4), months);
        return buffer;
    }

    private static DateOnly DecodeDate(byte[] data)
    {
        if (data.Length < 4)
        {
            return DateOnly.FromDateTime(Epoch2000.UtcDateTime);
        }
        var days = ReadInt32(data);
        return DateOnly.FromDateTime(Epoch2000.UtcDateTime.AddDays(days));
    }

    private static TimeOnly DecodeTime(byte[] data)
    {
        if (data.Length < 8)
        {
            return TimeOnly.MinValue;
        }
        var micros = ReadInt64(data);
        var ticks = micros * 10;
        return TimeOnly.FromTimeSpan(TimeSpan.FromTicks(ticks));
    }

    private static ScratchBirdTimeTz DecodeTimeTz(byte[] data)
    {
        var payload = StripLengthPrefix(data);
        if (payload.Length < 12)
        {
            return new ScratchBirdTimeTz(0, 0);
        }

        var micros = BinaryPrimitives.ReadInt64LittleEndian(payload.AsSpan(0, 8));
        var secondsWestOfUtc = BinaryPrimitives.ReadInt32LittleEndian(payload.AsSpan(8, 4));
        var utcOffsetSeconds = -secondsWestOfUtc;
        return new ScratchBirdTimeTz(micros, utcOffsetSeconds);
    }

    private static DateTimeOffset DecodeTimestamp(byte[] data)
    {
        if (data.Length < 8)
        {
            return Epoch2000;
        }
        var micros = ReadInt64(data);
        return Epoch2000.AddTicks(micros * 10);
    }

    private static ScratchBirdInterval DecodeInterval(byte[] data)
    {
        if (data.Length < 16)
        {
            return new ScratchBirdInterval(0, 0, 0);
        }
        var micros = BinaryPrimitives.ReadInt64LittleEndian(data.AsSpan(0, 8));
        var days = BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(8, 4));
        var months = BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(12, 4));
        return new ScratchBirdInterval(micros, days, months);
    }

    private static short ReadInt16(byte[] data)
    {
        return BinaryPrimitives.ReadInt16LittleEndian(data.AsSpan(0, 2));
    }

    private static int ReadInt32(byte[] data)
    {
        return BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(0, 4));
    }

    private static long ReadInt64(byte[] data)
    {
        return BinaryPrimitives.ReadInt64LittleEndian(data.AsSpan(0, 8));
    }

    private static float ReadFloat(byte[] data)
    {
        return BinaryPrimitives.ReadSingleLittleEndian(data.AsSpan(0, 4));
    }

    private static double ReadDouble(byte[] data)
    {
        return BinaryPrimitives.ReadDoubleLittleEndian(data.AsSpan(0, 8));
    }

    private static string BytesToUuid(byte[] data)
    {
        var hex = Convert.ToHexString(data).ToLowerInvariant();
        if (hex.Length != 32)
        {
            return hex;
        }
        return $"{hex[..8]}-{hex.Substring(8, 4)}-{hex.Substring(12, 4)}-{hex.Substring(16, 4)}-{hex.Substring(20)}";
    }

    private static Guid DecodeUuidBinary(byte[] data)
    {
        var payload = StripLengthPrefix(data);
        if (payload.Length == 16)
        {
            return Guid.Parse(BytesToUuid(payload));
        }

        if (payload.Length > 0 && LooksLikeText(payload))
        {
            var text = Encoding.UTF8.GetString(payload).TrimEnd('\0');
            if (Guid.TryParse(text, out var parsedText))
            {
                return parsedText;
            }
        }

        if (Guid.TryParse(BytesToUuid(payload), out var parsed))
        {
            return parsed;
        }

        throw new FormatException($"Invalid UUID binary payload length {payload.Length}.");
    }

    private static bool TryParseIntegralText(byte[] data, out long value)
    {
        value = 0;
        var trimmed = StripTrailingNulls(data);
        if (trimmed.Length == 0 || !LooksLikeText(trimmed))
        {
            return false;
        }

        var text = Encoding.UTF8.GetString(trimmed).Trim();
        return long.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out value);
    }

    private static bool TryParseFloatingText(byte[] data, out double value)
    {
        value = 0;
        var trimmed = StripTrailingNulls(data);
        if (trimmed.Length == 0 || !LooksLikeText(trimmed))
        {
            return false;
        }

        var text = Encoding.UTF8.GetString(trimmed).Trim();
        return double.TryParse(text, NumberStyles.Float | NumberStyles.AllowThousands, CultureInfo.InvariantCulture, out value);
    }

    private static bool TryParseBooleanText(byte[] data, out bool value)
    {
        value = false;
        var trimmed = StripTrailingNulls(data);
        if (trimmed.Length == 0 || !LooksLikeText(trimmed))
        {
            return false;
        }

        var text = Encoding.UTF8.GetString(trimmed).Trim();
        if (string.Equals(text, "true", StringComparison.OrdinalIgnoreCase) || text == "1" || string.Equals(text, "t", StringComparison.OrdinalIgnoreCase))
        {
            value = true;
            return true;
        }
        if (string.Equals(text, "false", StringComparison.OrdinalIgnoreCase) || text == "0" || string.Equals(text, "f", StringComparison.OrdinalIgnoreCase))
        {
            value = false;
            return true;
        }

        return false;
    }

    private static bool TryDecodeBoolPayload(byte[] payload, out bool value)
    {
        if (payload.Length > 0 && (payload[0] == 0 || payload[0] == 1))
        {
            value = payload[0] == 1;
            return true;
        }
        return TryParseBooleanText(payload, out value);
    }

    private static bool TryDecodeInt16Payload(byte[] payload, out short value)
    {
        value = 0;
        if (TryParseIntegralText(payload, out var parsedInt16) &&
            parsedInt16 >= short.MinValue && parsedInt16 <= short.MaxValue)
        {
            value = (short)parsedInt16;
            return true;
        }

        if (payload.Length == 2)
        {
            value = ReadInt16(payload);
            return true;
        }
        if (payload.Length == 1)
        {
            value = (short)(sbyte)payload[0];
            return true;
        }
        return false;
    }

    private static bool TryDecodeInt32Payload(byte[] payload, out int value)
    {
        value = 0;
        if (TryParseIntegralText(payload, out var parsedInt32) &&
            parsedInt32 >= int.MinValue && parsedInt32 <= int.MaxValue)
        {
            value = (int)parsedInt32;
            return true;
        }

        if (payload.Length == 4)
        {
            value = ReadInt32(payload);
            return true;
        }
        if (payload.Length == 2)
        {
            value = ReadInt16(payload);
            return true;
        }
        if (payload.Length == 1)
        {
            value = (sbyte)payload[0];
            return true;
        }
        return false;
    }

    private static bool TryDecodeInt64Payload(byte[] payload, out long value)
    {
        value = 0;
        if (TryParseIntegralText(payload, out var parsedInt64))
        {
            value = parsedInt64;
            return true;
        }

        if (payload.Length == 8)
        {
            value = ReadInt64(payload);
            return true;
        }
        if (payload.Length == 4)
        {
            value = ReadInt32(payload);
            return true;
        }
        if (payload.Length == 2)
        {
            value = ReadInt16(payload);
            return true;
        }
        if (payload.Length == 1)
        {
            value = (sbyte)payload[0];
            return true;
        }
        return false;
    }

    private static bool TryDecodeFloatPayload(byte[] payload, out float value)
    {
        value = 0;
        if (TryParseFloatingText(payload, out var parsedFloat))
        {
            value = (float)parsedFloat;
            return true;
        }

        if (payload.Length == 4)
        {
            value = ReadFloat(payload);
            return true;
        }
        return false;
    }

    private static bool TryDecodeDoublePayload(byte[] payload, out double value)
    {
        value = 0;
        if (TryParseFloatingText(payload, out var parsedDouble))
        {
            value = parsedDouble;
            return true;
        }

        if (payload.Length == 8)
        {
            value = ReadDouble(payload);
            return true;
        }
        if (payload.Length == 4)
        {
            value = ReadFloat(payload);
            return true;
        }
        return false;
    }

    private static bool TryGetLengthPrefixedPayload(byte[] data, out byte[] payload)
    {
        payload = Array.Empty<byte>();
        if (data.Length < 4)
        {
            return false;
        }

        var declaredLength = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(0, 4));
        if (declaredLength != data.Length - 4)
        {
            return false;
        }

        payload = data.AsSpan(4, (int)declaredLength).ToArray();
        return true;
    }

    private static byte[] GuidToBytes(Guid guid)
    {
        var text = guid.ToString("N");
        var buffer = new byte[16];
        for (var i = 0; i < 16; i++)
        {
            buffer[i] = Convert.ToByte(text.Substring(i * 2, 2), 16);
        }
        return buffer;
    }

    private static object ParseDecimal(byte[] data)
    {
        var text = Encoding.UTF8.GetString(data);
        if (decimal.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var value))
        {
            return value;
        }
        return text;
    }

    private static bool IsNumeric(object? value)
    {
        return value is sbyte or byte or short or ushort or int or uint or long or ulong or float or double or decimal;
    }

    private static string FormatArrayLiteral(IReadOnlyList<object?> values)
    {
        var items = values.Select(FormatArrayItem);
        return "{" + string.Join(",", items) + "}";
    }

    private static string FormatArrayItem(object? value)
    {
        if (value == null)
        {
            return "NULL";
        }
        if (value is IEnumerable enumerable && value is not string)
        {
            var nested = new List<object?>();
            foreach (var item in enumerable)
            {
                nested.Add(item);
            }
            return FormatArrayLiteral(nested);
        }
        if (value is string text)
        {
            return "\"" + text.Replace("\"", "\\\"") + "\"";
        }
        if (value is bool boolean)
        {
            return boolean ? "true" : "false";
        }
        return Convert.ToString(value, CultureInfo.InvariantCulture) ?? string.Empty;
    }

    private static object[] ParseArrayLiteral(string text)
    {
        var trimmed = text.Trim();
        if (string.IsNullOrEmpty(trimmed) || trimmed == "{}")
        {
            return Array.Empty<object>();
        }
        if (trimmed.StartsWith("{") && trimmed.EndsWith("}"))
        {
            trimmed = trimmed.Substring(1, trimmed.Length - 2);
        }
        return SplitArrayItems(trimmed).ToArray();
    }

    private static List<object?> SplitArrayItems(string text)
    {
        var items = new List<object?>();
        var depth = 0;
        var sb = new StringBuilder();
        foreach (var ch in text)
        {
            if (ch == '{')
            {
                depth++;
                sb.Append(ch);
            }
            else if (ch == '}')
            {
                depth = Math.Max(0, depth - 1);
                sb.Append(ch);
            }
            else if (ch == ',' && depth == 0)
            {
                items.Add(ParseArrayItem(sb.ToString()));
                sb.Clear();
            }
            else
            {
                sb.Append(ch);
            }
        }
        if (sb.Length > 0 || text.Length > 0)
        {
            items.Add(ParseArrayItem(sb.ToString()));
        }
        return items;
    }

    private static object? ParseArrayItem(string raw)
    {
        var token = raw.Trim();
        if (string.Equals(token, "NULL", StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }
        if (token.StartsWith("{") && token.EndsWith("}"))
        {
            return ParseArrayLiteral(token);
        }
        if (token.StartsWith("[") && token.EndsWith("]"))
        {
            return ParseVectorLiteral(token);
        }
        if (token == "true" || token == "false")
        {
            return token == "true";
        }
        if (double.TryParse(token, NumberStyles.Float, CultureInfo.InvariantCulture, out var numeric))
        {
            return numeric;
        }
        if (token.StartsWith("\"") && token.EndsWith("\"") && token.Length >= 2)
        {
            return token.Substring(1, token.Length - 2).Replace("\\\"", "\"");
        }
        return token;
    }

    private static float[] ParseVectorLiteral(string text)
    {
        var trimmed = text.Trim();
        if (trimmed.StartsWith("[") && trimmed.EndsWith("]"))
        {
            trimmed = trimmed.Substring(1, trimmed.Length - 2);
        }
        if (string.IsNullOrWhiteSpace(trimmed))
        {
            return Array.Empty<float>();
        }
        return trimmed.Split(',')
            .Select(part => float.TryParse(part.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out var value) ? value : 0f)
            .ToArray();
    }

    private static string FormatVectorLiteral(IEnumerable<double> values)
    {
        var parts = values.Select(value => double.IsFinite(value) ? value.ToString(CultureInfo.InvariantCulture) : "0");
        return "[" + string.Join(",", parts) + "]";
    }

    private static (byte[] Data, uint Oid) EncodeRange(ScratchBirdRange<object> range)
    {
        var oid = ResolveRangeOid(range);
        var flags = (byte)
            ((range.Empty ? RangeEmpty : 0) |
             (range.LowerInclusive ? RangeLowerInclusive : 0) |
             (range.UpperInclusive ? RangeUpperInclusive : 0) |
             (range.LowerInfinite ? RangeLowerInfinite : 0) |
             (range.UpperInfinite ? RangeUpperInfinite : 0));

        var parts = new List<byte[]> { new[] { flags, (byte)0, (byte)0, (byte)0 } };
        if (!range.Empty && !range.LowerInfinite)
        {
            var bound = EncodeRangeBound(oid, range.Lower);
            parts.Add(EncodeInt32(bound.Length));
            parts.Add(bound);
        }
        if (!range.Empty && !range.UpperInfinite)
        {
            var bound = EncodeRangeBound(oid, range.Upper);
            parts.Add(EncodeInt32(bound.Length));
            parts.Add(bound);
        }
        return (parts.SelectMany(part => part).ToArray(), oid);
    }

    private static object DecodeRange(uint oid, byte[] data)
    {
        return oid switch
        {
            OidInt4Range => DecodeRange(data, oid, bound => BinaryPrimitives.ReadInt32LittleEndian(bound.AsSpan(0, 4))),
            OidInt8Range => DecodeRange(data, oid, bound => BinaryPrimitives.ReadInt64LittleEndian(bound.AsSpan(0, 8))),
            OidNumRange => DecodeRange<object>(data, oid, bound => ParseDecimal(StripLengthPrefix(bound))),
            OidDateRange => DecodeRange(data, oid, DecodeDate),
            OidTsRange => DecodeRange(data, oid, bound => DecodeTimestamp(bound).DateTime),
            OidTstzRange => DecodeRange(data, oid, DecodeTimestamp),
            _ => new ScratchBirdRange<object>()
        };
    }

    private static ScratchBirdRange<T> DecodeRange<T>(byte[] data, uint oid, Func<byte[], T> decodeBound)
    {
        if (data.Length < 4)
        {
            return new ScratchBirdRange<T> { RangeOid = oid };
        }
        var flags = data[0];
        var offset = 4;
        var range = new ScratchBirdRange<T>
        {
            Empty = (flags & RangeEmpty) != 0,
            LowerInclusive = (flags & RangeLowerInclusive) != 0,
            UpperInclusive = (flags & RangeUpperInclusive) != 0,
            LowerInfinite = (flags & RangeLowerInfinite) != 0,
            UpperInfinite = (flags & RangeUpperInfinite) != 0,
            RangeOid = oid
        };
        if (range.Empty)
        {
            return range;
        }
        if (!range.LowerInfinite)
        {
            if (offset + 4 > data.Length)
            {
                return range;
            }
            var length = BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(offset, 4));
            offset += 4;
            if (offset + length > data.Length)
            {
                return range;
            }
            var bound = data.AsSpan(offset, length).ToArray();
            offset += length;
            range.Lower = decodeBound(bound);
        }
        if (!range.UpperInfinite)
        {
            if (offset + 4 > data.Length)
            {
                return range;
            }
            var length = BinaryPrimitives.ReadInt32LittleEndian(data.AsSpan(offset, 4));
            offset += 4;
            if (offset + length > data.Length)
            {
                return range;
            }
            var bound = data.AsSpan(offset, length).ToArray();
            offset += length;
            range.Upper = decodeBound(bound);
        }
        return range;
    }

    private static uint ResolveRangeOid(ScratchBirdRange<object> range)
    {
        if (range.RangeOid.HasValue)
        {
            return range.RangeOid.Value;
        }
        var sample = range.Lower ?? range.Upper;
        if (sample == null)
        {
            throw new InvalidOperationException("range type cannot be inferred from empty bounds");
        }
        return sample switch
        {
            ScratchBirdDate or DateOnly => OidDateRange,
            ScratchBirdTimestamp => OidTsRange,
            ScratchBirdTimestampTz or DateTimeOffset => OidTstzRange,
            DateTime => OidTsRange,
            ScratchBirdDecimal or decimal or string => OidNumRange,
            long or ulong => OidInt8Range,
            int or short => OidInt4Range,
            _ => throw new InvalidOperationException("unsupported range bound type")
        };
    }

    private static byte[] EncodeRangeBound(uint rangeOid, object? value)
    {
        switch (rangeOid)
        {
            case OidInt4Range:
                return EncodeInt32(Convert.ToInt32(value, CultureInfo.InvariantCulture));
            case OidInt8Range:
                return EncodeInt64(Convert.ToInt64(value, CultureInfo.InvariantCulture));
            case OidNumRange:
                return EncodeLengthPrefixed(Encoding.UTF8.GetBytes(Convert.ToString(value, CultureInfo.InvariantCulture) ?? "0"));
            case OidDateRange:
            {
                if (value is ScratchBirdDate date)
                {
                    return EncodeDate(date.Value);
                }
                if (value is DateOnly dateOnly)
                {
                    return EncodeDate(dateOnly);
                }
                throw new InvalidOperationException("daterange requires date bounds");
            }
            case OidTsRange:
            {
                if (value is ScratchBirdTimestamp ts)
                {
                    return EncodeTimestamp(ts.Value);
                }
                if (value is DateTime dt)
                {
                    return EncodeTimestamp(dt);
                }
                throw new InvalidOperationException("tsrange requires timestamp bounds");
            }
            case OidTstzRange:
            {
                if (value is ScratchBirdTimestampTz tstz)
                {
                    return EncodeTimestamp(tstz.Value.UtcDateTime);
                }
                if (value is DateTimeOffset dto)
                {
                    return EncodeTimestamp(dto.UtcDateTime);
                }
                throw new InvalidOperationException("tstzrange requires timestamptz bounds");
            }
            default:
                throw new InvalidOperationException("unsupported range type");
        }
    }

    private static ScratchBirdRange<object> CastRange<T>(ScratchBirdRange<T> range)
    {
        return new ScratchBirdRange<object>
        {
            Lower = range.Lower,
            Upper = range.Upper,
            LowerInclusive = range.LowerInclusive,
            UpperInclusive = range.UpperInclusive,
            LowerInfinite = range.LowerInfinite,
            UpperInfinite = range.UpperInfinite,
            Empty = range.Empty,
            RangeOid = range.RangeOid
        };
    }

    private static uint DbTypeToOid(DbType? dbType)
    {
        if (!dbType.HasValue)
        {
            return 0;
        }
        return dbType.Value switch
        {
            DbType.Boolean => OidBool,
            DbType.Int16 => OidInt2,
            DbType.Int32 => OidInt4,
            DbType.Int64 => OidInt8,
            DbType.Single => OidFloat4,
            DbType.Double => OidFloat8,
            DbType.Decimal => OidNumeric,
            DbType.Currency => OidMoney,
            DbType.String => OidText,
            DbType.AnsiString => OidText,
            DbType.StringFixedLength => OidChar,
            DbType.AnsiStringFixedLength => OidChar,
            DbType.Binary => OidBytea,
            DbType.Date => OidDate,
            DbType.Time => OidTime,
            DbType.DateTime => OidTimestamp,
            DbType.DateTimeOffset => OidTimestamptz,
            DbType.Guid => OidUuid,
            DbType.Xml => OidXml,
            _ => 0
        };
    }
}
