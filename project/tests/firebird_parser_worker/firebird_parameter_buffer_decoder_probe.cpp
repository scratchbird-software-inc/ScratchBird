// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_wire_descriptor.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
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

bool ExpectOk(const scratchbird::parser::firebird::ParameterBufferDecodeResult& result,
              std::string_view name,
              std::string_view json_fragment) {
  if (!Expect(result.ok, std::string(name) + " decode failed: " + result.json)) {
    return false;
  }
  if (!Expect(Contains(result.json, json_fragment),
              std::string(name) + " JSON missing fragment")) {
    std::cerr << result.json << '\n';
    return false;
  }
  return true;
}

bool ExpectFail(const scratchbird::parser::firebird::ParameterBufferDecodeResult& result,
                std::string_view diagnostic) {
  if (!Expect(!result.ok, "bad parameter buffer was accepted")) return false;
  if (!Expect(result.diagnostic_code == diagnostic, "diagnostic code mismatch")) {
    std::cerr << result.json << '\n';
    return false;
  }
  if (!Expect(Contains(result.json, "\"runtime_policy\":\"fail_closed\""),
              "bad parameter buffer did not fail closed")) {
    return false;
  }
  return true;
}

} // namespace

int main() {
  using scratchbird::parser::firebird::DecodeFirebirdParameterBuffer;
  using Bytes = std::vector<std::uint8_t>;

  const auto dpb = DecodeFirebirdParameterBuffer(
      "DPB",
      Bytes{1, 28, 6, 'S', 'Y', 'S', 'D', 'B', 'A',
            48, 4, 'U', 'T', 'F', '8',
            60, 7, 'R', 'D', 'B', '$', 'A', 'D', 'M'});
  if (!ExpectOk(dpb, "DPB", "isc_dpb_user_name")) return EXIT_FAILURE;
  if (!Expect(Contains(dpb.json, "isc_dpb_lc_ctype") &&
              Contains(dpb.json, "isc_dpb_sql_role_name") &&
              Contains(dpb.json, "\"runtime_policy\":\"descriptor_only_no_engine_authority\""),
              "DPB descriptor content mismatch")) {
    std::cerr << dpb.json << '\n';
    return EXIT_FAILURE;
  }

  const auto tpb = DecodeFirebirdParameterBuffer(
      "TPB", Bytes{3, 15, 17, 6, 8});
  if (!ExpectOk(tpb, "TPB", "isc_tpb_read_committed")) return EXIT_FAILURE;
  if (!Expect(Contains(tpb.json, "isc_tpb_rec_version") &&
              Contains(tpb.json, "isc_tpb_wait") &&
              Contains(tpb.json, "isc_tpb_read"),
              "TPB descriptor content mismatch")) {
    return EXIT_FAILURE;
  }

  const auto bpb = DecodeFirebirdParameterBuffer(
      "BPB", Bytes{1, 1, 1, 0, 2, 1, 1, 3, 1, 0});
  if (!ExpectOk(bpb, "BPB", "isc_bpb_source_type")) return EXIT_FAILURE;
  if (!Expect(Contains(bpb.json, "isc_bpb_target_type") &&
              Contains(bpb.json, "isc_bpb_type"),
              "BPB descriptor content mismatch")) {
    return EXIT_FAILURE;
  }

  const auto spb = DecodeFirebirdParameterBuffer(
      "SPB", Bytes{2, 11, 106, 8, 'e', 'm', 'p', 'l', 'o', 'y', 'e', 'e',
                   108, 1, 1});
  if (!ExpectOk(spb, "SPB", "isc_action_svc_db_stats")) return EXIT_FAILURE;
  if (!Expect(spb.service_action == "isc_action_svc_db_stats",
              "SPB service action mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(spb.json, "isc_spb_dbname") &&
              Contains(spb.json, "\"runtime_policy\":\"emulated_service_or_authority_diagnostic\""),
              "SPB descriptor content mismatch")) {
    std::cerr << spb.json << '\n';
    return EXIT_FAILURE;
  }

  const auto spb_attach = DecodeFirebirdParameterBuffer(
      "SPB", Bytes{2, 28, 6, 'S', 'Y', 'S', 'D', 'B', 'A',
                   29, 9, 'm', 'a', 's', 't', 'e', 'r', 'k', 'e', 'y'});
  if (!ExpectOk(spb_attach, "SPB attach", "isc_spb_user_name")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(spb_attach.json, "secret_redacted"),
              "SPB password policy missing")) {
    return EXIT_FAILURE;
  }

  const auto blr = DecodeFirebirdParameterBuffer(
      "BLR", Bytes{5, 2, 255, 76});
  if (!ExpectOk(blr, "BLR", "blr_begin")) return EXIT_FAILURE;
  if (!Expect(Contains(blr.json, "\"saw_message\":false") &&
              Contains(blr.json, "\"runtime_policy\":\"descriptor_only_no_engine_authority\""),
              "BLR descriptor content mismatch")) {
    std::cerr << blr.json << '\n';
    return EXIT_FAILURE;
  }

  const auto message_blr = DecodeFirebirdParameterBuffer(
      "MESSAGE_BLR", Bytes{5, 2, 4, 0, 2, 0, 8, 0, 7, 0, 255, 76});
  if (!ExpectOk(message_blr, "MESSAGE_BLR", "blr_message")) return EXIT_FAILURE;
  if (!Expect(Contains(message_blr.json, "\"message_slot_count\":2") &&
              Contains(message_blr.json, "\"name\":\"blr_long\"") &&
              Contains(message_blr.json, "\"role\":\"value\"") &&
              Contains(message_blr.json, "\"role\":\"null_indicator\""),
              "MESSAGE_BLR descriptor content mismatch")) {
    std::cerr << message_blr.json << '\n';
    return EXIT_FAILURE;
  }

  const auto sqlda_v1 = DecodeFirebirdParameterBuffer(
      "SQLDA",
      Bytes{1, 0, 'X', 'S', 'Q', 'L', 'D', 'A', ' ', ' ',
            34, 0, 0, 0, 2, 0, 2, 0,
            0xf0, 0x01, 0, 0, 0, 0, 4, 0,
            0xc5, 0x01, 0, 0, 0, 0, 10, 0});
  if (!ExpectOk(sqlda_v1, "SQLDA v1", "SQL_LONG")) return EXIT_FAILURE;
  if (!Expect(Contains(sqlda_v1.json, "\"sqld\":2") &&
              Contains(sqlda_v1.json, "\"base_type\":\"SQL_TEXT\"") &&
              Contains(sqlda_v1.json, "\"nullable\":true"),
              "SQLDA v1 descriptor content mismatch")) {
    std::cerr << sqlda_v1.json << '\n';
    return EXIT_FAILURE;
  }

  const auto sqlda_v2 = DecodeFirebirdParameterBuffer(
      "SQLDA",
      Bytes{2, 0, 'X', 'S', 'Q', 'L', 'D', 'A', '2', ' ',
            26, 0, 0, 0, 1, 0, 1, 0,
            0xfc, 0x7f, 0, 0, 0, 0, 1, 0});
  if (!ExpectOk(sqlda_v2, "SQLDA v2", "SQL_BOOLEAN")) return EXIT_FAILURE;
  if (!Expect(Contains(sqlda_v2.json, "\"version\":2") &&
              Contains(sqlda_v2.json, "\"nullable\":false"),
              "SQLDA v2 descriptor content mismatch")) {
    std::cerr << sqlda_v2.json << '\n';
    return EXIT_FAILURE;
  }

  if (!ExpectFail(DecodeFirebirdParameterBuffer("DPB", Bytes{}),
                  "FIREBIRD.WIRE.VERSION_INVALID")) {
    return EXIT_FAILURE;
  }
  if (!ExpectFail(DecodeFirebirdParameterBuffer("DPB", Bytes{7}),
                  "FIREBIRD.WIRE.VERSION_INVALID")) {
    return EXIT_FAILURE;
  }
  if (!ExpectFail(DecodeFirebirdParameterBuffer("DPB", Bytes{1, 28, 4, 'S'}),
                  "FIREBIRD.WIRE.CLUMPLET_LENGTH_INVALID")) {
    return EXIT_FAILURE;
  }
  if (!ExpectFail(DecodeFirebirdParameterBuffer("TPB", Bytes{3, 255}),
                  "FIREBIRD.WIRE.UNKNOWN_TAG")) {
    return EXIT_FAILURE;
  }
  if (!ExpectFail(DecodeFirebirdParameterBuffer(
                      "MESSAGE_BLR", Bytes{5, 2, 4, 0, 2, 0, 8, 76}),
                  "FIREBIRD.WIRE.BLR_TRUNCATED")) {
    return EXIT_FAILURE;
  }
  if (!ExpectFail(DecodeFirebirdParameterBuffer("SQLDA", Bytes{3, 0}),
                  "FIREBIRD.WIRE.SQLDA_TRUNCATED")) {
    return EXIT_FAILURE;
  }
  if (!ExpectFail(DecodeFirebirdParameterBuffer("WIRE", Bytes{1}),
                  "FIREBIRD.WIRE.BUFFER_KIND_INVALID")) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
