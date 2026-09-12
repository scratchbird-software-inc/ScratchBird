// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "behavior_support/api_behavior_store.hpp"

namespace scratchbird::engine::internal_api {

std::string ApiBehaviorPrimaryName(const EngineApiRequest& request,
                                   const std::string& fallback) {
  if (!request.localized_names.empty() && !request.localized_names.front().name.empty())
    return request.localized_names.front().name;
  for (const auto& option : request.option_envelopes)
    if (option.rfind("name:", 0) == 0) return option.substr(5);
  // This is a caller-provided display cache, not catalog name authority.
  // Never synthesize a user-facing name from the system identity.
  return fallback;
}

EngineUuid ApiBehaviorObjectUuid(const EngineApiRequest& request,
                                  const std::string& kind) {
  // Related objects describe dependencies. Choosing their first identity as
  // the primary object could mutate/alias an unrelated catalog object.
  return UuidOrGenerated(request.target_object.uuid,
      kind == "database" ? "database" : kind == "schema" ? "schema" : "object");
}
} // namespace scratchbird::engine::internal_api
