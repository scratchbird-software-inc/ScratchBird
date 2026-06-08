// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

import XCTest
@testable import ScratchBird

final class TxnExecParityTests: XCTestCase {
    func testValidateTxnBeginOptionsAcceptsSupportedValues() throws {
        XCTAssertNoThrow(try validateTxnBeginOptions(
            isolationLevel: isolationSerializable,
            readCommittedMode: nil,
            accessMode: 1,
            autocommitMode: 1
        ))
    }

    func testValidateTxnBeginOptionsRejectsUnsupportedIsolation() throws {
        XCTAssertThrowsError(try validateTxnBeginOptions(
            isolationLevel: isolationSerializable + 1,
            readCommittedMode: nil,
            accessMode: nil,
            autocommitMode: nil
        ))
    }

    func testValidateTxnBeginOptionsRejectsUnsupportedAccessMode() throws {
        XCTAssertThrowsError(try validateTxnBeginOptions(
            isolationLevel: nil,
            readCommittedMode: nil,
            accessMode: 2,
            autocommitMode: nil
        ))
    }

    func testValidateTxnBeginOptionsRejectsUnsupportedAutocommitMode() throws {
        XCTAssertThrowsError(try validateTxnBeginOptions(
            isolationLevel: nil,
            readCommittedMode: nil,
            accessMode: nil,
            autocommitMode: 2
        ))
    }

    func testValidateTxnBeginOptionsRejectsReadCommittedModeWithSnapshotAlias() throws {
        XCTAssertThrowsError(try validateTxnBeginOptions(
            isolationLevel: isolationSerializable,
            readCommittedMode: .readConsistency,
            accessMode: nil,
            autocommitMode: nil
        )) { error in
            let sbError = error as? ScratchBirdNotSupportedException
            XCTAssertEqual(sbError?.sqlState, "0A000")
            XCTAssertTrue(sbError?.message.contains("READ COMMITTED isolation alias") ?? false)
        }
    }

    func testNormalizeSavepointNameTrimsAndRejectsBlank() throws {
        XCTAssertEqual(try normalizeSavepointName("  sp_main\t"), "sp_main")
        XCTAssertThrowsError(try normalizeSavepointName("   \n"))
    }

    func testRequireCancelableSequenceRejectsZero() throws {
        XCTAssertEqual(try requireCancelableSequence(17), 17)
        XCTAssertThrowsError(try requireCancelableSequence(0))
    }

    func testNormalizePortalResumeMaxRowsClampsValues() throws {
        XCTAssertEqual(normalizePortalResumeMaxRows(fetchSize: -3), 0)
        XCTAssertEqual(normalizePortalResumeMaxRows(fetchSize: 0), 0)
        XCTAssertEqual(normalizePortalResumeMaxRows(fetchSize: 128), 128)
        XCTAssertEqual(normalizePortalResumeMaxRows(fetchSize: Int(UInt32.max) + 17), UInt32.max)
    }

    func testBuildPreparedTransactionSqlEmitsCanonicalControlSql() throws {
        XCTAssertEqual(
            try buildPreparedTransactionSql(
                verb: "PREPARE TRANSACTION",
                globalTransactionId: "gid-1"
            ),
            "PREPARE TRANSACTION 'gid-1'"
        )
        XCTAssertEqual(
            try buildPreparedTransactionSql(
                verb: "COMMIT PREPARED",
                globalTransactionId: "gid-1"
            ),
            "COMMIT PREPARED 'gid-1'"
        )
        XCTAssertEqual(
            try buildPreparedTransactionSql(
                verb: "ROLLBACK PREPARED",
                globalTransactionId: "gid'2"
            ),
            "ROLLBACK PREPARED 'gid''2'"
        )
    }

    func testBuildPreparedTransactionSqlRejectsBlankGlobalTransactionId() throws {
        XCTAssertThrowsError(
            try buildPreparedTransactionSql(
                verb: "PREPARE TRANSACTION",
                globalTransactionId: "   "
            )
        ) { error in
            let sbError = error as? ScratchBirdProgrammingException
            XCTAssertEqual(sbError?.sqlState, "42601")
            XCTAssertTrue(sbError?.message.contains("Global transaction id is required") ?? false)
        }
    }

    func testPreparedAndDormantCapabilitiesStayExplicit() async throws {
        let connection = ScratchBirdConnection.debugCreateForTesting(
            config: ScratchBirdConfig(database: "db", user: "user")
        )
        XCTAssertTrue(connection.supportsPreparedTransactions())
        XCTAssertFalse(connection.supportsDormantReattach())

        do {
            try await connection.detachToDormant()
            XCTFail("Expected detachToDormant to fail closed")
        } catch let error as ScratchBirdNotSupportedException {
            XCTAssertEqual(error.sqlState, "0A000")
            XCTAssertTrue(error.message.contains("Dormant detach"))
        }

        do {
            try await connection.reattachDormant("dormant-1", authToken: "token")
            XCTFail("Expected reattachDormant to fail closed")
        } catch let error as ScratchBirdNotSupportedException {
            XCTAssertEqual(error.sqlState, "0A000")
            XCTAssertTrue(error.message.contains("Dormant reattach"))
        }
    }

    func testResumeSuspendedPortalRequiresExplicitSuspendedState() throws {
        let connection = ScratchBirdConnection.debugCreateForTesting(
            config: ScratchBirdConfig(database: "db", user: "user")
        )
        XCTAssertThrowsError(try connection.resumeSuspendedPortal(32)) { error in
            let sbError = error as? ScratchBirdOperationalException
            XCTAssertEqual(sbError?.sqlState, "55000")
            XCTAssertTrue(sbError?.message.contains("Portal resume requires an explicit suspended result state") ?? false)
        }
    }

    func testReadyStateCanMarkFreshBoundaryActiveWithZeroTxnId() {
        let connection = ScratchBirdConnection.debugCreateForTesting(
            config: ScratchBirdConfig(database: "db", user: "user")
        )
        connection.debugApplyRuntimeReadyStateForTesting(status: Character("T").asciiValue ?? 0, txnId: 0)
        XCTAssertEqual(field(connection, named: "txnId") as UInt64?, 0)
        XCTAssertEqual(field(connection, named: "transactionActive") as Bool?, true)
    }

    func testDefaultBeginAdoptsFreshBoundaryButNonDefaultFailsClosed() async throws {
        let connection = ScratchBirdConnection.debugCreateForTesting(
            config: ScratchBirdConfig(database: "db", user: "user")
        )
        connection.debugSeedAbandonedSessionStateForTesting(
            sequence: 0,
            lastQuerySequence: 0,
            attachmentId: Data(repeating: 0, count: 16),
            txnId: 0,
            transactionActive: true,
            explicitTransaction: false,
            parameters: [:],
            lastPlan: nil,
            lastSblr: nil
        )

        try await connection.begin()
        XCTAssertEqual(field(connection, named: "explicitTransaction") as Bool?, true)
        XCTAssertEqual(field(connection, named: "txnId") as UInt64?, 0)
        XCTAssertEqual(field(connection, named: "transactionActive") as Bool?, true)

        let nonDefault = ScratchBirdConnection.debugCreateForTesting(
            config: ScratchBirdConfig(database: "db", user: "user")
        )
        nonDefault.debugSeedAbandonedSessionStateForTesting(
            sequence: 0,
            lastQuerySequence: 0,
            attachmentId: Data(repeating: 0, count: 16),
            txnId: 0,
            transactionActive: true,
            explicitTransaction: false,
            parameters: [:],
            lastPlan: nil,
            lastSblr: nil
        )

        do {
            try await nonDefault.begin(readCommittedMode: .readConsistency)
            XCTFail("Expected non-default fresh-boundary begin adoption to fail closed")
        } catch let error as ScratchBirdNotSupportedException {
            XCTAssertEqual(error.sqlState, "0A000")
            XCTAssertTrue(error.message.contains("fresh native MGA boundaries"))
        }
    }

    func testBuildTxnBeginPayloadEncodesFlagsAndOptions() throws {
        let flags = txnFlagHasIsolation | txnFlagHasAccess | txnFlagHasDeferrable | txnFlagHasWait | txnFlagHasTimeout | txnFlagHasAutocommit
        let payload = buildTxnBeginPayload(
            flags: flags,
            conflictAction: 2,
            autocommitMode: 1,
            isolationLevel: isolationRepeatableRead,
            accessMode: 1,
            deferrable: 1,
            waitMode: 1,
            timeoutMs: 9000,
            readCommittedMode: readCommittedModeDefault
        )

        XCTAssertEqual(payload.count, 12)
        XCTAssertEqual(readUInt16LE(payload, 0), flags)
        XCTAssertEqual(payload[2], 2)
        XCTAssertEqual(payload[3], 1)
        XCTAssertEqual(payload[4], isolationRepeatableRead)
        XCTAssertEqual(payload[5], 1)
        XCTAssertEqual(payload[6], 1)
        XCTAssertEqual(payload[7], 1)
        XCTAssertEqual(readUInt32LE(payload, 8), 9000)
    }

    func testBuildTxnBeginPayloadExpandsForReadCommittedMode() throws {
        let flags = txnFlagHasIsolation | txnFlagHasTimeout | txnFlagHasReadCommittedMode
        let payload = buildTxnBeginPayload(
            flags: flags,
            conflictAction: 0,
            autocommitMode: 0,
            isolationLevel: isolationReadCommitted,
            accessMode: 0,
            deferrable: 0,
            waitMode: 0,
            timeoutMs: 25,
            readCommittedMode: readCommittedModeReadConsistency
        )

        XCTAssertEqual(payload.count, 16)
        XCTAssertEqual(readUInt16LE(payload, 0), flags)
        XCTAssertEqual(payload[4], isolationReadCommitted)
        XCTAssertEqual(readUInt32LE(payload, 8), 25)
        XCTAssertEqual(payload[12], readCommittedModeReadConsistency)
    }

    func testBuildTxnCommitRollbackAndSavepointPayloads() throws {
        XCTAssertEqual(buildTxnCommitPayload(flags: 3), Data([3, 0, 0, 0]))
        XCTAssertEqual(buildTxnRollbackPayload(flags: 4), Data([4, 0, 0, 0]))

        let savepoint = buildTxnSavepointPayload(name: "sp1")
        XCTAssertEqual(readUInt32LE(savepoint, 0), 3)
        XCTAssertEqual(String(data: savepoint.subdata(in: 4..<7), encoding: .utf8), "sp1")
    }

    func testBuildExecuteAndCancelPayloads() throws {
        let execute = buildExecutePayload(portal: "p1", maxRows: 42)
        XCTAssertEqual(readUInt32LE(execute, 0), 2)
        XCTAssertEqual(String(data: execute.subdata(in: 4..<6), encoding: .utf8), "p1")
        XCTAssertEqual(readUInt32LE(execute, 6), 42)

        let cancel = buildCancelPayload(cancelType: 0, targetSequence: 77)
        XCTAssertEqual(readUInt32LE(cancel, 0), 0)
        XCTAssertEqual(readUInt32LE(cancel, 4), 77)
    }

    private func readUInt16LE(_ data: Data, _ offset: Int) -> UInt16 {
        UInt16(littleEndian: data.subdata(in: offset..<(offset + 2)).withUnsafeBytes { $0.load(as: UInt16.self) })
    }

    private func readUInt32LE(_ data: Data, _ offset: Int) -> UInt32 {
        UInt32(littleEndian: data.subdata(in: offset..<(offset + 4)).withUnsafeBytes { $0.load(as: UInt32.self) })
    }

    private func field<T>(_ object: Any, named label: String) -> T? {
        let mirror = Mirror(reflecting: object)
        for child in mirror.children where child.label == label {
            return child.value as? T
        }
        return nil
    }
}
