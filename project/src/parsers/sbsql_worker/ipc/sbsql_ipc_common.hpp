// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "parser_ipc_common.hpp"

// SBsql source-compatibility aliases belong to the SBsql parser package, not
// to the family-neutral parser/server IPC schema.
namespace scratchbird::parser::sbsql {
using ipc::Diagnostic;
using ipc::EscapeJson;
using ipc::Field;
using ipc::IsPublicDiagnosticFieldAllowed;
using ipc::LooksLikeCanonicalUuid;
using ipc::MakeDiagnostic;
using ipc::MessageVectorSet;
using ipc::MessageVectorToJson;
using ipc::ToUpperAscii;
} // namespace scratchbird::parser::sbsql
