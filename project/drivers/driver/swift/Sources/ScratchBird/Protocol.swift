// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import Foundation

enum ProtocolError: Error {
    case invalidHeader
    case unsupportedVersion
    case payloadTooLarge
    case errorMessageTruncated
}

struct WireErrorMessage {
    let severity: String
    let sqlState: String
    let message: String
    let detail: String
    let hint: String
}

let scratchBirdErrorSQLStateKey = "ScratchBirdSQLState"
let scratchBirdErrorSeverityKey = "ScratchBirdSeverity"
let scratchBirdErrorDetailKey = "ScratchBirdDetail"
let scratchBirdErrorHintKey = "ScratchBirdHint"

enum MessageType: UInt8 {
    case startup = 0x01
    case authResponse = 0x02
    case query = 0x03
    case parse = 0x04
    case bind = 0x05
    case describe = 0x06
    case execute = 0x07
    case close = 0x08
    case sync = 0x09
    case flush = 0x0a
    case cancel = 0x0b
    case terminate = 0x0c
    case copyData = 0x0d
    case copyDone = 0x0e
    case copyFail = 0x0f
    case sblrExecute = 0x10
    case subscribe = 0x11
    case unsubscribe = 0x12
    case federatedQuery = 0x13
    case streamControl = 0x14
    case txnBegin = 0x15
    case txnCommit = 0x16
    case txnRollback = 0x17
    case txnSavepoint = 0x18
    case txnRelease = 0x19
    case txnRollbackTo = 0x1a
    case ping = 0x1b
    case setOption = 0x1c
    case clusterAuth = 0x1d
    case attachCreate = 0x1e
    case attachDetach = 0x1f
    case attachList = 0x20

    case authRequest = 0x40
    case authOk = 0x41
    case authContinue = 0x42
    case ready = 0x43
    case rowDescription = 0x44
    case dataRow = 0x45
    case commandComplete = 0x46
    case emptyQuery = 0x47
    case error = 0x48
    case notice = 0x49
    case parseComplete = 0x4a
    case bindComplete = 0x4b
    case closeComplete = 0x4c
    case portalSuspended = 0x4d
    case noData = 0x4e
    case parameterStatus = 0x4f
    case parameterDescription = 0x50
    case copyInResponse = 0x51
    case copyOutResponse = 0x52
    case copyBothResponse = 0x53
    case notification = 0x54
    case functionResult = 0x55
    case negotiateVersion = 0x56
    case sblrCompiled = 0x57
    case queryPlan = 0x58
    case streamReady = 0x59
    case streamData = 0x5a
    case streamEnd = 0x5b
    case txnStatus = 0x5c
    case pong = 0x5d
    case clusterAuthOk = 0x5e
    case federatedResult = 0x5f
    case queryProgress = 0x60
    case serverInfo = 0x61
    case stateNotification = 0x62
    case cancelAck = 0x65
    case cancelled = 0x66
    case multiResultBegin = 0x67
    case multiResultEnd = 0x68
    case generatedKeys = 0x69
    case outParameters = 0x6a
    case batchResult = 0x6b
    case pipelineStatus = 0x6c
    case arrayBindStatus = 0x6d
    case heartbeat = 0x80
    case `extension` = 0x81
}

struct MessageHeader {
    var type: MessageType
    var flags: UInt8
    var length: UInt32
    var sequence: UInt32
    var attachmentId: Data
    var txnId: UInt64
}

struct ProtocolMessage {
    var header: MessageHeader
    var payload: Data
}

let protocolMagic: UInt32 = 0x50574253
let protocolMajor: UInt8 = 1
let protocolMinor: UInt8 = 1
let headerSize = 40
let maxMessageSize: UInt32 = 1024 * 1024 * 1024

let authOkMethod: UInt8 = 0
let authPasswordMethod: UInt8 = 1
let authMd5Method: UInt8 = 2
let authScramSha256Method: UInt8 = 3
let authScramSha512Method: UInt8 = 4
let authTokenMethod: UInt8 = 5
let authPeerMethod: UInt8 = 6
let authReattachMethod: UInt8 = 7

let messageFlagUrgent: UInt8 = 0x08

let featureCompression: UInt64 = 1 << 0
let featureStreaming: UInt64 = 1 << 1
let featureBinaryCopy: UInt64 = 1 << 8
let featureSavepoints: UInt64 = 1 << 9

let copyFormatText: UInt8 = 0
let copyFormatBinary: UInt8 = 1

let nativeRowsetTypeText: UInt8 = 1
let nativeRowsetTypeInt64: UInt8 = 2
let nativeRowsetTypeBoolean: UInt8 = 3
let nativeRowsetTypeInt32: UInt8 = 4
let nativeRowsetTypeUInt64: UInt8 = 5
let nativeRowsetTypeReal64: UInt8 = 6

let queryFlagDescribeOnly: UInt32 = 0x01
let queryFlagNoPortal: UInt32 = 0x02
let queryFlagBinaryResult: UInt32 = 0x04
let queryFlagIncludePlan: UInt32 = 0x08
let queryFlagReturnSblr: UInt32 = 0x10
let queryFlagNoCache: UInt32 = 0x20

let isolationReadUncommitted: UInt8 = 0
let isolationReadCommitted: UInt8 = 1
let isolationRepeatableRead: UInt8 = 2
let isolationSerializable: UInt8 = 3

let readCommittedModeDefault: UInt8 = 0
let readCommittedModeReadConsistency: UInt8 = 1
let readCommittedModeRecordVersion: UInt8 = 2
let readCommittedModeNoRecordVersion: UInt8 = 3

let txnFlagHasIsolation: UInt16 = 0x0001
let txnFlagHasAccess: UInt16 = 0x0002
let txnFlagHasDeferrable: UInt16 = 0x0004
let txnFlagHasWait: UInt16 = 0x0008
let txnFlagHasTimeout: UInt16 = 0x0010
let txnFlagHasAutocommit: UInt16 = 0x0020
let txnFlagHasReadCommittedMode: UInt16 = 0x0100

let streamStart: UInt8 = 0
let streamPause: UInt8 = 1
let streamResume: UInt8 = 2
let streamCancel: UInt8 = 3
let streamAck: UInt8 = 4

let subscribeTypeChannel: UInt8 = 0
let subscribeTypeTable: UInt8 = 1
let subscribeTypeQuery: UInt8 = 2
let subscribeTypeEvent: UInt8 = 3

func encodeMessage(header: MessageHeader, payload: Data) -> Data {
    var data = Data(count: headerSize + payload.count)
    data.withUnsafeMutableBytes { ptr in
        var offset = 0
        ptr.storeBytes(of: protocolMagic.littleEndian, toByteOffset: offset, as: UInt32.self)
        offset += 4
        ptr.storeBytes(of: protocolMajor, toByteOffset: offset, as: UInt8.self)
        offset += 1
        ptr.storeBytes(of: protocolMinor, toByteOffset: offset, as: UInt8.self)
        offset += 1
        ptr.storeBytes(of: header.type.rawValue, toByteOffset: offset, as: UInt8.self)
        offset += 1
        ptr.storeBytes(of: header.flags, toByteOffset: offset, as: UInt8.self)
        offset += 1
        ptr.storeBytes(of: UInt32(payload.count).littleEndian, toByteOffset: offset, as: UInt32.self)
        offset += 4
        ptr.storeBytes(of: header.sequence.littleEndian, toByteOffset: offset, as: UInt32.self)
        offset += 4
        header.attachmentId.copyBytes(to: ptr.baseAddress!.advanced(by: offset).assumingMemoryBound(to: UInt8.self), count: 16)
        offset += 16
        ptr.storeBytes(of: header.txnId.littleEndian, toByteOffset: offset, as: UInt64.self)
        offset += 8
        payload.copyBytes(to: ptr.baseAddress!.advanced(by: offset).assumingMemoryBound(to: UInt8.self), count: payload.count)
    }
    return data
}

func decodeHeader(_ data: Data) throws -> MessageHeader {
    if data.count != headerSize { throw ProtocolError.invalidHeader }
    let magic = data.withUnsafeBytes { $0.load(as: UInt32.self) }.littleEndian
    if magic != protocolMagic { throw ProtocolError.invalidHeader }
    let major = data[4]
    let minor = data[5]
    if major != protocolMajor || minor != protocolMinor { throw ProtocolError.unsupportedVersion }
    let length = data.subdata(in: 8..<12).withUnsafeBytes { $0.load(as: UInt32.self) }.littleEndian
    if length > maxMessageSize { throw ProtocolError.payloadTooLarge }
    let type = MessageType(rawValue: data[6]) ?? .error
    let flags = data[7]
    let sequence = data.subdata(in: 12..<16).withUnsafeBytes { $0.load(as: UInt32.self) }.littleEndian
    let attachment = data.subdata(in: 16..<32)
    let txnId = data.subdata(in: 32..<40).withUnsafeBytes { $0.load(as: UInt64.self) }.littleEndian
    return MessageHeader(type: type, flags: flags, length: length, sequence: sequence, attachmentId: attachment, txnId: txnId)
}

func parseErrorMessage(_ payload: Data) throws -> WireErrorMessage {
    var severity = ""
    var sqlState = ""
    var message = ""
    var detail = ""
    var hint = ""

    var offset = 0
    while offset < payload.count {
        let field = payload[offset]
        offset += 1
        if field == 0 {
            break
        }
        let start = offset
        while offset < payload.count && payload[offset] != 0 {
            offset += 1
        }
        if offset >= payload.count {
            throw ProtocolError.errorMessageTruncated
        }
        let valueData = payload.subdata(in: start..<offset)
        let value = String(data: valueData, encoding: .utf8) ?? ""
        offset += 1

        switch field {
        case UInt8(ascii: "S"):
            severity = value
        case UInt8(ascii: "C"):
            sqlState = value
        case UInt8(ascii: "M"):
            message = value
        case UInt8(ascii: "D"):
            detail = value
        case UInt8(ascii: "H"):
            hint = value
        default:
            continue
        }
    }

    return WireErrorMessage(
        severity: severity,
        sqlState: sqlState,
        message: message,
        detail: detail,
        hint: hint
    )
}

func buildScratchBirdError(
    from payload: Data,
    fallbackMessage: String,
    code: Int = -1,
    defaultSqlState: String? = nil
) -> ScratchBirdDriverException {
    guard let parsed = try? parseErrorMessage(payload) else {
        return mapSqlStateError(
            message: fallbackMessage,
            sqlState: defaultSqlState,
            severity: nil,
            detail: nil,
            hint: nil,
            code: code
        )
    }

    let sqlState = parsed.sqlState.isEmpty ? defaultSqlState : parsed.sqlState
    let base = parsed.message.isEmpty ? fallbackMessage : parsed.message
    var message = base
    if let sqlState, !sqlState.isEmpty {
        message += " [SQLSTATE \(sqlState)]"
    }
    if !parsed.detail.isEmpty {
        message += " DETAIL: \(parsed.detail)"
    }
    if !parsed.hint.isEmpty {
        message += " HINT: \(parsed.hint)"
    }

    return mapSqlStateError(
        message: message,
        sqlState: sqlState,
        severity: parsed.severity.isEmpty ? nil : parsed.severity,
        detail: parsed.detail.isEmpty ? nil : parsed.detail,
        hint: parsed.hint.isEmpty ? nil : parsed.hint,
        code: code
    )
}

func buildScratchBirdNSError(
    from payload: Data,
    fallbackMessage: String,
    code: Int = -1,
    defaultSqlState: String? = nil
) -> NSError {
    let typed = buildScratchBirdError(
        from: payload,
        fallbackMessage: fallbackMessage,
        code: code,
        defaultSqlState: defaultSqlState
    )
    return NSError(domain: ScratchBirdDriverException.errorDomain, code: typed.errorCode, userInfo: typed.errorUserInfo)
}

func parseAuthRequest(_ payload: Data) throws -> (method: UInt8, data: Data) {
    if payload.count < 4 {
        throw ScratchBirdConnectionException(
            message: "Auth request truncated",
            sqlState: "08P01"
        )
    }
    return (payload[0], payload.subdata(in: 4..<payload.count))
}

func parseAuthContinue(_ payload: Data) throws -> (method: UInt8, stage: UInt8, data: Data) {
    if payload.count < 8 {
        throw ScratchBirdConnectionException(
            message: "Auth continue truncated",
            sqlState: "08P01"
        )
    }
    let dataLen = Int(UInt32(littleEndian: payload.subdata(in: 4..<8).withUnsafeBytes { $0.load(as: UInt32.self) }))
    if 8 + dataLen > payload.count {
        throw ScratchBirdConnectionException(
            message: "Auth continue truncated",
            sqlState: "08P01"
        )
    }
    return (payload[0], payload[1], payload.subdata(in: 8..<(8 + dataLen)))
}

func parseAuthOk(_ payload: Data) throws -> (sessionId: Data, serverInfo: Data) {
    if payload.count < 20 {
        throw ScratchBirdConnectionException(
            message: "Auth ok truncated",
            sqlState: "08P01"
        )
    }
    let infoLen = Int(UInt32(littleEndian: payload.subdata(in: 16..<20).withUnsafeBytes { $0.load(as: UInt32.self) }))
    if 20 + infoLen > payload.count {
        throw ScratchBirdConnectionException(
            message: "Auth ok truncated",
            sqlState: "08P01"
        )
    }
    return (
        payload.subdata(in: 0..<16),
        payload.subdata(in: 20..<(20 + infoLen))
    )
}

func buildStartupPayload(features: UInt64, params: [String: String]) -> Data {
    let protocolVersion = UInt16(protocolMajor) << 8 | UInt16(protocolMinor)
    let paramBytes = buildP1ParamList(params)
    var data = Data()
    data.append(contentsOf: withUnsafeBytes(of: protocolVersion.littleEndian, Array.init))
    data.append(contentsOf: withUnsafeBytes(of: protocolVersion.littleEndian, Array.init))
    data.append(contentsOf: withUnsafeBytes(of: UInt32(0).littleEndian, Array.init))
    data.append(contentsOf: withUnsafeBytes(of: features.littleEndian, Array.init))
    data.append(contentsOf: withUnsafeBytes(of: UInt64(0).littleEndian, Array.init))
    data.append(contentsOf: withUnsafeBytes(of: UInt64(0).littleEndian, Array.init))
    data.append(Data(repeating: 0, count: 16))
    data.append(Data(repeating: 0, count: 16))
    data.append(Data(repeating: 0, count: 16))
    data.append(contentsOf: withUnsafeBytes(of: UInt32(params.count).littleEndian, Array.init))
    data.append(paramBytes)
    data.append(contentsOf: withUnsafeBytes(of: UInt32(0).littleEndian, Array.init))
    return data
}

func buildQueryPayload(sql: String, flags: UInt32, maxRows: UInt32, timeoutMs: UInt32) -> Data {
    var data = Data()
    data.append(contentsOf: withUnsafeBytes(of: flags.littleEndian, Array.init))
    data.append(contentsOf: withUnsafeBytes(of: maxRows.littleEndian, Array.init))
    data.append(contentsOf: withUnsafeBytes(of: timeoutMs.littleEndian, Array.init))
    data.append(sql.data(using: .utf8) ?? Data())
    data.append(0)
    return data
}

func buildParsePayload(statement: String, sql: String, paramTypes: [UInt32]) -> Data {
    var data = Data()
    let nameBytes = statement.data(using: .utf8) ?? Data()
    let sqlBytes = sql.data(using: .utf8) ?? Data()
    data.append(contentsOf: withUnsafeBytes(of: UInt32(nameBytes.count).littleEndian, Array.init))
    data.append(nameBytes)
    data.append(contentsOf: withUnsafeBytes(of: UInt32(sqlBytes.count).littleEndian, Array.init))
    data.append(sqlBytes)
    data.append(contentsOf: withUnsafeBytes(of: UInt16(paramTypes.count).littleEndian, Array.init))
    data.append(contentsOf: [0, 0])
    for oid in paramTypes {
        data.append(contentsOf: withUnsafeBytes(of: oid.littleEndian, Array.init))
    }
    return data
}

struct ParamValue {
    let format: UInt16
    let data: Data?
    let isNull: Bool
}

func buildBindPayload(portal: String, statement: String, params: [ParamValue], resultFormats: [UInt16]) -> Data {
    var data = Data()
    let portalBytes = portal.data(using: .utf8) ?? Data()
    let stmtBytes = statement.data(using: .utf8) ?? Data()
    data.append(contentsOf: withUnsafeBytes(of: UInt32(portalBytes.count).littleEndian, Array.init))
    data.append(portalBytes)
    data.append(contentsOf: withUnsafeBytes(of: UInt32(stmtBytes.count).littleEndian, Array.init))
    data.append(stmtBytes)
    data.append(contentsOf: withUnsafeBytes(of: UInt16(params.count).littleEndian, Array.init))
    for param in params {
        data.append(contentsOf: withUnsafeBytes(of: param.format.littleEndian, Array.init))
    }
    data.append(contentsOf: withUnsafeBytes(of: UInt16(params.count).littleEndian, Array.init))
    data.append(contentsOf: [0, 0])
    for param in params {
        if param.isNull {
            data.append(contentsOf: [0xFF, 0xFF, 0xFF, 0xFF])
        } else {
            let payload = param.data ?? Data()
            data.append(contentsOf: withUnsafeBytes(of: UInt32(payload.count).littleEndian, Array.init))
            data.append(payload)
        }
    }
    data.append(contentsOf: withUnsafeBytes(of: UInt16(resultFormats.count).littleEndian, Array.init))
    for fmt in resultFormats {
        data.append(contentsOf: withUnsafeBytes(of: fmt.littleEndian, Array.init))
    }
    return data
}

func buildDescribePayload(kind: UInt8, name: String) -> Data {
    var data = Data()
    data.append(kind)
    data.append(contentsOf: [0, 0, 0])
    let nameBytes = name.data(using: .utf8) ?? Data()
    data.append(contentsOf: withUnsafeBytes(of: UInt32(nameBytes.count).littleEndian, Array.init))
    data.append(nameBytes)
    return data
}

func buildExecutePayload(portal: String, maxRows: UInt32) -> Data {
    var data = Data()
    let portalBytes = portal.data(using: .utf8) ?? Data()
    data.append(contentsOf: withUnsafeBytes(of: UInt32(portalBytes.count).littleEndian, Array.init))
    data.append(portalBytes)
    data.append(contentsOf: withUnsafeBytes(of: maxRows.littleEndian, Array.init))
    return data
}

func buildCancelPayload(cancelType: UInt32, targetSequence: UInt32) -> Data {
    var data = Data()
    data.append(contentsOf: withUnsafeBytes(of: cancelType.littleEndian, Array.init))
    data.append(contentsOf: withUnsafeBytes(of: targetSequence.littleEndian, Array.init))
    return data
}

func buildSblrExecutePayload(sblrHash: UInt64, sblrBytecode: Data?, params: [ParamValue]) -> Data {
    var data = Data()
    let bytecode = sblrBytecode ?? Data()
    data.append(contentsOf: withUnsafeBytes(of: sblrHash.littleEndian, Array.init))
    data.append(contentsOf: withUnsafeBytes(of: UInt32(bytecode.count).littleEndian, Array.init))
    data.append(contentsOf: withUnsafeBytes(of: UInt16(params.count).littleEndian, Array.init))
    data.append(contentsOf: [0, 0])
    if !bytecode.isEmpty {
        data.append(bytecode)
    }
    for param in params {
        if param.isNull {
            data.append(contentsOf: [0xFF, 0xFF, 0xFF, 0xFF])
        } else {
            let payload = param.data ?? Data()
            data.append(contentsOf: withUnsafeBytes(of: UInt32(payload.count).littleEndian, Array.init))
            data.append(payload)
        }
    }
    return data
}

func buildSubscribePayload(subscribeType: UInt8, channel: String, filterExpr: String = "") -> Data {
    var data = Data()
    data.append(subscribeType)
    data.append(contentsOf: [0, 0, 0])
    let channelBytes = channel.data(using: .utf8) ?? Data()
    let filterBytes = filterExpr.data(using: .utf8) ?? Data()
    data.append(contentsOf: withUnsafeBytes(of: UInt32(channelBytes.count).littleEndian, Array.init))
    data.append(channelBytes)
    data.append(contentsOf: withUnsafeBytes(of: UInt32(filterBytes.count).littleEndian, Array.init))
    data.append(filterBytes)
    return data
}

func buildUnsubscribePayload(channel: String) -> Data {
    let channelBytes = channel.data(using: .utf8) ?? Data()
    var data = Data()
    data.append(contentsOf: withUnsafeBytes(of: UInt32(channelBytes.count).littleEndian, Array.init))
    data.append(channelBytes)
    return data
}

func buildTxnBeginPayload(
    flags: UInt16,
    conflictAction: UInt8,
    autocommitMode: UInt8,
    isolationLevel: UInt8,
    accessMode: UInt8,
    deferrable: UInt8,
    waitMode: UInt8,
    timeoutMs: UInt32,
    readCommittedMode: UInt8
) -> Data {
    var data = Data()
    data.append(contentsOf: withUnsafeBytes(of: flags.littleEndian, Array.init))
    data.append(conflictAction)
    data.append(autocommitMode)
    data.append(isolationLevel)
    data.append(accessMode)
    data.append(deferrable)
    data.append(waitMode)
    data.append(contentsOf: withUnsafeBytes(of: timeoutMs.littleEndian, Array.init))
    if (flags & txnFlagHasReadCommittedMode) != 0 {
        data.append(readCommittedMode)
        data.append(contentsOf: [0, 0, 0])
    }
    return data
}

func buildTxnCommitPayload(flags: UInt8) -> Data {
    return Data([flags, 0, 0, 0])
}

func buildTxnRollbackPayload(flags: UInt8) -> Data {
    return Data([flags, 0, 0, 0])
}

func buildTxnSavepointPayload(name: String) -> Data {
    let nameBytes = name.data(using: .utf8) ?? Data()
    var data = Data()
    data.append(contentsOf: withUnsafeBytes(of: UInt32(nameBytes.count).littleEndian, Array.init))
    data.append(nameBytes)
    return data
}

func buildTxnReleasePayload(name: String) -> Data {
    return buildTxnSavepointPayload(name: name)
}

func buildTxnRollbackToPayload(name: String) -> Data {
    return buildTxnSavepointPayload(name: name)
}

func buildSetOptionPayload(name: String, value: String) -> Data {
    let nameBytes = name.data(using: .utf8) ?? Data()
    let valueBytes = value.data(using: .utf8) ?? Data()
    var data = Data()
    data.append(contentsOf: withUnsafeBytes(of: UInt32(nameBytes.count).littleEndian, Array.init))
    data.append(nameBytes)
    data.append(contentsOf: withUnsafeBytes(of: UInt32(valueBytes.count).littleEndian, Array.init))
    data.append(valueBytes)
    return data
}

func buildStreamControlPayload(controlType: UInt8, windowSize: UInt32, timeoutMs: UInt32) -> Data {
    var data = Data()
    data.append(controlType)
    data.append(contentsOf: [0, 0, 0])
    data.append(contentsOf: withUnsafeBytes(of: windowSize.littleEndian, Array.init))
    data.append(contentsOf: withUnsafeBytes(of: timeoutMs.littleEndian, Array.init))
    return data
}

func buildAttachCreatePayload(emulationMode: String, dbName: String) -> Data {
    let modeBytes = emulationMode.data(using: .utf8) ?? Data()
    let dbBytes = dbName.data(using: .utf8) ?? Data()
    var data = Data()
    data.append(contentsOf: withUnsafeBytes(of: UInt32(modeBytes.count).littleEndian, Array.init))
    data.append(modeBytes)
    data.append(contentsOf: withUnsafeBytes(of: UInt32(dbBytes.count).littleEndian, Array.init))
    data.append(dbBytes)
    return data
}

func buildParamList(_ params: [String: String]) -> Data {
    var data = Data()
    for (key, value) in params {
        data.append(key.data(using: .utf8) ?? Data())
        data.append(0)
        data.append(value.data(using: .utf8) ?? Data())
        data.append(0)
    }
    data.append(0)
    return data
}

func buildP1ParamList(_ params: [String: String]) -> Data {
    var data = Data()
    for key in params.keys.sorted() {
        let keyBytes = key.data(using: .utf8) ?? Data()
        let valueBytes = (params[key] ?? "").data(using: .utf8) ?? Data()
        data.append(contentsOf: withUnsafeBytes(of: UInt32(keyBytes.count).littleEndian, Array.init))
        data.append(keyBytes)
        data.append(contentsOf: withUnsafeBytes(of: UInt16(1).littleEndian, Array.init))
        data.append(contentsOf: withUnsafeBytes(of: UInt32(valueBytes.count).littleEndian, Array.init))
        data.append(valueBytes)
    }
    return data
}

struct CopyInResponse {
    let format: UInt8
    let windowBytes: UInt32
}

func parseCopyInResponse(_ payload: Data) throws -> CopyInResponse {
    guard payload.count >= 5 else {
        throw ScratchBirdConnectionException(
            message: "copy in response truncated",
            sqlState: "08P01"
        )
    }
    let window = payload.subdata(in: 1..<5).withUnsafeBytes { raw in
        raw.load(as: UInt32.self).littleEndian
    }
    return CopyInResponse(format: payload[0], windowBytes: window)
}

func copyTextRowsToNativeFrame(_ data: Data) throws -> Data {
    if data.count >= 4 && data.prefix(4) == Data("SBNR".utf8) {
        return data
    }
    guard let text = String(data: data, encoding: .utf8) else {
        throw ScratchBirdDataException(
            message: "binary COPY input is not a native rowset frame or UTF-8 COPY text",
            sqlState: "22P03"
        )
    }
    let lines = text
        .split(whereSeparator: \.isNewline)
        .map { String($0).trimmingCharacters(in: CharacterSet(charactersIn: "\r")) }
        .filter { !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
    guard let first = lines.first else {
        throw ScratchBirdDataException(message: "COPY input contains no rows", sqlState: "22000")
    }
    guard first.contains(";") && first.contains("=") else {
        throw ScratchBirdDataException(
            message: "binary COPY conversion requires canonical name=value rows",
            sqlState: "22000"
        )
    }

    var columns: [String] = []
    var rows: [[String?]] = []
    for line in lines {
        var fields: [(String, String?)] = []
        for item in line.split(separator: ";", omittingEmptySubsequences: true) {
            let parts = item.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            guard parts.count == 2, !parts[0].isEmpty else {
                throw ScratchBirdDataException(message: "malformed canonical COPY field", sqlState: "22000")
            }
            let value = String(parts[1])
            fields.append((String(parts[0]), value.uppercased() == "NULL" ? nil : value))
        }
        if fields.isEmpty {
            continue
        }
        let fieldColumns = fields.map { $0.0 }
        if columns.isEmpty {
            columns = fieldColumns
        } else if fieldColumns != columns {
            throw ScratchBirdDataException(message: "COPY input changed row shape mid-stream", sqlState: "22000")
        }
        rows.append(fields.map { $0.1 })
    }
    return try buildNativeRowsetPayload(columns: columns, rows: rows)
}

private func buildNativeRowsetPayload(columns: [String], rows: [[String?]]) throws -> Data {
    guard !columns.isEmpty, !rows.isEmpty else {
        throw ScratchBirdDataException(
            message: "native rowset requires at least one column and one row",
            sqlState: "22000"
        )
    }
    for row in rows where row.count != columns.count {
        throw ScratchBirdDataException(message: "native rowset row shape mismatch", sqlState: "22000")
    }

    let types = inferNativeRowsetColumnTypes(rows)
    let nullBitmapBytes = (columns.count + 7) / 8
    var payload = Data("SBNR".utf8)
    appendUInt16LE(2, to: &payload)
    appendUInt16LE(0, to: &payload)
    appendUInt64LE(UInt64(rows.count), to: &payload)
    appendUInt32LE(UInt32(columns.count), to: &payload)
    payload.append(contentsOf: types)
    for column in columns {
        let encoded = Data(column.utf8)
        appendUInt32LE(UInt32(encoded.count), to: &payload)
        payload.append(encoded)
    }

    for row in rows {
        let bitmapStart = payload.count
        payload.append(Data(repeating: 0, count: nullBitmapBytes))
        for (index, value) in row.enumerated() {
            guard let value else {
                payload[bitmapStart + (index / 8)] |= UInt8(1 << (index % 8))
                continue
            }
            switch types[index] {
            case nativeRowsetTypeInt64:
                guard let parsed = Int64(value) else {
                    throw ScratchBirdDataException(message: "native rowset INT64 conversion failed", sqlState: "22000")
                }
                appendInt64LE(parsed, to: &payload)
            case nativeRowsetTypeBoolean:
                let normalized = value.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
                payload.append((normalized == "true" || normalized == "1") ? 1 : 0)
            case nativeRowsetTypeInt32:
                guard let parsed = Int32(value) else {
                    throw ScratchBirdDataException(message: "native rowset INT32 conversion failed", sqlState: "22000")
                }
                appendInt32LE(parsed, to: &payload)
            case nativeRowsetTypeUInt64:
                guard let parsed = UInt64(value) else {
                    throw ScratchBirdDataException(message: "native rowset UINT64 conversion failed", sqlState: "22000")
                }
                appendUInt64LE(parsed, to: &payload)
            case nativeRowsetTypeReal64:
                guard let parsed = Double(value) else {
                    throw ScratchBirdDataException(message: "native rowset REAL64 conversion failed", sqlState: "22000")
                }
                appendReal64LE(parsed, to: &payload)
            default:
                let encoded = Data(value.utf8)
                appendUInt32LE(UInt32(encoded.count), to: &payload)
                payload.append(encoded)
            }
        }
    }
    return payload
}

private func inferNativeRowsetColumnTypes(_ rows: [[String?]]) -> [UInt8] {
    let columnCount = rows[0].count
    var types = Array(repeating: nativeRowsetTypeText, count: columnCount)
    for column in 0..<columnCount {
        let values = rows.compactMap { $0[column] }
        if values.isEmpty {
            continue
        }
        if values.allSatisfy({ value in
            let lower = value.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            return lower == "true" || lower == "false"
        }) {
            types[column] = nativeRowsetTypeBoolean
        } else if values.allSatisfy({ losslessInt32($0) != nil }) {
            types[column] = nativeRowsetTypeInt32
        } else if values.allSatisfy({ losslessInt64($0) != nil }) {
            types[column] = nativeRowsetTypeInt64
        } else if values.allSatisfy({ losslessUInt64($0) != nil }) {
            types[column] = nativeRowsetTypeUInt64
        } else if values.allSatisfy({ losslessReal64($0) != nil }) {
            types[column] = nativeRowsetTypeReal64
        }
    }
    return types
}

private func losslessInt32(_ value: String) -> Int32? {
    guard let parsed = Int32(value), String(parsed) == value else { return nil }
    return parsed
}

private func losslessInt64(_ value: String) -> Int64? {
    guard let parsed = Int64(value), String(parsed) == value else { return nil }
    return parsed
}

private func losslessUInt64(_ value: String) -> UInt64? {
    guard let parsed = UInt64(value), String(parsed) == value else { return nil }
    return parsed
}

private func losslessReal64(_ value: String) -> Double? {
    let text = value.trimmingCharacters(in: .whitespacesAndNewlines)
    if text.isEmpty {
        return nil
    }
    switch text.lowercased() {
    case "nan", "+nan", "-nan", "inf", "+inf", "-inf", "infinity", "+infinity", "-infinity":
        return nil
    default:
        break
    }
    guard let parsed = Double(text), parsed.isFinite, text.contains(".") || text.contains("e") || text.contains("E") else {
        return nil
    }
    return parsed
}

private func appendUInt16LE(_ value: UInt16, to data: inout Data) {
    data.append(contentsOf: withUnsafeBytes(of: value.littleEndian, Array.init))
}

private func appendUInt32LE(_ value: UInt32, to data: inout Data) {
    data.append(contentsOf: withUnsafeBytes(of: value.littleEndian, Array.init))
}

private func appendUInt64LE(_ value: UInt64, to data: inout Data) {
    data.append(contentsOf: withUnsafeBytes(of: value.littleEndian, Array.init))
}

private func appendInt32LE(_ value: Int32, to data: inout Data) {
    data.append(contentsOf: withUnsafeBytes(of: value.littleEndian, Array.init))
}

private func appendInt64LE(_ value: Int64, to data: inout Data) {
    data.append(contentsOf: withUnsafeBytes(of: value.littleEndian, Array.init))
}

private func appendReal64LE(_ value: Double, to data: inout Data) {
    data.append(contentsOf: withUnsafeBytes(of: value.bitPattern.littleEndian, Array.init))
}
