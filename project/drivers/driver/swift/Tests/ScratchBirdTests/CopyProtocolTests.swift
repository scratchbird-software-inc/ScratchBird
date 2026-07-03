// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import XCTest
@testable import ScratchBird

final class CopyProtocolTests: XCTestCase {
    func testCopyTextRowsToNativeFrameEncodesCanonicalRows() throws {
        let text = Data("id=591;note=copy-alpha;payload_text=payload-a;marker=10\nid=592;note=copy-beta;payload_text=NULL;marker=20\n".utf8)
        let payload = try copyTextRowsToNativeFrame(text)

        XCTAssertEqual(payload.prefix(4), Data("SBNR".utf8))
        XCTAssertEqual(UInt16(littleEndian: payload.subdata(in: 4..<6).withUnsafeBytes { $0.load(as: UInt16.self) }), 2)
        XCTAssertEqual(UInt64(littleEndian: payload.subdata(in: 8..<16).withUnsafeBytes { $0.load(as: UInt64.self) }), 2)
        XCTAssertEqual(UInt32(littleEndian: payload.subdata(in: 16..<20).withUnsafeBytes { $0.load(as: UInt32.self) }), 4)
        XCTAssertEqual(Array(payload[20..<24]), [
            nativeRowsetTypeInt32,
            nativeRowsetTypeText,
            nativeRowsetTypeText,
            nativeRowsetTypeInt32
        ])
    }

    func testCopyTextRowsToNativeFrameRejectsShapeChange() {
        let text = Data("id=1;note=a\nid=2;other=b\n".utf8)

        XCTAssertThrowsError(try copyTextRowsToNativeFrame(text)) { error in
            let sbError = error as? ScratchBirdDataException
            XCTAssertEqual(sbError?.sqlState, "22000")
        }
    }

    func testParseCopyInResponseReadsFormatAndWindow() throws {
        var payload = Data([copyFormatBinary])
        payload.append(contentsOf: withUnsafeBytes(of: UInt32(8192).littleEndian, Array.init))

        let response = try parseCopyInResponse(payload)

        XCTAssertEqual(response.format, copyFormatBinary)
        XCTAssertEqual(response.windowBytes, 8192)
    }
}
