// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include "catalog_record_codec.hpp"

namespace scratchbird::engine::internal_api {
enum class ContextNamespaceAccess { read, write };
// Evaluates a definition already selected by the owning live catalog snapshot
// and a security context already materialized by its owning engine session.
// This does not acquire/replace either authority, publish a namespace, bind a
// name, admit a value, or issue a capability. Caller retains both owner pins.
bool EvaluateContextNamespaceAccess(const EngineRequestContext&,
    const core::catalog::CatalogMetadataVersionView&,
    const EngineUuid& expected_namespace_uuid, std::uint64_t expected_generation,
    ContextNamespaceAccess) noexcept;
}  // namespace scratchbird::engine::internal_api
