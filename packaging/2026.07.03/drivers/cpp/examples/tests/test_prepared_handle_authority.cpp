// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <istream>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#define private public
#include "scratchbird/client/network_client.h"
#undef private

namespace {

namespace client = scratchbird::client;
namespace core = scratchbird::core;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

std::array<std::uint8_t, 16> Uuid(std::uint8_t seed) {
  std::array<std::uint8_t, 16> uuid{};
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = static_cast<std::uint8_t>(seed + i);
  }
  uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x70u);
  uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
  return uuid;
}

client::NetworkPreparedStatement PreparedFor(client::NetworkClient* owner) {
  client::NetworkPreparedStatement statement;
  statement.sql_ = "select 1";
  statement.statement_name_ = "ipar_prepared_handle_authority";
  statement.valid_ = true;
  statement.owner_client_token_ = reinterpret_cast<std::uintptr_t>(owner);
  statement.owner_session_id_ = owner->session_id_;
  return statement;
}

void RequireDriverRefuses(client::NetworkClient* client,
                          client::NetworkPreparedStatement* statement,
                          std::string_view message) {
  client::NetworkResultSet results;
  core::ErrorContext error;
  const auto status = client->executePrepared(*statement, results, &error);
  Require(status == core::Status::INVALID_ARGUMENT, message);
  Require(error.message == "Prepared statement is not valid for this client session",
          "IPAR driver prepared refusal message mismatch");
  Require(results.rows.empty(), "IPAR driver prepared refusal produced rows");
}

void ValidateDriverPreparedHandlesBindClientSession() {
  client::NetworkClient owner;
  owner.connected_ = true;
  owner.session_id_ = Uuid(0x20);

  client::NetworkClient other;
  other.connected_ = true;
  other.session_id_ = Uuid(0x40);

  auto statement = PreparedFor(&owner);
  Require(statement.isValid(), "IPAR driver prepared baseline handle invalid");

  RequireDriverRefuses(&other,
                       &statement,
                       "IPAR driver accepted prepared handle on another client");

  owner.session_id_ = Uuid(0x60);
  RequireDriverRefuses(&owner,
                       &statement,
                       "IPAR driver accepted prepared handle after session change");

  statement.owner_session_id_ = {};
  owner.session_id_ = {};
  RequireDriverRefuses(&owner,
                       &statement,
                       "IPAR driver accepted prepared handle without session binding");
}

}  // namespace

int main() {
  ValidateDriverPreparedHandlesBindClientSession();
  return EXIT_SUCCESS;
}
