// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

// SEARCH_KEY: SB_NATIVE_LEGACY_REGISTRY_REFUSAL (thin-slice S3e, plan 38 Q243 = 1).
// A context that carries EngineRequestContext::native_served_facts belongs to a native database: no .sb.* side
// file may be read, created or written for it. The per-statement registries that still live in side files
// (prepared statements, diagnostic identities, transaction begin authority, catalog object events) either answer
// process-locally (the prepared-statement LOAD is the empty set) or refuse with this one specific code until a
// native relation replaces them. The vector carries the registry family as a build-time token and the database
// identity; no prose, no path.

#include "api_diagnostics.hpp"
#include "native_served_database_facts.hpp"

namespace scratchbird::engine::internal_api {

inline bool NativeServedContext(const EngineRequestContext& context) noexcept {
  return static_cast<bool>(context.native_served_facts);
}

// `registry_family` is a build-time token (e.g. "prepared_statement_registry"), never user data.
inline EngineApiDiagnostic NativeLegacyRegistryRefusal(const char* registry_family,
                                                       const EngineRequestContext& context) {
  auto diagnostic = MakeEngineApiDiagnostic("ENGINE.NATIVE.LEGACY_REGISTRY_REFUSED",
                                            "engine.native.legacy_registry_refused", {}, true);
  diagnostic.fields.push_back({"registry_family", registry_family});
  diagnostic.identity_fields.push_back({"database_uuid", context.database_uuid});
  return diagnostic;
}

}  // namespace scratchbird::engine::internal_api
