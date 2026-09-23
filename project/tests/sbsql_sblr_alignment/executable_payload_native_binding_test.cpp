// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/dml_executable_trigger_runtime.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
namespace api = scratchbird::engine::internal_api;
void Check(bool ok) { if (!ok) std::abort(); }
int main() {
  auto id = scratchbird::tests::FixtureUuid(1242, 1);
  // Delimiters, embedded NUL and high-bit octets must all survive framing.
  id.bytes[9] = ';'; id.bytes[10] = 0; id.bytes[11] = ':'; id.bytes[12] = 0xff;
  const auto option = api::BinaryViewUuidOption("trigger_target_table_uuid:", id);
  const std::string body("body;\0:tail\xff", 12);
  const auto encoded = api::EncodeBinaryViewOptions(
      {option, "trigger_timing:after", std::string("procedure_body_bytes:") + body});
  Check(!encoded.empty());
  const auto native = api::dml_trigger_runtime::PayloadFieldValue(encoded, "trigger_target_table_uuid:");
  Check(native.size() == 16 && api::BinaryViewUuid(native) == id);
  Check(api::BinaryViewOptionValue(encoded, "procedure_body_bytes:") == body);
  Check(api::dml_trigger_runtime::PayloadFieldValue(encoded, "trigger_timing:") == "after");
  Check(api::BinaryViewOptionValue(api::EncodeBinaryViewOptions({option, option}),
                                 "trigger_target_table_uuid:").empty());
  Check(api::BinaryViewOptionValue(encoded + "trailer", "trigger_timing:").empty());
  for (std::size_t size = 0; size < encoded.size(); ++size)
    Check(api::BinaryViewOptionValue(encoded.substr(0, size), "trigger_timing:").empty());
  Check(api::dml_trigger_runtime::PayloadFieldValue("trigger_timing:after;", "trigger_timing:").empty());
  Check(api::BinaryViewUuid("019d0000-0000-7000-8000-00000000d711").is_nil());
  Check(api::BinaryViewUuid(native.substr(1)).is_nil());
  auto invalid = native; invalid[6] = 0x40;
  Check(api::BinaryViewUuid(invalid).is_nil());
  std::cout << "PASS executable native UUID and binary body payload boundaries\n";
}
