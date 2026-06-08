// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import XCTest
@testable import ScratchBird

final class TypeMappingTests: XCTestCase {
    func testVectorRoundTrip() throws {
        let encoded = try encodeParam([1.0, 2.5, 3.25])
        XCTAssertEqual(encoded.oid, TypeOid.sbVector)
        let decoded = decodeValue(oid: TypeOid.sbVector, data: encoded.param.data ?? Data(), format: 1)
        guard let values = decoded as? [Double] else {
            XCTFail("Expected vector decode to [Double]")
            return
        }
        XCTAssertEqual(values.count, 3)
        XCTAssertEqual(values[0], 1.0, accuracy: 0.00001)
        XCTAssertEqual(values[1], 2.5, accuracy: 0.00001)
        XCTAssertEqual(values[2], 3.25, accuracy: 0.00001)
    }

    func testArrayRoundTrip() throws {
        let encoded = try encodeParam([1, 2, 3])
        let decoded = decodeValue(oid: 0, data: encoded.param.data ?? Data(), format: 1)
        guard let values = decoded as? [Any] else {
            XCTFail("Expected array decode to [Any]")
            return
        }
        let ints = values.compactMap { $0 as? Int }
        XCTAssertEqual(ints, [1, 2, 3])
    }

    func testRangeRoundTrip() throws {
        var range = ScratchBirdRange(lower: 1, upper: 10, lowerInclusive: true, upperInclusive: false)
        range.rangeOid = TypeOid.int4range
        let encoded = try encodeParam(range)
        XCTAssertEqual(encoded.oid, TypeOid.int4range)
        guard let decoded = decodeValue(oid: TypeOid.int4range, data: encoded.param.data ?? Data(), format: 1) as? ScratchBirdRange else {
            XCTFail("Expected range decode to ScratchBirdRange")
            return
        }
        XCTAssertEqual(decoded.lower as? Int, 1)
        XCTAssertEqual(decoded.upper as? Int, 10)
        XCTAssertTrue(decoded.lowerInclusive)
        XCTAssertFalse(decoded.upperInclusive)
        XCTAssertFalse(decoded.empty)
    }

    func testCompositeRoundTrip() throws {
        let comp = ScratchBirdComposite(fields: [
            ScratchBirdCompositeField(oid: TypeOid.int4, value: 7, raw: nil),
            ScratchBirdCompositeField(oid: TypeOid.text, value: "hello", raw: nil),
        ])
        let encoded = try encodeParam(comp)
        XCTAssertEqual(encoded.oid, TypeOid.record)
        guard let decoded = decodeValue(oid: TypeOid.record, data: encoded.param.data ?? Data(), format: 1) as? ScratchBirdComposite else {
            XCTFail("Expected composite decode to ScratchBirdComposite")
            return
        }
        XCTAssertEqual(decoded.fields.count, 2)
        XCTAssertEqual(decoded.fields[0].value as? Int, 7)
        XCTAssertEqual(decoded.fields[1].value as? String, "hello")
    }

    func testInetCidrMacaddrRoundTrip() throws {
        let inet = ScratchBirdInet(value: "127.0.0.1")
        let cidr = ScratchBirdCidr(value: "10.0.0.0/24")
        let mac = ScratchBirdMacaddr(value: "aa:bb:cc:dd:ee:ff")

        let inetEnc = try encodeParam(inet)
        let cidrEnc = try encodeParam(cidr)
        let macEnc = try encodeParam(mac)

        XCTAssertEqual(decodeValue(oid: TypeOid.inet, data: inetEnc.param.data ?? Data(), format: 1) as? String, "127.0.0.1")
        XCTAssertEqual(decodeValue(oid: TypeOid.cidr, data: cidrEnc.param.data ?? Data(), format: 1) as? String, "10.0.0.0/24")
        XCTAssertEqual(decodeValue(oid: TypeOid.macaddr, data: macEnc.param.data ?? Data(), format: 1) as? String, "aa:bb:cc:dd:ee:ff")
    }

    func testPrimitiveScalarRoundTrip() throws {
        let boolEnc = try encodeParam(true)
        XCTAssertEqual(boolEnc.oid, TypeOid.bool)
        XCTAssertEqual(decodeValue(oid: TypeOid.bool, data: boolEnc.param.data ?? Data(), format: 1) as? Bool, true)

        let intEnc = try encodeParam(123_456)
        XCTAssertEqual(intEnc.oid, TypeOid.int4)
        XCTAssertEqual(decodeValue(oid: TypeOid.int4, data: intEnc.param.data ?? Data(), format: 1) as? Int, 123_456)

        let doubleEnc = try encodeParam(12.5)
        XCTAssertEqual(doubleEnc.oid, TypeOid.float8)
        XCTAssertEqual(decodeValue(oid: TypeOid.float8, data: doubleEnc.param.data ?? Data(), format: 1) as? Double, 12.5)
    }

    func testTemporalRoundTripAndDecode() throws {
        let instant = Date(timeIntervalSince1970: 1_735_689_600) // 2025-01-01 00:00:00 UTC
        let encodedTs = try encodeParam(instant)
        XCTAssertEqual(encodedTs.oid, TypeOid.timestamptz)

        guard let decodedTs = decodeValue(oid: TypeOid.timestamptz, data: encodedTs.param.data ?? Data(), format: 1) as? Date else {
            XCTFail("Expected timestamptz decode to Date")
            return
        }
        XCTAssertEqual(decodedTs.timeIntervalSince1970, instant.timeIntervalSince1970, accuracy: 0.000001)

        var days = Int32(2).littleEndian
        let dateData = Data(bytes: &days, count: 4)
        guard let decodedDate = decodeValue(oid: TypeOid.date, data: dateData, format: 1) as? Date else {
            XCTFail("Expected date decode to Date")
            return
        }
        let utc = Calendar(identifier: .gregorian)
        let comps = utc.dateComponents(in: TimeZone(secondsFromGMT: 0)!, from: decodedDate)
        XCTAssertEqual(comps.year, 2000)
        XCTAssertEqual(comps.month, 1)
        XCTAssertEqual(comps.day, 3)
    }

    func testJsonAndJsonbRoundTrip() throws {
        let jsonText = "{\"k\":1}"
        let jsonEnc = try encodeParam(Json(raw: Data(jsonText.utf8)))
        XCTAssertEqual(jsonEnc.oid, TypeOid.json)
        XCTAssertEqual(
            decodeValue(oid: TypeOid.json, data: jsonEnc.param.data ?? Data(), format: 1) as? String,
            jsonText
        )

        let jsonbText = "{\"v\":[1,2,3]}"
        let jsonbEnc = try encodeParam(Jsonb(raw: Data(jsonbText.utf8)))
        XCTAssertEqual(jsonbEnc.oid, TypeOid.jsonb)
        guard let decodedJsonb = decodeValue(oid: TypeOid.jsonb, data: jsonbEnc.param.data ?? Data(), format: 1) as? Jsonb else {
            XCTFail("Expected jsonb decode to Jsonb")
            return
        }
        XCTAssertEqual(String(data: decodedJsonb.raw, encoding: .utf8), jsonbText)
    }

    func testUuidEncodingValidAndFallback() throws {
        let uuid = "12345678-1234-5678-1234-567812345678"
        let uuidEnc = try encodeParam(uuid)
        XCTAssertEqual(uuidEnc.oid, TypeOid.uuid)
        XCTAssertEqual(
            decodeValue(oid: TypeOid.uuid, data: uuidEnc.param.data ?? Data(), format: 1) as? String,
            uuid
        )

        let invalid = "not-a-uuid"
        let fallbackEnc = try encodeParam(invalid)
        XCTAssertEqual(fallbackEnc.oid, TypeOid.text)
        XCTAssertEqual(
            decodeValue(oid: TypeOid.text, data: fallbackEnc.param.data ?? Data(), format: 1) as? String,
            invalid
        )
    }

    func testNegativeDecodeAndEncodePaths() throws {
        let truncatedInt = decodeValue(oid: TypeOid.int4, data: Data([0x01, 0x02]), format: 1)
        guard let raw = truncatedInt as? RawValue else {
            XCTFail("Expected truncated int4 to decode as RawValue")
            return
        }
        XCTAssertEqual(raw.oid, TypeOid.int4)
        XCTAssertEqual(raw.data, Data([0x01, 0x02]))

        struct Unsupported {}
        XCTAssertThrowsError(try encodeParam(Unsupported())) { error in
            let message = (error as NSError).localizedDescription
            XCTAssertTrue(message.contains("Unsupported parameter type"))
        }
    }
}
