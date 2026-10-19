#pragma once
#include "sblr_session_setting_get_runtime.hpp"
#include "sblr_session_setting_set_coordinator.hpp"

namespace scratchbird::engine::internal_api {
using SblrSessionSettingGetCoordinationResult = SblrSessionSettingSetCoordinationResult;
inline auto CompileSblrSessionSettingGetDescriptor(
    const EngineRequestContext& context, const EngineUuid& receipt,
    uint64_t occurrence, uint64_t availability) {
  return CompileSblrSessionValueDescriptor(SessionValueOperation::setting_get,
      context, receipt, occurrence, availability);
}
inline auto ConsumeSblrSessionSettingGetDescriptor(
    const EngineRequestContext& context,
    const scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1& descriptor) {
  return ConsumeSblrSessionValueDescriptor(SessionValueOperation::setting_get,
      context, descriptor);
}
}  // namespace scratchbird::engine::internal_api
