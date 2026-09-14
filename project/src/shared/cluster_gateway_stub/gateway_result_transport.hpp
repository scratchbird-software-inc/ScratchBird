// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <scratchbird/cluster_gateway_abi_v1.h>

namespace scratchbird::shared::cluster_gateway {

// Memory/correlation checks only, not opcode, security, typed-result, digest,
// provider or MGA admission. The host retains an immutable copy of the buffer
// descriptor BEFORE invoking the gateway. Neither this helper nor a successful
// check grants permission for a local or cluster effect.
//
// No supplied payload pointer is dereferenced, and no memory is allocated.
// The caller owns the actual allocation described by supplied_storage; this
// check cannot prove the lifetime/accessibility of a caller's allocation.
SbCgStatusV1 ValidateReturnedStorage(
    const SbCgMutableBufferV1& supplied_storage,
    const SbCgMutableBufferV1& returned_storage,
    const SbCgResultV1& returned_result) noexcept;

// The host retains the independently validated request across the ABI call.
// Checks only exact v1 record shape and the three required correlation echoes.
// Full semantic/result-digest/vector validation must follow before effects.
SbCgStatusV1 ValidateResultCorrelation(
    const SbCgRequestV1& retained_request,
    const SbCgResultV1& returned_result) noexcept;

}  // namespace scratchbird::shared::cluster_gateway
