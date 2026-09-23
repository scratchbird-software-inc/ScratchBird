// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/query/plan_api.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
void Check(bool value) { if (!value) std::abort(); }
int main() {
  namespace api = scratchbird::engine::internal_api;
  scratchbird::engine::optimizer::CanonicalPreparedPlanResultDescriptor stored;
  Check(api::StoredTimezoneProfileMatches(stored, std::nullopt));
  Check(!api::StoredTimezoneProfileMatches(stored, std::string("UTC")));
  stored.encoded_descriptor = "canonical=timestamp;timezone_profile_id=UTC;nullable=false";
  Check(api::StoredTimezoneProfileMatches(stored, std::string("UTC")));
  Check(!api::StoredTimezoneProfileMatches(stored, std::string("Europe/Paris")));
  Check(!api::StoredTimezoneProfileMatches(stored, std::nullopt));
  stored.timezone_uuid = scratchbird::tests::FixtureUuid(1327, 1);
  Check(!api::StoredTimezoneProfileMatches(stored, std::string("UTC")));
  stored.timezone_uuid = {};
  stored.encoded_descriptor += ";timezone_profile_id=UTC";
  Check(!api::StoredTimezoneProfileMatches(stored, std::string("UTC")));
  stored.encoded_descriptor = "timezone_profile_id=";
  Check(!api::StoredTimezoneProfileMatches(stored, std::string{}));
  std::cout << "cached timezone authority regression PASS\n";
}
