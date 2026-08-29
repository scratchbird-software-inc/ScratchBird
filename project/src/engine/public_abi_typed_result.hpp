// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Engine-private adoption boundary for an already-authorized typed execute
// carrier. The outer request authority is supplied independently of the
// carrier bytes. Ownership of an optional producer cursor transfers only on a
// successful adoption; no private receipt, grant, callback, or finality state
// is exposed by the public C ABI.

#include "scratchbird/engine/engine.h"
#include "engine/internal_api/typed_result_producer_cursor.hpp"
#include "wire/typed_result_transport_carrier.hpp"

#include <memory>

namespace scratchbird::engine::internal_api {

struct PublicAbiTypedResultAdoptionV1 {
  wire::TypedResultExecuteRequestAuthorityV1 request_authority;
  wire::TypedResultExecuteCarrierV1 execute_carrier;
  wire::TypedResultDescriptorAuthorityValidator descriptor_authority;
  std::unique_ptr<TypedResultProducerCursorCarrierV1> producer_cursor;
};

sb_engine_status_t AdoptTypedResultPublicAbiV1(
    PublicAbiTypedResultAdoptionV1&& adoption,
    sb_engine_result_t* out_result) noexcept;

}  // namespace scratchbird::engine::internal_api
