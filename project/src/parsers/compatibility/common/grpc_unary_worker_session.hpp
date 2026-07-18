// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>

namespace scratchbird::parser::compatibility {

struct GrpcUnaryResponse {
  std::string payload;
  std::string grpc_status{"0"};
  std::string grpc_message;
};

using GrpcUnaryResponder = GrpcUnaryResponse (*)(std::string_view header_block,
                                                 std::string_view request_payload);

int ServeGrpcUnaryWorkerSession(int fd, GrpcUnaryResponder responder);

} // namespace scratchbird::parser::compatibility
