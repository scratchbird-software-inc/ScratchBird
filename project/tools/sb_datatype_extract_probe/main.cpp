// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_operations.hpp"

#include <iostream>

using namespace scratchbird::core::datatypes;

int main() {
  const auto year = ExtractDatatypeField({{CanonicalTypeId::timestamp, "2026-05-01T12:34:56", false}, "year"});
  const auto minute = ExtractDatatypeField({{CanonicalTypeId::timestamp, "2026-05-01T12:34:56", false}, "minute"});
  const auto uuid_version = ExtractDatatypeField({{CanonicalTypeId::uuid, "018f7f8f-7c00-7000-8000-000000000001", false}, "version"});
  const auto bad = ExtractDatatypeField({{CanonicalTypeId::int32, "1", false}, "year"});
  const auto opaque = ExtractDatatypeField({{CanonicalTypeId::opaque_extension, "opaque-render-token", false}, "length"});
  const bool ok = year.ok() && year.value.encoded_value == "2026" && minute.ok() && minute.value.encoded_value == "34" &&
                  uuid_version.ok() && uuid_version.value.encoded_value == "7" && !bad.ok() && !opaque.ok();
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"year\": \"" << year.value.encoded_value << "\",\n";
  std::cout << "  \"minute\": \"" << minute.value.encoded_value << "\",\n";
  std::cout << "  \"uuid_version\": \"" << uuid_version.value.encoded_value << "\",\n";
  std::cout << "  \"bad_rejected\": " << (!bad.ok() ? "true" : "false") << ",\n";
  std::cout << "  \"opaque_extract_rejected\": " << (!opaque.ok() ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
