// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_wire_descriptor.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
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

} // namespace

int main() {
  using namespace scratchbird::parser::firebird;

  const std::set<std::string> required_wire_ids{
      "fb_wire_attach_auth",
      "fb_wire_statement",
      "fb_wire_sqlda_message",
      "fb_wire_parameter_buffers",
      "fb_wire_blob_array",
      "fb_wire_events",
      "fb_wire_services",
      "fb_wire_backup_restore",
      "fb_wire_validation_stats",
      "fb_wire_security",
      "fb_wire_trace_profiler",
      "fb_wire_replication_migration",
      "fb_wire_proxy_topology",
  };
  std::set<std::string> actual_wire_ids;
  for (const auto& surface : WireApiSurfaces()) {
    actual_wire_ids.insert(surface.id);
    if (!Expect(!surface.owner.empty(), "wire surface missing owner")) return EXIT_FAILURE;
    if (!Expect(!surface.diagnostic_contract.empty(),
                "wire surface missing diagnostic contract")) {
      return EXIT_FAILURE;
    }
    if (!Expect(!surface.ctest_label.empty(), "wire surface missing CTest label")) {
      return EXIT_FAILURE;
    }
    if (!Expect(!surface.runtime_policy.empty(), "wire surface missing runtime policy")) {
      return EXIT_FAILURE;
    }
    if (!Expect(!Contains(surface.owner, "sbsql"), "wire owner crossed dialect boundary")) {
      return EXIT_FAILURE;
    }
  }
  if (!Expect(actual_wire_ids == required_wire_ids, "wire API surface id set mismatch")) {
    return EXIT_FAILURE;
  }

  const std::set<std::string> required_parameter_ids{
      "fb_blr_core",
      "fb_message_blr",
      "fb_sqlda",
      "fb_dpb",
      "fb_tpb",
      "fb_spb",
      "fb_bpb",
  };
  std::set<std::string> actual_parameter_ids;
  for (const auto& surface : ParameterBufferSurfaces()) {
    actual_parameter_ids.insert(surface.id);
    if (!Expect(surface.owner == "sbl_firebird_wire",
                "parameter buffer surface owner mismatch")) {
      return EXIT_FAILURE;
    }
    if (!Expect(surface.ctest_label == "firebird_blr_parameter_buffer_conformance",
                "parameter buffer CTest label mismatch")) {
      return EXIT_FAILURE;
    }
    if (!Expect(!surface.diagnostic_contract.empty(),
                "parameter buffer missing diagnostic contract")) {
      return EXIT_FAILURE;
    }
  }
  if (!Expect(actual_parameter_ids == required_parameter_ids,
              "parameter buffer surface id set mismatch")) {
    return EXIT_FAILURE;
  }

  const auto identity = FirebirdWireApiScopeJson();
  if (!Expect(Contains(identity, "\"wire_api_surface_count\":13"),
              "wire API identity count mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(identity, "\"parameter_buffer_surface_count\":7"),
              "parameter buffer identity count mismatch")) {
    return EXIT_FAILURE;
  }
  if (!Expect(Contains(identity, "\"cross_dialect_dependencies\":false"),
              "wire API identity dependency policy mismatch")) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
