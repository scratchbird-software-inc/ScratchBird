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

#include <type_traits>

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

// SEARCH_KEY: SB_NATIVE_FAMILY_GUARD (2026-12-04, owner: guard each ".sb." family at its entry before S4g).
// The entry-point form of the same refusal. A legacy side-file family guards its ENTRY (the load/open/append/publish function
// every caller of that store calls) with
//     if (NativeServedContext(ctx)) return NativeFamilyRefusalAs<ReturnType>("<side-file stem>", ctx);
// as the FIRST statement, before any lock, validation, path or file touch. `family` is the side-file stem the family names
// (e.g. "mga_row_versions"), a build-time token. The return type decides how the refusal is carried:
//   * EngineApiDiagnostic                      -> the refusal itself;
//   * a result type with a `.diagnostic` member -> a value-initialized result whose diagnostic is the refusal;
//   * a result type with an `ok` flag and a `.diagnostics` list (EngineApiResult and its derived results) -> ok = false and the
//     refusal as the one diagnostic;
//   * void -> a plain return (nothing is read or written);
//   * any other default-constructible type (bool, integers, optional, vector, plain records: an entry that cannot carry a
//     diagnostic) -> the value-initialized EMPTY answer, which touches no file. Such entries are listed in the inventory so the
//     owner can decide whether a caller outside the family needs a carrier.
// A non-native context is never affected.
template <class Result>
Result NativeFamilyRefusalAs(const char* family, const EngineRequestContext& context) {
  if constexpr (std::is_void_v<Result>) {
    (void)family;
    (void)context;
    return;
  } else if constexpr (std::is_same_v<Result, EngineApiDiagnostic>) {
    return NativeLegacyRegistryRefusal(family, context);
  } else if constexpr (requires(Result& r) { r.diagnostic = NativeLegacyRegistryRefusal(family, context); }) {
    Result result{};
    result.diagnostic = NativeLegacyRegistryRefusal(family, context);
    return result;
  } else if constexpr (requires(Result& r) {
                         r.ok = false;
                         r.diagnostics.push_back(NativeLegacyRegistryRefusal(family, context));
                       }) {
    Result result{};
    result.ok = false;
    result.diagnostics.push_back(NativeLegacyRegistryRefusal(family, context));
    return result;
  } else {
    return Result{};
  }
}

}  // namespace scratchbird::engine::internal_api
