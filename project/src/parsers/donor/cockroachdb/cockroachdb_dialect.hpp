// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donor_dialect.hpp"

namespace scratchbird::parser::cockroachdb {

using scratchbird::parser::donor::Diagnostic;
using scratchbird::parser::donor::Field;
using scratchbird::parser::donor::ParseResult;
using scratchbird::parser::donor::SurfaceDescriptor;
using scratchbird::parser::donor::Token;

const scratchbird::parser::donor::DialectProfile& Profile();
std::string TrimAscii(std::string_view text);
std::string NormalizeWhitespace(std::string_view text);
std::string ToUpperAscii(std::string_view text);
std::string MessageVectorToJson(const std::vector<Diagnostic>& diagnostics);
std::vector<Token> LexTokens(std::string_view sql_text);
ParseResult ParseStatement(std::string_view sql_text);
std::span<const SurfaceDescriptor> DatatypeSurfaces();
std::span<const SurfaceDescriptor> BuiltinFunctionSurfaces();
std::span<const SurfaceDescriptor> CatalogOverlaySurfaces();
std::span<const SurfaceDescriptor> DiagnosticSurfaces();
std::string CockroachdbPackageIdentityJson();
std::string CockroachdbSurfaceReportJson();

} // namespace scratchbird::parser::cockroachdb
