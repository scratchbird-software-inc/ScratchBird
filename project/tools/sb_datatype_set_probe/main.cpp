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
  DatatypeSetDescriptor descriptor;
  descriptor.element_type_id = CanonicalTypeId::character;
  const auto encoded = EncodeSetValue(descriptor,
                                      {{CanonicalTypeId::character, "alpha", false},
                                       {CanonicalTypeId::character, "beta", false},
                                       {CanonicalTypeId::character, "alpha", false}});
  DatatypeSetOperationRequest membership;
  membership.operation = DatatypeSetOperationKind::membership;
  membership.descriptor = descriptor;
  membership.left_encoded_set = encoded.encoded_set;
  membership.right_encoded_set_or_value = "beta";
  const auto has_beta = ApplySetOperation(membership);

  DatatypeSetOperationRequest cardinality = membership;
  cardinality.operation = DatatypeSetOperationKind::cardinality;
  const auto count = ApplySetOperation(cardinality);

  DatatypeSetOperationRequest bad = membership;
  bad.left_encoded_set = "not-a-set";
  const auto bad_result = ApplySetOperation(bad);

  DatatypeSetOperationRequest opaque = membership;
  opaque.descriptor.element_type_id = CanonicalTypeId::opaque_extension;
  const auto opaque_result = ApplySetOperation(opaque);

  const bool ok = encoded.ok() && has_beta.ok() && has_beta.value.encoded_value == "true" &&
                  count.ok() && count.value.encoded_value == "2" && !bad_result.ok() && !opaque_result.ok();
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"has_beta\": " << has_beta.value.encoded_value << ",\n";
  std::cout << "  \"cardinality\": \"" << count.value.encoded_value << "\",\n";
  std::cout << "  \"bad_rejected\": " << (!bad_result.ok() ? "true" : "false") << ",\n";
  std::cout << "  \"opaque_set_rejected\": " << (!opaque_result.ok() ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
