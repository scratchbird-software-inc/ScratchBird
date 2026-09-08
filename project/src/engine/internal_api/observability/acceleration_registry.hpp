// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_ACCELERATION_REGISTRY_SNAPSHOT_V1
// Immutable, engine-owned projection of the acceleration providers compiled
// into this process.  Parser text and SBLR operands cannot populate it.
struct EngineAccelerationProviderRow {
  std::string provider_id;
  std::string provider_class;
  std::string state;
  std::string requirement;
  std::string redacted_provider;
  std::string diagnostic_code;
  std::uint64_t generation = 0;
};

struct EngineAccelerationRegistrySnapshot {
  std::uint64_t generation = 0;
  std::string evidence_sha256;
  std::vector<EngineAccelerationProviderRow> rows;
};

std::shared_ptr<const EngineAccelerationRegistrySnapshot>
LoadEngineAccelerationRegistrySnapshot();

}  // namespace scratchbird::engine::internal_api
