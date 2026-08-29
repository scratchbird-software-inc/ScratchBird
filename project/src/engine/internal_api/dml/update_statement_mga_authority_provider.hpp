// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_DML_UPDATE_STATEMENT_MGA_AUTHORITY_PROVIDER_V1
// Engine-private typed facade over the durable MGA statement-savepoint
// authority.  Callers provide exact UUID/generation records only.  A parser
// name, SQL text, hash-derived label, or metadata epoch is never accepted as
// savepoint or publication authority.

inline constexpr const char* kDmlUpdateStatementMgaDiagnosticInvalid =
    "SBLR.OPERAND_INVALID";
inline constexpr const char* kDmlUpdateStatementMgaDiagnosticStale =
    "MGA.TRANSACTION.STALE";
inline constexpr const char* kDmlUpdateStatementMgaDiagnosticFailed =
    "DML.UPDATE_FAILED";
inline constexpr const char* kDmlUpdateStatementMgaDiagnosticRollbackFailed =
    "MGA.TRANSACTION.ROLLBACK_FAILED";
inline constexpr const char* kDmlUpdateStatementMgaDiagnosticAccessDenied =
    "SECURITY.ACCESS_DENIED";

struct EngineDmlUpdateStatementMgaAuthorityOpenRequestV1 {
  EngineRequestContext context;
  std::string authenticated_statement_receipt_uuid;
  std::string operation_uuid;
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation = 0;
  std::string recovery_token_uuid;
  std::uint64_t recovery_generation = 0;
  // Exact identity reserved durably by the MGA operation provider before the
  // bound DUJR is published.  Open consumes it; it never issues a replacement.
  std::string reserved_publication_barrier_uuid;
  std::uint64_t reserved_publication_barrier_generation = 0;
};

struct EngineDmlUpdateStatementMgaAuthorityResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  MgaDmlUpdateStatementSavepointAuthorityV1 authority;
};

struct EngineDmlUpdateStatementMgaAuthorityRecoverRequestV1 {
  EngineDmlUpdateStatementMgaAuthorityOpenRequestV1 current;
  std::string savepoint_uuid;
  std::uint64_t savepoint_generation = 0;
};

struct EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1 {
  EngineDmlUpdateStatementMgaAuthorityOpenRequestV1 current;
  MgaDmlUpdateStatementSavepointAuthorityV1 admitted;
};

EngineDmlUpdateStatementMgaAuthorityResultV1
OpenDmlUpdateStatementMgaAuthorityV1(
    const EngineDmlUpdateStatementMgaAuthorityOpenRequestV1& request);
EngineDmlUpdateStatementMgaAuthorityResultV1
RecoverDmlUpdateStatementMgaAuthorityV1(
    const EngineDmlUpdateStatementMgaAuthorityRecoverRequestV1& request);
EngineDmlUpdateStatementMgaAuthorityResultV1
RevalidateDmlUpdateStatementMgaAuthorityV1(
    const EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1& request);
EngineDmlUpdateStatementMgaAuthorityResultV1
RollbackDmlUpdateStatementMgaAuthorityV1(
    const EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1& request);
EngineDmlUpdateStatementMgaAuthorityResultV1
ReleaseDmlUpdateStatementMgaAuthorityV1(
    const EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1& request);

}  // namespace scratchbird::engine::internal_api
