// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "immudb_grpc_worker_session.hpp"

#include "grpc_unary_worker_session.hpp"
#include "immudb_dialect.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace scratchbird::parser::immudb {
namespace {

void AppendVarint(std::string* out, std::uint64_t value) {
  while (value >= 0x80) {
    out->push_back(static_cast<char>((value & 0x7fU) | 0x80U));
    value >>= 7;
  }
  out->push_back(static_cast<char>(value));
}

void AppendFieldVarint(std::string* out, std::uint32_t field, std::uint64_t value) {
  AppendVarint(out, (static_cast<std::uint64_t>(field) << 3) | 0);
  AppendVarint(out, value);
}

void AppendFieldBytes(std::string* out, std::uint32_t field, std::string_view value) {
  AppendVarint(out, (static_cast<std::uint64_t>(field) << 3) | 2);
  AppendVarint(out, value.size());
  out->append(value.data(), value.size());
}

scratchbird::parser::compatibility::GrpcUnaryResponse Ok(std::string payload) {
  scratchbird::parser::compatibility::GrpcUnaryResponse response;
  response.payload = std::move(payload);
  return response;
}

scratchbird::parser::compatibility::GrpcUnaryResponse Unimplemented(std::string_view method) {
  scratchbird::parser::compatibility::GrpcUnaryResponse response;
  response.grpc_status = "12";
  (void)method;
  response.grpc_message = "immudb method is not admitted by ScratchBird replay worker";
  return response;
}

std::string HealthPayload() {
  std::string out;
  AppendFieldVarint(&out, 1, 1);
  AppendFieldBytes(&out, 2, "1.11.0");
  return out;
}

std::string LoginPayload() {
  std::string out;
  AppendFieldBytes(&out, 1, "scratchbird-immudb-reference-token");
  return out;
}

std::string CurrentStatePayload() {
  const std::string tx_hash(32, '\0');
  std::string out;
  AppendFieldBytes(&out, 1, "defaultdb");
  AppendFieldVarint(&out, 2, 1);
  AppendFieldBytes(&out, 3, tx_hash);
  return out;
}

std::string OpenSessionPayload() {
  std::string out;
  AppendFieldBytes(&out, 1, "scratchbird-immudb-session-id");
  AppendFieldBytes(&out, 2, "scratchbird-immudb-reference-token");
  return out;
}

scratchbird::parser::compatibility::GrpcUnaryResponse ImmudbResponse(
    std::string_view header_block,
    std::string_view) {
  if (header_block.find("/immudb.schema.ImmuService/Health") != std::string_view::npos) {
    return Ok(HealthPayload());
  }
  if (header_block.find("/immudb.schema.ImmuService/Login") != std::string_view::npos) {
    return Ok(LoginPayload());
  }
  if (header_block.find("/immudb.schema.ImmuService/CurrentState") != std::string_view::npos) {
    const auto parsed = ParseStatement("CURRENT_STATE");
    if (!parsed.ok || !parsed.catalog_projection_only) {
      scratchbird::parser::compatibility::GrpcUnaryResponse response;
      response.grpc_status = "13";
      response.grpc_message = "immudb current-state catalog projection refused";
      return response;
    }
    return Ok(CurrentStatePayload());
  }
  if (header_block.find("/immudb.schema.ImmuService/DatabaseHealth") != std::string_view::npos) {
    return Ok({});
  }
  if (header_block.find("/immudb.schema.ImmuService/OpenSession") != std::string_view::npos) {
    return Ok(OpenSessionPayload());
  }
  if (header_block.find("/immudb.schema.ImmuService/KeepAlive") != std::string_view::npos ||
      header_block.find("/immudb.schema.ImmuService/CloseSession") != std::string_view::npos) {
    return Ok({});
  }
  if (header_block.find("immudb") == std::string_view::npos) {
    return Ok(HealthPayload());
  }
  return Unimplemented(header_block);
}

} // namespace

int ServeImmudbGrpcWorkerSession(int fd) {
  return scratchbird::parser::compatibility::ServeGrpcUnaryWorkerSession(fd, ImmudbResponse);
}

} // namespace scratchbird::parser::immudb
