// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "database_local_security_event_store.hpp"

namespace scratchbird::transaction::mga { class PublishedSnapshotPin; }
namespace scratchbird::engine::internal_api {
// Engine-private source read, not permission, policy activation, or a new
// statement snapshot. The actual retained publication supplies visibility;
// context observations cannot create it. Caller retains/quiesces the pin
// throughout this read. Ordinary publication retirement preserves an issued
// pin; explicit revocation, a foreign transaction or a retired reader refuses.
DatabaseLocalSecurityEventStoreLoadResultV1
LoadDatabaseLocalSecurityEventStoreAtRetainedSnapshotV1(
    const EngineRequestContext& context,
    const transaction::mga::PublishedSnapshotPin& pin);
} // namespace scratchbird::engine::internal_api
