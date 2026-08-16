// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// CSC-TEST-002329: public prepare/bind/execute process client for SBLR_PARAMETER.
#include "scratchbird/client/connection.h"

#include <cstdint>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 6) {
    std::cerr << "usage: parameter-client HOST PORT DATABASE USER PASSWORD\n";
    return 2;
  }
  scratchbird::client::ConnectionConfig config;
  config.host = argv[1];
  config.tcp_port = static_cast<std::uint16_t>(std::stoul(argv[2]));
  config.database_name = argv[3];
  config.username = argv[4];
  config.password = argv[5];
  config.ssl_mode = "disable";
  config.transport_mode = "inet_listener";
  config.front_door_mode = "direct";
  config.application_name = "sbsql-sblr-parameter-e2e";
  config.query_timeout_ms = 30000;
  config.read_timeout_ms = 30000;
  config.write_timeout_ms = 30000;

  scratchbird::core::ErrorContext error;
  scratchbird::client::Connection connection;
  if (connection.connect(config, &error) != scratchbird::core::Status::OK) {
    std::cerr << "connect: " << error.message << '\n';
    return 3;
  }
  scratchbird::client::PreparedStatement statement;
  if (connection.prepare(
          "SELECT id FROM app.customers WHERE id = ?", &statement,
          &error) != scratchbird::core::Status::OK) {
    std::cerr << "prepare: " << error.message << '\n';
    return 4;
  }
  if (!statement.isValid() || statement.getParameterCount() != 1) {
    std::cerr << "prepared descriptor did not expose one parameter\n";
    return 5;
  }
  statement.setInt64(0, 1);
  scratchbird::client::ResultSet rows;
  if (statement.executeQuery(&rows, &error) != scratchbird::core::Status::OK) {
    std::cerr << "execute: " << error.message << '\n';
    return 6;
  }
  if (!rows.next() || rows.getInt64(0) != 1 || rows.next()) {
    std::cerr << "prepared result was not the exact singleton bigint row\n";
    return 7;
  }
  std::cout << "1\n";
  return 0;
}
