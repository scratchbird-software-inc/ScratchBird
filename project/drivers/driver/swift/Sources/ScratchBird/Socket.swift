// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

#if canImport(Network)
import Network
#endif

#if canImport(NIOCore) && canImport(NIOPosix) && canImport(NIOSSL)
import NIOCore
import NIOPosix
import NIOSSL
#endif

#if canImport(Glibc)
import Glibc
private func systemConnect(_ fd: Int32, _ addr: UnsafePointer<sockaddr>, _ len: socklen_t) -> Int32 {
    Glibc.connect(fd, addr, len)
}
private func systemClose(_ fd: Int32) -> Int32 { Glibc.close(fd) }
private let socketStream: Int32 = Int32(SOCK_STREAM.rawValue)
#else
import Darwin
private func systemConnect(_ fd: Int32, _ addr: UnsafePointer<sockaddr>, _ len: socklen_t) -> Int32 {
    Darwin.connect(fd, addr, len)
}
private func systemClose(_ fd: Int32) -> Int32 { Darwin.close(fd) }
private let socketStream: Int32 = Int32(SOCK_STREAM)
#endif

struct ScratchBirdTlsConfig {
    let sslmode: String
    let sslrootcert: String?
    let sslcert: String?
    let sslkey: String?
    let sslpassword: String?
}

#if canImport(NIOCore) && canImport(NIOPosix) && canImport(NIOSSL)
private final class NioReadBuffer {
    private let condition = NSCondition()
    private var buffer = Data()
    private var error: Error?
    private var closed = false

    func append(_ bytes: [UInt8]) {
        condition.lock()
        buffer.append(contentsOf: bytes)
        condition.signal()
        condition.unlock()
    }

    func fail(_ error: Error) {
        condition.lock()
        self.error = error
        condition.broadcast()
        condition.unlock()
    }

    func markClosed() {
        condition.lock()
        closed = true
        condition.broadcast()
        condition.unlock()
    }

    func readExact(_ length: Int) throws -> Data {
        condition.lock()
        defer { condition.unlock() }

        while buffer.count < length && error == nil && !closed {
            condition.wait()
        }

        if let error {
            throw error
        }

        if buffer.count < length {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Socket closed"])
        }

        let out = buffer.prefix(length)
        buffer.removeFirst(length)
        return Data(out)
    }
}

private final class NioInboundHandler: ChannelInboundHandler {
    typealias InboundIn = ByteBuffer

    private let readBuffer: NioReadBuffer

    init(readBuffer: NioReadBuffer) {
        self.readBuffer = readBuffer
    }

    func channelRead(context: ChannelHandlerContext, data: NIOAny) {
        var buffer = unwrapInboundIn(data)
        if let bytes = buffer.readBytes(length: buffer.readableBytes), !bytes.isEmpty {
            readBuffer.append(bytes)
        }
    }

    func errorCaught(context: ChannelHandlerContext, error: Error) {
        readBuffer.fail(error)
        context.close(promise: nil as EventLoopPromise<Void>?)
    }

    func channelInactive(context: ChannelHandlerContext) {
        readBuffer.markClosed()
    }
}
#endif

final class ScratchBirdSocket {
    private var fd: Int32 = -1

    #if canImport(Network)
    private var nwConnection: NWConnection?
    private var nwBuffer = Data()
    private let nwQueue = DispatchQueue(label: "scratchbird.tls")
    #endif
    #if canImport(NIOCore) && canImport(NIOPosix) && canImport(NIOSSL)
    private var nioGroup: MultiThreadedEventLoopGroup?
    private var nioChannel: (any Channel)?
    private var nioReadBuffer: NioReadBuffer?
    #endif

    func connect(host: String, port: Int, tlsConfig: ScratchBirdTlsConfig?) throws {
        if let tlsConfig {
            try connectTls(host: host, port: port, tlsConfig: tlsConfig)
            return
        }

        var hints = addrinfo(
            ai_flags: 0,
            ai_family: AF_UNSPEC,
            ai_socktype: socketStream,
            ai_protocol: 0,
            ai_addrlen: 0,
            ai_addr: nil,
            ai_canonname: nil,
            ai_next: nil
        )

        var res: UnsafeMutablePointer<addrinfo>?
        let portStr = String(port)
        let err = getaddrinfo(host, portStr, &hints, &res)
        if err != 0 {
            throw NSError(domain: "ScratchBird", code: Int(err), userInfo: [NSLocalizedDescriptionKey: String(cString: gai_strerror(err))])
        }
        defer { freeaddrinfo(res) }

        var ptr = res
        while ptr != nil {
            let addr = ptr!.pointee
            fd = socket(addr.ai_family, addr.ai_socktype, addr.ai_protocol)
            if fd >= 0 {
                let result = withUnsafePointer(to: addr.ai_addr!.pointee) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { systemConnect(fd, $0, addr.ai_addrlen) }
                }
                if result == 0 {
                    return
                }
                _ = systemClose(fd)
                fd = -1
            }
            ptr = addr.ai_next
        }

        throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Failed to connect"])
    }

    func write(_ data: Data) throws {
        #if canImport(Network)
        if let connection = nwConnection {
            let sema = DispatchSemaphore(value: 0)
            var writeError: Error?
            connection.send(content: data, completion: .contentProcessed { error in
                writeError = error
                sema.signal()
            })
            sema.wait()
            if let err = writeError {
                throw err
            }
            return
        }
        #endif
        #if canImport(NIOCore) && canImport(NIOPosix) && canImport(NIOSSL)
        if let channel = nioChannel {
            var buffer = channel.allocator.buffer(capacity: data.count)
            buffer.writeBytes(data)
            try channel.writeAndFlush(buffer).wait()
            return
        }
        #endif

        let count = data.count
        let written = data.withUnsafeBytes { ptr -> Int in
            send(fd, ptr.baseAddress, count, 0)
        }
        if written != count {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Socket write failed"])
        }
    }

    func readExact(_ length: Int) throws -> Data {
        #if canImport(Network)
        if let connection = nwConnection {
            while nwBuffer.count < length {
                let sema = DispatchSemaphore(value: 0)
                var recvError: Error?
                connection.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { data, _, _, error in
                    if let data, !data.isEmpty {
                        self.nwBuffer.append(data)
                    }
                    recvError = error
                    sema.signal()
                }
                sema.wait()
                if let err = recvError {
                    throw err
                }
                if nwBuffer.isEmpty && length > 0 {
                    throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Socket closed"])
                }
            }
            let out = nwBuffer.prefix(length)
            nwBuffer.removeFirst(length)
            return Data(out)
        }
        #endif
        #if canImport(NIOCore) && canImport(NIOPosix) && canImport(NIOSSL)
        if let nioReadBuffer {
            return try nioReadBuffer.readExact(length)
        }
        #endif

        var buffer = Data(count: length)
        var offset = 0
        while offset < length {
            let readCount = buffer.withUnsafeMutableBytes { ptr -> Int in
                recv(fd, ptr.baseAddress!.advanced(by: offset), length - offset, 0)
            }
            if readCount <= 0 {
                throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "Socket closed"])
            }
            offset += readCount
        }
        return buffer
    }

    func hasPendingData() -> Bool {
        #if canImport(Network)
        if nwConnection != nil {
            return !nwBuffer.isEmpty
        }
        #endif
        #if canImport(NIOCore) && canImport(NIOPosix) && canImport(NIOSSL)
        if let nioReadBuffer {
            _ = nioReadBuffer
            return false
        }
        #endif

        if fd < 0 {
            return false
        }

        var probe: UInt8 = 0
        let flags = Int32(MSG_PEEK | MSG_DONTWAIT)
        let result = withUnsafeMutablePointer(to: &probe) { ptr in
            recv(fd, ptr, 1, flags)
        }
        if result > 0 {
            return true
        }
        if result == 0 {
            return false
        }

        #if canImport(Glibc)
        return errno != EAGAIN && errno != EWOULDBLOCK
        #else
        return errno != EAGAIN && errno != EWOULDBLOCK
        #endif
    }

    func close() {
        #if canImport(Network)
        if let connection = nwConnection {
            connection.cancel()
            nwConnection = nil
            nwBuffer.removeAll(keepingCapacity: true)
        }
        #endif
        #if canImport(NIOCore) && canImport(NIOPosix) && canImport(NIOSSL)
        if let channel = nioChannel {
            try? channel.close().wait()
            nioChannel = nil
        }
        if let group = nioGroup {
            try? group.syncShutdownGracefully()
            nioGroup = nil
        }
        nioReadBuffer = nil
        #endif

        if fd >= 0 {
            _ = systemClose(fd)
            fd = -1
        }
    }

    private func connectTls(host: String, port: Int, tlsConfig: ScratchBirdTlsConfig) throws {
        if tlsConfig.sslmode == "disable" {
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "TLS is required for ScratchBird connections"])
        }

        let hasCustomTLSFiles =
            (tlsConfig.sslrootcert?.isEmpty == false) ||
            (tlsConfig.sslcert?.isEmpty == false) ||
            (tlsConfig.sslkey?.isEmpty == false) ||
            (tlsConfig.sslpassword?.isEmpty == false)

        if hasCustomTLSFiles {
            #if canImport(NIOCore) && canImport(NIOPosix) && canImport(NIOSSL)
            try connectTlsNio(host: host, port: port, tlsConfig: tlsConfig)
            return
            #else
            throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "TLS certificate file options require NIOSSL support in this build"])
            #endif
        }

        #if canImport(Network)
        let tlsOptions = NWProtocolTLS.Options()
        sec_protocol_options_set_min_tls_protocol_version(tlsOptions.securityProtocolOptions, .TLSv13)
        sec_protocol_options_set_max_tls_protocol_version(tlsOptions.securityProtocolOptions, .TLSv13)

        let parameters = NWParameters(tls: tlsOptions, tcp: NWProtocolTCP.Options())
        let endpoint = NWEndpoint.hostPort(host: NWEndpoint.Host(host), port: NWEndpoint.Port(integerLiteral: NWEndpoint.Port.IntegerLiteralType(port)))
        let connection = NWConnection(to: endpoint, using: parameters)
        let sema = DispatchSemaphore(value: 0)
        var connectError: Error?
        connection.stateUpdateHandler = { state in
            switch state {
            case .ready:
                sema.signal()
            case .failed(let error):
                connectError = error
                sema.signal()
            default:
                break
            }
        }
        connection.start(queue: nwQueue)
        sema.wait()
        if let err = connectError {
            throw err
        }
        nwConnection = connection
        #else
        #if canImport(NIOCore) && canImport(NIOPosix) && canImport(NIOSSL)
        try connectTlsNio(host: host, port: port, tlsConfig: tlsConfig)
        #else
        throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "TLS transport is not available on this platform"])
        #endif
        #endif
    }

    #if canImport(NIOCore) && canImport(NIOPosix) && canImport(NIOSSL)
    private func connectTlsNio(host: String, port: Int, tlsConfig: ScratchBirdTlsConfig) throws {
        let readBuffer = NioReadBuffer()
        let group = MultiThreadedEventLoopGroup(numberOfThreads: 1)

        do {
            var config = TLSConfiguration.makeClientConfiguration()
            config.minimumTLSVersion = .tlsv13
            config.maximumTLSVersion = .tlsv13

            switch tlsConfig.sslmode {
            case "verify-full":
                config.certificateVerification = .fullVerification
            case "verify-ca":
                config.certificateVerification = .noHostnameVerification
            default:
                config.certificateVerification = .none
            }

            if let rootCert = tlsConfig.sslrootcert, !rootCert.isEmpty {
                config.trustRoots = .file(rootCert)
            }

            if let certFile = tlsConfig.sslcert, !certFile.isEmpty {
                guard let keyFile = tlsConfig.sslkey, !keyFile.isEmpty else {
                    throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "sslcert requires sslkey"])
                }
                let certificates = try NIOSSLCertificate.fromPEMFile(certFile)
                guard let leafCertificate = certificates.first else {
                    throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "sslcert provided no certificate"])
                }
                let privateKey: NIOSSLPrivateKey
                if let password = tlsConfig.sslpassword, !password.isEmpty {
                    privateKey = try NIOSSLPrivateKey(file: keyFile, format: .pem) { passphrase in
                        passphrase(password.utf8)
                    }
                } else {
                    privateKey = try NIOSSLPrivateKey(file: keyFile, format: .pem)
                }
                config.certificateChain = [NIOSSLCertificateSource.certificate(leafCertificate)]
                if certificates.count > 1 {
                    let additionalCertificates = certificates.dropFirst().map { NIOSSLCertificateSource.certificate($0) }
                    config.certificateChain.append(contentsOf: additionalCertificates)
                }
                config.privateKey = .privateKey(privateKey)
            } else if let keyFile = tlsConfig.sslkey, !keyFile.isEmpty {
                throw NSError(domain: "ScratchBird", code: -1, userInfo: [NSLocalizedDescriptionKey: "sslkey requires sslcert"])
            }

            let sslContext = try NIOSSLContext(configuration: config)

            let bootstrap = ClientBootstrap(group: group)
                .channelInitializer { channel in
                    do {
                        let handler = try NIOSSLClientHandler(context: sslContext, serverHostname: host)
                        return channel.pipeline.addHandler(handler).flatMap {
                            channel.pipeline.addHandler(NioInboundHandler(readBuffer: readBuffer))
                        }
                    } catch {
                        return channel.eventLoop.makeFailedFuture(error)
                    }
                }

            let channel = try bootstrap.connect(host: host, port: port).wait()

            self.nioGroup = group
            self.nioChannel = channel
            self.nioReadBuffer = readBuffer
        } catch {
            try? group.syncShutdownGracefully()
            throw error
        }
    }
    #endif
}
