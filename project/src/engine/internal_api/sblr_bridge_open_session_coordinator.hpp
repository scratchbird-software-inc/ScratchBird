#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bridge_open_session_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrBridgeOpenSessionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBridgeOpenSessionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrBridgeOpenSessionCoordinationResult CompileSblrBridgeOpenSessionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrBridgeOpenSessionCoordinationResult ConsumeSblrBridgeOpenSessionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBridgeOpenSessionDescriptorV1&); }
