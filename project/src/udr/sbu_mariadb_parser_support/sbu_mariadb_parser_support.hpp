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

namespace scratchbird::udr::mariadb_parser_support {

inline constexpr std::string_view kSbuMariadbPackageUuid =
    "019e13c0-0000-7000-8000-000000000306";
inline constexpr std::string_view kSbuMariadbPackageName = "sbup_mariadb";

struct UdrResult {
  bool ok{false};
  std::string payload;
  std::string message_vector_json;
};

UdrResult sbu_mariadb_validate_syntax(std::string_view sql_text,
                                    std::string_view profile);
UdrResult sbu_mariadb_parse_to_sblr(std::string_view sql_text,
                                  std::string_view context_packet);
UdrResult sbu_mariadb_normalize(std::string_view sql_text,
                              std::string_view profile);
UdrResult sbu_mariadb_describe_statement(std::string_view sql_text,
                                       std::string_view context_packet);
UdrResult sbu_mariadb_install_environment(std::string_view context_packet,
                                        std::string_view install_mode);
UdrResult sbu_mariadb_verify_environment(std::string_view context_packet);
UdrResult sbu_mariadb_management_operation_inventory(std::string_view render_policy);
UdrResult sbu_mariadb_management_package_request(std::string_view operation_name,
                                               std::string_view context_packet);
UdrResult sbu_mariadb_debug_capabilities(std::string_view render_policy);

scratchbird::udr::runtime::UdrPackageDescriptor sbu_mariadb_package_descriptor();

} // namespace scratchbird::udr::mariadb_parser_support
