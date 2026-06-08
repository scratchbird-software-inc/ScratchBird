// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation
import XCTest
@testable import ScratchBird

#if canImport(Glibc)
import Glibc
private func testSocket(_ domain: Int32, _ type: Int32, _ proto: Int32) -> Int32 {
    Glibc.socket(domain, type, proto)
}
private func testBind(_ fd: Int32, _ addr: UnsafePointer<sockaddr>, _ len: socklen_t) -> Int32 {
    Glibc.bind(fd, addr, len)
}
private func testListen(_ fd: Int32, _ backlog: Int32) -> Int32 {
    Glibc.listen(fd, backlog)
}
private func testAccept(_ fd: Int32, _ addr: UnsafeMutablePointer<sockaddr>?, _ len: UnsafeMutablePointer<socklen_t>?) -> Int32 {
    Glibc.accept(fd, addr, len)
}
private func testClose(_ fd: Int32) -> Int32 {
    Glibc.close(fd)
}
private func testSend(_ fd: Int32, _ buf: UnsafeRawPointer?, _ len: Int, _ flags: Int32) -> Int {
    Glibc.send(fd, buf, len, flags)
}
private func testRecv(_ fd: Int32, _ buf: UnsafeMutableRawPointer?, _ len: Int, _ flags: Int32) -> Int {
    Glibc.recv(fd, buf, len, flags)
}
private func testGetSockName(_ fd: Int32, _ addr: UnsafeMutablePointer<sockaddr>?, _ len: UnsafeMutablePointer<socklen_t>?) -> Int32 {
    Glibc.getsockname(fd, addr, len)
}
private let socketStreamType = Int32(SOCK_STREAM.rawValue)
#else
import Darwin
private func testSocket(_ domain: Int32, _ type: Int32, _ proto: Int32) -> Int32 {
    Darwin.socket(domain, type, proto)
}
private func testBind(_ fd: Int32, _ addr: UnsafePointer<sockaddr>, _ len: socklen_t) -> Int32 {
    Darwin.bind(fd, addr, len)
}
private func testListen(_ fd: Int32, _ backlog: Int32) -> Int32 {
    Darwin.listen(fd, backlog)
}
private func testAccept(_ fd: Int32, _ addr: UnsafeMutablePointer<sockaddr>?, _ len: UnsafeMutablePointer<socklen_t>?) -> Int32 {
    Darwin.accept(fd, addr, len)
}
private func testClose(_ fd: Int32) -> Int32 {
    Darwin.close(fd)
}
private func testSend(_ fd: Int32, _ buf: UnsafeRawPointer?, _ len: Int, _ flags: Int32) -> Int {
    Darwin.send(fd, buf, len, flags)
}
private func testRecv(_ fd: Int32, _ buf: UnsafeMutableRawPointer?, _ len: Int, _ flags: Int32) -> Int {
    Darwin.recv(fd, buf, len, flags)
}
private func testGetSockName(_ fd: Int32, _ addr: UnsafeMutablePointer<sockaddr>?, _ len: UnsafeMutablePointer<socklen_t>?) -> Int32 {
    Darwin.getsockname(fd, addr, len)
}
private let socketStreamType = Int32(SOCK_STREAM)
#endif

private let testManagerProtocolMagic: UInt32 = 0x42444253
private let testManagerProtocolVersion: UInt16 = 0x0101
private let testManagerHeaderSize = 12
private let testMcpMsgStatusResponse: UInt8 = 0x64
private let testMcpMsgHello: UInt8 = 0x65
private let testMcpMsgAuthStart: UInt8 = 0x66
private let testMcpMsgAuthChallenge: UInt8 = 0x12

final class AuthBootstrapContractTests: XCTestCase {
    func testProbeAuthSurfaceDirectScramSha512() async throws {
        let server = try LoopbackAuthServer { clientFd in
            let startup = try readProtocolMessage(from: clientFd)
            XCTAssertEqual(startup.header.type, .startup)
            try writeProtocolMessage(
                to: clientFd,
                type: .authRequest,
                payload: makeAuthRequestPayload(method: authScramSha512Method)
            )
        }

        let probe = try await ScratchBirdConnection.probeAuthSurface(
            ScratchBirdConfig(
                host: "127.0.0.1",
                port: server.port,
                database: "db",
                user: "user",
                sslmode: "disable"
            )
        )
        try server.wait()

        XCTAssertTrue(probe.reachable)
        XCTAssertEqual(probe.ingressMode, "direct")
        XCTAssertEqual(probe.requiredMethod, "SCRAM_SHA_512")
        XCTAssertEqual(probe.requiredPluginMethodId, "scratchbird.auth.scram_sha_512")
        XCTAssertEqual(probe.admittedMethods.count, 1)
        XCTAssertEqual(probe.admittedMethods.first?.wireMethod, "SCRAM_SHA_512")
        XCTAssertTrue(probe.admittedMethods.first?.executableLocally ?? false)
        XCTAssertTrue(probe.additionalContinuationPossible)
    }

    func testProbeAuthSurfaceManagerToken() async throws {
        let server = try LoopbackAuthServer { clientFd in
            let hello = try readManagerFrame(from: clientFd)
            XCTAssertEqual(hello.type, testMcpMsgHello)
            try writeManagerFrame(to: clientFd, type: testMcpMsgStatusResponse, payload: Data())

            let authStart = try readManagerFrame(from: clientFd)
            XCTAssertEqual(authStart.type, testMcpMsgAuthStart)
            try writeManagerFrame(to: clientFd, type: testMcpMsgAuthChallenge, payload: Data())
        }

        let probe = try await ScratchBirdConnection.probeAuthSurface(
            ScratchBirdConfig(
                host: "127.0.0.1",
                port: server.port,
                frontDoorMode: "manager_proxy",
                database: "db",
                user: "admin",
                sslmode: "disable"
            )
        )
        try server.wait()

        XCTAssertTrue(probe.reachable)
        XCTAssertEqual(probe.ingressMode, "manager_proxy")
        XCTAssertEqual(probe.requiredMethod, "TOKEN")
        XCTAssertEqual(probe.requiredPluginMethodId, "scratchbird.auth.authkey_token")
        XCTAssertEqual(probe.admittedMethods.first?.wireMethod, "TOKEN")
        XCTAssertTrue(probe.additionalContinuationPossible)
    }

    func testConnectTracksResolvedScramSha512AuthContext() async throws {
        let password = "secret"
        let server = try LoopbackAuthServer { clientFd in
            _ = try readProtocolMessage(from: clientFd)
            try writeProtocolMessage(
                to: clientFd,
                type: .authRequest,
                payload: makeAuthRequestPayload(method: authScramSha512Method)
            )

            let firstResponse = try readProtocolMessage(from: clientFd)
            XCTAssertEqual(firstResponse.header.type, .authResponse)
            let clientFirst = String(data: firstResponse.payload, encoding: .utf8) ?? ""
            let nonce = extractScramNonce(fromClientFirst: clientFirst)
            let salt = Data("swift-salt".utf8).base64EncodedString()
            let serverFirst = "r=\(nonce)server,s=\(salt),i=4096"
            try writeProtocolMessage(
                to: clientFd,
                type: .authContinue,
                payload: makeAuthContinuePayload(method: authScramSha512Method, stage: 0, value: serverFirst)
            )

            let finalResponse = try readProtocolMessage(from: clientFd)
            XCTAssertEqual(finalResponse.header.type, .authResponse)
            XCTAssertFalse(finalResponse.payload.isEmpty)

            try writeProtocolMessage(
                to: clientFd,
                type: .authOk,
                payload: makeAuthOkPayload(serverInfo: "")
            )
            try writeProtocolMessage(to: clientFd, type: .ready, payload: Data())
        }

        let connection = try await ScratchBirdConnection.connect(
            ScratchBirdConfig(
                host: "127.0.0.1",
                port: server.port,
                database: "db",
                user: "user",
                password: password,
                sslmode: "disable"
            )
        )
        try server.wait()

        let context = connection.getResolvedAuthContext()
        XCTAssertEqual(context.ingressMode, "direct")
        XCTAssertEqual(context.resolvedAuthMethod, "SCRAM_SHA_512")
        XCTAssertEqual(context.resolvedAuthPluginId, "scratchbird.auth.scram_sha_512")
        XCTAssertFalse(context.managerAuthenticated)
        XCTAssertTrue(context.attached)

        try await connection.close()
        XCTAssertFalse(connection.getResolvedAuthContext().attached)
    }

    func testConnectTracksResolvedTokenAuthContext() async throws {
        let token = "demo-token"
        let server = try LoopbackAuthServer { clientFd in
            _ = try readProtocolMessage(from: clientFd)
            try writeProtocolMessage(
                to: clientFd,
                type: .authRequest,
                payload: makeAuthRequestPayload(method: authTokenMethod)
            )

            let response = try readProtocolMessage(from: clientFd)
            XCTAssertEqual(response.header.type, .authResponse)
            XCTAssertEqual(String(data: response.payload, encoding: .utf8), token)

            try writeProtocolMessage(
                to: clientFd,
                type: .authOk,
                payload: makeAuthOkPayload(serverInfo: "")
            )
            try writeProtocolMessage(to: clientFd, type: .ready, payload: Data())
        }

        let connection = try await ScratchBirdConnection.connect(
            ScratchBirdConfig(
                host: "127.0.0.1",
                port: server.port,
                database: "db",
                user: "user",
                sslmode: "disable",
                authToken: token
            )
        )
        try server.wait()

        let context = connection.getResolvedAuthContext()
        XCTAssertEqual(context.resolvedAuthMethod, "TOKEN")
        XCTAssertEqual(context.resolvedAuthPluginId, "scratchbird.auth.authkey_token")
        XCTAssertTrue(context.attached)

        try await connection.close()
    }

    func testConnectFailsClosedForPeerAuth() async throws {
        let server = try LoopbackAuthServer { clientFd in
            _ = try readProtocolMessage(from: clientFd)
            try writeProtocolMessage(
                to: clientFd,
                type: .authRequest,
                payload: makeAuthRequestPayload(method: authPeerMethod)
            )
        }

        do {
            _ = try await ScratchBirdConnection.connect(
                ScratchBirdConfig(
                    host: "127.0.0.1",
                    port: server.port,
                    database: "db",
                    user: "user",
                    sslmode: "disable"
                )
            )
            XCTFail("Expected PEER auth to fail closed")
        } catch let error as ScratchBirdNotSupportedException {
            XCTAssertEqual(error.sqlState, "0A000")
            XCTAssertTrue(error.message.contains("PEER"))
        }

        try server.wait()
    }
}

private final class LoopbackAuthServer {
    let port: Int
    private let listenFd: Int32
    private let group = DispatchGroup()
    private var scenarioError: Error?

    init(handler: @escaping (Int32) throws -> Void) throws {
        let listenFd = testSocket(AF_INET, socketStreamType, 0)
        if listenFd < 0 {
            throw NSError(domain: "ScratchBirdTests", code: -1, userInfo: [NSLocalizedDescriptionKey: "socket() failed"])
        }
        self.listenFd = listenFd

        var reuse: Int32 = 1
        _ = withUnsafePointer(to: &reuse) {
            $0.withMemoryRebound(to: UInt8.self, capacity: MemoryLayout<Int32>.size) {
                setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, $0, socklen_t(MemoryLayout<Int32>.size))
            }
        }

        var addr = sockaddr_in()
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = in_port_t(UInt16(0).bigEndian)
        addr.sin_addr = in_addr(s_addr: inet_addr("127.0.0.1"))

        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                testBind(listenFd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if bindResult != 0 {
            _ = testClose(listenFd)
            throw NSError(domain: "ScratchBirdTests", code: -1, userInfo: [NSLocalizedDescriptionKey: "bind() failed"])
        }

        if testListen(listenFd, 4) != 0 {
            _ = testClose(listenFd)
            throw NSError(domain: "ScratchBirdTests", code: -1, userInfo: [NSLocalizedDescriptionKey: "listen() failed"])
        }

        var boundAddr = sockaddr_in()
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        let sockNameResult = withUnsafeMutablePointer(to: &boundAddr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                testGetSockName(listenFd, $0, &length)
            }
        }
        if sockNameResult != 0 {
            _ = testClose(listenFd)
            throw NSError(domain: "ScratchBirdTests", code: -1, userInfo: [NSLocalizedDescriptionKey: "getsockname() failed"])
        }
        self.port = Int(UInt16(bigEndian: boundAddr.sin_port))

        group.enter()
        DispatchQueue(label: "scratchbird.swift.auth.bootstrap.server").async {
            defer {
                _ = testClose(listenFd)
                self.group.leave()
            }

            let clientFd = testAccept(listenFd, nil, nil)
            if clientFd < 0 {
                self.scenarioError = NSError(
                    domain: "ScratchBirdTests",
                    code: -1,
                    userInfo: [NSLocalizedDescriptionKey: "accept() failed"]
                )
                return
            }

            defer {
                _ = testClose(clientFd)
            }

            do {
                try handler(clientFd)
            } catch {
                self.scenarioError = error
            }
        }
    }

    func wait() throws {
        group.wait()
        if let scenarioError {
            throw scenarioError
        }
    }
}

private func readProtocolMessage(from fd: Int32) throws -> ProtocolMessage {
    let headerData = try readExact(from: fd, length: headerSize)
    let header = try decodeHeader(headerData)
    let payload = header.length > 0 ? try readExact(from: fd, length: Int(header.length)) : Data()
    return ProtocolMessage(header: header, payload: payload)
}

private func writeProtocolMessage(
    to fd: Int32,
    type: MessageType,
    payload: Data,
    sequence: UInt32 = 0,
    attachmentId: Data = Data(repeating: 0, count: 16),
    txnId: UInt64 = 0
) throws {
    let data = encodeMessage(
        header: MessageHeader(
            type: type,
            flags: 0,
            length: UInt32(payload.count),
            sequence: sequence,
            attachmentId: attachmentId,
            txnId: txnId
        ),
        payload: payload
    )
    try writeAll(to: fd, data: data)
}

private func readManagerFrame(from fd: Int32) throws -> (type: UInt8, payload: Data) {
    let header = try readExact(from: fd, length: Int(testManagerHeaderSize))
    let magic = UInt32(littleEndian: header.subdata(in: 0..<4).withUnsafeBytes { $0.load(as: UInt32.self) })
    if magic != testManagerProtocolMagic {
        throw NSError(domain: "ScratchBirdTests", code: -1, userInfo: [NSLocalizedDescriptionKey: "Manager frame magic mismatch"])
    }
    let type = header[6]
    let payloadLength = UInt32(littleEndian: header.subdata(in: 8..<12).withUnsafeBytes { $0.load(as: UInt32.self) })
    let payload = payloadLength > 0 ? try readExact(from: fd, length: Int(payloadLength)) : Data()
    return (type, payload)
}

private func writeManagerFrame(to fd: Int32, type: UInt8, payload: Data) throws {
    var frame = Data()
    frame.append(contentsOf: withUnsafeBytes(of: testManagerProtocolMagic.littleEndian, Array.init))
    frame.append(contentsOf: withUnsafeBytes(of: testManagerProtocolVersion.littleEndian, Array.init))
    frame.append(type)
    frame.append(0)
    frame.append(contentsOf: withUnsafeBytes(of: UInt32(payload.count).littleEndian, Array.init))
    frame.append(payload)
    try writeAll(to: fd, data: frame)
}

private func readExact(from fd: Int32, length: Int) throws -> Data {
    var out = Data(count: length)
    var offset = 0
    while offset < length {
        let read = out.withUnsafeMutableBytes { ptr in
            testRecv(fd, ptr.baseAddress!.advanced(by: offset), length - offset, 0)
        }
        if read <= 0 {
            throw NSError(domain: "ScratchBirdTests", code: -1, userInfo: [NSLocalizedDescriptionKey: "recv() failed"])
        }
        offset += read
    }
    return out
}

private func writeAll(to fd: Int32, data: Data) throws {
    var offset = 0
    while offset < data.count {
        let written = data.withUnsafeBytes { ptr in
            testSend(fd, ptr.baseAddress!.advanced(by: offset), data.count - offset, 0)
        }
        if written <= 0 {
            throw NSError(domain: "ScratchBirdTests", code: -1, userInfo: [NSLocalizedDescriptionKey: "send() failed"])
        }
        offset += written
    }
}

private func makeAuthRequestPayload(method: UInt8) -> Data {
    Data([method, 0, 0, 0])
}

private func makeAuthContinuePayload(method: UInt8, stage: UInt8, value: String) -> Data {
    let bytes = Data(value.utf8)
    var payload = Data([method, stage, 0, 0])
    payload.append(contentsOf: withUnsafeBytes(of: UInt32(bytes.count).littleEndian, Array.init))
    payload.append(bytes)
    return payload
}

private func makeAuthOkPayload(serverInfo: String) -> Data {
    let serverInfoBytes = Data(serverInfo.utf8)
    var payload = Data(repeating: 0, count: 16)
    payload.append(contentsOf: withUnsafeBytes(of: UInt32(serverInfoBytes.count).littleEndian, Array.init))
    payload.append(serverInfoBytes)
    return payload
}

private func extractScramNonce(fromClientFirst clientFirst: String) -> String {
    for part in clientFirst.split(separator: ",") {
        if part.hasPrefix("r=") {
            return String(part.dropFirst(2))
        }
    }
    return ""
}
