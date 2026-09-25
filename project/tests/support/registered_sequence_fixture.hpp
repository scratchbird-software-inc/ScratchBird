// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "binary_uuid_fixture.hpp"
#include "../../src/engine/sblr/sblr_sequence_runtime.hpp"
#include <initializer_list>
#include <stdexcept>
namespace scratchbird::tests {
// Explicit registration in the engine sequence unit-test registry. These
// definitions/aliases confer no transaction finality or durable authority.
inline void RegisterSequenceFixtures(std::initializer_list<const char*> names,
                                     std::uint64_t fixture_domain) {
  namespace sblr = engine::sblr;
  auto& registry = sblr::ProcessSblrSequenceRegistry();
  std::uint64_t ordinal = 0;
  for (const auto* name : names) {
    sblr::SblrSequenceDefinition definition;
    definition.sequence_uuid = FixtureUuid(fixture_domain, ++ordinal);
    if (!sblr::RegisterSblrSequence(&registry, definition, {}).ok() ||
        !sblr::RegisterSblrSequenceAlias(&registry, definition.sequence_uuid, name, {}).ok())
      throw std::runtime_error("explicit sequence fixture registration failed");
  }
}
}
