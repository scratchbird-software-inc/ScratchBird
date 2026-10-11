// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "batch_point_lookup.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace idx = scratchbird::core::index;
namespace p = scratchbird::core::platform;

void Check(bool ok, const char* detail) {
  if (!ok) {
    std::cerr << detail << '\n';
    std::exit(EXIT_FAILURE);
  }
}

idx::BatchPointLookupProviderRow Row(std::string key, p::Uuid uuid) {
  idx::BatchPointLookupProviderRow row;
  row.encoded_key = std::move(key);
  row.candidate.row_uuid = {p::UuidKind::row, uuid};
  // Component inputs to the exact-recheck primitive, not a fabricated storage
  // or engine statement receipt. Live providers still own these decisions.
  row.candidate.exact_predicate_match = true;
  row.candidate.mga_visible = true;
  row.candidate.security_authorized = true;
  row.candidate.exact_payload_available = true;
  row.payload = std::string("binary\0payload", 14);
  return row;
}

idx::BatchPointLookupResult Run(
    const std::vector<std::string>& keys,
    std::vector<idx::BatchPointLookupProviderRow> rows,
    idx::BatchPointLookupPurpose purpose) {
  idx::BatchPointLookupPlan plan;
  plan.purpose = purpose;
  for (std::size_t n = 0; n < keys.size(); ++n)
    plan.keys.push_back({keys[n], n});
  return idx::RunBatchPointLookup(plan, {},
      [rows = std::move(rows)](const idx::BatchPointLookupProviderRequest&) {
        idx::BatchPointLookupProviderResult out;
        out.status = {p::StatusCode::ok, p::Severity::info, p::Subsystem::engine};
        out.rows = rows;
        return out;
      });
}

void Refused(const idx::BatchPointLookupResult& result, const char* code) {
  Check(!result.ok() && result.fail_closed, "invalid provider batch accepted");
  Check(result.diagnostic.diagnostic_code == code, "refusal precedence changed");
  Check(result.rows.empty() && result.misses.empty() &&
            result.candidate_stream.output.rows.empty() &&
            result.exact_recheck.output.rows.empty() &&
            !result.final_rows_authorized,
        "refused provider batch published a prefix");
}

void Exercise(idx::BatchPointLookupPurpose purpose, const std::string& key) {
  p::Uuid base;
  base.bytes[0] = 1;
  base.bytes[6] = 0x70;
  base.bytes[8] = 0x80;
  base.bytes[15] = 1;
  std::vector<p::Uuid> ids{base};
  // Every non-version/variant bit is significant, including embedded zero,
  // high-bit octets and the timestamp octets which GUID order rearranges.
  for (std::size_t byte = 0; byte < 16; ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      if ((byte == 6 && bit >= 4) || (byte == 8 && bit >= 6)) continue;
      auto id = base;
      id.bytes[byte] ^= static_cast<p::byte>(1u << bit);
      ids.push_back(id);
    }
  }
  std::sort(ids.begin(), ids.end(), [](const auto& a, const auto& b) {
    return a.bytes < b.bytes;
  });
  const std::string other = key + std::string("\0", 1);
  std::vector<idx::BatchPointLookupProviderRow> rows;
  for (auto it = ids.rbegin(); it != ids.rend(); ++it) rows.push_back(Row(key, *it));
  // The same row may be found through a distinct exact key. Duplicate request
  // keys preserve occurrences; neither case is a duplicate provider pair.
  rows.push_back(Row(other, base));
  const auto result = Run({key, other, key, "absent"}, rows, purpose);
  Check(result.ok() && result.final_rows_authorized, "valid binary identities refused");
  Check(result.rows.size() == ids.size() * 2 + 1 && result.misses.size() == 1,
        "row or miss cardinality changed");
  Check(result.unique_key_count == 3 && result.duplicate_key_occurrences == 1,
        "binary key occurrence accounting changed");
  for (std::size_t n = 0; n < ids.size(); ++n) {
    for (const auto offset : {std::size_t{0}, ids.size() + 1}) {
      const auto& row = result.rows[offset + n];
      Check(row.row_uuid.kind == p::UuidKind::row && row.row_uuid.value == ids[n] &&
                row.encoded_key == key && row.payload == rows.front().payload,
            "exact binary identity, key, payload or unsigned ordering changed");
      Check(row.input_ordinal == (offset == 0 ? 0 : 2) && row.duplicate_key &&
                row.duplicate_ordinal == (offset == 0 ? 0 : 1),
            "duplicate request occurrence changed");
    }
  }
  Check(result.rows[ids.size()].encoded_key == other &&
            result.rows[ids.size()].row_uuid.value == base &&
            result.rows[ids.size()].input_ordinal == 1 &&
            result.misses.front().input_ordinal == 3 &&
            result.misses.front().reason == "key_not_found",
        "separate key identity or missing-key position changed");

  auto duplicates = rows;
  duplicates.push_back(rows.front());
  Refused(Run({key, other}, duplicates, purpose),
          "SB_BATCH_POINT_LOOKUP.PROVIDER_DUPLICATE_ROW_UUID");
  auto untrusted = Row(key, base);
  untrusted.exact_key_match = false;
  // Preserve the first encountered failure, not a post-sort duplicate check.
  duplicates.push_back(untrusted);
  Refused(Run({key, other}, duplicates, purpose),
          "SB_BATCH_POINT_LOOKUP.PROVIDER_DUPLICATE_ROW_UUID");
  duplicates.insert(duplicates.begin(), untrusted);
  Refused(Run({key, other}, duplicates, purpose),
          "SB_BATCH_POINT_LOOKUP.PROVIDER_ROW_UNTRUSTED");

  auto invalid = Row(key, {});
  Refused(Run({key}, {invalid, invalid}, purpose),
          "SB_BATCH_POINT_LOOKUP.EXACT_ROW_UUID_REQUIRED");
  invalid = Row(key, base);
  invalid.candidate.row_uuid.kind = p::UuidKind::object;
  Refused(Run({key}, {invalid, invalid}, purpose),
          "SB_BATCH_POINT_LOOKUP.EXACT_ROW_UUID_REQUIRED");
  invalid.encoded_key = "not-requested";
  invalid.exact_key_match = false;
  Refused(Run({key}, {invalid, invalid}, purpose),
          "SB_BATCH_POINT_LOOKUP.PROVIDER_ROW_OUT_OF_PLAN");

  rows.front().candidate.mga_visible = false;
  rows[1].candidate.security_authorized = false;
  const auto filtered = Run({key, other}, rows, purpose);
  Check(filtered.ok() && filtered.rows.size() == ids.size() - 1,
        "visibility/security exact recheck was bypassed");
  for (const auto& row : filtered.rows)
    Check(row.row_uuid.value != rows.front().candidate.row_uuid.value &&
              row.row_uuid.value != rows[1].candidate.row_uuid.value,
          "filtered identity was published");

  std::vector<std::string> keys;
  rows.clear();
  // Force map rotations while the duplicate index borrows its keys. Cover
  // both small-string and separately allocated binary-key ownership.
  for (unsigned n = 0; n < 64; ++n) {
    keys.push_back(key + static_cast<char>(n));
    rows.push_back(Row(keys.back(), base));
  }
  std::reverse(rows.begin(), rows.end());
  const auto distinct = Run(keys, rows, purpose);
  Check(distinct.ok() && distinct.rows.size() == keys.size(),
        "stable map-key borrowing lost a distinct lookup key");
  for (std::size_t n = 0; n < keys.size(); ++n)
    Check(distinct.rows[n].encoded_key == keys[n] &&
              distinct.rows[n].input_ordinal == n &&
              distinct.rows[n].row_uuid.value == base,
          "map-key lifetime or repeated identity across keys changed");
}

void OccurrenceRechecks(idx::BatchPointLookupPurpose purpose) {
  p::Uuid id;
  id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80; id.bytes[15] = 3;
  // Exhaustive independent truth-table oracle: only 111 admits an occurrence.
  // In particular 101 + 110 must not synthesize a visible/authorized witness.
  for (unsigned a = 0; a < 8; ++a) for (unsigned b = 0; b < 8; ++b) {
    for (bool reverse : {false, true}) {
      auto first = Row("first", id), second = Row("second", id);
      const auto set = [](auto& row, unsigned bits, double score,
                          const char* payload) {
        row.candidate.exact_predicate_match = (bits & 1) != 0;
        row.candidate.mga_visible = (bits & 2) != 0;
        row.candidate.security_authorized = (bits & 4) != 0;
        row.candidate.score = score;
        row.payload = payload;
      };
      set(first, a, 13, "first-payload");
      set(second, b, 9000, "second-payload");
      std::vector<idx::BatchPointLookupProviderRow> rows{first, second};
      if (reverse) std::reverse(rows.begin(), rows.end());
      const auto result = Run({"first", "second", "first"}, rows, purpose);
      const bool admitted_a = a == 7, admitted_b = b == 7;
      const std::size_t expected_rows = (admitted_a ? 2 : 0) + (admitted_b ? 1 : 0);
      Check(result.ok() && result.rows.size() == expected_rows &&
                result.misses.size() == 3 - expected_rows,
            "UUID deduplication borrowed another occurrence's recheck");
      Check(!result.candidate_stream.output.final_rows_authorized &&
                result.exact_recheck.output.final_rows_authorized &&
                result.exact_recheck.output.rows.size() ==
                    (admitted_a || admitted_b ? 1u : 0u),
            "candidate-only stream synthesized final recheck evidence");
      if (admitted_a || admitted_b)
        Check(result.exact_recheck.output.rows.front().score ==
                  (admitted_b ? 9000 : 13),
              "denied occurrence contaminated authorized score");
      std::size_t output = 0, missed = 0;
      for (std::size_t ordinal = 0; ordinal < 3; ++ordinal) {
        const bool is_a = ordinal != 1;
        if (is_a ? admitted_a : admitted_b) {
          const auto& row = result.rows[output++];
          Check(row.input_ordinal == ordinal && row.row_uuid.value == id &&
                    row.encoded_key == (is_a ? "first" : "second") &&
                    row.payload == (is_a ? "first-payload" : "second-payload") &&
                    row.score == (is_a ? 13 : 9000) &&
                    row.duplicate_ordinal == (ordinal == 2 ? 1 : 0),
                "wrong admitted occurrence, payload, score or ordinal");
        } else {
          const auto& miss = result.misses[missed++];
          Check(miss.input_ordinal == ordinal &&
                    miss.reason == "filtered_by_exact_mga_security_recheck",
                "denied occurrence lost its exact miss disposition");
        }
      }
    }
  }
}
} // namespace

int main() {
  for (const auto purpose : {
      idx::BatchPointLookupPurpose::key_value,
      idx::BatchPointLookupPurpose::document_payload,
      idx::BatchPointLookupPurpose::vector_rerank_payload,
      idx::BatchPointLookupPurpose::graph_frontier,
      idx::BatchPointLookupPurpose::search_payload,
      idx::BatchPointLookupPurpose::foreign_key_check,
      idx::BatchPointLookupPurpose::time_series_bucket}) {
    const std::string binary_key("\0|17|AP\xff", 8);
    Exercise(purpose, binary_key);
    Exercise(purpose, binary_key + std::string(256, '\xff'));
    OccurrenceRechecks(purpose);
  }
  return EXIT_SUCCESS;
}
