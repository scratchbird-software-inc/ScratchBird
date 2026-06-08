// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PRODUCT_SKELETON_IDENTITY

#include "product_identity.hpp"

namespace scratchbird::server {

const ServerProductIdentity& GetServerProductIdentity() {
  static const ServerProductIdentity identity{
      "SBsrv",
      "standalone-server",
      "0.1.0",
      "SBPS",
      "product-skeleton",
  };
  return identity;
}

std::string ProductVersionLine() {
  const auto& identity = GetServerProductIdentity();
  return identity.product_name + " " + identity.product_version + " (" +
         identity.product_kind + ", " + identity.implementation_stage + ")";
}

}  // namespace scratchbird::server
