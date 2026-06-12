// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "candidate_set.hpp"
#include "candidate_set_executor.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid V7(platform::UuidKind kind,
                       platform::u64 unix_epoch_millis,
                       platform::byte suffix) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(unix_epoch_millis);
  Require(generated.ok(), "ODF-090 UUIDv7 generation failed");
  generated.value.bytes[6] = 0x70;
  generated.value.bytes[7] = 0x00;
  generated.value.bytes[8] = 0x80;
  for (std::size_t i = 9; i < generated.value.bytes.size(); ++i) {
    generated.value.bytes[i] = 0x90;
  }
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(kind, generated.value);
  Require(typed.ok(), "ODF-090 typed UUIDv7 creation failed");
  return typed.value;
}

idx::CandidateSetRow Row(platform::byte suffix,
                         double score,
                         bool exact = true,
                         bool visible = true,
                         bool authorized = true,
                         bool payload = true) {
  idx::CandidateSetRow row;
  row.row_uuid = V7(platform::UuidKind::row, 1710000090000ull, suffix);
  row.score = score;
  row.exact_predicate_match = exact;
  row.mga_visible = visible;
  row.security_authorized = authorized;
  row.exact_payload_available = payload;
  row.source = "odf090";
  return row;
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

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true", "parser_executes_sql=true",
          "client_visibility_or_finality_authority=true",
          "write_ahead_log_finality_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-090 evidence leaked forbidden documentation or authority token");
    }
  }
}

idx::CandidateSetAuthorityContext Authority() {
  idx::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

std::vector<idx::CandidateSetRow> Dictionary() {
  return {Row(0x01, 10.0), Row(0x02, 8.0), Row(0x03, 4.0),
          Row(0x04, 9.0, false), Row(0x05, 2.0),
          Row(0x06, 11.0, true, true, false)};
}

void AlgebraTopKRerankAndExecutorRecheck() {
  const auto authority = Authority();
  const auto dictionary = Dictionary();

  auto exact = idx::MakeExactRowUuidOrderedCandidateSet(
      {dictionary[0], dictionary[2], dictionary[4]}, authority);
  Require(exact.ok(), "ODF-090 exact row UUID stream build failed");
  Require(exact.output.encoding ==
              idx::CandidateSetEncoding::exact_row_uuid_ordered_stream,
          "ODF-090 exact stream encoding not reported");
  Require(EvidenceHas(exact.evidence,
                      "encoding=exact_row_uuid_ordered_stream"),
          "ODF-090 exact stream encoding evidence missing");
  Require(!exact.output.final_rows_authorized,
          "ODF-090 exact stream was allowed to return final rows");

  auto bitmap = idx::MakeCompressedBitmapCandidateSet(
      dictionary, {{1, 3}}, authority);
  Require(bitmap.ok(), "ODF-090 compressed bitmap build failed");
  Require(bitmap.output.encoding == idx::CandidateSetEncoding::compressed_bitmap,
          "ODF-090 compressed bitmap encoding not reported");
  Require(bitmap.output.compressed && bitmap.output.approximate,
          "ODF-090 compressed bitmap candidate flags missing");
  Require(EvidenceHas(bitmap.evidence, "compressed_bitmap.range_count=1"),
          "ODF-090 compressed bitmap range evidence missing");

  auto united =
      idx::UnionCandidateSets(exact.output, bitmap.output, authority);
  Require(united.ok(), "ODF-090 candidate-set union failed");
  Require(united.output.rows.size() == 5,
          "ODF-090 union did not preserve five unique candidate rows");
  Require(EvidenceHas(united.evidence, "operation=union"),
          "ODF-090 union operation evidence missing");

  auto intersected =
      idx::IntersectCandidateSets(exact.output, bitmap.output, authority);
  Require(intersected.ok(), "ODF-090 candidate-set intersect failed");
  Require(intersected.output.rows.size() == 1 &&
              SameUuid(intersected.output.rows[0].row_uuid,
                       dictionary[2].row_uuid),
          "ODF-090 intersect did not return the shared row UUID");

  auto subtracted =
      idx::SubtractCandidateSets(united.output, bitmap.output, authority);
  Require(subtracted.ok(), "ODF-090 candidate-set subtract failed");
  Require(subtracted.output.rows.size() == 2,
          "ODF-090 subtract did not remove bitmap candidates");

  auto top_k = idx::TopKCandidateSet(united.output, 3, authority);
  Require(top_k.ok(), "ODF-090 top-K failed");
  Require(top_k.output.rows.size() == 3,
          "ODF-090 top-K did not prune to three rows");
  Require(EvidenceHas(top_k.evidence, "top_k.action=score_prune"),
          "ODF-090 top-K action evidence missing");
  Require(EvidenceHas(top_k.evidence, "top_k.k=3"),
          "ODF-090 top-K count evidence missing");

  auto reranked = idx::RerankCandidateSet(
      top_k.output,
      [](const idx::CandidateSetRow& row) {
        return 1000.0 - static_cast<double>(row.row_uuid.value.bytes[15]);
      },
      authority);
  Require(reranked.ok(), "ODF-090 exact rerank failed");
  Require(EvidenceHas(reranked.evidence, "rerank.action=exact_payload_score"),
          "ODF-090 rerank evidence missing");
  Require(reranked.output.rows[0].score > reranked.output.rows[1].score,
          "ODF-090 rerank scores were not applied");

  auto rechecked = idx::ExactRecheckCandidateSet(united.output, authority);
  Require(rechecked.ok(), "ODF-090 exact recheck failed");
  Require(rechecked.output.final_rows_authorized,
          "ODF-090 exact recheck did not authorize final rows");
  Require(rechecked.output.rows.size() == 4,
          "ODF-090 exact recheck did not filter non-exact rows");
  Require(EvidenceHas(rechecked.evidence,
                      "mga_finality_authority=engine_transaction_inventory"),
          "ODF-090 MGA authority evidence missing");
  Require(EvidenceHas(rechecked.evidence, "final_rows_authorized=true"),
          "ODF-090 final-row authorization evidence missing");

  auto finalized =
      exec::FinalizeCandidateSetForExecutor(united.output, authority);
  Require(finalized.ok(), "ODF-090 executor finalization failed");
  Require(finalized.final_row_uuids.size() == 4,
          "ODF-090 executor finalization returned wrong final row count");
  Require(EvidenceHas(finalized.evidence,
                      "executor.final_result_requires_exact_row_uuid=true"),
          "ODF-090 executor exact row UUID evidence missing");
  Require(EvidenceHas(finalized.evidence,
                      "executor.final_result_requires_mga_recheck=true"),
          "ODF-090 executor MGA recheck evidence missing");
  Require(EvidenceHas(finalized.evidence,
                      "executor.final_result_requires_security_recheck=true"),
          "ODF-090 executor security recheck evidence missing");

  RequireEvidenceHygiene(exact.evidence);
  RequireEvidenceHygiene(bitmap.evidence);
  RequireEvidenceHygiene(united.evidence);
  RequireEvidenceHygiene(top_k.evidence);
  RequireEvidenceHygiene(reranked.evidence);
  RequireEvidenceHygiene(rechecked.evidence);
  RequireEvidenceHygiene(finalized.evidence);
}

void UnsafeAuthorityFailsClosed() {
  const auto rows = Dictionary();
  auto authority = Authority();
  auto exact = idx::MakeExactRowUuidOrderedCandidateSet(
      {rows[0], rows[1]}, authority);
  Require(exact.ok(), "ODF-090 authority test setup failed");

  authority = Authority();
  authority.parser_or_reference_finality_or_visibility_authority = true;
  auto refused = idx::UnionCandidateSets(exact.output, exact.output, authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 parser/reference authority did not fail closed");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_CANDIDATE_SET.UNSAFE_AUTHORITY",
          "ODF-090 parser/reference authority diagnostic changed");

  authority = Authority();
  authority.client_finality_or_visibility_authority = true;
  refused = idx::UnionCandidateSets(exact.output, exact.output, authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 client authority did not fail closed");

  authority = Authority();
  authority.provider_finality_or_visibility_authority = true;
  refused = idx::UnionCandidateSets(exact.output, exact.output, authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 provider authority did not fail closed");

  authority = Authority();
  authority.wal_recovery_or_finality_authority = true;
  refused = idx::UnionCandidateSets(exact.output, exact.output, authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 WAL authority did not fail closed");
}

void MissingRechecksAndUnsafeInputsFailClosed() {
  const auto rows = Dictionary();
  auto authority = Authority();

  auto refused = idx::MakeExactRowUuidOrderedCandidateSet(
      {rows[1], rows[0]}, authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 unsorted exact stream was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_CANDIDATE_SET.UNSORTED_EXACT_STREAM",
          "ODF-090 unsorted stream diagnostic changed");

  refused = idx::MakeCompressedBitmapCandidateSet(rows, {{3, 2}, {4, 1}},
                                                  authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 corrupt compressed bitmap was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_CANDIDATE_SET.COMPRESSED_INPUT_CORRUPT",
          "ODF-090 corrupt bitmap diagnostic changed");

  auto exact = idx::MakeExactRowUuidOrderedCandidateSet(
      {rows[0], rows[1]}, authority);
  Require(exact.ok(), "ODF-090 missing recheck setup failed");

  authority = Authority();
  authority.row_mga_recheck_required = false;
  refused = idx::ExactRecheckCandidateSet(exact.output, authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 missing MGA recheck was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_CANDIDATE_SET.MGA_RECHECK_REQUIRED",
          "ODF-090 missing MGA diagnostic changed");

  authority = Authority();
  authority.security_context_bound = false;
  refused = idx::ExactRecheckCandidateSet(exact.output, authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 missing security recheck was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_CANDIDATE_SET.SECURITY_RECHECK_REQUIRED",
          "ODF-090 missing security diagnostic changed");

  authority = Authority();
  authority.exact_recheck_available = false;
  refused = idx::ExactRecheckCandidateSet(exact.output, authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 missing exact recheck was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_CANDIDATE_SET.EXACT_RECHECK_REQUIRED",
          "ODF-090 missing exact recheck diagnostic changed");

  auto unsafe_contract = exact.output;
  unsafe_contract.requires_exact_recheck = false;
  authority = Authority();
  refused = idx::UnionCandidateSets(unsafe_contract, exact.output, authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 candidate without recheck contract was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_CANDIDATE_SET.RECHECK_CONTRACT_REQUIRED",
          "ODF-090 recheck contract diagnostic changed");

  auto unknown_encoding = exact.output;
  unknown_encoding.encoding = idx::CandidateSetEncoding::unknown;
  authority = Authority();
  refused = idx::UnionCandidateSets(unknown_encoding, exact.output, authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 unknown candidate-set encoding was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_CANDIDATE_SET.UNSUPPORTED_ENCODING",
          "ODF-090 unsupported encoding diagnostic changed");

  auto inconsistent_encoding = exact.output;
  inconsistent_encoding.compressed = true;
  authority = Authority();
  refused =
      idx::UnionCandidateSets(inconsistent_encoding, exact.output, authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 inconsistent candidate-set encoding flags were accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_CANDIDATE_SET.UNSUPPORTED_ENCODING",
          "ODF-090 inconsistent encoding diagnostic changed");

  authority = Authority();
  authority.exact_rerank_source_available = false;
  refused = idx::RerankCandidateSet(
      exact.output, [](const idx::CandidateSetRow& row) { return row.score; },
      authority);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 missing exact rerank source was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_CANDIDATE_SET.EXACT_RERANK_REQUIRED",
          "ODF-090 missing rerank diagnostic changed");

  auto missing_payload = idx::MakeExactRowUuidOrderedCandidateSet(
      {Row(0x10, 1.0, true, true, true, false)}, Authority());
  Require(missing_payload.ok(), "ODF-090 missing payload setup failed");
  refused = idx::RerankCandidateSet(
      missing_payload.output,
      [](const idx::CandidateSetRow& row) { return row.score + 1.0; },
      Authority());
  Require(!refused.ok() && refused.fail_closed,
          "ODF-090 missing exact rerank payload was accepted");
}

}  // namespace

int main() {
  AlgebraTopKRerankAndExecutorRecheck();
  UnsafeAuthorityFailsClosed();
  MissingRechecksAndUnsafeInputsFailClosed();
  return EXIT_SUCCESS;
}
