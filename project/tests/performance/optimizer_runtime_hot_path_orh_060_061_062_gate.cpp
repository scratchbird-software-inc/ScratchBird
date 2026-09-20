// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "direct_binary_result_frame.hpp"
#include "streaming_cursor_manager.hpp"
#include "vectorized_result_batch.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>
#include <openssl/hmac.h>

namespace exec = scratchbird::engine::executor;
namespace wire = scratchbird::wire;

namespace allocation_fault {
thread_local bool enabled = false;
thread_local std::size_t calls = 0;
thread_local std::size_t fail_at = 0;
thread_local bool consumed = false;
}

void* operator new(std::size_t size) {
  if (allocation_fault::enabled && ++allocation_fault::calls == allocation_fault::fail_at) {
    allocation_fault::enabled = false;
    allocation_fault::consumed = true;
    throw std::bad_alloc();
  }
  if (auto* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
  throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
using scratchbird::tests::FixtureUuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-060/061/062 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& values,
              const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

scratchbird::core::platform::u64 EvidenceU64(
    const std::vector<std::string>& values,
    const std::string& key) {
  const std::string prefix = key + "=";
  for (const auto& value : values) {
    if (value.rfind(prefix, 0) != 0) {
      continue;
    }
    scratchbird::core::platform::u64 parsed = 0;
    for (std::size_t index = prefix.size(); index < value.size(); ++index) {
      const char ch = value[index];
      Require(ch >= '0' && ch <= '9', "non-numeric evidence value");
      parsed = (parsed * 10u) +
               static_cast<scratchbird::core::platform::u64>(ch - '0');
    }
    return parsed;
  }
  Fail("required numeric evidence missing");
}

wire::StreamingCursorState CursorState() {
  wire::StreamingCursorState state;
  state.cursor_id = FixtureUuid(60, 1);
  state.plan_result_contract_hash = "fnv1a64:orh060-result-contract";
  state.catalog_epoch = 59;
  state.descriptor_epoch = 60;
  state.transaction_snapshot_class = "mga_statement_snapshot";
  state.transaction_uuid = FixtureUuid(60, 2);
  state.local_transaction_id = 601;
  state.snapshot_visible_through_local_transaction_id = 599;
  state.security_epoch = 61;
  state.redaction_epoch = 62;
  state.route_kind = "embedded";
  state.frame_sequence = 7;
  state.expiry_deadline_unix_millis = 10'000;
  state.client_credit.frame_credit = 2;
  state.client_credit.row_credit = 4;
  state.client_credit.byte_credit = 4096;
  state.client_credit.backpressure_active = false;
  return state;
}

void CursorManagerTracksStateAndCredit() {
  wire::StreamingCursorManager manager;
  const auto opened =
      manager.OpenCursor({.state = CursorState(), .now_unix_millis = 100});
  Require(opened.ok(), "cursor open failed");
  const auto duplicate =
      manager.OpenCursor({.state = CursorState(), .now_unix_millis = 100});
  Require(!duplicate.ok(), "duplicate cursor open was admitted");
  Require(duplicate.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.DUPLICATE_CURSOR_ID",
          "duplicate cursor diagnostic mismatch");
  Require(opened.state.cursor_id == CursorState().cursor_id,
          "cursor id not retained");
  Require(opened.state.plan_result_contract_hash ==
              "fnv1a64:orh060-result-contract",
          "result contract hash not retained");
  Require(opened.state.catalog_epoch == 59,
          "catalog epoch not retained");
  Require(opened.state.descriptor_epoch == 60,
          "descriptor epoch not retained");
  Require(opened.state.transaction_snapshot_class ==
              "mga_statement_snapshot",
          "snapshot class not retained");
  Require(opened.state.transaction_uuid ==
              FixtureUuid(60, 2),
          "transaction uuid not retained");
  Require(opened.state.local_transaction_id == 601,
          "local transaction id not retained");
  Require(opened.state.snapshot_visible_through_local_transaction_id == 599,
          "snapshot visible-through id not retained");
  Require(opened.state.security_epoch == 61,
          "security epoch not retained");
  Require(opened.state.redaction_epoch == 62,
          "redaction epoch not retained");
  Require(opened.state.route_kind == "embedded", "route kind not retained");
  Require(opened.state.frame_sequence == 7,
          "frame sequence not retained");
  Require(!opened.state.mga_visibility_or_finality_authority,
          "cursor claimed MGA finality authority");
  Require(opened.state.advisory_metadata_only,
          "cursor did not remain advisory metadata");
  Require(Contains(opened.evidence,
                   "cursor_mga_visibility_or_finality_authority=false"),
          "MGA authority evidence missing");

  const auto binding = wire::StreamingCursorBindingFromState(opened.state);
  const auto admitted =
      manager.ValidateFetch({.expected = binding, .now_unix_millis = 101});
  Require(admitted.ok(), "fetch was not admitted with credit");

  auto wrong_txn_uuid = binding;
  wrong_txn_uuid.transaction_uuid =
      FixtureUuid(60, 3);
  const auto txn_uuid_refused = manager.ValidateFetch(
      {.expected = wrong_txn_uuid, .now_unix_millis = 101});
  Require(txn_uuid_refused.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.TRANSACTION_UUID_MISMATCH",
          "manager transaction uuid mismatch diagnostic mismatch");

  auto wrong_local_tx = binding;
  wrong_local_tx.local_transaction_id = 602;
  const auto local_tx_refused = manager.ValidateFetch(
      {.expected = wrong_local_tx, .now_unix_millis = 101});
  Require(local_tx_refused.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.LOCAL_TRANSACTION_ID_MISMATCH",
          "manager local transaction mismatch diagnostic mismatch");

  auto wrong_snapshot_visible = binding;
  wrong_snapshot_visible.snapshot_visible_through_local_transaction_id = 598;
  const auto snapshot_visible_refused = manager.ValidateFetch(
      {.expected = wrong_snapshot_visible, .now_unix_millis = 101});
  Require(snapshot_visible_refused.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.SNAPSHOT_VISIBLE_THROUGH_MISMATCH",
          "manager snapshot visible-through mismatch diagnostic mismatch");

  auto wrong_expiry = binding;
  wrong_expiry.expiry_deadline_unix_millis = 10'001;
  const auto expiry_refused = manager.ValidateFetch(
      {.expected = wrong_expiry, .now_unix_millis = 101});
  Require(expiry_refused.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.EXPIRY_MISMATCH",
          "manager expiry mismatch diagnostic mismatch");

  auto delivered = manager.RecordFrameDelivery(
      {.expected = binding,
       .row_count = 2,
       .byte_count = 300,
       .now_unix_millis = 102});
  Require(delivered.ok(), "frame delivery rejected");
  Require(delivered.state.frame_sequence == 8,
          "frame sequence did not advance");
  Require(delivered.state.client_credit.frame_credit == 1,
          "frame credit not consumed");
  Require(delivered.state.client_credit.row_credit == 2,
          "row credit not consumed");
  Require(delivered.state.client_credit.byte_credit == 3796,
          "byte credit not consumed");

  auto backpressured = manager.GrantCredit(
      CursorState().cursor_id,
      {.frame_credit = 0, .row_credit = 8, .byte_credit = 4096});
  Require(backpressured.ok(), "credit update failed");
  auto blocked_binding = binding;
  blocked_binding.frame_sequence = 8;
  const auto blocked = manager.ValidateFetch(
      {.expected = blocked_binding, .now_unix_millis = 103});
  Require(!blocked.ok(), "zero frame credit did not block fetch");
  Require(blocked.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.BACKPRESSURE",
          "backpressure diagnostic mismatch");

  manager.GrantCredit(CursorState().cursor_id,
                      {.frame_credit = 1, .row_credit = 2, .byte_credit = 512});
  const auto cancelled = manager.CancelCursor(CursorState().cursor_id);
  Require(cancelled.ok(), "cancel request failed");
  const auto refused = manager.ValidateFetch(
      {.expected = blocked_binding, .now_unix_millis = 104});
  Require(!refused.ok(), "cancelled cursor admitted fetch");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_ORH_STREAMING_CURSOR.CANCELLED",
          "cancel diagnostic mismatch");
}

void ContinuationTokensValidateBindingsAndTamper() {
  const auto binding = wire::StreamingCursorBindingFromState(CursorState());
  const wire::ContinuationTokenSecret secret{.key_id = "orh061.key.with.dot",
                                             .secret_material = "test-secret"};
  const auto issued = wire::IssueContinuationToken(binding, secret);
  Require(issued.ok(), "token issue failed");
  Require(!issued.token.empty(), "issued token was empty");
  const auto admitted =
      wire::ValidateContinuationToken(issued.token, binding, secret, 100);
  Require(admitted.ok(), "issued token was not admitted");
  Require(Contains(admitted.evidence,
                   "continuation_token_signature_algorithm=HMAC-SHA256"),
          "HMAC-SHA256 token evidence missing");
  Require(Contains(admitted.evidence,
                   "continuation_token_key_id_encoding=hex"),
          "hex key id encoding evidence missing");

  auto tampered_token = issued.token;
  tampered_token.back() = tampered_token.back() == '0' ? '1' : '0';
  const auto tampered =
      wire::ValidateContinuationToken(tampered_token, binding, secret, 100);
  Require(!tampered.ok(), "tampered token was admitted");
  Require(tampered.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.TAMPERED",
          "tampered token diagnostic mismatch");

  auto wrong_route = binding;
  wrong_route.route_kind = "ipc";
  const auto route_refused =
      wire::ValidateContinuationToken(issued.token, wrong_route, secret, 100);
  Require(route_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.ROUTE_MISMATCH",
          "route mismatch diagnostic mismatch");

  auto wrong_epoch = binding;
  wrong_epoch.security_epoch = 6001;
  const auto epoch_refused =
      wire::ValidateContinuationToken(issued.token, wrong_epoch, secret, 100);
  Require(epoch_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.SECURITY_EPOCH_MISMATCH",
          "security epoch mismatch diagnostic mismatch");

  auto wrong_catalog = binding;
  wrong_catalog.catalog_epoch = 5901;
  const auto catalog_refused =
      wire::ValidateContinuationToken(issued.token, wrong_catalog, secret, 100);
  Require(catalog_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.CATALOG_EPOCH_MISMATCH",
          "catalog epoch mismatch diagnostic mismatch");

  auto wrong_contract = binding;
  wrong_contract.plan_result_contract_hash = "fnv1a64:other-contract";
  const auto contract_refused = wire::ValidateContinuationToken(
      issued.token, wrong_contract, secret, 100);
  Require(contract_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.CONTRACT_MISMATCH",
          "contract mismatch diagnostic mismatch");

  auto wrong_token_txn_uuid = binding;
  wrong_token_txn_uuid.transaction_uuid =
      FixtureUuid(60, 3);
  const auto token_txn_uuid_refused = wire::ValidateContinuationToken(
      issued.token, wrong_token_txn_uuid, secret, 100);
  Require(token_txn_uuid_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.TRANSACTION_UUID_MISMATCH",
          "token transaction uuid mismatch diagnostic mismatch");

  auto wrong_token_local_tx = binding;
  wrong_token_local_tx.local_transaction_id = 602;
  const auto token_local_tx_refused = wire::ValidateContinuationToken(
      issued.token, wrong_token_local_tx, secret, 100);
  Require(token_local_tx_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.LOCAL_TRANSACTION_ID_MISMATCH",
          "token local transaction mismatch diagnostic mismatch");

  auto wrong_token_snapshot_visible = binding;
  wrong_token_snapshot_visible.snapshot_visible_through_local_transaction_id =
      598;
  const auto token_snapshot_visible_refused =
      wire::ValidateContinuationToken(
          issued.token, wrong_token_snapshot_visible, secret, 100);
  Require(token_snapshot_visible_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.SNAPSHOT_VISIBLE_THROUGH_MISMATCH",
          "token snapshot visible-through mismatch diagnostic mismatch");

  auto wrong_token_expiry = binding;
  wrong_token_expiry.expiry_deadline_unix_millis = 10'001;
  const auto token_expiry_refused = wire::ValidateContinuationToken(
      issued.token, wrong_token_expiry, secret, 100);
  Require(token_expiry_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.EXPIRY_MISMATCH",
          "token expiry mismatch diagnostic mismatch");

  auto wrong_sequence = binding;
  wrong_sequence.frame_sequence = 8;
  const auto sequence_refused = wire::ValidateContinuationToken(
      issued.token, wrong_sequence, secret, 100);
  Require(sequence_refused.diagnostic.diagnostic_code ==
              "SB_ORH_CONTINUATION_TOKEN.SEQUENCE_MISMATCH",
          "sequence mismatch diagnostic mismatch");
}

std::string OracleHex(std::string_view bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (unsigned char byte : bytes) {
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}

using TokenFields = std::vector<std::pair<std::string, std::string>>;

TokenFields OracleFields() {
  const auto raw = [](wire::StreamingCursorUuid id) {
    return std::string(reinterpret_cast<const char*>(id.bytes.data()), 16);
  };
  return {{"cursor_id", raw(FixtureUuid(60, 1))},
          {"result_contract_hash", "fnv1a64:orh060-result-contract"},
          {"catalog_epoch", "59"}, {"descriptor_epoch", "60"},
          {"transaction_snapshot_class", "mga_statement_snapshot"},
          {"transaction_uuid", raw(FixtureUuid(60, 2))},
          {"local_transaction_id", "601"},
          {"snapshot_visible_through_local_transaction_id", "599"},
          {"security_epoch", "61"}, {"redaction_epoch", "62"},
          {"route_kind", "embedded"}, {"frame_sequence", "7"},
          {"expiry_deadline_unix_millis", "10000"}};
}

std::string OraclePayload(const TokenFields& fields) {
  std::string result;
  for (const auto& [key, value] : fields)
    result += std::to_string(key.size()) + ":" + key +
              std::to_string(value.size()) + ":" + value;
  return result;
}

std::string OracleToken(std::string_view payload,
                        const wire::ContinuationTokenSecret& secret) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
  unsigned length = 0;
  const std::string message = secret.key_id + ':' + std::string(payload);
  Require(HMAC(EVP_sha256(), secret.secret_material.data(),
               static_cast<int>(secret.secret_material.size()),
               reinterpret_cast<const unsigned char*>(message.data()),
               message.size(), mac.data(), &length) && length == 32,
          "independent HMAC oracle failed");
  return "SBORH2." + OracleHex(payload) + '.' + OracleHex(secret.key_id) + '.' +
         OracleHex(std::string_view(reinterpret_cast<const char*>(mac.data()), length));
}

void BinaryIdentityAndCanonicalTokenProfiles() {
  const auto state = CursorState();
  const auto binding = wire::StreamingCursorBindingFromState(state);
  const wire::ContinuationTokenSecret secret{"key", "independent-test-secret"};
  const auto fields = OracleFields();
  const auto payload = OraclePayload(fields);
  const auto token = OracleToken(payload, secret);
  Require(wire::IssueContinuationToken(binding, secret).token == token,
          "production token differs from independent binary payload/HMAC oracle");
  Require(wire::ValidateContinuationToken(token, binding, secret, 10000).ok(),
          "token rejected at inclusive expiry boundary");
  Require(!wire::ValidateContinuationToken(token, binding, secret, 10001).ok(),
          "expired token admitted");
  auto legacy = token;
  legacy[5] = '1';
  Require(!wire::ValidateContinuationToken(legacy, binding, secret, 100).ok(),
          "legacy token admitted");
  Require(!wire::ValidateContinuationToken(std::string(32769, 'a'), binding, secret, 100).ok(),
          "oversize token admitted");
  const auto refused_payload = [&](const TokenFields& changed) {
    Require(!wire::ValidateContinuationToken(OracleToken(OraclePayload(changed), secret),
                                             binding, secret, 100).ok(),
            "signed malformed token admitted");
  };
  for (std::size_t i = 0; i < fields.size(); ++i) {
    auto changed = fields;
    changed.erase(changed.begin() + i);
    refused_payload(changed);
    changed = fields;
    changed.push_back(fields[i]);
    refused_payload(changed);
    changed = fields;
    changed[i].first += "_unknown";
    refused_payload(changed);
    changed = fields;
    std::swap(changed[i], changed[(i + 1) % fields.size()]);
    refused_payload(changed);
  }
  for (std::size_t slot : {0u, 5u}) {
    auto changed = fields;
    changed[slot].second = "019d0000-003c-7000-8000-000000000001";
    refused_payload(changed);
    changed[slot].second.assign(16, '\0');
    refused_payload(changed);
    for (unsigned byte = 0; byte < 16; ++byte) {
      changed = fields;
      changed[slot].second[byte] ^= 1;
      refused_payload(changed);
    }
  }
  auto changed = fields;
  changed[2].second = "059";
  refused_payload(changed);
  changed[2].second = "18446744073709551616";
  refused_payload(changed);
  Require(!wire::ValidateContinuationToken(OracleToken("0" + payload, secret),
                                           binding, secret, 100).ok(),
          "noncanonical length spelling admitted");
  for (unsigned version = 0; version < 16; ++version) {
    if (version == 7) continue;
    auto bad = binding;
    bad.cursor_id.bytes[6] = static_cast<std::uint8_t>(version << 4);
    Require(!wire::IssueContinuationToken(bad, secret).ok(), "non-v7 cursor issued");
    bad = binding;
    bad.transaction_uuid.bytes[6] = static_cast<std::uint8_t>(version << 4);
    Require(!wire::IssueContinuationToken(bad, secret).ok(), "non-v7 transaction issued");
  }
  for (unsigned variant : {0u, 0x40u, 0xc0u}) {
    auto bad = state;
    bad.cursor_id.bytes[8] = static_cast<std::uint8_t>(variant);
    wire::StreamingCursorManager manager;
    Require(!manager.OpenCursor({.state = bad, .now_unix_millis = 100}).ok(),
            "non-RFC cursor admitted");
  }
  auto maximum = binding;
  maximum.plan_result_contract_hash.assign(4096, 'h');
  maximum.transaction_snapshot_class.assign(4096, 's');
  maximum.route_kind.assign(4096, 'r');
  const wire::ContinuationTokenSecret maximum_secret{std::string(256, 'k'), "secret"};
  const auto maximum_token = wire::IssueContinuationToken(maximum, maximum_secret);
  Require(maximum_token.ok() && maximum_token.token.size() <= 32768 &&
              wire::ValidateContinuationToken(maximum_token.token, maximum, maximum_secret, 100).ok(),
          "maximum bounded token failed");
  maximum.route_kind.push_back('x');
  Require(!wire::IssueContinuationToken(maximum, maximum_secret).ok(),
          "oversize binding text issued");
  auto zero_snapshot = binding;
  zero_snapshot.snapshot_visible_through_local_transaction_id = 0;
  const auto zero_token = wire::IssueContinuationToken(zero_snapshot, secret);
  Require(zero_token.ok() && wire::ValidateContinuationToken(
              zero_token.token, zero_snapshot, secret, 100).ok(),
          "cursor invented a nonzero visibility boundary");
  for (unsigned identity = 0; identity < 2; ++identity) {
    auto invalid = binding;
    auto fields_with_nil = fields;
    if (identity == 0) invalid.cursor_id = {};
    else invalid.transaction_uuid = {};
    fields_with_nil[identity == 0 ? 0 : 5].second.assign(16, '\0');
    Require(!wire::IssueContinuationToken(invalid, secret).ok() &&
                !wire::ValidateContinuationToken(OracleToken(OraclePayload(fields_with_nil), secret),
                                                 invalid, secret, 100).ok(),
            "signed nil identity admitted against matching nil expectation");
  }
}

wire::StreamingCursorState GovernedState(
    wire::memory::ResultCursorPlanMemoryGovernor* governor,
    wire::memory::HierarchicalMemoryBudgetLedger* ledger) {
  auto state = CursorState();
  state.memory_governor = governor;
  state.memory_ledger = ledger;
  state.memory_scope.process_id = FixtureUuid(61, 1);
  state.memory_scope.database_id = FixtureUuid(61, 2);
  state.memory_scope.session_id = FixtureUuid(61, 3);
  state.memory_scope.connection_id = FixtureUuid(61, 4);
  state.memory_scope.query_id = FixtureUuid(61, 5);
  state.memory_epochs = {59, 61, 62, 1, 1, 60, 1};
  state.cursor_memory_bytes = 128;
  return state;
}

void BinaryCursorOwnershipAndCleanup() {
  wire::memory::HierarchicalMemoryBudgetLedger ledger;
  wire::memory::ResultCursorPlanMemoryGovernor governor;
  wire::StreamingCursorManager manager;
  auto state = GovernedState(&governor, &ledger);
  auto fabricated = state;
  fabricated.memory_lease_id = FixtureUuid(61, 7);
  Require(!manager.OpenCursor({.state = fabricated, .now_unix_millis = 100}).ok(),
          "cursor imported an unacquired memory lease");
  auto mismatched = state;
  mismatched.memory_scope.transaction_id = FixtureUuid(61, 6);
  Require(!manager.OpenCursor({.state = mismatched, .now_unix_millis = 100}).ok(),
          "foreign transaction memory scope admitted");
  auto missing_query = state;
  missing_query.memory_scope.query_id = {};
  Require(!manager.OpenCursor({.state = missing_query, .now_unix_millis = 100}).ok(),
          "query identity fabricated from cursor identity");
  auto opened = manager.OpenCursor({.state = state, .now_unix_millis = 100});
  Require(opened.ok() && !opened.state.memory_lease_id.is_nil() &&
              ledger.Snapshot().current_bytes == 128, "binary cursor memory acquisition failed");
  auto binding = wire::StreamingCursorBindingFromState(opened.state);
  auto delivered = manager.RecordFrameDelivery({.expected = binding, .row_count = 1,
                                                .byte_count = 64, .now_unix_millis = 101});
  Require(delivered.ok() && ledger.Snapshot().current_bytes == 192,
          "binary frame owner not charged");
  const auto credited = manager.GrantCredit(state.cursor_id, {2, 4, 4096, false});
  Require(credited.ok() && ledger.Snapshot().current_bytes == 128 &&
              credited.state.outstanding_frame_count == 0, "credit refresh did not release frames");
  Require(manager.CancelCursor(state.cursor_id).ok() && ledger.Snapshot().current_bytes == 0,
          "cancel did not release exact cursor lease");
  Require(manager.CancelCursor(state.cursor_id).ok(), "completed cancellation was not idempotent");

  wire::StreamingCursorManager stale_manager;
  opened = stale_manager.OpenCursor({.state = state, .now_unix_millis = 100});
  Require(opened.ok(), "stale owner setup failed");
  Require(ledger.CleanupOwner(state.cursor_id.bytes).ok(), "external cleanup setup failed");
  const auto failed = stale_manager.CancelCursor(state.cursor_id);
  Require(!failed.ok() && failed.state.cancellation_requested &&
              failed.state.memory_lease_id == opened.state.memory_lease_id &&
              failed.state.cursor_memory_bytes == 128,
          "failed cursor release reported success or discarded its owner");

  wire::memory::HierarchicalMemoryBudgetLedger frame_ledger;
  wire::memory::ResultCursorPlanMemoryGovernor frame_governor;
  wire::StreamingCursorManager frame_manager;
  auto frame_state = GovernedState(&frame_governor, &frame_ledger);
  Require(frame_manager.OpenCursor({.state = frame_state, .now_unix_millis = 100}).ok(),
          "frame cleanup failure setup open failed");
  auto frame_delivery = frame_manager.RecordFrameDelivery({
      .expected = wire::StreamingCursorBindingFromState(frame_state),
      .row_count = 1, .byte_count = 64, .now_unix_millis = 101});
  Require(frame_delivery.ok() && frame_ledger.CleanupOwner(frame_state.cursor_id.bytes).ok(),
          "frame cleanup failure setup failed");
  const auto frame_refused = frame_manager.GrantCredit(frame_state.cursor_id, {10, 10, 10000, false});
  Require(!frame_refused.ok() && frame_refused.state.outstanding_frame_bytes == 64 &&
              frame_refused.state.outstanding_frame_count == 1 &&
              frame_refused.state.client_credit.byte_credit == frame_delivery.state.client_credit.byte_credit,
          "failed frame release granted credit or erased remaining ownership");

  wire::StreamingCursorManager overflow_manager;
  auto overflow = CursorState();
  overflow.frame_sequence = std::numeric_limits<scratchbird::core::platform::u64>::max();
  Require(overflow_manager.OpenCursor({.state = overflow, .now_unix_millis = 100}).ok(),
          "maximum sequence setup failed");
  const auto denied = overflow_manager.RecordFrameDelivery({
      .expected = wire::StreamingCursorBindingFromState(overflow),
      .row_count = 1, .byte_count = 1, .now_unix_millis = 101});
  Require(!denied.ok() && denied.state.frame_sequence == overflow.frame_sequence &&
              denied.state.client_credit.byte_credit == overflow.client_credit.byte_credit,
          "frame sequence overflow mutated cursor state");
}

void CursorPublicationAllocationFaults() {
  std::array<std::size_t, 2> sites{};
  for (unsigned frame = 0; frame < 2; ++frame) {
    for (std::size_t fault = 0; fault <= sites[frame]; ++fault) {
      wire::memory::HierarchicalMemoryBudgetLedger ledger;
      wire::memory::ResultCursorPlanMemoryGovernor governor;
      wire::StreamingCursorManager manager;
      auto state = GovernedState(&governor, &ledger);
      const wire::StreamingCursorOpenRequest request{.state = state, .now_unix_millis = 100};
      if (frame) Require(manager.OpenCursor(request).ok(), "frame fault setup failed");
      const wire::StreamingCursorFrameDelivery delivery{
          .expected = wire::StreamingCursorBindingFromState(state),
          .row_count = 1, .byte_count = 64, .now_unix_millis = 101};
      allocation_fault::calls = 0;
      allocation_fault::fail_at = fault;
      allocation_fault::consumed = false;
      allocation_fault::enabled = true;
      bool success = false;
      try {
        success = frame ? manager.RecordFrameDelivery(delivery).ok() : manager.OpenCursor(request).ok();
      } catch (const std::bad_alloc&) { }
      allocation_fault::enabled = false;
      if (fault == 0) {
        sites[frame] = allocation_fault::calls;
        Require(success && sites[frame] != 0, "fault baseline did not execute real allocations");
        Require(manager.CancelCursor(state.cursor_id).ok(), "fault baseline cleanup failed");
      } else {
        Require(allocation_fault::consumed && !success, "allocation fault was not consumed or reported success");
        const auto retained = manager.Lookup(state.cursor_id);
        Require(ledger.Snapshot().current_bytes == (frame ? 128u : 0u) &&
                    governor.Snapshot().active_lease_count == (frame ? 1u : 0u),
                "allocation failure leaked unpublished lease or changed prior ownership");
        if (frame) {
          Require(retained && retained->frame_sequence == state.frame_sequence &&
                      retained->client_credit.byte_credit == state.client_credit.byte_credit &&
                      retained->outstanding_frame_count == 0,
                  "failed frame delivery published credit or frame state");
          Require(manager.CancelCursor(state.cursor_id).ok(), "frame fault cleanup failed");
        } else {
          Require(!retained, "failed cursor open published an owner");
        }
      }
    }
  }
  std::cout << "cursor publication allocation faults=" << sites[0] << '+' << sites[1] << '\n';
}

exec::VectorizedResultBatch Batch(const std::vector<std::string>& values) {
  std::vector<scratchbird::core::platform::u64> offsets;
  std::vector<std::uint8_t> data;
  offsets.push_back(0);
  for (const auto& value : values) {
    data.insert(data.end(), value.begin(), value.end());
    offsets.push_back(static_cast<scratchbird::core::platform::u64>(
        data.size()));
  }
  exec::VectorizedResultBatchBuilder builder(values.size());
  builder.AddColumn(exec::MakeVariableWidthResultBatchColumn(
      "payload",
      values.size(),
      offsets,
      data,
      exec::MakeResultBatchValidityBitmap(values.size())));
  auto finalized = builder.Finalize();
  Require(finalized.ok(), "batch finalization failed");
  return finalized.batch;
}

scratchbird::core::platform::u64 FullFrameBytes(
    const std::vector<std::string>& values) {
  const auto built = wire::BuildDirectBinaryResultFrame(Batch(values));
  Require(built.ok(), "full frame build failed");
  return built.frame.bytes.size();
}

void FrameWindowUsesActualBytesCancellationAndBackpressure() {
  const std::vector<std::string> rows = {
      "a",
      "variable-row-two-is-longer",
      "variable-row-three-is-longer-than-two",
  };
  const auto two_row_bytes = FullFrameBytes({rows[0], rows[1]});
  const auto three_row_bytes = FullFrameBytes(rows);
  Require(three_row_bytes > two_row_bytes,
          "test fixture did not grow frame bytes");

  wire::DirectBinaryResultFrameWindowPolicy policy;
  policy.start_row = 0;
  policy.requested_rows = 3;
  policy.max_rows = 3;
  policy.max_frame_bytes = two_row_bytes;
  policy.client_credit_rows = 3;
  policy.client_credit_bytes = three_row_bytes;
  policy.frame_sequence = 9;
  const auto window = wire::BuildDirectBinaryResultFrameWindow(Batch(rows), policy);
  Require(window.ok(), "window frame build failed");
  Require(window.row_count == 2,
          "window did not stop at actual byte row boundary");
  Require(window.actual_frame_bytes == two_row_bytes,
          "window did not report actual serialized frame bytes");
  Require(window.next_start_row == 2,
          "window next row mismatch");
  Require(window.continuation_required,
          "window did not require continuation");
  Require(window.ordering_preserved,
          "ordered output was not preserved");
  Require(Contains(window.evidence,
                   "direct_binary_frame.actual_byte_accounting=true"),
          "actual byte accounting evidence missing");
  Require(EvidenceU64(window.evidence,
                      "direct_binary_frame.actual_byte_probe_count") <
              policy.requested_rows,
          "actual byte frame window used linear prefix probing");

  auto no_credit = policy;
  no_credit.client_credit_bytes = 0;
  const auto backpressured =
      wire::BuildDirectBinaryResultFrameWindow(Batch(rows), no_credit);
  Require(!backpressured.ok(), "zero byte credit was admitted");
  Require(backpressured.diagnostic.diagnostic_code ==
              "SB_DIRECT_BINARY_RESULT_FRAME.BACKPRESSURE",
          "frame backpressure diagnostic mismatch");

  auto cancelled_policy = policy;
  cancelled_policy.cancellation_requested = true;
  const auto cancelled =
      wire::BuildDirectBinaryResultFrameWindow(Batch(rows), cancelled_policy);
  Require(cancelled.ok(), "cancelled frame window should return handled state");
  Require(cancelled.cancelled, "cancelled frame window not marked cancelled");
  Require(cancelled.diagnostic.diagnostic_code ==
              "SB_DIRECT_BINARY_RESULT_FRAME.CANCELLED",
          "frame cancellation diagnostic mismatch");
}

}  // namespace

int main() {
  CursorManagerTracksStateAndCredit();
  ContinuationTokensValidateBindingsAndTamper();
  BinaryIdentityAndCanonicalTokenProfiles();
  BinaryCursorOwnershipAndCleanup();
  CursorPublicationAllocationFaults();
  FrameWindowUsesActualBytesCancellationAndBackpressure();
  return EXIT_SUCCESS;
}
