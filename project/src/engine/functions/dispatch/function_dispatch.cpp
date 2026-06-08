// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"

#include "common/function_gate.hpp"
#include "common/function_result_helpers.hpp"
#include "families/crypto_hash/crypto_hash_function_landing_zone.hpp"
#include "families/data_aggregate_functions.hpp"
#include "families/data_scalar_functions.hpp"
#include "families/cursor_stream_functions.hpp"
#include "families/extension_functions.hpp"
#include "families/lob_locator_functions.hpp"
#include "families/management_functions.hpp"
#include "families/metrics_functions.hpp"
#include "families/native_surface_functions.hpp"
#include "families/nosql_document_functions.hpp"
#include "families/nosql_graph_functions.hpp"
#include "families/nosql_kv_functions.hpp"
#include "families/range/range_function_landing_zone.hpp"
#include "families/rowset_table_functions.hpp"
#include "families/schema_ddl_functions.hpp"
#include "families/search_functions.hpp"
#include "families/security_functions.hpp"
#include "families/spatial_functions.hpp"
#include "families/timeseries_functions.hpp"
#include "families/vector_functions.hpp"

namespace scratchbird::engine::functions {

FunctionCallResult DispatchFunctionCall(const FunctionRegistry& registry,
                                        FunctionCallRequest request) {
  const auto* entry = registry.Lookup(request.context.function_id);
  if (!entry) {
    return RefuseFunctionWithDiagnostic(request,
                                        scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                        "SB_DIAG_FUNCTION_NOT_REGISTERED",
                                        "function_id is not present in the active function registry");
  }

  request.context.function_uuid = entry->function_uuid;
  request.context.package_name = entry->family;
  request.context.implementation_state = entry->implementation_state;
  request.context.package_state = entry->package_state;

  for (const auto decision : {EvaluateFunctionPackageGate(*entry, request.context),
                             EvaluateFunctionDependencyGate(*entry, request.context),
                             EvaluateFunctionSecurityGate(*entry, request.context),
                             EvaluateFunctionPolicyGate(*entry, request.context)}) {
    if (!decision.allowed) return RefuseFunctionForGate(request, decision);
  }

  if (!FunctionMayExecute(request.context)) {
    return RefuseFunctionCall(request, "function is not executable after gate evaluation");
  }

  if (IsNativeSurfaceFunction(request)) return DispatchNativeSurfaceFunction(request);
  if (IsRangeFunction(request)) return DispatchRangeFunction(request);
  if (IsLobLocatorFunction(request)) return DispatchLobLocatorFunction(request);
  if (IsCursorStreamFunction(request)) return DispatchCursorStreamFunction(request);
  if (IsRowsetTableFunction(request)) return DispatchRowsetTableFunction(request);
  if (IsSpatialFunction(request)) return DispatchSpatialFunction(request);
  if (IsCryptoHashFunction(request)) return DispatchCryptoHashFunction(request);
  if (IsDataScalarFunction(request)) return DispatchDataScalarFunction(request);
  if (IsDataAggregateFunction(request)) return DispatchDataAggregateFunction(request);
  if (IsExtensionFunction(request)) return DispatchExtensionFunction(request);
  if (IsManagementFunction(request)) return DispatchManagementFunction(request);
  if (IsMetricsFunction(request)) return DispatchMetricsFunction(request);
  if (IsNoSqlDocumentFunction(request)) return DispatchNoSqlDocumentFunction(request);
  if (IsNoSqlGraphFunction(request)) return DispatchNoSqlGraphFunction(request);
  if (IsNoSqlKvFunction(request)) return DispatchNoSqlKvFunction(request);
  if (IsSchemaDdlFunction(request)) return DispatchSchemaDdlFunction(request);
  if (IsSearchFunction(request)) return DispatchSearchFunction(request);
  if (IsSecurityFunction(request)) return DispatchSecurityFunction(request);
  if (IsTimeseriesFunction(request)) return DispatchTimeseriesFunction(request);
  if (IsVectorFunction(request)) return DispatchVectorFunction(request);

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_FUNCTION_FAMILY_HANDLER_MISSING",
                                      "function family does not have a dispatch handler");
}

}  // namespace scratchbird::engine::functions
