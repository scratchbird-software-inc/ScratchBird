// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_IDENTITY

#pragma once

#include <string>

namespace scratchbird::server {

struct ServerProductIdentity {
  std::string product_name;
  std::string product_kind;
  std::string product_version;
  std::string protocol_family;
  std::string release_channel;
};

const ServerProductIdentity& GetServerProductIdentity();
std::string ProductVersionLine();

}  // namespace scratchbird::server
