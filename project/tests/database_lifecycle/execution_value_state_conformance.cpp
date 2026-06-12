// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "scratchbird/engine/value.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace engine = scratchbird::engine;
namespace internal = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

void TestPublicExecutionValueSqlNullState() {
  engine::ExecutionValue value;
  Require(value.state == engine::ExecutionValueState::sql_null,
          "EDR-002 public default state must be SQL null");
  Require(value.is_null,
          "EDR-002 public default compatibility flag must be SQL null");
  Require(value.isSqlNull(),
          "EDR-002 public default value must report SQL null");
  Require(value.isNull(),
          "EDR-002 public isNull must report SQL null only");
  Require(!value.hasPayload(),
          "EDR-002 SQL null must not report a payload state");

  value.setState(engine::ExecutionValueState::sql_null);
  Require(value.is_null,
          "EDR-002 setState(SQL null) must sync the compatibility flag");
  Require(value.isSqlNull(),
          "EDR-002 explicit SQL null must report SQL null");
}

void TestPublicExecutionValueSpecialStatesAreNotSqlNull() {
  engine::ExecutionValue value;
  value.setState(engine::ExecutionValueState::value);
  Require(!value.is_null,
          "EDR-002 public value state must clear the compatibility null flag");
  Require(!value.isSqlNull(),
          "EDR-002 public value state must not report SQL null");
  Require(value.hasPayload(),
          "EDR-002 public value state must be a payload-bearing state");

  value.is_null = true;
  Require(value.isSqlNull(),
          "EDR-002 public legacy value-state null flag must remain SQL null");
  Require(!value.hasPayload(),
          "EDR-002 public legacy value-state null must not report payload");

  constexpr std::array non_null_states = {
      engine::ExecutionValueState::missing,
      engine::ExecutionValueState::default_requested,
      engine::ExecutionValueState::unknown,
      engine::ExecutionValueState::error,
      engine::ExecutionValueState::lob_handle,
      engine::ExecutionValueState::protected_value};

  for (const auto state : non_null_states) {
    value.setState(state);
    Require(!value.is_null,
            "EDR-002 public non-null special state must clear legacy null flag");
    Require(!value.isSqlNull(),
            "EDR-002 public non-null special state reported SQL null");
    Require(!value.isNull(),
            "EDR-002 public isNull overloaded a non-null special state");

    value.is_null = true;
    Require(!value.isSqlNull(),
            "EDR-002 public stale null flag overloaded a special state");
    Require(!value.isNull(),
            "EDR-002 public stale null flag made a special state null");
  }
}

void TestPublicExecutionValuePayloadStates() {
  engine::ExecutionValue value;
  value.setState(engine::ExecutionValueState::missing);
  Require(!value.hasPayload(),
          "EDR-002 public missing state must not report payload");

  value.setState(engine::ExecutionValueState::default_requested);
  Require(!value.hasPayload(),
          "EDR-002 public default-requested state must not report payload");

  value.setState(engine::ExecutionValueState::unknown);
  Require(!value.hasPayload(),
          "EDR-002 public unknown state must not report payload");

  value.setState(engine::ExecutionValueState::error);
  Require(value.hasPayload(),
          "EDR-002 public error state must report payload capability");

  value.setState(engine::ExecutionValueState::lob_handle);
  Require(value.hasPayload(),
          "EDR-002 public LOB handle state must report payload capability");

  value.setState(engine::ExecutionValueState::protected_value);
  Require(value.hasPayload(),
          "EDR-002 public protected value state must report payload capability");
}

void TestInternalEngineTypedValueSqlNullState() {
  internal::EngineTypedValue value;
  Require(value.state == internal::EngineValueState::value,
          "EDR-002 internal default state must preserve value compatibility");
  Require(!value.is_null,
          "EDR-002 internal default compatibility flag must be non-null");
  Require(!value.isSqlNull(),
          "EDR-002 internal default value must not report SQL null");

  value.is_null = true;
  Require(value.isSqlNull(),
          "EDR-002 internal legacy value-state null flag must remain SQL null");
  Require(value.isNull(),
          "EDR-002 internal isNull must preserve legacy SQL null");
  Require(!value.hasPayload(),
          "EDR-002 internal legacy SQL null must not report payload");

  value.setState(internal::EngineValueState::sql_null);
  Require(value.is_null,
          "EDR-002 internal setState(SQL null) must sync legacy null flag");
  Require(value.isSqlNull(),
          "EDR-002 internal explicit SQL null must report SQL null");
  Require(!value.hasPayload(),
          "EDR-002 internal SQL null must not report payload");
}

void TestInternalEngineTypedValueSpecialStatesAreNotSqlNull() {
  internal::EngineTypedValue value;
  value.setState(internal::EngineValueState::value);
  Require(!value.is_null,
          "EDR-002 internal value state must clear the legacy null flag");
  Require(!value.isSqlNull(),
          "EDR-002 internal value state must not report SQL null");
  Require(value.hasPayload(),
          "EDR-002 internal value state must report payload capability");

  constexpr std::array non_null_states = {
      internal::EngineValueState::missing,
      internal::EngineValueState::default_requested,
      internal::EngineValueState::unknown,
      internal::EngineValueState::error,
      internal::EngineValueState::lob_handle,
      internal::EngineValueState::protected_value};

  for (const auto state : non_null_states) {
    value.setState(state);
    Require(!value.is_null,
            "EDR-002 internal non-null special state must clear legacy null flag");
    Require(!value.isSqlNull(),
            "EDR-002 internal non-null special state reported SQL null");
    Require(!value.isNull(),
            "EDR-002 internal isNull overloaded a non-null special state");

    value.is_null = true;
    Require(!value.isSqlNull(),
            "EDR-002 internal stale null flag overloaded a special state");
    Require(!value.isNull(),
            "EDR-002 internal stale null flag made a special state null");
  }
}

void TestInternalEngineTypedValuePayloadStates() {
  internal::EngineTypedValue value;
  value.setState(internal::EngineValueState::missing);
  Require(!value.hasPayload(),
          "EDR-002 internal missing state must not report payload");

  value.setState(internal::EngineValueState::default_requested);
  Require(!value.hasPayload(),
          "EDR-002 internal default-requested state must not report payload");

  value.setState(internal::EngineValueState::unknown);
  Require(!value.hasPayload(),
          "EDR-002 internal unknown state must not report payload");

  value.setState(internal::EngineValueState::error);
  Require(value.hasPayload(),
          "EDR-002 internal error state must report payload capability");

  value.setState(internal::EngineValueState::lob_handle);
  Require(value.hasPayload(),
          "EDR-002 internal LOB handle state must report payload capability");

  value.setState(internal::EngineValueState::protected_value);
  Require(value.hasPayload(),
          "EDR-002 internal protected value state must report payload capability");
}

}  // namespace

int main() {
  TestPublicExecutionValueSqlNullState();
  TestPublicExecutionValueSpecialStatesAreNotSqlNull();
  TestPublicExecutionValuePayloadStates();
  TestInternalEngineTypedValueSqlNullState();
  TestInternalEngineTypedValueSpecialStatesAreNotSqlNull();
  TestInternalEngineTypedValuePayloadStates();
  return EXIT_SUCCESS;
}
