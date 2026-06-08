// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbu_firebird_parser_support.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool Expect(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

bool ExpectInstallMode(std::string_view catalog_uuid,
                       std::string_view mode,
                       bool drift_expected,
                       bool mutation_expected,
                       std::string_view expected_after_state) {
  using scratchbird::udr::firebird_parser_support::sbu_firebird_install_environment;
  const std::string context =
      "engine_context=trusted;catalog_uuid=" + std::string(catalog_uuid);
  const auto result = sbu_firebird_install_environment(
      context, mode);
  if (!Expect(result.ok, "install mode failed")) return false;
  if (!Expect(Contains(result.payload, "\"mode\":\""), "payload missing mode")) {
    return false;
  }
  if (!Expect(Contains(result.payload, std::string("\"mode\":\"") + std::string(mode) + "\""),
              "payload mode mismatch")) {
    return false;
  }
  if (!Expect(Contains(result.payload, "\"atomic\":true"), "payload missing atomic")) {
    return false;
  }
  if (!Expect(Contains(result.payload, "\"silent_repair\":false"),
              "payload allowed silent repair")) {
    return false;
  }
  const std::string mutation =
      std::string("\"catalog_mutation_applied\":") +
      (mutation_expected ? "true" : "false");
  if (!Expect(Contains(result.payload, mutation), "catalog mutation flag mismatch")) {
    return false;
  }
  const std::string after_state =
      std::string("\"catalog_state_after\":\"") + std::string(expected_after_state) + "\"";
  if (!Expect(Contains(result.payload, after_state), "catalog state mismatch")) {
    return false;
  }
  if (!Expect(Contains(result.payload, "\"overlay_hash\":\"firebird_catalog_overlay_"),
              "overlay hash missing")) {
    return false;
  }
  if (!Expect(Contains(result.payload, "\"overlay_row_count\":7"),
              "overlay row count missing")) {
    return false;
  }
  if (!Expect(Contains(result.payload, "\"INFORMATION_SCHEMA\""),
              "information schema overlay missing")) {
    return false;
  }
  if (!Expect(Contains(result.payload, "\"mutated_objects\""),
              "mutated object list missing")) {
    return false;
  }
  const std::string drift =
      std::string("\"version_drift_detected\":") + (drift_expected ? "true" : "false");
  if (!Expect(Contains(result.payload, drift), "drift flag mismatch")) return false;
  if (!Expect(Contains(result.payload, "\"real_firebird_file_effects\":false"),
              "payload permits Firebird file effects")) {
    return false;
  }
  return true;
}

} // namespace

int main() {
  using namespace scratchbird::udr::firebird_parser_support;

  if (!ExpectInstallMode("drift-test", "install", false, true, "installed")) {
    return EXIT_FAILURE;
  }
  if (!ExpectInstallMode("drift-test", "verify", false, false, "installed")) {
    return EXIT_FAILURE;
  }
  if (!ExpectInstallMode("drift-test", "force_reinstall", false, true,
                         "installed")) {
    return EXIT_FAILURE;
  }
  if (!ExpectInstallMode("drift-test", "upgrade", false, true, "installed")) {
    return EXIT_FAILURE;
  }
  if (!ExpectInstallMode("drift-test", "repair_version_drift", true, true,
                         "installed")) {
    return EXIT_FAILURE;
  }

  const auto verified =
      sbu_firebird_verify_environment("engine_context=trusted;catalog_uuid=drift-test");
  if (!Expect(verified.ok, "verify environment failed")) return EXIT_FAILURE;
  if (!Expect(Contains(verified.payload, "\"catalog_overlay_installed\":true"),
              "verify installed flag mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(verified.payload, "\"catalog_state\":\"installed\""),
              "verify catalog state mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(verified.payload, "\"overlay_revision\":4"),
              "verify overlay revision mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(verified.payload, "\"version_drift_detected\":false"),
              "verify drift flag mismatch")) {
    return EXIT_FAILURE;
  }

  const auto drift_marked = sbu_firebird_install_environment(
      "engine_context=trusted;catalog_uuid=drift-marked;catalog_overlay_state=drifted",
      "verify");
  if (!Expect(drift_marked.ok, "drift-marked verify failed")) return EXIT_FAILURE;
  if (!Expect(Contains(drift_marked.payload,
                       "\"catalog_state_after\":\"version_drift_detected\""),
              "drift-marked state mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(drift_marked.payload, "\"version_drift_detected\":true"),
              "drift-marked drift flag mismatch")) {
    return EXIT_FAILURE;
  }

  const auto drift_repaired = sbu_firebird_install_environment(
      "engine_context=trusted;catalog_uuid=drift-marked",
      "repair_version_drift");
  if (!Expect(drift_repaired.ok, "drift repair failed")) return EXIT_FAILURE;
  if (!Expect(Contains(drift_repaired.payload,
                       "\"catalog_state_after\":\"installed\""),
              "drift repair state mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(drift_repaired.payload,
                       "\"version_drift_repaired\":true"),
              "drift repair flag mismatch")) {
    return EXIT_FAILURE;
  }

  const auto silent =
      sbu_firebird_install_environment("engine_context=trusted;catalog_uuid=drift-test",
                                       "silent_repair");
  if (!Expect(!silent.ok, "silent repair should be rejected")) return EXIT_FAILURE;
  if (!Expect(Contains(silent.message_vector_json, "UDR.FIREBIRD.INSTALL_MODE_INVALID"),
              "silent repair diagnostic mismatch")) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
