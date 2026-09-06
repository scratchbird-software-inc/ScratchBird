// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "mga_relation_store/mga_update_durable_store.hpp"
#include "typed_update_carrier_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scratchbird::engine::internal_api::mga_update_durable_detail {

// Internal frame-format bridge shared by the durable UPDATE coordinator and
// its statement-savepoint support. Frame presence is mutation evidence only;
// durable transaction inventory remains finality authority.

enum class DmlUpdateDurableFrameKindV1 : std::uint16_t {
  authority_snapshot = 1,
  journal = 2,
  statement_savepoint = 3,
  recovery_observation = 4,
  authority_reservation = 5,
  // Contains the already encoded canonical journal frame that may be
  // published after the statement barrier without any further
  // encode/hash/allocation step.
  prepared_successor = 6,
  // Durable tombstone for a prepared publication successor that was
  // cancelled before the publication barrier. The tombstone names the exact
  // staged successor in its fixed frame fields; recovery clears that staged
  // successor before considering any later aborted successor.
  prepared_successor_invalidated = 7,
};

struct DmlUpdateDurableFrameV1 {
  DmlUpdateDurableFrameKindV1 kind =
      DmlUpdateDurableFrameKindV1::authority_snapshot;
  MgaDmlUpdateDurableOperationIdentityV1 identity;
  std::uint64_t sequence = 0;
  std::uint8_t state = 0;
  std::uint8_t flags = 0;
  MgaDmlUpdateDurableSha256V1 prior_record_sha256{};
  MgaDmlUpdateDurableSha256V1 record_evidence_sha256{};
  std::vector<std::uint8_t> payload;
};

struct DmlUpdateDurableFrameLoadV1 {
  bool ok = false;
  bool missing = false;
  std::string detail;
  std::vector<DmlUpdateDurableFrameV1> frames;
};

class DmlUpdateDurableFileLock final {
 public:
  explicit DmlUpdateDurableFileLock(const std::string& data_path);
  ~DmlUpdateDurableFileLock();

  bool ok() const;

 private:
  bool ok_ = false;
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

std::string DmlUpdateDurableOperationStorePath(
    const EngineRequestContext& context);
std::string DmlUpdateStatementSavepointBinaryStorePath(
    const EngineRequestContext& context);

void DmlUpdateDurablePutU16(std::vector<std::uint8_t>* bytes,
                            std::size_t offset,
                            std::uint16_t value);
void DmlUpdateDurablePutU32(std::vector<std::uint8_t>* bytes,
                            std::size_t offset,
                            std::uint32_t value);
void DmlUpdateDurablePutU64(std::vector<std::uint8_t>* bytes,
                            std::size_t offset,
                            std::uint64_t value);
bool DmlUpdateDurableReadU16(std::span<const std::uint8_t> bytes,
                             std::size_t offset,
                             std::uint16_t* value);
bool DmlUpdateDurableReadU32(std::span<const std::uint8_t> bytes,
                             std::size_t offset,
                             std::uint32_t* value);
bool DmlUpdateDurableReadU64(std::span<const std::uint8_t> bytes,
                             std::size_t offset,
                             std::uint64_t* value);
bool DmlUpdateDurableZero(std::span<const std::uint8_t> bytes);

bool DmlUpdateDurableUuidBytes(
    std::string_view uuid,
    std::array<std::uint8_t, 16>* bytes);
bool DmlUpdateDurableTypedUuid(
    std::string_view uuid,
    scratchbird::wire::TypedUpdateUuid* bytes);
std::string DmlUpdateDurableUuidText(std::span<const std::uint8_t> bytes);
std::string DmlUpdateDurableTypedUuidText(
    const scratchbird::wire::TypedUpdateUuid& bytes);
bool DmlUpdateDurablePutUuid(std::vector<std::uint8_t>* bytes,
                             std::size_t offset,
                             std::string_view uuid);

bool DmlUpdateDurableBaseIdentityValid(
    const MgaDmlUpdateDurableOperationIdentityV1& identity);
bool DmlUpdateDurableIdentityValid(
    const MgaDmlUpdateDurableOperationIdentityV1& identity);
bool DmlUpdateDurableIdentityMatchesContext(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity);

std::string DmlUpdateDurableDescriptorPath(
    const EngineRequestContext& context,
    const MgaDmlUpdateDurableOperationIdentityV1& identity);
std::string DmlUpdateDurableSavepointPath(
    const EngineRequestContext& context,
    std::string_view savepoint_uuid);

MgaDmlUpdateDurableSha256V1 DmlUpdateDurableSha256(
    std::span<const std::uint8_t> bytes);
bool DmlUpdateDurableEncodeFrame(
    const DmlUpdateDurableFrameV1& frame,
    std::vector<std::uint8_t>* encoded);
bool DmlUpdateDurableDecodeFrame(
    std::span<const std::uint8_t> bytes,
    DmlUpdateDurableFrameV1* frame,
    std::string* detail);
DmlUpdateDurableFrameLoadV1 DmlUpdateDurableLoadFrames(
    const std::string& path);
bool DmlUpdateDurableEnsureDirectory(const std::string& directory);

}  // namespace scratchbird::engine::internal_api::mga_update_durable_detail
