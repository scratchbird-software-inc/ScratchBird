// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_management_control_bundle.hpp"
#include <memory_resource>
namespace scratchbird::storage::database::detail {
// Internal composition only: caller owns the complete backing and excludes all
// immutable inputs. May run under an enclosing ordered source fence; a supplied
// observation batch must outlive that fence. No grant, implicit heap fallback,
// scheduling, authorization or durable completion is established.
NativeManagementControlBundleViewRead DecodeNativeManagementControlBundleBacked(
    std::span<const std::span<const byte>>,const NativeManagementControlBundleRoot&,
    const Uuid& database,const Uuid& bootstrap,u64,std::pmr::memory_resource&) noexcept;
NativeManagementControlBundleDeviceRead ReadNativeManagementControlBundleBacked(
    const disk::NativeFilespaceDevice&,const NativeManagementControlBundleRoot&,
    const Uuid& database,const Uuid& bootstrap,u64 budget,NativeManagementControlReadContext,
    std::span<const byte> historical,disk::FileDevice::ReadLatencyBatch*,
    std::pmr::memory_resource&) noexcept;
} // namespace scratchbird::storage::database::detail
