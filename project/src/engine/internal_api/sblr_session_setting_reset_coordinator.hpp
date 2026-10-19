#pragma once
#include "sblr_session_setting_reset_runtime.hpp"
#include "sblr_session_setting_set_coordinator.hpp"

namespace scratchbird::engine::internal_api {
using SblrSessionSettingResetCoordinationResult = SblrSessionSettingSetCoordinationResult;
inline auto CompileSblrSessionSettingResetDescriptor(
    const EngineRequestContext& context, const EngineUuid& receipt,
    uint64_t occurrence, uint64_t availability) {
  return CompileSblrSessionValueDescriptor(SessionValueOperation::setting_reset,
      context, receipt, occurrence, availability);
}
inline auto ConsumeSblrSessionSettingResetDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1& descriptor) {
  return ConsumeSblrSessionValueDescriptor(SessionValueOperation::setting_reset,
      context, descriptor);
}
}  // namespace scratchbird::engine::internal_api
