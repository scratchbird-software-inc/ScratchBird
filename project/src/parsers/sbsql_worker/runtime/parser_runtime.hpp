// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "cache/sblr_template_cache.hpp"
#include "common/common.hpp"
#include "metrics/parser_metrics.hpp"

namespace scratchbird::parser::sbsql {

ParserConfig ConfigFromArgs(int argc, char** argv, bool force_probe);
std::string SbsqlParserLifecycleMappingReportJson();
int RunParserWorker(ParserConfig config);

} // namespace scratchbird::parser::sbsql
