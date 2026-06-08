// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

use std::fmt;
use std::str::FromStr;

use bigdecimal::BigDecimal;
use chrono::{DateTime, NaiveDate, NaiveTime, TimeZone, Timelike, Utc};
use serde_json::Value as JsonValue;

use crate::errors::{Error, ErrorKind, Result};
use crate::protocol::ParamValue;

pub const FORMAT_TEXT: u16 = 0;
pub const FORMAT_BINARY: u16 = 1;

pub const OID_BOOL: u32 = 16;
pub const OID_BYTEA: u32 = 17;
pub const OID_CHAR: u32 = 18;
pub const OID_INT8: u32 = 20;
pub const OID_INT2: u32 = 21;
pub const OID_INT4: u32 = 23;
pub const OID_TEXT: u32 = 25;
pub const OID_JSON: u32 = 114;
pub const OID_XML: u32 = 142;
pub const OID_POINT: u32 = 600;
pub const OID_LSEG: u32 = 601;
pub const OID_PATH: u32 = 602;
pub const OID_BOX: u32 = 603;
pub const OID_POLYGON: u32 = 604;
pub const OID_LINE: u32 = 628;
pub const OID_FLOAT4: u32 = 700;
pub const OID_FLOAT8: u32 = 701;
pub const OID_CIRCLE: u32 = 718;
pub const OID_MONEY: u32 = 790;
pub const OID_MACADDR: u32 = 829;
pub const OID_CIDR: u32 = 650;
pub const OID_INET: u32 = 869;
pub const OID_MACADDR8: u32 = 774;
pub const OID_BPCHAR: u32 = 1042;
pub const OID_VARCHAR: u32 = 1043;
pub const OID_DATE: u32 = 1082;
pub const OID_TIME: u32 = 1083;
pub const OID_TIMESTAMP: u32 = 1114;
pub const OID_TIMESTAMPTZ: u32 = 1184;
pub const OID_INTERVAL: u32 = 1186;
pub const OID_TIMETZ: u32 = 1266;
pub const OID_NUMERIC: u32 = 1700;
pub const OID_UUID: u32 = 2950;
pub const OID_JSONB: u32 = 3802;
pub const OID_RECORD: u32 = 2249;
pub const OID_INT4RANGE: u32 = 3904;
pub const OID_NUMRANGE: u32 = 3906;
pub const OID_TSRANGE: u32 = 3908;
pub const OID_TSTZRANGE: u32 = 3910;
pub const OID_DATERANGE: u32 = 3912;
pub const OID_INT8RANGE: u32 = 3926;
pub const OID_TSVECTOR: u32 = 3614;
pub const OID_TSQUERY: u32 = 3615;
pub const OID_SB_VECTOR: u32 = 16386;

const RANGE_EMPTY: u8 = 0x01;
const RANGE_LB_INC: u8 = 0x02;
const RANGE_UB_INC: u8 = 0x04;
const RANGE_LB_INF: u8 = 0x08;
const RANGE_UB_INF: u8 = 0x10;

#[derive(Debug, Clone)]
pub struct Column {
    pub name: String,
    pub type_oid: u32,
    pub type_modifier: i32,
    pub format: u8,
    pub nullable: bool,
}

#[derive(Debug, Clone)]
pub struct Jsonb {
    pub raw: Vec<u8>,
    pub value: Option<JsonValue>,
}

#[derive(Debug, Clone)]
pub struct Json {
    pub raw: Vec<u8>,
    pub value: Option<JsonValue>,
}

#[derive(Debug, Clone)]
pub struct Geometry {
    pub wkb: Vec<u8>,
    pub srid: Option<u32>,
    pub wkt: Option<String>,
}

#[derive(Debug, Clone)]
pub struct Interval {
    pub micros: i64,
    pub days: i32,
    pub months: i32,
}

#[derive(Debug, Clone)]
pub struct Date {
    pub value: NaiveDate,
}

#[derive(Debug, Clone)]
pub struct Time {
    pub micros: i64,
}

#[derive(Debug, Clone)]
pub struct Timestamp {
    pub value: DateTime<Utc>,
}

#[derive(Debug, Clone)]
pub struct TimestampTz {
    pub value: DateTime<Utc>,
}

#[derive(Debug, Clone)]
pub struct Decimal {
    pub value: String,
}

#[derive(Debug, Clone)]
pub struct Money {
    pub cents: i64,
}

#[derive(Debug, Clone)]
pub struct RawValue {
    pub oid: u32,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct CompositeField {
    pub oid: u32,
    pub value: Option<Value>,
    pub raw: Option<Vec<u8>>,
}

#[derive(Debug, Clone)]
pub struct Composite {
    pub type_oid: u32,
    pub fields: Vec<CompositeField>,
}

#[derive(Debug, Clone)]
pub struct Range<T> {
    pub lower: Option<T>,
    pub upper: Option<T>,
    pub lower_inclusive: bool,
    pub upper_inclusive: bool,
    pub lower_infinite: bool,
    pub upper_infinite: bool,
    pub empty: bool,
    pub range_oid: Option<u32>,
}

impl<T> Range<T> {
    pub fn new() -> Self {
        Self {
            lower: None,
            upper: None,
            lower_inclusive: false,
            upper_inclusive: false,
            lower_infinite: false,
            upper_infinite: false,
            empty: false,
            range_oid: None,
        }
    }
}

#[derive(Debug, Clone)]
pub enum RangeValue {
    Int32(i32),
    Int64(i64),
    Decimal(Decimal),
    Date(Date),
    Timestamp(Timestamp),
    TimestampTz(TimestampTz),
}

#[derive(Debug, Clone)]
pub enum Param {
    Null,
    Bool(bool),
    Int16(i16),
    Int32(i32),
    Int64(i64),
    Float32(f32),
    Float64(f64),
    Decimal(BigDecimal),
    String(String),
    Bytes(Vec<u8>),
    Date(NaiveDate),
    Time(NaiveTime),
    Timestamp(DateTime<Utc>),
    TimestampTz(DateTime<Utc>),
    Interval(Interval),
    Uuid(String),
    Json(JsonValue),
    Jsonb(Jsonb),
    Array(Vec<Param>),
    Vector(Vec<f32>),
    Inet(String),
    Cidr(String),
    Macaddr(String),
    Geometry(Geometry),
    Range(Range<RangeValue>),
    Raw(RawValue),
    Money(i64),
    Composite(Composite),
}

#[derive(Debug, Clone)]
pub enum Value {
    Null,
    Bool(bool),
    Int16(i16),
    Int32(i32),
    Int64(i64),
    Float32(f32),
    Float64(f64),
    Decimal(BigDecimal),
    String(String),
    Bytes(Vec<u8>),
    Date(NaiveDate),
    Time(NaiveTime),
    Timestamp(DateTime<Utc>),
    Interval(Interval),
    Uuid(String),
    Json(JsonValue),
    Jsonb(Jsonb),
    Array(Vec<Value>),
    Vector(Vec<f32>),
    Range(Range<RangeValue>),
    Geometry(Geometry),
    Composite(Composite),
}

impl From<bool> for Param {
    fn from(value: bool) -> Self {
        Param::Bool(value)
    }
}

impl From<i16> for Param {
    fn from(value: i16) -> Self {
        Param::Int16(value)
    }
}

impl From<i32> for Param {
    fn from(value: i32) -> Self {
        Param::Int32(value)
    }
}

impl From<i64> for Param {
    fn from(value: i64) -> Self {
        Param::Int64(value)
    }
}

impl From<f32> for Param {
    fn from(value: f32) -> Self {
        Param::Float32(value)
    }
}

impl From<f64> for Param {
    fn from(value: f64) -> Self {
        Param::Float64(value)
    }
}

impl From<&str> for Param {
    fn from(value: &str) -> Self {
        Param::String(value.to_string())
    }
}

impl From<String> for Param {
    fn from(value: String) -> Self {
        Param::String(value)
    }
}

impl From<Vec<u8>> for Param {
    fn from(value: Vec<u8>) -> Self {
        Param::Bytes(value)
    }
}

impl From<&[u8]> for Param {
    fn from(value: &[u8]) -> Self {
        Param::Bytes(value.to_vec())
    }
}

impl From<NaiveDate> for Param {
    fn from(value: NaiveDate) -> Self {
        Param::Date(value)
    }
}

impl From<NaiveTime> for Param {
    fn from(value: NaiveTime) -> Self {
        Param::Time(value)
    }
}

impl From<DateTime<Utc>> for Param {
    fn from(value: DateTime<Utc>) -> Self {
        Param::TimestampTz(value)
    }
}

impl From<BigDecimal> for Param {
    fn from(value: BigDecimal) -> Self {
        Param::Decimal(value)
    }
}

impl From<JsonValue> for Param {
    fn from(value: JsonValue) -> Self {
        Param::Json(value)
    }
}

impl From<Jsonb> for Param {
    fn from(value: Jsonb) -> Self {
        Param::Jsonb(value)
    }
}

impl From<Geometry> for Param {
    fn from(value: Geometry) -> Self {
        Param::Geometry(value)
    }
}

impl From<Money> for Param {
    fn from(value: Money) -> Self {
        Param::Money(value.cents)
    }
}

impl From<Range<i32>> for Param {
    fn from(value: Range<i32>) -> Self {
        let range = map_range(value, RangeValue::Int32, OID_INT4RANGE);
        Param::Range(range)
    }
}

impl From<Range<i64>> for Param {
    fn from(value: Range<i64>) -> Self {
        let range = map_range(value, RangeValue::Int64, OID_INT8RANGE);
        Param::Range(range)
    }
}

impl From<Range<BigDecimal>> for Param {
    fn from(value: Range<BigDecimal>) -> Self {
        let range = map_range(
            value,
            |v| {
                RangeValue::Decimal(Decimal {
                    value: v.to_string(),
                })
            },
            OID_NUMRANGE,
        );
        Param::Range(range)
    }
}

impl From<Range<NaiveDate>> for Param {
    fn from(value: Range<NaiveDate>) -> Self {
        let range = map_range(
            value,
            |v| RangeValue::Date(Date { value: v }),
            OID_DATERANGE,
        );
        Param::Range(range)
    }
}

impl From<Range<Timestamp>> for Param {
    fn from(value: Range<Timestamp>) -> Self {
        let range = map_range(value, RangeValue::Timestamp, OID_TSRANGE);
        Param::Range(range)
    }
}

impl From<Range<TimestampTz>> for Param {
    fn from(value: Range<TimestampTz>) -> Self {
        let range = map_range(value, RangeValue::TimestampTz, OID_TSTZRANGE);
        Param::Range(range)
    }
}

fn map_range<T, F>(range: Range<T>, mapper: F, default_oid: u32) -> Range<RangeValue>
where
    F: Fn(T) -> RangeValue,
{
    Range {
        lower: range.lower.map(&mapper),
        upper: range.upper.map(&mapper),
        lower_inclusive: range.lower_inclusive,
        upper_inclusive: range.upper_inclusive,
        lower_infinite: range.lower_infinite,
        upper_infinite: range.upper_infinite,
        empty: range.empty,
        range_oid: range.range_oid.or(Some(default_oid)),
    }
}

pub fn encode_param(param: &Param) -> Result<(ParamValue, u32)> {
    match param {
        Param::Null => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: None,
            },
            0,
        )),
        Param::Bool(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(vec![if *value { 1 } else { 0 }]),
            },
            OID_BOOL,
        )),
        Param::Int16(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(value.to_le_bytes().to_vec()),
            },
            OID_INT2,
        )),
        Param::Int32(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(value.to_le_bytes().to_vec()),
            },
            OID_INT4,
        )),
        Param::Int64(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(value.to_le_bytes().to_vec()),
            },
            OID_INT8,
        )),
        Param::Float32(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(value.to_le_bytes().to_vec()),
            },
            OID_FLOAT4,
        )),
        Param::Float64(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(value.to_le_bytes().to_vec()),
            },
            OID_FLOAT8,
        )),
        Param::Decimal(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_length_prefixed(value.to_string().as_bytes())),
            },
            OID_NUMERIC,
        )),
        Param::Money(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(value.to_le_bytes().to_vec()),
            },
            OID_MONEY,
        )),
        Param::String(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_length_prefixed(value.as_bytes())),
            },
            OID_TEXT,
        )),
        Param::Bytes(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_length_prefixed(value)),
            },
            OID_BYTEA,
        )),
        Param::Date(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_date(*value)),
            },
            OID_DATE,
        )),
        Param::Time(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_time(*value)),
            },
            OID_TIME,
        )),
        Param::Timestamp(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_timestamp(*value)),
            },
            OID_TIMESTAMP,
        )),
        Param::TimestampTz(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_timestamp(*value)),
            },
            OID_TIMESTAMPTZ,
        )),
        Param::Interval(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_interval(value)),
            },
            OID_INTERVAL,
        )),
        Param::Uuid(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(uuid_to_bytes(value)),
            },
            OID_UUID,
        )),
        Param::Json(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_length_prefixed(value.to_string().as_bytes())),
            },
            OID_JSON,
        )),
        Param::Jsonb(value) => {
            let raw = if value.raw.is_empty() {
                if let Some(ref parsed) = value.value {
                    serde_json::to_vec(parsed)
                        .map_err(|e| Error::new(ErrorKind::Data, e.to_string()))?
                } else {
                    return Err(Error::new(ErrorKind::Data, "JSONB requires raw payload"));
                }
            } else {
                value.raw.clone()
            };
            Ok((
                ParamValue {
                    format: FORMAT_BINARY,
                    data: Some(encode_length_prefixed(&raw)),
                },
                OID_JSONB,
            ))
        }
        Param::Array(values) => {
            let text = format_array_literal(values);
            Ok((
                ParamValue {
                    format: FORMAT_BINARY,
                    data: Some(encode_length_prefixed(text.as_bytes())),
                },
                0,
            ))
        }
        Param::Vector(values) => {
            let text = format_vector_literal(values);
            Ok((
                ParamValue {
                    format: FORMAT_BINARY,
                    data: Some(encode_length_prefixed(text.as_bytes())),
                },
                OID_SB_VECTOR,
            ))
        }
        Param::Inet(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_length_prefixed(value.as_bytes())),
            },
            OID_INET,
        )),
        Param::Cidr(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_length_prefixed(value.as_bytes())),
            },
            OID_CIDR,
        )),
        Param::Macaddr(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(encode_length_prefixed(value.as_bytes())),
            },
            OID_MACADDR,
        )),
        Param::Geometry(value) => {
            if value.wkb.is_empty() {
                return Err(Error::new(ErrorKind::Data, "geometry requires WKB payload"));
            }
            Ok((
                ParamValue {
                    format: FORMAT_BINARY,
                    data: Some(encode_length_prefixed(&value.wkb)),
                },
                OID_POINT,
            ))
        }
        Param::Range(range) => {
            let (data, oid) = encode_range(range)?;
            Ok((
                ParamValue {
                    format: FORMAT_BINARY,
                    data: Some(data),
                },
                oid,
            ))
        }
        Param::Raw(value) => Ok((
            ParamValue {
                format: FORMAT_BINARY,
                data: Some(value.data.clone()),
            },
            value.oid,
        )),
        Param::Composite(value) => {
            let (data, oid) = encode_composite(value)?;
            Ok((
                ParamValue {
                    format: FORMAT_BINARY,
                    data: Some(data),
                },
                oid,
            ))
        }
    }
}

pub fn decode_value(type_oid: u32, data: Option<Vec<u8>>, format: u16) -> Result<Value> {
    let Some(data) = data else {
        return Ok(Value::Null);
    };
    if type_oid == 0 {
        if format == FORMAT_TEXT {
            return Ok(parse_unknown_text(&decode_text_value(&data)));
        }
        return Ok(decode_unknown_binary(&data));
    }
    if format == FORMAT_TEXT {
        return Ok(Value::String(decode_text_value(&data)));
    }
    decode_binary_value(type_oid, &data)
}

fn decode_binary_value(type_oid: u32, data: &[u8]) -> Result<Value> {
    match type_oid {
        OID_BOOL => Ok(Value::Bool(data.get(0).copied().unwrap_or(0) == 1)),
        OID_INT2 => Ok(Value::Int16(i16::from_le_bytes(read_fixed::<2>(data)))),
        OID_INT4 => Ok(Value::Int32(i32::from_le_bytes(read_fixed::<4>(data)))),
        OID_INT8 => Ok(Value::Int64(i64::from_le_bytes(read_fixed::<8>(data)))),
        OID_FLOAT4 => Ok(Value::Float32(f32::from_le_bytes(read_fixed::<4>(data)))),
        OID_FLOAT8 => Ok(Value::Float64(f64::from_le_bytes(read_fixed::<8>(data)))),
        OID_NUMERIC => {
            let text = String::from_utf8_lossy(strip_length_prefix(data)).to_string();
            let dec = BigDecimal::from_str(&text).unwrap_or_else(|_| BigDecimal::from(0));
            Ok(Value::Decimal(dec))
        }
        OID_MONEY => {
            let cents = i64::from_le_bytes(read_fixed::<8>(data));
            let mut dec = BigDecimal::from(cents);
            dec /= 100;
            Ok(Value::Decimal(dec))
        }
        OID_TEXT | OID_VARCHAR | OID_CHAR | OID_BPCHAR | OID_JSON | OID_XML | OID_TSVECTOR
        | OID_TSQUERY => Ok(Value::String(
            String::from_utf8_lossy(strip_length_prefix(data)).to_string(),
        )),
        OID_JSONB => Ok(Value::Jsonb(Jsonb {
            raw: strip_length_prefix(data).to_vec(),
            value: None,
        })),
        OID_BYTEA => Ok(Value::Bytes(strip_length_prefix(data).to_vec())),
        OID_DATE => Ok(Value::Date(decode_date(data))),
        OID_TIME => Ok(Value::Time(decode_time(data))),
        OID_TIMESTAMP => Ok(Value::Timestamp(decode_timestamp(data))),
        OID_TIMESTAMPTZ => Ok(Value::Timestamp(decode_timestamp(data))),
        OID_INTERVAL => Ok(Value::Interval(decode_interval(data))),
        OID_UUID => Ok(Value::Uuid(bytes_to_uuid(data))),
        OID_INET | OID_CIDR | OID_MACADDR | OID_MACADDR8 => Ok(Value::String(
            String::from_utf8_lossy(strip_length_prefix(data)).to_string(),
        )),
        OID_INT4RANGE | OID_INT8RANGE | OID_NUMRANGE | OID_TSRANGE | OID_TSTZRANGE
        | OID_DATERANGE => Ok(Value::Range(decode_range(type_oid, data)?)),
        OID_SB_VECTOR => Ok(Value::Vector(parse_vector_literal(
            &String::from_utf8_lossy(strip_length_prefix(data)),
        ))),
        OID_POINT | OID_LSEG | OID_PATH | OID_BOX | OID_POLYGON | OID_LINE | OID_CIRCLE => {
            Ok(Value::Geometry(Geometry {
                wkb: strip_length_prefix(data).to_vec(),
                srid: None,
                wkt: None,
            }))
        }
        OID_RECORD => Ok(Value::Composite(decode_composite(data)?)),
        _ => Ok(Value::Bytes(data.to_vec())),
    }
}

fn decode_text_value(data: &[u8]) -> String {
    if data.len() >= 4 {
        let length = u32::from_le_bytes(read_fixed::<4>(data)) as usize;
        if length <= data.len().saturating_sub(4) {
            return String::from_utf8_lossy(&data[4..4 + length]).to_string();
        }
    }
    String::from_utf8_lossy(data).to_string()
}

fn decode_unknown_binary(data: &[u8]) -> Value {
    let trimmed = strip_trailing_nulls(data);
    if !trimmed.is_empty() && looks_like_text(trimmed) {
        return parse_unknown_text(&String::from_utf8_lossy(trimmed));
    }
    match data.len() {
        1 => Value::Int16(i16::from_le_bytes(read_fixed::<2>(&[data[0], 0]))),
        2 => Value::Int16(i16::from_le_bytes(read_fixed::<2>(data))),
        4 => Value::Int32(i32::from_le_bytes(read_fixed::<4>(data))),
        8 => Value::Int64(i64::from_le_bytes(read_fixed::<8>(data))),
        16 => Value::Uuid(bytes_to_uuid(data)),
        _ => Value::Bytes(data.to_vec()),
    }
}

fn parse_unknown_text(text: &str) -> Value {
    let trimmed = text.trim();
    if trimmed.is_empty() {
        return Value::String(text.to_string());
    }
    let lowered = trimmed.to_ascii_lowercase();
    if lowered == "true" {
        return Value::Bool(true);
    }
    if lowered == "false" {
        return Value::Bool(false);
    }
    if let Ok(value) = trimmed.parse::<i64>() {
        if value >= i32::MIN as i64 && value <= i32::MAX as i64 {
            return Value::Int32(value as i32);
        }
        return Value::Int64(value);
    }
    if let Ok(value) = trimmed.parse::<f64>() {
        return Value::Float64(value);
    }
    Value::String(text.to_string())
}

fn strip_trailing_nulls(data: &[u8]) -> &[u8] {
    let mut end = data.len();
    while end > 0 && data[end - 1] == 0 {
        end -= 1;
    }
    &data[..end]
}

fn looks_like_text(data: &[u8]) -> bool {
    for &byte in data {
        if byte == 0x09 || byte == 0x0a || byte == 0x0d {
            continue;
        }
        if byte < 0x20 || byte > 0x7e {
            return false;
        }
    }
    true
}

fn encode_composite(value: &Composite) -> Result<(Vec<u8>, u32)> {
    let type_oid = if value.type_oid != 0 {
        value.type_oid
    } else {
        OID_RECORD
    };
    let mut buf = Vec::new();
    buf.extend_from_slice(&(value.fields.len() as i32).to_le_bytes());
    for field in &value.fields {
        let mut field_oid = field.oid;
        let mut data: Option<Vec<u8>> = None;
        if let Some(raw) = &field.raw {
            data = Some(raw.clone());
        } else if let Some(val) = &field.value {
            let param = value_to_param(val)?;
            let (encoded, oid) = encode_param(&param)?;
            if field_oid == 0 {
                field_oid = oid;
            }
            data = encoded.data;
        }

        if field_oid == 0 {
            return Err(Error::new(
                ErrorKind::Data,
                "composite field OID is required",
            ));
        }
        buf.extend_from_slice(&field_oid.to_le_bytes());
        match data {
            Some(bytes) => {
                buf.extend_from_slice(&(bytes.len() as i32).to_le_bytes());
                buf.extend_from_slice(&bytes);
            }
            None => {
                buf.extend_from_slice(&(-1i32).to_le_bytes());
            }
        }
    }
    Ok((buf, type_oid))
}

fn decode_composite(data: &[u8]) -> Result<Composite> {
    if data.len() < 4 {
        return Ok(Composite {
            type_oid: OID_RECORD,
            fields: Vec::new(),
        });
    }
    let count = i32::from_le_bytes(read_fixed::<4>(data)) as usize;
    let mut offset = 4;
    let mut fields = Vec::with_capacity(count);
    for _ in 0..count {
        if offset + 8 > data.len() {
            break;
        }
        let oid = u32::from_le_bytes(read_fixed::<4>(&data[offset..]));
        offset += 4;
        let length = i32::from_le_bytes(read_fixed::<4>(&data[offset..]));
        offset += 4;
        if length < 0 {
            fields.push(CompositeField {
                oid,
                value: None,
                raw: None,
            });
            continue;
        }
        let len = length as usize;
        if offset + len > data.len() {
            break;
        }
        let raw = data[offset..offset + len].to_vec();
        offset += len;
        let value = decode_binary_value(oid, &raw).ok();
        fields.push(CompositeField {
            oid,
            value,
            raw: Some(raw),
        });
    }
    Ok(Composite {
        type_oid: OID_RECORD,
        fields,
    })
}

fn value_to_param(value: &Value) -> Result<Param> {
    match value {
        Value::Null => Ok(Param::Null),
        Value::Bool(v) => Ok(Param::Bool(*v)),
        Value::Int16(v) => Ok(Param::Int16(*v)),
        Value::Int32(v) => Ok(Param::Int32(*v)),
        Value::Int64(v) => Ok(Param::Int64(*v)),
        Value::Float32(v) => Ok(Param::Float32(*v)),
        Value::Float64(v) => Ok(Param::Float64(*v)),
        Value::Decimal(v) => Ok(Param::Decimal(v.clone())),
        Value::String(v) => Ok(Param::String(v.clone())),
        Value::Bytes(v) => Ok(Param::Bytes(v.clone())),
        Value::Date(v) => Ok(Param::Date(*v)),
        Value::Time(v) => Ok(Param::Time(*v)),
        Value::Timestamp(v) => Ok(Param::Timestamp(*v)),
        Value::Interval(v) => Ok(Param::Interval(v.clone())),
        Value::Uuid(v) => Ok(Param::Uuid(v.clone())),
        Value::Json(v) => Ok(Param::Json(v.clone())),
        Value::Jsonb(v) => Ok(Param::Jsonb(v.clone())),
        Value::Array(values) => {
            let mut params = Vec::with_capacity(values.len());
            for item in values {
                params.push(value_to_param(item)?);
            }
            Ok(Param::Array(params))
        }
        Value::Vector(values) => Ok(Param::Vector(values.clone())),
        Value::Range(range) => Ok(Param::Range(range.clone())),
        Value::Geometry(geom) => Ok(Param::Geometry(geom.clone())),
        Value::Composite(comp) => Ok(Param::Composite(comp.clone())),
    }
}

fn read_fixed<const N: usize>(data: &[u8]) -> [u8; N] {
    let mut out = [0u8; N];
    let len = std::cmp::min(N, data.len());
    out[..len].copy_from_slice(&data[..len]);
    out
}

fn encode_length_prefixed(data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(4 + data.len());
    out.extend_from_slice(&(data.len() as u32).to_le_bytes());
    out.extend_from_slice(data);
    out
}

fn strip_length_prefix(data: &[u8]) -> &[u8] {
    if data.len() < 4 {
        return data;
    }
    let length = u32::from_le_bytes([data[0], data[1], data[2], data[3]]) as usize;
    if length <= data.len().saturating_sub(4) {
        return &data[4..4 + length];
    }
    data
}

fn encode_date(value: NaiveDate) -> Vec<u8> {
    let base = NaiveDate::from_ymd_opt(2000, 1, 1).unwrap();
    let days = value.signed_duration_since(base).num_days() as i32;
    days.to_le_bytes().to_vec()
}

fn encode_time(value: NaiveTime) -> Vec<u8> {
    let micros =
        value.num_seconds_from_midnight() as i64 * 1_000_000 + (value.nanosecond() as i64 / 1000);
    micros.to_le_bytes().to_vec()
}

fn encode_timestamp(value: DateTime<Utc>) -> Vec<u8> {
    let base = Utc.with_ymd_and_hms(2000, 1, 1, 0, 0, 0).unwrap();
    let micros = (value - base).num_microseconds().unwrap_or(0);
    micros.to_le_bytes().to_vec()
}

fn encode_interval(value: &Interval) -> Vec<u8> {
    let mut out = Vec::with_capacity(16);
    out.extend_from_slice(&value.micros.to_le_bytes());
    out.extend_from_slice(&value.days.to_le_bytes());
    out.extend_from_slice(&value.months.to_le_bytes());
    out
}

fn decode_date(data: &[u8]) -> NaiveDate {
    if data.len() < 4 {
        return NaiveDate::from_ymd_opt(2000, 1, 1).unwrap();
    }
    let days = i32::from_le_bytes(read_fixed::<4>(data));
    let base = NaiveDate::from_ymd_opt(2000, 1, 1).unwrap();
    base.checked_add_signed(chrono::Duration::days(days as i64))
        .unwrap_or(base)
}

fn decode_time(data: &[u8]) -> NaiveTime {
    if data.len() < 8 {
        return NaiveTime::from_hms_opt(0, 0, 0).unwrap();
    }
    let micros = i64::from_le_bytes(read_fixed::<8>(data));
    let secs = micros / 1_000_000;
    let nanos = (micros % 1_000_000) * 1000;
    NaiveTime::from_num_seconds_from_midnight_opt(secs as u32, nanos as u32)
        .unwrap_or_else(|| NaiveTime::from_hms_opt(0, 0, 0).unwrap())
}

fn decode_timestamp(data: &[u8]) -> DateTime<Utc> {
    if data.len() < 8 {
        return Utc.with_ymd_and_hms(2000, 1, 1, 0, 0, 0).unwrap();
    }
    let micros = i64::from_le_bytes(read_fixed::<8>(data));
    let base = Utc.with_ymd_and_hms(2000, 1, 1, 0, 0, 0).unwrap();
    base + chrono::Duration::microseconds(micros)
}

fn decode_interval(data: &[u8]) -> Interval {
    if data.len() < 16 {
        return Interval {
            micros: 0,
            days: 0,
            months: 0,
        };
    }
    let micros = i64::from_le_bytes(read_fixed::<8>(data));
    let days = i32::from_le_bytes(read_fixed::<4>(&data[8..]));
    let months = i32::from_le_bytes(read_fixed::<4>(&data[12..]));
    Interval {
        micros,
        days,
        months,
    }
}

fn uuid_to_bytes(value: &str) -> Vec<u8> {
    let hex = value.replace('-', "");
    hex::decode(hex).unwrap_or_else(|_| value.as_bytes().to_vec())
}

fn bytes_to_uuid(data: &[u8]) -> String {
    let hex = hex::encode(data);
    if hex.len() != 32 {
        return hex;
    }
    format!(
        "{}-{}-{}-{}-{}",
        &hex[0..8],
        &hex[8..12],
        &hex[12..16],
        &hex[16..20],
        &hex[20..32]
    )
}

fn encode_range(range: &Range<RangeValue>) -> Result<(Vec<u8>, u32)> {
    let oid = range.range_oid.unwrap_or_else(|| infer_range_oid(range));
    let mut flags = 0u8;
    if range.empty {
        flags |= RANGE_EMPTY;
    }
    if range.lower_inclusive {
        flags |= RANGE_LB_INC;
    }
    if range.upper_inclusive {
        flags |= RANGE_UB_INC;
    }
    if range.lower_infinite {
        flags |= RANGE_LB_INF;
    }
    if range.upper_infinite {
        flags |= RANGE_UB_INF;
    }

    let mut out = Vec::new();
    out.push(flags);
    out.extend_from_slice(&[0, 0, 0]);
    if !range.empty && !range.lower_infinite {
        let bound = encode_range_bound(
            oid,
            range
                .lower
                .as_ref()
                .ok_or_else(|| Error::new(ErrorKind::Data, "range lower missing"))?,
        )?;
        out.extend_from_slice(&(bound.len() as i32).to_le_bytes());
        out.extend_from_slice(&bound);
    }
    if !range.empty && !range.upper_infinite {
        let bound = encode_range_bound(
            oid,
            range
                .upper
                .as_ref()
                .ok_or_else(|| Error::new(ErrorKind::Data, "range upper missing"))?,
        )?;
        out.extend_from_slice(&(bound.len() as i32).to_le_bytes());
        out.extend_from_slice(&bound);
    }
    Ok((out, oid))
}

fn infer_range_oid(range: &Range<RangeValue>) -> u32 {
    if let Some(oid) = range.range_oid {
        return oid;
    }
    if let Some(ref bound) = range.lower {
        return range_oid_for_bound(bound);
    }
    if let Some(ref bound) = range.upper {
        return range_oid_for_bound(bound);
    }
    0
}

fn range_oid_for_bound(bound: &RangeValue) -> u32 {
    match bound {
        RangeValue::Int32(_) => OID_INT4RANGE,
        RangeValue::Int64(_) => OID_INT8RANGE,
        RangeValue::Decimal(_) => OID_NUMRANGE,
        RangeValue::Date(_) => OID_DATERANGE,
        RangeValue::Timestamp(_) => OID_TSRANGE,
        RangeValue::TimestampTz(_) => OID_TSTZRANGE,
    }
}

fn encode_range_bound(oid: u32, value: &RangeValue) -> Result<Vec<u8>> {
    match (oid, value) {
        (OID_INT4RANGE, RangeValue::Int32(v)) => Ok(v.to_le_bytes().to_vec()),
        (OID_INT8RANGE, RangeValue::Int64(v)) => Ok(v.to_le_bytes().to_vec()),
        (OID_NUMRANGE, RangeValue::Decimal(v)) => Ok(encode_length_prefixed(v.value.as_bytes())),
        (OID_DATERANGE, RangeValue::Date(v)) => Ok(encode_date(v.value)),
        (OID_TSRANGE, RangeValue::Timestamp(v)) => Ok(encode_timestamp(v.value)),
        (OID_TSTZRANGE, RangeValue::TimestampTz(v)) => Ok(encode_timestamp(v.value)),
        _ => Err(Error::new(ErrorKind::Data, "unsupported range bound")),
    }
}

fn decode_range(oid: u32, data: &[u8]) -> Result<Range<RangeValue>> {
    if data.len() < 4 {
        return Ok(Range::new());
    }
    let flags = data[0];
    let mut offset = 4;
    let mut range = Range::new();
    range.empty = (flags & RANGE_EMPTY) != 0;
    range.lower_inclusive = (flags & RANGE_LB_INC) != 0;
    range.upper_inclusive = (flags & RANGE_UB_INC) != 0;
    range.lower_infinite = (flags & RANGE_LB_INF) != 0;
    range.upper_infinite = (flags & RANGE_UB_INF) != 0;
    range.range_oid = Some(oid);

    if range.empty {
        return Ok(range);
    }
    if !range.lower_infinite {
        if offset + 4 > data.len() {
            return Ok(range);
        }
        let len = i32::from_le_bytes(read_fixed::<4>(&data[offset..])) as usize;
        offset += 4;
        if offset + len > data.len() {
            return Ok(range);
        }
        range.lower = Some(decode_range_bound(oid, &data[offset..offset + len])?);
        offset += len;
    }
    if !range.upper_infinite {
        if offset + 4 > data.len() {
            return Ok(range);
        }
        let len = i32::from_le_bytes(read_fixed::<4>(&data[offset..])) as usize;
        offset += 4;
        if offset + len > data.len() {
            return Ok(range);
        }
        range.upper = Some(decode_range_bound(oid, &data[offset..offset + len])?);
    }
    Ok(range)
}

fn decode_range_bound(oid: u32, data: &[u8]) -> Result<RangeValue> {
    match oid {
        OID_INT4RANGE => Ok(RangeValue::Int32(i32::from_le_bytes(read_fixed::<4>(data)))),
        OID_INT8RANGE => Ok(RangeValue::Int64(i64::from_le_bytes(read_fixed::<8>(data)))),
        OID_NUMRANGE => Ok(RangeValue::Decimal(Decimal {
            value: String::from_utf8_lossy(strip_length_prefix(data)).to_string(),
        })),
        OID_DATERANGE => Ok(RangeValue::Date(Date {
            value: decode_date(data),
        })),
        OID_TSRANGE => Ok(RangeValue::Timestamp(Timestamp {
            value: decode_timestamp(data),
        })),
        OID_TSTZRANGE => Ok(RangeValue::TimestampTz(TimestampTz {
            value: decode_timestamp(data),
        })),
        _ => Err(Error::new(ErrorKind::Data, "unsupported range bound")),
    }
}

fn format_array_literal(values: &[Param]) -> String {
    let items: Vec<String> = values.iter().map(format_array_item).collect();
    format!("{{{}}}", items.join(","))
}

fn format_array_item(value: &Param) -> String {
    match value {
        Param::Null => "NULL".to_string(),
        Param::String(val) => format!("\"{}\"", val.replace('"', "\\\"")),
        Param::Array(items) => format_array_literal(items),
        Param::Bool(val) => val.to_string(),
        Param::Int16(val) => val.to_string(),
        Param::Int32(val) => val.to_string(),
        Param::Int64(val) => val.to_string(),
        Param::Float32(val) => val.to_string(),
        Param::Float64(val) => val.to_string(),
        Param::Vector(values) => format_vector_literal(values),
        Param::Decimal(val) => val.to_string(),
        Param::Uuid(val) => val.to_string(),
        _ => format!("{}", value_string(value)),
    }
}

fn value_string(value: &Param) -> String {
    match value {
        Param::Bool(val) => val.to_string(),
        Param::Int16(val) => val.to_string(),
        Param::Int32(val) => val.to_string(),
        Param::Int64(val) => val.to_string(),
        Param::Float32(val) => val.to_string(),
        Param::Float64(val) => val.to_string(),
        Param::Decimal(val) => val.to_string(),
        Param::String(val) => val.clone(),
        _ => String::new(),
    }
}

fn parse_array_literal(text: &str) -> Vec<Value> {
    let trimmed = text.trim();
    if trimmed.is_empty() || trimmed == "{}" {
        return Vec::new();
    }
    let inner = trimmed
        .strip_prefix('{')
        .and_then(|s| s.strip_suffix('}'))
        .unwrap_or(trimmed);
    split_array_items(inner)
}

fn split_array_items(text: &str) -> Vec<Value> {
    let mut items = Vec::new();
    let mut depth = 0i32;
    let mut buffer = String::new();
    for ch in text.chars() {
        match ch {
            '{' => {
                depth += 1;
                buffer.push(ch);
            }
            '}' => {
                depth -= 1;
                buffer.push(ch);
            }
            ',' if depth == 0 => {
                items.push(parse_array_item(&buffer));
                buffer.clear();
            }
            _ => buffer.push(ch),
        }
    }
    if !buffer.is_empty() || !text.is_empty() {
        items.push(parse_array_item(&buffer));
    }
    items
}

fn parse_array_item(raw: &str) -> Value {
    let token = raw.trim();
    if token.eq_ignore_ascii_case("NULL") {
        return Value::Null;
    }
    if token.starts_with('{') && token.ends_with('}') {
        return Value::Array(parse_array_literal(token));
    }
    if token.starts_with('[') && token.ends_with(']') {
        return Value::Vector(parse_vector_literal(token));
    }
    if token == "true" || token == "false" {
        return Value::Bool(token == "true");
    }
    if let Ok(val) = token.parse::<i64>() {
        return Value::Int64(val);
    }
    if let Ok(val) = token.parse::<f64>() {
        return Value::Float64(val as f64);
    }
    Value::String(token.to_string())
}

fn format_vector_literal(values: &[f32]) -> String {
    let parts: Vec<String> = values.iter().map(|v| format_number(*v as f64)).collect();
    format!("[{}]", parts.join(","))
}

fn parse_vector_literal(text: &str) -> Vec<f32> {
    let trimmed = text.trim().trim_start_matches('[').trim_end_matches(']');
    if trimmed.is_empty() {
        return Vec::new();
    }
    trimmed
        .split(',')
        .filter_map(|part| part.trim().parse::<f32>().ok())
        .collect()
}

fn format_number(value: f64) -> String {
    if value.is_finite() {
        value.to_string()
    } else {
        "0".to_string()
    }
}

impl fmt::Display for Param {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Param::Null => write!(f, "NULL"),
            Param::Bool(v) => write!(f, "{}", v),
            Param::Int16(v) => write!(f, "{}", v),
            Param::Int32(v) => write!(f, "{}", v),
            Param::Int64(v) => write!(f, "{}", v),
            Param::Float32(v) => write!(f, "{}", v),
            Param::Float64(v) => write!(f, "{}", v),
            Param::Decimal(v) => write!(f, "{}", v),
            Param::String(v) => write!(f, "{}", v),
            _ => write!(f, ""),
        }
    }
}

pub fn decode_array_literal(text: &str) -> Vec<Value> {
    parse_array_literal(text)
}

pub fn type_name(oid: u32) -> &'static str {
    match oid {
        OID_BOOL => "boolean",
        OID_INT2 => "int2",
        OID_INT4 => "int4",
        OID_INT8 => "int8",
        OID_FLOAT4 => "float4",
        OID_FLOAT8 => "float8",
        OID_NUMERIC => "numeric",
        OID_MONEY => "money",
        OID_TEXT => "text",
        OID_VARCHAR => "varchar",
        OID_CHAR | OID_BPCHAR => "char",
        OID_BYTEA => "bytea",
        OID_DATE => "date",
        OID_TIME => "time",
        OID_TIMESTAMP => "timestamp",
        OID_TIMESTAMPTZ => "timestamptz",
        OID_INTERVAL => "interval",
        OID_UUID => "uuid",
        OID_JSON => "json",
        OID_JSONB => "jsonb",
        OID_XML => "xml",
        OID_INET => "inet",
        OID_CIDR => "cidr",
        OID_MACADDR => "macaddr",
        OID_MACADDR8 => "macaddr8",
        OID_TSVECTOR => "tsvector",
        OID_TSQUERY => "tsquery",
        OID_INT4RANGE => "int4range",
        OID_INT8RANGE => "int8range",
        OID_NUMRANGE => "numrange",
        OID_TSRANGE => "tsrange",
        OID_TSTZRANGE => "tstzrange",
        OID_DATERANGE => "daterange",
        OID_SB_VECTOR => "vector",
        _ => "unknown",
    }
}
