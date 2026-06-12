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
#include <cstdint>
#include <vector>

namespace scratchbird::parser::firebird {

struct WireApiSurface {
  std::string id;
  std::string family;
  std::string owner;
  std::string diagnostic_contract;
  std::string ctest_label;
  std::string runtime_policy;
};

struct ParameterBufferSurface {
  std::string id;
  std::string family;
  std::string input_kind;
  std::string owner;
  std::string diagnostic_contract;
  std::string ctest_label;
};

struct ParameterBufferItem {
  std::uint8_t tag{0};
  std::string name;
  std::vector<std::uint8_t> payload;
  std::string policy;
};

struct ParameterBufferDecodeResult {
  bool ok{false};
  std::string kind;
  std::uint8_t version{0};
  std::vector<ParameterBufferItem> items;
  std::string diagnostic_code;
  std::string diagnostic_message;
  std::string service_action;
  std::string runtime_policy;
  std::string json;
};

const std::vector<WireApiSurface>& WireApiSurfaces();
const std::vector<ParameterBufferSurface>& ParameterBufferSurfaces();
ParameterBufferDecodeResult DecodeFirebirdParameterBuffer(
    std::string_view kind,
    const std::vector<std::uint8_t>& buffer);
std::string FirebirdWireApiScopeJson();

} // namespace scratchbird::parser::firebird
