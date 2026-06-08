// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System.Data;

namespace ScratchBird.Data;

public enum ScratchBirdTransactionAccessMode : byte
{
    ReadWrite = 0,
    ReadOnly = 1
}

public enum ScratchBirdReadCommittedMode : byte
{
    Default = ProtocolConstants.ReadCommittedModeDefault,
    ReadConsistency = ProtocolConstants.ReadCommittedModeReadConsistency,
    RecordVersion = ProtocolConstants.ReadCommittedModeRecordVersion,
    NoRecordVersion = ProtocolConstants.ReadCommittedModeNoRecordVersion
}

public sealed class ScratchBirdTransactionOptions
{
    // Public isolation aliases map onto the canonical MGA modes:
    // ReadCommitted => READ COMMITTED
    // RepeatableRead => SNAPSHOT
    // Serializable / Snapshot / Chaos => SNAPSHOT TABLE STABILITY
    // ReadCommittedMode selects the canonical READ COMMITTED sub-mode when the
    // public isolation surface stays in the READ COMMITTED alias family.
    public IsolationLevel IsolationLevel { get; set; } = IsolationLevel.ReadCommitted;
    public ScratchBirdReadCommittedMode? ReadCommittedMode { get; set; }
    public ScratchBirdTransactionAccessMode? AccessMode { get; set; }
    public bool? Deferrable { get; set; }
    public bool? Wait { get; set; }
    public int? TimeoutMs { get; set; }
    public bool? AutoCommit { get; set; }
}
