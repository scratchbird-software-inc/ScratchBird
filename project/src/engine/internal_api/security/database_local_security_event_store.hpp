// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api {

// CORE-ENGINE-PRIVATE-DB-SECURITY-LIFECYCLE-V1
// This is an engine-private database-lifecycle/bootstrap carrier. It is not a
// SQL, SBLR, parser, listener, driver, public-ABI, or public-profile surface.
inline constexpr std::string_view
    kDatabaseLocalSecurityLifecycleBootstrapAuthorityTagV1 =
        "engine.private.database_security_lifecycle_bootstrap.v1";
inline constexpr std::string_view kDatabaseLocalSecurityRelationUuidV1 =
    "018f7a10-1280-7000-8000-000000000107";

inline constexpr const char*
    kDatabaseLocalSecurityDiagnosticAuthorityRequired =
        "SECURITY.CATALOG.PRIVATE_LIFECYCLE_AUTHORITY_REQUIRED";
inline constexpr const char*
    kDatabaseLocalSecurityDiagnosticTransactionRequired =
        "SECURITY.PRINCIPAL.MGA_TRANSACTION_REQUIRED";
inline constexpr const char* kDatabaseLocalSecurityDiagnosticBatchInvalid =
    "SECURITY.CATALOG.PRIVATE_LIFECYCLE_BATCH_INVALID";
inline constexpr const char* kDatabaseLocalSecurityDiagnosticCorrupt =
    "SECURITY.CATALOG.PRIVATE_LIFECYCLE_CORRUPT";
inline constexpr const char* kDatabaseLocalSecurityDiagnosticWriteFailed =
    "SECURITY.PRINCIPAL.DATABASE_WRITE_FAILED";

enum class DatabaseLocalSecurityEventVisibilityV1 : std::uint8_t {
  latest_committed,
  include_reader_own_uncommitted,
};

struct DatabaseLocalSecurityEventStoreStateV1 {
  // These are only the page-backed SBSECPL1 lifecycle rows. Bootstrap catalog
  // rows remain owned by ReadDatabaseBootstrapSecurityCatalog and are not
  // synthesized into this vector. AUTH_CONTEXT_SUCCESSOR rows are included.
  std::vector<std::string> events;
  std::uint64_t security_context_generation = 0;
  std::vector<std::uint64_t> page_numbers;
  bool database_identity_authenticated = false;
  bool page_identity_authenticated = false;
  bool durable_inventory_authenticated = false;
  // Diagnostic/test evidence only; never authority.
  std::uint64_t locator_page_reads = 0;
  std::uint64_t security_chain_page_reads = 0;
  std::uint64_t legacy_scan_page_reads = 0;
  std::uint64_t locator_migration_count = 0;
};

struct DatabaseLocalSecurityEventStoreLoadResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  DatabaseLocalSecurityEventStoreStateV1 state;
  std::vector<std::string> evidence;
};

struct DatabaseLocalSecurityEventStoreAppendResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::uint64_t prior_security_context_generation = 0;
  std::uint64_t security_context_generation = 0;
  std::uint64_t page_number = 0;
  std::vector<std::string> sealed_events;
  std::vector<std::string> evidence;
};

// Loads and validates the database-local sys.security event chain. The
// returned generation begins at the authenticated bootstrap catalog generation
// and advances through exact MGA-visible page-backed successors.
DatabaseLocalSecurityEventStoreLoadResultV1
LoadDatabaseLocalSecurityEventStoreV1(
    const EngineRequestContext& context,
    DatabaseLocalSecurityEventVisibilityV1 visibility =
        DatabaseLocalSecurityEventVisibilityV1::latest_committed);

// Appends exactly one authority event plus its AUDIT and CACHE_INVALIDATE rows.
// The provider creates and seals the sole AUTH_CONTEXT_SUCCESSOR, then stages
// the batch under the caller's already-active durable MGA transaction.
DatabaseLocalSecurityEventStoreAppendResultV1
AppendDatabaseLocalSecurityEventBatchV1(
    const EngineRequestContext& context,
    std::span<const std::string> unsealed_events);

}  // namespace scratchbird::engine::internal_api
