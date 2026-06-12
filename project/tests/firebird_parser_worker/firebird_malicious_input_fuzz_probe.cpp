// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"
#include "firebird_wire_descriptor.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool ExpectInvalid(std::string_view sql, std::string_view diagnostic) {
  const auto result = scratchbird::parser::firebird::ParseStatement(sql);
  if (!Expect(!result.ok, "malicious input was accepted")) return false;
  if (!Expect(Contains(result.message_vector_json, diagnostic),
              "malicious input diagnostic mismatch")) {
    return false;
  }
  if (!Expect(result.sblr_envelope.empty(), "invalid parse leaked SBLR payload")) return false;
  return true;
}

bool ExpectDecodeOk(std::string_view kind,
                    const std::vector<std::uint8_t>& bytes,
                    std::string_view required_json) {
  const auto result =
      scratchbird::parser::firebird::DecodeFirebirdParameterBuffer(kind, bytes);
  if (!Expect(result.ok, "valid wire descriptor was rejected")) return false;
  if (!Expect(Contains(result.json, required_json),
              "valid wire descriptor JSON mismatch")) {
    return false;
  }
  if (!Expect(result.runtime_policy.find("no_engine_authority") != std::string::npos ||
                  result.runtime_policy.find("emulated_service") != std::string::npos,
              "valid wire descriptor used an unsafe runtime policy")) {
    return false;
  }
  return true;
}

bool ExpectDecodeFail(std::string_view kind,
                      const std::vector<std::uint8_t>& bytes,
                      std::string_view diagnostic) {
  const auto result =
      scratchbird::parser::firebird::DecodeFirebirdParameterBuffer(kind, bytes);
  if (!Expect(!result.ok, "malformed wire descriptor was accepted")) return false;
  if (!Expect(result.diagnostic_code == diagnostic,
              "malformed wire descriptor diagnostic mismatch")) {
    std::cerr << "expected=" << diagnostic
              << " actual=" << result.diagnostic_code << '\n';
    return false;
  }
  if (!Expect(result.runtime_policy == "fail_closed",
              "malformed wire descriptor did not fail closed")) {
    return false;
  }
  if (!Expect(Contains(result.json, diagnostic),
              "malformed wire descriptor JSON omitted diagnostic")) {
    return false;
  }
  return true;
}

bool ExpectWireDescriptorFuzzCases() {
  if (!ExpectDecodeOk("DPB", {1, 28, 4, 'u', 's', 'e', 'r'},
                      "isc_dpb_user_name")) {
    return false;
  }
  if (!ExpectDecodeFail("DPB", {}, "FIREBIRD.WIRE.VERSION_INVALID")) {
    return false;
  }
  if (!ExpectDecodeFail("DPB", {1, 28},
                        "FIREBIRD.WIRE.CLUMPLET_LENGTH_MISSING")) {
    return false;
  }
  if (!ExpectDecodeFail("DPB", {1, 28, 5, 'u'},
                        "FIREBIRD.WIRE.CLUMPLET_LENGTH_INVALID")) {
    return false;
  }
  if (!ExpectDecodeFail("DPB", {1, 250, 0}, "FIREBIRD.WIRE.UNKNOWN_TAG")) {
    return false;
  }

  if (!ExpectDecodeOk("TPB", {3, 15, 17}, "isc_tpb_read_committed")) {
    return false;
  }
  if (!ExpectDecodeFail("TPB", {3, 255}, "FIREBIRD.WIRE.UNKNOWN_TAG")) {
    return false;
  }

  if (!ExpectDecodeOk("SPB", {2, 1}, "isc_action_svc_backup")) return false;
  if (!ExpectDecodeFail("SPB", {2, 105, 4, 'g'},
                        "FIREBIRD.WIRE.CLUMPLET_LENGTH_INVALID")) {
    return false;
  }
  if (!ExpectDecodeFail("SPB", {2, 255}, "FIREBIRD.WIRE.UNKNOWN_TAG")) {
    return false;
  }

  if (!ExpectDecodeOk("BPB", {1, 1, 1, 0}, "isc_bpb_source_type")) {
    return false;
  }
  if (!ExpectDecodeFail("BPB", {2}, "FIREBIRD.WIRE.VERSION_INVALID")) {
    return false;
  }
  if (!ExpectDecodeFail("BPB", {1, 99, 0}, "FIREBIRD.WIRE.UNKNOWN_TAG")) {
    return false;
  }

  if (!ExpectDecodeOk("BLR", {5, 4, 0, 2, 0, 8, 0, 7, 0, 76},
                      "\"message_slot_count\":2")) {
    return false;
  }
  if (!ExpectDecodeOk("MESSAGE_BLR", {5, 4, 0, 2, 0, 8, 0, 7, 0, 76},
                      "\"saw_message\":true")) {
    return false;
  }
  if (!ExpectDecodeFail("BLR", {5, 4}, "FIREBIRD.WIRE.BLR_TRUNCATED")) {
    return false;
  }
  if (!ExpectDecodeFail("BLR", {5, 4, 0, 0, 0},
                        "FIREBIRD.WIRE.BLR_EOC_MISSING")) {
    return false;
  }
  if (!ExpectDecodeFail("BLR", {5, 4, 0, 1, 1, 76},
                        "FIREBIRD.WIRE.BLR_MESSAGE_COUNT_INVALID")) {
    return false;
  }
  if (!ExpectDecodeFail("BLR", {5, 4, 0, 1, 0, 99, 76},
                        "FIREBIRD.WIRE.BLR_OPCODE_UNKNOWN")) {
    return false;
  }
  if (!ExpectDecodeFail("MESSAGE_BLR", {5, 2, 255, 76},
                        "FIREBIRD.WIRE.BLR_MESSAGE_EXPECTED")) {
    return false;
  }

  if (!ExpectDecodeOk("SQLDA",
                      {1, 0, 'S', 'Q', 'L', 'D', 'A', ' ', ' ', ' ', 26, 0, 0,
                       0, 1, 0, 1, 0, 240, 1, 0, 0, 0, 0, 4, 0},
                      "\"base_type\":\"SQL_LONG\"")) {
    return false;
  }
  if (!ExpectDecodeFail("SQLDA", {1, 0}, "FIREBIRD.WIRE.SQLDA_TRUNCATED")) {
    return false;
  }
  if (!ExpectDecodeFail("SQLDA",
                        {3, 0, 'S', 'Q', 'L', 'D', 'A', ' ', ' ', ' ', 18, 0, 0,
                         0, 0, 0, 0, 0},
                        "FIREBIRD.WIRE.SQLDA_VERSION_INVALID")) {
    return false;
  }
  if (!ExpectDecodeFail("SQLDA",
                        {1, 0, 'S', 'Q', 'L', 'D', 'A', ' ', ' ', ' ', 18, 0, 0,
                         0, 0, 0, 1, 0},
                        "FIREBIRD.WIRE.SQLDA_COUNT_INVALID")) {
    return false;
  }
  if (!ExpectDecodeFail("SQLDA",
                        {1, 0, 'S', 'Q', 'L', 'D', 'A', ' ', ' ', ' ', 26, 0, 0,
                         0, 1, 0, 1, 0, 240, 1},
                        "FIREBIRD.WIRE.SQLDA_TRUNCATED")) {
    return false;
  }

  return true;
}

} // namespace

int main() {
  using namespace scratchbird::parser::firebird;

  if (!ExpectInvalid("", "FIREBIRD.PARSE.EMPTY_INPUT")) return EXIT_FAILURE;
  if (!ExpectInvalid("not recognized", "FIREBIRD.PARSE.INVALID_INPUT")) {
    return EXIT_FAILURE;
  }
  if (!ExpectInvalid("insert into t(id) values (", "FIREBIRD.PARSE.INVALID_INPUT")) {
    return EXIT_FAILURE;
  }
  if (!ExpectInvalid("merge into t using s on (t.id = s.id",
                     "FIREBIRD.PARSE.INVALID_INPUT")) {
    return EXIT_FAILURE;
  }
  if (!ExpectInvalid("execute block as begin if (:id = 1 then suspend; end",
                     "FIREBIRD.PARSE.INVALID_INPUT")) {
    return EXIT_FAILURE;
  }
  if (!ExpectInvalid("create procedure p as begin x = (1; end",
                     "FIREBIRD.PARSE.INVALID_INPUT")) {
    return EXIT_FAILURE;
  }

  const std::string deeply_nested(8192, '(');
  if (!ExpectInvalid(deeply_nested, "FIREBIRD.PARSE.INVALID_INPUT")) {
    return EXIT_FAILURE;
  }

  const std::string oversized_comment = "/*" + std::string(65536, 'x') + "*/";
  const auto tokens = LexTokens(oversized_comment);
  if (!Expect(tokens.size() == 1 && tokens[0].kind == "block_comment",
              "oversized comment tokenization mismatch")) {
    return EXIT_FAILURE;
  }

  const auto hostile_tool =
      ParseStatement("GBAK -backup ../../etc/passwd /tmp/escape.fbk");
  if (!Expect(!hostile_tool.ok, "hostile reference-tool command was accepted")) {
    return EXIT_FAILURE;
  }
  if (!Expect(hostile_tool.statement_family == "low_level_utility",
              "hostile reference-tool command did not enter low-level utility denial lane")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(hostile_tool.message_vector_json,
                       "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED"),
              "hostile reference-tool unsupported-denied diagnostic missing")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(hostile_tool.message_vector_json,
                       "\"real_firebird_file_effects\":\"false\"") &&
                  Contains(hostile_tool.message_vector_json,
                           "\"reference_engine_sql_executed\":\"false\""),
              "hostile reference-tool denial did not prove no file or reference execution effects")) {
    return EXIT_FAILURE;
  }
  if (!Expect(hostile_tool.sblr_envelope.empty(),
              "hostile reference-tool denial produced SBLR payload")) {
    return EXIT_FAILURE;
  }

  if (!ExpectWireDescriptorFuzzCases()) return EXIT_FAILURE;

  return EXIT_SUCCESS;
}
