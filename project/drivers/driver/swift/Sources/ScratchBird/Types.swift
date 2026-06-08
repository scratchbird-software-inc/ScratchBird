// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

enum TypeOid {
    static let bool: UInt32 = 16
    static let bytea: UInt32 = 17
    static let char: UInt32 = 18
    static let int8: UInt32 = 20
    static let int2: UInt32 = 21
    static let int4: UInt32 = 23
    static let text: UInt32 = 25
    static let json: UInt32 = 114
    static let xml: UInt32 = 142
    static let point: UInt32 = 600
    static let float4: UInt32 = 700
    static let float8: UInt32 = 701
    static let money: UInt32 = 790
    static let cidr: UInt32 = 650
    static let inet: UInt32 = 869
    static let macaddr: UInt32 = 829
    static let bpchar: UInt32 = 1042
    static let varchar: UInt32 = 1043
    static let date: UInt32 = 1082
    static let time: UInt32 = 1083
    static let timestamp: UInt32 = 1114
    static let timestamptz: UInt32 = 1184
    static let interval: UInt32 = 1186
    static let numeric: UInt32 = 1700
    static let uuid: UInt32 = 2950
    static let jsonb: UInt32 = 3802
    static let record: UInt32 = 2249
    static let int4range: UInt32 = 3904
    static let numrange: UInt32 = 3906
    static let tsrange: UInt32 = 3908
    static let tstzrange: UInt32 = 3910
    static let daterange: UInt32 = 3912
    static let int8range: UInt32 = 3926
    static let tsvector: UInt32 = 3614
    static let tsquery: UInt32 = 3615
    static let sbVector: UInt32 = 16386
}

struct Jsonb { let raw: Data }
struct Json { let raw: Data }
struct Geometry { let wkb: Data }
struct Interval { let micros: Int64; let days: Int32; let months: Int32 }
struct RawValue { let oid: UInt32; let data: Data }
struct ScratchBirdInet { let value: String }
struct ScratchBirdCidr { let value: String }
struct ScratchBirdMacaddr { let value: String }

struct ScratchBirdCompositeField {
    var oid: UInt32?
    var value: Any?
    var raw: Data?
}

struct ScratchBirdComposite {
    var typeOid: UInt32
    var fields: [ScratchBirdCompositeField]

    init(typeOid: UInt32 = TypeOid.record, fields: [ScratchBirdCompositeField]) {
        self.typeOid = typeOid
        self.fields = fields
    }
}

struct ScratchBirdRange {
    var lower: Any?
    var upper: Any?
    var lowerInclusive: Bool
    var upperInclusive: Bool
    var lowerInfinite: Bool
    var upperInfinite: Bool
    var empty: Bool
    var rangeOid: UInt32?

    init(
        lower: Any? = nil,
        upper: Any? = nil,
        lowerInclusive: Bool = true,
        upperInclusive: Bool = false,
        lowerInfinite: Bool = false,
        upperInfinite: Bool = false,
        empty: Bool = false,
        rangeOid: UInt32? = nil
    ) {
        self.lower = lower
        self.upper = upper
        self.lowerInclusive = lowerInclusive
        self.upperInclusive = upperInclusive
        self.lowerInfinite = lowerInfinite
        self.upperInfinite = upperInfinite
        self.empty = empty
        self.rangeOid = rangeOid
    }
}

struct ParamEncoding {
    let param: ParamValue
    let oid: UInt32
}

func encodeParam(_ value: Any?) throws -> ParamEncoding {
    guard let value = value else {
        return ParamEncoding(param: ParamValue(format: 1, data: nil, isNull: true), oid: 0)
    }
    if let comp = value as? ScratchBirdComposite {
        let encoded = try encodeComposite(comp)
        return ParamEncoding(param: ParamValue(format: 1, data: encoded.data, isNull: false), oid: encoded.oid)
    }
    if let range = value as? ScratchBirdRange {
        let encoded = try encodeRange(range)
        return ParamEncoding(param: ParamValue(format: 1, data: encoded.data, isNull: false), oid: encoded.oid)
    }
    if let raw = value as? RawValue {
        return ParamEncoding(param: ParamValue(format: 1, data: raw.data, isNull: false), oid: raw.oid)
    }
    if let jsonb = value as? Jsonb {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(jsonb.raw), isNull: false), oid: TypeOid.jsonb)
    }
    if let json = value as? Json {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(json.raw), isNull: false), oid: TypeOid.json)
    }
    if let geom = value as? Geometry {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(geom.wkb), isNull: false), oid: TypeOid.point)
    }
    if let interval = value as? Interval {
        var data = Data()
        data.append(contentsOf: withUnsafeBytes(of: interval.micros.littleEndian, Array.init))
        data.append(contentsOf: withUnsafeBytes(of: interval.days.littleEndian, Array.init))
        data.append(contentsOf: withUnsafeBytes(of: interval.months.littleEndian, Array.init))
        return ParamEncoding(param: ParamValue(format: 1, data: data, isNull: false), oid: TypeOid.interval)
    }
    if let inet = value as? ScratchBirdInet {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(inet.value.utf8)), isNull: false), oid: TypeOid.inet)
    }
    if let cidr = value as? ScratchBirdCidr {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(cidr.value.utf8)), isNull: false), oid: TypeOid.cidr)
    }
    if let macaddr = value as? ScratchBirdMacaddr {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(macaddr.value.utf8)), isNull: false), oid: TypeOid.macaddr)
    }
    if let boolVal = value as? Bool {
        return ParamEncoding(param: ParamValue(format: 1, data: Data([boolVal ? 1 : 0]), isNull: false), oid: TypeOid.bool)
    }
    if let intVal = value as? Int {
        if intVal >= -32768 && intVal <= 32767 {
            var v = Int16(intVal).littleEndian
            return ParamEncoding(param: ParamValue(format: 1, data: Data(bytes: &v, count: 2), isNull: false), oid: TypeOid.int2)
        }
        if intVal >= Int(Int32.min) && intVal <= Int(Int32.max) {
            var v = Int32(intVal).littleEndian
            return ParamEncoding(param: ParamValue(format: 1, data: Data(bytes: &v, count: 4), isNull: false), oid: TypeOid.int4)
        }
        var v = Int64(intVal).littleEndian
        return ParamEncoding(param: ParamValue(format: 1, data: Data(bytes: &v, count: 8), isNull: false), oid: TypeOid.int8)
    }
    if let doubleVal = value as? Double {
        var v = doubleVal.bitPattern.littleEndian
        return ParamEncoding(param: ParamValue(format: 1, data: Data(bytes: &v, count: 8), isNull: false), oid: TypeOid.float8)
    }
    if let dateVal = value as? Date {
        let base = DateComponents(calendar: Calendar(identifier: .gregorian), timeZone: TimeZone(secondsFromGMT: 0), year: 2000, month: 1, day: 1).date!
        let micros = Int64(dateVal.timeIntervalSince(base) * 1_000_000)
        var v = micros.littleEndian
        return ParamEncoding(param: ParamValue(format: 1, data: Data(bytes: &v, count: 8), isNull: false), oid: TypeOid.timestamptz)
    }
    if let dataVal = value as? Data {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(dataVal), isNull: false), oid: TypeOid.bytea)
    }
    if let vector = value as? [Float] {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(formatVectorLiteral(vector).utf8)), isNull: false), oid: TypeOid.sbVector)
    }
    if let vector = value as? [Double] {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(formatVectorLiteral(vector).utf8)), isNull: false), oid: TypeOid.sbVector)
    }
    if let arrayVal = value as? [Any] {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(formatArrayLiteral(arrayVal).utf8)), isNull: false), oid: 0)
    }
    if let arrayVal = value as? [String] {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(formatArrayLiteral(arrayVal.map { $0 as Any }).utf8)), isNull: false), oid: 0)
    }
    if let arrayVal = value as? [Int] {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(formatArrayLiteral(arrayVal.map { $0 as Any }).utf8)), isNull: false), oid: 0)
    }
    if let arrayVal = value as? [Int32] {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(formatArrayLiteral(arrayVal.map { $0 as Any }).utf8)), isNull: false), oid: 0)
    }
    if let arrayVal = value as? [Int64] {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(formatArrayLiteral(arrayVal.map { $0 as Any }).utf8)), isNull: false), oid: 0)
    }
    if let arrayVal = value as? [Bool] {
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(formatArrayLiteral(arrayVal.map { $0 as Any }).utf8)), isNull: false), oid: 0)
    }
    if let strVal = value as? String {
        if let uuidBytes = uuidToBytes(strVal) {
            return ParamEncoding(param: ParamValue(format: 1, data: uuidBytes, isNull: false), oid: TypeOid.uuid)
        }
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(Data(strVal.utf8)), isNull: false), oid: TypeOid.text)
    }
    if let obj = value as? [String: Any] {
        let data = try JSONSerialization.data(withJSONObject: obj)
        return ParamEncoding(param: ParamValue(format: 1, data: lengthPrefixed(data), isNull: false), oid: TypeOid.json)
    }
    throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "Unsupported parameter type"])
}

func decodeValue(oid: UInt32, data: Data, format: UInt16) -> Any {
    if format == 0 {
        let text = String(data: data, encoding: .utf8) ?? ""
        if oid == 0 {
            return parseUnknownText(text)
        }
        return text
    }
    switch oid {
    case TypeOid.bool:
        guard let first = data.first else { return RawValue(oid: oid, data: data) }
        return first == 1
    case TypeOid.int2:
        guard data.count >= 2 else { return RawValue(oid: oid, data: data) }
        return Int(Int16(littleEndian: data.withUnsafeBytes { $0.load(as: Int16.self) }))
    case TypeOid.int4:
        guard data.count >= 4 else { return RawValue(oid: oid, data: data) }
        return Int(Int32(littleEndian: data.withUnsafeBytes { $0.load(as: Int32.self) }))
    case TypeOid.int8:
        guard data.count >= 8 else { return RawValue(oid: oid, data: data) }
        return Int64(littleEndian: data.withUnsafeBytes { $0.load(as: Int64.self) })
    case TypeOid.float4:
        guard data.count >= 4 else { return RawValue(oid: oid, data: data) }
        let bits = UInt32(littleEndian: data.withUnsafeBytes { $0.load(as: UInt32.self) })
        return Float(bitPattern: bits)
    case TypeOid.float8:
        guard data.count >= 8 else { return RawValue(oid: oid, data: data) }
        let bits = UInt64(littleEndian: data.withUnsafeBytes { $0.load(as: UInt64.self) })
        return Double(bitPattern: bits)
    case TypeOid.numeric:
        return String(data: stripLengthPrefix(data), encoding: .utf8) ?? ""
    case TypeOid.text, TypeOid.varchar, TypeOid.char, TypeOid.bpchar, TypeOid.json, TypeOid.xml, TypeOid.tsvector, TypeOid.tsquery:
        return String(data: stripLengthPrefix(data), encoding: .utf8) ?? ""
    case TypeOid.jsonb:
        return Jsonb(raw: stripLengthPrefix(data))
    case TypeOid.uuid:
        guard data.count >= 16 else { return RawValue(oid: oid, data: data) }
        return uuidFromBytes(data)
    case TypeOid.inet, TypeOid.cidr, TypeOid.macaddr:
        return String(data: stripLengthPrefix(data), encoding: .utf8) ?? ""
    case TypeOid.date:
        guard data.count >= 4 else { return RawValue(oid: oid, data: data) }
        let days = Int32(littleEndian: data.withUnsafeBytes { $0.load(as: Int32.self) })
        let base = Calendar(identifier: .gregorian).date(from: DateComponents(timeZone: TimeZone(secondsFromGMT: 0), year: 2000, month: 1, day: 1))!
        return Calendar(identifier: .gregorian).date(byAdding: .day, value: Int(days), to: base) ?? base
    case TypeOid.time:
        guard data.count >= 8 else { return RawValue(oid: oid, data: data) }
        return Int64(littleEndian: data.withUnsafeBytes { $0.load(as: Int64.self) })
    case TypeOid.timestamp, TypeOid.timestamptz:
        guard data.count >= 8 else { return RawValue(oid: oid, data: data) }
        let micros = Int64(littleEndian: data.withUnsafeBytes { $0.load(as: Int64.self) })
        let base = Calendar(identifier: .gregorian).date(from: DateComponents(timeZone: TimeZone(secondsFromGMT: 0), year: 2000, month: 1, day: 1))!
        return base.addingTimeInterval(Double(micros) / 1_000_000)
    case TypeOid.interval:
        guard data.count >= 16 else { return RawValue(oid: oid, data: data) }
        let micros = Int64(littleEndian: data.withUnsafeBytes { $0.load(as: Int64.self) })
        let days = Int32(littleEndian: data.subdata(in: 8..<12).withUnsafeBytes { $0.load(as: Int32.self) })
        let months = Int32(littleEndian: data.subdata(in: 12..<16).withUnsafeBytes { $0.load(as: Int32.self) })
        return Interval(micros: micros, days: days, months: months)
    case TypeOid.point:
        return Geometry(wkb: stripLengthPrefix(data))
    case TypeOid.record:
        return decodeComposite(data)
    case TypeOid.int4range, TypeOid.int8range, TypeOid.numrange, TypeOid.tsrange, TypeOid.tstzrange, TypeOid.daterange:
        return decodeRange(oid, data)
    case TypeOid.sbVector:
        return parseVectorLiteral(String(data: stripLengthPrefix(data), encoding: .utf8) ?? "")
    default:
        if oid == 0 {
            return decodeUnknownBinary(data)
        }
        return RawValue(oid: oid, data: data)
    }
}

func lengthPrefixed(_ data: Data) -> Data {
    var out = Data()
    var len = UInt32(data.count).littleEndian
    out.append(Data(bytes: &len, count: 4))
    out.append(data)
    return out
}

func stripLengthPrefix(_ data: Data) -> Data {
    if data.count < 4 { return data }
    let len = UInt32(littleEndian: data.withUnsafeBytes { $0.load(as: UInt32.self) })
    if data.count < 4 + Int(len) { return data }
    return data.subdata(in: 4..<(4 + Int(len)))
}

func uuidToBytes(_ value: String) -> Data? {
    let regex = try? NSRegularExpression(pattern: "^[0-9a-fA-F-]{36}$")
    if regex?.firstMatch(in: value, range: NSRange(location: 0, length: value.count)) == nil {
        return nil
    }
    let hex = value.replacingOccurrences(of: "-", with: "")
    var data = Data(capacity: 16)
    var index = hex.startIndex
    while index < hex.endIndex {
        let next = hex.index(index, offsetBy: 2)
        let byte = UInt8(hex[index..<next], radix: 16) ?? 0
        data.append(byte)
        index = next
    }
    return data
}

func uuidFromBytes(_ data: Data) -> String {
    let hex = data.map { String(format: "%02x", $0) }.joined()
    let p1 = hex.prefix(8)
    let p2 = hex.dropFirst(8).prefix(4)
    let p3 = hex.dropFirst(12).prefix(4)
    let p4 = hex.dropFirst(16).prefix(4)
    let p5 = hex.dropFirst(20)
    return "\(p1)-\(p2)-\(p3)-\(p4)-\(p5)"
}

private let rangeEmpty: UInt8 = 0x01
private let rangeLbInc: UInt8 = 0x02
private let rangeUbInc: UInt8 = 0x04
private let rangeLbInf: UInt8 = 0x08
private let rangeUbInf: UInt8 = 0x10

private func encodeValueForOid(_ oid: UInt32, value: Any) throws -> Data? {
    switch oid {
    case TypeOid.int2:
        if let v = value as? Int16 {
            var le = v.littleEndian
            return Data(bytes: &le, count: 2)
        }
        if let v = value as? Int {
            guard v >= Int(Int16.min) && v <= Int(Int16.max) else { return nil }
            var le = Int16(v).littleEndian
            return Data(bytes: &le, count: 2)
        }
        if let v = value as? Int32 {
            guard v >= Int32(Int16.min) && v <= Int32(Int16.max) else { return nil }
            var le = Int16(v).littleEndian
            return Data(bytes: &le, count: 2)
        }
        if let v = value as? Int64 {
            guard v >= Int64(Int16.min) && v <= Int64(Int16.max) else { return nil }
            var le = Int16(v).littleEndian
            return Data(bytes: &le, count: 2)
        }
    case TypeOid.int4:
        if let v = value as? Int32 {
            var le = v.littleEndian
            return Data(bytes: &le, count: 4)
        }
        if let v = value as? Int {
            guard v >= Int(Int32.min) && v <= Int(Int32.max) else { return nil }
            var le = Int32(v).littleEndian
            return Data(bytes: &le, count: 4)
        }
        if let v = value as? Int64 {
            guard v >= Int64(Int32.min) && v <= Int64(Int32.max) else { return nil }
            var le = Int32(v).littleEndian
            return Data(bytes: &le, count: 4)
        }
    case TypeOid.int8:
        if let v = value as? Int64 {
            var le = v.littleEndian
            return Data(bytes: &le, count: 8)
        }
        if let v = value as? Int {
            var le = Int64(v).littleEndian
            return Data(bytes: &le, count: 8)
        }
    default:
        return nil
    }
    return nil
}

private func encodeComposite(_ comp: ScratchBirdComposite) throws -> (data: Data, oid: UInt32) {
    let typeOid = comp.typeOid == 0 ? TypeOid.record : comp.typeOid
    var out = Data()
    let count = Int32(comp.fields.count).littleEndian
    out.append(contentsOf: withUnsafeBytes(of: count, Array.init))
    for field in comp.fields {
        var fieldOid = field.oid ?? 0
        var fieldData: Data?
        if let raw = field.raw {
            fieldData = raw
        } else if let value = field.value {
            if fieldOid != 0 {
                fieldData = try encodeValueForOid(fieldOid, value: value)
            }
            if fieldData == nil {
                let encoded = try encodeParam(value)
                if fieldOid == 0 {
                    fieldOid = encoded.oid
                }
                if !encoded.param.isNull {
                    fieldData = encoded.param.data ?? Data()
                }
            }
        }
        if fieldOid == 0 {
            throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "composite field OID is required"])
        }
        let oidLE = fieldOid.littleEndian
        out.append(contentsOf: withUnsafeBytes(of: oidLE, Array.init))
        if fieldData == nil {
            let len = Int32(-1).littleEndian
            out.append(contentsOf: withUnsafeBytes(of: len, Array.init))
            continue
        }
        let len = Int32(fieldData!.count).littleEndian
        out.append(contentsOf: withUnsafeBytes(of: len, Array.init))
        out.append(fieldData!)
    }
    return (out, typeOid)
}

private func decodeComposite(_ data: Data) -> ScratchBirdComposite {
    if data.count < 4 {
        return ScratchBirdComposite(fields: [])
    }
    let count = Int(Int32(littleEndian: data.withUnsafeBytes { $0.load(as: Int32.self) }))
    var offset = 4
    var fields: [ScratchBirdCompositeField] = []
    for _ in 0..<max(0, count) {
        if offset + 8 > data.count { break }
        let oid = UInt32(littleEndian: data.subdata(in: offset..<(offset + 4)).withUnsafeBytes { $0.load(as: UInt32.self) })
        offset += 4
        let length = Int(Int32(littleEndian: data.subdata(in: offset..<(offset + 4)).withUnsafeBytes { $0.load(as: Int32.self) }))
        offset += 4
        if length < 0 {
            fields.append(ScratchBirdCompositeField(oid: oid, value: nil, raw: nil))
            continue
        }
        if offset + length > data.count { break }
        let raw = data.subdata(in: offset..<(offset + length))
        offset += length
        let value = decodeValue(oid: oid, data: raw, format: 1)
        fields.append(ScratchBirdCompositeField(oid: oid, value: value, raw: raw))
    }
    return ScratchBirdComposite(fields: fields)
}

private func encodeRange(_ range: ScratchBirdRange) throws -> (data: Data, oid: UInt32) {
    let oid = try resolveRangeOid(range)
    var flags: UInt8 = 0
    if range.empty { flags |= rangeEmpty }
    if range.lowerInclusive { flags |= rangeLbInc }
    if range.upperInclusive { flags |= rangeUbInc }
    if range.lowerInfinite { flags |= rangeLbInf }
    if range.upperInfinite { flags |= rangeUbInf }

    var out = Data([flags, 0, 0, 0])
    if !range.empty && !range.lowerInfinite {
        let bound = try encodeRangeBound(oid: oid, value: range.lower)
        let len = Int32(bound.count).littleEndian
        out.append(contentsOf: withUnsafeBytes(of: len, Array.init))
        out.append(bound)
    }
    if !range.empty && !range.upperInfinite {
        let bound = try encodeRangeBound(oid: oid, value: range.upper)
        let len = Int32(bound.count).littleEndian
        out.append(contentsOf: withUnsafeBytes(of: len, Array.init))
        out.append(bound)
    }
    return (out, oid)
}

private func resolveRangeOid(_ range: ScratchBirdRange) throws -> UInt32 {
    if let oid = range.rangeOid {
        return oid
    }
    let sample = range.lower ?? range.upper
    if sample == nil {
        throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "range type cannot be inferred from empty bounds"])
    }
    switch sample {
    case _ as Int32:
        return TypeOid.int4range
    case let value as Int:
        if value >= Int(Int32.min) && value <= Int(Int32.max) {
            return TypeOid.int4range
        }
        return TypeOid.int8range
    case _ as Int64:
        return TypeOid.int8range
    case _ as Decimal:
        return TypeOid.numrange
    case _ as Date:
        return TypeOid.tstzrange
    default:
        throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "unsupported range bound type"])
    }
}

private func encodeRangeBound(oid: UInt32, value: Any?) throws -> Data {
    guard let value = value else {
        return Data()
    }
    switch oid {
    case TypeOid.int4range:
        if let v = value as? Int32 {
            var le = v.littleEndian
            return Data(bytes: &le, count: 4)
        }
        if let v = value as? Int {
            if v < Int(Int32.min) || v > Int(Int32.max) {
                throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "int4range requires int32 bounds"])
            }
            var le = Int32(v).littleEndian
            return Data(bytes: &le, count: 4)
        }
        throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "int4range requires int32 bounds"])
    case TypeOid.int8range:
        if let v = value as? Int64 {
            var le = v.littleEndian
            return Data(bytes: &le, count: 8)
        }
        if let v = value as? Int {
            var le = Int64(v).littleEndian
            return Data(bytes: &le, count: 8)
        }
        throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "int8range requires int64 bounds"])
    case TypeOid.numrange:
        if let v = value as? Decimal {
            return lengthPrefixed(Data(NSDecimalNumber(decimal: v).stringValue.utf8))
        }
        if let v = value as? String {
            return lengthPrefixed(Data(v.utf8))
        }
        throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "numrange requires decimal bounds"])
    case TypeOid.daterange:
        if let v = value as? Date {
            return encodeDate(v)
        }
        throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "daterange requires date bounds"])
    case TypeOid.tsrange:
        if let v = value as? Date {
            return encodeTimestamp(v)
        }
        throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "tsrange requires timestamp bounds"])
    case TypeOid.tstzrange:
        if let v = value as? Date {
            return encodeTimestamp(v)
        }
        throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "tstzrange requires timestamptz bounds"])
    default:
        throw NSError(domain: "ScratchBird", code: 0, userInfo: [NSLocalizedDescriptionKey: "unsupported range type"])
    }
}

private func decodeRange(_ oid: UInt32, _ data: Data) -> ScratchBirdRange {
    if data.count < 4 {
        return ScratchBirdRange(empty: true, rangeOid: oid)
    }
    let flags = data[0]
    var offset = 4
    var range = ScratchBirdRange(
        lowerInclusive: (flags & rangeLbInc) != 0,
        upperInclusive: (flags & rangeUbInc) != 0,
        lowerInfinite: (flags & rangeLbInf) != 0,
        upperInfinite: (flags & rangeUbInf) != 0,
        empty: (flags & rangeEmpty) != 0,
        rangeOid: oid
    )
    if range.empty {
        return range
    }
    if !range.lowerInfinite {
        if offset + 4 > data.count { return range }
        let length = Int(Int32(littleEndian: data.subdata(in: offset..<(offset + 4)).withUnsafeBytes { $0.load(as: Int32.self) }))
        offset += 4
        if length >= 0 && offset + length <= data.count {
            let bound = data.subdata(in: offset..<(offset + length))
            offset += length
            range.lower = decodeRangeBound(oid: oid, data: bound)
        }
    }
    if !range.upperInfinite {
        if offset + 4 > data.count { return range }
        let length = Int(Int32(littleEndian: data.subdata(in: offset..<(offset + 4)).withUnsafeBytes { $0.load(as: Int32.self) }))
        offset += 4
        if length >= 0 && offset + length <= data.count {
            let bound = data.subdata(in: offset..<(offset + length))
            range.upper = decodeRangeBound(oid: oid, data: bound)
        }
    }
    return range
}

private func decodeRangeBound(oid: UInt32, data: Data) -> Any? {
    switch oid {
    case TypeOid.int4range:
        if data.count < 4 { return Int(0) }
        return Int(Int32(littleEndian: data.withUnsafeBytes { $0.load(as: Int32.self) }))
    case TypeOid.int8range:
        if data.count < 8 { return Int64(0) }
        return Int64(littleEndian: data.withUnsafeBytes { $0.load(as: Int64.self) })
    case TypeOid.numrange:
        return String(data: stripLengthPrefix(data), encoding: .utf8) ?? ""
    case TypeOid.daterange:
        return decodeDate(data)
    case TypeOid.tsrange, TypeOid.tstzrange:
        return decodeTimestamp(data)
    default:
        return nil
    }
}

private func encodeDate(_ value: Date) -> Data {
    let base = Calendar(identifier: .gregorian).date(from: DateComponents(timeZone: TimeZone(secondsFromGMT: 0), year: 2000, month: 1, day: 1))!
    let days = Int32(value.timeIntervalSince(base) / 86400)
    var le = days.littleEndian
    return Data(bytes: &le, count: 4)
}

private func decodeDate(_ data: Data) -> Date {
    if data.count < 4 {
        return Date(timeIntervalSince1970: 0)
    }
    let days = Int32(littleEndian: data.withUnsafeBytes { $0.load(as: Int32.self) })
    let base = Calendar(identifier: .gregorian).date(from: DateComponents(timeZone: TimeZone(secondsFromGMT: 0), year: 2000, month: 1, day: 1))!
    return Calendar(identifier: .gregorian).date(byAdding: .day, value: Int(days), to: base) ?? base
}

private func encodeTimestamp(_ value: Date) -> Data {
    let base = Calendar(identifier: .gregorian).date(from: DateComponents(timeZone: TimeZone(secondsFromGMT: 0), year: 2000, month: 1, day: 1))!
    let micros = Int64(value.timeIntervalSince(base) * 1_000_000)
    var le = micros.littleEndian
    return Data(bytes: &le, count: 8)
}

private func decodeTimestamp(_ data: Data) -> Date {
    if data.count < 8 {
        return Date(timeIntervalSince1970: 0)
    }
    let micros = Int64(littleEndian: data.withUnsafeBytes { $0.load(as: Int64.self) })
    let base = Calendar(identifier: .gregorian).date(from: DateComponents(timeZone: TimeZone(secondsFromGMT: 0), year: 2000, month: 1, day: 1))!
    return base.addingTimeInterval(Double(micros) / 1_000_000)
}

private func parseUnknownText(_ text: String) -> Any {
    let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    if trimmed.isEmpty {
        return text
    }
    if looksLikeArrayLiteral(trimmed) {
        return parseArrayLiteral(trimmed)
    }
    if trimmed == "true" || trimmed == "false" {
        return trimmed == "true"
    }
    if let intVal = Int(trimmed) {
        return intVal
    }
    if let doubleVal = Double(trimmed) {
        return doubleVal
    }
    return text
}

private func decodeUnknownBinary(_ data: Data) -> Any {
    if data.count >= 4 {
        let stripped = stripLengthPrefix(data)
        if !stripped.isEmpty && looksLikeText(stripped) {
            let text = String(data: stripped, encoding: .utf8) ?? ""
            return parseUnknownText(text)
        }
    }
    let trimmed = stripTrailingNulls(data)
    if !trimmed.isEmpty && looksLikeText(trimmed) {
        let text = String(data: trimmed, encoding: .utf8) ?? ""
        return parseUnknownText(text)
    }
    switch data.count {
    case 1:
        return Int(data[0])
    case 2:
        return Int16(littleEndian: data.withUnsafeBytes { $0.load(as: Int16.self) })
    case 4:
        return Int32(littleEndian: data.withUnsafeBytes { $0.load(as: Int32.self) })
    case 8:
        return Int64(littleEndian: data.withUnsafeBytes { $0.load(as: Int64.self) })
    case 16:
        return uuidFromBytes(data)
    default:
        return data
    }
}

private func looksLikeText(_ data: Data) -> Bool {
    for byte in data {
        if byte == 0x09 || byte == 0x0A || byte == 0x0D {
            continue
        }
        if byte < 0x20 || byte > 0x7E {
            return false
        }
    }
    return true
}

private func stripTrailingNulls(_ data: Data) -> Data {
    var end = data.count
    while end > 0 && data[end - 1] == 0 {
        end -= 1
    }
    if end == data.count {
        return data
    }
    return data.subdata(in: 0..<end)
}

private func looksLikeArrayLiteral(_ text: String) -> Bool {
    return text.hasPrefix("{") && text.hasSuffix("}")
}

private func parseArrayLiteral(_ text: String) -> [Any] {
    var trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    if trimmed == "{}" || trimmed.isEmpty {
        return []
    }
    if trimmed.hasPrefix("{") && trimmed.hasSuffix("}") {
        trimmed.removeFirst()
        trimmed.removeLast()
    }
    return splitArrayItems(trimmed)
}

private func splitArrayItems(_ text: String) -> [Any] {
    var items: [Any] = []
    var depth = 0
    var buffer = ""
    for ch in text {
        if ch == "{" {
            depth += 1
            buffer.append(ch)
        } else if ch == "}" {
            depth = max(0, depth - 1)
            buffer.append(ch)
        } else if ch == "," && depth == 0 {
            items.append(parseArrayItem(buffer))
            buffer = ""
        } else {
            buffer.append(ch)
        }
    }
    if !buffer.isEmpty || !text.isEmpty {
        items.append(parseArrayItem(buffer))
    }
    return items
}

private func parseArrayItem(_ raw: String) -> Any {
    let token = raw.trimmingCharacters(in: .whitespacesAndNewlines)
    if token.isEmpty {
        return ""
    }
    if token.uppercased() == "NULL" {
        return NSNull()
    }
    if token.hasPrefix("{") && token.hasSuffix("}") {
        return parseArrayLiteral(token)
    }
    if token.hasPrefix("[") && token.hasSuffix("]") {
        return parseVectorLiteral(token)
    }
    if token == "true" || token == "false" {
        return token == "true"
    }
    if let intVal = Int(token) {
        return intVal
    }
    if let doubleVal = Double(token) {
        return doubleVal
    }
    return token
}

private func formatArrayLiteral(_ values: [Any]) -> String {
    let parts = values.map { formatArrayItem($0) }
    return "{\(parts.joined(separator: ","))}"
}

private func formatArrayItem(_ value: Any) -> String {
    if value is NSNull {
        return "NULL"
    }
    if let v = value as? String {
        let escaped = v.replacingOccurrences(of: "\"", with: "\\\"")
        return "\"\(escaped)\""
    }
    if let v = value as? [Any] {
        return formatArrayLiteral(v)
    }
    if let v = value as? [Float] {
        return formatVectorLiteral(v)
    }
    if let v = value as? [Double] {
        return formatVectorLiteral(v)
    }
    if let v = value as? CustomStringConvertible {
        return v.description
    }
    return String(describing: value)
}

private func parseVectorLiteral(_ text: String) -> [Double] {
    var trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    if trimmed.hasPrefix("[") && trimmed.hasSuffix("]") {
        trimmed.removeFirst()
        trimmed.removeLast()
    }
    if trimmed.isEmpty {
        return []
    }
    return trimmed.split(separator: ",").map { part in
        Double(part.trimmingCharacters(in: .whitespacesAndNewlines)) ?? 0
    }
}

private func formatVectorLiteral(_ values: [Float]) -> String {
    let parts = values.map { $0.isFinite ? String($0) : "0" }
    return "[\(parts.joined(separator: ","))]"
}

private func formatVectorLiteral(_ values: [Double]) -> String {
    let parts = values.map { $0.isFinite ? String($0) : "0" }
    return "[\(parts.joined(separator: ","))]"
}
