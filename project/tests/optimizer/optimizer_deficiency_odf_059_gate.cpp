// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_layout.hpp"
#include "row_data_page.hpp"
#include "row_ordinal_locator.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "ODF-059 UUID generation failed");
  return generated.value;
}

bool SameUuid(const platform::TypedUuid& left,
              const platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireNoRuntimeDocTokens(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-059 runtime evidence leaked documentation token");
    }
  }
}

page::RowDataRecord Row(platform::u64 local_tx, platform::u64 salt) {
  page::RowDataRecord row;
  row.row_uuid = NewUuid(platform::UuidKind::row, 59000 + salt);
  row.transaction_uuid = NewUuid(platform::UuidKind::transaction, 59100 + salt);
  row.local_transaction_id = local_tx;
  row.row_version = 1;
  return row;
}

page::RowDataPageBody Body() {
  page::RowDataPageBody body;
  body.relation_uuid = NewUuid(platform::UuidKind::object, 59001);
  body.segment_id = 5;
  body.segment_generation = 6;
  body.page_number = 42;
  body.page_generation = 7;
  body.next_page_number = 0;
  body.rows = {Row(101, 1), Row(102, 2), Row(103, 3)};
  return body;
}

page::RowDataPageBody RoundTripBody() {
  const auto built = page::BuildRowDataPageBody(Body(), 4096);
  Require(built.ok(), "ODF-059 row data page build failed");
  const auto parsed = page::ParseRowDataPageBody(built.serialized, built.body.page_number);
  Require(parsed.ok(), "ODF-059 row data page parse failed");
  return parsed.body;
}

void PageLayoutAdvertisesDenseOrdinals() {
  const auto layout = page::LookupPageLayout(scratchbird::storage::disk::PageType::row_data);
  Require(layout.ok(), "ODF-059 row data layout lookup failed");
  Require(layout.descriptor.supports_dense_internal_row_ordinals,
          "ODF-059 row data layout did not advertise dense internal ordinals");
}

void StorageAssignsAndPersistsDenseOrdinalsWithoutReplacingUuidIdentity() {
  const auto body = RoundTripBody();
  Require(body.rows.size() == 3, "ODF-059 row count drifted");
  for (std::size_t i = 0; i < body.rows.size(); ++i) {
    Require(body.rows[i].internal_row_ordinal == i + 1,
            "ODF-059 row ordinal was not dense after round trip");
    Require(body.rows[i].row_uuid.kind == platform::UuidKind::row,
            "ODF-059 external row UUID kind was replaced");
    Require(body.rows[i].transaction_uuid.kind == platform::UuidKind::transaction,
            "ODF-059 transaction UUID evidence was lost");
  }
  Require(body.relation_uuid.kind == platform::UuidKind::object,
          "ODF-059 relation scope UUID was not persisted");
  Require(body.segment_id == 5 && body.segment_generation == 6,
          "ODF-059 segment scope was not persisted");
  Require(body.page_generation == 7,
          "ODF-059 page generation scope was not persisted");
}

void AcceptedLocatorPreservesUuidAndMgaAuthorityEvidence() {
  const auto body = RoundTripBody();
  const auto scope = page::MakeDenseRowOrdinalScope(body);
  const auto locator =
      page::MakeDenseRowOrdinalLocator(scope, body.rows[1], true, true);
  const auto validation = page::ValidateDenseRowOrdinalLocator(body, locator);
  Require(validation.accepted, "ODF-059 valid ordinal locator was refused");
  Require(!validation.fail_closed_to_uuid_mga_lookup,
          "ODF-059 valid locator still failed closed");
  Require(!validation.ordinal_is_visibility_or_finality_authority,
          "ODF-059 ordinal became visibility/finality authority");
  Require(validation.durable_mga_inventory_remains_authority,
          "ODF-059 durable MGA inventory authority flag missing");
  Require(SameUuid(validation.row.row_uuid, body.rows[1].row_uuid),
          "ODF-059 accepted locator did not preserve row UUID identity");
  Require(EvidenceHas(validation.evidence, "uuid_identity_preserved=true"),
          "ODF-059 accepted locator lacked UUID identity evidence");
  Require(EvidenceHas(validation.evidence, "finality_authority=false"),
          "ODF-059 accepted locator lacked non-authority evidence");
  RequireNoRuntimeDocTokens(validation.evidence);
}

void RefusalsFailClosedToUuidMgaLookup() {
  const auto body = RoundTripBody();
  const auto scope = page::MakeDenseRowOrdinalScope(body);
  const auto locator =
      page::MakeDenseRowOrdinalLocator(scope, body.rows[1], true, true);

  auto stale = locator;
  ++stale.scope.page_generation;
  auto validation = page::ValidateDenseRowOrdinalLocator(body, stale);
  Require(!validation.accepted &&
              validation.fail_closed_to_uuid_mga_lookup &&
              validation.refusal_reason == "page_generation_mismatch",
          "ODF-059 stale page generation did not fail closed");

  auto stale_segment = locator;
  ++stale_segment.scope.segment_generation;
  validation = page::ValidateDenseRowOrdinalLocator(body, stale_segment);
  Require(!validation.accepted &&
              validation.fail_closed_to_uuid_mga_lookup &&
              validation.refusal_reason == "segment_generation_mismatch",
          "ODF-059 stale segment generation did not fail closed");

  auto wrong_relation = locator;
  wrong_relation.scope.relation_uuid = NewUuid(platform::UuidKind::object, 59990);
  validation = page::ValidateDenseRowOrdinalLocator(body, wrong_relation);
  Require(!validation.accepted &&
              validation.refusal_reason == "relation_mismatch",
          "ODF-059 relation mismatch did not fail closed");

  auto wrong_uuid = locator;
  wrong_uuid.row_uuid = body.rows[2].row_uuid;
  validation = page::ValidateDenseRowOrdinalLocator(body, wrong_uuid);
  Require(!validation.accepted &&
              validation.refusal_reason == "row_uuid_mismatch",
          "ODF-059 row UUID mismatch did not fail closed");

  auto missing_authority = locator;
  missing_authority.durable_mga_inventory_authority_available = false;
  validation = page::ValidateDenseRowOrdinalLocator(body, missing_authority);
  Require(!validation.accepted &&
              validation.refusal_reason == "mga_authority_missing",
          "ODF-059 missing MGA authority did not fail closed");
  RequireNoRuntimeDocTokens(validation.evidence);
}

void ExecutorHelperUsesStorageValidationAndFailsClosed() {
  const auto body = RoundTripBody();
  const auto locator = page::MakeDenseRowOrdinalLocator(
      page::MakeDenseRowOrdinalScope(body), body.rows[0], true, true);

  exec::ExecutorRowOrdinalLookupRequest disabled;
  disabled.page_body = &body;
  disabled.locator = locator;
  disabled.allow_internal_ordinal_acceleration = false;
  auto executor_result = exec::ValidateExecutorRowOrdinalLocator(disabled);
  Require(!executor_result.evidence.accepted &&
              executor_result.evidence.fail_closed_to_uuid_mga_lookup,
          "ODF-059 disabled executor ordinal path did not fail closed");

  exec::ExecutorRowOrdinalLookupRequest enabled;
  enabled.page_body = &body;
  enabled.locator = locator;
  enabled.allow_internal_ordinal_acceleration = true;
  executor_result = exec::ValidateExecutorRowOrdinalLocator(enabled);
  Require(executor_result.evidence.accepted,
          "ODF-059 executor helper refused valid locator");
  Require(!executor_result.evidence.ordinal_visibility_or_finality_authority,
          "ODF-059 executor helper made ordinal authoritative");
  Require(executor_result.evidence.durable_mga_inventory_remains_authority,
          "ODF-059 executor helper lost MGA authority evidence");
  Require(SameUuid(executor_result.row.row_uuid, body.rows[0].row_uuid),
          "ODF-059 executor helper did not preserve row UUID identity");
  RequireNoRuntimeDocTokens(executor_result.evidence.evidence);
}

}  // namespace

int main() {
  PageLayoutAdvertisesDenseOrdinals();
  StorageAssignsAndPersistsDenseOrdinalsWithoutReplacingUuidIdentity();
  AcceptedLocatorPreservesUuidAndMgaAuthorityEvidence();
  RefusalsFailClosedToUuidMgaLookup();
  ExecutorHelperUsesStorageValidationAndFailsClosed();
  return 0;
}
