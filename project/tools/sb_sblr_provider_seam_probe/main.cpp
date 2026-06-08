// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "common/function_result_helpers.hpp"
#include "families/data_scalar_functions.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fn = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;

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
  if (!condition) errors->push_back(std::move(message));
}

fn::FunctionCallRequest Request(std::string function_id) {
  fn::FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.function_uuid = "018f0000-0000-7000-8000-000000000000";
  request.context.package_name = "core";
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.implementation_state = fn::FunctionImplementationState::implement_now;
  request.context.package_state = fn::FunctionPackageState::core;
  request.context.sblr_context.database_uuid = "019b6cf8-b000-7000-8000-000000000001";
  request.context.sblr_context.statement_uuid = "019b6cf8-b000-7000-8000-000000000002";
  request.context.sblr_context.user_uuid = "019b6cf8-b000-7000-8000-000000000003";
  request.context.sblr_context.security_snapshot_uuid = "019b6cf8-b000-7000-8000-000000000004";
  return request;
}

}  // namespace

int main() {
  std::vector<std::string> errors;

  auto now_request = Request("data.scalar.now");
  now_request.context.sblr_context.current_timestamp = "2026-05-03T12:34:56Z";
  const auto now_result = fn::DispatchDataScalarFunction(now_request);
  Expect(now_result.result.ok(), "injected now should succeed", &errors);
  Expect(now_result.result.scalar_values.size() == 1 &&
             now_result.result.scalar_values[0].text_value == "2026-05-03T12:34:56Z" &&
             now_result.result.scalar_values[0].payload_kind == sblr::SblrValuePayloadKind::temporal_text,
         "now should use injected timestamp provider", &errors);

  auto random_request = Request("data.scalar.random");
  random_request.context.sblr_context.deterministic_random_u64_present = true;
  random_request.context.sblr_context.deterministic_random_u64 = 0;
  const auto random_result = fn::DispatchDataScalarFunction(random_request);
  Expect(random_result.result.ok(), "injected random should succeed", &errors);
  Expect(random_result.result.scalar_values.size() == 1 &&
             random_result.result.scalar_values[0].has_real64_value &&
             random_result.result.scalar_values[0].real64_value == 0.0,
         "random should use deterministic u64 provider", &errors);

  auto bytes_request = Request("data.scalar.crypto_random_bytes");
  bytes_request.context.sblr_context.deterministic_random_bytes_hex = "00010203aabbccdd";
  bytes_request.arguments.push_back({"length", fn::MakeInt64Value("int64", 4)});
  const auto bytes_result = fn::DispatchDataScalarFunction(bytes_request);
  Expect(bytes_result.result.ok(), "injected random bytes should succeed", &errors);
  Expect(bytes_result.result.scalar_values.size() == 1 &&
             bytes_result.result.scalar_values[0].payload_kind == sblr::SblrValuePayloadKind::binary &&
             bytes_result.result.scalar_values[0].binary_value.size() == 4 &&
             bytes_result.result.scalar_values[0].binary_value[0] == 0x00 &&
             bytes_result.result.scalar_values[0].binary_value[3] == 0x03,
         "crypto_random_bytes should use deterministic hex provider", &errors);

  auto uuid_request = Request("data.scalar.uuid_generate");
  uuid_request.context.sblr_context.deterministic_uuid_text = "019b6cf8-b000-7000-8000-00000000feed";
  const auto uuid_result = fn::DispatchDataScalarFunction(uuid_request);
  Expect(uuid_result.result.ok(), "injected uuid should succeed", &errors);
  Expect(uuid_result.result.scalar_values.size() == 1 &&
             uuid_result.result.scalar_values[0].text_value == "019b6cf8-b000-7000-8000-00000000feed" &&
             uuid_result.result.scalar_values[0].payload_kind == sblr::SblrValuePayloadKind::uuid_text,
         "uuid_generate should use deterministic UUID provider", &errors);

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"checks\": 4,\n";
  std::cout << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[i]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
