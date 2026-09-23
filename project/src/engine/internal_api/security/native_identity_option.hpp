// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "mga_relation_store/mga_metadata_record_codec.hpp"

namespace scratchbird::engine::internal_api {
// Options are individually bounded buffers. Identity values are exactly raw16;
// duplicate, absent, textual, nil, and invalid engine identities are refused.
inline bool ReadSecurityIdentityOption(const EngineApiRequest& request,
                                      std::string_view name,
                                      EngineUuid* output) {
  if (!output) return false;
  const std::string prefix = std::string(name) + ":";
  bool found = false;
  EngineUuid candidate;
  for (const auto& option : request.option_envelopes) {
    if (!option.starts_with(prefix)) continue;
    if (found || !ReadMetadataUuid(std::string_view(option).substr(prefix.size()),
                                   &candidate)) return false;
    found = true;
  }
  if (!found) return false;
  *output = candidate;
  return true;
}
}  // namespace scratchbird::engine::internal_api
