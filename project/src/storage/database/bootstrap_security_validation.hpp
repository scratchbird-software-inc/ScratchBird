// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "database_lifecycle.hpp"
#include "catalog_record_codec.hpp"
#include <span>

namespace scratchbird::storage::database {

// Shared, order-independent semantic validation for bootstrap catalog records.
// Validates native headers/payloads, cardinalities, credentials, provenance and
// matching generations. Does not read storage or prove visibility, database
// ownership, transaction commitment, freshness, authentication or authorization.
// Success always leaves state.committed_by_inventory=false. Only an owning
// storage reader may combine these fields with its verified inventory/source.
// diagnostic_path is a diagnostic label, never a source of authority.
// Allocation failures propagate; they cannot produce a successful result.
DatabaseBootstrapSecurityCatalogReadResult ValidateBootstrapSecurityRecords(
    std::span<const scratchbird::core::catalog::CatalogTypedRecord> records,
    const std::string& diagnostic_path);

} // namespace scratchbird::storage::database
