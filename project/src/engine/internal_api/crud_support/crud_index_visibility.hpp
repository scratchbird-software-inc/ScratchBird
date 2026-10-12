// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "crud_support/crud_store.hpp"

namespace scratchbird::engine::internal_api {

// Latest visible version per binary index identity within one admitted source
// sequence domain. Distinct identities retain first-visible encounter order.
std::vector<CrudIndexRecord> VisibleCrudIndexes(const RelationReadSnapshot& state,
                                               std::uint64_t observer_tx);

}  // namespace scratchbird::engine::internal_api
