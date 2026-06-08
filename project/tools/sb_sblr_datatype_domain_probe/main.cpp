// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"

#include "datatype_operations.hpp"

#include <iostream>
#include <string>
#include <utility>

using namespace scratchbird::engine::internal_api;
using namespace scratchbird::engine::sblr;

namespace {

EngineDescriptor Descriptor(std::string type_name) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(type_name);
  return descriptor;
}

EngineRequestContext Context() {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "sblr-datatype-domain-probe";
  return context;
}

}  // namespace

int main() {
  EngineApiRequest cast_request;
  cast_request.descriptors.push_back(Descriptor("character"));
  cast_request.descriptors.push_back(Descriptor("int32"));
  EngineRowValue row;
  row.fields.push_back({"value", {Descriptor("character"), "42", false}});
  cast_request.rows.push_back(row);

  SblrDispatchRequest dispatch;
  dispatch.context = Context();
  dispatch.envelope = MakeSblrEnvelope("query.cast_value", "SBLR_QUERY_CAST_VALUE", "datatype-cast");
  dispatch.envelope.requires_security_context = false;
  dispatch.api_request = cast_request;
  const auto cast = DispatchSblrOperation(dispatch);

  EngineApiRequest extract_request;
  extract_request.descriptors.push_back(Descriptor("timestamp"));
  EngineRowValue extract_row;
  extract_row.fields.push_back({"value", {Descriptor("timestamp"), "2026-05-01T12:34:56", false}});
  extract_request.rows.push_back(extract_row);
  extract_request.option_envelopes.push_back("field:year");
  dispatch.envelope = MakeSblrEnvelope("query.extract_value", "SBLR_QUERY_EXTRACT_VALUE", "datatype-extract");
  dispatch.envelope.requires_security_context = false;
  dispatch.api_request = extract_request;
  const auto extract = DispatchSblrOperation(dispatch);

  const bool ok = cast.accepted && cast.api_result.ok && extract.accepted && extract.api_result.ok;
  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"cast_dispatch_ok\": " << (cast.api_result.ok ? "true" : "false") << ",\n";
  std::cout << "  \"extract_dispatch_ok\": " << (extract.api_result.ok ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
