#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bridge_close_session_runtime.hpp"
namespace scratchbird::engine::internal_api { struct SblrBridgeCloseSessionCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBridgeCloseSessionDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;}; SblrBridgeCloseSessionCoordinationResult CompileSblrBridgeCloseSessionDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t); SblrBridgeCloseSessionCoordinationResult ConsumeSblrBridgeCloseSessionDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBridgeCloseSessionDescriptorV1&); }
