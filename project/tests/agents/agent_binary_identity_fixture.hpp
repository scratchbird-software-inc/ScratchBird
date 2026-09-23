// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "uuid.hpp"
#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
namespace scratchbird::tests {
inline std::string BinaryFixtureIdentity(const core::platform::Uuid& id) {
  return {reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size()};
}
inline core::platform::Uuid NativeFixtureIdentity(std::string_view bytes) {
  if (bytes.size() != 16) throw std::invalid_argument("fixture identity requires binary16");
  core::platform::Uuid id;
  std::copy_n(reinterpret_cast<const std::uint8_t*>(bytes.data()), 16, id.bytes.begin());
  if (!core::uuid::IsEngineIdentityUuid(id)) throw std::invalid_argument("invalid fixture identity");
  return id;
}
// Labels index a fixture-local cache; they are never hashed into authority IDs.
inline std::string FixtureIdentityForLabel(const std::string& key) {
  static std::mutex mutex;
  static std::map<std::string, core::platform::Uuid> identities;
  std::lock_guard lock(mutex);
  if (auto found = identities.find(key); found != identities.end())
    return BinaryFixtureIdentity(found->second);
  const auto generated = core::uuid::GenerateEngineIdentityV7(core::platform::UuidKind::object, 1900000000000ull);
  if (!generated.ok()) throw std::runtime_error("fixture identity generation failed");
  identities.emplace(key, generated.value.value);
  return BinaryFixtureIdentity(generated.value.value);
}
}
