#pragma once
#include "sblr_session_setting_set_coordinator.hpp"
#include "../sblr/sblr_session_snapshot_handle_runtime.hpp"
namespace scratchbird::engine::internal_api { using SblrSessionSnapshotHandleCoordinationResult=SblrSessionSettingSetCoordinationResult; inline auto CompileSblrSessionSnapshotHandleDescriptor(const EngineRequestContext&c,const std::string&r,uint64_t o,uint64_t a){return CompileSblrSessionSettingSetDescriptor(c,r,o,a);} inline auto ConsumeSblrSessionSnapshotHandleDescriptor(const EngineRequestContext&c,const scratchbird::engine::sblr::SblrSessionSnapshotHandleDescriptorV1&v){return ConsumeSblrSessionSettingSetDescriptor(c,v);} }
