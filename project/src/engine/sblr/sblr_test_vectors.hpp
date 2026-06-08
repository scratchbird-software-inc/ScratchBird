// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrExecutorTestVector {
  std::string slice_id;
  std::string vector_id;
  std::string category;
  std::string operation_id;
  std::string expected_diagnostic;
  bool expects_mutation = false;
};

std::vector<SblrExecutorTestVector> BuiltInSblrExecutorTestVectors();

}  // namespace scratchbird::engine::sblr
