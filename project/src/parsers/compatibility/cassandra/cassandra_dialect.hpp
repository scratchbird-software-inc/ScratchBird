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

#include "compatibility_dialect.hpp"

namespace scratchbird::parser::cassandra {

using scratchbird::parser::compatibility::Diagnostic;
using scratchbird::parser::compatibility::Field;
using scratchbird::parser::compatibility::ParseResult;
using scratchbird::parser::compatibility::SurfaceDescriptor;
using scratchbird::parser::compatibility::Token;

const scratchbird::parser::compatibility::DialectProfile& Profile();
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
std::string CassandraPackageIdentityJson();
std::string CassandraSurfaceReportJson();

} // namespace scratchbird::parser::cassandra
