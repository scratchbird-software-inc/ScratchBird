// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation
import Crypto

enum ScramAlgorithm {
    case sha256
    case sha512
}

final class ScramClient {
    private let username: String
    private let algorithm: ScramAlgorithm
    private let clientNonce: String
    private var clientFirstBare: String = ""
    private var serverSignature: Data = Data()

    init(username: String, algorithm: ScramAlgorithm = .sha256) {
        self.username = username
        self.algorithm = algorithm
        var generator = SystemRandomNumberGenerator()
        let nonce = (0..<18).map { _ in UInt8.random(in: 0...255, using: &generator) }
        self.clientNonce = Data(nonce).base64EncodedString()
    }

    func clientFirstMessage() -> String {
        clientFirstBare = "n=\(escape(username)),r=\(clientNonce)"
        return "n,," + clientFirstBare
    }

    func handleServerFirst(password: String, serverFirst: String) throws -> String {
        let attrs = parseAttrs(serverFirst)
        guard let nonce = attrs["r"], nonce.hasPrefix(clientNonce) else {
            throw ScramError.invalidNonce
        }
        guard let salt = attrs["s"], let iter = attrs["i"], let iterations = Int(iter) else {
            throw ScramError.invalidServerFirst
        }
        let saltData = Data(base64Encoded: salt) ?? Data()
        let keyLength = algorithm == .sha256 ? 32 : 64
        let salted = pbkdf2(password: password, salt: saltData, iterations: iterations, keyLen: keyLength)
        let clientKey = hmac(key: salted, data: Data("Client Key".utf8))
        let storedKey = hash(clientKey)
        let clientFinalWithoutProof = "c=biws,r=\(nonce)"
        let authMessage = clientFirstBare + "," + serverFirst + "," + clientFinalWithoutProof
        let clientSignature = hmac(key: storedKey, data: Data(authMessage.utf8))
        let clientProof = xorBytes(clientKey, clientSignature)
        let serverKey = hmac(key: salted, data: Data("Server Key".utf8))
        serverSignature = hmac(key: serverKey, data: Data(authMessage.utf8))
        return clientFinalWithoutProof + ",p=" + clientProof.base64EncodedString()
    }

    func verifyServerFinal(_ serverFinal: String) throws {
        let attrs = parseAttrs(serverFinal)
        guard let verifier = attrs["v"] else { throw ScramError.invalidServerFinal }
        let expected = serverSignature.base64EncodedString()
        if verifier != expected {
            throw ScramError.signatureMismatch
        }
    }

    private func escape(_ input: String) -> String {
        return input.replacingOccurrences(of: "=", with: "=3D").replacingOccurrences(of: ",", with: "=2C")
    }

    private func parseAttrs(_ message: String) -> [String: String] {
        var out: [String: String] = [:]
        for part in message.split(separator: ",") {
            let pieces = part.split(separator: "=", maxSplits: 1)
            if pieces.count == 2 {
                out[String(pieces[0])] = String(pieces[1])
            }
        }
        return out
    }

    private func hmac(key: Data, data: Data) -> Data {
        switch algorithm {
        case .sha256:
            let mac = HMAC<SHA256>.authenticationCode(for: data, using: SymmetricKey(data: key))
            return Data(mac)
        case .sha512:
            let mac = HMAC<SHA512>.authenticationCode(for: data, using: SymmetricKey(data: key))
            return Data(mac)
        }
    }

    private func hash(_ data: Data) -> Data {
        switch algorithm {
        case .sha256:
            return Data(SHA256.hash(data: data))
        case .sha512:
            return Data(SHA512.hash(data: data))
        }
    }

    private func xorBytes(_ left: Data, _ right: Data) -> Data {
        let l = [UInt8](left)
        let r = [UInt8](right)
        var out = [UInt8](repeating: 0, count: l.count)
        for i in 0..<l.count {
            out[i] = l[i] ^ r[i]
        }
        return Data(out)
    }

    private func pbkdf2(password: String, salt: Data, iterations: Int, keyLen: Int) -> Data {
        var result = Data()
        let digestLength = algorithm == .sha256 ? 32 : 64
        let blocks = (keyLen + digestLength - 1) / digestLength
        for i in 1...blocks {
            result.append(pbkdf2F(password: password, salt: salt, iterations: iterations, blockIndex: i))
        }
        return result.prefix(keyLen)
    }

    private func pbkdf2F(password: String, salt: Data, iterations: Int, blockIndex: Int) -> Data {
        var block = Data(salt)
        var idx = UInt32(blockIndex).bigEndian
        block.append(Data(bytes: &idx, count: 4))
        var u = hmac(key: Data(password.utf8), data: block)
        var out = u
        for _ in 1..<iterations {
            u = hmac(key: Data(password.utf8), data: u)
            out = xorBytes(out, u)
        }
        return out
    }
}

enum ScramError: Error {
    case invalidNonce
    case invalidServerFirst
    case invalidServerFinal
    case signatureMismatch
}
