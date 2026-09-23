// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/write_result_policy.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <cstdio>
#include <cstdlib>

namespace api = scratchbird::engine::internal_api;
static void Check(bool condition, const char* message) {
  if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(EXIT_FAILURE); }
}

int main() {
  const auto original = scratchbird::tests::FixtureUuid(48, 1);
  for (const char* policy_name : {"ids_only", "changed_fields"}) {
    const auto policy = api::ResolveWriteResultPolicyOptions(
        {std::string("write_result_policy:") + policy_name}, "dml.insert_rows");
    Check(policy.ok, "valid result policy rejected");
    for (unsigned bit = 0; bit <= 128; ++bit) {
      auto id = original;
      if (bit < 128) id.bytes[bit / 8] ^= static_cast<unsigned char>(1u << (bit % 8));
      api::EngineApiResult result;
      api::EngineRowValue row;
      row.requested_row_uuid = id;
      api::EngineTypedValue text;
      text.descriptor.canonical_type_name = "text";
      text.encoded_value = "019d0000-0000-7000-8000-000000000001";
      row.fields.push_back({"customer_uuid", text});
      result.result_shape.rows.push_back(row);
      api::ApplyWriteResultPolicy(policy, &result);
      Check(result.result_shape.rows.size() == 1, "result row lost");
      const auto& actual = result.result_shape.rows.front();
      Check(actual.requested_row_uuid == id, "requested UUID changed");
      bool found = false, kept_text = false;
      for (const auto& [name, value] : actual.fields) {
        if (name == "row_uuid") {
          found = true;
          Check(value.descriptor.canonical_type_name == "uuid" &&
                    value.binary_value == std::vector<std::uint8_t>(id.bytes.begin(), id.bytes.end()) &&
                    value.encoded_value.empty() && !value.is_null,
                "result identity was formatted as text or lost UUID bits");
        } else if (name == "customer_uuid" || name == "changed.customer_uuid") {
          kept_text = true;
          Check(value.descriptor.canonical_type_name == "text" &&
                    value.encoded_value == text.encoded_value && value.binary_value.empty(),
                "user TEXT was reinterpreted as identity");
        }
      }
      Check(found && kept_text, "identity or existing typed field missing");
    }
  }
  std::puts("write-result binary identity: PASS; raw16 preserved under both policies");
}
