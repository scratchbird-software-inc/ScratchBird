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

namespace scratchbird::parser::firebird {

struct PackageAdmissionResult {
  bool ok{false};
  std::string package_state;
  std::string operation_class;
  std::string behavior;
  std::string diagnostic_code;
  std::string message_vector_json;
  std::string json;
};

PackageAdmissionResult EvaluateFirebirdPackageAdmission(
    std::string_view package_state,
    std::string_view operation_class);

} // namespace scratchbird::parser::firebird
