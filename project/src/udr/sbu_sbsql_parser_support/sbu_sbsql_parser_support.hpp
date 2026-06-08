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

namespace scratchbird::udr::sbsql_parser_support {

inline constexpr std::string_view kSbuSbsqlPackageUuid =
    "019e13c0-0000-7000-8000-000000000301";
inline constexpr std::string_view kSbuSbsqlPackageName = "sbup_sbsql";

struct UdrResult {
  bool ok{false};
  std::string payload;
  std::string message_vector_json;
};

UdrResult sbu_sbsql_validate_syntax(std::string_view sql_text, std::string_view profile);
UdrResult sbu_sbsql_parse_to_sblr(std::string_view sql_text, std::string_view context_packet);
UdrResult sbu_sbsql_parse_expression(std::string_view sql_text, std::string_view descriptor_context);
UdrResult sbu_sbsql_normalize(std::string_view sql_text, std::string_view profile);
UdrResult sbu_sbsql_describe_statement(std::string_view sql_text, std::string_view context_packet);
UdrResult sbu_sbsql_decompile_sblr(std::string_view sblr_packet, std::string_view render_policy);
UdrResult sbu_sbsql_debug_capabilities(std::string_view render_policy);
UdrResult sbu_sbsql_bridge_capabilities(std::string_view render_policy);
UdrResult sbu_sbsql_bridge_dispatch(std::string_view request_packet,
                                    std::string_view context_packet);

scratchbird::udr::runtime::UdrPackageDescriptor sbu_sbsql_package_descriptor();

} // namespace scratchbird::udr::sbsql_parser_support
