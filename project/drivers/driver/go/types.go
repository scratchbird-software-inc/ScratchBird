// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"net"
	"reflect"
	"strconv"
	"strings"
	"time"
)

const (
	oidBool        = 16
	oidBytea       = 17
	oidChar        = 18
	oidInt8        = 20
	oidInt2        = 21
	oidInt4        = 23
	oidText        = 25
	oidJSON        = 114
	oidXML         = 142
	oidPoint       = 600
	oidLseg        = 601
	oidPath        = 602
	oidBox         = 603
	oidPolygon     = 604
	oidLine        = 628
	oidFloat4      = 700
	oidFloat8      = 701
	oidCircle      = 718
	oidMoney       = 790
	oidMacaddr     = 829
	oidCidr        = 650
	oidInet        = 869
	oidMacaddr8    = 774
	oidBPChar      = 1042
	oidVarchar     = 1043
	oidDate        = 1082
	oidTime        = 1083
	oidTimestamp   = 1114
	oidTimestamptz = 1184
	oidInterval    = 1186
	oidTimetz      = 1266
	oidNumeric     = 1700
	oidUUID        = 2950
	oidJSONB       = 3802
	oidRecord      = 2249
	oidInt4Range   = 3904
	oidNumRange    = 3906
	oidTSRange     = 3908
	oidTstzRange   = 3910
	oidDateRange   = 3912
	oidInt8Range   = 3926
	oidTSVector    = 3614
	oidTSQuery     = 3615
	oidSBVector    = 16386
)

const (
	rangeEmpty = 0x01
	rangeLbInc = 0x02
	rangeUbInc = 0x04
	rangeLbInf = 0x08
	rangeUbInf = 0x10
)

type JSONB struct {
	Raw   []byte
	Value any
}

type JSON struct {
	Raw   []byte
	Value any
}

type Geometry struct {
	WKB  []byte
	SRID *uint32
	WKT  string
}

type Range[T any] struct {
	Lower          T
	Upper          T
	LowerInclusive bool
	UpperInclusive bool
	LowerInfinite  bool
	UpperInfinite  bool
	Empty          bool
}

type Interval struct {
	Micros int64
	Days   int32
	Months int32
}

type Date struct {
	Time time.Time
}

type Time struct {
	Micros int64
}

type TimeTZ struct {
	Micros            int64
	OffsetSecondsWest int32
}

type Timestamp struct {
	Time time.Time
}

type TimestampTZ struct {
	Time time.Time
}

type Decimal struct {
	Value string
}

type Money struct {
	Cents int64
}

type RawValue struct {
	OID  uint32
	Data []byte
}

type CompositeField struct {
	OID   uint32
	Value any
	Raw   []byte
}

type Composite struct {
	TypeOID uint32
	Fields  []CompositeField
}

func encodeParam(value any) (paramValue, uint32, error) {
	if value == nil {
		return paramValue{null: true}, 0, nil
	}
	switch v := value.(type) {
	case Composite:
		data, oid, err := encodeComposite(v)
		if err != nil {
			return paramValue{}, 0, err
		}
		return paramValue{data: data}, oid, nil
	case *Composite:
		if v == nil {
			return paramValue{null: true}, 0, nil
		}
		data, oid, err := encodeComposite(*v)
		if err != nil {
			return paramValue{}, 0, err
		}
		return paramValue{data: data}, oid, nil
	case RawValue:
		return paramValue{data: append([]byte{}, v.Data...)}, v.OID, nil
	case JSONB:
		raw := v.Raw
		if len(raw) == 0 && v.Value != nil {
			encoded, err := json.Marshal(v.Value)
			if err != nil {
				return paramValue{}, 0, err
			}
			raw = encoded
		}
		if len(raw) == 0 {
			return paramValue{}, 0, errors.New("JSONB requires raw payload")
		}
		return paramValue{data: encodeLengthPrefixed(raw)}, oidJSONB, nil
	case JSON:
		raw := v.Raw
		if len(raw) == 0 && v.Value != nil {
			encoded, err := json.Marshal(v.Value)
			if err != nil {
				return paramValue{}, 0, err
			}
			raw = encoded
		}
		return paramValue{data: encodeLengthPrefixed(raw)}, oidJSON, nil
	case Geometry:
		if len(v.WKB) == 0 {
			return paramValue{}, 0, errors.New("geometry requires WKB payload")
		}
		return paramValue{data: encodeLengthPrefixed(v.WKB)}, oidPoint, nil
	case Range[int32]:
		data, err := encodeRange(int32RangeOID, v)
		return paramValue{data: data}, oidInt4Range, err
	case Range[int64]:
		data, err := encodeRange(int64RangeOID, v)
		return paramValue{data: data}, oidInt8Range, err
	case Range[Decimal]:
		data, err := encodeRange(decimalRangeOID, v)
		return paramValue{data: data}, oidNumRange, err
	case Range[Date]:
		data, err := encodeRange(dateRangeOID, v)
		return paramValue{data: data}, oidDateRange, err
	case Range[Timestamp]:
		data, err := encodeRange(timestampRangeOID, v)
		return paramValue{data: data}, oidTSRange, err
	case Range[TimestampTZ]:
		data, err := encodeRange(timestamptzRangeOID, v)
		return paramValue{data: data}, oidTstzRange, err
	case bool:
		if v {
			return paramValue{data: []byte{1}}, oidBool, nil
		}
		return paramValue{data: []byte{0}}, oidBool, nil
	case int16:
		buf := make([]byte, 2)
		binary.LittleEndian.PutUint16(buf, uint16(v))
		return paramValue{data: buf}, oidInt2, nil
	case int32:
		buf := make([]byte, 4)
		binary.LittleEndian.PutUint32(buf, uint32(v))
		return paramValue{data: buf}, oidInt4, nil
	case int64:
		buf := make([]byte, 8)
		binary.LittleEndian.PutUint64(buf, uint64(v))
		return paramValue{data: buf}, oidInt8, nil
	case int:
		buf := make([]byte, 8)
		binary.LittleEndian.PutUint64(buf, uint64(v))
		return paramValue{data: buf}, oidInt8, nil
	case float32:
		buf := make([]byte, 4)
		binary.LittleEndian.PutUint32(buf, math.Float32bits(v))
		return paramValue{data: buf}, oidFloat4, nil
	case float64:
		buf := make([]byte, 8)
		binary.LittleEndian.PutUint64(buf, math.Float64bits(v))
		return paramValue{data: buf}, oidFloat8, nil
	case string:
		return paramValue{data: encodeLengthPrefixed([]byte(v))}, oidText, nil
	case []byte:
		return paramValue{data: encodeLengthPrefixed(v)}, oidBytea, nil
	case time.Time:
		return paramValue{data: encodeTimestamp(v)}, oidTimestamptz, nil
	case Date:
		return paramValue{data: encodeDate(v.Time)}, oidDate, nil
	case Time:
		return paramValue{data: encodeTimeMicros(v.Micros)}, oidTime, nil
	case TimeTZ:
		return paramValue{data: encodeTimetz(v)}, oidTimetz, nil
	case Timestamp:
		return paramValue{data: encodeTimestamp(v.Time)}, oidTimestamp, nil
	case TimestampTZ:
		return paramValue{data: encodeTimestamp(v.Time)}, oidTimestamptz, nil
	case Interval:
		return paramValue{data: encodeInterval(v)}, oidInterval, nil
	case time.Duration:
		return paramValue{data: encodeInterval(Interval{Micros: v.Microseconds()})}, oidInterval, nil
	case net.IP:
		return paramValue{data: encodeLengthPrefixed([]byte(v.String()))}, oidInet, nil
	case net.IPNet:
		return paramValue{data: encodeLengthPrefixed([]byte(v.String()))}, oidCidr, nil
	case []float32:
		return paramValue{data: encodeLengthPrefixed([]byte(formatVectorLiteral(v)))}, oidSBVector, nil
	case []float64:
		return paramValue{data: encodeLengthPrefixed([]byte(formatVectorLiteral64(v)))}, oidSBVector, nil
	case Decimal:
		return paramValue{data: encodeLengthPrefixed([]byte(v.Value))}, oidNumeric, nil
	case Money:
		buf := make([]byte, 8)
		binary.LittleEndian.PutUint64(buf, uint64(v.Cents))
		return paramValue{data: buf}, oidMoney, nil
	case []any:
		return paramValue{data: encodeLengthPrefixed([]byte(formatArrayLiteral(v)))}, 0, nil
	case []string:
		return paramValue{data: encodeLengthPrefixed([]byte(formatArrayLiteral(toAnySlice(v))))}, 0, nil
	case []int:
		return paramValue{data: encodeLengthPrefixed([]byte(formatArrayLiteral(toAnySlice(v))))}, 0, nil
	case []int32:
		return paramValue{data: encodeLengthPrefixed([]byte(formatArrayLiteral(toAnySlice(v))))}, 0, nil
	case []int64:
		return paramValue{data: encodeLengthPrefixed([]byte(formatArrayLiteral(toAnySlice(v))))}, 0, nil
	case []bool:
		return paramValue{data: encodeLengthPrefixed([]byte(formatArrayLiteral(toAnySlice(v))))}, 0, nil
	default:
		if stringer, ok := value.(fmt.Stringer); ok {
			return paramValue{data: encodeLengthPrefixed([]byte(stringer.String()))}, oidText, nil
		}
	}
	return paramValue{}, 0, errors.New("unsupported parameter type")
}

func decodeColumnValue(col columnInfo, data []byte) (any, error) {
	if col.typeOID == 0 {
		if col.format == uint8(formatText) {
			return parseUnknownText(decodeTextValue(data)), nil
		}
		return decodeUnknownBinary(data), nil
	}
	if col.format == uint8(formatText) {
		return decodeTextValue(data), nil
	}
	return decodeBinaryValue(col.typeOID, data)
}

func decodeBinaryValue(oid uint32, data []byte) (any, error) {
	switch oid {
	case oidBool:
		return len(data) > 0 && data[0] == 1, nil
	case oidInt2:
		return int16(binary.LittleEndian.Uint16(data)), nil
	case oidInt4:
		return int32(binary.LittleEndian.Uint32(data)), nil
	case oidInt8:
		return int64(binary.LittleEndian.Uint64(data)), nil
	case oidFloat4:
		return math.Float32frombits(binary.LittleEndian.Uint32(data)), nil
	case oidFloat8:
		return math.Float64frombits(binary.LittleEndian.Uint64(data)), nil
	case oidNumeric:
		return string(stripLengthPrefix(data)), nil
	case oidMoney:
		if len(data) >= 8 {
			return int64(binary.LittleEndian.Uint64(data)), nil
		}
		return int64(0), nil
	case oidText, oidVarchar, oidChar, oidBPChar, oidJSON, oidXML, oidTSVector, oidTSQuery:
		return string(stripLengthPrefix(data)), nil
	case oidJSONB:
		return JSONB{Raw: append([]byte{}, stripLengthPrefix(data)...)}, nil
	case oidBytea:
		return append([]byte{}, stripLengthPrefix(data)...), nil
	case oidDate:
		return decodeDate(data), nil
	case oidTime:
		return decodeTime(data), nil
	case oidTimetz:
		return decodeTimetz(data), nil
	case oidTimestamp:
		return decodeTimestamp(data), nil
	case oidTimestamptz:
		return decodeTimestamp(data), nil
	case oidInterval:
		return decodeInterval(data), nil
	case oidUUID:
		return bytesToUUIDString(data), nil
	case oidInet, oidCidr, oidMacaddr, oidMacaddr8:
		return string(stripLengthPrefix(data)), nil
	case oidPoint, oidLseg, oidPath, oidBox, oidPolygon, oidLine, oidCircle:
		return decodeGeometry(data), nil
	case oidInt4Range, oidInt8Range, oidNumRange, oidTSRange, oidTstzRange, oidDateRange:
		return decodeRange(oid, data)
	case oidSBVector:
		return parseVectorLiteral(string(stripLengthPrefix(data))), nil
	case oidRecord:
		return decodeComposite(data)
	default:
		return append([]byte{}, data...), nil
	}
}

func decodeTextValue(data []byte) string {
	if len(data) >= 4 {
		length := int(binary.LittleEndian.Uint32(data[:4]))
		if length >= 0 && length <= len(data)-4 {
			return string(data[4 : 4+length])
		}
	}
	return string(data)
}

func decodeUnknownBinary(data []byte) any {
	if len(data) >= 4 {
		stripped := stripLengthPrefix(data)
		if len(stripped) > 0 && looksLikeText(stripped) {
			return parseUnknownText(string(stripped))
		}
	}
	trimmed := stripTrailingNulls(data)
	if len(trimmed) > 0 && looksLikeText(trimmed) {
		return parseUnknownText(string(trimmed))
	}
	switch len(data) {
	case 1:
		return int16(int8(data[0]))
	case 2:
		return int16(binary.LittleEndian.Uint16(data))
	case 4:
		return int32(binary.LittleEndian.Uint32(data))
	case 8:
		return int64(binary.LittleEndian.Uint64(data))
	case 16:
		return bytesToUUIDString(data)
	default:
		return append([]byte{}, data...)
	}
}

func parseUnknownText(text string) any {
	trimmed := strings.TrimSpace(text)
	if trimmed == "" {
		return text
	}
	if strings.HasPrefix(trimmed, "{") && strings.HasSuffix(trimmed, "}") {
		return parseArrayLiteral(trimmed)
	}
	lowered := strings.ToLower(trimmed)
	if lowered == "true" {
		return true
	}
	if lowered == "false" {
		return false
	}
	if intVal, err := strconv.ParseInt(trimmed, 10, 64); err == nil {
		if intVal >= math.MinInt32 && intVal <= math.MaxInt32 {
			return int32(intVal)
		}
		return intVal
	}
	if floatVal, err := strconv.ParseFloat(trimmed, 64); err == nil {
		return floatVal
	}
	return text
}

func stripTrailingNulls(data []byte) []byte {
	end := len(data)
	for end > 0 && data[end-1] == 0 {
		end--
	}
	return data[:end]
}

func toAnySlice[T any](values []T) []any {
	out := make([]any, 0, len(values))
	for _, v := range values {
		out = append(out, v)
	}
	return out
}

func looksLikeText(data []byte) bool {
	for _, b := range data {
		if b == 0x09 || b == 0x0a || b == 0x0d {
			continue
		}
		if b < 0x20 || b > 0x7e {
			return false
		}
	}
	return true
}

func stripLengthPrefix(data []byte) []byte {
	if len(data) < 4 {
		return data
	}
	length := int(binary.LittleEndian.Uint32(data[:4]))
	if length < 0 || length > len(data)-4 {
		return data
	}
	return data[4 : 4+length]
}

func encodeLengthPrefixed(data []byte) []byte {
	buf := make([]byte, 4+len(data))
	binary.LittleEndian.PutUint32(buf[0:4], uint32(len(data)))
	copy(buf[4:], data)
	return buf
}

func encodeComposite(comp Composite) ([]byte, uint32, error) {
	typeOID := comp.TypeOID
	if typeOID == 0 {
		typeOID = oidRecord
	}
	buf := &bytes.Buffer{}
	if err := binary.Write(buf, binary.LittleEndian, int32(len(comp.Fields))); err != nil {
		return nil, 0, err
	}
	for _, field := range comp.Fields {
		fieldOID := field.OID
		var data []byte
		if field.Raw != nil {
			data = field.Raw
		} else if field.Value != nil {
			encoded, oid, err := encodeParam(field.Value)
			if err != nil {
				return nil, 0, err
			}
			if fieldOID == 0 {
				fieldOID = oid
			}
			if encoded.null {
				if err := binary.Write(buf, binary.LittleEndian, uint32(fieldOID)); err != nil {
					return nil, 0, err
				}
				if err := binary.Write(buf, binary.LittleEndian, int32(-1)); err != nil {
					return nil, 0, err
				}
				continue
			}
			data = encoded.data
		} else {
			if fieldOID == 0 {
				return nil, 0, errors.New("composite field OID is required")
			}
			if err := binary.Write(buf, binary.LittleEndian, uint32(fieldOID)); err != nil {
				return nil, 0, err
			}
			if err := binary.Write(buf, binary.LittleEndian, int32(-1)); err != nil {
				return nil, 0, err
			}
			continue
		}
		if fieldOID == 0 {
			return nil, 0, errors.New("composite field OID is required")
		}
		if err := binary.Write(buf, binary.LittleEndian, uint32(fieldOID)); err != nil {
			return nil, 0, err
		}
		if err := binary.Write(buf, binary.LittleEndian, int32(len(data))); err != nil {
			return nil, 0, err
		}
		if _, err := buf.Write(data); err != nil {
			return nil, 0, err
		}
	}
	return buf.Bytes(), typeOID, nil
}

func decodeComposite(data []byte) (Composite, error) {
	comp := Composite{TypeOID: oidRecord}
	if len(data) < 4 {
		return comp, errors.New("composite payload truncated")
	}
	count := int(int32(binary.LittleEndian.Uint32(data[:4])))
	offset := 4
	fields := make([]CompositeField, 0, count)
	for i := 0; i < count; i++ {
		if offset+8 > len(data) {
			return Composite{TypeOID: oidRecord, Fields: fields}, errors.New("composite payload truncated")
		}
		oid := binary.LittleEndian.Uint32(data[offset : offset+4])
		offset += 4
		length := int(int32(binary.LittleEndian.Uint32(data[offset : offset+4])))
		offset += 4
		if length < 0 {
			fields = append(fields, CompositeField{OID: oid})
			continue
		}
		if offset+length > len(data) {
			return Composite{TypeOID: oidRecord, Fields: fields}, errors.New("composite payload truncated")
		}
		raw := append([]byte{}, data[offset:offset+length]...)
		offset += length
		value, err := decodeBinaryValue(oid, raw)
		if err != nil {
			value = RawValue{OID: oid, Data: raw}
		}
		fields = append(fields, CompositeField{OID: oid, Value: value, Raw: raw})
	}
	comp.Fields = fields
	return comp, nil
}

func encodeDate(t time.Time) []byte {
	base := time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC)
	days := int32(t.UTC().Sub(base).Hours() / 24)
	buf := make([]byte, 4)
	binary.LittleEndian.PutUint32(buf, uint32(days))
	return buf
}

func encodeTimeMicros(micros int64) []byte {
	buf := make([]byte, 8)
	binary.LittleEndian.PutUint64(buf, uint64(micros))
	return buf
}

func encodeTimetz(value TimeTZ) []byte {
	buf := make([]byte, 12)
	binary.LittleEndian.PutUint64(buf[0:8], uint64(value.Micros))
	binary.LittleEndian.PutUint32(buf[8:12], uint32(value.OffsetSecondsWest))
	return buf
}

func encodeTimestamp(t time.Time) []byte {
	base := time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC)
	micros := t.UTC().Sub(base).Microseconds()
	buf := make([]byte, 8)
	binary.LittleEndian.PutUint64(buf, uint64(micros))
	return buf
}

func encodeInterval(interval Interval) []byte {
	buf := make([]byte, 16)
	binary.LittleEndian.PutUint64(buf[0:8], uint64(interval.Micros))
	binary.LittleEndian.PutUint32(buf[8:12], uint32(interval.Days))
	binary.LittleEndian.PutUint32(buf[12:16], uint32(interval.Months))
	return buf
}

func decodeDate(data []byte) time.Time {
	if len(data) < 4 {
		return time.Time{}
	}
	days := int32(binary.LittleEndian.Uint32(data))
	base := time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC)
	return base.AddDate(0, 0, int(days))
}

func decodeTime(data []byte) time.Duration {
	if len(data) < 8 {
		return 0
	}
	micros := int64(binary.LittleEndian.Uint64(data))
	return time.Duration(micros) * time.Microsecond
}

func decodeTimetz(data []byte) TimeTZ {
	if len(data) < 8 {
		return TimeTZ{}
	}
	micros := int64(binary.LittleEndian.Uint64(data[0:8]))
	offset := int32(0)
	if len(data) >= 12 {
		offset = int32(binary.LittleEndian.Uint32(data[8:12]))
	}
	return TimeTZ{Micros: micros, OffsetSecondsWest: offset}
}

func decodeTimestamp(data []byte) time.Time {
	if len(data) < 8 {
		return time.Time{}
	}
	micros := int64(binary.LittleEndian.Uint64(data))
	base := time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC)
	return base.Add(time.Duration(micros) * time.Microsecond)
}

func decodeInterval(data []byte) Interval {
	if len(data) < 16 {
		return Interval{}
	}
	micros := int64(binary.LittleEndian.Uint64(data[0:8]))
	days := int32(binary.LittleEndian.Uint32(data[8:12]))
	months := int32(binary.LittleEndian.Uint32(data[12:16]))
	return Interval{Micros: micros, Days: days, Months: months}
}

func bytesToUUIDString(data []byte) string {
	if len(data) != 16 {
		return hex.EncodeToString(data)
	}
	hexStr := hex.EncodeToString(data)
	return hexStr[0:8] + "-" + hexStr[8:12] + "-" + hexStr[12:16] + "-" + hexStr[16:20] + "-" + hexStr[20:]
}

func decodeGeometry(data []byte) Geometry {
	return Geometry{WKB: append([]byte{}, stripLengthPrefix(data)...)}
}

func parseArrayLiteral(text string) []any {
	trimmed := strings.TrimSpace(text)
	if trimmed == "" || trimmed == "{}" {
		return []any{}
	}
	if strings.HasPrefix(trimmed, "{") && strings.HasSuffix(trimmed, "}") {
		trimmed = trimmed[1 : len(trimmed)-1]
	}
	return splitArrayItems(trimmed)
}

func splitArrayItems(text string) []any {
	var items []any
	depth := 0
	var sb strings.Builder
	for _, ch := range text {
		switch ch {
		case '{':
			depth++
			sb.WriteRune(ch)
		case '}':
			if depth > 0 {
				depth--
			}
			sb.WriteRune(ch)
		case ',':
			if depth == 0 {
				items = append(items, parseArrayItem(sb.String()))
				sb.Reset()
			} else {
				sb.WriteRune(ch)
			}
		default:
			sb.WriteRune(ch)
		}
	}
	if sb.Len() > 0 || text != "" {
		items = append(items, parseArrayItem(sb.String()))
	}
	return items
}

func parseArrayItem(raw string) any {
	token := strings.TrimSpace(raw)
	if strings.EqualFold(token, "NULL") {
		return nil
	}
	if strings.HasPrefix(token, "{") && strings.HasSuffix(token, "}") {
		return parseArrayLiteral(token)
	}
	if strings.HasPrefix(token, "[") && strings.HasSuffix(token, "]") {
		return parseVectorLiteral(token)
	}
	if token == "" {
		return ""
	}
	if token == "true" || token == "false" {
		return token == "true"
	}
	if i, err := strconv.Atoi(token); err == nil {
		return i
	}
	if f, err := strconv.ParseFloat(token, 64); err == nil {
		return f
	}
	return token
}

func parseVectorLiteral(text string) []float32 {
	trimmed := strings.TrimSpace(text)
	if strings.HasPrefix(trimmed, "[") && strings.HasSuffix(trimmed, "]") {
		trimmed = trimmed[1 : len(trimmed)-1]
	}
	if trimmed == "" {
		return []float32{}
	}
	parts := strings.Split(trimmed, ",")
	out := make([]float32, 0, len(parts))
	for _, part := range parts {
		val, err := strconv.ParseFloat(strings.TrimSpace(part), 32)
		if err != nil {
			out = append(out, 0)
		} else {
			out = append(out, float32(val))
		}
	}
	return out
}

func formatArrayLiteral(values []any) string {
	items := make([]string, 0, len(values))
	for _, v := range values {
		items = append(items, formatArrayItem(v))
	}
	return "{" + strings.Join(items, ",") + "}"
}

func formatArrayItem(value any) string {
	if value == nil {
		return "NULL"
	}
	switch v := value.(type) {
	case string:
		return "\"" + strings.ReplaceAll(v, "\"", "\\\"") + "\""
	case []any:
		return formatArrayLiteral(v)
	case []float32:
		return formatVectorLiteral(v)
	case []float64:
		return formatVectorLiteral64(v)
	case fmt.Stringer:
		return v.String()
	default:
		return fmt.Sprintf("%v", value)
	}
}

func formatVectorLiteral(values []float32) string {
	parts := make([]string, 0, len(values))
	for _, v := range values {
		parts = append(parts, strconv.FormatFloat(float64(v), 'f', -1, 32))
	}
	return "[" + strings.Join(parts, ",") + "]"
}

func formatVectorLiteral64(values []float64) string {
	parts := make([]string, 0, len(values))
	for _, v := range values {
		parts = append(parts, strconv.FormatFloat(v, 'f', -1, 64))
	}
	return "[" + strings.Join(parts, ",") + "]"
}

func oidName(oid uint32) string {
	switch oid {
	case oidBool:
		return "boolean"
	case oidInt2:
		return "int2"
	case oidInt4:
		return "int4"
	case oidInt8:
		return "int8"
	case oidFloat4:
		return "float4"
	case oidFloat8:
		return "float8"
	case oidNumeric:
		return "numeric"
	case oidMoney:
		return "money"
	case oidText:
		return "text"
	case oidVarchar:
		return "varchar"
	case oidChar, oidBPChar:
		return "char"
	case oidBytea:
		return "bytea"
	case oidDate:
		return "date"
	case oidTime:
		return "time"
	case oidTimetz:
		return "timetz"
	case oidTimestamp:
		return "timestamp"
	case oidTimestamptz:
		return "timestamptz"
	case oidInterval:
		return "interval"
	case oidUUID:
		return "uuid"
	case oidJSON:
		return "json"
	case oidJSONB:
		return "jsonb"
	case oidXML:
		return "xml"
	case oidInet:
		return "inet"
	case oidCidr:
		return "cidr"
	case oidMacaddr:
		return "macaddr"
	case oidMacaddr8:
		return "macaddr8"
	case oidTSVector:
		return "tsvector"
	case oidTSQuery:
		return "tsquery"
	case oidInt4Range:
		return "int4range"
	case oidInt8Range:
		return "int8range"
	case oidNumRange:
		return "numrange"
	case oidTSRange:
		return "tsrange"
	case oidTstzRange:
		return "tstzrange"
	case oidDateRange:
		return "daterange"
	case oidSBVector:
		return "vector"
	case oidPoint:
		return "point"
	case oidLseg:
		return "lseg"
	case oidPath:
		return "path"
	case oidBox:
		return "box"
	case oidPolygon:
		return "polygon"
	case oidLine:
		return "line"
	case oidCircle:
		return "circle"
	default:
		return "unknown"
	}
}

func scanTypeForOID(oid uint32) reflect.Type {
	switch oid {
	case oidBool:
		return reflect.TypeOf(false)
	case oidInt2:
		return reflect.TypeOf(int16(0))
	case oidInt4:
		return reflect.TypeOf(int32(0))
	case oidInt8:
		return reflect.TypeOf(int64(0))
	case oidFloat4:
		return reflect.TypeOf(float32(0))
	case oidFloat8:
		return reflect.TypeOf(float64(0))
	case oidBytea:
		return reflect.TypeOf([]byte{})
	case oidDate, oidTimestamp, oidTimestamptz:
		return reflect.TypeOf(time.Time{})
	case oidTime:
		return reflect.TypeOf(time.Duration(0))
	case oidTimetz:
		return reflect.TypeOf(TimeTZ{})
	case oidInterval:
		return reflect.TypeOf(Interval{})
	case oidJSONB:
		return reflect.TypeOf(JSONB{})
	case oidSBVector:
		return reflect.TypeOf([]float32{})
	case oidPoint, oidLseg, oidPath, oidBox, oidPolygon, oidLine, oidCircle:
		return reflect.TypeOf(Geometry{})
	default:
		return reflect.TypeOf("")
	}
}

const (
	int32RangeOID       = oidInt4Range
	int64RangeOID       = oidInt8Range
	decimalRangeOID     = oidNumRange
	dateRangeOID        = oidDateRange
	timestampRangeOID   = oidTSRange
	timestamptzRangeOID = oidTstzRange
)

func encodeRange[T any](rangeOID uint32, r Range[T]) ([]byte, error) {
	flags := byte(0)
	if r.Empty {
		flags |= rangeEmpty
	}
	if r.LowerInclusive {
		flags |= rangeLbInc
	}
	if r.UpperInclusive {
		flags |= rangeUbInc
	}
	if r.LowerInfinite {
		flags |= rangeLbInf
	}
	if r.UpperInfinite {
		flags |= rangeUbInf
	}
	buf := bytes.NewBuffer(nil)
	buf.WriteByte(flags)
	buf.Write([]byte{0, 0, 0})
	if !r.Empty && !r.LowerInfinite {
		bound, err := encodeRangeBound(rangeOID, r.Lower)
		if err != nil {
			return nil, err
		}
		writeInt32(buf, int32(len(bound)))
		buf.Write(bound)
	}
	if !r.Empty && !r.UpperInfinite {
		bound, err := encodeRangeBound(rangeOID, r.Upper)
		if err != nil {
			return nil, err
		}
		writeInt32(buf, int32(len(bound)))
		buf.Write(bound)
	}
	return buf.Bytes(), nil
}

func encodeRangeBound[T any](rangeOID uint32, value T) ([]byte, error) {
	switch rangeOID {
	case oidInt4Range:
		v, ok := any(value).(int32)
		if !ok {
			return nil, errors.New("int4range requires int32 bounds")
		}
		buf := make([]byte, 4)
		binary.LittleEndian.PutUint32(buf, uint32(v))
		return buf, nil
	case oidInt8Range:
		v, ok := any(value).(int64)
		if !ok {
			return nil, errors.New("int8range requires int64 bounds")
		}
		buf := make([]byte, 8)
		binary.LittleEndian.PutUint64(buf, uint64(v))
		return buf, nil
	case oidNumRange:
		v, ok := any(value).(Decimal)
		if !ok {
			return nil, errors.New("numrange requires Decimal bounds")
		}
		return encodeLengthPrefixed([]byte(v.Value)), nil
	case oidDateRange:
		v, ok := any(value).(Date)
		if !ok {
			return nil, errors.New("daterange requires Date bounds")
		}
		return encodeDate(v.Time), nil
	case oidTSRange:
		v, ok := any(value).(Timestamp)
		if !ok {
			return nil, errors.New("tsrange requires Timestamp bounds")
		}
		return encodeTimestamp(v.Time), nil
	case oidTstzRange:
		v, ok := any(value).(TimestampTZ)
		if !ok {
			return nil, errors.New("tstzrange requires TimestampTZ bounds")
		}
		return encodeTimestamp(v.Time), nil
	default:
		return nil, errors.New("unsupported range type")
	}
}

func decodeRange(rangeOID uint32, data []byte) (any, error) {
	if len(data) < 4 {
		return Range[any]{}, nil
	}
	flags := data[0]
	offset := 4
	result := Range[any]{
		Empty:          (flags & rangeEmpty) != 0,
		LowerInclusive: (flags & rangeLbInc) != 0,
		UpperInclusive: (flags & rangeUbInc) != 0,
		LowerInfinite:  (flags & rangeLbInf) != 0,
		UpperInfinite:  (flags & rangeUbInf) != 0,
	}
	if result.Empty {
		return result, nil
	}
	if !result.LowerInfinite {
		if offset+4 > len(data) {
			return result, errors.New("range lower bound truncated")
		}
		length := int(int32(binary.LittleEndian.Uint32(data[offset : offset+4])))
		offset += 4
		if offset+length > len(data) {
			return result, errors.New("range lower bound truncated")
		}
		bound := data[offset : offset+length]
		offset += length
		value, err := decodeRangeBound(rangeOID, bound)
		if err != nil {
			return result, err
		}
		result.Lower = value
	}
	if !result.UpperInfinite {
		if offset+4 > len(data) {
			return result, errors.New("range upper bound truncated")
		}
		length := int(int32(binary.LittleEndian.Uint32(data[offset : offset+4])))
		offset += 4
		if offset+length > len(data) {
			return result, errors.New("range upper bound truncated")
		}
		bound := data[offset : offset+length]
		value, err := decodeRangeBound(rangeOID, bound)
		if err != nil {
			return result, err
		}
		result.Upper = value
	}
	return result, nil
}

func decodeRangeBound(rangeOID uint32, data []byte) (any, error) {
	switch rangeOID {
	case oidInt4Range:
		if len(data) < 4 {
			return int32(0), nil
		}
		return int32(binary.LittleEndian.Uint32(data)), nil
	case oidInt8Range:
		if len(data) < 8 {
			return int64(0), nil
		}
		return int64(binary.LittleEndian.Uint64(data)), nil
	case oidNumRange:
		return Decimal{Value: string(stripLengthPrefix(data))}, nil
	case oidDateRange:
		return Date{Time: decodeDate(data)}, nil
	case oidTSRange:
		return Timestamp{Time: decodeTimestamp(data)}, nil
	case oidTstzRange:
		return TimestampTZ{Time: decodeTimestamp(data)}, nil
	default:
		return nil, errors.New("unsupported range bound")
	}
}

func writeInt32(buf *bytes.Buffer, value int32) {
	var tmp [4]byte
	binary.LittleEndian.PutUint32(tmp[:], uint32(value))
	buf.Write(tmp[:])
}
