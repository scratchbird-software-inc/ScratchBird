#pragma once
#include "sblr_session_setting_set_coordinator.hpp"
#include "../sblr/sblr_session_discard_runtime.hpp"

namespace scratchbird::engine::internal_api {
using SblrSessionDiscardCoordinationResult = SblrSessionSettingSetCoordinationResult;
inline auto CompileSblrSessionDiscardDescriptor(
    const EngineRequestContext& context, const EngineUuid& receipt,
    uint64_t occurrence, uint64_t availability) {
  return CompileSblrSessionValueDescriptor(SessionValueOperation::discard,
      context, receipt, occurrence, availability);
}
inline auto ConsumeSblrSessionDiscardDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1& descriptor) {
  return ConsumeSblrSessionValueDescriptor(SessionValueOperation::discard,
      context, descriptor);
}
}  // namespace scratchbird::engine::internal_api
