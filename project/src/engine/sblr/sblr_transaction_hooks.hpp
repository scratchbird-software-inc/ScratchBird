// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_runtime.hpp"

#include <string_view>

namespace scratchbird::engine::sblr {

SblrResult sb_sblr_begin_statement_boundary(const SblrExecutionContext& context);
SblrResult sb_sblr_open_internal_savepoint(const SblrExecutionContext& context, std::string_view savepoint_name);
SblrResult sb_sblr_commit_internal_region(const SblrExecutionContext& context, std::string_view savepoint_name);
SblrResult sb_sblr_rollback_internal_region(const SblrExecutionContext& context, std::string_view savepoint_name);
SblrResult sb_sblr_sequence_next(const SblrExecutionContext& context, std::string_view sequence_uuid);
SblrResult sb_sblr_runtime_log(const SblrExecutionContext& context, std::string_view message);
SblrResult sb_sblr_end_statement_boundary(const SblrExecutionContext& context);

SblrResult sb_sblr_cluster_begin_branch(const SblrExecutionContext& context);
SblrResult sb_sblr_cluster_prepare_branch(const SblrExecutionContext& context);
SblrResult sb_sblr_cluster_record_decision(const SblrExecutionContext& context);
SblrResult sb_sblr_cluster_publish_commit(const SblrExecutionContext& context);
SblrResult sb_sblr_cluster_publish_rollback(const SblrExecutionContext& context);
SblrResult sb_sblr_cluster_recover_limbo(const SblrExecutionContext& context);
SblrResult sb_sblr_cluster_route_remote_fragment(const SblrExecutionContext& context);

}  // namespace scratchbird::engine::sblr
