// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace scratchbird::parser::sbsql {

struct GeneratedSurfaceRegistryRow {
  std::string_view surface_id;
  std::string_view fixed_uuid_v7;
  std::string_view canonical_name;
  std::string_view surface_kind;
  std::string_view family;
  std::string_view source_status;
  std::string_view cluster_scope;
  std::string_view canonical_spec;
  std::string_view sblr_operation_family;
  std::string_view parser_packet;
  std::string_view engine_packet;
  std::string_view owner_lane;
  std::string_view batch_id;
  std::string_view ctest_label;
  std::string_view parser_handler_key;
  std::string_view udr_handler_key;
  std::string_view lowering_handler_key;
  std::string_view server_admission_key;
  std::string_view engine_rule_key;
  std::string_view diagnostic_key;
  std::string_view oracle_key;
  std::string_view validation_fixture_id;
  std::string_view final_acceptance_rule;
  std::string_view closure_action;
};

inline constexpr std::size_t kGeneratedSurfaceRegistryRowCount = 2617;

std::span<const GeneratedSurfaceRegistryRow> GeneratedSurfaceRegistryRows();
const GeneratedSurfaceRegistryRow* FindGeneratedSurfaceRegistryRowById(
    std::string_view surface_id);
const GeneratedSurfaceRegistryRow* FindGeneratedSurfaceRegistryRowByCanonicalName(
    std::string_view canonical_name);

} // namespace scratchbird::parser::sbsql
