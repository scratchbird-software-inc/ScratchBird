// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_filespace_initialization.hpp"
#include "catalog_database_record_codec.hpp"
#include "catalog_named_creation.hpp"

namespace scratchbird::storage::database {
struct NativeInitialDatabaseRequest {
  const NativeFilespaceInitializationRequest* construction=nullptr;
  // Exact independently admitted node datatype cohort; never defaulted from
  // the current executable or the initial catalog generation.
  core::catalog::CatalogDatabaseDatatypeCohort datatype_cohort;
  std::string_view database_default_language;
  // Complete owner-issued database common metadata and actual aliases. The
  // definition payload is empty; native construction values supply it.
  core::catalog::CatalogNamedCreationInput definition;
  core::catalog::CatalogNamedCreationLimits limits;
};
enum class NativeInitialDatabaseError {
  none,invalid_request,invalid_construction,invalid_definition,invalid_names,
  duplicate_identity,resource_exhausted
};
struct NativeInitialDatabaseResult {
  NativeInitialDatabaseError error=NativeInitialDatabaseError::invalid_request;
  core::catalog::CatalogNamedCreationError name_error=core::catalog::CatalogNamedCreationError::none;
  std::optional<core::catalog::CatalogNamedCreationSet> records;
  bool ok() const noexcept{return error==NativeInitialDatabaseError::none&&records.has_value();}
};
// Owning native database identity/cohort definition and complete names, bound
// to the actual initial primary construction request. No legacy header/cache
// import, reference/owner invention, physical effects, authentication or node
// activation. Caller retains the immutable construction/input fence and actual
// input/output/scratch memory grants; limits are not grants. Never call under a
// device guard. The enclosing producer still admits namespaces/profiles/policies
// and combines all families before the native storage constructor is called.
NativeInitialDatabaseResult AssembleNativeInitialDatabaseRecords(
    const NativeInitialDatabaseRequest&) noexcept;
struct NativeInitialDatabaseFilespaceRequest {
  NativeInitialDatabaseRequest database;
  core::catalog::CatalogNamedCreationInput primary_filespace;
};
// Coherent initial database AND actual primary filespace definitions/names.
// Complete borrowed admission and cross-family identity/budget checks precede
// owning copies. Primary manifest state remains the actual initializing state;
// required-validation flags are declarations, not completed checks or serving.
NativeInitialDatabaseResult AssembleNativeInitialDatabaseFilespaceRecords(
    const NativeInitialDatabaseFilespaceRequest&) noexcept;
} // namespace scratchbird::storage::database
