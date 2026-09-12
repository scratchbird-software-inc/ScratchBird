// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_text_value_preparation.hpp"
#include "api_diagnostics.hpp"
#include "memory.hpp"
#include "typed_update_carrier_codec.hpp"
#include <cstring>
#include <new>

namespace scratchbird::engine::internal_api {
namespace memory = scratchbird::core::memory;

struct EngineDmlUpdatePreparedTextValueV2::Storage {
  EngineDmlUpdateTextTargetHandleV2 target;
  EngineValueState state = EngineValueState::unknown;
  std::uint64_t scalar_count = 0;
  memory::ScopedAllocation payload;
};

namespace {
EngineApiDiagnostic Refuse(std::string code, std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code),
      "sblr.dml_update_rows.text_value_preparation", std::move(detail), true);
}
}  // namespace

EngineValueState EngineDmlUpdatePreparedTextValueV2::state() const noexcept {
  return storage_ ? storage_->state : EngineValueState::unknown;
}
std::uint64_t EngineDmlUpdatePreparedTextValueV2::scalar_count() const noexcept {
  return storage_ ? storage_->scalar_count : 0;
}
std::span<const std::uint8_t> EngineDmlUpdatePreparedTextValueV2::bytes() const noexcept {
  if (!storage_ || !storage_->payload.valid()) return {};
  return {static_cast<const std::uint8_t*>(storage_->payload.data()),
          storage_->payload.size()};
}

EngineDmlUpdateTextPreparationResultV2 PrepareDmlUpdateTextValueV2(
    const EngineRequestContext& context,
    const EngineDmlUpdateTextTargetHandleV2& target,
    EngineValueState state, std::span<const std::uint8_t> bytes) {
  EngineDmlUpdateTextPreparationResultV2 result;
  result.diagnostic = RevalidateDmlUpdateTextTargetV2(context, target);
  if (result.diagnostic.error) return result;
  const auto& snapshot = *target.snapshot();
  if ((state != EngineValueState::value && state != EngineValueState::sql_null) ||
      (state == EngineValueState::sql_null && !bytes.empty())) {
    result.diagnostic = Refuse("DML.ASSIGNMENT_SHAPE_INVALID",
                               "TEXT requires VALUE or zero-payload SQL NULL");
    return result;
  }
  if (state == EngineValueState::sql_null && !snapshot.nullable) {
    result.diagnostic = Refuse("CLI.CONSTRAINT_NOT_NULL_VIOLATION", "TEXT target is not nullable");
    return result;
  }
  if (bytes.size() > scratchbird::wire::kTypedUpdateMaximumCanonicalValueBytesPerValue) {
    result.diagnostic = Refuse("RESOURCE.BUDGET_EXCEEDED", "TEXT exceeds DUAV value ceiling");
    return result;
  }
  std::uint64_t scalars = 0;
  if (!scratchbird::wire::ValidateTypedUpdateTextUtf8V2(bytes, &scalars)) {
    result.diagnostic = Refuse("CTB.TEXT.INVALID_ENCODING", "TEXT is not shortest-form scalar UTF-8");
    return result;
  }
  if (bytes.size() > snapshot.byte_limit ||
      scalars > snapshot.character_limit) {
    result.diagnostic = Refuse("CTB.TEXT.LENGTH_EXCEEDED", "TEXT exceeds exact target limits");
    return result;
  }
  try {
    auto storage = std::make_shared<EngineDmlUpdatePreparedTextValueV2::Storage>();
    storage->target = target;
    storage->state = state;
    storage->scalar_count = scalars;
    if (!bytes.empty()) {
      memory::MemoryTag tag;
      tag.subsystem = scratchbird::core::platform::Subsystem::engine;
      tag.purpose = "dml.update_rows.text_canonical_value";
      tag.category = memory::MemoryCategory::datatype_payload;
      tag.lifetime = memory::MemoryLifetime::statement;
      tag.owner = context.principal_uuid;
      tag.context_id = context.statement_receipt_uuid;
      tag.database_id = context.database_uuid;
      tag.session_id = context.session_uuid;
      tag.transaction_id = context.transaction_uuid;
      tag.statement_id = context.statement_uuid;
      tag.callsite = "PrepareDmlUpdateTextValueV2";
      auto allocated = memory::DefaultMemoryManager().AllocateScoped(
          bytes.size(), alignof(std::max_align_t), std::move(tag));
      if (!allocated.ok()) {
        result.diagnostic = Refuse("RESOURCE.BUDGET_EXCEEDED",
                                   "TEXT canonical payload allocation refused");
        return result;
      }
      storage->payload = std::move(allocated.allocation);
      std::memcpy(storage->payload.data(), bytes.data(), bytes.size());
    }
    result.value.storage_ = std::move(storage);
  } catch (const std::bad_alloc&) {
    result.diagnostic = Refuse("RESOURCE.BUDGET_EXCEEDED", "TEXT preparation allocation failed");
    return result;
  }
  result.ok = true;
  return result;
}

EngineApiDiagnostic RevalidateDmlUpdatePreparedTextValueV2(
    const EngineRequestContext& context,
    const EngineDmlUpdatePreparedTextValueV2& value) {
  if (!value.valid()) return Refuse("DML.ASSIGNMENT_SHAPE_INVALID", "TEXT value handle is absent");
  return RevalidateDmlUpdateTextTargetV2(context, value.storage_->target);
}

}  // namespace scratchbird::engine::internal_api
