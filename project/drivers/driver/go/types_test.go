// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

package scratchbird

import (
	"encoding/binary"
	"math"
	"net"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestEncodeParamRepresentativeValues(t *testing.T) {
	boolParam, boolOID, err := encodeParam(true)
	if err != nil {
		t.Fatalf("encode bool failed: %v", err)
	}
	if boolOID != oidBool {
		t.Fatalf("bool oid mismatch: got %d want %d", boolOID, oidBool)
	}
	if boolParam.null || len(boolParam.data) != 1 || boolParam.data[0] != 1 {
		t.Fatalf("bool encoding mismatch: %#v", boolParam)
	}

	intParam, intOID, err := encodeParam(int32(42))
	if err != nil {
		t.Fatalf("encode int32 failed: %v", err)
	}
	if intOID != oidInt4 {
		t.Fatalf("int32 oid mismatch: got %d want %d", intOID, oidInt4)
	}
	if got := int32(binary.LittleEndian.Uint32(intParam.data)); got != 42 {
		t.Fatalf("int32 payload mismatch: got %d want 42", got)
	}

	jsonbParam, jsonbOID, err := encodeParam(JSONB{Value: map[string]any{"k": 1}})
	if err != nil {
		t.Fatalf("encode jsonb failed: %v", err)
	}
	if jsonbOID != oidJSONB {
		t.Fatalf("jsonb oid mismatch: got %d want %d", jsonbOID, oidJSONB)
	}
	decodedJSONBAny, err := decodeBinaryValue(oidJSONB, jsonbParam.data)
	if err != nil {
		t.Fatalf("decode jsonb failed: %v", err)
	}
	decodedJSONB, ok := decodedJSONBAny.(JSONB)
	if !ok {
		t.Fatalf("decoded jsonb type mismatch: %T", decodedJSONBAny)
	}
	if got := string(decodedJSONB.Raw); got != `{"k":1}` {
		t.Fatalf("jsonb payload mismatch: got %q", got)
	}

	rangeParam, rangeOID, err := encodeParam(Range[int64]{Lower: 10, Upper: 20})
	if err != nil {
		t.Fatalf("encode int8 range failed: %v", err)
	}
	if rangeOID != oidInt8Range {
		t.Fatalf("range oid mismatch: got %d want %d", rangeOID, oidInt8Range)
	}
	decodedRangeAny, err := decodeBinaryValue(oidInt8Range, rangeParam.data)
	if err != nil {
		t.Fatalf("decode range failed: %v", err)
	}
	decodedRange, ok := decodedRangeAny.(Range[any])
	if !ok {
		t.Fatalf("decoded range type mismatch: %T", decodedRangeAny)
	}
	if got, ok := decodedRange.Lower.(int64); !ok || got != 10 {
		t.Fatalf("range lower mismatch: %#v", decodedRange.Lower)
	}
	if got, ok := decodedRange.Upper.(int64); !ok || got != 20 {
		t.Fatalf("range upper mismatch: %#v", decodedRange.Upper)
	}

	compositeParam, compositeOID, err := encodeParam(Composite{
		Fields: []CompositeField{{OID: oidInt4, Value: int32(7)}},
	})
	if err != nil {
		t.Fatalf("encode composite failed: %v", err)
	}
	if compositeOID != oidRecord {
		t.Fatalf("composite oid mismatch: got %d want %d", compositeOID, oidRecord)
	}
	decodedCompositeAny, err := decodeBinaryValue(oidRecord, compositeParam.data)
	if err != nil {
		t.Fatalf("decode composite failed: %v", err)
	}
	decodedComposite, ok := decodedCompositeAny.(Composite)
	if !ok {
		t.Fatalf("decoded composite type mismatch: %T", decodedCompositeAny)
	}
	if len(decodedComposite.Fields) != 1 {
		t.Fatalf("composite field count mismatch: got %d", len(decodedComposite.Fields))
	}
	if got, ok := decodedComposite.Fields[0].Value.(int32); !ok || got != 7 {
		t.Fatalf("composite field value mismatch: %#v", decodedComposite.Fields[0].Value)
	}

	vectorParam, vectorOID, err := encodeParam([]float32{1, 2.5})
	if err != nil {
		t.Fatalf("encode vector failed: %v", err)
	}
	if vectorOID != oidSBVector {
		t.Fatalf("vector oid mismatch: got %d want %d", vectorOID, oidSBVector)
	}
	if got := string(stripLengthPrefix(vectorParam.data)); got != "[1,2.5]" {
		t.Fatalf("vector payload mismatch: got %q", got)
	}
}

func TestEncodeParamRejectsInvalidInputs(t *testing.T) {
	if _, _, err := encodeParam(JSONB{}); err == nil || !strings.Contains(err.Error(), "JSONB requires raw payload") {
		t.Fatalf("expected JSONB raw payload error, got %v", err)
	}
	if _, _, err := encodeParam(Geometry{}); err == nil || !strings.Contains(err.Error(), "geometry requires WKB payload") {
		t.Fatalf("expected geometry WKB error, got %v", err)
	}

	type unsupported struct{}
	if _, _, err := encodeParam(unsupported{}); err == nil || !strings.Contains(err.Error(), "unsupported parameter type") {
		t.Fatalf("expected unsupported parameter type error, got %v", err)
	}
}

func TestDecodeBinaryValueRepresentativeTypes(t *testing.T) {
	numericAny, err := decodeBinaryValue(oidNumeric, encodeLengthPrefixed([]byte("12.34")))
	if err != nil {
		t.Fatalf("decode numeric failed: %v", err)
	}
	if got, ok := numericAny.(string); !ok || got != "12.34" {
		t.Fatalf("numeric mismatch: %#v", numericAny)
	}

	moneyBuf := make([]byte, 8)
	binary.LittleEndian.PutUint64(moneyBuf, uint64(12345))
	moneyAny, err := decodeBinaryValue(oidMoney, moneyBuf)
	if err != nil {
		t.Fatalf("decode money failed: %v", err)
	}
	if got, ok := moneyAny.(int64); !ok || got != 12345 {
		t.Fatalf("money mismatch: %#v", moneyAny)
	}

	uuidBytes := []byte{
		0x00, 0x11, 0x22, 0x33,
		0x44, 0x55,
		0x66, 0x77,
		0x88, 0x99,
		0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	}
	uuidAny, err := decodeBinaryValue(oidUUID, uuidBytes)
	if err != nil {
		t.Fatalf("decode uuid failed: %v", err)
	}
	if got, ok := uuidAny.(string); !ok || got != "00112233-4455-6677-8899-aabbccddeeff" {
		t.Fatalf("uuid mismatch: %#v", uuidAny)
	}

	byteaPayload := encodeLengthPrefixed([]byte{1, 2, 3})
	byteaAny, err := decodeBinaryValue(oidBytea, byteaPayload)
	if err != nil {
		t.Fatalf("decode bytea failed: %v", err)
	}
	bytea, ok := byteaAny.([]byte)
	if !ok {
		t.Fatalf("bytea type mismatch: %T", byteaAny)
	}
	bytea[0] = 9
	if got := stripLengthPrefix(byteaPayload)[0]; got != 1 {
		t.Fatalf("bytea decode should return copy, saw source mutation: %d", got)
	}

	vectorAny, err := decodeBinaryValue(oidSBVector, encodeLengthPrefixed([]byte("[0.5,1.5,2.5]")))
	if err != nil {
		t.Fatalf("decode vector failed: %v", err)
	}
	vector, ok := vectorAny.([]float32)
	if !ok {
		t.Fatalf("vector type mismatch: %T", vectorAny)
	}
	if !reflect.DeepEqual(vector, []float32{0.5, 1.5, 2.5}) {
		t.Fatalf("vector value mismatch: %#v", vector)
	}
}

func TestDecodeColumnValueUnknownTypeHeuristics(t *testing.T) {
	textCol := columnInfo{typeOID: 0, format: uint8(formatText)}
	parsedIntAny, err := decodeColumnValue(textCol, []byte("42"))
	if err != nil {
		t.Fatalf("decode unknown text int failed: %v", err)
	}
	if got, ok := parsedIntAny.(int32); !ok || got != 42 {
		t.Fatalf("unknown text int mismatch: %#v", parsedIntAny)
	}

	parsedBoolAny, err := decodeColumnValue(textCol, []byte("true"))
	if err != nil {
		t.Fatalf("decode unknown text bool failed: %v", err)
	}
	if got, ok := parsedBoolAny.(bool); !ok || !got {
		t.Fatalf("unknown text bool mismatch: %#v", parsedBoolAny)
	}

	binaryCol := columnInfo{typeOID: 0, format: uint8(formatBinary)}
	parsedArrayAny, err := decodeColumnValue(binaryCol, encodeLengthPrefixed([]byte("{1,2,3}")))
	if err != nil {
		t.Fatalf("decode unknown binary array failed: %v", err)
	}
	if !reflect.DeepEqual(parsedArrayAny, []any{1, 2, 3}) {
		t.Fatalf("unknown binary array mismatch: %#v", parsedArrayAny)
	}
}

func TestTypeMetadataHelpers(t *testing.T) {
	if got := oidName(oidSBVector); got != "vector" {
		t.Fatalf("oidName vector mismatch: %q", got)
	}
	if got := oidName(424242); got != "unknown" {
		t.Fatalf("oidName unknown mismatch: %q", got)
	}
	if got := scanTypeForOID(oidTime); got != reflect.TypeOf(time.Duration(0)) {
		t.Fatalf("scan type time mismatch: %v", got)
	}
	if got := scanTypeForOID(oidJSONB); got != reflect.TypeOf(JSONB{}) {
		t.Fatalf("scan type jsonb mismatch: %v", got)
	}
	if got := scanTypeForOID(oidTimetz); got != reflect.TypeOf(TimeTZ{}) {
		t.Fatalf("scan type timetz mismatch: %v", got)
	}
	if got := scanTypeForOID(oidPoint); got != reflect.TypeOf(Geometry{}) {
		t.Fatalf("scan type geometry mismatch: %v", got)
	}
	if got := scanTypeForOID(999999); got != reflect.TypeOf("") {
		t.Fatalf("scan type default mismatch: %v", got)
	}
}

func TestEncodeDecodeTimeTZ(t *testing.T) {
	param, oid, err := encodeParam(TimeTZ{Micros: 123456, OffsetSecondsWest: -3600})
	if err != nil {
		t.Fatalf("encode timetz failed: %v", err)
	}
	if oid != oidTimetz {
		t.Fatalf("timetz oid mismatch: got %d want %d", oid, oidTimetz)
	}
	decodedAny, err := decodeBinaryValue(oidTimetz, param.data)
	if err != nil {
		t.Fatalf("decode timetz failed: %v", err)
	}
	decoded, ok := decodedAny.(TimeTZ)
	if !ok {
		t.Fatalf("decoded timetz type mismatch: %T", decodedAny)
	}
	if decoded.Micros != 123456 || decoded.OffsetSecondsWest != -3600 {
		t.Fatalf("decoded timetz mismatch: %#v", decoded)
	}
}

func TestDecodeBinaryValueOIDMatrix(t *testing.T) {
	ts := time.Date(2026, time.March, 6, 12, 34, 56, 0, time.UTC)
	dateVal := time.Date(2026, time.March, 6, 0, 0, 0, 0, time.UTC)
	intervalVal := Interval{Micros: 1200, Days: 2, Months: 3}
	ipVal := net.ParseIP("127.0.0.1")

	int2 := make([]byte, 2)
	binary.LittleEndian.PutUint16(int2, uint16(42))
	int4 := make([]byte, 4)
	binary.LittleEndian.PutUint32(int4, uint32(42))
	int8 := make([]byte, 8)
	binary.LittleEndian.PutUint64(int8, uint64(42))
	float4 := make([]byte, 4)
	binary.LittleEndian.PutUint32(float4, math.Float32bits(1.5))
	float8 := make([]byte, 8)
	binary.LittleEndian.PutUint64(float8, math.Float64bits(2.5))
	money := make([]byte, 8)
	binary.LittleEndian.PutUint64(money, uint64(99))

	tests := []struct {
		name    string
		oid     uint32
		payload []byte
		assert  func(t *testing.T, got any)
	}{
		{"bool", oidBool, []byte{1}, func(t *testing.T, got any) { assertEqual(t, got, true) }},
		{"int2", oidInt2, int2, func(t *testing.T, got any) { assertEqual(t, got, int16(42)) }},
		{"int4", oidInt4, int4, func(t *testing.T, got any) { assertEqual(t, got, int32(42)) }},
		{"int8", oidInt8, int8, func(t *testing.T, got any) { assertEqual(t, got, int64(42)) }},
		{"float4", oidFloat4, float4, func(t *testing.T, got any) { assertEqual(t, got, float32(1.5)) }},
		{"float8", oidFloat8, float8, func(t *testing.T, got any) { assertEqual(t, got, float64(2.5)) }},
		{"numeric", oidNumeric, encodeLengthPrefixed([]byte("12.34")), func(t *testing.T, got any) { assertEqual(t, got, "12.34") }},
		{"money", oidMoney, money, func(t *testing.T, got any) { assertEqual(t, got, int64(99)) }},
		{"text", oidText, encodeLengthPrefixed([]byte("hello")), func(t *testing.T, got any) { assertEqual(t, got, "hello") }},
		{"char", oidChar, encodeLengthPrefixed([]byte("c")), func(t *testing.T, got any) { assertEqual(t, got, "c") }},
		{"bpchar", oidBPChar, encodeLengthPrefixed([]byte("bp")), func(t *testing.T, got any) { assertEqual(t, got, "bp") }},
		{"json", oidJSON, encodeLengthPrefixed([]byte(`{"k":1}`)), func(t *testing.T, got any) { assertEqual(t, got, `{"k":1}`) }},
		{"jsonb", oidJSONB, encodeLengthPrefixed([]byte(`{"k":1}`)), func(t *testing.T, got any) {
			v, ok := got.(JSONB)
			if !ok || string(v.Raw) != `{"k":1}` {
				t.Fatalf("jsonb mismatch: %#v", got)
			}
		}},
		{"xml", oidXML, encodeLengthPrefixed([]byte("<x/>")), func(t *testing.T, got any) { assertEqual(t, got, "<x/>") }},
		{"bytea", oidBytea, encodeLengthPrefixed([]byte{0x61, 0x62}), func(t *testing.T, got any) { assertEqual(t, got, []byte{0x61, 0x62}) }},
		{"date", oidDate, encodeDate(dateVal), func(t *testing.T, got any) { assertEqual(t, got, dateVal) }},
		{"time", oidTime, encodeTimeMicros(12345), func(t *testing.T, got any) { assertEqual(t, got, 12345*time.Microsecond) }},
		{"timetz", oidTimetz, encodeTimetz(TimeTZ{Micros: 12345, OffsetSecondsWest: -3600}), func(t *testing.T, got any) {
			assertEqual(t, got, TimeTZ{Micros: 12345, OffsetSecondsWest: -3600})
		}},
		{"timestamp", oidTimestamp, encodeTimestamp(ts), func(t *testing.T, got any) { assertEqual(t, got, ts) }},
		{"timestamptz", oidTimestamptz, encodeTimestamp(ts), func(t *testing.T, got any) { assertEqual(t, got, ts) }},
		{"interval", oidInterval, encodeInterval(intervalVal), func(t *testing.T, got any) { assertEqual(t, got, intervalVal) }},
		{"uuid", oidUUID, []byte{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, func(t *testing.T, got any) {
			assertEqual(t, got, "00010203-0405-0607-0809-0a0b0c0d0e0f")
		}},
		{"inet", oidInet, encodeLengthPrefixed([]byte(ipVal.String())), func(t *testing.T, got any) { assertEqual(t, got, "127.0.0.1") }},
		{"cidr", oidCidr, encodeLengthPrefixed([]byte("127.0.0.0/24")), func(t *testing.T, got any) { assertEqual(t, got, "127.0.0.0/24") }},
		{"macaddr", oidMacaddr, encodeLengthPrefixed([]byte("08:00:2b:01:02:03")), func(t *testing.T, got any) { assertEqual(t, got, "08:00:2b:01:02:03") }},
		{"tsvector", oidTSVector, encodeLengthPrefixed([]byte("'fat':2")), func(t *testing.T, got any) { assertEqual(t, got, "'fat':2") }},
		{"tsquery", oidTSQuery, encodeLengthPrefixed([]byte("fat & rat")), func(t *testing.T, got any) { assertEqual(t, got, "fat & rat") }},
		{"int4range", oidInt4Range, mustEncodeRange(t, Range[int32]{Lower: 1, Upper: 2}), func(t *testing.T, got any) {
			r, ok := got.(Range[any])
			if !ok {
				t.Fatalf("range type mismatch: %T", got)
			}
			assertEqual(t, r.Lower, int32(1))
			assertEqual(t, r.Upper, int32(2))
		}},
		{"vector", oidSBVector, encodeLengthPrefixed([]byte("[1,2,3]")), func(t *testing.T, got any) {
			assertEqual(t, got, []float32{1, 2, 3})
		}},
		{"record", oidRecord, mustEncodeComposite(t), func(t *testing.T, got any) {
			comp, ok := got.(Composite)
			if !ok || len(comp.Fields) != 1 {
				t.Fatalf("composite mismatch: %#v", got)
			}
		}},
		{"geometry", oidPoint, encodeLengthPrefixed([]byte{0x01, 0x02}), func(t *testing.T, got any) {
			g, ok := got.(Geometry)
			if !ok || !reflect.DeepEqual(g.WKB, []byte{0x01, 0x02}) {
				t.Fatalf("geometry mismatch: %#v", got)
			}
		}},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got, err := decodeBinaryValue(tc.oid, tc.payload)
			if err != nil {
				t.Fatalf("decode failed: %v", err)
			}
			tc.assert(t, got)
		})
	}
}

func assertEqual(t *testing.T, got, want any) {
	t.Helper()
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("value mismatch: got %#v want %#v", got, want)
	}
}

func mustEncodeRange(t *testing.T, value Range[int32]) []byte {
	t.Helper()
	data, err := encodeRange(int32RangeOID, value)
	if err != nil {
		t.Fatalf("encode range failed: %v", err)
	}
	return data
}

func mustEncodeComposite(t *testing.T) []byte {
	t.Helper()
	data, _, err := encodeComposite(Composite{Fields: []CompositeField{{OID: oidInt4, Value: int32(7)}}})
	if err != nil {
		t.Fatalf("encode composite failed: %v", err)
	}
	return data
}
