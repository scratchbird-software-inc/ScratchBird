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

namespace scratchbird::udr::opensearch_sql_ppl_parser_support {

inline constexpr std::string_view kSbuOpensearchSqlPplPackageUuid =
    "019e13c0-0000-7000-8000-000000000310";
inline constexpr std::string_view kSbuOpensearchSqlPplPackageName = "sbup_opensearch_sql_ppl";

struct UdrResult {
  bool ok{false};
  std::string payload;
  std::string message_vector_json;
};

UdrResult sbu_opensearch_sql_ppl_validate_syntax(std::string_view sql_text,
                                    std::string_view profile);
UdrResult sbu_opensearch_sql_ppl_parse_to_sblr(std::string_view sql_text,
                                  std::string_view context_packet);
UdrResult sbu_opensearch_sql_ppl_normalize(std::string_view sql_text,
                              std::string_view profile);
UdrResult sbu_opensearch_sql_ppl_describe_statement(std::string_view sql_text,
                                       std::string_view context_packet);
UdrResult sbu_opensearch_sql_ppl_install_environment(std::string_view context_packet,
                                        std::string_view install_mode);
UdrResult sbu_opensearch_sql_ppl_verify_environment(std::string_view context_packet);
UdrResult sbu_opensearch_sql_ppl_management_operation_inventory(std::string_view render_policy);
UdrResult sbu_opensearch_sql_ppl_management_package_request(std::string_view operation_name,
                                               std::string_view context_packet);
UdrResult sbu_opensearch_sql_ppl_debug_capabilities(std::string_view render_policy);

scratchbird::udr::runtime::UdrPackageDescriptor sbu_opensearch_sql_ppl_package_descriptor();

} // namespace scratchbird::udr::opensearch_sql_ppl_parser_support
