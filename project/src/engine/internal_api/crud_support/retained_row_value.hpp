// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../api_types.hpp"
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// A retained/staged payload is not its value state. In particular, every byte
// spelling, including former NULL/default markers, is legal present data.
// There is deliberately no conversion back to string: consumers must inspect
// the state before interpreting bytes. Column descriptors remain separately
// bound by the owning row operation; this carrier supplies no type authority.
struct CrudStoredValue {
  EngineValueState state = EngineValueState::value;
  std::string bytes;

  CrudStoredValue() = default;
  CrudStoredValue(const char* payload) : bytes(payload) {}
  CrudStoredValue(std::string payload) : bytes(std::move(payload)) {}
  CrudStoredValue(EngineValueState value_state, std::string payload)
      : state(value_state), bytes(std::move(payload)) {}

  static CrudStoredValue SqlNull() { return {EngineValueState::sql_null, {}}; }
  static CrudStoredValue Missing() { return {EngineValueState::missing, {}}; }
  static CrudStoredValue DefaultRequested() {
    return {EngineValueState::default_requested, {}};
  }
  bool isPresent() const noexcept { return state == EngineValueState::value; }
  bool isSqlNull() const noexcept { return state == EngineValueState::sql_null; }
  bool isMissing() const noexcept { return state == EngineValueState::missing; }
  bool isDefaultRequested() const noexcept {
    return state == EngineValueState::default_requested;
  }
  bool valid() const noexcept {
    return static_cast<unsigned>(state) <= static_cast<unsigned>(EngineValueState::protected_value) &&
        (EngineValueStateHasPayload(state) || bytes.empty());
  }
  bool operator==(const CrudStoredValue&) const = default;
};

using CrudValueFields = std::vector<std::pair<std::string, CrudStoredValue>>;

}  // namespace scratchbird::engine::internal_api
