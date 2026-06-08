// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_SECONDARY_INDEX_DELTA_LEDGER
#include "secondary_index_delta_overlay.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::byte;

inline constexpr const char* kSecondaryIndexDeltaLedgerSearchKey =
    "DPC_SECONDARY_INDEX_DELTA_LEDGER";
inline constexpr const char* kSecondaryIndexDeltaLedgerArtifactKind =
    "dpc_secondary_index_delta_ledger";
inline constexpr const char* kSecondaryIndexDeltaLedgerFeatureMapKey =
    "optimizer.persisted.secondary_index_delta_ledgers";
inline constexpr u32 kSecondaryIndexDeltaLedgerMinSupportedFormatMajor = 1;
inline constexpr u32 kSecondaryIndexDeltaLedgerMinSupportedFormatMinor = 0;
inline constexpr u32 kSecondaryIndexDeltaLedgerFormatMajor = 2;
inline constexpr u32 kSecondaryIndexDeltaLedgerFormatMinor = 0;
inline constexpr u32 kSecondaryIndexDeltaLedgerMaxSupportedFormatMajor = 3;
inline constexpr u32 kSecondaryIndexDeltaLedgerMaxSupportedFormatMinor = 0;

enum class SecondaryIndexDeltaLedgerCommitState : u32 {
  precommit_uncommitted = 1,
  committed_premerge = 2,
  merged_cleaned = 3,
  repair_rebuild_required = 4,
  refused = 5
};

enum class SecondaryIndexDeltaLedgerRecoveryClass : u32 {
  empty_clean = 1,
  has_uncommitted_precommit_delta = 2,
  committed_premerge_requires_overlay_merge = 3,
  clean_after_merge = 4,
  repair_rebuild_required = 5,
  refused = 6,
  corrupt_incompatible_fail_closed = 7
};

enum class SecondaryIndexDeltaLedgerRecoveryAction : u32 {
  no_action = 1,
  retain_for_mga_transaction_finality = 2,
  apply_overlay_then_merge = 3,
  rebuild_from_authoritative_base = 4,
  refuse_open = 5,
  fail_closed = 6
};

struct SecondaryIndexDeltaLedgerRecord {
  SecondaryIndexDeltaEntry delta;
  SecondaryIndexDeltaLedgerCommitState commit_state =
      SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted;
  std::string source_evidence_reference;
};

struct SecondaryIndexDeltaLedgerLimits {
  u64 max_entries = 1024;
  u64 max_encoded_bytes = 1024 * 1024;
};

struct PersistentSecondaryIndexDeltaLedger {
  u32 format_major = kSecondaryIndexDeltaLedgerFormatMajor;
  u32 format_minor = kSecondaryIndexDeltaLedgerFormatMinor;
  std::string artifact_kind = kSecondaryIndexDeltaLedgerArtifactKind;
  std::vector<SecondaryIndexDeltaLedgerRecord> records;
  u64 encoded_bytes = 0;
};

struct SecondaryIndexDeltaLedgerAppendResult {
  Status status;
  bool appended = false;
  u64 record_count = 0;
  u64 encoded_bytes = 0;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && appended; }
};

struct SecondaryIndexDeltaLedgerEncodeResult {
  Status status;
  std::vector<byte> bytes;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct SecondaryIndexDeltaLedgerDecodeResult {
  Status status;
  PersistentSecondaryIndexDeltaLedger ledger;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct SecondaryIndexDeltaLedgerRecoveryResult {
  Status status;
  SecondaryIndexDeltaLedgerRecoveryClass recovery_class =
      SecondaryIndexDeltaLedgerRecoveryClass::corrupt_incompatible_fail_closed;
  SecondaryIndexDeltaLedgerRecoveryAction action =
      SecondaryIndexDeltaLedgerRecoveryAction::fail_closed;
  bool fail_closed = true;
  u64 uncommitted_delta_count = 0;
  u64 committed_premerge_delta_count = 0;
  std::string stable_reason;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* SecondaryIndexDeltaLedgerCommitStateName(
    SecondaryIndexDeltaLedgerCommitState state);
const char* SecondaryIndexDeltaLedgerRecoveryClassName(
    SecondaryIndexDeltaLedgerRecoveryClass recovery_class);
const char* SecondaryIndexDeltaLedgerRecoveryActionName(
    SecondaryIndexDeltaLedgerRecoveryAction action);

bool SecondaryIndexDeltaLedgerRecordEquals(
    const SecondaryIndexDeltaLedgerRecord& left,
    const SecondaryIndexDeltaLedgerRecord& right);
bool PersistentSecondaryIndexDeltaLedgerEquals(
    const PersistentSecondaryIndexDeltaLedger& left,
    const PersistentSecondaryIndexDeltaLedger& right);

SecondaryIndexDeltaLedgerAppendResult AppendPersistentSecondaryIndexDelta(
    PersistentSecondaryIndexDeltaLedger* ledger,
    const SecondaryIndexDeltaLedgerRecord& record,
    const SecondaryIndexDeltaLedgerLimits& limits);

SecondaryIndexDeltaLedgerEncodeResult EncodePersistentSecondaryIndexDeltaLedger(
    const PersistentSecondaryIndexDeltaLedger& ledger,
    const SecondaryIndexDeltaLedgerLimits& limits);
SecondaryIndexDeltaLedgerDecodeResult DecodePersistentSecondaryIndexDeltaLedger(
    const std::vector<byte>& bytes,
    const SecondaryIndexDeltaLedgerLimits& limits);

u64 StableSecondaryIndexDeltaLedgerFingerprint(
    const PersistentSecondaryIndexDeltaLedger& ledger,
    const SecondaryIndexDeltaLedgerLimits& limits);

SecondaryIndexDeltaLedgerRecoveryResult ClassifySecondaryIndexDeltaLedgerForRecovery(
    const PersistentSecondaryIndexDeltaLedger& ledger);
SecondaryIndexDeltaLedgerRecoveryResult ClassifySecondaryIndexDeltaLedgerImageForRecovery(
    const std::vector<byte>& bytes,
    const SecondaryIndexDeltaLedgerLimits& limits);

DiagnosticRecord MakeSecondaryIndexDeltaLedgerDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
