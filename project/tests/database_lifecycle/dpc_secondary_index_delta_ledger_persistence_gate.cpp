// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "secondary_index_delta_ledger.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace uuid = scratchbird::core::uuid;
namespace platform = scratchbird::core::platform;

using platform::StatusCode;
using platform::TypedUuid;
using platform::UuidKind;
using platform::byte;
using platform::u64;

constexpr std::string_view kLedgerSearchKey = "DPC_SECONDARY_INDEX_DELTA_LEDGER";
constexpr std::string_view kGateSearchKey = "DPC_SECONDARY_INDEX_DELTA_LEDGER_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

u64 NowMillis() {
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

struct UuidFactory {
  u64 base_millis = NowMillis();

  TypedUuid Typed(UuidKind kind, u64 salt) const {
    const auto generated = uuid::GenerateEngineIdentityV7(kind, base_millis + salt);
    Require(generated.ok(), "DPC-021 generated UUID creation failed");
    return generated.value;
  }
};

idx::SecondaryIndexDeltaLedgerRecord Record(
    const UuidFactory& factory,
    u64 salt,
    idx::SecondaryIndexDeltaLedgerCommitState commit_state,
    idx::SecondaryIndexDeltaKind delta_kind = idx::SecondaryIndexDeltaKind::insert) {
  idx::SecondaryIndexDeltaLedgerRecord record;
  record.delta.delta_id = factory.Typed(UuidKind::object, 10 + salt);
  record.delta.index_uuid = factory.Typed(UuidKind::object, 100);
  record.delta.table_uuid = factory.Typed(UuidKind::object, 101);
  record.delta.row_uuid = factory.Typed(UuidKind::row, 200 + salt);
  record.delta.version_uuid = factory.Typed(UuidKind::row, 300 + salt);
  record.delta.transaction_uuid = factory.Typed(UuidKind::transaction, 400 + salt);
  record.delta.local_transaction_id = 42 + salt;
  record.delta.delta_kind = delta_kind;
  record.delta.key_payload = "key-payload-" + std::to_string(salt);
  record.delta.cleanup_horizon_token = "mga-cleanup-horizon-" + std::to_string(salt);
  record.delta.committed =
      commit_state != idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted;
  record.commit_state = commit_state;
  record.source_evidence_reference = "engine.secondary_delta.source." + std::to_string(salt);
  return record;
}

u64 FingerprintBytes(const std::vector<byte>& bytes) {
  u64 hash = 1469598103934665603ull;
  for (byte value : bytes) {
    hash ^= static_cast<u64>(value);
    hash *= 1099511628211ull;
  }
  return hash;
}

void StoreU32Little(std::vector<byte>* bytes, std::size_t offset, platform::u32 value) {
  const platform::u32 stored = platform::HostToLittle32(value);
  const auto* data = reinterpret_cast<const byte*>(&stored);
  std::copy_n(data, sizeof(stored), bytes->begin() + static_cast<std::ptrdiff_t>(offset));
}

void StoreU64Little(std::vector<byte>* bytes, std::size_t offset, u64 value) {
  const u64 stored = platform::HostToLittle64(value);
  const auto* data = reinterpret_cast<const byte*>(&stored);
  std::copy_n(data, sizeof(stored), bytes->begin() + static_cast<std::ptrdiff_t>(offset));
}

void RefreshChecksum(std::vector<byte>* bytes) {
  Require(bytes->size() >= sizeof(u64), "DPC-021 test checksum fixture too small");
  const std::vector<byte> material(bytes->begin(), bytes->end() - sizeof(u64));
  StoreU64Little(bytes, bytes->size() - sizeof(u64), FingerprintBytes(material));
}

std::size_t FirstRecordOperationKindOffset(const std::vector<byte>& bytes) {
  Require(bytes.size() > 8 + 4 + 4 + 4, "DPC-021 encoded fixture too small");
  std::size_t offset = 8 + 4 + 4;
  const auto read_u32 = [&](std::size_t at) {
    return platform::LoadLittle32(bytes.data() + at);
  };
  const platform::u32 artifact_len = read_u32(offset);
  offset += 4 + artifact_len;
  const platform::u32 feature_len = read_u32(offset);
  offset += 4 + feature_len;
  offset += 4;
  offset += 6 * (1 + 16);
  offset += 8;
  Require(offset + 4 < bytes.size(), "DPC-021 operation offset outside encoded fixture");
  return offset;
}

void RequireDiagnosticCode(const platform::DiagnosticRecord& diagnostic,
                           std::string_view code,
                           std::string_view message) {
  if (diagnostic.diagnostic_code != code) {
    std::cerr << "expected_code=" << code
              << " actual_code=" << diagnostic.diagnostic_code << '\n';
  }
  Require(diagnostic.diagnostic_code == code, message);
}

void ValidateRoundTripAndDeterminism() {
  const UuidFactory factory;
  const idx::SecondaryIndexDeltaLedgerLimits limits{4, 8192};
  idx::PersistentSecondaryIndexDeltaLedger ledger;
  const auto first = Record(factory, 1, idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge);
  const auto second = Record(factory, 2, idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge,
                             idx::SecondaryIndexDeltaKind::update_after);

  const auto append_first = idx::AppendPersistentSecondaryIndexDelta(&ledger, first, limits);
  Require(append_first.ok(), "DPC-021 first append failed");
  const auto append_second = idx::AppendPersistentSecondaryIndexDelta(&ledger, second, limits);
  Require(append_second.ok(), "DPC-021 second append failed");

  const auto encoded_a = idx::EncodePersistentSecondaryIndexDeltaLedger(ledger, limits);
  const auto encoded_b = idx::EncodePersistentSecondaryIndexDeltaLedger(ledger, limits);
  Require(encoded_a.ok() && encoded_b.ok(), "DPC-021 deterministic encode failed");
  Require(encoded_a.bytes == encoded_b.bytes, "DPC-021 persisted bytes were not deterministic");
  Require(idx::StableSecondaryIndexDeltaLedgerFingerprint(ledger, limits) != 0,
          "DPC-021 stable fingerprint was empty");

  const auto decoded = idx::DecodePersistentSecondaryIndexDeltaLedger(encoded_a.bytes, limits);
  Require(decoded.ok(), "DPC-021 decode rejected valid ledger image");
  Require(idx::PersistentSecondaryIndexDeltaLedgerEquals(ledger, decoded.ledger),
          "DPC-021 decoded ledger did not equal source ledger");

  const auto classified = idx::ClassifySecondaryIndexDeltaLedgerForRecovery(decoded.ledger);
  Require(classified.recovery_class ==
              idx::SecondaryIndexDeltaLedgerRecoveryClass::committed_premerge_requires_overlay_merge,
          "DPC-021 committed premerge classification changed");
  Require(classified.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::apply_overlay_then_merge,
          "DPC-021 committed premerge recovery action changed");
}

void ValidateBoundedOverflowAndInvalidIdentity() {
  const UuidFactory factory;
  const idx::SecondaryIndexDeltaLedgerLimits limits{1, 8192};
  idx::PersistentSecondaryIndexDeltaLedger ledger;
  const auto first = idx::AppendPersistentSecondaryIndexDelta(
      &ledger, Record(factory, 11, idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted), limits);
  Require(first.ok(), "DPC-021 bounded ledger first append failed");
  const auto overflow = idx::AppendPersistentSecondaryIndexDelta(
      &ledger, Record(factory, 12, idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted), limits);
  Require(!overflow.ok(), "DPC-021 bounded ledger accepted overflow append");
  Require(ledger.records.size() == 1, "DPC-021 overflow append changed ledger size");
  RequireDiagnosticCode(overflow.diagnostic,
                        "secondary_index_delta_ledger_overflow",
                        "DPC-021 overflow diagnostic changed");

  auto invalid = Record(factory, 13, idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted);
  invalid.delta.delta_id = TypedUuid{};
  const auto invalid_result = idx::AppendPersistentSecondaryIndexDelta(&ledger, invalid, {4, 8192});
  Require(!invalid_result.ok(), "DPC-021 append accepted invalid identity");
  RequireDiagnosticCode(invalid_result.diagnostic,
                        "secondary_index_delta_ledger_invalid_identity",
                        "DPC-021 invalid identity diagnostic changed");

  auto malformed = Record(factory, 14, idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted);
  malformed.delta.delta_kind = static_cast<idx::SecondaryIndexDeltaKind>(99);
  const auto malformed_result = idx::AppendPersistentSecondaryIndexDelta(&ledger, malformed, {4, 8192});
  Require(!malformed_result.ok(), "DPC-021 append accepted malformed operation kind");
  RequireDiagnosticCode(malformed_result.diagnostic,
                        "secondary_index_delta_ledger_malformed_operation",
                        "DPC-021 malformed operation diagnostic changed");
}

void ValidateDecodeRefusals() {
  const UuidFactory factory;
  const idx::SecondaryIndexDeltaLedgerLimits limits{4, 8192};
  idx::PersistentSecondaryIndexDeltaLedger ledger;
  Require(idx::AppendPersistentSecondaryIndexDelta(
              &ledger, Record(factory, 21, idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge), limits)
              .ok(),
          "DPC-021 setup append failed");
  const auto encoded = idx::EncodePersistentSecondaryIndexDeltaLedger(ledger, limits);
  Require(encoded.ok(), "DPC-021 setup encode failed");

  const auto missing = idx::DecodePersistentSecondaryIndexDeltaLedger({}, limits);
  Require(!missing.ok(), "DPC-021 decode accepted missing ledger image");
  RequireDiagnosticCode(missing.diagnostic,
                        "secondary_index_delta_ledger_missing",
                        "DPC-021 missing image diagnostic changed");

  auto corrupt = encoded.bytes;
  corrupt[3] ^= 0x7fu;
  const auto corrupt_result = idx::DecodePersistentSecondaryIndexDeltaLedger(corrupt, limits);
  Require(!corrupt_result.ok(), "DPC-021 decode accepted corrupt ledger image");
  RequireDiagnosticCode(corrupt_result.diagnostic,
                        "secondary_index_delta_ledger_corrupt_checksum",
                        "DPC-021 corrupt checksum diagnostic changed");

  auto wrong_version = encoded.bytes;
  StoreU32Little(&wrong_version, 8, 99);
  RefreshChecksum(&wrong_version);
  const auto wrong_version_result =
      idx::DecodePersistentSecondaryIndexDeltaLedger(wrong_version, limits);
  Require(!wrong_version_result.ok(), "DPC-021 decode accepted wrong version");
  RequireDiagnosticCode(wrong_version_result.diagnostic,
                        "secondary_index_delta_ledger_wrong_version",
                        "DPC-021 wrong version diagnostic changed");

  auto incompatible_image = encoded.bytes;
  incompatible_image[0] = static_cast<byte>('X');
  RefreshChecksum(&incompatible_image);
  const auto incompatible_image_result =
      idx::DecodePersistentSecondaryIndexDeltaLedger(incompatible_image, limits);
  Require(!incompatible_image_result.ok(), "DPC-021 decode accepted incompatible image");
  RequireDiagnosticCode(incompatible_image_result.diagnostic,
                        "secondary_index_delta_ledger_incompatible_object",
                        "DPC-021 incompatible image diagnostic changed");

  idx::PersistentSecondaryIndexDeltaLedger incompatible;
  incompatible.artifact_kind = "not_secondary_index_delta_ledger";
  const auto incompatible_result =
      idx::EncodePersistentSecondaryIndexDeltaLedger(incompatible, limits);
  Require(!incompatible_result.ok(), "DPC-021 encode accepted incompatible object");
  RequireDiagnosticCode(incompatible_result.diagnostic,
                        "secondary_index_delta_ledger_incompatible_object",
                        "DPC-021 incompatible object diagnostic changed");

  auto malformed = encoded.bytes;
  StoreU32Little(&malformed, FirstRecordOperationKindOffset(malformed), 99);
  RefreshChecksum(&malformed);
  const auto malformed_result = idx::DecodePersistentSecondaryIndexDeltaLedger(malformed, limits);
  Require(!malformed_result.ok(), "DPC-021 decode accepted malformed operation data");
  RequireDiagnosticCode(malformed_result.diagnostic,
                        "secondary_index_delta_ledger_malformed_operation",
                        "DPC-021 malformed decode diagnostic changed");
}

void ValidateRecoveryClassifications() {
  const UuidFactory factory;
  const idx::SecondaryIndexDeltaLedgerLimits limits{4, 8192};

  const auto empty = idx::ClassifySecondaryIndexDeltaLedgerImageForRecovery({}, limits);
  Require(empty.recovery_class == idx::SecondaryIndexDeltaLedgerRecoveryClass::empty_clean,
          "DPC-021 empty image was not clean");
  Require(empty.action == idx::SecondaryIndexDeltaLedgerRecoveryAction::no_action,
          "DPC-021 empty image action changed");

  idx::PersistentSecondaryIndexDeltaLedger uncommitted_ledger;
  Require(idx::AppendPersistentSecondaryIndexDelta(
              &uncommitted_ledger,
              Record(factory, 31, idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted),
              limits)
              .ok(),
          "DPC-021 uncommitted setup append failed");
  const auto uncommitted =
      idx::ClassifySecondaryIndexDeltaLedgerForRecovery(uncommitted_ledger);
  Require(uncommitted.recovery_class ==
              idx::SecondaryIndexDeltaLedgerRecoveryClass::has_uncommitted_precommit_delta,
          "DPC-021 uncommitted precommit classification changed");
  Require(uncommitted.action ==
              idx::SecondaryIndexDeltaLedgerRecoveryAction::retain_for_mga_transaction_finality,
          "DPC-021 uncommitted precommit action changed");

  idx::PersistentSecondaryIndexDeltaLedger repair_ledger;
  Require(idx::AppendPersistentSecondaryIndexDelta(
              &repair_ledger,
              Record(factory, 32, idx::SecondaryIndexDeltaLedgerCommitState::repair_rebuild_required),
              limits)
              .ok(),
          "DPC-021 repair setup append failed");
  const auto repair = idx::ClassifySecondaryIndexDeltaLedgerForRecovery(repair_ledger);
  Require(repair.recovery_class ==
              idx::SecondaryIndexDeltaLedgerRecoveryClass::repair_rebuild_required,
          "DPC-021 repair classification changed");
  Require(repair.action ==
              idx::SecondaryIndexDeltaLedgerRecoveryAction::rebuild_from_authoritative_base,
          "DPC-021 repair action changed");

  idx::PersistentSecondaryIndexDeltaLedger refused_ledger;
  Require(idx::AppendPersistentSecondaryIndexDelta(
              &refused_ledger,
              Record(factory, 33, idx::SecondaryIndexDeltaLedgerCommitState::refused),
              limits)
              .ok(),
          "DPC-021 refusal setup append failed");
  const auto refused = idx::ClassifySecondaryIndexDeltaLedgerForRecovery(refused_ledger);
  Require(refused.recovery_class == idx::SecondaryIndexDeltaLedgerRecoveryClass::refused,
          "DPC-021 refusal classification changed");
  Require(refused.fail_closed, "DPC-021 refusal did not fail closed");
}

void ValidateNoExternalAuthorityPayloads() {
  const UuidFactory factory;
  const idx::SecondaryIndexDeltaLedgerLimits limits{4, 8192};
  idx::PersistentSecondaryIndexDeltaLedger ledger;
  Require(idx::AppendPersistentSecondaryIndexDelta(
              &ledger, Record(factory, 41, idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge), limits)
              .ok(),
          "DPC-021 authority payload setup append failed");
  const auto encoded = idx::EncodePersistentSecondaryIndexDeltaLedger(ledger, limits);
  Require(encoded.ok(), "DPC-021 authority payload setup encode failed");
  const auto decoded = idx::DecodePersistentSecondaryIndexDeltaLedger(encoded.bytes, limits);
  Require(decoded.ok(), "DPC-021 authority payload setup decode failed");
  std::string body = decoded.ledger.artifact_kind;
  for (const auto& record : decoded.ledger.records) {
    body += '\n';
    body += record.delta.key_payload;
    body += '\n';
    body += record.delta.cleanup_horizon_token;
    body += '\n';
    body += record.source_evidence_reference;
  }
  const std::vector<std::string> forbidden = {
      std::string("par") + "ser",
      std::string("do") + "nor",
      std::string("w") + "al"};
  for (const auto& token : forbidden) {
    Require(body.find(token) == std::string::npos,
            "DPC-021 encoded ledger carried non-MGA authority payload text");
  }
}

}  // namespace

int main() {
  Require(kLedgerSearchKey == "DPC_SECONDARY_INDEX_DELTA_LEDGER",
          "DPC-021 ledger search key drifted");
  Require(kGateSearchKey == "DPC_SECONDARY_INDEX_DELTA_LEDGER_GATE",
          "DPC-021 gate search key drifted");
  Require(idx::kSecondaryIndexDeltaLedgerMinSupportedFormatMajor == 1 &&
              idx::kSecondaryIndexDeltaLedgerMinSupportedFormatMinor == 0 &&
              idx::kSecondaryIndexDeltaLedgerFormatMajor == 2 &&
              idx::kSecondaryIndexDeltaLedgerFormatMinor == 0 &&
              idx::kSecondaryIndexDeltaLedgerMaxSupportedFormatMajor == 3 &&
              idx::kSecondaryIndexDeltaLedgerMaxSupportedFormatMinor == 0,
          "DPC-021 ledger format version drifted from the DPC-009 persisted-format contract");

  ValidateRoundTripAndDeterminism();
  ValidateBoundedOverflowAndInvalidIdentity();
  ValidateDecodeRefusals();
  ValidateRecoveryClassifications();
  ValidateNoExternalAuthorityPayloads();
  return EXIT_SUCCESS;
}
