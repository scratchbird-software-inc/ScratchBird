#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bridge_open_channel_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrBridgeOpenChannelCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBridgeOpenChannelDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrBridgeOpenChannelCoordinationResult CompileSblrBridgeOpenChannelDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrBridgeOpenChannelCoordinationResult ConsumeSblrBridgeOpenChannelDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBridgeOpenChannelDescriptorV1&); }
