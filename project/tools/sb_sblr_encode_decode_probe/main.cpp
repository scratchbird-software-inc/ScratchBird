// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbsql_v3_sblr_catalog.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace sblr = scratchbird::parser::sbsql_v3_sblr;

namespace {

std::string JsonEscape(std::string_view text) {
  std::string out;
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += ch; break;
    }
  }
  return out;
}

void Expect(bool condition, std::string message, std::vector<std::string>* errors) {
  if (!condition) {
    errors->push_back(std::move(message));
  }
}

}  // namespace

int main() {
  std::vector<std::string> errors;
  const sblr::SblrOpcodeEntry select_entry{
      .sblr_operation = "SBLR_SBSQL_V3_SELECT",
      .opcode_value = 0x53030001,
      .payload_class = "query_tree_payload",
      .api_operation_id = "dml.select_rows",
      .cluster_authority_required = false,
      .fail_closed_without_cluster_authority = false,
      .raw_sql_payload_allowed = false,
  };
  const sblr::SblrEnvelope envelope{
      .sblr_operation = "SBLR_SBSQL_V3_SELECT",
      .opcode_value = 0x53030001,
      .sblr_version = 1,
      .binding_epoch = "catalog-epoch-1",
      .bound_root_uuid = "018f0000-0000-7000-8000-000000000123",
      .descriptor_digest = "descriptor-digest-1",
      .payload_class = "query_tree_payload",
      .contains_raw_sql_text = false,
      .cluster_authority_present = false,
  };
  const auto encoded = sblr::EncodeEnvelopeForProbe(envelope);
  std::vector<std::string> decode_errors;
  const auto decoded = sblr::DecodeEnvelopeForProbe(encoded, &decode_errors);
  Expect(decoded.has_value(), "encoded envelope should decode", &errors);
  for (const auto& error : decode_errors) {
    errors.push_back("decode: " + error);
  }
  if (decoded) {
    Expect(sblr::ValidateEnvelope(select_entry, *decoded, &errors), "decoded envelope should validate", &errors);
  }

  auto raw_sql = envelope;
  raw_sql.contains_raw_sql_text = true;
  std::vector<std::string> raw_errors;
  Expect(!sblr::ValidateEnvelope(select_entry, raw_sql, &raw_errors), "raw SQL flagged envelope should fail", &errors);
  bool saw_raw_error = false;
  for (const auto& error : raw_errors) {
    if (error.find("raw SQL") != std::string::npos) {
      saw_raw_error = true;
    }
  }
  Expect(saw_raw_error, "raw SQL failure diagnostic should be present", &errors);

  const sblr::SblrOpcodeEntry cluster_entry{
      .sblr_operation = "SBLR_SBSQL_V3_CLUSTER_INSPECT_STATE",
      .opcode_value = 0x5303F001,
      .payload_class = "cluster_placeholder_payload",
      .api_operation_id = "cluster.inspect_state",
      .cluster_authority_required = true,
      .fail_closed_without_cluster_authority = true,
      .raw_sql_payload_allowed = false,
  };
  auto missing_cluster_token = envelope;
  missing_cluster_token.sblr_operation = cluster_entry.sblr_operation;
  missing_cluster_token.opcode_value = cluster_entry.opcode_value;
  missing_cluster_token.payload_class = cluster_entry.payload_class;
  missing_cluster_token.cluster_authority_present = false;
  std::vector<std::string> cluster_errors;
  Expect(!sblr::ValidateEnvelope(cluster_entry, missing_cluster_token, &cluster_errors), "cluster envelope without authority should fail", &errors);

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"encoded\": \"" << JsonEscape(encoded) << "\",\n";
  std::cout << "  \"raw_sql_rejected\": " << (saw_raw_error ? "true" : "false") << ",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[i]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
