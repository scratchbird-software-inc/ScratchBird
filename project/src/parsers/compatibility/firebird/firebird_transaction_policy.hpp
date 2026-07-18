// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::firebird {

enum class FirebirdTransactionPolicySource {
  kTpb,
  kSql,
};

enum class FirebirdTransactionIsolation {
  kConcurrency,
  kConsistency,
  kReadCommitted,
};

enum class FirebirdRecordVersionMode {
  kDatabaseDefault,
  kRecordVersion,
  kNoRecordVersion,
  kReadConsistency,
};

enum class FirebirdTransactionWaitMode {
  kWait,
  kNoWait,
};

// A parser-owned request descriptor only. Transaction identity, admission,
// snapshot selection, visibility, and finality remain engine-owned MGA state.
struct FirebirdTransactionPolicy {
  FirebirdTransactionPolicySource source{FirebirdTransactionPolicySource::kTpb};
  std::uint8_t tpb_version{0};
  FirebirdTransactionIsolation isolation{FirebirdTransactionIsolation::kConcurrency};
  FirebirdRecordVersionMode record_version_mode{
      FirebirdRecordVersionMode::kDatabaseDefault};
  FirebirdTransactionWaitMode wait_mode{FirebirdTransactionWaitMode::kWait};
  bool read_only{false};
  bool isolation_explicit{false};
  bool record_version_explicit{false};
  bool wait_mode_explicit{false};
  bool access_mode_explicit{false};
  bool lock_timeout_present{false};
  std::uint16_t lock_timeout_seconds{0};
};

// Ordered Firebird status-vector symbols and arguments are kept separate so a
// worker can later render them without reconstructing upstream diagnostics.
struct FirebirdTransactionPolicyDiagnostic {
  std::string scratchbird_code;
  std::vector<std::string> firebird_status_symbols;
  std::vector<std::string> arguments;
  std::int32_t sql_code{0};
  std::size_t input_offset{0};
};

struct FirebirdTransactionPolicyResult {
  bool ok{false};
  FirebirdTransactionPolicy policy;
  FirebirdTransactionPolicyDiagnostic diagnostic;
  // For SQL input this is the source-equivalent, fixed-order Firebird TPB.
  // An option-free SET TRANSACTION intentionally produces an empty TPB.
  std::vector<std::uint8_t> canonical_tpb;
};

struct NeutralTransactionSblrField {
  std::string name;
  std::string value;
};

FirebirdTransactionPolicyResult ParseFirebirdTransactionTpb(
    std::span<const std::uint8_t> tpb);

FirebirdTransactionPolicyResult ParseFirebirdSetTransactionSql(
    std::string_view sql);

// The current neutral transaction envelope has no record-version field.
// kRecordVersion and kNoRecordVersion therefore both render as
// transaction_isolation_level=read_committed. The Firebird-owned policy and
// canonical TPB retain the distinction, but this renderer alone does not claim
// exact legacy-config semantics when Firebird database ReadConsistency is 0.
std::vector<NeutralTransactionSblrField> RenderNeutralTransactionSblrFields(
    const FirebirdTransactionPolicy& policy);

std::string EncodeNeutralTransactionSblrFields(
    const FirebirdTransactionPolicy& policy);

}  // namespace scratchbird::parser::firebird
