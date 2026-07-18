// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "tikv_grpc_worker_session.hpp"

#include "grpc_unary_worker_session.hpp"
#include "tikv_dialect.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace scratchbird::parser::tikv {
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

scratchbird::parser::compatibility::GrpcUnaryResponse Ok(std::string payload) {
  scratchbird::parser::compatibility::GrpcUnaryResponse response;
  response.payload = std::move(payload);
  return response;
}

scratchbird::parser::compatibility::GrpcUnaryResponse Unimplemented(std::string_view method) {
  scratchbird::parser::compatibility::GrpcUnaryResponse response;
  response.grpc_status = "12";
  (void)method;
  response.grpc_message = "tikv method is not admitted by ScratchBird replay worker";
  return response;
}

std::string StoreInfoPayload() {
  std::string out;
  AppendFieldVarint(&out, 1, 1);
  AppendFieldVarint(&out, 2, 0);
  return out;
}

scratchbird::parser::compatibility::GrpcUnaryResponse TikvResponse(
    std::string_view header_block,
    std::string_view) {
  if (header_block.find("/debugpb.Debug/GetStoreInfo") != std::string_view::npos) {
    const auto parsed = ParseStatement("STORE_INFO");
    if (!parsed.ok || !parsed.catalog_projection_only) {
      scratchbird::parser::compatibility::GrpcUnaryResponse response;
      response.grpc_status = "13";
      response.grpc_message = "tikv store-info catalog projection refused";
      return response;
    }
    return Ok(StoreInfoPayload());
  }
  return Unimplemented(header_block);
}

} // namespace

int ServeTikvGrpcWorkerSession(int fd) {
  return scratchbird::parser::compatibility::ServeGrpcUnaryWorkerSession(fd, TikvResponse);
}

} // namespace scratchbird::parser::tikv
