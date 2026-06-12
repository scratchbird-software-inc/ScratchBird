// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_DATATYPE_TRANSPORT_API
// Deterministic descriptor/value transport envelope used by backup, restore,
// archive, and replication surfaces. This is evidence transport, not runtime
// authority: restored values must still be validated by their descriptors.

struct EngineDatatypeTransportRecord {
  std::string transport_scope;
  EngineDescriptor descriptor;
  EngineTypedValue value;
  std::string compatibility_dialect;
  std::string reference_label;
  bool reference_label_alias_only = true;
  bool opaque_render_only = false;
};

struct EngineDatatypeTransportEncodeResult {
  bool ok = false;
  std::string encoded_envelope;
  std::string diagnostic_detail;
};

struct EngineDatatypeTransportDecodeResult {
  bool ok = false;
  EngineDatatypeTransportRecord record;
  std::string diagnostic_detail;
};

EngineDatatypeTransportEncodeResult EncodeDatatypeTransportRecord(const EngineDatatypeTransportRecord& record);
EngineDatatypeTransportDecodeResult DecodeDatatypeTransportRecord(const std::string& encoded_envelope);

}  // namespace scratchbird::engine::internal_api
