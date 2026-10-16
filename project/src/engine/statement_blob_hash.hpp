// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "engine/statement_blob_input.hpp"
#include "core/datatypes/datatype_blob_component.hpp"

namespace scratchbird::engine::internal_api {
enum class StatementBlobHashFailureV3 : std::uint8_t {
  none, source_unbound, admission_closed, operation_not_admitted, sql_null_has_no_content_hash,
  resource_preflight, allocation_failed, lifetime, digest_failed, internal_failure
};
// Observed digest of the actual guarded source, NOT equality or a published
// index entry. Every candidate match still requires a protected exact recheck.
// The digest is published only on success and is zero on every failure.
struct StatementBlobHashResultV3 {
  StatementBlobHashFailureV3 failure=StatementBlobHashFailureV3::source_unbound;
  core::hash::Blake3Digest digest{};
  bool lifetime_invoked=false;
  core::datatypes::BlobLifetimeRuntimeResultV3 lifetime{};
  core::datatypes::BlobComponentPassResultV3 computation{};
  core::datatypes::BlobLifetimeResourceV3 resource{};
  std::uint64_t required=0,available=0,bytes_visited=0;
  bool ok() const noexcept { return failure==StatementBlobHashFailureV3::none && lifetime_invoked && lifetime.ok(); }
};
} // namespace scratchbird::engine::internal_api
