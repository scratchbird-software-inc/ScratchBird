// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "direct_binary_result_frame.hpp"
#include "vectorized_result_batch.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace wire = scratchbird::wire;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
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
          "finality_authority=true", "visibility_authority=true",
          "parser_authority=true", "parser_execution_authority=true",
          "provider_authority=true", "client_authority=true",
          "security_authority=true", "mga_authority=true",
          "wal_authority=true", "row_object_formatting=true",
          "row_object_fallback=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-098 evidence leaked forbidden authority or row-object token");
    }
  }
}

std::vector<std::uint8_t> Bytes(std::string_view text) {
  return {text.begin(), text.end()};
}

std::vector<std::uint8_t> FixedBytes(std::size_t count) {
  std::vector<std::uint8_t> data(count);
  for (std::size_t i = 0; i < count; ++i) {
    data[i] = static_cast<std::uint8_t>(i + 1u);
  }
  return data;
}

void WriteU32(std::vector<std::uint8_t>* bytes,
              std::size_t offset,
              exec::u32 value) {
  for (int shift = 0; shift < 32; shift += 8) {
    (*bytes)[offset++] = static_cast<std::uint8_t>((value >> shift) & 0xffu);
  }
}

void WriteU64(std::vector<std::uint8_t>* bytes,
              std::size_t offset,
              exec::u64 value) {
  for (int shift = 0; shift < 64; shift += 8) {
    (*bytes)[offset++] = static_cast<std::uint8_t>((value >> shift) & 0xffu);
  }
}

bool ContainsBytes(const std::vector<std::uint8_t>& bytes,
                   std::string_view needle) {
  return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) !=
         bytes.end();
}

exec::VectorizedResultBatchResult BuildFrameBatch() {
  exec::VectorizedResultBatchBuilder builder(4);

  auto id = exec::MakeFixedWidthResultBatchColumn(
      "id", 4, 4, FixedBytes(16), exec::MakeResultBatchValidityBitmap(4));
  id.redaction_bitmap = exec::MakeResultBatchRedactionBitmap(4, {3});
  builder.AddColumn(std::move(id));

  auto secret = exec::MakeVariableWidthResultBatchColumn(
      "secret", 4, {0, 6, 12, 18, 24}, Bytes("PUBLICSECRETVISIBLTAIL!!"),
      exec::MakeResultBatchValidityBitmap(4, {2}));
  secret.redaction_bitmap = exec::MakeResultBatchRedactionBitmap(4, {1});
  builder.AddColumn(std::move(secret));

  auto dictionary = exec::MakeDictionaryResultBatchColumn(
      "status", 4, {0, 1, 0, 0}, {"public", "DICTSECRET"},
      exec::MakeResultBatchValidityBitmap(4));
  dictionary.redaction_bitmap = exec::MakeResultBatchRedactionBitmap(4, {1});
  builder.AddColumn(std::move(dictionary));

  auto run = exec::MakeRunEndResultBatchColumn(
      "segment", 4, {1, 4}, {"RUNSECRET", "visible"},
      exec::MakeResultBatchValidityBitmap(4));
  run.redaction_bitmap = exec::MakeResultBatchRedactionBitmap(4, {0});
  builder.AddColumn(std::move(run));

  std::vector<exec::ResultBatchColumn> struct_children;
  struct_children.push_back(exec::MakeFixedWidthResultBatchColumn(
      "metrics.count", 4, 2, FixedBytes(8),
      exec::MakeResultBatchValidityBitmap(4)));
  struct_children.push_back(exec::MakeVariableWidthResultBatchColumn(
      "metrics.label", 4, {0, 1, 3, 6, 10}, Bytes("abcdefqrst"),
      exec::MakeResultBatchValidityBitmap(4)));
  auto metrics = exec::MakeStructViewResultBatchColumn(
      "metrics", 4, std::move(struct_children),
      exec::MakeResultBatchValidityBitmap(4));
  metrics.redaction_bitmap = exec::MakeResultBatchRedactionBitmap(4);
  builder.AddColumn(std::move(metrics));

  auto list_child = exec::MakeVariableWidthResultBatchColumn(
      "tags.value", 5, {0, 1, 2, 3, 4, 5}, Bytes("abcde"),
      exec::MakeResultBatchValidityBitmap(5));
  auto tags = exec::MakeListViewResultBatchColumn(
      "tags", 4, {0, 2, 2, 3, 5}, std::move(list_child),
      exec::MakeResultBatchValidityBitmap(4, {1}));
  tags.redaction_bitmap = exec::MakeResultBatchRedactionBitmap(4, {2});
  builder.AddColumn(std::move(tags));

  return builder.Finalize();
}

void RequireRefusal(const wire::DirectBinaryResultFrameResult& result,
                    std::string_view reason) {
  Require(!result.ok() && result.fail_closed,
          "ODF-098 invalid direct binary frame did not fail closed");
  Require(result.diagnostic.diagnostic_code ==
              "SB_DIRECT_BINARY_RESULT_FRAME.INVALID",
          "ODF-098 refusal diagnostic changed");
  Require(EvidenceHas(result.evidence, "fallback_refusal_reason="),
          "ODF-098 refusal evidence missing");
  Require(EvidenceHas(result.evidence, reason),
          "ODF-098 exact refusal reason missing");
  RequireEvidenceHygiene(result.evidence);
}

void DirectBinaryFrameCreationAndRoundTrip() {
  const auto batch = BuildFrameBatch();
  Require(batch.ok(), "ODF-098 vectorized fixture did not finalize");

  const auto frame = wire::BuildDirectBinaryResultFrame(batch.batch);
  Require(frame.ok(), "ODF-098 direct binary frame build failed");
  Require(frame.frame.descriptor.version == wire::kDirectBinaryResultFrameVersion,
          "ODF-098 direct binary frame version changed");
  Require(frame.frame.descriptor.row_count == 4,
          "ODF-098 frame row count changed");
  Require(frame.frame.descriptor.column_count == 6,
          "ODF-098 frame column count changed");
  Require(frame.frame.descriptor.payload_length > 0,
          "ODF-098 frame payload missing");
  Require(EvidenceHas(frame.evidence,
                      "direct_binary_frame.data_transport_only=true"),
          "ODF-098 transport-only evidence missing");
  Require(EvidenceHas(frame.evidence,
                      "direct_binary_frame.row_order_preserved=true"),
          "ODF-098 row-order evidence missing");
  Require(EvidenceHas(frame.evidence,
                      "direct_binary_frame.row_object_formatting=false"),
          "ODF-098 row-object refusal evidence missing");
  Require(EvidenceHas(frame.evidence,
                      "direct_binary_frame.column.redaction_count=1"),
          "ODF-098 redaction evidence missing");
  RequireEvidenceHygiene(frame.evidence);

  Require(frame.frame.descriptor.columns[1].null_count == 1,
          "ODF-098 null bitmap metadata missing");
  Require(frame.frame.descriptor.columns[1].redaction_count == 1,
          "ODF-098 redaction bitmap metadata missing");
  Require(frame.frame.descriptor.columns[4].children.size() == 2,
          "ODF-098 struct layout descriptors missing");
  Require(frame.frame.descriptor.columns[5].children.size() == 1,
          "ODF-098 list layout descriptor missing");
  Require(!ContainsBytes(frame.frame.bytes, "SECRET"),
          "ODF-098 redacted variable payload bytes leaked");
  Require(!ContainsBytes(frame.frame.bytes, "DICTSECRET"),
          "ODF-098 redacted dictionary payload bytes leaked");
  Require(!ContainsBytes(frame.frame.bytes, "RUNSECRET"),
          "ODF-098 redacted run-end payload bytes leaked");

  const auto second = wire::BuildDirectBinaryResultFrame(batch.batch);
  Require(second.ok(), "ODF-098 second deterministic build failed");
  Require(frame.frame.bytes == second.frame.bytes,
          "ODF-098 direct binary frame bytes are not deterministic");

  const auto parsed = wire::ParseDirectBinaryResultFrame(frame.frame.bytes);
  Require(parsed.ok(), "ODF-098 direct binary frame parse failed");
  Require(parsed.frame.descriptor.row_count == frame.frame.descriptor.row_count,
          "ODF-098 parsed row count changed");
  Require(parsed.frame.descriptor.column_count ==
              frame.frame.descriptor.column_count,
          "ODF-098 parsed column count changed");
  Require(parsed.frame.bytes == frame.frame.bytes,
          "ODF-098 parsed frame did not round trip bytes");
  RequireEvidenceHygiene(parsed.evidence);
}

void BuildFailClosedCases() {
  exec::VectorizedResultBatch unfinalized;
  unfinalized.row_count = 4;
  unfinalized.columns.push_back(exec::MakeFixedWidthResultBatchColumn(
      "id", 4, 4, FixedBytes(16), exec::MakeResultBatchValidityBitmap(4)));
  RequireRefusal(
      wire::BuildDirectBinaryResultFrame(unfinalized),
      "batch_transfer_descriptor_failed:batch_missing_executor_column_diagnostics");

  auto batch = BuildFrameBatch();
  Require(batch.ok(), "ODF-098 mutable fixture did not finalize");
  batch.batch.columns.front().fixed_width_data.pop_back();
  RequireRefusal(
      wire::BuildDirectBinaryResultFrame(batch.batch),
      "batch_transfer_descriptor_failed:batch_revalidation_failed:fixed_width_data_size_mismatch");

  auto redaction_bad = BuildFrameBatch();
  Require(redaction_bad.ok(), "ODF-098 redaction fixture did not finalize");
  redaction_bad.batch.columns.front().redaction_bitmap.push_back(0);
  RequireRefusal(
      wire::BuildDirectBinaryResultFrame(redaction_bad.batch),
      "batch_transfer_descriptor_failed:batch_revalidation_failed:redaction_bitmap_size_mismatch");
}

void ParseFailClosedCases() {
  const auto batch = BuildFrameBatch();
  Require(batch.ok(), "ODF-098 parse fixture did not finalize");
  const auto frame = wire::BuildDirectBinaryResultFrame(batch.batch);
  Require(frame.ok(), "ODF-098 parse fixture frame build failed");

  auto unsupported_version = frame.frame.bytes;
  WriteU32(&unsupported_version, 8, 2);
  RequireRefusal(wire::ParseDirectBinaryResultFrame(unsupported_version),
                 "unsupported_frame_version");

  auto truncated = frame.frame.bytes;
  truncated.resize(10);
  RequireRefusal(wire::ParseDirectBinaryResultFrame(truncated),
                 "malformed_truncated_frame");

  auto payload_size_mismatch = frame.frame.bytes;
  payload_size_mismatch.pop_back();
  RequireRefusal(wire::ParseDirectBinaryResultFrame(payload_size_mismatch),
                 "payload_size_mismatch");

  auto descriptor_mismatch = frame.frame.bytes;
  WriteU64(&descriptor_mismatch, 56, 99);
  RequireRefusal(wire::ParseDirectBinaryResultFrame(descriptor_mismatch),
                 "descriptor_mismatch:header_descriptor_mismatch");

  constexpr std::size_t kDescriptorOffset = 64;
  constexpr std::size_t kDescriptorPrefix = 4 + 8 + 8 + 8;
  constexpr std::size_t kFirstColumn = kDescriptorOffset + kDescriptorPrefix;
  constexpr std::size_t kFirstColumnLayout = kFirstColumn + 2 + 2;
  constexpr std::size_t kFirstColumnRedactionCount =
      kFirstColumnLayout + 4 + 8 + 8;
  constexpr std::size_t kFirstColumnValidityLength =
      kFirstColumnLayout + 4 + 8 + 8 + 8 + 8;
  constexpr std::size_t kFirstColumnRedactionOffset =
      kFirstColumnValidityLength + 8;
  constexpr std::size_t kFirstColumnRedactionLength =
      kFirstColumnValidityLength + 8 + 8;

  auto unsupported_layout = frame.frame.bytes;
  WriteU32(&unsupported_layout, kFirstColumnLayout, 0);
  RequireRefusal(wire::ParseDirectBinaryResultFrame(unsupported_layout),
                 "descriptor_mismatch:malformed_column_layout");

  auto null_bitmap_mismatch = frame.frame.bytes;
  WriteU64(&null_bitmap_mismatch, kFirstColumnValidityLength, 99);
  RequireRefusal(wire::ParseDirectBinaryResultFrame(null_bitmap_mismatch),
                 "null_bitmap_size_mismatch");

  auto redaction_bitmap_mismatch = frame.frame.bytes;
  WriteU64(&redaction_bitmap_mismatch, kFirstColumnRedactionLength, 99);
  RequireRefusal(wire::ParseDirectBinaryResultFrame(redaction_bitmap_mismatch),
                 "redaction_bitmap_size_mismatch");

  auto overlapping_ranges = frame.frame.bytes;
  WriteU64(&overlapping_ranges, kFirstColumnRedactionCount, 4);
  WriteU64(&overlapping_ranges, kFirstColumnRedactionOffset, 0);
  RequireRefusal(wire::ParseDirectBinaryResultFrame(overlapping_ranges),
                 "payload_size_mismatch");
}

void AuthorityAndRowObjectClaimsFailClosed() {
  RequireRefusal(wire::ValidateDirectBinaryResultFrameEvidenceClaims(
                     {"direct_binary_frame.parser_authority=true"}),
                 "forbidden_authority_or_row_object_claim:parser_authority");
  RequireRefusal(wire::ValidateDirectBinaryResultFrameEvidenceClaims(
                     {"direct_binary_frame.client_authority=true"}),
                 "forbidden_authority_or_row_object_claim:client_authority");
  RequireRefusal(wire::ValidateDirectBinaryResultFrameEvidenceClaims(
                     {"direct_binary_frame.provider_authority=true"}),
                 "forbidden_authority_or_row_object_claim:provider_authority");
  RequireRefusal(wire::ValidateDirectBinaryResultFrameEvidenceClaims(
                     {"direct_binary_frame.security_authority=true"}),
                 "forbidden_authority_or_row_object_claim:security_authority");
  RequireRefusal(wire::ValidateDirectBinaryResultFrameEvidenceClaims(
                     {"direct_binary_frame.mga_authority=true"}),
                 "forbidden_authority_or_row_object_claim:mga_authority");
  RequireRefusal(wire::ValidateDirectBinaryResultFrameEvidenceClaims(
                     {"direct_binary_frame.wal_authority=true"}),
                 "forbidden_authority_or_row_object_claim:wal_authority");
  RequireRefusal(wire::ValidateDirectBinaryResultFrameEvidenceClaims(
                     {"direct_binary_frame.row_object_fallback=true"}),
                 "forbidden_authority_or_row_object_claim:row_object_fallback");
}

}  // namespace

int main() {
  DirectBinaryFrameCreationAndRoundTrip();
  BuildFailClosedCases();
  ParseFailClosedCases();
  AuthorityAndRowObjectClaimsFailClosed();
  return EXIT_SUCCESS;
}
