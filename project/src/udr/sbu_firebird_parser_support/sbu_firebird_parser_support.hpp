// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>

#include "sb_udr_runtime.hpp"

namespace scratchbird::udr::firebird_parser_support {

inline constexpr std::string_view kSbuFirebirdPackageUuid =
    "019e13c0-0000-7000-8000-000000000302";
inline constexpr std::string_view kSbuFirebirdPackageName = "sbup_firebird";

struct UdrResult {
  bool ok{false};
  std::string payload;
  std::string message_vector_json;
};

UdrResult sbu_firebird_validate_syntax(std::string_view sql_text,
                                       std::string_view profile);
UdrResult sbu_firebird_parse_to_sblr(std::string_view sql_text,
                                     std::string_view context_packet);
UdrResult sbu_firebird_parse_concatenated_dynamic_sql(std::string_view left_fragment,
                                                      std::string_view right_fragment,
                                                      std::string_view context_packet);
UdrResult sbu_firebird_normalize(std::string_view sql_text,
                                 std::string_view profile);
UdrResult sbu_firebird_describe_statement(std::string_view sql_text,
                                          std::string_view context_packet);
UdrResult sbu_firebird_install_environment(std::string_view context_packet,
                                           std::string_view install_mode);
UdrResult sbu_firebird_verify_environment(std::string_view context_packet);
UdrResult sbu_firebird_management_operation_inventory(std::string_view render_policy);
UdrResult sbu_firebird_management_package_request(std::string_view operation_name,
                                                  std::string_view context_packet);
UdrResult sbu_firebird_debug_capabilities(std::string_view render_policy);
UdrResult sbu_firebird_render_status_vector(std::string_view message_vector_json,
                                            std::string_view context_packet);
UdrResult sbu_firebird_bridge_capabilities(std::string_view render_policy);
UdrResult sbu_firebird_bridge_dispatch(std::string_view request_packet,
                                       std::string_view context_packet);

scratchbird::udr::runtime::UdrPackageDescriptor sbu_firebird_package_descriptor();

} // namespace scratchbird::udr::firebird_parser_support
